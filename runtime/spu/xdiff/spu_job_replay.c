/*
 * ps3recomp - spu_job_replay: the offline runner of docs/SPU_XDIFF.md section 13.
 *
 * One fixture, one verdict. The replay runs in a child process and the parent
 * answers for it: a subject that hangs or dies is `void` / `host-fault` and
 * never takes the caller (a corpus driver) down. The verdict is the exit
 * status (spu_xdiff.h); 1 is a usage error or a refused knob and is not a
 * verdict. Standard output carries one JSON object (--json, schema
 * `ps3recomp.spujob-verdict/1`) or one human line; diagnostics go to standard
 * error.
 *
 * JSON conventions: addresses (`pc`, `lsa`, `ea`, `code_end`), the 64-bit
 * identities (`image_fingerprint`, `lift_options`, `native_rev`) and byte
 * strings (`want`, `got`, `toolkit_commit`) are hex strings, so no consumer
 * rounds them through a double; counts, indices and enumerations are
 * integers. A field that does not apply to the verdict is null. Strings are
 * escaped byte for byte: a byte >= 0x80 of the fixture path becomes \u00XX
 * (the path is the caller's own string, not data this tool interprets).
 */
#include "spu_xdiff_replay.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

typedef struct runner_args {
    const char* path;
    uint32_t    tier;       /* SPU_TIER_INTERP or SPU_TIER_LIFTED */
    uint64_t    budget;
    unsigned    timeout_s;
    int         json;
    const char* poison;     /* --poison: the liveness plan's path, or NULL */
    const spu_xdiff_poison_plan* plan;   /* that plan, loaded */
} runner_args;

/* What the replay child hands back: the verdict record, and the poison run's report (zero without --poison). */
typedef struct child_reply {
    spu_xdiff_verdict_record v;
    spu_xdiff_poison_report  poison;
} child_reply;

static int usage(const char* argv0, const char* why)
{
    if (why) fprintf(stderr, "%s: %s\n", argv0, why);
    fprintf(stderr, "usage: %s [--tier interp|registered] [--budget N] [--timeout S] [--poison PLAN] [--json] FIXTURE\n", argv0);
    return 1;
}

/* A whole-string unsigned decimal or 0x number; a sign, a blank or trailing text is refused. */
static int parse_u64(const char* s, uint64_t* out)
{
    char* end = NULL;
    if (!s || !(s[0] >= '0' && s[0] <= '9')) return 0;
    errno = 0;
    const unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || !end || *end != 0) return 0;
    *out = (uint64_t)v;
    return 1;
}

/* 0 on success, 1 (printed) on a usage error. The default tier is the registered body: the tool exists to
 * compare the other tier with a fixture, and a fixture's own tier needs no replay. */
static int parse_args(int argc, char** argv, runner_args* a)
{
    const char* argv0 = argc > 0 && argv[0] ? argv[0] : "spu_job_replay";
    a->path = NULL;
    a->tier = (uint32_t)SPU_TIER_LIFTED;
    a->budget = 0;
    a->timeout_s = 60;
    a->json = 0;
    a->poison = NULL;
    a->plan = NULL;
    for (int i = 1; i < argc; i++) {
        const char* o = argv[i];
        if (!strcmp(o, "--json")) {
            a->json = 1;
        } else if (!strcmp(o, "--poison")) {
            if (i + 1 >= argc) return usage(argv0, "an option lacks its value");
            a->poison = argv[++i];
        } else if (!strcmp(o, "--tier") || !strcmp(o, "--budget") || !strcmp(o, "--timeout")) {
            if (i + 1 >= argc) return usage(argv0, "an option lacks its value");
            const char* v = argv[++i];
            uint64_t n = 0;
            if (!strcmp(o, "--tier")) {
                if (!strcmp(v, "interp")) a->tier = (uint32_t)SPU_TIER_INTERP;
                else if (!strcmp(v, "registered")) a->tier = (uint32_t)SPU_TIER_LIFTED;
                else return usage(argv0, "--tier is interp or registered");
            } else if (!parse_u64(v, &n)) {
                return usage(argv0, "a number is expected after --budget and --timeout");
            } else if (!strcmp(o, "--budget")) {
                a->budget = n;
            } else {
                if (n == 0 || n > 86400u) return usage(argv0, "--timeout is 1 to 86400 seconds");
                a->timeout_s = (unsigned)n;
            }
        } else if (o[0] == '-' && o[1] != 0) {
            return usage(argv0, "unknown option");
        } else if (a->path) {
            return usage(argv0, "one fixture only");
        } else {
            a->path = o;
        }
    }
    if (!a->path) return usage(argv0, "no fixture");
    if (a->poison && a->tier != (uint32_t)SPU_TIER_INTERP)
        return usage(argv0, "--poison is T0's observer: it needs --tier interp");
    return 0;
}

/* ---- the verdict records the runner makes itself --------------------------------- */

static uint32_t set_reason(spu_xdiff_verdict_record* v, uint32_t reason)
{
    v->verdict = spu_xdiff_reason_verdict(reason);
    v->reason = reason;
    return v->verdict;
}

static void blank_record(spu_xdiff_verdict_record* v, uint32_t tier)
{
    memset(v, 0, sizeof *v);
    v->effect_index = SPU_XDIFF_NO_EFFECT;
    v->subject_body.tier = (uint8_t)tier;
}

/* void / host-fault: the replay could not be run or did not finish. The pair's matching rule is known from the
 * fixture and the tier alone, so it stays in the record. */
static uint32_t fault_record(spu_xdiff_verdict_record* v, uint32_t tier, uint32_t match)
{
    blank_record(v, tier);
    v->match = match;
    return set_reason(v, SPU_XDIFF_R_HOST_FAULT);
}

/* ---- running the replay ----------------------------------------------------------- */

/* Returns the verdict, or SPU_XDIFF_VERDICT_INVALID when the replay itself refused a knob. `report` is the poison
 * run's (zero when there is none). */
static uint32_t replay_here(const spu_xdiff_fixture* f, const runner_args* a, spu_xdiff_verdict_record* v,
                            spu_xdiff_poison_report* report)
{
    spu_xdiff_replay_options opt;
    spu_xdiff_shadow* s = spu_xdiff_shadow_new();
    memset(report, 0, sizeof *report);
    if (!s) {
        fprintf(stderr, "spu_job_replay: out of memory for the shadow context\n");
        return fault_record(v, a->tier, spu_xdiff_match_for(f->identity.authority.tier, a->tier));
    }
    opt.subject_tier = a->tier;
    opt.budget = a->budget;
    const uint32_t r = a->plan ? spu_xdiff_replay_poison(f, &opt, a->plan, s, v, report) : spu_xdiff_replay(f, &opt, s, v);
    spu_xdiff_shadow_free(s);
    return r;
}

#ifndef _WIN32

static int write_all(int fd, const void* buf, size_t n)
{
    const uint8_t* p = (const uint8_t*)buf;
    while (n) {
        const ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t host_fault(const runner_args* a, spu_xdiff_verdict_record* v, uint32_t match, const char* what, int detail)
{
    fprintf(stderr, "spu_job_replay: the replay child %s %d\n", what, detail);
    return fault_record(v, a->tier, match);
}

/* The replay runs in a child that writes its reply (the verdict record and the poison report) to a pipe and
 * exits. The parent waits for the reply with a deadline; no reply before the deadline, or none at all (a
 * signal, an abort, a nonzero exit), is void / host-fault. A subject that runs away or crashes costs only the
 * child. Whenever fewer than a whole reply arrived the child is killed before it is waited for: a child that is
 * hung after a poll or read error must not block the supervisor. */
static uint32_t replay_supervised(const spu_xdiff_fixture* f, const runner_args* a, spu_xdiff_verdict_record* v,
                                  spu_xdiff_poison_report* report)
{
    const uint32_t match = v->match;
    memset(report, 0, sizeof *report);
    int fds[2];
    if (pipe(fds) != 0) {
        fprintf(stderr, "spu_job_replay: pipe failed: errno %d\n", errno);
        return fault_record(v, a->tier, match);
    }
    fflush(NULL);   /* the child must not flush the parent's buffered output a second time */
    const pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "spu_job_replay: fork failed: errno %d\n", errno);
        close(fds[0]);
        close(fds[1]);
        return fault_record(v, a->tier, match);
    }
    if (pid == 0) {
        struct rlimit nocore = { 0, 0 };
        child_reply reply;
        close(fds[0]);
        memset(&reply, 0, sizeof reply);
        setrlimit(RLIMIT_CORE, &nocore);   /* a crashing subject is reported, not dumped */
        replay_here(f, a, v, &reply.poison);
        reply.v = *v;
        _exit(write_all(fds[1], &reply, sizeof reply) == 0 ? 0 : 71);
    }
    close(fds[1]);

    child_reply rep;
    const spu_xdiff_verdict_record* rec = &rep.v;
    size_t got = 0;
    int timed_out = 0;
    const int64_t deadline = mono_ms() + (int64_t)a->timeout_s * 1000;
    uint8_t* const dst = (uint8_t*)&rep;
    while (got < sizeof rep) {
        const int64_t left = deadline - mono_ms();
        if (left <= 0) { timed_out = 1; break; }
        struct pollfd pf = { fds[0], POLLIN, 0 };
        const int pr = poll(&pf, 1, left > 2000000000 ? 2000000000 : (int)left);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;   /* the deadline test above decides */
        const ssize_t n = read(fds[0], dst + got, sizeof rep - got);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;       /* the child closed the pipe: it is done or dead */
        got += (size_t)n;
    }
    close(fds[0]);
    if (got < sizeof rep) kill(pid, SIGKILL);   /* a timeout, an error, or a child that never answered */
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }

    if (timed_out) {
        fprintf(stderr, "spu_job_replay: the replay child did not finish within %u s and was killed\n", a->timeout_s);
        return fault_record(v, a->tier, match);
    }
    if (got == sizeof rep && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        if (rec->verdict == (uint32_t)SPU_XDIFF_VERDICT_INVALID) { *v = *rec; return rec->verdict; }
        if (spu_xdiff_reason_verdict(rec->reason) != rec->verdict)
            return host_fault(a, v, match, "wrote an inconsistent verdict, reason", (int)rec->reason);
        *v = *rec;
        *report = rep.poison;
        return v->verdict;
    }
    if (WIFSIGNALED(st)) return host_fault(a, v, match, "was killed by signal", WTERMSIG(st));
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        return host_fault(a, v, match, "exited with status", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return host_fault(a, v, match, "ended without a verdict record after bytes:", (int)got);
}

#endif /* !_WIN32 */

/* ---- JSON ------------------------------------------------------------------------ */

static void js_str(FILE* o, const char* s)
{
    fputc('"', o);
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(o, "\\%c", c);
        else if (c < 0x20 || c >= 0x80) fprintf(o, "\\u%04x", c);
        else fputc(c, o);
    }
    fputc('"', o);
}

static void js_hex(FILE* o, uint64_t v, int digits)
{
    fprintf(o, "\"0x%0*llx\"", digits, (unsigned long long)v);
}

static void js_bytes(FILE* o, const uint8_t* b, size_t n)
{
    fputc('"', o);
    for (size_t i = 0; i < n; i++) fprintf(o, "%02x", b[i]);
    fputc('"', o);
}

static const char* effect_kind_id(uint32_t kind)
{
    switch (kind) {
    case SPU_FX_WRCH:    return "wrch";
    case SPU_FX_RDCH:    return "rdch";
    case SPU_FX_RCHCNT:  return "rchcnt";
    case SPU_FX_MFC_CMD: return "mfc-cmd";
    case SPU_FX_XFER:    return "xfer";
    default:             return "unknown";
    }
}

static void js_effect(FILE* o, const spu_xdiff_effect* e)
{
    fprintf(o, "{\"kind\":\"%s\",\"channel\":%u,\"tag\":%u,\"dir\":%u,\"pc\":", effect_kind_id(e->kind),
            (unsigned)e->channel, (unsigned)e->tag, (unsigned)e->dir);
    js_hex(o, e->pc, 8);
    fprintf(o, ",\"value\":%u,\"status\":%u,\"ea\":", (unsigned)e->value, (unsigned)e->status);
    js_hex(o, e->ea, 16);
    fprintf(o, ",\"lsa\":");
    js_hex(o, e->lsa, 8);
    fprintf(o, ",\"size\":%u}", (unsigned)e->size);
}

/* The reasons whose record carries want/got/where (spu_xdiff.h, spu_xdiff_verdict_record). */
static int has_bytes(uint32_t reason)
{
    return reason == (uint32_t)SPU_XDIFF_R_EFFECT_REQUEST || reason == (uint32_t)SPU_XDIFF_R_EFFECT_PAYLOAD ||
           reason == (uint32_t)SPU_XDIFF_R_END_CLASS || reason == (uint32_t)SPU_XDIFF_R_END_REGS ||
           reason == (uint32_t)SPU_XDIFF_R_END_LS;
}

static int all_zero(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

static const char* poison_kind_id(uint32_t kind)
{
    return kind == (uint32_t)SPU_XDIFF_POISON_REGISTER ? "register" : kind == (uint32_t)SPU_XDIFF_POISON_STACK ? "stack" : NULL;
}

/* The `poison` member, present only under --poison: what the run applied, and for a `poison` verdict the
 * function and the register or frame byte that explain it, where one does. */
static void js_poison(FILE* o, const runner_args* a, const spu_xdiff_verdict_record* v, const spu_xdiff_poison_report* pr)
{
    fprintf(o, ",\"poison\":{\"plan\":");
    js_str(o, a->poison);
    fprintf(o, ",\"functions\":%u,\"ran\":%s,\"returns\":%u,\"poisoned\":%u,\"culprit\":", (unsigned)a->plan->count,
            pr->ran ? "true" : "false", (unsigned)pr->returns, (unsigned)pr->poisoned);
    if (v->reason != (uint32_t)SPU_XDIFF_R_POISON) {
        fputs("null}", o);
        return;
    }
    const char* kind = poison_kind_id(pr->kind);
    fputs("{\"function\":", o);
    if (pr->has_function) js_hex(o, pr->function, 5); else fputs("null", o);
    fputs(",\"kind\":", o);
    if (pr->has_function && kind) js_str(o, kind); else fputs("null", o);
    fputs(",\"register\":", o);
    if (pr->has_function && pr->kind == (uint32_t)SPU_XDIFF_POISON_REGISTER) fprintf(o, "%u", (unsigned)pr->reg); else fputs("null", o);
    /* below_sp is the byte at sp - below_sp; 0 is a frame claim that failed on the stack pointer itself */
    fputs(",\"below_sp\":", o);
    if (pr->has_function && pr->kind == (uint32_t)SPU_XDIFF_POISON_STACK) fprintf(o, "%u", (unsigned)pr->below_sp); else fputs("null", o);
    fputs(",\"exact\":", o);
    if (pr->has_function && pr->kind == (uint32_t)SPU_XDIFF_POISON_STACK) fputs(pr->exact ? "true" : "false", o); else fputs("null", o);
    fputs(",\"underlying\":", o);
    js_str(o, spu_xdiff_reason_id(pr->underlying));
    fputs("}}", o);
}

static void print_json(FILE* o, const runner_args* a, const spu_xdiff_fixture* f, const spu_xdiff_verdict_record* v,
                       const spu_xdiff_poison_report* pr, double seconds)
{
    const uint32_t klass = spu_xdiff_differ_class_of(v->reason, f ? f->unit.profile : 0);
    fprintf(o, "{\"schema\":\"ps3recomp.spujob-verdict/1\",\"tool\":\"spu_job_replay\",\"version\":1,"
               "\"verdict\":");
    js_str(o, spu_xdiff_verdict_id(v->verdict));
    fprintf(o, ",\"exit_code\":%u,\"reason\":", (unsigned)v->verdict);
    js_str(o, spu_xdiff_reason_id(v->reason));
    fprintf(o, ",\"match\":%u,\"class\":", (unsigned)v->match);
    js_str(o, spu_xdiff_differ_class_id(klass));

    fprintf(o, ",\"fixture\":{\"path\":");
    js_str(o, a->path);
    fprintf(o, ",\"image_fingerprint\":");
    if (f) js_hex(o, f->identity.image_fingerprint, 16); else fputs("null", o);
    fprintf(o, ",\"authority_tier\":");
    if (f) fprintf(o, "%u", (unsigned)f->identity.authority.tier); else fputs("null", o);
    fprintf(o, ",\"effects\":");
    if (f) fprintf(o, "%u", (unsigned)f->effect_count); else fputs("null", o);
    fprintf(o, ",\"toolkit_commit\":");
    if (f && !all_zero(f->identity.toolkit_commit, sizeof f->identity.toolkit_commit))
        js_bytes(o, f->identity.toolkit_commit, sizeof f->identity.toolkit_commit);
    else
        fputs("null", o);

    fprintf(o, "},\"subject\":{\"tier\":%u,\"lifter_abi\":%u,\"code_end\":", (unsigned)v->subject_body.tier,
            (unsigned)v->subject_body.lifter_abi);
    js_hex(o, v->subject_body.code_end, 8);
    fprintf(o, ",\"lift_options\":");
    js_hex(o, v->subject_body.lift_options, 16);
    fprintf(o, ",\"native_rev\":");
    js_hex(o, v->subject_body.native_rev, 16);

    fprintf(o, "},\"first\":{\"effect_index\":");
    if (v->effect_index != SPU_XDIFF_NO_EFFECT) fprintf(o, "%u", (unsigned)v->effect_index); else fputs("null", o);
    fprintf(o, ",\"authority\":");
    if (f && v->effect_index < f->effect_count) js_effect(o, &f->effects[v->effect_index]); else fputs("null", o);
    fprintf(o, ",\"subject\":");
    if (v->subject.kind) js_effect(o, &v->subject); else fputs("null", o);
    /* `where` is null when the difference has no byte position (a scalar field of a request, the end class); `want`
     * and `got` then are null too, except for end-class, where they are the two words {end_class, stop_code}, little
     * endian, of the fixture and of the subject. */
    const int positioned = v->where != SPU_XDIFF_NO_WHERE;
    if (v->verdict == (uint32_t)SPU_XDIFF_DIFFER && has_bytes(v->reason) && (positioned || v->reason == (uint32_t)SPU_XDIFF_R_END_CLASS)) {
        if (positioned) fprintf(o, ",\"where\":%u,\"want\":", (unsigned)v->where);
        else fputs(",\"where\":null,\"want\":", o);
        js_bytes(o, v->want, sizeof v->want);
        fprintf(o, ",\"got\":");
        js_bytes(o, v->got, sizeof v->got);
    } else {
        fputs(",\"where\":null,\"want\":null,\"got\":null", o);
    }
    /* Where the subject stood when the verdict was reached: for `unlifted-branch` the local store address no
     * lifted code covers, which is what a coverage report needs to name. */
    fputs("},\"subject_pc\":", o);
    js_hex(o, v->subject_pc, 8);
    fprintf(o, ",\"budget\":{\"limit\":%llu,\"spent\":%llu},\"seconds\":%.6f", (unsigned long long)v->budget,
            (unsigned long long)v->budget_spent, seconds);
    if (a->plan) js_poison(o, a, v, pr);
    fputs("}\n", o);
}

static void print_line(FILE* o, const runner_args* a, const spu_xdiff_fixture* f, const spu_xdiff_verdict_record* v,
                       const spu_xdiff_poison_report* pr)
{
    const uint32_t klass = spu_xdiff_differ_class_of(v->reason, f ? f->unit.profile : 0);
    fprintf(o, "%s %s", spu_xdiff_verdict_id(v->verdict), spu_xdiff_reason_id(v->reason));
    if (v->effect_index != SPU_XDIFF_NO_EFFECT) fprintf(o, " effect=%u", (unsigned)v->effect_index);
    if (v->verdict == (uint32_t)SPU_XDIFF_DIFFER && has_bytes(v->reason) && v->where != SPU_XDIFF_NO_WHERE)
        fprintf(o, " where=0x%X", (unsigned)v->where);
    if (v->reason == (uint32_t)SPU_XDIFF_R_UNLIFTED_BRANCH) fprintf(o, " pc=0x%05X", (unsigned)v->subject_pc);
    if (v->reason == (uint32_t)SPU_XDIFF_R_POISON && pr->has_function) {
        fprintf(o, " function=0x%05X", (unsigned)pr->function);
        if (pr->kind == (uint32_t)SPU_XDIFF_POISON_REGISTER) fprintf(o, " reg=%u", (unsigned)pr->reg);
        else if (pr->kind == (uint32_t)SPU_XDIFF_POISON_STACK) fprintf(o, " below_sp=0x%X", (unsigned)pr->below_sp);
    }
    fprintf(o, " %s", a->path);
    if (klass == (uint32_t)SPU_XDIFF_CLASS_LATENT) fputs(" (latent)", o);
    fputc('\n', o);
}

static double now_seconds(void)
{
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int spu_job_replay_main(int argc, char** argv)
{
    runner_args a;
    spu_xdiff_poison_plan plan;
    spu_xdiff_poison_report report;
    if (parse_args(argc, argv, &a) != 0) return 1;

    const char* knob = spu_xdiff_refused_knob();
    if (knob) {
        fprintf(stderr, "spu_job_replay: refusing to run with %s set: it changes what a run does at a stop, a "
                        "return or a branch, so the replay would compare a different program (docs/SPU_XDIFF.md 8.1)\n",
                knob);
        return 1;
    }
    /* A plan that cannot be read is a usage error, not a verdict: nothing was compared. */
    memset(&plan, 0, sizeof plan);
    memset(&report, 0, sizeof report);
    if (a.poison) {
        char why[256];
        if (spu_xdiff_poison_plan_load(a.poison, &plan, why, sizeof why) != 0) {
            fprintf(stderr, "spu_job_replay: %s: %s\n", a.poison, why);
            return 1;
        }
        a.plan = &plan;
    }

    const double t0 = now_seconds();
    spu_xdiff_fixture f;
    spu_xdiff_error err;
    spu_xdiff_verdict_record v;
    int loaded = 0;
    memset(&f, 0, sizeof f);
    memset(&err, 0, sizeof err);
    blank_record(&v, a.tier);

    if (spu_xdiff_fixture_read(a.path, &f, &err) != 0) {
        fprintf(stderr, "spu_job_replay: %s: %s: %s\n", a.path, spu_xdiff_reason_id(err.reason), err.detail);
        /* An out-of-memory decode (CAPTURE_LIMIT, spu_xdiff_fixture.h) is the host's failure, not a malformed fixture. */
        if (err.reason == (uint32_t)SPU_XDIFF_R_CAPTURE_LIMIT)
            fault_record(&v, a.tier, 0);
        else if (err.reason == (uint32_t)SPU_XDIFF_R_NO_FIXTURE || err.reason == (uint32_t)SPU_XDIFF_R_FIXTURE_IDENTITY)
            set_reason(&v, err.reason);
        else
            set_reason(&v, SPU_XDIFF_R_FIXTURE_FORMAT);
    } else {
        loaded = 1;
        v.match = spu_xdiff_match_for(f.identity.authority.tier, a.tier);
#ifdef _WIN32
        /* No fork on Windows: the replay runs in this process, so a subject that hangs or faults takes the
         * runner with it. A corpus driver on Windows supervises the runner itself. */
        replay_here(&f, &a, &v, &report);
#else
        replay_supervised(&f, &a, &v, &report);
#endif
        if (v.verdict == (uint32_t)SPU_XDIFF_VERDICT_INVALID) {
            const char* k = spu_xdiff_refused_knob();
            fprintf(stderr, "spu_job_replay: the replay refused to run%s%s set\n", k ? ": " : "", k ? k : "");
            spu_xdiff_fixture_free(&f);
            spu_xdiff_poison_plan_free(&plan);
            return 1;
        }
    }

    if (a.json) print_json(stdout, &a, loaded ? &f : NULL, &v, &report, now_seconds() - t0);
    else print_line(stdout, &a, loaded ? &f : NULL, &v, &report);
    fflush(stdout);
    if (loaded) spu_xdiff_fixture_free(&f);
    spu_xdiff_poison_plan_free(&plan);
    return (int)v.verdict;
}

/*
 * test_lv2conf: the lv2 conformance cases (tools/hwprobe/probes/lv2conf) against the toolkit's HLE.
 *
 *   test_lv2conf --out FILE [--timeout SEC]
 *
 * Every case runs in a forked child: the syscall table, the kernel objects and any host thread a
 * handler starts die with it, and so does a case the HLE stops (named stop, exit status 70), crashes,
 * or hangs. The parent turns each of those into a row, so one broken slot costs one case:
 *
 *   CASE,<id>,stop,<kind>      a named stop (docs/STOPS.md), kind from its PS3STOP line
 *   CASE,<id>,hang,1           the per-case watchdog fired
 *   CASE,<id>,crash,<signal>   a fatal signal
 *   CASE,<id>,exit,<status>    the child ended without finishing the case (process-exit syscall, ...)
 *   SKIP,<id>,hw_only | threads
 *
 * The runner only runs cases and writes the result file. Comparing it with the hardware golden is
 * tools/hwprobe/probes/lv2conf/compare_lv2conf.py, the one comparator (driven by run_lv2conf.py).
 *
 * Exit status: 0 the cases ran, 1 the registry is malformed or a case finished and recorded nothing,
 * 2 usage or I/O error. A stopped, hung or crashed case is a result, not a failure.
 */
/* glibc declares Dl_info and dladdr (handler_name) only under _GNU_SOURCE, which has to be set before
 * the first system header of the translation unit. Darwin declares them unconditionally. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <dlfcn.h>

#include "conf.h"
#include "lv2conf_host.h"
#include "ps3emu/stop.h"

#ifndef LV2CONF_TOOLKIT_SHA
#define LV2CONF_TOOLKIT_SHA "unknown"
#endif

#define DEFAULT_TIMEOUT_S 10u
#define STDERR_TAIL_BYTES 65536

/* ---- per-case outcome -------------------------------------------------------------------------- */

enum outcome_kind { OC_RAN, OC_SKIP, OC_STOP, OC_HANG, OC_CRASH, OC_EXIT };

struct outcome {
    enum outcome_kind kind;
    unsigned rows; /* CASE and TIME rows the case itself wrote */
};

static const char *group_of(unsigned i)
{
    return conf_cases[i].group;
}

static int valid_id(const char *id)
{
    if (!id || !*id)
        return 0;
    int dot = 0;
    for (const char *p = id; *p; p++) {
        if (*p == '.')
            dot = 1;
        else if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
            return 0;
    }
    return dot;
}

/* The registry is the input of the whole run; a malformed one is a bug of the case sources. */
static int check_registry(void)
{
    int bad = 0;
    if (conf_case_count == 0) {
        fprintf(stderr, "lv2conf: the case registry is empty\n");
        return 1;
    }
    for (unsigned i = 0; i < conf_case_count; i++) {
        const struct conf_case *c = &conf_cases[i];
        if (!valid_id(c->id) || !c->group || !c->fn) {
            fprintf(stderr, "lv2conf: registry entry %u has a bad id, group or function\n", i);
            bad = 1;
            continue;
        }
        size_t glen = strlen(c->group);
        if (strncmp(c->id, c->group, glen) != 0 || c->id[glen] != '.') {
            fprintf(stderr, "lv2conf: case %s is not in its group %s\n", c->id, c->group);
            bad = 1;
        }
        for (unsigned j = 0; j < i; j++) {
            if (strcmp(conf_cases[j].id, c->id) == 0) {
                fprintf(stderr, "lv2conf: duplicate case id %s\n", c->id);
                bad = 1;
            }
        }
    }
    return bad;
}

/* ---- running one case -------------------------------------------------------------------------- */

static unsigned g_timeout_s = DEFAULT_TIMEOUT_S;

/* The kind of the last `PS3STOP v=1 kind=<kind>` line in the child's captured output. */
static void stop_line(FILE *cap, char *kind, size_t kind_n, char *line_out, size_t line_n)
{
    snprintf(kind, kind_n, "unknown");
    line_out[0] = '\0';
    fseek(cap, 0, SEEK_END);
    long end = ftell(cap);
    long start = end > STDERR_TAIL_BYTES ? end - STDERR_TAIL_BYTES : 0;
    fseek(cap, start, SEEK_SET);
    char *buf = malloc((size_t)(end - start) + 1);
    if (!buf)
        return;
    size_t n = fread(buf, 1, (size_t)(end - start), cap);
    buf[n] = '\0';
    static const char tag[] = "PS3STOP v=1 kind=";
    char *last = NULL;
    for (char *p = buf; (p = strstr(p, tag)) != NULL; p += sizeof tag - 1)
        if (p == buf || p[-1] == '\n')
            last = p;
    if (last) {
        char *e = strchr(last, '\n');
        if (e)
            *e = '\0';
        snprintf(line_out, line_n, "%s", last);
        const char *k = last + sizeof tag - 1;
        size_t kl = strcspn(k, " \t");
        if (kl && kl < kind_n) {
            memcpy(kind, k, kl);
            kind[kl] = '\0';
        }
    }
    free(buf);
}

static void print_tail(FILE *cap, unsigned nlines)
{
    fseek(cap, 0, SEEK_END);
    long end = ftell(cap);
    long start = end > 4096 ? end - 4096 : 0;
    fseek(cap, start, SEEK_SET);
    char buf[4097];
    size_t n = fread(buf, 1, sizeof buf - 1, cap);
    buf[n] = '\0';
    char *p = buf + n;
    while (p > buf && nlines) {
        if (p > buf && p[-1] == '\n')
            p--;
        while (p > buf && p[-1] != '\n')
            p--;
        nlines--;
    }
    if (n && *p)
        fprintf(stderr, "    | %s%s", p, buf[n - 1] == '\n' ? "" : "\n");
}

/* "<handler>" for a table entry: its symbol when the dynamic loader can name one, else the address. */
static void handler_name(uintptr_t h, char *out, size_t n)
{
    Dl_info di;
    if (h && dladdr((void *)h, &di) && di.dli_sname)
        snprintf(out, n, "%s", di.dli_sname);
    else
        snprintf(out, n, "0x%llx", (unsigned long long)h);
}

static int run_case(unsigned idx, FILE *out, struct outcome *oc)
{
    const struct conf_case *c = &conf_cases[idx];
    FILE *rows = tmpfile();
    FILE *cap = tmpfile();
    if (!rows || !cap) {
        fprintf(stderr, "lv2conf: tmpfile failed: %s\n", strerror(errno));
        return -1;
    }
    /* Whatever the parent still has buffered would be flushed a second time by a child that calls exit(). */
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "lv2conf: fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        dup2(fileno(cap), STDOUT_FILENO);
        dup2(fileno(cap), STDERR_FILENO);
        alarm(g_timeout_s);
        lv2conf_host_begin_case(c->id, rows);
        c->fn();
        fflush(rows);
        lv2conf_host_status()->finished = 1;
        _exit(0);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "lv2conf: waitpid failed: %s\n", strerror(errno));
            return -1;
        }
    }

    unsigned nrows = 0;
    char line[1024];
    rewind(rows);
    while (fgets(line, sizeof line, rows)) {
        fputs(line, out);
        nrows++;
    }
    oc->rows = nrows;
    oc->kind = OC_RAN;

    const struct lv2conf_host_status *s = lv2conf_host_status();
    char hn[128];
    handler_name(s->sc_handler, hn, sizeof hn);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && s->finished) {
        fclose(rows);
        fclose(cap);
        return 0;
    }

    if (WIFEXITED(st) && WEXITSTATUS(st) == PS3_STOP_EXIT_CODE) {
        char kind[64], sl[768];
        stop_line(cap, kind, sizeof kind, sl, sizeof sl);
        fprintf(out, "CASE,%s,stop,%s\n", c->id, kind);
        oc->kind = OC_STOP;
        fprintf(stderr, "lv2conf: %s: STOP %s", c->id, kind);
        if (s->sc_num != UINT32_MAX)
            fprintf(stderr, " in syscall %u (%s)", s->sc_num, hn);
        fprintf(stderr, "\n    %s\n", sl);
    } else if (WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM) {
        fprintf(out, "CASE,%s,hang,1\n", c->id);
        oc->kind = OC_HANG;
        fprintf(stderr, "lv2conf: %s: HANG after %us", c->id, g_timeout_s);
        if (s->sc_num != UINT32_MAX)
            fprintf(stderr, " in syscall %u (%s)", s->sc_num, hn);
        fprintf(stderr, "\n");
        print_tail(cap, 3);
    } else if (WIFSIGNALED(st)) {
        fprintf(out, "CASE,%s,crash,%d\n", c->id, WTERMSIG(st));
        oc->kind = OC_CRASH;
        fprintf(stderr, "lv2conf: %s: CRASH signal %d", c->id, WTERMSIG(st));
        if (s->sc_num != UINT32_MAX)
            fprintf(stderr, " in syscall %u (%s)", s->sc_num, hn);
        fprintf(stderr, "\n");
        print_tail(cap, 3);
    } else {
        fprintf(out, "CASE,%s,exit,%d\n", c->id, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        oc->kind = OC_EXIT;
        fprintf(stderr, "lv2conf: %s: EXIT status %d", c->id, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        if (s->sc_num != UINT32_MAX)
            fprintf(stderr, " in syscall %u (%s)", s->sc_num, hn);
        fprintf(stderr, "\n");
        print_tail(cap, 3);
    }
    fclose(rows);
    fclose(cap);
    return 0;
}

/* ---- result file ------------------------------------------------------------------------------- */

static void write_header(FILE *out)
{
    fprintf(out, "HWPROBE,1\n");
    fprintf(out, "HDR,probe,lv2conf\n");
    fprintf(out, "HDR,probe_version,1\n");
    fprintf(out, "HDR,git_describe,unknown\n");
    fprintf(out, "HDR,source_sha256,unknown\n");
    fprintf(out, "HDR,fw_version,unknown\n");
    fprintf(out, "HDR,fw_type,host-hle\n");
    fprintf(out, "HDR,console_model,host\n");
    fprintf(out, "HDR,console_info_source,operator\n");
    fprintf(out, "HDR,config,default\n");
    fprintf(out, "HDR,tbfreq,%llu\n", (unsigned long long)LV2CONF_HOST_TBFREQ);
}

static void print_summary(const struct outcome *oc)
{
    printf("%-14s %6s %6s %6s %6s %6s %6s %6s\n", "group", "cases", "ran", "stop", "hang", "crash", "exit", "skip");
    unsigned tot[7] = { 0 };
    for (unsigned i = 0; i < conf_case_count; i++) {
        int first = 1;
        for (unsigned j = 0; j < i; j++)
            if (strcmp(group_of(j), group_of(i)) == 0)
                first = 0;
        if (!first)
            continue;
        unsigned n[7] = { 0 }; /* cases, ran, stop, hang, crash, exit, skip */
        for (unsigned j = 0; j < conf_case_count; j++) {
            if (strcmp(group_of(j), group_of(i)) != 0)
                continue;
            n[0]++;
            switch (oc[j].kind) {
            case OC_RAN:   n[1]++; break;
            case OC_STOP:  n[2]++; break;
            case OC_HANG:  n[3]++; break;
            case OC_CRASH: n[4]++; break;
            case OC_EXIT:  n[5]++; break;
            case OC_SKIP:  n[6]++; break;
            }
        }
        printf("%-14s %6u %6u %6u %6u %6u %6u %6u\n", group_of(i), n[0], n[1], n[2], n[3], n[4], n[5], n[6]);
        for (int k = 0; k < 7; k++)
            tot[k] += n[k];
    }
    printf("%-14s %6u %6u %6u %6u %6u %6u %6u\n", "total", tot[0], tot[1], tot[2], tot[3], tot[4], tot[5], tot[6]);
}

/* ---- main -------------------------------------------------------------------------------------- */

static int usage(void)
{
    fprintf(stderr, "usage: test_lv2conf --out FILE [--timeout SEC]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *out_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_path = argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            char *e;
            unsigned long v = strtoul(argv[++i], &e, 10);
            if (*e || !v || v > 3600)
                return usage();
            g_timeout_s = (unsigned)v;
        } else
            return usage();
    }
    if (!out_path)
        return usage();

    if (check_registry())
        return 1;
    if (lv2conf_host_init() != 0)
        return 2;

    FILE *out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "lv2conf: cannot write %s: %s\n", out_path, strerror(errno));
        return 2;
    }
    write_header(out);

    struct outcome *oc = calloc(conf_case_count, sizeof *oc);
    if (!oc)
        return 2;
    unsigned fails = 0;
    for (unsigned i = 0; i < conf_case_count; i++) {
        const struct conf_case *c = &conf_cases[i];
        if (c->flags & (CONF_HW_ONLY | CONF_THREADS)) {
            fprintf(out, "SKIP,%s,%s\n", c->id, (c->flags & CONF_HW_ONLY) ? "hw_only" : "threads");
            oc[i].kind = OC_SKIP;
            continue;
        }
        if (run_case(i, out, &oc[i]) != 0)
            return 2;
        if (oc[i].kind == OC_RAN && oc[i].rows == 0) {
            fprintf(stderr, "FAIL %s: the case finished and recorded nothing\n", c->id);
            fails++;
        }
    }
    unsigned n_ran = 0, n_skipped = 0;
    for (unsigned i = 0; i < conf_case_count; i++) {
        n_ran += oc[i].kind == OC_RAN;
        n_skipped += oc[i].kind == OC_SKIP;
    }
    /* META.cases_run counts the cases that returned; a stopped, hung or crashed case did not. */
    fprintf(out, "META,backend,host\n");
    fprintf(out, "META,toolkit,%s\n", LV2CONF_TOOLKIT_SHA);
    fprintf(out, "META,cases_total,%u\n", conf_case_count);
    fprintf(out, "META,cases_run,%u\n", n_ran);
    fprintf(out, "META,cases_skipped,%u\n", n_skipped);
    if (fclose(out) != 0) {
        fprintf(stderr, "lv2conf: writing %s failed\n", out_path);
        return 2;
    }
    print_summary(oc);

    if (fails) {
        fprintf(stderr, "lv2conf: %u check(s) failed\n", fails);
        return 1;
    }
    printf("lv2conf: result written to %s\n", out_path);
    return 0;
}

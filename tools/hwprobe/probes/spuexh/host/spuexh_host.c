/* spuexh_host - dry run of the spuexh probe without a console.
 *
 * Loads the probe's SPU ELF (the very file embedded in the console SELF) into the toolkit's SPU interpreter and runs
 * the shared supervisor (common/spuexh_core.c) against a host backend: SPU threads are interpreter contexts on host
 * threads, "main memory" is a byte array, the MFC DMA the program uses is memcpy. The output is the file the console
 * run writes (hash rows in the host harness' line format), so it can be compared with the host harness (scalar.txt):
 * a difference is a defect of the SPU adapters, the generator on SPU, or the toolkit's interpreter.
 *
 *   spuexh_host --elf F [--preset P] [--set key=val]... [--cfg FILE] [--out FILE] [--no-sweep]
 *               [--special-a] [--special-b] [--plan] [--inject SPU:N]     (lanes= / lanesum= keys: RUN.md "Lanes")
 *
 * The interpreter itself is host_interp.c (the repository's interpreter source, included there).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "../common/spuexh_core.h"
#include "host_interp.h"

double spuexh_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* ---- guest memory */
#define GUEST_SIZE (192u << 20)
#define GUEST_BASE 0x10000u
static uint8_t *g_guest;
static size_t g_guest_used;
static uint8_t *g_elf; static size_t g_elf_len;
static uint64_t g_host_steps;
static int g_inject_spu = -1, g_inject_n, g_inject_group;
static int g_hang_exit_spu = -1;   /* --inject-hang-exit: that SPU hangs after its final status instead of exiting */

static void *host_alloc(spuexh_backend_t *be, size_t size, uint64_t *ea) {
    (void)be;
    size = (size + 127) & ~(size_t)127;
    if (g_guest_used + size > GUEST_SIZE - GUEST_BASE) return NULL;
    void *p = g_guest + GUEST_BASE + g_guest_used;
    *ea = GUEST_BASE + g_guest_used;
    g_guest_used += size;
    memset(p, 0, size);
    return p;
}

static int g_inject_fail_connect, g_inject_fail_terminate;   /* fault injection for the launch policy (see --inject-fail) */

#define HOST_PENDING_EXIT_CODE 0x8001FFFF   /* modeled sysSpuThreadGetExitStatus code for "not exited" (not lv2's) */

typedef struct { spuexh_progress_fn fn; void *u; } poll_t;
static int poll_thunk(void *u) { poll_t *p = (poll_t *)u; return p->fn ? p->fn(p->u) : 0; }

static int hr_fault(const char *d) { return strstr(d, "injected fault") || strstr(d, "group ended by fault"); }

static int host_run(spuexh_backend_t *be, int n, const uint64_t *param_ea, const volatile spuexh_status_t *const *st,
                    spuexh_progress_fn progress, void *pu, int limit_ms, spuexh_thread_res_t *res) {
    (void)be; (void)st;
    int idx[6] = {0}; hi_res_t hr[6]; poll_t pt = { progress, pu };
    /* the console backend's rules, modeled here so the dry run exercises the same supervisor paths */
    if (limit_ms <= 0) { snprintf(res[0].desc, sizeof res[0].desc, "no launch limit given"); res[0].fatal = 1; return -3; }
    if (g_inject_fail_connect) {     /* the console backend failing to connect the exception queue: nothing starts */
        snprintf(res[0].desc, sizeof res[0].desc, "injected connect failure 0x80010001 (group destroyed, nothing started)");
        res[0].fatal = 1;
        return -3;
    }
    for (int i = 0; i < n; i++) idx[i] = (int)W32(((const spuexh_params_t *)(g_guest + param_ea[i]))->spu_index);
    if (hi_run(n, idx, param_ea, g_guest, GUEST_SIZE, g_elf, g_elf_len, g_inject_spu, g_inject_n, g_inject_group,
               g_hang_exit_spu, poll_thunk, &pt, limit_ms, hr) < 0) return -1;
    /* The console backend joins only after every thread reported exited or a terminate succeeded; a group exception
     * also forces the terminate. Model the same: a fault, a thread that stopped without exiting, or a supervisor end
     * means terminate first. Terminate "succeeds" here unless the test asks it to fail. */
    int fault = 0, not_exited = 0;
    for (int i = 0; i < n; i++) { fault |= hr_fault(hr[i].desc); not_exited |= !hr[i].exited; }
    if (fault || not_exited) {
        if (g_inject_fail_terminate) {
            for (int i = 0; i < n; i++) {
                snprintf(res[i].desc, sizeof res[i].desc, "terminate 0x80010009 (injected) FATAL: no join after a failed terminate %s",
                         hr[i].desc);
                res[i].fatal = 1; res[i].timed_out = 1;
                res[i].exit_status_pending = HOST_PENDING_EXIT_CODE;
            }
            return -3;
        }
        for (int i = 0; i < n; i++) {
            snprintf(res[i].desc, sizeof res[i].desc, "terminate 0x0 (modeled) terminate-join %s", hr[i].desc);
            res[i].exited = hr[i].exited; res[i].timed_out = hr[i].timed_out;
            res[i].terminate_join = 1;
            res[i].exit_status_pending = hr[i].exited ? 0 : HOST_PENDING_EXIT_CODE;
            g_host_steps += hr[i].steps;
        }
        return 0;
    }
    for (int i = 0; i < n; i++) {
        snprintf(res[i].desc, sizeof res[i].desc, "%s", hr[i].desc);
        res[i].exited = hr[i].exited; res[i].timed_out = hr[i].timed_out;
        g_host_steps += hr[i].steps;
    }
    return 0;
}

static int load_elf(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END); g_elf_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    g_elf = malloc(g_elf_len); if (fread(g_elf, 1, g_elf_len, f) != g_elf_len) { fclose(f); return -1; }
    fclose(f);
    if (g_elf_len < 52 || memcmp(g_elf, "\177ELF", 4) || g_elf[4] != 1 || g_elf[5] != 2) { fprintf(stderr, "%s: not a 32-bit big-endian ELF\n", path); return -1; }
    return 0;
}

/* ---- command line */
static void trim(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0; }

int main(int argc, char **argv) {
    const char *elf = NULL, *outp = NULL, *cfgp = NULL, *preset = NULL, *tag = "dryrun";
    char sets[32][160]; int nsets = 0, plan_only = 0;
    int force_sweep = -1, force_a = -1, force_b = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--elf") && i + 1 < argc) elf = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outp = argv[++i];
        else if (!strcmp(argv[i], "--cfg") && i + 1 < argc) cfgp = argv[++i];
        else if (!strcmp(argv[i], "--preset") && i + 1 < argc) preset = argv[++i];
        else if (!strcmp(argv[i], "--tag") && i + 1 < argc) tag = argv[++i];
        else if (!strcmp(argv[i], "--set") && i + 1 < argc && nsets < 32) snprintf(sets[nsets++], 160, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--no-sweep")) force_sweep = 0;
        else if (!strcmp(argv[i], "--special-a")) force_a = 1;
        else if (!strcmp(argv[i], "--special-b")) force_b = 1;
        else if (!strcmp(argv[i], "--plan")) plan_only = 1;
        else if (!strcmp(argv[i], "--gencheck")) {      /* the digests the SPU must reproduce, computed natively */
            exh_init();
            for (int d = 0; d < SPUEXH_NDOMS_GEN; d++) printf("#gencheck\t%s\t%016llx\n", spuexh_dom_names[d], (unsigned long long)spuexh_gencheck(d));
            return 0;
        }
        else if (!strcmp(argv[i], "--inject") && i + 1 < argc) {
            /* --inject SPU:N kills that SPU at its N-th DMA put; --inject group:SPU:N ends the whole group there
             * (the console's group-exception model, for the collateral-FAULT tests) */
            if (sscanf(argv[++i], "group:%d:%d", &g_inject_spu, &g_inject_n) == 2) g_inject_group = 1;
            else if (sscanf(argv[i], "%d:%d", &g_inject_spu, &g_inject_n) != 2) return 2;
        }
        else if (!strcmp(argv[i], "--inject-hang-exit") && i + 1 < argc) {
            /* the named SPU reaches its final status and then never leaves the thread (lv2 never reports it exited):
             * the launch must go through the terminate path instead of a join */
            if (sscanf(argv[++i], "%d", &g_hang_exit_spu) != 1) return 2;
        }
        else if (!strcmp(argv[i], "--inject-fail") && i + 1 < argc) {
            /* launch-policy fault injection: connect = the exception queue connect fails before anything starts,
             * terminate = terminate reports an error after the supervisor ended a group (both make the launch fatal) */
            const char *w = argv[++i];
            if (!strcmp(w, "connect")) g_inject_fail_connect = 1;
            else if (!strcmp(w, "terminate")) g_inject_fail_terminate = 1;
            else { fprintf(stderr, "unknown --inject-fail target %s (connect, terminate)\n", w); return 2; }
        }
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    spuexh_runcfg_t rc; char err[200];
    spuexh_runcfg_defaults(&rc, preset ? preset : "smoke");
    if (cfgp) {
        FILE *f = fopen(cfgp, "r"); char line[320];
        if (!f) { perror(cfgp); return 2; }
        while (fgets(line, sizeof line, f)) {
            trim(line);
            if (!line[0] || line[0] == '#') continue;
            char *eq = strchr(line, '=');
            if (!eq) { fprintf(stderr, "%s: line without '=': %s\n", cfgp, line); return 2; }     /* a typo must not be skipped */
            *eq++ = 0;
            int r = spuexh_runcfg_set(&rc, line, eq, err, sizeof err);
            if (r < 0) { fprintf(stderr, "%s: %s\n", cfgp, err); return 2; }
        }
        fclose(f);
    }
    for (int i = 0; i < nsets; i++) {
        char *eq = strchr(sets[i], '=');
        if (!eq) { fprintf(stderr, "--set %s: expected key=value\n", sets[i]); return 2; }
        *eq++ = 0;
        if (spuexh_runcfg_set(&rc, sets[i], eq, err, sizeof err) < 0) { fprintf(stderr, "--set %s: %s\n", sets[i], err); return 2; }
    }
    if (spuexh_runcfg_finish(&rc, err, sizeof err) < 0) { fprintf(stderr, "%s\n", err); return 2; }
    if (force_sweep >= 0) rc.sweep = force_sweep;
    if (force_a >= 0) rc.special_a = force_a; else if (!cfgp && !preset) rc.special_a = 0;
    if (force_b >= 0) rc.special_b = force_b; else if (!cfgp && !preset) rc.special_b = 0;

    if (plan_only) {
        spuexh_plan_t p; spuexh_plan(&rc.wire, &p);
        printf("plan preset=%s jobs=%llu hashes=%llu calls=%llu gen_calls=%llu\n", rc.preset, (unsigned long long)p.jobs,
               (unsigned long long)p.hashes, (unsigned long long)p.calls, (unsigned long long)p.gen_calls);
        for (int d = 0; d < SPUEXH_NDOMS_GEN; d++) {
            spuexh_cfg_t c = rc.wire;
            for (int e = 0; e < SPUEXH_NDOMS_GEN; e++) spuexh_set_dom(&c, e, e == d && spuexh_dom_enabled(&rc.wire, d));
            spuexh_plan(&c, &p);
            if (p.jobs) printf("dom %s jobs=%llu hashes=%llu calls=%llu gen_calls=%llu\n", spuexh_dom_names[d], (unsigned long long)p.jobs,
                               (unsigned long long)p.hashes, (unsigned long long)p.calls, (unsigned long long)p.gen_calls);
        }
        return 0;
    }
    if (!elf) { fprintf(stderr, "--elf is required\n"); return 2; }
    if (load_elf(elf) < 0) return 2;
    g_guest = calloc(1, GUEST_SIZE);
    if (!g_guest) return 2;

    FILE *out = outp ? fopen(outp, "w") : stdout;
    if (!out) { perror(outp); return 2; }
    fprintf(out, "HWPROBE,1\nHDR,probe,spuexh\nHDR,probe_version,1\nHDR,git_describe,unknown\nHDR,source_sha256,unknown\n"
                 "HDR,fw_version,unknown\nHDR,fw_type,host-interp\nHDR,console_model,host\nHDR,console_info_source,unavailable\n"
                 "HDR,config,%s\nHDR,tbfreq,79800000\n", tag);
    fprintf(out, "META,backend,host-interp\nMETA,preset,%s\nMETA,spus_requested,%d\n", rc.preset, rc.spus);
    spuexh_backend_t be = { "host-interp", NULL, host_alloc, host_run };
    spuexh_sink_t sink = { out, NULL, NULL };
    int rcode = 0;
    if (rc.special_a) spuexh_run_special_a(&be, &rc, &sink);
    if (rc.special_b) spuexh_run_special_b(&be, &rc, &sink);
    if ((rc.nlanes || rc.nsums) && spuexh_run_lanes(&be, &rc, &sink) < 0) rcode = 1;
    if (rc.sweep && !rcode) {
        spuexh_sweep_res_t r;
        rcode = spuexh_run_sweep(&be, &rc, &sink, &r);
        fprintf(out, "META,hashes,%llu\nMETA,jobs_done,%llu\nMETA,faults,%llu\nMETA,relaunches,%llu\nMETA,timing_overflows,%llu\nMETA,completed,%d\nMETA,aborted,%d\nMETA,wall_s,%.1f\n",
                (unsigned long long)r.hashes, (unsigned long long)r.jobs_done, (unsigned long long)r.faults, (unsigned long long)r.relaunches,
                (unsigned long long)r.timing_overflows, r.completed, r.aborted, r.wall_s);
    }
    fprintf(out, "META,interp_steps,%llu\n", (unsigned long long)g_host_steps);
    if (outp) fclose(out);
    return rcode < 0 ? 1 : rcode;
}

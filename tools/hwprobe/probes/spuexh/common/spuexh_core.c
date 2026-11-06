/* spuexh_core.c - see spuexh_core.h. Plain C, no PSL1GHT and no host-only calls: compiled by ppu-gcc and by the host
 * driver. Time comes from spuexh_now(), defined by the layer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spuexh_core.h"
#include "spuexh_badprefix_gen.h"

const char *const spuexh_dpcmp_names[5] = { "dfceq", "dfcmeq", "dfcgt", "dfcmgt", "dftsv" };

/* ------------------------------------------------------------------ configuration */
static void cfg_clear(spuexh_cfg_t *c) {
    memset(c, 0, sizeof *c);
    c->magic = W32(SPUEXH_CFG_MAGIC);
    c->stride_all = W32(1u); c->stride_heavy = W32(1u);
    for (int k = 0; k < SPUEXH_NOPS; k++) spuexh_set_op(c, k, 1);
    for (int d = 0; d < SPUEXH_NDOMS_GEN; d++) spuexh_set_dom(c, d, 1);
}

int spuexh_runcfg_defaults(spuexh_runcfg_t *rc, const char *preset) {
    if (!preset) preset = "smoke";
    memset(rc, 0, sizeof *rc);
    cfg_clear(&rc->wire);
    rc->spus = 6; rc->stall_s = 120; rc->max_faults = 24;
    rc->special_a = 1; rc->special_b = 0; rc->special_b_max = 0; rc->case_timeout_ms = 2000; rc->b_timeout_ms = 600;
    rc->launch_timeout_ms = 10800000; rc->special_b_count = 0; rc->special_b_both = 1;
    rc->auto_dpcmp = 1; rc->sweep = 1;
    snprintf(rc->preset, sizeof rc->preset, "%s", preset);
    if (!strcmp(preset, "smoke")) { rc->wire.bmax = W32(1u); }                       /* block 0 of every domain, every op */
    else if (!strcmp(preset, "standard")) { rc->wire.stride_all = W32(64u); rc->wire.stride_heavy = W32(64u); }
    else if (!strcmp(preset, "full")) { }                                              /* every block, heavy conversions too */
    else return -1;
    return 0;
}

static int dom_index(const char *n) {
    for (int d = 0; d < SPUEXH_NDOMS_GEN; d++) if (!strcmp(spuexh_dom_names[d], n)) return d;
    return -1;
}
static int name_match(const char *pat, const char *s) {       /* comma list, trailing * = prefix */
    char buf[512]; snprintf(buf, sizeof buf, "%s", pat);
    for (char *t = buf, *e; t; t = e ? e + 1 : NULL) {
        e = strchr(t, ','); if (e) *e = 0;
        size_t n = strlen(t);
        if (n && t[n - 1] == '*') { if (!strncmp(t, s, n - 1)) return 1; }
        else if (n && !strcmp(t, s)) return 1;
        if (!e) break;
    }
    return 0;
}

/* "NAME:S;NAME:S" into a per-domain table */
static int parse_dom_list(const char *val, uint32_t *tab, char *err, size_t cap) {
    char buf[256]; snprintf(buf, sizeof buf, "%s", val);
    for (char *t = buf, *e; t; t = e ? e + 1 : NULL) {
        e = strchr(t, ';'); if (e) *e = 0;
        char *colon = strchr(t, ':');
        if (!colon) { snprintf(err, cap, "expected NAME:N, got '%s'", t); return -1; }
        *colon = 0;
        int d = dom_index(t);
        if (d < 0) { snprintf(err, cap, "unknown domain '%s'", t); return -1; }
        tab[d] = W32((uint32_t)atoi(colon + 1));
        if (!e) break;
    }
    return 0;
}

int spuexh_runcfg_set(spuexh_runcfg_t *rc, const char *k, const char *v, char *err, size_t cap) {
    err[0] = 0;
    if (!strcmp(k, "preset")) {
        if (spuexh_runcfg_defaults(rc, v) < 0) { snprintf(err, cap, "unknown preset '%s' (smoke, standard, full)", v); return -1; }
    }
    else if (!strcmp(k, "spus")) { rc->spus = atoi(v); if (rc->spus < 1 || rc->spus > 6) { snprintf(err, cap, "spus must be 1..6"); return -1; } }
    else if (!strcmp(k, "stall_s")) { rc->stall_s = atoi(v); if (rc->stall_s < 0) { snprintf(err, cap, "stall_s must be >= 0"); return -1; } }
    else if (!strcmp(k, "max_faults")) { rc->max_faults = atoi(v); if (rc->max_faults < 0) { snprintf(err, cap, "max_faults must be >= 0"); return -1; } }
    else if (!strcmp(k, "special_a")) rc->special_a = atoi(v);
    else if (!strcmp(k, "special_b")) rc->special_b = atoi(v);
    else if (!strcmp(k, "special_b_max")) rc->special_b_max = atoi(v);
    else if (!strcmp(k, "special_b_from")) { rc->special_b_from = atoi(v); if (rc->special_b_from < 0) { snprintf(err, cap, "special_b_from must be >= 0"); return -1; } }
    else if (!strcmp(k, "special_b_count")) { rc->special_b_count = atoi(v); if (rc->special_b_count < 0) { snprintf(err, cap, "special_b_count must be >= 0"); return -1; } }
    else if (!strcmp(k, "special_b_patterns")) {
        if (!strcmp(v, "both")) rc->special_b_both = 1;
        else if (!strcmp(v, "zero")) rc->special_b_both = 0;
        else { snprintf(err, cap, "special_b_patterns must be zero or both, got '%s'", v); return -1; }
    }
    else if (!strcmp(k, "a_ops")) snprintf(rc->a_ops, sizeof rc->a_ops, "%s", v);
    /* every launch kind is bounded: 0 or a negative limit would be an unbounded launch (RUN.md policy) */
    else if (!strcmp(k, "case_timeout_ms")) { rc->case_timeout_ms = atoi(v); if (rc->case_timeout_ms <= 0) { snprintf(err, cap, "case_timeout_ms must be > 0"); return -1; } }
    else if (!strcmp(k, "b_timeout_ms")) { rc->b_timeout_ms = atoi(v); if (rc->b_timeout_ms <= 0) { snprintf(err, cap, "b_timeout_ms must be > 0"); return -1; } }
    else if (!strcmp(k, "launch_timeout_ms")) { rc->launch_timeout_ms = atoi(v); if (rc->launch_timeout_ms <= 0) { snprintf(err, cap, "launch_timeout_ms must be > 0"); return -1; } }
    else if (!strcmp(k, "sweep")) rc->sweep = atoi(v);
    else if (!strcmp(k, "force_dpcmp")) rc->force_dpcmp = atoi(v);
    else if (!strcmp(k, "stride_all")) rc->wire.stride_all = W32((uint32_t)atoi(v));
    else if (!strcmp(k, "stride_heavy")) rc->wire.stride_heavy = W32((uint32_t)atoi(v));
    else if (!strcmp(k, "bmax")) rc->wire.bmax = W32((uint32_t)atoi(v));
    else if (!strcmp(k, "stride_dom")) return parse_dom_list(v, rc->wire.dom_stride, err, cap) < 0 ? -1 : 1;
    else if (!strcmp(k, "bmax_dom")) return parse_dom_list(v, rc->wire.dom_bmax, err, cap) < 0 ? -1 : 1;
    else if (!strcmp(k, "ops")) snprintf(rc->ops, sizeof rc->ops, "%s", v);
    else if (!strcmp(k, "skip_ops")) snprintf(rc->skip_ops, sizeof rc->skip_ops, "%s", v);
    else if (!strcmp(k, "doms")) snprintf(rc->doms, sizeof rc->doms, "%s", v);
    else return 0;
    return 1;
}

int spuexh_runcfg_finish(spuexh_runcfg_t *rc, char *err, size_t cap) {
    err[0] = 0;
    int nop = 0, ndom = 0;
    for (int d = 0; d < SPUEXH_NDOMS_GEN; d++) {
        int on = !rc->doms[0] || name_match(rc->doms, spuexh_dom_names[d]);
        spuexh_set_dom(&rc->wire, d, on); ndom += on;
    }
    for (int k = 0; k < SPUEXH_NOPS; k++) {
        const char *n = spuexh_ops[k].name;
        int on = (!rc->ops[0] || name_match(rc->ops, n)) && !(rc->skip_ops[0] && name_match(rc->skip_ops, n));
        spuexh_set_op(&rc->wire, k, on); nop += on;
    }
    if (!ndom) { snprintf(err, cap, "doms='%s' matches no domain", rc->doms); return -1; }
    if (!nop) { snprintf(err, cap, "ops='%s' skip_ops='%s' leave no op", rc->ops, rc->skip_ops); return -1; }
    return 0;
}

int spuexh_outdir_ok(const char *dir) {
    static const char root[] = "/dev_hdd0/tmp/hwprobe";
    const size_t rl = sizeof root - 1;
    if (!dir[0] || strncmp(dir, root, rl)) return 0;
    if (dir[rl] && dir[rl] != '/') return 0;                 /* not /dev_hdd0/tmp/hwprobeX */
    /* no ".." path component: the results must stay inside the assigned directory */
    for (const char *p = dir;;) {
        while (*p == '/') p++;
        if (!*p) return 1;
        const char *e = strchr(p, '/');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (!e) return 1;
        p = e + 1;
    }
}

void spuexh_plan(const spuexh_cfg_t *c, spuexh_plan_t *p) {
    uint16_t act[SPUEXH_NOPS];
    memset(p, 0, sizeof *p);
    for (int d = 0; d < SPUEXH_NDOMS_GEN; d++) {
        if (!spuexh_dom_enabled(c, d)) continue;
        const uint64_t nb = exh_nblocks(&exh_doms[d]);
        for (uint64_t b = 0; b < nb; b++) {
            const int na = spuexh_job_ops(c, d, b, act);
            if (!na) continue;
            uint64_t lo = b << EXH_BLOCK_LOG2, hi = lo + EXH_BLOCK; if (hi > exh_doms[d].ncalls) hi = exh_doms[d].ncalls;
            p->jobs++; p->hashes += (uint64_t)na; p->calls += (uint64_t)na * (hi - lo); p->gen_calls += hi - lo;
        }
    }
}

/* ------------------------------------------------------------------ the sweep */
typedef struct {
    spuexh_backend_t *be;
    const spuexh_runcfg_t *rc;
    const spuexh_sink_t *sink;
    int n;                                   /* spu_count of this sweep */
    uint64_t cfg_ea;
    spuexh_params_t *par[6];  uint64_t par_ea[6];
    volatile spuexh_status_t *st[6];  uint64_t st_ea[6];
    spuexh_entry_t *reg[6];   uint64_t reg_ea[6];  uint32_t cap[6];
    uint32_t drained[6];
    uint32_t start_job[6], start_entries[6];
    int finished[6], dead[6], faults_seen[6];
    uint64_t hashes;
    uint64_t dom_ticks[SPUEXH_NDOMS_GEN], dom_jobs[SPUEXH_NDOMS_GEN];
    uint64_t jobs_total;
    uint64_t launches, timing_overflows;
    double t_change, t0, t_ui;
    uint64_t last_sig;
    int stalled, stopped;
} sweep_t;

static void drain(sweep_t *s, int i) {
    __sync_synchronize();
    uint32_t n = W32(s->st[i]->entries);
    __sync_synchronize();
    if (n > s->cap[i]) n = s->cap[i];
    for (uint32_t k = s->drained[i]; k < n; k++) {
        const spuexh_entry_t *e = &s->reg[i][k];
        const unsigned op = W16(e->op), dom = W16(e->dom);
        const uint64_t blk = W32(e->blk), h = W64(e->hash);
        if (dom >= SPUEXH_NDOMS_GEN) continue;
        if (op == SPUEXH_ENT_GEN) { fprintf(s->sink->out, "#gencheck\t%s\t%016llx\n", spuexh_dom_names[dom], (unsigned long long)h); continue; }
        if (op == SPUEXH_ENT_TIME) {
            if (h == SPUEXH_TIME_OVERFLOW) {
                /* the job ran past one decrementer period (~53.8 s at 79.8 MHz): the tick count is not a number */
                fprintf(s->sink->out, "TIMING_OVERFLOW,%s,%llu\n", spuexh_dom_names[dom], (unsigned long long)blk);
                s->timing_overflows++;
            } else {
                s->dom_ticks[dom] += h;
            }
            s->dom_jobs[dom]++; s->jobs_total++;
            continue;
        }
        if (op >= SPUEXH_NOPS) continue;
        fprintf(s->sink->out, "%s\t%s\t%llu\t%016llx\n", spuexh_ops[op].name, spuexh_dom_names[dom],
                (unsigned long long)blk, (unsigned long long)h);
        s->hashes++;
    }
    if (n > s->drained[i]) s->drained[i] = n;     /* never backwards: a relaunched SPU reports 0 until its first flush */
}

static int sweep_progress(void *u) {
    sweep_t *s = (sweep_t *)u;
    uint64_t sig = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->finished[i] || s->dead[i]) continue;
        drain(s, i);
        sig += W32(s->st[i]->jobs_started) * 3ull + W32(s->st[i]->jobs_done) * 5ull + W32(s->st[i]->entries) * 7ull +
               W64(s->st[i]->heartbeat) * 11ull + W32(s->st[i]->state);
    }
    const double now = spuexh_now();
    if (sig != s->last_sig) { s->last_sig = sig; s->t_change = now; }
    else if (s->rc->stall_s > 0 && now - s->t_change > s->rc->stall_s) { s->stalled = 1; return 1; }
    if (now - s->t_ui >= 1.0) {
        s->t_ui = now;
        fflush(s->sink->out);
        if (s->sink->ui) {
            char b[160]; snprintf(b, sizeof b, "hashes=%llu jobs=%llu %.0fs", (unsigned long long)s->hashes,
                                  (unsigned long long)s->jobs_total, now - s->t0);
            if (s->sink->ui(s->sink->uu, b)) { s->stopped = 1; return 1; }
        }
    }
    return 0;
}

static void clean(char *dst, size_t cap, const char *src) {          /* a field of a CSV row: no comma, tab or line break */
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = (src[i] == ',' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r') ? ' ' : src[i];
    dst[i] = 0;
}

/* META rows that describe how a launch ended, written once per launch right after the backend returned:
 *   META,terminate_join,launchN       the backend had to terminate the group before it could be joined (the group had
 *                                     not ended by itself, or an exception event arrived)
 *   META,exit_status_pending,0xCODE   a thread had not exited and sysSpuThreadGetExitStatus returned CODE (first
 *                                     occurrence per code; the codes document lv2's running/stopped-thread semantics,
 *                                     which no source in this tree settles)
 * g_launch_seq numbers every launch of the process (special probes and sweep together), so launchN is unique per file. */
static uint64_t g_launch_seq;
static uint32_t g_pending_codes[8];
static int g_npending_codes;

static void launch_meta(FILE *f, uint64_t launch, const spuexh_thread_res_t *tr, int n) {
    int tj = 0;
    for (int i = 0; i < n; i++) tj |= tr[i].terminate_join != 0;
    if (tj) fprintf(f, "META,terminate_join,launch%llu\n", (unsigned long long)launch);
    for (int i = 0; i < n; i++) {
        const uint32_t code = (uint32_t)tr[i].exit_status_pending;
        if (!code) continue;
        int seen = 0;
        for (int k = 0; k < g_npending_codes; k++) if (g_pending_codes[k] == code) seen = 1;
        if (seen) continue;
        if (g_npending_codes < (int)(sizeof g_pending_codes / sizeof g_pending_codes[0])) g_pending_codes[g_npending_codes++] = code;
        fprintf(f, "META,exit_status_pending,0x%08x\n", (unsigned)code);
    }
}

/* One thread-group launch of the SPUs in idx[0..n): fills the params blocks and runs the backend with the configured
 * launch limit (always positive: no unbounded launch). After the launch, launch_meta() records what the backend had to
 * do to join and one META row per thread records the FPSCR the SPU program read back after setting it to the defined
 * state at start (SPU ISA v1.2 section 9.3). Returns the rv. */
static int sweep_launch(sweep_t *s, const int *idx, int n, spuexh_thread_res_t *tr) {
    uint64_t pea[6];
    const volatile spuexh_status_t *stp[6];
    for (int q = 0; q < n; q++) {
        const int i = idx[q];
        spuexh_params_t *p = s->par[i];
        memset(p, 0, sizeof *p);
        p->magic = W32(SPUEXH_PARAMS_MAGIC); p->mode = W32(SPUEXH_MODE_MAIN);
        p->spu_index = W32((uint32_t)i); p->spu_count = W32((uint32_t)s->n);
        p->start_job = W32(s->start_job[i]); p->results_cap = W32(s->cap[i]); p->start_entries = W32(s->start_entries[i]);
        p->results_ea = W64(s->reg_ea[i]); p->status_ea = W64(s->st_ea[i]); p->cfg_ea = W64(s->cfg_ea);
        p->cfg_size = W32((uint32_t)sizeof(spuexh_cfg_t));
        memset((void *)s->st[i], 0, sizeof(spuexh_status_t));
        pea[q] = s->par_ea[i]; stp[q] = s->st[i];
    }
    __sync_synchronize();
    memset(tr, 0, sizeof(spuexh_thread_res_t) * 6);
    s->t_change = spuexh_now(); s->t_ui = 0; s->stalled = 0;
    const int rv = s->be->run(s->be, n, pea, stp, sweep_progress, s, s->rc->launch_timeout_ms, tr);
    s->launches++;
    const uint64_t ln = ++g_launch_seq;
    launch_meta(s->sink->out, ln, tr, n);
    for (int q = 0; q < n; q++) {
        const volatile spuexh_status_t *st = s->st[idx[q]];
        fprintf(s->sink->out, "META,launch%llu_spu%d_fpscr,%08x.%08x.%08x.%08x\n", (unsigned long long)ln, idx[q],
                W32(st->fpscr_start[0]), W32(st->fpscr_start[1]), W32(st->fpscr_start[2]), W32(st->fpscr_start[3]));
    }
    return rv;
}

/* The FAULT row of a worker that did not finish: the job it was running, its state word and what the backend reported.
 * The job is skipped (start_job moves past it); a worker that keeps losing jobs without starting a new one is marked dead. */
static void sweep_fault_row(sweep_t *s, int i, const spuexh_thread_res_t *tr, FILE *f) {
    const uint32_t started = W32(s->st[i]->jobs_started);
    const uint32_t dom = W32(s->st[i]->last_dom), blk = W32(s->st[i]->last_blk);
    char d1[200];
    clean(d1, sizeof d1, tr->desc);
    fprintf(f, "FAULT,spu%d,%s,%u,job_%u,state_%u,%s%s\n", i,
            dom < SPUEXH_NDOMS_GEN ? spuexh_dom_names[dom] : "none", blk, started, W32(s->st[i]->state),
            s->stalled || tr->timed_out ? "hang:" : "", d1);
    s->faults_seen[i]++;
    if (started > s->start_job[i]) s->start_job[i] = started;
    else if (s->faults_seen[i] > 2) { s->dead[i] = 1; fprintf(f, "META,spu%d_dead,no_progress\n", i); }
    s->start_entries[i] = s->drained[i];
}

/* A group-ending fault or kill describes every unfinished thread with the same event, so a healthy worker may have been
 * collateral. Re-run this worker's unfinished jobs once, alone (single-SPU launch from the interrupted job): only a job
 * that faults there is a FAULT; one that completes is a RETRY, and its hashes are kept. Returns -1 on FATAL. */
static int sweep_verify_alone(sweep_t *s, int i, FILE *f, spuexh_sweep_res_t *res, char *fatal_desc, size_t dcap) {
    const int idx[1] = { i };
    spuexh_thread_res_t tr[6];
    const uint32_t interrupted = W32(s->st[i]->jobs_started);    /* the job that was running when the group died */
    const int rv = sweep_launch(s, idx, 1, tr);
    if (rv == -3) {
        clean(fatal_desc, dcap, tr[0].desc);
        return -1;                               /* uncontainable: the caller stops the whole probe */
    }
    if (rv == -2) {                              /* the launch never ran: retry the job, it is not known to be bad */
        s->stalled = 0;
        return 0;
    }
    drain(s, i);
    if (rv < 0) {                                /* backend refused the launch: keep the skip-job behaviour */
        sweep_fault_row(s, i, &tr[0], f);
        res->faults++;
        return 0;
    }
    if (W32(s->st[i]->state) == 2) {
        fprintf(f, "RETRY,spu%d,job_%u,ok\n", i, interrupted);
        s->finished[i] = 1;
        return 0;
    }
    sweep_fault_row(s, i, &tr[0], f);            /* it faults alone: this job is the FAULT */
    res->faults++;
    return 0;
}

int spuexh_run_sweep(spuexh_backend_t *be, const spuexh_runcfg_t *rc0, const spuexh_sink_t *sink, spuexh_sweep_res_t *res) {
    static sweep_t S;                                       /* large; one sweep at a time */
    memset(&S, 0, sizeof S);
    memset(res, 0, sizeof *res);
    spuexh_runcfg_t rc = *rc0;                              /* the wire config gets the DP-compare filter below */
    if (!rc.force_dpcmp) {
        for (int q = 0; q < 5; q++) {
            if (rc.dpcmp_known && rc.dpcmp_ok[q]) continue;
            for (int k = 0; k < SPUEXH_NOPS; k++)
                if (!strcmp(spuexh_ops[k].name, spuexh_dpcmp_names[q]) && spuexh_op_enabled(&rc.wire, k)) {
                    spuexh_set_op(&rc.wire, k, 0);
                    fprintf(sink->out, "SKIP,%s,%s\n", spuexh_ops[k].name, rc.dpcmp_known ? "rejected_by_special_a" : "special_a_not_run");
                }
        }
    }
    S.be = be; S.rc = &rc; S.sink = sink;
    const double t_begin = spuexh_now();
    uint16_t act[SPUEXH_NOPS];

    for (int spus = rc.spus; spus >= 1; spus--) {
        /* ---- plan: entries each SPU will produce (upper bound = every job completes) */
        uint64_t ents[6] = {0}; uint32_t njobs[6] = {0}; uint32_t job = 0;
        for (int d = 0; d < SPUEXH_NDOMS_GEN; d++) {
            if (!spuexh_dom_enabled(&rc.wire, d)) continue;
            const uint64_t nb = exh_nblocks(&exh_doms[d]);
            for (uint64_t b = 0; b < nb; b++) {
                const int na = spuexh_job_ops(&rc.wire, d, b, act);
                if (!na) continue;
                const int o = (int)(job++ % (uint32_t)spus);
                ents[o] += (uint64_t)na + 1; njobs[o]++;
            }
        }
        ents[0] += SPUEXH_NDOMS_GEN;                          /* SPU 0 also emits the generator self-check */
        S.n = spus;
        int ok = 1;
        void *cfgp = NULL; uint64_t cfg_ea = 0;
        cfgp = be->alloc(be, 512, &cfg_ea);
        if (!cfgp) ok = 0; else memcpy(cfgp, &rc.wire, sizeof rc.wire);
        for (int i = 0; ok && i < spus; i++) {
            S.par[i] = (spuexh_params_t *)be->alloc(be, sizeof(spuexh_params_t), &S.par_ea[i]);
            S.st[i] = (volatile spuexh_status_t *)be->alloc(be, sizeof(spuexh_status_t), &S.st_ea[i]);
            S.cap[i] = (uint32_t)(ents[i] + 16);
            S.reg[i] = (spuexh_entry_t *)be->alloc(be, (size_t)S.cap[i] * sizeof(spuexh_entry_t), &S.reg_ea[i]);
            if (!S.par[i] || !S.st[i] || !S.reg[i]) ok = 0;
        }
        if (!ok) { fprintf(sink->out, "META,sweep_error,allocation_failed\n"); res->aborted = 1; return -1; }
        S.cfg_ea = cfg_ea;
        memset(S.drained, 0, sizeof S.drained); memset(S.start_job, 0, sizeof S.start_job); memset(S.start_entries, 0, sizeof S.start_entries);
        memset(S.finished, 0, sizeof S.finished); memset(S.dead, 0, sizeof S.dead); memset(S.faults_seen, 0, sizeof S.faults_seen);
        for (int i = 0; i < spus; i++) if (njobs[i] == 0) S.finished[i] = 1;   /* nothing to do for this SPU */
        res->spus_used = spus;
        fprintf(sink->out, "META,spus,%d\n", spus);

        /* ---- launch / fault loop */
        int launched_once = 0;
        for (int attempt = 0;; attempt++) {
            int idx[6], n = 0;
            for (int i = 0; i < spus; i++) if (!S.finished[i] && !S.dead[i]) idx[n++] = i;
            if (!n) break;
            if (attempt > rc.max_faults) { res->aborted = 1; fprintf(sink->out, "META,sweep_error,max_faults_reached\n"); break; }
            if (!launched_once) S.t0 = spuexh_now();
            spuexh_thread_res_t tr[6];
            const int rv = sweep_launch(&S, idx, n, tr);
            if (rv == -3) {
                /* terminate failed and the group may still be live: record it flushed and stop the whole probe
                 * without a join (process exit lets lv2 reclaim the group; RUN.md policy) */
                char d1[200];
                clean(d1, sizeof d1, tr[0].desc);
                fprintf(sink->out, "FATAL,launch,%s\nMETA,sweep_error,fatal_launch\n", d1);
                fflush(sink->out);
                res->aborted = 1;
                return -1;
            }
            if (rv == -2 && !launched_once) { fprintf(sink->out, "META,spus_unavailable,%d\n", spus); ok = 0; break; }
            launched_once = 1;
            if (rv == -2) continue;                 /* a launch failed before any thread ran: retry the group as it is */
            if (rv < 0) { res->aborted = 1; fprintf(sink->out, "META,sweep_error,backend_run_failed_%d\n", rv); break; }
            res->relaunches = S.launches - 1;
            for (int q = 0; q < n; q++) drain(&S, idx[q]);
            int all_done = 1;
            for (int q = 0; q < n; q++) {
                const uint32_t st = W32(S.st[idx[q]]->state);
                if (st != 2 && st != 3) all_done = 0;
            }
            /* worker accounting: finished threads are done. A thread that did not finish is declared FAULT only when the
             * group ended by itself (its own state is 3) or the operator stopped us; after a group-wide fault, hang or
             * launch limit it gets its own single-SPU verification launch first (RETRY vs FAULT). */
            int fatal = 0;
            char d1fatal[200];
            for (int q = 0; q < n; q++) {
                const int i = idx[q];
                if (W32(S.st[i]->state) == 2) { S.finished[i] = 1; continue; }
                if (all_done || S.stopped) {
                    sweep_fault_row(&S, i, &tr[q], sink->out);
                    res->faults++;
                } else if (sweep_verify_alone(&S, i, sink->out, res, d1fatal, sizeof d1fatal) < 0) {
                    fatal = 1;
                    break;
                }
            }
            if (fatal) {
                fprintf(sink->out, "FATAL,launch,%s\nMETA,sweep_error,fatal_launch\n", d1fatal);
                fflush(sink->out);
                res->aborted = 1;
                return -1;
            }
            if (S.stopped) { res->aborted = 1; fprintf(sink->out, "META,sweep_error,stopped_by_operator\n"); break; }
            fflush(sink->out);
        }
        if (!ok && !launched_once) continue;                 /* try one SPU fewer */
        break;
    }
    res->completed = 1;
    for (int i = 0; i < S.n; i++) { if (!S.finished[i]) res->completed = 0; if (S.dead[i]) res->aborted = 1; }
    res->hashes = S.hashes; res->jobs_done = S.jobs_total; res->timing_overflows = S.timing_overflows;
    for (int i = 0; i < S.n; i++) {
        res->busy_ticks += W64(S.st[i]->busy_ticks); res->entries += S.drained[i];
        char b[128];
        snprintf(b, sizeof b, "%08x.%08x.%08x.%08x", W32(S.st[i]->fpscr_end[0]), W32(S.st[i]->fpscr_end[1]), W32(S.st[i]->fpscr_end[2]), W32(S.st[i]->fpscr_end[3]));
        fprintf(sink->out, "META,spu%d_fpscr_end,%s\n", i, b);
        fprintf(sink->out, "META,spu%d_busy_ticks,%llu\n", i, (unsigned long long)W64(S.st[i]->busy_ticks));
    }
    for (int d = 0; d < SPUEXH_NDOMS_GEN; d++)
        if (S.dom_jobs[d]) fprintf(sink->out, "#hwdom\t%s\tjobs=%llu\tticks=%llu\n", spuexh_dom_names[d],
                                   (unsigned long long)S.dom_jobs[d], (unsigned long long)S.dom_ticks[d]);
    res->wall_s = spuexh_now() - t_begin;
    fflush(sink->out);
    return res->completed ? 0 : 1;
}

/* ------------------------------------------------------------------ special probes */
typedef struct {
    spuexh_case_t cases[96];
    spuexh_caseout_t out[96];
} spec_buf_t;

static const spuexh_op_t *find_op(const char *name) {
    for (int k = 0; k < SPUEXH_NOPS; k++) if (!strcmp(spuexh_ops[k].name, name)) return &spuexh_ops[k];
    return NULL;
}

/* Runs the n cases in one group (mode CASES or RAW). Returns the number of cases that reached state 2, -1 when the
 * group was stopped by the supervisor or faulted (the callers re-run the cases one by one), or -2 when the launch
 * could not be contained: the callers record the FATAL row the backend's res describes and stop the probe.
 * Every launch also writes its launch_meta() and FPSCR META rows (RUN.md: one row per launch). */
static int run_cases(spuexh_backend_t *be, int mode, const spuexh_case_t *cs, int n, int limit_ms,
                     spuexh_caseout_t *outs, spuexh_thread_res_t *tr, uint32_t *started_out, FILE *f) {
    static spuexh_params_t *par; static volatile spuexh_status_t *st; static spuexh_case_t *cbuf; static spuexh_caseout_t *obuf;
    static uint64_t par_ea, st_ea, c_ea, o_ea;
    if (!par) {
        par = (spuexh_params_t *)be->alloc(be, sizeof *par, &par_ea);
        st = (volatile spuexh_status_t *)be->alloc(be, sizeof *st, &st_ea);
        cbuf = (spuexh_case_t *)be->alloc(be, 96 * sizeof(spuexh_case_t), &c_ea);
        obuf = (spuexh_caseout_t *)be->alloc(be, 96 * sizeof(spuexh_caseout_t), &o_ea);
        if (!par || !st || !cbuf || !obuf) { par = NULL; return -1; }
    }
    memcpy(cbuf, cs, (size_t)n * sizeof *cs);
    memset(obuf, 0, 96 * sizeof(spuexh_caseout_t));
    memset((void *)st, 0, sizeof *st);
    memset(par, 0, sizeof *par);
    par->magic = W32(SPUEXH_PARAMS_MAGIC); par->mode = W32((uint32_t)mode); par->spu_index = 0; par->spu_count = W32(1u);
    par->ncases = W32((uint32_t)n); par->results_ea = W64(o_ea); par->status_ea = W64(st_ea); par->cases_ea = W64(c_ea);
    __sync_synchronize();
    const volatile spuexh_status_t *stp[1] = { st };
    memset(tr, 0, sizeof *tr);
    const int rv = be->run(be, 1, &par_ea, stp, NULL, NULL, limit_ms, tr);
    __sync_synchronize();
    const uint64_t ln = ++g_launch_seq;
    launch_meta(f, ln, tr, 1);
    fprintf(f, "META,launch%llu_spu0_fpscr,%08x.%08x.%08x.%08x\n", (unsigned long long)ln,
            W32(st->fpscr_start[0]), W32(st->fpscr_start[1]), W32(st->fpscr_start[2]), W32(st->fpscr_start[3]));
    if (started_out) *started_out = W32(st->cases_started);
    if (rv == -3) return -2;
    int ok = 0;
    for (int i = 0; i < n; i++) {
        memcpy(&outs[i], &obuf[i], sizeof outs[i]);
        if (W32(outs[i].state) == 2) ok++;
    }
    return rv < 0 ? -1 : ok;
}

static void hex128(char *dst, const uint32_t *w) {
    snprintf(dst, 40, "%08x%08x%08x%08x", W32(w[0]), W32(w[1]), W32(w[2]), W32(w[3]));
}

/* The FATAL row of a launch the backend could not contain (see backend contract): flushed at once, the probe stops. */
static void fatal_row(FILE *f, const spuexh_thread_res_t *tr, const char *where) {
    char d1[200];
    clean(d1, sizeof d1, tr->desc);
    fprintf(f, "FATAL,%s,%s\n", where, d1);
    fflush(f);
}

static const uint64_t DP_VALS[] = {
    0x0000000000000000ull, 0x8000000000000000ull,                       /* +-0 */
    0x0000000000000001ull, 0x8000000000000001ull,                       /* +-min denormal */
    0x0008000000000000ull, 0x8008000000000000ull,                       /* +-mid denormal (the 3.4 repro operand) */
    0x000FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull,                       /* +-max denormal */
    0x000A56DA8A8D5917ull, 0x800A56DA8A8D5917ull, 0x000F6B9D2B8B96A2ull, 0x800F6B9D2B8B96A2ull,   /* the repro denormals */
    0x0010000000000000ull, 0x8010000000000000ull,                       /* +-min normal */
    0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x4000000000000000ull, 0x4008000000000000ull,
    0x7FEFFFFFFFFFFFFFull, 0xFFEFFFFFFFFFFFFFull,                       /* +-max */
    0x7FF0000000000000ull, 0xFFF0000000000000ull,                       /* +-inf */
    0x7FF8000000000000ull, 0x7FF0000000000001ull, 0xFFF8000000000000ull /* qNaN, sNaN, -qNaN */
};
#define NDP ((int)(sizeof DP_VALS / sizeof DP_VALS[0]))

static void putdp(uint32_t *q, int lane, uint64_t v) { q[2 * lane] = W32((uint32_t)(v >> 32)); q[2 * lane + 1] = W32((uint32_t)v); }

int spuexh_run_special_a(spuexh_backend_t *be, const spuexh_runcfg_t *rc, const spuexh_sink_t *sink, int accepted[5]) {
    static spuexh_case_t cs[96]; static spuexh_caseout_t outs[96]; static spuexh_thread_res_t tr;
    FILE *f = sink->out;
    for (int q = 0; q < 5; q++) accepted[q] = 0;
    for (int q = 0; q < 5; q++) {
        const spuexh_op_t *op = find_op(spuexh_dpcmp_names[q]);
        if (!op) continue;
        if (rc->a_ops[0] && !name_match(rc->a_ops, op->name)) continue;
        /* case list of this op */
        static spuexh_case_t all[1700]; int nall = 0;
        if (q < 4) {                             /* binary compares: every ordered pair of DP_VALS, two lanes per call */
            const int pairs = NDP * NDP;
            for (int p = 0; p < pairs; p += 2) {
                spuexh_case_t *c = &all[nall]; memset(c, 0, sizeof *c);
                c->word = W32(op->word); c->flags = W32((uint32_t)op->rt); c->id = W32((uint32_t)nall);
                putdp(c->a, 0, DP_VALS[p / NDP]); putdp(c->b, 0, DP_VALS[p % NDP]);
                if (p + 1 < pairs) { putdp(c->a, 1, DP_VALS[(p + 1) / NDP]); putdp(c->b, 1, DP_VALS[(p + 1) % NDP]); }
                nall++;
            }
        } else {                                 /* dftsv: every I7 on pairs of values */
            for (uint32_t i7 = 0; i7 < 128; i7++)
                for (int p = 0; p < NDP; p += 2) {
                    spuexh_case_t *c = &all[nall]; memset(c, 0, sizeof *c);
                    c->word = W32(op->word | (i7 << op->imm_shift)); c->flags = W32((uint32_t)op->rt); c->id = W32((uint32_t)nall);
                    putdp(c->a, 0, DP_VALS[p]); if (p + 1 < NDP) putdp(c->a, 1, DP_VALS[p + 1]);
                    nall++;
                }
        }
        /* validity: case 0 alone, in its own group */
        uint32_t started = 0;
        int ok = run_cases(be, SPUEXH_MODE_CASES, &all[0], 1, rc->case_timeout_ms, outs, &tr, &started, f);
        if (ok == -2) { fatal_row(f, &tr, "launch"); return -1; }
        char d1[200]; clean(d1, sizeof d1, tr.desc);
        fprintf(f, "SPECA_VALID,%s,%s,word_%08x,started_%u,%s\n", op->name, ok == 1 ? "executed" : "NOT_executed", W32(all[0].word), started, d1);
        fflush(f);
        if (ok != 1) continue;
        accepted[q] = 1;
        /* the rest in batches; a batch that does not complete is re-run case by case */
        for (int b0 = 0; b0 < nall; b0 += 96) {
            const int nb = nall - b0 < 96 ? nall - b0 : 96;
            memcpy(cs, &all[b0], (size_t)nb * sizeof cs[0]);
            int okn = run_cases(be, SPUEXH_MODE_CASES, cs, nb, rc->case_timeout_ms * 4, outs, &tr, NULL, f);
            if (okn == -2) { fatal_row(f, &tr, "launch"); return -1; }
            if (okn == nb) {
                for (int i = 0; i < nb; i++) {
                    char a[40], bb[40], r[40];
                    hex128(a, cs[i].a); hex128(bb, cs[i].b); hex128(r, outs[i].r);
                    fprintf(f, "SPECA,%s,%08x,%u,%s,%s,%s\n", op->name, W32(cs[i].word), W32(cs[i].id), a, bb, r);
                }
            } else {
                for (int i = 0; i < nb; i++) {
                    spuexh_caseout_t o1; uint32_t st1 = 0; char a[40], bb[40], r[40];
                    int k = run_cases(be, SPUEXH_MODE_CASES, &cs[i], 1, rc->case_timeout_ms, &o1, &tr, &st1, f);
                    if (k == -2) { fatal_row(f, &tr, "launch"); return -1; }
                    hex128(a, cs[i].a); hex128(bb, cs[i].b); hex128(r, o1.r);
                    clean(d1, sizeof d1, tr.desc);
                    if (k == 1) fprintf(f, "SPECA,%s,%08x,%u,%s,%s,%s\n", op->name, W32(cs[i].word), W32(cs[i].id), a, bb, r);
                    else fprintf(f, "SPECA_FAULT,%s,%08x,%u,%s,%s,%s\n", op->name, W32(cs[i].word), W32(cs[i].id), a, bb, d1);
                }
            }
            fflush(f);
            if (sink->ui) { char b[96]; snprintf(b, sizeof b, "special A: %s %d/%d", op->name, b0 + nb, nall); if (sink->ui(sink->uu, b)) return 1; }
        }
    }
    return 0;
}

int spuexh_run_special_b(spuexh_backend_t *be, const spuexh_runcfg_t *rc, const spuexh_sink_t *sink) {
    static spuexh_case_t cs; static spuexh_caseout_t o; static spuexh_thread_res_t tr;
    FILE *f = sink->out;
    const int total = (rc->special_b_max > 0 && rc->special_b_max < SPUEXH_NBAD) ? rc->special_b_max : SPUEXH_NBAD;
    fprintf(f, "META,special_b_prefixes,%d\n", total);
    if (rc->special_b_from > 0) fprintf(f, "META,special_b_from,%d\n", rc->special_b_from);
    fprintf(f, "META,special_b_patterns,%s\nMETA,special_b_count,%d\n", rc->special_b_both ? "both" : "zero",
            rc->special_b_count);
    int done = 0;
    for (int i = rc->special_b_from > 0 ? rc->special_b_from : 0; i < total; i++) {
        const uint32_t pre = spuexh_bad_prefix[i];
        for (int pat = 0; pat < (rc->special_b_both ? 2 : 1); pat++) {
            if (rc->special_b_count > 0 && done >= rc->special_b_count) { fflush(f); return 0; }
            const uint32_t operand = pat == 0 ? 0u : (uint32_t)exh_mix(0x5E5E000000000000ull + pre) & 0x1FFFFFu;
            const uint32_t word = (pre << 21) | operand;
            memset(&cs, 0, sizeof cs);
            cs.word = W32(word); cs.id = W32((uint32_t)(i * 2 + pat));
            /* pre-log the word before its group starts: if this launch cannot be contained, the file names the one tried */
            fprintf(f, "SPECB_TRY,%d,%03x,%s,%08x\n", i, pre, pat ? "rand" : "zero", word);
            fflush(f);
            uint32_t started = 0;
            const int ok = run_cases(be, SPUEXH_MODE_RAW, &cs, 1, rc->b_timeout_ms, &o, &tr, &started, f);
            if (ok == -2) { fatal_row(f, &tr, "launch"); return -1; }
            const uint32_t state = W32(o.state);
            char d1[200], regs[160]; regs[0] = 0;
            clean(d1, sizeof d1, tr.desc);
            if (ok == 1) {
                const uint32_t nch = W32(o.nchanged);
                for (uint32_t k = 0; k < nch && k < SPUEXH_RAW_MAXCH; k++) {
                    char hx[40]; hex128(hx, o.ch[k].v);
                    size_t L = strlen(regs);
                    snprintf(regs + L, sizeof regs - L, "%s%u:%s", k ? "|" : "", W32(o.ch[k].reg), hx);
                    if (strlen(regs) > 120) break;
                }
            }
            fprintf(f, "SPECB,%d,%03x,%s,%08x,%s,%u,%s,%u,%08x,%s\n", i, pre, pat ? "rand" : "zero", word,
                    ok == 1 ? "executed" : (state == 1 ? "no_return" : "not_started"),
                    ok == 1 ? W32(o.nchanged) : 0u, regs, ok == 1 ? W32(o.arena_hit) : 0u, ok == 1 ? W32(o.arena_addr) : 0u, d1);
            fflush(f);                       /* every word: if the process is lost, the file names the last one tried */
            done++;
            if ((i * 2 + pat) % 32 == 31) {
                if (sink->ui) { char b[96]; snprintf(b, sizeof b, "special B: %d/%d prefixes", i + 1, total); if (sink->ui(sink->uu, b)) return 1; }
            }
        }
        if (rc->special_b_count > 0 && done >= rc->special_b_count) break;
    }
    fflush(f);
    return 0;
}

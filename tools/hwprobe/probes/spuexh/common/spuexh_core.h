/* spuexh_core.h - the part of the spuexh probe that is the same on the console and in the host dry run: run
 * configuration, the supervisor of the sweep (launch, drain, fault handling, resume), the two special probes and
 * the result rows. Only the way SPU threads are started differs (spuexh_backend_t): the PPU layer uses PSL1GHT's
 * sys_spu_* calls, the host driver runs the SPU program in the toolkit's interpreter.
 */
#ifndef SPUEXH_CORE_H
#define SPUEXH_CORE_H

#include <stdio.h>
#include "spuexh.h"

/* ---------------------------------------------------------------- run configuration */
typedef struct {
    spuexh_cfg_t wire;               /* what the SPUs get (host byte order converted by W32) */
    int spus;                        /* SPU threads to use (<= 6 on a console) */
    int stall_s;                     /* no progress of any SPU for this long = hang */
    int max_faults;                  /* relaunches after a fault before the sweep gives up */
    int special_a, special_b;        /* run the special probes before the sweep */
    int special_b_max;               /* 0 = every undecoded prefix, else only the first N */
    int special_b_from;              /* skip the first N prefixes (resume after a run that was lost in the middle) */
    int special_b_count;             /* stop after N words (0 = every word): the staging control for probe B batches */
    int special_b_both;              /* 1 = zero and random pattern per prefix, 0 = only the zero pattern */
    char a_ops[64];                  /* probe A: comma list of its ops (default all five: dfceq dfcmeq dfcgt dfcmgt dftsv) */
    int case_timeout_ms;             /* wall limit of one special-A case (> 0: every launch is bounded) */
    int b_timeout_ms;                /* wall limit of one special-B word (> 0) */
    int launch_timeout_ms;           /* wall limit of one thread-group launch, sweep included (> 0) */
    int sweep;                       /* run the sweep at all (0: only the special probes) */
    char preset[16];
    int nlanes;                      /* lanes= entries (see RUN.md "Lanes"): per-lane dumps of chosen (op, domain, block) triples */
    struct { int op, scale, dom, count; uint32_t blk, start; } lanes[SPUEXH_LANES_MAX_ENTRIES];   /* op: index into spuexh_ops; scale: replacing I8 conversion scale or -1; dom: index into spuexh_dom_names */
    int nsums;                       /* lanesum= entries: one hash per chunk of calls over a whole block */
    struct { int op, scale, dom; uint32_t blk, chunk; } sums[SPUEXH_LANESUM_MAX_ENTRIES];
    char ops[512], skip_ops[512], doms[256];
} spuexh_runcfg_t;

/* Fills `rc` with the defaults of `preset` ("smoke", "standard", "full"; NULL = smoke). Returns 0, or -1 for an unknown preset. */
int spuexh_runcfg_defaults(spuexh_runcfg_t *rc, const char *preset);
/* One key=value of the probe's configuration (the keys documented in RUN.md). Returns 1 if handled, 0 if the key is one the
 * caller handles itself (pad, hold_s, screen, tag, outdir, fw_version, fw_type, console_model, progress_file), -1 on a bad
 * value or an UNKNOWN key ("unknown key 'k'" in err): a misspelt key never falls back to a default, the caller must stop. */
int spuexh_runcfg_set(spuexh_runcfg_t *rc, const char *key, const char *val, char *err, size_t errcap);
/* Applies ops=/skip_ops=/doms= to the wire config. Returns 0, or -1 with a message. */
int spuexh_runcfg_finish(spuexh_runcfg_t *rc, char *err, size_t errcap);

/* Write containment: 1 when `dir` is /dev_hdd0/tmp/hwprobe or a path below it without a ".." component,
 * 0 otherwise (the console probe must then refuse to run: it writes only its results below `dir`). */
int spuexh_outdir_ok(const char *dir);
/* Plan statistics of a configuration: jobs, hashes (op-blocks) and calls (op applications) over all SPUs. */
typedef struct { uint64_t jobs, hashes, calls, gen_calls; } spuexh_plan_t;
void spuexh_plan(const spuexh_cfg_t *c, spuexh_plan_t *p);

/* ---------------------------------------------------------------- backend */
typedef struct {
    int exited;                      /* the thread left through spu_thread_exit */
    int timed_out;                   /* the supervisor had to end it */
    int fatal;                       /* the launch could not be contained: the probe must stop (see run below) */
    int terminate_join;              /* the group had to be terminated before it could be joined (recorded per launch) */
    int exit_status_pending;         /* sysSpuThreadGetExitStatus' nonzero code when this thread had not exited (0 = exited) */
    char desc[160];                  /* how it ended, free text without commas (join cause/status, events, stop code) */
} spuexh_thread_res_t;

struct spuexh_backend;
typedef int (*spuexh_progress_fn)(void *u);   /* ~2 Hz while threads run; nonzero = end the group now (hang) */

typedef struct spuexh_backend {
    const char *name;
    void *u;
    /* aligned main memory (128-byte aligned, zeroed): host pointer and the effective address the SPU uses */
    void *(*alloc)(struct spuexh_backend *, size_t size, uint64_t *ea);
    /* Start n threads of the SPU program (thread i gets params at param_ea[i]) as one group, wait until every thread has
     * left (status block state != 1), a fault event arrives, the supervisor's progress callback asks to stop, or the wall
     * limit passes (limit_ms must be > 0: a launch without an end-to-end bound is rejected). Neither a fault event nor
     * the final status block proves that lv2 ended the threads, and join has no timeout, so the backend joins only after
     * every thread reports exited (sysSpuThreadGetExitStatus) or after a successful terminate (res[].terminate_join).
     * Returns 0, or
     *   -2  launch failure before any thread ran, possibly a matter of the thread count (the caller may retry smaller),
     *   -3  uncontainable failure: a launch step failed and destroyed the unstarted group, or terminate failed and the
     *       live group was left behind on purpose (no unbounded join; process exit lets lv2 reclaim it). The caller
     *       records a FATAL row and stops the whole probe.
     * The outcome of each thread is collected into res (res[i].fatal is set together with a -3 return).
     */
    int (*run)(struct spuexh_backend *, int n, const uint64_t *param_ea, const volatile spuexh_status_t *const *st,
               spuexh_progress_fn progress, void *pu, int limit_ms, spuexh_thread_res_t *res);
} spuexh_backend_t;

/* ---------------------------------------------------------------- results */
typedef struct {
    FILE *out;                       /* hash rows and diagnostics go here (header already written by the caller) */
    int (*ui)(void *u, const char *summary);    /* optional one-line status for the screen; returns nonzero when the operator asks to stop */
    void *uu;
} spuexh_sink_t;

typedef struct {
    uint64_t hashes, jobs_done, faults, relaunches;
    uint64_t timing_overflows;       /* jobs whose decrementer count wrapped (~53.8 s at 79.8 MHz): TIMING_OVERFLOW rows */
    int completed;                   /* every SPU reported state 2 */
    int aborted;                     /* gave up (max_faults, or allocation failure) */
    int spus_used;
    uint64_t busy_ticks, entries;
    double wall_s;
} spuexh_sweep_res_t;

/* The sweep. */
int spuexh_run_sweep(spuexh_backend_t *be, const spuexh_runcfg_t *rc, const spuexh_sink_t *sink, spuexh_sweep_res_t *res);

/* Special probe A: dfceq dfcmeq dfcgt dfcmgt dftsv as raw words (a fixed list in core.c: they are not in the op table). The
 * retail SPU executes none of them; an `executed` SPECA_VALID row is a regression of the model (compare_spuexh.py reports it).
 * Returns 0, 1 when the operator asked to stop, or -1 when a launch could not be contained (FATAL row written). */
int spuexh_run_special_a(spuexh_backend_t *be, const spuexh_runcfg_t *rc, const spuexh_sink_t *sink);
/* Special probe B: decoder validity of every prefix the toolkit decodes as SPU_word (special_b_count/special_b_both
 * narrow it: the staging control for the first batches). Returns 0, 1 when the operator asked to stop, or -1 on FATAL. */
int spuexh_run_special_b(spuexh_backend_t *be, const spuexh_runcfg_t *rc, const spuexh_sink_t *sink);

/* Lanes jobs: for every lanes= entry one launch on one SPU (the supervisor's launch limit, FATAL row and META rows as
 * everywhere) that dumps the operands, the patched immediate and the result of the first `count` generator calls of the
 * entry's block (`LANES,...` header row, `LANE,...` rows, `LANES_DONE` or `LANES_FAULT`; FORMAT.md), flushed per entry.
 * Returns 0, 1 when the operator asked to stop, or -1 when a launch could not be contained or no buffer could be
 * allocated (FATAL row written). A fault costs the one entry (LANES_FAULT) and the run goes on. */
int spuexh_run_lanes(spuexh_backend_t *be, const spuexh_runcfg_t *rc, const spuexh_sink_t *sink);

double spuexh_now(void);             /* seconds, monotonic (core.c uses it for the stall clock) */

#endif

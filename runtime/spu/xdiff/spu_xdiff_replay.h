/*
 * ps3recomp - replaying a fixture into a shadow context
 *
 * A replay builds a context from the fixture alone (docs/SPU_XDIFF.md section
 * 9), installs a serve table that answers every channel operation from the
 * transcript and checks every request against it (section 8.1), runs the
 * subject tier to its end, and compares the end state. The subject can neither
 * see nor change the live world: an effect the transcript does not hold ends
 * the run.
 *
 * spu_job_replay_main is the offline runner of section 13. It is a library
 * entry, not a program: lifted bodies are a title's code, so the executable
 * that calls it is linked with the registry of the images it can replay (a
 * title's, or the toolkit's synthetic test images).
 */
#ifndef SPU_XDIFF_REPLAY_H
#define SPU_XDIFF_REPLAY_H

#include "spu_xdiff_fixture.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A shadow: one context and its local store, built anew for every replay and
 * reused across replays on one thread. Never shared between threads. */
typedef struct spu_xdiff_shadow spu_xdiff_shadow;

spu_xdiff_shadow* spu_xdiff_shadow_new(void);
void              spu_xdiff_shadow_free(spu_xdiff_shadow* s);

typedef struct spu_xdiff_replay_options {
    uint32_t subject_tier; /* SPU_TIER_INTERP, or SPU_TIER_LIFTED for the image's registered body */
    uint64_t budget;       /* before `void` / `budget`; 0: none. T0: instructions. A registered body: backward and
                            * cross-function transfers of the image's budgeted twin when one is registered
                            * (spu_workload_find_twin), else the budget does not apply to it */
} spu_xdiff_replay_options;

/* Replays `f` into `s` and returns the verdict; `out` holds the whole record
 * (section 7.7). Checks, in order: the knobs of section 8.1 (refused: the
 * caller decides how to report it, see spu_job_replay_main), the fixture's
 * float mode against this process's (`malformed` / `fixture-identity`), the
 * pair of tiers (spu_xdiff_match_for), a registered body for a lifted subject
 * (`not-found` / `no-body`). Then it runs the subject under spu_run_with_halt
 * and compares. Runs in the calling process: a subject that faults takes it down; spu_job_replay_main supervises
 * that. A named stop the subject raises (ps3_stop on this thread) does not: it is diverted for the run (ps3_stop_divert_begin) and
 * unwinds through the halt landing pad into `void` / `host-fault`, which is what lets the in-run audit (libs/spurs/jq2/xdiff_audit.hpp)
 * run a subject inside a title's process. abort(), a signal or a direct exit() are not stops and are not diverted. */
uint32_t spu_xdiff_replay(const spu_xdiff_fixture* f, const spu_xdiff_replay_options* opt,
                          spu_xdiff_shadow* s, spu_xdiff_verdict_record* out);

typedef struct spu_xdiff_function_result {
    uint32_t verdict;
    uint32_t reason;
    uint32_t match;
    uint32_t dead_registers; /* number of dead registers differing from the authority */
    uint32_t where;          /* register index, LS address, expected return PC, or SPU_XDIFF_NO_WHERE */
    uint8_t  want[16];
    uint8_t  got[16];
} spu_xdiff_function_result;

/* Replays a kind-FUNCTION unit through the registered original lift or its
 * native override, then compares the return PC, live-out registers and LS
 * outside FMAS. */
uint32_t spu_xdiff_replay_function_unit(const spu_xdiff_fixture* f, int native_override,
                                        spu_xdiff_function_result* out);

/* The name of a set behaviour-changing knob (section 8.1), or NULL when none
 * is set. A replay under one would compare a different program. */
const char* spu_xdiff_refused_knob(void);

/* ---- the poison run (docs/SPU_XDIFF.md 8.2, docs/SPU_LIFTER.md "Liveness") ----
 *
 * A liveness plan (tools/spu_liveness.py, schema `ps3recomp.spu-liveness/1`) claims, per function, registers
 * that are dead when it returns and the stack bytes of its frame below the entry stack pointer. The poison
 * run checks the claims on one fixture instead of trusting them: T0 replays the fixture, then replays it
 * again and, at every return of a listed function, overwrites what the plan calls dead. A second run that
 * differs from the first (the fixture) proves the claim false for this fixture; one that does not proves
 * nothing beyond it.
 *
 * What is poisoned, and when. A call is a `brsl`, `brasl` or `bisl` whose target is a listed entry; it pushes
 * the link and $r1. A return is the next instruction executed after a `bi`/`biz`/`binz`/`bihz`/`bihnz` on
 * $r0 whose address is the pushed link. Poison is applied before that instruction runs, so the return itself
 * has read $r0, and never earlier. $r1 is never poisoned: a plan that lists it is refused. Registers get
 * four words of 0xDEADBEEF, the frame 0xA5 bytes at sp - k for k in [stack_from, stack_to], with sp the
 * value $r1 had at the call. A frame whose return finds $r1 elsewhere, or that does not lie inside local
 * store, makes the claim false (the plan said the frame is the n bytes below sp). Poison nobody read stays
 * in the end state; it is put back to the fixture's value before the end states are compared, so only state
 * that carries it elsewhere differs. */
#define SPU_XDIFF_POISON_MAX_FUNCTIONS 65536u
#define SPU_XDIFF_POISON_REG_WORD      0xDEADBEEFu
#define SPU_XDIFF_POISON_STACK_BYTE    0xA5u

typedef struct spu_xdiff_poison_fn {
    uint32_t entry;       /* the local store address a call lands on */
    uint32_t stack_from;  /* the dead frame is the bytes at sp - k, k in [stack_from, stack_to] */
    uint32_t stack_to;    /* 0: no dead frame */
    uint32_t reserved;
    uint64_t dead[2];     /* bit r of dead[r >> 6]: $rN is dead at return; never $r1 */
} spu_xdiff_poison_fn;

typedef struct spu_xdiff_poison_plan {
    int                  has_fingerprint;
    uint64_t             image_fingerprint;  /* the image the plan was computed for (fixture identity) */
    uint32_t             count;
    spu_xdiff_poison_fn* fn;                 /* sorted by entry, no duplicates */
} spu_xdiff_poison_plan;

/* Parses a plan. 0 on success; -1 with a message in `err`. A plan that lists $r1 as dead, a function twice,
 * a register outside 0..127, an entry that is not a word address inside local store, or a frame larger than
 * local store is refused. Members the runner does not use (live_out, callers, ...) are skipped. */
int  spu_xdiff_poison_plan_parse(const char* text, size_t bytes, spu_xdiff_poison_plan* out, char* err, size_t err_bytes);
int  spu_xdiff_poison_plan_load(const char* path, spu_xdiff_poison_plan* out, char* err, size_t err_bytes);
void spu_xdiff_poison_plan_free(spu_xdiff_poison_plan* plan);

/* Replay a T0 job fixture with its instruction observer and write at most the
 * first 64 calls of `entry` as deterministic kind-FUNCTION fixtures in out_dir. */
int spu_xdiff_capture_function_units(const spu_xdiff_fixture* f,
                                    const spu_xdiff_poison_plan* plan, uint32_t entry,
                                    const char* out_dir, uint32_t* captured,
                                    char* err, size_t err_bytes);

typedef enum spu_xdiff_poison_kind {
    SPU_XDIFF_POISON_UNKNOWN  = 0, /* no single register or frame byte explains the difference */
    SPU_XDIFF_POISON_REGISTER = 1,
    SPU_XDIFF_POISON_STACK    = 2
} spu_xdiff_poison_kind;

typedef struct spu_xdiff_poison_report {
    uint32_t ran;           /* 1: the unpoisoned run was equal, so the poison run was made */
    uint32_t returns;       /* returns of listed functions the unpoisoned run made */
    uint32_t poisoned;      /* returns the poison run applied poison at */
    uint32_t has_function;  /* 1: `function` names the first listed function whose poison alone changes the run */
    uint32_t function;      /* its entry */
    uint32_t kind;          /* spu_xdiff_poison_kind, for `function` */
    uint32_t reg;           /* REGISTER: the lowest dead register whose poison alone changes the run */
    uint32_t below_sp;      /* STACK: the largest k (the lowest byte) that matters; 0 when the claim failed on the stack pointer */
    uint32_t exact;         /* STACK: poisoning that one byte alone changes the run */
    uint32_t underlying;    /* the reason the narrowest failing run would have had without --poison (none: a frame the stack pointer left) */
    uint32_t reserved;
} spu_xdiff_poison_report;

/* The poison run. `opt->subject_tier` is SPU_TIER_INTERP (the observer is T0's; anything else is
 * `malformed` / `not-comparable`). The unpoisoned run goes first and its verdict is returned as is unless it
 * is `equal`: a replay that does not match the fixture has nothing to compare a poison run with. When the
 * poisoned run differs, or a frame claim fails, the verdict is `differ` / `poison`; `out` then holds the
 * narrowest failing run's record (where, want, got and the effect are those of the difference it would have
 * been without the label) and `report` names the function and, when a single one explains it, the register
 * or frame byte. A plan computed for another image is `malformed` / `fixture-identity`. */
uint32_t spu_xdiff_replay_poison(const spu_xdiff_fixture* f, const spu_xdiff_replay_options* opt,
                                 const spu_xdiff_poison_plan* plan, spu_xdiff_shadow* s,
                                 spu_xdiff_verdict_record* out, spu_xdiff_poison_report* report);

/* `spu_job_replay [--tier interp|registered] [--budget N] [--timeout S] [--poison PLAN] [--json] FIXTURE`:
 * runs the replay in a child process and prints one verdict (JSON schema
 * `ps3recomp.spujob-verdict/1` with --json, one line otherwise) after the
 * child ends. --poison (needs --tier interp) makes it the poison run of
 * spu_xdiff_replay_poison with the liveness plan PLAN; the JSON then carries a
 * `poison` member, and an unreadable plan is a usage error. The exit status is the verdict (spu_xdiff.h); 1 is a usage
 * error or a refused knob. A child that times out or dies is `void` /
 * `host-fault`. */
int spu_job_replay_main(int argc, char** argv);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_XDIFF_REPLAY_H */

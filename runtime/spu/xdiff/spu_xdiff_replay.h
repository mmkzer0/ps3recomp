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
 * and compares. Runs in the calling process: a subject that faults takes it
 * down; spu_job_replay_main supervises that. */
uint32_t spu_xdiff_replay(const spu_xdiff_fixture* f, const spu_xdiff_replay_options* opt,
                          spu_xdiff_shadow* s, spu_xdiff_verdict_record* out);

/* The name of a set behaviour-changing knob (section 8.1), or NULL when none
 * is set. A replay under one would compare a different program. */
const char* spu_xdiff_refused_knob(void);

/* `spu_job_replay [--tier interp|registered] [--budget N] [--timeout S] [--json] FIXTURE`:
 * runs the replay in a child process and prints one verdict (JSON schema
 * `ps3recomp.spujob-verdict/1` with --json, one line otherwise) after the
 * child ends. The exit status is the verdict (spu_xdiff.h); 1 is a usage
 * error or a refused knob. A child that times out or dies is `void` /
 * `host-fault`. */
int spu_job_replay_main(int argc, char** argv);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_XDIFF_REPLAY_H */

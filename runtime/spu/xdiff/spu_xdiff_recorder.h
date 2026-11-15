/*
 * ps3recomp - recording one job into an xdiff fixture
 *
 * The recorder is a tap on the effect port (spu_effect_port.h): it hears every
 * channel effect and transfer of the authority's run and appends one
 * transcript record per callback, in arrival order (docs/SPU_XDIFF.md section
 * 6). It takes payload bytes from the context's own local store at the
 * reported address, never from guest memory and never by walking a list, so
 * it records what the runtime did.
 *
 * Lifecycle, on the thread that runs the job:
 *
 *     spu_xdiff_recorder_begin(&r, &fixture, ctx, &unit, &authority, NULL, NULL, 0);
 *     ... the loader runs the job against the real world ...
 *     spu_job_end end = spu_job_classify_end(ctx);
 *     if (spu_xdiff_recorder_end(&r, ctx, end.end_class, end.stop_code) == SPU_XDIFF_R_NONE)
 *         spu_xdiff_fixture_write(&fixture, path, &err);
 *
 * A recorder is not thread-safe and serves one context at a time.
 */
#ifndef SPU_XDIFF_RECORDER_H
#define SPU_XDIFF_RECORDER_H

#include "spu_xdiff_fixture.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

typedef struct spu_xdiff_recorder {
    spu_effect_port      port;     /* installed on the context between begin and end */
    spu_effect_tap       tap;
    spu_xdiff_fixture*   fixture;
    struct spu_context*  ctx;
    uint32_t             max_blob_bytes; /* 0: SPU_XDIFF_MAX_BLOB_BYTES */
    /* The first thing that could not be recorded, or NONE. Recording stops at
     * it (nothing after it is appended), the run itself goes on, and end()
     * returns it: CAPTURE_LIMIT when a bound or memory ran out,
     * UNSUPPORTED_EFFECT for a callback the format has no record for. */
    uint32_t             failed;
    spu_xdiff_error      error;
} spu_xdiff_recorder;

/* Starts recording `ctx`, which must be at its entry state and carry no port.
 * Fills IDEN (fingerprint over [unit->image_ls, +image_bytes) of the entry
 * local store, `authority`, the port ABI, the context census, host arch and
 * ISA bits, the latched float mode), UNIT (copied from `unit`), REG0 and LS_0,
 * then installs the recording tap. `serve`/`serve_user`, when not NULL, answer
 * the context instead of the runtime (a synthetic world for fuzzing); the
 * recorder still hears every answer. Returns 0, or -1 with `r->error` set. */
int spu_xdiff_recorder_begin(spu_xdiff_recorder* r, spu_xdiff_fixture* f, struct spu_context* ctx,
                             const spu_xdiff_unit* unit, const spu_xdiff_body_id* authority,
                             const spu_effect_serve* serve, void* serve_user, uint32_t max_blob_bytes);

/* Removes the port and fills REG1, LS_1 and END_ from the context as the run
 * left it. `end_class` and `stop_code` come from the loader's classification
 * (spu_job_classify_end, runtime/spu/spurs_job.h); END_.pc and .status are
 * the raw fields. Returns `r->failed` (NONE: the fixture is complete). A
 * failed recording leaves END_.end_class 0, which the encoder refuses: a
 * truncated transcript would replay as a `differ` the lift never made. */
uint32_t spu_xdiff_recorder_end(spu_xdiff_recorder* r, struct spu_context* ctx,
                                uint32_t end_class, uint32_t stop_code);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_XDIFF_RECORDER_H */

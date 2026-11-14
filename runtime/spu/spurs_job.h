/* spurs_job.h - how a SPURS job body ended.
 *
 * One classifier, shared by the JobQueue2 executor (its policy and the end
 * hook a recorder reads) and anything that compares two runs of one job
 * (docs/SPU_XDIFF.md sections 7.6 and 8.1). The class exists because the raw
 * context fields depend on the tier that ran the body.
 */
#ifndef SPURS_JOB_H
#define SPURS_JOB_H

#include "spu_context.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The link address the loader plants for a job to return to. Local store there
 * is zero (an SPU `stop 0`). */
#define SPU_JOB_RETURN_LINK_LS 0x3FF00u

/* Values are the `END_` section's end_class (docs/SPU_XDIFF.md 7.6): stored in
 * fixtures, never renumbered. */
enum {
    SPU_JOB_END_RETURN = 1, /* returned to the job manager, interpreted or lifted */
    SPU_JOB_END_STOP   = 2, /* a `stop` that is not the return and not a jm call */
    SPU_JOB_END_HALT   = 3, /* a halt, an assertion in lifted code or an interpreted `heq`/`hgt` */
    SPU_JOB_END_JM_CALL = 4, /* a call of jm word 0, the scheduler switch */
    SPU_JOB_END_OTHER  = 5  /* anything else: the run ended with neither stop nor halt, off the link */
};

/* The `END_` record's four words (7.6), sixteen bytes. Only the class and, for
 * a stop, the code are comparable between tiers; pc and status say where this
 * tier left the context. */
typedef struct spu_job_end {
    uint32_t end_class; /* SPU_JOB_END_* */
    uint32_t stop_code; /* ctx->stop_code */
    uint32_t pc;        /* ctx->pc & SPU_LS_MASK */
    uint32_t status;    /* ctx->status */
} spu_job_end;

#ifdef __cplusplus
static_assert(sizeof(spu_job_end) == 16, "spu_job_end is the 16-byte END_ record");
#else
_Static_assert(sizeof(spu_job_end) == 16, "spu_job_end is the 16-byte END_ record");
#endif

/* Classify the context of a finished JobQueue2 job body. Pure: reads the
 * context, stops nothing, credits nothing.
 *   return   interpreted: status STOP, stop code 0, pc just past the link;
 *            lifted: pc at the link and status neither STOP nor HALT (the
 *            resolver halts the run when a branch lands on the link);
 *   jm-call  status STOP, code SPU_JQ2_JM_CALL_STOP, pc just past
 *            SPU_JQ2_JM_CALL_LS (interpreted, or lifted through the resolver);
 *   stop     any other status STOP; halt: status HALT; other: the rest. */
spu_job_end spu_job_classify_end(const spu_context* ctx);

#ifdef __cplusplus
}
#endif

#endif /* SPURS_JOB_H */

/* The budgeted twin of the lift test_spu_xdiff_budget.c replays (docs/SPU_XDIFF.md section 10): the lifter's own
 * source compiled a second time with SPU_LIFT_BUDGET. The definition has to precede the first inclusion of
 * spu_context.h, which spu_recomp.c makes. tools/build_spu_workloads.py --budget-twin writes this file for
 * each image of a title. */
#define SPU_LIFT_BUDGET 1
#include "spu_recomp.c"

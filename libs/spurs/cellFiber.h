/*
 * ps3recomp - cellFiber HLE
 *
 * Cooperative PPU fibers with guest-visible signal and scheduler queues.
 * UtilWorkerControl keeps calling PPU workers alive while fibers are idle;
 * Shutdown cannot cancel a live fiber or discard its suspended host stack.
 * Each fiber has a caller-owned guest stack and a private host context
 * (ucontext on POSIX, Windows fibers on Windows). Once started, the host
 * context stays on its worker so suspended native TLS references remain valid.
 *
 * Pointer arguments and u32 object arguments are guest EAs, never host pointers.
 * The established SDK fiber size is retained; firmware accesses through +0x338
 * were decoded, not an independent proof of all 896 bytes.
 */

#ifndef PS3RECOMP_CELL_FIBER_H
#define PS3RECOMP_CELL_FIBER_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes (the SDK's, module 0x8076)
 * -----------------------------------------------------------------------*/
#define CELL_FIBER_ERROR_AGAIN             0x80760001
#define CELL_FIBER_ERROR_INVAL             0x80760002
#define CELL_FIBER_ERROR_NOMEM             0x80760004
#define CELL_FIBER_ERROR_DEADLK            0x80760008
#define CELL_FIBER_ERROR_PERM              0x80760009
#define CELL_FIBER_ERROR_BUSY              0x8076000A
#define CELL_FIBER_ERROR_ABORT             0x8076000C
#define CELL_FIBER_ERROR_STAT              0x8076000F
#define CELL_FIBER_ERROR_ALIGN             0x80760010
#define CELL_FIBER_ERROR_NULL_POINTER      0x80760011
#define CELL_FIBER_ERROR_NOSYSINIT         0x80760020

/* ---------------------------------------------------------------------------
 * Guest structures: size and alignment as the SDK lays them out
 * -----------------------------------------------------------------------*/
#define CELL_FIBER_PPU_SCHEDULER_ATTRIBUTE_SIZE   256
#define CELL_FIBER_PPU_ATTRIBUTE_SIZE             256
#define CELL_FIBER_PPU_SCHEDULER_SIZE             512
#define CELL_FIBER_PPU_SIZE                       896
#define CELL_FIBER_PPU_SCHEDULER_ALIGN            128
#define CELL_FIBER_PPU_ALIGN                      128
#define CELL_FIBER_PPU_WORKER_CONTROL_SIZE        768
#define CELL_FIBER_PPU_WORKER_ATTRIBUTE_SIZE      384

typedef struct CellFiberPpuSchedulerAttribute CellFiberPpuSchedulerAttribute;
typedef struct CellFiberPpuAttribute          CellFiberPpuAttribute;
typedef struct CellFiberPpuScheduler          CellFiberPpuScheduler;
typedef struct CellFiberPpu                   CellFiberPpu;

/* ---------------------------------------------------------------------------
 * API
 * -----------------------------------------------------------------------*/

/* Instruction-level ABI; SDK C declaration unknown. Returns anchor-r13,
 * wrapping in 64 bits, and records the low word (libfiber 02003D18..3D28). */
u64 _cellFiberPpuInitialize(u64 anchor);
/* The SDK's inline cellFiberPpu*AttributeInitialize(attr) wrappers call these. */
s32 _cellFiberPpuSchedulerAttributeInitialize(CellFiberPpuSchedulerAttribute* attr, u32 sdkVersion);
s32 _cellFiberPpuAttributeInitialize(CellFiberPpuAttribute* attr, u32 sdkVersion);

s32 cellFiberPpuInitializeScheduler(CellFiberPpuScheduler* scheduler,
                                    const CellFiberPpuSchedulerAttribute* attr);
s32 cellFiberPpuFinalizeScheduler(CellFiberPpuScheduler* scheduler);

/* `entry` is the guest address of the fiber function's descriptor (OPD); its return value is the exit status.
 * `eaStack`/`sizeStack` are the fiber's guest stack. A lower `priority` number runs first, creation order breaks ties. */
s32 cellFiberPpuCreateFiber(CellFiberPpuScheduler* scheduler, CellFiberPpu* fiber, u32 entry, u64 arg,
                            u32 priority, void* eaStack, u32 sizeStack, const CellFiberPpuAttribute* attr);

/* Runs the scheduler's fibers on the calling thread until every one has ended. */
s32 cellFiberPpuRunFibers(CellFiberPpuScheduler* scheduler);

/* Called by a fiber. */
s32 cellFiberPpuWaitSignal(void);
s32 cellFiberPpuYield(void);
s32 cellFiberPpuExit(s32 status);
/* Guest address of the calling fiber, 0 outside a fiber. */
CellFiberPpu* cellFiberPpuSelf(void);

/* Callable from any thread. */
s32 cellFiberPpuSendSignal(CellFiberPpu* fiber, u32* numWorker);
s32 cellFiberPpuJoinFiber(CellFiberPpu* fiber, s32* status);

s32 _cellFiberPpuUtilWorkerControlAttributeInitialize(u32 attr, u32 sdkVersion);
s32 cellFiberPpuUtilWorkerControlInitializeWithAttribute(u32 control, u32 attr);
s32 cellFiberPpuUtilWorkerControlCreateFiber(u32 control, u32 fiber, u32 entry, u64 arg,
                                          u32 priority, u32 stack, u32 size, u32 attr);
s32 cellFiberPpuUtilWorkerControlRunFibers(u32 control);
s32 cellFiberPpuUtilWorkerControlWakeup(u32 control);
s32 cellFiberPpuUtilWorkerControlSendSignal(u32 fiber, u32 output);
s32 cellFiberPpuUtilWorkerControlShutdown(u32 control);
s32 cellFiberPpuUtilWorkerControlFinalize(u32 control);

/* Host notification and observer boundary, not firmware exports. Registry ->
 * reservation line; returns only after publication and worker notification.
 * Set the exit hook while no worker runs; it executes outside the registry
 * lock, before the exiting worker releases its guest slot/count. */
typedef enum ps3_fiber_notify_status {
    PS3_FIBER_NOTIFY_OK, PS3_FIBER_NOTIFY_NULL, PS3_FIBER_NOTIFY_ALIGN, PS3_FIBER_NOTIFY_STATE
} ps3_fiber_notify_status;
ps3_fiber_notify_status ps3_fiber_notify(u32 fiber);
void ps3_fiber_set_worker_exit_hook(void (*hook)(void*, u32), void* user);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_FIBER_H */

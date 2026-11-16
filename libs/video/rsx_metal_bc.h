/*
 * ps3recomp - the GPU tier of the BC1/2/3 emulation (Metal)
 *
 * A device that cannot sample BC (A11 and older) gets a guest DXT texture as RGBA8Unorm. The CPU tier is
 * rsx_bc_decode(); this is the tier for levels large enough that decoding on the RSX thread costs more than
 * handing the blocks to the GPU. A compute kernel reads the blocks from a shared buffer and writes packed RGBA8
 * words, with the same integer arithmetic as the CPU decoder, into another shared buffer; a blit copies them
 * into the texture (bytes, no float conversion), so the texture holds exactly what the CPU tier would have
 * written (libs/video/tests/test_rsx_bc_metal.m proves it byte for byte).
 *
 * Each decode is its own command buffer, committed before the call returns. The caller's contract is
 * what makes that enough: the texture is new and no draw has been recorded against it, so the GPU runs the decode
 * before any command buffer the caller commits later on the same queue (commit order is execution order on one
 * queue). Rewriting a texture that recorded draws already sample would be a write-after-read hazard against
 * them; such a rewrite takes the CPU tier. See rsx_metal_backend.m, eng_bc_upload.
 *
 * Not a separate translation unit in the build: rsx_metal_backend.m includes rsx_metal_bc.m, as the fixtures
 * that include the backend have fixed link lines (the same reason host_gpu_gate.h is header-only). The test and
 * the bench compile rsx_metal_bc.m as a source of their own.
 */
#ifndef PS3RECOMP_RSX_METAL_BC_H
#define PS3RECOMP_RSX_METAL_BC_H

#import <Metal/Metal.h>
#include "rsx_bc_decode.h"

/* What the host (the backend) lends the decoder. Every member is optional.
 *   new_buffer   a shared buffer of `length` bytes, nil when refused (the backend's allocator also stops the
 *                run on a refusal); NULL allocates on the device and reports nil.
 *   gate_*       bracket the creation and the commit of a command buffer, in the host's GPU gate section
 *                (host_gpu_gate.h). The decoder never holds a section across a wait.
 *   active_ms    a monotonic millisecond clock that does not advance while the host GPU gate is suspended
 *                (the app is inactive): what a bounded wait is charged against. NULL: CLOCK_MONOTONIC.
 *   commit       commits `cb`, inside the gate section the decoder holds, and records it as the last buffer
 *                committed on its queue (the host waits for its scheduling before the app backgrounds).
 *                NULL: [cb commit]. */
typedef struct rsx_metal_bc_host {
    void* user;
    id<MTLBuffer> (*new_buffer)(void* user, NSUInteger length);
    void (*gate_enter)(void* user);
    void (*gate_checkpoint)(void* user);
    void (*gate_leave)(void* user);
    uint64_t (*active_ms)(void* user);
    void (*commit)(void* user, id<MTLCommandBuffer> cb);
} rsx_metal_bc_host;

@interface RsxMetalBc : NSObject

/* nil, with `error`, when the kernel does not compile or the pipeline cannot be made. */
- (instancetype)initWithDevice:(id<MTLDevice>)device
                          host:(const rsx_metal_bc_host*)host
                         error:(NSError* __autoreleasing *)error;

/* Decode the level `blocks` (tight block rows, as rsx_bc_decode takes them) of `width` x `height` texels and
 * copy the RGBA8 rows into `texture` (MTLPixelFormatRGBA8Unorm) at `slice`, `level` and depth `z` (a 3D
 * texture's slice, else 0). The blocks are copied before the call returns, so the caller reuses its buffer at
 * once. The command buffer is created on `queue` and committed before the return; `completion`, if given, runs
 * when it has finished, on a Metal thread. At most a few decodes are in flight: a call past that waits for the
 * oldest, outside any gate section.
 *
 * Returns YES when a command buffer was committed. NO (nothing committed, the texture untouched) for invalid
 * arguments, a refused buffer or a nil command buffer. */
- (BOOL)decodeBlocks:(const void*)blocks
                size:(size_t)size
              format:(rsx_bc_format)format
               width:(uint32_t)width
              height:(uint32_t)height
                into:(id<MTLTexture>)texture
               slice:(NSUInteger)slice
               level:(NSUInteger)level
                   z:(NSUInteger)z
               queue:(id<MTLCommandQueue>)queue
          completion:(void (^)(id<MTLCommandBuffer> cb))completion;

/* Wait until every decode committed so far has completed and recorded its outcome, for at most `budgetMs` of
 * active time (see rsx_metal_bc_host.active_ms; time the host GPU gate stands suspended is not counted); one
 * deadline, never extended. Returns nil when every decode has published its outcome, else the decodes still
 * pending when the budget ran out ("BC3 64x64 level 2 slice 0 z 0; ..."), never an empty string. A decode
 * publishes its failure record and the end of its pending entry together, before it returns its slot, so at the
 * deadline an empty pending list means all outcomes are final (a decode that completed as the budget ran out
 * is not a timeout; its error is failureTexture's, and a caller must read that on a nil return) even if a slot
 * is still on its way back. Sleeps in short slices, never spins. Call it outside any gate section, before the
 * commit of anything that samples the textures, and before dropping the decoder. */
- (NSString*)waitIdleWithin:(uint64_t)budgetMs;

/* Decodes committed and not yet completed. */
- (unsigned)inFlight;

/* Whether a decode committed and not yet completed writes `texture`. The host must not write that texture from
 * the CPU (replaceRegion) while this is YES: the GPU may be writing it, and the two overlap undefined. */
- (BOOL)hasPendingDecodeFor:(id<MTLTexture>)texture;

/* The first decode whose command buffer ended in an error, since init: what it was filling ("BC3 64x64 level 2
 * slice 0 z 0") and what Metal said; nil when none failed. The texture it was to fill is undefined. Valid
 * after waitIdleWithin returned nil; safe from any thread. */
- (NSString*)failureTexture;
- (NSString*)failureError;

/* Decodes whose command buffer ended in an error, since init. */
- (unsigned)failureCount;

#ifdef RSX_METAL_BC_TEST_HOOKS
/* Compiled only into the tests. The next decode's completion is delayed by `delay` microseconds and, with `fail`,
 * treated as a command buffer that ended in an error. */
- (void)testInjectNextDecodeFailure:(BOOL)fail delayMicroseconds:(useconds_t)delay;
/* The next decode's completion does not run until testReleaseHeldCompletion: a command buffer that never ends. */
- (void)testHoldNextDecodeCompletion;
- (void)testReleaseHeldCompletion;
/* Slices (rsx_metal_bc.m, kRsxBcWaitSliceMs) a waitIdleWithin has waited so far without getting a slot, over
 * all calls: more than zero proves a waiter is blocked on an unfinished decode, and each slice is time passed
 * under the waiter's own clock, not a sleep of the test's. */
- (unsigned)testBlockedSlices;
/* Runs `block` once, on the waiting thread, when a bounded wait's budget ran out, before it reads what is
 * pending: a completion released there lands between the timeout and the report. */
- (void)testOnBudgetExhausted:(void (^)(void))block;
/* The next decode's completion handler stops after it has published its outcome (failure record and end of its
 * pending entry) and before it returns its slot, until testResumePausedHandler; testWaitUntilHandlerPaused
 * blocks until it is there. A waiter's deadline can then fall exactly between the outcome and the hand-back. */
- (void)testPauseNextDecodeBeforeSlotReturn;
- (void)testWaitUntilHandlerPaused;
- (void)testResumePausedHandler;
#endif

@end

#endif

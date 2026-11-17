/* The GPU tier of the BC1/2/3 emulation; see rsx_metal_bc.h. */
#include "rsx_metal_bc.h"
#include "../../include/ps3emu/ps3_log.h"   /* PS3_LOG: tagged lines, stderr bytes unchanged */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* The kernel is rsx_bc_decode.c in MSL, statement for statement: bit-replicated endpoints, Q8 weights with
 * +128 >> 8, the three-colour BC1 mode with a transparent index 3, BC2 nibbles * 17, BC3 alpha ramps. The
 * constants are the Apple GPU's own (the Metal oracle, libs/video/tests/test_rsx_bc_oracle_metal.m). Integer
 * arithmetic only, so no float rounding can differ from the CPU tier. One thread decodes one block and writes
 * the texels inside the level; a partial edge block writes only its in-range texels. */
static const char kRsxBcMSL[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct RsxBcParams { uint width; uint height; uint blocks_w; uint blocks_h; uint format; };\n"
"constant uint kSixWeights[6] = { 220u, 184u, 148u, 108u, 72u, 36u };\n"
"constant uint kFiveWeights[4] = { 204u, 152u, 104u, 52u };\n"
"static uint expand5(uint v) { return (v << 3) | (v >> 2); }\n"
"static uint expand6(uint v) { return (v << 2) | (v >> 4); }\n"
"static uint lerp_q8(uint c0, uint c1, uint w0) { return (c0 * w0 + c1 * (256u - w0) + 128u) >> 8; }\n"
"static uint3 lerp3_q8(uint3 c0, uint3 c1, uint w0) { return (c0 * w0 + c1 * (256u - w0) + 128u) >> 8; }\n"
"kernel void rsx_bc_decode(device const uint* blocks [[buffer(0)]],\n"
"                          device uint* rgba [[buffer(1)]],\n"
"                          constant RsxBcParams& p [[buffer(2)]],\n"
"                          uint gid [[thread_position_in_grid]])\n"
"{\n"
"    if (gid >= p.blocks_w * p.blocks_h) return;\n"
"    const uint format = p.format;\n"
"    device const uint* blk = blocks + gid * (format == 1u ? 2u : 4u);\n"
"    const uint color_word = format == 1u ? 0u : 2u;\n"
"    const uint endpoints = blk[color_word];\n"
"    const uint indices = blk[color_word + 1u];\n"
"    const uint e0 = endpoints & 0xFFFFu;\n"
"    const uint e1 = endpoints >> 16;\n"
"    const uint3 c0 = uint3(expand5((e0 >> 11) & 31u), expand6((e0 >> 5) & 63u), expand5(e0 & 31u));\n"
"    const uint3 c1 = uint3(expand5((e1 >> 11) & 31u), expand6((e1 >> 5) & 63u), expand5(e1 & 31u));\n"
"    uint4 palette[4];\n"
"    palette[0] = uint4(c0, 255u);\n"
"    palette[1] = uint4(c1, 255u);\n"
"    if (format == 1u && e0 <= e1) {\n"
"        palette[2] = uint4((c0 + c1 + 1u) / 2u, 255u);\n"
"        palette[3] = uint4(0u);\n"
"    } else {\n"
"        palette[2] = uint4(lerp3_q8(c0, c1, 172u), 255u);\n"
"        palette[3] = uint4(lerp3_q8(c0, c1, 84u), 255u);\n"
"    }\n"
"    uint alpha[8] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };\n"
"    uint alpha_lo = 0u, alpha_hi = 0u;\n"
"    if (format == 3u) {\n"
"        const uint w0 = blk[0], w1 = blk[1];\n"
"        const uint a0 = w0 & 255u, a1 = (w0 >> 8) & 255u;\n"
"        alpha_lo = (w0 >> 16) | (w1 << 16);\n"
"        alpha_hi = w1 >> 16;\n"
"        alpha[0] = a0; alpha[1] = a1;\n"
"        if (a0 > a1) {\n"
"            for (uint i = 0u; i < 6u; ++i) alpha[i + 2u] = lerp_q8(a0, a1, kSixWeights[i]);\n"
"        } else {\n"
"            for (uint i = 0u; i < 4u; ++i) alpha[i + 2u] = lerp_q8(a0, a1, kFiveWeights[i]);\n"
"            alpha[6] = 0u; alpha[7] = 255u;\n"
"        }\n"
"    }\n"
"    const uint bx = gid % p.blocks_w;\n"
"    const uint by = gid / p.blocks_w;\n"
"    for (uint q = 0u; q < 16u; ++q) {\n"
"        const uint x = bx * 4u + (q & 3u);\n"
"        const uint y = by * 4u + (q >> 2);\n"
"        if (x >= p.width || y >= p.height) continue;\n"
"        const uint4 c = palette[(indices >> (2u * q)) & 3u];\n"
"        uint a = c.w;\n"
"        if (format == 2u) {\n"
"            a = ((blk[q >> 3] >> ((q & 7u) * 4u)) & 15u) * 17u;\n"
"        } else if (format == 3u) {\n"
"            const uint bit = 3u * q;\n"
"            uint ai;\n"
"            if (bit >= 32u)      ai = (alpha_hi >> (bit - 32u)) & 7u;\n"
"            else if (bit <= 29u) ai = (alpha_lo >> bit) & 7u;\n"
"            else                 ai = ((alpha_lo >> bit) | (alpha_hi << (32u - bit))) & 7u;\n"
"            a = alpha[ai];\n"
"        }\n"
"        rgba[y * p.width + x] = c.x | (c.y << 8) | (c.z << 16) | (a << 24);\n"
"    }\n"
"}\n";

typedef struct { uint32_t width, height, blocks_w, blocks_h, format; } RsxBcParams;

/* Two shared buffers that live until the command buffer using them completes. */
@interface RsxMetalBcSlot : NSObject
@property(nonatomic, strong) id<MTLBuffer> blocks;
@property(nonatomic, strong) id<MTLBuffer> rgba;
@end
@implementation RsxMetalBcSlot
@end

/* One decode committed and not yet completed: what it fills (for a timeout's report) and the texture it writes. */
@interface RsxMetalBcPending : NSObject
@property(nonatomic, copy) NSString* what;
@property(nonatomic, strong) id<MTLTexture> texture;
@end
@implementation RsxMetalBcPending
@end

/* Decodes in flight. A decode holds up to a level's blocks plus four times that in RGBA; capping them bounds
 * the staging a texture-heavy scene can pin on a 3 GB device. */
enum { RSX_METAL_BC_SLOTS = 4 };
/* A slot that grew past this (a 2048^2 level) gives its buffers back when it is released. */
static const NSUInteger kRsxBcKeepBytes = 32u << 20;
static const NSUInteger kRsxBcMinBuffer = 64u << 10;

/* Slice of a bounded wait: short enough that a gate suspension or resume is noticed within a frame's time, long
 * enough that waiting is a sleep, not a spin. */
static const uint64_t kRsxBcWaitSliceMs = 10;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

@implementation RsxMetalBc {
    id<MTLDevice> _device;
    id<MTLComputePipelineState> _pso;
    rsx_metal_bc_host _host;
    dispatch_semaphore_t _slots;
    pthread_mutex_t _lock;                  /* _free, the failure record and the test hooks */
    NSMutableArray<RsxMetalBcSlot*>* _free;
    NSMutableArray<RsxMetalBcPending*>* _pending;   /* decodes committed and not yet completed: a timeout's report and the
                                                     * textures a CPU write must not overlap */
    unsigned _failures;
    NSString* _failureTexture;              /* the first failed decode: what it was filling ... */
    NSString* _failureError;                /* ... and what Metal said */
#ifdef RSX_METAL_BC_TEST_HOOKS
    BOOL _testFail;
    useconds_t _testDelayUs;
    BOOL _testHold;
    dispatch_semaphore_t _testRelease;
    unsigned _testSlices;                   /* slices a waitIdleWithin waited without getting a slot */
    void (^_testAtDeadline)(void);          /* run once when a bounded wait's budget runs out, before the report */
    BOOL _testSlotHold;                     /* the next decode's handler pauses after publishing its outcome ... */
    dispatch_semaphore_t _testSlotReached, _testSlotGo;   /* ... and before it returns its slot */
#endif
}

- (instancetype)initWithDevice:(id<MTLDevice>)device
                          host:(const rsx_metal_bc_host*)host
                         error:(NSError* __autoreleasing *)error
{
    self = [super init];
    if (!self) return nil;
    if (host) _host = *host;
    _device = device;
    _slots = dispatch_semaphore_create(RSX_METAL_BC_SLOTS);
    _free = [NSMutableArray new];
    _pending = [NSMutableArray new];
#ifdef RSX_METAL_BC_TEST_HOOKS
    _testRelease = dispatch_semaphore_create(0);
    _testSlotReached = dispatch_semaphore_create(0);
    _testSlotGo = dispatch_semaphore_create(0);
#endif
    pthread_mutex_init(&_lock, NULL);
    if (_host.gate_enter) _host.gate_enter(_host.user);
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:kRsxBcMSL]
                                              options:nil error:error];
    id<MTLFunction> fn = [lib newFunctionWithName:@"rsx_bc_decode"];
    if (fn) _pso = [device newComputePipelineStateWithFunction:fn error:error];
    if (_host.gate_leave) _host.gate_leave(_host.user);
    if (!_pso) return nil;
    return self;
}

- (void)dealloc
{
    pthread_mutex_destroy(&_lock);
}

- (id<MTLBuffer>)bufferOfAtLeast:(NSUInteger)bytes
{
    NSUInteger cap = kRsxBcMinBuffer;
    while (cap < bytes) cap *= 2u;
    return _host.new_buffer ? _host.new_buffer(_host.user, cap)
                            : [_device newBufferWithLength:cap options:MTLResourceStorageModeShared];
}

/* A free slot (or a new one) whose buffers hold at least the sizes asked, or nil when a buffer is refused;
 * the slot's semaphore count is then returned. */
- (RsxMetalBcSlot*)acquireSlotWithBlocks:(NSUInteger)blocks rgba:(NSUInteger)rgba
{
    dispatch_semaphore_wait(_slots, DISPATCH_TIME_FOREVER);
    RsxMetalBcSlot* slot = nil;
    pthread_mutex_lock(&_lock);
    slot = [_free lastObject];
    if (slot) [_free removeLastObject];
    pthread_mutex_unlock(&_lock);
    if (!slot) slot = [RsxMetalBcSlot new];
    if (!slot.blocks || [slot.blocks length] < blocks) slot.blocks = [self bufferOfAtLeast:blocks];
    if (!slot.rgba || [slot.rgba length] < rgba) slot.rgba = [self bufferOfAtLeast:rgba];
    if (!slot.blocks || !slot.rgba) {
        [self releaseSlot:slot];
        return nil;
    }
    return slot;
}

- (void)releaseSlot:(RsxMetalBcSlot*)slot
{
    pthread_mutex_lock(&_lock);
    [_free addObject:slot];
    pthread_mutex_unlock(&_lock);
    dispatch_semaphore_signal(_slots);
}

/* Called from a command buffer's completion handler with _lock held, in the same critical section that ends the
 * decode's _pending entry. Only the first failure is kept. */
- (void)recordFailureLockedOf:(NSString*)texture error:(NSString*)error
{
    __atomic_fetch_add(&_failures, 1u, __ATOMIC_RELAXED);
    if (!_failureTexture) { _failureTexture = texture; _failureError = error; }
}

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
          completion:(void (^)(id<MTLCommandBuffer> cb))completion
{
    size_t block_bytes;
    switch (format) {
    case RSX_BC_FORMAT_BC1: block_bytes = 8u; break;
    case RSX_BC_FORMAT_BC2:
    case RSX_BC_FORMAT_BC3: block_bytes = 16u; break;
    default: return NO;
    }
    if (!blocks || !texture || !queue || !width || !height ||
        [texture pixelFormat] != MTLPixelFormatRGBA8Unorm)
        return NO;
    const uint32_t blocks_w = width / 4u + (width % 4u != 0u);
    const uint32_t blocks_h = height / 4u + (height % 4u != 0u);
    /* A level is far below these bounds (the engine caps a side at 4096); they keep the products exact. */
    if (width > 16384u || height > 16384u) return NO;
    const size_t block_total = (size_t)blocks_w * blocks_h * block_bytes;
    const size_t rgba_total = (size_t)width * height * 4u;
    if (block_total > size) return NO;

    RsxMetalBcSlot* slot = [self acquireSlotWithBlocks:block_total rgba:rgba_total];
    if (!slot) return NO;
    memcpy([slot.blocks contents], blocks, block_total);

    if (_host.gate_enter) _host.gate_enter(_host.user);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = cb ? [cb computeCommandEncoder] : nil;
    if (!compute) {
        if (_host.gate_leave) _host.gate_leave(_host.user);
        [self releaseSlot:slot];
        return NO;
    }
    const RsxBcParams params = { width, height, blocks_w, blocks_h, (uint32_t)format };
    [compute setComputePipelineState:_pso];
    [compute setBuffer:slot.blocks offset:0 atIndex:0];
    [compute setBuffer:slot.rgba offset:0 atIndex:1];
    [compute setBytes:&params length:sizeof params atIndex:2];
    const NSUInteger groups = ((NSUInteger)blocks_w * blocks_h + 63u) / 64u;
    [compute dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [compute endEncoding];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:slot.rgba sourceOffset:0
       sourceBytesPerRow:(NSUInteger)width * 4u
     sourceBytesPerImage:rgba_total
              sourceSize:MTLSizeMake(width, height, 1)
               toTexture:texture destinationSlice:slice destinationLevel:level
       destinationOrigin:MTLOriginMake(0, 0, z)];
    [blit endEncoding];
    NSString* what = [NSString stringWithFormat:@"%s %ux%u level %lu slice %lu z %lu",
                      format == RSX_BC_FORMAT_BC1 ? "BC1" : format == RSX_BC_FORMAT_BC2 ? "BC2" : "BC3",
                      width, height, (unsigned long)level, (unsigned long)slice, (unsigned long)z];
#ifdef RSX_METAL_BC_TEST_HOOKS
    pthread_mutex_lock(&_lock);
    const BOOL inject = _testFail;
    const useconds_t delay_us = _testDelayUs;
    _testFail = NO;
    _testDelayUs = 0;
    const BOOL hold = _testHold;
    _testHold = NO;
    /* Captured, not read from self in the handler: the Metal thread has no ordering with init that TSan sees. */
    dispatch_semaphore_t release = _testRelease;
    const BOOL slot_hold = _testSlotHold;
    _testSlotHold = NO;
    dispatch_semaphore_t slot_reached = _testSlotReached, slot_go = _testSlotGo;
    pthread_mutex_unlock(&_lock);
#endif
    pthread_mutex_lock(&_lock);
    RsxMetalBcPending* entry = [RsxMetalBcPending new];
    entry.what = what;
    entry.texture = texture;
    [_pending addObject:entry];
    pthread_mutex_unlock(&_lock);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
#ifdef RSX_METAL_BC_TEST_HOOKS
        if (hold) dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER);
        if (delay_us) usleep(delay_us);
        const BOOL injected = inject;
#else
        const BOOL injected = NO;
#endif
        BOOL failed = injected || [done status] != MTLCommandBufferStatusCompleted || [done error];
        NSString* why = nil;
        if (failed) {
            why = injected ? @"injected failure (test hook)"
                           : [NSString stringWithFormat:@"%@ (code %ld)",
                              [[done error] localizedDescription] ?: @"no error object",
                              (long)[[done error] code]];
            PS3_LOG(PS3_LOG_WARN, "rsx engine/metal", "BC decode command buffer failed: %s: %s\n",
                    [what UTF8String], [why UTF8String]);
        }
        /* The outcome is published in one critical section: the failure record and the end of the pending entry
         * together, so a waiter that sees the entry gone sees the failure. The slot goes back after, and is not
         * part of the outcome; the semaphore signal in releaseSlot is the last thing this handler does with the
         * decoder (the caller's completion block aside). */
        pthread_mutex_lock(&self->_lock);
        if (failed) [self recordFailureLockedOf:what error:why];
        { const NSUInteger at = [self->_pending indexOfObjectIdenticalTo:entry];   /* by identity: equal levels can be pending twice */
          if (at != NSNotFound) [self->_pending removeObjectAtIndex:at]; }
        pthread_mutex_unlock(&self->_lock);
#ifdef RSX_METAL_BC_TEST_HOOKS
        if (slot_hold) {   /* outcome published, slot not yet returned: a waiter's deadline may fall here */
            dispatch_semaphore_signal(slot_reached);
            dispatch_semaphore_wait(slot_go, DISPATCH_TIME_FOREVER);
        }
#endif
        [self releaseSlot:slot];
        if (completion) completion(done);
    }];
    if (_host.gate_checkpoint) _host.gate_checkpoint(_host.user);
    /* The host commits it where it records the last buffer committed on the queue (lifecycle drain). */
    if (_host.commit) _host.commit(_host.user, cb); else [cb commit];
    if (_host.gate_leave) _host.gate_leave(_host.user);
    return YES;
}

/* Takes every slot, then gives them back, so it returns once each decode committed so far has completed and
 * recorded its outcome. Idle, that is four uncontended semaphore operations each way. It waits in slices of
 * kRsxBcWaitSliceMs on the semaphore (no spinning) and charges only *active* time against the budget: each
 * slice adds the host's active clock's advance (host.active_ms, which does not run while the GPU gate is
 * suspended, i.e. the app is inactive), so a wait that spans a suspension does not run out.
 *
 * One deadline, never extended. When the budget is spent the pending list is read under _lock, the lock a
 * completion handler publishes its outcome under. The handler records its failure (if any) and removes its
 * _pending entry in ONE critical section and returns its slot afterwards, so:
 *   - a decode still in _pending has not published its outcome: that is the timeout, and the report names
 *     exactly those decodes (never an empty list);
 *   - nothing in _pending means every committed decode has published its outcome, a failure included, and only
 *     the hand-back of a slot (not part of the outcome) is still in flight: no timeout. nil is returned
 *     without waiting for those slots, and the caller reads failureTexture, which is already final. The trim
 *     below is skipped then, as it needs every slot. */
- (NSString*)waitIdleWithin:(uint64_t)budgetMs
{
    uint64_t spent = 0;
    uint64_t last = _host.active_ms ? _host.active_ms(_host.user) : monotonic_ms();
    int held = 0;
    NSString* pending = nil;
    while (held < RSX_METAL_BC_SLOTS) {
        if (dispatch_semaphore_wait(_slots, dispatch_time(DISPATCH_TIME_NOW, kRsxBcWaitSliceMs * NSEC_PER_MSEC)) == 0) {
            held++;
            continue;
        }
#ifdef RSX_METAL_BC_TEST_HOOKS
        __atomic_fetch_add(&_testSlices, 1u, __ATOMIC_RELEASE);
#endif
        const uint64_t now = _host.active_ms ? _host.active_ms(_host.user) : monotonic_ms();
        if (now > last) { spent += now - last; last = now; }   /* a clock that stepped back charges nothing */
        if (spent < budgetMs) continue;
#ifdef RSX_METAL_BC_TEST_HOOKS
        void (^at_deadline)(void) = nil;
        pthread_mutex_lock(&_lock);
        at_deadline = _testAtDeadline;
        _testAtDeadline = nil;
        pthread_mutex_unlock(&_lock);
        if (at_deadline) at_deadline();
#endif
        pthread_mutex_lock(&_lock);
        if ([_pending count]) pending = [[_pending valueForKey:@"what"] componentsJoinedByString:@"; "];
        pthread_mutex_unlock(&_lock);
        break;
    }
    if (held == RSX_METAL_BC_SLOTS) {
        /* Every slot is back in _free and no completion handler is running: the one place a slot that grew past
         * kRsxBcKeepBytes (a 2048^2 level) gives its buffers back. Only this thread and the submitting thread
         * touch a slot's buffers, never a Metal thread, so no completion handler races them. */
        pthread_mutex_lock(&_lock);
        for (RsxMetalBcSlot* slot in _free) {
            if ([slot.blocks length] + [slot.rgba length] > kRsxBcKeepBytes) {
                slot.blocks = nil;
                slot.rgba = nil;
            }
        }
        pthread_mutex_unlock(&_lock);
    }
    for (int i = 0; i < held; ++i) dispatch_semaphore_signal(_slots);
    return pending;
}

- (unsigned)inFlight
{
    pthread_mutex_lock(&_lock);
    const unsigned n = (unsigned)[_pending count];
    pthread_mutex_unlock(&_lock);
    return n;
}

- (BOOL)hasPendingDecodeFor:(id<MTLTexture>)texture
{
    BOOL any = NO;
    pthread_mutex_lock(&_lock);
    for (RsxMetalBcPending* e in _pending)
        if (e.texture == texture) { any = YES; break; }
    pthread_mutex_unlock(&_lock);
    return any;
}

- (NSString*)failureTexture
{
    pthread_mutex_lock(&_lock);
    NSString* s = _failureTexture;
    pthread_mutex_unlock(&_lock);
    return s;
}

- (NSString*)failureError
{
    pthread_mutex_lock(&_lock);
    NSString* s = _failureError;
    pthread_mutex_unlock(&_lock);
    return s;
}

- (unsigned)failureCount
{
    return __atomic_load_n(&_failures, __ATOMIC_RELAXED);
}

#ifdef RSX_METAL_BC_TEST_HOOKS
- (void)testInjectNextDecodeFailure:(BOOL)fail delayMicroseconds:(useconds_t)delay
{
    pthread_mutex_lock(&_lock);
    _testFail = fail;
    _testDelayUs = delay;
    pthread_mutex_unlock(&_lock);
}

- (void)testHoldNextDecodeCompletion
{
    pthread_mutex_lock(&_lock);
    _testHold = YES;
    pthread_mutex_unlock(&_lock);
}

- (void)testReleaseHeldCompletion
{
    dispatch_semaphore_signal(_testRelease);
}

- (unsigned)testBlockedSlices
{
    return __atomic_load_n(&_testSlices, __ATOMIC_ACQUIRE);
}

- (void)testOnBudgetExhausted:(void (^)(void))block
{
    pthread_mutex_lock(&_lock);
    _testAtDeadline = [block copy];
    pthread_mutex_unlock(&_lock);
}

- (void)testPauseNextDecodeBeforeSlotReturn
{
    pthread_mutex_lock(&_lock);
    _testSlotHold = YES;
    pthread_mutex_unlock(&_lock);
}

- (void)testWaitUntilHandlerPaused
{
    dispatch_semaphore_wait(_testSlotReached, DISPATCH_TIME_FOREVER);
}

- (void)testResumePausedHandler
{
    dispatch_semaphore_signal(_testSlotGo);
}
#endif

@end

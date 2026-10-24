/*
 * ps3recomp - SPURS JobQueue2 HLE: the C contract of the PPU export surface
 *
 * The 17 `cellSpursJq` imports of a title are thin adapters: they decode the
 * PPU arguments (HLE ABI, docs/HLE_ABI.md), call one function below, and return
 * its value as the firmware's s32 (wire form 0x80410Axx for an error). Every
 * firmware guard, guest-memory update, ring write and wait lives behind this
 * header, in libs/spurs/jq2/executor.cpp, so the surface carries no JobQueue
 * logic of its own.
 *
 *   export (NID)                                         calls
 *   _cellSpursCreateJobQueue (0xF244E799), 10 args       jq2_queue_create, pool = pool_attr = 0
 *   _cellSpursCreateJobQueueWithJobDescriptorPool
 *                              (0x983BE7FC), 12 args     jq2_queue_create
 *   cellSpursShutdownJobQueue (0x8C97A96C)               jq2_queue_shutdown
 *   cellSpursJoinJobQueue (0x8CE4CFFA)                   jq2_queue_join, then *out = *exit_value
 *   cellSpursJobQueuePort2Create (0xF11FE0F1)            jq2_port_create
 *   cellSpursJobQueuePort2Destroy (0xC485D207)           jq2_port_destroy
 *   _cellSpursJobQueuePort2PushJobBody (0xCF89F218)      jq2_port_push_job
 *   _cellSpursJobQueuePort2CopyPushJobBody (0xC39173A4)  jq2_port_copy_push
 *   cellSpursJobQueuePort2PushSync (0xFC526B72)          jq2_port_push_sync
 *   cellSpursJobQueuePort2Sync (0xFACB3CED)              jq2_port_sync
 *
 * The seven JobQueue attribute exports (Initialize, SetMaxSizeJobDescriptor,
 * SetGrabParameters, SetDoBusyWaiting, SetSubmitWithEntryLock,
 * SetIsJobTypeMemoryCheck, SetIsHaltOnError) only edit the caller's attribute
 * block and are the surface's own. The create body reads
 * exactly two of its fields, as the firmware does (JQ-FW-PPU, jq2fw-020):
 *
 *   attr+0x04  the class-count field, 1..16 (jq2fw-019; the surface's Initialize
 *              must leave it in range)
 *   attr+0x08  maximum job descriptor size, 256..1023 and a multiple of 128;
 *              stored at queue+0xF14 and the bound of every push
 *
 * Conventions
 *   - Every pointer is a guest effective address (u32). The one host pointer is
 *     the out parameter of jq2_queue_join.
 *   - Every function returns the firmware's s32: 0 on success, otherwise
 *       JQ2_ERR_AGAIN         0x80410A01  ring full, port slots exhausted, Sync
 *                                         poll with jobs outstanding (mode 4)
 *       JQ2_ERR_INVAL         0x80410A02  argument out of range
 *       JQ2_ERR_BUSY          0x80410A0A  Sync claim collision, destroy with
 *                                         submissions outstanding
 *       JQ2_ERR_STAT          0x80410A0F  port or queue not in a usable state
 *       JQ2_ERR_ALIGN         0x80410A10  misaligned EA
 *       JQ2_ERR_NULL_POINTER  0x80410A11  null EA
 *     No other value is ever produced (SPURS-JQ2-CONTRACT C2).
 *   - A failure the firmware cannot return and the runtime cannot honour is a
 *     named stop (include/ps3emu/stop.h), never a status.
 *   - Any thread may call any function. A blocking call blocks only its caller.
 *   - The guest buffers (queue, command ring, pool, port) stay the caller's
 *     (contract L2); the queue's host state lives outside them and is keyed by
 *     the queue EA.
 *
 * Blocking calls: jq2_port_sync (mode 0), jq2_queue_join, and
 * jq2_port_copy_push when the descriptor pool has no free slot of the class.
 */

#ifndef PS3RECOMP_SPURS_JQ2_JQ2_H
#define PS3RECOMP_SPURS_JQ2_JQ2_H

#include <stdint.h>
#include "ps3emu/stop.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t ppu_vm_size;

/* Returns the guest VM limit: ppu_vm_size if set, else 2^32. */
static inline uint64_t jq2_guest_limit(void)
{
    return ppu_vm_size ? (uint64_t)ppu_vm_size : 0x100000000ull;
}

/* Returns 1 if [ea, ea+size) sits wholly within guest memory.
 * Computes the bound in 64 bits so a large ea + size that would wrap past the
 * top of the 32-bit space cannot wrap to a low address. */
static inline int jq2_guest_range_ok(uint64_t ea, uint64_t size)
{
    const uint64_t limit = jq2_guest_limit();
    return ea <= limit && size <= limit - ea;
}

#define JQ2_OK 0
#define JQ2_ERR_AGAIN ((int32_t)0x80410A01u)
#define JQ2_ERR_INVAL ((int32_t)0x80410A02u)
#define JQ2_ERR_BUSY ((int32_t)0x80410A0Au)
#define JQ2_ERR_STAT ((int32_t)0x80410A0Fu)
#define JQ2_ERR_ALIGN ((int32_t)0x80410A10u)
#define JQ2_ERR_NULL_POINTER ((int32_t)0x80410A11u)

/* Largest host-thread worker count and ring depth the firmware accepts. */
#define JQ2_MAX_WORKERS 8u
#define JQ2_MAX_DEPTH 0x8000u

/* ---- queue ------------------------------------------------------------- */

/* The common create body (0x020003CC): both create exports end here, the
 * 10-argument one with pool = pool_attr = 0.
 *
 *   revision, sdk_version  stored at queue+0xFFC / +0xFF8
 *   spurs                  128-aligned SPURS EA, stored at queue+0xF10
 *   queue                  128-aligned, >= 0x1000 bytes; zeroed (0x1000) here
 *   attr                   the attribute block (fields above)
 *   pool, pool_attr        both 0 or both 128-aligned (the descriptor pool of
 *                          the CopyPush path); anything else is INVAL
 *   name                   guest C string naming the queue, may be 0; the
 *                          firmware names its SPURS workload with it, the host
 *                          workers are anonymous, so it is not read
 *   command_ring           128-aligned, depth entries of 8 bytes; zeroed here
 *   command_depth          1..0x8000 (see JQ2.md for the depths the model
 *                          supports)
 *   worker_count           1..8: the number of host SPU workers (threads)
 *   extra                  unused by the firmware body as far as RE shows
 *
 * Guards in firmware order: null (spurs, queue, attr, command_ring) A11;
 * alignment (spurs, queue, command_ring, then pool, pool_attr) A10; pool xor
 * pool_attr A02; depth / worker_count A02; attribute range A02.
 *
 * Leaves the queue as the firmware leaves it (m1r-004, JQ-FW-PPU): ring EA at
 * +0x10, depth at +0x14, ring indices and phase at +0x00/+0x04 (read 0, write 0,
 * +0x04 = 0x60), event flag ids at +0x80/+0x84 (LV2 event flags), the port slot
 * bitmap at +0x580 and its count at +0x70 (+0x180..+0x57F is the PM's own and is
 * never written), max descriptor at +0xF14, and the pool's fields when a pool
 * is given. Starts
 * the workers. A queue EA that already has a live queue is STAT. */
int32_t jq2_queue_create(uint32_t revision, uint32_t sdk_version, uint32_t spurs, uint32_t queue, uint32_t attr,
                         uint32_t pool, uint32_t pool_attr, uint32_t name, uint32_t command_ring,
                         uint32_t command_depth, uint32_t worker_count, uint32_t extra);

/* cellSpursShutdownJobQueue: refuse further pushes and tell the workers to
 * drain the ring and exit. Null A11, alignment A10, no live queue STAT, a live
 * port still attached BUSY (the game's order is Destroy -> Shutdown -> Join).
 * Returns at once; Join waits. */
int32_t jq2_queue_shutdown(uint32_t queue);

/* cellSpursJoinJobQueue: block until the ring is drained and every job
 * finished (contract L1), stop and join the workers, destroy the event flags,
 * and forget the queue. `*exit_value` receives the word at queue+0xF04 (what
 * the firmware writes through its `out` pointer); the caller stores it as a
 * guest u32. The queue must have been shut down (STAT otherwise). */
int32_t jq2_queue_join(uint32_t queue, uint32_t* exit_value);

/* ---- port -------------------------------------------------------------- */

/* cellSpursJobQueuePort2Create(port, queue): zero the 128-byte port, link the
 * queue at +0x08/+0x18, allocate a slot from the queue's bitmap into +0x1C, set
 * +0x20 = 1. Null A11, alignment A10, no live queue or shut down STAT, no free
 * slot AGAIN. */
int32_t jq2_port_create(uint32_t port, uint32_t queue);

/* cellSpursJobQueuePort2Destroy(port): null A11, alignment A10, not an
 * initialised port STAT, submissions outstanding (port+0x10 != 0) BUSY. Frees
 * the slot, detaches the link, clears +0x20. */
int32_t jq2_port_destroy(uint32_t port);

/* ---- pushes ------------------------------------------------------------ */

/* _cellSpursJobQueuePort2PushJobBody(port, descriptor, size, tag, flags):
 * enqueue a JOB naming the caller's descriptor, which is NOT copied: the job
 * reads it when a worker admits it (contract S2). size is 64, 128 or 256
 * (<= the queue maximum), descriptor 16-aligned, tag 0..15 (INVAL otherwise).
 * flags & 1 selects whether the entry carries the port EA, and with it the
 * submission count B at port+0x10 and the completion credit: a job pushed
 * without it is run but never credited to the port. Full ring AGAIN; a port
 * whose Sync holds the claim BUSY. Wakes the workers. */
int32_t jq2_port_push_job(uint32_t port, uint32_t descriptor, uint32_t size, uint32_t tag, uint32_t flags);

/* _cellSpursJobQueuePort2CopyPushJobBody(port, source, size, copy_size, tag,
 * flags): as above for a COPY. Takes a pool slot of copy_size bytes (64, or
 * 128..0x3FF in multiples of 128; size <= copy_size), copies exactly `size`
 * bytes from `source` into it BEFORE the entry is enqueued (contract S3: the
 * caller may reuse `source` on return), and queues the slot's EA. Blocks while
 * the class has no free slot. The slot goes back to the pool when the job has
 * run. A queue created without a pool has no classes: NULL_POINTER, as the
 * firmware's allocator returns for an unconfigured class (jq2c-015). */
int32_t jq2_port_copy_push(uint32_t port, uint32_t source, uint32_t size, uint32_t copy_size, uint32_t tag,
                           uint32_t flags);

/* cellSpursJobQueuePort2PushSync(port, tag_mask, flags): enqueue a SYNC for the
 * tags in tag_mask (non-zero, 16 bits; INVAL otherwise). The port's words are
 * not touched. */
int32_t jq2_port_push_sync(uint32_t port, uint32_t tag_mask, uint32_t flags);

/* cellSpursJobQueuePort2Sync(port, mode): mode 0 blocks until every job pushed
 * with flags & 1 has been credited, mode 4 returns AGAIN while any is
 * outstanding, other bits of mode INVAL. A concurrent Sync on the port BUSY.
 * Port accounting is libs/spurs/jq2/port.hpp's. */
int32_t jq2_port_sync(uint32_t port, uint32_t mode);

/* ---- observation ------------------------------------------------------- */

typedef struct jq2_stats {
    uint64_t pushed;      /* entries written to the ring (JOB, COPY, SYNC) */
    uint64_t dispatched;  /* jobs admitted by a worker */
    uint64_t executed;    /* jobs that ran to a normal return */
    uint64_t credited;    /* port completions credited, always after the run */
    uint64_t uncredited;  /* executed jobs that carried no port EA */
} jq2_stats;

/* Counters of a live queue; STAT when there is none. At quiescence
 * dispatched == executed == credited + uncredited, and at any instant
 * credited + uncredited <= executed <= dispatched. */
int32_t jq2_queue_stats(uint32_t queue, jq2_stats* out);

/* ---- job observer (the T1 gate's emitter) ------------------------------ */

struct spu_context;

/* A job as a worker admitted it, and the local store the loader laid out for
 * it (runtime/spu/spu_workload.h, spu_job_layout: LS offsets and byte sizes,
 * passed out by the loader, not recomputed). */
typedef struct jq2_job_info {
    uint32_t queue, port;             /* port 0: pushed without flags & 1, never credited */
    uint32_t descriptor_ea;           /* the caller's descriptor, or the COPY's pool slot */
    uint32_t descriptor_size;         /* 64, 128 or 256 */
    const uint8_t* descriptor;        /* the bytes fetched at admission (valid for the call) */
    uint32_t tag, copy, worker, dispatch_id;
    uint32_t image_ea, image_ls, image_bytes, bss_bytes;
    uint32_t desc_ls, desc_bytes;
    uint32_t io_ls, io_bytes, out_ls, out_bytes;
    uint32_t scratch_ls, scratch_bytes, stack_bytes;
    uint32_t r1;                      /* the stack pointer the job is entered with */
    uint32_t ctx_ls;
} jq2_job_info;

/* Callbacks on the worker thread that runs the job, with that worker's
 * spu_context (the place to install an spu_observe observer). job_start runs
 * once the job is staged and before its first instruction; job_end once the
 * job returned, with r3 as it returned it, before anything is released or
 * credited. A job that ends in a named stop never reaches job_end. Either may
 * be NULL. */
typedef struct jq2_job_observer {
    void* user;
    void (*job_start)(void* user, struct spu_context* ctx, const jq2_job_info* job);
    void (*job_end)(void* user, struct spu_context* ctx, uint32_t r3, const jq2_job_info* job);
} jq2_job_observer;

/* Install (or, with NULL, remove) the observer of a live queue; it applies to
 * the jobs admitted afterwards. Off unless installed: an unset queue takes the
 * plain path. STAT when there is no such queue. */
int32_t jq2_set_job_observer(uint32_t queue, const jq2_job_observer* observer);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SPURS_JQ2_JQ2_H */

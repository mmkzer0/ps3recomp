/*
 * ps3recomp - SPURS JobQueue2 HLE executor
 *
 * The runtime's stand-in for the firmware's job queue: the policy module (PM)
 * that runs on the SPUs, and the PPU half that feeds it. One Queue is one guest
 * JobQueue (a 128-aligned guest block the title owns); everything the firmware
 * keeps in that block is kept there, byte for byte where the evidence settles
 * the layout, and the host holds only what the firmware holds in SPU local
 * store or in a SPURS workload.
 *
 *   PPU side   jq2.h is the entry surface. A push validates, accounts on the
 *              port line (port.hpp), writes one 8-byte entry (ring.hpp) into
 *              the guest command ring through ring_push() and wakes the workers.
 *   SPU side   `worker_count` host threads are the SPU instances. A worker runs
 *              a PM intake pass (sched.hpp, over the guest ring) under the queue
 *              lock; a JOB it admits has its descriptor read from guest memory
 *              right there, and the job then runs OUTSIDE the lock on the
 *              descriptor copy it took (contract S2).
 *   Completion the port is credited only after the job has run to a normal
 *              return (credit_completion(), port.hpp), a COPY's pool slot goes
 *              back to the pool first, then the in-flight count drops and
 *              barriers release. A job that does anything else is a named stop
 *              (jq2-job-end) and is never credited.
 *
 * Guest layout of the queue block (every field big-endian; "evidence" is the
 * research id, [assumption] is a choice the evidence does not settle):
 *
 *   +0x00 u16 read index, +0x02 u16 write index          m1r-003: one u64 word
 *   +0x04 u32 flags: 0x20 the writer's phase, 0x40 the PM's expected phase;
 *         0x60 at create                                  m1r-003, m1r-004;
 *         [assumption] bit 0x40 is the PM's phase: it is the other bit set in
 *         the 0x60 the firmware stores, and the PM flips its expected phase on
 *         wrap (m1r-015); nothing reads it but the executor
 *   +0x10 u32 command ring EA, +0x14 u16 depth, +0x16 u16 256   jq2fw-018
 *   +0x6C u32 workload flags: bit 0x20 set by Join          jq2fw-031
 *   +0x70 u32 attached ports (<= 0x400)                     jq2fw-004
 *   +0x80, +0x84 u32 LV2 event flag ids                     jq2fw-018
 *   +0x180..+0x57F never written: +0x180..+0x1FF is the PM's own line of 16
 *         per-grab-slot 8-byte records (GETLLAR/PUTLLUC, type-3 path at line +
 *         slot*8), private scheduling state no PPU path reads; this executor
 *         keeps its out-of-order marks in host state (the scheduler's contract
 *         is unchanged, only where the marks live)
 *   +0x580..+0x5FF port slot bitmap                         jq2fw-004
 *   +0x600..+0x6FF descriptor pool classes                  pool.hpp
 *   +0xF04 u32 exit value, +0xF10 spurs EA, +0xF14 max descriptor size,
 *   +0xFF8 SDK version, +0xFFC revision                     jq2fw-018/019
 *
 * Port2Sync blocking. A blocked Sync registers a waiter id in the port line
 * (port.hpp) and sleeps on one bit of the queue's first 64-bit event flag
 * (+0x80): id n is bit n of it. The four BE32 guest bitmap words at
 * +0x88..+0x94 allocate MSB first through reservation-model transactions.
 * Id0 leaks on first allocation (the firmware's sentinel collision), leaving
 * ids 1..63 wakeable. The PM's wake helper (0x6348) rejects ids above 63, so
 * the 64th concurrent Sync publishes its attempted id64 allocation and stops
 * with `jq2-wake reason=waiter-id`, rather than waiting forever.
 *
 * A job in the scheduler's terms: one instance per worker with ONE job slot,
 * because a worker runs a job to completion before it examines the ring again
 * (the PM interleaves two coroutine slots on one SPU; the model's observable
 * effects, admission order and barriers, do not depend on it).
 */

#ifndef PS3RECOMP_SPURS_JQ2_EXECUTOR_HPP
#define PS3RECOMP_SPURS_JQ2_EXECUTOR_HPP

#include "jq2/jq2.h"
#include "jq2/sched.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

struct spu_context;

namespace ps3recomp::spurs::jq2 {

/* The body of a job: runs on the thread of the worker that admitted it, from
 * inside spu_run_spurs_job_snapshot() (runtime/spu/spurs_job.c), with the job's
 * local store staged. It returns when the job has returned to the job manager
 * and must end the process with a named stop for any other end. Entries that
 * are lifted SPU code (the job-image registry, C1) plug in here. */
using JobEntryFn = void (*)(::spu_context*);

struct JobBody {
    JobEntryFn entry = nullptr;
    int image_id = -1; /* the loader's image id; -1 selects no lifted image */
};

/* What a worker knows about a job it admitted. */
struct JobStart {
    std::uint32_t queue_ea = 0;
    std::uint32_t port_ea = 0;       /* 0: pushed without flags & 1, never credited */
    std::uint32_t descriptor_ea = 0; /* the caller's descriptor, or the COPY's pool slot */
    std::uint32_t size = 0;          /* descriptor bytes */
    std::uint8_t tag = 0;
    bool copy = false;
    unsigned worker = 0;
    std::uint32_t dispatch_id = 0;
    /* The descriptor as it was when the job was admitted. */
    std::span<const std::uint8_t> descriptor;
};

/* Seams for the interpreter-or-lifted choice and for observation. All are
 * process-wide, read when a queue is created, and called on worker threads
 * without the queue lock held. */
struct ExecutorHooks {
    void* user = nullptr;
    /* Picks the body of a job; null (or an empty entry) selects the SPU
     * interpreter. */
    JobBody (*select_body)(void* user, const JobStart&) = nullptr;
    /* A job was admitted and its descriptor copied; the job has not started. */
    void (*on_admit)(void* user, const JobStart&) = nullptr;
    /* A job returned; nothing has been credited or released yet. */
    void (*on_done)(void* user, const JobStart&) = nullptr;
    /* A push (job, copy or sync) has passed its checks and is about to take the
     * port-line update and the ring write; it runs on the pushing thread with no
     * queue or line lock held. A test seam: it lets a test hold a push at exactly
     * the point where Shutdown can overtake it (the reservation model exposes no
     * contention signal to observe a pusher parked on a line, and a sleep does
     * not order anything). Null in production, where the cost is one null test.
     * `port_ea` is the pushing port. */
    void (*on_push_checked)(void* user, std::uint32_t port_ea) = nullptr;
};

/* Install hooks for the queues created after this call. */
void set_executor_hooks(const ExecutorHooks& hooks);

/* The body that interprets the job image from LS 0 and accepts only a return to
 * the job manager (pc just past the stop at the return address). */
[[nodiscard]] JobBody interpreter_body() noexcept;

/* The link address spu_run_spurs_job_snapshot plants for a job to return to:
 * the word there is 0, an SPU `stop 0`. */
inline constexpr std::uint32_t kJobReturnLs = 0x3FF00;

/* Test and diagnostic seams on a live queue. While paused no worker starts an
 * intake pass; running jobs finish. False when there is no such queue. */
bool set_intake_paused(std::uint32_t queue_ea, bool paused);

struct QueueView {
    RingIndices indices;
    unsigned outstanding = 0;
    unsigned running = 0;                           /* jobs admitted and not yet completed */
    std::uint16_t barred = 0;                       /* per-tag barrier set */
    std::array<unsigned, kTagCount> inflight{};     /* admitted and not yet completed, per tag */
    std::vector<std::uint16_t> out_of_order;        /* ring indices carrying the mark */
    jq2_stats stats{};
};

/* A coherent view taken under the queue lock. nullopt when there is no such
 * queue. */
[[nodiscard]] std::optional<QueueView> observe_queue(std::uint32_t queue_ea);

}  // namespace ps3recomp::spurs::jq2

#endif  // PS3RECOMP_SPURS_JQ2_EXECUTOR_HPP

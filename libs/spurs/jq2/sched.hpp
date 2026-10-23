/*
 * ps3recomp - SPURS JobQueue2 reference scheduler (queue-level model)
 *
 * A deterministic, single-threaded, time-free model of what the JobQueue2
 * policy module (PM) does with the command ring. It is the oracle the HLE
 * executor (B4) is checked against; it owns no guest memory, runs no jobs and
 * reads no clock. Callers drive it with three operations:
 *
 *   push(entry)           the PPU writer: phase bookkeeping, wrap, publish
 *   intake_pass(instance) one PM intake pass on one SPU, returns what it did
 *   complete(job)         a dispatched job finished on its instance
 *
 * Contract (M1-SCOPE Amendment 2, F2; evidence M1R-REPORT 3 and 8, jq2co-001,
 * jq2co-003/004, m1r-001/002/003). `[assumption]` marks what no source settles.
 *
 *   Queue state is shared by every PM instance (the barrier lives in the
 *   queue's ring line, jq2co-002): ring, read/write index, expected phase,
 *   per-tag in-flight counts, per-tag barrier set. Each instance owns N job
 *   slots (the PM has two coroutine slots).
 *
 *   C1' SYNC(mask) is a per-tag dispatch barrier consumed at admission. Every
 *       masked tag that is not already barred becomes barred, recording the
 *       SYNC's mask as that tag's barrier. The SYNC never fences other tags.
 *   C2  A barred tag is released when every tag of its barrier mask has an
 *       in-flight count of 0 (checked at admission and on every complete()).
 *   C3' A JOB of a barred tag is retained in the ring and not dispatched;
 *       later entries are still examined. At most one JOB is dispatched per
 *       pass. The read index advances over consumed entries and stops at the
 *       first retained one.
 *   C4  Jobs of different tags admitted before a SYNC may run together; a SYNC
 *       waits only for its masked tags.
 *   P   An entry whose w1 bit 0 does not match the expected phase ends the
 *       pass (not yet written). The expected phase starts at the queue's
 *       initial phase and flips when the read index wraps.
 *
 *   Pass rules taken from the PM's static handler (0x24B4-0x2584, M1R-REPORT
 *   8) and needed by the OQ-A read-index trajectory (3 vs 4):
 *   - A SYNC is consumed, then the pass continues only if the SYNC completed
 *     at admission (all masked tags idle and barrier == mask) and every masked
 *     tag was un-barred before it. Otherwise the pass ends (r2 = -1).
 *   - A SYNC whose masked tags are all already barred takes the same skip path
 *     as a barred JOB: it is retained, not consumed, and is re-examined by the
 *     next pass (so it re-arms the barrier once the earlier jobs drained).
 *   - A pass on an instance with no free job slot examines nothing.
 *
 * Points isolated behind one named function each:
 *
 *   Scheduler::mark_consumed_out_of_order(): how an entry consumed *behind* a
 *       retained one is remembered. Measured on the firmware PM (m1r harness,
 *       ring A(tag1) SYNC(2) C(tag1) D(tag0), 2 SPUs): the PM does not rewrite
 *       the ring entry; the mark lives in queue state (a bitmap at queue+0x180).
 *       The model keeps a per-slot mark beside the ring. Trajectory measured
 *       and reproduced: read 2 while D (dispatched behind the retained C) runs
 *       and ends, then 3 when C dispatches, then 4 on the next pass, which
 *       skips the marked D. The type-3 low-byte-3 path at LS 0x2484, which
 *       would rewrite the entry quadword, is not what was observed.
 *   [assumption] The same mark is used for a SYNC consumed behind a retained
 *       entry; only a JOB behind a retained one was measured. Consequence: a
 *       SYNC records a barrier only for the masked tags that are not already
 *       barred (jq2co-003, step 2), so when it covers a tag whose earlier JOB
 *       is still retained, that tag is held only by the older barrier and a JOB
 *       after the SYNC can run while the formerly retained one is in flight.
 *       The model reproduces the static handler here; unmeasured.
 *   pm_accepts_phase(): the PM side of the phase rule, measured on the firmware
 *       PM (smallest accepted depth 4; 8, 16, 32 identical): the expected phase
 *       flips on wrap (PM 0x2080) and is compared strictly with w1 bit 0
 *       (0x22B0-0x22D0). An entry whose bit 0 differs from the expected phase
 *       ends the pass at 0x22D0 without dispatching. The writer stores only the
 *       phase in w1 bit 0 (OQ-F closed, m1r-017), so flags&1 = 0 and 1 JOBs
 *       both pass a wrap.
 *   Job slots: dispatches rotate through an instance's slots (0, 1, 0, ...),
 *       as measured on one SPU (JobContext2 alternating 0x68D0 / 0x6900).
 *
 * Other deliberate simplifications (not evidence-backed behaviours):
 *   [assumption] The writer refuses a push when `depth` entries are
 *       outstanding (the firmware's AGAIN, jq2fw-029, has an unknown rule).
 *   [assumption] The scan of one pass is bounded by the write index, so a
 *       stale entry of the previous lap is never re-read.
 *   The PM's grab limit (maxGrab) and the secondary busy sets r36/r37
 *   (jq2co-003 Open) are not modelled; in-flight is the exact per-tag count
 *   over all instances.
 *   COPY, JOBLIST, FLUSH and unknown entries have no scheduling semantics in
 *   the evidence: push() rejects COPY/Unknown and a SYNC with an empty mask,
 *   and a pass that meets an unmodelled entry ends with `unsupported_entry`.
 */

#ifndef PS3RECOMP_SPURS_JQ2_SCHED_HPP
#define PS3RECOMP_SPURS_JQ2_SCHED_HPP

#include "jq2/ring.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ps3recomp::spurs::jq2 {

inline constexpr unsigned kTagCount = 16;

/* queue+4 starts at 0x60: bit 5, the writer's phase, is set. [m1r-003] */
inline constexpr bool kInitialPhase = true;

struct SchedConfig {
    std::uint16_t depth = 128;       /* ring entries, queue+0x14; >= 1 */
    unsigned instances = 2;          /* PM instances (SPUs) sharing the queue; >= 1 */
    unsigned slots_per_instance = 2; /* job slots per PM instance; >= 1 */
};

/* The PM's phase rule (measured on the firmware PM): it treats `w1_bit0` as a
 * written entry iff it equals `expected_phase`. */
[[nodiscard]] bool pm_accepts_phase(bool w1_bit0, bool expected_phase) noexcept;

enum class PushError : std::uint8_t {
    none = 0,
    ring_full,          /* `depth` entries outstanding */
    unsupported_entry,  /* COPY or UnknownEntry: no scheduling semantics */
    empty_sync_mask,    /* SYNC(0): no defined PM behaviour, refused */
    encode_failed,      /* the codec rejected the entry; see encode_error */
};

struct PushResult {
    PushError error = PushError::none;
    EncodeError encode_error = EncodeError::none;
    std::uint16_t index = 0; /* ring slot written; valid when ok() */

    [[nodiscard]] bool ok() const noexcept { return error == PushError::none; }
};

/* A job the PM handed to a coroutine slot. `id` is unique and increases in
 * dispatch order. */
struct Dispatch {
    std::uint32_t id = 0;
    unsigned instance = 0;
    unsigned slot = 0;
    std::uint16_t ring_index = 0;
    JobEntry job;
};

enum class PassEnd : std::uint8_t {
    ring_not_written,   /* phase mismatch or the write index reached */
    job_dispatched,     /* one JOB dispatched; the pass is over */
    sync_incomplete,    /* a SYNC was consumed but did not complete */
    no_free_slot,       /* the instance has no free job slot */
    unsupported_entry,  /* an entry the model does not interpret */
    invalid_instance,   /* instance >= configured instances */
};

struct PassResult {
    std::optional<Dispatch> dispatched;
    PassEnd end = PassEnd::ring_not_written;
    std::uint16_t read_before = 0;
    std::uint16_t read_after = 0;
    unsigned syncs_consumed = 0;   /* SYNCs consumed by this pass */
    unsigned entries_retained = 0; /* barred JOBs and re-armed SYNCs skipped */
};

class Scheduler {
public:
    /* nullopt when depth, instances or slots_per_instance is 0. */
    [[nodiscard]] static std::optional<Scheduler> create(const SchedConfig& config);

    /* The PPU writer. JOB/SYNC only. The ring entry's w1 bit 0 is the queue's
     * write phase; the API flag lives in the JOB's port EA (JobEntry::port_ea
     * != 0). */
    PushResult push(const Entry& entry);

    /* One PM intake pass on `instance`; commits the read index at its end. */
    PassResult intake_pass(unsigned instance);

    /* A dispatched job finished. Frees its slot, decrements the in-flight
     * count and releases satisfied barriers. False if `dispatch_id` is not a
     * job in flight (a caller bug, never ignored). */
    [[nodiscard]] bool complete(std::uint32_t dispatch_id);

    /* Queue observers. */
    [[nodiscard]] std::uint16_t depth() const noexcept { return depth_; }
    [[nodiscard]] unsigned instances() const noexcept { return instances_; }
    [[nodiscard]] unsigned slots_per_instance() const noexcept { return slots_per_instance_; }
    [[nodiscard]] std::uint16_t read_index() const noexcept { return read_; }
    [[nodiscard]] std::uint16_t write_index() const noexcept { return write_; }
    [[nodiscard]] bool read_phase() const noexcept { return read_phase_; }
    [[nodiscard]] bool write_phase() const noexcept { return write_phase_; }
    /* Entries pushed and not yet passed by the read index. */
    [[nodiscard]] std::uint16_t outstanding() const noexcept { return outstanding_; }
    [[nodiscard]] unsigned inflight(unsigned tag) const noexcept;
    /* Tags currently barred, one bit per tag. */
    [[nodiscard]] std::uint16_t barred_mask() const noexcept { return barred_; }
    /* The SYNC mask recorded for a barred tag; 0 when the tag is not barred. */
    [[nodiscard]] std::uint16_t barrier_mask(unsigned tag) const noexcept;
    [[nodiscard]] unsigned free_slots(unsigned instance) const noexcept;
    [[nodiscard]] const Dispatch* find_job(std::uint32_t dispatch_id) const noexcept;
    /* Raw ring bytes at a slot, exactly as the writer stored them. */
    [[nodiscard]] const std::array<std::uint8_t, kEntryBytes>& ring_entry(
        std::uint16_t index) const noexcept;
    /* True when the entry at `index` was consumed behind a retained entry and
     * the read index has not yet passed it. */
    [[nodiscard]] bool consumed_out_of_order(std::uint16_t index) const noexcept;
    /* Tags this SYNC newly barred when it was consumed at ring slot `index`
     * (its `fresh` set); 0 if the slot does not hold a consumed SYNC. A
     * consumed SYNC always has a non-empty set. Observer for tests and for
     * comparing against firmware traces. */
    [[nodiscard]] std::uint16_t sync_barred_tags(std::uint16_t index) const noexcept;

private:
    Scheduler(std::uint16_t depth, unsigned instances, unsigned slots_per_instance);

    /* The out-of-order mark; see the header comment. */
    void mark_consumed_out_of_order(std::uint16_t index);

    void release_satisfied_barriers() noexcept;
    void advance_read(unsigned in_order_entries) noexcept;

    std::uint16_t depth_;
    unsigned instances_;
    unsigned slots_per_instance_;

    std::vector<std::array<std::uint8_t, kEntryBytes>> ring_;
    std::vector<std::uint8_t> out_of_order_;
    std::vector<std::uint16_t> sync_fresh_;
    std::uint16_t read_ = 0;
    std::uint16_t write_ = 0;
    bool read_phase_ = kInitialPhase;
    bool write_phase_ = kInitialPhase;
    std::uint16_t outstanding_ = 0;

    std::array<unsigned, kTagCount> inflight_{};
    std::uint16_t barred_ = 0;
    std::array<std::uint16_t, kTagCount> barrier_{};

    std::vector<std::optional<Dispatch>> slots_; /* instances * slots, instance-major */
    std::vector<unsigned> next_slot_;            /* per-instance rotor: first slot tried next */
    std::uint32_t next_dispatch_id_ = 1;
};

}  // namespace ps3recomp::spurs::jq2

#endif  // PS3RECOMP_SPURS_JQ2_SCHED_HPP

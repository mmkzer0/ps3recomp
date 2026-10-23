/*
 * ps3recomp - SPURS JobQueue2 reference scheduler (queue-level model)
 *
 * The contract and its evidence live in sched.hpp. This file is the
 * deterministic implementation: no threads, no clock, no allocation after
 * create().
 */

#include "jq2/sched.hpp"

#include <variant>

namespace ps3recomp::spurs::jq2 {

bool pm_accepts_phase(bool w1_bit0, bool expected_phase) noexcept
{
    /* PM side of OQ-F, measured on the firmware PM: the strict compare of the
     * expected-phase vector against w1 bit 0 (PM 0x22B0-0x22D0). An entry whose
     * bit 0 differs from the expected phase ends the pass (stall). */
    return w1_bit0 == expected_phase;
}

Scheduler::Scheduler(std::uint16_t depth, unsigned instances, unsigned slots_per_instance)
    : depth_(depth),
      instances_(instances),
      slots_per_instance_(slots_per_instance),
      ring_(depth),
      out_of_order_(depth, 0),
      sync_fresh_(depth, 0),
      slots_(std::size_t(instances) * slots_per_instance),
      next_slot_(instances, 0)
{
}

std::optional<Scheduler> Scheduler::create(const SchedConfig& config)
{
    if (config.depth == 0 || config.instances == 0 || config.slots_per_instance == 0)
        return std::nullopt;
    return Scheduler(config.depth, config.instances, config.slots_per_instance);
}

unsigned Scheduler::inflight(unsigned tag) const noexcept
{
    return tag < kTagCount ? inflight_[tag] : 0;
}

std::uint16_t Scheduler::barrier_mask(unsigned tag) const noexcept
{
    return tag < kTagCount ? barrier_[tag] : 0;
}

unsigned Scheduler::free_slots(unsigned instance) const noexcept
{
    if (instance >= instances_) return 0;
    unsigned free = 0;
    for (unsigned s = 0; s < slots_per_instance_; ++s)
        if (!slots_[std::size_t(instance) * slots_per_instance_ + s]) ++free;
    return free;
}

const Dispatch* Scheduler::find_job(std::uint32_t dispatch_id) const noexcept
{
    for (const auto& slot : slots_)
        if (slot && slot->id == dispatch_id) return &*slot;
    return nullptr;
}

const std::array<std::uint8_t, kEntryBytes>& Scheduler::ring_entry(
    std::uint16_t index) const noexcept
{
    return ring_[index % depth_];
}

bool Scheduler::consumed_out_of_order(std::uint16_t index) const noexcept
{
    return out_of_order_[index % depth_] != 0;
}

std::uint16_t Scheduler::sync_barred_tags(std::uint16_t index) const noexcept
{
    return sync_fresh_[index % depth_];
}

PushResult Scheduler::push(const Entry& entry)
{
    PushResult result;
    if (std::holds_alternative<CopyEntry>(entry) ||
        std::holds_alternative<UnknownEntry>(entry)) {
        result.error = PushError::unsupported_entry;
        return result;
    }
    if (const auto* sync = std::get_if<SyncEntry>(&entry); sync != nullptr && sync->mask == 0) {
        result.error = PushError::empty_sync_mask;
        return result;
    }

    const EncodeResult encoded = encode(entry, write_phase_);
    if (!encoded.ok()) {
        result.error = PushError::encode_failed;
        result.encode_error = encoded.error;
        return result;
    }
    if (outstanding_ == depth_) {
        result.error = PushError::ring_full;
        return result;
    }

    result.index = write_;
    ring_[write_] = encoded.bytes;
    sync_fresh_[write_] = 0;
    write_ = static_cast<std::uint16_t>((write_ + 1) % depth_);
    if (write_ == 0) write_phase_ = !write_phase_;
    ++outstanding_;
    return result;
}

void Scheduler::mark_consumed_out_of_order(std::uint16_t index)
{
    /* Measured on the firmware PM: the ring bytes are not rewritten; the mark
     * lives in queue state (bitmap at queue+0x180). The model keeps it beside
     * the ring. The read index passes a marked entry only when a later pass
     * reaches it at the frontier (advance_read does not look ahead). */
    out_of_order_[index] = 1;
}

void Scheduler::advance_read(unsigned in_order_entries) noexcept
{
    const auto step = [this]() {
        out_of_order_[read_] = 0;
        read_ = static_cast<std::uint16_t>((read_ + 1) % depth_);
        if (read_ == 0) read_phase_ = !read_phase_;
        --outstanding_;
    };
    for (unsigned i = 0; i < in_order_entries; ++i) step();
}

void Scheduler::release_satisfied_barriers() noexcept
{
    std::uint16_t release = 0;
    for (unsigned t = 0; t < kTagCount; ++t) {
        if ((barred_ & (1u << t)) == 0) continue;
        bool satisfied = true;
        for (unsigned u = 0; u < kTagCount; ++u)
            if ((barrier_[t] & (1u << u)) != 0 && inflight_[u] != 0) satisfied = false;
        if (satisfied) release = static_cast<std::uint16_t>(release | (1u << t));
    }
    barred_ = static_cast<std::uint16_t>(barred_ & ~release);
    for (unsigned t = 0; t < kTagCount; ++t)
        if ((release & (1u << t)) != 0) barrier_[t] = 0;
}

PassResult Scheduler::intake_pass(unsigned instance)
{
    PassResult result;
    result.read_before = read_;
    result.read_after = read_;
    if (instance >= instances_) {
        result.end = PassEnd::invalid_instance;
        return result;
    }
    if (free_slots(instance) == 0) {
        result.end = PassEnd::no_free_slot;
        return result;
    }

    bool behind_retained = false;
    unsigned in_order = 0; /* entries consumed contiguously from the read index */
    result.end = PassEnd::ring_not_written;

    for (unsigned k = 0; k < outstanding_; ++k) {
        const std::uint32_t pos = std::uint32_t(read_) + k;
        const auto index = static_cast<std::uint16_t>(pos % depth_);
        const bool expected = read_phase_ != (((pos / depth_) & 1u) != 0);

        const Decoded decoded = decode(ring_[index]);
        if (!pm_accepts_phase(decoded.writer_bit, expected)) {
            result.end = PassEnd::ring_not_written;
            break;
        }

        const auto consume = [&]() {
            if (behind_retained)
                mark_consumed_out_of_order(index);
            else
                in_order = k + 1;
        };

        if (out_of_order_[index] != 0) {
            /* Consumed by an earlier pass behind a since-resolved retained
             * entry; the read index only has to move over it. */
            if (!behind_retained) in_order = k + 1;
            continue;
        }

        if (const auto* job = std::get_if<JobEntry>(&decoded.entry)) {
            if ((barred_ & (1u << job->tag)) != 0) {
                behind_retained = true;
                ++result.entries_retained;
                continue;
            }

            /* Slots rotate (0, 1, 0, ...); the first free one from the rotor. */
            unsigned slot = next_slot_[instance];
            while (slots_[std::size_t(instance) * slots_per_instance_ + slot])
                slot = (slot + 1) % slots_per_instance_;
            next_slot_[instance] = (slot + 1) % slots_per_instance_;
            Dispatch dispatch;
            dispatch.id = next_dispatch_id_++;
            dispatch.instance = instance;
            dispatch.slot = slot;
            dispatch.ring_index = index;
            dispatch.job = *job;
            slots_[std::size_t(instance) * slots_per_instance_ + slot] = dispatch;
            ++inflight_[job->tag];
            consume();
            result.dispatched = dispatch;
            result.end = PassEnd::job_dispatched;
            break;
        }

        if (const auto* sync = std::get_if<SyncEntry>(&decoded.entry)) {
            const std::uint16_t mask = sync->mask;
            const auto unbarred = static_cast<std::uint16_t>(~barred_);
            const auto fresh = static_cast<std::uint16_t>(mask & unbarred);
            if (fresh == 0) {
                /* Every masked tag is already barred: the PM's skip path. */
                behind_retained = true;
                ++result.entries_retained;
                continue;
            }

            for (unsigned t = 0; t < kTagCount; ++t)
                if ((fresh & (1u << t)) != 0) barrier_[t] = mask;
            barred_ = static_cast<std::uint16_t>(barred_ | fresh);
            sync_fresh_[index] = fresh;

            bool satisfied = true;
            for (unsigned t = 0; t < kTagCount; ++t)
                if ((mask & (1u << t)) != 0 && (inflight_[t] != 0 || barrier_[t] != mask))
                    satisfied = false;
            if (satisfied) {
                barred_ = static_cast<std::uint16_t>(barred_ & ~mask);
                for (unsigned t = 0; t < kTagCount; ++t)
                    if ((mask & (1u << t)) != 0) barrier_[t] = 0;
            }

            consume();
            ++result.syncs_consumed;
            if (!satisfied || (mask & ~unbarred) != 0) {
                result.end = PassEnd::sync_incomplete;
                break;
            }
            continue;
        }

        result.end = PassEnd::unsupported_entry;
        break;
    }

    advance_read(in_order);
    result.read_after = read_;
    return result;
}

bool Scheduler::complete(std::uint32_t dispatch_id)
{
    for (auto& slot : slots_) {
        if (!slot || slot->id != dispatch_id) continue;
        --inflight_[slot->job.tag];
        slot.reset();
        release_satisfied_barriers();
        return true;
    }
    return false;
}

}  // namespace ps3recomp::spurs::jq2

/*
 * ps3recomp - SPURS JobQueue2 reference scheduler (queue-level model)
 *
 * The contract and its evidence live in sched.hpp. This file is the
 * implementation: no threads and no clock; the ring is whatever RingStore the
 * scheduler was given (private memory by default).
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

namespace {

/* The ring in private memory: the deterministic store of the model. */
class LocalRing final : public RingStore {
public:
    explicit LocalRing(std::uint16_t depth) : depth_(depth), ring_(depth), out_of_order_(depth, 0) {}

    std::uint16_t depth() const noexcept override { return depth_; }

    RingIndices indices() const noexcept override { return {read_, write_, read_phase_, write_phase_}; }

    EntryBytes load(std::uint16_t index) const noexcept override { return ring_[index]; }

    void store(std::uint16_t index, const EntryBytes& bytes) noexcept override { ring_[index] = bytes; }

    bool out_of_order(std::uint16_t index) const noexcept override { return out_of_order_[index] != 0; }

    void set_out_of_order(std::uint16_t index, bool marked) noexcept override
    {
        out_of_order_[index] = marked ? 1 : 0;
    }

    std::optional<Slot> reserve() noexcept override
    {
        if (ring_outstanding(indices(), depth_) == depth_) return std::nullopt;
        const Slot slot{write_, write_phase_};
        write_ = static_cast<std::uint16_t>((write_ + 1) % depth_);
        if (write_ == 0) write_phase_ = !write_phase_;
        return slot;
    }

    void advance_read(unsigned n) noexcept override
    {
        for (unsigned i = 0; i < n; ++i) {
            out_of_order_[read_] = 0;
            read_ = static_cast<std::uint16_t>((read_ + 1) % depth_);
            if (read_ == 0) read_phase_ = !read_phase_;
        }
    }

private:
    std::uint16_t depth_;
    std::vector<EntryBytes> ring_;
    std::vector<std::uint8_t> out_of_order_;
    std::uint16_t read_ = 0;
    std::uint16_t write_ = 0;
    bool read_phase_ = kInitialPhase;
    bool write_phase_ = kInitialPhase;
};

/* The storability rules shared by every writer: the same refusals, in the same
 * order. The phase does not influence acceptance. */
PushResult encode_for_store(const Entry& entry, bool phase, bool allow_copy, EncodeResult& encoded)
{
    PushResult result;
    if ((!allow_copy && std::holds_alternative<CopyEntry>(entry)) ||
        std::holds_alternative<UnknownEntry>(entry)) {
        result.error = PushError::unsupported_entry;
        return result;
    }
    if (const auto* sync = std::get_if<SyncEntry>(&entry); sync != nullptr && sync->mask == 0) {
        result.error = PushError::empty_sync_mask;
        return result;
    }

    encoded = encode(entry, phase);
    if (!encoded.ok()) {
        result.error = PushError::encode_failed;
        result.encode_error = encoded.error;
    }
    return result;
}

}  // namespace

std::unique_ptr<RingStore> make_local_ring(std::uint16_t depth)
{
    if (depth == 0) return nullptr;
    return std::make_unique<LocalRing>(depth);
}

PushResult ring_push(RingStore& ring, const Entry& entry, bool allow_copy, std::optional<bool> phase)
{
    EncodeResult probe;
    PushResult result = encode_for_store(entry, phase.value_or(kInitialPhase), allow_copy, probe);
    if (!result.ok()) return result;

    const std::optional<RingStore::Slot> slot = ring.reserve();
    if (!slot) {
        result.error = PushError::ring_full;
        return result;
    }
    /* The slot's phase is known only now; the codec's acceptance never depends
     * on it, so this encode succeeds like the probe. */
    ring.store(slot->index, encode(entry, phase.value_or(slot->phase)).bytes);
    result.index = slot->index;
    return result;
}

Scheduler::Scheduler(std::unique_ptr<RingStore> ring, unsigned instances, unsigned slots_per_instance,
                     bool copy_entries)
    : depth_(ring->depth()),
      instances_(instances),
      slots_per_instance_(slots_per_instance),
      copy_entries_(copy_entries),
      ring_(std::move(ring)),
      sync_fresh_(depth_, 0),
      slots_(std::size_t(instances) * slots_per_instance),
      next_slot_(instances, 0)
{
}

std::optional<Scheduler> Scheduler::create(const SchedConfig& config)
{
    if (config.depth == 0) return std::nullopt;
    return create(config, make_local_ring(config.depth));
}

std::optional<Scheduler> Scheduler::create(const SchedConfig& config, std::unique_ptr<RingStore> ring)
{
    if (!ring || ring->depth() == 0 || config.instances == 0 || config.slots_per_instance == 0)
        return std::nullopt;
    return Scheduler(std::move(ring), config.instances, config.slots_per_instance, config.copy_entries);
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

EntryBytes Scheduler::ring_entry(std::uint16_t index) const noexcept
{
    return ring_->load(static_cast<std::uint16_t>(index % depth_));
}

bool Scheduler::consumed_out_of_order(std::uint16_t index) const noexcept
{
    return ring_->out_of_order(static_cast<std::uint16_t>(index % depth_));
}

std::uint16_t Scheduler::sync_barred_tags(std::uint16_t index) const noexcept
{
    return sync_fresh_[index % depth_];
}

PushResult Scheduler::push(const Entry& entry)
{
    return push_impl(entry, std::nullopt);
}

PushResult Scheduler::push_with_phase(const Entry& entry, bool phase)
{
    return push_impl(entry, phase);
}

PushResult Scheduler::push_impl(const Entry& entry, std::optional<bool> phase)
{
    PushResult result = ring_push(*ring_, entry, copy_entries_, phase);
    if (result.ok()) sync_fresh_[result.index] = 0;
    return result;
}

bool Scheduler::rewrite_entry(std::uint16_t index, const Entry& entry, bool phase)
{
    if (index >= depth_) return false;
    const RingIndices ix = ring_->indices();
    const unsigned ahead = (index + depth_ - ix.read) % depth_;
    if (ahead >= ring_outstanding(ix, depth_) || ring_->out_of_order(index)) return false;
    EncodeResult encoded;
    if (!encode_for_store(entry, phase, copy_entries_, encoded).ok()) return false;
    ring_->store(index, encoded.bytes);
    sync_fresh_[index] = 0;
    return true;
}

void Scheduler::mark_consumed_out_of_order(std::uint16_t index)
{
    /* Measured on the firmware PM: the ring bytes are not rewritten; the mark
     * lives in queue state (bitmap at queue+0x180), which is the store's. The
     * read index passes a marked entry only when a later pass reaches it at the
     * frontier (the store's advance_read does not look ahead). */
    ring_->set_out_of_order(index, true);
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
    const RingIndices ix = ring_->indices();
    result.read_before = ix.read;
    result.read_after = ix.read;
    if (instance >= instances_) {
        result.end = PassEnd::invalid_instance;
        return result;
    }
    if (free_slots(instance) == 0) {
        result.end = PassEnd::no_free_slot;
        return result;
    }

    /* The write index is read once: an entry a writer reserves later is the
     * next pass's. */
    const unsigned outstanding = ring_outstanding(ix, depth_);
    bool behind_retained = false;
    unsigned in_order = 0; /* entries consumed contiguously from the read index */
    result.end = PassEnd::ring_not_written;

    for (unsigned k = 0; k < outstanding; ++k) {
        const std::uint32_t pos = std::uint32_t(ix.read) + k;
        const auto index = static_cast<std::uint16_t>(pos % depth_);
        const bool expected = ix.read_phase != (((pos / depth_) & 1u) != 0);

        const Decoded decoded = decode(ring_->load(index));
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

        if (ring_->out_of_order(index)) {
            /* Consumed by an earlier pass behind a since-resolved retained
             * entry; the read index only has to move over it. */
            if (!behind_retained) in_order = k + 1;
            continue;
        }

        /* JOB, and COPY when modelled: one handler, one tag rule. */
        std::optional<JobEntry> job;
        bool copy = false;
        if (const auto* j = std::get_if<JobEntry>(&decoded.entry)) {
            job = *j;
        } else if (const auto* c = std::get_if<CopyEntry>(&decoded.entry); c != nullptr && copy_entries_) {
            job = JobEntry{c->port_ea, c->pool_ea, c->tag, c->size};
            copy = true;
        }
        if (job) {
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
            dispatch.copy = copy;
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

    ring_->advance_read(in_order);
    result.read_after = ring_->indices().read;
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

/*
 * ps3recomp - SPURS JobQueue2 CopyPushJobBody descriptor pool (see pool.hpp)
 *
 * Guest memory is the runtime's flat mapping `vm_base + ea` (runtime/ppu/
 * ppu_memory.h; under native-VA mapping vm_base is 0 and the EA is the host
 * address). All multi-byte guest fields are big-endian, as the firmware stores
 * and loads them.
 *
 * Guest updates use the A2(b) reservation model exactly as resv_stcx32/64 do
 * (runtime/memory/reservation.c:247): SPU lock-line lock when the line is
 * SPU-marked, then the 128-byte epoch lock, then the store, then the epoch
 * advance and the lost-reservation event. `LineHold` is that sequence.
 *
 * Evidence ids are from the research repository's KB; firmware EAs name the
 * instruction range that establishes each field.
 */

#include "pool.hpp"
#include "jq2.h"

#include "../../../include/ps3emu/endian.h"
#include "../../../include/ps3emu/stop.h"
#include "../../../runtime/memory/reservation.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" {

/* Provided by the host layer (runtime/host/host_posix.c, a game build, or a
 * test fixture): guest EA -> host address. */
extern std::uint8_t* vm_base;

/* The guest address-space size (runtime/memory/vm.h): non-zero once the host has
 * mapped guest memory, 0 for a host that maps the whole 32-bit space and
 * commits on fault. */
extern std::uint32_t ppu_vm_size;

/* The SPU side of the reservation model (runtime/spu/spu_coherency.c). */
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
void spu_coh_notify_write(std::uint32_t ea);

}  // extern "C"

namespace ps3recomp::spurs::jq2 {
namespace {

constexpr std::uint32_t kLineBytes = 128;

[[nodiscard]] std::uint8_t* guest_bytes(std::uint32_t ea) noexcept
{
    return vm_base ? vm_base + static_cast<std::size_t>(ea)
                   : reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(ea));
}
/* Plain big-endian field loads at create time (the queue is not published yet,
 * so there is no concurrent writer; the class counts are read once). */
[[nodiscard]] std::uint32_t load_be32(std::uint32_t ea) noexcept
{
    std::uint32_t raw;
    std::memcpy(&raw, guest_bytes(ea), sizeof raw);
    return ps3_bswap32(raw);
}

[[nodiscard]] std::uint64_t load_raw64(std::uint32_t ea) noexcept
{
    std::atomic_ref<std::uint64_t> ref(*reinterpret_cast<std::uint64_t*>(guest_bytes(ea)));
    return ref.load(std::memory_order_relaxed);
}

void store_raw64(std::uint32_t ea, std::uint64_t raw) noexcept
{
    std::atomic_ref<std::uint64_t> ref(*reinterpret_cast<std::uint64_t*>(guest_bytes(ea)));
    ref.store(raw, std::memory_order_relaxed);
}

void store_be32(std::uint32_t ea, std::uint32_t value) noexcept
{
    std::atomic_ref<std::uint32_t> ref(*reinterpret_cast<std::uint32_t*>(guest_bytes(ea)));
    ref.store(ps3_bswap32(value), std::memory_order_relaxed);
}

/* A 16-byte class free-queue record, host-order fields. Byte +0x06 is carried
 * through unchanged; +0x07 is the firmware's blocked-waiter count. */
struct Record {
    std::uint32_t head = 0;
    std::uint16_t count = 0;
    std::uint8_t reserved = 0;
    std::uint8_t waiters = 0;
};

[[nodiscard]] Record load_record(std::uint32_t ea) noexcept
{
    const std::uint64_t host = ps3_bswap64(load_raw64(ea));
    return Record{static_cast<std::uint32_t>(host >> 32),
                  static_cast<std::uint16_t>(host >> 16),
                  static_cast<std::uint8_t>(host >> 8),
                  static_cast<std::uint8_t>(host)};
}

void store_record(std::uint32_t ea, const Record& record) noexcept
{
    const std::uint64_t host = (static_cast<std::uint64_t>(record.head) << 32) |
                               (static_cast<std::uint64_t>(record.count) << 16) |
                               (static_cast<std::uint64_t>(record.reserved) << 8) |
                               static_cast<std::uint64_t>(record.waiters);
    store_raw64(ea, ps3_bswap64(host));
}

/* The reservation model's host critical section, in its documented lock order
 * (runtime/memory/reservation.h): spu_lockline_lock -> line epoch lock. The
 * destructor commits; an update sets wrote() after its store so the release
 * advances the epoch. A hold that wrote on a marked line also raises the SPU
 * lost-reservation event, like resv_stcx32/64. A multi-line operation takes the
 * global lock once (SpuLockGuard) and passes spu_lock_held=true to its holds,
 * so the global lock always stays outermost. */
class LineHold {
public:
    explicit LineHold(std::uint32_t ea, bool spu_lock_held = false) noexcept
        : ea_(ea), line_(ea & ~(kLineBytes - 1u))
    {
        marked_ = resv_marked_spu(line_) != 0;
        if (marked_ && !spu_lock_held) {
            spu_lockline_lock();
            owns_spu_ = true;
        }
        resv_line_lock(line_);
    }

    ~LineHold() { commit(); }

    LineHold(const LineHold&) = delete;
    LineHold& operator=(const LineHold&) = delete;

    void wrote() noexcept { wrote_ = true; }

    void commit() noexcept
    {
        if (done_) return;
        done_ = true;
        resv_line_unlock(line_, wrote_ ? 1 : 0);
        if (marked_ && wrote_) spu_coh_notify_write(ea_);
        if (owns_spu_) spu_lockline_unlock();
    }

private:
    std::uint32_t ea_ = 0;
    std::uint32_t line_ = 0;
    bool marked_ = false;
    bool owns_spu_ = false;
    bool wrote_ = false;
    bool done_ = false;
};

/* One global SPU lock-line hold for an operation that nests two line locks. */
class SpuLockGuard {
public:
    SpuLockGuard() noexcept { spu_lockline_lock(); }
    ~SpuLockGuard() { spu_lockline_unlock(); }
    SpuLockGuard(const SpuLockGuard&) = delete;
    SpuLockGuard& operator=(const SpuLockGuard&) = delete;
};

/* A big-endian u32 store to a guest field another thread/SPU can observe:
 * take that line's host critical section and publish. */
void store_be32_locked(std::uint32_t ea, std::uint32_t value) noexcept
{
    LineHold hold(ea);
    store_be32(ea, value);
    hold.wrote();
    hold.commit();
}

/* Store bytes into guest memory as relaxed 64-bit atomics, so a concurrent PPU
 * lwarx/ldarx (runtime/memory/reservation.c reads the word lock-free) never
 * sees a torn word and the access is race-free (runtime/memory/reservation.h,
 * "128-byte line copies"). A word the range covers is stored whole; at an
 * unaligned edge the covered bytes are merged into the word with a
 * compare-and-swap, so the neighbouring bytes of that word - another
 * descriptor's, or a PPU's - are never written back stale. The bytes are
 * copied verbatim: no byte swap, the guest memory is big-endian and so is the
 * source. */
void store_bytes_atomic(std::uint32_t dst_ea, const std::uint8_t* src, std::size_t size) noexcept
{
    while (size != 0) {
        const std::uint32_t skip = dst_ea & 7u;
        const std::size_t chunk = std::min<std::size_t>(size, 8u - skip);
        std::atomic_ref<std::uint64_t> word(
            *reinterpret_cast<std::uint64_t*>(guest_bytes(dst_ea - skip)));
        if (chunk == 8) {
            std::uint64_t raw;
            std::memcpy(&raw, src, sizeof raw);
            word.store(raw, std::memory_order_relaxed);
        } else {
            std::uint64_t current = word.load(std::memory_order_relaxed);
            std::uint64_t merged;
            do {
                merged = current;
                std::memcpy(reinterpret_cast<std::uint8_t*>(&merged) + skip, src, chunk);
            } while (!word.compare_exchange_weak(current, merged, std::memory_order_relaxed,
                                                 std::memory_order_relaxed));
        }
        dst_ea += static_cast<std::uint32_t>(chunk);
        src += chunk;
        size -= chunk;
    }
}

/* Copy into guest memory through the destination lines' critical sections.
 * Used for the create-path control areas and the descriptor snapshot. A whole
 * aligned line goes through resv_copy_in (sixteen 64-bit atomics); the rest
 * through store_bytes_atomic. */
void copy_to_guest(std::uint32_t dst_ea, const std::uint8_t* src, std::size_t size) noexcept
{
    while (size != 0) {
        const std::uint32_t offset = dst_ea & (kLineBytes - 1u);
        const std::size_t chunk = std::min<std::size_t>(size, kLineBytes - offset);
        LineHold hold(dst_ea);
        if (chunk == kLineBytes)
            resv_copy_in(guest_bytes(dst_ea), src);
        else
            store_bytes_atomic(dst_ea, src, chunk);
        hold.wrote();
        hold.commit();
        dst_ea += static_cast<std::uint32_t>(chunk);
        src += chunk;
        size -= chunk;
    }
}

}  // namespace

std::int32_t firmware_code(PoolError error) noexcept
{
    switch (error) {
    case PoolError::none:
        return 0;
    case PoolError::again:
        return kSpursJobErrorAgain;
    case PoolError::address:
    case PoolError::inval:
        return kSpursJobErrorInval;
    case PoolError::align:
        return kSpursJobErrorAlign;
    case PoolError::null_pointer:
        return kSpursJobErrorNullPointer;
    }
    return kSpursJobErrorInval;
}

/* ---- ClassState ---------------------------------------------------------- */

std::uint32_t DescriptorPool::ClassState::index_of(std::uint32_t ea) const noexcept
{
    return (ea - first_block) / block_size;
}

bool DescriptorPool::ClassState::is_free(std::uint32_t index) const noexcept
{
    return ((free_bits[index >> 6] >> (index & 63u)) & 1u) != 0;
}

void DescriptorPool::ClassState::set_free(std::uint32_t index, bool free) noexcept
{
    const std::uint64_t bit = 1ull << (index & 63u);
    if (free)
        free_bits[index >> 6] |= bit;
    else
        free_bits[index >> 6] &= ~bit;
}

/* ---- Construction -------------------------------------------------------- */

DescriptorPool::DescriptorPool(const PoolConfig& config, PoolWaitHook& wait_hook,
                               std::array<ClassState, kPoolClassCount> classes,
                               std::uint32_t block_area_end, std::uint32_t control_area_end)
    : config_(config),
      wait_hook_(&wait_hook),
      classes_(std::move(classes)),
      block_area_end_(block_area_end),
      control_area_end_(control_area_end)
{
}

ClassResult DescriptorPool::class_of_copy_size(std::uint32_t copy_size) noexcept
{
    if (copy_size == 64) return ClassResult{0, PoolError::none};
    if (copy_size < 128 || copy_size > 0x3FF || (copy_size & 0x7F) != 0)
        return ClassResult{0, PoolError::inval};
    return ClassResult{copy_size >> 7, PoolError::none};
}

std::unique_ptr<DescriptorPool> DescriptorPool::create(const PoolConfig& config,
                                                       PoolWaitHook& wait_hook,
                                                       PoolError& error)
{
    error = PoolError::none;

    if (config.queue_ea == 0 || config.pool_ea == 0 || config.pool_attr_ea == 0) {
        error = PoolError::null_pointer;
        return nullptr;
    }
    if (((config.queue_ea | config.pool_ea | config.pool_attr_ea) & 0x7Fu) != 0) {
        error = PoolError::align;
        return nullptr;
    }
    if (config.queue_ea > 0xFFFFFFFFu - 0x700u ||
        config.pool_attr_ea > 0xFFFFFFFFu - 0x20u) {
        /* The header and the eight counts must fit the 32-bit EA space. */
        error = PoolError::inval;
        return nullptr;
    }
    if (!jq2_guest_range_ok(config.pool_attr_ea, 0x20)) {
        ps3_stop_guest_range("DescriptorPool::create", "pool_attr", config.pool_attr_ea, 0x20, jq2_guest_limit());
    }


    std::array<std::uint32_t, kPoolClassCount> counts{};
    for (unsigned cls = 0; cls < kPoolClassCount; ++cls) {
        counts[cls] = load_be32(config.pool_attr_ea + 4u * cls);
        /* The firmware stores the count through sth at record+4 (u16) but
         * links with the full u32; a count above the field is a caller bug,
         * not a layout this component can represent. */
        if (counts[cls] > kPoolMaxCount) {
            error = PoolError::inval;
            return nullptr;
        }
    }

    std::array<ClassState, kPoolClassCount> classes{};
    std::uint64_t cursor = config.pool_ea;

    /* Carve descending class order from one cursor: class 7 first at the pool
     * base, class 0 last (0x02000814, 0x02000870-0x0200091C). */
    for (unsigned cls = kPoolClassCount; cls-- > 0;) {
        ClassState& state = classes[cls];
        state.block_size = kPoolClassSizes[cls];
        state.count = static_cast<std::uint16_t>(counts[cls]);
        if (state.count == 0) continue;

        if (cursor + static_cast<std::uint64_t>(state.count) * state.block_size > 0xFFFFFFFFull) {
            error = PoolError::inval;
            return nullptr;
        }
        state.first_block = static_cast<std::uint32_t>(cursor);
        cursor += static_cast<std::uint64_t>(state.count) * state.block_size;
        state.block_end = static_cast<std::uint32_t>(cursor);
        state.free_count = state.count;
        state.free_bits.assign((state.count + 63u) / 64u, ~0ull);
        const unsigned remainder = state.count % 64u;
        if (remainder != 0)
            state.free_bits.back() = (1ull << remainder) - 1ull;
    }

    if (cursor > 0xFFFFFFFFull) {  /* defensive; each class was checked */
        error = PoolError::inval;
        return nullptr;
    }
    const auto block_area_end = static_cast<std::uint32_t>(cursor);

    /* Control areas are assigned in ascending class order after the block area
     * and zeroed (0x02000920-0x02000988). */
    for (unsigned cls = 0; cls < kPoolClassCount; ++cls) {
        if (classes[cls].count == 0) continue;
        if (cursor > 0xFFFFFFFFull - kPoolControlBytes) {
            error = PoolError::inval;
            return nullptr;
        }
        classes[cls].control_ea = static_cast<std::uint32_t>(cursor);
        cursor += kPoolControlBytes;
    }

    /* A pool that overlaps the queue's own header would make a release take the
     * same line lock twice (all records are one line) and is corrupt guest
     * memory besides. Fail closed before writing anything. */
    if (cursor > config.pool_ea) {
        const std::uint64_t queue_lo = config.queue_ea;
        const std::uint64_t queue_hi = queue_lo + 0x700u;
        if (static_cast<std::uint64_t>(config.pool_ea) < queue_hi && queue_lo < cursor) {
            error = PoolError::inval;
            return nullptr;
        }
    }

    const std::uint64_t pool_size = cursor - config.pool_ea;
    if (!jq2_guest_range_ok(config.pool_ea, pool_size)) {
        ps3_stop_guest_range("DescriptorPool::create", "pool", config.pool_ea, pool_size, jq2_guest_limit());
    }

    auto pool = std::unique_ptr<DescriptorPool>(
        new DescriptorPool(config, wait_hook, std::move(classes), block_area_end,
                           static_cast<std::uint32_t>(cursor)));
    pool->build_guest_layout(counts);
    return pool;
}

void DescriptorPool::build_guest_layout(
    const std::array<std::uint32_t, kPoolClassCount>& counts)
{
    const std::uint32_t queue = config_.queue_ea;

    /* Dependency: [queue_ea, queue_ea + 0x1000) was validated by jq2_queue_create,
     * which covers [queue_ea + 0x600, queue_ea + 0x700). */

    /* The create body memsets the queue before this runs; clearing the pool's
     * own 0x100-byte header makes create independent of that and leaves the
     * descriptor blocks themselves untouched (the firmware does not clear the
     * pool buffer - the runtime dump's 0xCC fill survives). */
    std::array<std::uint8_t, 0x100> header_zero{};
    copy_to_guest(queue + pool_queue_offset::class_first_block, header_zero.data(),
                  header_zero.size());

    for (unsigned cls = kPoolClassCount; cls-- > 0;) {
        const ClassState& state = classes_[cls];
        if (state.count == 0) continue;

        /* Free list: block[i].first_u32 = block[i] + size, last = 0
         * (0x020008EC-0x0200090C). */
        for (std::uint32_t index = 0; index < state.count; ++index) {
            const std::uint32_t block = state.first_block + index * state.block_size;
            const std::uint32_t next = (index + 1u < state.count)
                                           ? block + state.block_size
                                           : 0u;
            store_be32_locked(block, next);
        }

        store_be32_locked(queue + pool_queue_offset::class_first_block + 4u * cls,
                          state.first_block);
        store_be32_locked(queue + pool_queue_offset::class_block_end + 4u * cls,
                          state.block_end);

        {
            LineHold hold(record_ea(cls));
            store_record(record_ea(cls), Record{state.first_block, state.count, 0, 0});
            std::uint8_t* record = guest_bytes(record_ea(cls));
            record[pool_queue_record::flag_0d] = kPoolClassFlag0D;
            record[pool_queue_record::flag_0e] = kPoolClassFlag0E;
            hold.wrote();
        }
    }

    store_be32_locked(queue + pool_queue_offset::pool_base, config_.pool_ea);
    store_be32_locked(queue + pool_queue_offset::block_area_end, block_area_end_);

    unsigned configured = 0;
    for (const ClassState& state : classes_)
        if (state.count != 0) ++configured;
    if (configured != 0) {
        std::array<std::uint8_t, kPoolControlBytes * kPoolClassCount> zero{};
        copy_to_guest(block_area_end_, zero.data(),
                      static_cast<std::size_t>(configured) * kPoolControlBytes);
    }

    for (unsigned cls = 0; cls < kPoolClassCount; ++cls) {
        if (classes_[cls].count == 0) continue;
        store_be32_locked(queue + pool_queue_offset::class_control + 4u * cls,
                          classes_[cls].control_ea);
    }
}

/* ---- Observers ----------------------------------------------------------- */

bool DescriptorPool::class_configured(unsigned cls) const noexcept
{
    return cls < kPoolClassCount && classes_[cls].count != 0;
}

std::uint32_t DescriptorPool::class_first_block(unsigned cls) const noexcept
{
    return cls < kPoolClassCount ? classes_[cls].first_block : 0u;
}

std::uint32_t DescriptorPool::class_block_end(unsigned cls) const noexcept
{
    return cls < kPoolClassCount ? classes_[cls].block_end : 0u;
}

std::uint16_t DescriptorPool::class_count(unsigned cls) const noexcept
{
    return cls < kPoolClassCount ? classes_[cls].count : 0u;
}

std::uint64_t DescriptorPool::class_free_count(unsigned cls) const noexcept
{
    return cls < kPoolClassCount ? classes_[cls].free_count : 0u;
}

std::uint32_t DescriptorPool::class_control_ea(unsigned cls) const noexcept
{
    return cls < kPoolClassCount ? classes_[cls].control_ea : 0u;
}

std::uint32_t DescriptorPool::record_ea(unsigned cls) const noexcept
{
    return config_.queue_ea + pool_queue_offset::class_queue + 16u * cls;
}

DescriptorPool::Located DescriptorPool::locate(std::uint32_t ea) const noexcept
{
    for (unsigned cls = 0; cls < kPoolClassCount; ++cls) {
        const ClassState& state = classes_[cls];
        if (state.count == 0) continue;
        if (ea < state.first_block || ea >= state.block_end) continue;

        const std::uint32_t offset = ea - state.first_block;
        if ((offset % state.block_size) != 0)
            return Located{false, static_cast<int>(cls), 0, "misaligned"};
        return Located{true, static_cast<int>(cls), offset / state.block_size, nullptr};
    }
    return Located{false, -1, 0, "foreign"};
}

/* ---- Acquire / release --------------------------------------------------- */

AcquireResult DescriptorPool::acquire(unsigned cls, bool blocking)
{
    if (cls >= kPoolClassCount) return AcquireResult{0, PoolError::inval};

    ClassState& state = classes_[cls];
    if (state.count == 0) return AcquireResult{0, PoolError::null_pointer};

    const char token = 0; /* waiter identity; unique on this thread's stack */

    for (;;) {
        bool must_wait = false;
        std::uint64_t seen = 0;

        {
            LineHold hold(record_ea(cls));
            Record record = load_record(record_ea(cls));

            /* The host free map and the guest count are written together under
             * this line lock; a mismatch means the guest scribbled the header
             * or a writer bypassed the pool. Never hand out a block on it. */
            if (record.count != state.free_count) {
                hold.commit();
                return AcquireResult{0, PoolError::inval};
            }

            if (record.head != 0) {
                const Located block = locate(record.head);
                if (!block.ok || block.cls != static_cast<int>(cls) ||
                    !state.is_free(block.index)) {
                    hold.commit();
                    return AcquireResult{0, PoolError::inval};
                }
                if (record.count == 0) {
                    hold.commit();
                    return AcquireResult{0, PoolError::inval};
                }

                const std::uint32_t next = load_be32(record.head);
                if (next != 0) {
                    const Located tail = locate(next);
                    if (!tail.ok || tail.cls != static_cast<int>(cls) ||
                        tail.index == block.index || !state.is_free(tail.index)) {
                        hold.commit();
                        return AcquireResult{0, PoolError::inval};
                    }
                }

                const std::uint32_t popped = record.head;
                state.set_free(block.index, false);
                --state.free_count;
                record.head = next;
                --record.count;
                store_record(record_ea(cls), record);
                hold.wrote();
                hold.commit();
                return AcquireResult{popped, PoolError::none};
            }

            if (record.count != 0) { /* head == 0 with blocks on the list */
                hold.commit();
                return AcquireResult{0, PoolError::inval};
            }
            if (!blocking) {
                hold.commit();
                return AcquireResult{0, PoolError::again};
            }
            if (shutting_down_) {
                hold.commit();
                return AcquireResult{0, PoolError::again};
            }
            /* The firmware refuses a new waiter once the byte reaches 0x7F
             * (0x020021A4-0x020021AC, 0x020023DC). */
            if (record.waiters == kPoolWaiterSaturated) {
                hold.commit();
                return AcquireResult{0, PoolError::again};
            }

            ++record.waiters;
            store_record(record_ea(cls), record);
            seen = state.generation;
            {
                /* Registered before the line is released, so a release after the
                 * empty observation must see us and notify. */
                std::lock_guard<std::mutex> lock(waiters_->mutex);
                waiters_->waiters.push_back(
                    Waiter{cls, seen, wait_hook_, static_cast<const void*>(&token)});
            }
            hold.wrote();
            hold.commit();
            must_wait = true;
        }

        if (must_wait) {
            wait_hook_->wait(cls, seen);

            {
                LineHold hold(record_ea(cls));
                Record record = load_record(record_ea(cls));
                if (record.waiters == 0) { /* impossible unless the header moved */
                    hold.commit();
                    return AcquireResult{0, PoolError::inval};
                }
                --record.waiters;
                store_record(record_ea(cls), record);
                {
                    std::lock_guard<std::mutex> lock(waiters_->mutex);
                    auto& list = waiters_->waiters;
                    list.erase(std::remove_if(list.begin(), list.end(),
                                              [&](const Waiter& waiter) {
                                                  return waiter.token == &token &&
                                                         waiter.cls == cls;
                                              }),
                               list.end());
                }
                hold.wrote();
                hold.commit();
            }
        }
    }
}

void DescriptorPool::release(std::uint32_t ea)
{
    const Located block = locate(ea);
    if (!block.ok) {
        stop_release(ea, block.cls, block.reason);
    }

    ClassState& state = classes_[static_cast<unsigned>(block.cls)];
    std::uint64_t generation = 0;

    /* release touches two lines (the class record and the block link), so the
     * global SPU lock is taken once before either line lock. The alternative -
     * letting the inner LineHold take it - would violate the documented order
     * (global -> line) and deadlock against a PPU stcx. on the record line. */
    SpuLockGuard spu_lock;

    {
        LineHold hold(record_ea(static_cast<unsigned>(block.cls)), true);
        Record record = load_record(record_ea(static_cast<unsigned>(block.cls)));

        if (record.count != state.free_count) {
            hold.commit();
            stop_release(ea, block.cls, "count-mismatch");
        }
        if (state.is_free(block.index)) {
            hold.commit();
            stop_release(ea, block.cls, "double-free");
        }

        /* The inverse of the observed pop (jq2c-017): the freed block's first
         * u32 becomes the old head and the block becomes the head. */
        {
            LineHold link(ea, true);
            store_be32(ea, record.head);
            link.wrote();
            link.commit();
        }

        state.set_free(block.index, true);
        ++state.free_count;
        ++state.generation;
        generation = state.generation;

        record.head = ea;
        ++record.count;
        store_record(record_ea(static_cast<unsigned>(block.cls)), record);
        hold.wrote();
        hold.commit();
    }

    std::lock_guard<std::mutex> lock(waiters_->mutex);
    for (const Waiter& waiter : waiters_->waiters) {
        if (waiter.cls == static_cast<unsigned>(block.cls))
            waiter.hook->notify(static_cast<unsigned>(block.cls), generation);
    }
}

/* ---- Shutdown ------------------------------------------------------------ */

void DescriptorPool::shutdown() noexcept
{
    std::array<std::uint64_t, kPoolClassCount> generations{};

    {
        LineHold hold(record_ea(0));
        shutting_down_ = true;
        /* A new generation token is what makes wait(seen) return: the hook is
         * allowed to ignore a notify that carries the value it already saw. */
        for (unsigned cls = 0; cls < kPoolClassCount; ++cls)
            generations[cls] = ++classes_[cls].generation;
        /* No guest bytes changed: the line lock is host bookkeeping only, so
         * the hold commits without advancing the epoch. */
    }

    std::lock_guard<std::mutex> lock(waiters_->mutex);
    for (const Waiter& waiter : waiters_->waiters)
        waiter.hook->notify(waiter.cls, generations[waiter.cls]);
}

/* ---- Snapshot ------------------------------------------------------------ */

PoolError DescriptorPool::snapshot(std::uint32_t src_ea, std::uint32_t size,
                                   std::uint32_t dst_ea) noexcept
{
    const Located block = locate(dst_ea);
    if (!block.ok) return PoolError::inval;

    const ClassState& state = classes_[static_cast<unsigned>(block.cls)];
    {
        LineHold hold(record_ea(static_cast<unsigned>(block.cls)));
        if (state.is_free(block.index)) {
            hold.commit();
            return PoolError::inval;
        }
        if (size > state.block_size) {
            hold.commit();
            return PoolError::inval;
        }
        hold.commit();
    }

    if (size == 0) return PoolError::none;

    /* Fail closed instead of reading or writing outside guest memory, wrapping
     * the address space or copying overlapping regions (the firmware's vector
     * copy is forward). The range is checked against the runtime's mapped guest
     * memory before any host pointer is formed. */
    if (!jq2_guest_range_ok(src_ea, size) || !jq2_guest_range_ok(dst_ea, size))
        return PoolError::address;
    const std::uint64_t src_end = static_cast<std::uint64_t>(src_ea) + size;
    const std::uint64_t dst_end = static_cast<std::uint64_t>(dst_ea) + size;
    if (src_ea < dst_end && dst_ea < src_end)
        return PoolError::inval;

    const std::uint8_t* source = guest_bytes(src_ea);
    copy_to_guest(dst_ea, source, size);
    return PoolError::none;
}

/* ---- Named stop ---------------------------------------------------------- */

void DescriptorPool::stop_release(std::uint32_t ea, int cls, const char* reason) const noexcept
{
    char queue_text[16];
    char ea_text[16];
    char class_text[16];
    std::snprintf(queue_text, sizeof queue_text, "0x%08X", config_.queue_ea);
    std::snprintf(ea_text, sizeof ea_text, "0x%08X", ea);
    if (cls >= 0)
        std::snprintf(class_text, sizeof class_text, "%d", cls);
    else
        std::snprintf(class_text, sizeof class_text, "none");

    const ps3_stop_field fields[] = {
        {"queue", queue_text},
        {"ea", ea_text},
        {"class", class_text},
        {"reason", reason},
    };
    ps3_stop(PS3_STOP_JQ2_POOL_RELEASE,
             "a CopyPush descriptor-pool release does not name a live block of this pool",
             "fix the job-completion bookkeeping; a corrupt pool release is never a no-op",
             fields, sizeof fields / sizeof fields[0], nullptr);
}

}  // namespace ps3recomp::spurs::jq2

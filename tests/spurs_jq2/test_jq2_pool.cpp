/*
 * T0/TSAN: the Jq2 CopyPushJobBody descriptor pool (libs/spurs/jq2/pool.hpp).
 *
 *   - the firmware's guest layout: class table, descending block carve,
 *     ascending control-area assignment, free-list links and per-class record
 *     bytes, checked against the firmware EAs cited in pool.hpp and the
 *     runtime pool dump (jq2c-014/016/017/019),
 *   - per-class acquire/release round trips with the exact guest bytes,
 *   - class selection and the firmware error codes,
 *   - exhaustion (AGAIN non-blocking), the 0x7F waiter saturation and a
 *     blocking acquire woken by a release through the injected hook (threads,
 *     deterministic handshake, no sleep used as synchronisation),
 *   - snapshot copies exactly the requested byte count and the source may be
 *     scribbled afterwards (contract S3),
 *   - a corrupt release (foreign/misaligned/double-free) stops by name in a
 *     subprocess with exit 70,
 *   - the A2(b) reservation protocol: a pool write kills a PPU reservation on
 *     the record line and raises the SPU lost-reservation event,
 *   - concurrent acquire/release from four threads: every slot is owned at
 *     most once (run under TSAN as well).
 *
 * Synthetic guest memory only; no game or firmware bytes.
 */

#include <jq2/pool.hpp>

#include <ps3emu/endian.h>
#include <ps3emu/stop.h>
#include <reservation.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define PS3_POOL_SUBPROCESS 0
#else
#define PS3_POOL_SUBPROCESS 1
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* The host layer's flat guest mapping and the SPU half of the reservation
 * model, both stubbed for the fixture. notify_write() must bump the epoch like
 * runtime/spu/spu_coherency.c does; the counter observes the event. */
static std::atomic<unsigned> g_spu_notifies{0};

extern "C" {
std::uint8_t* vm_base = nullptr;

void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

void spu_coh_notify_write(std::uint32_t ea)
{
    resv_bump(ea);
    g_spu_notifies.fetch_add(1, std::memory_order_relaxed);
}
}

using namespace ps3recomp::spurs::jq2;

namespace {

constexpr std::uint32_t kQueueEa = 0x10000;
constexpr std::uint32_t kPoolEa = 0x20000;
constexpr std::uint32_t kAttrEa = 0x30000;
constexpr std::uint32_t kSrcEa = 0x40000;
constexpr std::size_t kMemSize = 1u << 20;

int g_failures;
[[maybe_unused]] const char* g_self_path = nullptr; /* argv[0] for the stop tests */

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                          \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

/* Synthetic guest memory at EA 0: vm_base + ea. 0xCC fill makes "the firmware
 * does not clear the pool buffer" observable. */
struct Fixture {
    std::vector<std::uint8_t> mem;

    Fixture() : mem(kMemSize, 0xCC) { vm_base = mem.data(); }
    ~Fixture() { vm_base = nullptr; }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    [[nodiscard]] std::uint8_t* bytes(std::uint32_t ea) { return mem.data() + ea; }
    [[nodiscard]] const std::uint8_t* bytes(std::uint32_t ea) const
    {
        return mem.data() + ea;
    }

    void put8(std::uint32_t ea, std::uint8_t value) { bytes(ea)[0] = value; }
    [[nodiscard]] std::uint8_t get8(std::uint32_t ea) const { return bytes(ea)[0]; }

    void put32(std::uint32_t ea, std::uint32_t value)
    {
        const std::uint32_t raw = ps3_bswap32(value);
        std::memcpy(bytes(ea), &raw, sizeof raw);
    }
    [[nodiscard]] std::uint32_t get32(std::uint32_t ea) const
    {
        std::uint32_t raw;
        std::memcpy(&raw, bytes(ea), sizeof raw);
        return ps3_bswap32(raw);
    }
    [[nodiscard]] std::uint64_t get64(std::uint32_t ea) const
    {
        std::uint64_t raw;
        std::memcpy(&raw, bytes(ea), sizeof raw);
        return ps3_bswap64(raw);
    }

    void counts(const std::array<std::uint32_t, kPoolClassCount>& per_class)
    {
        for (unsigned cls = 0; cls < kPoolClassCount; ++cls)
            put32(kAttrEa + 4u * cls, per_class[cls]);
    }
};

struct GuestRecord {
    std::uint32_t head = 0;
    std::uint16_t count = 0;
    std::uint8_t reserved = 0;
    std::uint8_t waiters = 0;
};

GuestRecord record_of(const Fixture& fixture, std::uint32_t queue_ea, unsigned cls)
{
    const std::uint64_t value = fixture.get64(queue_ea + 0x680u + 16u * cls);
    return GuestRecord{static_cast<std::uint32_t>(value >> 32),
                       static_cast<std::uint16_t>(value >> 16),
                       static_cast<std::uint8_t>(value >> 8),
                       static_cast<std::uint8_t>(value)};
}

struct Layout {
    std::array<std::uint32_t, kPoolClassCount> first{};
    std::array<std::uint32_t, kPoolClassCount> end{};
    std::array<std::uint32_t, kPoolClassCount> control{};
    std::uint32_t block_end = 0;
    std::uint32_t control_end = 0;
};

/* Independent re-derivation of the firmware's carve: blocks class 7..0 from one
 * cursor, control areas class 0..7 after the block area. */
Layout layout_for(const std::array<std::uint32_t, kPoolClassCount>& per_class,
                  std::uint32_t pool_ea)
{
    Layout layout;
    std::uint32_t cursor = pool_ea;
    for (int cls = static_cast<int>(kPoolClassCount) - 1; cls >= 0; --cls) {
        const unsigned index = static_cast<unsigned>(cls);
        if (per_class[index] == 0) continue; /* skipped, fields stay 0 */
        layout.first[index] = cursor;
        cursor += per_class[index] * kPoolClassSizes[index];
        layout.end[index] = cursor;
    }
    layout.block_end = cursor;
    for (unsigned cls = 0; cls < kPoolClassCount; ++cls) {
        if (per_class[cls] == 0) continue;
        layout.control[cls] = cursor;
        cursor += kPoolControlBytes;
    }
    layout.control_end = cursor;
    return layout;
}

/* A hook that never blocks: only used where a wait is not expected (calling it
 * would make acquire loop until a release, which the test measures). */
struct CountingHook final : PoolWaitHook {
    std::atomic<unsigned> waits{0};
    std::atomic<unsigned> notifies{0};

    void wait(unsigned, std::uint64_t) override { waits.fetch_add(1); }
    void notify(unsigned, std::uint64_t) override { notifies.fetch_add(1); }
};

/* A real condition-variable wait primitive for the blocking tests. wait() obeys
 * the generation contract; entered() is the deterministic handshake the test
 * uses instead of a sleep. */
class CondHook final : public PoolWaitHook {
public:
    void wait(unsigned cls, std::uint64_t seen) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ++entered_[cls];
        entered_cv_.notify_all();
        cv_[cls].wait(lock, [&] { return latest_[cls] != seen; });
    }

    void notify(unsigned cls, std::uint64_t generation) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation > latest_[cls]) latest_[cls] = generation;
        ++notifies_;
        cv_[cls].notify_all();
    }

    bool wait_until_entered(unsigned cls, unsigned count)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return entered_cv_.wait_for(lock, std::chrono::seconds(10),
                                    [&] { return entered_[cls] >= count; });
    }

    [[nodiscard]] unsigned entered(unsigned cls)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return entered_[cls];
    }

    [[nodiscard]] unsigned notifies()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return notifies_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_[kPoolClassCount];
    std::condition_variable entered_cv_;
    unsigned entered_[kPoolClassCount]{};
    std::uint64_t latest_[kPoolClassCount]{};
    unsigned notifies_ = 0;
};

std::unique_ptr<DescriptorPool> make_pool(const Fixture&, PoolWaitHook& hook,
                                          const std::array<std::uint32_t, kPoolClassCount>& per_class,
                                          PoolError& error,
                                          std::uint32_t queue_ea = kQueueEa,
                                          std::uint32_t pool_ea = kPoolEa)
{
    return DescriptorPool::create({queue_ea, pool_ea, kAttrEa}, hook, error);
}

/* ---- Layout -------------------------------------------------------------- */

void test_create_layout()
{
    Fixture fixture;
    CountingHook hook;
    const std::array<std::uint32_t, kPoolClassCount> counts{1, 0, 3, 0, 0, 0, 0, 2};
    fixture.counts(counts);

    PoolError error = PoolError::null_pointer;
    const auto pool = make_pool(fixture, hook, counts, error);
    CHECK(pool != nullptr);
    CHECK(error == PoolError::none);
    if (!pool) return;

    const Layout layout = layout_for(counts, kPoolEa);

    /* Block areas: class 7 first at the pool base, class 0 last. */
    CHECK(fixture.get32(kQueueEa + 0x660) == kPoolEa);
    CHECK(fixture.get32(kQueueEa + 0x664) == layout.block_end);
    CHECK(layout.block_end == kPoolEa + 2 * 896 + 3 * 256 + 64);
    for (unsigned cls = 0; cls < kPoolClassCount; ++cls) {
        const std::uint32_t first = fixture.get32(kQueueEa + 0x600 + 4 * cls);
        const std::uint32_t end = fixture.get32(kQueueEa + 0x620 + 4 * cls);
        CHECK(first == layout.first[cls]);
        CHECK(end == layout.end[cls]);
        CHECK(pool->class_first_block(cls) == layout.first[cls]);
        CHECK(pool->class_block_end(cls) == layout.end[cls]);
        CHECK(pool->class_count(cls) == counts[cls]);
        CHECK(pool->class_control_ea(cls) == layout.control[cls]);
        if (counts[cls] == 0) {
            CHECK(first == 0 && end == 0);
            CHECK(fixture.get32(kQueueEa + 0x640 + 4 * cls) == 0);
        }
    }

    /* Control areas: ascending class order after the block area. */
    CHECK(fixture.get32(kQueueEa + 0x640 + 0) == layout.control[0]);
    CHECK(fixture.get32(kQueueEa + 0x640 + 4 * 2) == layout.control[2]);
    CHECK(fixture.get32(kQueueEa + 0x640 + 4 * 7) == layout.control[7]);
    CHECK(layout.control[0] == layout.block_end);
    CHECK(layout.control[2] == layout.block_end + kPoolControlBytes);
    CHECK(layout.control[7] == layout.block_end + 2 * kPoolControlBytes);
    CHECK(layout.control_end == layout.block_end + 3 * kPoolControlBytes);
    CHECK(pool->control_area_end() == layout.control_end);
    CHECK(fixture.get8(layout.control[0]) == 0);
    CHECK(fixture.get8(layout.control[2] + 0x100) == 0);
    CHECK(fixture.get8(layout.control_end - 1) == 0);
    CHECK(fixture.get8(layout.control_end) == 0xCC); /* outside the pool */

    /* Free-list records: head/count/flags; +0xD and +0xE are the class-init
     * flags (0x020008AC/0x020008B4). */
    const GuestRecord class7 = record_of(fixture, kQueueEa, 7);
    CHECK(class7.head == layout.first[7]);
    CHECK(class7.count == 2);
    CHECK(class7.waiters == 0);
    CHECK(fixture.get8(kQueueEa + 0x680 + 16 * 7 + 0x0D) == kPoolClassFlag0D);
    CHECK(fixture.get8(kQueueEa + 0x680 + 16 * 7 + 0x0E) == kPoolClassFlag0E);
    const GuestRecord class2 = record_of(fixture, kQueueEa, 2);
    CHECK(class2.head == layout.first[2]);
    CHECK(class2.count == 3);
    const GuestRecord class0 = record_of(fixture, kQueueEa, 0);
    CHECK(class0.head == layout.first[0]);
    CHECK(class0.count == 1);

    /* Unconfigured class: record all zero, no flags. */
    const GuestRecord unconfigured = record_of(fixture, kQueueEa, 1);
    CHECK(unconfigured.head == 0 && unconfigured.count == 0);
    CHECK(fixture.get8(kQueueEa + 0x680 + 16 * 1 + 0x0D) == 0);
    CHECK(fixture.get8(kQueueEa + 0x680 + 16 * 1 + 0x0E) == 0);

    /* Links: block[i].first_u32 = next, last = 0; bytes 4.. stay as they were
     * (the pool buffer is not cleared). */
    CHECK(fixture.get32(layout.first[7]) == layout.first[7] + 896);
    CHECK(fixture.get32(layout.first[7] + 896) == 0);
    CHECK(fixture.get32(layout.first[2]) == layout.first[2] + 256);
    CHECK(fixture.get32(layout.first[2] + 256) == layout.first[2] + 512);
    CHECK(fixture.get32(layout.first[2] + 512) == 0);
    CHECK(fixture.get32(layout.first[0]) == 0);
    CHECK(fixture.get8(layout.first[7] + 4) == 0xCC);

    /* Rejections, with the firmware codes. */
    {
        PoolError reject = PoolError::none;
        CHECK(DescriptorPool::create({0, kPoolEa, kAttrEa}, hook, reject) == nullptr);
        CHECK(reject == PoolError::null_pointer);
        CHECK(firmware_code(reject) == kSpursJobErrorNullPointer);
    }
    {
        PoolError reject = PoolError::none;
        CHECK(DescriptorPool::create({kQueueEa, kPoolEa + 0x40, kAttrEa}, hook, reject) == nullptr);
        CHECK(reject == PoolError::align);
        CHECK(firmware_code(reject) == kSpursJobErrorAlign);
    }
    {
        Fixture misaligned;
        std::array<std::uint32_t, kPoolClassCount> bad{};
        misaligned.counts(bad);
        PoolError reject = PoolError::none;
        CHECK(DescriptorPool::create({kQueueEa + 8, kPoolEa, kAttrEa}, hook, reject) == nullptr);
        CHECK(reject == PoolError::align);
    }
    {
        Fixture oversized;
        auto bad = counts;
        bad[0] = kPoolMaxCount + 1u;
        oversized.counts(bad);
        PoolError reject = PoolError::none;
        CHECK(DescriptorPool::create({kQueueEa, kPoolEa, kAttrEa}, hook, reject) == nullptr);
        CHECK(reject == PoolError::inval);
        CHECK(firmware_code(reject) == kSpursJobErrorInval);
    }
    {
        Fixture overflowing;
        std::array<std::uint32_t, kPoolClassCount> bad{};
        bad[7] = kPoolMaxCount;
        overflowing.counts(bad);
        PoolError reject = PoolError::none;
        CHECK(DescriptorPool::create({kQueueEa, 0xFFFFF000u, kAttrEa}, hook, reject) == nullptr);
        CHECK(reject == PoolError::inval);
    }
    {
        /* A pool covering the queue header would take the record line lock
         * twice in one release. */
        Fixture overlapping;
        std::array<std::uint32_t, kPoolClassCount> one{};
        one[0] = 1;
        overlapping.counts(one);
        PoolError reject = PoolError::none;
        CHECK(DescriptorPool::create({kQueueEa, kQueueEa, kAttrEa}, hook, reject) == nullptr);
        CHECK(reject == PoolError::inval);
    }
    {
        /* The 0x700-byte header must fit below 4 GiB. */
        Fixture header_edge;
        std::array<std::uint32_t, kPoolClassCount> none{};
        header_edge.counts(none);
        PoolError reject = PoolError::none;
        CHECK(DescriptorPool::create({0xFFFFFF80u, kPoolEa, kAttrEa}, hook, reject) == nullptr);
        CHECK(reject == PoolError::inval);
    }
    {
        Fixture empty;
        std::array<std::uint32_t, kPoolClassCount> none{};
        empty.counts(none);
        PoolError reject = PoolError::none;
        auto pool_empty = make_pool(empty, hook, none, reject);
        CHECK(pool_empty != nullptr && reject == PoolError::none);
        CHECK(empty.get32(kQueueEa + 0x660) == kPoolEa);
        CHECK(empty.get32(kQueueEa + 0x664) == kPoolEa);
        CHECK(pool_empty && pool_empty->block_area_end() == kPoolEa);
        auto result = pool_empty->acquire(0, false);
        CHECK(!result.ok() && result.error == PoolError::null_pointer);
    }
}

/* The exact pool image the runtime trace recorded for queue 0x37648380:
 * class 2 x 32 blocks of 256 bytes (jq2c-019, JQ-CLOSURE-STATIC D.4). Relative
 * addresses are what the fixture can map; the layout algorithm is the same. */
void test_runtime_pool_image()
{
    Fixture fixture;
    CountingHook hook;
    std::array<std::uint32_t, kPoolClassCount> counts{};
    counts[2] = 32;
    fixture.counts(counts);

    PoolError error = PoolError::inval;
    const auto pool = make_pool(fixture, hook, counts, error);
    CHECK(pool != nullptr && error == PoolError::none);
    if (!pool) return;

    CHECK(fixture.get32(kQueueEa + 0x608) == kPoolEa);
    CHECK(fixture.get32(kQueueEa + 0x628) == kPoolEa + 0x2000);
    CHECK(fixture.get32(kQueueEa + 0x648) == kPoolEa + 0x2000);
    CHECK(fixture.get32(kQueueEa + 0x660) == kPoolEa);
    CHECK(fixture.get32(kQueueEa + 0x664) == kPoolEa + 0x2000);
    CHECK(pool->block_area_end() == kPoolEa + 0x2000);
    CHECK(pool->control_area_end() == kPoolEa + 0x2400);

    const GuestRecord record = record_of(fixture, kQueueEa, 2);
    CHECK(record.head == kPoolEa && record.count == 32);
    CHECK(fixture.get32(kPoolEa) == kPoolEa + 0x100);
    CHECK(fixture.get32(kPoolEa + 30 * 0x100) == kPoolEa + 31 * 0x100);
    CHECK(fixture.get32(kPoolEa + 31 * 0x100) == 0);

    const AcquireResult first = pool->acquire(2, false);
    CHECK(first.ok() && first.ea == kPoolEa);
    CHECK(fixture.get32(kPoolEa) == kPoolEa + 0x100); /* the observed first pop */
    CHECK(record_of(fixture, kQueueEa, 2).count == 31);
}

/* ---- Class selection and errors ------------------------------------------ */

void test_class_selection()
{
    const std::array<std::uint32_t, kPoolClassCount> sizes = kPoolClassSizes;
    for (unsigned cls = 0; cls < kPoolClassCount; ++cls) {
        const ClassResult result = DescriptorPool::class_of_copy_size(sizes[cls]);
        CHECK(result.ok());
        CHECK(result.cls == cls);
    }

    for (const std::uint32_t bad : {0u, 1u, 63u, 65u, 127u, 129u, 192u, 255u,
                                    320u, 0x3FFu, 0x400u}) {
        const ClassResult result = DescriptorPool::class_of_copy_size(bad);
        CHECK(!result.ok());
        CHECK(result.error == PoolError::inval);
    }

    CHECK(firmware_code(PoolError::none) == 0);
    CHECK(firmware_code(PoolError::again) == kSpursJobErrorAgain);
    CHECK(firmware_code(PoolError::inval) == kSpursJobErrorInval);
    CHECK(firmware_code(PoolError::align) == kSpursJobErrorAlign);
    CHECK(firmware_code(PoolError::null_pointer) == kSpursJobErrorNullPointer);
}

/* ---- Per-class acquire/release ------------------------------------------- */

void test_per_class_round_trip()
{
    Fixture fixture;
    CountingHook hook;
    std::array<std::uint32_t, kPoolClassCount> counts{};
    counts.fill(2);
    fixture.counts(counts);

    PoolError error = PoolError::inval;
    const auto pool = make_pool(fixture, hook, counts, error);
    CHECK(pool != nullptr && error == PoolError::none);
    if (!pool) return;

    const Layout layout = layout_for(counts, kPoolEa);

    for (unsigned cls = 0; cls < kPoolClassCount; ++cls) {
        const std::uint32_t first = layout.first[cls];
        const std::uint32_t second = first + kPoolClassSizes[cls];
        const GuestRecord initial = record_of(fixture, kQueueEa, cls);
        CHECK(initial.head == first && initial.count == 2);
        CHECK(fixture.get32(first) == second);
        CHECK(fixture.get32(second) == 0);

        const AcquireResult a = pool->acquire(cls, false);
        CHECK(a.ok());
        CHECK(a.ea == first);
        GuestRecord record = record_of(fixture, kQueueEa, cls);
        CHECK(record.head == second && record.count == 1);
        CHECK(pool->class_free_count(cls) == 1);

        const AcquireResult b = pool->acquire(cls, false);
        CHECK(b.ok());
        CHECK(b.ea == second);
        record = record_of(fixture, kQueueEa, cls);
        CHECK(record.head == 0 && record.count == 0);
        CHECK(pool->class_free_count(cls) == 0);

        const AcquireResult empty = pool->acquire(cls, false);
        CHECK(!empty.ok());
        CHECK(empty.error == PoolError::again);
        CHECK(firmware_code(empty.error) == kSpursJobErrorAgain);
        CHECK(hook.waits.load() == 0); /* non-blocking never calls the hook */

        /* Release writes the old head into the freed block's first u32. */
        fixture.put32(a.ea, 0xDEADBEEFu);
        pool->release(a.ea);
        CHECK(fixture.get32(a.ea) == 0);
        record = record_of(fixture, kQueueEa, cls);
        CHECK(record.head == a.ea && record.count == 1);

        fixture.put32(b.ea, 0xCAFEBABEu);
        pool->release(b.ea);
        CHECK(fixture.get32(b.ea) == a.ea);
        CHECK(fixture.get32(a.ea) == 0);
        record = record_of(fixture, kQueueEa, cls);
        CHECK(record.head == b.ea && record.count == 2);
        CHECK(record.waiters == 0);
        CHECK(pool->class_free_count(cls) == 2);

        /* LIFO: the last released block is popped first. */
        const AcquireResult again_first = pool->acquire(cls, false);
        const AcquireResult again_second = pool->acquire(cls, false);
        CHECK(again_first.ok() && again_first.ea == b.ea);
        CHECK(again_second.ok() && again_second.ea == a.ea);
        pool->release(again_first.ea);
        pool->release(again_second.ea);
        CHECK(record_of(fixture, kQueueEa, cls).count == 2);

        /* Flags survive every update. */
        CHECK(fixture.get8(kQueueEa + 0x680 + 16 * cls + 0x0D) == kPoolClassFlag0D);
        CHECK(fixture.get8(kQueueEa + 0x680 + 16 * cls + 0x0E) == kPoolClassFlag0E);
    }
}

/* ---- Exhaustion and the waiter saturation -------------------------------- */

void test_exhaustion()
{
    Fixture fixture;
    CountingHook hook;
    std::array<std::uint32_t, kPoolClassCount> counts{};
    counts[0] = 1;
    fixture.counts(counts);

    PoolError error = PoolError::inval;
    const auto pool = make_pool(fixture, hook, counts, error);
    CHECK(pool != nullptr && error == PoolError::none);
    if (!pool) return;

    const AcquireResult held = pool->acquire(0, false);
    CHECK(held.ok());

    const AcquireResult empty = pool->acquire(0, false);
    CHECK(!empty.ok() && empty.error == PoolError::again);

    /* Unconfigured class: the firmware's queue+0x600 check returns A11.
     * Out-of-range class is the host API's own INVAL. */
    const AcquireResult unconfigured = pool->acquire(3, false);
    CHECK(!unconfigured.ok() && unconfigured.error == PoolError::null_pointer);
    const AcquireResult out_of_range = pool->acquire(8, false);
    CHECK(!out_of_range.ok() && out_of_range.error == PoolError::inval);

    /* The firmware refuses a new waiter at byte +0x7 instead of blocking
     * forever (0x020021A4-0x020021AC). */
    fixture.put8(kQueueEa + 0x680 + 7, kPoolWaiterSaturated);
    const AcquireResult saturated = pool->acquire(0, true);
    CHECK(!saturated.ok() && saturated.error == PoolError::again);
    CHECK(hook.waits.load() == 0);
    fixture.put8(kQueueEa + 0x680 + 7, 0);
}

/* ---- Blocking wait and shutdown ------------------------------------------ */

void test_blocking_wakeup()
{
    Fixture fixture;
    CondHook hook;
    std::array<std::uint32_t, kPoolClassCount> counts{};
    counts[0] = 1;
    fixture.counts(counts);

    PoolError error = PoolError::inval;
    const auto pool = make_pool(fixture, hook, counts, error);
    CHECK(pool != nullptr && error == PoolError::none);
    if (!pool) return;

    const AcquireResult held = pool->acquire(0, false);
    CHECK(held.ok());

    std::atomic<std::uint32_t> acquired{0};
    std::atomic<bool> acquired_ok{false};
    std::thread waiter([&] {
        const AcquireResult result = pool->acquire(0, true);
        acquired.store(result.ea, std::memory_order_relaxed);
        acquired_ok.store(result.ok(), std::memory_order_relaxed);
    });

    /* Deterministic handshake: the waiter tells the hook it is inside wait()
     * before the release. No sleep is used as synchronisation. */
    CHECK(hook.wait_until_entered(0, 1));
    CHECK(record_of(fixture, kQueueEa, 0).waiters == 1);

    pool->release(held.ea);
    waiter.join();

    CHECK(acquired_ok.load());
    CHECK(acquired.load() == held.ea);
    CHECK(record_of(fixture, kQueueEa, 0).waiters == 0);
    CHECK(hook.notifies() >= 1);
    CHECK(pool->class_free_count(0) == 0);

    /* Give the block back: the worker that took it is gone, and the teardown
     * case below needs the class to be full again. */
    pool->release(acquired.load());
    CHECK(pool->class_free_count(0) == 1);

    /* Teardown wakes a blocked acquire instead of stranding it. */
    const AcquireResult held_again = pool->acquire(0, false);
    CHECK(held_again.ok());

    std::atomic<int> shutdown_error{-1};
    std::thread blocked([&] {
        const AcquireResult result = pool->acquire(0, true);
        shutdown_error.store(static_cast<int>(result.error), std::memory_order_relaxed);
    });
    CHECK(hook.wait_until_entered(0, 2));
    pool->shutdown();
    blocked.join();
    CHECK(shutdown_error.load() == static_cast<int>(PoolError::again));

    /* After shutdown the recorded waiters were all woken and unregistered. */
    CHECK(record_of(fixture, kQueueEa, 0).waiters == 0);
}

/* ---- Snapshot ------------------------------------------------------------ */

void test_snapshot()
{
    Fixture fixture;
    CountingHook hook;
    std::array<std::uint32_t, kPoolClassCount> counts{};
    counts[2] = 1; /* 256-byte class */
    fixture.counts(counts);

    PoolError error = PoolError::inval;
    const auto pool = make_pool(fixture, hook, counts, error);
    CHECK(pool != nullptr && error == PoolError::none);
    if (!pool) return;

    const AcquireResult slot = pool->acquire(2, false);
    CHECK(slot.ok());

    std::memset(fixture.bytes(slot.ea), 0xAA, 256);
    for (std::uint32_t i = 0; i < 300; ++i)
        fixture.put8(kSrcEa + i, static_cast<std::uint8_t>(i * 7u + 3u));

    /* descriptorSize 200 < copySize/class size 256: exactly 200 bytes move. */
    CHECK(pool->snapshot(kSrcEa, 200, slot.ea) == PoolError::none);
    for (std::uint32_t i = 0; i < 200; ++i)
        CHECK(fixture.get8(slot.ea + i) == static_cast<std::uint8_t>(i * 7u + 3u));
    for (std::uint32_t i = 200; i < 256; ++i)
        CHECK(fixture.get8(slot.ea + i) == 0xAA);

    /* The caller may reuse the source immediately: the snapshot is a copy. */
    std::memset(fixture.bytes(kSrcEa), 0x55, 200);
    for (std::uint32_t i = 0; i < 200; ++i)
        CHECK(fixture.get8(slot.ea + i) == static_cast<std::uint8_t>(i * 7u + 3u));

    CHECK(pool->snapshot(kSrcEa, 257, slot.ea) == PoolError::inval); /* > class */
    CHECK(pool->snapshot(kSrcEa, 16, slot.ea + 0x100) == PoolError::inval); /* foreign */

    pool->release(slot.ea);
    CHECK(pool->snapshot(kSrcEa, 1, slot.ea) == PoolError::inval); /* free block */

    const AcquireResult again = pool->acquire(2, false);
    CHECK(again.ok());
    /* Overflow and overlap are refused. */
    CHECK(pool->snapshot(0xFFFFFFFFu, 2, again.ea) == PoolError::inval);
    CHECK(pool->snapshot(again.ea, 16, again.ea) == PoolError::inval);

    /* Zero bytes is a no-op (the firmware copies descriptorSize bytes). */
    std::memset(fixture.bytes(again.ea), 0x77, 256);
    CHECK(pool->snapshot(kSrcEa, 0, again.ea) == PoolError::none);
    CHECK(fixture.get8(again.ea) == 0x77);
}

/* ---- Reservation protocol ------------------------------------------------ */

void test_reservation_protocol()
{
    Fixture fixture;
    CountingHook hook;
    constexpr std::uint32_t kResvQueue = 0x60000;
    constexpr std::uint32_t kResvPool = 0x70000;
    std::array<std::uint32_t, kPoolClassCount> counts{};
    counts[3] = 1; /* 384 bytes */
    fixture.counts(counts);

    PoolError error = PoolError::inval;
    const auto pool = make_pool(fixture, hook, counts, error, kResvQueue, kResvPool);
    CHECK(pool != nullptr && error == PoolError::none);
    if (!pool) return;

    const AcquireResult slot = pool->acquire(3, false);
    CHECK(slot.ok());

    const std::uint32_t record_ea = kResvQueue + 0x680;
    resv_mark_spu(record_ea);

    ppu_context context{};
    const std::uint64_t before = resv_epoch(record_ea);
    const std::uint64_t observed = resv_ldarx64(&context, record_ea);

    g_spu_notifies.store(0);
    pool->release(slot.ea);

    /* The host store advanced the epoch and raised the SPU event. */
    CHECK(resv_epoch(record_ea) > before);
    CHECK(g_spu_notifies.load() > 0);

    /* The PPU reservation taken before the release cannot commit, even with
     * the exact value it loaded. */
    CHECK(resv_stcx64(&context, record_ea, observed) == 0);

    /* A fresh reservation observes the released state and can commit. */
    const std::uint64_t fresh = resv_ldarx64(&context, record_ea);
    CHECK(resv_stcx64(&context, record_ea, fresh) == 1);
    CHECK(record_of(fixture, kResvQueue, 3).count == 1);
}

/* ---- Concurrent acquire/release ------------------------------------------ */

void test_concurrent()
{
    Fixture fixture;
    CondHook hook;
    std::array<std::uint32_t, kPoolClassCount> counts{};
    counts[1] = 8;
    fixture.counts(counts);

    PoolError error = PoolError::inval;
    const auto pool = make_pool(fixture, hook, counts, error);
    CHECK(pool != nullptr && error == PoolError::none);
    if (!pool) return;

    constexpr unsigned kThreads = 4;
    constexpr unsigned kIterations = 200;
    std::array<std::atomic<int>, 8> owner;
    for (auto& slot : owner) slot.store(-1);

    std::atomic<unsigned> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (unsigned thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([&, thread] {
            for (unsigned iteration = 0; iteration < kIterations; ++iteration) {
                const AcquireResult result = pool->acquire(1, true);
                if (!result.ok()) {
                    failures.fetch_add(1);
                    continue;
                }
                const std::uint32_t index =
                    (result.ea - pool->class_first_block(1)) / kPoolClassSizes[1];
                if (index >= 8) {
                    failures.fetch_add(1);
                    pool->release(result.ea);
                    continue;
                }
                int expected = -1;
                if (!owner[index].compare_exchange_strong(expected,
                                                          static_cast<int>(thread)))
                    failures.fetch_add(1);

                std::uint8_t* block = fixture.bytes(result.ea);
                for (unsigned i = 0; i < 16; ++i)
                    block[i] = static_cast<std::uint8_t>(thread * 32u + iteration + i);
                for (unsigned i = 0; i < 16; ++i) {
                    if (block[i] !=
                        static_cast<std::uint8_t>(thread * 32u + iteration + i))
                        failures.fetch_add(1);
                }

                owner[index].store(-1);
                pool->release(result.ea);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    CHECK(failures.load() == 0);
    CHECK(pool->class_free_count(1) == 8);
    const GuestRecord record = record_of(fixture, kQueueEa, 1);
    CHECK(record.count == 8 && record.waiters == 0);
    CHECK(record.head != 0);
}

/* ---- Named stop (subprocess) --------------------------------------------- */

int run_child(const char* scenario)
{
    Fixture fixture;
    CountingHook hook;
    std::array<std::uint32_t, kPoolClassCount> counts{};
    counts[2] = 1;
    fixture.counts(counts);

    PoolError error = PoolError::inval;
    const auto pool = make_pool(fixture, hook, counts, error);
    if (!pool) {
        std::fprintf(stderr, "child: create failed %d\n", static_cast<int>(error));
        return 2;
    }

    const std::uint32_t block = pool->class_first_block(2);
    if (std::strcmp(scenario, "foreign") == 0) {
        pool->release(kPoolEa + 0x1000); /* outside every class region */
    } else if (std::strcmp(scenario, "misaligned") == 0) {
        pool->release(block + 0x80); /* inside the class, not a block start */
    } else if (std::strcmp(scenario, "double-free") == 0) {
        const AcquireResult slot = pool->acquire(2, false);
        if (!slot.ok()) return 2;
        pool->release(slot.ea);
        pool->release(slot.ea);
    } else {
        std::fprintf(stderr, "child: unknown scenario %s\n", scenario);
        return 2;
    }

    std::fprintf(stderr, "child: release returned; the pool must have stopped\n");
    return 3;
}

struct ChildRun {
    int exit_code = -1;
    bool signalled = false;
    std::string error_text;
};

#if PS3_POOL_SUBPROCESS
ChildRun run_child_process(const char* self, const char* scenario)
{
    ChildRun run;

    /* Build the child's argv before fork: after fork only async-signal-safe
     * calls are made before exec. */
    char child_flag[] = "--child";
    char child_scenario[32];
    std::snprintf(child_scenario, sizeof child_scenario, "%s", scenario);
    char* argv[] = {const_cast<char*>(self), child_flag, child_scenario, nullptr};

    int fds[2];
    if (pipe(fds) != 0) return run;

    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return run;
    }
    if (pid == 0) {
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execv(self, argv);
        _exit(127);
    }

    close(fds[1]);
    char buffer[512];
    for (;;) {
        const ssize_t got = read(fds[0], buffer, sizeof buffer);
        if (got > 0) {
            run.error_text.append(buffer, static_cast<std::size_t>(got));
        } else {
            break;
        }
    }
    close(fds[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) == pid) {
        if (WIFEXITED(status))
            run.exit_code = WEXITSTATUS(status);
        else
            run.signalled = true;
    }
    return run;
}
#endif  /* PS3_POOL_SUBPROCESS */

void test_corrupt_release_stops()
{
#if PS3_POOL_SUBPROCESS
    const char* self = g_self_path;

    {
        const ChildRun run = run_child_process(self, "foreign");
        CHECK(!run.signalled);
        CHECK(run.exit_code == PS3_STOP_EXIT_CODE);
        CHECK(run.error_text.find("PS3STOP v=1 kind=jq2-pool-release") != std::string::npos);
        CHECK(run.error_text.find("queue=0x00010000") != std::string::npos);
        CHECK(run.error_text.find("ea=0x00021000") != std::string::npos);
        CHECK(run.error_text.find("class=none") != std::string::npos);
        CHECK(run.error_text.find("reason=foreign") != std::string::npos);
    }
    {
        const ChildRun run = run_child_process(self, "misaligned");
        CHECK(!run.signalled);
        CHECK(run.exit_code == PS3_STOP_EXIT_CODE);
        CHECK(run.error_text.find("PS3STOP v=1 kind=jq2-pool-release") != std::string::npos);
        CHECK(run.error_text.find("ea=0x00020080") != std::string::npos);
        CHECK(run.error_text.find("class=2") != std::string::npos);
        CHECK(run.error_text.find("reason=misaligned") != std::string::npos);
    }
    {
        const ChildRun run = run_child_process(self, "double-free");
        CHECK(!run.signalled);
        CHECK(run.exit_code == PS3_STOP_EXIT_CODE);
        CHECK(run.error_text.find("PS3STOP v=1 kind=jq2-pool-release") != std::string::npos);
        CHECK(run.error_text.find("ea=0x00020000") != std::string::npos);
        CHECK(run.error_text.find("class=2") != std::string::npos);
        CHECK(run.error_text.find("reason=double-free") != std::string::npos);
    }
#else
    std::printf("jq2 pool: corrupt-release subprocess cases skipped (no fork)\n");
#endif
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0)
        return run_child(argv[2]);

    g_self_path = argv[0];

    test_create_layout();
    test_runtime_pool_image();
    test_class_selection();
    test_per_class_round_trip();
    test_exhaustion();
    test_blocking_wakeup();
    test_snapshot();
    test_concurrent();
    test_corrupt_release_stops();
    test_reservation_protocol(); /* last: resv_mark_spu is permanent */

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("jq2 pool: all checks passed\n");
    return 0;
}

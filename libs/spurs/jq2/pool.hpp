/*
 * ps3recomp - SPURS JobQueue2 CopyPushJobBody descriptor pool
 *
 * `_cellSpursJobQueuePort2CopyPushJobBody(port, src, descriptorSize, copySize,
 * tag, flags)` snapshots the descriptor into a queue-owned pool before it
 * enqueues the command: the firmware's queue session allocator
 * `JobQueueSemaphoreAcquire` (EA 0x0200204C) hands out a block of the class
 * selected by copySize, the copy helper 0x020063E8 copies exactly
 * descriptorSize bytes, and only then is the returned EA enqueued
 * (0x02004C9C-0x02004DD0; jq2fw-024-027; contract S3). This module is that
 * pool: the guest-memory layout the firmware builds in the queue header, plus
 * the acquire/release/snapshot verbs without the HLE surface around them.
 *
 * GUEST LAYOUT (queue EA is 128-byte aligned; firmware EA in brackets)
 *
 *   queue+0x600 + 4*class   u32  first block of the class, 0 if unconfigured
 *                                [class initializer 0x020008A8; create reads it
 *                                 at 0x02002170 and returns NULL_POINTER when 0]
 *   queue+0x620 + 4*class   u32  end of that class's block region
 *                                [0x020008C8]
 *   queue+0x640 + 4*class   u32  EA of the class's zeroed 0x400-byte control
 *                                area (the firmware's per-class waiter table)
 *                                [0x02000960; wait path 0x02002218]
 *   queue+0x660             u32  descriptor-pool base (the create argument)
 *                                [0x02000938]
 *   queue+0x664             u32  end of the descriptor-block area, before the
 *                                control areas [0x0200093C]
 *   queue+0x680 + 16*class  16 B per-class free queue [0x020008A8-0x0200090C,
 *                                pop 0x0200217C-0x020023F0]:
 *     +0x00 u32 head (first free block EA, 0 = empty; high word of the atomically
 *                  loaded 64-bit state)
 *     +0x04 u16 free count
 *     +0x06 u16 reserved; byte +0x06 is preserved, byte +0x07 is the blocked
 *                  waiter count. The firmware increments it before blocking and
 *                  refuses a new waiter at 0x7F (0x020021A4-0x020021AC,
 *                  0x020023DC-0x020023F0).
 *     +0x08 ...  the firmware's per-class wait word. A configured class has
 *                  byte +0x0D = 1 and byte +0x0E = 1 (0x020008AC, 0x020008B4);
 *                  the host wait hook replaces the rest of the wait machinery
 *                  and leaves this word as initialized.
 *
 *   A free block's first u32 is the next free block EA (last = 0); the create
 *   path links `block[i] = block[i] + size` (0x020008EC-0x0200090C).
 *
 * Class sizes are the firmware table at EA 0x02006DE8: 64, 128, 256, 384,
 * 512, 640, 768, 896 bytes (jq2c-014). copySize is accepted when it is exactly
 * 64, or 128..0x3FF with (copySize & 0x7F) == 0; class = copySize >> 7
 * (0x02002114-0x0200214C; jq2c-015), so 64 -> 0 and the eight class sizes map
 * to classes 0..7. A class whose queue+0x600 entry is 0 rejects with
 * NULL_POINTER, other sizes with INVAL (jq2c-015).
 *
 * Create carves the pool from the class counts in poolAttr u32[0..7]
 * (CreateCommon arg 7; jq2c-016). Blocks are carved in DESCENDING class order
 * (class 7 first at pool base, class 0 last) because the firmware's loop walks
 * classes 7..0 with one running cursor (0x02000814, 0x02000870-0x0200091C).
 * Control areas are then assigned in ASCENDING class order at pool-block-end +
 * k*0x400 and zeroed (0x02000920-0x02000988). The runtime pool
 * (0x37649380, class 2 x32) confirms the whole image: q+0x608/0x628/0x648 =
 * 0x37649380/0x3764B380/0x3764B380, q+0x660/0x664 =
 * 0x37649380/0x3764B380, and the first block's link = base+0x100
 * (jq2c-019, JQ-CLOSURE-STATIC D.4).
 *
 * OBSERVERS AND ORDERING
 *
 * The SPU side never reads the pool header: the policy module consumes the
 * command ring and DMA-GETs the descriptor snapshot at the allocated block EA
 * at admission. It therefore observes exactly the bytes snapshot() writes into
 * an allocated block, after the executor publishes the ring entry (a release
 * fence after snapshot, an acquire before the job fetch). Everything else here
 * is PPU/host state.
 *
 * Every guest-memory header update goes through the A2(b) reservation model
 * (`runtime/memory/reservation.h`) with the same protocol as resv_stcx32/64
 * (runtime/memory/reservation.c:247): take the SPU lock-line lock first when
 * the line is SPU-marked, take the 128-byte line epoch lock, update, release
 * and advance the epoch, then raise the SPU lost-reservation event when the
 * line is marked. All eight class records live in the single 128-byte line at
 * queue+0x680, so - as on hardware - a class operation conflicts with every
 * other class via that reservation line. snapshot() takes the destination
 * line locks and advances the epochs of the lines it writes, so no stale PPU
 * or SPU reservation survives a descriptor copy.
 *
 * WAIT HOOK
 *
 * A blocking acquire of an empty class calls PoolWaitHook::wait(class,
 * generation) after releasing every pool lock; release() calls
 * notify(class, generation) with the bumped per-class generation. The
 * generation is read under the class line lock before waiting, so a release
 * that races the wait is never lost: wait() returns when the last notified
 * generation differs from `seen_generation`. The hook may wake spuriously;
 * acquire() re-checks the free list and waits again. The component never spins
 * on its own, so the executor can plug its PPU wait primitive (fiber signal or
 * event-flag wait) without changing the pool.
 */

#ifndef PS3RECOMP_SPURS_JQ2_POOL_HPP
#define PS3RECOMP_SPURS_JQ2_POOL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ps3recomp::spurs::jq2 {

/* Descriptor size classes and their byte sizes, firmware table EA 0x02006DE8
 * (jq2c-014). Class index = copySize >> 7. */
inline constexpr unsigned kPoolClassCount = 8;
inline constexpr std::array<std::uint32_t, kPoolClassCount> kPoolClassSizes{
    64, 128, 256, 384, 512, 640, 768, 896};

/* Queue-header offsets; see the layout block above for the firmware EA behind
 * each one. The per-class arrays are indexed by class (0..7). */
namespace pool_queue_offset {
inline constexpr std::uint32_t class_first_block = 0x600;
inline constexpr std::uint32_t class_block_end = 0x620;
inline constexpr std::uint32_t class_control = 0x640;
inline constexpr std::uint32_t pool_base = 0x660;
inline constexpr std::uint32_t block_area_end = 0x664;
inline constexpr std::uint32_t class_queue = 0x680;
}  // namespace pool_queue_offset

/* Fields of one 16-byte class free-queue record at
 * queue+0x680+16*class. */
namespace pool_queue_record {
inline constexpr std::uint32_t head = 0x00;    /* u32, high word of the u64 */
inline constexpr std::uint32_t count = 0x04;   /* u16 */
inline constexpr std::uint32_t waiter = 0x07;  /* u8; 0x7F refuses a waiter */
inline constexpr std::uint32_t flag_0d = 0x0D;
inline constexpr std::uint32_t flag_0e = 0x0E;
}  // namespace pool_queue_record

inline constexpr std::uint32_t kPoolControlBytes = 0x400;
inline constexpr std::uint8_t kPoolClassFlag0D = 1;
inline constexpr std::uint8_t kPoolClassFlag0E = 1;
inline constexpr std::uint8_t kPoolWaiterSaturated = 0x7F;
inline constexpr std::uint32_t kPoolMaxCount = 0xFFFF;  /* stored as u16 */

/* Firmware error values this component can return (JQ-FW-PPU; jq2c-015,
 * jq2fw-020/022/024/029). */
inline constexpr std::int32_t kSpursJobErrorAgain = 0x80410A01;
inline constexpr std::int32_t kSpursJobErrorInval = 0x80410A02;
inline constexpr std::int32_t kSpursJobErrorAlign = 0x80410A10;
inline constexpr std::int32_t kSpursJobErrorNullPointer = 0x80410A11;

enum class PoolError : std::uint8_t {
    none = 0,
    again,         /* 0x80410A01: class empty, non-blocking acquire */
    inval,         /* 0x80410A02: invalid class/layout/snapshot argument */
    align,         /* 0x80410A10: queue/pool not 128-byte aligned (poolAttr is never checked) */
    null_pointer,  /* 0x80410A11: null create argument or unconfigured class */
    /* A snapshot source or destination range that is not inside the runtime's
     * mapped guest memory (ppu_vm_size, or the 32-bit space when the host does
     * not size it) or wraps it. The firmware validates no address - its vector
     * copy (0x020063E8, jq2fw-025/026) moves exactly the length it is given -
     * so there is no firmware code of its own; invalid arguments return
     * INVAL (0x80410A02, jq2fw-022) and this error maps there. [INFERENCE: on
     * hardware the copy would take a storage fault.] It is a separate value so
     * a caller can tell the range from an argument-shape INVAL. */
    address,
};

/* The guest-visible firmware code for an error, 0 for none. */
[[nodiscard]] std::int32_t firmware_code(PoolError error) noexcept;

/* The three create-path arguments this pool needs. queue_ea and pool_ea are
 * guest EAs and must be 128-byte aligned (CreateCommon alignment rule, jq2fw-019;
 * pool_attr_ea is never alignment-checked). `pool_attr_ea` points at the eight
 * per-class block counts, u32[0..7] (CreateCommon arg 7). */
struct PoolConfig {
    std::uint32_t queue_ea = 0;
    std::uint32_t pool_ea = 0;
    std::uint32_t pool_attr_ea = 0;
};

/* class_of_copy_size() result. */
struct ClassResult {
    unsigned cls = 0;
    PoolError error = PoolError::none;

    [[nodiscard]] bool ok() const noexcept { return error == PoolError::none; }
};

struct AcquireResult {
    std::uint32_t ea = 0; /* valid iff error == none */
    PoolError error = PoolError::none;

    [[nodiscard]] bool ok() const noexcept { return error == PoolError::none; }
};

/* The executor's wait primitive. `wait` is called with no pool lock held after
 * an empty-class observation; it must return when the newest notify generation
 * for `cls` differs from `seen_generation` (a spurious return is allowed:
 * acquire re-checks and waits again). `notify` is called from release() while
 * the pool's waiter registry is locked: it must not call back into the pool
 * and must not block. One hook serves every thread of the queue; a thread-aware
 * implementation can pick its own PPU/fiber wait object inside wait(). The hook
 * must outlive the pool. */
class PoolWaitHook {
public:
    virtual ~PoolWaitHook() = default;
    virtual void wait(unsigned cls, std::uint64_t seen_generation) = 0;
    virtual void notify(unsigned cls, std::uint64_t generation) = 0;
};

/* The queue's descriptor pool. Create it from the create path's arguments
 * before the queue is published; it writes the queue header and builds the
 * free lists. acquire/release/snapshot are thread-safe: every guest update is
 * serialized through the reservation model's 128-byte line locks. */
class DescriptorPool {
public:
    /* nullopt with `error` set when the arguments, the class counts or the
     * computed layout cannot be honoured. Writes the queue header and the
     * per-class free lists on success; the pool buffer's block contents are
     * otherwise untouched (the firmware does not clear them). */
    [[nodiscard]] static std::unique_ptr<DescriptorPool> create(
        const PoolConfig& config, PoolWaitHook& wait_hook, PoolError& error);

    /* The firmware's copySize check and class selection (0x02002114-0x0200214C;
     * jq2c-015): 64 -> class 0, else 128..896 in steps of 128 -> class
     * copySize>>7 (the firmware's accept range is the same rule bounded by
     * 0x3FF); anything else INVAL. */
    [[nodiscard]] static ClassResult class_of_copy_size(std::uint32_t copy_size) noexcept;

    /* Pop a block of `cls`. Non-blocking returns `again` when the class is
     * empty; blocking waits through the hook. An unconfigured class returns
     * null_pointer, exactly as the firmware's queue+0x600 check (0x02002170). */
    [[nodiscard]] AcquireResult acquire(unsigned cls, bool blocking);

    /* Push an allocated block back onto its class's free list and notify the
     * class waiters. A corrupt release (foreign EA, misaligned EA, double
     * free, or guest count/host free map disagreement) raises the named stop
     * `jq2-pool-release`; it is never a silent no-op. */
    void release(std::uint32_t ea);

    /* Copy exactly `size` bytes from guest EA `src_ea` to the allocated block
     * `dst_ea`, before the executor enqueues the command (contract S3). The
     * destination must be a live block and the copy must fit its class; source
     * and destination ranges must lie inside mapped guest memory (`address`
     * otherwise, before any host pointer is formed) and not overlap (`inval`).
     * The destination lines are updated through the reservation model, so a
     * stale PPU/SPU reservation on them is invalidated, and the bytes are
     * stored as 64-bit atomics (a concurrent lwarx/ldarx never reads a torn
     * word). The source may be overwritten by the caller as soon as this
     * returns. */
    [[nodiscard]] PoolError snapshot(std::uint32_t src_ea, std::uint32_t size,
                                     std::uint32_t dst_ea) noexcept;

    /* Teardown: wake every waiter and make a blocking acquire return `again`
     * instead of waiting. The executor calls this before joining its workers,
     * then destroys the pool. A pool that still has live blocks is not
     * validated here: the executor owns the queue lifecycle. */
    void shutdown() noexcept;

    /* Observers for the executor and tests. */
    [[nodiscard]] std::uint32_t queue_ea() const noexcept { return config_.queue_ea; }
    [[nodiscard]] std::uint32_t pool_ea() const noexcept { return config_.pool_ea; }
    [[nodiscard]] std::uint32_t block_area_end() const noexcept { return block_area_end_; }
    [[nodiscard]] std::uint32_t control_area_end() const noexcept { return control_area_end_; }
    [[nodiscard]] bool class_configured(unsigned cls) const noexcept;
    [[nodiscard]] std::uint32_t class_first_block(unsigned cls) const noexcept;
    [[nodiscard]] std::uint32_t class_block_end(unsigned cls) const noexcept;
    [[nodiscard]] std::uint16_t class_count(unsigned cls) const noexcept;
    [[nodiscard]] std::uint64_t class_free_count(unsigned cls) const noexcept;
    [[nodiscard]] std::uint32_t class_control_ea(unsigned cls) const noexcept;

private:
    struct ClassState {
        std::uint32_t first_block = 0;
        std::uint32_t block_end = 0;
        std::uint32_t control_ea = 0;
        std::uint32_t block_size = 0;
        std::uint16_t count = 0;
        std::uint64_t free_count = 0;
        std::uint64_t generation = 0;
        std::vector<std::uint64_t> free_bits; /* 1 = on the free list */

        [[nodiscard]] std::uint32_t index_of(std::uint32_t ea) const noexcept;
        [[nodiscard]] bool is_free(std::uint32_t index) const noexcept;
        void set_free(std::uint32_t index, bool free) noexcept;
    };

    struct Waiter {
        unsigned cls = 0;
        std::uint64_t seen = 0;
        PoolWaitHook* hook = nullptr;
        const void* token = nullptr;
    };

    DescriptorPool(const PoolConfig& config, PoolWaitHook& wait_hook,
                   std::array<ClassState, kPoolClassCount> classes,
                   std::uint32_t block_area_end, std::uint32_t control_area_end);

    /* Locate a block start. `reason` is null on success; `cls` is -1 when no
     * class region owns the EA. */
    struct Located {
        bool ok = false;
        int cls = -1;
        std::uint32_t index = 0;
        const char* reason = nullptr;
    };
    [[nodiscard]] Located locate(std::uint32_t ea) const noexcept;

    [[nodiscard]] std::uint32_t record_ea(unsigned cls) const noexcept;
    void build_guest_layout(const std::array<std::uint32_t, kPoolClassCount>& counts);

    [[noreturn]] void stop_release(std::uint32_t ea, int cls, const char* reason) const noexcept;

    PoolConfig config_;
    PoolWaitHook* wait_hook_;
    std::array<ClassState, kPoolClassCount> classes_{};
    std::uint32_t block_area_end_ = 0;
    std::uint32_t control_area_end_ = 0;
    /* Guarded by the class-record line lock (all records share one line). */
    bool shutting_down_ = false;

    /* Waiters are registered under the class line lock, so a release can never
     * miss one. notify() is called with this mutex held (see the hook
     * contract) and never calls back into the pool. */
    struct WaitRegistry {
        std::mutex mutex;
        std::vector<Waiter> waiters;
    };
    std::unique_ptr<WaitRegistry> waiters_ = std::make_unique<WaitRegistry>();
};

}  // namespace ps3recomp::spurs::jq2

#endif  // PS3RECOMP_SPURS_JQ2_POOL_HPP

/*
 * ps3recomp - SPURS JobQueue2 HLE executor. The design, the guest layout and the
 * evidence are in executor.hpp; the entry surface is jq2.h.
 *
 * Lock order: Queue::mu, then any reservation-model line lock (resv_host_rmw).
 * The bodies of a job, the pool and the LV2 event flags run outside Queue::mu.
 */

#include "jq2/executor.hpp"

#include "jq2/jq2.h"
#include "jq2/pool.hpp"
#include "jq2/port.hpp"
#include "jq2/sched.hpp"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "memory/reservation.h"
#include "ps3emu/endian.h"
#include "ps3emu/stop.h"
#include "spu/spu_context.h"
#include "spu/spu_interp.h"
#include "spu/spu_workload.h"
#include "syscalls/sys_event.h"

extern "C" {
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
void spurs_job_set_spu_num(uint32_t n);
}

namespace ps3recomp::spurs::jq2 {

namespace {

/* ---- the queue block --------------------------------------------------- */

constexpr std::uint32_t kQRing = 0x10;
constexpr std::uint32_t kQDepth = 0x14;
constexpr std::uint32_t kQInit16 = 0x16;
/* queue+0x6C: CreateCommon stores the SPURS EA | the workload id (m1r-004); the
 * join sets bit 0x20 (kJoined). The workload id is the low 5 bits. The HLE has
 * no SPURS workload registry, so it gives the queue's workload the id the
 * firmware gives the first one, 0 (kWorkloadId), and the job staging reads it
 * back from the word - the kernel view's wklCurrentId/UniqueId (0x1D8/0x1DC). */
constexpr std::uint32_t kQWorkloadFlags = 0x6C;
constexpr std::uint32_t kWorkloadIdMask = 0x1F;
constexpr std::uint32_t kWorkloadId = 0;
constexpr std::uint32_t kQPortCount = 0x70;
constexpr std::uint32_t kQEventFlag = 0x80; /* two u32 ids */
constexpr std::uint32_t kQWaiterBitmap = 0x88; /* four BE32 words, MSB first */
constexpr std::uint32_t kQPortBitmap = 0x580;
constexpr std::uint32_t kQExit = 0xF04;
constexpr std::uint32_t kQSpurs = 0xF10;
constexpr std::uint32_t kQMaxDescriptor = 0xF14;
constexpr std::uint32_t kQSdk = 0xFF8;
constexpr std::uint32_t kQRevision = 0xFFC;
constexpr std::uint32_t kQueueBytes = 0x1000;

constexpr std::uint32_t kFlagsInitial = 0x60;
constexpr std::uint32_t kFlagWriterPhase = 0x20;
constexpr std::uint32_t kFlagReaderPhase = 0x40;
constexpr std::uint32_t kJoined = 0x20;

constexpr std::uint32_t kMaxPorts = 0x400;
/* The PPU allocator scans 128 bits, but the PM wake rejects ids above 63.
 * Zero collides with the PPU's "not allocated" sentinel and leaks on first
 * use; only ids 1..63 can be registered. Preserve attempted higher bits before
 * stopping rather than reproducing the firmware's uninterruptible wait. */
constexpr unsigned kWaiterIds = 64;

/* The port block's own words (the line's accounting words are port.hpp's). */
constexpr std::uint32_t kPLink = 0x08;
constexpr std::uint32_t kPQueue = 0x18;
constexpr std::uint32_t kPSlot = 0x1C;
constexpr std::uint32_t kPInit = 0x20;

/* ---- guest memory ------------------------------------------------------ */

inline std::uint8_t* host(std::uint32_t ea) { return vm_base + ea; }

inline std::atomic_ref<std::uint32_t> word_at(std::uint32_t ea)
{
    return std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(host(ea)));
}
inline std::uint32_t ld32(std::uint32_t ea) { return ps3_bswap32(word_at(ea).load(std::memory_order_acquire)); }
inline void st32(std::uint32_t ea, std::uint32_t v) { word_at(ea).store(ps3_bswap32(v), std::memory_order_release); }

inline std::uint16_t ld16(std::uint32_t ea)
{
    std::uint16_t v;
    std::memcpy(&v, host(ea), sizeof v);
    return ps3_bswap16(v);
}
inline void st16(std::uint32_t ea, std::uint16_t v)
{
    v = ps3_bswap16(v);
    std::memcpy(host(ea), &v, sizeof v);
}

constexpr bool aligned(std::uint32_t ea, std::uint32_t to) { return (ea & (to - 1)) == 0; }

/* ---- named stops ------------------------------------------------------- */

struct Kv {
    const char* key;
    char value[48];
};

Kv hex(const char* key, std::uint64_t v)
{
    Kv kv{key, {}};
    std::snprintf(kv.value, sizeof kv.value, "0x%llX", static_cast<unsigned long long>(v));
    return kv;
}
Kv dec(const char* key, std::uint64_t v)
{
    Kv kv{key, {}};
    std::snprintf(kv.value, sizeof kv.value, "%llu", static_cast<unsigned long long>(v));
    return kv;
}
Kv text(const char* key, const char* v)
{
    Kv kv{key, {}};
    std::snprintf(kv.value, sizeof kv.value, "%s", v);
    return kv;
}

[[noreturn]] void stop_with(ps3_stop_kind kind, const char* what, const char* remedy, std::initializer_list<Kv> kvs,
                            const ps3_stop_site* site = nullptr)
{
    std::vector<ps3_stop_field> fields;
    fields.reserve(kvs.size());
    for (const Kv& kv : kvs) fields.push_back({kv.key, kv.value});
    ps3_stop(kind, what, remedy, fields.data(), fields.size(), site);
}

/* ---- LV2 event flags --------------------------------------------------- */

/* The queue's flags are the runtime's LV2 event flags, driven through the
 * syscall handlers a guest's own `sc` would reach. */
std::int32_t evf_create(std::uint32_t id_out_ea)
{
    ppu_context ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = id_out_ea;
    return static_cast<std::int32_t>(sys_event_flag_create(&ctx));
}
std::int32_t evf_destroy(std::uint32_t id)
{
    ppu_context ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = id;
    return static_cast<std::int32_t>(sys_event_flag_destroy(&ctx));
}
std::int32_t evf_set(std::uint32_t id, std::uint64_t bits)
{
    ppu_context ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = id;
    ctx.gpr[4] = bits;
    return static_cast<std::int32_t>(sys_event_flag_set(&ctx));
}
/* The firmware's wait: AND | CLEAR (0x11), no result word, no timeout. */
std::int32_t evf_wait(std::uint32_t id, std::uint64_t bits)
{
    ppu_context ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = id;
    ctx.gpr[4] = bits;
    ctx.gpr[5] = SYS_EVENT_FLAG_WAIT_AND | SYS_EVENT_FLAG_WAIT_CLEAR;
    return static_cast<std::int32_t>(sys_event_flag_wait(&ctx));
}

/* ---- the ring over the guest queue ------------------------------------- */

/* The index word at queue+0: read u16, write u16, flags u32, one big-endian
 * u64 (m1r-003). Every change goes through resv_host_rmw like the PPU writer's
 * ldarx/stdcx., so a reservation on the line dies; reading it is a plain atomic
 * load. The entries are 8-byte atomic stores/loads at ring + 8*index. */
class GuestRing final : public RingStore {
public:
    GuestRing(std::uint32_t queue_ea, std::uint32_t ring_ea, std::uint16_t depth)
        : queue_(queue_ea), ring_(ring_ea), depth_(depth), marks_(depth, 0)
    {
    }

    std::uint16_t depth() const noexcept override { return depth_; }

    RingIndices indices() const noexcept override
    {
        return unpack(word(queue_).load(std::memory_order_acquire)).ix;
    }

    EntryBytes load(std::uint16_t index) const noexcept override
    {
        const std::uint64_t raw = word(ring_ + 8u * index).load(std::memory_order_acquire);
        EntryBytes bytes;
        std::memcpy(bytes.data(), &raw, sizeof raw);
        return bytes;
    }

    void store(std::uint16_t index, const EntryBytes& bytes) noexcept override
    {
        std::uint64_t raw;
        std::memcpy(&raw, bytes.data(), sizeof raw);
        word(ring_ + 8u * index).store(raw, std::memory_order_release);
    }

    /* The marks are the executor's: the PM keeps its own out-of-order state in
     * a 128-byte record line at queue+0x180 (per grab slot, rewritten by the PM
     * with GETLLAR/PUTLLUC), which nothing on the PPU side reads and which this
     * executor leaves alone. Only the scheduler's reader touches them. */
    bool out_of_order(std::uint16_t index) const noexcept override { return marks_[index] != 0; }

    void set_out_of_order(std::uint16_t index, bool marked) noexcept override { marks_[index] = marked ? 1 : 0; }

    std::optional<Slot> reserve() noexcept override
    {
        Rmw rmw{this, 0, {}};
        rmw.kind = Kind::reserve;
        resv_host_rmw(queue_, &GuestRing::rmw_thunk, &rmw);
        return rmw.slot;
    }

    void advance_read(unsigned n) noexcept override
    {
        if (n == 0) return;
        const std::uint16_t read = indices().read;
        for (unsigned i = 0; i < n; ++i) set_out_of_order(static_cast<std::uint16_t>((read + i) % depth_), false);
        Rmw rmw{this, n, {}};
        rmw.kind = Kind::advance;
        resv_host_rmw(queue_, &GuestRing::rmw_thunk, &rmw);
    }

private:
    enum class Kind { reserve, advance };
    struct Rmw {
        GuestRing* self;
        unsigned n;
        std::optional<Slot> slot;
        Kind kind = Kind::reserve;
    };
    struct Unpacked {
        RingIndices ix;
        std::uint32_t flags;
    };

    static std::atomic_ref<std::uint64_t> word(std::uint32_t ea)
    {
        return std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(host(ea)));
    }

    static Unpacked unpack(std::uint64_t raw) noexcept
    {
        const std::uint64_t v = ps3_bswap64(raw);
        const auto flags = static_cast<std::uint32_t>(v);
        return {{static_cast<std::uint16_t>(v >> 48), static_cast<std::uint16_t>(v >> 32),
                 (flags & kFlagReaderPhase) != 0, (flags & kFlagWriterPhase) != 0},
                flags};
    }
    static std::uint64_t pack(const RingIndices& ix, std::uint32_t flags) noexcept
    {
        flags = (flags & ~(kFlagWriterPhase | kFlagReaderPhase)) | (ix.write_phase ? kFlagWriterPhase : 0) |
                (ix.read_phase ? kFlagReaderPhase : 0);
        return ps3_bswap64((std::uint64_t{ix.read} << 48) | (std::uint64_t{ix.write} << 32) | flags);
    }

    static int rmw_thunk(std::uint8_t* line, void* user)
    {
        auto& rmw = *static_cast<Rmw*>(user);
        const std::uint16_t depth = rmw.self->depth_;
        std::atomic_ref<std::uint64_t> w(*reinterpret_cast<std::uint64_t*>(line));
        std::uint64_t raw = w.load(std::memory_order_acquire);
        for (;;) {
            Unpacked u = unpack(raw);
            if (rmw.kind == Kind::reserve) {
                if (ring_outstanding(u.ix, depth) == depth) return 0;
                rmw.slot = Slot{u.ix.write, u.ix.write_phase};
                u.ix.write = static_cast<std::uint16_t>((u.ix.write + 1) % depth);
                if (u.ix.write == 0) u.ix.write_phase = !u.ix.write_phase;
            } else {
                for (unsigned i = 0; i < rmw.n; ++i) {
                    u.ix.read = static_cast<std::uint16_t>((u.ix.read + 1) % depth);
                    if (u.ix.read == 0) u.ix.read_phase = !u.ix.read_phase;
                }
            }
            /* The model's line lock excludes every reported writer; the
             * compare exchange is the stdcx. against a store it does not see. */
            if (w.compare_exchange_strong(raw, pack(u.ix, u.flags), std::memory_order_acq_rel,
                                          std::memory_order_acquire))
                return 1;
        }
    }

    std::uint32_t queue_;
    std::uint32_t ring_;
    std::uint16_t depth_;
    std::vector<std::uint8_t> marks_;
};

/* ---- the pool's wait primitive ------------------------------------------ */

/* One generation per class; wait() returns once the class's generation differs
 * from the one the pool saw. notify() is called with the pool's registry lock
 * held: it takes only this mutex and never calls back. */
class PoolWake final : public PoolWaitHook {
public:
    void wait(unsigned cls, std::uint64_t seen) override
    {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return gen_[cls] != seen; });
    }
    void notify(unsigned cls, std::uint64_t generation) override
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            gen_[cls] = generation;
        }
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::array<std::uint64_t, kPoolClassCount> gen_{};
};

/* ---- hooks ------------------------------------------------------------- */

std::mutex g_hooks_mu;
ExecutorHooks g_hooks;

/* ---- the queue ---------------------------------------------------------- */

struct Counters {
    std::atomic<std::uint64_t> pushed{0}, dispatched{0}, executed{0}, credited{0}, uncredited{0};
};

class Queue {
public:
    Queue(std::uint32_t ea, std::uint32_t ring_ea, std::uint16_t depth, std::uint32_t max_descriptor,
          unsigned workers, const ExecutorHooks& hooks)
        : ea_(ea), ring_ea_(ring_ea), depth_(depth), max_descriptor_(max_descriptor), workers_(workers),
          hooks_(hooks)
    {
        auto ring = std::make_unique<GuestRing>(ea, ring_ea, depth);
        ring_ = ring.get();
        SchedConfig config;
        config.instances = workers;
        config.slots_per_instance = 1;
        config.copy_entries = true;
        sched_ = Scheduler::create(config, std::move(ring));
    }

    bool valid() const { return sched_.has_value(); }

    std::uint32_t ea() const { return ea_; }
    std::uint32_t max_descriptor() const { return max_descriptor_; }
    GuestRing& ring() { return *ring_; }
    Counters& counters() { return counters_; }
    DescriptorPool* pool() { return pool_.get(); }
    bool shut() const { return shut_.load(std::memory_order_acquire); }
    std::uint32_t flag_id(unsigned n) const { return flag_ids_[n]; }

    /* Attach what create built: the event flag ids and the pool. */
    void attach(std::uint32_t flag0, std::uint32_t flag1) { flag_ids_ = {flag0, flag1}; }
    PoolWaitHook& pool_hook() { return pool_wake_; }
    void set_pool(std::unique_ptr<DescriptorPool> pool) { pool_ = std::move(pool); }

    /* ---- workers ---- */

    bool start()
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        /* The loader keeps a whole spu_context (256 KB of local store) on the
         * worker's stack. */
        pthread_attr_setstacksize(&attr, 8u << 20);
        for (unsigned i = 0; i < workers_; ++i) {
            auto* arg = new WorkerArg{this, i};
            pthread_t t;
            if (pthread_create(&t, &attr, &Queue::worker_thunk, arg) != 0) {
                delete arg;
                pthread_attr_destroy(&attr);
                return false;
            }
            threads_.push_back(t);
        }
        pthread_attr_destroy(&attr);
        return true;
    }

    /* Workers re-examine the ring: a push or a completion happened. */
    void kick()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            ++gen_;
        }
        cv_.notify_all();
    }

    /* Refuse further pushes; the workers exit once the ring is drained. */
    void begin_shutdown()
    {
        shut_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopping_ = true;
            ++gen_;
        }
        cv_.notify_all();
        if (pool_) pool_->shutdown();
    }

    /* A push has passed its checks; see ExecutorHooks::on_push_checked. */
    void push_checked(std::uint32_t port_ea) const
    {
        if (hooks_.on_push_checked != nullptr) hooks_.on_push_checked(hooks_.user, port_ea);
    }

    /* The ring write of every push, ordered against Shutdown by mu_: Shutdown
     * sets stopping_ under mu_, and a worker leaves only after seeing stopping_
     * with an empty ring under mu_. An entry written here under mu_ with
     * stopping_ clear is therefore seen by a worker; nullopt means Shutdown won
     * and the entry was not written (it would never run or be credited). The
     * lock order is mu_, then the ring's reservation-model line lock. */
    std::optional<PushResult> push_unless_shut(const Entry& entry)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_) return std::nullopt;
        return ring_push(*ring_, entry, true);
    }

    void join_workers()
    {
        for (pthread_t t : threads_) pthread_join(t, nullptr);
        threads_.clear();
    }

    void set_observer(const jq2_job_observer* obs)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (obs != nullptr)
            observer_ = *obs;
        else
            observer_.reset();
    }

    void set_paused(bool paused)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            paused_ = paused;
            ++gen_;
        }
        cv_.notify_all();
    }

    QueueView view()
    {
        std::lock_guard<std::mutex> lk(mu_);
        QueueView v;
        v.indices = ring_->indices();
        v.outstanding = sched_->outstanding();
        v.running = running_;
        v.barred = sched_->barred_mask();
        for (unsigned t = 0; t < kTagCount; ++t) v.inflight[t] = sched_->inflight(t);
        for (std::uint16_t i = 0; i < depth_; ++i)
            if (sched_->consumed_out_of_order(i)) v.out_of_order.push_back(i);
        v.stats = snapshot_stats();
        return v;
    }

    jq2_stats snapshot_stats() const
    {
        jq2_stats s{};
        s.pushed = counters_.pushed.load();
        s.dispatched = counters_.dispatched.load();
        s.executed = counters_.executed.load();
        s.credited = counters_.credited.load();
        s.uncredited = counters_.uncredited.load();
        return s;
    }

    /* ---- Port2Sync's wait ---- */

    unsigned acquire_waiter()
    {
        for (;;) {
            unsigned id = 128;
            resv_host_rmw(ea_ + kQWaiterBitmap, [](std::uint8_t* line, void* user) {
                auto& allocated = *static_cast<unsigned*>(user);
                for (unsigned w = 0; w < 4; ++w) {
                    auto word = std::atomic_ref<std::uint32_t>(
                        *reinterpret_cast<std::uint32_t*>(line + 8 + 4 * w));
                    const std::uint32_t bits = ps3_bswap32(word.load(std::memory_order_relaxed));
                    if (bits == UINT32_MAX) continue;
                    const unsigned bit = static_cast<unsigned>(__builtin_clz(~bits));
                    word.store(ps3_bswap32(bits | (0x80000000u >> bit)), std::memory_order_release);
                    allocated = 32 * w + bit;
                    return 1;
                }
                return 0;
            }, &id);
            if (id == 0) continue; /* firmware retries without freeing the sentinel */
            if (id < kWaiterIds) return id;
            std::array<std::uint32_t, 4> words{};
            resv_host_rmw(ea_ + kQWaiterBitmap, [](std::uint8_t* line, void* user) {
                auto& words = *static_cast<std::array<std::uint32_t, 4>*>(user);
                for (unsigned w = 0; w < words.size(); ++w)
                    words[w] = ps3_bswap32(std::atomic_ref<std::uint32_t>(
                        *reinterpret_cast<std::uint32_t*>(line + 8 + 4 * w)).load(std::memory_order_relaxed));
                return 0;
            }, &words);
            Kv bitmap{"bitmap", {}};
            std::snprintf(bitmap.value, sizeof bitmap.value, "0x%08X%08X%08X%08X",
                          words[0], words[1], words[2], words[3]);
            stop_with(PS3_STOP_JQ2_WAKE, "a Port2Sync waiter id the firmware's wake cannot signal was needed",
                      "Only ids 1..63 can wake. The guest bitmap retains the attempted allocation; "
                      "the HLE stops instead of waiting forever or entering the firmware assertion loop.",
                      {hex("queue", ea_), text("reason", id == 128 ? "waiter-exhausted" : "waiter-id"),
                       dec("id", id), bitmap, text("fw", "pm-wake-rejects-id>63")});
        }
    }
    void release_waiter(unsigned id)
    {
        if (id == 0) return;
        struct Release { unsigned id; } release{id};
        resv_host_rmw(ea_ + kQWaiterBitmap, [](std::uint8_t* line, void* user) {
            const unsigned id = static_cast<Release*>(user)->id;
            auto word = std::atomic_ref<std::uint32_t>(
                *reinterpret_cast<std::uint32_t*>(line + 8 + 4 * (id / 32)));
            const std::uint32_t bits = ps3_bswap32(word.load(std::memory_order_relaxed));
            word.store(ps3_bswap32(bits & ~(0x80000000u >> (id % 32))), std::memory_order_release);
            return 1;
        }, &release);
    }
    void block_on(unsigned id)
    {
        /* A failed wait (a destroyed event flag) would otherwise return to
         * sync(), find bit 31 still set and wait again, forever. */
        const std::int32_t rc = evf_wait(flag_ids_[id >> 6], std::uint64_t{1} << (id & 63u));
        if (rc != 0) {
            stop_with(PS3_STOP_JQ2_WAKE, "a blocked Port2Sync's event-flag wait failed",
                      "The queue's event flag was destroyed or refused the wait; the waiter can never be woken.",
                      {hex("queue", ea_), text("reason", "wait"), dec("id", id), hex("rc", static_cast<std::uint32_t>(rc))});
        }
    }

    std::mutex& port_mutex() { return port_mu_; }

    bool destroy_flags()
    {
        bool ok = true;
        for (std::uint32_t id : flag_ids_) ok = evf_destroy(id) == 0 && ok;
        return ok;
    }

private:
    struct WorkerArg {
        Queue* q;
        unsigned instance;
    };

    static void* worker_thunk(void* p)
    {
        std::unique_ptr<WorkerArg> arg(static_cast<WorkerArg*>(p));
        arg->q->worker_loop(arg->instance);
        return nullptr;
    }

    [[noreturn]] void stop_ring_entry(const char* reason, const Dispatch& d) const
    {
        stop_with(PS3_STOP_JQ2_RING_ENTRY,
                  "a JobQueue2 ring entry the executor cannot run",
                  "The entry was not written by jq2_port_push_job / jq2_port_copy_push / jq2_port_push_sync: "
                  "check that the title does not write the command ring itself, or that the queue block was not "
                  "overwritten.",
                  {hex("queue", ea_), text("reason", reason), hex("index", d.ring_index), hex("descriptor", d.job.descriptor_ea)});
    }

    void worker_loop(unsigned instance)
    {
        spurs_job_set_spu_num(instance);
        std::vector<std::uint8_t> desc;
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            const std::uint64_t seen = gen_;
            if (!paused_) {
                const PassResult pass = sched_->intake_pass(instance);
                if (pass.end == PassEnd::unsupported_entry) {
                    const std::uint32_t at = ring_ea_ + 8u * pass.read_after;
                    stop_with(PS3_STOP_JQ2_RING_ENTRY,
                              "the command ring holds an entry the JobQueue2 executor does not model "
                              "(JOBLIST, FLUSH or an unknown command)",
                              "Only JOB, COPY and SYNC are modelled (ring.hpp); anything else needs its own model "
                              "before it can run.",
                              {hex("queue", ea_), hex("read_index", pass.read_after), hex("ring_ea", at)});
                }
                if (pass.dispatched) {
                    const Dispatch d = *pass.dispatched;
                    JobStart js = admit(d, instance, desc);
                    const std::optional<jq2_job_observer> obs = observer_;
                    ++running_;
                    counters_.dispatched.fetch_add(1);
                    ++gen_;
                    cv_.notify_all(); /* the entries behind it may be runnable elsewhere */
                    lk.unlock();
                    run_job(js, obs);
                    lk.lock();
                    if (!sched_->complete(d.id)) {
                        stop_with(PS3_STOP_JQ2_STATE, "a finished job is not in flight in the scheduler",
                                  "An executor invariant broke; this is a bug in the executor.",
                                  {hex("queue", ea_), dec("dispatch", d.id)});
                    }
                    --running_;
                    ++gen_;
                    cv_.notify_all();
                    continue;
                }
                if (pass.syncs_consumed != 0 || pass.read_after != pass.read_before) {
                    /* Consumed something and dispatched nothing (a SYNC, or entries
                     * behind a barrier): the next pass may dispatch. */
                    ++gen_;
                    cv_.notify_all();
                    continue;
                }
            }
            if (stopping_ && running_ == 0 && sched_->outstanding() == 0) break;
            cv_.wait(lk, [&] { return gen_ != seen; });
        }
    }

    /* Under mu_: read the descriptor of a job just admitted. This is the
     * admission fetch of contract S2: the bytes are taken now and the job runs
     * on them, whatever the guest does to its descriptor afterwards. */
    JobStart admit(const Dispatch& d, unsigned instance, std::vector<std::uint8_t>& desc)
    {
        const std::uint32_t size = size_bytes(d.job.size);
        const std::uint32_t ea = d.job.descriptor_ea;
        const std::uint64_t limit = ppu_vm_size ? ppu_vm_size : 0x1'0000'0000ull;
        if (ea == 0 || size == 0 || std::uint64_t{ea} + size > limit) stop_ring_entry("descriptor", d);
        desc.assign(host(ea), host(ea) + size);

        JobStart js;
        js.queue_ea = ea_;
        js.port_ea = d.job.port_ea;
        js.descriptor_ea = ea;
        js.size = size;
        js.tag = d.job.tag;
        js.copy = d.copy;
        js.worker = instance;
        js.dispatch_id = d.id;
        js.descriptor = desc;
        return js;
    }

    void run_job(const JobStart& js, const std::optional<jq2_job_observer>& obs);
    void credit(const JobStart& js);
    void wake(const Wakeup& w, const JobStart& js);

    const std::uint32_t ea_;
    const std::uint32_t ring_ea_;
    const std::uint16_t depth_;
    const std::uint32_t max_descriptor_;
    const unsigned workers_;
    const ExecutorHooks hooks_;

    GuestRing* ring_ = nullptr; /* owned by sched_ */
    std::optional<Scheduler> sched_;
    std::mutex mu_; /* sched_, gen_, paused_, stopping_, running_ */
    std::condition_variable cv_;
    std::uint64_t gen_ = 0;
    bool paused_ = false;
    bool stopping_ = false;
    std::optional<jq2_job_observer> observer_; /* under mu_ */
    unsigned running_ = 0;
    std::atomic<bool> shut_{false};

    Counters counters_;
    std::array<std::uint32_t, 2> flag_ids_{};
    std::vector<pthread_t> threads_;

    std::mutex port_mu_; /* port slot allocation and the attached count */

    PoolWake pool_wake_;
    std::unique_ptr<DescriptorPool> pool_;
};

/* ---- running a job ------------------------------------------------------ */

thread_local const JobStart* t_job = nullptr;

[[noreturn]] void job_end_stop(const spu_context* ctx, const char* reason)
{
    const JobStart* js = t_job;
    const ps3_stop_site site{ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK, js ? js->worker : 0};
    stop_with(PS3_STOP_JQ2_JOB_END,
              "a JobQueue2 job ended without returning to the job manager",
              "A job must return through its link register; a stop or halt in a job body is a fault in the job "
              "image or in an SPU instruction the runtime does not run. The port was not credited.",
              {hex("queue", js ? js->queue_ea : 0), hex("job", js ? js->descriptor_ea : 0),
               hex("port", js ? js->port_ea : 0), dec("tag", js ? js->tag : 0), dec("worker", js ? js->worker : 0),
               text("reason", reason), hex("status", ctx->status), hex("stop_code", ctx->stop_code),
               hex("spu_pc", ctx->pc & SPU_LS_MASK)},
              &site);
}

/* A job that called jm word 0 (SPU_JQ2_JM_CALL_LS), the policy module's
 * scheduler switch. The HLE runs one job per worker and has no manager to
 * switch to; the job is neither executed nor credited. */
[[noreturn]] void jm_call_stop(const spu_context* ctx)
{
    const JobStart* js = t_job;
    const ps3_stop_site site{SPU_JQ2_JM_CALL_LS, ctx->gpr[0]._u32[0] & SPU_LS_MASK, js ? js->worker : 0};
    stop_with(PS3_STOP_JQ2_JM_CALL,
              "a JobQueue2 job called jm interface word 0, the policy module's scheduler switch",
              "No Odin Sphere job image does; the executor runs one job per worker and has no manager to yield an "
              "Execute slot to. The port was not credited.",
              {hex("queue", js ? js->queue_ea : 0), hex("job", js ? js->descriptor_ea : 0),
               hex("port", js ? js->port_ea : 0), dec("tag", js ? js->tag : 0), dec("worker", js ? js->worker : 0),
               hex("request", ctx->gpr[3]._u32[0]), hex("status", ctx->status)},
              &site);
}

/* The one definition of a job's normal return, for every job body. The job
 * returns through the link register the loader plants (kJobReturnLs, 0x3FF00),
 * and the two kinds of body arrive there differently:
 *   interpreted  local store at the link is zero: `stop 0`, status STOP, stop
 *                code 0, pc just past it;
 *   lifted       the resolver (spu_indirect_branch, SPU_JOB_RETURN_LS) halts the
 *                run when a branch lands on the link: pc == the link and the
 *                status is neither STOP nor HALT.
 * spu_run_with_halt reports a halt for the lifted return as well as for a
 * lifted `heq`/`hgt` assertion, so its result cannot tell them apart; the
 * context can. Anything else - a `stop` with another code, a halt-assertion
 * (status HALT), a run that ended elsewhere - ended the job without returning. */
void require_normal_return(const spu_context* ctx)
{
    const std::uint32_t pc = ctx->pc & SPU_LS_MASK;
    /* The stop word the loader planted at jm word 0, reached by an interpreted
     * job or, through the resolver's interpretation of the live bytes, a lifted one. */
    if (ctx->status == SPU_STATUS_STOPPED_BY_STOP && ctx->stop_code == SPU_JQ2_JM_CALL_STOP &&
        pc == SPU_JQ2_JM_CALL_LS + 4u)
        jm_call_stop(ctx);
    if (ctx->status == SPU_STATUS_STOPPED_BY_STOP && ctx->stop_code == 0 && pc == kJobReturnLs + 4u) return;
    if (pc == kJobReturnLs && (ctx->status & (SPU_STATUS_STOPPED_BY_STOP | SPU_STATUS_STOPPED_BY_HALT)) == 0) return;
    const char* reason = ctx->status == SPU_STATUS_STOPPED_BY_HALT   ? "halt"
                         : ctx->status == SPU_STATUS_STOPPED_BY_STOP ? "stop"
                                                                     : "status";
    job_end_stop(ctx, reason);
}

void interpreter_entry(spu_context* ctx)
{
    /* From the entry state the loader staged: pc is the image base (0x7C00 in
     * the JQ2 profile), not LS 0. */
    spu_interp_run(ctx, ctx->pc);
    require_normal_return(ctx);
}

void Queue::run_job(const JobStart& js, const std::optional<jq2_job_observer>& obs)
{
    if (hooks_.on_admit != nullptr) hooks_.on_admit(hooks_.user, js);

    JobBody body;
    if (hooks_.select_body != nullptr) body = hooks_.select_body(hooks_.user, js);
    if (body.entry == nullptr) body = interpreter_body();

    /* What the queue contributes to the PM's staging (spu_workload.h), from the
     * queue block as the PM reads it: maxDescriptor, the SPURS EA, the
     * workload id (queue+0x6C); the SPU the job stands on is the worker. */
    const spu_jq2_env env{max_descriptor_, ld32(ea_ + kQSpurs), ld32(ea_ + kQWorkloadFlags) & kWorkloadIdMask,
                          js.worker};

    t_job = &js;
    int rc;
    if (!obs) {
        /* The loader runs the body and calls `end` with the finished context,
         * where the job's ending is classified (require_normal_return). */
        spu_job_hooks hooks{};
        hooks.end = [](void*, spu_context* ctx) { require_normal_return(ctx); };
        rc = spu_run_jq2_job(body.entry, body.image_id, js.descriptor_ea, js.descriptor.data(), js.size, &env,
                             &hooks);
    } else {
        /* The loader hands the layout it chose to the observer; the job's
         * identity is what this worker admitted. */
        struct Observed {
            const jq2_job_observer* obs;
            jq2_job_info info;
        } o{&*obs, {}};
        o.info.queue = js.queue_ea;
        o.info.port = js.port_ea;
        o.info.descriptor_ea = js.descriptor_ea;
        o.info.descriptor_size = js.size;
        o.info.descriptor = js.descriptor.data();
        o.info.tag = js.tag;
        o.info.copy = js.copy ? 1u : 0u;
        o.info.worker = js.worker;
        o.info.dispatch_id = js.dispatch_id;
        spu_job_hooks hooks{};
        hooks.user = &o;
        hooks.start = [](void* user, spu_context* ctx, const spu_job_layout* l) {
            auto& ob = *static_cast<Observed*>(user);
            jq2_job_info& i = ob.info;
            i.image_ea = l->image_ea;
            i.image_ls = l->image_ls;
            i.image_bytes = l->image_bytes;
            i.bss_bytes = l->bss_bytes;
            i.desc_ls = l->desc_ls;
            i.desc_bytes = l->desc_bytes;
            i.io_ls = l->io_ls;
            i.io_bytes = l->io_bytes;
            i.out_ls = l->out_ls;
            i.out_bytes = l->out_bytes;
            i.scratch_ls = l->scratch_ls;
            i.scratch_bytes = l->scratch_bytes;
            i.stack_bytes = l->stack_bytes;
            i.r1 = l->r1;
            i.ctx_ls = l->ctx_ls;
            if (ob.obs->job_start != nullptr) ob.obs->job_start(ob.obs->user, ctx, &i);
        };
        hooks.end = [](void* user, spu_context* ctx) {
            require_normal_return(ctx);
            auto& ob = *static_cast<Observed*>(user);
            if (ob.obs->job_end != nullptr) ob.obs->job_end(ob.obs->user, ctx, ctx->gpr[3]._u32[0], &ob.info);
        };
        rc = spu_run_jq2_job(body.entry, body.image_id, js.descriptor_ea, js.descriptor.data(), js.size, &env,
                             &hooks);
    }
    t_job = nullptr;
    if (rc != 0) {
        /* The loader could not stage the job: nothing ran. */
        stop_with(PS3_STOP_JQ2_JOB_END, "the SPURS job loader refused a JobQueue2 job",
                  "The descriptor names an image the loader cannot stage; the port was not credited.",
                  {hex("queue", ea_), hex("job", js.descriptor_ea), dec("tag", js.tag), text("reason", "loader"),
                   dec("rc", static_cast<std::uint32_t>(rc))});
    }
    counters_.executed.fetch_add(1);
    if (hooks_.on_done != nullptr) hooks_.on_done(hooks_.user, js);

    /* The snapshot's slot is the job's own: give it back before the port is
     * credited, so a Sync that returns also sees the slot free. */
    if (js.copy) pool_->release(js.descriptor_ea);
    credit(js);
}

void Queue::credit(const JobStart& js)
{
    if (js.port_ea == 0) {
        counters_.uncredited.fetch_add(1);
        return;
    }
    const std::optional<Port> port = bind_port(js.port_ea);
    if (!port) {
        stop_with(PS3_STOP_JQ2_STATE, "a ring entry names a port EA that is not a port block",
                  "An executor invariant broke: the writer only encodes 128-aligned port EAs.",
                  {hex("queue", ea_), hex("port", js.port_ea)});
    }
    /* Counted first: a Sync that sees the credit must see the count, and the
     * count must never exceed the runs. */
    counters_.credited.fetch_add(1);
    Port p = *port;
    const Wakeup w = p.credit_completion();
    if (w.due) wake(w, js);
}

void Queue::wake(const Wakeup& w, const JobStart& js)
{
    const bool event_flag = w.kind == static_cast<std::uint32_t>(WaitKind::event_flag);
    if (event_flag && w.id < kWaiterIds) {
        evf_set(flag_ids_[w.id >> 6], std::uint64_t{1} << (w.id & 63u));
        return;
    }
    const bool fiber = w.kind == static_cast<std::uint32_t>(WaitKind::fiber);
    stop_with(PS3_STOP_JQ2_WAKE, "a registered Port2Sync waiter cannot be woken",
              fiber ? "The waiter is a fiber (wait kind 0); fibers are not modelled yet, so no HLE Port2Sync registers "
                      "one. The port line was written by something else."
              : event_flag ? "The firmware's PM wake rejects waiter ids above 63 (helper 0x6348), so it never wakes "
                             "such a waiter; this executor registers ids 1..63 only. The port line was written by "
                             "something else."
                           : "The port line's wait kind is not one this executor registers; the line was written by "
                             "something else.",
              {hex("queue", ea_), hex("port", js.port_ea), dec("kind", w.kind), dec("id", w.id),
               text("reason", fiber ? "fiber" : event_flag ? "waiter-id" : "wait-kind")});
}

/* ---- the registry ------------------------------------------------------- */

std::mutex g_registry_mu;
std::unordered_map<std::uint32_t, std::shared_ptr<Queue>> g_queues;

std::shared_ptr<Queue> find_queue(std::uint32_t ea)
{
    std::lock_guard<std::mutex> lk(g_registry_mu);
    const auto it = g_queues.find(ea);
    return it == g_queues.end() ? nullptr : it->second;
}

/* ---- ports ---------------------------------------------------------------- */

struct PortRef {
    std::shared_ptr<Queue> queue;
    std::uint32_t slot = 0;
    std::int32_t error = JQ2_OK;
};

/* The firmware's port guards: null, alignment, initialised, and a queue that
 * is still alive. */
PortRef resolve_port(std::uint32_t port)
{
    PortRef ref;
    PortStatus guard{};
    if (!bind_port(port, &guard)) {
        ref.error = static_cast<std::int32_t>(guard);
        return ref;
    }
    if (!jq2_guest_range_ok(port, kPortBytes)) {
        ps3_stop_guest_range("resolve_port", "port", port, kPortBytes, jq2_guest_limit());
    }
    if (host(port)[kPInit] != 1) {
        ref.error = JQ2_ERR_STAT;
        return ref;
    }
    ref.queue = find_queue(ld32(port + kPQueue));
    if (!ref.queue) {
        ref.error = JQ2_ERR_STAT;
        return ref;
    }
    ref.slot = ld32(port + kPSlot);
    return ref;
}

struct BitmapRmw {
    unsigned slot;       /* in: the slot to free; out: the slot allocated */
    bool allocate;
    bool ok;
};

int bitmap_rmw(std::uint8_t* line, void* user)
{
    auto& b = *static_cast<BitmapRmw*>(user);
    auto* words = reinterpret_cast<std::uint32_t*>(line);
    if (b.allocate) {
        for (unsigned w = 0; w < 32; ++w) {
            std::atomic_ref<std::uint32_t> ref(words[w]);
            std::uint32_t raw = ref.load(std::memory_order_acquire);
            for (;;) {
                const std::uint32_t v = ps3_bswap32(raw);
                if (v == 0xFFFF'FFFFu) break;
                const unsigned bit = static_cast<unsigned>(__builtin_ctz(~v));
                if (ref.compare_exchange_strong(raw, ps3_bswap32(v | (1u << bit)), std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                    b.slot = w * 32 + bit;
                    b.ok = true;
                    return 1;
                }
            }
        }
        b.ok = false;
        return 0;
    }
    std::atomic_ref<std::uint32_t> ref(words[b.slot / 32]);
    std::uint32_t raw = ref.load(std::memory_order_acquire);
    for (;;) {
        const std::uint32_t v = ps3_bswap32(raw);
        const std::uint32_t bit = 1u << (b.slot % 32);
        if ((v & bit) == 0) {
            b.ok = false;
            return 0;
        }
        if (ref.compare_exchange_strong(raw, ps3_bswap32(v & ~bit), std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            b.ok = true;
            return 1;
        }
    }
}

/* Enqueue one entry for a port's push, with the port accounting when the push
 * carries flags & 1. */
std::int32_t enqueue(Queue& q, std::uint32_t port_ea, const Entry& entry, bool accounted)
{
    q.push_checked(port_ea);
    const auto write = [&]() -> std::uint32_t {
        const std::optional<PushResult> pushed = q.push_unless_shut(entry);
        if (!pushed) {
            /* Reached by a push that passed check_push while Shutdown ran. The
             * accounted path has already counted it in B and the copy path
             * holds a pool slot: neither can be undone here, so it is a stop
             * rather than a silent drop. */
            stop_with(PS3_STOP_JQ2_STATE, "a push reached the JobQueue2 ring after Shutdown",
                      "A push raced Shutdown: the workers may already be gone, so the entry would never run or be "
                      "credited. Do not push to a queue that is being shut down.",
                      {hex("queue", q.ea()), hex("port", port_ea), text("reason", "push-after-shutdown")});
        }
        const PushResult& r = *pushed;
        if (r.ok()) {
            q.counters().pushed.fetch_add(1);
            return 0;
        }
        if (r.error == PushError::ring_full) return static_cast<std::uint32_t>(JQ2_ERR_AGAIN);
        stop_with(PS3_STOP_JQ2_STATE, "the ring writer refused an entry the executor had validated",
                  "An executor invariant broke; this is a bug in the executor.",
                  {hex("queue", q.ea()), dec("error", static_cast<unsigned>(r.error)),
                   dec("encode", static_cast<unsigned>(r.encode_error))});
    };
    std::uint32_t rc;
    if (accounted) {
        std::optional<Port> port = bind_port(port_ea);
        rc = port->push_accounted(write);
    } else {
        rc = write();
    }
    if (rc == 0) q.kick();
    return static_cast<std::int32_t>(rc);
}

std::optional<SizeCode> size_code_of(std::uint32_t size)
{
    switch (size) {
    case 64: return SizeCode::bytes64;
    case 128: return SizeCode::bytes128;
    case 256: return SizeCode::bytes256;
    }
    return std::nullopt;
}

/* The shared front of both pushes: port, tag, descriptor size. */
struct PushCheck {
    PortRef port;
    SizeCode code = SizeCode::bytes64;
    std::int32_t error = JQ2_OK;
};

PushCheck check_push(std::uint32_t port, std::uint32_t descriptor, std::uint32_t size, std::uint32_t tag)
{
    PushCheck c;
    c.port = resolve_port(port);
    if (c.port.error != JQ2_OK) {
        c.error = c.port.error;
        return c;
    }
    if (descriptor == 0) {
        c.error = JQ2_ERR_NULL_POINTER;
    } else if (!aligned(descriptor, 16)) {
        c.error = JQ2_ERR_ALIGN;
    } else if (tag > 15) {
        c.error = JQ2_ERR_INVAL;
    } else if (const auto code = size_code_of(size); !code || size > c.port.queue->max_descriptor()) {
        c.error = JQ2_ERR_INVAL;
    } else {
        c.code = *code;
    }
    if (c.error == JQ2_OK && c.port.queue->shut()) c.error = JQ2_ERR_STAT;
    return c;
}

/* Port2Sync's wait: one bit of the queue's event flags (executor.hpp). */
struct EventFlagWait {
    Queue& q;
    Waiter acquire() { return {WaitKind::event_flag, q.acquire_waiter()}; }
    void block(const Waiter& w) { q.block_on(w.id); }
    void release(const Waiter& w) { q.release_waiter(w.id); }
};

}  // namespace

/* ---- the public C++ surface --------------------------------------------- */

void set_executor_hooks(const ExecutorHooks& hooks)
{
    std::lock_guard<std::mutex> lk(g_hooks_mu);
    g_hooks = hooks;
}

JobBody interpreter_body() noexcept { return {&interpreter_entry, -1}; }

bool set_intake_paused(std::uint32_t queue_ea, bool paused)
{
    const auto q = find_queue(queue_ea);
    if (!q) return false;
    q->set_paused(paused);
    return true;
}

std::optional<QueueView> observe_queue(std::uint32_t queue_ea)
{
    const auto q = find_queue(queue_ea);
    if (!q) return std::nullopt;
    return q->view();
}

}  // namespace ps3recomp::spurs::jq2

/* ---- jq2.h ---------------------------------------------------------------- */

using namespace ps3recomp::spurs::jq2;

extern "C" {

int32_t jq2_queue_create(uint32_t revision, uint32_t sdk_version, uint32_t spurs, uint32_t queue, uint32_t attr,
                         uint32_t pool, uint32_t pool_attr, uint32_t name, uint32_t command_ring,
                         uint32_t command_depth, uint32_t worker_count, uint32_t extra)
{
    (void)name;  /* only names the workload in the firmware; the workers are anonymous */
    (void)extra; /* the firmware body does not read it */

    if (spurs == 0 || queue == 0 || attr == 0 || command_ring == 0) return JQ2_ERR_NULL_POINTER;
    if (!aligned(spurs, 128) || !aligned(queue, 128) || !aligned(command_ring, 128)) return JQ2_ERR_ALIGN;
    if ((pool == 0) != (pool_attr == 0)) return JQ2_ERR_INVAL;
    if (command_depth > JQ2_MAX_DEPTH || worker_count > JQ2_MAX_WORKERS) return JQ2_ERR_INVAL;
    if (!jq2_guest_range_ok(attr, 0x0C)) {
        ps3_stop_guest_range("jq2_queue_create", "attr", attr, 0x0C, jq2_guest_limit());
    }
    const std::uint32_t classes = ld32(attr + 0x04);
    const std::uint32_t max_descriptor = ld32(attr + 0x08);
    if (classes < 1 || classes > 16 || max_descriptor < 256 || max_descriptor > 0x3FF || !aligned(max_descriptor, 128))
        return JQ2_ERR_INVAL;
    if (pool != 0 && (!aligned(pool, 128) || !aligned(pool_attr, 128))) return JQ2_ERR_ALIGN;
    /* Beyond the firmware's checks: a ring of no entries, or no worker to run
     * it, can never make progress. */
    if (command_depth == 0 || worker_count == 0) return JQ2_ERR_INVAL;
    if (find_queue(queue)) return JQ2_ERR_STAT;

    if (!jq2_guest_range_ok(queue, kQueueBytes)) {
        ps3_stop_guest_range("jq2_queue_create", "queue", queue, kQueueBytes, jq2_guest_limit());
    }
    const std::uint64_t ring_size = 2048ull + (((static_cast<std::uint64_t>(command_depth) + 15ull) * 8ull) & ~0x7Full);
    if (!jq2_guest_range_ok(command_ring, ring_size)) {
        ps3_stop_guest_range("jq2_queue_create", "command_ring", command_ring, ring_size, jq2_guest_limit());
    }

    ExecutorHooks hooks;
    {
        std::lock_guard<std::mutex> lk(g_hooks_mu);
        hooks = g_hooks;
    }

    /* The firmware zeroes the queue block and the command buffer (the ring and
     * the room the buffer carries beyond it). */
    std::memset(host(queue), 0, kQueueBytes);
    std::memset(host(command_ring), 0, ring_size);

    std::int32_t rc = evf_create(queue + kQEventFlag);
    if (rc != 0) return rc;
    rc = evf_create(queue + kQEventFlag + 4);
    if (rc != 0) {
        evf_destroy(ld32(queue + kQEventFlag));
        return rc;
    }
    const std::uint32_t flag0 = ld32(queue + kQEventFlag);
    const std::uint32_t flag1 = ld32(queue + kQEventFlag + 4);

    auto q = std::make_shared<Queue>(queue, command_ring, static_cast<std::uint16_t>(command_depth), max_descriptor,
                                     worker_count, hooks);
    q->attach(flag0, flag1);
    if (!q->valid()) {
        stop_with(PS3_STOP_JQ2_STATE, "the scheduler refused a queue the create body had validated",
                  "An executor invariant broke; this is a bug in the executor.", {hex("queue", queue)});
    }

    if (pool != 0) {
        PoolError error = PoolError::none;
        auto p = DescriptorPool::create(PoolConfig{queue, pool, pool_attr}, q->pool_hook(), error);
        if (!p) {
            q->destroy_flags();
            return firmware_code(error);
        }
        q->set_pool(std::move(p));
    }

    /* The queue the firmware leaves: ring, depth and the index word with its
     * phase bits (read 0, write 0, flags 0x60). */
    st32(queue + kQRing, command_ring);
    st16(queue + kQDepth, static_cast<std::uint16_t>(command_depth));
    st16(queue + kQInit16, 256);
    st32(queue + 0x04, kFlagsInitial);
    st32(queue + kQSpurs, spurs);
    st32(queue + kQWorkloadFlags, spurs | kWorkloadId);
    st32(queue + kQMaxDescriptor, max_descriptor);
    st32(queue + kQSdk, sdk_version);
    st32(queue + kQRevision, revision);

    {
        std::lock_guard<std::mutex> lk(g_registry_mu);
        if (!g_queues.emplace(queue, q).second) {
            q->destroy_flags();
            return JQ2_ERR_STAT;
        }
    }
    if (!q->start()) {
        stop_with(PS3_STOP_JQ2_STATE, "a JobQueue2 worker thread could not be started",
                  "The host refused a thread; check the process's thread limit.",
                  {hex("queue", queue), dec("workers", worker_count)});
    }
    return JQ2_OK;
}

int32_t jq2_queue_shutdown(uint32_t queue)
{
    if (queue == 0) return JQ2_ERR_NULL_POINTER;
    if (!aligned(queue, 128)) return JQ2_ERR_ALIGN;
    const auto q = find_queue(queue);
    if (!q) return JQ2_ERR_STAT;
    if (ld32(queue + kQPortCount) != 0) return JQ2_ERR_BUSY;
    q->begin_shutdown();
    return JQ2_OK;
}

int32_t jq2_queue_join(uint32_t queue, uint32_t* exit_value)
{
    if (queue == 0 || exit_value == nullptr) return JQ2_ERR_NULL_POINTER;
    if (!aligned(queue, 128)) return JQ2_ERR_ALIGN;
    const auto q = find_queue(queue);
    if (!q || !q->shut()) return JQ2_ERR_STAT;

    q->join_workers(); /* the workers leave once the ring is drained and every job is done */
    {
        std::lock_guard<std::mutex> lk(g_registry_mu);
        g_queues.erase(queue);
    }
    const bool flags_ok = q->destroy_flags();
    *exit_value = ld32(queue + kQExit);
    st32(queue + kQWorkloadFlags, ld32(queue + kQWorkloadFlags) | kJoined);
    return flags_ok ? JQ2_OK : JQ2_ERR_STAT;
}

int32_t jq2_port_create(uint32_t port, uint32_t queue)
{
    if (port == 0 || queue == 0) return JQ2_ERR_NULL_POINTER;
    if (!aligned(port, 128) || !aligned(queue, 128)) return JQ2_ERR_ALIGN;
    const auto q = find_queue(queue);
    if (!q || q->shut()) return JQ2_ERR_STAT;

    if (!jq2_guest_range_ok(port, kPortBytes)) {
        ps3_stop_guest_range("jq2_port_create", "port", port, kPortBytes, jq2_guest_limit());
    }

    std::memset(host(port), 0, kPortBytes);
    st32(port + kPLink, queue);

    std::lock_guard<std::mutex> lk(q->port_mutex());
    const std::uint32_t count = ld32(queue + kQPortCount);
    BitmapRmw rmw{0, true, false};
    if (count >= kMaxPorts || (resv_host_rmw(queue + kQPortBitmap, &bitmap_rmw, &rmw), !rmw.ok))
        return JQ2_ERR_AGAIN;
    st32(queue + kQPortCount, count + 1);
    st32(port + kPQueue, queue);
    st32(port + kPSlot, rmw.slot);
    host(port)[kPInit] = 1;
    return JQ2_OK;
}

int32_t jq2_port_destroy(uint32_t port)
{
    const PortRef ref = resolve_port(port);
    if (ref.error != JQ2_OK) return ref.error;
    /* Firmware 0x02004890: a port whose state byte +0x21 is set is not in a
     * state Destroy accepts (A0F), before the submissions check. (+0x18 == 0
     * is already STAT: resolve_port finds no queue.) */
    if (host(port)[0x21] != 0) return JQ2_ERR_STAT;
    if (ld32(port + 0x10) != 0) return JQ2_ERR_BUSY; /* submissions outstanding */

    Queue& q = *ref.queue;
    std::lock_guard<std::mutex> lk(q.port_mutex());
    BitmapRmw rmw{ref.slot, false, false};
    resv_host_rmw(q.ea() + kQPortBitmap, &bitmap_rmw, &rmw);
    if (!rmw.ok) {
        stop_with(PS3_STOP_JQ2_STATE, "a port's queue slot is not allocated",
                  "The port block was overwritten, or the slot bitmap was.",
                  {hex("queue", q.ea()), hex("port", port), dec("slot", ref.slot)});
    }
    st32(q.ea() + kQPortCount, ld32(q.ea() + kQPortCount) - 1);
    st32(port + kPLink, 0);
    st32(port + kPQueue, 0);
    host(port)[kPInit] = 0;
    return JQ2_OK;
}

int32_t jq2_port_push_job(uint32_t port, uint32_t descriptor, uint32_t size, uint32_t tag, uint32_t flags)
{
    const PushCheck c = check_push(port, descriptor, size, tag);
    if (c.error != JQ2_OK) return c.error;
    const bool accounted = (flags & 1u) != 0;
    const JobEntry entry{accounted ? port : 0u, descriptor, static_cast<std::uint8_t>(tag), c.code};
    return enqueue(*c.port.queue, port, entry, accounted);
}

int32_t jq2_port_copy_push(uint32_t port, uint32_t source, uint32_t size, uint32_t copy_size, uint32_t tag,
                           uint32_t flags)
{
    const PushCheck c = check_push(port, source, size, tag);
    if (c.error != JQ2_OK) return c.error;
    if (size > copy_size) return JQ2_ERR_INVAL;
    Queue& q = *c.port.queue;
    DescriptorPool* pool = q.pool();
    if (pool == nullptr) return JQ2_ERR_NULL_POINTER; /* no class is configured (jq2c-015) */

    const ClassResult cls = DescriptorPool::class_of_copy_size(copy_size);
    if (!cls.ok()) return firmware_code(cls.error);
    const AcquireResult slot = pool->acquire(cls.cls, true);
    if (!slot.ok()) return firmware_code(slot.error);
    if (const PoolError e = pool->snapshot(source, size, slot.ea); e != PoolError::none) {
        pool->release(slot.ea);
        return firmware_code(e);
    }

    const bool accounted = (flags & 1u) != 0;
    const CopyEntry entry{accounted ? port : 0u, slot.ea, static_cast<std::uint8_t>(tag), c.code};
    const std::int32_t rc = enqueue(q, port, entry, accounted);
    if (rc != JQ2_OK) pool->release(slot.ea);
    return rc;
}

int32_t jq2_port_push_sync(uint32_t port, uint32_t tag_mask, uint32_t flags)
{
    const PortRef ref = resolve_port(port);
    if (ref.error != JQ2_OK) return ref.error;
    if (tag_mask == 0 || tag_mask > 0xFFFFu || (flags & ~1u) != 0) return JQ2_ERR_INVAL;
    if (ref.queue->shut()) return JQ2_ERR_STAT;
    return enqueue(*ref.queue, port, SyncEntry{static_cast<std::uint16_t>(tag_mask)}, false);
}

int32_t jq2_port_sync(uint32_t port, uint32_t mode)
{
    const PortRef ref = resolve_port(port);
    if (ref.error != JQ2_OK) return ref.error;
    std::optional<Port> p = bind_port(port);
    EventFlagWait wait{*ref.queue};
    return static_cast<std::int32_t>(p->sync(mode, wait));
}

int32_t jq2_queue_stats(uint32_t queue, jq2_stats* out)
{
    if (out == nullptr) return JQ2_ERR_NULL_POINTER;
    const auto q = find_queue(queue);
    if (!q) return JQ2_ERR_STAT;
    *out = q->snapshot_stats();
    return JQ2_OK;
}

int32_t jq2_set_job_observer(uint32_t queue, const jq2_job_observer* observer)
{
    const auto q = find_queue(queue);
    if (!q) return JQ2_ERR_STAT;
    q->set_observer(observer);
    return JQ2_OK;
}

} /* extern "C" */

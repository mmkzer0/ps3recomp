/* sys_lwmutex_* / sys_lwcond_* through the HLE dispatch a title uses.
 *
 * Every call goes through ps3_hle_call with the NID of the firmware name, on a
 * ppu_context carrying the calling guest thread's id, over a guest memory image
 * whose sys_lwmutex_t / sys_lwcond_t words are read back big-endian: the same
 * path and the same bytes a lifted title produces. The runtime library
 * supplies the handlers (runtime/syscalls/sys_lwsync.cpp).
 *
 *   test_lwsync contract
 *       the guest-visible contract: recursion, EBUSY / EDEADLK / EPERM / ESRCH /
 *       EINVAL, destroy with a waiter, timeouts that leave the mutex owned,
 *       signal one / all / to, FIFO order.
 *   test_lwsync mp <iterations> [pairs]
 *       litmus, message passing: a producer publishes a payload under an
 *       lwmutex, a consumer reads it under the same lwmutex and must never see
 *       a payload that does not match its sequence number, or time run backward.
 *   test_lwsync lostwake <iterations> [pairs]
 *       litmus, lost wakeup: a signal raised while the waiter is entering
 *       sys_lwcond_wait must wake it, a wait must not return without a signal.
 *
 * <iterations> is the total over all pairs of threads. ctest runs a short set
 * (label t0, also run under TSan) and the same source at 10^8 (label stress).
 * A stalled run (no progress for 30 s: a lost wakeup, a deadlock) is a failure
 * of its own, not a ctest timeout. All time is 64-bit steady_clock. */
#include "ppu_context.h"
#include "ps3emu/error_codes.h"
#include <algorithm>
#include "ps3emu/nid.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <unistd.h>

/* ---- what ppu_hle.cpp needs from a loader this fixture does not link ------ */
extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size = 0x10000;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ps3_hle_call(uint32_t, ppu_context*);
void ps3_lwsync_register(void);
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) { std::abort(); }
void ppu_prof_stamp(void*, unsigned) {}
void ppu_gcm_pump() {}
uint32_t ppu_prof_resolve_host(void*) { return 0; }
void ps3_msf(const char*, ...) {}
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) {}
void spu_lockline_lock() {}
void spu_lockline_unlock() {}
void ps3_ww_report_inline(uint32_t, uint64_t, int) {}
/* PS3_PRIO_START's scheduling hint (sys_ppu_thread.c). It only delays a creator while its higher-priority child
 * has not blocked yet, there is no thread table here, and the knob is off by default, so nothing is noted. */
void ppu_thread_note_block(uint64_t) {}
/* ps3_hle_call spills the caller's r2 into its stack frame (r1 + 0x28). */
uint32_t vm_read32(uint64_t a) { uint32_t v; std::memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
void vm_write32(uint64_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(vm_base + a, &v, 4); }
uint64_t vm_read64(uint64_t a) { uint64_t v; std::memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }
void ppu_dump_guest_stack(ppu_context*, const char*) {}
void ppu_dump_bctrl_ring(uint32_t, const char*) {}
void ppu_guest_callstack(const char*) {}
}
void vm_write64(uint64_t a, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(vm_base + a, &v, 8); }
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                  std::fflush(stderr); _exit(1); } } while (0)

using Clock = std::chrono::steady_clock;
static int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/* ---- guest memory ---------------------------------------------------------- */
static constexpr uint32_t kMem = 0x400000;
static std::atomic<uint32_t> g_brk{0x10000};
static uint32_t alloc(uint32_t n)
{
    const uint32_t ea = g_brk.fetch_add((n + 15) & ~15u);
    CHECK(ea + n < kMem);
    return ea;
}
/* Words the library mirrors or reads are accessed atomically (it does too);
 * `plain_*` is guest data the lwmutex under test protects. */
static std::atomic_ref<uint32_t> cell(uint32_t ea) { return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(vm_base + ea)); }
static uint32_t rd(uint32_t ea) { return __builtin_bswap32(cell(ea).load(std::memory_order_relaxed)); }
static void wr(uint32_t ea, uint32_t v) { cell(ea).store(__builtin_bswap32(v), std::memory_order_relaxed); }
static uint32_t plain_rd(uint32_t ea) { return *reinterpret_cast<uint32_t*>(vm_base + ea); }
static void plain_wr(uint32_t ea, uint32_t v) { *reinterpret_cast<uint32_t*>(vm_base + ea) = v; }

/* sys_lwmutex_t / sys_lwcond_t */
static constexpr uint32_t OWNER = 0x00, WAITER = 0x04, ATTRIBUTE = 0x08, RECUR = 0x0C, SQ = 0x10, PAD = 0x14;
static constexpr uint32_t FREE = 0xFFFFFFFFu, DEAD = 0xFFFFFFFEu;
static constexpr int32_t OK = 0;
static constexpr int32_t E_INVAL = (int32_t)CELL_EINVAL, E_SRCH = (int32_t)CELL_ESRCH, E_DEADLK = (int32_t)CELL_EDEADLK,
                         E_PERM = (int32_t)CELL_EPERM, E_BUSY = (int32_t)CELL_EBUSY, E_TIMEDOUT = (int32_t)CELL_ETIMEDOUT,
                         E_FAULT = (int32_t)CELL_EFAULT;

static struct Nids {
    uint32_t create, destroy, lock, trylock, unlock;
    uint32_t ccreate, cdestroy, signal, signal_all, signal_to, wait;
} N;

static void init_nids()
{
    N = { ps3_compute_nid("sys_lwmutex_create"), ps3_compute_nid("sys_lwmutex_destroy"),
          ps3_compute_nid("sys_lwmutex_lock"), ps3_compute_nid("sys_lwmutex_trylock"),
          ps3_compute_nid("sys_lwmutex_unlock"), ps3_compute_nid("sys_lwcond_create"),
          ps3_compute_nid("sys_lwcond_destroy"), ps3_compute_nid("sys_lwcond_signal"),
          ps3_compute_nid("sys_lwcond_signal_all"), ps3_compute_nid("sys_lwcond_signal_to"),
          ps3_compute_nid("sys_lwcond_wait") };
}

/* One guest thread: its own context, so its own thread id. */
struct Guest {
    ppu_context ctx;
    explicit Guest(uint32_t tid)
    {
        std::memset(&ctx, 0, sizeof ctx);
        ctx.thread_id = tid;
        ctx.lr = 0x10100;
        ctx.gpr[1] = 0x400 + tid * 0x40;                  /* a frame of its own: ps3_hle_call writes at r1 + 0x28 */
    }
    int32_t call(uint32_t nid, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0)
    {
        ctx.gpr[3] = a3; ctx.gpr[4] = a4; ctx.gpr[5] = a5;
        ps3_hle_call(nid, &ctx);
        /* a PPC64 s32 result comes back sign-extended, not zero-extended */
        CHECK(ctx.gpr[3] == (uint64_t)(int64_t)(int32_t)ctx.gpr[3]);
        return (int32_t)ctx.gpr[3];
    }
    int32_t lock(uint32_t m, uint64_t timeout_us = 0) { return call(N.lock, m, timeout_us); }
    int32_t trylock(uint32_t m) { return call(N.trylock, m); }
    int32_t unlock(uint32_t m) { return call(N.unlock, m); }
    int32_t wait(uint32_t c, uint64_t timeout_us = 0) { return call(N.wait, c, timeout_us); }
};

static uint32_t make_mutex(Guest& g, bool recursive, uint32_t protocol = 1)
{
    const uint32_t attr = alloc(16), m = alloc(32);
    wr(attr, protocol);
    wr(attr + 4, recursive ? 0x10 : 0x20);
    CHECK(g.call(N.create, m, attr) == OK);
    return m;
}
static uint32_t make_cond(Guest& g, uint32_t m)
{
    const uint32_t attr = alloc(16), c = alloc(16);
    CHECK(g.call(N.ccreate, c, m, attr) == OK);
    return c;
}

template <class F>
static void wait_for(const char* what, F pred, int secs = 20)
{
    const int64_t t0 = now_ns();
    while (!pred()) {
        if (now_ns() - t0 > (int64_t)secs * 1000000000ll) {
            std::fprintf(stderr, "FAIL: gave up waiting for %s\n", what);
            std::fflush(stderr);
            _exit(1);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

/* ---- contract --------------------------------------------------------------- */

static void t_layout()
{
    Guest g(1);
    /* guard bytes around both structures: nothing is written outside them */
    const uint32_t pm = alloc(96);
    std::memset(vm_base + pm, 0xAA, 96);
    const uint32_t m = pm + 32;                           /* 24 bytes */
    const uint32_t attr = alloc(16);
    wr(attr, 1); wr(attr + 4, 0x10);
    CHECK(g.call(N.create, m, attr) == OK);
    CHECK(rd(m + OWNER) == FREE && rd(m + WAITER) == 0 && rd(m + ATTRIBUTE) == 0x11 &&
          rd(m + RECUR) == 0 && rd(m + SQ) != 0 && rd(m + PAD) == 0);
    for (uint32_t i = 0; i < 32; i++) CHECK(vm_base[pm + i] == 0xAA);
    for (uint32_t i = 24; i < 64; i++) CHECK(vm_base[m + i] == 0xAA);

    const uint32_t pc = alloc(64);
    std::memset(vm_base + pc, 0xAA, 64);
    const uint32_t c = pc + 16;                           /* 8 bytes */
    CHECK(g.call(N.ccreate, c, m, alloc(16)) == OK);
    CHECK(rd(c) == m && rd(c + 4) != 0 && rd(c + 4) != 0xAAAAAAAAu);
    for (uint32_t i = 0; i < 16; i++) CHECK(vm_base[pc + i] == 0xAA);
    for (uint32_t i = 8; i < 48; i++) CHECK(vm_base[c + i] == 0xAA);
    std::puts("PASS layout: sys_lwmutex_t is 24 bytes (owner free, attribute, sleep_queue), "
              "sys_lwcond_t is +0 lwmutex EA +4 queue id, nothing written outside them");
}

static void t_invalid()
{
    Guest g(1);
    const uint32_t attr = alloc(16), m = alloc(32);
    wr(attr, 1); wr(attr + 4, 0x30);
    CHECK(g.call(N.create, m, attr) == E_INVAL);          /* neither recursive nor not */
    wr(attr + 4, 0x10); wr(attr, 3);
    CHECK(g.call(N.create, m, attr) == E_INVAL);          /* priority-inherit is not an lwmutex protocol */
    wr(attr, 0);
    CHECK(g.call(N.create, m, attr) == E_INVAL);
    wr(attr, 1);
    CHECK(g.call(N.create, 0, attr) == E_FAULT);
    CHECK(g.call(N.create, m + 2, attr) == E_FAULT);
    CHECK(g.call(N.create, m, 0) == E_FAULT);
    /* an lwmutex nobody created is no kernel object */
    const uint32_t z = alloc(32);
    CHECK(g.lock(z) == E_SRCH);
    CHECK(g.trylock(z) == E_SRCH);
    CHECK(g.unlock(z) == E_SRCH);
    CHECK(g.call(N.destroy, z) == E_SRCH);
    CHECK(g.call(N.ccreate, alloc(16), z, alloc(16)) == E_SRCH);
    CHECK(g.call(N.signal, alloc(16)) == E_SRCH);
    CHECK(g.wait(alloc(16)) == E_FAULT);                  /* its lwmutex word is 0 */
    std::puts("PASS invalid: bad attribute EINVAL, bad address EFAULT, never-created object ESRCH");
}

static void t_recursive()
{
    Guest g(1);
    const uint32_t m = make_mutex(g, true);
    CHECK(g.lock(m) == OK && rd(m + OWNER) == 1 && rd(m + RECUR) == 0);
    CHECK(g.lock(m) == OK && rd(m + RECUR) == 1);
    CHECK(g.trylock(m) == OK && rd(m + RECUR) == 2);
    CHECK(g.unlock(m) == OK && rd(m + RECUR) == 1);
    CHECK(g.unlock(m) == OK && rd(m + RECUR) == 0 && rd(m + OWNER) == 1);
    CHECK(g.unlock(m) == OK && rd(m + OWNER) == FREE);
    CHECK(g.unlock(m) == E_PERM);                         /* not the owner: nobody is */

    const uint32_t n = make_mutex(g, false);              /* not recursive */
    CHECK(g.lock(n) == OK);
    CHECK(g.lock(n) == E_DEADLK);
    CHECK(g.trylock(n) == E_DEADLK);
    CHECK(rd(n + RECUR) == 0);
    CHECK(g.unlock(n) == OK && rd(n + OWNER) == FREE);
    std::puts("PASS recursive: counted re-locks, EDEADLK for a non-recursive owner, EPERM past the last unlock");
}

static void t_ownership()
{
    Guest g1(1), g2(2);
    const uint32_t m = make_mutex(g1, true);
    CHECK(g1.lock(m) == OK);
    CHECK(g2.trylock(m) == E_BUSY);                       /* EBUSY (0x8001000A), not ETIMEDOUT */
    CHECK(g2.unlock(m) == E_PERM);
    CHECK(rd(m + OWNER) == 1);
    CHECK(g1.unlock(m) == OK);
    CHECK(g2.trylock(m) == OK && rd(m + OWNER) == 2);
    CHECK(g1.trylock(m) == E_BUSY);
    CHECK(g2.unlock(m) == OK);
    std::puts("PASS ownership: trylock EBUSY, unlock by a non-owner EPERM, ownership follows the thread id");
}

static void t_destroy()
{
    Guest g1(1), g2(2);
    const uint32_t m = make_mutex(g1, false);
    CHECK(g1.lock(m) == OK);
    CHECK(g1.call(N.destroy, m) == E_BUSY);               /* held */
    std::thread t([&] { CHECK(g2.lock(m) == OK); CHECK(g2.unlock(m) == OK); });
    wait_for("the waiter to queue", [&] { return rd(m + WAITER) == 1; });
    CHECK(g1.call(N.destroy, m) == E_BUSY);               /* held, with a waiter */
    CHECK(g1.unlock(m) == OK);
    t.join();
    CHECK(rd(m + OWNER) == FREE);

    /* an lwcond waiter keeps its lwmutex alive */
    const uint32_t c = make_cond(g1, m);
    std::atomic<int> in{0};
    std::thread w([&] { CHECK(g2.lock(m) == OK); in = 1; CHECK(g2.wait(c) == OK); CHECK(g2.unlock(m) == OK); });
    wait_for("the waiter to start", [&] { return in.load() == 1; });
    CHECK(g1.lock(m) == OK);                              /* held by g2 until it is inside the wait */
    CHECK(g1.call(N.destroy, m) == E_BUSY);               /* g1 owns it */
    CHECK(g1.unlock(m) == OK);
    CHECK(g1.call(N.destroy, m) == E_BUSY);               /* free, but g2 still waits through the lwcond */
    CHECK(g1.call(N.cdestroy, c) == E_BUSY);              /* and the lwcond has a waiter */
    CHECK(g1.call(N.signal, c) == OK);
    w.join();
    CHECK(g1.call(N.cdestroy, c) == OK);
    CHECK(g1.call(N.cdestroy, c) == E_SRCH);              /* gone */
    CHECK(g1.call(N.destroy, m) == OK && rd(m + OWNER) == DEAD);
    CHECK(g1.call(N.destroy, m) == E_INVAL);              /* destroyed twice */
    CHECK(g1.lock(m) == E_INVAL && g1.trylock(m) == E_INVAL);
    CHECK(g1.unlock(m) == E_PERM);
    std::puts("PASS destroy: EBUSY while held, queued on or waited on through an lwcond; "
              "EINVAL and EPERM afterwards");
}

static void t_lock_timeout()
{
    Guest g1(1), g2(2);
    const uint32_t m = make_mutex(g1, false);
    CHECK(g1.lock(m) == OK);
    int64_t took = 0;
    int32_t rc = -1;
    std::thread t([&] {
        const int64_t t0 = now_ns();
        rc = g2.lock(m, 50000);
        took = now_ns() - t0;
    });
    t.join();
    CHECK(rc == E_TIMEDOUT);                              /* never OK for a lock nobody released */
    CHECK(took >= 50000000ll && took < 5000000000ll);
    CHECK(rd(m + OWNER) == 1 && rd(m + WAITER) == 0);     /* g1 keeps it; the timed-out waiter left the queue */
    CHECK(g1.unlock(m) == OK && rd(m + OWNER) == FREE);
    CHECK(g2.lock(m, 50000) == OK && g2.unlock(m) == OK);
    std::puts("PASS lock timeout: ETIMEDOUT after the full wait, owner untouched, waiter dequeued");
}

static void t_lock_fifo()
{
    Guest g1(1);
    const uint32_t m = make_mutex(g1, false);
    CHECK(g1.lock(m) == OK);
    std::atomic<uint32_t> order[3];
    std::atomic<int> n{0};
    for (auto& o : order) o = 0;
    std::vector<std::thread> ts;
    for (uint32_t tid = 2; tid <= 4; tid++) {
        ts.emplace_back([&, tid] {
            Guest g(tid);
            CHECK(g.lock(m) == OK);
            order[n.fetch_add(1)] = tid;
            CHECK(g.unlock(m) == OK);
        });
        wait_for("the waiter to queue", [&] { return rd(m + WAITER) == tid - 1; });
    }
    CHECK(g1.unlock(m) == OK);
    for (auto& t : ts) t.join();
    CHECK(order[0] == 2 && order[1] == 3 && order[2] == 4);
    std::puts("PASS lock FIFO: the lock goes to waiters in arrival order");
}

static void t_cond_timeout()
{
    Guest g(1);
    const uint32_t m = make_mutex(g, true), c = make_cond(g, m);
    CHECK(g.wait(c) == E_PERM);                           /* the mutex is not held */
    Guest other(2);
    CHECK(g.lock(m) == OK);
    CHECK(other.wait(c) == E_PERM);                       /* held by someone else */
    CHECK(g.lock(m) == OK && rd(m + RECUR) == 1);
    const int64_t t0 = now_ns();
    CHECK(g.wait(c, 40000) == E_TIMEDOUT);                /* nobody signals */
    const int64_t took = now_ns() - t0;
    CHECK(took >= 40000000ll && took < 5000000000ll);
    CHECK(rd(m + OWNER) == 1 && rd(m + RECUR) == 1);      /* owned again, at the saved recursion count */
    CHECK(g.unlock(m) == OK && g.unlock(m) == OK && rd(m + OWNER) == FREE);

    /* a signal with nobody waiting is not remembered for the next waiter */
    CHECK(g.call(N.signal, c) == OK && g.call(N.signal_all, c) == OK);
    CHECK(g.call(N.signal_to, c, 7) == E_PERM);
    CHECK(g.lock(m) == OK);
    CHECK(g.wait(c, 30000) == E_TIMEDOUT);
    CHECK(g.unlock(m) == OK);
    std::puts("PASS cond timeout: EPERM without the mutex, ETIMEDOUT with the mutex re-owned at its "
              "recursion count, no stored signal");
}

/* The waiter protocol of the signal tests: lock, announce, wait, record, unlock. */
struct CondFix {
    uint32_t m = 0, c = 0;
    std::atomic<int> started{0};
    std::atomic<int> woke{0};
    std::atomic<uint32_t> order[8];
    CondFix() { for (auto& o : order) o = 0; }
};

static std::thread start_waiter(CondFix& f, uint32_t tid, int already_started)
{
    std::thread t([&f, tid] {
        Guest g(tid);
        CHECK(g.lock(f.m) == OK);
        f.started++;
        CHECK(g.wait(f.c) == OK);
        CHECK(rd(f.m + OWNER) == tid);                    /* woken, and the lwmutex is ours */
        f.order[f.woke.fetch_add(1)] = tid;
        CHECK(g.unlock(f.m) == OK);
    });
    wait_for("the waiter to start", [&] { return f.started.load() == already_started + 1; });
    Guest main(1);
    CHECK(main.lock(f.m) == OK);                          /* the waiter holds it until it is queued */
    CHECK(main.unlock(f.m) == OK);
    return t;
}

static void t_signal()
{
    for (int hold = 1; hold >= 0; hold--) {
        Guest main(1);
        CondFix f;
        f.m = make_mutex(main, false);
        f.c = make_cond(main, f.m);
        std::vector<std::thread> ts;
        ts.push_back(start_waiter(f, 2, 0));
        ts.push_back(start_waiter(f, 3, 1));

        auto signal = [&](uint32_t nid, uint64_t arg = 0) {
            if (hold) CHECK(main.lock(f.m) == OK);
            CHECK(main.call(nid, f.c, arg) == OK);
            if (hold) CHECK(main.unlock(f.m) == OK);
        };
        signal(N.signal);
        wait_for("one waiter to wake", [&] { return f.woke.load() >= 1; });
        sleep_ms(150);
        CHECK(f.woke.load() == 1 && f.order[0] == 2);     /* exactly one, the first to queue */
        signal(N.signal);
        wait_for("the second waiter to wake", [&] { return f.woke.load() == 2; });
        CHECK(f.order[1] == 3);
        for (auto& t : ts) t.join();
    }
    std::puts("PASS signal: wakes exactly one of two waiters, in queue order, with or without the mutex held");
}

static void t_signal_all()
{
    for (int hold = 1; hold >= 0; hold--) {
        Guest main(1);
        CondFix f;
        f.m = make_mutex(main, false);
        f.c = make_cond(main, f.m);
        std::vector<std::thread> ts;
        ts.push_back(start_waiter(f, 2, 0));
        ts.push_back(start_waiter(f, 3, 1));
        sleep_ms(100);
        CHECK(f.woke.load() == 0);                        /* nothing wakes a waiter but a signal */
        if (hold) CHECK(main.lock(f.m) == OK);
        CHECK(main.call(N.signal_all, f.c) == OK);
        sleep_ms(50);
        if (hold) {
            CHECK(f.woke.load() == 0);                    /* queued on the mutex main still owns */
            CHECK(main.unlock(f.m) == OK);
        }
        wait_for("both waiters to wake", [&] { return f.woke.load() == 2; });
        for (auto& t : ts) t.join();
        const uint32_t a = f.order[0], b = f.order[1];
        CHECK((a == 2 && b == 3) || (a == 3 && b == 2));
        if (hold) CHECK(a == 2 && b == 3);                /* requeued in order, handed the lock in order */
    }
    std::puts("PASS signal_all: wakes both waiters");
}

static void t_signal_to()
{
    for (int hold = 1; hold >= 0; hold--) {
        Guest main(1);
        CondFix f;
        f.m = make_mutex(main, false);
        f.c = make_cond(main, f.m);
        std::vector<std::thread> ts;
        ts.push_back(start_waiter(f, 2, 0));
        ts.push_back(start_waiter(f, 3, 1));
        ts.push_back(start_waiter(f, 4, 2));
        if (hold) CHECK(main.lock(f.m) == OK);
        CHECK(main.call(N.signal_to, f.c, 9) == E_PERM);  /* no such waiter */
        CHECK(main.call(N.signal_to, f.c, 3) == OK);
        if (hold) CHECK(main.unlock(f.m) == OK);
        wait_for("thread 3 to wake", [&] { return f.woke.load() >= 1; });
        sleep_ms(100);
        CHECK(f.woke.load() == 1 && f.order[0] == 3);     /* the one asked for, not the first in line */
        CHECK(main.call(N.signal_to, f.c, 3) == E_PERM);  /* it is no longer waiting */
        CHECK(main.call(N.signal_all, f.c) == OK);
        wait_for("the rest to wake", [&] { return f.woke.load() == 3; });
        for (auto& t : ts) t.join();
    }
    std::puts("PASS signal_to: wakes the named waiter only; EPERM when it is not waiting");
}

static int run_contract(const char* only)
{
    static const struct { const char* name; void (*fn)(); } scenarios[] = {
        {"layout", t_layout}, {"invalid", t_invalid}, {"recursive", t_recursive},
        {"ownership", t_ownership}, {"destroy", t_destroy}, {"lock_timeout", t_lock_timeout},
        {"lock_fifo", t_lock_fifo}, {"cond_timeout", t_cond_timeout}, {"signal", t_signal},
        {"signal_all", t_signal_all}, {"signal_to", t_signal_to},
    };
    bool ran = false;
    for (const auto& s : scenarios) {
        if (only && std::strcmp(only, s.name)) continue;
        s.fn();
        ran = true;
    }
    if (!ran) return 2;
    std::puts("CONTRACT OK");
    return 0;
}

/* ---- litmus ------------------------------------------------------------------- */

struct Stall {
    std::vector<std::atomic<uint64_t>*> counters;
    std::atomic<bool> stop{false};
};

/* 30 s without any pair advancing is a failure, reported as such. */
static void watchdog(Stall* s, const char* name)
{
    uint64_t last = 0;
    int64_t since = now_ns();
    while (!s->stop.load(std::memory_order_acquire)) {
        sleep_ms(200);
        uint64_t sum = 0;
        for (auto* c : s->counters) sum += c->load(std::memory_order_relaxed);
        if (sum != last) { last = sum; since = now_ns(); continue; }
        if (now_ns() - since > 30ll * 1000000000ll) {
            std::fprintf(stderr, "FAIL %s: no progress for 30 s at %llu steps: a lost wakeup or a deadlock\n",
                         name, (unsigned long long)sum);
            std::fflush(stderr);
            _exit(1);
        }
    }
}

static uint32_t payload(uint32_t seq, uint32_t j) { return seq * 0x9E3779B1u + j * 0x85EBCA6Bu + 0x1234567u; }

struct MpPair {
    uint32_t m = 0, slot = 0;                             /* slot: seq, 4 payload words, guest plain data */
    std::atomic<uint64_t> progress{0};
};

static void mp_producer(MpPair* p, uint32_t tid, uint32_t n)
{
    Guest g(tid);
    for (uint32_t i = 1; i <= n; i++) {
        CHECK(g.lock(p->m) == OK);
        plain_wr(p->slot, i);
        for (uint32_t j = 0; j < 4; j++) plain_wr(p->slot + 4 + 4 * j, payload(i, j));
        CHECK(g.unlock(p->m) == OK);
        if ((i & 255) == 0) p->progress.fetch_add(256, std::memory_order_relaxed);
    }
}

static void mp_consumer(MpPair* p, uint32_t tid, uint32_t n)
{
    Guest g(tid);
    uint32_t last = 0;
    for (uint64_t spin = 1;; spin++) {
        /* mostly trylock polling, so the producer's blocking lock rarely queues
         * behind the consumer; every 16th call blocks, so hand-offs happen too */
        const int32_t rc = (spin & 15) ? g.trylock(p->m) : g.lock(p->m);
        if (rc == E_BUSY) continue;
        CHECK(rc == OK);
        const uint32_t seq = plain_rd(p->slot);
        uint32_t w[4];
        for (uint32_t j = 0; j < 4; j++) w[j] = plain_rd(p->slot + 4 + 4 * j);
        CHECK(g.unlock(p->m) == OK);
        if (seq) {
            for (uint32_t j = 0; j < 4; j++) {
                if (w[j] != payload(seq, j)) {
                    std::fprintf(stderr, "FAIL mp: seq %u word %u = 0x%08X, published 0x%08X\n", seq, j, w[j], payload(seq, j));
                    std::fflush(stderr);
                    _exit(1);
                }
            }
        }
        CHECK(seq >= last);                               /* a slot that went back in time */
        last = seq;
        if (seq == n) return;
        p->progress.fetch_add(1, std::memory_order_relaxed);
    }
}

struct LwPair {
    uint32_t m = 0, c = 0, flag = 0;                      /* flag: guest plain data under m */
    std::atomic<uint32_t> ack{0};
    std::atomic<uint64_t> progress{0};
};

static void lw_waiter(LwPair* p, uint32_t tid, uint32_t n)
{
    Guest g(tid);
    CHECK(g.lock(p->m) == OK);
    for (uint32_t i = 1; i <= n; i++) {
        while (plain_rd(p->flag) == 0) {
            CHECK(g.wait(p->c) == OK);
            /* the only thing that ends a wait is a signal, and the signaller
             * sets the flag before it signals */
            CHECK(plain_rd(p->flag) == 1);
        }
        plain_wr(p->flag, 0);
        p->ack.store(i, std::memory_order_release);
        p->progress.fetch_add(1, std::memory_order_relaxed);
    }
    CHECK(g.unlock(p->m) == OK);
}

static void lw_signaller(LwPair* p, uint32_t tid, uint32_t n)
{
    Guest g(tid);
    for (uint32_t i = 1; i <= n; i++) {
        while (p->ack.load(std::memory_order_acquire) != i - 1) std::this_thread::yield();
        CHECK(g.lock(p->m) == OK);
        plain_wr(p->flag, 1);
        switch (i % 3) {
        case 0: CHECK(g.call(N.signal, p->c) == OK); CHECK(g.unlock(p->m) == OK); break;
        case 1: CHECK(g.unlock(p->m) == OK); CHECK(g.call(N.signal, p->c) == OK); break;
        default: CHECK(g.unlock(p->m) == OK); CHECK(g.call(N.signal_all, p->c) == OK); break;
        }
    }
}

static int run_litmus(bool mp, uint64_t iters, unsigned pairs)
{
    if (!pairs) pairs = std::max(1u, std::min(4u, std::thread::hardware_concurrency() / 2));
    const uint32_t n = (uint32_t)std::max<uint64_t>(1, iters / pairs);
    Guest setup(1);
    std::vector<std::unique_ptr<MpPair>> mps;
    std::vector<std::unique_ptr<LwPair>> lws;
    Stall stall;
    for (unsigned k = 0; k < pairs; k++) {
        if (mp) {
            mps.emplace_back(new MpPair);
            MpPair& p = *mps.back();
            p.m = make_mutex(setup, false);
            p.slot = alloc(32);
            stall.counters.push_back(&p.progress);
        } else {
            lws.emplace_back(new LwPair);
            LwPair& p = *lws.back();
            p.m = make_mutex(setup, false);
            p.c = make_cond(setup, p.m);
            p.flag = alloc(16);
            stall.counters.push_back(&p.progress);
        }
    }
    const char* name = mp ? "mp" : "lostwake";
    std::thread dog(watchdog, &stall, name);
    const int64_t t0 = now_ns();
    std::vector<std::thread> ts;
    for (unsigned k = 0; k < pairs; k++) {
        const uint32_t tid = 100 + 2 * k;
        if (mp) {
            ts.emplace_back(mp_producer, mps[k].get(), tid, n);
            ts.emplace_back(mp_consumer, mps[k].get(), tid + 1, n);
        } else {
            ts.emplace_back(lw_waiter, lws[k].get(), tid, n);
            ts.emplace_back(lw_signaller, lws[k].get(), tid + 1, n);
        }
    }
    for (auto& t : ts) t.join();
    stall.stop.store(true, std::memory_order_release);
    dog.join();
    const double secs = (double)(now_ns() - t0) / 1e9;
    std::printf("LITMUS %s OK: %llu iterations on %u thread pairs in %.1f s\n", name,
                (unsigned long long)n * pairs, pairs, secs);
    return 0;
}

int main(int argc, char** argv)
{
    vm_base = static_cast<uint8_t*>(calloc(kMem + 64, 1));
    if (!vm_base) return 2;
    init_nids();
    ps3_lwsync_register();
    if ((argc == 2 || argc == 3) && !std::strcmp(argv[1], "contract")) return run_contract(argc == 3 ? argv[2] : nullptr);
    if ((argc == 3 || argc == 4) && (!std::strcmp(argv[1], "mp") || !std::strcmp(argv[1], "lostwake")))
        return run_litmus(!std::strcmp(argv[1], "mp"), std::strtoull(argv[2], nullptr, 0),
                          argc == 4 ? (unsigned)std::strtoul(argv[3], nullptr, 0) : 0);
    std::fprintf(stderr, "usage: test_lwsync contract [scenario] | mp <n> [pairs] | lostwake <n> [pairs]\n");
    return 2;
}

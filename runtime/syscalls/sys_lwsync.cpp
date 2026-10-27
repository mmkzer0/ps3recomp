/*
 * ps3recomp - lightweight mutex and condition variable (sysPrxForUser)
 *
 * The one backend of sys_lwmutex_* and sys_lwcond_* (the older host-pointer
 * variant in libs/system/sysPrxForUser.c is gone; see docs/RUNTIME.md,
 * "lwmutex and lwcond"). Each is a context handler: lwmutex ownership is by
 * the calling guest thread's id (ppu_context::thread_id), and every pointer
 * argument is a guest effective address.
 *
 * Object model. The firmware keeps the wait queue in a kernel object named by
 * the id the guest stores at sys_lwmutex_t::sleep_queue / sys_lwcond_t::
 * lwcond_queue, and keeps owner, waiter count and recursion count in guest
 * memory. The state here is authoritative on the host, in tables of such
 * objects behind the same ids, and the guest words are written through as a
 * mirror (owner is 0xFFFFFFFF when free, 0xFFFFFFFE once destroyed, as the
 * firmware's sysPrxForUser leaves them), so a guest that reads its own struct
 * sees the firmware's values.
 *
 *   sys_lwmutex_t (24 bytes, big-endian words)
 *     +0x00 owner   +0x04 waiter   +0x08 attribute (protocol | recursive)
 *     +0x0C recursive_count   +0x10 sleep_queue (object id)   +0x14 pad
 *   sys_lwcond_t (8 bytes)
 *     +0x00 lwmutex EA   +0x04 lwcond_queue (object id)
 *
 * Synchronisation. Each object has one host mutex guarding its state and its
 * FIFO wait queue. A blocked thread sleeps on a node of its own (a host mutex
 * and condition variable, the node living on its stack), so a wake is directed
 * at exactly the thread dequeued and "signal one" wakes one. A wake is the
 * only thing that ends a sleep: the node's `posted` flag is the predicate, so
 * a host-level spurious wakeup is never reported to the guest as a signal. A
 * node is on a queue before the lock that put it there is released, and a wake
 * sets `posted` under the node's own mutex, so a signal issued after a waiter
 * is enqueued reaches it even if the waiter has not started sleeping yet: no
 * lost wakeup. Unlock hands the lock directly to the head waiter (the firmware
 * parks the owner word at "reserved" for the same effect), so a waiter is
 * never overtaken by a thread that arrives later.
 *
 * Memory ordering. Every state change and every wake happens under a host
 * mutex or the node mutex; a lock acquisition is therefore an acquire and the
 * unlock or hand-off that enabled it a release of the same mutex, and what a
 * guest thread wrote inside a critical section is visible to the next owner
 * on arm64 as on x86. The guest-word mirror uses relaxed atomics only: it
 * carries no ordering of its own.
 *
 * Lock order: lwcond object, then lwmutex object, then a waiter node. A thread
 * holds a node's mutex only while it sleeps or posts, never while taking
 * another lock.
 *
 * Semantics follow the firmware as RPCS3's sysPrxForUser / sys_lwmutex /
 * sys_lwcond oracle documents it (read, not copied):
 *   - lock: owner == self is a recursive re-lock when the attribute says
 *     recursive (count in recursive_count, which holds the re-locks beyond
 *     the first) and CELL_EDEADLK otherwise; a destroyed mutex is CELL_EINVAL;
 *     an object id the kernel does not know is CELL_ESRCH;
 *   - trylock: contention is CELL_EBUSY;
 *   - unlock: CELL_EPERM unless the caller owns it;
 *   - destroy: CELL_EBUSY while owned, queued on, or waited on through an
 *     lwcond;
 *   - lwcond wait: CELL_EPERM unless the caller owns the lwcond's lwmutex; the
 *     mutex is released and the caller queued as one step; whatever ends the
 *     wait, the caller owns the mutex again, at its saved recursion count,
 *     when the call returns; a timeout returns CELL_ETIMEDOUT;
 *   - signal and signal_all with no waiter succeed; signal_to a thread that is
 *     not waiting is CELL_EPERM; when the signalling thread owns the lwmutex
 *     the woken waiters are moved onto its queue instead of woken (they would
 *     only block on it), as the firmware's "mode 1" does.
 *
 * Differences from the firmware, deliberate: wait queues are FIFO whatever the
 * protocol attribute (RPCS3 documents the same: "protocol ignored"); an id is
 * a slot index plus a generation, not an lv2 object id.
 */

#include "sys_lwsync.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/error_codes.h"
#include "../../include/ps3emu/nid.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
/* ppu_hle.cpp: a fast form beside a full handler; it returns 0 to take the full call. Weak where the compiler has
 * it: a fixture that supplies its own ps3_hle_register_ctx and links this file without ppu_hle.cpp (tests/
 * test_loader_gaps.cpp) has no fast table, and the handlers then simply run in full. */
#if defined(__GNUC__) || defined(__clang__)
extern "C" __attribute__((weak)) void ps3_hle_register_fast(uint32_t nid, int (*fn)(ppu_context*));
#  define LW_REGISTER_FAST(nid, fn) do { if (ps3_hle_register_fast) ps3_hle_register_fast(nid, fn); } while (0)
#else
extern "C" void ps3_hle_register_fast(uint32_t nid, int (*fn)(ppu_context*));
#  define LW_REGISTER_FAST(nid, fn) ps3_hle_register_fast(nid, fn)
#endif
/* sys_ppu_thread.c: this thread is about to wait (PS3_PRIO_START). sys_ppu_thread.h is not included: its lv2
 * headers redefine ppu_context. Weak where the compiler has it, so a fixture that links this file without the
 * thread table (tests/lwsync) neither pulls sys_ppu_thread.o in nor fails to link; the call is skipped there. */
#if defined(__GNUC__) || defined(__clang__)
extern "C" __attribute__((weak)) void ppu_thread_note_block(uint64_t tid);
#  define LW_NOTE_BLOCK(tid) do { if (ppu_thread_note_block) ppu_thread_note_block(tid); } while (0)
#else
extern "C" void ppu_thread_note_block(uint64_t tid);
#  define LW_NOTE_BLOCK(tid) ppu_thread_note_block(tid)
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr int32_t kOk        = 0;
constexpr int32_t kEAGAIN    = static_cast<int32_t>(CELL_EAGAIN);
constexpr int32_t kEINVAL    = static_cast<int32_t>(CELL_EINVAL);
constexpr int32_t kESRCH     = static_cast<int32_t>(CELL_ESRCH);
constexpr int32_t kEDEADLK   = static_cast<int32_t>(CELL_EDEADLK);
constexpr int32_t kEPERM     = static_cast<int32_t>(CELL_EPERM);
constexpr int32_t kEBUSY     = static_cast<int32_t>(CELL_EBUSY);
constexpr int32_t kETIMEDOUT = static_cast<int32_t>(CELL_ETIMEDOUT);
constexpr int32_t kEFAULT    = static_cast<int32_t>(CELL_EFAULT);
constexpr int32_t kEKRESOURCE = static_cast<int32_t>(CELL_EKRESOURCE);

/* Owner word values; a thread id is never one of them (ids are below 2^31). */
constexpr uint32_t kFree = 0xFFFFFFFFu;
constexpr uint32_t kDead = 0xFFFFFFFEu;

constexpr uint32_t kSyncFifo         = 0x01;
constexpr uint32_t kSyncPriority     = 0x02;
constexpr uint32_t kSyncRetry        = 0x04;
constexpr uint32_t kSyncRecursive    = 0x10;
constexpr uint32_t kSyncNotRecursive = 0x20;

/* sys_lwmutex_t, sys_lwmutex_attribute_t, sys_lwcond_t */
constexpr uint32_t kOwner = 0x00, kWaiter = 0x04, kAttribute = 0x08, kRecur = 0x0C,
                   kSleepQueue = 0x10, kPad = 0x14;
constexpr uint32_t kAttrProtocol = 0x00, kAttrRecursive = 0x04;
constexpr uint32_t kCondMutex = 0x00, kCondQueue = 0x04;

/* ---- guest words ------------------------------------------------------- */

inline bool bad_ea(uint32_t ea) { return ea == 0 || (ea & 3u) != 0; }

inline std::atomic_ref<uint32_t> word(uint32_t ea)
{
    return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(vm_base + ea));
}
inline uint32_t ld(uint32_t ea) { return __builtin_bswap32(word(ea).load(std::memory_order_relaxed)); }
inline void st(uint32_t ea, uint32_t v) { word(ea).store(__builtin_bswap32(v), std::memory_order_relaxed); }

/* ---- wait queue and sleeping ------------------------------------------- */

struct Waiter {
    uint32_t tid = 0;
    uint32_t restore_rec = 0;   /* recursion count the lock is installed with */
    bool     cond = false;      /* an lwcond waiter: counted in Mutex::cond_waiters until it owns the lock */
    bool     in_cond_q = false; /* guarded by the lwcond object's mutex */
    bool     in_mx_q = false;   /* guarded by the lwmutex object's mutex */
    bool     posted = false;    /* guarded by wm */
    bool     owns = false;      /* guarded by wm: the wake handed it the lwmutex */
    Waiter*  prev = nullptr;
    Waiter*  next = nullptr;
    std::mutex              wm;
    std::condition_variable wcv;
};

struct WaitList {
    Waiter*  head = nullptr;
    Waiter*  tail = nullptr;
    uint32_t n = 0;

    void push_back(Waiter* w)
    {
        w->prev = tail;
        w->next = nullptr;
        (tail ? tail->next : head) = w;
        tail = w;
        ++n;
    }
    void remove(Waiter* w)
    {
        (w->prev ? w->prev->next : head) = w->next;
        (w->next ? w->next->prev : tail) = w->prev;
        w->prev = w->next = nullptr;
        --n;
    }
    Waiter* pop_front()
    {
        Waiter* w = head;
        if (w) remove(w);
        return w;
    }
    Waiter* find(uint64_t tid) const
    {
        for (Waiter* w = head; w; w = w->next)
            if (w->tid == tid) return w;
        return nullptr;
    }
};

using Deadline = std::optional<Clock::time_point>;

Deadline deadline_of(uint64_t timeout_us)
{
    if (!timeout_us) return std::nullopt;            /* 0: wait without limit */
    /* ~11 days: keeps now() + timeout inside the clock's signed 64-bit
     * nanosecond count; no guest waits that long for anything but "forever". */
    constexpr uint64_t kMaxUs = 1'000'000'000'000ull;
    return Clock::now() + std::chrono::microseconds(timeout_us < kMaxUs ? timeout_us : kMaxUs);
}

/* Wake `w`. Notifying under the node's mutex means the sleeper cannot return,
 * and free the node, before this call is done with it. */
void post(Waiter& w, bool owns)
{
    std::lock_guard<std::mutex> g(w.wm);
    w.owns = owns;
    w.posted = true;
    w.wcv.notify_one();
}

/* Sleep until posted (true) or the deadline passes first (false). Holds no
 * object lock; the caller has queued `w` and released them. */
bool park(Waiter& w, const Deadline& dl)
{
    std::unique_lock<std::mutex> g(w.wm);
    if (!dl) {
        w.wcv.wait(g, [&] { return w.posted; });
        return true;
    }
    return w.wcv.wait_until(g, *dl, [&] { return w.posted; });
}

bool took_lock(Waiter& w)
{
    std::lock_guard<std::mutex> g(w.wm);
    return w.owns;
}

/* ---- object tables ----------------------------------------------------- */

/* Objects live in fixed chunks that are never freed, so a pointer to a slot
 * stays valid for the life of the process and a lookup can take the slot's own
 * mutex without a table-wide lock. An id is (generation << 16) | (slot + 1);
 * the generation changes on every reuse of a slot, so a stale id finds a slot
 * whose id differs and is reported as ESRCH. */
template <class T>
class Table {
public:
    static constexpr uint32_t kChunk = 256, kChunks = 256;
    static constexpr uint32_t kCap = kChunk * kChunks - 1;   /* slot + 1 fits 16 bits */

    Table() { for (auto& c : chunks_) c.store(nullptr, std::memory_order_relaxed); }

    T* at(uint32_t slot) const
    {
        T* c = chunks_[slot / kChunk].load(std::memory_order_acquire);
        return c ? &c[slot % kChunk] : nullptr;
    }
    bool reserve(uint32_t* slot)
    {
        std::lock_guard<std::mutex> g(m_);
        if (!free_.empty()) {
            *slot = free_.back();
            free_.pop_back();
            return true;
        }
        if (next_ >= kCap) return false;
        if (!chunks_[next_ / kChunk].load(std::memory_order_relaxed))
            chunks_[next_ / kChunk].store(new T[kChunk], std::memory_order_release);
        *slot = next_++;
        return true;
    }
    void release(uint32_t slot)
    {
        std::lock_guard<std::mutex> g(m_);
        free_.push_back(slot);
    }

private:
    std::mutex                m_;
    std::vector<uint32_t>     free_;
    uint32_t                  next_ = 0;
    std::atomic<T*>           chunks_[kChunks];
};

struct Mutex {
    std::mutex m;
    uint32_t   id = 0;            /* 0: slot not in use */
    uint32_t   gen = 0;
    uint32_t   ea = 0;            /* the guest struct the mirror is written to */
    bool       recursive = false;
    uint32_t   owner = kFree;
    uint32_t   rec = 0;           /* re-locks beyond the first */
    uint32_t   cond_waiters = 0;  /* lwcond waiters that have not yet re-owned this lock */
    WaitList   q;
};

struct Cond {
    std::mutex m;
    uint32_t   id = 0;
    uint32_t   gen = 0;
    WaitList   q;
};

/* Process-lifetime tables, never destroyed: a thread blocked at exit must not
 * find its mutex gone. */
Table<Mutex>& mutexes() { static auto* t = new Table<Mutex>(); return *t; }
Table<Cond>&  conds()   { static auto* t = new Table<Cond>(); return *t; }

template <class T>
struct Ref {
    T* x = nullptr;
    std::unique_lock<std::mutex> lk;   /* holds x->m while x is set */
};

template <class T>
Ref<T> find(Table<T>& t, uint32_t id)
{
    Ref<T> r;
    const uint32_t lo = id & 0xFFFFu;
    if (!lo) return r;
    T* x = t.at(lo - 1);
    if (!x) return r;
    std::unique_lock<std::mutex> lk(x->m);
    if (x->id != id) return r;
    r.x = x;
    r.lk = std::move(lk);
    return r;
}

template <class T>
uint32_t new_id(T& x, uint32_t slot)
{
    x.gen = x.gen % 0x7FFFu + 1;
    return x.id = (x.gen << 16) | (slot + 1);
}

void publish(const Mutex& x)
{
    st(x.ea + kOwner, x.owner);
    st(x.ea + kRecur, x.rec);
    st(x.ea + kWaiter, x.q.n);
}

/* ---- lwmutex ----------------------------------------------------------- */

/* The caller owns `x` (x.m held) with no re-locks outstanding: hand it to the
 * head waiter, or free it. */
void release_locked(Mutex& x)
{
    if (Waiter* w = x.q.pop_front()) {
        w->in_mx_q = false;
        if (w->cond) --x.cond_waiters;
        x.owner = w->tid;
        x.rec = w->restore_rec;
        publish(x);
        post(*w, true);
        return;
    }
    x.owner = kFree;
    x.rec = 0;
    publish(x);
}

int32_t mutex_create(uint32_t ea, uint32_t attr_ea)
{
    if (bad_ea(ea) || bad_ea(attr_ea)) return kEFAULT;
    const uint32_t protocol = ld(attr_ea + kAttrProtocol);
    const uint32_t recursive = ld(attr_ea + kAttrRecursive);
    if (recursive != kSyncRecursive && recursive != kSyncNotRecursive) return kEINVAL;
    if (protocol != kSyncFifo && protocol != kSyncPriority && protocol != kSyncRetry) return kEINVAL;

    uint32_t slot;
    if (!mutexes().reserve(&slot)) return kEAGAIN;
    Mutex& x = *mutexes().at(slot);
    std::lock_guard<std::mutex> g(x.m);
    x.ea = ea;
    x.recursive = recursive == kSyncRecursive;
    x.owner = kFree;
    x.rec = 0;
    x.cond_waiters = 0;
    st(ea + kAttribute, protocol | recursive);
    st(ea + kSleepQueue, new_id(x, slot));
    st(ea + kPad, 0);
    publish(x);
    return kOk;
}

int32_t mutex_destroy(uint32_t ea)
{
    if (bad_ea(ea)) return kEFAULT;
    if (ld(ea + kOwner) == kDead) return kEINVAL;
    Ref<Mutex> r = find(mutexes(), ld(ea + kSleepQueue));
    if (!r.x) return kESRCH;
    Mutex& x = *r.x;
    if (x.owner != kFree || x.q.n || x.cond_waiters) return kEBUSY;
    const uint32_t slot = (x.id & 0xFFFFu) - 1;
    x.id = 0;
    x.ea = ea;
    x.owner = kDead;
    st(ea + kOwner, kDead);
    r.lk.unlock();
    mutexes().release(slot);
    return kOk;
}

int32_t mutex_acquire(uint32_t tid, uint32_t ea, uint64_t timeout_us, bool block)
{
    if (bad_ea(ea)) return kEFAULT;
    if (ld(ea + kOwner) == kDead) return kEINVAL;
    Ref<Mutex> r = find(mutexes(), ld(ea + kSleepQueue));
    if (!r.x) return kESRCH;
    Mutex& x = *r.x;
    x.ea = ea;

    if (x.owner == kFree) {
        x.owner = tid;
        x.rec = 0;
        publish(x);
        return kOk;
    }
    if (x.owner == tid) {
        if (!x.recursive) return kEDEADLK;
        if (x.rec == UINT32_MAX) return kEKRESOURCE;
        ++x.rec;
        publish(x);
        return kOk;
    }
    if (!block) return kEBUSY;

    Waiter w;
    w.tid = tid;
    x.q.push_back(&w);
    w.in_mx_q = true;
    publish(x);
    r.lk.unlock();

    LW_NOTE_BLOCK(tid);                                 /* PS3_PRIO_START: about to wait */
    if (park(w, deadline_of(timeout_us))) return kOk;   /* handed the lock by an unlock */

    r.lk.lock();
    if (w.in_mx_q) {                                    /* nobody claimed us: really timed out */
        x.q.remove(&w);
        w.in_mx_q = false;
        publish(x);
        return kETIMEDOUT;
    }
    r.lk.unlock();                                      /* an unlock dequeued us as the deadline fell */
    park(w, std::nullopt);
    return kOk;
}

int32_t mutex_unlock(uint32_t tid, uint32_t ea)
{
    if (bad_ea(ea)) return kEFAULT;
    if (ld(ea + kOwner) == kDead) return kEPERM;
    Ref<Mutex> r = find(mutexes(), ld(ea + kSleepQueue));
    if (!r.x) return kESRCH;
    Mutex& x = *r.x;
    x.ea = ea;
    if (x.owner != tid) return kEPERM;
    if (x.rec) {
        --x.rec;
        publish(x);
        return kOk;
    }
    release_locked(x);
    return kOk;
}

/* ---- lwcond ------------------------------------------------------------ */

int32_t cond_create(uint32_t ea, uint32_t mutex_ea, uint32_t attr_ea)
{
    if (bad_ea(ea) || bad_ea(mutex_ea) || bad_ea(attr_ea)) return kEFAULT;
    {   /* the lwmutex it will be paired with must exist */
        Ref<Mutex> r = find(mutexes(), ld(mutex_ea + kSleepQueue));
        if (!r.x) return kESRCH;
    }
    uint32_t slot;
    if (!conds().reserve(&slot)) return kEAGAIN;
    Cond& c = *conds().at(slot);
    std::lock_guard<std::mutex> g(c.m);
    st(ea + kCondMutex, mutex_ea);
    st(ea + kCondQueue, new_id(c, slot));
    return kOk;
}

int32_t cond_destroy(uint32_t ea)
{
    if (bad_ea(ea)) return kEFAULT;
    Ref<Cond> r = find(conds(), ld(ea + kCondQueue));
    if (!r.x) return kESRCH;
    if (r.x->q.n) return kEBUSY;
    const uint32_t slot = (r.x->id & 0xFFFFu) - 1;
    r.x->id = 0;
    r.lk.unlock();
    conds().release(slot);
    return kOk;
}

/* The lock is `x`'s and the thread has been taken off its cond queue: queue
 * for it again until an unlock installs this thread as owner. */
void reacquire(Mutex& x, Waiter& w)
{
    std::unique_lock<std::mutex> g(x.m);
    if (x.owner == kFree) {
        x.owner = w.tid;
        x.rec = w.restore_rec;
        --x.cond_waiters;
        publish(x);
        return;
    }
    {
        std::lock_guard<std::mutex> wg(w.wm);
        w.posted = false;
        w.owns = false;
    }
    x.q.push_back(&w);
    w.in_mx_q = true;
    publish(x);
    g.unlock();
    park(w, std::nullopt);
}

int32_t cond_wait(uint32_t tid, uint32_t ea, uint64_t timeout_us)
{
    if (bad_ea(ea)) return kEFAULT;
    const uint32_t mutex_ea = ld(ea + kCondMutex);
    if (bad_ea(mutex_ea)) return kEFAULT;
    if (ld(mutex_ea + kOwner) != tid) return kEPERM;

    Ref<Cond> cr = find(conds(), ld(ea + kCondQueue));
    if (!cr.x) return kESRCH;
    Ref<Mutex> mr = find(mutexes(), ld(mutex_ea + kSleepQueue));
    if (!mr.x) return kESRCH;
    Cond& c = *cr.x;
    Mutex& x = *mr.x;
    x.ea = mutex_ea;
    if (x.owner != tid) return kEPERM;

    /* Queue first, release second, both before either lock is dropped: a
     * signal cannot slip between them. */
    Waiter w;
    w.tid = tid;
    w.restore_rec = x.rec;
    w.cond = true;
    c.q.push_back(&w);
    w.in_cond_q = true;
    ++x.cond_waiters;
    x.rec = 0;
    release_locked(x);
    mr.lk.unlock();
    cr.lk.unlock();

    int32_t result = kOk;
    LW_NOTE_BLOCK(tid);                                 /* PS3_PRIO_START: about to wait */
    if (!park(w, deadline_of(timeout_us))) {
        std::unique_lock<std::mutex> cl(c.m);
        if (w.in_cond_q) {                              /* not signalled: timed out */
            c.q.remove(&w);
            w.in_cond_q = false;
            result = kETIMEDOUT;
        } else {                                        /* a signal claimed it as the deadline fell */
            cl.unlock();
            park(w, std::nullopt);
        }
    }
    if (!took_lock(w)) reacquire(x, w);
    return result;
}

enum class Signal { One, All, To };

int32_t cond_signal(uint32_t tid, uint32_t ea, Signal which, uint64_t target)
{
    if (bad_ea(ea)) return kEFAULT;
    Ref<Cond> cr = find(conds(), ld(ea + kCondQueue));
    if (!cr.x) return kESRCH;
    Cond& c = *cr.x;
    if (!c.q.n) return which == Signal::To ? kEPERM : kOk;

    /* A signaller that owns the lwmutex queues the woken waiters on it. */
    Ref<Mutex> mr;
    const uint32_t mutex_ea = ld(ea + kCondMutex);
    if (!bad_ea(mutex_ea) && ld(mutex_ea + kOwner) == tid) {
        mr = find(mutexes(), ld(mutex_ea + kSleepQueue));
        if (mr.x && mr.x->owner != tid) mr = Ref<Mutex>();
    }
    Mutex* held = mr.x;

    auto wake = [&](Waiter* w) {
        c.q.remove(w);
        w->in_cond_q = false;
        if (held) {
            held->q.push_back(w);
            w->in_mx_q = true;
        } else {
            post(*w, false);
        }
    };

    int32_t rc = kOk;
    switch (which) {
    case Signal::One:
        wake(c.q.head);
        break;
    case Signal::All:
        while (c.q.head) wake(c.q.head);
        break;
    case Signal::To:
        if (Waiter* w = c.q.find(target)) wake(w);
        else rc = kEPERM;
        break;
    }
    if (held) {
        held->ea = mutex_ea;
        publish(*held);
    }
    return rc;
}

/* ---- guest ABI --------------------------------------------------------- */

inline uint32_t self(const ppu_context* ctx) { return static_cast<uint32_t>(ctx->thread_id); }
inline void ret(ppu_context* ctx, int32_t rc) { ctx->gpr[3] = static_cast<uint64_t>(static_cast<int64_t>(rc)); }
inline uint32_t arg32(const ppu_context* ctx, int r) { return static_cast<uint32_t>(ctx->gpr[r]); }

void h_lwmutex_create(ppu_context* c)  { ret(c, mutex_create(arg32(c, 3), arg32(c, 4))); }
void h_lwmutex_destroy(ppu_context* c) { ret(c, mutex_destroy(arg32(c, 3))); }
void h_lwmutex_lock(ppu_context* c)    { ret(c, mutex_acquire(self(c), arg32(c, 3), c->gpr[4], true)); }
void h_lwmutex_trylock(ppu_context* c) { ret(c, mutex_acquire(self(c), arg32(c, 3), 0, false)); }
void h_lwmutex_unlock(ppu_context* c)  { ret(c, mutex_unlock(self(c), arg32(c, 3))); }

/* Fast forms for ps3_hle_dispatch (ps3_hle_register_fast): they run no guest code, leave r2 alone and do not
 * block. A lock that is free or already the caller's is taken here; a lock that has to wait, and every error, return
 * 0 and take the full handler, which redoes the call from scratch (a failed non-blocking acquire changes nothing).
 * Unlock never waits: it hands the lock to the head waiter or frees it, and always finishes. Fast calls do not
 * show in PS3_FTRACE or the in-flight table. Upstream's atomic lock word (dc37f00b) has no counterpart: this
 * backend's uncontended path is already one host-mutex acquire and no kernel call. */
int f_lwmutex_lock(ppu_context* c)
{
    if (mutex_acquire(self(c), arg32(c, 3), 0, false) != kOk) return 0;
    ret(c, kOk);
    return 1;
}
int f_lwmutex_unlock(ppu_context* c)
{
    ret(c, mutex_unlock(self(c), arg32(c, 3)));
    return 1;
}

void h_lwcond_create(ppu_context* c)     { ret(c, cond_create(arg32(c, 3), arg32(c, 4), arg32(c, 5))); }
void h_lwcond_destroy(ppu_context* c)    { ret(c, cond_destroy(arg32(c, 3))); }
void h_lwcond_signal(ppu_context* c)     { ret(c, cond_signal(self(c), arg32(c, 3), Signal::One, 0)); }
void h_lwcond_signal_all(ppu_context* c) { ret(c, cond_signal(self(c), arg32(c, 3), Signal::All, 0)); }
void h_lwcond_signal_to(ppu_context* c)  { ret(c, cond_signal(self(c), arg32(c, 3), Signal::To, c->gpr[4])); }
void h_lwcond_wait(ppu_context* c)       { ret(c, cond_wait(self(c), arg32(c, 3), c->gpr[4])); }

}  // namespace

extern "C" void ps3_lwsync_register(void)
{
#define LW(name, fn) ps3_hle_register_ctx(ps3_compute_nid(name), name, fn)
    LW("sys_lwmutex_create",     h_lwmutex_create);
    LW("sys_lwmutex_destroy",    h_lwmutex_destroy);
    LW("sys_lwmutex_lock",       h_lwmutex_lock);
    LW("sys_lwmutex_trylock",    h_lwmutex_trylock);
    LW("sys_lwmutex_unlock",     h_lwmutex_unlock);
    LW_REGISTER_FAST(ps3_compute_nid("sys_lwmutex_lock"),   f_lwmutex_lock);
    LW_REGISTER_FAST(ps3_compute_nid("sys_lwmutex_unlock"), f_lwmutex_unlock);
    LW("sys_lwcond_create",      h_lwcond_create);
    LW("sys_lwcond_destroy",     h_lwcond_destroy);
    LW("sys_lwcond_signal",      h_lwcond_signal);
    LW("sys_lwcond_signal_all",  h_lwcond_signal_all);
    LW("sys_lwcond_signal_to",   h_lwcond_signal_to);
    LW("sys_lwcond_wait",        h_lwcond_wait);
#undef LW
}

/*
 * ps3recomp - SPURS JobQueue2 port line: the PPU/SPU completion accounting
 *
 * A port is a 128-byte, 128-aligned block the guest owns. Four of its words
 * carry the accounting (big-endian u32s in guest memory); every other byte is
 * neither read nor written here:
 *
 *   +0x00  completion A   bits 28:0 count completed jobs, compared as a signed
 *                         29-bit value. Bits 31:29 are the Sync waiter state
 *                         (see below), not part of the count. [jq2c-003; jq2c-010]
 *   +0x04  waiter         the registered Sync waiter's id (a fiber EA or an
 *                         event-flag slot index). The u32 is left stale after a
 *                         wake; it is meaningful only while +0x00 bit 31 is set.
 *   +0x10  submitted B    jobs pushed with the API flags&1 and not yet reaped by
 *                         a successful Sync. [jq2c-007]
 *   +0x14  claim          byte 0 (bits 31:24 of the u32) is 1 while a Port2Sync
 *                         owns the port, 0 otherwise. Bytes 1..3 are not touched.
 *                         [jq2c-002]
 *
 * The PPU writes B and the claim byte with one 64-bit ldarx/stdcx. on the
 * doubleword at +0x10; Port2Sync reads and writes A (and the waiter word) with
 * a 64-bit ldarx/stdcx. on the doubleword at +0x00; the SPU job manager (PM)
 * credits A with a 128-byte GETLLAR/PUTLLC of the whole line. Every update here
 * is therefore one critical section on the line in the reservation model
 * (runtime/memory/reservation.h, resv_host_rmw), so a concurrent SPU PUTLLC or
 * PPU stwcx./stdcx. on the same line fails and retries instead of being lost.
 *
 * Operations (firmware PPU: libspurs_jq 0x02004AF0-0x02004DD0 push body,
 * 0x020045D8-0x020046F0 Port2Sync, 0x02003018-0x0200339C its wait helper; SPU:
 * PM LS 0x33A4-0x3454, decoded from fw/re/jq_pm.dis):
 *
 *   try_submit()        the push, when flags&1. B += 1 iff the claim byte is 0
 *                       and B <= 0x0FFFFFFE; otherwise BUSY and the port is
 *                       untouched. [jq2c-007; JQ-CLOSURE-STATIC B.1]
 *   rollback_submit()   the push, when its ring enqueue failed. If the claim byte
 *                       is 0, B -= 1 modulo 2^32; if it is set, BUSY and the port
 *                       is untouched. Firmware returns BUSY from push_accounted()
 *                       in that case, replacing the enqueue error (0x02004BAC
 *                       -> 0x02004C10, JQ-CLOSURE-REVIEW finding 4). There is
 *                       no underflow guard: a rollback that follows a Sync which
 *                       reaped the push (B == 0) leaves B = 0xFFFFFFFF, as the
 *                       firmware does; the contract is that a rollback pairs with
 *                       the try_submit() before it. [jq2c-007]
 *   credit_completion() what the PM does per completed job that has no semaphore
 *                       of its own: A := (A + 1) mod 2^29 on the line. Bits 30:29
 *                       are kept. Bit 31 is kept unless A was -1, when it is
 *                       cleared: a registered waiter's A (below) has reached 0
 *                       and the PM then wakes it (Wakeup::due). [jq2c-009;
 *                       jq2c-010 corrected: the PM sequence at LS 0x3404-0x3444
 *                       is shli/rotmai (sext29), ai (+1), selb with 0x1FFFFFFF
 *                       for bits 28:0, and an andbi-masked selb for bit 31;
 *                       bits 30:29 come from the old word, they are not cleared]
 *   sync(mode, wait)    Port2Sync. mode & ~7 -> INVAL, before the port is
 *                       touched. Otherwise claim (BUSY if held), then:
 *                         bit 31 of A set (a stale waiter)      -> BUSY
 *                         B > 0x0FFFFFFF                        -> INVAL
 *                         sext29(A) >= B                        -> A := A - B in
 *                                       bits 28:0 (bits 31:29 kept), OK
 *                         sext29(A) <  B, mode & 4              -> AGAIN
 *                         sext29(A) <  B, mode & 4 == 0         -> wait (below)
 *                       then, on every path, the claim byte is cleared and, on
 *                       OK only, B := 0. AGAIN, BUSY and INVAL leave A and B as
 *                       they were. Nothing else is ever returned (contract C2).
 *                       [jq2c-001..004; JQ-CLOSURE-STATIC C]
 *
 * The blocking wait. The firmware registers the waiter in the same doubleword
 * update that subtracts B: A := A - B (negative, 29-bit), bit 31 := 1, bits
 * 30:29 := the wait kind, +0x04 := the waiter id (0x02003104-0x0200337C). Each
 * PM credit then adds one; when A passes from -1 to 0 the PM clears bit 31 and
 * wakes the waiter (fiber signal, kind 0; event-flag bit, kind 2). The waiter
 * returns OK without reading A again: A is 0 by construction. sync() models that
 * exactly. The environment's part is the three calls of a SyncWait policy:
 *   acquire()      allocate the waiter identity (cellFiberPpuSelf or an
 *                  event-flag slot); called with no lock held, only when the
 *                  port needs a wait
 *   block(waiter)  return only once the PM has woken that waiter; sync()
 *                  re-checks bit 31 and calls block() again on a spurious return
 *   release(waiter) free the identity, on every path that acquired one
 * The wake itself is the caller's: credit_completion() reports Wakeup::due, the
 * PM's release call, with the kind and id to signal. The real event-flag wait is
 * not here; tests inject a policy.
 *
 * Tracing evidence: every traced port dump (jq-boot 1802, closure 1224, stall2
 * 1080 records) has bits 31:29 of A, the waiter word and the claim word all
 * zero, i.e. no traced Sync reached the registration path; B3's T1 replay
 * (leifthrasir-recomp tests/test_jq2_port_replay.cpp) checks the rest.
 *
 * Layers:
 *   PortWords      the four words in host order and every transition as a pure
 *                  member function. No memory, no atomics: the unit under test
 *                  for the accounting rules and the oracle for trace replay.
 *   LocalPortLine  a PortWords in host memory, single-threaded.
 *   GuestPortLine  the four words at a guest EA, updated through resv_host_rmw.
 *   BasicPort<L>   the operations above over any line L (L::update(f) runs
 *                  f(PortWords&) as one critical section; f returns true iff it
 *                  changed the words). Port is BasicPort<GuestPortLine>.
 * The header is pure C++20 with no runtime dependency; only GuestPortLine's
 * update (port.cpp) needs the runtime (vm_base, reservation.c).
 */

#ifndef PS3RECOMP_SPURS_JQ2_PORT_HPP
#define PS3RECOMP_SPURS_JQ2_PORT_HPP

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace ps3recomp::spurs::jq2 {

inline constexpr std::uint32_t kPortBytes = 128;
inline constexpr std::uint32_t kPortCompletionOffset = 0x00;
inline constexpr std::uint32_t kPortWaiterOffset = 0x04;
inline constexpr std::uint32_t kPortSubmittedOffset = 0x10;
inline constexpr std::uint32_t kPortClaimOffset = 0x14;

/* A's counter bits, the waiter-state bits above them, and the claim byte. */
inline constexpr std::uint32_t kCreditMask = 0x1FFF'FFFF;
inline constexpr std::uint32_t kWaitingBit = 0x8000'0000;
inline constexpr unsigned kWaitKindShift = 29;
inline constexpr std::uint32_t kWaitKindMask = 0x6000'0000;
inline constexpr std::uint32_t kClaimByteMask = 0xFF00'0000;
inline constexpr std::uint32_t kClaimSet = 0x0100'0000;

/* The largest B a push may store; sync() refuses a larger one in the line. The
 * bound keeps B inside sext29's positive range. [jq2c-007; helper 0x020030B0] */
inline constexpr std::uint32_t kSubmittedMax = 0x0FFF'FFFF;

/* The firmware's JobQueue error codes this surface can produce. */
enum class PortStatus : std::uint32_t {
    ok = 0,
    again = 0x80410A01,         /* mode 4 and A < B */
    inval = 0x80410A02,         /* mode & ~7, or B above kSubmittedMax */
    busy = 0x80410A0A,          /* claim held, stale waiter, or B saturated */
    misaligned = 0x80410A10,    /* port EA not 128-aligned */
    null_pointer = 0x80410A11,  /* port EA 0 */
};

/* The guest guards of every port entry point, in the firmware's order
 * (0x0200462C-0x0200464C): null first, then alignment. */
[[nodiscard]] constexpr PortStatus check_port_ea(std::uint32_t ea) noexcept
{
    if (ea == 0) return PortStatus::null_pointer;
    if ((ea & (kPortBytes - 1)) != 0) return PortStatus::misaligned;
    return PortStatus::ok;
}

/* The low 29 bits of a completion word as a signed value. */
[[nodiscard]] constexpr std::int32_t sext29(std::uint32_t word) noexcept
{
    return static_cast<std::int32_t>(word << 3) >> 3;
}

/* What a registered waiter is woken through: the value of A bits 30:29 and the
 * id stored at +0x04. Kind 0 is a fiber (the id is the fiber EA), kind 2 an
 * event-flag slot (the id is the slot index). */
enum class WaitKind : std::uint32_t {
    fiber = 0,
    event_flag = 2,
};

struct Waiter {
    WaitKind kind = WaitKind::event_flag;
    std::uint32_t id = 0;
    friend bool operator==(const Waiter&, const Waiter&) = default;
};

/* The PM's release call after a credit that brought a registered waiter's A to
 * 0. `kind` is bits 30:29 of the line as the PM re-reads it (the PM's
 * release helper takes 0, 1, 2 or 3), `id` the word at +0x04. */
struct Wakeup {
    bool due = false;
    std::uint32_t kind = 0;
    std::uint32_t id = 0;
    friend bool operator==(const Wakeup&, const Wakeup&) = default;
};

/* What one look at the line decided in a Sync (the firmware helper's single
 * iteration, 0x020030E4-0x0200337C). */
enum class SyncPoll : std::uint8_t {
    complete,    /* A >= B: A := A - B stored */
    again,       /* A < B and not blocking */
    busy,        /* a stale waiter owns A */
    must_wait,   /* A < B, blocking, and no waiter identity yet: nothing stored */
    registered,  /* A := A - B, waiter registered: stored; now block */
};

/* The four accounting words in host order. */
struct PortWords {
    std::uint32_t completion = 0; /* +0x00 */
    std::uint32_t waiter = 0;     /* +0x04 */
    std::uint32_t submitted = 0;  /* +0x10 */
    std::uint32_t claim = 0;      /* +0x14, claim byte in bits 31:24 */

    friend bool operator==(const PortWords&, const PortWords&) = default;

    [[nodiscard]] constexpr bool claim_held() const noexcept { return (claim & kClaimByteMask) != 0; }
    [[nodiscard]] constexpr bool waiting() const noexcept { return (completion & kWaitingBit) != 0; }
    [[nodiscard]] constexpr std::int32_t credit() const noexcept { return sext29(completion); }

    /* Push accounting. Stores iff it returns ok. */
    [[nodiscard]] constexpr PortStatus try_submit() noexcept
    {
        if (claim_held() || submitted > kSubmittedMax - 1) return PortStatus::busy;
        submitted += 1;
        return PortStatus::ok;
    }

    [[nodiscard]] constexpr PortStatus rollback_submit() noexcept
    {
        if (claim_held()) return PortStatus::busy;
        submitted -= 1;
        return PortStatus::ok;
    }

    /* The PM's per-job credit: always stores. */
    constexpr Wakeup credit_completion() noexcept
    {
        const std::int32_t a = sext29(completion);
        const bool was_waiting = waiting();
        const bool still_waiting = was_waiting && a != -1;
        completion = (still_waiting ? kWaitingBit : 0u) | (completion & kWaitKindMask) |
                     (static_cast<std::uint32_t>(a + 1) & kCreditMask);
        if (!(was_waiting && a == -1)) return {};
        return {true, (completion & kWaitKindMask) >> kWaitKindShift, waiter};
    }

    /* Sync, claim step: stores iff it returns ok, and then `submitted_out` is B
     * as the claim saw it. */
    [[nodiscard]] constexpr PortStatus begin_sync(std::uint32_t& submitted_out) noexcept
    {
        if (claim_held()) return PortStatus::busy;
        claim = (claim & ~kClaimByteMask) | kClaimSet;
        submitted_out = submitted;
        return PortStatus::ok;
    }

    /* Sync, one look at A for a Sync that claimed with B == `b` <= kSubmittedMax.
     * Stores iff it returns complete or registered. `waiter_in` is the identity
     * a blocking Sync has acquired, or null before that. */
    [[nodiscard]] constexpr SyncPoll poll_sync(std::uint32_t b, bool block, const Waiter* waiter_in) noexcept
    {
        if (waiting()) return SyncPoll::busy;
        const std::int64_t diff = std::int64_t{sext29(completion)} - std::int64_t{b};
        const std::uint32_t field = static_cast<std::uint32_t>(diff) & kCreditMask;
        if (diff >= 0) {
            completion = (completion & ~kCreditMask) | field;
            return SyncPoll::complete;
        }
        if (!block) return SyncPoll::again;
        if (waiter_in == nullptr) return SyncPoll::must_wait;
        completion = kWaitingBit | (static_cast<std::uint32_t>(waiter_in->kind) << kWaitKindShift) | field;
        waiter = waiter_in->id;
        return SyncPoll::registered;
    }

    /* Sync, tail: always stores. Clears the claim byte; B := 0 on success only. */
    constexpr void end_sync(bool success) noexcept
    {
        claim &= ~kClaimByteMask;
        if (success) submitted = 0;
    }
};

/* The environment of a blocking Sync (see the header comment). */
template <class W>
concept SyncWait = requires(W& wait, const Waiter& waiter) {
    { wait.acquire() } -> std::convertible_to<Waiter>;
    wait.block(waiter);
    wait.release(waiter);
};

/* A port line is anything that can run f(PortWords&) as one critical section
 * and report whether f changed the words. */
template <class L>
concept PortLine = requires(L& line, bool (*f)(PortWords&)) {
    { line.update(f) } -> std::convertible_to<bool>;
};

/* The four words in host memory; single-threaded. The deterministic model for
 * unit tests and trace replay. */
struct LocalPortLine {
    PortWords words;

    template <class F>
    bool update(F&& f)
    {
        return std::forward<F>(f)(words);
    }
};

/* The four words at a guest EA, updated through the reservation model. Built
 * only by at() (or bind_port()), which applies the guest guards, so `ea` is
 * always a valid port EA. Each update() holds the line for one
 * resv_host_rmw() and re-runs `f` if a store the model does not report moved the
 * words under it (the stdcx. retry loop), so `f` must be repeatable. */
class GuestPortLine {
public:
    [[nodiscard]] static std::optional<GuestPortLine> at(std::uint32_t ea, PortStatus* error = nullptr) noexcept
    {
        const PortStatus st = check_port_ea(ea);
        if (error != nullptr) *error = st;
        if (st != PortStatus::ok) return std::nullopt;
        return GuestPortLine(ea);
    }

    template <class F>
    bool update(F&& f) const
    {
        return update_raw(
            [](void* user, PortWords& w) { return (*static_cast<std::remove_reference_t<F>*>(user))(w); },
            &f);
    }

    [[nodiscard]] std::uint32_t ea() const noexcept { return ea_; }

private:
    explicit GuestPortLine(std::uint32_t ea) noexcept : ea_(ea) {}
    using Thunk = bool (*)(void*, PortWords&);
    bool update_raw(Thunk fn, void* user) const;

    std::uint32_t ea_;
};

/* The port operations over a line. */
template <PortLine L>
class BasicPort {
public:
    /* Two constructors, not one with a defaulted argument: a defaulted `L{}`
     * is instantiated by std::is_constructible<BasicPort> (std::optional<Port>
     * asks), and GuestPortLine has no public default constructor; libstdc++
     * rejects that hard where libc++ does not. */
    BasicPort() requires std::default_initializable<L> : line_{} {}
    explicit BasicPort(L line) : line_(std::move(line)) {}

    [[nodiscard]] L& line() noexcept { return line_; }

    [[nodiscard]] PortStatus try_submit()
    {
        PortStatus st = PortStatus::busy;
        line_.update([&](PortWords& w) {
            st = w.try_submit();
            return st == PortStatus::ok;
        });
        return st;
    }

    [[nodiscard]] PortStatus rollback_submit()
    {
        PortStatus st = PortStatus::busy;
        line_.update([&](PortWords& w) {
            st = w.rollback_submit();
            return st == PortStatus::ok;
        });
        return st;
    }

    /* The firmware push, port accounting only: try_submit(), then `enqueue()`
     * (the ring write, returning the firmware status as a u32), then a rollback
     * if it failed. Returns the push's result: the try_submit error, the enqueue
     * error, or BUSY when the rollback met a held claim (replacing the enqueue
     * error, as the firmware does). For a push without the API flags&1 call
     * enqueue() directly: it touches no port word. */
    template <std::invocable E>
        requires std::convertible_to<std::invoke_result_t<E>, std::uint32_t>
    [[nodiscard]] std::uint32_t push_accounted(E&& enqueue)
    {
        if (const PortStatus st = try_submit(); st != PortStatus::ok) return static_cast<std::uint32_t>(st);
        const std::uint32_t r = std::forward<E>(enqueue)();
        if (r == 0) return 0;
        if (const PortStatus st = rollback_submit(); st != PortStatus::ok) return static_cast<std::uint32_t>(st);
        return r;
    }

    /* The PM's credit. The caller performs the wake when the result is due. */
    Wakeup credit_completion()
    {
        Wakeup wake;
        line_.update([&](PortWords& w) {
            wake = w.credit_completion();
            return true;
        });
        return wake;
    }

    /* Port2Sync. `wait` is used only when mode & 4 == 0 and A < B. */
    template <SyncWait W>
    [[nodiscard]] PortStatus sync(std::uint32_t mode, W& wait)
    {
        if ((mode & ~std::uint32_t{7}) != 0) return PortStatus::inval;

        std::uint32_t b = 0;
        PortStatus rc = PortStatus::busy;
        line_.update([&](PortWords& w) {
            rc = w.begin_sync(b);
            return rc == PortStatus::ok;
        });
        if (rc != PortStatus::ok) return rc;

        std::optional<Waiter> waiter;
        if (b > kSubmittedMax) {
            rc = PortStatus::inval;
        } else {
            const bool block = (mode & 4) == 0;
            for (;;) {
                SyncPoll poll{};
                line_.update([&](PortWords& w) {
                    poll = w.poll_sync(b, block, waiter ? &*waiter : nullptr);
                    return poll == SyncPoll::complete || poll == SyncPoll::registered;
                });
                if (poll == SyncPoll::must_wait) {
                    waiter = Waiter(wait.acquire());
                    continue;
                }
                if (poll == SyncPoll::registered) {
                    bool woken = false;
                    do {
                        wait.block(*waiter);
                        line_.update([&](PortWords& w) {
                            woken = !w.waiting();
                            return false;
                        });
                    } while (!woken);
                    rc = PortStatus::ok;
                } else {
                    rc = poll == SyncPoll::complete ? PortStatus::ok
                         : poll == SyncPoll::again  ? PortStatus::again
                                                    : PortStatus::busy;
                }
                break;
            }
            if (waiter) wait.release(*waiter);
        }

        line_.update([&](PortWords& w) {
            w.end_sync(rc == PortStatus::ok);
            return true;
        });
        return rc;
    }

private:
    L line_;
};

using LocalPort = BasicPort<LocalPortLine>;
using Port = BasicPort<GuestPortLine>;

/* The port at guest EA `ea`, or nullopt with the firmware's guard error in
 * *error (null_pointer, misaligned). */
[[nodiscard]] inline std::optional<Port> bind_port(std::uint32_t ea, PortStatus* error = nullptr) noexcept
{
    const std::optional<GuestPortLine> line = GuestPortLine::at(ea, error);
    if (!line) return std::nullopt;
    return Port(*line);
}

}  // namespace ps3recomp::spurs::jq2

#endif  // PS3RECOMP_SPURS_JQ2_PORT_HPP

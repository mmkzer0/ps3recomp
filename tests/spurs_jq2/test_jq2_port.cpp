/*
 * T0: the Jq2 port line (libs/spurs/jq2/port.hpp).
 *
 *   test_jq2_port model       every transition and error code on the pure
 *                             model (LocalPort): push accounting and its
 *                             saturation, rollback (including a rollback
 *                             blocked by a Sync claim), the PM's credit against
 *                             an independent SPU-operation formulation, sext29
 *                             wrap, Port2Sync modes 0 and 4 (valid) against
 *                             1, 2, 3, 5, 6, 7, 8 and above (INVAL), AGAIN / BUSY /
 *                             BAD_ARG, claim release on every path, the waiter
 *                             registration and wake protocol
 *   test_jq2_port guest       the guest binding: big-endian byte layout, bytes
 *                             outside the four words untouched, the guest
 *                             guards, and the reservation model's rule through
 *                             the real PPU (resv_*) and SPU (spu_wrch MFC)
 *                             paths: a store kills a reservation on the line, a
 *                             BUSY (no store) does not
 *   test_jq2_port claim       threads: a Port2Sync holding the claim makes a
 *                             concurrent push return BUSY and leaves B exact
 *   test_jq2_port race N      threads: an SPU credit loop (GETLLAR / PUTLLC of
 *                             the line), host pushes, host Port2Sync of modes 0
 *                             and 4 and a PPU stwcx. loop on other words of the
 *                             same line hammer one port; final A / B exact
 *
 * Synthetic data only. Exit status 0 = pass.
 */

#include <jq2/port.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "jq2_port_harness.h"

using namespace ps3recomp::spurs::jq2;

static std::atomic<int> g_failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                          \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

namespace {

constexpr std::uint32_t kAgain = 0x80410A01, kInval = 0x80410A02, kBusy = 0x80410A0A;
constexpr std::uint32_t kRingFull = 0x80410A07; /* any non-zero enqueue error will do */
constexpr std::uint32_t kLine = 0x00100000u;     /* above the DMA engine's null guard */
constexpr std::uint32_t kGuestBytes = 4u << 20;

constexpr std::uint32_t u32(PortStatus s) { return static_cast<std::uint32_t>(s); }

PortWords W(std::uint32_t a, std::uint32_t waiter, std::uint32_t b, std::uint32_t claim)
{
    return PortWords{a, waiter, b, claim};
}

LocalPort local(PortWords w)
{
    return LocalPort(LocalPortLine{w});
}

/* A Sync environment that records its calls and plays the PM: block() delivers
 * credits through the port until the PM's release is due. */
struct ScriptedWait {
    LocalPort* port;
    explicit ScriptedWait(LocalPort* p) : port(p) {}
    Waiter issue{WaitKind::event_flag, 7};
    int spurious = 0;          /* block() returns this many times without a wake */
    int credits_in_acquire = 0;

    int acquires = 0, blocks = 0, releases = 0;
    int credits_in_block = 0;
    std::vector<PortWords> seen; /* the line at each block() */
    Wakeup last_wake;

    Waiter acquire()
    {
        ++acquires;
        for (int i = 0; i < credits_in_acquire; ++i) (void)port->credit_completion();
        return issue;
    }
    void block(const Waiter&)
    {
        ++blocks;
        seen.push_back(port->line().words);
        if (spurious > 0) {
            --spurious;
            return;
        }
        do {
            last_wake = port->credit_completion();
            ++credits_in_block;
        } while (!last_wake.due);
    }
    void release(const Waiter&) { ++releases; }
};

/* For modes that must never reach the environment. */
struct NoWait {
    Waiter acquire()
    {
        ++calls;
        return {};
    }
    void block(const Waiter&) { ++calls; }
    void release(const Waiter&) { ++calls; }
    int calls = 0;
};

/* ---------------------------------------------------------------------------
 * model
 * ------------------------------------------------------------------------- */

void test_codes_and_guards()
{
    CHECK(u32(PortStatus::ok) == 0);
    CHECK(u32(PortStatus::again) == 0x80410A01);
    CHECK(u32(PortStatus::inval) == 0x80410A02);
    CHECK(u32(PortStatus::busy) == 0x80410A0A);
    CHECK(u32(PortStatus::misaligned) == 0x80410A10);
    CHECK(u32(PortStatus::null_pointer) == 0x80410A11);

    CHECK(check_port_ea(0) == PortStatus::null_pointer);
    CHECK(check_port_ea(0x40) == PortStatus::misaligned);
    CHECK(check_port_ea(0x3b707a01) == PortStatus::misaligned);
    CHECK(check_port_ea(0x3b707a00) == PortStatus::ok);
    CHECK(check_port_ea(0x80) == PortStatus::ok);

    CHECK(sext29(0) == 0);
    CHECK(sext29(0x0FFFFFFF) == 268435455);
    CHECK(sext29(0x10000000) == -268435456);
    CHECK(sext29(0x1FFFFFFF) == -1);
    CHECK(sext29(0xE0000005) == 5); /* bits 31:29 are not part of the count */
    CHECK(sext29(0x9FFFFFFE) == -2);
}

void test_submit()
{
    PortWords w{};
    CHECK(w.try_submit() == PortStatus::ok);
    CHECK(w == W(0, 0, 1, 0));

    /* A held claim refuses the push and changes nothing. */
    w = W(7, 9, 4, 0x01000000);
    CHECK(w.try_submit() == PortStatus::busy);
    CHECK(w == W(7, 9, 4, 0x01000000));
    /* Any non-zero claim byte counts; bytes 1..3 of the word do not. */
    w = W(0, 0, 4, 0x80000000);
    CHECK(w.try_submit() == PortStatus::busy);
    w = W(0, 0, 4, 0x00FFFFFF);
    CHECK(w.try_submit() == PortStatus::ok);
    CHECK(w == W(0, 0, 5, 0x00FFFFFF));

    /* Saturation: 0x0FFFFFFE is the last B that may still grow. */
    w = W(0, 0, 0x0FFFFFFD, 0);
    CHECK(w.try_submit() == PortStatus::ok);
    CHECK(w.submitted == 0x0FFFFFFE);
    CHECK(w.try_submit() == PortStatus::ok);
    CHECK(w.submitted == 0x0FFFFFFF);
    CHECK(w.try_submit() == PortStatus::busy);
    CHECK(w == W(0, 0, 0x0FFFFFFF, 0));
    w = W(0, 0, 0xFFFFFFFF, 0);
    CHECK(w.try_submit() == PortStatus::busy);
    CHECK(w.submitted == 0xFFFFFFFF);
}

void test_rollback()
{
    PortWords w = W(3, 0, 5, 0);
    CHECK(w.rollback_submit() == PortStatus::ok);
    CHECK(w == W(3, 0, 4, 0));

    w = W(3, 0, 5, 0x01000000);
    CHECK(w.rollback_submit() == PortStatus::busy);
    CHECK(w == W(3, 0, 5, 0x01000000));

    /* The firmware has no underflow guard: a rollback with nothing to undo
     * leaves B at 0xFFFFFFFF. */
    w = PortWords{};
    CHECK(w.rollback_submit() == PortStatus::ok);
    CHECK(w.submitted == 0xFFFFFFFF);

    /* The whole push: submit, enqueue, rollback on failure. */
    LocalPort p = local({});
    int enqueues = 0;
    CHECK(p.push_accounted([&] { ++enqueues; return 0u; }) == 0);
    CHECK(enqueues == 1 && p.line().words.submitted == 1);

    CHECK(p.push_accounted([&] { ++enqueues; return kRingFull; }) == kRingFull);
    CHECK(enqueues == 2 && p.line().words.submitted == 1); /* rolled back */

    /* Rollback meets a Sync's claim: BUSY replaces the enqueue error and B keeps
     * the failed push's increment (0x02004BAC -> 0x02004C10). */
    CHECK(p.push_accounted([&] {
        p.line().words.claim = 0x01000000;
        return kRingFull;
    }) == kBusy);
    CHECK(p.line().words.submitted == 2);
    CHECK(p.line().words.claim == 0x01000000);

    /* A held claim refuses before the enqueue is attempted. */
    enqueues = 0;
    CHECK(p.push_accounted([&] { ++enqueues; return 0u; }) == kBusy);
    CHECK(enqueues == 0 && p.line().words.submitted == 2);

    /* Saturated: BUSY, no enqueue. */
    p = local(W(0, 0, 0x0FFFFFFF, 0));
    CHECK(p.push_accounted([&] { ++enqueues; return 0u; }) == kBusy);
    CHECK(enqueues == 0 && p.line().words.submitted == 0x0FFFFFFF);
}

/* The PM's credit against the SPU-operation formulation of the same word. */
void test_credit()
{
    PortWords w{};
    CHECK(w.credit_completion() == Wakeup{});
    CHECK(w.completion == 1);
    w.credit_completion();
    w.credit_completion();
    CHECK(w.completion == 3);
    CHECK(w == W(3, 0, 0, 0)); /* nothing else of the line moves */

    /* Bits 31:29 are kept (the traced 0x80000005 -> 0x80000006 case, A != -1). */
    w = W(0x80000005, 11, 2, 0);
    CHECK(w.credit_completion() == Wakeup{});
    CHECK(w.completion == 0x80000006);
    w = W(0x20000007, 0, 0, 0);
    (void)w.credit_completion();
    CHECK(w.completion == 0x20000008);
    w = W(0x60000001, 0, 0, 0);
    (void)w.credit_completion();
    CHECK(w.completion == 0x60000002);

    /* mod 2^29: the count wraps, the waiter bits do not move. */
    w = W(0x1FFFFFFF, 0, 0, 0); /* -1, no waiter */
    CHECK(w.credit_completion() == Wakeup{});
    CHECK(w.completion == 0);
    w = W(0xE0000000 | 0x0FFFFFFF, 0, 0, 0); /* +2^28-1 -> -2^28, not -1 */
    CHECK(w.credit_completion() == Wakeup{});
    CHECK(w.completion == (0xE0000000 | 0x10000000));
    CHECK(sext29(w.completion) == -268435456);

    /* A registered waiter whose A reaches 0: bit 31 is cleared, bits 30:29 kept
     * and the release is due with the kind and the id at +0x04. */
    w = W(0x80000000 | (2u << 29 & 0x60000000) | 0x1FFFFFFF, 0x1234, 0, 0);
    const Wakeup wake = w.credit_completion();
    CHECK(wake.due && wake.kind == 2 && wake.id == 0x1234);
    CHECK(w.completion == 0x40000000);
    w = W(0x80000000 | 0x1FFFFFFE, 5, 0, 0); /* -2: still waiting */
    CHECK(w.credit_completion() == Wakeup{});
    CHECK(w.completion == (0x80000000 | 0x1FFFFFFF));
    w = W(0xE0000000 | 0x1FFFFFFF, 6, 0, 0); /* kind 3 is reported as 3 */
    CHECK(w.credit_completion() == (Wakeup{true, 3, 6}));
    /* The same -1 without bit 31 is nobody's wake. */
    w = W(0x40000000 | 0x1FFFFFFF, 6, 0, 0);
    CHECK(w.credit_completion() == Wakeup{});
    CHECK(w.completion == 0x40000000);

    /* Against the PM's operation sequence, over a grid and random words. */
    std::mt19937 rng(0xC0FFEE);
    std::vector<std::uint32_t> words;
    const std::uint32_t fields[] = {0,          1,          2,          0x0000FFFF, 0x0FFFFFFE,
                                    0x0FFFFFFF, 0x10000000, 0x10000001, 0x1FFFFFFE, 0x1FFFFFFF};
    for (std::uint32_t top = 0; top < 8; ++top)
        for (const std::uint32_t f : fields) words.push_back((top << 29) | f);
    for (int i = 0; i < 200000; ++i) words.push_back(rng());
    for (const std::uint32_t word : words) {
        PortWords p{word, 0xABCD, 0, 0};
        const Wakeup k = p.credit_completion();
        CHECK(p.completion == jq2h_pm_credit_word(word));
        const bool due = (word >> 31) != 0 && sext29(word) == -1;
        CHECK(k.due == due);
        if (k.due) CHECK(k.kind == ((p.completion >> 29) & 3) && k.id == 0xABCD);
        CHECK(p.waiter == 0xABCD);
    }
}

void test_sync_codes()
{
    NoWait none;

    /* Only modes 0 and 4 are valid: PPU 0x020045E8 computes mode & ~4 and
     * returns INVAL for anything left. INVAL comes before anything is claimed,
     * even on a claimed port, and leaves A, B and the claim byte as they were,
     * whether or not A >= B (so a mode that is "mode 4 plus a bit" neither
     * polls nor blocks). */
    for (const std::uint32_t mode : {1u, 2u, 3u, 5u, 6u, 7u, 8u, 9u, 0xCu, 0x10u, 0x80000000u, 0xFFFFFFF8u,
                                     0xFFFFFFFFu}) {
        LocalPort p = local(W(5, 0, 5, 0));
        CHECK(u32(p.sync(mode, none)) == kInval);
        CHECK(p.line().words == W(5, 0, 5, 0));
        p = local(W(1, 0x55, 3, 0)); /* A < B: would poll or block in a valid mode */
        ScriptedWait would_block(&p); /* answers a wait, so a wrongly accepted mode fails instead of hanging */
        CHECK(u32(p.sync(mode, would_block)) == kInval);
        CHECK(would_block.acquires == 0);
        CHECK(p.line().words == W(1, 0x55, 3, 0));
        p = local(W(5, 0, 5, 0x01000000));
        CHECK(u32(p.sync(mode, none)) == kInval);
        CHECK(p.line().words == W(5, 0, 5, 0x01000000));
    }
    CHECK(none.calls == 0);

    /* Both valid modes complete when A >= B: A := A - B, B := 0, claim cleared.
     * Mode 4 polls and mode 0 would block; neither waits here. */
    for (const std::uint32_t mode : {0u, 4u}) {
        LocalPort p = local(W(5, 0x77, 3, 0x00AABBCC));
        CHECK(p.sync(mode, none) == PortStatus::ok);
        CHECK(p.line().words == W(2, 0x77, 0, 0x00AABBCC)); /* claim byte only */
        p = local(W(4, 0, 4, 0));
        CHECK(p.sync(mode, none) == PortStatus::ok);
        CHECK(p.line().words == W(0, 0, 0, 0));
        p = local(W(0, 0, 0, 0)); /* nothing outstanding */
        CHECK(p.sync(mode, none) == PortStatus::ok);
        CHECK(p.line().words == W(0, 0, 0, 0));
    }
    CHECK(none.calls == 0);

    /* Bits 31:29 of A survive a successful Sync. */
    {
        LocalPort p = local(W(0x20000000 | 5, 4, 3, 0));
        CHECK(p.sync(0, none) == PortStatus::ok);
        CHECK(p.line().words == W(0x20000002, 4, 0, 0));
        p = local(W(0x60000000 | 3, 4, 3, 0));
        CHECK(p.sync(4, none) == PortStatus::ok);
        CHECK(p.line().words == W(0x60000000, 4, 0, 0));
    }

    /* Mode 4: AGAIN while A < B, counters and waiter word as they were, claim
     * released. */
    for (const std::uint32_t mode : {4u}) {
        LocalPort p = local(W(1, 0x55, 3, 0));
        CHECK(u32(p.sync(mode, none)) == kAgain);
        CHECK(p.line().words == W(1, 0x55, 3, 0));
    }
    CHECK(none.calls == 0);

    /* A wrapped past +2^28 reads negative, so it is behind any B >= 0. */
    {
        LocalPort p = local(W(0x10000000, 0, 1, 0));
        CHECK(u32(p.sync(4, none)) == kAgain);
        CHECK(p.line().words == W(0x10000000, 0, 1, 0));
        p = local(W(0x1FFFFFFF, 0, 0, 0)); /* -1 against B = 0 */
        CHECK(u32(p.sync(4, none)) == kAgain);
        p = local(W(0x0FFFFFFF, 0, 0x0FFFFFFF, 0)); /* the largest equal pair */
        CHECK(p.sync(4, none) == PortStatus::ok);
        CHECK(p.line().words == W(0, 0, 0, 0));
        p = local(W(0x0FFFFFFE, 0, 0x0FFFFFFF, 0));
        CHECK(u32(p.sync(4, none)) == kAgain);
        /* A word whose upper bits alias a smaller count: only bits 28:0 count. */
        p = local(W(0x60000000 | 7, 0, 7, 0));
        CHECK(p.sync(4, none) == PortStatus::ok);
        CHECK(p.line().words == W(0x60000000, 0, 0, 0));
    }

    /* A held claim: BUSY, and the owner's claim is not released by the loser. */
    for (const std::uint32_t mode : {0u, 4u}) {
        LocalPort p = local(W(5, 0, 5, 0x01000000));
        CHECK(u32(p.sync(mode, none)) == kBusy);
        CHECK(p.line().words == W(5, 0, 5, 0x01000000));
    }
    CHECK(none.calls == 0);

    /* A stale waiter bit owns A: BUSY, nothing changed, claim released. */
    for (const std::uint32_t mode : {0u, 4u}) {
        LocalPort p = local(W(0x80000000 | 0x1FFFFFFF, 3, 2, 0));
        CHECK(u32(p.sync(mode, none)) == kBusy);
        CHECK(p.line().words == W(0x80000000 | 0x1FFFFFFF, 3, 2, 0));
    }

    /* B above the saturation bound: BAD_ARG after the claim, claim released. */
    for (const std::uint32_t mode : {0u, 4u}) {
        LocalPort p = local(W(0, 0, 0x10000000, 0));
        CHECK(u32(p.sync(mode, none)) == kInval);
        CHECK(p.line().words == W(0, 0, 0x10000000, 0));
    }
    CHECK(none.calls == 0);
}

void test_sync_wait()
{
    /* Blocking: the Sync registers (A := A - B, bit 31, kind, id), the PM's
     * credits wake it, and the state it leaves is exactly the firmware's. */
    {
        LocalPort p = local(W(1, 0x33, 3, 0));
        ScriptedWait env{&p};
        CHECK(p.sync(0, env) == PortStatus::ok);
        CHECK(env.acquires == 1 && env.blocks == 1 && env.releases == 1);
        CHECK(env.seen.size() == 1);
        /* A - B = -2 as a 29-bit field, bit 31, kind 2 (event flag), id at +0x04,
         * B untouched, claim held while it waits. */
        CHECK(env.seen[0] == W(0x80000000 | 0x40000000 | 0x1FFFFFFE, 7, 3, 0x01000000));
        CHECK(env.credits_in_block == 2); /* -2 -> -1 -> 0 */
        CHECK(env.last_wake == (Wakeup{true, 2, 7}));
        /* Woken at A == 0: bit 31 gone, the kind bits and the id stay, B is 0 and
         * the claim is released. */
        CHECK(p.line().words == W(0x40000000, 7, 0, 0));
    }
    /* A fiber waiter: kind 0, the id is its EA. */
    {
        LocalPort p = local(W(0, 0, 2, 0));
        ScriptedWait env{&p};
        env.issue = {WaitKind::fiber, 0x30001000};
        CHECK(p.sync(0, env) == PortStatus::ok);
        CHECK(env.seen[0] == W(0x80000000 | 0x1FFFFFFE, 0x30001000, 2, 0x01000000));
        CHECK(env.last_wake == (Wakeup{true, 0, 0x30001000}));
        CHECK(p.line().words == W(0, 0x30001000, 0, 0));
    }
    /* A spurious return from block(): the Sync blocks again; only the PM's wake
     * (bit 31 cleared) ends the wait. */
    {
        LocalPort p = local(W(0, 0, 1, 0));
        ScriptedWait env{&p};
        env.spurious = 2;
        CHECK(p.sync(0, env) == PortStatus::ok);
        CHECK(env.blocks == 3 && env.releases == 1 && env.credits_in_block == 1);
        CHECK(p.line().words == W(0x40000000, 7, 0, 0));
    }
    /* Credits that arrive while the identity is being acquired: the next look
     * completes without registering, the waiter word is untouched, and the
     * identity is still released. */
    {
        LocalPort p = local(W(1, 0x33, 3, 0));
        ScriptedWait env{&p};
        env.credits_in_acquire = 2;
        CHECK(p.sync(0, env) == PortStatus::ok);
        CHECK(env.acquires == 1 && env.blocks == 0 && env.releases == 1);
        CHECK(p.line().words == W(0, 0x33, 0, 0));
    }
    /* Poll mode never reaches the environment. */
    {
        LocalPort p = local(W(1, 0, 3, 0));
        ScriptedWait env{&p};
        CHECK(u32(p.sync(4, env)) == kAgain);
        CHECK(env.acquires == 0 && env.blocks == 0 && env.releases == 0);
    }
    /* A Sync that does not need to wait never reaches it either. */
    {
        LocalPort p = local(W(3, 0, 3, 0));
        ScriptedWait env{&p};
        CHECK(p.sync(0, env) == PortStatus::ok);
        CHECK(env.acquires == 0 && env.blocks == 0 && env.releases == 0);
    }
    /* Two Syncs in a row on one port (a stale kind/id after the first must not
     * make the second BUSY). */
    {
        LocalPort p = local(W(0, 0, 2, 0));
        ScriptedWait env{&p};
        CHECK(p.sync(0, env) == PortStatus::ok);
        CHECK(p.line().words == W(0x40000000, 7, 0, 0));
        for (int i = 0; i < 3; ++i) CHECK(p.try_submit() == PortStatus::ok);
        CHECK(p.credit_completion() == Wakeup{});
        CHECK(p.credit_completion() == Wakeup{});
        CHECK(p.credit_completion() == Wakeup{});
        CHECK(p.sync(4, env) == PortStatus::ok);
        CHECK(p.line().words == W(0x40000000, 7, 0, 0));
    }
}

void test_model()
{
    test_codes_and_guards();
    test_submit();
    test_rollback();
    test_credit();
    test_sync_codes();
    test_sync_wait();
}

/* ---------------------------------------------------------------------------
 * guest binding
 * ------------------------------------------------------------------------- */

void fill_line(std::uint32_t ea, std::uint8_t pattern)
{
    for (std::uint32_t i = 0; i < 128; ++i) jq2h_put8(ea + i, pattern);
}

void put_words(std::uint32_t ea, std::uint32_t a, std::uint32_t waiter, std::uint32_t b, std::uint32_t claim)
{
    jq2h_put32(ea + 0x00, a);
    jq2h_put32(ea + 0x04, waiter);
    jq2h_put32(ea + 0x10, b);
    jq2h_put32(ea + 0x14, claim);
}

PortWords get_words(std::uint32_t ea)
{
    return W(jq2h_get32(ea + 0x00), jq2h_get32(ea + 0x04), jq2h_get32(ea + 0x10), jq2h_get32(ea + 0x14));
}

bool others_untouched(std::uint32_t ea, std::uint8_t pattern)
{
    for (std::uint32_t i = 0; i < 128; ++i) {
        if (i < 8 || (i >= 0x10 && i < 0x18)) continue;
        if (jq2h_get8(ea + i) != pattern) return false;
    }
    return true;
}

void test_guest_layout()
{
    PortStatus err = PortStatus::ok;
    CHECK(!bind_port(0, &err) && err == PortStatus::null_pointer);
    CHECK(!bind_port(kLine + 0x40, &err) && err == PortStatus::misaligned);
    CHECK(!bind_port(kLine + 4, &err) && err == PortStatus::misaligned);
    CHECK(!bind_port(0));

    fill_line(kLine, 0x5A);
    put_words(kLine, 0, 0, 0, 0);
    std::optional<Port> port = bind_port(kLine, &err);
    CHECK(port && err == PortStatus::ok);
    if (!port) return;

    /* Big-endian bytes: B = 1 is 00 00 00 01 at +0x10. */
    CHECK(port->try_submit() == PortStatus::ok);
    CHECK(jq2h_get8(kLine + 0x10) == 0 && jq2h_get8(kLine + 0x13) == 1);
    CHECK(get_words(kLine) == W(0, 0, 1, 0));
    CHECK(port->try_submit() == PortStatus::ok);
    CHECK(get_words(kLine) == W(0, 0, 2, 0));

    /* The PM's credit lands in the high word of the doubleword at +0x00. */
    CHECK(!port->credit_completion().due);
    CHECK(jq2h_get8(kLine + 0x00) == 0 && jq2h_get8(kLine + 0x03) == 1);
    CHECK(get_words(kLine) == W(1, 0, 2, 0));

    /* A rollback and a push through the whole path. */
    CHECK(port->rollback_submit() == PortStatus::ok);
    CHECK(get_words(kLine) == W(1, 0, 1, 0));
    CHECK(port->push_accounted([] { return kRingFull; }) == kRingFull);
    CHECK(get_words(kLine) == W(1, 0, 1, 0));

    /* Poll Sync, claim byte in byte 0 of +0x14 only. */
    jq2h_put8(kLine + 0x15, 0x11);
    jq2h_put8(kLine + 0x17, 0x22);
    NoWait none;
    CHECK(port->sync(4, none) == PortStatus::ok);
    CHECK(get_words(kLine) == W(0, 0, 0, 0x00110022));
    jq2h_put8(kLine + 0x14, 1);
    CHECK(port->try_submit() == PortStatus::busy);
    CHECK(u32(port->sync(0, none)) == kBusy);
    CHECK(jq2h_get8(kLine + 0x14) == 1); /* the owner's claim */
    jq2h_put8(kLine + 0x14, 0);
    CHECK(others_untouched(kLine, 0x5A));

    /* A registered wait through the guest line, then its wake by the PM's credit
     * (the second line the test owns: a different EA, a fresh bind). */
    constexpr std::uint32_t kWaitLine = kLine + 0x1000;
    fill_line(kWaitLine, 0x6B);
    put_words(kWaitLine, 0, 0, 0, 0);
    std::optional<Port> wp = bind_port(kWaitLine);
    struct GuestWait {
        Port* port;
        explicit GuestWait(Port* p) : port(p) {}
        std::uint32_t seen_a = 0, seen_waiter = 0, seen_claim = 0;
        Wakeup wake;
        Waiter acquire() { return {WaitKind::event_flag, 0x21}; }
        void block(const Waiter&)
        {
            seen_a = jq2h_get32(kWaitLine);
            seen_waiter = jq2h_get32(kWaitLine + 4);
            seen_claim = jq2h_get32(kWaitLine + 0x14);
            do wake = port->credit_completion(); while (!wake.due);
        }
        void release(const Waiter&) {}
    } env{&*wp};
    CHECK(wp->try_submit() == PortStatus::ok);
    CHECK(wp->try_submit() == PortStatus::ok);
    CHECK(wp->sync(0, env) == PortStatus::ok);
    CHECK(env.seen_a == (0x80000000u | 0x40000000u | 0x1FFFFFFEu) && env.seen_waiter == 0x21 &&
          env.seen_claim == 0x01000000);
    CHECK(env.wake == (Wakeup{true, 2, 0x21}));
    CHECK(get_words(kWaitLine) == W(0x40000000, 0x21, 0, 0));
    CHECK(others_untouched(kWaitLine, 0x6B));
}

void test_guest_reservations()
{
    const std::uint32_t ea = kLine + 0x2000;
    put_words(ea, 0, 0, 0, 0);
    Port port = *bind_port(ea);
    jq2h_ppu* cpu = jq2h_ppu_new();
    jq2h_spu* spu = jq2h_spu_new(0);
    NoWait none;

    /* PPU lwarx on B, then a host push: the stwcx. must fail. */
    (void)jq2h_lwarx32(cpu, ea + 0x10);
    CHECK(port.try_submit() == PortStatus::ok);
    CHECK(!jq2h_stcx32(cpu, ea + 0x10, 99));
    CHECK(get_words(ea).submitted == 1);

    /* PPU ldarx on the credit doubleword, then the PM's credit (host side). */
    (void)jq2h_ldarx64(cpu, ea + 0x00);
    (void)port.credit_completion();
    CHECK(!jq2h_stcx64(cpu, ea + 0x00, 0));
    CHECK(get_words(ea).completion == 1);

    /* A reservation on another word of the same 128-byte line dies too (the
     * model's granule is the line). */
    (void)jq2h_lwarx32(cpu, ea + 0x40);
    (void)port.credit_completion();
    CHECK(!jq2h_stcx32(cpu, ea + 0x40, 5));

    /* A refused push stores nothing and so kills nothing. */
    jq2h_put8(ea + 0x14, 1);
    (void)jq2h_lwarx32(cpu, ea + 0x40);
    CHECK(port.try_submit() == PortStatus::busy);
    CHECK(u32(port.sync(4, none)) == kBusy);
    CHECK(jq2h_stcx32(cpu, ea + 0x40, 5));
    jq2h_put8(ea + 0x14, 0);

    /* A host store kills an SPU reservation: PUTLLC fails, the lost-reservation
     * event is raised, the retry (fresh GETLLAR) succeeds. */
    jq2h_getllar(spu, ea);
    CHECK(port.try_submit() == PortStatus::ok);
    CHECK(jq2h_lr_raised(spu));
    CHECK(!jq2h_putllc(spu, ea));
    jq2h_getllar(spu, ea);
    CHECK(jq2h_putllc(spu, ea));

    /* The SPU's own PUTLLC kills the host's view: the next host op sees it. */
    jq2h_getllar(spu, ea);
    jq2h_stage_put32(spu, 0, jq2h_stage_get32(spu, 0) + 1);
    CHECK(jq2h_putllc(spu, ea));
    CHECK(get_words(ea).completion == 3); /* the host's two credits + the SPU's */
    CHECK(get_words(ea).submitted == 2);

    /* A BUSY (no store) leaves the SPU's reservation alone. */
    jq2h_put8(ea + 0x14, 1);
    jq2h_getllar(spu, ea);
    CHECK(port.try_submit() == PortStatus::busy);
    CHECK(u32(port.sync(0, none)) == kBusy);
    CHECK(jq2h_putllc(spu, ea));
    jq2h_put8(ea + 0x14, 0);

    /* A Sync that stores kills it. */
    jq2h_getllar(spu, ea);
    CHECK(port.sync(4, none) == PortStatus::ok); /* A = 3 >= B = 2 */
    CHECK(!jq2h_putllc(spu, ea));
    CHECK(get_words(ea) == W(1, 0, 0, 0));

    /* A neighbouring line is not affected. */
    const std::uint32_t other = ea + 128;
    put_words(other, 0, 0, 0, 0);
    jq2h_getllar(spu, other);
    CHECK(port.try_submit() == PortStatus::ok);
    CHECK(jq2h_putllc(spu, other));

    jq2h_spu_free(spu);
    jq2h_ppu_free(cpu);
}

void test_guest()
{
    jq2h_guest_init(kGuestBytes);
    test_guest_layout();
    test_guest_reservations();
    jq2h_guest_free();
}

/* ---------------------------------------------------------------------------
 * claim: a push meets a Sync that owns the port
 * ------------------------------------------------------------------------- */

void test_claim_blocks_push()
{
    jq2h_guest_init(kGuestBytes);
    const std::uint32_t ea = kLine + 0x4000;
    put_words(ea, 0, 0, 0, 0);
    Port pusher = *bind_port(ea);
    jq2h_spu* spu = jq2h_spu_new(0);

    CHECK(pusher.try_submit() == PortStatus::ok);
    CHECK(pusher.try_submit() == PortStatus::ok);

    struct Latch {
        std::atomic<bool> blocked{false}, go{false};
        Waiter acquire() { return {WaitKind::event_flag, 9}; }
        void block(const Waiter&)
        {
            blocked.store(true, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        }
        void release(const Waiter&) {}
    } latch;

    PortStatus sync_status = PortStatus::busy;
    std::thread syncer([&] {
        Port mine = *bind_port(ea);
        sync_status = mine.sync(0, latch); /* A = 0 < B = 2: registers and blocks */
    });
    while (!latch.blocked.load(std::memory_order_acquire)) std::this_thread::yield();

    /* The Sync owns the port: pushes are refused and B stays exact. */
    CHECK(pusher.try_submit() == PortStatus::busy);
    CHECK(pusher.try_submit() == PortStatus::busy);
    int enqueues = 0;
    CHECK(pusher.push_accounted([&] { ++enqueues; return 0u; }) == kBusy);
    CHECK(enqueues == 0);
    NoWait none;
    CHECK(u32(pusher.sync(4, none)) == kBusy); /* a second Sync is refused too */
    CHECK(get_words(ea).submitted == 2);
    CHECK(jq2h_get8(ea + 0x14) == 1); /* the owner's claim survives the losers */

    /* The PM completes the two jobs through the real SPU path; the second credit
     * brings A to 0 and is the wake. */
    std::uint64_t retries = 0;
    CHECK(jq2h_spu_credit(spu, ea, &retries) == 0);
    CHECK(jq2h_spu_credit(spu, ea, &retries) == 1);
    latch.go.store(true, std::memory_order_release);
    syncer.join();
    CHECK(sync_status == PortStatus::ok);
    CHECK(get_words(ea) == W(0x40000000, 9, 0, 0));

    /* The port is free again, and counts from zero. */
    CHECK(pusher.try_submit() == PortStatus::ok);
    CHECK(get_words(ea).submitted == 1);

    jq2h_spu_free(spu);
    jq2h_guest_free();
}

/* ---------------------------------------------------------------------------
 * race
 * ------------------------------------------------------------------------- */

struct RaceStats {
    std::uint64_t push_busy = 0, spu_retries = 0, ppu_retries = 0;
    std::uint64_t sync_ok = 0, sync_again = 0, sync_busy = 0, sync_blocked = 0, sync_other = 0;
};

int run_race(std::uint64_t n)
{
    jq2h_guest_init(kGuestBytes);
    const std::uint32_t ea = kLine + 0x6000;
    put_words(ea, 0, 0, 0, 0);
    const std::uint32_t word_ea = ea + 0x40, dword_ea = ea + 0x48; /* the PPU's, in the port line */

    std::atomic<std::uint64_t> pushed{0}, credited{0};
    std::atomic<bool> wake{false}, producers_done{false};
    RaceStats st;
    std::uint64_t spu_retries = 0;

    std::thread spu_thread([&] {
        jq2h_spu* spu = jq2h_spu_new(0);
        for (std::uint64_t i = 0; i < n; ++i) {
            while (credited.load(std::memory_order_relaxed) == pushed.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (jq2h_spu_credit(spu, ea, &spu_retries)) wake.store(true, std::memory_order_release);
            credited.fetch_add(1, std::memory_order_release);
        }
        jq2h_spu_free(spu);
    });

    std::thread push_thread([&] {
        Port port = *bind_port(ea);
        for (std::uint64_t i = 0; i < n; ++i) {
            while (port.try_submit() != PortStatus::ok) {
                ++st.push_busy;
                std::this_thread::yield();
            }
            pushed.fetch_add(1, std::memory_order_release);
        }
    });

    std::atomic<std::uint64_t> ppu_done{0};
    std::thread ppu_thread([&] {
        jq2h_ppu* cpu = jq2h_ppu_new();
        const std::uint64_t m = n < 100000 ? n : 100000;
        for (std::uint64_t i = 0; i < m; ++i) {
            std::uint32_t v;
            do { v = jq2h_lwarx32(cpu, word_ea); ++st.ppu_retries; } while (!jq2h_stcx32(cpu, word_ea, v + 1));
            std::uint64_t d;
            do { d = jq2h_ldarx64(cpu, dword_ea); } while (!jq2h_stcx64(cpu, dword_ea, d + 1));
            ppu_done.fetch_add(1, std::memory_order_relaxed);
        }
        jq2h_ppu_free(cpu);
    });

    struct EventWait {
        std::atomic<bool>* wake;
        RaceStats* st;
        Waiter acquire() { return {WaitKind::event_flag, 5}; }
        void block(const Waiter&)
        {
            ++st->sync_blocked;
            while (!wake->exchange(false, std::memory_order_acq_rel)) std::this_thread::yield();
        }
        void release(const Waiter&) {}
    };
    std::thread sync_thread([&] {
        Port port = *bind_port(ea);
        EventWait env{&wake, &st};
        std::uint64_t round = 0;
        while (!producers_done.load(std::memory_order_acquire)) {
            const std::uint32_t mode = (round++ % 3 == 0) ? 4u : 0u;
            switch (port.sync(mode, env)) {
            case PortStatus::ok: ++st.sync_ok; break;
            case PortStatus::again: ++st.sync_again; break;
            case PortStatus::busy: ++st.sync_busy; break;
            default: ++st.sync_other; break;
            }
            std::this_thread::yield();
        }
    });
    /* A lost credit or a lost wake hangs a thread; a run that stops advancing
     * for 20 s fails with the counters instead of waiting for the test timeout. */
    std::atomic<bool> finished{false};
    std::thread watchdog([&] {
        std::uint64_t last = 0;
        int idle_ticks = 0;
        while (!finished.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const std::uint64_t now = pushed.load() + credited.load() + ppu_done.load();
            idle_ticks = now == last ? idle_ticks + 1 : 0;
            last = now;
            if (idle_ticks >= 400) {
                std::fprintf(stderr,
                             "FAIL: jq2 port race made no progress for 20 s: pushed=%llu credited=%llu ppu=%llu of n=%llu\n",
                             (unsigned long long)pushed.load(), (unsigned long long)credited.load(),
                             (unsigned long long)ppu_done.load(), (unsigned long long)n);
                std::_Exit(1);
            }
        }
    });

    push_thread.join();
    spu_thread.join();
    ppu_thread.join();
    producers_done.store(true, std::memory_order_release);
    sync_thread.join();
    finished.store(true, std::memory_order_release);
    watchdog.join();

    /* Everything pushed has been credited: one more Sync drains the port. */
    Port port = *bind_port(ea);
    NoWait none;
    const PortStatus last = port.sync(4, none);
    const PortWords w = get_words(ea);
    const std::uint64_t ppu_m = n < 100000 ? n : 100000;

    std::printf("jq2 port race n=%llu: push busy=%llu spu retries=%llu ppu retries=%llu sync ok=%llu again=%llu "
                "busy=%llu blocked=%llu\n",
                (unsigned long long)n, (unsigned long long)st.push_busy, (unsigned long long)spu_retries,
                (unsigned long long)st.ppu_retries, (unsigned long long)st.sync_ok,
                (unsigned long long)st.sync_again, (unsigned long long)st.sync_busy,
                (unsigned long long)st.sync_blocked);

    CHECK(pushed.load() == n && credited.load() == n);
    CHECK(st.sync_other == 0); /* contract C2: never any other error */
    CHECK(last == PortStatus::ok);
    /* A has been lowered by exactly the B of every Sync that succeeded, so with
     * all n pushes credited and reaped the count is 0, B is 0 and the claim is
     * free; a lost credit hangs a blocking Sync, a lost push leaves A > 0, a
     * lost Sync reset leaves B > 0. */
    CHECK((w.completion & kCreditMask) == 0);
    CHECK(!w.waiting());
    CHECK(w.submitted == 0);
    CHECK(!w.claim_held());
    /* The PPU's counters, on other words of the same line, lost nothing. */
    CHECK(jq2h_get32(word_ea) == ppu_m);
    std::uint32_t hi = jq2h_get32(dword_ea), lo = jq2h_get32(dword_ea + 4);
    CHECK(hi == 0 && lo == ppu_m);
    CHECK(ppu_done.load() == ppu_m);

    jq2h_guest_free();
    return g_failures.load() == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "model";
    if (!std::strcmp(mode, "model")) {
        test_model();
    } else if (!std::strcmp(mode, "guest")) {
        test_guest();
    } else if (!std::strcmp(mode, "claim")) {
        test_claim_blocks_push();
    } else if (!std::strcmp(mode, "race")) {
        const std::uint64_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 100000;
        return run_race(n);
    } else {
        std::fprintf(stderr, "usage: test_jq2_port model | guest | claim | race N\n");
        return 2;
    }
    if (g_failures.load() != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures.load());
        return 1;
    }
    std::printf("jq2 port %s: all checks passed\n", mode);
    return 0;
}

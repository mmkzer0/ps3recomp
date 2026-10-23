/*
 * ps3recomp - SPURS JobQueue2 port line, guest binding. See port.hpp.
 *
 * The only code here that touches guest memory: GuestPortLine::update_raw reads
 * the two doublewords that hold the accounting words, runs the transition, and
 * writes back the doubleword it changed, all inside one resv_host_rmw() so a
 * PPU lwarx/ldarx or SPU GETLLAR reservation on the line dies and a concurrent
 * conditional store waits for the section (reservation.h, LOCK ORDER).
 */

#include "jq2/port.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "memory/reservation.h"
#include "ps3emu/endian.h"

namespace ps3recomp::spurs::jq2 {

namespace {

struct Call {
    bool (*fn)(void*, PortWords&);
    void* user;
};

/* Guest memory is big-endian: the doubleword at +0x00 holds A in its high word
 * and the waiter word in its low word; the doubleword at +0x10 holds B and the
 * claim word. */
inline std::uint64_t pack(std::uint32_t hi, std::uint32_t lo)
{
    return ps3_bswap64((std::uint64_t{hi} << 32) | lo);
}
inline std::uint32_t hi_of(std::uint64_t raw) { return static_cast<std::uint32_t>(ps3_bswap64(raw) >> 32); }
inline std::uint32_t lo_of(std::uint64_t raw) { return static_cast<std::uint32_t>(ps3_bswap64(raw)); }

int rmw(std::uint8_t* line, void* user)
{
    const Call& call = *static_cast<const Call*>(user);
    auto head = std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(line + kPortCompletionOffset));
    auto tail = std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(line + kPortSubmittedOffset));

    for (;;) {
        std::uint64_t head_raw = head.load(std::memory_order_acquire);
        std::uint64_t tail_raw = tail.load(std::memory_order_acquire);
        PortWords w{hi_of(head_raw), lo_of(head_raw), hi_of(tail_raw), lo_of(tail_raw)};
        if (!call.fn(call.user, w)) return 0;

        const std::uint64_t new_head = pack(w.completion, w.waiter);
        const std::uint64_t new_tail = pack(w.submitted, w.claim);
        if (new_head != head_raw && new_tail != tail_raw) {
            /* No transition changes both doublewords (port.hpp), and two
             * separate stores would not be one update. */
            std::fprintf(stderr, "[jq2-port] a transition changed both accounting doublewords\n");
            std::abort();
        }
        /* The model's lock excludes every reported writer; the compare exchange
         * is the stdcx. against a store it does not see (reservation.h, KNOWN
         * LIMITS). On failure the transition runs again on the new words. */
        if (new_head != head_raw &&
            !head.compare_exchange_strong(head_raw, new_head, std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        if (new_tail != tail_raw &&
            !tail.compare_exchange_strong(tail_raw, new_tail, std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        return 1;
    }
}

}  // namespace

bool GuestPortLine::update_raw(Thunk fn, void* user) const
{
    Call call{fn, user};
    return resv_host_rmw(ea_, rmw, &call) != 0;
}

}  // namespace ps3recomp::spurs::jq2

/*
 * ps3recomp - the HLE call observer (include/ps3emu/hle_observer.h)
 *
 * Calls handlers through the real ps3_hle_register and ps3_hle_call (runtime/ppu/ppu_hle.cpp compiled into
 * this test) and checks what an installed observer sees:
 *
 *   order        on_call, then the handler, then on_return
 *   arguments    r3..r10 as the guest left them at on_call; the return value in r3 at on_return
 *   threads      each guest thread's call is reported on that thread, with its own ctx
 *   off          no observer, or one removed again: the handler still runs, nothing is reported
 *   half         an observer with only on_return (or only on_call) is allowed
 *   replace      installing a second observer replaces the first
 */
#include "ppu_context.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "ps3emu/hle_observer.h"

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size = 0x10000;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ps3_hle_call(uint32_t, ppu_context*);
void ps3_hle_register(uint32_t nid, const char* name, void (*fn)(ppu_context*));
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
uint32_t vm_read32(uint64_t a) { uint32_t v; std::memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
void vm_write32(uint64_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(vm_base + a, &v, 4); }
uint64_t vm_read64(uint64_t a) { uint64_t v; std::memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }
void ppu_dump_guest_stack(ppu_context*, const char*) {}
void ppu_dump_bctrl_ring(uint32_t, const char*) {}
void ppu_guest_callstack(const char*) {}
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0;
}
void vm_write64(uint64_t a, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(vm_base + a, &v, 8); }

static int g_fail;
#define CHECK(c, ...)                                                             \
    do {                                                                          \
        if (!(c)) {                                                               \
            ++g_fail;                                                             \
            std::printf("[FAIL] %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                             \
            std::printf("\n");                                                    \
        }                                                                         \
    } while (0)

constexpr uint32_t kNid = 0x11112222u, kOther = 0x33334444u;
std::atomic<int> g_clock{0};
std::atomic<int> g_handler_runs{0};

void handler(ppu_context* c)
{
    g_handler_runs++;
    c->gpr[3] = 0x80010001u + (c->gpr[3] & 0xFF);   // a return derived from the argument
}

/* The callbacks run on the calling guest thread, concurrently: the fields they record are the fixture's
 * own shared state, so they are written under `mu` (the counters are atomic for the main thread's reads). */
struct Seen {
    std::mutex mu;
    std::atomic<int> calls{0}, returns{0};
    int call_at = -1, return_at = -1, handler_runs_at_call = -1, handler_runs_at_return = -1;
    uint32_t nid = 0, r3 = 0, r4 = 0, r10 = 0, ret = 0;
    uint64_t tid_call = 0, tid_return = 0;
};

static void on_call(void* u, uint32_t nid, const ppu_context* c)
{
    Seen* s = static_cast<Seen*>(u);
    std::lock_guard<std::mutex> lk(s->mu);
    s->calls++;
    s->call_at = g_clock++;
    s->handler_runs_at_call = g_handler_runs;
    s->nid = nid; s->r3 = (uint32_t)c->gpr[3]; s->r4 = (uint32_t)c->gpr[4]; s->r10 = (uint32_t)c->gpr[10];
    s->tid_call = c->thread_id;
}
static void on_return(void* u, uint32_t nid, const ppu_context* c)
{
    Seen* s = static_cast<Seen*>(u);
    (void)nid;
    std::lock_guard<std::mutex> lk(s->mu);
    s->returns++;
    s->return_at = g_clock++;
    s->handler_runs_at_return = g_handler_runs;
    s->ret = (uint32_t)c->gpr[3];
    s->tid_return = c->thread_id;
}

static ppu_context make_ctx(uint64_t tid)
{
    ppu_context c;
    std::memset(&c, 0, sizeof c);
    c.thread_id = tid;
    c.gpr[1] = 0x8000;   // the dispatch saves r2 at r1+0x28
    return c;
}

int main()
{
    vm_base = static_cast<uint8_t*>(calloc(0x10000, 1));
    if (!vm_base) return 2;
    ps3_hle_register(kNid, "observed", handler);
    ps3_hle_register(kOther, "observed_other", handler);

    // No observer: the handler runs, nothing to report.
    ppu_context c0 = make_ctx(1);
    c0.gpr[3] = 5;
    ps3_hle_call(kNid, &c0);
    CHECK((uint32_t)c0.gpr[3] == 0x80010006u && g_handler_runs == 1, "plain dispatch");

    Seen s;
    ps3_hle_observer obs{&s, on_call, on_return};
    ps3_hle_set_observer(&obs);
    ppu_context c = make_ctx(7);
    c.gpr[3] = 0x1234; c.gpr[4] = 0x99; c.gpr[10] = 0xABCD;
    ps3_hle_call(kNid, &c);
    CHECK(s.calls == 1 && s.returns == 1, "one call, one return");
    CHECK(s.call_at < s.return_at, "on_call before on_return");
    CHECK(s.handler_runs_at_call == 1 && s.handler_runs_at_return == 2, "the handler ran between them (%d, %d)",
          s.handler_runs_at_call, s.handler_runs_at_return);
    CHECK(s.nid == kNid && s.r3 == 0x1234 && s.r4 == 0x99 && s.r10 == 0xABCD, "arguments at the call: 0x%X 0x%X 0x%X",
          s.r3, s.r4, s.r10);
    CHECK(s.ret == 0x80010001u + 0x34u && (uint32_t)c.gpr[3] == s.ret, "return value 0x%X", s.ret);
    CHECK(s.tid_call == 7 && s.tid_return == 7, "the calling thread's context");

    // Concurrent guest threads: every call and return is reported, each on its own thread.
    Seen many;
    ps3_hle_observer obs2{&many, on_call, on_return};
    ps3_hle_set_observer(&obs2);                    // replaces the first
    CHECK(s.calls == 1, "a replaced observer sees nothing more");
    std::vector<std::thread> th;
    for (int t = 0; t < 4; ++t)
        th.emplace_back([t] {
            ppu_context tc = make_ctx(100 + t);
            tc.gpr[1] = 0x8000 + 0x100 * (t + 1);
            for (int i = 0; i < 200; ++i) { tc.gpr[3] = i; ps3_hle_call(kOther, &tc); }
        });
    for (auto& x : th) x.join();
    CHECK(many.calls == 800 && many.returns == 800, "800 calls from 4 threads: %d, %d", many.calls.load(), many.returns.load());

    // Only one of the two callbacks.
    Seen half;
    ps3_hle_observer only_ret{&half, nullptr, on_return};
    ps3_hle_set_observer(&only_ret);
    ps3_hle_call(kNid, &c);
    CHECK(half.calls == 0 && half.returns == 1, "on_return alone");
    ps3_hle_observer only_call{&half, on_call, nullptr};
    ps3_hle_set_observer(&only_call);
    ps3_hle_call(kNid, &c);
    CHECK(half.calls == 1 && half.returns == 1, "on_call alone");

    // Removed: the handler still runs, nothing is reported.
    ps3_hle_set_observer(nullptr);
    const int before = g_handler_runs;
    ps3_hle_call(kNid, &c);
    CHECK(g_handler_runs == before + 1 && half.calls == 1 && many.calls == 800, "removed observer");

    std::printf("[hle_observer] %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    free(vm_base);
    return g_fail ? 1 : 0;
}

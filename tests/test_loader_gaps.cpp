/* Runs the loader's gap paths -- unimplemented lv2 syscall, call into an
 * unlifted function, indirect call to no function, a lifted function that
 * executes an instruction the lifter had no lowering for -- and reports whether
 * each one returned. Every one of them must stop (tools/test_loader_gaps.py):
 * a line "RETURNED" means the runtime made something up and carried on.
 *
 * Built against the real ppu_loader.cpp and a lift produced by the real
 * ppu_lifter.py, so the trap the lifter emits is the one that executes.
 *
 *   test_loader_gaps syscall <num>
 *   test_loader_gaps unlifted <addr>
 *   test_loader_gaps indirect <target>
 *   test_loader_gaps call <addr>        a lifted function, through the registry
 *   test_loader_gaps depth <addr>       an indirect call into a function that
 *                                       calls itself until the host dispatch cap
 *   test_loader_gaps opdswap <ctr> <r2> an indirect call whose ctr is not a
 *                                       function and whose r2 is (swapped OPD)
 *   test_loader_gaps guestcall <opd>    an HLE-initiated guest callback by OPD
 *   test_loader_gaps guestcallopd <opd> <code> <toc>
 *                                       writes that OPD, then calls it
 *   test_loader_gaps guestcallct <code> [toc]
 *                                       a callback by an already-resolved code
 *   test_loader_gaps cbstack 0          host-initiated callbacks on concurrent
 *                                       threads (with and without a guest
 *                                       context) and nested: distinct,
 *                                       non-overlapping guest stacks, outer
 *                                       frames and return values intact
 *   test_loader_gaps cbexhaust 0        callbacks nested until a host thread's
 *                                       stack slot is out of room
 *   test_loader_gaps cbbadr1 0          a callback raised under a guest ctx
 *                                       whose r1 is not a stack address
 *   test_loader_gaps cbsmallvm 0        a host-thread callback in a VM that
 *                                       ends below the callback stack window
 *   test_loader_gaps cbleave <below>    a host-slot callback whose r1 is <below>
 *                                       under its slot (0: at it) re-enters:
 *                                       the nested callback must not run
 *   test_loader_gaps hostid 0           two host threads with no guest context
 *                                       run callbacks at once: each has a guest
 *                                       thread id of its own, kept for life and
 *                                       never reused, and an lwmutex one holds
 *                                       blocks the other (ownership by id)
 *   test_loader_gaps nullread <ea> <width>   a guest load at <ea> (1/2/4/8 bytes)
 *   test_loader_gaps nullwrite <ea> <width>  a guest store at <ea>; reports the
 *                                       first byte of memory afterwards
 *   test_loader_gaps nullsweep 0        a null-page store, one 0x1000 above it
 *                                       and one far past it (ppu.null_page=drop)
 */
#include "ppu_recomp.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include "win32_compat.h"
#include "../memory/vm.h"

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

extern "C" {
uint8_t* vm_base;
// A loader-only fixture: nothing below runs guest threads, SPUs or RSX. Any use is a failure.
uint32_t ppu_hle_inject_base = 0x20000000;
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
void cellGcm_ref_on_poll(void) { std::abort(); }
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN, LPVOID, DWORD, DWORD*) { std::abort(); }
BOOL CloseHandle(HANDLE) { std::abort(); }
DWORD GetFileAttributesA(const char*) { return INVALID_FILE_ATTRIBUTES; }
uint64_t ppu_thread_register_main(void) { return 1; }
void (*g_ppu_thread_entry_trampoline)(uint32_t, uint64_t, uint32_t, uint64_t);
uint32_t ps3_hle_count(void) { return 1; }
int lv2_try_syscall(ppu_context*) { return 0; }   // no syscall is implemented here
// The syscall dispatcher's other collaborators; the gap paths below never reach them.
void ppu_gcm_pump(void) {}
void ppu_prof_stamp(void*, unsigned) {}
uint32_t ps3_mmapper_block_size(uint32_t) { std::abort(); }
void ps3_msf(const char*, ...) {}
int64_t sys_mmapper_allocate_address(ppu_context*) { std::abort(); }
int64_t sys_mmapper_search_map_address(uint32_t, uint32_t, uint32_t*) { std::abort(); }
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
void ppu_recomp_register(void);
void ps3_indirect_call(ppu_context* ctx);
void lv2_syscall(ppu_context* ctx);
void ppu_unlifted_stub(uint64_t addr, ppu_context* ctx);
extern uint32_t ppu_vm_size;
// Parenthesised: ppu_recomp.h maps vm_read32 to its inline fast path by a
// function-like macro, and redeclaring that static inline with C linkage is
// an error to GCC.
uint32_t (vm_read32)(uint64_t a);
void vm_write32(uint64_t a, uint32_t v);
uint64_t ppu_guest_call(uint32_t opd, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t ppu_guest_call_ct(uint32_t code, uint32_t toc, uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t, uint64_t);
/* The lwmutex/lwcond handlers register themselves here; hostid calls them by name. */
typedef void (*hle_ctx_fn)(ppu_context*);
static struct { const char* name; hle_ctx_fn fn; } g_hle[16];
static int g_hle_n;
void ps3_hle_register_ctx(uint32_t, const char* name, hle_ctx_fn fn) { g_hle[g_hle_n].name = name; g_hle[g_hle_n++].fn = fn; }
void ps3_lwsync_register(void);
}

extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                  std::fflush(stderr); _exit(1); } } while (0)

/* ---- guest callback stacks ------------------------------------------------
 * `cb` stands in for a lifted callback: it takes a frame below its entry r1 the
 * way a prologue does (gpr[1] -= frame), fills the frame and the 0x70 linkage
 * area at entry with a per-activation byte, optionally meets the other
 * activations (they must all be live at once), optionally re-enters
 * ppu_guest_call_ct, then checks its fill is still intact. Args: r3 = tag,
 * r4 = nested calls still to make, r5 = meet the other threads. */
static constexpr uint32_t CB_ADDR  = 0x30000;
static constexpr uint32_t CB_TOC   = 0x1234;
static constexpr uint32_t CB_FRAME = 0x400;
static constexpr uint32_t RED_ZONE = 0x120;     /* ELFv1: 288 bytes below r1 */
static constexpr uint32_t ENTRY_GAP = 0x70 + RED_ZONE;
static constexpr uint32_t HOST_TOP = VM_STACK_BASE + VM_STACK_REGION;   /* top of the guest stack region */
static constexpr uint32_t HOST_WINDOW = VM_CB_HOST_STACK_WINDOW;

struct Act { uint32_t entry, lo, hi, tag; };
static std::mutex g_mu;
static std::vector<Act> g_acts;
static std::atomic<int> g_arrived;
static int g_rendezvous;

static void fill(uint32_t lo, uint32_t hi, uint8_t v) { std::memset(vm_base + lo, v, hi - lo); }
static bool all_is(uint32_t lo, uint32_t hi, uint8_t v)
{
    for (uint32_t a = lo; a < hi; a++) if (vm_base[a] != v) return false;
    return true;
}

/* cb_leave runs on a host thread's slot, moves its r1 to `slot lo - r3` and calls
 * cb_mark through ppu_guest_call_ct. A nested callback entered from there would be
 * on another slot or a thread stack: it must stop before cb_mark runs. */
static constexpr uint32_t CB_LEAVE = 0x30010;
static constexpr uint32_t CB_MARK  = 0x30020;
static void cb_mark(ppu_context* ctx)
{
    std::printf("NESTED CALLBACK RAN\n");
    std::fflush(stdout);
    ctx->gpr[3] = 0x77;
}
static void cb_leave(ppu_context* ctx)
{
    const uint32_t lo = (uint32_t)ctx->gpr[1] + 0x80 - VM_PPU_STACK_SIZE;   /* entry r1 = slot top - 0x80 */
    ctx->gpr[1] = lo - ctx->gpr[3];
    ctx->gpr[3] = ppu_guest_call_ct(CB_MARK, CB_TOC, 0, 0, 0, 0, 0, 0, 0, 0);
}

static uint64_t call_cb(uint64_t tag, uint64_t depth, uint64_t meet)
{
    return ppu_guest_call_ct(CB_ADDR, CB_TOC, tag, depth, meet, 0, 0, 0, 0, 0);
}

static void cb(ppu_context* ctx)
{
    const uint32_t tag = (uint32_t)ctx->gpr[3], depth = (uint32_t)ctx->gpr[4];
    const bool meet = ctx->gpr[5] != 0;
    const uint32_t entry = (uint32_t)ctx->gpr[1];
    CHECK((entry & 0xF) == 0);
    CHECK(ctx->gpr[2] == CB_TOC);
    ctx->gpr[1] -= CB_FRAME;
    const uint32_t lo = (uint32_t)ctx->gpr[1], hi = entry + 0x70;
    fill(lo, hi, (uint8_t)(0x40 + tag));
    { std::lock_guard<std::mutex> g(g_mu); g_acts.push_back({entry, lo, hi, tag}); }
    if (meet) {
        g_arrived++;
        const auto t0 = std::chrono::steady_clock::now();
        while (g_arrived < g_rendezvous) {
            CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20));
            std::this_thread::yield();
        }
    }
    uint64_t r = 0x1000 + tag;
    if (depth)
        r += call_cb(tag + 1, depth - 1, 0);
    CHECK(all_is(lo, hi, (uint8_t)(0x40 + tag)));   /* nobody wrote over this activation's frame */
    ctx->gpr[1] += CB_FRAME;
    ctx->gpr[3] = r;
}

static const Act& act_of(uint32_t tag)
{
    for (const Act& a : g_acts) if (a.tag == tag) return a;
    CHECK(!"activation not recorded");
    std::abort();
}

/* The three chained activations of thread `i` (tags i*16 .. i*16+2): each nested one is
 * entered below the one in flight, clear of its frame and of the red zone above it. */
static void check_chain(int i)
{
    for (uint32_t k = 0; k < 2; k++) {
        const Act& outer = act_of(i * 16 + k);
        const Act& inner = act_of(i * 16 + k + 1);
        CHECK(inner.hi + RED_ZONE <= outer.lo);
    }
}

static void check_disjoint(size_t expect)
{
    CHECK(g_acts.size() == expect);
    for (size_t i = 0; i < g_acts.size(); i++)
        for (size_t j = i + 1; j < g_acts.size(); j++)
            CHECK(g_acts[i].hi <= g_acts[j].lo || g_acts[j].hi <= g_acts[i].lo);
}

static uint64_t expected_chain(int i) { return 3 * (0x1000 + (uint64_t)i * 16) + 3; }

static void ctx_thread(int i)
{
    ppu_context outer;
    std::memset(&outer, 0, sizeof outer);
    outer.thread_id = 10 + i;
    outer.gpr[1] = 0x40000000u + (uint32_t)i * 0x100000u + 0x80000u;
    const uint32_t r1 = (uint32_t)outer.gpr[1];
    const uint32_t clo = r1 - RED_ZONE, chi = r1 + 0x80;    /* this thread's own frame + red zone */
    fill(clo, chi, (uint8_t)(0xC0 + i));
    g_active_ctx = &outer;
    const uint64_t r = call_cb(i * 16, 2, 1);
    CHECK(g_active_ctx == &outer);
    CHECK(outer.gpr[1] == r1);
    CHECK(all_is(clo, chi, (uint8_t)(0xC0 + i)));
    CHECK(r == expected_chain(i));
    g_active_ctx = nullptr;
}

static void host_thread(int i, uint32_t* r)
{
    CHECK(g_active_ctx == nullptr);   /* no guest context: the thread owns a host slot */
    *r = (uint32_t)call_cb(i * 16, 2, 1);
}

static int cb_stack_scenarios()
{
    constexpr int N = 4;
    /* Guest-context threads: each callback runs on its own thread's stack, under its r1. */
    g_acts.clear(); g_arrived = 0; g_rendezvous = N;
    { std::vector<std::thread> t;
      for (int i = 0; i < N; i++) t.emplace_back(ctx_thread, i);
      for (auto& x : t) x.join(); }
    check_disjoint(N * 3);
    for (int i = 0; i < N; i++) {
        const uint32_t r1 = 0x40000000u + (uint32_t)i * 0x100000u + 0x80000u;
        CHECK(act_of(i * 16).entry == ((r1 - ENTRY_GAP) & ~0xFu));
        check_chain(i);
    }

    /* Host threads with no guest context, all live at once: one stack each. */
    g_acts.clear(); g_arrived = 0; g_rendezvous = N;
    uint32_t res[N] = {};
    { std::vector<std::thread> t;
      for (int i = 0; i < N; i++) t.emplace_back(host_thread, i, &res[i]);
      for (auto& x : t) x.join(); }
    check_disjoint(N * 3);
    uint32_t slot_seen = 0;
    for (int i = 0; i < N; i++) {
        CHECK(res[i] == (uint32_t)expected_chain(i));
        const uint32_t e = act_of(i * 16).entry;
        CHECK(e < HOST_TOP && e >= HOST_TOP - HOST_WINDOW);
        const uint32_t slot = (HOST_TOP - e) >> 20;
        CHECK(((HOST_TOP - e) & 0xFFFFF) == 0x80 && !(slot_seen & (1u << slot)));
        slot_seen |= 1u << slot;
        check_chain(i);
    }

    /* A thread's slot is released when it exits: far more threads than slots
     * in a row. Run sequentially, each also re-enters. */
    g_rendezvous = 0;
    for (int n = 0; n < 100; n++) {
        g_acts.clear();
        uint32_t r = 0;
        std::thread t(host_thread, 0, &r);
        t.join();
        CHECK(r == (uint32_t)expected_chain(0));
    }

    /* Same thread, no context, back to back: the callback restarts at its slot's top. */
    g_acts.clear();
    uint32_t r0 = 0, r1_ = 0;
    g_rendezvous = 0;
    r0 = (uint32_t)call_cb(0, 2, 0);
    const uint32_t first_entry = g_acts[0].entry;
    g_acts.clear();
    r1_ = (uint32_t)call_cb(0, 2, 0);
    CHECK(r0 == expected_chain(0) && r1_ == expected_chain(0));
    CHECK(g_acts[0].entry == first_entry);

    /* The thread-stack allocator cannot hand out a stack that intersects the host
     * callback window: it ends below it, so exhausting it returns CELL_ENOMEM
     * (and sys_ppu_thread_create CELL_ENOMEM) instead of overlapping a live slot. */
    vm_stack_alloc sa;
    vm_stack_alloc_init(&sa);
    CHECK(sa.region_end == VM_CB_HOST_STACK_BASE && VM_CB_HOST_STACK_BASE == HOST_TOP - HOST_WINDOW);
    unsigned stacks = 0;
    for (uint32_t base = 0; vm_stack_allocate(&sa, VM_PPU_STACK_SIZE, &base) == CELL_OK; ++stacks) {
        CHECK(base >= VM_STACK_BASE);
        CHECK((uint64_t)base + VM_PPU_STACK_SIZE <= VM_CB_HOST_STACK_BASE);   /* [base, base+size) is below the window */
    }
    CHECK(stacks > 100 && stacks < VM_STACK_REGION / VM_PPU_STACK_SIZE);   /* it ran out, and before the region's end */
    uint32_t unused = 0;
    CHECK(vm_stack_allocate(&sa, VM_PPU_STACK_SIZE, &unused) == (int32_t)CELL_ENOMEM);
    CHECK(vm_stack_allocate(&sa, VM_STACK_REGION, &unused) == (int32_t)CELL_ENOMEM);
    {
        /* An arena that does not reach the stack region: the guard page is refused,
         * so no stack is handed out (a stack without its guard overflows silently). */
        const uint32_t orig_vm_size = ppu_vm_size;
        ppu_vm_size = 0x10000u;
        CHECK(vm_protect(0x10000000u, 0x1000, 0, 0, 0) == (int32_t)CELL_EFAULT);
        CHECK(vm_commit(0x10000000u, 0x1000) == (int32_t)CELL_EFAULT);
        vm_stack_alloc bounded{0x10000000u, 0xA0000000u};
        uint32_t bad = 0;
        CHECK(vm_stack_allocate(&bounded, 0x10000, &bad) == (int32_t)CELL_EFAULT);
        CHECK(bad == 0 && bounded.next_addr == 0x10000000u);
        ppu_vm_size = orig_vm_size;
    }
    {
        /* The one guest-range rule (runtime/memory/vm.h): a zero ppu_vm_size is
         * the whole 4 GB space, not a zero-length arena; an identity mapping is
         * ready with vm_base == 0; a span that wraps past 2^32 is refused while
         * an exact end is accepted. */
        const uint32_t orig_vm_size = ppu_vm_size;
        uint8_t* const orig_base = vm_base;
        ppu_vm_size = 0;
        CHECK(vm_range_ok(0x00001000u, 0x10u) == 1);
        CHECK(vm_range_ok(0xFFFFFFF0u, 0x10u) == 1);      /* ends exactly at 4 GB */
        CHECK(vm_range_ok(0xFFFFFFF0u, 0x20u) == 0);      /* wraps past it */
        CHECK(vm_range_ok(0x80000000u, 0x80000000u) == 1);
        CHECK(vm_range_ok(0u, 0u) == 1);
        ppu_vm_size = 0x10000u;
        CHECK(vm_range_ok(0x10000u, 0u) == 1);            /* empty span at the end */
        CHECK(vm_range_ok(0x10000u, 1u) == 0);            /* one past the end */
        CHECK(vm_range_ok(0xF000u, 0x1000u) == 1);        /* exact end */
        CHECK(vm_range_ok(0xF001u, 0x1000u) == 0);
        /* Identity mapping: the EA is the host address, so vm_base is 0 by
         * design and must not be read as "no VM". */
        vm_base = nullptr;
        CHECK(vm_range_ok(0x8000u, 0x100u) == 1);
        CHECK(vm_range_ok(0x10000u, 0x100u) == 0);
        /* Nothing initialised: no guest pointer may be formed at all. */
        ppu_vm_size = 0;
        CHECK(vm_range_ok(0x1000u, 4u) == 0);
        vm_base = orig_base;
        ppu_vm_size = orig_vm_size;
    }
    {
        /* A request that does not fit the region: vm_stack_allocate's alignment
         * and guard-page addition used to wrap in 32 bits, so this passed and
         * handed out a stack with no room in it. */
        vm_stack_alloc huge;
        vm_stack_alloc_init(&huge);
        uint32_t base = 0;
        CHECK(vm_stack_allocate(&huge, 0xFFFFF000u, &base) == (int32_t)CELL_ENOMEM);
        CHECK(base == 0 && huge.next_addr == VM_STACK_BASE);
        CHECK(vm_stack_allocate(&huge, 0xFFFFFFFFu, &base) == (int32_t)CELL_ENOMEM);
        CHECK(base == 0 && huge.next_addr == VM_STACK_BASE);
    }
    std::printf("CBSTACK OK: %d ctx threads + %d host threads + 100 short-lived threads, "
                "stacks distinct and disjoint; %u thread stacks fit below the host window\n",
                N, N, stacks);
    return 0;
}

/* ---- host-thread identity ---------------------------------------------------
 * A callback raised by a host thread with no guest context runs as a guest
 * thread of its own. The identity lwmutex ownership keys on is ppu_context's
 * thread_id, so two such threads must not share one: a lock one holds would
 * read as the other's, and trylock / unlock from it would succeed. The
 * callbacks below call the real lwmutex handlers on their own context, so the
 * id they pass is the id the loader gave them. */
static constexpr uint32_t CB_HOLD = 0x30030, CB_PEER = 0x30040, CB_WHO = 0x30050;
/* Above 0x10000: the page below it is the null page, which stops a guest load. */
static constexpr uint32_t LW_ATTR = 0x12000, LW_MUTEX = 0x12100;
static constexpr int32_t LW_OK = 0, LW_EBUSY = (int32_t)0x8001000A, LW_EPERM = (int32_t)0x80010009;
static std::atomic<uint64_t> g_who[8];
static std::atomic<int> g_holds, g_tried, g_unlocking;

static int32_t lw(const char* name, ppu_context* ctx, uint64_t a3, uint64_t a4 = 0)
{
    for (int i = 0; i < g_hle_n; i++) {
        if (std::strcmp(g_hle[i].name, name)) continue;
        ctx->gpr[3] = a3;
        ctx->gpr[4] = a4;
        g_hle[i].fn(ctx);
        return (int32_t)ctx->gpr[3];
    }
    CHECK(!"lwmutex handler not registered");
    return 0;
}

static void await(const std::atomic<int>& flag)
{
    const auto t0 = std::chrono::steady_clock::now();
    while (!flag.load()) {
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20));
        std::this_thread::yield();
    }
}

static void cb_who(ppu_context* ctx)      /* r3: which g_who slot records this thread's id */
{
    g_who[ctx->gpr[3] & 7] = ctx->thread_id;
    ctx->gpr[3] = 0;
}
static void cb_hold(ppu_context* ctx)
{
    g_who[0] = ctx->thread_id;
    CHECK(lw("sys_lwmutex_lock", ctx, LW_MUTEX) == LW_OK);
    g_holds = 1;
    await(g_tried);
    g_unlocking = 1;
    CHECK(lw("sys_lwmutex_unlock", ctx, LW_MUTEX) == LW_OK);
    ctx->gpr[3] = 0;
}
static void cb_peer(ppu_context* ctx)
{
    await(g_holds);
    g_who[1] = ctx->thread_id;
    CHECK(lw("sys_lwmutex_trylock", ctx, LW_MUTEX) == LW_EBUSY);   /* held by another thread, not by us */
    CHECK(lw("sys_lwmutex_unlock", ctx, LW_MUTEX) == LW_EPERM);
    g_tried = 1;
    CHECK(lw("sys_lwmutex_lock", ctx, LW_MUTEX) == LW_OK);        /* blocks until the holder unlocks */
    CHECK(g_unlocking == 1);
    CHECK(lw("sys_lwmutex_unlock", ctx, LW_MUTEX) == LW_OK);
    ctx->gpr[3] = 0;
}

static int host_id_scenario()
{
    ps3_lwsync_register();
    vm_write32(LW_ATTR, 1);          /* FIFO */
    vm_write32(LW_ATTR + 4, 0x20);   /* not recursive */
    ppu_context main_ctx;
    std::memset(&main_ctx, 0, sizeof main_ctx);
    main_ctx.thread_id = 1;
    CHECK(lw("sys_lwmutex_create", &main_ctx, LW_MUTEX, LW_ATTR) == LW_OK);

    auto host = [](uint32_t cb_addr, uint64_t who_again) {
        CHECK(g_active_ctx == nullptr);   /* no guest context: a host thread's own identity */
        ppu_guest_call_ct(cb_addr, CB_TOC, 0, 0, 0, 0, 0, 0, 0, 0);
        ppu_guest_call_ct(CB_WHO, CB_TOC, who_again, 0, 0, 0, 0, 0, 0, 0);   /* a second callback, same thread */
    };
    { std::thread a(host, CB_HOLD, 2), b(host, CB_PEER, 3); a.join(); b.join(); }

    const uint64_t a = g_who[0], b = g_who[1];
    CHECK(a != b);
    for (uint64_t id : {a, b}) CHECK(id >= 0x7F000000ull && id < 0x7FFF0000ull);
    CHECK(g_who[2] == a && g_who[3] == b);      /* stable for the life of the thread */
    /* a thread that starts after both ended does not inherit either id (no reuse: it
     * could otherwise inherit a lock a dead thread still holds) */
    { std::thread c([&] { host(CB_WHO, 4); }); c.join(); }
    CHECK(g_who[4] != a && g_who[4] != b && g_who[4] >= 0x7F000000ull);
    std::printf("HOSTID OK: host threads 0x%llX and 0x%llX distinct and stable, a later one 0x%llX; "
                "the lock one held blocked the other\n",
                (unsigned long long)a, (unsigned long long)b, (unsigned long long)g_who[4].load());
    return 0;
}

/* `depth` recurses through 4000 host dispatches before the cap stops it. ASan
 * and UBSan instrument every frame, so the default 8 MB stack runs out first
 * and the process dies at the stack guard instead of reaching the named stop.
 * Run that mode on a thread whose stack is sized for the instrumented frames. */
static constexpr size_t DEPTH_STACK = 64u << 20;

struct DepthRun { ppu_context* ctx; };

static void* depth_thread(void* p)
{
    DepthRun* r = (DepthRun*)p;
    ps3_indirect_call(r->ctx);
    return nullptr;
}

int main(int argc, char** argv)
{
    if (argc < 3) return 2;
    /* The whole guest stack region (to 0xE0000000) is mapped: a callback raised
     * by a host thread with no guest context runs on a stack slot at its top.
     * Lazily backed, so only the touched pages cost anything. */
    vm_base = (uint8_t*)mmap(nullptr, 0xE0000000ull, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (vm_base == MAP_FAILED) return 2;
    ppu_vm_size = 0xE0000000u;
    ppu_recomp_register();
    ppu_register_function(CB_ADDR, cb);
    ppu_register_function(CB_LEAVE, cb_leave);
    ppu_register_function(CB_MARK, cb_mark);
    ppu_register_function(CB_HOLD, cb_hold);
    ppu_register_function(CB_PEER, cb_peer);
    ppu_register_function(CB_WHO, cb_who);

    ppu_context ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = 1;
    ctx.lr = 0x10100;
    const uint64_t arg = std::strtoull(argv[2], nullptr, 0);

    if (!std::strcmp(argv[1], "syscall")) {
        ctx.gpr[11] = arg;
        lv2_syscall(&ctx);
    } else if (!std::strcmp(argv[1], "unlifted")) {
        ppu_unlifted_stub(arg, &ctx);
    } else if (!std::strcmp(argv[1], "indirect")) {
        ctx.ctr = arg;
        ps3_indirect_call(&ctx);
    } else if (!std::strcmp(argv[1], "call")) {
        ctx.ctr = arg;
        ps3_indirect_call(&ctx);
    } else if (!std::strcmp(argv[1], "depth")) {
        /* The fixture function does `mtctr r4; bctrl`, so r4 names the target
         * it calls (itself) on every recursion. */
        ctx.gpr[4] = arg;
        ctx.ctr = arg;
        DepthRun run{&ctx};
        pthread_attr_t attr;
        CHECK(pthread_attr_init(&attr) == 0);
        CHECK(pthread_attr_setstacksize(&attr, DEPTH_STACK) == 0);
        pthread_t thread;
        CHECK(pthread_create(&thread, &attr, depth_thread, &run) == 0);
        pthread_attr_destroy(&attr);
        pthread_join(thread, nullptr);
    } else if (!std::strcmp(argv[1], "opdswap") && argc == 4) {
        ctx.ctr = arg;
        ctx.gpr[2] = std::strtoull(argv[3], nullptr, 0);
        ps3_indirect_call(&ctx);
    } else if (!std::strcmp(argv[1], "guestcall")) {
        ctx.gpr[3] = ppu_guest_call((uint32_t)arg, 0, 0, 0, 0, 0, 0, 0, 0);
    } else if (!std::strcmp(argv[1], "guestcallopd") && argc == 5) {
        /* Build an OPD in guest memory, then call it: <opd> <code> <toc>. */
        const uint32_t opd = (uint32_t)arg;
        vm_write32(opd, (uint32_t)std::strtoull(argv[3], nullptr, 0));
        vm_write32(opd + 4, (uint32_t)std::strtoull(argv[4], nullptr, 0));
        vm_write32(opd + 8, 0);
        ctx.gpr[3] = ppu_guest_call(opd, 0, 0, 0, 0, 0, 0, 0, 0);
    } else if (!std::strcmp(argv[1], "guestcallct")) {
        const uint32_t toc = argc > 3 ? (uint32_t)std::strtoull(argv[3], nullptr, 0) : 0;
        ctx.gpr[3] = ppu_guest_call_ct((uint32_t)arg, toc, 0, 0, 0, 0, 0, 0, 0, 0);
    } else if (!std::strcmp(argv[1], "cbstack")) {
        return cb_stack_scenarios();
    } else if (!std::strcmp(argv[1], "hostid")) {
        return host_id_scenario();
    } else if (!std::strcmp(argv[1], "cbexhaust")) {
        /* No guest context: the thread's host slot is the only stack, and 100000 nested
         * activations cannot fit in it. */
        ctx.gpr[3] = call_cb(0, 100000, 0);
    } else if (!std::strcmp(argv[1], "cbbadr1")) {
        ppu_context bad;
        std::memset(&bad, 0, sizeof bad);
        bad.thread_id = 7;
        g_active_ctx = &bad;       /* r1 == 0: not a stack address */
        ctx.gpr[3] = call_cb(0, 0, 0);
    } else if (!std::strcmp(argv[1], "cbsmallvm")) {
        ppu_vm_size = 0x20000000u;   /* ends below the host callback stack window */
        ctx.gpr[3] = call_cb(0, 0, 0);
    } else if (!std::strcmp(argv[1], "cbleave")) {
        /* A host-slot callback whose r1 sits <arg> below its slot's lowest address
         * (0: exactly at it) re-enters ppu_guest_call_ct: the nested callback must
         * not run (cb_mark would print). */
        ctx.gpr[3] = ppu_guest_call_ct(CB_LEAVE, CB_TOC, arg, 0, 0, 0, 0, 0, 0, 0);
    } else if (!std::strcmp(argv[1], "nullread") && argc == 4) {
        /* A guest load of <width> bytes at <arg>. Memory behind the null page is
         * ordinary RAM here, so a load that is let through returns the 0xAB fill. */
        g_active_ctx = &ctx;
        const uint32_t ea = (uint32_t)arg;
        std::memset(vm_base + ea, 0xAB, 16);
        switch (std::atoi(argv[3])) {
        case 1: ctx.gpr[3] = vm_read8(ea); break;
        case 2: ctx.gpr[3] = vm_read16(ea); break;
        case 4: ctx.gpr[3] = vm_read32(ea); break;
        case 8: ctx.gpr[3] = vm_read64(ea); break;
        default: return 2;
        }
    } else if (!std::strcmp(argv[1], "nullwrite") && argc == 4) {
        /* A guest store of <width> bytes of 0x5A at <arg>; reports the first byte
         * of memory afterwards: 0xAB if the store was dropped, 0x5A if it landed. */
        g_active_ctx = &ctx;
        const uint32_t ea = (uint32_t)arg;
        std::memset(vm_base + ea, 0xAB, 16);
        switch (std::atoi(argv[3])) {
        case 1: vm_write8(ea, 0x5A); break;
        case 2: vm_write16(ea, 0x5A5A); break;
        case 4: vm_write32(ea, 0x5A5A5A5Au); break;
        case 8: vm_write64(ea, 0x5A5A5A5A5A5A5A5Aull); break;
        default: return 2;
        }
        ctx.gpr[3] = vm_base[ea];
    } else if (!std::strcmp(argv[1], "nullsweep")) {
        /* A store into the null page, one 0x1000 above it (the memset walking up),
         * and one far past it: under "drop" the first two are dropped and the third
         * ends the sweep and lands. */
        g_active_ctx = &ctx;
        std::memset(vm_base, 0xAB, 0x4000);
        vm_write32(0x10, 0x5A5A5A5Au);
        vm_write32(0x1010, 0x5A5A5A5Au);
        vm_write32(0x3000, 0x5A5A5A5Au);
        std::printf("SWEEP 10=%02X 1010=%02X 3000=%02X\n", vm_base[0x10], vm_base[0x1010], vm_base[0x3000]);
    } else {
        return 2;
    }
    std::printf("RETURNED r3=0x%llX\n", (unsigned long long)ctx.gpr[3]);
    return 0;
}

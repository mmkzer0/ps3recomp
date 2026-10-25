/* Fibers blocked in the real Port2Sync and Sync2 paths.
 *
 * The real loader dispatches synthetic PPU functions as fiber bodies, the
 * generated HLE table dispatches the firmware imports they call, and the
 * JobQueue2 executor runs a synthetic SPU body that PUTs a marker. Orders are
 * fixed by the executor's pause and admission hooks and by guest state, never
 * by sleeping. All bytes are synthetic. */
#include "ppu_context.h"
#include "sys_ppu_thread.h"
#include "libs/spurs/cellFiber.h"
#include "jq2/executor.hpp"
#include "jq2/jq2.h"
#include "spu_context.h"
#include "spu_helpers.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ps3emu/lifter_abi.h"   /* the lifted-code ABI this fixture is written in */

/* The generated loader's three-field registry entry; this fixture registers
 * its synthetic functions at runtime and has no static lifted program. */
typedef struct { uint64_t addr; void (*func)(ppu_context*); const char* name; } func_entry;
extern "C" {
uint8_t* vm_base;
extern uint32_t ppu_vm_size;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern const func_entry function_table[1] = {{0, nullptr, nullptr}};
extern const uint64_t function_table_count = 0;
const char* ppu_lift_image_sha256(void) { return ""; }
/* The lifted-code ABI this fixture is written in (ppu_recomp.h stamps it for a
 * real lift); the loader checks it at startup. */
extern const uint32_t ppu_lifter_abi = PPU_RUNTIME_LIFTER_ABI;   /* C linkage from the block above */
void ppu_hle_init(void);
void ps3_hle_call(uint32_t, ppu_context*);
void ppu_register_function(uint64_t, void (*)(ppu_context*));
void ps3_indirect_call(ppu_context*);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
u128 spu_rdch(spu_context* ctx, uint32_t channel);
}
using namespace ps3recomp::spurs::jq2;

namespace {

constexpr uint32_t MEM = 0x400000;
constexpr uint32_t OPD = 0x8000, ENTRY = 0x1000;
constexpr uint32_t CONTROL = 0x10000, WATTR_EA = 0x11000, FATTR = 0x12000, FIBER = 0x20000;
constexpr uint32_t STACK = 0x200000;
constexpr uint32_t SEM = 0x290000, SATTR = 0x291000, SBUF = 0x292000;
constexpr uint32_t QUEUE = 0x100000, RING = 0x110000, QATTR = 0x120000, SPURS = 0x130000;
constexpr uint32_t PORT = 0x140000, IMAGE = 0x150000;
constexpr uint32_t DESC_BASE = 0x300000, OUT_BASE = 0x340000;
constexpr unsigned MAX_JOBS = 512;

/* cellFiber */
constexpr uint32_t INIT = 0x55870804, ATTR_INIT = 0xC11F8056, WATTR = 0x68BA4568, INITIALIZE = 0xF2CCAD4F;
constexpr uint32_t CREATE = 0xBFCA88D3, RUN = 0x1E7A247A, SIGNAL = 0x5D3992DD;
constexpr uint32_t SHUTDOWN = 0xC04E2438, FINALIZE = 0xA27C95CA;
/* cellSync2 semaphore */
constexpr uint32_t SEM_ATTR = 0x2D77FE17, SEM_INIT = 0xC5DEE254, SEM_GET = 0x4E2EE031;
constexpr uint32_t ACQUIRE = 0xD1B0D146, RELEASE = 0x0C2983AC;
/* cellSpursJq */
constexpr uint32_t PUSH = 0xCF89F218, SYNC = 0xFACB3CED;

constexpr uint64_t FIBER_BUSY = uint64_t(int64_t(int32_t(0x8076000A)));
constexpr uint32_t SDK = 0x360001;

std::atomic<unsigned> failures{0};
#define CHECK(c) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

uint32_t rd(uint32_t ea) { return __builtin_bswap32(std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(vm_base + ea)).load()); }
void wr(uint32_t ea, uint32_t x) { std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(vm_base + ea)).store(__builtin_bswap32(x)); }
void wr16(uint32_t ea, uint16_t x) { x = __builtin_bswap16(x); std::memcpy(vm_base + ea, &x, 2); }
void wr64(uint32_t ea, uint64_t x) { wr(ea, uint32_t(x >> 32)); wr(ea + 4, uint32_t(x)); }
uint64_t word(uint32_t ea) { return __builtin_bswap64(std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(vm_base + ea)).load()); }

template <class P> bool until(P p)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!p()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

/* A calling thread's own thread-table context: the profiler maps every
 * dispatched context to its slot, so concurrent callers need distinct ones. */
ppu_context& caller()
{
    static std::atomic<unsigned> slots{0};
    static thread_local const unsigned slot = slots.fetch_add(1);
    CHECK(slot < 32);
    auto& ctx = g_ppu_threads[slot].ctx;
    ctx.thread_id = slot + 1; ctx.gpr[1] = 0x40000 + 0x1000 * slot; ctx.gpr[13] = 0x60000 + 0x1000 * slot;
    return ctx;
}
uint64_t call(uint32_t nid, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0,
              uint64_t f = 0, uint64_t g = 0, uint64_t h = 0)
{
    auto& ctx = caller();
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d;
    ctx.gpr[7] = e; ctx.gpr[8] = f; ctx.gpr[9] = g; ctx.gpr[10] = h;
    auto* previous = g_active_ctx; g_active_ctx = &ctx;
    ps3_hle_call(nid, &ctx);
    g_active_ctx = previous;
    return ctx.gpr[3];
}
/* The same import issued from a fiber body, on the fiber's own context. */
uint64_t hc(ppu_context* ctx, uint32_t nid, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0,
            uint64_t e = 0)
{
    ctx->gpr[3] = a; ctx->gpr[4] = b; ctx->gpr[5] = c; ctx->gpr[6] = d; ctx->gpr[7] = e;
    ps3_hle_call(nid, ctx);
    return ctx->gpr[3];
}

/* ---- the clock and the ledger ------------------------------------------------ */

std::atomic<uint64_t> clk{0};
uint64_t tick() { return clk.fetch_add(1) + 1; }
struct Ledger {
    std::atomic<uint64_t> admit[MAX_JOBS], start[MAX_JOBS], done[MAX_JOBS], synced[MAX_JOBS], released[MAX_JOBS];
} L;

uint32_t desc_of(unsigned j) { return DESC_BASE + 0x100 * j; }
uint32_t out_of(unsigned j) { return OUT_BASE + 0x10 * j; }
uint32_t mark_of(unsigned j) { return 0xC0DE0000u + j; }
bool output_ok(unsigned j)
{
    for (unsigned i = 0; i < 4; ++i) if (rd(out_of(j) + 4 * i) != mark_of(j) + i) return false;
    return true;
}

/* ---- the synthetic SPU job --------------------------------------------------- */

constexpr uint32_t kCode = 0x300, kImageSize = 0x1000, kBssQuads = 0x100;
constexpr uint32_t kBase = 0x7C00, kLsOut = kBase + 0x1810;
constexpr uint32_t kBiLr = 0x35000000u;
uint32_t e_br(uint32_t pc, uint32_t target) { return (0x64u << 23) | ((((int32_t(target) - int32_t(pc)) / 4) & 0xFFFFu) << 7); }

/* The image only has to stage; the body below does the work, as a lifted body. */
void build_image()
{
    std::uint8_t* img = vm_base + IMAGE;
    std::memset(img, 0, kImageSize);
    std::memcpy(img + 0x30, "JOBCRT Ver13", 12);
    wr(IMAGE, e_br(0, kCode));
    wr(IMAGE + kCode, kBiLr);
}
void build_job(unsigned j)
{
    std::memset(vm_base + desc_of(j), 0, 0x100);
    wr64(desc_of(j), (uint64_t{kBssQuads} << 32) | (IMAGE | 1u));
    wr16(desc_of(j) + 0x08, kImageSize >> 4);
    wr16(desc_of(j) + 0x1C, 0x40);
    wr16(desc_of(j) + 0x1E, 0x10);
    wr(desc_of(j) + 0x30, out_of(j));
    wr(desc_of(j) + 0x34, mark_of(j));
    wr(desc_of(j) + 0x38, j);
    std::memset(vm_base + out_of(j), 0, 16);
}
uint32_t ls_word(const spu_context* c, uint32_t lsa)
{
    uint32_t v; std::memcpy(&v, c->ls + lsa, 4); return __builtin_bswap32(v);
}
void body(spu_context* c)
{
    const uint32_t d = c->gpr[4]._u32[0];
    const uint32_t out = ls_word(c, d + 0x30), mark = ls_word(c, d + 0x34), j = ls_word(c, d + 0x38);
    L.start[j] = tick();
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t v = __builtin_bswap32(mark + i);
        std::memcpy(c->ls + kLsOut + 4 * i, &v, 4);
    }
    spu_wrch(c, MFC_LSA, spu_splat_u32(kLsOut));
    spu_wrch(c, MFC_EAH, spu_splat_u32(0));
    spu_wrch(c, MFC_EAL, spu_splat_u32(out));
    spu_wrch(c, MFC_Size, spu_splat_u32(16));
    spu_wrch(c, MFC_TagID, spu_splat_u32(1));
    spu_wrch(c, MFC_Cmd, spu_splat_u32(MFC_PUT_CMD));
    spu_wrch(c, MFC_WrTagMask, spu_splat_u32(2));
    spu_wrch(c, MFC_WrTagUpdate, spu_splat_u32(2));
    (void)spu_rdch(c, MFC_RdTagStat);
    SPU_RET(c);
}
JobBody hook_select(void*, const JobStart&) { return JobBody{&body, -1}; }
unsigned serial_of(const JobStart& js)
{
    uint32_t v; std::memcpy(&v, js.descriptor.data() + 0x38, 4); return __builtin_bswap32(v);
}

/* A job's admission can be held, which makes it a long-running job. */
struct Gate {
    std::mutex mu; std::condition_variable cv;
    int serial = -1; bool reached = false, open = true;
} gate;
void hook_admit(void*, const JobStart& js)
{
    const unsigned j = serial_of(js);
    L.admit[j] = tick();
    std::unique_lock lk(gate.mu);
    if (int(j) != gate.serial) return;
    gate.reached = true; gate.cv.notify_all();
    gate.cv.wait(lk, [] { return gate.open; });
}
void hook_done(void*, const JobStart& js) { L.done[serial_of(js)] = tick(); }
void hold_job(int serial)
{
    std::lock_guard lk(gate.mu);
    gate.serial = serial; gate.reached = false; gate.open = serial < 0;
}
bool gate_reached() { std::lock_guard lk(gate.mu); return gate.reached; }
void open_gate() { { std::lock_guard lk(gate.mu); gate.open = true; gate.serial = -1; } gate.cv.notify_all(); }

/* A push parked on a full ring (ExecutorHooks::on_push_blocked). */
std::atomic<unsigned> push_blocked{0};
void hook_push_blocked(void*, uint32_t) { push_blocked.fetch_add(1); }

/* ---- world ------------------------------------------------------------------ */

std::atomic<unsigned> fibers_done{0};
unsigned nfibers = 0, nports = 0;
std::thread pool[4];
unsigned pooled = 0;

void trampoline(ppu_context* ctx)
{
    ctx->ctr = rd(uint32_t(ctx->cia));
    ps3_indirect_call(ctx);
}

uint32_t port_ea(unsigned k) { return PORT + 0x80 * k; }
void make_port(unsigned k)
{
    CHECK(jq2_port_create(port_ea(k), QUEUE) == 0);
    nports = k + 1 > nports ? k + 1 : nports;
}
void boot(unsigned queue_workers, unsigned ring_depth = 16)
{
    build_image();
    ExecutorHooks hooks;
    hooks.select_body = &hook_select;
    hooks.on_admit = &hook_admit;
    hooks.on_done = &hook_done;
    hooks.on_push_blocked = &hook_push_blocked;
    set_executor_hooks(hooks);
    wr(QATTR + 4, 8); wr(QATTR + 8, 256);
    CHECK(jq2_queue_create(1, SDK, SPURS, QUEUE, QATTR, 0, 0, 0, RING, ring_depth, queue_workers, 0) == 0);
    make_port(0);
    CHECK(call(INIT, caller().gpr[13] + 0x100) == 0x100);
    CHECK(call(WATTR, WATTR_EA, SDK) == 0);
    CHECK(call(INITIALIZE, CONTROL, WATTR_EA) == 0);
}
uint32_t make_fiber(uint64_t arg)
{
    const unsigned i = nfibers++;
    CHECK(call(ATTR_INIT, FATTR, SDK) == 0);
    wr(OPD + 8 * i, ENTRY); wr(OPD + 8 * i + 4, 0);
    const uint32_t ea = FIBER + 0x400 * i;
    CHECK(call(CREATE, CONTROL, ea, OPD + 8 * i, arg, 0, STACK + 0x10000 * i, 0x10000, FATTR) == 0);
    return ea;
}
void start_workers(unsigned n)
{
    for (unsigned i = 0; i < n; ++i) pool[pooled++] = std::thread([] { CHECK(call(RUN, CONTROL) == 0); });
}
/* Shutdown is BUSY while a fiber lives; it succeeds once the last has ended. */
void stop_workers()
{
    uint64_t rc = 0;
    CHECK(until([&] { rc = call(SHUTDOWN, CONTROL); return rc != FIBER_BUSY; }));
    CHECK(rc == 0);
    for (unsigned i = 0; i < pooled; ++i) pool[i].join();
    pooled = 0;
    CHECK(call(FINALIZE, CONTROL) == 0);
}
void teardown()
{
    open_gate();
    CHECK(set_intake_paused(QUEUE, false));
    stop_workers();
    for (unsigned k = 0; k < nports; ++k) CHECK(jq2_port_destroy(port_ea(k)) == 0);
    CHECK(jq2_queue_shutdown(QUEUE) == 0);
    uint32_t exit_value = 0xFFFFFFFF;
    CHECK(jq2_queue_join(QUEUE, &exit_value) == 0);
}
jq2_stats stats()
{
    jq2_stats s{};
    CHECK(jq2_queue_stats(QUEUE, &s) == 0);
    return s;
}
uint32_t a_of(unsigned k = 0) { return rd(port_ea(k)); }
bool waiting(unsigned k = 0) { return (a_of(k) & 0x80000000u) != 0; }
bool bitmap_is(uint32_t w0, uint32_t w1 = 0, uint32_t w2 = 0, uint32_t w3 = 0)
{
    return rd(QUEUE + 0x88) == w0 && rd(QUEUE + 0x8C) == w1 && rd(QUEUE + 0x90) == w2 && rd(QUEUE + 0x94) == w3;
}

/* ---- fiber bodies ----------------------------------------------------------- */

enum Role : uint64_t { R_SYNC = 1, R_OTHER, R_RACE, R_PRODUCER, R_CONSUMER, R_BREAK, R_PUSHFULL, R_RESUME, R_BREAK_PUSH };
/* arg: role | job << 8 | port << 24 */
uint64_t arg_of(Role r, unsigned job = 0, unsigned port = 0) { return r | (uint64_t(job) << 8) | (uint64_t(port) << 24); }

/* The producer's push; the ring may be full until the SPU workers drain it. */
void push(ppu_context* ctx, unsigned port, unsigned j, unsigned tag)
{
    const uint64_t rc = hc(ctx, PUSH, port_ea(port), desc_of(j), 64, tag, 1); /* flags 1 waits on a full ring */
    CHECK(rc == 0);
}

std::atomic<bool> second_ran{false};
std::atomic<uint32_t> a_when_second{0}, seen_marker{0};
std::atomic<uint64_t> sync_rc{~0ull};
std::atomic<uint64_t> full_push_rc{~0ull};
std::atomic<bool> pusher_done{false}, pusher_done_when_resumed{true};
std::atomic<unsigned> blocked_when_resumed{0};

struct Phase {
    unsigned first = 0, jobs = 0, batch = 1, consumers = 0;
    int gate = -1;
} ph;
std::atomic<unsigned> want{0}, taken{0}, rel_n{0};
std::atomic<uint64_t> acq[MAX_JOBS];

int32_t count_of(ppu_context* ctx)
{
    CHECK(hc(ctx, SEM_GET, SEM, SEM + 0x3000) == 0);
    return int32_t(rd(SEM + 0x3000));
}

void producer(ppu_context* ctx)
{
    for (unsigned b = 0; b < ph.jobs; b += ph.batch) {
        const unsigned n = ph.batch < ph.jobs - b ? ph.batch : ph.jobs - b;
        for (unsigned k = 0; k < n; ++k) push(ctx, 0, ph.first + b + k, k & 1);
        CHECK(hc(ctx, SYNC, port_ea(0), 0) == 0);
        const uint64_t t = tick();
        for (unsigned k = 0; k < n; ++k) L.synced[ph.first + b + k] = t;
        for (unsigned k = 0; k < n; ++k) {
            const unsigned j = ph.first + b + k;
            CHECK(output_ok(j)); /* the Sync returned: the job's output is visible */
            /* GetCount never exceeds the releases begun, never goes below zero. */
            const int32_t c = count_of(ctx);
            CHECK(c >= 0 && unsigned(c) <= rel_n.load());
            rel_n.fetch_add(1);
            L.released[j] = tick();
            CHECK(hc(ctx, RELEASE, SEM, 1, 0) == 0);
        }
    }
}
void consumer(ppu_context* ctx)
{
    while (want.fetch_add(1) < ph.jobs) {
        CHECK(hc(ctx, ACQUIRE, SEM, 1, 0) == 0);
        /* The t-th successful Acquire (counted after it returns) had t+1
         * releases begun before it, so jobs first..first+t are done. */
        const unsigned t = taken.fetch_add(1);
        acq[t] = tick();
        for (unsigned j = 0; j <= t; ++j) CHECK(output_ok(ph.first + j));
    }
}

void fiber_main(ppu_context* ctx)
{
    const Role role = Role(ctx->gpr[3] & 0xFF);
    const unsigned job = unsigned(ctx->gpr[3] >> 8) & 0xFFFF, port = unsigned(ctx->gpr[3] >> 24);
    CHECK(g_active_ctx == ctx);
    switch (role) {
    case R_SYNC:
        push(ctx, port, job, 0);
        sync_rc = hc(ctx, SYNC, port_ea(port), 0);
        L.synced[job] = tick();
        seen_marker = rd(out_of(job));
        break;
    case R_OTHER:
        a_when_second = a_of(port);
        second_ran = true;
        break;
    case R_RACE:
        for (unsigned i = 0; i < 100; ++i) {
            const unsigned j = port * 128 + i;
            push(ctx, port, j, 0);
            CHECK(hc(ctx, SYNC, port_ea(port), 0) == 0);
            CHECK(output_ok(j));
        }
        break;
    case R_PRODUCER: producer(ctx); break;
    case R_CONSUMER: consumer(ctx); break;
    case R_PUSHFULL:
        full_push_rc = hc(ctx, PUSH, port_ea(port), desc_of(job), 64, 0, 1);
        pusher_done = true;
        break;
    case R_RESUME:
        blocked_when_resumed = push_blocked.load();
        pusher_done_when_resumed = pusher_done.load();
        CHECK(set_intake_paused(QUEUE, false));
        break;
    case R_BREAK_PUSH:
        wr(FIBER, 0); /* its own state word: the wait on a full ring must refuse */
        hc(ctx, PUSH, port_ea(port), desc_of(job), 64, 0, 1);
        break;
    case R_BREAK: {
        /* Zero this fiber's own state word: its WaitSignal must now refuse. */
        wr(FIBER, 0);
        push(ctx, port, job, 0);
        hc(ctx, SYNC, port_ea(port), 0);
        break;
    }
    }
    fibers_done.fetch_add(1);
    ctx->gpr[3] = 0;
}

/* ---- scenarios -------------------------------------------------------------- */

/* A fiber's mode 0 Sync registers kind 0 with its own EA and parks without a
 * bitmap bit; another fiber runs meanwhile; the credit signals it. */
void kind0()
{
    boot(1);
    CHECK(set_intake_paused(QUEUE, true));
    build_job(0);
    const uint32_t first = make_fiber(arg_of(R_SYNC, 0));
    make_fiber(arg_of(R_OTHER));
    start_workers(1);
    CHECK(until([] { return second_ran.load(); }));
    /* The first fiber is parked in Sync, so the second ran on its worker. */
    CHECK((a_when_second.load() & 0x80000000u) != 0);
    CHECK(waiting() && ((a_of() >> 29) & 3) == 0);
    CHECK(a_of() == 0x9FFFFFFFu && rd(PORT + 4) == first); /* waiting, kind 0, A-B = -1 */
    CHECK(rd(PORT + 0x10) == 1 && (rd(PORT + 0x14) >> 24) == 1);
    CHECK(bitmap_is(0));
    CHECK((rd(first) & 0x08) && !(rd(first) & 0x10)); /* waiting, no stale signal */
    CHECK(sync_rc.load() == ~0ull && L.admit[0] == 0);

    CHECK(set_intake_paused(QUEUE, false));
    CHECK(until([] { return fibers_done == 2; }));
    CHECK(sync_rc.load() == 0 && seen_marker.load() == mark_of(0));
    CHECK(L.admit[0] < L.start[0] && L.start[0] < L.done[0] && L.done[0] < L.synced[0]);
    CHECK(!waiting() && (a_of() & 0x1FFFFFFFu) == 0 && rd(PORT + 0x10) == 0 && (rd(PORT + 0x14) >> 24) == 0);
    CHECK(rd(PORT + 4) == first); /* the stale waiter word is not cleared */
    CHECK(bitmap_is(0));
    const jq2_stats s = stats();
    CHECK(s.dispatched == 1 && s.executed == 1 && s.credited == 1);
    teardown();
}

/* A signal sent before the fiber waits ends its first WaitSignal early. The
 * Sync notices bit 31 still set and parks again until the credit. */
void spurious()
{
    boot(1);
    CHECK(set_intake_paused(QUEUE, true));
    build_job(0);
    const uint32_t first = make_fiber(arg_of(R_SYNC, 0));
    CHECK(call(SIGNAL, first, 0) == 0);
    CHECK(rd(first) & 0x10);
    start_workers(1);
    CHECK(until([&] { return waiting() && (rd(first) & 0x18) == 0x08; }));
    CHECK(sync_rc.load() == ~0ull && rd(PORT + 4) == first && (rd(PORT + 0x14) >> 24) == 1);
    CHECK(set_intake_paused(QUEUE, false));
    CHECK(until([] { return fibers_done == 1; }));
    CHECK(sync_rc.load() == 0 && seen_marker.load() == mark_of(0) && L.done[0] < L.synced[0]);
    teardown();
}

/* One blocked Sync: fiber, then a plain thread, then a fiber again. Only the
 * thread takes an event-flag id; +4 keeps whatever the last waiter stored. */
void mixed()
{
    boot(1);
    start_workers(1);
    unsigned j = 0;
    auto fiber_round = [&](unsigned job) {
        const unsigned before = fibers_done;
        CHECK(set_intake_paused(QUEUE, true));
        build_job(job);
        sync_rc = ~0ull;
        const uint32_t ea = make_fiber(arg_of(R_SYNC, job));
        CHECK(until([] { return waiting(); }));
        CHECK(((a_of() >> 29) & 3) == 0 && rd(PORT + 4) == ea);
        const bool no_new_bit = rd(QUEUE + 0x88) == 0 || rd(QUEUE + 0x88) == 0x80000000u;
        CHECK(no_new_bit);
        CHECK(set_intake_paused(QUEUE, false));
        CHECK(until([&] { return fibers_done == before + 1; }));
        CHECK(sync_rc.load() == 0 && output_ok(job));
        CHECK(rd(PORT + 4) == ea);
        return ea;
    };
    const uint32_t f1 = fiber_round(j++);
    CHECK(bitmap_is(0)); /* a fiber never touched the event-flag bitmap */

    CHECK(set_intake_paused(QUEUE, true));
    build_job(j);
    std::atomic<uint64_t> thread_rc{~0ull};
    const unsigned tj = j++;
    std::thread blocked([&] {
        CHECK(call(PUSH, port_ea(0), desc_of(tj), 64, 0, 1) == 0);
        thread_rc = call(SYNC, port_ea(0), 0);
    });
    CHECK(until([] { return waiting(); }));
    CHECK(((a_of() >> 29) & 3) == 2 && rd(PORT + 4) == 1);
    CHECK(bitmap_is(0xC0000000u)); /* id 0 leaks, id 1 is the waiter */
    CHECK(set_intake_paused(QUEUE, false));
    blocked.join();
    CHECK(thread_rc.load() == 0 && output_ok(tj));
    CHECK(rd(PORT + 4) == 1 && f1 != 1); /* stale: the event id, not the old fiber */
    CHECK(bitmap_is(0x80000000u));

    const uint32_t f2 = fiber_round(j++);
    CHECK(f2 != f1 && bitmap_is(0x80000000u)); /* +4 is the new fiber EA; bitmap unchanged */
    const jq2_stats s = stats();
    CHECK(s.dispatched == 3 && s.executed == 3 && s.credited == 3);
    teardown();
}

/* Two fibers on two ports run Push+Sync back to back, so completion races
 * registration both ways; no wake is lost and every job is credited once. */
void race()
{
    boot(2, 8);
    make_port(1);
    for (unsigned port = 0; port < 2; ++port)
        for (unsigned i = 0; i < 100; ++i) build_job(port * 128 + i);
    make_fiber(arg_of(R_RACE, 0, 0));
    make_fiber(arg_of(R_RACE, 0, 1));
    start_workers(2);
    CHECK(until([] { return fibers_done == 2; }));
    const jq2_stats s = stats();
    CHECK(s.dispatched == 200 && s.executed == 200 && s.credited == 200);
    CHECK(bitmap_is(0)); /* fibers only */
    CHECK(!waiting(0) && !waiting(1));
    teardown();
}

/* Shutdown while a fiber is parked in Sync: BUSY from the worker control, the
 * queue and the port; no signal, count or credit is invented. */
void shutdown()
{
    boot(1);
    CHECK(set_intake_paused(QUEUE, true));
    build_job(0);
    const uint32_t first = make_fiber(arg_of(R_SYNC, 0));
    start_workers(1);
    /* Registered is not yet parked: the fiber is waiting once its state shows 8. */
    CHECK(until([&] { return waiting() && (rd(first) & 0x08); }));
    CHECK(((a_of() >> 29) & 3) == 0 && rd(PORT + 4) == first);
    const uint32_t a = a_of(), b = rd(PORT + 0x10), claim = rd(PORT + 0x14), fiber_state = rd(first);
    /* Snapshot once the worker has gone to sleep: its own idle bookkeeping then
     * is not what a failed Shutdown is compared against. */
    CHECK(until([] { return uint32_t(word(CONTROL + 0x200)) != 0; }));
    const uint64_t control = word(CONTROL + 0x200);

    CHECK(call(SHUTDOWN, CONTROL) == FIBER_BUSY);
    CHECK(jq2_queue_shutdown(QUEUE) == JQ2_ERR_BUSY);
    CHECK(jq2_port_destroy(port_ea(0)) == JQ2_ERR_BUSY);
    CHECK(call(SYNC, port_ea(0), 4) == uint64_t(int64_t(int32_t(JQ2_ERR_BUSY)))); /* the claim is held */
    CHECK(a_of() == a && rd(PORT + 0x10) == b && rd(PORT + 0x14) == claim && rd(PORT + 4) == first);
    CHECK(rd(first) == fiber_state && !(rd(first) & 0x10)); /* not signalled */
    CHECK(word(CONTROL + 0x200) == control);                /* the failed Shutdown changed nothing */
    CHECK(sync_rc.load() == ~0ull);
    const jq2_stats parked = stats();
    CHECK(parked.executed == 0 && parked.credited == 0);

    CHECK(set_intake_paused(QUEUE, false));
    CHECK(until([] { return fibers_done == 1; }));
    CHECK(sync_rc.load() == 0 && seen_marker.load() == mark_of(0));
    teardown();
}

void phase_run(const Phase& p, bool consumers_first)
{
    ph = p;
    want = 0; taken = 0; rel_n = 0;
    for (unsigned t = 0; t < p.jobs; ++t) acq[t] = 0;
    CHECK(call(SEM_ATTR, SATTR, 0x270000) == 0);
    wr16(SATTR + 6, 4);
    CHECK(call(SEM_INIT, SEM, SBUF, 0, SATTR) == 0);
    for (unsigned j = p.first; j < p.first + p.jobs; ++j) build_job(j);
    const unsigned before = fibers_done;
    auto make_consumers = [&] { for (unsigned i = 0; i < p.consumers; ++i) make_fiber(arg_of(R_CONSUMER)); };
    const uint64_t state_mask = 0xFFFF;
    if (consumers_first) {
        make_consumers();
        CHECK(until([&] { return (word(SEM + 0x10) & state_mask) == p.consumers; }));
        if (p.gate >= 0) hold_job(int(p.first) + p.gate);
        make_fiber(arg_of(R_PRODUCER));
        if (p.gate >= 0) {
            /* The long job is admitted and held, the others complete: the
             * producer's Sync still waits for it, so nothing is released. */
            CHECK(until([] { return gate_reached(); }));
            CHECK(until([&] { return L.done[p.first] != 0 && waiting(); }));
            CHECK(L.synced[p.first + p.gate] == 0 && rel_n == 0 && taken == 0);
            CHECK((word(SEM + 0x10) & state_mask) == p.consumers); /* consumers still blocked */
            CHECK(((a_of() >> 29) & 3) == 0);
            open_gate();
        }
    } else {
        make_fiber(arg_of(R_PRODUCER));
        CHECK(until([&] { return fibers_done == before + 1; }));
        /* Releases came first: every one is available before any Acquire. */
        CHECK(rel_n == p.jobs && (word(SEM + 0x10) & state_mask) == 0);
        CHECK(call(SEM_GET, SEM, SEM + 0x3000) == 0 && int32_t(rd(SEM + 0x3000)) == int32_t(p.jobs));
        make_consumers();
    }
    CHECK(until([&] { return fibers_done == before + 1 + p.consumers; }));

    CHECK(taken == p.jobs && rel_n == p.jobs);
    CHECK(call(SEM_GET, SEM, SEM + 0x3000) == 0 && rd(SEM + 0x3000) == 0);
    for (unsigned t = 0; t < p.jobs; ++t) {
        const unsigned j = p.first + t;
        CHECK(L.admit[j] < L.start[j] && L.start[j] < L.done[j] && L.done[j] < L.synced[j]);
        CHECK(L.synced[j] < L.released[j]);
        CHECK(acq[t] > L.released[j]); /* the t-th Acquire follows the t-th Release */
        CHECK(output_ok(j));
    }
}

/* B's producer property with real imports: a job's output precedes its Sync,
 * the Sync precedes the Release, and each Acquire follows the Release that
 * enabled it. Three shapes: Acquire parked first behind a held job on another
 * tag, Release before Acquire, and competing consumers. */
void c4()
{
    boot(2);
    start_workers(2);
    phase_run({0, 2, 2, 2, 1}, true);
    phase_run({2, 3, 1, 2, -1}, false);
    phase_run({5, 9, 3, 3, -1}, true);
    const jq2_stats s = stats();
    CHECK(s.dispatched == 14 && s.executed == 14 && s.credited == 14);
    CHECK(bitmap_is(0));
    teardown();
}

/* A fiber pushes onto a full ring (flags 1: do not return AGAIN, wait for room).
 * It parks in WaitSignal, so the one worker thread runs the second fiber, which
 * resumes the queue; the intake pass that frees a slot signals the first. The
 * order comes from the hooks and the pause, not from timing. */
void push_full()
{
    boot(1, 4);
    CHECK(set_intake_paused(QUEUE, true));
    for (unsigned j = 0; j < 5; ++j) build_job(j);
    for (unsigned j = 0; j < 4; ++j) CHECK(call(PUSH, port_ea(0), desc_of(j), 64, 0, 1) == 0);
    CHECK(rd(PORT + 0x10) == 4);
    make_fiber(arg_of(R_PUSHFULL, 4));
    make_fiber(arg_of(R_RESUME));
    start_workers(1);
    CHECK(until([] { return fibers_done == 2; }));
    CHECK(full_push_rc.load() == 0);
    CHECK(push_blocked.load() == 1 && blocked_when_resumed.load() == 1);
    CHECK(!pusher_done_when_resumed.load()); /* parked, not returned, while the second fiber ran */
    CHECK(call(SYNC, port_ea(0), 0) == 0);
    for (unsigned j = 0; j < 5; ++j) CHECK(output_ok(j));
    CHECK(L.admit[3] < L.admit[4]);
    CHECK(rd(PORT + 0x10) == 0 && (a_of() & 0x1FFFFFFFu) == 0);
    const jq2_stats s = stats();
    CHECK(s.pushed == 5 && s.dispatched == 5 && s.credited == 5);
    teardown();
}

/* ---- stops ------------------------------------------------------------------ */

void expect_stop(const std::function<void()>& child, std::initializer_list<const char*> needles)
{
    int fd[2];
    CHECK(pipe(fd) == 0);
    const pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]); dup2(fd[1], STDERR_FILENO); close(fd[1]);
        alarm(20);
        child();
        std::fprintf(stderr, "the stop did not happen\n");
        _exit(1);
    }
    close(fd[1]);
    std::string out; char buf[512]; ssize_t n;
    while ((n = read(fd[0], buf, sizeof buf)) > 0) out.append(buf, size_t(n));
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 70);
    CHECK(out.find("kind=jq2-wake") != std::string::npos);
    for (const char* needle : needles) {
        CHECK(out.find(needle) != std::string::npos);
        if (out.find(needle) == std::string::npos) std::fprintf(stderr, "missing %s in:\n%s\n", needle, out.c_str());
    }
}
/* A fiber whose state word is zero cannot wait: WaitSignal answers STAT, and
 * the executor stops rather than loop on bit 31 or invent a wake. */
void stop_wait()
{
    expect_stop([] {
        boot(1);
        CHECK(set_intake_paused(QUEUE, true));
        build_job(0);
        make_fiber(arg_of(R_BREAK, 0));
        start_workers(1);
        for (;;) std::this_thread::yield();
    }, {"reason=fiber-wait", "rc=0x8076000F"});
}

/* A fiber that cannot wait cannot park on a full ring either: the executor stops
 * instead of returning a code or spinning. */
void stop_push_wait()
{
    expect_stop([] {
        boot(1, 4);
        CHECK(set_intake_paused(QUEUE, true));
        for (unsigned j = 0; j < 5; ++j) build_job(j);
        for (unsigned j = 0; j < 4; ++j) CHECK(call(PUSH, port_ea(0), desc_of(j), 64, 0, 1) == 0);
        make_fiber(arg_of(R_BREAK_PUSH, 4));
        start_workers(1);
        for (;;) std::this_thread::yield();
    }, {"reason=push-room-wait", "rc=0x8076000F"});
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    vm_base = static_cast<uint8_t*>(mmap(nullptr, MEM, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    if (vm_base == MAP_FAILED) return 2;
    ppu_vm_size = MEM;
    ppu_hle_init();
    g_ppu_thread_entry_trampoline = trampoline;
    ppu_register_function(ENTRY, fiber_main);
    const std::string mode = argv[1];
    if (mode == "kind0") kind0();
    else if (mode == "spurious") spurious();
    else if (mode == "mixed") mixed();
    else if (mode == "race") race();
    else if (mode == "shutdown") shutdown();
    else if (mode == "c4") c4();
    else if (mode == "stop-wait") stop_wait();
    else if (mode == "push-full") push_full();
    else if (mode == "stop-push-wait") stop_push_wait();
    else return 2;
    std::printf("%s %s\n", failures ? "FAIL" : "PASS", argv[1]);
    return failures ? 1 : 0;
}

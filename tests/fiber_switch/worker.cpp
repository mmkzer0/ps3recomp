/* Synthetic PPU functions dispatched by the real loader, on real fiber stacks. */
#include "ppu_context.h"
#include "sys_ppu_thread.h"
#include "memory/reservation.h"
#include "ppu_dispatch_state.h"
#include "libs/spurs/cellFiber.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ps3emu/lifter_abi.h"   /* the lifted-code ABI this fixture is written in */
/* The generated loader's three-field registry entry. This fixture registers
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
void ppu_prof_stamp(void*, unsigned);
int ppu_prof_snapshot(int, unsigned*, unsigned*, const char**);
}
constexpr uint32_t MEM = 0x400000, CONTROL = 0x10000, FIBER = 0x20000, ATTR = 0x30000, FATTR = 0x31000;
constexpr uint32_t INIT = 0x55870804, ATTR_INIT = 0xC11F8056, WAIT = 0x6C164B3B;
constexpr uint32_t WATTR = 0x68BA4568, INITIALIZE = 0xF2CCAD4F, CREATE = 0xBFCA88D3;
constexpr uint32_t RUN = 0x1E7A247A, WAKE = 0xBABF714B, SIGNAL = 0x5D3992DD;
constexpr uint32_t SHUTDOWN = 0xC04E2438, FINALIZE = 0xA27C95CA;
constexpr uint32_t ENTRY_A = 0x1000, ENTRY_B = 0x1100, LEAF_A = 0x1200, LEAF_B = 0x1300;
std::atomic<unsigned> failures{0}, phase{0};
std::atomic<unsigned> entered{0};
std::atomic<unsigned> profiled{0};
std::atomic<bool> profiles_sampled{false};
#define CHECK(c) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
uint32_t rd(uint32_t ea) { return __builtin_bswap32(std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(vm_base + ea)).load()); }
void wr(uint32_t ea, uint32_t x) { x = __builtin_bswap32(x); std::memcpy(vm_base + ea, &x, 4); }
uint64_t word(uint32_t ea) { return __builtin_bswap64(std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(vm_base + ea)).load()); }
uint64_t err(uint32_t x) { return uint64_t(int64_t(int32_t(x))); }
ppu_context& caller()
{
    static std::atomic<unsigned> slots{0};
    static thread_local const unsigned slot = slots.fetch_add(1);
    CHECK(slot < 32);
    auto& ctx = g_ppu_threads[slot].ctx;
    ctx.thread_id = slot + 1; ctx.gpr[1] = 0x40000 + 0x1000 * slot; ctx.gpr[13] = 0x60000 + 0x1000 * slot;
    return ctx;
}
uint64_t call(uint32_t nid, uint64_t a=0, uint64_t b=0, uint64_t c=0, uint64_t d=0,
              uint64_t e=0, uint64_t f=0, uint64_t g=0, uint64_t h=0)
{
    auto& ctx = caller();
    ctx.gpr[3]=a; ctx.gpr[4]=b; ctx.gpr[5]=c; ctx.gpr[6]=d;
    ctx.gpr[7]=e; ctx.gpr[8]=f; ctx.gpr[9]=g; ctx.gpr[10]=h;
    auto* previous = g_active_ctx; g_active_ctx = &ctx;
    ps3_hle_call(nid, &ctx);
    g_active_ctx = previous;
    return ctx.gpr[3];
}
template<class P> bool until(P p)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!p()) { if (std::chrono::steady_clock::now() >= deadline) return false; std::this_thread::yield(); }
    return true;
}
void initialize()
{
    CHECK(call(INIT, caller().gpr[13] + 0x100) == 0x100);
    CHECK(call(WATTR, ATTR, 0x360001) == 0);
    CHECK(call(INITIALIZE, CONTROL, ATTR) == 0);
    CHECK(word(CONTROL + 0x200) == (1ull << 38) && rd(CONTROL + 0x208) != 0);
    CHECK(rd(CONTROL + 0x28C) == 0x360001 && rd(CONTROL + 0x288) == 0);
    CHECK(rd(CONTROL + 0xE8) == CONTROL + 0xE8 && rd(CONTROL + 0xEC) == CONTROL + 0xE8);
}
void create(unsigned i, uint64_t argument)
{
    CHECK(call(ATTR_INIT, FATTR, 0x360001) == 0);
    wr(0x8000 + 8*i, i ? ENTRY_B : ENTRY_A); wr(0x8004 + 8*i, 0);
    uint8_t saved_attr[256];
    std::memcpy(saved_attr, vm_base + FATTR, sizeof saved_attr);
    vm_base[FATTR + 256] = 0xCC;
    vm_base[FIBER + 0x400*i - 1] = 0xA5;
    vm_base[FIBER + 0x400*i + CELL_FIBER_PPU_SIZE] = 0x5A;
    CHECK(call(CREATE, CONTROL, FIBER + 0x400*i, 0x8000 + 8*i, argument, 0,
               0x80000 + 0x10000*i, 0x10000, FATTR) == 0);
    CHECK(std::memcmp(saved_attr, vm_base + FATTR, sizeof saved_attr) == 0 && vm_base[FATTR + 256] == 0xCC);
    CHECK(vm_base[FIBER + 0x400*i - 1] == 0xA5 && vm_base[FIBER + 0x400*i + CELL_FIBER_PPU_SIZE] == 0x5A);
    CHECK(rd(FIBER + 0x400*i + 0x10) == 1);
    CHECK(word(FIBER + 0x400*i + 0x18) == CONTROL);
    CHECK(word(FIBER + 0x400*i + 0x308) == argument);
}
void direct_entry(ppu_context* ctx)
{
    CHECK(ctx->gpr[3] == 0x1122334455667788ull);
    CHECK(g_active_ctx == ctx && ctx->thread_id == caller().thread_id);
    entered.fetch_add(1);
    CHECK(until([] { return entered == 2; })); /* both real PPU workers execute */
    ps3_hle_call(0x5D9A7034, ctx);
    CHECK(ctx->gpr[3] == FIBER || ctx->gpr[3] == FIBER + 0x400);
    for (unsigned i = 0; i < 1000; ++i) ppu_prof_stamp(ctx, 0xB000u + unsigned(ctx->thread_id));
    profiled.fetch_add(1);
    CHECK(until([] { return profiles_sampled.load(); }));
    phase.fetch_add(1); ctx->gpr[3] = 0;
}
void trampoline(ppu_context* ctx)
{
    ctx->ctr = rd(uint32_t(ctx->cia));
    ps3_indirect_call(ctx);
}
struct ExitGate { std::mutex mu; std::condition_variable cv; bool reached=false, release=false; } gate;
void exit_hook(void*, uint32_t control)
{
    CHECK(control == CONTROL);
    std::unique_lock lk(gate.mu); gate.reached=true; gate.cv.notify_all(); gate.cv.wait(lk, [] { return gate.release; });
}
void lifecycle()
{
    /* Stable registered-slot metadata precedes both worker threads. The slots are
     * registered by hand, not created, so they carry no thread id; the profiler
     * samples only PCs while the workers update their breadcrumbs. */
    for (unsigned i = 0; i < 3; ++i) {
        g_ppu_threads[i].state = PPU_THREAD_STATE_RUNNING;
        std::snprintf(g_ppu_threads[i].name, sizeof g_ppu_threads[i].name, "worker-%u", i + 1);
    }
    initialize();
    ps3_fiber_set_worker_exit_hook(exit_hook, nullptr);
    std::atomic<unsigned> returned{0};
    std::thread workers[2];
    for (auto& worker : workers) worker = std::thread([&] { CHECK(call(RUN, CONTROL) == 0); ++returned; });
    CHECK(until([] { return uint32_t(word(CONTROL + 0x200)) == 3; }));
    CHECK(returned == 0 && (word(CONTROL + 0x200) >> 56) == 2 && rd(CONTROL + 0x288) == 0xC0000000);
    create(0, 0x1122334455667788ull); create(1, 0x1122334455667788ull);
    CHECK(phase == 0); /* Create did not execute a fiber or wake idle workers. */
    CHECK(call(FINALIZE, CONTROL) == err(0x8076000F));
    CHECK(call(SHUTDOWN, CONTROL) == err(0x8076000A));
    CHECK(call(WAKE, CONTROL) == 0);
    CHECK(until([] {
        for (int i = 1; i <= 2; ++i) {
            unsigned tid, pc; const char* name;
            CHECK(ppu_prof_snapshot(i, &tid, &pc, &name) && name);
        }
        return profiled == 2;
    }));
    for (int i = 1; i <= 2; ++i) {
        unsigned tid, pc; const char* name;
        CHECK(ppu_prof_snapshot(i, &tid, &pc, &name) && pc == 0xB000u + unsigned(i + 1));
    }
    profiles_sampled = true;
    CHECK(until([] { return phase == 2 && rd(FIBER) == 0x40 && rd(FIBER + 0x400) == 0x40 &&
                           uint32_t(word(CONTROL + 0x200)) == 3; }));
    CHECK(returned == 0);
    CHECK(call(SHUTDOWN, CONTROL) == 0);
    CHECK(until([] { std::lock_guard lk(gate.mu); return gate.reached; }));
    CHECK(call(FINALIZE, CONTROL) == err(0x8076000A));
    { std::lock_guard lk(gate.mu); gate.release=true; } gate.cv.notify_all();
    for (auto& worker : workers) worker.join();
    CHECK(returned == 2 && rd(CONTROL + 0x288) == 0 && (word(CONTROL + 0x200) >> 56) == 0);
    CHECK(call(WAKE, CONTROL) == err(0x8076000F));
    CHECK(call(FINALIZE, CONTROL) == 0);
    ps3_fiber_set_worker_exit_hook(nullptr, nullptr);
}
void leaf_a(ppu_context* ctx)
{
    CHECK(*ps3_ppu_dispatch_depth_slot() == 2 && *ps3_ppu_vcall_sp_slot() == 2);
    CHECK(ctx->thread_id == caller().thread_id && g_active_ctx == ctx);
    ctx->gpr[29] = 0xAABBCCDDEEFF0011ull;
    const auto value = resv_lwarx32(ctx, 0xA000);
    ps3_hle_call(WAIT, ctx); /* consumes a coalesced early signal */
    CHECK(ctx->gpr[3] == 0 && !(rd(FIBER) & 0x10));
    phase = 1;
    ps3_hle_call(WAIT, ctx); /* must actually suspend; another fiber runs */
    CHECK(ctx->gpr[3] == 0 && phase == 2);
    CHECK(*ps3_ppu_dispatch_depth_slot() == 2 && *ps3_ppu_vcall_sp_slot() == 2);
    CHECK((*ps3_ppu_vcall_stack_slot())[0] == ENTRY_A);
    CHECK((*ps3_ppu_vcall_stack_slot())[1] == LEAF_A);
    CHECK(ctx->gpr[29] == 0xAABBCCDDEEFF0011ull && g_active_ctx == ctx);
    CHECK(resv_stcx32(ctx, 0xA000, value + 1));
    phase = 3;
}
void leaf_b(ppu_context* ctx)
{
    CHECK(phase == 1 && *ps3_ppu_dispatch_depth_slot() == 2 && *ps3_ppu_vcall_sp_slot() == 2);
    CHECK((*ps3_ppu_vcall_stack_slot())[0] == ENTRY_B);
    ctx->gpr[29] = 0x123456789ABCDEF0ull;
    const auto value = resv_lwarx32(ctx, 0xA080);
    phase = 2;
    ps3_hle_call(WAIT, ctx);
    CHECK(ctx->gpr[3] == 0 && phase == 3);
    CHECK(*ps3_ppu_dispatch_depth_slot() == 2 && *ps3_ppu_vcall_sp_slot() == 2);
    CHECK((*ps3_ppu_vcall_stack_slot())[0] == ENTRY_B && (*ps3_ppu_vcall_stack_slot())[1] == LEAF_B);
    CHECK(ctx->gpr[29] == 0x123456789ABCDEF0ull && g_active_ctx == ctx);
    CHECK(resv_stcx32(ctx, 0xA080, value + 1));
    phase = 4;
}
void nested_a(ppu_context* ctx) { ctx->ctr = LEAF_A; ps3_indirect_call(ctx); ctx->gpr[3] = 0; }
void nested_b(ppu_context* ctx) { ctx->ctr = LEAF_B; ps3_indirect_call(ctx); ctx->gpr[3] = 0; }
void signal_dispatch()
{
    initialize(); create(0, 1); create(1, 2);
    CHECK(call(SIGNAL, FIBER, 0) == 0 && call(SIGNAL, FIBER, MEM + 4) == 0);
    CHECK(rd(FIBER) == 0x11);
    std::thread worker([] { CHECK(call(RUN, CONTROL) == 0); CHECK(*ps3_ppu_dispatch_depth_slot() == 0 && *ps3_ppu_vcall_sp_slot() == 0); });
    CHECK(until([] { return phase == 2 && (rd(FIBER) & 8) && (rd(FIBER + 0x400) & 8); }));
    CHECK(call(SHUTDOWN, CONTROL) == err(0x8076000A));
    CHECK((rd(FIBER) & 8) && !(rd(FIBER) & 0x10));
    wr(0xA100, 0x12345678);
    CHECK(call(SIGNAL, FIBER, 0xA100) == 0 && rd(0xA100) == 0x12345678);
    CHECK(until([] { return phase == 3 && rd(FIBER) == 0x40 && (rd(FIBER + 0x400) & 8); }));
    CHECK(call(SHUTDOWN, CONTROL) == err(0x8076000A));
    CHECK(call(SIGNAL, FIBER + 0x400, 0) == 0);
    CHECK(until([] { return phase == 4 && rd(FIBER) == 0x40 && rd(FIBER + 0x400) == 0x40 &&
                           rd(CONTROL + 0xE8) == CONTROL + 0xE8; }));
    CHECK(rd(CONTROL + 0xE8) == CONTROL + 0xE8);
    CHECK(call(SHUTDOWN, CONTROL) == 0); worker.join(); CHECK(call(FINALIZE, CONTROL) == 0);
}
void raw_init()
{
    auto& ctx = caller();
    ctx.gpr[13] = 0x123456789ABCDEF0ull;
    ctx.gpr[3] = 0xFEDCBA9876543210ull;
    g_active_ctx=&ctx; ps3_hle_call(INIT, &ctx); g_active_ctx=nullptr;
    CHECK(ctx.gpr[3] == uint64_t(0xFEDCBA9876543210ull - 0x123456789ABCDEF0ull));
    ctx.gpr[13]=0x200; ctx.gpr[3]=0x100;
    g_active_ctx=&ctx; ps3_hle_call(INIT, &ctx); g_active_ctx=nullptr;
    CHECK(ctx.gpr[3] == 0xFFFFFFFFFFFFFF00ull);
    wr(0x120, 0x23480);
    ctx.gpr[13] = 0x200;
    g_active_ctx = &ctx; ps3_hle_call(0x5D9A7034, &ctx); g_active_ctx = nullptr;
    CHECK(ctx.gpr[3] == 0x23480); /* Self uses the low-word TLS offset. */
    ctx.gpr[3] = 0x1FF;
    g_active_ctx = &ctx; ps3_hle_call(INIT, &ctx); g_active_ctx = nullptr;
    CHECK(ctx.gpr[3] == UINT64_MAX);
    g_active_ctx = &ctx; ps3_hle_call(WAIT, &ctx); g_active_ctx = nullptr;
    CHECK(ctx.gpr[3] == err(0x80760020)); /* low word -1 is NOSYSINIT */
}
void stopped(unsigned scenario)
{
    int pipes[2]; CHECK(pipe(pipes) == 0);
    const pid_t pid = fork();
    if (pid == 0) {
        close(pipes[0]); dup2(pipes[1], STDERR_FILENO); close(pipes[1]);
        initialize();
        if (scenario == 0) {
            CHECK(call(ATTR_INIT, FATTR, 0x330000) == 0);
            wr(FATTR + 0x30, 0x12345678);
            std::memset(vm_base + FIBER, 0xA5, CELL_FIBER_PPU_SIZE);
            call(CREATE, CONTROL, FIBER, 0x8000, 0, 0, 0x80000, 0x10000, FATTR);
        } else if (scenario == 1) {
            const uint64_t state = __builtin_bswap64((32ull << 56) | (1ull << 38));
            std::memcpy(vm_base + CONTROL + 0x200, &state, 8);
            wr(CONTROL + 0x288, 0xFFFFFFFF);
            call(RUN, CONTROL);
        } else if (scenario == 2) {
            std::memset(vm_base + MEM - 128, 0xA5, 128);
            call(ATTR_INIT, MEM - 128, 0x330000);
        } else if (scenario == 3) {
            call(SHUTDOWN, CONTROL);
            call(FINALIZE, CONTROL);
            CHECK(call(WATTR, ATTR, 0x360001) == 0);
            vm_base[ATTR + 0x10] = 1;
            call(INITIALIZE, CONTROL, ATTR);
        }
        _exit(1);
    }
    close(pipes[1]); std::string output; char buffer[512]; ssize_t n;
    while ((n = read(pipes[0], buffer, sizeof buffer)) > 0) output.append(buffer, n);
    close(pipes[0]); int status; waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 70);
    CHECK(output.find(scenario == 2 ? "kind=hle-guest-range" : "kind=fiber-unsupported") != std::string::npos);
    if (scenario == 0) {
        CHECK(output.find("field=+0x30") != std::string::npos && output.find("reason=attribute-field") != std::string::npos);
        CHECK(rd(FIBER) == 0xA5A5A5A5);
    } else if (scenario == 1) {
        CHECK(output.find("field=worker-slot") != std::string::npos && output.find("reason=worker-capacity") != std::string::npos);
        CHECK((word(CONTROL + 0x200) >> 56) == 33);
    } else if (scenario == 3) {
        CHECK(output.find("field=+0x10") != std::string::npos && output.find("reason=auto-check") != std::string::npos);
    } else {
        for (unsigned i = 0; i < 128; ++i) CHECK(vm_base[MEM - 128 + i] == 0xA5);
    }
}
void attributes()
{
    std::memset(vm_base + ATTR - 8, 0xA5, 0x190);
    CHECK(call(WATTR, ATTR, 0x360001) == 0);
    CHECK(word(ATTR) == 0x44007515864E5F4Cull && rd(ATTR + 8) == 0x360001 && rd(ATTR + 0x108) == 0x360001);
    CHECK(vm_base[ATTR - 1] == 0xA5 && vm_base[ATTR + 0x180] == 0xA5);
    CHECK(call(ATTR_INIT, FATTR, 0x330000) == 0);
    CHECK(word(FATTR) == 0xF784E782F9EA9359ull && rd(FATTR + 8) == 0x330000);
    CHECK(std::strcmp(reinterpret_cast<char*>(vm_base + FATTR + 0x10), "*NO NAME*") == 0);
    CHECK(call(WATTR, 0, 0) == err(0x80760011));
    CHECK(call(WATTR, ATTR + 1, 0) == err(0x80760010));
    CHECK(call(ATTR_INIT, FATTR + 1, 0) == err(0x80760010));
    CHECK(word(FATTR) == 0xF784E782F9EA9359ull);
    CHECK(call(WAIT) == err(0x80760020));
    std::memset(vm_base + CONTROL - 1, 0xA5, 0x302);
    CHECK(call(INITIALIZE, 0, ATTR + 1) == err(0x80760011));
    CHECK(call(INITIALIZE, CONTROL + 1, ATTR + 1) == err(0x80760010));
    CHECK(rd(CONTROL) == 0xA5A5A5A5);
    CHECK(call(INITIALIZE, CONTROL, ATTR) == err(0x80760020));
    CHECK(rd(CONTROL) == 0 && rd(CONTROL + 0x208) != 0);
    CHECK(vm_base[CONTROL - 1] == 0xA5 && vm_base[CONTROL + 0x300] == 0xA5);
    CHECK(call(CREATE, 0, 0, 0, 0, 0, 0, 0, FATTR + 1) == err(0x80760010));
    CHECK(call(CREATE, 0, FIBER, 1, 0, 0, 0x80000, 0x10000, FATTR) == err(0x80760011));
    CHECK(call(CREATE, CONTROL, FIBER, 1, 0, 4, 0x80000, 0x10000, FATTR) == err(0x80760002));
    CHECK(call(CREATE, CONTROL, FIBER, 1, 0, 0, 0x80000, 0x10000, FATTR) == err(0x80760020));
    wr(FATTR, 0);
    CHECK(call(CREATE, CONTROL, FIBER, 1, 0, 0, 0x80000, 0x10000, FATTR) == err(0x80760002));
    CHECK(call(INIT, caller().gpr[13] + 0x100) == 0x100);
    CHECK(call(INITIALIZE, CONTROL, ATTR + 1) == err(0x80760010));
    CHECK(call(INITIALIZE, CONTROL, ATTR) == 0);
    CHECK(call(WAIT) == err(0x80760009));
    CHECK(call(SIGNAL, 0, 0) == err(0x80760011));
    CHECK(call(SIGNAL, FIBER + 1, 0) == err(0x80760010));
    CHECK(call(SIGNAL, FIBER, 0) == err(0x8076000F));
    CHECK(call(RUN, 0) == err(0x80760011) && call(RUN, CONTROL + 1) == err(0x80760010));
    CHECK(call(FINALIZE, CONTROL) == err(0x8076000F));
    CHECK(call(SHUTDOWN, CONTROL) == 0 && call(FINALIZE, CONTROL) == 0);

    /* +0x11=1 initializes OK and +0x168 stays 0 */
    CHECK(call(WATTR, ATTR, 0x360001) == 0);
    vm_base[ATTR + 0x11] = 1;
    std::memset(vm_base + CONTROL + 0x168, 0xCC, 8);
    CHECK(call(INITIALIZE, CONTROL, ATTR) == 0);
    CHECK(word(CONTROL + 0x168) == 0);
    CHECK(call(SHUTDOWN, CONTROL) == 0 && call(FINALIZE, CONTROL) == 0);

    /* +0x14=1000 gives expected ticks at +0x138 with +0x140=0 */
    CHECK(call(WATTR, ATTR, 0x360001) == 0);
    wr(ATTR + 0x14, 1000);
    std::memset(vm_base + CONTROL + 0x138, 0xCC, 16);
    CHECK(call(INITIALIZE, CONTROL, ATTR) == 0);
    constexpr uint64_t expected_ticks = 1000ull * 79800000ull / 1000000ull;
    CHECK(word(CONTROL + 0x138) == expected_ticks);
    CHECK(word(CONTROL + 0x140) == 0);
    CHECK(call(SHUTDOWN, CONTROL) == 0 && call(FINALIZE, CONTROL) == 0);

    /* garbage in unread bytes is accepted */
    CHECK(call(WATTR, ATTR, 0x360001) == 0);
    std::memset(vm_base + ATTR + 0x0C, 0xA5, 0x180 - 0x0C);
    vm_base[ATTR + 0x10] = 0;
    vm_base[ATTR + 0x11] = 1;
    wr(ATTR + 0x14, 1000);
    wr(ATTR + 0x108, 0x360001);
    CHECK(call(INITIALIZE, CONTROL, ATTR) == 0);
    CHECK(word(CONTROL + 0x138) == expected_ticks);
    CHECK(word(CONTROL + 0x140) == 0);
    CHECK(word(CONTROL + 0x168) == 0);
    CHECK(call(SHUTDOWN, CONTROL) == 0 && call(FINALIZE, CONTROL) == 0);

    for (unsigned scenario = 0; scenario < 4; ++scenario) stopped(scenario);
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    vm_base=static_cast<uint8_t*>(mmap(nullptr,MEM,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANON,-1,0));
    if (vm_base == MAP_FAILED) return 2;
    ppu_vm_size=MEM; ppu_hle_init(); g_ppu_thread_entry_trampoline=trampoline;
    /* A title-style link must select the real loader, never the game-less
     * host's weak NULL hooks. Actual nested calls below check their contents. */
    CHECK(ps3_ppu_dispatch_depth_slot() && ps3_ppu_vcall_sp_slot());
    CHECK(ps3_ppu_vcall_stack_slot() && *ps3_ppu_vcall_stack_slot());
    if (failures) return 1;
    ppu_register_function(ENTRY_A, direct_entry);
    ppu_register_function(ENTRY_B, direct_entry);
    const std::string mode=argv[1];
    if (mode=="init") raw_init(); else if (mode=="attributes") attributes();
    else if (mode=="lifecycle") lifecycle();
    else if (mode=="dispatch") {
        ppu_register_function(ENTRY_A,nested_a); ppu_register_function(ENTRY_B,nested_b);
        ppu_register_function(LEAF_A,leaf_a); ppu_register_function(LEAF_B,leaf_b); signal_dispatch();
    } else return 2;
    std::printf("%s %s\n",failures ? "FAIL":"PASS",argv[1]); return failures ? 1:0;
}

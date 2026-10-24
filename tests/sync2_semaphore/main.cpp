/* Synthetic integration tests: firmware HLE adapters, guest bytes, real waits. */
#include "ppu/ppu_context.h"
#include "syscalls/sys_ppu_thread.h"
#include "spurs/cellFiber.h"
#include "memory/reservation.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
uint8_t* vm_base;
extern uint32_t ppu_vm_size;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ppu_hle_init(void);
void ps3_hle_call(uint32_t, ppu_context*);
uint64_t vm_read64(uint64_t ea) { uint64_t v; std::memcpy(&v, vm_base + ea, 8); return __builtin_bswap64(v); }
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) { std::abort(); }
uint32_t ppu_prof_resolve_host(void*) { return 0; }
struct CellSyncQueue;
extern "C" int32_t cellSyncQueueInitialize(CellSyncQueue* queue, void* buffer, uint32_t size, uint32_t depth);
}
void vm_write64(uint64_t ea, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(vm_base + ea, &v, 8); }

constexpr uint32_t MEM = 0x80000, SEM = 0x1000, ATTR = 0x2001, BUF = 0x3000, OUT = 0x3801;
constexpr uint32_t ATTRIBUTE = 0x2D77FE17, INITIALIZE = 0xC5DEE254, GET = 0x4E2EE031;
constexpr uint32_t ACQUIRE = 0xD1B0D146, RELEASE = 0x0C2983AC, TRY = 0x5E4B0F87;
int failures;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
uint64_t call(uint32_t nid, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0)
{
    /* PPU dispatch contexts belong to distinct thread-table slots (the
     * profiler's container-of contract). Fibers retain their private context
     * and stack instead of aliasing that thread's suspended HLE frame. */
    static std::atomic<unsigned> slots{0};
    static thread_local const unsigned slot = slots.fetch_add(1);
    CHECK(slot < 64);
    ppu_context local{};
    ppu_context& ctx = g_active_ctx ? local : g_ppu_threads[slot].ctx;
    ctx.gpr[1] = g_active_ctx ? g_active_ctx->gpr[1] : 0x40000 + 0x1000 * slot;
    ctx.thread_id = slot + 1;
    ctx.gpr[13] = g_active_ctx ? g_active_ctx->gpr[13] : 0x3A00;
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d;
    ctx.gpr[7] = 0x77777777; ctx.gpr[10] = 0xAAAAAAAA;
    auto* saved_active = g_active_ctx;
    g_active_ctx = &ctx;
    ps3_hle_call(nid, &ctx);
    g_active_ctx = saved_active;
    CHECK(ctx.gpr[7] == 0x77777777 && ctx.gpr[10] == 0xAAAAAAAA);
    return ctx.gpr[3];
}
uint64_t error(uint32_t e) { return uint64_t(int64_t(int32_t(e))); }
uint32_t rd(uint32_t ea) { uint32_t v; std::memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v); }
void wr(uint32_t ea, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(vm_base + ea, &v, 4); }
void wr16(uint32_t ea, uint16_t v) { v = __builtin_bswap16(v); std::memcpy(vm_base + ea, &v, 2); }
uint64_t state() { return __builtin_bswap64(std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(vm_base + SEM + 0x10)).load()); }
template<class P> bool until(P p)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!p()) { if (std::chrono::steady_clock::now() >= end) return false; std::this_thread::yield(); }
    return true;
}
int32_t count() { CHECK(call(GET, SEM, OUT) == 0); return int32_t(rd(OUT)); }
void init(int32_t value, unsigned max = 1)
{
    CHECK(call(ATTRIBUTE, ATTR, 0x270000) == 0);
    wr16(ATTR + 6, max);
    CHECK(call(INITIALIZE, SEM, max == 1 ? 0 : BUF, uint32_t(value), ATTR) == 0);
}
void layout()
{
    std::memset(vm_base + ATTR - 1, 0xA5, 130);
    CHECK(call(ATTRIBUTE, ATTR, 0x300123) == 0);
    CHECK(vm_base[ATTR - 1] == 0xA5 && vm_base[ATTR + 128] == 0xA5);
    CHECK(rd(ATTR) == 0x300123 && rd(ATTR + 4) == 0x010F0001);
    CHECK(std::strcmp(reinterpret_cast<char*>(vm_base + ATTR + 8), "CellSync2Semaphore") == 0);
    for (unsigned i = 40; i < 128; ++i) CHECK(vm_base[ATTR + i] == 0);
    init(-1);
    CHECK(count() == -1 && rd(SEM + 8) == SEM + 0x48);
    CHECK(rd(SEM) == 0x80000000 && rd(SEM + 4) == 0x80000000);
    CHECK(rd(SEM + 0xC) == 0x00020000 && rd(SEM + 0x18) == 0x270000);
    CHECK(rd(SEM + 0x20) == 0x010F0001);
    CHECK(call(GET, SEM, 0) == error(0x80410C11));
    CHECK(call(GET, SEM + 1, 0) == error(0x80410C11));
    CHECK(call(GET, SEM + 1, OUT) == error(0x80410C10));
    CHECK(call(ACQUIRE, 0, 0, 1) == error(0x80410C11));
    CHECK(call(RELEASE, SEM + 1, 0, 1) == error(0x80410C10));
    CHECK(call(INITIALIZE, SEM + 1, 1, 0, 0) == error(0x80410C11));
    CHECK(call(INITIALIZE, SEM, 1, 0, ATTR) == error(0x80410C10));
    wr(ATTR, 0x260000);
    CHECK(call(INITIALIZE, SEM, BUF, 0, ATTR) == error(0x80410C02));
    wr(ATTR, 0x270000);
    CHECK(call(INITIALIZE, SEM, BUF, 0, ATTR) == error(0x80410C02));
    std::memset(vm_base + SEM, 0xA5, 129);
    CHECK(call(INITIALIZE, SEM, 0, 0x800000, ATTR) == error(0x80410C02));
    for (unsigned i = 0; i < 128; ++i) CHECK(vm_base[SEM + i] == 0);
    CHECK(vm_base[SEM + 128] == 0xA5);
    wr16(ATTR + 6, 2);
    CHECK(call(INITIALIZE, SEM, 0, 0, ATTR) == error(0x80410C11));
    wr16(ATTR + 4, 0x20);
    CHECK(call(INITIALIZE, SEM, BUF, 0, ATTR) == error(0x80410C02));
    /* cellSyncQueueInitialize order: NULL pointer -> ALIGN -> INVAL */
    CHECK(cellSyncQueueInitialize(nullptr, (void*)(uintptr_t)0x1000, 16, 1) == int32_t(0x80410111));
    CHECK(cellSyncQueueInitialize((CellSyncQueue*)(uintptr_t)0x1000, nullptr, 16, 1) == int32_t(0x80410111));
    /* Unaligned queue or buffer with invalid depth/size must return ALIGN (0x80410110), not INVAL (0x80410102) */
    CHECK(cellSyncQueueInitialize((CellSyncQueue*)(uintptr_t)0x1004, (void*)(uintptr_t)0x2000, 16, 0) == int32_t(0x80410110));
    CHECK(cellSyncQueueInitialize((CellSyncQueue*)(uintptr_t)0x1000, (void*)(uintptr_t)0x2004, 16, 0) == int32_t(0x80410110));
    CHECK(cellSyncQueueInitialize((CellSyncQueue*)(uintptr_t)0x1004, (void*)(uintptr_t)0x2000, 15, 1) == int32_t(0x80410110));
    CHECK(cellSyncQueueInitialize((CellSyncQueue*)(uintptr_t)0x1000, (void*)(uintptr_t)0x2004, 15, 1) == int32_t(0x80410110));
    /* Valid alignment with invalid depth or size returns INVAL */
    CHECK(cellSyncQueueInitialize((CellSyncQueue*)(uintptr_t)0x1000, (void*)(uintptr_t)0x2000, 16, 0) == int32_t(0x80410102));
    CHECK(cellSyncQueueInitialize((CellSyncQueue*)(uintptr_t)0x1000, (void*)(uintptr_t)0x2000, 15, 1) == int32_t(0x80410102));
    /* Valid parameters initialize successfully */
    CHECK(cellSyncQueueInitialize((CellSyncQueue*)(uintptr_t)0x1000, (void*)(uintptr_t)0x2000, 16, 1) == 0);
}
void counts()
{
    init(2);
    CHECK(call(TRY, SEM, 3, 0) == error(0x80410C01) && count() == 2);
    CHECK(call(ACQUIRE, SEM, 0, 0) == 0 && count() == 2);
    CHECK(call(ACQUIRE, SEM, 2, 0) == 0 && count() == 0);
    CHECK(call(RELEASE, SEM, 0x800000, 0) == error(0x80410C02));
    CHECK(call(ACQUIRE, SEM, 0x800000, 0) == error(0x80410C02));
    init(0x7FFFFF);
    CHECK(call(RELEASE, SEM, 1, 0) == error(0x80410C0F) && count() == 0x7FFFFF);
    init(-0x800000);
    CHECK(count() == -0x800000);
    CHECK(call(RELEASE, SEM, 0x7FFFFF, 0) == 0 && count() == -1);
    CHECK(call(RELEASE, SEM, 2, 0) == 0 && count() == 1);
    init(0, 2);
    CHECK(call(ACQUIRE, SEM, 2, 0) == error(0x80410C02));
    wr16(SEM + 0x20, 2);
    CHECK(call(ACQUIRE, SEM, 0, 0) == error(0x80410C12));
}
void debt()
{
    init(2);
    for (unsigned round = 0; round < 8; ++round) {
        if (round) CHECK(call(RELEASE, SEM, 2, 0) == 0);
        std::atomic<bool> done{false};
        std::thread t([&] {
            CHECK(call(ACQUIRE, SEM, 3, 0) == 0);
            CHECK(rd(OUT + 8) == 0x12340000u + round);
            done = true;
        });
        CHECK(until([] { return uint32_t(state()) == 0x40000001; }));
        CHECK(!done && count() == 0);
        CHECK(call(ACQUIRE, SEM, 1, 0) == error(0x80410C0A));
        ppu_context reserve{};
        resv_lwarx32(&reserve, SEM + 0x78);
        wr(OUT + 8, 0x12340000u + round);
        CHECK(call(RELEASE, SEM, 1, 0) == 0);
        t.join();
        CHECK(done && count() == 0 && uint32_t(state()) == 0);
        CHECK(resv_stcx32(&reserve, SEM + 0x78, 0) == 0);
    }
    CHECK(vm_read64(SEM) == 0x8000000080000000ull); /* four FIFO wraps */
    CHECK((vm_read64(SEM + 0x48) >> 52 & 0x1FF) == 4);
    CHECK((vm_read64(SEM + 0x50) >> 52 & 0x1FF) == 4);
}
void multi()
{
    init(0, 4);
    std::atomic<unsigned> done{0};
    std::thread threads[4];
    for (auto& t : threads) t = std::thread([&] { CHECK(call(ACQUIRE, SEM, 1, 0) == 0); ++done; });
    CHECK(until([] { return (state() & 0xFFFF) == 4; }));
    CHECK(call(ACQUIRE, SEM, 1, 0) == error(0x80410C04));
    CHECK(count() == 0);
    CHECK(call(RELEASE, SEM, 2, 0) == 0);
    CHECK(until([&] { return done == 2; }));
    CHECK(count() == 0);
    CHECK(call(RELEASE, SEM, 3, 0) == 0);
    for (auto& t : threads) t.join();
    CHECK(done == 4 && count() == 1 && uint32_t(state()) == 0);
}
std::atomic<unsigned> ledger{0};
void fiber_entry(ppu_context* ctx)
{
    if (ctx->gpr[3] == 1) {
        CHECK(call(ACQUIRE, SEM, 1, 0) == 0);
        CHECK(ledger.load() == 1); ledger = 2;
    } else {
        CHECK(ledger == 0); ledger = 1;
        CHECK(call(RELEASE, SEM, 1, 0) == 0);
    }
    ctx->gpr[3] = 0;
}
void fiber()
{
    init(0);
    ppu_context home{};
    home.gpr[1] = 0x35000; home.gpr[13] = 0x3A00; home.thread_id = 1;
    g_active_ctx = &home;
    CHECK(call(0x55870804, 0x3B00) == 0x100);
    g_ppu_thread_entry_trampoline = fiber_entry;
    auto* s = reinterpret_cast<CellFiberPpuScheduler*>(0x4000);
    CHECK(cellFiberPpuInitializeScheduler(s, nullptr) == 0);
    for (unsigned i = 0; i < 2; ++i) {
        wr(0x800 + 8 * i, 0x900 + i); wr(0x804 + 8 * i, 0);
        CHECK(cellFiberPpuCreateFiber(s, reinterpret_cast<CellFiberPpu*>(0x5000 + 0x400 * i),
              0x800 + 8 * i, i + 1, 0, reinterpret_cast<void*>(0x10000 + 0x10000 * i), 0x10000, nullptr) == 0);
    }
    CHECK(cellFiberPpuRunFibers(s) == 0 && ledger == 2 && count() == 0);
    CHECK(cellFiberPpuFinalizeScheduler(s) == 0);
    g_active_ctx = nullptr;
}
/* ppu_vm_size == 0 is the whole 32-bit space (the runtime's convention), not a
 * zero-length arena: every span a valid EA names is in range, so neither the
 * semaphore nor the fiber path may stop. Both used to treat 0 as the limit and
 * stopped on the first call; they also treated vm_base == 0 as "no VM", which is
 * wrong for the identity mapping. */
void fullvm()
{
    const uint32_t saved = ppu_vm_size;
    ppu_vm_size = 0;
    fiber();    /* cellFiber: scheduler/fiber attributes, stacks, signals, Syncs */
    layout();   /* cellSync2: attribute block, initialize, out parameters */
    ppu_vm_size = saved;
}
void stopped(const std::string& mode)
{
    int fd[2]; CHECK(pipe(fd) == 0);
    const auto pid = fork();
    if (pid == 0) {
        close(fd[0]); dup2(fd[1], STDERR_FILENO); close(fd[1]);
        init(1);
        if (mode == "config-acquire") call(ACQUIRE, SEM, 1, 0x4444);
        else if (mode == "config-release") call(RELEASE, SEM, 1, 0x4444);
        else if (mode == "range") call(GET, SEM, MEM - 2);
        _exit(1);
    }
    close(fd[1]); std::string out; char bytes[512]; ssize_t n;
    while ((n = read(fd[0], bytes, sizeof bytes)) > 0) out.append(bytes, n);
    close(fd[0]); int status; waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 70);
    CHECK(out.find(mode == "range" ? "kind=hle-guest-range" : "kind=sync2-semaphore-unsupported") != std::string::npos);
    if (mode != "range") {
        CHECK(out.find("config=0x4444") != std::string::npos);
        CHECK(out.find("reason=custom-config") != std::string::npos);
        CHECK(state() >> 32 == 1);
    }
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    vm_base = static_cast<uint8_t*>(mmap(nullptr, MEM, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    if (vm_base == MAP_FAILED) return 2;
    ppu_vm_size = MEM; ppu_hle_init();
    const std::string mode = argv[1];
    if (mode == "layout") layout(); else if (mode == "counts") counts();
    else if (mode == "debt") debt(); else if (mode == "multi") multi();
    else if (mode == "fiber") fiber(); else if (mode == "fullvm") fullvm();
    else stopped(mode);
    std::printf("%s %s\n", failures ? "FAIL" : "PASS", argv[1]);
    return failures ? 1 : 0;
}

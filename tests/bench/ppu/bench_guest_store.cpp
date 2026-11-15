// Cost of one guest store through the loader's extern family (vm_write8/16/32/64), as lifted code calls it.
//
// Not a test: it prints ns per store for each width in four situations, each adding to the last,
//   idle     an unreserved line, and no SPU code has run (the reservation mark's flag is still zero)
//   window   the same with a store window registered that the stores miss (every title that drives GCM has one)
//   mark     the same while an SPU has reserved some other line (the flag is set; the mark bitmap is read)
//   marked   a line an SPU has reserved (the store takes the lock-line lock and reports itself; the lock and the
//            notify are empty stubs here, so this is the dispatch, not the lock's cost)
// The stores are 4/8/1/2-byte stores cycling over 16 KiB of guest memory, direct calls into the real
// ppu_loader.cpp. tests/bench/ppu/run_guest_store_bench.py builds this against two checkouts of the toolkit
// (the one under test and a baseline) and alternates them. The file only uses what both trees have, so it
// builds against either; spu_coh_is_reserved is defined here as the real spu_coherency.c defines it, for the
// baseline's loader, which calls it (the current loader does not).
#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <sys/mman.h>
#include "win32_compat.h"

extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x20000000;
void cellGcm_ref_on_poll(void) { std::abort(); }
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN, LPVOID, DWORD, DWORD*) { std::abort(); }
BOOL CloseHandle(HANDLE) { std::abort(); }
DWORD GetFileAttributesA(const char*) { return INVALID_FILE_ATTRIBUTES; }
uint64_t ppu_thread_register_main(void) { return 1; }
void (*g_ppu_thread_entry_trampoline)(uint32_t, uint64_t, uint32_t, uint64_t);
const func_entry function_table[1] = {{0, nullptr, nullptr}};
const uint64_t function_table_count = 0;
const char* ppu_lift_image_sha256(void) { return ""; }
const uint32_t ppu_lifter_abi = PPU_LIFTER_ABI;
uint32_t ps3_hle_count(void) { return 0; }

int resv_marked_spu(uint32_t ea);
void resv_mark_spu(uint32_t ea);
int spu_coh_is_reserved(uint32_t addr) { return resv_marked_spu(addr); }
void ppu_set_store_hook(uint32_t lo, uint32_t len, void (*fn)(uint32_t ea, uint32_t value));
static void hook(uint32_t, uint32_t) { std::abort(); }   // the stores here never reach the window

static volatile unsigned g_locks, g_notifies;
void spu_lockline_lock(void) { g_locks = g_locks + 1; }
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t) { g_notifies = g_notifies + 1; }

void vm_write8(uint64_t a, uint8_t v);
void vm_write16(uint64_t a, uint16_t v);
void vm_write32(uint64_t a, uint32_t v);
void vm_write64(uint64_t a, uint64_t v);
}

static double now()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static const uint32_t AREA = 0x20000, MARKED = 0x40000, ELSEWHERE = 0x50000;

template <class F>
static double ns_per_store(F store, uint32_t base, uint32_t mask, uint64_t n)
{
    for (uint64_t i = 0; i < 100000; i++) store(base + (uint32_t)((i * 8) & mask), i);   // warm
    const double t0 = now();
    for (uint64_t i = 0; i < n; i++) store(base + (uint32_t)((i * 8) & mask), i);
    return (now() - t0) * 1e9 / (double)n;
}

static void row(const char* situation, uint32_t base, uint32_t mask, uint64_t n)
{
    const double w8 = ns_per_store([](uint32_t a, uint64_t i) { vm_write8(a, (uint8_t)i); }, base, mask, n);
    const double w16 = ns_per_store([](uint32_t a, uint64_t i) { vm_write16(a, (uint16_t)i); }, base, mask, n);
    const double w32 = ns_per_store([](uint32_t a, uint64_t i) { vm_write32(a, (uint32_t)i); }, base, mask, n);
    const double w64 = ns_per_store([](uint32_t a, uint64_t i) { vm_write64(a, i); }, base, mask, n);
    std::printf("%s write8 %.3f write16 %.3f write32 %.3f write64 %.3f\n", situation, w8, w16, w32, w64);
}

int main(int argc, char** argv)
{
    const uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 50000000ull;
    vm_base = static_cast<uint8_t*>(mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0));
    if (vm_base == MAP_FAILED) return 1;
    std::memset(vm_base + AREA, 0, 0x4000);
    vm_write32(AREA, 0);                         // the loader reads its environment on the first store
    row("idle", AREA, 0x3FF8, n);
    ppu_set_store_hook(0x10000, 0x100, hook);    // what an RSX title has: a label window the stores miss
    row("window", AREA, 0x3FF8, n);
    resv_mark_spu(ELSEWHERE);
    row("mark", AREA, 0x3FF8, n);
    resv_mark_spu(MARKED);
    row("marked", MARKED, 0x78, n);              // the 128-byte line itself
    return g_locks == 0xFFFFFFFFu;               // keep the stubs live
}

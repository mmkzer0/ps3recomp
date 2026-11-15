// The guest store family of ppu_loader.cpp (vm_write8/16/32/64) as lifted code calls it.
//
// A store runs, in this order: the bounds check, the null page, the store window's hook, the bytes, and the
// raw SPU register side effect; a line an SPU has reserved takes the lock-line lock around the bytes and is
// reported to the model. While no write watch is armed (ppu_store_diag, runtime/ppu/ppu_store_diag.h) the
// family takes a short path that skips the watches; this fixture pins that the short path and the full one do
// the same thing: what lands, which side effects run and in what order, for every width, on an unreserved
// line, on a line an SPU has reserved (resv_mark_spu), in the raw SPU register window and past the end of
// guest memory, with and without a store window over some of the stores.
//
// Modes (argv[1]); the default runs every in-process check and the others are one-store probes whose stderr
// tools/test_ppu_store_path.py reads:
//   sync   g_barrier_sync_watch names a line and a store lands in it: the watch must see the store
//   ww     run with PPU_WWATCH set: the watch must see a store
//   wval   run with PPU_WVAL set: the value watch must see a store
//   quiet  no watch armed: no watch output
//   null   a store into the null page: a named stop
//
// Built by tools/test_ppu_store_path.py against the real ppu_loader.cpp (a loader-only fixture, with the stubs
// of test_ppu_store_hook.cpp; the lock-line lock, the SPU notify and the raw-register store are recorded).
#define PPU_NO_INLINE_LOADS   // the fixture declares the loader's own vm_read32 below
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <sys/mman.h>
#include "win32_compat.h"
#include "label_window.h"
#include "ppu_store_diag.h"
#include "resv_mark.h"
#include "spu_raw.h"

extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x20000000;
void cellGcm_ref_on_poll(void) { std::abort(); }
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
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

extern uint32_t ppu_vm_size;
extern uint32_t g_barrier_sync_watch;
void vm_write8(uint64_t a, uint8_t v);
void vm_write16(uint64_t a, uint16_t v);
void vm_write32(uint64_t a, uint32_t v);
void vm_write64(uint64_t a, uint64_t v);
uint32_t ppu_lwarx32(ppu_context* ctx, uint64_t ea);
int ppu_stwcx32(ppu_context* ctx, uint64_t ea, uint32_t val);
void resv_mark_spu(uint32_t ea);
void resv_bump(uint32_t ea);
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
}

// ---- what the runtime did, in order ---------------------------------------------------------------------
enum Kind { HOOK, LOCK, NOTIFY, UNLOCK, RAW };
struct Ev { Kind kind; uint32_t a, b; uint8_t byte; };
static Ev s_ev[64];
static unsigned s_n;
static void note(Kind k, uint32_t a = 0, uint32_t b = 0, uint8_t byte = 0)
{
    if (s_n < 64) s_ev[s_n] = Ev{k, a, b, byte};
    ++s_n;
}

extern "C" {
void spu_lockline_lock(void) { note(LOCK); }
void spu_lockline_unlock(void) { note(UNLOCK); }
// The real notify advances the line's epoch; the fixture does the same so the reservation model sees a store.
void spu_coh_notify_write(uint32_t ea) { note(NOTIFY, ea); resv_bump(ea); }
// Called after the plain store: the byte at the register's address is already the stored one.
void spu_raw_reg_store(uint32_t ea, uint32_t val, int width) { note(RAW, ea, val, vm_base[ea]); (void)width; }
}

static void hook(uint32_t ea, uint32_t value) { note(HOOK, ea, value, vm_base[ea]); }

static int fails;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

static void clear_events() { s_n = 0; }
static std::string events()
{
    std::string s;
    char b[64];
    for (unsigned i = 0; i < s_n && i < 64; i++) {
        static const char* const names[] = {"hook", "lock", "notify", "unlock", "raw"};
        std::snprintf(b, sizeof b, "%s(%X,%X,%02X) ", names[s_ev[i].kind], s_ev[i].a, s_ev[i].b, s_ev[i].byte);
        s += b;
    }
    return s;
}

static uint8_t* g(uint32_t ea) { return vm_base + ea; }
static void fill(uint32_t ea, uint32_t n, uint8_t v) { std::memset(g(ea), v, n); }

static const uint32_t AREA = 0x20000, AREA_LEN = 0x1000;
static const uint32_t MARKED = 0x20200;          // the one line an SPU has "reserved"
static const uint32_t RAW_REG = SPU_RAW_BASE + 0x44000u + 0x1Cu;   // a raw SPU's run-control register
static const uint32_t RAW_LS = SPU_RAW_BASE + 0x100u;              // the same SPU's local store: plain memory

// Every store form on every kind of address, returning what the runtime did as a string. Run with and without
// a write watch armed: the two strings have to match, hook calls included.
static std::string scenario()
{
    std::string out;
    char b[96];
    auto section = [&](const char* name) { out += name; out += ": "; };

    fill(AREA, AREA_LEN, 0xA5);
    fill(RAW_REG - 0x1C, 0x100, 0xA5);
    fill(RAW_LS & ~0xFu, 0x20, 0xA5);

    section("plain"); clear_events();
    vm_write8(AREA + 0x100, 0x11);
    vm_write16(AREA + 0x102, 0x2233);
    vm_write32(AREA + 0x104, 0x44556677u);
    vm_write64(AREA + 0x108, 0x8899AABBCCDDEEFFull);
    for (uint32_t i = 0xFF; i < 0x118; i++) { std::snprintf(b, sizeof b, "%02X", *g(AREA + i)); out += b; }
    out += " " + events() + "\n";

    section("marked"); clear_events();
    vm_write8(MARKED + 1, 0x12);
    vm_write16(MARKED + 2, 0x3456);
    vm_write32(MARKED + 4, 0x789ABCDEu);
    vm_write64(MARKED + 8, 0x0102030405060708ull);
    for (uint32_t i = 0; i < 0x10; i++) { std::snprintf(b, sizeof b, "%02X", *g(MARKED + i)); out += b; }
    out += " " + events() + "\n";

    section("neighbours"); clear_events();
    vm_write32(MARKED - 4, 1);          // the line below
    vm_write32(MARKED + 0x80, 2);       // the line above
    vm_write64(MARKED - 8, 3);
    out += events() + "\n";

    section("raw"); clear_events();
    vm_write32(RAW_REG, 0x00000001u);
    vm_write8(RAW_REG, 2); vm_write16(RAW_REG, 3); vm_write64(RAW_REG - 4, 4);   // only a word store has an effect
    vm_write32(RAW_LS, 5);                                                       // local store is plain memory
    std::snprintf(b, sizeof b, "%02X%02X%02X%02X ", *g(RAW_REG - 4), *g(RAW_REG), *g(RAW_REG + 3), *g(RAW_LS + 3));
    out += b + events() + "\n";

    section("oob"); clear_events();
    ppu_vm_size = AREA + AREA_LEN;
    vm_write32(AREA + AREA_LEN - 4, 0x0A0B0C0Du);       // the last word: lands
    vm_write32(AREA + AREA_LEN - 2, 0x0E0F1011u);       // straddles the end: dropped
    vm_write8(AREA + AREA_LEN, 0x77);                   // past the end: dropped
    vm_write64(AREA + AREA_LEN - 4, 0x1213141516171819ull);
    ppu_vm_size = 0;
    for (uint32_t i = AREA + AREA_LEN - 6; i < AREA + AREA_LEN + 2; i++) { std::snprintf(b, sizeof b, "%02X", *g(i)); out += b; }
    out += " " + events() + "\n";
    return out;
}

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "all";
    vm_base = static_cast<uint8_t*>(mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0));
    if (vm_base == MAP_FAILED) { std::puts("FAIL: guest VM alloc"); return 1; }

    // The first store of a process reads the watch environment on the full path whatever is armed, so each
    // probe stores once elsewhere first: the store it is about must be seen from the short path's decision.
    if (mode != "all") vm_write32(AREA + 0x400, 0);
    if (mode == "sync") { g_barrier_sync_watch = AREA; vm_write32(AREA + 0x40, 1); return 0; }
    if (mode == "ww")   { vm_write32(AREA + 0x10, 0x55); return 0; }
    if (mode == "wval") { vm_write32(AREA + 0x300, 0x1234); return 0; }
    if (mode == "quiet") { vm_write32(AREA + 0x10, 0x55); vm_write64(AREA + 0x300, 0x1234); return 0; }
    if (mode == "null") { vm_write32(0x100, 1); return 0; }
    if (mode == "sweep") {
        // ppu.null_page = drop: a null-page store is dropped, and so is the sweep a memset on a null buffer
        // walks up out of the page (a later store within 0x1000 of the last dropped one, on the same thread).
        ppu_context ctx;
        std::memset(&ctx, 0, sizeof ctx);
        g_active_ctx = &ctx;
        vm_write32(0x100, 0xAABBCCDDu);
        vm_write32(0x1100, 0x11223344u);
        std::printf("sweep: %02X %02X\n", *g(0x100), *g(0x1100));
        return 0;
    }

    // The write watches' word: unread until the first store reads the watch environment, then zero.
    check(ppu_store_diag_load() == PPU_STORE_DIAG_UNREAD, "before the first store the watch environment is unread");
    vm_write8(AREA, 1);
    check(ppu_store_diag_load() == 0, "after it, with no watch set, none is armed");

    // The store window is not one of the watches: every title that drives GCM registers one, so a store that
    // misses it stays on the short path, and one that overlaps it runs the hook before the bytes land.
    ppu_set_store_hook(AREA, 0x100, hook);
    check(ppu_store_diag_load() == 0, "a registered window arms no write watch");
    clear_events(); fill(AREA, 8, 0xA5);
    vm_write32(AREA + 4, 0x01020304u);
    check(s_n == 1 && s_ev[0].kind == HOOK && s_ev[0].a == AREA + 4 && s_ev[0].byte == 0xA5 && *g(AREA + 4) == 1,
          "a store in the window runs the hook once, before the bytes land");
    ppu_set_store_hook(AREA, 0x100, nullptr);
    clear_events(); vm_write32(AREA + 4, 2);
    check(s_n == 0, "a removed window hooks nothing");
    ppu_set_store_hook(AREA, 0, hook);
    clear_events(); vm_write32(AREA + 4, 2);
    check(s_n == 0, "an empty window hooks nothing");
    ppu_set_store_hook(0, 0, nullptr);

    // The sync-line probe is a plain global that other threads and the title write; it is read on every store.
    g_barrier_sync_watch = 0x1000;
    clear_events(); vm_write32(AREA + 8, 3);
    check(*g(AREA + 8 + 3) == 3, "a store with the sync-line probe armed still lands");
    g_barrier_sync_watch = 0;

    // The short path against the full one: the same stores, once with nothing armed and once with a watch
    // bit set (which sends every store down the full path; the watch itself has nothing to report), must do
    // exactly the same, hook calls included.
    resv_mark_spu(MARKED);
    ppu_set_store_hook(AREA + 0x104, 0x10, hook);
    const std::string fast = scenario();
    ppu_store_diag_arm(PPU_STORE_DIAG_WATCH);
    const std::string full = scenario();
    ppu_store_diag_disarm(PPU_STORE_DIAG_WATCH);
    ppu_set_store_hook(0, 0, nullptr);
    std::fputs(fast.c_str(), stdout);
    check(fast.find("hook(") != std::string::npos, "the scenario's window hooked the stores it covers");
    check(fast == full, "with and without a write watch armed, every width, every kind of address does the same");

    // What the short path does, spelled out (the string above is compared, this says what it should hold).
    clear_events();
    fill(AREA, AREA_LEN, 0xA5);
    vm_write64(AREA + 0x108, 0x8899AABBCCDDEEFFull);
    check(s_n == 0 && *g(AREA + 0x108) == 0x88 && *g(AREA + 0x10F) == 0xFF, "an unreserved line: big-endian bytes, no lock, no notify");
    clear_events();
    vm_write32(MARKED + 4, 0x789ABCDEu);
    check(s_n == 3 && s_ev[0].kind == LOCK && s_ev[1].kind == NOTIFY && s_ev[1].a == MARKED + 4 && s_ev[2].kind == UNLOCK &&
          *g(MARKED + 4) == 0x78,
          "a reserved line: lock, the store, the notify for its address, unlock");
    clear_events();
    vm_write32(MARKED + 0x80, 1); vm_write32(MARKED - 4, 1); vm_write8(MARKED + 0x7F, 1);
    check(s_n == 3 && s_ev[0].kind == LOCK && s_ev[1].kind == NOTIFY && s_ev[1].a == MARKED + 0x7F && s_ev[2].kind == UNLOCK,
          "only the reserved line is serialised: the lines either side are plain stores");
    clear_events();
    vm_write32(RAW_REG, 9);
    check(s_n == 1 && s_ev[0].kind == RAW && s_ev[0].a == RAW_REG && s_ev[0].b == 9 && s_ev[0].byte == 0,
          "a word store to a raw SPU register runs its side effect after the bytes landed");

    // A hook that is armed runs before the lock, the store and the notify, on a reserved line too.
    ppu_set_store_hook(MARKED, 0x80, hook);
    clear_events();
    fill(MARKED + 4, 4, 0xA5);
    vm_write32(MARKED + 4, 0xCAFEF00Du);
    check(s_n == 4 && s_ev[0].kind == HOOK && s_ev[0].byte == 0xA5 && s_ev[1].kind == LOCK && s_ev[2].kind == NOTIFY &&
          s_ev[3].kind == UNLOCK && *g(MARKED + 4) == 0xCA,
          "the window's hook runs first, with the old bytes, then the reserved line's lock and notify");
    ppu_set_store_hook(0, 0, nullptr);

    // A plain store by the thread that holds a reservation into the line keeps it; another thread's does not.
    ppu_context mine, other;
    std::memset(&mine, 0, sizeof mine);
    std::memset(&other, 0, sizeof other);
    const uint32_t word = MARKED + 0x10, neighbour = MARKED + 0x18;
    for (int who = 0; who < 2; who++) {
        ppu_lwarx32(&mine, word);
        g_active_ctx = who == 0 ? &mine : &other;
        vm_write32(neighbour, 0x01010101u + (uint32_t)who);
        g_active_ctx = nullptr;
        clear_events();
        const int ok = ppu_stwcx32(&mine, word, 0x0BADF00Du);
        check(who == 0 ? ok == 1 : ok == 0, who == 0 ? "a store by the reserving thread keeps its reservation"
                                                      : "another thread's store to the line kills it");
    }

    std::printf("[ppu_store_path] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

// The store hook of ppu_loader.cpp (ppu_set_store_hook, runtime/memory/label_window.{h,c}).
//
// A device model registers a window of guest memory; a guest store that overlaps it runs the hook first, on
// the storing thread, with the first window address the store covers, while the old bytes are still in
// memory (the RSX walker's label ordering, libs/video/cellGcmSys.c, needs exactly that). That holds for every
// form of guest store, not only the 32-bit one: the load/store family of any width, the multiple-word and
// string stores, the byte-reversed stores, the store-conditionals, the vector stores and dcbz. The forms are
// the code the real lifter emits (tools/test_ppu_store_hook.py generates store_forms.cpp from it). Nothing
// calls the hook for a store next to the window, with no hook registered, with an empty window or after
// the hook was removed.
//
// Built by tools/test_ppu_store_hook.py against the real ppu_loader.cpp (the stubs are those of
// test_ppu_dispatch_depth.cpp: a loader-only fixture).
#define PPU_NO_INLINE_LOADS   // the fixture declares the loader's own vm_read32 below
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "win32_compat.h"

extern "C" {
uint8_t* vm_base;
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
const func_entry function_table[1] = {{0, nullptr, nullptr}};
const uint64_t function_table_count = 0;
const char* ppu_lift_image_sha256(void) { return ""; }
/* The lifted-code ABI this fixture is written in (ppu_recomp.h stamps it for a
 * real lift); the loader checks it at startup. */
const uint32_t ppu_lifter_abi = PPU_LIFTER_ABI;
uint32_t ps3_hle_count(void) { return 0; }
void ppu_set_store_hook(uint32_t lo, uint32_t len, void (*fn)(uint32_t ea, uint32_t value));
void vm_write8(uint64_t a, uint8_t v);
void vm_write16(uint64_t a, uint16_t v);
void vm_write32(uint64_t a, uint32_t v);
void vm_write64(uint64_t a, uint64_t v);
uint32_t vm_read32(uint64_t a);
uint32_t ppu_lwarx32(ppu_context* ctx, uint64_t ea);
uint64_t ppu_ldarx64(ppu_context* ctx, uint64_t ea);

typedef struct { const char* name; void (*run)(ppu_context*); int kind;
                 unsigned ea_off, first, len, calls; } store_form;
extern const store_form g_forms[];
extern const unsigned g_nforms;
}

static const uint32_t LO = 0x20000, LEN = 0x100;
static int fails;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

static unsigned s_calls;
static uint32_t s_ea, s_value, s_old;
static bool s_old_bytes_intact;      // at every hook call, the byte at its address still held the old value
static uint32_t s_win_lo, s_win_len;
static bool s_ea_in_window;
static void hook(uint32_t ea, uint32_t value)
{
    ++s_calls; s_ea = ea; s_value = value;
    s_old = vm_read32(ea);          // what memory holds while the hook runs
    s_old_bytes_intact = s_old_bytes_intact && vm_base[ea] == 0xA5;
    s_ea_in_window = s_ea_in_window && ea >= s_win_lo && ea - s_win_lo < s_win_len;
}
static void reset() { s_calls = 0; s_ea = s_value = s_old = 0xDEADBEEF; s_old_bytes_intact = s_ea_in_window = true; }

// Counts only: the top-of-address-space checks call the span path without executing a store, so there are no
// host bytes there for the ordinary hook to read.
static void span_hook(uint32_t ea, uint32_t) { ++s_calls; s_ea = ea; }

// The store forms run against one probe area of 0xA5 bytes: a form's store has to turn exactly the bytes
// [BASE + first, +len) into something else.
static const uint32_t AREA = 0x20000, AREA_LEN = 0x400, BASE = 0x20100;

static void window(uint32_t lo, uint32_t len)
{
    s_win_lo = lo; s_win_len = len;
    ppu_set_store_hook(lo, len, len ? hook : nullptr);
}

static ppu_context s_ctx;   // the context the current form ran on (its CR0 after a store-conditional)

static void prepare_form(const store_form& f)
{
    std::memset(&s_ctx, 0, sizeof s_ctx);
    std::memset(vm_base + AREA, 0xA5, AREA_LEN);
    s_ctx.gpr[4] = BASE + f.ea_off;
    s_ctx.gpr[5] = 0x1122334455667788ull;
    s_ctx.gpr[6] = 0x55667788u;              // stswi / stswx: the second register of the string
    s_ctx.gpr[29] = 0x11111111u; s_ctx.gpr[30] = 0x22222222u; s_ctx.gpr[31] = 0x33333333u;   // stmw
    s_ctx.xer = 7;                           // stswx: the byte count
    for (int i = 0; i < 16; i++) s_ctx.vr[5].b[i] = (uint8_t)(0x10 + i);
    const uint64_t one = 0x3FF0000000000001ull; std::memcpy(&s_ctx.fpr[5], &one, 8);
    if (f.kind == 1) ppu_lwarx32(&s_ctx, BASE + f.ea_off);
    if (f.kind == 2) ppu_ldarx64(&s_ctx, BASE + f.ea_off);
}

static void run_form(const store_form& f)
{
    prepare_form(f);
    reset();
    f.run(&s_ctx);
}

// After a form ran: its bytes changed, no other byte of the area did.
static bool store_landed_exactly(const store_form& f)
{
    for (uint32_t a = AREA; a < AREA + AREA_LEN; a++) {
        const bool in_region = a >= BASE + f.first && a < BASE + f.first + f.len;
        if ((vm_base[a] != 0xA5) != in_region) return false;
    }
    return true;
}

static void test_form(const store_form& f)
{
    char what[160];
    const uint32_t first = BASE + f.first, last = first + f.len - 1;

    window(AREA, AREA_LEN);
    run_form(f);
    std::snprintf(what, sizeof what, "%s: a window over the whole area runs the hook %u time(s), each within the window",
                  f.name, f.calls);
    check(s_calls == f.calls && s_ea_in_window, what);
    std::snprintf(what, sizeof what, "%s: the hook ran before the bytes it covers were stored", f.name);
    check(s_old_bytes_intact, what);
    std::snprintf(what, sizeof what, "%s: the store landed, and only its own bytes changed", f.name);
    check(store_landed_exactly(f), what);
    if (f.kind) {
        // The emitted stwcx./stdcx. writes CR0 itself: EQ on success, SO from XER (0 here).
        std::snprintf(what, sizeof what, "%s: the successful conditional leaves CR0 = EQ only", f.name);
        check((s_ctx.cr >> 28) == 2, what);
    }

    // An overlap of one byte is an overlap: the window is the last byte the store writes. The hook gets the
    // first window address the store covers.
    window(last, 1);
    run_form(f);
    std::snprintf(what, sizeof what, "%s: a store that overlaps the window by its last byte runs the hook once, at that byte", f.name);
    check(s_calls == 1 && s_ea == last && s_old_bytes_intact, what);
    check(store_landed_exactly(f), "  ... and it landed");

    window(first, 1);
    run_form(f);
    std::snprintf(what, sizeof what, "%s: a store that overlaps the window by its first byte runs the hook", f.name);
    check(s_calls >= 1 && s_ea == first && s_old_bytes_intact, what);

    window(first - 1, 1);
    run_form(f);
    std::snprintf(what, sizeof what, "%s: a window one byte below the store is not hooked", f.name);
    check(s_calls == 0 && store_landed_exactly(f), what);

    window(last + 1, 1);
    run_form(f);
    std::snprintf(what, sizeof what, "%s: a window one byte above the store is not hooked", f.name);
    check(s_calls == 0 && store_landed_exactly(f), what);

    ppu_set_store_hook(0, 0, nullptr);
    run_form(f);
    std::snprintf(what, sizeof what, "%s: with no hook registered the store lands and calls nothing", f.name);
    check(s_calls == 0 && store_landed_exactly(f), what);
}

// A store-conditional whose reservation was replaced stores nothing and clears CR0[EQ]. ppu_stwcx32 runs the
// store hook before it validates: the wait the hook represents happens either way, and the guest's own loop
// retries a failed conditional.
static void test_store_conditional_failure(const store_form& f)
{
    char what[160];
    prepare_form(f);
    ppu_lwarx32(&s_ctx, BASE + 0x300);         // the guest's one reservation now names another line
    window(AREA, AREA_LEN);
    reset();
    f.run(&s_ctx);
    std::snprintf(what, sizeof what, "%s: a failed conditional still runs the hook once, at its address", f.name);
    check(s_calls == 1 && s_ea == BASE + f.ea_off, what);
    std::snprintf(what, sizeof what, "%s: ... stores nothing and clears CR0[EQ]", f.name);
    check(vm_base[BASE + f.ea_off] == 0xA5 && (s_ctx.cr >> 28) == 0, what);
}

int main()
{
    vm_base = static_cast<uint8_t*>(std::calloc(1, 0x40000));
    if (!vm_base) { std::puts("FAIL: guest VM alloc"); return 1; }

    reset();
    vm_write32(LO, 1);
    check(s_calls == 0, "no hook registered: a store in the window calls nothing");

    ppu_set_store_hook(LO, LEN, hook);
    s_win_lo = LO; s_win_len = LEN;
    reset();
    vm_write32(LO + 0x10, 0x1234);
    check(s_calls == 1 && s_ea == LO + 0x10 && s_value == 0x1234, "a 32-bit store in the window runs the hook with its address and value");
    check(s_old == 0, "the hook ran before the store landed (it read the old value)");
    check(vm_read32(LO + 0x10) == 0x1234, "and the store landed afterwards");

    reset(); vm_write32(LO, 2);
    check(s_calls == 1, "the first word of the window");
    reset(); vm_write32(LO + LEN - 4, 3);
    check(s_calls == 1, "the last word of the window");
    reset(); vm_write32(LO + LEN, 4);
    check(s_calls == 0, "the word after the window");
    reset(); vm_write32(LO - 4, 5);
    check(s_calls == 0, "the word before the window");
    reset(); vm_write32(LO - 2, 6);
    check(s_calls == 1 && s_ea == LO, "an unaligned word that straddles the window's start is hooked, at the window's first address");

    reset(); vm_write8(LO + 0x20, 0x41);
    check(s_calls == 1 && s_ea == LO + 0x20 && s_value == 0x41, "an 8-bit store in the window runs the hook with its value");
    reset(); vm_write16(LO + 0x22, 0x4243);
    check(s_calls == 1 && s_ea == LO + 0x22 && s_value == 0x4243, "a 16-bit store in the window runs the hook with its value");
    reset(); vm_write64(LO + 0x28, 0x1111111122222222ull);
    check(s_calls == 1 && s_ea == LO + 0x28 && s_value == 0x22222222u, "a 64-bit store in the window runs the hook once (value: its low word)");
    reset(); vm_write64(LO + LEN - 4, 7);
    check(s_calls == 1 && s_ea == LO + LEN - 4, "a doubleword whose first word is the window's last word is hooked once");
    reset(); vm_write64(LO - 4, 8);
    check(s_calls == 1 && s_ea == LO, "a doubleword whose second word is the window's first word is hooked once");
    reset(); vm_write64(LO + LEN, 9); vm_write64(LO - 8, 10);
    check(s_calls == 0, "doublewords on either side of the window are not hooked");
    reset(); vm_write8(LO - 1, 1); vm_write8(LO + LEN, 1); vm_write16(LO - 2, 1); vm_write16(LO + LEN, 1);
    check(s_calls == 0, "bytes and halfwords on either side of the window are not hooked");

    ppu_set_store_hook(LO, 0, hook);
    reset(); vm_write32(LO + 0x10, 6); vm_write64(LO + 0x10, 6); vm_write8(LO + 0x10, 6);
    check(s_calls == 0, "an empty window hooks nothing");

    ppu_set_store_hook(LO, LEN, hook);
    ppu_set_store_hook(0, 0, nullptr);
    reset(); vm_write32(LO + 0x10, 7); vm_write64(LO + 0x10, 7);
    check(s_calls == 0, "after the hook is removed the window is dead");

    ppu_set_store_hook(0, 0xFFFFFFFFu, hook);
    s_win_lo = 0; s_win_len = 0xFFFFFFFFu;
    reset(); vm_write32(0x20100, 8);
    check(s_calls == 1 && s_ea == 0x20100, "a window of nearly 4 GB hooks a store inside it (the bound does not wrap)");
    ppu_set_store_hook(0, 0, nullptr);

    // The 32-bit top of the address space: the window is the last 256 bytes, and the span checks below are
    // the inline modular compare plus the out-of-line 64-bit decision. No store is executed here (the probe
    // VM is 256 KiB, so only the range check can run); the hook only counts.
    ppu_set_store_hook(0xFFFFFF00u, 0x100, span_hook);
    s_win_lo = 0xFFFFFF00u; s_win_len = 0x100;
    reset(); PPU_STORE_HOOK(0xFFFFFFF8u, 8);
    check(s_calls == 1 && s_ea == 0xFFFFFFF8u, "a store in the last 8 bytes of the address space is hooked");
    reset(); PPU_STORE_HOOK(0xFFFFFF00u, 16);
    check(s_calls == 1 && s_ea == 0xFFFFFF00u, "a 16-byte store whose last byte is 4 GiB-1 is hooked");
    reset(); PPU_STORE_HOOK(0xFFFFFEF8u, 8);
    check(s_calls == 0, "a store that ends one byte below the top window is not hooked");
    ppu_set_store_hook(0, 0x1000, span_hook);
    s_win_lo = 0; s_win_len = 0x1000;
    reset(); PPU_STORE_HOOK(0xFFFFFFF8u, 16);
    check(s_calls == 0, "a span from the top of the address space does not reach a low window (no wrapped bytes are written)");
    ppu_set_store_hook(0, 0, nullptr);

    for (unsigned i = 0; i < g_nforms; i++) {
        test_form(g_forms[i]);
        if (g_forms[i].kind) test_store_conditional_failure(g_forms[i]);
    }

    std::printf("[ppu_store_hook] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    std::free(vm_base);
    return fails ? 1 : 0;
}

// The store hook of ppu_loader.cpp's vm_write32 (ppu_set_store_hook).
//
// A device model registers a window of guest memory; a 32-bit guest store into it runs the hook first, on
// the storing thread, with the address and the value, while the old value is still in memory (the RSX
// walker's label ordering, libs/video/cellGcmSys.c, needs exactly that). Nothing else calls it: not a store
// outside the window, not a 8/16/64-bit store, not after the hook was removed or with an empty window.
//
// Built by tools/test_ppu_store_hook.py against the real ppu_loader.cpp (the stubs are those of
// test_ppu_dispatch_depth.cpp: a loader-only fixture).
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
uint32_t ps3_hle_count(void) { return 0; }

void ppu_set_store_hook(uint32_t lo, uint32_t len, void (*fn)(uint32_t ea, uint32_t value));
void vm_write8(uint64_t a, uint8_t v);
void vm_write16(uint64_t a, uint16_t v);
void vm_write32(uint64_t a, uint32_t v);
void vm_write64(uint64_t a, uint64_t v);
uint32_t vm_read32(uint64_t a);
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
static void hook(uint32_t ea, uint32_t value)
{
    ++s_calls; s_ea = ea; s_value = value;
    s_old = vm_read32(ea);          // what memory holds while the hook runs
}
static void reset() { s_calls = 0; s_ea = s_value = s_old = 0xDEADBEEF; }

int main()
{
    vm_base = static_cast<uint8_t*>(std::calloc(1, 0x40000));
    if (!vm_base) { std::puts("FAIL: guest VM alloc"); return 1; }

    reset();
    vm_write32(LO, 1);
    check(s_calls == 0, "no hook registered: a store in the window calls nothing");

    ppu_set_store_hook(LO, LEN, hook);
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

    reset();
    vm_write8(LO + 0x20, 1); vm_write16(LO + 0x22, 2); vm_write64(LO + 0x28, 3);
    check(s_calls == 0, "8-, 16- and 64-bit stores are not hooked");

    ppu_set_store_hook(LO, 0, hook);
    reset(); vm_write32(LO + 0x10, 6);
    check(s_calls == 0, "an empty window hooks nothing");

    ppu_set_store_hook(LO, LEN, hook);
    ppu_set_store_hook(0, 0, nullptr);
    reset(); vm_write32(LO + 0x10, 7);
    check(s_calls == 0, "after the hook is removed the window is dead");

    std::printf("[ppu_store_hook] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    std::free(vm_base);
    return fails ? 1 : 0;
}

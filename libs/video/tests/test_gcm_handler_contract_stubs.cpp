// What the real runtime/ppu/ppu_loader.cpp takes from the rest of a title, for
// test_gcm_handler_contract.c: no lifted program (the fixture registers its
// functions by hand), no SPU, no PPU threads. Same set as
// runtime/ppu/tests/test_ppu_guest_call_stack.cpp. vm_base, ppu_hle_inject_base and the
// SPU coherence hooks are the C fixture's (cellGcmSys.c needs them too).
//
// Built by tools/test_gcm_handler_contract.py.
#include "ppu_recomp.h"
#include <cstdint>
#include <cstdlib>
#include "win32_compat.h"

extern "C" {
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN, LPVOID, DWORD, DWORD*) { std::abort(); }
BOOL CloseHandle(HANDLE) { std::abort(); }
BOOL SetEvent(HANDLE) { std::abort(); }   // cellGcm_ref_on_poll's kick; never armed here
DWORD GetFileAttributesA(const char*) { return INVALID_FILE_ATTRIBUTES; }
uint64_t ppu_thread_register_main(void) { return 1; }
void (*g_ppu_thread_entry_trampoline)(uint32_t, uint64_t, uint32_t, uint64_t);
const func_entry function_table[1] = {{0, nullptr, nullptr}};   // no lifted program
const uint64_t function_table_count = 0;
uint32_t ps3_hle_count(void) { return 0; }
}

/* Which site a guest-null-access stop names (tools/test_stop_attribution.py).
 *
 * Built against the real ppu_loader.cpp and ppu_hle.cpp. A host-side handler (an lv2 syscall, an HLE import
 * from either dispatch table) that reads or writes guest address 0 is not a guest function's access, so the
 * stop must name the handler and the guest call site that entered it, and no `fn`: `fn` is the nearest lifted
 * function below a host return address, which for a runtime address is unrelated code. A guest function that
 * faults, including one a handler runs through ppu_guest_call, is still the guest's own access.
 *
 *   test_stop_attribution syscall <num>          lv2_syscall with an active guest context
 *   test_stop_attribution syscall-noctx <num>    the same on a thread with no active guest context
 *   test_stop_attribution hle <nid>              ps3_hle_call with an active guest context
 *   test_stop_attribution guest                  a guest function (under the thread's own context) that loads page 0
 *   test_stop_attribution after-syscall <num>    a syscall returns, then a guest function loads page 0
 *   test_stop_attribution after-hle <nid>        an import returns, then a guest function loads page 0
 *
 * Syscalls: 1000 host handler loads page 0, 1001 host handler stores to it, 1002 handler runs a guest callback
 * that loads it, 1003 returns 0x33. Imports (NID 0x5A00000N): 1 ctx-table loads, 2 generic-table loads, 3 ctx
 * table runs the callback, 4 generic table runs the callback, 5 returns 0x44.
 */
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>
#include "win32_compat.h"
#include "../memory/vm.h"

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

static constexpr uint32_t GUEST_NULL_FN = 0x30060;
static constexpr uint32_t GUEST_TOC = 0x1234;

extern "C" {
uint8_t* vm_base;
// A loader-and-HLE fixture: nothing below runs guest threads, SPUs or RSX. Any use is a failure.
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
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ppu_gcm_pump(void) {}
void ppu_prof_stamp(void*, unsigned) {}
uint32_t ps3_mmapper_block_size(uint32_t) { std::abort(); }
void ps3_msf(const char*, ...) {}
int64_t sys_mmapper_allocate_address(ppu_context*) { std::abort(); }
int64_t sys_mmapper_search_map_address(uint32_t, uint32_t, uint32_t*) { std::abort(); }
void ppu_thread_note_block(uint64_t) {}
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0;
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
void ppu_recomp_register(void);
void lv2_syscall(ppu_context* ctx);
extern uint32_t ppu_vm_size;
void vm_write32(uint64_t a, uint32_t v);
uint64_t ppu_guest_call_ct(uint32_t code, uint32_t toc, uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t, uint64_t);
typedef void (*hle_ctx_fn)(ppu_context*);
void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn);
void ps3_hle_register(uint32_t nid, const char* name, void (*fn)(ppu_context*));
void ps3_hle_call(uint32_t nid, ppu_context* ctx);
}

extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;

/* The guest's own access: a registered function, so its host return address resolves to its guest address. */
static __attribute__((noinline)) void guest_null_fn(ppu_context* ctx) { ctx->gpr[3] = vm_read32(0); }

static uint64_t run_guest_callback() { return ppu_guest_call_ct(GUEST_NULL_FN, GUEST_TOC, 0, 0, 0, 0, 0, 0, 0, 0); }

extern "C" int lv2_try_syscall(ppu_context* ctx)
{
    switch ((uint32_t)ctx->gpr[11]) {
    case 1000: ctx->gpr[3] = vm_read32(0); return 1;
    case 1001: vm_write32(0, 0x5A5A5A5Au); ctx->gpr[3] = 0; return 1;
    case 1002: ctx->gpr[3] = run_guest_callback(); return 1;
    case 1003: ctx->gpr[3] = 0x33; return 1;
    default: return 0;
    }
}

static void h_load(ppu_context* ctx) { ctx->gpr[3] = vm_read32(0); }
static void h_callback(ppu_context* ctx) { ctx->gpr[3] = run_guest_callback(); }
static void h_ok(ppu_context* ctx) { ctx->gpr[3] = 0x44; }

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    vm_base = (uint8_t*)mmap(nullptr, 0xE0000000ull, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (vm_base == MAP_FAILED) return 2;
    ppu_vm_size = 0xE0000000u;
    ppu_recomp_register();
    ppu_register_function(GUEST_NULL_FN, guest_null_fn);
    ps3_hle_register_ctx(0x5A000001, "fixtureCtxLoad", h_load);
    ps3_hle_register(0x5A000002, "fixtureTableLoad", h_load);
    ps3_hle_register_ctx(0x5A000003, "fixtureCtxCallback", h_callback);
    ps3_hle_register(0x5A000004, "fixtureTableCallback", h_callback);
    ps3_hle_register_ctx(0x5A000005, "fixtureCtxOk", h_ok);

    ppu_context ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = 1;
    ctx.lr = 0x10100;
    ctx.gpr[1] = 0x40080000u;
    ctx.gpr[2] = GUEST_TOC;
    std::memset(vm_base + 0x40000000u, 0, 0x100000);
    const char* mode = argv[1];
    const uint64_t arg = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 0;

    if (!std::strcmp(mode, "syscall") || !std::strcmp(mode, "after-syscall")) {
        g_active_ctx = &ctx;
        ctx.gpr[11] = arg;
        lv2_syscall(&ctx);
        if (!std::strcmp(mode, "after-syscall")) guest_null_fn(&ctx);
    } else if (!std::strcmp(mode, "syscall-noctx")) {
        ctx.gpr[11] = arg;
        lv2_syscall(&ctx);
    } else if (!std::strcmp(mode, "hle") || !std::strcmp(mode, "after-hle")) {
        g_active_ctx = &ctx;
        ps3_hle_call((uint32_t)arg, &ctx);
        if (!std::strcmp(mode, "after-hle")) guest_null_fn(&ctx);
    } else if (!std::strcmp(mode, "guest")) {
        g_active_ctx = &ctx;
        guest_null_fn(&ctx);
    } else {
        return 2;
    }
    std::printf("RETURNED r3=0x%llX\n", (unsigned long long)ctx.gpr[3]);
    return 0;
}

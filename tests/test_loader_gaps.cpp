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
 */
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>
#include "win32_compat.h"

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
}

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    vm_base = (uint8_t*)mmap(nullptr, 0x20000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (vm_base == MAP_FAILED) return 2;
    ppu_vm_size = 0x20000000u;
    ppu_recomp_register();

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
    } else {
        return 2;
    }
    std::printf("RETURNED r3=0x%llX\n", (unsigned long long)ctx.gpr[3]);
    return 0;
}

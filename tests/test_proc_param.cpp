/* Manufactured ELF data only; exercises the production loader's PROC_PARAM handling.
 *
 * sys_process_get_sdk_version (lv2 syscall 25) answers g_ps3_sdk_version, and ppu_load_elf must set it
 * from the title's sys_process_param_t.sdk_version. Each case loads an ELF and reads the variable back:
 *   argv: <elf> <its sha256> <expected sdk version, hex>   (the sentinel means "left unchanged")
 */
#include "../runtime/syscalls/sys_memory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include "win32_compat.h"

extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x20000000u;
uint32_t ppu_load_elf(const char*);
extern uint32_t g_ps3_sdk_version;
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN, LPVOID, DWORD, DWORD*) { std::abort(); }
BOOL CloseHandle(HANDLE) { std::abort(); }
DWORD GetFileAttributesA(const char*) { std::abort(); }
int64_t sys_mmapper_allocate_memory(ppu_context*);
}
struct func_entry { uint32_t addr; void (*func)(ppu_context*); const char* name; };
extern const func_entry function_table[] = {{0, nullptr, nullptr}};
extern const uint64_t function_table_count = 0;
static char g_image_sha[65];
extern "C" const char* ppu_lift_image_sha256(void) { return g_image_sha; }

static constexpr uint32_t kSentinel = 0xDEADBEEFu;

int main(int argc, char** argv)
{
    if (argc != 4) { std::fprintf(stderr, "usage: %s <elf> <sha256> <expected>\n", argv[0]); return 2; }
    vm_base = (uint8_t*)mmap(nullptr, 0xE0000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (vm_base == MAP_FAILED) return 2;
    ppu_vm_size = 0xE0000000u;
    std::snprintf(g_image_sha, sizeof g_image_sha, "%s", argv[2]);
    g_ps3_sdk_version = kSentinel;
    if (ppu_load_elf(argv[1]) != 0x10000) { std::fprintf(stderr, "FAIL: load\n"); return 1; }
    const uint32_t want = (uint32_t)std::strtoul(argv[3], nullptr, 16);
    if (g_ps3_sdk_version != want) {
        std::fprintf(stderr, "FAIL: sdk version 0x%08X, want 0x%08X\n", g_ps3_sdk_version, want);
        return 1;
    }
    std::printf("PASS sdk version 0x%08X\n", g_ps3_sdk_version);
    return 0;
}

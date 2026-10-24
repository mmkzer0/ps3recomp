/*
 * ps3recomp - the sysPrxForUser CRT shims bound the guest spans they copy.
 *
 * _sys_memset/_sys_memcpy/_sys_memcmp and the length-taking string shims
 * (sysPrxForUser.c) translate a guest EA and then copy `size` bytes through it.
 * They checked neither the address nor the length, so a size that left guest
 * memory copied past the arena. The fixture maps an arena with a PROT_NONE
 * guard page right after it and checks that
 *   - an in-arena copy still works byte for byte;
 *   - a destination or source span that leaves the arena stops
 *     (hle-guest-range, naming the function and the argument) BEFORE the copy,
 *     so the guard page is never written (a write there used to fault);
 *   - the same for _sys_memset, _sys_strncpy and _sys_memcmp.
 *
 * The stop ends the process (PS3_STOP_EXIT_CODE), so every bad span runs in a
 * forked child; the parent checks the exit status and the machine line.
 *
 *   tests/test_sysprx_guest_range.c, linked against libps3recomp_runtime.
 */
#include "sysPrxForUser.h"
#include "ps3emu/stop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- the runtime globals this shim reads ---- */
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void* g_ps3_guest_caller = NULL;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }

/* ---- the rest of the host layer, unused on these paths ---- */
void ps3_host_exit(int status) { (void)status; _exit(1); }
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static const uint32_t ARENA = 0x10000;
static int fails;

#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

/* One refused call: the child makes it, the parent requires the stop. */
typedef void (*bad_call)(void);

static void expect_stop(bad_call fn, const char* func, const char* arg)
{
    int fds[2];
    if (pipe(fds) != 0) { CHECK(0, "pipe"); return; }
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        fn();
        _exit(1);   /* returned: the span was not refused */
    }
    close(fds[1]);
    char out[2048];
    size_t used = 0;
    ssize_t n;
    while (used < sizeof out - 1 && (n = read(fds[0], out + used, sizeof out - 1 - used)) > 0)
        used += (size_t)n;
    out[used] = 0;
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == PS3_STOP_EXIT_CODE,
          "%s(%s) stops (status 0x%X)", func, arg, status);
    char want[64];
    snprintf(want, sizeof want, "func=%s arg=%s", func, arg);
    const char* line = strstr(out, "PS3STOP v=1");
    CHECK(line && strstr(line, "kind=hle-guest-range") && strstr(line, want),
          "%s(%s) names itself in the stop machine line%s%s", func, arg,
          line ? ": " : " (no machine line)", line ? line : "");
}

static void copy_out_of_range(void) { _sys_memcpy((void*)(uintptr_t)(ARENA - 0x10), (const void*)(uintptr_t)0x100, 0x20); }
static void copy_from_out_of_range(void) { _sys_memcpy((void*)(uintptr_t)0x100, (const void*)(uintptr_t)(ARENA - 0x10), 0x20); }
static void copy_beyond_arena(void) { _sys_memcpy((void*)(uintptr_t)(ARENA + 0x1000), (const void*)(uintptr_t)0x100, 0x20); }
static void set_out_of_range(void) { _sys_memset((void*)(uintptr_t)(ARENA - 0x10), 0x5A, 0x20); }
static void strncpy_out_of_range(void) { _sys_strncpy((char*)(uintptr_t)(ARENA - 0x10), (const char*)(uintptr_t)0x100, 0x20); }
static void memcmp_out_of_range(void) { _sys_memcmp((const void*)(uintptr_t)0x100, (const void*)(uintptr_t)(ARENA - 0x10), 0x20); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* forked children must not repeat it */
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t* p = mmap(NULL, ARENA + page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { printf("FAIL: could not map the arena\n"); return 1; }
    if (mprotect(p + ARENA, page, PROT_NONE) != 0) { printf("FAIL: guard page\n"); return 1; }
    vm_base = p;
    ppu_vm_size = ARENA;   /* the whole arena; the guard page is not guest memory */

    memset(vm_base, 0xAB, ARENA);
    for (unsigned i = 0; i < 0x40; ++i) vm_base[0x100 + i] = (uint8_t)(0x40 + i);

    /* Still works: an in-arena copy moves the bytes and returns the guest EA. */
    void* ret = _sys_memcpy((void*)(uintptr_t)0x200, (const void*)(uintptr_t)0x100, 0x40);
    CHECK((uintptr_t)ret == 0x200, "_sys_memcpy returns its dst guest EA");
    CHECK(memcmp(vm_base + 0x200, vm_base + 0x100, 0x40) == 0, "an in-arena copy moves the bytes");
    _sys_memset((void*)(uintptr_t)0x300, 0x5A, 0x10);
    CHECK(vm_base[0x300] == 0x5A && vm_base[0x30F] == 0x5A && vm_base[0x310] == 0xAB,
          "_sys_memset writes exactly the span it was given");
    CHECK(_sys_memcmp((const void*)(uintptr_t)0x200, (const void*)(uintptr_t)0x200, 0x40) == 0,
          "_sys_memcmp over an in-arena span");
    _sys_strncpy((char*)(uintptr_t)0x400, (const char*)(uintptr_t)0x100, 4);
    CHECK(vm_base[0x400] == 0x40 && vm_base[0x401] == 0x41, "_sys_strncpy over an in-arena span");

    /* Every span that leaves the arena is a named stop, raised before the copy. */
    expect_stop(copy_out_of_range, "_sys_memcpy", "dst");
    expect_stop(copy_from_out_of_range, "_sys_memcpy", "src");
    expect_stop(copy_beyond_arena, "_sys_memcpy", "dst");
    expect_stop(set_out_of_range, "_sys_memset", "dst");
    expect_stop(strncpy_out_of_range, "_sys_strncpy", "dst");
    expect_stop(memcmp_out_of_range, "_sys_memcmp", "s2");

    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

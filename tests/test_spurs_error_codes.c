/* The SPURS return codes the guest sees: one API per error family that has one, and the
 * firmware/RPCS3 values of the families that have no HLE path (policy module, job).
 *
 * The values are literals here, not macros: the point is that the header's macros equal what the
 * firmware returns (libsre 4.93; RPCS3 cellSpurs.h at e53ac18f). A macro that drifts back to the
 * old 0x804107xx task range fails this test, and a value the firmware never builds must not be
 * defined at all (the #ifdef guards below).
 *
 * Firmware evidence: cellSpursCreateTask (code 0x02012520) returns 0x80410911 for a null taskset,
 * 0x80410910 for a misaligned one and calls the shared creator (0x02011ACC) whose PPU-side ELF
 * header check returns 0x80410907; cellSpursTaskExitCodeInitialize (0x02013638) 0x8041090F on its
 * state check. The task STAT path below reaches the HLE's own initialised-taskset check without an
 * initialised taskset. */
#include <ps3emu/ps3types.h>
#include "../libs/spurs/cellSpurs.h"

/* Values libsre 4.93 never builds: a site for any of them would make this fail to compile. */
#ifdef CELL_SPURS_TASK_ERROR_NOSYS
#error "TASK NOSYS (0x80410903) is not a firmware code"
#endif
#ifdef CELL_SPURS_TASK_ERROR_NOMEM
#error "TASK NOMEM (0x80410904) is not a firmware code"
#endif
#ifdef CELL_SPURS_TASK_ERROR_FAULT
#error "TASK FAULT (0x8041090D) is not a firmware code"
#endif
#ifdef CELL_SPURS_CORE_ERROR_NOMEM
#error "CORE NOMEM (0x80410704) is not a firmware code"
#endif

#include "../libs/spurs/cellSpurs.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x20000;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
/* Reached only where a test starts a task, which none of these modes does: CreateTask returns
 * before the dispatch on every code path exercised here. */
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)ls; (void)a; (void)b; (void)c; (void)d; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
unsigned long long ps3_ms_now(void) { return 0; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
void sys_event_queue_cancel_by_id(u32 id) { (void)id; }
void sys_event_queue_uncancel_by_id(u32 id) { (void)id; }
/* The creator asks the SDK version (libsre 0x02011B78); the fixture is the
 * runtime's own default, above 0x27FFFF. */
static uint32_t fixture_sdk = 0x00350001u;
uint32_t ps3_process_sdk_version(void) { return fixture_sdk; }

#define ERR_ELF 0x1000u
#define ERR_TS  0x3000u
#define ERR_TID 0x4000u

/* A 32-bit big-endian SPU ELF the PPU-side check accepts: the 8-byte magic,
 * e_machine 23, e_entry 0x3000, no program headers. */
static void make_min_elf(uint32_t ea)
{
    memset(vm_base + ea, 0, 0x40);
    memcpy(vm_base + ea, "\177ELF", 4);
    vm_base[ea + 4] = 1; vm_base[ea + 5] = 2; vm_base[ea + 6] = 1; vm_base[ea + 7] = 0;
    vm_base[ea + 0x13] = 23;                    /* e_machine, big-endian */
    vm_base[ea + 0x1A] = 0x30;                  /* e_entry 0x00003000 */
    /* e_phnum (+0x2C) = 0 */
}

static int g_failures;

#define CHECK_CODE(what, got, want)                                            \
    do {                                                                       \
        const unsigned g_ = (unsigned)(got), w_ = (unsigned)(want);            \
        if (g_ == w_)                                                          \
            printf("PASS %-42s 0x%08X\n", what, g_);                           \
        else {                                                                 \
            printf("FAIL %-42s 0x%08X, want 0x%08X\n", what, g_, w_);          \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 0x20000);
    if (!vm_base) return 1;

    /* CORE family: a null instance is the core NULL_POINTER. */
    CHECK_CODE("cellSpursInitialize(NULL) [core]",
               cellSpursInitialize(NULL, 1, 0, 0, 0), 0x80410711u);

    /* TASK family, through CreateTask: null, unaligned, a header the PPU
     * rejects, and the HLE's no-init STAT once the image is acceptable. */
    CHECK_CODE("cellSpursCreateTask(NULL ...) [task]",
               cellSpursCreateTask(NULL, NULL, NULL, NULL, 0, 0, 0), 0x80410911u);
    CHECK_CODE("cellSpursCreateTask(elf=...001) [task align]",
               cellSpursCreateTask((CellSpursTaskset*)0x1000, (CellSpursTaskId*)0x1100,
                                   (void*)0x1001, NULL, 0, 0, 0), 0x80410910u);
    CHECK_CODE("cellSpursCreateTask(zero image) [task noexec]",
               cellSpursCreateTask((CellSpursTaskset*)(uintptr_t)ERR_TS,
                                   (CellSpursTaskId*)(uintptr_t)ERR_TID,
                                   (void*)0x1000, NULL, 0, 0, 0), 0x80410907u);
    make_min_elf(ERR_ELF);
    CHECK_CODE("cellSpursCreateTask(valid ELF, no init) [task stat]",
               cellSpursCreateTask((CellSpursTaskset*)(uintptr_t)ERR_TS,
                                   (CellSpursTaskId*)(uintptr_t)ERR_TID,
                                   (void*)(uintptr_t)ERR_ELF, NULL, 0, 0, 0), 0x8041090Fu);

    /* TASK family, through a second API: the exit code object. */
    CHECK_CODE("cellSpursTaskExitCodeInitialize(NULL) [task]",
               cellSpursTaskExitCodeInitialize(NULL), 0x80410911u);
    CHECK_CODE("cellSpursTaskExitCodeInitialize(...01) [task align]",
               cellSpursTaskExitCodeInitialize((CellSpursTaskExitCode*)0x1001), 0x80410910u);

    /* Families with no HLE path that returns them: the constants themselves carry the firmware's
     * values (RPCS3 CellSpursTaskError / CellSpursPolicyModuleError / CellSpursJobError). */
    CHECK_CODE("CELL_SPURS_TASK_ERROR_AGAIN", CELL_SPURS_TASK_ERROR_AGAIN, 0x80410901u);
    CHECK_CODE("CELL_SPURS_TASK_ERROR_INVAL", CELL_SPURS_TASK_ERROR_INVAL, 0x80410902u);
    CHECK_CODE("CELL_SPURS_TASK_ERROR_NOEXEC", CELL_SPURS_TASK_ERROR_NOEXEC, 0x80410907u);
    CHECK_CODE("CELL_SPURS_TASK_ERROR_FATAL", CELL_SPURS_TASK_ERROR_FATAL, 0x80410914u);
    CHECK_CODE("CELL_SPURS_TASK_ERROR_SHUTDOWN", CELL_SPURS_TASK_ERROR_SHUTDOWN, 0x80410920u);
    CHECK_CODE("CELL_SPURS_POLICY_MODULE_ERROR_AGAIN", CELL_SPURS_POLICY_MODULE_ERROR_AGAIN, 0x80410801u);
    CHECK_CODE("CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER", CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER, 0x80410811u);
    CHECK_CODE("CELL_SPURS_JOB_ERROR_AGAIN", CELL_SPURS_JOB_ERROR_AGAIN, 0x80410A01u);
    CHECK_CODE("CELL_SPURS_JOB_ERROR_MEMORY_SIZE", CELL_SPURS_JOB_ERROR_MEMORY_SIZE, 0x80410A17u);
    CHECK_CODE("CELL_SPURS_JOB_ERROR_INVALID_BINARY", CELL_SPURS_JOB_ERROR_INVALID_BINARY, 0x80410A1Fu);

    free(vm_base);
    printf(g_failures ? "\n%d code(s) WRONG\n" : "\nall codes match the firmware\n", g_failures);
    return g_failures ? 1 : 0;
}

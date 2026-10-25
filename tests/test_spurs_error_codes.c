/* The SPURS return codes the guest sees: one API per error family that has one, and the
 * firmware/RPCS3 values of the families that have no HLE path (policy module, job).
 *
 * The values are literals here, not macros: the point is that the header's macros equal what the
 * firmware returns (libsre 4.93; RPCS3 cellSpurs.h at e53ac18f). A macro that drifts back to the
 * old 0x804107xx task range fails this test.
 *
 * Firmware evidence: cellSpursCreateTask (code 0x02012520) returns 0x80410911 for a null taskset,
 * 0x80410910 for a misaligned one; cellSpursTaskExitCodeInitialize (0x02013638) 0x8041090F on its
 * state check. The task STAT path below reaches the same check without an initialised taskset. */
#include <ps3emu/ps3types.h>
#include "../libs/spurs/cellSpurs.h"

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

    /* TASK family, through CreateTask: null, unaligned, and the no-init STAT. */
    CHECK_CODE("cellSpursCreateTask(NULL ...) [task]",
               cellSpursCreateTask(NULL, NULL, NULL, NULL, 0, 0, 0), 0x80410911u);
    CHECK_CODE("cellSpursCreateTask(elf=...001) [task align]",
               cellSpursCreateTask((CellSpursTaskset*)0x1000, (CellSpursTaskId*)0x1100,
                                   (void*)0x1001, NULL, 0, 0, 0), 0x80410910u);
    CHECK_CODE("cellSpursCreateTask(uninitialised) [task stat]",
               cellSpursCreateTask((CellSpursTaskset*)0x1000, (CellSpursTaskId*)0x1100,
                                   (void*)0x1000, NULL, 0, 0, 0), 0x8041090Fu);

    /* TASK family, through a second API: the exit code object. */
    CHECK_CODE("cellSpursTaskExitCodeInitialize(NULL) [task]",
               cellSpursTaskExitCodeInitialize(NULL), 0x80410911u);
    CHECK_CODE("cellSpursTaskExitCodeInitialize(...01) [task align]",
               cellSpursTaskExitCodeInitialize((CellSpursTaskExitCode*)0x1001), 0x80410910u);

    /* Families with no HLE path that returns them: the constants themselves carry the firmware's
     * values (RPCS3 CellSpursTaskError / CellSpursPolicyModuleError / CellSpursJobError). */
    CHECK_CODE("CELL_SPURS_TASK_ERROR_NOEXEC", CELL_SPURS_TASK_ERROR_NOEXEC, 0x80410907u);
    CHECK_CODE("CELL_SPURS_TASK_ERROR_FAULT", CELL_SPURS_TASK_ERROR_FAULT, 0x8041090Du);
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

/* The SPURS return codes the guest sees: one API per error family that has one, and the
 * firmware/RPCS3 values of the families that have no HLE path (policy module, job).
 *
 * The values are literals here, not macros: the point is that the header's macros equal what the
 * firmware returns (libsre 4.81 of the decrypted tree for the attribute, RequestIdleSpu,
 * SetMaxContention, flag-receiver and CreateTask2 checks recorded in cellSpurs.c's comments;
 * libsre 4.93 for the CreateTask sites below; RPCS3 cellSpurs.h at e53ac18f). A macro that
 * drifts back to the old 0x804107xx task range fails this test, and a value the firmware never
 * builds must not be defined at all (the #ifdef guards below). The policy module family is
 * real 0x804108xx: the firmware answers RequestIdleSpu and the flag receiver in it, which is
 * why those pins below take CELL_SPURS_POLICY_MODULE_ERROR_*.
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
#include <sys/wait.h>   /* the stop cases fork: the fixture is POSIX only (the driver skips elsewhere) */
#include <unistd.h>

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
/* CreateTaskset2's success path creates the taskset's two lv2 event flags.
 * None of this fixture's CreateTaskset2 pins reach creation (each returns
 * before the creator runs), so reaching the stub is a bug. */
int64_t sys_event_flag_create_host(u32 id_out, u64 init_pattern)
{ (void)id_out; (void)init_pattern; abort(); }
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

/* A refused guest span ends the process in a named stop (kind=hle-guest-range), so the call runs in a forked child
 * and the parent reads the machine line the stop prints on stderr. */
static void flag_receiver_straddling(void) { _cellSpursWorkloadFlagReceiver(0x20000u - 0x80u, 0, 1); }
static void flag_receiver_far(void) { _cellSpursWorkloadFlagReceiver(0x80000000u, 0, 1); }

static void stops_with(const char* what, void (*call)(void), const char* func, const char* arg)
{
    int fds[2];
    if (pipe(fds) != 0) { printf("FAIL %s: pipe\n", what); g_failures++; return; }
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]); dup2(fds[1], STDERR_FILENO); close(fds[1]);
        call();
        _exit(1);   /* returned: the span was not refused */
    }
    close(fds[1]);
    char out[4096];
    size_t used = 0;
    ssize_t n;
    while (used < sizeof out - 1 && (n = read(fds[0], out + used, sizeof out - 1 - used)) > 0) used += (size_t)n;
    out[used] = 0;
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    char want[160];
    snprintf(want, sizeof want, "func=%s arg=%s", func, arg);
    const char* line = strstr(out, "PS3STOP v=1");
    if (WIFEXITED(status) && WEXITSTATUS(status) == PS3_STOP_EXIT_CODE && line &&
        strstr(line, "kind=hle-guest-range") && strstr(line, want))
        printf("PASS %-42s stops: %s\n", what, want);
    else {
        printf("FAIL %-42s status 0x%X, stop line: %s\n", what, status, line ? line : "(none)");
        g_failures++;
    }
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 0x20000);
    if (!vm_base) return 1;

    /* Attribute limits (libsre 4.81 0x0200F518, core family: every ori
     * immediate in it is 0x7xx). The fixture attribute is built by the
     * 7-argument export like the SDK inline does; the attribute is a
     * host-order mirror written by this module's HLE, so the fixture touches
     * it the same way. */
    {
        const u32 ATTR = 0x5000u, PRIO = 0x5100u, PREEM = 0x5200u;
        CellSpursAttribute* a = (CellSpursAttribute*)(vm_base + ATTR);
        CHECK_CODE("_cellSpursAttributeInitialize(NULL) [core]",
                   _cellSpursAttributeInitialize((CellSpursAttribute*)0, 1, 0x350001, 2, 1, 1, 0),
                   0x80410711u);
        CHECK_CODE("EnableSystemWorkload(attr ...4) [core align]",
                   cellSpursAttributeEnableSystemWorkload(ATTR + 4, PRIO, 1, PREEM),
                   CELL_SPURS_CORE_ERROR_ALIGN);
        assert(_cellSpursAttributeInitialize((CellSpursAttribute*)(uintptr_t)ATTR, 1, 0x350001, 2, 1, 1, 0) == 0);
        memset(vm_base + PRIO, 1, 8);
        memset(vm_base + PREEM, 1, 8);
        CHECK_CODE("EnableSystemWorkload(no prio 1) [core inval]",
                   cellSpursAttributeEnableSystemWorkload(ATTR, PRIO + 0x10, 1, PREEM),
                   CELL_SPURS_CORE_ERROR_INVAL);
        a->nSpus = 0;
        CHECK_CODE("EnableSystemWorkload(nSpus 0) [core inval]",
                   cellSpursAttributeEnableSystemWorkload(ATTR, PRIO, 1, PREEM),
                   CELL_SPURS_CORE_ERROR_INVAL);
        assert(_cellSpursAttributeInitialize((CellSpursAttribute*)(uintptr_t)ATTR, 1, 0x350001, 2, 1, 1, 0) == 0);
        CHECK_CODE("EnableSystemWorkload(maxSpu 0) [core inval]",
                   cellSpursAttributeEnableSystemWorkload(ATTR, PRIO, 0, PREEM),
                   CELL_SPURS_CORE_ERROR_INVAL);
        CHECK_CODE("_cellSpursAttributeInitialize(1 SPU) (setup)",
                   _cellSpursAttributeInitialize((CellSpursAttribute*)(uintptr_t)ATTR, 1, 0x350001, 1, 1, 1, 0), 0);
        CHECK_CODE("EnableSystemWorkload(1 SPU) [core perm]",
                   cellSpursAttributeEnableSystemWorkload(ATTR, PRIO, 1, PREEM),
                   CELL_SPURS_CORE_ERROR_PERM);
        assert(_cellSpursAttributeInitialize((CellSpursAttribute*)(uintptr_t)ATTR, 1, 0x350001, 2, 1, 1, 0) == 0);
        a->exitIfNoWork = 1;
        CHECK_CODE("EnableSystemWorkload(exitIfNoWork) [core perm]",
                   cellSpursAttributeEnableSystemWorkload(ATTR, PRIO, 1, PREEM),
                   CELL_SPURS_CORE_ERROR_PERM);
        a->exitIfNoWork = 0;
        CHECK_CODE("EnableSystemWorkload(configured)",
                   cellSpursAttributeEnableSystemWorkload(ATTR, PRIO, 1, PREEM), 0);
        CHECK_CODE("EnableSystemWorkload(again) [core busy]",
                   cellSpursAttributeEnableSystemWorkload(ATTR, PRIO, 1, PREEM),
                   CELL_SPURS_CORE_ERROR_BUSY);
        /* The call must have recorded what it accepted: the enable flag, the
         * maxSpu and the isPreemptible bitmask live in the attribute now
         * (host-order mirror). The pre-#130 code answered CELL_OK and wrote
         * nothing, so every pin in this block fails against it. */
        CHECK_CODE("EnableSystemWorkload(flag stored)", a->flags, 0x02000000u);
        CHECK_CODE("EnableSystemWorkload(maxSpu stored)", a->swlMaxSpu, 1u);
        CHECK_CODE("EnableSystemWorkload(preem stored)", a->swlIsPreem, 3u);
    }

    /* InitializeWithAttribute2 (libsre 4.81 0x020083AC, core family): null
     * attr NULL_POINTER, attr & 7 ALIGN, revision above 2 INVAL. */
    {
        const u32 ATTR = 0x5800u;
        CellSpursAttribute* a = (CellSpursAttribute*)(vm_base + ATTR);
        CHECK_CODE("InitializeWithAttribute2(NULL attr) [core]",
                   cellSpursInitializeWithAttribute2((CellSpurs*)0x4000u, NULL),
                   0x80410711u);
        CHECK_CODE("InitializeWithAttribute2(attr ...4) [core align]",
                   cellSpursInitializeWithAttribute2((CellSpurs*)0x4000u,
                                                     (const CellSpursAttribute*)(ATTR + 4)),
                   CELL_SPURS_CORE_ERROR_ALIGN);
        assert(_cellSpursAttributeInitialize((CellSpursAttribute*)(uintptr_t)ATTR, 1, 0x350001, 2, 1, 1, 0) == 0);
        a->revision = 3;   /* a version the firmware never took */
        CHECK_CODE("InitializeWithAttribute2(revision 3) [core inval]",
                   cellSpursInitializeWithAttribute2((CellSpurs*)0x4000u,
                                                     (const CellSpursAttribute*)(uintptr_t)ATTR),
                   CELL_SPURS_CORE_ERROR_INVAL);
    }

    /* CreateTaskset2 (libsre 4.81 creator 0x02014A58): a misaligned spurs is
     * the task family's ALIGN, not a core code. */
    {
        const u32 ATTR = 0x5C00u;
        CHECK_CODE("CreateTaskset2(spurs ...1) [task align]",
                   cellSpursCreateTaskset2((CellSpurs*)0x1001u, (CellSpursTaskset*)0x3000u, 0),
                   0x80410910u);
        assert(_cellSpursTasksetAttribute2Initialize(ATTR, 0) == 0);
        vm_write32(ATTR + TS2ATTR_NAME_BUF, 0x1234);   /* task name buffer & 0xF */
        CHECK_CODE("CreateTaskset2(nameBuf ...4) [task align]",
                   cellSpursCreateTaskset2((CellSpurs*)0x1000u, (CellSpursTaskset*)0x3000u, ATTR),
                   0x80410910u);
    }

    /* CreateTask2 (libsre 4.81 0x02011F60, task family): taskset & 0x7F
     * ALIGN, an attr revision the firmware never took INVAL, and a taskset
     * that is not a Taskset2 (its size field reads 0 here) PERM. */
    CHECK_CODE("CreateTask2(NULL id) [task]",
               cellSpursCreateTask2((CellSpursTaskset*)0x6000u, 0, 0, 0, 0), 0x80410911u);
    CHECK_CODE("CreateTask2(taskset ...1) [task align]",
               cellSpursCreateTask2((CellSpursTaskset*)0x6001u, 0x6100u, 0, 0, 0),
               0x80410910u);
    vm_write32(0x6200u, 1);   /* CellSpursTaskAttribute2 revision at +0x00 */
    CHECK_CODE("CreateTask2(attr revision 1) [task inval]",
               cellSpursCreateTask2((CellSpursTaskset*)0x6000u, 0x6100u, 0, 0, 0x6200u),
               0x80410902u);
    vm_write32(0x6200u, 0);
    CHECK_CODE("CreateTask2(zero taskset) [task perm]",
               cellSpursCreateTask2((CellSpursTaskset*)0x6000u, 0x6100u, 0, 0, 0),
               CELL_SPURS_TASK_ERROR_PERM);

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


    /* _cellSpursWorkloadFlagReceiver reads and writes the instance's flag
     * word, receiver byte, 32-workload flag and exception word at the EA it
     * is given: past its null/alignment answers, a span that leaves guest
     * memory is a named stop, and one that fits (the arena is 0x20000 bytes)
     * is answered by the policy module family -- libsre 4.81 0x0200F260 uses
     * 0x8xx immediates throughout, unlike the neighbouring core-family
     * calls. */
    stops_with("_cellSpursWorkloadFlagReceiver(span straddles)", flag_receiver_straddling,
               "_cellSpursWorkloadFlagReceiver", "spurs");
    stops_with("_cellSpursWorkloadFlagReceiver(span far away)", flag_receiver_far,
               "_cellSpursWorkloadFlagReceiver", "spurs");
    CHECK_CODE("_cellSpursWorkloadFlagReceiver(NULL) [policy]",
               _cellSpursWorkloadFlagReceiver(0, 0, 1), CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    CHECK_CODE("_cellSpursWorkloadFlagReceiver(...0x40) [policy align]",
               _cellSpursWorkloadFlagReceiver(0x1040u, 0, 1), CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    CHECK_CODE("_cellSpursWorkloadFlagReceiver(fits, not enabled)",
               _cellSpursWorkloadFlagReceiver(0x1F000u, 0, 1), CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    free(vm_base);
    printf(g_failures ? "\n%d code(s) WRONG\n" : "\nall codes match the firmware\n", g_failures);
    return g_failures ? 1 : 0;
}

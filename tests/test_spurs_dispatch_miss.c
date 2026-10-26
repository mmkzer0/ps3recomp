/* A task or job the SPURS HLE cannot dispatch is a named stop, never CELL_OK (#20).
 *
 * The real libs/spurs/cellSpurs.c (cellSpursCreateTask, the job-chain walker's jc_run_one_job) over the real
 * runtime units. Where a test wants a task or job to start, the host dispatch is a counting stub; where it
 * wants the real registry's answer (nothing registered), the call goes through to the real dispatch. One mode
 * per process, because a stop ends the process (tools/test_spurs_dispatch_miss.py runs them and reads the
 * PS3STOP line):
 *
 *   task-ok             a parseable SPU ELF is dispatched once, with the image and the context the guest gave
 *   task-null-elf       the firmware's NULL_POINTER (RPCS3 _spurs::create_task), no task id taken
 *   task-unaligned-elf  the firmware's ALIGN, no task id taken
 *   task-bad-elf        spurs-task-halt, reason not-an-spu-elf, with the calling guest thread as the site
 *   task-bad-elf-host   the same, from a thread with no guest context: no site
 *   task-segment        spurs-task-halt, reason segment-outside-task-area (a PT_LOAD below 0x3000)
 *   task-unmapped-elf   hle-guest-range: an aligned ELF EA whose header is not inside guest memory
 *   task-no-lifted      a loadable ELF no lifted program is registered for: spurs-dispatch-miss
 *   job-ok              a job whose binary is in guest memory and dispatches goes on
 *   job-no-ea, job-no-size, job-out-of-range
 *                       spu-job-dma-list (reason eaBinary / sizeBinary / binary-range)
 *   job-no-lifted       a job binary no lifted program is registered for: spurs-dispatch-miss
 */
#define spu_workload_dispatch_async fixture_dispatch
#define spu_workload_dispatch_job fixture_dispatch_job
#include "../libs/spurs/cellSpurs.c"
#undef spu_workload_dispatch_async
#undef spu_workload_dispatch_job
#include <assert.h>
#include <stdatomic.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x40000;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
/* libsre 4.93 gates the context alignment on the SDK version (0x02011B78 asks
 * ps3_process_sdk_version, 0x02011B88/0x02011BD4 pick the mask). The runtime
 * answers from the title's PROC_PARAM; the fixture sets it per mode and starts
 * at the runtime's own default (runtime/syscalls/sdk_version_weak.c). */
static uint32_t fixture_sdk = 0x00350001u;
uint32_t ps3_process_sdk_version(void) { return fixture_sdk; }
int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size, uint32_t args_ea);
int spu_workload_dispatch_job(const uint8_t* image, uint32_t image_size, uint32_t job_ea, uint32_t job_desc_size);
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
/* Reached only by the real async worker, which no mode here starts (a miss stops first). */
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)ls; (void)a; (void)b; (void)c; (void)d; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
unsigned long long ps3_ms_now(void) { return 0; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
void sys_event_queue_cancel_by_id(u32 id) { (void)id; }
void sys_event_queue_uncancel_by_id(u32 id) { (void)id; }
/* The restart scan's entry point (runtime/spu/spu_workload.c; no header declares it). */
void spu_taskset_start_pending(uint32_t taskset_ea);

static int real_dispatch;                       /* 1: go through to the real registry */
static unsigned dispatches;
static const uint8_t* dispatched_image;
static uint32_t dispatched_size, dispatched_context;
int fixture_dispatch(const uint8_t* image, uint32_t size, uint32_t context)
{
    if (real_dispatch) return spu_workload_dispatch_async(image, size, context);
    dispatches++; dispatched_image = image; dispatched_size = size; dispatched_context = context;
    return 1;
}
static unsigned job_dispatches;
int fixture_dispatch_job(const uint8_t* image, uint32_t size, uint32_t job_ea, uint32_t desc_size)
{
    if (real_dispatch) return spu_workload_dispatch_job(image, size, job_ea, desc_size);
    (void)image; (void)size; (void)job_ea; (void)desc_size;
    job_dispatches++;
    return 1;
}

enum { TASKSET = 0x4000, TASK_ID_OUT = 0x2000, ELF = 0x10000, TASK_CTX = 0x20000,
       LSP = 0x21000, ATTR = 0x22000, JOB = 0x3000, BINARY = 0x30000 };

static void be16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* A 32-bit big-endian SPU ELF with one PT_LOAD at `vaddr`: spu_elf_image_size says 100. */
static void make_elf(uint8_t* image, uint32_t vaddr)
{
    memcpy(image, "\177ELF", 4); image[4] = 1; image[5] = 2; image[6] = 1;
    image[7] = 0;   /* EI_OSABI: the creator compares all 8 bytes (0x02011BFC) */
    be16(image + 0x10, 2); be16(image + 0x12, 23); be32(image + 0x14, 1);
    be32(image + 0x18, vaddr); be32(image + 0x1C, 52);
    be16(image + 0x28, 52); be16(image + 0x2A, 32); be16(image + 0x2C, 1);
    be32(image + 52, 1); be32(image + 56, 84); be32(image + 60, vaddr); be32(image + 64, vaddr);
    be32(image + 68, 16); be32(image + 72, 16); be32(image + 76, 5);
    be32(image + 80, 16); memset(image + 84, 0x5A, 16);
}

/* An ELF header with room for `n` program headers; the caller fills them with phdr(). */
static void make_elf_phdrs(uint8_t* image, uint16_t n)
{
    memset(image, 0, 0x40 + (size_t)n * 32 + 0x20);
    memcpy(image, "\177ELF", 4); image[4] = 1; image[5] = 2; image[6] = 1;
    be16(image + 0x10, 2); be16(image + 0x12, 23); be32(image + 0x14, 1);
    be32(image + 0x18, 0x3000); be32(image + 0x1C, 52);
    be16(image + 0x28, 52); be16(image + 0x2A, 32); be16(image + 0x2C, n);
}

static void phdr(uint8_t* image, unsigned k, uint32_t type, uint32_t off, uint32_t vaddr,
                 uint32_t paddr, uint32_t filesz, uint32_t memsz, uint32_t flags)
{
    uint8_t* p = image + 52 + (size_t)k * 32;
    be32(p + 0, type); be32(p + 4, off); be32(p + 8, vaddr); be32(p + 12, paddr);
    be32(p + 16, filesz); be32(p + 20, memsz); be32(p + 24, flags); be32(p + 28, 4);
}

static void setup(void)
{
    memset(vm_base, 0, ppu_vm_size);
    g_ydkj_real_taskset_ea = TASKSET;           /* the HLE's "taskset initialised" flag */
    dispatches = job_dispatches = 0; dispatched_image = NULL; dispatched_size = dispatched_context = 0;
}

/* A context at TASK_CTX with a size the creator accepts: the firmware returns
 * 0x80410902 for any context with fewer than 0x400 bytes (0x02011BF4). */
static int create_task_ex(uint32_t elf, uint32_t ctx, uint32_t size, uint32_t lsp)
{
    return (int)cellSpursCreateTask((CellSpursTaskset*)(uintptr_t)TASKSET, (CellSpursTaskId*)(uintptr_t)TASK_ID_OUT,
                                    (void*)(uintptr_t)elf, (void*)(uintptr_t)ctx, size, lsp, 0);
}

static int create_task(uint32_t elf)
{
    return create_task_ex(elf, TASK_CTX, 0x2000, 0);
}

static int enabled_bits_clear(void)
{
    for (unsigned i = 0; i < 16; i++) if (vm_base[TASKSET + 0x30 + i]) return 0;
    return 1;
}

#define CHECK(c, what) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", what); return 1; } } while (0)

static void job_descriptor(uint32_t ea_bin, uint32_t size_bin)
{
    be32(vm_base + JOB + 0x00, 0);              /* eaBinary high word */
    be32(vm_base + JOB + 0x04, ea_bin);
    be32(vm_base + JOB + 0x08, (size_bin >> 4) << 16);
}

int main(int argc, char** argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s mode\n", argv[0]); return 2; }
    const char* mode = argv[1];
    vm_base = calloc(1, ppu_vm_size);
    assert(vm_base);
    static ppu_context guest = { .lr = 0x00123450, .thread_id = 7 };
    g_active_ctx = &guest;
    setup();

    if (!strcmp(mode, "task-ok")) {
        make_elf(vm_base + ELF, 0x3000);
        CHECK(create_task(ELF) == 0, "a parseable task returns CELL_OK");
        CHECK(dispatches == 1 && dispatched_image == vm_base + ELF && dispatched_size == 100 &&
              dispatched_context == TASK_CTX, "the task is dispatched once with its image and context");
        CHECK(vm_read32(TASK_ID_OUT) == 0, "the task id comes back");
    } else if (!strcmp(mode, "task-null-elf")) {
        CHECK((uint32_t)create_task(0) == 0x80410911u, "a null ELF is NULL_POINTER");
        CHECK(dispatches == 0 && enabled_bits_clear(), "and no task id was taken");
    } else if (!strcmp(mode, "task-unaligned-elf")) {
        make_elf(vm_base + ELF + 8, 0x3000);
        CHECK((uint32_t)create_task(ELF + 8) == 0x80410910u, "an ELF not 16-byte aligned is ALIGN");
        CHECK(dispatches == 0 && enabled_bits_clear(), "and no task id was taken");
    } else if (!strcmp(mode, "task-ok-phdr-paddr")) {
        /* RPCS3 stops the program-header scan at the first header, of ANY type, whose p_paddr is at
         * or past the task bottom (cellSpursSpu.cpp:2014). A later PT_LOAD outside the area is then
         * never reached. The segment below is outside the area at the TOP (vaddr+memsz > 0x40000),
         * which the PPU-side header check does not test: it only rejects p_vaddr <= 0x2FFF. */
        make_elf_phdrs(vm_base + ELF, 2);
        phdr(vm_base + ELF, 0, 2 /*PT_DYNAMIC*/, 0x100, 0, 0x50000, 0, 0, 0);
        phdr(vm_base + ELF, 1, 1 /*PT_LOAD*/, 0x100, 0x3F000, 0x3000, 0x10, 0x2000, 5);
        CHECK(create_task(ELF) == 0, "a header past the task bottom ends the scan");
        CHECK(dispatches == 1, "and the load carries on to dispatch");
    } else if (!strcmp(mode, "task-ok-vaddr-wrap")) {
        /* p_vaddr + p_memsz is 32-bit in RPCS3 (:2023): 0xFFFFF000 + 0x2000 wraps to 0x1000,
         * inside the task area. The 64-bit sum used here said 0x100001000 and halted. p_paddr is
         * inside the task bottom, so the scan does not end before the check. */
        make_elf_phdrs(vm_base + ELF, 1);
        phdr(vm_base + ELF, 0, 1, 0x100, 0xFFFFF000u, 0x3000, 0x10, 0x2000, 5);
        CHECK(create_task(ELF) == 0, "a segment whose size wraps into the area is inside it");
        CHECK(dispatches == 1, "and the load carries on to dispatch");
    } else if (!strcmp(mode, "task-ok-empty-elf")) {
        /* No program headers and no sections: nothing to load, and RPCS3 accepts it (:2006 only
         * rejects an image the parser cannot read). The old size check called it not-an-spu-elf. */
        make_elf_phdrs(vm_base + ELF, 0);
        CHECK(create_task(ELF) == 0, "an ELF with no loadable bytes is accepted");
        CHECK(dispatches == 1 && dispatched_size == 0, "and dispatched with size 0");
    } else if (!strcmp(mode, "task-restart-bit3")) {
        /* TaskInfo's low three bits are flags; bit 3 is not. RPCS3 masks 0xFFFFFFF8 (:1760) and
         * the loader rejects what remains as INVAL (:1999). The restart scan masked four bits. */
        memset(vm_base + TASKSET, 0, 0x2000);
        make_elf(vm_base + ELF, 0x3000);
        vm_base[TASKSET + 0x10] = 0x80;                  /* task 0 ready */
        vm_base[TASKSET + 0x30] = 0x80;                  /* task 0 enabled */
        be32(vm_base + TASKSET + 0x80 + 0x14, ELF | 8u);  /* TaskInfo[0].elf, bit 3 set */
        spu_taskset_start_pending(TASKSET);
        fprintf(stderr, "FAIL: the restart scan loaded a task whose ELF address is not 16-byte aligned\n");
        return 1;
    } else if (!strcmp(mode, "task-bad-elf")) {
        /* The PPU-side header check (0x02011BFC..0x02011C24): the first 8 bytes
         * must be 7F454C46 01020100, so a non-ELF image is NOEXEC here -- a
         * return value, never the SPU-side halt. */
        memset(vm_base + ELF, 0xA5, 0x100);     /* aligned, mapped, and not an ELF */
        CHECK((uint32_t)create_task(ELF) == 0x80410907u, "a non-ELF image is NOEXEC on the PPU");
        CHECK(dispatches == 0 && enabled_bits_clear() && vm_read32(TASK_ID_OUT) == 0,
              "and no task id was taken or dispatched");
    } else if (!strcmp(mode, "task-bad-machine")) {
        make_elf(vm_base + ELF, 0x3000);
        be16(vm_base + ELF + 0x12, 2);          /* EM_PPC64, not EM_SPU (23) */
        CHECK((uint32_t)create_task(ELF) == 0x80410907u, "e_machine != 23 is NOEXEC");
    } else if (!strcmp(mode, "task-bad-entry")) {
        make_elf(vm_base + ELF, 0x3000);
        be32(vm_base + ELF + 0x18, 0x2FFF);     /* must be > 0x2FFF */
        CHECK((uint32_t)create_task(ELF) == 0x80410907u, "e_entry <= 0x2FFF is NOEXEC");
    } else if (!strcmp(mode, "task-vaddr-low")) {
        /* A PT_LOAD below the task area. The ELF's e_entry and its other field
         * stay above 0x2FFF, so only the program-header test rejects it. */
        make_elf(vm_base + ELF, 0x3000);
        be32(vm_base + ELF + 52 + 8, 0x2FFF);
        CHECK((uint32_t)create_task(ELF) == 0x80410907u, "a PT_LOAD vaddr <= 0x2FFF is NOEXEC");
        CHECK(dispatches == 0 && enabled_bits_clear(), "and no task id was taken");
    } else if (!strcmp(mode, "task-ctx-align")) {
        /* 0x02011B78: above SDK 0x27FFFF the context must be 128-byte aligned;
         * at or below it 16 bytes (0x02011B88 / 0x02011BD4). 0x20040 passes the
         * 16-byte test and fails the 128-byte one. */
        make_elf(vm_base + ELF, 0x3000);
        fixture_sdk = 0x00350001u;
        CHECK((uint32_t)create_task_ex(ELF, 0x20040, 0x1000, 0) == 0x80410910u,
              "a context at 0x20040 is misaligned above SDK 0x27FFFF");
        fixture_sdk = 0x00270000u;
        CHECK(create_task_ex(ELF, 0x20040, 0x1000, 0) == 0, "the same context is accepted at or below it");
        CHECK(dispatches == 1, "and the task starts");
    } else if (!strcmp(mode, "task-ctx-size")) {
        /* 0x02011BF4: a context with fewer than 0x400 bytes is INVAL, and the
         * check is skipped only for a null context. */
        make_elf(vm_base + ELF, 0x3000);
        CHECK((uint32_t)create_task_ex(ELF, TASK_CTX, 0x3FF, 0) == 0x80410902u,
              "a context shorter than 0x400 bytes is INVAL");
        CHECK(create_task_ex(ELF, TASK_CTX, 0x400, 0) == 0, "0x400 bytes is accepted");
    } else if (!strcmp(mode, "task-ls-pattern")) {
        /* 0x02011D6C: the pattern may mark no more 2 KiB blocks than the context
         * save area holds; 0x2000 bytes give (0x2000-1024)/2048 = 3 slots. */
        make_elf(vm_base + ELF, 0x3000);
        be32(vm_base + LSP + 0,  0x01000000u);
        be32(vm_base + LSP + 4,  0x01000000u);
        be32(vm_base + LSP + 8,  0x01000000u);
        be32(vm_base + LSP + 12, 0x01000000u);
        CHECK((uint32_t)create_task_ex(ELF, TASK_CTX, 0x2000, LSP) == 0x80410902u,
              "four blocks for three slots is INVAL");
        be32(vm_base + LSP + 12, 0);
        CHECK(create_task_ex(ELF, TASK_CTX, 0x2000, LSP) == 0, "three blocks for three slots is accepted");
    } else if (!strcmp(mode, "task-ls-pattern-word0")) {
        /* 0x02011D74: the first word's top six bits (local store 0..0x5FF) must
         * be clear. */
        make_elf(vm_base + ELF, 0x3000);
        be32(vm_base + LSP + 0, 0x80000000u);
        CHECK((uint32_t)create_task_ex(ELF, TASK_CTX, 0x2000, LSP) == 0x80410902u,
              "a pattern marking the first 2 KB block is INVAL");
    } else if (!strcmp(mode, "task-no-slot")) {
        /* 0x02011F20: the creator's slot scan gives up with AGAIN. */
        make_elf(vm_base + ELF, 0x3000);
        memset(vm_base + TASKSET + 0x30, 0xFF, 16);   /* all 128 enabled bits set */
        CHECK((uint32_t)create_task(ELF) == 0x80410901u, "no free task slot is AGAIN");
        CHECK(dispatches == 0, "and nothing is dispatched");
    } else if (!strcmp(mode, "task-attr-bad-elf")) {
        /* _cellSpursTaskAttributeInitialize 0x02010D3C runs the same header check
         * (ori 0x907 at 0x02010EC4) and a null ELF is its 0x80410911. */
        CellSpursTaskAttribute* attr = (CellSpursTaskAttribute*)(uintptr_t)ATTR;
        memset(vm_base + ELF, 0xA5, 0x100);
        CHECK((uint32_t)_cellSpursTaskAttributeInitialize(attr, 1, 0x00350001u, ELF, TASK_CTX, 0x1000, 0, 0)
              == 0x80410907u, "the attribute initializer rejects a non-ELF image with NOEXEC");
        make_elf(vm_base + ELF, 0x3000);
        CHECK(_cellSpursTaskAttributeInitialize(attr, 1, 0x00350001u, ELF, TASK_CTX, 0x1000, 0, 0) == 0,
              "and accepts a real SPU ELF");
    } else if (!strcmp(mode, "task-segment")) {
        /* Accepted by the PPU check (vaddr > 0x2FFF) with a segment that leaves
         * the task area at the top: the SPU-side loader halts. */
        make_elf_phdrs(vm_base + ELF, 1);
        phdr(vm_base + ELF, 0, 1, 84, 0x3F000, 0x3000, 16, 0x2000, 5);
        (void)create_task(ELF);
        fprintf(stderr, "FAIL: cellSpursCreateTask returned for an ELF the policy module cannot load\n");
        return 1;
    } else if (!strcmp(mode, "task-unmapped-elf")) {
        (void)create_task(ppu_vm_size - 0x10);  /* aligned; the 0x34-byte header ends past guest memory */
        fprintf(stderr, "FAIL: cellSpursCreateTask read an ELF header outside guest memory\n");
        return 1;
    } else if (!strcmp(mode, "task-no-lifted")) {
        real_dispatch = 1;                      /* the real registry: nothing is registered for this image */
        make_elf(vm_base + ELF, 0x3000);
        (void)create_task(ELF);
        fprintf(stderr, "FAIL: cellSpursCreateTask reported a task started that no lifted program can run\n");
        return 1;
    } else if (!strcmp(mode, "job-ok")) {
        memset(vm_base + BINARY, 0x11, 0x100);
        job_descriptor(BINARY, 0x100);
        jc_run_one_job(JOB, 0, 0x100);
        CHECK(job_dispatches == 1, "a job whose binary is in guest memory is dispatched");
    } else if (!strcmp(mode, "job-no-ea") || !strcmp(mode, "job-no-size") || !strcmp(mode, "job-out-of-range")) {
        if (!strcmp(mode, "job-no-ea")) job_descriptor(0, 0x100);
        else if (!strcmp(mode, "job-no-size")) job_descriptor(BINARY, 0);
        else job_descriptor(ppu_vm_size - 0x80, 0x100);   /* crosses the end of guest memory */
        jc_run_one_job(JOB, 0, 0x100);
        fprintf(stderr, "FAIL: the job chain walker went on with a binary that is not there\n");
        return 1;
    } else if (!strcmp(mode, "job-no-lifted")) {
        real_dispatch = 1;
        memset(vm_base + BINARY, 0x11, 0x100);
        job_descriptor(BINARY, 0x100);
        jc_run_one_job(JOB, 0, 0x100);
        fprintf(stderr, "FAIL: the job chain walker went on with a job no lifted program can run\n");
        return 1;
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }
    printf("PASS: %s\n", mode);
    free(vm_base);
    return 0;
}

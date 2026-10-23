/* spurs_job.c — stage and enter a SPURS jobchain job the way Sony's jm2 does.
 *
 * A job binary is not an SPU ELF and not a policy module: it is a raw image
 * built by the SDK's job_elf-to-bin, linked against Sony's job_start_w_crt.o.
 * That CRT is what every job entry actually runs first — it saves r3/r4, runs
 * the ctors, and calls
 *
 *     void cellSpursJobMain2(CellSpursJobContext2 *jobContext,   // r3
 *                            CellSpursJob256      *job);         // r4
 *
 * (target/spu/include/cell/spurs/job_chain.h). Both are LOCAL STORE pointers,
 * so the manager must have laid the whole working set out in LS before entry.
 * Entering a job without that context is what left LBP's job parked with zero
 * DMA traffic: it reads its ioBuffer pointer out of a zeroed context and stalls.
 *
 * The layout is pinned by _cellSpursCheckJob() in
 * target/common/include/cell/spurs/job_descriptor.h, which sums the exact LS a
 * job consumes. For a jobchain it runs with align = 1024, isScratchStackUnified
 * = 1, maxUsableMemorySize = CELL_SPURS_MAX_SIZE_JOB_MEMORY (234 KB):
 *
 *     align1024(sizeBinary<<4)                                   binary + bss
 *   + align1024(sizeInOrInOut)                                   ioBuffer
 *   + align1024(sizeOut)                                         oBuffer
 *   + align1024((sizeScratch<<4) + (sizeStack ? sizeStack<<4 : 8192))
 *                                                                scratch + stack
 *   + per cache entry: align1024(size)                           cacheBuffer[0..3]
 *
 * We reproduce that arithmetic rather than invent a layout, so a job that
 * range-checks its own buffers sees what it would on hardware. The binary's
 * memory is its image plus the bss that the high word of eaBinary names.
 *
 * Only two CRT versions are modelled, JOBCRT Ver13 and Ver14; each gets the
 * context's jm interface pointer and the kernel's view of LS 0x100-0x2FF, and
 * Ver14 the two jm entries in the job runtime area. Any other version is a
 * named stop. See docs/RUNTIME.md, "SPURS job start".
 *
 * Load address: the job binary is position-independent (job_start_w_crt derives
 * its load delta with a brsl-to-next / subtract-link-time-address idiom), so jm2
 * may place it anywhere. We place it at LS 0 — the lifted C is keyed to
 * link-time addresses, which makes the delta zero and the lifted branch targets
 * exact.
 */

#include "spu_coherency.h"
#include "spu_workload.h"
#include "spu_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/log_first.h"
#include "ps3emu/stop.h"

extern uint8_t* vm_base;
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);

/* ---- guest reads (big-endian) ------------------------------------------ */
static uint32_t g32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint16_t g16(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}
static uint64_t g64(uint32_t ea)
{
    return ((uint64_t)g32(ea) << 32) | (uint64_t)g32(ea + 4);
}

/* ---- local-store writes (big-endian) ----------------------------------- */
static void ls32(uint8_t* ls, uint32_t o, uint32_t v)
{
    ls[o] = (uint8_t)(v >> 24); ls[o+1] = (uint8_t)(v >> 16);
    ls[o+2] = (uint8_t)(v >> 8); ls[o+3] = (uint8_t)v;
}
static void ls16(uint8_t* ls, uint32_t o, uint16_t v)
{
    ls[o] = (uint8_t)(v >> 8); ls[o+1] = (uint8_t)v;
}

/* CellSpursJobHeader, 48 B — job_descriptor.h */
enum {
    JH_EA_BINARY           = 0x00,   /* be u64; low bit = CACHE_INHIBIT flag */
    JH_SIZE_BINARY         = 0x08,   /* be u16; bytes >> 4                   */
    JH_SIZE_DMA_LIST       = 0x0A,   /* be u16; input list size in BYTES     */
    JH_EA_HIGH_INPUT       = 0x0C,
    JH_USE_INOUT_BUFFER    = 0x10,
    JH_SIZE_IN_OR_INOUT    = 0x14,
    JH_SIZE_OUT            = 0x18,
    JH_SIZE_STACK          = 0x1C,   /* be u16; quadwords */
    JH_SIZE_SCRATCH        = 0x1E,   /* be u16; quadwords */
    JH_EA_HIGH_CACHE       = 0x20,
    JH_SIZE_CACHE_DMA_LIST = 0x24,   /* be u32; bytes */
    JH_JOB_TYPE            = 0x2C,
    JH_SIZE                = 0x30,
};

/* CellSpursJobContext2, 48 B — job_context_types.h. SPU pointers are 32-bit LS
 * offsets and the struct lives in local store, so every field is big-endian. */
enum {
    JC_IO_BUFFER          = 0x00,
    JC_CACHE_BUFFER       = 0x04,   /* void *cacheBuffer[4] */
    JC_SIZE_JOB_DESC      = 0x14,   /* sizeJobDescriptor:4 + pad:28 */
    JC_NUM_IO_BUFFER      = 0x18,
    JC_NUM_CACHE_BUFFER   = 0x1A,
    JC_O_BUFFER           = 0x1C,
    JC_S_BUFFER           = 0x20,
    JC_DMA_TAG            = 0x24,
    JC_EA_JOB_DESCRIPTOR  = 0x28,   /* be u64 */
    JC_SIZE               = 0x30,
};

#define ALIGN1024(x)  (((uint64_t)(x) + 1023u) & ~(uint64_t)1023u)
#define JOB_DMA_TAG   20u          /* "one of: {20,21}" — job_context_types.h */
#define JOB_STACK_DEFAULT 8192u    /* _cellSpursCheckJob's sizeStack==0 default */

/* Top of local store, from the top down: the job descriptor (<= 1 KB), the
 * JobContext2 (48 B) with the jm interface object in the 16 bytes after it,
 * and below those the job runtime area (SPU_JOB_RT_SIZE). Everything the job
 * itself owns -- image, bss, buffers, stack -- lies below JOB_RT_LS. */
#define JOB_DESC_LS   (SPU_LS_SIZE - 0x400u)
#define JOB_CTX_LS    (JOB_DESC_LS - 0x40u)
#define JOB_JM_IFACE  (JOB_CTX_LS + 0x30u)
#define JOB_RT_LS     (JOB_CTX_LS - SPU_JOB_RT_SIZE)

/* The CRT marker at LS 0x30 (the first 12 bytes of job_start_w_crt.o's header).
 * The PM accepts any "JOBCRT Ver" suffix; this loader models two CRTs only. */
#define JOB_CRT_MARKER_LS  0x30u
#define JOB_CRT_MARKER_LEN 12u

/* The Ver14 jm interface: two LS pointers the PM plants for the CRT (word 0
 * its scheduler switch, word 1 its coroutine context swap). The CRT stashes
 * word 1 and calls it, with the tag status it just read in r3, when a DMA tag
 * it waits on is not complete. This loader runs one job at a time, so there
 * is nothing to switch to: word 1 is a routine that waits for the MFC and
 * returns. The PM's swap preserves r0-r2 and r80-r127 and hands back whatever
 * the other context left in r3-r79; this routine preserves every register but
 * r3, which carries the tag status (all ones) on return. The tag mask channel
 * is left at -1.
 *
 *     0x10 loop: il      $r3, -1
 *               wrch    MFC_WrTagMask, $r3
 *               il      $r3, 0                   ; immediate update
 *               wrch    MFC_WrTagUpdate, $r3
 *               rdch    $r3, MFC_RdTagStat
 *               ceqi    $r3, $r3, -1             ; every tag group complete?
 *               brz     $r3, loop
 *               bi      $r0
 *
 * Idle tag groups read as complete, so the loop ends exactly when every
 * command the job issued has. Word 0 is never called by a Ver14 image; it
 * points at a trap (a `stop` with SPU_JOB_RT_TRAP_STOP) so a call that does
 * come is a named stop rather than a branch to LS 0, which ends the job as if
 * it had finished. */
#define JOB_RT_TRAP   0x00u
#define JOB_RT_SWAP   0x10u
static const uint32_t s_job_rt_swap[] = {
    0x40FFFF83u,   /* il      $r3, -1                        */
    0x21A00B03u,   /* wrch    MFC_WrTagMask, $r3             */
    0x40800003u,   /* il      $r3, 0                         */
    0x21A00B83u,   /* wrch    MFC_WrTagUpdate, $r3           */
    0x01A00C03u,   /* rdch    $r3, MFC_RdTagStat             */
    0x7CFFC183u,   /* ceqi    $r3, $r3, -1                   */
    0x207FFD03u,   /* brz     $r3, loop (-6 words)           */
    0x35000000u,   /* bi      $r0                            */
};

static void job_rt_plant(uint8_t* ls)
{
    ls32(ls, JOB_RT_LS + JOB_RT_TRAP, SPU_JOB_RT_TRAP_STOP);
    for (uint32_t i = 0; i < sizeof s_job_rt_swap / sizeof s_job_rt_swap[0]; i++)
        ls32(ls, JOB_RT_LS + JOB_RT_SWAP + i * 4u, s_job_rt_swap[i]);
    ls32(ls, JOB_JM_IFACE,      JOB_RT_LS + JOB_RT_TRAP);
    ls32(ls, JOB_JM_IFACE + 4u, JOB_RT_LS + JOB_RT_SWAP);
}

/* Local store of the most recent job, kept alive past the run.
 *
 * A title can poll the SPU that runs a job chain with sys_spu_thread_read_LS,
 * identifying it by the CHAIN HANDLE rather than an lv2 thread id -- Tokyo
 * Jungle reads its bus counts that way, 45k polls of offset 0 in a 45 s run.
 * The job context is a stack local, so by the time the PPU looks there is
 * nothing to read and every poll fails. Retain the store and let the syscall
 * resolve a chain handle to it. */
static uint8_t  s_job_ls[SPU_LS_SIZE];
static int      s_job_ls_valid;
uint32_t        g_spurs_job_ls_handle;   /* set by the chain walker */

const uint8_t* spurs_job_ls_for_handle(uint32_t handle)
{
    if (!s_job_ls_valid || !handle || handle != g_spurs_job_ls_handle) return 0;
    return s_job_ls;
}

/* Last outbound mailbox posted by a finished job (see below). */
/* THREAD-LOCAL. A job's answer is read back by the completion event that
 * follows it, and jc_execute signals immediately after each job -- but the walk
 * runs on whichever guest thread drives the chain, and more than one can be in
 * flight. As plain globals these were clobbered between a job finishing and its
 * own completion being pushed, so a query could be answered with an unrelated
 * job's mailbox. Per-thread keeps each job paired with its own answer. */
/* __declspec(thread) for MSVC only: MinGW GCC ignores it, which would make
 * these process-wide. spurs_job.c and cellSpurs.c must agree. */
#if defined(_MSC_VER)
#  define SPURS_JOB_TLS __declspec(thread)
#else
#  define SPURS_JOB_TLS __thread
#endif
SPURS_JOB_TLS uint32_t g_spurs_job_mbox, g_spurs_job_mbox_intr;
/* The command this job ran, read from its own command block. Only a query
 * expects an answer back through the completion event. */
SPURS_JOB_TLS uint32_t g_spurs_job_cmd;
SPURS_JOB_TLS int g_spurs_job_mbox_valid;
/* The SPU number a job sees (cellSpursGetCurrentSpuId, kernel LS 0x1C8). A host
 * thread that runs jobs concurrently with others must claim its own: titles
 * key per-SPU resources on it (Edge output rings, one RSX label per SPU), and
 * two jobs that both think they are SPU 0 corrupt each other's ring. */
SPURS_JOB_TLS uint32_t g_spurs_job_spu_num;
void spurs_job_set_spu_num(uint32_t n) { g_spurs_job_spu_num = n; }

/* What the descriptor asked for, for the spu-job-memory stop. Sizes in bytes. */
typedef struct job_mem {
    uint32_t job_ea;
    int      image_id;
    uint32_t size_bin;
    uint64_t bss;
    uint32_t size_io, size_out;
    uint64_t size_scr, size_stk;
    uint64_t cache;            /* cache entry sizes read so far, the failing one included */
} job_mem;

/* p is the end address of the region that crosses JOB_RT_LS (64-bit, so a huge
 * descriptor size names itself instead of wrapping). */
PS3_NORETURN static void job_memory_stop(const job_mem* m, const char* region, uint64_t p)
{
    char job_s[16], img_s[16], bin_s[16], bss_s[24], mem_s[24], io_s[16], out_s[16];
    char scr_s[24], stk_s[24], cache_s[24], p_s[24], lim_s[16];
    snprintf(job_s, sizeof job_s, "0x%08X", m->job_ea);
    snprintf(img_s, sizeof img_s, "%d", m->image_id);
    snprintf(bin_s, sizeof bin_s, "0x%X", m->size_bin);
    snprintf(bss_s, sizeof bss_s, "0x%llX", (unsigned long long)m->bss);
    snprintf(mem_s, sizeof mem_s, "0x%llX", (unsigned long long)(m->size_bin + m->bss));
    snprintf(io_s, sizeof io_s, "0x%X", m->size_io);
    snprintf(out_s, sizeof out_s, "0x%X", m->size_out);
    snprintf(scr_s, sizeof scr_s, "0x%llX", (unsigned long long)m->size_scr);
    snprintf(stk_s, sizeof stk_s, "0x%llX", (unsigned long long)m->size_stk);
    snprintf(cache_s, sizeof cache_s, "0x%llX", (unsigned long long)m->cache);
    snprintf(p_s, sizeof p_s, "0x%llX", (unsigned long long)p);
    snprintf(lim_s, sizeof lim_s, "0x%X", JOB_RT_LS);
    ps3_stop_field f[] = {
        {"job", job_s}, {"image", img_s}, {"binary", bin_s},
        {"bss", bss_s}, {"memory", mem_s}, {"io", io_s}, {"out", out_s},
        {"scratch", scr_s}, {"stack", stk_s}, {"cache", cache_s},
        {"region", region}, {"p", p_s}, {"limit", lim_s},
    };
    ps3_stop(PS3_STOP_SPU_JOB_MEMORY,
             "a SPURS job's image, bss, buffers or stack reach the job runtime area "
             "at the top of local store",
             "The descriptor's sizeBinary, the eaBinary high word (bss / 16), the "
             "buffer sizes and sizeStack must leave the job context room above them; "
             "check that the job descriptor is intact.",
             f, sizeof f / sizeof f[0], NULL);
}

int spu_run_spurs_job(spu_lifted_entry_fn entry, int image_id,
                      uint32_t job_ea, uint32_t job_desc_size)
{
    if (!entry || !job_ea) return -1;

    /* ---- descriptor ---------------------------------------------------- */
    /* eaBinary is a be u64: the low word is the guest EA (bit 0 = CACHE_INHIBIT),
     * the high word is, for an ELF job, the bss the crt zeroes itself, in
     * quadwords (jq2e2-027). Truncating it to the EA loses the bss and lays the
     * stack and buffers over it. */
    uint64_t ea_bin64 = g64(job_ea + JH_EA_BINARY);
    uint32_t ea_bin   = (uint32_t)(ea_bin64 & ~1ull);
    uint64_t bss      = (ea_bin64 >> 32) << 4;
    uint32_t size_bin = (uint32_t)g16(job_ea + JH_SIZE_BINARY) << 4;
    uint32_t n_dma    = (uint32_t)g16(job_ea + JH_SIZE_DMA_LIST) / 8u;
    uint32_t use_io   = g32(job_ea + JH_USE_INOUT_BUFFER);
    uint32_t size_io  = g32(job_ea + JH_SIZE_IN_OR_INOUT);
    uint32_t size_out = g32(job_ea + JH_SIZE_OUT);
    uint32_t size_stk = (uint32_t)g16(job_ea + JH_SIZE_STACK) << 4;
    uint32_t size_scr = (uint32_t)g16(job_ea + JH_SIZE_SCRATCH) << 4;
    uint32_t n_cache  = g32(job_ea + JH_SIZE_CACHE_DMA_LIST) / 8u;

    if (n_cache > 4) n_cache = 4;                    /* jm2 caps at 4 */
    if (!size_stk) size_stk = JOB_STACK_DEFAULT;
    if (!size_bin || size_bin > SPU_LS_SIZE) {
        fprintf(stderr, "[spurs-job] job 0x%08X: bad sizeBinary=%u -- not run\n",
                job_ea, size_bin);
        return -1;
    }

    /* The job's memory is its image plus bss, from LS 0, then the io, out,
     * scratch+stack and cache regions above it. All of it must end at or below
     * the job runtime area: past it the descriptor, context and jm interface
     * written below would land on the job's own bytes, and a stack that wraps
     * LS addressing would run over them. */
    job_mem jm = { job_ea, image_id, size_bin, bss, size_io, size_out,
                   (uint64_t)size_scr, (uint64_t)size_stk, 0 };
    if ((uint64_t)size_bin + bss > JOB_RT_LS)
        job_memory_stop(&jm, "image", (uint64_t)size_bin + bss);

    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (!ls) return -1;

    /* ---- binary at LS 0 (PIC, so delta 0 keeps the lifted addresses) ---- */
    memcpy(ls, vm_base + ea_bin, size_bin);

    /* ---- CRT gate ---------------------------------------------------------
     * Only the two CRTs below are modelled (jm interface, kernel view). The PM
     * accepts any "JOBCRT Ver" suffix, so a job from another SDK drop gets
     * here and would run on a context it was not built for. */
    unsigned crt = 0;
    if (!memcmp(ls + JOB_CRT_MARKER_LS, "JOBCRT Ver13", JOB_CRT_MARKER_LEN)) crt = 13;
    else if (!memcmp(ls + JOB_CRT_MARKER_LS, "JOBCRT Ver14", JOB_CRT_MARKER_LEN)) crt = 14;
    if (!crt) {
        char job_s[16], img_s[16], text_s[JOB_CRT_MARKER_LEN + 1], hex_s[2 * JOB_CRT_MARKER_LEN + 1];
        snprintf(job_s, sizeof job_s, "0x%08X", job_ea);
        snprintf(img_s, sizeof img_s, "%d", image_id);
        for (unsigned i = 0; i < JOB_CRT_MARKER_LEN; i++) {
            const uint8_t b = ls[JOB_CRT_MARKER_LS + i];
            text_s[i] = (b >= 0x20 && b < 0x7F) ? (char)b : '?';
            snprintf(hex_s + 2 * i, 3, "%02X", b);
        }
        text_s[JOB_CRT_MARKER_LEN] = 0;
        ps3_stop_field f[] = {
            {"job", job_s}, {"image", img_s}, {"marker", text_s}, {"bytes", hex_s},
        };
        ps3_stop(PS3_STOP_SPU_JOB_CRT_VERSION,
                 "a SPURS job whose CRT marker is not JOBCRT Ver13 or JOBCRT Ver14",
                 "Only those two CRTs' jm interface and kernel view are modelled; "
                 "a job built against another CRT needs its own model before it can run.",
                 f, sizeof f / sizeof f[0], NULL);
    }

    /* ---- regions, following _cellSpursCheckJob's own arithmetic --------- */
    /* Same rule as the scratch pointer: these are REGION BASES and stay valid
     * when the region is empty. NULL invites the job to dereference 0 -- and
     * since the job binary loads at LS 0, reading through a null base returns
     * the job's OWN INSTRUCTION WORDS. That is exactly what Tokyo Jungle's
     * wedged jobs were doing: each spun DMAing to the word at offset 0x64 of
     * its own image, which is why the addresses were misaligned and looked
     * like floats -- they were opcodes. */
    uint64_t p         = ALIGN1024((uint64_t)size_bin + bss);
    uint32_t io_ls     = (uint32_t)p;
    if (size_io && p + size_io > JOB_RT_LS)
        job_memory_stop(&jm, "io", p + size_io);
    p += ALIGN1024((uint64_t)size_io);
    uint32_t out_ls    = (uint32_t)p;
    if (size_out && p + size_out > JOB_RT_LS)
        job_memory_stop(&jm, "out", p + size_out);
    p += ALIGN1024((uint64_t)size_out);
    if (use_io && !out_ls) out_ls = io_ls;    /* in-out: output shares ioBuffer */
    uint32_t ss_size   = (uint32_t)ALIGN1024((uint64_t)size_scr + size_stk);
    /* The scratch pointer is the BASE OF THE SCRATCH+STACK BLOCK, and it is
     * valid even when sizeScratch is 0 -- a zero-length buffer still has an
     * address. Handing the job a NULL here meant it dereferenced 0 and read
     * ITS OWN CODE as data: every wedged job in Tokyo Jungle was loading a
     * word from LS 0x64 and using that SPU instruction word as a DMA address,
     * which is why the addresses looked like misaligned floats. */
    uint32_t s_ls      = (uint32_t)p;
    /* The block's top is where r1 starts: the stack must lie wholly below
     * JOB_RT_LS, or LS masking would put it on the context and descriptor. */
    if (p + ss_size > JOB_RT_LS)
        job_memory_stop(&jm, "stack", p + ss_size);
    uint32_t stack_top = s_ls + ss_size;
    p += ss_size;

    /* ---- cache-hinted read-only inputs ---------------------------------- */
    uint32_t dma_base   = job_ea + JH_SIZE;          /* input list ... */
    uint32_t cache_base = dma_base + n_dma * 8u;     /* ... then cache list */
    uint32_t cache_ls[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < n_cache; i++) {
        uint64_t e   = g64(cache_base + i * 8u);
        uint32_t sz  = (uint32_t)(e >> 32);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        jm.cache += sz;
        if (p + sz > JOB_RT_LS)
            job_memory_stop(&jm, "cache", p + sz);
        cache_ls[i] = (uint32_t)p;
        memcpy(ls + p, vm_base + eal, sz);
        p += ALIGN1024((uint64_t)sz);
    }

    /* ---- input DMA list -> ioBuffer, each entry 16-byte aligned ---------- */
    uint32_t io_cur = io_ls, io_used = 0;
    for (uint32_t i = 0; i < n_dma; i++) {
        uint64_t e   = g64(dma_base + i * 8u);
        uint32_t sz  = (uint32_t)((e >> 32) & 0x7FFFu);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        if (!io_ls || io_cur + sz > SPU_LS_SIZE) break;
        memcpy(ls + io_cur, vm_base + eal, sz);
        io_cur  += (sz + 15u) & ~15u;
        io_used += (sz + 15u) & ~15u;
    }

    /* ---- context + descriptor, parked above the job's memory region ------
     * jm2 keeps these in its own bss (outside job memory); we load the job at
     * LS 0, so the top of LS is the equivalent free space. */
    const uint32_t desc_ls = JOB_DESC_LS;       /* descriptors are <= 1024 B */
    const uint32_t ctx_ls  = JOB_CTX_LS;        /* CellSpursJobContext2, 48 B */
    uint32_t dsz     = job_desc_size ? job_desc_size : 256u;
    if (dsz > 0x400) dsz = 0x400;
    /* Every region, the stack included, ended at or below JOB_RT_LS (checked
     * above), so the descriptor, context and jm interface below land on free
     * local store. */
    memcpy(ls + desc_ls, vm_base + job_ea, dsz);

    ls32(ls, ctx_ls + JC_IO_BUFFER, io_ls);
    for (int i = 0; i < 4; i++)
        ls32(ls, ctx_ls + JC_CACHE_BUFFER + (uint32_t)i * 4u, cache_ls[i]);
    /* sizeJobDescriptor:4 is the first bitfield of a big-endian u32, so it
     * occupies the TOP 4 bits. 0 means 64 bytes, otherwise n*128. */
    /* The low 28 bits are not padding to a "JOBCRT Ver13"/"Ver14" crt (WWS and
     * Edge jobs, Twisted Metal's JobQueue jobs, Odin Sphere's): the job manager
     * parks an LS pointer to its jm interface there, and the crt returns
     * 0x80410A11 and STOPs before main when it is 0 -- so the job never runs
     * and never clears its completion counter. The interface is the 16 bytes
     * after the context: zero for Ver13, which only stashes the pointer; for
     * Ver14 the two entries (job_rt_plant). */
    const uint32_t jm_iface = JOB_JM_IFACE;
    if (crt == 14) job_rt_plant(ls);
    /* The same crt also reads the SPURS kernel context with ABSOLUTE loads
     * (lqa): spuNum at 0x1C8 (cellSpursGetCurrentSpuId), the trace buffer at
     * 0x210, a trace-enable byte at 0x175. On hardware the kernel owns
     * 0x100-0x2FF; here the job is loaded at 0, so those reads returned the
     * job's own instruction words and it indexed per-SPU tables with an opcode
     * and halted. The lifted code never reads those bytes as data (Ver14: only
     * 0x170, 0x1C0 and, with tracing on, 0x210 -- jq2e2-023), so overlay the
     * kernel's view: SPU 0, our DMA tag, tracing off. */
    memset(ls + 0x170, 0, 0x10);
    memset(ls + 0x1C0, 0, 0x10);
    ls32(ls, 0x1C8, g_spurs_job_spu_num);
    ls32(ls, 0x1CC, JOB_DMA_TAG);
    memset(ls + 0x210, 0, 0x10);
    ls32(ls, ctx_ls + JC_SIZE_JOB_DESC,
         ((dsz == 64 ? 0u : (dsz / 128u) & 0xFu) << 28) | jm_iface);
    ls16(ls, ctx_ls + JC_NUM_IO_BUFFER,    (uint16_t)n_dma);
    ls16(ls, ctx_ls + JC_NUM_CACHE_BUFFER, (uint16_t)n_cache);
    ls32(ls, ctx_ls + JC_O_BUFFER, out_ls);
    ls32(ls, ctx_ls + JC_S_BUFFER, s_ls);
    ls32(ls, ctx_ls + JC_DMA_TAG,  JOB_DMA_TAG);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR,     0);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR + 4, job_ea);

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr,
              "[spurs-job] job 0x%08X: bin 0x%08X+%u -> LS 0 | io=0x%05X(%u, %u dma, %u used)"
              " out=0x%05X(%u) scratch=0x%05X(%u) stack_top=0x%05X(%u) cache=%u"
              " ctx=0x%05X desc=0x%05X(%u)\n",
              job_ea, ea_bin, size_bin, io_ls, size_io, n_dma, io_used,
              out_ls, size_out, s_ls, size_scr, stack_top, size_stk, n_cache,
              ctx_ls, desc_ls, dsz); }

    /* SPURS_JOB_DESCDUMP=1: hexdump the guest job descriptor. The SPU derives
     * its DMA and atomic EAs from these words, so when a job spins on a
     * lock-line address that is not backed, this is what to read first. */
    { static int _d = 0;
      if (getenv("SPURS_JOB_DESCDUMP") && PS3_LOG_FIRST(_d, 4)) {
          fprintf(stderr, "[spurs-job] desc @0x%08X (%u bytes):", job_ea, dsz);
          for (uint32_t o = 0; o < dsz && o < 128; o += 4) {
              if ((o & 31) == 0) fprintf(stderr, "\n    +%02X:", o);
              fprintf(stderr, " %08X", g32(job_ea + o));
          }
          fprintf(stderr, "\n");
          /* SPURS_JOB_DESCDUMP=2: follow plausible guest EAs in the job user
           * data (past JH_SIZE) and dump what they point at. The job reads its
           * parameters from there, so a wild DMA/atomic address usually
           * originates in one of these blocks. */
          if (atoi(getenv("SPURS_JOB_DESCDUMP")) >= 2) {
              for (uint32_t o = JH_SIZE; o < dsz && o < 128; o += 4) {
                  uint32_t v = g32(job_ea + o);
                  if (v < 0x10000u || v >= 0xD0000000u) continue;
                  fprintf(stderr, "[spurs-job]   user+0x%02X -> 0x%08X:", o, v);
                  for (uint32_t q = 0; q < 8; q++)
                      fprintf(stderr, " %08X", g32(v + q * 4));
                  fprintf(stderr, "\n");
              }
          }
      } }

    /* LBP job protocol probe (SPURS_JOB_DUMP): the game's one shared job binary
     * ("JOBCRT Ver13" crt + main @0x1570) reads a be u64 EA out of the
     * descriptor's USER DATA at +0x30, GETs a 128-byte command block from it,
     * then jump-tables on the command TYPE (word1 of the block, cases 0..5).
     * The header's input-DMA-list/io fields are legitimately zero for it. Dump
     * the user data + that command block so the bail path is attributable. */
    uint32_t dump_cmd = 0;
    if (getenv("SPURS_JOB_DUMP")) {
        uint64_t ud  = g64(job_ea + JH_SIZE);        /* desc+0x30 */
        uint32_t cmd = (uint32_t)ud;                 /* low word = EA */
        fprintf(stderr, "[spurs-job] job 0x%08X userdata: %016llX %016llX %016llX %016llX\n",
                job_ea, (unsigned long long)ud,
                (unsigned long long)g64(job_ea + JH_SIZE + 8),
                (unsigned long long)g64(job_ea + JH_SIZE + 16),
                (unsigned long long)g64(job_ea + JH_SIZE + 24));
        if (cmd && cmd < 0x50000000u) {
            dump_cmd = cmd;
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X:", cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(cmd + o));
            }
            fprintf(stderr, "\n");
            /* cmdblock+0x1C points at a 2048-byte WORK-ITEM table the type-0/1
             * handler GETs (pc=0x1A78) before entering its per-item worker with
             * count = cmdblock+0x24. Dump its head: all-zero means the game
             * never filled the items (upstream/ordering); real entries mean the
             * SPU worker mishandles them. */
            uint32_t tbl = g32(cmd + 0x1C);
            if (tbl && tbl < 0x50000000u) {
                fprintf(stderr, "[spurs-job]   worktable @0x%08X (n=%u):", tbl, g32(cmd + 0x24));
                for (int o = 0; o < 96; o += 4) {
                    if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                    fprintf(stderr, " %08X", g32(tbl + o));
                }
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }

    /* ---- enter the job -------------------------------------------------- */
    spu_context ctx;
    spu_context_init(&ctx, 0);
    ctx.image_id = image_id;
    ctx.job_rt_ls = crt == 14 ? JOB_RT_LS : 0;
    memcpy(ctx.ls, ls, SPU_LS_SIZE);
    /* SPU stack grows down. A function saves its link register in its
     * caller's frame, at 16($sp), so the entry's first store lands 16 bytes
     * above $sp. The cache buffers start right at stack_top: with $sp at
     * stack_top - 16 that save overwrote the first quadword of
     * cacheBuffer[0] (Twisted Metal's Edge jobs read their viewport there and
     * got width = height = 0). Leave the caller's 32-byte frame inside the
     * stack. */
    ctx.gpr[1]._u32[0] = (stack_top - 32u) & ~15u;
    /* Link register: where the job returns when it is done. The job manager
     * would pass an address inside itself; 0 makes the job re-enter its own
     * entry at LS 0 and run a bogus second lap. */
    ctx.gpr[0]._u32[0] = 0x3FF00u;                   /* SPU_JOB_RETURN_LS */
    ctx.gpr[3]._u32[0] = ctx_ls;                     /* CellSpursJobContext2* */
    ctx.gpr[4]._u32[0] = desc_ls;                    /* CellSpursJob256*      */

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr, "[spurs-job] ENTER r3=%08X r4=%08X r1=%08X entry-fn image=%d\n",
                  ctx.gpr[3]._u32[0], ctx.gpr[4]._u32[0], ctx.gpr[1]._u32[0],
                  image_id); }
    /* NOTE: after its work the job's runtime jumps to LS 0 ("return to the
     * jm2 kernel" -- real layout has the kernel at 0, jobs above; we load the
     * job at 0). That second lap re-enters the job's crt with dead registers
     * and trips its parameter guard, which HALTS -- ending the run. So the
     * HALT-ASSERT heqi 0x1650 seen once per jm2 job is NORMAL COMPLETION
     * noise, not a failure: the job's real work finished before the jump. */
    spu_run_with_halt(entry, &ctx);

    /* The only way into the trap quadword is a call through jm word 0. Both
     * paths end the run there: the interpreter stops at the `stop`, and the
     * resolver halts the lifted run once the interpreted stub stops. */
    if (ctx.job_rt_ls && (ctx.status & SPU_STATUS_STOPPED_BY_STOP) &&
        ctx.stop_code == SPU_JOB_RT_TRAP_STOP &&
        (ctx.pc & SPU_LS_MASK) == ctx.job_rt_ls + JOB_RT_TRAP + 4u) {
        char job_s[16], img_s[16], ent_s[16];
        snprintf(job_s, sizeof job_s, "0x%08X", job_ea);
        snprintf(img_s, sizeof img_s, "%d", image_id);
        snprintf(ent_s, sizeof ent_s, "0x%05X", ctx.job_rt_ls + JOB_RT_TRAP);
        ps3_stop_field f[] = { {"job", job_s}, {"image", img_s}, {"entry", ent_s} };
        ps3_stop_site site = { ctx.job_rt_ls + JOB_RT_TRAP, ctx.gpr[0]._u32[0] & SPU_LS_MASK, 0 };
        ps3_stop(PS3_STOP_SPU_JOB_JM_ENTRY,
                 "a Ver14 SPURS job called the jm interface's scheduler-switch entry",
                 "No Odin Sphere job image does; the entry stands for the job manager's "
                 "scheduler, which a single job run does not model.",
                 f, sizeof f / sizeof f[0], &site);
    }

    /* Capture the job's outbound mailbox. These are LS POINTERS TO STRINGS for
     * the SPU printf service, NOT query results -- value 0x83C0 from the sound
     * job points at " compressor, 1/n..." inside its own image. An earlier
     * comment here claimed they carried the title's bus counts; they do not,
     * and feeding them into the completion event turned 0x40000000 into a 1 GB
     * allocation request. Kept because the printf service needs them. */
    memcpy(s_job_ls, ctx.ls, SPU_LS_SIZE);   /* keep the store readable */
    /* TEMP SPURS_JOB_LSWORD=<img>:<lsa>: after a job of that image, print the
     * LS word at lsa and every other LS offset holding the same value. */
    { static int img = -2; static uint32_t lsa; static int n = 0;
      if (img == -2) { const char* e = getenv("SPURS_JOB_LSWORD"); img = -1;
          if (e) { img = atoi(e); const char* c = strchr(e, ':'); lsa = c ? (uint32_t)strtoul(c + 1, 0, 16) : 0; } }
      if (img == image_id && n++ < 3) {
          const uint8_t* q = ctx.ls + (lsa & 0x3FFFC);
          uint32_t v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
          fprintf(stderr, "[lsword] img=%d job=0x%08X LS[0x%05X]=0x%08X io=0x%05X also at:", image_id, job_ea, lsa, v, io_ls);
          for (uint32_t o = 0; o + 4 <= SPU_LS_SIZE; o += 4) {
              const uint8_t* p = ctx.ls + o;
              if (o != (lsa & 0x3FFFC) && ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) == v)
                  fprintf(stderr, " 0x%05X", o);
          }
          fprintf(stderr, "\n");
      } }
    s_job_ls_valid = 1;

    g_spurs_job_mbox      = spu_channel_has_data(&ctx.ch_out_mbox)
                          ? ctx.ch_out_mbox.value : 0;
    g_spurs_job_mbox_intr = spu_channel_has_data(&ctx.ch_out_intr_mbox)
                          ? ctx.ch_out_intr_mbox.value : 0;
    g_spurs_job_mbox_valid = g_spurs_job_mbox || g_spurs_job_mbox_intr;
    { uint32_t _cb = g32(job_ea + 0x4C);
      g_spurs_job_cmd = _cb ? (g32(_cb) >> 16) : 0; }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPURS_JOB_MBOX") ? 1 : 0;
      static int n = 0;
      if (s_t && PS3_LOG_FIRST(n, 16) && g_spurs_job_mbox_valid)
          fprintf(stderr, "[spurs-job] job 0x%08X posted mbox=0x%08X intr=0x%08X\n",
                  job_ea, g_spurs_job_mbox, g_spurs_job_mbox_intr); }

    if (getenv("SPURS_JOB_DUMP")) {
        fprintf(stderr, "[spurs-job] job 0x%08X exit: status=0x%X stop=0x%X pc=0x%05X\n",
                job_ea, ctx.status, ctx.stop_code, ctx.pc);
        /* Post-run view of the same command block: the game polls result/status
         * fields the job writes back -- a before/after diff shows whether the
         * handler delivered its completion or the PPU waits on stale state. */
        if (dump_cmd) {
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X AFTER:", dump_cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(dump_cmd + o));
            }
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }

    spu_mfc_release(&ctx);
    spu_coh_unregister(&ctx);   /* stack local: out of the reserver set */
    { extern void spu_mfc_release(spu_context*); spu_mfc_release(&ctx); }
    free(ls);
    return 0;
}

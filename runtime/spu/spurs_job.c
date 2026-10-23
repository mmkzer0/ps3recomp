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
#include "spu_dma.h"
#include "spu_workload.h"
#include "spu_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/log_first.h"
#include "ps3emu/stop.h"

/* The job's local store comes from these two. A test builds this file with
 * SPURS_JOB_LS_TEST and supplies the functions, to see what is written to the
 * store and when (tools/test_spurs_job_crt.py). */
#ifdef SPURS_JOB_LS_TEST
extern uint8_t* spurs_job_test_ls_alloc(void);
extern void spurs_job_test_ls_free(uint8_t* ls);
#define SPURS_JOB_LS_ALLOC()  spurs_job_test_ls_alloc()
#define SPURS_JOB_LS_FREE(p)  spurs_job_test_ls_free(p)
#else
#define SPURS_JOB_LS_ALLOC()  ((uint8_t*)calloc(1, SPU_LS_SIZE))
#define SPURS_JOB_LS_FREE(p)  free(p)
#endif

extern uint8_t* vm_base;
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
/* The guest VM's size: 0 means "not sized" (a host that maps the whole 32-bit
 * space and commits on fault), in which case the 32-bit space is the bound. */
extern uint32_t ppu_vm_size;

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

/* The job descriptor as the loader reads it: guest memory at `ea` (the job
 * manager that walks a chain reads it where the title left it), or `snap`, the
 * bytes captured when a job queue admitted the job. A snapshot is the job's
 * own copy: a store to the guest descriptor after admission does not reach it,
 * and a read past its end is 0. */
typedef struct desc_view {
    const uint8_t* snap;     /* NULL: read guest memory */
    uint32_t       n;        /* snapshot bytes */
    uint32_t       ea;
} desc_view;

static int dv_has(const desc_view* d, uint32_t off, uint32_t len)
{
    return (uint64_t)off + len <= d->n;
}
static uint16_t dv16(const desc_view* d, uint32_t off)
{
    if (!d->snap) return g16(d->ea + off);
    return dv_has(d, off, 2) ? (uint16_t)(((uint32_t)d->snap[off] << 8) | d->snap[off + 1]) : 0;
}
static uint32_t dv32(const desc_view* d, uint32_t off)
{
    if (!d->snap) return g32(d->ea + off);
    if (!dv_has(d, off, 4)) return 0;
    const uint8_t* p = d->snap + off;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t dv64(const desc_view* d, uint32_t off)
{
    return ((uint64_t)dv32(d, off) << 32) | (uint64_t)dv32(d, off + 4);
}

/* Is [ea, ea+size) inside the guest's VM? Summed in 64 bits, so an EA near the
 * top of the 32-bit space cannot wrap to a low address. */
static int guest_range_ok(uint64_t ea, uint64_t size)
{
    const uint64_t limit = ppu_vm_size ? (uint64_t)ppu_vm_size : 0x100000000ull;
    return ea <= limit && size <= limit - ea;
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
#define ALIGN128(x)   (((uint64_t)(x) + 127u) & ~(uint64_t)127u)
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

/* The JobQueue2 profile's jm interface (docs/JQ2.md, "JQ2 job ABI"). The PM
 * keeps the table at SPU_JQ2_JM_LS = {0x1500, 0x5870, 0, 0} and the CRT copies
 * those words into the job's bss, so the addresses are part of what the job's
 * image+bss shows; the code behind them is authored here, not the PM's.
 *   0x5870  the coroutine swap. A queue worker runs one job, so there is no
 *           other context to switch to: the same poll loop as s_job_rt_swap
 *           (it waits for every tag group, clobbers only r3, returns through
 *           r0, and leaves the tag mask channel at -1) is the swap's whole
 *           observable effect, as docs/JQ2.md argues from the PM's swap.
 *   0x1500  the scheduler switch: a `stop` with SPU_JQ2_JM_CALL_STOP. The
 *           executor names it (jq2-jm-call); no Odin Sphere image calls it. */
static void job_jq2_plant(uint8_t* ls)
{
    ls32(ls, SPU_JQ2_JM_CALL_LS, SPU_JQ2_JM_CALL_STOP);
    for (uint32_t i = 0; i < sizeof s_job_rt_swap / sizeof s_job_rt_swap[0]; i++)
        ls32(ls, SPU_JQ2_JM_SWAP_LS + i * 4u, s_job_rt_swap[i]);
    ls32(ls, SPU_JQ2_JM_LS,      SPU_JQ2_JM_CALL_LS);
    ls32(ls, SPU_JQ2_JM_LS + 4u, SPU_JQ2_JM_SWAP_LS);
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
    uint64_t limit;            /* the lowest address the job's memory must stay below */
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
    snprintf(lim_s, sizeof lim_s, "0x%llX", (unsigned long long)m->limit);
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

/* What the descriptor's input DMA list asked for, for the spu-job-dma-list
 * stop. Sizes in bytes, offsets in local store. */
typedef struct job_dma {
    uint32_t job_ea;
    int      image_id;
    uint32_t capacity;         /* sizeInOrInOut, the ioBuffer's declared size */
} job_dma;

/* One validated copy from guest memory into local store: where it lands, where
 * it comes from and how many bytes. The loader reads and checks the whole
 * descriptor first and only then stages these, so a descriptor changing in
 * guest memory between the two cannot turn a checked copy into another one. */
typedef struct job_xfer {
    uint32_t ls, ea, size;
} job_xfer;

/* index is the list element, -1 when the failure is not an element (the
 * descriptor names no binary, or the list itself leaves guest memory). ls is
 * where the element would start, cumulative the bytes through it. */
PS3_NORETURN static void job_dma_stop(const job_dma* d, long long index,
                                      uint64_t ea, uint64_t size, uint64_t ls,
                                      uint64_t cumulative, const char* reason)
{
    char job_s[16], img_s[16], idx_s[16], ea_s[24], sz_s[24], ls_s[16];
    char cum_s[24], cap_s[16];
    snprintf(job_s, sizeof job_s, "0x%08X", d->job_ea);
    snprintf(img_s, sizeof img_s, "%d", d->image_id);
    snprintf(idx_s, sizeof idx_s, "%lld", index);
    snprintf(ea_s, sizeof ea_s, "0x%llX", (unsigned long long)ea);
    snprintf(sz_s, sizeof sz_s, "0x%llX", (unsigned long long)size);
    snprintf(ls_s, sizeof ls_s, "0x%05llX", (unsigned long long)ls);
    snprintf(cum_s, sizeof cum_s, "0x%llX", (unsigned long long)cumulative);
    snprintf(cap_s, sizeof cap_s, "0x%X", d->capacity);
    ps3_stop_field f[] = {
        {"job", job_s}, {"image", img_s}, {"index", idx_s}, {"ea", ea_s},
        {"size", sz_s}, {"ls", ls_s}, {"cumulative", cum_s},
        {"capacity", cap_s}, {"reason", reason},
    };
    ps3_stop(PS3_STOP_SPU_JOB_DMA_LIST,
             "a SPURS job descriptor names no binary, a binary or cache entry outside "
             "guest memory, or carries an input DMA "
             "list that cannot be staged into its ioBuffer",
             "eaBinary and sizeBinary must be non-zero and the binary, and every cache-list "
             "entry, must lie inside guest memory. The list at descriptor+0x30 "
             "is sizeDmaList bytes of 8-byte {size word, EAL} elements, issued as an "
             "MFC list GET: each size is 1/2/4/8 or a 16-byte multiple up to 16 KB, "
             "each EAL is naturally aligned and inside guest memory, a transfer below "
             "16 bytes lands at the local-store offset EAL & 15 within its quadword, "
             "and the staged bytes fit sizeInOrInOut. The stall-and-notify bit "
             "(bit 31 of the size word) is not modelled: it needs a job manager to "
             "acknowledge the stall, which a job start does not have. Check that the "
             "job descriptor is intact.",
             f, sizeof f / sizeof f[0], NULL);
}

/* The cache-hinted read-only inputs: placed and checked, staged later. `p` is
 * the running heap pointer on entry and the pointer past the last buffer on
 * return. Jobchain (q == 0): an entry with no size or no EAL is skipped (no
 * buffer, pointer 0), buffers are 1024-aligned and must end at or below
 * `limit`. JobQueue2 (q): the PM allocates A128(size) for every entry and
 * points cacheBuffer[i] at the running pointer, so an entry with no size has
 * the pointer and no room, and one with no EAL has its room and nothing copied
 * into it. */
static uint64_t job_place_cache(const desc_view* dv, const job_dma* jd, job_mem* jm, int q,
                                uint32_t n_dma, uint32_t n_cache, uint64_t p,
                                uint32_t cache_ls[4], job_xfer cache_x[4])
{
    for (uint32_t i = 0; i < n_cache; i++) {
        const uint64_t e   = dv64(dv, (uint32_t)(JH_SIZE + (uint64_t)n_dma * 8u + (uint64_t)i * 8u));
        const uint32_t sz  = (uint32_t)(e >> 32);
        const uint32_t eal = (uint32_t)e;
        if (!q && (!sz || !eal)) continue;
        if (q && !sz) { cache_ls[i] = (uint32_t)p; continue; }
        if (eal && !guest_range_ok(eal, sz))
            job_dma_stop(jd, (long long)i, eal, sz, 0, jm->cache + sz, "cache-range");
        jm->cache += sz;
        const uint64_t room = q ? ALIGN128(sz) : sz;
        if (p + room > jm->limit)
            job_memory_stop(jm, "cache", p + room);
        cache_ls[i] = (uint32_t)p;
        if (eal) { cache_x[i].ls = (uint32_t)p; cache_x[i].ea = eal; cache_x[i].size = sz; }
        p += q ? ALIGN128(sz) : ALIGN1024(sz);
    }
    return p;
}

/* The loader. `snap` NULL reads the descriptor from guest memory at job_ea;
 * otherwise from the snap_size bytes at `snap`, with job_ea still the EA the job
 * sees as its eaJobDescriptor. `retain_ls` keeps the finished local store for
 * spurs_job_ls_for_handle (a chain job); a queue's job has no chain handle and
 * keeps no shared state, so two of them can run on two threads at once.
 * `jq2` NULL stages the jobchain layout; non-NULL stages the JobQueue2 policy
 * module's (spu_run_jq2_job). The descriptor, DMA-list and cache validation is
 * the same for both; only the placement, the context and the entry state
 * differ, and each place that does is marked `q`. */
static int run_spurs_job(spu_lifted_entry_fn entry, int image_id, uint32_t job_ea,
                         const uint8_t* snap, uint32_t snap_size,
                         uint32_t job_desc_size, int retain_ls, const spu_job_hooks* hooks,
                         const spu_jq2_env* jq2)
{
    if (!entry || !job_ea) return -1;
    const desc_view dv = { snap, snap_size, job_ea };
    /* Without a snapshot (the chain walker) the header is read, and later
     * copied into local store, straight from guest memory: the whole span the
     * copy takes must be mapped before the first header read. */
    if (!snap) {
        uint32_t span = job_desc_size ? job_desc_size : 256u;
        if (span > 0x400) span = 0x400;
        if (span < JH_SIZE) span = JH_SIZE;
        if (!guest_range_ok(job_ea, span)) {
            const job_dma jd0 = { job_ea, image_id, 0 };
            job_dma_stop(&jd0, -1, job_ea, span, 0, 0, "descriptor-range");
        }
    }

    /* ---- descriptor ---------------------------------------------------- */
    /* eaBinary is a be u64: the low word is the guest EA (bit 0 = CACHE_INHIBIT),
     * the high word is, for an ELF job, the bss the crt zeroes itself, in
     * quadwords (jq2e2-027). Truncating it to the EA loses the bss and lays the
     * stack and buffers over it. */
    uint64_t ea_bin64 = dv64(&dv, JH_EA_BINARY);
    uint32_t ea_bin   = (uint32_t)(ea_bin64 & ~1ull);
    const int q       = jq2 != NULL;
    uint64_t bss      = (ea_bin64 >> 32) << 4;
    uint32_t size_bin = (uint32_t)dv16(&dv, JH_SIZE_BINARY) << 4;
    uint32_t n_dma    = (uint32_t)dv16(&dv, JH_SIZE_DMA_LIST) / 8u;
    uint32_t use_io   = dv32(&dv, JH_USE_INOUT_BUFFER);
    uint32_t size_io  = dv32(&dv, JH_SIZE_IN_OR_INOUT);
    uint32_t size_out = dv32(&dv, JH_SIZE_OUT);
    uint32_t size_stk = (uint32_t)dv16(&dv, JH_SIZE_STACK) << 4;
    uint32_t size_scr = (uint32_t)dv16(&dv, JH_SIZE_SCRATCH) << 4;
    uint32_t n_cache  = dv32(&dv, JH_SIZE_CACHE_DMA_LIST) / 8u;

    const uint32_t n_cache_raw = n_cache;
    if (n_cache > 4) n_cache = 4;                    /* jm2 caps at 4 */
    if (!size_stk) size_stk = JOB_STACK_DEFAULT;

    /* q: the image's base, the end of the heap the PM allocates from, and the
     * image's size. The PM sizes image+bss as A128((sizeBinary + u16@+2) << 4)
     * -- only the LOW half of the eaBinary high word counts (that is `bss`),
     * and the PM copies only sizeBinary << 4 bytes of it, so the rest is bss it
     * never clears (the job's crt0 does). The 128-byte rounding is the PM's
     * allocation, not bss: the job's own footprint, image + bss, is what the
     * image header's word at +0x3C says. The heap runs up from the image base
     * to the descriptor arena D0 = 0x3F400 - 4 * maxDesc (maxDesc 0 means
     * 256). */
    const uint32_t base  = q ? SPU_JQ2_IMAGE_LS : 0u;
    uint64_t limit = JOB_RT_LS;
    uint32_t d0 = 0, max_desc = 0;
    if (q) {
        max_desc = jq2->max_desc ? jq2->max_desc : 256u;
        d0 = SPU_JQ2_DESC_TOP - 4u * max_desc;
        limit = d0;
        bss = (uint64_t)dv16(&dv, 2) << 4;
    }

    job_dma jd = { job_ea, image_id, size_io };
    /* P3: a job that names no image is a malformed descriptor, never a
     * success. Masking the cache-inhibit bit can leave eaBinary zero even
     * though the guest word was 1. */
    if (!ea_bin)
        job_dma_stop(&jd, -1, ea_bin, size_bin, 0, 0, "eaBinary");
    if (!size_bin)
        job_dma_stop(&jd, -1, ea_bin, 0, 0, 0, "sizeBinary");
    /* q: what the PM's layout evidence does not decode is a stop, never a
     * guess (docs/JQ2.md, "Unresolved"): the jobType byte's bit 1 shifts the
     * scratch block and the stack by amounts nobody has decoded (U1), the
     * eaBinary low word's bit 1 makes the PM hold in a debugger stop (U6), and
     * the cache count was observed up to 2 only (U5; the PM's loop has no cap
     * and the context has four slots). */
    if (q) {
        const unsigned job_type = (unsigned)(dv16(&dv, JH_JOB_TYPE) >> 8);
        if (ea_bin64 & 2u)
            job_dma_stop(&jd, -1, (uint32_t)ea_bin64, size_bin, 0, 0, "binary-bit1");
        if (job_type & 2u)
            job_dma_stop(&jd, -1, ea_bin, job_type, 0, 0, "jobtype-bit1");
        if (n_cache_raw > 2)
            job_dma_stop(&jd, -1, ea_bin, (uint64_t)n_cache_raw * 8u, 0, 0, "cache-count");
    }
    if (size_bin > SPU_LS_SIZE) {
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
                   (uint64_t)size_scr, (uint64_t)size_stk, 0, limit };
    const uint64_t img_alloc = q ? ALIGN128(size_bin + bss) : (uint64_t)size_bin + bss;
    if ((uint64_t)base + img_alloc > limit)
        job_memory_stop(&jm, "image", (uint64_t)base + img_alloc);

    /* The binary is read from guest memory by the CRT gate just below and again
     * when it is staged: its whole extent must lie inside mapped guest memory
     * before either read forms a host pointer. */
    if (!guest_range_ok(ea_bin, size_bin))
        job_dma_stop(&jd, -1, ea_bin, size_bin, 0, 0, "binary-range");

    /* ---- CRT gate ---------------------------------------------------------
     * The binary lands at LS 0 further down, once the whole descriptor has been
     * validated; the marker is read from the bytes that will be there (zero
     * past the image, as the store is). */
    uint8_t marker[JOB_CRT_MARKER_LEN];
    for (uint32_t i = 0; i < JOB_CRT_MARKER_LEN; i++) {
        const uint32_t o = JOB_CRT_MARKER_LS + i;
        marker[i] = o < size_bin ? vm_base[(uint64_t)ea_bin + o] : 0;
    }

    /* Only the two CRTs below are modelled (jm interface, kernel view). The PM
     * accepts any "JOBCRT Ver" suffix, so a job from another SDK drop gets
     * here and would run on a context it was not built for. */
    unsigned crt = 0;
    if (!memcmp(marker, "JOBCRT Ver13", JOB_CRT_MARKER_LEN)) crt = 13;
    else if (!memcmp(marker, "JOBCRT Ver14", JOB_CRT_MARKER_LEN)) crt = 14;
    if (!crt) {
        char job_s[16], img_s[16], text_s[JOB_CRT_MARKER_LEN + 1], hex_s[2 * JOB_CRT_MARKER_LEN + 1];
        snprintf(job_s, sizeof job_s, "0x%08X", job_ea);
        snprintf(img_s, sizeof img_s, "%d", image_id);
        for (unsigned i = 0; i < JOB_CRT_MARKER_LEN; i++) {
            const uint8_t b = marker[i];
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

    /* ---- regions ----------------------------------------------------------
     * Jobchain: _cellSpursCheckJob's own arithmetic, 1024-aligned, image at LS
     * 0, cache buffers last. JobQueue2 (q): the PM's -- image at 0x7C00, 128-
     * aligned, cache buffers first. */
    /* Both lists are descriptor bytes. Reading them must stay inside guest
     * memory: the extent is summed in 64 bits, so a sizeDmaList at the u16 cap
     * next to a high descriptor EA cannot wrap into a low address. */
    const uint64_t dma_base   = (uint64_t)job_ea + JH_SIZE;      /* input list ... */
    /* ... then the cache list, at JH_SIZE + n_dma * 8 in the descriptor. */
    const int lists_bad = snap ? (uint64_t)JH_SIZE + ((uint64_t)n_dma + n_cache) * 8u > snap_size
                               : !guest_range_ok(dma_base, ((uint64_t)n_dma + n_cache) * 8u);
    uint32_t cache_ls[4] = { 0, 0, 0, 0 };
    job_xfer cache_x[4]  = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
    uint64_t p;
    uint32_t io_ls, out_ls, s_ls, stack_top;
    if (q) {
        if (lists_bad)
            job_dma_stop(&jd, -1, dma_base, ((uint64_t)n_dma + n_cache) * 8u,
                         0, 0, "descriptor-range");
        p = (uint64_t)base + ALIGN128(size_bin + bss);
        p = job_place_cache(&dv, &jd, &jm, q, n_dma, n_cache, p, cache_ls, cache_x);
        io_ls = (uint32_t)p;
        if (p + ALIGN128(size_io) > limit)
            job_memory_stop(&jm, "io", p + ALIGN128(size_io));
        p += ALIGN128(size_io);
        out_ls = (uint32_t)p;                            /* independent of the useInOut word */
        if (p + ALIGN128(size_out) > limit)
            job_memory_stop(&jm, "out", p + ALIGN128(size_out));
        p += ALIGN128(size_out);
        /* sBuffer is the base of the scratch + stack block; the stack starts
         * after A128(scratch) and its top T is where r1 comes from. */
        s_ls = (uint32_t)p;
        const uint64_t stack_base = p + ALIGN128(size_scr);
        if (stack_base + ALIGN128(size_stk) > limit)
            job_memory_stop(&jm, "stack", stack_base + ALIGN128(size_stk));
        stack_top = (uint32_t)(stack_base + size_stk);
        p = stack_base + ALIGN128(size_stk);
    } else {
    /* Same rule as the scratch pointer: these are REGION BASES and stay valid
     * when the region is empty. NULL invites the job to dereference 0 -- and
     * since the job binary loads at LS 0, reading through a null base returns
     * the job's OWN INSTRUCTION WORDS. That is exactly what Tokyo Jungle's
     * wedged jobs were doing: each spun DMAing to the word at offset 0x64 of
     * its own image, which is why the addresses were misaligned and looked
     * like floats -- they were opcodes. */
    p                  = ALIGN1024((uint64_t)size_bin + bss);
    io_ls              = (uint32_t)p;
    if (size_io && p + size_io > JOB_RT_LS)
        job_memory_stop(&jm, "io", p + size_io);
    p += ALIGN1024((uint64_t)size_io);
    out_ls             = (uint32_t)p;
    if (size_out && p + size_out > JOB_RT_LS)
        job_memory_stop(&jm, "out", p + size_out);
    p += ALIGN1024((uint64_t)size_out);
    if (use_io && !out_ls) out_ls = io_ls;    /* in-out: output shares ioBuffer */
    const uint32_t ss_size = (uint32_t)ALIGN1024((uint64_t)size_scr + size_stk);
    /* The scratch pointer is the BASE OF THE SCRATCH+STACK BLOCK, and it is
     * valid even when sizeScratch is 0 -- a zero-length buffer still has an
     * address. Handing the job a NULL here meant it dereferenced 0 and read
     * ITS OWN CODE as data: every wedged job in Tokyo Jungle was loading a
     * word from LS 0x64 and using that SPU instruction word as a DMA address,
     * which is why the addresses looked like misaligned floats. */
    s_ls               = (uint32_t)p;
    /* The block's top is where r1 starts: the stack must lie wholly below
     * JOB_RT_LS, or LS masking would put it on the context and descriptor. */
    if (p + ss_size > JOB_RT_LS)
        job_memory_stop(&jm, "stack", p + ss_size);
    stack_top = s_ls + ss_size;
    p += ss_size;

    /* ---- cache-hinted read-only inputs: placed and checked here, staged below */
    if (lists_bad)
        job_dma_stop(&jd, -1, dma_base, ((uint64_t)n_dma + n_cache) * 8u,
                     0, 0, "descriptor-range");
    p = job_place_cache(&dv, &jd, &jm, q, n_dma, n_cache, p, cache_ls, cache_x);
    }

    /* ---- input DMA list -> ioBuffer, each entry 16-byte aligned ----------
     * The PM issues this list as an MFC list GET (jq2e2-018), so an element
     * lands where spu_dma.h's mfc_run_list puts it: at the running quadword
     * base, its LSA low nibble taken from EAL (a 1/2/4/8-byte transfer sits at
     * its own offset in the quadword), the next element at the next quadword
     * boundary after its size. Every element is validated before the first
     * byte lands in local store, binary and cache included. A violation must
     * stop, not skip the element (the job would run on a buffer the console
     * never assembled), and must not leave the elements before it staged (a
     * partial ioBuffer is just as wrong). The rules are the MFC's, as
     * spu_dma.h applies them per transfer: a 15-bit size, at most 16 KB, a
     * 16-byte multiple and a 16-byte aligned pair from 16 bytes up, natural
     * 1/2/4/8 alignment below.
     *
     * Bit 31 of the size word is stall-and-notify: the MFC transfers the
     * element, parks the rest of the list and raises SPU event 2, and the
     * list resumes when the notified party writes MFC_WrListStallAck. At a job
     * start there is no such party: the job has not entered, so the notify
     * would reach no one and the tail of the list would never be staged (or,
     * delivered to the job that did not issue the list, would corrupt its
     * event state). The PM's own handler for it is not in the evidence
     * (jq2e2-018 records the issue only). So the flag is a named stop, whether
     * or not the element also carries a transfer. The engine cannot stage the
     * list instead: it treats a malformed element as a silent no-op and runs
     * against a live context's local store, which would mean writing before
     * the whole list was checked. */
    uint64_t io_cursor = 0;                          /* bytes from io_ls */
    job_xfer* io_x = n_dma ? (job_xfer*)malloc((size_t)n_dma * sizeof *io_x) : NULL;
    if (n_dma && !io_x) return -1;
    for (uint32_t i = 0; i < n_dma; i++) {
        const uint64_t e     = dv64(&dv, (uint32_t)(JH_SIZE + (uint64_t)i * 8u));
        const uint64_t size  = (e >> 32) & 0x7FFFu;
        const int      stall = (int)((e >> 63) & 1u);
        const uint32_t eal   = (uint32_t)e;
        const uint32_t lane  = eal & 15u;            /* the MFC's LSA low nibble */
        const uint64_t el_ls = (uint64_t)io_ls + io_cursor + lane;
        const uint64_t end   = io_cursor + lane + size;   /* 64-bit: no wrap */
        const char* why = NULL;
        if (stall)
            why = "stall-notify";
        else if (!size)
            why = "size-zero";
        else if (size > MFC_MAX_DMA_SIZE)
            why = "size-limit";
        else if (size >= 16u) {
            if (size & 15u) why = "size-align";
            else if (lane) why = "ea-align";
        } else {
            if (size != 1 && size != 2 && size != 4 && size != 8) why = "size-align";
            else if (eal & (uint32_t)(size - 1u)) why = "ea-align";
        }
        if (!why && !eal)
            why = "ea-null";
        if (!why && !guest_range_ok(eal, size))
            why = "ea-range";
        if (!why && end > size_io)
            why = "io-overrun";
        if (why)
            job_dma_stop(&jd, (long long)i, eal, size, el_ls, end, why);
        io_x[i].ls   = (uint32_t)el_ls;
        io_x[i].ea   = eal;
        io_x[i].size = (uint32_t)size;
        io_cursor += (size + 15u) & ~15ull;
    }
    const uint32_t io_used = (uint32_t)io_cursor;

    /* ---- the descriptor is valid: stage binary, cache, input list -------- */
    uint8_t* ls = SPURS_JOB_LS_ALLOC();
    if (!ls) { free(io_x); return -1; }

    /* binary at its base: LS 0 for a jobchain job (PIC, so delta 0 keeps the
     * lifted addresses), 0x7C00 for a JobQueue2 job. Only sizeBinary << 4 bytes
     * are copied; the rest of image+bss stays as the store was (zero). */
    memcpy(ls + base, vm_base + ea_bin, size_bin);
    for (uint32_t i = 0; i < n_cache; i++)
        if (cache_x[i].size) memcpy(ls + cache_x[i].ls, vm_base + cache_x[i].ea, cache_x[i].size);
    for (uint32_t i = 0; i < n_dma; i++)
        memcpy(ls + io_x[i].ls, vm_base + io_x[i].ea, io_x[i].size);
    free(io_x);

    /* ---- context + descriptor, parked above the job's memory region ------
     * jm2 keeps these in its own bss (outside job memory); we load the job at
     * LS 0, so the top of LS is the equivalent free space.
     * q: the PM's places. The worker runs one job at a time, so this is the
     * PM's Execute slot k = 0 (the PM toggles k between 0 and 1 per job while
     * it stays resident; a worker has no residency to toggle): the descriptor
     * copy at D0 + k * maxDesc = D0, the JobContext2 at 0x68D0 + 0x30 * k. */
    const uint32_t desc_ls = q ? d0 : JOB_DESC_LS;               /* descriptors are <= 1 KB */
    const uint32_t ctx_ls  = q ? SPU_JQ2_CTX_LS : JOB_CTX_LS;    /* CellSpursJobContext2, 48 B */
    uint32_t dsz     = job_desc_size ? job_desc_size : 256u;
    if (dsz > 0x400) dsz = 0x400;
    if (q && dsz > max_desc) dsz = max_desc;
    /* Every region, the stack included, ended at or below JOB_RT_LS (checked
     * above), so the descriptor, context and jm interface below land on free
     * local store. */
    if (snap)
        memcpy(ls + desc_ls, snap, dsz < snap_size ? dsz : snap_size);
    else
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
    const uint32_t jm_iface = q ? SPU_JQ2_JM_LS : JOB_JM_IFACE;
    if (q) job_jq2_plant(ls);
    else if (crt == 14) job_rt_plant(ls);
    /* The same crt also reads the SPURS kernel context with ABSOLUTE loads
     * (lqa): spuNum at 0x1C8 (cellSpursGetCurrentSpuId), the trace buffer at
     * 0x210, a trace-enable byte at 0x175. On hardware the kernel owns
     * 0x100-0x2FF; here the job is loaded at 0, so those reads returned the
     * job's own instruction words and it indexed per-SPU tables with an opcode
     * and halted. The lifted code never reads those bytes as data (Ver14: only
     * 0x170, 0x1C0 and, with tracing on, 0x210 -- jq2e2-023), so overlay the
     * kernel's view: SPU 0, our DMA tag, tracing off. */
    if (q) {
        /* The PM's kernel view, exactly these words and nothing else in
         * 0x0..0x2FF (jq2 static ABI doc 2): the CellSpurs EA as a u64 at
         * 0x1C0, spuNum 0x1C8, dmaTagId 8 at 0x1CC, the PM's base 0xA00 as the
         * u64 wklCurrentAddr at 0x1D0, wid at 0x1D8 and 0x1DC. The image sits
         * at 0x7C00, so none of it can land on job code. */
        ls32(ls, 0x1C0, 0);
        ls32(ls, 0x1C4, jq2->spurs_ea);
        ls32(ls, 0x1C8, jq2->spu_num);
        ls32(ls, 0x1CC, 8);
        ls32(ls, 0x1D4, 0xA00);
        ls32(ls, 0x1D8, jq2->wid);
        ls32(ls, 0x1DC, jq2->wid);
    } else {
    memset(ls + 0x170, 0, 0x10);
    memset(ls + 0x1C0, 0, 0x10);
    ls32(ls, 0x1C8, g_spurs_job_spu_num);
    ls32(ls, 0x1CC, JOB_DMA_TAG);
    memset(ls + 0x210, 0, 0x10);
    }
    ls32(ls, ctx_ls + JC_SIZE_JOB_DESC,
         ((dsz == 64 ? 0u : (dsz / 128u) & 0xFu) << 28) | jm_iface);
    ls16(ls, ctx_ls + JC_NUM_IO_BUFFER,    (uint16_t)n_dma);
    ls16(ls, ctx_ls + JC_NUM_CACHE_BUFFER, (uint16_t)n_cache);
    ls32(ls, ctx_ls + JC_O_BUFFER, out_ls);
    ls32(ls, ctx_ls + JC_S_BUFFER, s_ls);
    ls32(ls, ctx_ls + JC_DMA_TAG,  q ? 0u : JOB_DMA_TAG);       /* the PM never writes it */
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR,     0);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR + 4, job_ea);

    { static SPURS_JOB_TLS int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr,
              "[spurs-job] job 0x%08X: bin 0x%08X+%u -> LS 0x%X | io=0x%05X(%u, %u dma, %u used)"
              " out=0x%05X(%u) scratch=0x%05X(%u) stack_top=0x%05X(%u) cache=%u"
              " ctx=0x%05X desc=0x%05X(%u)\n",
              job_ea, ea_bin, size_bin, base, io_ls, size_io, n_dma, io_used,
              out_ls, size_out, s_ls, size_scr, stack_top, size_stk, n_cache,
              ctx_ls, desc_ls, dsz); }

    /* SPURS_JOB_DESCDUMP=1: hexdump the guest job descriptor. The SPU derives
     * its DMA and atomic EAs from these words, so when a job spins on a
     * lock-line address that is not backed, this is what to read first. */
    { static SPURS_JOB_TLS int _d = 0;
      if (getenv("SPURS_JOB_DESCDUMP") && PS3_LOG_FIRST(_d, 4)) {
          fprintf(stderr, "[spurs-job] desc @0x%08X (%u bytes):", job_ea, dsz);
          for (uint32_t o = 0; o < dsz && o < 128; o += 4) {
              if ((o & 31) == 0) fprintf(stderr, "\n    +%02X:", o);
              fprintf(stderr, " %08X", dv32(&dv, o));
          }
          fprintf(stderr, "\n");
          /* SPURS_JOB_DESCDUMP=2: follow plausible guest EAs in the job user
           * data (past JH_SIZE) and dump what they point at. The job reads its
           * parameters from there, so a wild DMA/atomic address usually
           * originates in one of these blocks. */
          if (atoi(getenv("SPURS_JOB_DESCDUMP")) >= 2) {
              for (uint32_t o = JH_SIZE; o < dsz && o < 128; o += 4) {
                  uint32_t v = dv32(&dv, o);
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
        uint64_t ud  = dv64(&dv, JH_SIZE);           /* desc+0x30 */
        uint32_t cmd = (uint32_t)ud;                 /* low word = EA */
        fprintf(stderr, "[spurs-job] job 0x%08X userdata: %016llX %016llX %016llX %016llX\n",
                job_ea, (unsigned long long)ud,
                (unsigned long long)dv64(&dv, JH_SIZE + 8),
                (unsigned long long)dv64(&dv, JH_SIZE + 16),
                (unsigned long long)dv64(&dv, JH_SIZE + 24));
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
    ctx.job_rt_ls = q ? SPU_JQ2_JM_CALL_LS : (crt == 14 ? JOB_RT_LS : 0);
    ctx.job_rt_ls2 = q ? SPU_JQ2_JM_SWAP_LS : 0;
    memcpy(ctx.ls, ls, SPU_LS_SIZE);
    if (q) {
        /* The PM's entry state (jq2 static ABI doc 1.4): everything zero, then
         * r0 = the link the executor's `stop 0` sentinel answers (the PM's own
         * is inside itself and is not an input), r1 = stack top - 0x60, r3 the
         * context, r4 the descriptor copy, r80/r81 = the PM's pointers to its
         * saved slots (r1 + 0x20 / + 0x30), r126 = the entry. The PM also stores
         * its link and r126 at those slots; the job never reads them, and our
         * link is not the PM's, so they are not written. */
        ctx.gpr[0]._u32[0]   = 0x3FF00u;               /* SPU_JOB_RETURN_LS */
        ctx.gpr[1]._u32[0]   = stack_top - 0x60u;
        /* r1 is a vector in the SPU ABI: word 0 the stack pointer, words 1..3
         * the stack bytes left below it (the job's stack-limit code subtracts
         * word 1 from the pointer -- the Setting job's helper at image+0x5F40
         * stores the result, and the stored value differs without it). The PM
         * enters with {sp, 0x1FA0, 0x1FA0, 0x1FA0} for an 8 KB stack: the
         * stack's size less the 0x60 reserved above sp. The static spec lists
         * only word 0; the dynamic run (m1r --dbg-pc 0x5834) shows the rest. */
        ctx.gpr[1]._u32[1] = ctx.gpr[1]._u32[2] = ctx.gpr[1]._u32[3] = size_stk - 0x60u;
        ctx.gpr[3]._u32[0]   = ctx_ls;
        ctx.gpr[4]._u32[0]   = desc_ls;
        ctx.gpr[80]._u32[0]  = ctx.gpr[1]._u32[0] + 0x20u;
        ctx.gpr[81]._u32[0]  = ctx.gpr[1]._u32[0] + 0x30u;
        ctx.gpr[126]._u32[0] = base;
        ctx.pc               = base;
    } else {
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
    }

    { static SPURS_JOB_TLS int _n = 0;
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
    if (hooks && hooks->start) {
        /* What the loader laid out, handed over rather than recomputed. r1 is
         * the stack the job enters with. */
        const spu_job_layout layout = {
            ea_bin, base, size_bin, (uint32_t)bss, desc_ls, dsz, io_ls, size_io, out_ls, size_out,
            s_ls, size_scr, size_stk, ctx.gpr[1]._u32[0], ctx_ls };
        hooks->start(hooks->user, &ctx, &layout);
    }
    /* The runner's result (halted or returned) is not a completion test and is
     * not consulted here. A lifted job's normal return is itself reported as a
     * halt (the resolver halts the lifted run when the interpreted stub at the
     * link stops), a chain job's second lap halts in the CRT as normal
     * completion (see the NOTE above), and a lifted `heq`/`hgt` assertion halts
     * too. What the job ended on is in the finished context; whether that is a
     * success is the caller's policy, and the queue executor's is the strict
     * one: it classifies the context in hooks->end (libs/spurs/jq2/executor.cpp,
     * require_normal_return) and stops with jq2-job-end otherwise. */
    spu_run_with_halt(entry, &ctx);

    /* The only way into the trap quadword is a call through jm word 0. Both
     * paths end the run there: the interpreter stops at the `stop`, and the
     * resolver halts the lifted run once the interpreted stub stops. This
     * loader's own stop comes before the caller's end hook, so a body that
     * called word 0 is named for it, not as a generic job end. */
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

    if (hooks && hooks->end) hooks->end(hooks->user, &ctx);

    /* Capture the job's outbound mailbox. These are LS POINTERS TO STRINGS for
     * the SPU printf service, NOT query results -- value 0x83C0 from the sound
     * job points at " compressor, 1/n..." inside its own image. An earlier
     * comment here claimed they carried the title's bus counts; they do not,
     * and feeding them into the completion event turned 0x40000000 into a 1 GB
     * allocation request. Kept because the printf service needs them. */
    if (retain_ls) {
        memcpy(s_job_ls, ctx.ls, SPU_LS_SIZE);   /* keep the store readable */
        s_job_ls_valid = 1;
    }
    /* TEMP SPURS_JOB_LSWORD=<img>:<lsa>: after a job of that image, print the
     * LS word at lsa and every other LS offset holding the same value. */
    { const char* e = getenv("SPURS_JOB_LSWORD");
      static SPURS_JOB_TLS int n = 0;
      const char* c = e ? strchr(e, ':') : 0;
      const uint32_t lsa = c ? (uint32_t)strtoul(c + 1, 0, 16) : 0;
      if (e && atoi(e) == image_id && PS3_LOG_FIRST(n, 3)) {
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

    g_spurs_job_mbox      = spu_channel_has_data(&ctx.ch_out_mbox)
                          ? ctx.ch_out_mbox.value : 0;
    g_spurs_job_mbox_intr = spu_channel_has_data(&ctx.ch_out_intr_mbox)
                          ? ctx.ch_out_intr_mbox.value : 0;
    g_spurs_job_mbox_valid = g_spurs_job_mbox || g_spurs_job_mbox_intr;
    /* The word at descriptor+0x4C is a job's own user data: an EA only for the
     * chain jobs that keep a command block there. Read it only when it names
     * mapped guest memory (a JobQueue2 descriptor's user words are anything). */
    { uint32_t _cb = dv32(&dv, 0x4C);
      g_spurs_job_cmd = (_cb && guest_range_ok(_cb, 4)) ? (g32(_cb) >> 16) : 0; }
    { const int s_t = getenv("SPURS_JOB_MBOX") ? 1 : 0;
      static SPURS_JOB_TLS int n = 0;
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
    SPURS_JOB_LS_FREE(ls);
    return 0;
}

int spu_run_spurs_job(spu_lifted_entry_fn entry, int image_id,
                      uint32_t job_ea, uint32_t job_desc_size)
{
    return run_spurs_job(entry, image_id, job_ea, NULL, 0, job_desc_size, 1, NULL, NULL);
}

int spu_run_spurs_job_snapshot(spu_lifted_entry_fn entry, int image_id, uint32_t job_ea,
                               const uint8_t* desc, uint32_t desc_size)
{
    if (!desc || !desc_size) return -1;
    return run_spurs_job(entry, image_id, job_ea, desc, desc_size, desc_size, 0, NULL, NULL);
}

int spu_run_spurs_job_hooked(spu_lifted_entry_fn entry, int image_id, uint32_t job_ea,
                             const uint8_t* desc, uint32_t desc_size, const spu_job_hooks* hooks)
{
    if (!desc || !desc_size) return -1;
    return run_spurs_job(entry, image_id, job_ea, desc, desc_size, desc_size, 0, hooks, NULL);
}

int spu_run_jq2_job(spu_lifted_entry_fn entry, int image_id, uint32_t job_ea,
                    const uint8_t* desc, uint32_t desc_size, const spu_jq2_env* env,
                    const spu_job_hooks* hooks)
{
    if (!desc || !desc_size || !env) return -1;
    /* maxDesc is the queue attribute's maxSizeJobDescriptor: 0 (256) or
     * 256..0x3FF in 128-byte steps, as queue creation checks it. D0 =
     * 0x3F400 - 4 * maxDesc is formed in 32 bits, so anything else would
     * wrap the heap limit past local store. */
    if (env->max_desc && (env->max_desc < 256u || env->max_desc > 0x3FFu || (env->max_desc & 127u)))
        return -1;
    return run_spurs_job(entry, image_id, job_ea, desc, desc_size, desc_size, 0, hooks, env);
}

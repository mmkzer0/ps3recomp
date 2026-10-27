/* An SPU MFC PUT/PUTL, or an atomic line store (PUTLLC/PUTLLUC/PUTQLLUC), into
 * the guest store window a device model registered (the RSX label window) is
 * counted and stops by name: runtime/spu/spu_dma.h and
 * runtime/spu/spu_channels.c, ps3_stop_spu_label_window. The runtime cannot
 * tell whether the store produces the value an RSX ACQUIRE waits for (it has to
 * land before the walker reads it) or overwrites a label the walker has not
 * read yet (it has to land after a pass), so it fails closed instead of waiting
 * for a pass and deadlocking the producer case. docs/STOPS.md,
 * "spu-label-window".
 *
 * Modes:
 *   overlap         a plain PUT whose span is inside the window -> stop 70
 *   overlap-edge    a plain PUT that overlaps the window by one byte -> stop 70
 *   overlap-list    a PUTL whose single element is inside the window -> stop 70
 *   outside         a PUT just below and a PUT just past the window -> exit 0,
 *                   both landed, ps3_label_window_spu_puts() == 0
 *   overlap-control the same PUT with no window registered: it lands and exits 0
 *                   (what the runtime did before the check; the fail-before)
 *   outside-covered the same "outside" PUT with a window covering it -> stop 70
 *                   (a check that over-captures would fail the outside mode)
 *   putllc          a GETLLAR, then a PUTLLC whose 128-byte line overlaps the
 *                   window -> stop 70, the guest line untouched
 *   putlluc         a PUTLLUC of the same line -> stop 70, line untouched
 *   putqlluc        a PUTQLLUC of the same line -> stop 70, line untouched
 *   putlluc-outside a PUTLLUC of the line just past the window -> exit 0, the
 *                   line committed, ps3_label_window_spu_puts() == 0
 *
 * The atomic modes run the command in a child over guest memory shared with
 * the parent: a stop is terminal (_Exit, no atexit handler), so only the parent
 * can observe that the line's bytes did not move. The parent re-exits 70, so
 * the driver still reads exactly one PS3STOP line.
 *
 * One process per case: a stop is terminal. tools/test_spu_label_window.py runs
 * every mode and checks the stop fields and the bytes.
 */
#include "spu_dma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Guest memory is shared with the forked child so the parent can see what the
 * child left behind after its stop. */
enum { GUEST_SIZE = 1u << 20 };
static uint8_t* g_mem;
uint8_t* vm_base;

/* The channel ABI, linked from runtime/spu/spu_channels.c (spu_interp.c keeps
 * the same local prototype). */
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);

/* The rest of the SPU runtime is linked, so only the state it leaves to the
 * host program is provided here (the set test_spu_dma_repeat.c uses). */
uint32_t ppu_vm_size;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

/* State the runtime's diagnostics around mfc_submit inspect. Every diagnostic
 * that would use them is off (the driver strips the SPU_/YDKJ_ environment). */
unsigned long long ps3_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull
         + (unsigned long long)ts.tv_nsec / 1000000ull;
}

enum {
    WINDOW_EA = 0x00020000,       /* 16-byte aligned, above the null-page PUT guard */
    WINDOW_LEN = 0x100,
    LINE_EA   = WINDOW_EA + 0x80, /* the 128-byte line overlapping the window's top half */
    OUTSIDE_LINE_EA = WINDOW_EA + WINDOW_LEN,  /* the line just past the window */
    LS_DATA   = 0x3000,
    LS_LIST   = 0x2000,
    LS_LINE   = 0x4000,
};

static spu_context g_ctx;
static mfc_engine g_mfc;

static void begin(void)
{
    spu_context_init(&g_ctx, 2);
    g_ctx.image_id = 7;
    g_ctx.pc = 0x1234;
    g_ctx.mfc_tag = 7;
    mfc_engine_init(&g_mfc);
    memset(g_mem, 0, GUEST_SIZE);
    for (uint32_t i = 0; i < 16; i++) g_ctx.ls[LS_DATA + i] = (uint8_t)(0xA0u + i);
}

/* Queue one plain PUT of LS_DATA to `ea`. */
static void put(uint32_t ea)
{
    g_ctx.mfc_lsa = LS_DATA;
    g_ctx.mfc_eah = 0;
    g_ctx.mfc_eal = ea;
    g_ctx.mfc_size = 16;
    mfc_submit(&g_mfc, &g_ctx, MFC_PUT_CMD);
}

/* Queue one PUTL whose single list element transfers LS_DATA to `ea`. */
static void putl(uint32_t ea)
{
    spu_ls_write32(&g_ctx, LS_LIST, 16);          /* element: size */
    spu_ls_write32(&g_ctx, LS_LIST + 4, ea);      /* element: EA low */
    g_ctx.mfc_lsa = LS_DATA;                      /* the destination in local store */
    g_ctx.mfc_eah = 0;
    g_ctx.mfc_eal = LS_LIST;                      /* the list's local-store address */
    g_ctx.mfc_size = 8;                           /* one 8-byte element */
    mfc_submit(&g_mfc, &g_ctx, MFC_PUTL_CMD);
}

static int landed(uint32_t ea)
{
    for (uint32_t i = 0; i < 16; i++)
        if (g_mem[ea + i] != (uint8_t)(0xA0u + i)) return 0;
    return 1;
}

static int fail(const char* what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

/* A window can be registered with an inert hook: this fixture exercises the SPU
 * side's overlap decision, not the PPU callback's ordering. */
static void noop_hook(uint32_t ea, uint32_t value) { (void)ea; (void)value; }

/* One MFC channel write as the SPU issues it: the preferred slot of the value
 * is what the channel sees (spu_dma.h, mfc_channel_write). MFC_Cmd enters the
 * production path, spu_wrch -> spu_mfc_atomic. */
static void mfc_write(uint32_t channel, uint32_t value)
{
    u128 v;
    memset(&v, 0, sizeof v);
    v._u32[0] = value;
    spu_wrch(&g_ctx, channel, v);
}

/* Stage one atomic command: LSA/EAH/EAL/Size/TagID, then the command itself.
 * `ea` is deliberately not line-aligned in the stop modes: the runtime owns the
 * line base (mfc_eal & ~127) and the stop must report that base. */
static void mfc_atomic_cmd(uint32_t cmd, uint32_t ea)
{
    mfc_write(MFC_LSA, LS_LINE);
    mfc_write(MFC_EAH, 0);
    mfc_write(MFC_EAL, ea);
    mfc_write(MFC_Size, MFC_ATOMIC_LINE);
    mfc_write(MFC_TagID, 7);
    mfc_write(MFC_Cmd, cmd);
}

static void fill_line(uint32_t lsa)
{
    for (uint32_t i = 0; i < MFC_ATOMIC_LINE; i++)
        g_ctx.ls[(lsa + i) & SPU_LS_MASK] = (uint8_t)(0xA0u + i);
}

/* The atomic command under test, in the child, with `ea` the staged EA inside
 * the line (not line-aligned: the runtime owns the line base). A return means
 * the runtime let it commit, which the parent reports. */
static void atomic_mode(const char* mode, uint32_t ea)
{
    if (!strcmp(mode, "putllc")) {
        /* A live, matching reservation first, so this PUTLLC would commit if
         * the window check were not in front of it. */
        mfc_atomic_cmd(MFC_GETLLAR_CMD, ea);
        fill_line(LS_LINE);
        mfc_atomic_cmd(MFC_PUTLLC_CMD, ea);
    } else if (!strcmp(mode, "putlluc")) {
        fill_line(LS_LINE);
        mfc_atomic_cmd(MFC_PUTLLUC_CMD, ea);
    } else {
        fill_line(LS_LINE);
        mfc_atomic_cmd(MFC_PUTQLLUC_CMD, ea);
    }
}

/* Run one atomic mode in a child over the shared guest memory; return how many
 * of the line's bytes the child left changed, and set *stopped when the child
 * ended in the named stop. */
static int run_atomic(const char* mode, uint32_t line_ea, int* stopped)
{
    uint8_t orig[MFC_ATOMIC_LINE];
    memcpy(orig, g_mem + line_ea, sizeof orig);
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }
    if (pid == 0) {
        atomic_mode(mode, line_ea + 0x20);
        _exit(0);                  /* it returned: the parent decides */
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) { perror("waitpid"); exit(2); }
    *stopped = WIFEXITED(st) && WEXITSTATUS(st) == 70;
    int changed = 0;
    for (uint32_t i = 0; i < MFC_ATOMIC_LINE; i++)
        changed += g_mem[line_ea + i] != orig[i];
    return changed;
}

/* The stop happens before the commit, so the line's bytes must be untouched.
 * The parent ends with the same code the child stopped on, so the driver reads
 * one PS3STOP line and the run's status is the stop's. */
static void expect_untouched_stop(const char* mode, uint32_t line_ea)
{
    int stopped = 0;
    const int changed = run_atomic(mode, line_ea, &stopped);
    if (!stopped) {
        fprintf(stderr, "FAIL: %s returned without a stop (%d guest bytes changed)\n",
                mode, changed);
        _exit(1);
    }
    if (changed) {
        fprintf(stderr, "FAIL: %s wrote %d guest bytes before the stop\n", mode, changed);
        _exit(1);
    }
    printf("UNTOUCHED: %s\n", mode);
    fflush(stdout);
    _exit(70);
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <overlap|overlap-edge|overlap-list|outside|overlap-control|outside-covered|putllc|putlluc|putqlluc|putlluc-outside>\n",
                argv[0]);
        return 2;
    }
    const char* mode = argv[1];

    g_mem = (uint8_t*)mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_mem == MAP_FAILED) { perror("mmap"); return 2; }
    vm_base = g_mem;
    begin();

    if (!strcmp(mode, "overlap")) {
        ppu_set_store_hook(WINDOW_EA, WINDOW_LEN, noop_hook);  /* window, no PPU hook needed */
        put(WINDOW_EA + 0x20);
    } else if (!strcmp(mode, "overlap-edge")) {
        /* The window begins at the PUT's last byte: one shared byte. */
        ppu_set_store_hook(WINDOW_EA + 0x20 + 15, 0x100, noop_hook);
        put(WINDOW_EA + 0x20);
    } else if (!strcmp(mode, "overlap-list")) {
        ppu_set_store_hook(WINDOW_EA, WINDOW_LEN, noop_hook);
        putl(WINDOW_EA + 0x10);
    } else if (!strcmp(mode, "outside")) {
        ppu_set_store_hook(WINDOW_EA, WINDOW_LEN, noop_hook);
        put(WINDOW_EA - 16);                       /* [lo-16, lo): below the window */
        if (!landed(WINDOW_EA - 16)) return fail("the PUT below the window did not land");
        put(WINDOW_EA + WINDOW_LEN);               /* [lo+len, ...): past the window */
        if (!landed(WINDOW_EA + WINDOW_LEN)) return fail("the PUT past the window did not land");
        printf("PASS: PUTs outside the window landed; spu_puts=%llu\n",
               ps3_label_window_spu_puts());
        return ps3_label_window_spu_puts() == 0 ? 0 : 1;
    } else if (!strcmp(mode, "overlap-control")) {
        /* The pre-fix runtime: no window registered, so the PUT lands. The
         * protected mode must be the only thing that turns this into a stop. */
        put(WINDOW_EA + 0x20);
        if (!landed(WINDOW_EA + 0x20)) return fail("the uncontrolled PUT did not land");
        printf("PASS: without a window the PUT landed; spu_puts=%llu\n",
               ps3_label_window_spu_puts());
        return 0;
    } else if (!strcmp(mode, "outside-covered")) {
        /* The fail-before witness for the outside mode's boundary: the same PUT
         * with a window that does cover it has to stop. */
        ppu_set_store_hook(WINDOW_EA - 16, 16, noop_hook);
        put(WINDOW_EA - 16);
    } else if (!strcmp(mode, "putllc") || !strcmp(mode, "putlluc") ||
               !strcmp(mode, "putqlluc")) {
        /* The line the window's top half covers: the atomic form must stop
         * before it commits, whether or not its reservation would have held.
         * The seed 0x5A is outside the 0xA0..0xFF and 0x00..0x1F the store
         * pattern covers, so the parent's byte count is all 128 bytes on a
         * commit and 0 on an untouched line. */
        ppu_set_store_hook(WINDOW_EA, WINDOW_LEN, noop_hook);
        memset(g_mem + LINE_EA, 0x5A, MFC_ATOMIC_LINE);
        expect_untouched_stop(mode, LINE_EA);
    } else if (!strcmp(mode, "putlluc-outside")) {
        ppu_set_store_hook(WINDOW_EA, WINDOW_LEN, noop_hook);
        fill_line(LS_LINE);
        mfc_atomic_cmd(MFC_PUTLLUC_CMD, OUTSIDE_LINE_EA);
        for (uint32_t i = 0; i < MFC_ATOMIC_LINE; i++)
            if (g_mem[OUTSIDE_LINE_EA + i] != (uint8_t)(0xA0u + i))
                return fail("the PUTLLUC past the window did not commit the line");
        printf("PASS: PUTLLUC past the window committed; spu_puts=%llu\n",
               ps3_label_window_spu_puts());
        return ps3_label_window_spu_puts() == 0 ? 0 : 1;
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }

    fprintf(stderr, "FAIL: %s returned without a stop (spu_puts=%llu)\n",
            mode, ps3_label_window_spu_puts());
    return 1;
}

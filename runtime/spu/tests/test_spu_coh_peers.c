/*
 * Synthetic concurrent-peer fixture for the lock-line notification contract.
 *
 * docs/SPU_XDIFF.md section 8.2 ("Open, before any T2/T3 promotion") records
 * that a transcript keeps the writer's bytes and status but not WHO was told:
 * an MFC PUT or an atomic line op also invalidates reservations held by PEER
 * contexts and wakes the line watchers registered on the lines it covers. A
 * contract rule cannot represent that until the recipient set and the order of
 * each kind of overlapping write are pinned.
 *
 * This fixture pins them, one case per kind of write against a line L that
 * context A has reserved with GETLLAR:
 *
 *   (1) a plain PUT overlapping L
 *   (2) a plain PUT to the adjacent line only
 *   (3) a plain PUT spanning L and its neighbour
 *   (4) a PUTLLC to L that succeeds
 *   (5) a PUTLLUC to L
 *   (6) a plain PUT by A itself (self-write)
 *
 * For each it asserts exactly: A's reservation survives or is cleared,
 * SPU_EVENT_LR is raised in A's event_status or not (and how many raises the
 * runtime counted), the watcher registered on L is woken or not and how many
 * times, whether the writer is told, and which host line-commit hook fires.
 * The order the runtime makes observable is asserted too:
 *
 *   - a watcher runs after the write's bytes are in guest memory and before
 *     any holder is touched: inside the callback A still holds its reservation,
 *     has no event bit, and g_spu_lr_raise has not moved;
 *   - the line-commit hook runs after the holder walk: for a committing
 *     PUTLLC/PUTLLUC, inside the hook A has already lost the reservation and
 *     taken the event.
 *
 * The order WITHIN the holder walk (event bit, reservation clear, host wake)
 * is NOT observable -- it happens inside one notify call with no interleaving
 * point, and the runtime exposes no counter for the wake -- so only its end
 * state is asserted.
 *
 * Everything runs on the test thread, in one process, with no sleeps. The two
 * contexts are driven through the real channel path (spu_wrch ->
 * spu_mfc_atomic / mfc_do_transfer) and the watcher through spu_coh_watch_add,
 * the API spu_workload.c's WAIT_SIGNAL uses. spu_channels.c is compiled in as
 * in test_spu_lockline_peer.c, so the runtime's other SPU units are linked.
 *
 * Exit code: 0 if all passed, 1 if any failed. Final line prints a summary.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Guest memory. The DMA and atomic paths index straight off this; a PUT below
 * 0x10000 is rejected as malformed, so the lines live well above it. */
static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

/* The real channel, DMA and atomic path. spu_channels.c also pulls in
 * spu_context.h and spu_coherency.h; they are named here because this fixture
 * drives their two contracts (the channel path and the watch/notify API). */
#include "../spu_context.h"
#include "../spu_coherency.h"
#include "../spu_channels.c"

/* ---------------------------------------------------------------------------
 * Harness
 * -----------------------------------------------------------------------*/
static int g_pass, g_fail;
static const char* g_current = "(none)";

#define TEST(name) g_current = (name)

#define CHECK(cond) do {                                          \
    if (cond) { g_pass++; }                                       \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n",             \
                g_current, __FILE__, __LINE__, #cond);            \
    }                                                             \
} while (0)

#define CHECK_EQ_U32(actual, expected) do {                       \
    uint32_t _a = (uint32_t)(actual), _e = (uint32_t)(expected);  \
    if (_a == _e) { g_pass++; }                                   \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n"              \
                        "  actual   0x%08X\n  expected 0x%08X\n", \
                g_current, __FILE__, __LINE__, #actual, _a, _e);  \
    }                                                             \
} while (0)

/* Each case owns a 0x600-byte window of guest memory: L and the line above it,
 * plus room either side. Per-case windows keep one case's permanent SPU mark
 * (marks are never cleared) out of the next case's path. */
#define CASE_LINES(i)  (0x00020000u + (uint32_t)(i) * 0x600u)

/* Local-store staging, one aligned block per context. */
#define LSA_A  0x00001000u
#define LSA_B  0x00002000u

/* 256 KB of local store each: not on the stack. */
static spu_context g_a;   /* the reserving peer */
static spu_context g_b;   /* the writer in cases 1..5 */

/* ---------------------------------------------------------------------------
 * What the runtime lets the fixture see
 * -----------------------------------------------------------------------*/

/* The watcher samples A's state at the instant it is called. All three fields
 * are written by the notifier on this same thread, after this callback. */
typedef struct {
    int wakes;          /* callbacks */
    int saw_bytes;      /* the write's bytes were already in guest memory */
    int saw_valid;      /* A still held the reservation */
    int saw_lr;         /* A already had SPU_EVENT_LR */
    int saw_raise;      /* g_spu_lr_raise had already moved */
    int seq;            /* position among the host recipients of this write */
    uint64_t saw_epoch; /* A->resv_epoch at the wake */
} watch_obs;

static watch_obs g_obs;
static spu_coh_watch g_watch;

/* The write extent of the current case, so the callback can tell whether the
 * data is in guest memory when it runs. */
static uint32_t g_write_ea, g_write_len;
static uint8_t  g_write_tag;
static uint32_t g_reserved_line;

/* A raise count at the start of the case: every case has at most one holder
 * that can be notified, so the delta is 0 or 1. */
static unsigned long g_raise_before;

/* The atomic line-commit hook (spu_channels.c): the runtime's own record that
 * a line was committed by an atomic store. spu_taskset_line_committed runs
 * first and has no observable effect with no taskset registered. */
static int g_hook_hits;
static uint32_t g_hook_line;
static int g_hook_saw_valid, g_hook_saw_lr;

/* Every host recipient the fixture can see takes a ticket when it runs, so the
 * order BETWEEN them is a fact of the run and not an inference. */
static int g_recipient_seq;
static int g_hook_seq;

static void on_commit(uint32_t line)
{
    g_hook_seq = ++g_recipient_seq;
    g_hook_hits++;
    g_hook_line = line;
    if (g_a.resv_valid) g_hook_saw_valid = 1;
    if (g_a.event_status & SPU_EVENT_LR) g_hook_saw_lr = 1;
}

static int mem_is(uint32_t ea, uint8_t tag, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        if (g_guest_mem[ea + i] != (uint8_t)(tag + (i & 0x0F))) return 0;
    return 1;
}

/* Called from spu_coh_notify_write{,_except} under the lock-line lock, between
 * the line's epoch bump and the holder walk. It may not re-enter coherence or
 * alter the watch, so it only samples. */
static void on_wake(void* opaque)
{
    watch_obs* o = (watch_obs*)opaque;
    o->seq = ++g_recipient_seq;
    o->wakes++;
    if (mem_is(g_write_ea, g_write_tag, g_write_len)) o->saw_bytes = 1;
    if (g_a.resv_valid) o->saw_valid = 1;
    if (g_a.event_status & SPU_EVENT_LR) o->saw_lr = 1;
    if (g_spu_lr_raise != g_raise_before) o->saw_raise = 1;
    o->saw_epoch = g_a.resv_epoch;
}

/* ---------------------------------------------------------------------------
 * Driving the real channel path
 *
 * Every command goes through spu_wrch, so what runs is spu_mfc_atomic() and
 * mfc_do_transfer() themselves rather than a copy of their reservation rules.
 * -----------------------------------------------------------------------*/
static void wrch(spu_context* ctx, uint32_t channel, uint32_t v)
{
    spu_wrch(ctx, channel, spu_make_preferred_u32(v));
}

static void spu_atomic(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t cmd)
{
    wrch(ctx, MFC_LSA, lsa);
    wrch(ctx, MFC_EAH, 0);
    wrch(ctx, MFC_EAL, ea);
    wrch(ctx, MFC_Cmd, cmd);
}

static void spu_put(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t size)
{
    wrch(ctx, MFC_LSA, lsa);
    wrch(ctx, MFC_EAH, 0);
    wrch(ctx, MFC_EAL, ea);
    wrch(ctx, MFC_Size, size);
    wrch(ctx, MFC_TagID, 0);
    wrch(ctx, MFC_Cmd, MFC_PUT_CMD);
}

/* Recognisable pattern, so "the write landed" and "the write landed from the
 * context that should have won" stay separate assertions. */
static void stage(spu_context* ctx, uint32_t lsa, uint8_t tag, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        ctx->ls[(lsa + i) & SPU_LS_MASK] = (uint8_t)(tag + (i & 0x0F));
}

/* ---------------------------------------------------------------------------
 * One case
 * -----------------------------------------------------------------------*/
static void case_begin(unsigned idx, uint32_t write_ea, uint32_t write_len,
                       uint8_t tag)
{
    /* Re-initialised rather than re-created: spu_coherency.c's holder table
     * keys on the context address and keeps it, so reusing the two contexts
     * keeps the fixture to two holders. */
    spu_context_init(&g_a, 0);
    spu_context_init(&g_b, 1);
    /* The guest idiom this contract exists for: park in RdEventStat on the
     * loss of the reservation. */
    g_a.event_mask = SPU_EVENT_LR;

    g_reserved_line = CASE_LINES(idx);
    memset(g_guest_mem + g_reserved_line, 0, 0x600);
    memset(&g_obs, 0, sizeof g_obs);
    g_write_ea  = write_ea;
    g_write_len = write_len;
    g_write_tag = tag;
    g_raise_before = g_spu_lr_raise;
    g_recipient_seq = 0;
    g_hook_seq = 0;
    g_hook_hits = 0;
    g_hook_line = 0;
    g_hook_saw_valid = 0;
    g_hook_saw_lr = 0;
}

/* A takes the reservation on the case's line, through GETLLAR. */
static void reserve_a(void)
{
    spu_atomic(&g_a, LSA_A, g_reserved_line, MFC_GETLLAR_CMD);
    CHECK(g_a.resv_valid == 1);
    CHECK_EQ_U32(g_a.resv_ea, g_reserved_line);
    CHECK_EQ_U32(g_a.event_status, 0);
}

static void watch_begin(void)
{
    /* Add and remove are the caller's critical section, as in
     * spu_workload.c's WAIT_SIGNAL. Registration also marks the line, which it
     * already is: A reserved it. */
    spu_lockline_lock();
    spu_coh_watch_add(&g_watch, g_reserved_line, on_wake, &g_obs);
    spu_lockline_unlock();
}

static void watch_end(void)
{
    spu_lockline_lock();
    spu_coh_watch_remove(&g_watch);
    spu_lockline_unlock();
}

typedef struct {
    const char* name;
    int valid;              /* A's reservation after the write */
    int lr;                 /* SPU_EVENT_LR in A's event_status after */
    int wakes;              /* watcher callbacks */
    int hook;               /* line-commit hook calls */
    int epoch_after_wake;   /* 1: A's resv_epoch must move after the wake */
} want;

/* Print the observed recipient set for this case and assert it. The order
 * checks are the ones the runtime makes observable; `wakes == 0` prints "-"
 * for them, because nothing ran to order. */
static void finish(const want* w)
{
    const int r_valid = g_a.resv_valid != 0;
    const int r_lr = (g_a.event_status & SPU_EVENT_LR) != 0;
    const unsigned long raises = g_spu_lr_raise - g_raise_before;
    char at_wake[24] = "-";
    char order[16] = "-";

    if (g_obs.wakes)
        snprintf(at_wake, sizeof at_wake, "%d/%d/%d",
                 g_obs.saw_bytes, g_obs.saw_valid, g_obs.saw_lr);
    if (g_obs.wakes && g_hook_hits)
        snprintf(order, sizeof order, "%s", g_obs.seq < g_hook_seq ? "watch<hook" : "hook<watch");
    else if (g_obs.wakes)
        snprintf(order, sizeof order, "%s", "watch only");
    else if (g_hook_hits)
        snprintf(order, sizeof order, "%s", "hook only");

    watch_end();

    printf("  %-36s %-9s %-6s %-7d %-7d %-6lu %-22s %s\n",
           w->name,
           r_valid ? "held" : "cleared",
           r_lr ? "raised" : "-",
           g_obs.wakes, g_hook_hits, raises, at_wake, order);

    CHECK_EQ_U32(r_valid, w->valid);
    CHECK_EQ_U32(r_lr, w->lr);
    CHECK_EQ_U32(g_obs.wakes, w->wakes);
    CHECK_EQ_U32(g_hook_hits, w->hook);
    /* One raise per notified holder; A is the only holder these cases give
     * the line, so a raise is exactly one count. */
    CHECK_EQ_U32(raises, w->lr ? 1u : 0u);

    if (w->wakes) {
        /* The write is in memory before the watcher runs... */
        CHECK_EQ_U32(g_obs.saw_bytes, 1);
        /* ...and the holder has not been touched yet: watchers are woken
         * before the holder walk, not after it. */
        CHECK_EQ_U32(g_obs.saw_valid, 1);
        CHECK_EQ_U32(g_obs.saw_lr, 0);
        CHECK_EQ_U32(g_obs.saw_raise, 0);
    }
    if (w->epoch_after_wake) {
        /* Self-write: A's reservation adopts the new epoch, and it does so
         * after the watcher has been called. */
        CHECK(g_a.resv_epoch != g_obs.saw_epoch);
    }
    if (w->hook) {
        /* The atomic line-commit hook is the last thing the commit does, so by
         * then the peer notification has already happened. */
        CHECK_EQ_U32(g_hook_line, g_reserved_line);
        CHECK_EQ_U32(g_hook_saw_valid, 0);
        CHECK_EQ_U32(g_hook_saw_lr, 1);
        /* With both recipients present, the watcher ran first. */
        if (w->wakes) CHECK(g_obs.seq < g_hook_seq);
    }
}

/* ===========================================================================
 * (1) a plain PUT overlapping the reserved line
 *
 * spu_dma.h's PUT path finds the line marked and runs the copy and its notify
 * as one critical section, with the ISSUER excluded
 * (spu_coh_notify_write_except): A is a recipient, B is not.
 * ===========================================================================*/
static void test_plain_put_overlapping(void)
{
    TEST("plain PUT overlapping the reserved line");
    case_begin(0, CASE_LINES(0), 128, 0xB0);
    reserve_a();
    watch_begin();

    stage(&g_b, LSA_B, 0xB0, 128);
    spu_put(&g_b, LSA_B, CASE_LINES(0), 128);

    CHECK(mem_is(CASE_LINES(0), 0xB0, 128));
    /* The writer holds no reservation, so the write is not a loss to it. */
    CHECK_EQ_U32(g_b.event_status, 0);
    finish(&(want){ "PUT, 128B over L", 0, 1, 1, 0, 0 });
}

/* ===========================================================================
 * (2) a plain PUT to the adjacent line only
 *
 * The adjacent line is not marked and nobody holds it, so the copy takes the
 * lock-free path and no notify happens at all: A's reservation and event bit
 * are untouched and the watcher registered on L is not woken.
 * ===========================================================================*/
static void test_plain_put_adjacent(void)
{
    TEST("plain PUT to the adjacent line only");
    case_begin(1, CASE_LINES(1) + 128, 128, 0xC0);
    reserve_a();
    watch_begin();

    stage(&g_b, LSA_B, 0xC0, 128);
    spu_put(&g_b, LSA_B, CASE_LINES(1) + 128, 128);

    CHECK(mem_is(CASE_LINES(1) + 128, 0xC0, 128));
    CHECK_EQ_U32(g_b.event_status, 0);
    finish(&(want){ "PUT, 128B into L+128", 1, 0, 0, 0, 0 });
}

/* ===========================================================================
 * (3) a plain PUT spanning both lines
 *
 * The span contains the reserved line, so the copy moves under the lock-line
 * lock and every RESERVED line in it is notified -- once, for L. The
 * neighbour is copied but is not a recipient: no reservation and no watcher
 * are registered on that line.
 * ===========================================================================*/
static void test_plain_put_spanning(void)
{
    TEST("plain PUT spanning the reserved line and its neighbour");
    case_begin(2, CASE_LINES(2), 256, 0xB0);
    reserve_a();
    watch_begin();

    stage(&g_b, LSA_B, 0xB0, 256);
    spu_put(&g_b, LSA_B, CASE_LINES(2), 256);

    CHECK(mem_is(CASE_LINES(2), 0xB0, 256));
    CHECK_EQ_U32(g_b.event_status, 0);
    finish(&(want){ "PUT, 256B over L and L+128", 0, 1, 1, 0, 0 });
}

/* ===========================================================================
 * (4) a PUTLLC to the reserved line that succeeds
 *
 * The committer needs its own reservation on the same line, so B GETLLARs L
 * as well -- taking a reservation notifies nobody. On commit the code clears
 * B's own reservation BEFORE the notify, so the notify finds only A: the
 * committing SPU consumes its reservation and is not told it was lost.
 * ===========================================================================*/
static void test_putllc_success(void)
{
    TEST("PUTLLC to the reserved line that succeeds");
    case_begin(3, CASE_LINES(3), 128, 0xB0);
    reserve_a();
    spu_atomic(&g_b, LSA_B, CASE_LINES(3), MFC_GETLLAR_CMD);
    CHECK(g_b.resv_valid == 1);
    watch_begin();

    stage(&g_b, LSA_B, 0xB0, 128);
    spu_atomic(&g_b, LSA_B, CASE_LINES(3), MFC_PUTLLC_CMD);

    CHECK_EQ_U32(g_b.atomic_stat, 0);          /* PUTLLC_SUCCESS */
    CHECK_EQ_U32(g_b.event_status, 0);         /* consumed, not reported lost */
    CHECK(mem_is(CASE_LINES(3), 0xB0, 128));
    finish(&(want){ "PUTLLC, success on L", 0, 1, 1, 1, 0 });
}

/* ===========================================================================
 * (5) a PUTLLUC to the reserved line
 *
 * Unconditional, so it is a write like any other: A loses the reservation and
 * takes the event. B holds nothing here, so the "every reservation on the
 * line" rule has only A to notify.
 * ===========================================================================*/
static void test_putlluc(void)
{
    TEST("PUTLLUC to the reserved line");
    case_begin(4, CASE_LINES(4), 128, 0xB0);
    reserve_a();
    watch_begin();

    stage(&g_b, LSA_B, 0xB0, 128);
    spu_atomic(&g_b, LSA_B, CASE_LINES(4), MFC_PUTLLUC_CMD);

    CHECK_EQ_U32(g_b.atomic_stat, 2);          /* PUTLLUC complete */
    CHECK_EQ_U32(g_b.event_status, 0);
    CHECK(mem_is(CASE_LINES(4), 0xB0, 128));
    finish(&(want){ "PUTLLUC on L", 0, 1, 1, 1, 0 });
}

/* ===========================================================================
 * (6) a plain PUT by the reserving context itself
 *
 * The MFC is part of the issuing SPE, so its own write does not cost it its
 * reservation: spu_coh_notify_write_except skips the issuer, whose reservation
 * adopts the line's new epoch instead. The watcher is still woken -- it watches
 * the line, not a particular writer -- and no event is raised.
 * ===========================================================================*/
static void test_self_put(void)
{
    TEST("plain PUT by the reserving context itself");
    case_begin(5, CASE_LINES(5), 128, 0xC0);
    reserve_a();
    watch_begin();

    /* The bytes the PUT sends are A's own local store. Staging a new pattern
     * over the GETLLAR copy makes "the PUT landed" checkable. */
    stage(&g_a, LSA_A, 0xC0, 128);
    spu_put(&g_a, LSA_A, CASE_LINES(5), 128);

    CHECK(mem_is(CASE_LINES(5), 0xC0, 128));
    CHECK_EQ_U32(g_a.resv_ea, CASE_LINES(5));
    finish(&(want){ "PUT by A itself over L", 1, 0, 1, 0, 1 });
}

/* ===========================================================================
 * main
 * ===========================================================================*/
int main(void)
{
    g_spu_line_commit_hook = on_commit;

    printf("SPU lock-line peers: recipients and order per overlapping write\n");
    printf("  %-36s %-9s %-6s %-7s %-7s %-6s %-22s %s\n",
           "write against the reservation", "A resv", "L-R", "watcher",
           "commit", "raises", "at-wake(bytes/valid/L-R)", "order");

    test_plain_put_overlapping();
    test_plain_put_adjacent();
    test_plain_put_spanning();
    test_putllc_success();
    test_putlluc();
    test_self_put();

    g_spu_line_commit_hook = 0;
    printf("\nSPU lock-line peer notification: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

/* ---------------------------------------------------------------------------
 * The runtime beyond the SPU channel layer, stubbed. These are what
 * spu_channels.c reaches for outside its own subsystem: the PPU's watch
 * helpers, the host clock, and the title-specific SPURS hooks. None of them is
 * on the path this fixture drives. Same set as test_spu_lockline_peer.c.
 * -----------------------------------------------------------------------*/
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

/* Loader diagnostics and worker publication are outside this fixture. */
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

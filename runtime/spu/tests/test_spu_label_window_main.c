/* An SPU MFC PUT/PUTL into the guest store window a device model registered
 * (the RSX label window) is counted and stops by name: runtime/spu/spu_dma.h,
 * ps3_stop_spu_label_window. The runtime cannot tell whether the PUT produces
 * the value an RSX ACQUIRE waits for (it has to land before the walker reads
 * it) or overwrites a label the walker has not read yet (it has to land after a
 * pass), so it fails closed instead of waiting for a pass and deadlocking the
 * producer case. docs/STOPS.md, "spu-label-window".
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
 *
 * One process per case: a stop is terminal. tools/test_spu_label_window.py runs
 * every mode and checks the stop fields and the bytes.
 */
#include "spu_dma.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static uint8_t g_mem[1u << 20];
uint8_t* vm_base = g_mem;

int g_cri_video_dma = 0;
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_coh_notify_write_except(uint32_t ea, const void* self) { (void)ea; (void)self; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockguard_check(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size,
                         const char* what)
{
    (void)ctx; (void)ea; (void)ls; (void)size; (void)what;
}
void (*g_spu_put_hook)(uint32_t ea, uint32_t size) = 0;

/* State the runtime's diagnostics around mfc_submit inspect. Every diagnostic
 * that would use them is off (the driver strips the SPU_/YDKJ_ environment). */
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_taskset_ea;
SPU_THREAD_LOCAL volatile unsigned g_wws_batch_gets;
void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size)
{
    (void)ctx; (void)ea; (void)ls; (void)size;
}
unsigned* spu_ls_watch_list(int* out_n)
{
    if (out_n) *out_n = 0;
    return NULL;
}
void* volatile g_sn_defer_ctx;
volatile unsigned g_sn_defer;
unsigned spu_sn_defer_ticks(void) { return 0; }
void spu_ch_wake(spu_context* ctx) { (void)ctx; }

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
    LS_DATA   = 0x3000,
    LS_LIST   = 0x2000,
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
    memset(g_mem, 0, sizeof g_mem);
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

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <overlap|overlap-edge|overlap-list|outside|overlap-control|outside-covered>\n",
                argv[0]);
        return 2;
    }
    const char* mode = argv[1];
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
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }

    fprintf(stderr, "FAIL: %s returned without a stop (spu_puts=%llu)\n",
            mode, ps3_label_window_spu_puts());
    return 1;
}

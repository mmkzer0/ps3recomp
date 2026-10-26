/* The FIFO walker's side of an RSX capture (rsx_capture.h, ps3diff.rsx/1).
 *
 * Runs the real cellGcm_rsx_process_fifo over a synthetic ring with a capture open, and leaves the
 * capture in the directory named by argv[1] for tools/test_rsx_capture.py to check:
 *
 *   - a SET_OBJECT bind, a rebind of the same subchannel, a method packet, a run of zero words
 *     (one event), CALL -> subroutine -> RET, a JUMP: the executed order, not the ring order;
 *   - a NV406E semaphore: OFFSET, RELEASE (a write), ACQUIRE that waits (no event: the walker reads
 *     it again next pass) and then consumes the label's value as it stood at that moment.
 *
 * argv[2], when given, names a scenario. "retries" runs to the end like the default one; the others must END IN A
 * STOP: the process then exits 70 through the stop and the Python side asserts the machine line:
 *   midstream      the first RSX command comes before any cellGcmSetupContext
 *
 * cc -std=gnu17 ... (CMake target test_rsx_capture_walker)
 */
#include "../cellGcmSys.c"
#include "ps3emu/config.h"
#include <stdio.h>
#include <stdlib.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
int  rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
u32  rsx_dsp_method_class(u32 method) { (void)method; return RSX_DSP_CLASS_TODO; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_set_display_buffer(u32 id, u32 loc, u32 off, u32 pitch, u32 w, u32 h)
{ (void)id; (void)loc; (void)off; (void)pitch; (void)w; (void)h; }
void rsx_live_draw_set_display_buffer(u32 id, u32 loc, u32 off, u32 pitch, u32 w, u32 h)
{ (void)id; (void)loc; (void)off; (void)pitch; (void)w; (void)h; }
void rsx_draw_engine_set_capture_sink(const rsx_capture_sink* sink) { (void)sink; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }

const ps3_config* ps3_config_get(void)
{
    static ps3_config cfg;
    cfg.gcm_wait_timeout_ms = 10000;
    return &cfg;
}

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00400000u
#define CMD_SIZE  0x00080000u
#define LABEL     5u

static uint32_t g_heap = 0x00800000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void host_w32(u32 ea, u32 v) { vm_write32(ea, v); }

/* Ring writer: one word at IO offset `io`. */
static void ring(u32 io, u32 v) { vm_write32(IO_ADDR + io, v); }
static u32 hdr(u32 count, u32 subch, u32 method) { return (count << 18) | (subch << 13) | method; }

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: %s <capture dir> [scenario]\n", argv[0]); return 2; }
    const char* scenario = argc > 2 ? argv[2] : "";
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    rsx_capture_options opt = { argv[1], 0 };
    rsx_capture_environment env = { "synthetic", "n/a", "n/a", "Debug", "n/a", "n/a", "synthetic", "n/a" };
    (void)rsx_capture_install(&env, &opt);

    const u32 ring0 = 0x1000;                          /* the first segment: GET after SetupContext */
    if (!strcmp(scenario, "midstream")) {
        /* No cellGcmSetupContext: the walker has no context and returns before it reads a word, so
         * drive the first step by hand the way the walker does. */
        rsx_cap_step st = { RSX_CAP_STEP_METHOD, ring0, ring0 + 8, 0, 0, 1, 1, 0, NULL, 1, gcm_io2ea };
        u32 w = hdr(1, 0, 0x100);
        st.words = &w;
        s_cap->step_begin(&st);
        return 1;                                      /* not reached */
    }

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    s_display_buffer_set[1] = 1;
    cellGcmSetDisplayBuffer(1, 0x100000, 5120, 1280, 720);

    u32 at = ring0;
    ring(at, hdr(1, 1, 0x0000)); ring(at + 4, 0x31337303u); at += 8;          /* SET_OBJECT sub1 = M2MF */
    ring(at, hdr(2, 0, 0x0200));  ring(at + 4, 0x00000500u); ring(at + 8, 0x000002D0u); at += 12;
    ring(at, 0); ring(at + 4, 0); ring(at + 8, 0); at += 12;                    /* three zero words */
    ring(at, hdr(1, 1, 0x0000)); ring(at + 4, 0x313371C3u); at += 8;           /* sub1 rebound */
    const u32 sub = 0x1400;
    ring(at, sub | 2u); at += 4;                                               /* CALL sub */
    const u32 after_call = at;
    ring(at, 0x20000000u | 0x1040u); at += 4;                                  /* JUMP 0x1040 */
    ring(sub, hdr(1, 0, 0x0300)); ring(sub + 4, 0x12345678u); ring(sub + 8, 0x00020000u);   /* RET */
    (void)after_call;
    at = 0x1040;
    ring(at, hdr(1, 0, 0x0064)); ring(at + 4, LABEL * GCM_LABEL_STRIDE); at += 8;   /* SEMAPHORE_OFFSET */
    ring(at, hdr(1, 0, 0x006C)); ring(at + 4, 7u); at += 8;                          /* RELEASE 7 */
    ring(at, hdr(1, 0, 0x0068)); ring(at + 4, 9u); at += 8;                          /* ACQUIRE 9 */
    ring(at, 0); ring(at + 4, 0); at += 8;                                           /* two zero words */

    const u32 label_ea = GCM_LABEL_GUEST_BASE + LABEL * GCM_LABEL_STRIDE;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, at);
    cellGcm_rsx_process_fifo();
    /* "retries": the walker sits on the ACQUIRE for several more passes before the agent arrives. How many is
     * host timing, so it must not show in the capture (the read ordinals of a cancelled step are given back). */
    if (!strcmp(scenario, "retries"))
        for (int i = 0; i < 7; i++) cellGcm_rsx_process_fifo();
    int ok = vm_read32(label_ea) == 7u && s_fifo_getoff < at;
    printf("[%s] the walk stops at the ACQUIRE (label %u, get 0x%x of 0x%x)\n", ok ? "PASS" : "FAIL",
           vm_read32(label_ea), s_fifo_getoff, at);

    vm_write32(label_ea, 9u);                                                        /* the agent arrives */
    cellGcm_rsx_process_fifo();
    const int ok2 = s_fifo_getoff == at;
    printf("[%s] the ACQUIRE is consumed once the label holds 9 (get 0x%x)\n", ok2 ? "PASS" : "FAIL", s_fifo_getoff);

    /* The agent rewrites the label after the RSX has consumed it: the capture holds the value it read. */
    vm_write32(label_ea, 0xFFFFu);

    rsx_capture_finish();
    printf("[walker capture] %s\n", ok && ok2 ? "PASS" : "FAIL");
    free(vm_base);
    return ok && ok2 ? 0 : 1;
}

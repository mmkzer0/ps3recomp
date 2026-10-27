/* A method is interpreted in the class bound on its subchannel, with the REAL walker and the REAL
 * register-file draw engine behind it (no recording stubs: the engine's register file is read back).
 *
 * The draw engine and the live-draw mirror keep one NV4097 register file and mask the subchannel off the
 * method, so a word of another class used to be stored as an NV4097 register: NV308A's 0x018C/0x0194/0x0198
 * are SET_CONTEXT_DMA_COLOR1/COLOR0/ZETA there, and a 2D write to them moved the colour and depth surfaces to
 * RSX local memory while their offsets still named main memory. The census fixture
 * (test_rsx_method_census.c) cannot see that: it stubs the engine out.
 *
 * libgcm's default state binds subchannel 0 NV4097, 1 NV0039, 3 NV3062, 4 NV309E, 5 NV308A, 6 NV3089 and 7
 * the driver object (gcm_default_state.c; cellGcmSetupContext binds the same table). Subchannel 2 is
 * unbound until a title binds it.
 *
 * It also pins the census verdict of methods only the engine consumes, with the engine on and no vtable
 * backend (the legacy state machine is then not fed): a register the engine reads is classified, not an
 * unknown method that ends the run.
 *
 * cc -std=gnu17 -I include -I libs/video libs/video/tests/test_gcm_class_routing.c
 *   libs/video/rsx_dispatch.c libs/video/rsx_vertex_compact.c libs/video/rsx_texture_layout.c
 *   libs/video/rsx_vp_decompiler.c libs/video/rsx_fp_decompiler.c runtime/memory/label_window.c
 *   libs/video/gcm_default_state.c -ffunction-sections -fdata-sections -Wl,-dead_strip -o /tmp/t
 *   (gcc: -Wl,--gc-sections -pthread -lm; tools/test_gcm_class_routing.py builds and runs it)
 */
#include "../cellGcmSys.c"
#include "../rsx_draw_engine.c"
#include "gcm_fixture_stubs.h"   /* ps3_config_get and the named stops cellGcmSys.c ends in */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }

/* The legacy vtable state machine is not under test: with the engine on and no backend registered the
 * walker hands it only the methods it acts on beyond its own state (gcm_legacy_method_has_effect). */
static u32 t_legacy_seen[16], t_legacy_n;
int rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state; (void)data;
    if (t_legacy_n < 16) t_legacy_seen[t_legacy_n++] = method;
    if (method == 0x0050u) g_rsx_last_reference = data;   /* what the real handler does */
    return 0;
}
int rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
rsx_backend* rsx_get_backend(void) { return NULL; }
/* The live-draw engine is off; the engine above is the real one. */
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
void rsx_live_draw_set_display_buffer(u32 id, u32 loc, u32 off, u32 pitch, u32 w, u32 h)
{ (void)id; (void)loc; (void)off; (void)pitch; (void)w; (void)h; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g_, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g_; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00100000u
#define CMD_SIZE  0x00010000u

static uint32_t g_heap = 0x00400000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

static int t_fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    t_fails += !ok;
}

/* One FIFO stream: single-word method packets, walked in one pass. */
static u32 t_words[128], t_nwords;
static void pkt(u32 subch, u32 method, u32 data)
{
    t_words[t_nwords++] = (1u << 18) | (subch << 13) | method;
    t_words[t_nwords++] = data;
}
static void walk(u32 ctrl)
{
    for (u32 i = 0; i < t_nwords; i++) gw32(IO_ADDR + i * 4, t_words[i]);
    t_legacy_n = 0;
    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, t_nwords * 4);
    t_nwords = 0;
    cellGcm_rsx_process_fifo();
}

static u32 reg(u32 method) { return rsx_dsp_reg(&g.rsx, method); }
static GcmMethodStats stats(void) { GcmMethodStats s; gcm_method_stats(&s); return s; }

#define DMA_MAIN 0xFEED0001u
#define DMA_LOCAL 0xFEED0000u

/* The backend the engine needs to take the default state's CLEAR: surface handles and calls that draw nothing. */
static u32 t_next_handle = 1;
static u32 t_color_create(void* u, rsx_be_format f, u32 w, u32 h, const void* seed, u32 rb)
{ (void)u; (void)f; (void)w; (void)h; (void)seed; (void)rb; return t_next_handle++; }
static u32 t_depth_create(void* u, u32 w, u32 h) { (void)u; (void)w; (void)h; return t_next_handle++; }
static void t_release(void* u, u32 s) { (void)u; (void)s; }
static void t_clear_color(void* u, u32 s, const float rgba[4]) { (void)u; (void)s; (void)rgba; }
static void t_clear_depth(void* u, u32 d, u32 flags, float dv, u8 st) { (void)u; (void)d; (void)flags; (void)dv; (void)st; }
static void t_present(void* u, u32 s) { (void)u; (void)s; }

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    /* "fast": no engine and no per-word diagnostic, so the walker's 3D-run fast path is eligible. */
    const int fast = argc > 1 && !strcmp(argv[1], "fast");

    /* The engine comes up before the first pass: the walker caches whether it is on. */
    /* The register file is what is read back, so the backend only has to give the default state's CLEAR
     * (mask 0) targets to clear: handles, and calls that do nothing. */
    static rsx_draw_backend be = {
        .color_target_create = t_color_create, .depth_target_create = t_depth_create,
        .color_target_release = t_release, .depth_target_release = t_release,
        .clear_color = t_clear_color, .clear_depth_stencil = t_clear_depth, .present = t_present,
    };
    if (!fast) {
        rsx_draw_engine_set_backend(&be);
        rsx_draw_engine_set_default(1);
        if (rsx_draw_engine_init(64, 64) != 0) { puts("FAIL: engine init"); return 1; }
    }
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    if (fast) {
        /* NV4097 bound on slot 7, a run of one word on the fast path's terms: 0xE944 is the driver's flip
         * queue word, not NV4097 method 0x0944 (a vertex-texture register). */
        gcm_bind_object(7, RSX_OBJ_NV4097);
        const unsigned long long q0 = gcm_driver_event_count(GCM_DRV_EVENT_QUEUE_FLIP);
        pkt(7, 0x0944, 6u);
        walk(ctrl);
        check(gcm_driver_event_count(GCM_DRV_EVENT_QUEUE_FLIP) == q0 + 1u && s_drv_flip_queue == 6u && t_legacy_n == 0,
              "NV4097 bound on slot 7, fast path eligible: 0xE944 is the driver's flip queue word");
        printf("[gcm_class_routing fast] %s (%d failed)\n", t_fails ? "FAIL" : "PASS", t_fails);
        free(vm_base);
        return t_fails ? 1 : 0;
    }

    /* 0. libgcm's own default-state stream, the one every flip CALLs, through the walker with the engine on:
     * every word classified (an unknown one is a named stop that ends this fixture), and its 2D traffic
     * neither reaches the NV4097 file nor reads NV308A's COLOR_FORMAT (0x0300) as the swizzled surface's
     * SET_FORMAT. Its RET is left out: nothing CALLed it. */
    const u32 dlen = gcm_emit_default_state(IO_ADDR);
    const GcmMethodStats d0 = stats();
    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, dlen - 4u);
    cellGcm_rsx_process_fifo();
    const GcmMethodStats d1 = stats();
    check(s_fifo_getoff == dlen - 4u && d1.unknown == d0.unknown && d1.unbound == d0.unbound,
          "the default-state stream is walked to its end, every word classified, none dropped");
    check(reg(0x018C) == DMA_LOCAL && reg(0x0194) == DMA_LOCAL && reg(0x0198) == DMA_LOCAL,
          "...NV308A's 0x018C/0x0194/0x0198 (zeros there) leave NV4097's DMA contexts as NV4097 set them");
    check(!s_nv309e.active, "...and NV308A's COLOR_FORMAT is not the swizzled surface's SET_FORMAT");

    /* 1. The census verdict of a register only the engine reads. The engine is on, no vtable backend is
     * registered, so the state machine is not fed and the class table is the only consumer on record. These
     * are the words libgcm's default state sends every flip (gcm_default_state.c). */
    pkt(0, 0x08C0, 0x10000000u);                     /* SET_SCISSOR_HORIZONTAL */
    pkt(0, 0x08C4, 0x10000000u);                     /* SET_SCISSOR_VERTICAL */
    pkt(0, 0x0908, 0x00000101u);                     /* vertex texture 0 format */
    pkt(0, 0x1FC0, 0x00000000u);                     /* SET_FREQUENCY_DIVIDER_OPERATION */
    pkt(0, 0x1FF8, 0x00000000u);                     /* VP static branch bits */
    const GcmMethodStats s0 = stats();
    walk(ctrl);
    const GcmMethodStats s1 = stats();
    check(s1.unknown == s0.unknown && s1.handled - s0.handled == 5,
          "an engine-consumed register is handled with the engine on and no vtable backend");
    check(reg(0x08C0) == 0x10000000u && reg(0x08C4) == 0x10000000u && reg(0x0908) == 0x101u,
          "...and the engine took it");
    check(t_legacy_n == 0, "...without the legacy state machine");

    /* 2. NV4097 surface state, then NV308A traffic on its own subchannel (5) writing the same numbers. */
    pkt(0, 0x018C, DMA_MAIN);                        /* COLOR1 */
    pkt(0, 0x0194, DMA_MAIN);                        /* COLOR0 */
    pkt(0, 0x0198, DMA_MAIN);                        /* ZETA */
    pkt(0, 0x0210, 0x1000);                          /* COLOR0 offset */
    pkt(0, 0x0218, 0x2000);                          /* COLOR1 offset */
    pkt(0, 0x0214, 0x3000);                          /* ZETA offset */
    pkt(0, 0x0180, 0x66604200u);                     /* notify context: the state machine's, by effect */
    walk(ctrl);
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    check(sf.color_location[0] == RSX_LOCATION_MAIN && sf.color_location[1] == RSX_LOCATION_MAIN &&
          sf.zeta_location == RSX_LOCATION_MAIN && sf.color_offset[0] == 0x1000 &&
          sf.color_offset[1] == 0x2000 && sf.zeta_offset == 0x3000,
          "NV4097 on its own subchannel sets the surface state");

    pkt(5, 0x018C, 0); pkt(5, 0x0194, 0); pkt(5, 0x0198, 0);   /* the census's NV308A words */
    pkt(5, 0x0180, 0x66604200u);                               /* NV308A_SET_CONTEXT_DMA_NOTIFIES */
    pkt(5, 0x0304, 0x00020003u);                               /* NV308A_POINT */
    pkt(3, 0x030C, 0x00005000u);                               /* NV3062_SET_OFFSET_DESTIN */
    const GcmMethodStats s2 = stats();
    walk(ctrl);
    const GcmMethodStats s3 = stats();
    rsx_dsp_get_surface(&g.rsx, &sf);
    check(sf.color_location[0] == RSX_LOCATION_MAIN && sf.color_location[1] == RSX_LOCATION_MAIN &&
          sf.zeta_location == RSX_LOCATION_MAIN,
          "NV308A words on 0x018C/0x0194/0x0198 leave the NV4097 surface locations alone");
    check(reg(0x018C) == DMA_MAIN && reg(0x0194) == DMA_MAIN && reg(0x0198) == DMA_MAIN && reg(0x0180) == 0x66604200u,
          "...and the NV4097 DMA context registers hold what NV4097 wrote");
    check(reg(0x0304) == 0, "an NV308A/NV3062 method is no NV4097 register");
    check(s_gcm2d.point == 0x00020003u && s_gcm2d.dst_offset == 0x00005000u,
          "the 2D classes still update their own state");
    check(s3.unknown == s2.unknown, "...and the census counts the NV308A words as classified");

    /* 3. A real NV4097 write to the same offsets still works. */
    pkt(0, 0x018C, DMA_LOCAL);
    pkt(0, 0x0198, DMA_LOCAL);
    pkt(0, 0x0210, 0x4000);
    walk(ctrl);
    rsx_dsp_get_surface(&g.rsx, &sf);
    check(sf.color_location[1] == RSX_LOCATION_LOCAL && sf.zeta_location == RSX_LOCATION_LOCAL &&
          sf.color_location[0] == RSX_LOCATION_MAIN && sf.color_offset[0] == 0x4000,
          "a real NV4097 write to the same registers takes effect");

    /* 4. One stream rebinds a slot: the same numeric method lands in the class bound at that point. */
    pkt(2, 0x0000, RSX_OBJ_NV3062);
    pkt(2, 0x0304, 0xAAAA0001u);                     /* NV3062_SET_PITCH */
    pkt(2, 0x0000, RSX_OBJ_NV4097);
    pkt(2, 0x0304, 0xBBBB0002u);                     /* an NV4097 register */
    pkt(2, 0x0000, RSX_OBJ_NV3062);
    pkt(2, 0x0304, 0xCCCC0003u);                     /* NV3062_SET_PITCH again */
    walk(ctrl);
    check(s_gcm2d.pitch == 0xCCCC0003u && reg(0x0304) == 0xBBBB0002u,
          "SET_OBJECT rebinds inside one stream: 0x0304 reaches NV3062, then NV4097, then NV3062");
    pkt(2, 0x0000, RSX_OBJ_NV4097);
    pkt(2, 0x0198, DMA_MAIN);                        /* NV4097 ZETA context on the rebound slot */
    walk(ctrl);
    rsx_dsp_get_surface(&g.rsx, &sf);
    check(sf.zeta_location == RSX_LOCATION_MAIN, "a slot bound to NV4097 other than subchannel 0 feeds the engine");
    pkt(2, 0x0000, RSX_OBJ_NV308A);
    pkt(2, 0x0198, DMA_LOCAL);
    walk(ctrl);
    rsx_dsp_get_surface(&g.rsx, &sf);
    check(sf.zeta_location == RSX_LOCATION_MAIN, "...and the same slot rebound to NV308A no longer does");

    /* NV0039 on its default slot: its 0x0314 is PITCH_IN there, an NV4097 register on slot 0. */
    pkt(1, 0x0314, 0x00001234u);
    walk(ctrl);
    check(s_m2mf.pitch_in == 0x1234u && reg(0x0314) != 0x1234u, "NV0039 words reach the M2MF and not the register file");

    /* The puller decodes NV406E words on every slot, whatever is bound: SET_REFERENCE on the NV0039 slot
     * takes effect as a fence and is not an unknown NV0039 method. */
    const GcmMethodStats p0 = stats();
    pkt(1, 0x0050, 0x5A5Au);
    walk(ctrl);
    check(g_rsx_last_reference == 0x5A5Au && stats().unknown == p0.unknown && t_legacy_n == 1,
          "NV406E SET_REFERENCE on the NV0039-bound slot reaches the puller, not the M2MF");

    /* 5. The unbound guard drops only methods that need an object. Slots 2 and 7 are unbound now. */
    gcm_bind_object(2, 0);
    gcm_bind_object(7, 0);
    const GcmMethodStats s4 = stats();
    const u32 pitch_before = s_gcm2d.pitch, r304 = reg(0x0304);
    const unsigned long long events = gcm_driver_event_count(GCM_DRV_EVENT_USER_COMMAND);
    pkt(2, 0x0304, 0x11112222u);                     /* needs an object: dropped */
    pkt(2, 0x0050, 0x77u);                           /* NV406E SET_REFERENCE: the puller decodes it */
    pkt(7, 0x0B04, 7u);                              /* the driver's user-command word (0xEB04) */
    walk(ctrl);
    const GcmMethodStats s5 = stats();
    check(s5.unbound - s4.unbound == 1, "an unbound slot drops exactly the method that needs an object");
    check(s_gcm2d.pitch == pitch_before && reg(0x0304) == r304,
          "...and the dropped word reached neither the 2D state nor the register file");
    check(t_legacy_n == 1 && t_legacy_seen[0] == 0x0050u, "...the puller's SET_REFERENCE is not dropped");
    check(gcm_driver_event_count(GCM_DRV_EVENT_USER_COMMAND) == events + 1u && reg(0xEB04) == 7u,
          "...nor the driver's user-command word, which still reaches the mirror");
    pkt(2, 0x0000, RSX_OBJ_NV3062);                  /* the bind itself is not dropped */
    pkt(2, 0x0304, 0x33334444u);
    walk(ctrl);
    check(s_gcm2d.pitch == 0x33334444u && stats().unbound == s5.unbound,
          "...the bind takes effect, and the slot decodes afterwards");

    /* 6. Slot 7 is the driver's: its full-address words (0xEB00/0xEB04, 0xE944, 0xE920/0xE924) are decoded
     * before any class route, whatever object the slot carries, and any other word on it means what its
     * bound class says, at its own method number. */
    /* NV4097 bound on slot 7: BEGIN_END reaches the engine as 0x1808, not as 0xF808. */
    gcm_bind_object(7, RSX_OBJ_NV4097);
    pkt(7, 0x1808, RSX_PRIMITIVE_TRIANGLES);
    walk(ctrl);
    check(g.rsx.in_begin_end && reg(0x1808) == RSX_PRIMITIVE_TRIANGLES,
          "NV4097 bound on slot 7: BEGIN_END reaches the engine's begin/end, not 0xF808");
    pkt(7, 0x1808, 0u);
    walk(ctrl);
    check(!g.rsx.in_begin_end, "...and END closes it");
    /* ...and the driver's flip queue word on that same slot still takes the driver's route. */
    const unsigned long long q0 = gcm_driver_event_count(GCM_DRV_EVENT_QUEUE_FLIP);
    pkt(7, 0x0944, 5u);
    walk(ctrl);
    check(gcm_driver_event_count(GCM_DRV_EVENT_QUEUE_FLIP) == q0 + 1u && s_drv_flip_queue == 5u,
          "NV4097 bound on slot 7, per-word path: 0xE944 is the driver's flip queue word");
    /* NV0039 bound on slot 7: the driver's words are not the M2MF's. */
    gcm_bind_object(7, RSX_OBJ_NV0039);
    const GcmMethodStats n0 = stats();
    const unsigned long long h0 = gcm_driver_event_count(GCM_DRV_EVENT_FLIP_HEAD);
    pkt(7, 0x0944, 2u);                              /* 0xE944: flip queue */
    pkt(7, 0x0924, 0x8000010Fu);                     /* 0xE924 with bit 31: flip head */
    walk(ctrl);
    check(s_drv_flip_queue == 2u && gcm_driver_event_count(GCM_DRV_EVENT_FLIP_HEAD) == h0 + 1u &&
          stats().unknown == n0.unknown,
          "NV0039 bound on slot 7: 0xE944 and 0xE924 are the driver's, not unknown M2MF methods");
    gcm_bind_object(7, RSX_OBJ_DRIVER);

    printf("[gcm_class_routing] %s (%d failed)\n", t_fails ? "FAIL" : "PASS", t_fails);
    rsx_draw_engine_shutdown();
    free(vm_base);
    return t_fails ? 1 : 0;
}

/*
 * The walker's method classification, against the census it has to explain (issue #47).
 *
 * A synthetic FIFO carries one write for every (class, method) pair the 120 s census measured on the
 * title (214 pairs, h1b-census). The test drives the real cellGcm_rsx_process_fifo over it and asserts
 * that the pairs land in the classes the table in rsx_method_class.h claims, that the table covers all
 * of them, and that a pair with no row stops with the four fields an operator needs to find it again.
 *
 * The pairs are hardware method numbers (class, offset, observed first data word): synthetic input, no
 * title bytes. The method ids and counts are the census's, so a regression in the table is measurable.
 *
 *   cc -std=gnu17 ... (CMake target test_rsx_method_census, labels rsx;t0)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../cellGcmSys.c"
#include "ps3emu/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

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

/* The vtable 3D state machine is off in this fixture: every method it is asked about is unconsumed, so
 * the verdict comes from the table alone (plus the walker's own consumers). That is the configuration
 * the census was taken in when the register-file engine classified the method itself. */
int rsx_process_method(rsx_state* state, u32 method, u32 data)
{ (void)state; (void)method; (void)data; return -1; }
/* 0 = no batched fast path: every word reaches rsx_process_method above. */
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
rsx_backend* rsx_get_backend(void) { return NULL; }
int  rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
u32  rsx_dsp_method_class(u32 method);   /* the real one, from rsx_dispatch.c */
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
#define LOCAL_EA  0x10000000u

typedef struct { rsx_class_id cls; u32 method; unsigned n; int sub; u32 data; } census_pair;

/* The 214 (class, method) pairs of scratch/h1b-census/run3, in the census's order. */
static const census_pair k_census[] = {
    { RSX_CLASS_UNBOUND, 0x0110u, 5u, 0, 0x00000000u },
    { RSX_CLASS_UNBOUND, 0x02B8u, 3u, 0, 0x00000000u },
    { RSX_CLASS_UNBOUND, 0x0300u, 1u, 0, 0x00000001u },
    { RSX_CLASS_UNBOUND, 0x0348u, 1u, 0, 0x00000000u },
    { RSX_CLASS_UNBOUND, 0x0368u, 1u, 0, 0x00001D01u },
    { RSX_CLASS_UNBOUND, 0x036Cu, 1u, 0, 0x00000000u },
    { RSX_CLASS_UNBOUND, 0x0374u, 1u, 0, 0x00000000u },
    { RSX_CLASS_UNBOUND, 0x0380u, 1u, 0, 0x00000000u },
    { RSX_CLASS_UNBOUND, 0x0394u, 86u, 0, 0x00000000u },
    { RSX_CLASS_UNBOUND, 0x0398u, 86u, 0, 0x3F800000u },
    { RSX_CLASS_UNBOUND, 0x0A68u, 1u, 0, 0x00000000u },
    { RSX_CLASS_UNBOUND, 0x0B40u, 1u, 0, 0x00000001u },
    { RSX_CLASS_UNBOUND, 0x1710u, 1u, 0, 0x00000000u },
    { RSX_CLASS_UNBOUND, 0x1D88u, 3u, 0, 0x000012D0u },
    { RSX_CLASS_UNBOUND, 0x1EE4u, 1u, 0, 0x00000000u },
    { RSX_CLASS_UNBOUND, 0x1EE8u, 1u, 0, 0x00000000u },
    { RSX_CLASS_UNBOUND, 0x1EF8u, 1u, 0, 0x0020FFFFu },
    { RSX_CLASS_UNBOUND, 0x1FECu, 1u, 0, 0x00000001u },
    { RSX_CLASS_NV406E, 0x0060u, 17446u, 0, 0x56616661u },
    { RSX_CLASS_NV4097, 0x0110u, 56318u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0180u, 8723u, 0, 0x66604200u },
    { RSX_CLASS_NV4097, 0x0184u, 8723u, 0, 0xFEED0000u },
    { RSX_CLASS_NV4097, 0x0188u, 8723u, 0, 0xFEED0001u },
    { RSX_CLASS_NV4097, 0x0190u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x019Cu, 8723u, 0, 0xFEED0000u },
    { RSX_CLASS_NV4097, 0x01A0u, 8723u, 0, 0xFEED0001u },
    { RSX_CLASS_NV4097, 0x01A4u, 8723u, 0, 0x66606660u },
    { RSX_CLASS_NV4097, 0x01A8u, 8723u, 0, 0x66626660u },
    { RSX_CLASS_NV4097, 0x01ACu, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x01B0u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0230u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0238u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0240u, 8723u, 0, 0x0000FFFFu },
    { RSX_CLASS_NV4097, 0x0244u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0248u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x024Cu, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x02B8u, 44037u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x02BCu, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x02C0u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02C4u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02C8u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02CCu, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02D0u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02D4u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02D8u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02DCu, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02E0u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02E4u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02E8u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02ECu, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02F0u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02F4u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02F8u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x02FCu, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x0300u, 17445u, 0, 0x00000001u },
    { RSX_CLASS_NV4097, 0x032Cu, 8723u, 0, 0x000000FFu },
    { RSX_CLASS_NV4097, 0x0348u, 17445u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x034Cu, 8723u, 0, 0x000000FFu },
    { RSX_CLASS_NV4097, 0x0350u, 8723u, 0, 0x00000207u },
    { RSX_CLASS_NV4097, 0x0354u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0358u, 8723u, 0, 0x000000FFu },
    { RSX_CLASS_NV4097, 0x035Cu, 8723u, 0, 0x00001E00u },
    { RSX_CLASS_NV4097, 0x0360u, 8723u, 0, 0x00001E00u },
    { RSX_CLASS_NV4097, 0x0364u, 8723u, 0, 0x00001E00u },
    { RSX_CLASS_NV4097, 0x0368u, 17445u, 0, 0x00001D01u },
    { RSX_CLASS_NV4097, 0x036Cu, 17445u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0374u, 17445u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0378u, 8723u, 0, 0x00001503u },
    { RSX_CLASS_NV4097, 0x037Cu, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0380u, 17445u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0384u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0388u, 8723u, 0, 0x3F800000u },
    { RSX_CLASS_NV4097, 0x0394u, 759239u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0398u, 759239u, 0, 0x3F800000u },
    { RSX_CLASS_NV4097, 0x03B0u, 8723u, 0, 0x00100000u },
    { RSX_CLASS_NV4097, 0x03B8u, 8723u, 0, 0x00000008u },
    { RSX_CLASS_NV4097, 0x03BCu, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x03C0u, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03C4u, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03C8u, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03CCu, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03D0u, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03D4u, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03D8u, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03DCu, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03E0u, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03E4u, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03E8u, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03ECu, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03F0u, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03F4u, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03F8u, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x03FCu, 8723u, 0, 0x00010101u },
    { RSX_CLASS_NV4097, 0x0400u, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x0404u, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x0408u, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x040Cu, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x0410u, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x0414u, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x0418u, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x041Cu, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x0420u, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x0424u, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x0428u, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x042Cu, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x0430u, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x0434u, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x0438u, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x043Cu, 8723u, 0, 0x00007421u },
    { RSX_CLASS_NV4097, 0x0440u, 8723u, 0, 0x9AABAA98u },
    { RSX_CLASS_NV4097, 0x0444u, 8723u, 0, 0x66666789u },
    { RSX_CLASS_NV4097, 0x0448u, 8723u, 0, 0x98766666u },
    { RSX_CLASS_NV4097, 0x044Cu, 8723u, 0, 0x89AABAA9u },
    { RSX_CLASS_NV4097, 0x0450u, 8723u, 0, 0x99999999u },
    { RSX_CLASS_NV4097, 0x0454u, 8723u, 0, 0x88888889u },
    { RSX_CLASS_NV4097, 0x0458u, 8723u, 0, 0x98888888u },
    { RSX_CLASS_NV4097, 0x045Cu, 8723u, 0, 0x99999999u },
    { RSX_CLASS_NV4097, 0x0460u, 8723u, 0, 0x56676654u },
    { RSX_CLASS_NV4097, 0x0464u, 8723u, 0, 0x33333345u },
    { RSX_CLASS_NV4097, 0x0468u, 8723u, 0, 0x54333333u },
    { RSX_CLASS_NV4097, 0x046Cu, 8723u, 0, 0x45667665u },
    { RSX_CLASS_NV4097, 0x0470u, 8723u, 0, 0xAABBBA99u },
    { RSX_CLASS_NV4097, 0x0474u, 8723u, 0, 0x66667899u },
    { RSX_CLASS_NV4097, 0x0478u, 8723u, 0, 0x99876666u },
    { RSX_CLASS_NV4097, 0x047Cu, 8723u, 0, 0x99ABBBAAu },
    { RSX_CLASS_NV4097, 0x08CCu, 8723u, 0, 0x00000800u },
    { RSX_CLASS_NV4097, 0x08D0u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x08D4u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x08D8u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0A0Cu, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0A60u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0A64u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0A68u, 26168u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0A78u, 17446u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0A7Cu, 17446u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0B00u, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B04u, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B08u, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B0Cu, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B10u, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B14u, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B18u, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B1Cu, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B20u, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B24u, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B28u, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B2Cu, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B30u, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B34u, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B38u, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B3Cu, 8723u, 0, 0x00002DC8u },
    { RSX_CLASS_NV4097, 0x0B40u, 553105u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0B44u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0B48u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0B4Cu, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0B50u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0B54u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0B58u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0B5Cu, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0B60u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x0B64u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x1428u, 8723u, 0, 0x00000001u },
    { RSX_CLASS_NV4097, 0x142Cu, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x1450u, 8723u, 0, 0x00080003u },
    { RSX_CLASS_NV4097, 0x1454u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x145Cu, 8723u, 0, 0x00000001u },
    { RSX_CLASS_NV4097, 0x1478u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x147Cu, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x1710u, 37733u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x17CCu, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x1828u, 8723u, 0, 0x00001B02u },
    { RSX_CLASS_NV4097, 0x182Cu, 8723u, 0, 0x00001B02u },
    { RSX_CLASS_NV4097, 0x1838u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x1D64u, 8723u, 0, 0x02000000u },
    { RSX_CLASS_NV4097, 0x1D78u, 8723u, 0, 0x00000001u },
    { RSX_CLASS_NV4097, 0x1D7Cu, 8723u, 0, 0xFFFF0000u },
    { RSX_CLASS_NV4097, 0x1D80u, 8723u, 0, 0x00000003u },
    { RSX_CLASS_NV4097, 0x1D88u, 44037u, 0, 0x00001000u },
    { RSX_CLASS_NV4097, 0x1D98u, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x1D9Cu, 8723u, 0, 0x0FFF0000u },
    { RSX_CLASS_NV4097, 0x1DA4u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x1DB4u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x1E94u, 8723u, 0, 0x00000011u },
    { RSX_CLASS_NV4097, 0x1E98u, 8723u, 0, 0x01000000u },
    { RSX_CLASS_NV4097, 0x1EE0u, 8723u, 0, 0x3F800000u },
    { RSX_CLASS_NV4097, 0x1EE4u, 17445u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x1EE8u, 17445u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x1EF8u, 621337u, 0, 0x0020FFFFu },
    { RSX_CLASS_NV4097, 0x1FC4u, 8723u, 0, 0x06144321u },
    { RSX_CLASS_NV4097, 0x1FC8u, 8723u, 0, 0xEDCBA987u },
    { RSX_CLASS_NV4097, 0x1FCCu, 8723u, 0, 0x0000006Fu },
    { RSX_CLASS_NV4097, 0x1FD0u, 8723u, 0, 0x00171615u },
    { RSX_CLASS_NV4097, 0x1FD4u, 8723u, 0, 0x001B1A19u },
    { RSX_CLASS_NV4097, 0x1FE0u, 8723u, 0, 0x00000001u },
    { RSX_CLASS_NV4097, 0x1FE8u, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV4097, 0x1FECu, 8723u, 0, 0x00000000u },
    { RSX_CLASS_NV3062, 0x0000u, 8723u, 3, 0x313371C3u },
    { RSX_CLASS_NV3062, 0x0180u, 8723u, 3, 0x66604200u },
    { RSX_CLASS_NV308A, 0x0000u, 8723u, 5, 0x31337808u },
    { RSX_CLASS_NV308A, 0x0180u, 8723u, 5, 0x66604200u },
    { RSX_CLASS_NV308A, 0x0184u, 8723u, 5, 0x00000000u },
    { RSX_CLASS_NV308A, 0x0188u, 8723u, 5, 0x00000000u },
    { RSX_CLASS_NV308A, 0x018Cu, 8723u, 5, 0x00000000u },
    { RSX_CLASS_NV308A, 0x0190u, 8723u, 5, 0x00000000u },
    { RSX_CLASS_NV308A, 0x0194u, 8723u, 5, 0x00000000u },
    { RSX_CLASS_NV308A, 0x0198u, 8723u, 5, 0x00000000u },
    { RSX_CLASS_NV308A, 0x019Cu, 8723u, 5, 0x313371C3u },
    { RSX_CLASS_NV308A, 0x02FCu, 8723u, 5, 0x00000003u },
    { RSX_CLASS_NV309E, 0x0000u, 8723u, 4, 0x31337A73u },
    { RSX_CLASS_NV309E, 0x0180u, 8723u, 4, 0x66604200u },
    { RSX_CLASS_NV309E, 0x0184u, 8723u, 4, 0xFEED0000u },
    { RSX_CLASS_NV3089, 0x0000u, 8723u, 6, 0x3137AF00u },
    { RSX_CLASS_NV3089, 0x0180u, 8723u, 6, 0x66604200u },
    { RSX_CLASS_DRIVER, 0x0000u, 8723u, 7, 0xCAFEBABEu },
};

static u32 g_heap = 0x00800000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void host_w32(u32 ea, u32 v) { vm_write32(ea, v); }

static u32 g_at = 0x1000;
static u32 hdr(u32 count, u32 subch, u32 method) { return (count << 18) | (subch << 13) | method; }
static void put1(u32 subch, u32 method, u32 v)
{
    vm_write32(IO_ADDR + g_at, hdr(1, subch, method));
    vm_write32(IO_ADDR + g_at + 4, v);
    g_at += 8;
}
static void run_fifo(void)
{
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, g_at);
    cellGcm_rsx_process_fifo();
}

static int g_fail;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    g_fail += !ok;
}

static void setup(void)
{
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)mmap(NULL, vm_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (vm_base == MAP_FAILED) { puts("FAIL: guest VM"); exit(1); }
    ppu_vm_size = vm_size;
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    s_config.localAddress = LOCAL_EA;
}

/* One pass over the whole census. The 18 pairs the census saw "unbound" arrived before the first observed
 * SET_OBJECT on a channel the console has bound from init (cellGcmInit binds the seven objects the driver's
 * default-state subroutine names); cellGcmInit now models that, so they are fed on their own subchannel like
 * every other pair, and the table classifies them as what they are: NV4097 state. The binds below keep the
 * fixture independent of that model. */
static void feed_census(void)
{
    put1(0, 0, RSX_OBJ_NV4097);
    put1(1, 0, RSX_OBJ_NV0039);
    put1(3, 0, RSX_OBJ_NV3062);
    put1(4, 0, RSX_OBJ_NV309E);
    put1(5, 0, RSX_OBJ_NV308A);
    put1(6, 0, RSX_OBJ_NV3089);
    put1(7, 0, RSX_OBJ_DRIVER);
    for (unsigned i = 0; i < sizeof k_census / sizeof k_census[0]; i++)
        put1(k_census[i].sub, k_census[i].method, k_census[i].data);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setup();

    /* The table covers every pair the census saw, except the unbound ones (dropped before a class
     * exists) which are checked below through the walker's unbound counter. */
    { unsigned missing_row = 0, checked = 0;
      for (unsigned i = 0; i < sizeof k_census / sizeof k_census[0]; i++) {
          checked++;
          /* An "unbound" census row is an NV4097 method seen before the first observed bind: its class
           * is the one the driver's default state binds to subchannel 0. */
          const rsx_class_id c = (k_census[i].method && k_census[i].method < 0x100u)
                                     ? RSX_CLASS_NV406E
                                     : (k_census[i].cls == RSX_CLASS_UNBOUND ? RSX_CLASS_NV4097
                                                                             : k_census[i].cls);
          if (!rsx_method_rule_find(c, k_census[i].method)) {
              if (missing_row++ < 8)
                  printf("  no row: %s 0x%04X\n", rsx_class_name(c), k_census[i].method);
          }
      }
    check(missing_row == 0, "the table has a row for every census pair (none unclassified by omission)");
      check(checked == 214, "all 214 pairs need a row: the 18 seen 'unbound' are NV4097 state"); }

    gcm_method_stats_reset();
    feed_census();
    run_fifo();

    GcmMethodStats st;
    gcm_method_stats(&st);
    printf("[census] handled=%llu inert=%llu missing=%llu unknown=%llu unbound=%llu\n",
           st.handled, st.inert, st.missing, st.unknown, st.unbound);
    check(st.unknown == 0, "no census pair is unknown: the frame is valid and the live stop does not fire");
    check(st.unbound == 0, "nothing is dropped: every subchannel the census used is bound");
    /* 22 census pairs are consumed by a named consumer, plus the seven SET_OBJECT writes that bind the
     * slots (each is a method the walker consumes). */
    check(st.handled == 34, "34 writes are consumed by a named consumer");
    check(st.inert == 180, "180 writes are allow-listed for the value they carry");
    check(st.missing == 7, "7 writes are known gaps (5 distinct pairs), counted but not a stop");
    check(st.handled + st.inert + st.missing == 221, "...and the three classes cover every write (214 pairs + 7 binds)");

    { GcmUnknownMethod u[8];
      check(gcm_unknown_methods(u, 8) == 0, "the unknown table is empty");
      GcmUnknownMethod m[8];
      const u32 n = gcm_missing_methods(m, 8);
      /* The five known gaps the census carries: the window offset, the three back-face stencil
       * registers, and the shader window. Each has an S2 row (docs/RSX_GRAPHICS.md). */
      int seen[5] = { 0, 0, 0, 0, 0 };
      static const u32 want[5] = { 0x02B8u, 0x034Cu, 0x0354u, 0x0358u, 0x1D88u };
      for (u32 i = 0; i < n && i < 8; i++) {
          printf("[census]   missing %s 0x%04X x%llu data 0x%08X\n",
                 gcm_method_class_name(m[i].cls), m[i].method, m[i].count, m[i].data);
          for (unsigned k = 0; k < 5; k++)
              if (m[i].cls == RSX_CLASS_NV4097 && m[i].method == want[k]) seen[k] = 1;
      }
      check(n == 5 && seen[0] && seen[1] && seen[2] && seen[3] && seen[4],
            "exactly the five S2 gaps are reported missing, per pair"); }

    /* Fail-before for the stop: a pair with no row ends the walker with the four fields that locate it,
     * in a child so this process can continue. */
    fflush(stdout);
    { int fds[2];
      if (pipe(fds)) { puts("FAIL: pipe"); return 1; }
      const pid_t pid = fork();
      if (pid == 0) {
          close(fds[0]);
          dup2(fds[1], 2);
          put1(0, 0x1F80u, 0xDEADBEEFu);      /* NV4097, outside every row */
          run_fifo();
          _exit(0);                            /* reaching here means it did not stop */
      }
      close(fds[1]);
      char buf[8192];
      size_t n = 0;
      for (ssize_t r; n < sizeof buf - 1 && (r = read(fds[0], buf + n, sizeof buf - 1 - n)) > 0;) n += (size_t)r;
      buf[n] = 0;
      close(fds[0]);
      int status = 0;
      waitpid(pid, &status, 0);
      char want_method[64], want_fifo[64];
      snprintf(want_method, sizeof want_method, "method=0x1F80");
      /* the child's FIFO cursor is its own copy: the packet it appends starts at the parent's g_at */
      snprintf(want_fifo, sizeof want_fifo, "fifo=0x%08X", g_at);
      const int stopped = WIFEXITED(status) && WEXITSTATUS(status) == 70;
      const int named = strstr(buf, "kind=rsx-unknown-method") != NULL;
      const int fields = strstr(buf, "class=NV4097") && strstr(buf, want_method) &&
                         strstr(buf, "subchannel=0") && strstr(buf, want_fifo) &&
                         strstr(buf, "word=0x") && strstr(buf, "data=0xDEADBEEF");
      char what[256];
      snprintf(what, sizeof what,
               "an unclassified pair (NV4097 0x1F80) is a named stop with class, method, subchannel, fifo (%s) and data",
               want_fifo);
      check(stopped && named && fields, what);
      if (!(stopped && named && fields))
          printf("  status=%d stopped=%d named=%d fields=%d\n  child stderr: %.600s\n",
                 status, stopped, named, fields, buf);
    }

    printf("[rsx_method_census] %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}

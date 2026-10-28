/* NV308A image-from-CPU (the inline transfer) writing the NV3062 destination (issue #81).
 *
 * Runs the real cellGcm_rsx_process_fifo over synthetic rings and reads the destination bytes in guest
 * memory. The reference is RPCS3 d00d8023 Emu/RSX/NV47/HW/nv308a.cpp (`color::impl`) and rsx_methods.cpp:1723;
 * what the reference does not decide is a named stop, and a stop carries the transfer state.
 *
 *   run     every transfer scenario, the NV308A / NV309E method-routing guard, the verdicts and the guest
 *           visibility of the store (reservation kill, ordering against a later NV406E label release) and the
 *           FIFO window a stop writes (gcm_stop_fifo_dump)
 *   stops   each way a transfer must end in a named stop, in a forked child that shares guest memory, so
 *           the parent can see that not one destination byte moved and that the stop line names the state
 *
 * cc -std=gnu17 ... (CMake target test_gcm_nv308a)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../cellGcmSys.c"
#include "ps3emu/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
/* The SPU coherency hooks gcm_row_store calls, observed: a reservation on one 128-byte line, how often a
 * store killed it, and what the NV406E label at g_label_ea held at that moment. */
static uint32_t g_resv_line = 0xFFFFFFFFu, g_label_ea;
static unsigned g_killed;
static uint32_t g_label_at_kill = 0xFFFFFFFFu;
int spu_coh_is_reserved(uint32_t a) { return a == g_resv_line; }
void spu_coh_notify_write(uint32_t a)
{
    if (a != g_resv_line) return;
    g_killed++;
    if (g_label_ea) g_label_at_kill = vm_read32(g_label_ea);
}
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

int rsx_process_method(rsx_state* state, u32 method, u32 data)
{ (void)state; (void)method; (void)data; return -1; }
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
rsx_backend* rsx_get_backend(void) { return NULL; }
int  rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
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
#define DST_IO    0x00280000u
#define DST_EA    (IO_ADDR + DST_IO)
#define LOCAL_EA  0x10000000u
#define MAIN      0xFEED0001u
#define LOCAL     0xFEED0000u

#define FMT_R5G6B5   4u
#define FMT_A8R8G8B8 10u
#define FMT_Y32      11u

static int g_fail, g_show;   /* g_show: `stops show` prints each child's stop output */
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    g_fail += !ok;
}

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

static u32 g_at = 0x1000;   /* the ring cursor: an IO offset */
static u32 hdr(u32 count, u32 subch, u32 method) { return (count << 18) | (subch << 13) | method; }
static void put1(u32 subch, u32 method, u32 v)
{
    vm_write32(IO_ADDR + g_at, hdr(1, subch, method));
    vm_write32(IO_ADDR + g_at + 4, v);
    g_at += 8;
}
/* A packet of `n` words: incrementing (method + 4 * i), or non-incrementing (one method repeated). */
static void putn(u32 subch, u32 method, const u32* v, u32 n, int non_inc)
{
    vm_write32(IO_ADDR + g_at, (non_inc ? 0x40000000u : 0u) | hdr(n, subch, method));
    for (u32 i = 0; i < n; i++) vm_write32(IO_ADDR + g_at + 4 + i * 4, v[i]);
    g_at += 4 + n * 4;
}
static void run(void)
{
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, g_at);
    cellGcm_rsx_process_fifo();
}

static u8* mem(u32 ea) { return vm_base + ea; }
static int all(const u8* p, u32 n, u8 v) { for (u32 i = 0; i < n; i++) if (p[i] != v) return 0; return 1; }
static int be32_at(const u8* p, u32 v)
{ return p[0] == (u8)(v >> 24) && p[1] == (u8)(v >> 16) && p[2] == (u8)(v >> 8) && p[3] == (u8)v; }

/* The libgcm slot table, then the state a transfer needs: NV3062 on 3, NV309E on 4, NV308A on 5. */
static void setup(void)
{
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)mmap(NULL, vm_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (vm_base == MAP_FAILED) { puts("FAIL: guest VM"); exit(1); }
    ppu_vm_size = vm_size;
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    s_config.localAddress = LOCAL_EA;    /* inside the fixture's guest memory */
    put1(0, 0, RSX_OBJ_NV4097);
    put1(3, 0, RSX_OBJ_NV3062);
    put1(4, 0, RSX_OBJ_NV309E);
    put1(5, 0, RSX_OBJ_NV308A);
    run();
}

/* NV3062 destination: DMA, COLOR_FORMAT, PITCH (the destination pitch is bits 16-31), OFFSET_DESTIN. */
static void dest(u32 dma, u32 fmt, u32 pitch, u32 off)
{
    put1(3, 0x188, dma);
    put1(3, 0x300, fmt);
    put1(3, 0x304, (pitch << 16) | 0x40u);
    put1(3, 0x30C, off);
}
/* The default state's operation and surface context, then the packet geometry. */
static void geometry(u32 px, u32 py, u32 ow, u32 oh, u32 iw, u32 ih)
{
    put1(5, 0x2FC, 3);
    put1(5, 0x19C, RSX_OBJ_NV3062);
    put1(5, 0x304, (py << 16) | px);
    put1(5, 0x308, (oh << 16) | ow);
    put1(5, 0x30C, (ih << 16) | iw);
}

static void words(u32 n, u32 seed, u32* out) { for (u32 i = 0; i < n; i++) out[i] = (seed + i) * 0x01010101u ^ 0x00A5F00Fu; }

/* ---- transfers ------------------------------------------------------------------------------ */

static void one_by_one(void)
{
    /* The stop the issue reports: SIZE_IN = 1x1 (0x00010001), one COLOR word. */
    memset(mem(DST_EA), 0xEE, 64);
    dest(MAIN, FMT_A8R8G8B8, 0x40, DST_IO);
    geometry(0, 0, 1, 1, 1, 1);
    put1(5, 0x400, 0x11223344u);
    run();
    check(be32_at(mem(DST_EA), 0x11223344u), "1x1 into main memory, A8R8G8B8: the destination holds the word");
    check(all(mem(DST_EA) + 4, 60, 0xEE), "...and not one byte past it moved");
}

static void format_y32_local(void)
{
    /* Y32 stores the word verbatim too; local destination; POINT (3,2) with a 0x80-byte pitch: x * 4 + y * pitch.
     * The source DMA (0x184) is not the destination: written after 0x188, it must not move it. */
    memset(mem(LOCAL_EA + 0x1000), 0xEE, 0x300);
    put1(3, 0x184, MAIN);
    dest(LOCAL, FMT_Y32, 0x80, 0x1000);
    put1(3, 0x184, MAIN);
    geometry(3, 2, 2, 1, 2, 1);
    const u32 w[2] = { 0xCAFEBABEu, 0x0BADF00Du };
    putn(5, 0x400, w, 2, 0);
    run();
    const u8* at = mem(LOCAL_EA + 0x1000 + 2 * 0x80 + 3 * 4);
    check(be32_at(at, w[0]) && be32_at(at + 4, w[1]), "Y32 into local memory at POINT (3,2), pitch 0x80: x * 4 + y * pitch");
    check(all(mem(LOCAL_EA + 0x1000), 2 * 0x80 + 12, 0xEE) && all(at + 8, 0x300 - (2 * 0x80 + 20), 0xEE),
          "...the destination is the NV3062 IMAGE_DESTIN (a later IMAGE_SOURCE write moves nothing), neighbours untouched");
}

static void long_rows(void)
{
    /* 300 words in one packet (the old window held 256), then the whole 0x700-word array. */
    static u32 w[0x700];
    words(0x700, 1, w);
    memset(mem(DST_EA), 0xEE, 0x700 * 4 + 64);
    dest(MAIN, FMT_A8R8G8B8, 0x40, DST_IO);
    geometry(0, 0, 300, 1, 300, 1);
    putn(5, 0x400, w, 300, 0);
    run();
    int ok = 1;
    for (u32 i = 0; i < 300; i++) ok &= be32_at(mem(DST_EA) + i * 4, w[i]);
    check(ok, "a 300-word row (more than 256) lands word for word");
    check(all(mem(DST_EA) + 300 * 4, 64, 0xEE), "...and nothing lands past it");

    memset(mem(DST_EA), 0xEE, 0x700 * 4 + 64);
    geometry(0, 0, 0x700, 1, 0x700, 1);
    putn(5, 0x400, w, 0x700, 0);
    run();
    ok = 1;
    for (u32 i = 0; i < 0x700; i++) ok &= be32_at(mem(DST_EA) + i * 4, w[i]);
    check(ok && all(mem(DST_EA) + 0x700 * 4, 64, 0xEE), "a full 0x700-word packet (COLOR 0x400..0x1FFC) lands, the last word at index 0x6FF");
}

static void clipping(void)
{
    /* SIZE_OUT.x bounds the words: the rest of an 8-word packet is dropped (nv308a.cpp: index >= out_x_max). */
    static u32 w[8];
    words(8, 40, w);
    memset(mem(DST_EA), 0xEE, 64);
    dest(MAIN, FMT_A8R8G8B8, 0x40, DST_IO);
    geometry(0, 0, 5, 1, 5, 1);
    putn(5, 0x400, w, 8, 0);
    run();
    int ok = 1;
    for (u32 i = 0; i < 5; i++) ok &= be32_at(mem(DST_EA) + i * 4, w[i]);
    check(ok && all(mem(DST_EA) + 20, 44, 0xEE), "SIZE_OUT.x = 5 of an 8-word packet: five words land, three are dropped");

    /* POINT.x shifts the start and the bound still counts from the packet's first word. */
    memset(mem(DST_EA), 0xEE, 64);
    geometry(2, 0, 3, 1, 3, 1);
    putn(5, 0x400, w, 8, 0);
    run();
    ok = all(mem(DST_EA), 8, 0xEE) && be32_at(mem(DST_EA) + 8, w[0]) && be32_at(mem(DST_EA) + 12, w[1]) &&
         be32_at(mem(DST_EA) + 16, w[2]);
    check(ok && all(mem(DST_EA) + 20, 44, 0xEE), "POINT.x = 2, SIZE_OUT.x = 3: three words from x = 2, the bound counts words, not columns");

    /* A packet that starts past the bound writes nothing: the word at COLOR index 3 of SIZE_OUT.x = 3. */
    memset(mem(DST_EA), 0xEE, 64);
    geometry(0, 0, 3, 1, 3, 1);
    put1(5, 0x400 + 3 * 4, 0x55555555u);
    run();
    check(all(mem(DST_EA), 64, 0xEE), "a single word at index 3 of SIZE_OUT.x = 3 is dropped");

    /* SIZE_OUT.x = 0 (the reset value) bounds everything out. */
    geometry(0, 0, 0, 0, 0, 0);
    put1(5, 0x400, 0x66666666u);
    run();
    check(all(mem(DST_EA), 64, 0xEE), "SIZE_OUT 0x0: every word is dropped");
}

static void non_incrementing(void)
{
    static const u32 w[3] = { 0x01010101u, 0x02020202u, 0x03030303u };
    memset(mem(DST_EA), 0xEE, 64);
    dest(MAIN, FMT_A8R8G8B8, 0x40, DST_IO);
    geometry(0, 0, 4, 1, 4, 1);
    putn(5, 0x400 + 8, w, 3, 1);     /* one method (index 2) repeated: the last word stays */
    run();
    check(be32_at(mem(DST_EA) + 8, w[2]) && all(mem(DST_EA), 8, 0xEE) && all(mem(DST_EA) + 12, 52, 0xEE),
          "a non-incrementing packet writes every word to one index: the last stays");
}

static void sixteen_bit(void)
{
    /* R5G6B5: 2 bytes a pixel, big-endian, channels truncated (r >> 3, g >> 2, b >> 3). */
    static const u32 w[4] = { 0x00FF8040u, 0x00000000u, 0x00FFFFFFu, 0x00080408u };
    memset(mem(DST_EA), 0xEE, 64);
    dest(MAIN, FMT_R5G6B5, 0x40, DST_IO);
    geometry(1, 0, 4, 1, 4, 1);
    putn(5, 0x400, w, 4, 0);
    run();
    const u8* d = mem(DST_EA) + 2;       /* x = 1, 2 bytes a pixel */
    check(d[0] == 0xFC && d[1] == 0x08, "R5G6B5: 0x00FF8040 stores 0xFC08 (31, 32, 8), high byte first");
    check(d[2] == 0x00 && d[3] == 0x00, "...black stays 0x0000");
    check(d[4] == 0xFF && d[5] == 0xFF, "...white is 0xFFFF (RPCS3's (255 * 32) / 255 = 32 would overflow the red field)");
    check(d[6] == 0x08 && d[7] == 0x21, "...0x00080408 stores 0x0821 (1, 1, 1)");
    check(all(mem(DST_EA), 2, 0xEE) && all(mem(DST_EA) + 10, 54, 0xEE), "...and the bytes either side are untouched");
}

static void main_memory_pitch(void)
{
    /* Main-memory destination, second row by POINT.y with a pitch that is not the row length. */
    memset(mem(DST_EA), 0xEE, 0x400);
    dest(MAIN, FMT_Y32, 0x100, DST_IO + 0x10);
    geometry(1, 3, 1, 1, 1, 1);
    put1(5, 0x400, 0xDEADBEEFu);
    run();
    check(be32_at(mem(DST_EA) + 0x10 + 3 * 0x100 + 4, 0xDEADBEEFu), "main memory: OFFSET_DESTIN + x * 4 + POINT.y * pitch");
    check(all(mem(DST_EA), 0x10 + 3 * 0x100 + 4, 0xEE) && all(mem(DST_EA) + 0x10 + 3 * 0x100 + 8, 0x400 - (0x10 + 3 * 0x100 + 8), 0xEE),
          "...and every other byte of the surface is untouched");
}

/* ---- routing --------------------------------------------------------------------------------- */

/* NV308A 0x0300 is SET_COLOR_FORMAT and 0x0304 is POINT; NV309E's are SET_FORMAT and SET_OFFSET. The class
 * bound on the slot decides (ddf76ac4 stopped one reading the other), never the method number. */
static void routing_guard(void)
{
    s_nv309e.active = 0; s_nv309e.fmt = 0; s_nv309e.offset = 0;
    put1(5, 0x300, 4);                  /* NV308A COLOR_FORMAT, as the default state writes it */
    put1(5, 0x304, 0x00050007u);        /* NV308A POINT */
    run();
    check(!s_nv309e.active && s_nv309e.fmt == 0 && s_nv309e.offset == 0,
          "NV308A 0x0300 / 0x0304 are not NV309E SET_FORMAT / SET_OFFSET");
    check(s_gcm2d.point == 0x00050007u && s_gcm2d.src_fmt == 4u, "...they reach the NV308A state (POINT and COLOR_FORMAT)");

    put1(4, 0x300, 0x00030303u);        /* NV309E SET_FORMAT on its own slot */
    put1(4, 0x304, 0x00001234u);
    run();
    check(s_nv309e.active && s_nv309e.fmt == 0x00030303u && s_nv309e.offset == 0x1234u && s_gcm2d.point == 0x00050007u &&
          s_gcm2d.src_fmt == 4u, "NV309E's own 0x0300 / 0x0304 reach NV309E and leave the NV308A state alone");

    memset(mem(DST_EA), 0xEE, 64);
    dest(MAIN, FMT_A8R8G8B8, 0x40, DST_IO);
    geometry(0, 0, 1, 1, 1, 1);
    put1(5, 0x300, 4);
    put1(5, 0x400, 0x0A0B0C0Du);
    run();
    check(be32_at(mem(DST_EA), 0x0A0B0C0Du), "...and an NV309E bound beside it does not swallow the NV308A transfer");
}

static void verdicts(void)
{
    gcm_method_stats_reset();
    put1(5, 0x184, 0); put1(5, 0x188, 0); put1(5, 0x18C, 0); put1(5, 0x190, 0); put1(5, 0x194, 0); put1(5, 0x198, 0);
    put1(5, 0x300, 4);
    dest(MAIN, FMT_A8R8G8B8, 0x40, DST_IO);
    geometry(0, 0, 1, 1, 1, 1);
    put1(5, 0x400, 0x12345678u);
    put1(3, 0x184, LOCAL);
    run();
    GcmMethodStats st;
    gcm_method_stats(&st);
    check(st.unknown == 0 && st.missing == 0, "the default state's NV308A words and a transfer leave nothing unknown or missing");
    check(st.handled == 10 && st.inert == 8, "...the transfer registers are consumed; the raster contexts, COLOR_FORMAT and IMAGE_SOURCE are allow-listed");
}

/* A COLOR write into main memory is a guest-visible store: it goes through the reservation bookkeeping an
 * SPU line reservation needs (gcm_row_store), and it is in guest memory before a later NV406E RELEASE in the
 * same FIFO stores its label. The label is read at the moment the reservation dies. */
static void visibility(void)
{
    const u32 label = GCM_LABEL_GUEST_BASE + 0x40u;
    memset(mem(DST_EA), 0xEE, 256);
    vm_write32(label, 0);
    g_label_ea = label;
    g_resv_line = (DST_EA + 8u) & ~127u;      /* the line the word at x = 2 lands on */
    g_killed = 0;
    g_label_at_kill = 0xFFFFFFFFu;
    dest(MAIN, FMT_Y32, 0x40, DST_IO);
    geometry(2, 0, 1, 1, 1, 1);
    put1(5, 0x400, 0x600DF00Du);
    put1(0, 0x64, 0x40);                      /* NV406E SEMAPHORE_OFFSET */
    put1(0, 0x6C, 0xA5A5A5A5u);               /* SEMAPHORE_RELEASE: fence, then the label store */
    run();
    check(g_killed == 1, "a COLOR store into a line an SPU holds a reservation on kills it, once");
    check(g_label_at_kill == 0, "...before the later NV406E RELEASE stored its label (the walker runs the methods in order)");
    check(vm_read32(label) == 0xA5A5A5A5u && be32_at(mem(DST_EA) + 8, 0x600DF00Du),
          "...and after the pass the label and the transferred word are both in guest memory");
    g_label_ea = 0;
    g_resv_line = 0xFFFFFFFFu;
}

/* ---- stops ------------------------------------------------------------------------------------ */

typedef enum { ST_FORMAT, ST_DMA, ST_OPERATION, ST_SURFACE, ST_ROWS, ST_ROWS_IN, ST_SIZE_MISMATCH, ST_MAIN_UNMAPPED,
               ST_LOCAL_PAST, ST_WRAP, ST_LOCAL_VM, ST_UNKNOWN_308A, ST_UNKNOWN_3062, ST_COUNT } stop_id;

typedef struct {
    const char* name;
    const char* kind;
    const char* word;     /* the feature (gcm-unmodelled) or arg (hle-guest-range), or the class (unknown method) */
} stop_case;

static const stop_case k_stops[] = {
    [ST_FORMAT]       = { "an NV3062 COLOR_FORMAT the oracle cannot reach", "gcm-unmodelled", "nv3062-color-format" },
    [ST_DMA]          = { "a destination DMA object that is neither local nor main", "gcm-unmodelled", "nv3062-dma" },
    [ST_OPERATION]    = { "an operation other than SRCCOPY", "gcm-unmodelled", "nv308a-operation" },
    [ST_SURFACE]      = { "a swizzled surface context", "gcm-unmodelled", "nv308a-surface-context" },
    [ST_ROWS]         = { "a two-row SIZE_OUT", "gcm-unmodelled", "nv308a-rows" },
    [ST_ROWS_IN]      = { "a two-row SIZE_IN", "gcm-unmodelled", "nv308a-rows" },
    [ST_SIZE_MISMATCH] = { "a SIZE_IN wider than SIZE_OUT", "gcm-unmodelled", "nv308a-size-mismatch" },
    [ST_MAIN_UNMAPPED] = { "a main-memory offset with no IO mapping", "hle-guest-range", "nv308a-destination" },
    [ST_LOCAL_PAST]   = { "a local offset past local memory", "hle-guest-range", "nv308a-destination" },
    [ST_WRAP]         = { "a destination word that crosses the 32-bit offset space", "hle-guest-range", "nv308a-destination" },
    [ST_LOCAL_VM]     = { "a local offset inside local memory but outside guest memory", "hle-guest-range", "nv308a-destination" },
    [ST_UNKNOWN_308A] = { "an NV308A method with no row", "rsx-unknown-method", "class=NV308A" },
    [ST_UNKNOWN_3062] = { "an NV3062 method with no row", "rsx-unknown-method", "class=NV3062" },
};

static void stop_stream(stop_id id)
{
    /* One good transfer first (three words), so the stop's word counters are not at zero. */
    static const u32 w[3] = { 0x0A0A0A0Au, 0x0B0B0B0Bu, 0x0C0C0C0Cu };
    dest(MAIN, FMT_A8R8G8B8, 0x40, DST_IO);
    geometry(0, 0, 3, 1, 3, 1);
    putn(5, 0x400, w, 3, 0);
    /* The packet that stops: POINT (6,0), SIZE 4x1, one COLOR word. */
    geometry(6, 0, 4, 1, 4, 1);
    switch (id) {
    case ST_FORMAT:       put1(3, 0x300, 7); break;
    case ST_DMA:          put1(3, 0x188, 0x12345678u); break;
    case ST_OPERATION:    put1(5, 0x2FC, 5); break;
    case ST_SURFACE:      put1(5, 0x19C, RSX_OBJ_NV309E); break;
    case ST_ROWS:         put1(5, 0x308, 0x00020004u); break;
    case ST_ROWS_IN:      put1(5, 0x30C, 0x00020004u); break;
    case ST_SIZE_MISMATCH: put1(5, 0x30C, 0x00010009u); break;
    case ST_MAIN_UNMAPPED: put1(3, 0x30C, 0x00800000u); break;
    case ST_LOCAL_PAST:   put1(3, 0x188, LOCAL); put1(3, 0x30C, 0x10000000u); break;
    case ST_WRAP:         put1(3, 0x300, FMT_Y32); put1(3, 0x30C, 0xFFFFFFFEu); put1(5, 0x304, 0); break;
    case ST_LOCAL_VM:     s_config.localSize = 0x80000000u; put1(3, 0x188, LOCAL); put1(3, 0x30C, 0x20000000u); break;
    case ST_UNKNOWN_308A: put1(5, 0x310, 1); break;
    case ST_UNKNOWN_3062: put1(3, 0x308, 1); break;
    default: break;
    }
    if (id != ST_UNKNOWN_308A && id != ST_UNKNOWN_3062) put1(5, 0x400, 0x77777777u);
}

static const char* g_tmpdir;   /* where the stop window file of this run goes */

static int contains(const char* hay, const char* needle) { return strstr(hay, needle) != NULL; }

/* A whole small file into `buf`, NUL-terminated; 0 when it cannot be read. */
static size_t slurp(const char* path, char* buf, size_t cap)
{
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    const size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

/* The FIFO window of a stop (gcm_stop_fifo_dump), on a synthetic FIFO: an NV3062 offset, the NV308A packet
 * POINT/SIZE_OUT/SIZE_IN and the COLOR packet that follows it, then words behind `put` that must not appear. */
static void fifo_window(void)
{
    static const u32 col[5] = { 0xC0100001u, 0xC0100002u, 0xC0100003u, 0xC0100004u, 0xC0100005u };
    g_at = (g_at + 0x400u) & ~0xFu;
    const u32 io_off = g_at;
    put1(3, 0x30C, 0x00001000u);                            /* NV3062 OFFSET_DESTIN */
    const u32 io_geo = g_at;
    vm_write32(IO_ADDR + g_at, hdr(3, 5, 0x304));          /* POINT, SIZE_OUT, SIZE_IN in one packet */
    vm_write32(IO_ADDR + g_at + 4, 0x00000002u);
    vm_write32(IO_ADDR + g_at + 8, 0x00010005u);
    vm_write32(IO_ADDR + g_at + 12, 0x00010005u);
    g_at += 16;
    const u32 io_col = g_at;
    putn(5, 0x400, col, 5, 0);
    const u32 put = g_at;
    for (u32 i = 0; i < 8; i++) vm_write32(IO_ADDR + g_at + i * 4, 0xBADC0DE0u + i);   /* behind put */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, put);
    s_fifo_getoff = io_off;

    static char buf[1 << 19];                               /* the wrapped-ring window is ~2100 lines */
    char path[256];
    snprintf(path, sizeof path, "%s/rsx-stop-fifo.txt", g_tmpdir);
    unlink(path);
    const u32 stop = io_geo + 12;                           /* the SIZE_IN data word: the stop the issue reports */
    gcm_stop_fifo_dump("test", io_geo, stop, gcm_2d_state);
    size_t n = slurp(path, buf, sizeof buf);
    char want[160];
    snprintf(want, sizeof want, "window io=0x%08X..0x%08X", stop - 256u, put);
    check(n && contains(buf, want), "the window runs from 64 words before the stop word to put");
    snprintf(want, sizeof want, "io=0x%08X ea=0x%08X w=0x%08X    data 3/3 of sub5 NV308A method=0x030C   <-- stop word",
             stop, IO_ADDR + stop, 0x00010005u);
    check(contains(buf, want), "...the stop word is the SIZE_IN data word, named with its method and class, and marked");
    snprintf(want, sizeof want, "w=0x%08X    METHOD incrementing count=3 subch=5 method=0x0304 (NV308A)", hdr(3, 5, 0x304));
    check(contains(buf, want), "...the packet header is decoded (count, subchannel, method, class)");
    snprintf(want, sizeof want, "w=0x%08X    METHOD incrementing count=5 subch=5 method=0x0400 (NV308A)", hdr(5, 5, 0x400));
    int ok = contains(buf, want);
    for (u32 i = 0; i < 5; i++) {
        snprintf(want, sizeof want, "io=0x%08X ea=0x%08X w=0x%08X    data %u/5 of sub5 NV308A method=0x%04X", io_col + 4 + i * 4,
                 IO_ADDR + io_col + 4 + i * 4, col[i], i + 1, 0x400 + i * 4);
        ok &= contains(buf, want);
    }
    check(ok, "...the COLOR packet after the stop word is in the window, header and every data word");
    snprintf(want, sizeof want, "w=0x%08X    METHOD incrementing count=1 subch=3 method=0x030C (NV3062)", hdr(1, 3, 0x30C));
    check(contains(buf, want) && contains(buf, "state word=") && contains(buf, "size_in="),
          "...so is the NV3062 offset before it, and the file starts with the stop's state");
    check(!contains(buf, "BADC0DE"), "...and no word at or behind put");
    snprintf(want, sizeof want, "%u words, io=0x%08X..0x%08X", (put - (stop - 256u)) / 4u, stop - 256u, put - 4u);
    check(contains(buf, want), "...the word count and the last offset are printed");

    /* The start clamps at IO offset 0: a stop 8 words in has 8 words before it, not 64. */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, 0x100u);
    gcm_stop_fifo_dump("test", 0x20u, 0x20u, NULL);
    n = slurp(path, buf, sizeof buf);
    check(n && contains(buf, "window io=0x00000000..0x00000100") && contains(buf, "io=0x00000000 ea="),
          "a stop near the start of the command window starts at IO offset 0");

    /* put behind the stop (a wrapped ring): the 2048 words after the stop stand. */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, 0x10u);
    gcm_stop_fifo_dump("test", stop, stop, NULL);
    n = slurp(path, buf, sizeof buf);
    snprintf(want, sizeof want, "%u words, io=0x%08X..0x%08X", 64u + 1u + 2048u, stop - 256u, stop + 2048u * 4u);
    check(n && contains(buf, want), "put behind the stop: 64 words before it, the stop word and 2048 words after");
    if (!contains(buf, want)) printf("  want \"%s\"; file tail: %s\n", want, n > 200 ? buf + n - 200 : buf);
    unlink(path);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, g_at);
}

static int run_stop_case(stop_id id)
{
    const stop_case* sc = &k_stops[id];
    memset(mem(DST_EA), 0xEE, 256);
    memset(mem(LOCAL_EA), 0xEE, 64);
    memset(mem(0), 0, 64);
    int fds[2];
    if (pipe(fds)) return 0;
    fflush(stdout);
    /* The three words the good transfer writes are the only bytes allowed to differ in the destination. */
    const u64 words_before = s_gcm2d.words_total;
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        stop_stream(id);
        run();
        _exit(0);                        /* reaching here means the transfer did not stop */
    }
    close(fds[1]);
    char buf[16384];
    size_t n = 0;
    for (ssize_t r; n < sizeof buf - 1 && (r = read(fds[0], buf + n, sizeof buf - 1 - n)) > 0;) n += (size_t)r;
    buf[n] = 0;
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    char want[64];
    snprintf(want, sizeof want, "kind=%s", sc->kind);
    const int stopped = WIFEXITED(status) && WEXITSTATUS(status) == 70;
    /* An unmodelled NV308A path says what it awaits: hardware evidence, #63 stage B. */
    const int named = contains(buf, want) && contains(buf, sc->word) &&
                      (strcmp(sc->kind, "gcm-unmodelled") || contains(buf, "awaits hardware evidence, #63 stage B"));
    /* The state the stop must carry: the packet geometry, the counters, the destination and the bindings. A
     * COLOR word counts as seen when the stop is on it; an unknown method stops before any word. */
    const int on_word = id != ST_UNKNOWN_308A && id != ST_UNKNOWN_3062;
    char cw[64];
    snprintf(cw, sizeof cw, "color_words=%u/%llu", on_word ? 1u : 0u, (unsigned long long)(words_before + (on_word ? 4 : 3)));
    const int state = contains(buf, "point=0x") && contains(buf, "size_out=0x") && contains(buf, "size_in=0x") &&
                      contains(buf, cw) && contains(buf, "dst_offset=0x") && contains(buf, "dst_pitch=0x") &&
                      contains(buf, "dst_dma=0x") && contains(buf, "dst_format=0x") && contains(buf, "operation=") &&
                      contains(buf, "bind=0:NV4097,1:") && contains(buf, "3:NV3062,4:NV309E,5:NV308A");
    /* The stop is on the PS3STOP line too (the log line is the same text): the kind line holds the state. */
    const char* sl = strstr(buf, "PS3STOP v=1");
    const int on_line = id == ST_UNKNOWN_308A || id == ST_UNKNOWN_3062 || (sl && contains(sl, "point=0x"));
    const int untouched = all(mem(DST_EA) + 12, 244, 0xEE) && all(mem(LOCAL_EA), 64, 0xEE) && all(mem(0), 64, 0);
    /* The FIFO window the stop wrote: the stop word marked, the stop's own packet decoded, and the good
     * packet before it (three COLOR words) with its header. */
    char path[256], fbuf[1 << 16];
    snprintf(path, sizeof path, "%s/rsx-stop-fifo.txt", g_tmpdir);
    const size_t fn = slurp(path, fbuf, sizeof fbuf);
    unlink(path);
    const int window = fn && contains(fbuf, "<-- stop word") && contains(fbuf, "METHOD incrementing count=1 subch=") &&
                       contains(fbuf, "METHOD incrementing count=3 subch=5 method=0x0400 (NV308A)") &&
                       contains(fbuf, "w=0x0C0C0C0C    data 3/3 of sub5 NV308A method=0x0408");
    char what[200];
    snprintf(what, sizeof what, "%s: a named stop (%s, %s) carrying the transfer state, no byte of the stopped word moved",
             sc->name, sc->kind, sc->word);
    const int ok = stopped && named && state && on_line && untouched && window;
    if (g_show) printf("---- %s\n%s---- FIFO window file (head)\n%.2400s----\n", sc->name, buf, fbuf);
    check(ok, what);
    if (!ok)
        printf("  status=%d stopped=%d named=%d state=%d on_line=%d untouched=%d (want %s)\n  child stderr: %.1200s\n",
               status, stopped, named, state, on_line, untouched, cw, buf);
    return ok;
}

static void drop_tmpdir(void) { if (g_tmpdir) (void)rmdir(g_tmpdir); }

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: %s run | stops [show]\n", argv[0]); return 2; }
    /* The stop window file goes where TMPDIR says (the children inherit it). */
    static char tmpl[] = "/tmp/nv308a-fifo-XXXXXX";
    if (!mkdtemp(tmpl)) { puts("FAIL: mkdtemp"); return 1; }
    g_tmpdir = tmpl;
    atexit(drop_tmpdir);
    setenv("TMPDIR", tmpl, 1);
    setup();
    g_show = argc > 2 && !strcmp(argv[2], "show");
    if (!strcmp(argv[1], "stops")) {
        /* The good transfer in the child's stream writes bytes 0..11 of the destination; stop_case checks the rest. */
        for (int i = 0; i < ST_COUNT; i++) run_stop_case((stop_id)i);
        printf("[nv308a stops] %s\n", g_fail ? "FAIL" : "PASS");
        return g_fail ? 1 : 0;
    }
    one_by_one();
    format_y32_local();
    long_rows();
    clipping();
    non_incrementing();
    sixteen_bit();
    main_memory_pitch();
    routing_guard();
    verdicts();
    visibility();
    fifo_window();
    printf("[nv308a run] %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}

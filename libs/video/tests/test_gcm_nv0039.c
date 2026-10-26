/* NV0039 (M2MF) and the method verdicts of the FIFO walker (cellGcmSys.c, rsx_method_class.h).
 *
 * Runs the real cellGcm_rsx_process_fifo over synthetic rings. Everything observed is guest memory after
 * the walk, a counter the walker exports, or a capture left in a directory for tools/test_rsx_methods.py.
 *
 *   run [dir]    every copy scenario and the verdict scenarios; with a directory, under a capture whose one
 *                frame is invalid (unknown methods were consumed)
 *   clean <dir>  the copies and only handled / allow-listed methods, under a capture whose frame is valid
 *   stops        each way a copy must end in a named stop, in a forked child that shares guest memory, so
 *                the parent can see that not one destination byte moved before the stop
 *
 * cc -std=gnu17 ... (CMake target test_gcm_nv0039)
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

/* The 3D state machine: what it received (in order), and which methods it has no handler for. */
static u32 g_3d[64], g_3d_n;
int rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state; (void)data;
    if (g_3d_n < 64) g_3d[g_3d_n++] = method;
    /* The vtable state machine consumes BEGIN_END (a draw, so the walker's `handled` comes from the
     * consumer); it consumes nothing else this fixture asks about, so the verdict comes from the table. */
    return method == 0x1808u ? 0 : -1;
}
int  rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
/* rsx_dsp_method_class is the real one (rsx_dispatch.c): its class table is a property of the method
 * number and is the engine's consumer list, so the verdicts below see it. */
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
#define SRC_IO    0x00200000u
#define DST_IO    0x00280000u
#define SRC_EA    (IO_ADDR + SRC_IO)
#define DST_EA    (IO_ADDR + DST_IO)
#define LOCAL_EA  0x10000000u
#define MAIN      0xFEED0001u
#define LOCAL     0xFEED0000u

static int g_fail;
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
static void run(void)
{
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, g_at);
    cellGcm_rsx_process_fifo();
}

static u8* mem(u32 ea) { return vm_base + ea; }
static void pat(u8* p, u32 n, u32 seed) { for (u32 i = 0; i < n; i++) p[i] = (u8)(seed + i * 7u + (i >> 3)); }
static int all(const u8* p, u32 n, u8 v) { for (u32 i = 0; i < n; i++) if (p[i] != v) return 0; return 1; }

/* The registers a copy needs, on subchannel 1 (bound to NV0039 first), then the trigger. */
static void m2mf(u32 dma_in, u32 dma_out, u32 in_off, u32 out_off, u32 pin, u32 pout, u32 len, u32 cnt, u32 fmt)
{
    put1(1, 0x184, dma_in);   put1(1, 0x188, dma_out);
    put1(1, 0x30C, in_off);   put1(1, 0x310, out_off);
    put1(1, 0x314, pin);      put1(1, 0x318, pout);
    put1(1, 0x31C, len);      put1(1, 0x320, cnt);
    put1(1, 0x324, fmt);
    put1(1, 0x328, 0);
}

static void setup(const char* capture_dir)
{
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)mmap(NULL, vm_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (vm_base == MAP_FAILED) { puts("FAIL: guest VM"); exit(1); }
    ppu_vm_size = vm_size;
    if (capture_dir) {
        rsx_capture_options opt = { capture_dir, 0 };
        rsx_capture_environment env = { "synthetic", "n/a", "n/a", "Debug", "n/a", "n/a", "synthetic", "n/a" };
        (void)rsx_capture_install(&env, &opt);
    }
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    s_config.localAddress = LOCAL_EA;    /* inside the fixture's guest memory */
    put1(1, 0, RSX_OBJ_NV0039);
    put1(0, 0, RSX_OBJ_NV4097);
    run();
}

/* ---- copies ---------------------------------------------------------------------------------- */

static void copy_block(void)
{
    pat(mem(SRC_EA), 64, 1);
    memset(mem(DST_EA), 0xEE, 256);
    m2mf(MAIN, MAIN, SRC_IO, DST_IO, 64, 64, 64, 1, 0x0101);
    run();
    check(!memcmp(mem(DST_EA), mem(SRC_EA), 64), "increment 1/1, pitch = line length: the destination holds the source");
    check(all(mem(DST_EA) + 64, 192, 0xEE), "...and not one byte past the span moved");
}

static void copy_pitch(void)
{
    pat(mem(SRC_EA), 128, 3);
    memset(mem(DST_EA), 0xEE, 256);
    m2mf(MAIN, MAIN, SRC_IO, DST_IO, 16, 32, 8, 4, 0x0101);   /* 8 bytes a row, 16 apart in, 32 apart out */
    run();
    int rows = 1, gaps = 1;
    for (u32 r = 0; r < 4; r++) {
        rows &= !memcmp(mem(DST_EA) + 32 * r, mem(SRC_EA) + 16 * r, 8);
        gaps &= all(mem(DST_EA) + 32 * r + 8, 24, 0xEE);
    }
    check(rows, "pitch in 16, pitch out 32, line 8: each row lands at its own pitch");
    check(gaps, "...and the bytes between the rows are untouched");
}

static void copy_increments(void)
{
    u8 exp[64];
    pat(mem(SRC_EA), 64, 5);
    memset(mem(DST_EA), 0xEE, 128);
    memset(exp, 0xEE, sizeof exp);
    for (u32 r = 0; r < 2; r++)
        for (u32 c = 0; c < 6; c++) exp[r * 24 + c * 4] = mem(SRC_EA)[r * 12 + c * 2];
    m2mf(MAIN, MAIN, SRC_IO, DST_IO, 12, 24, 6, 2, 0x0402);   /* input every 2nd byte, output every 4th */
    run();
    check(!memcmp(mem(DST_EA), exp, 48) && all(mem(DST_EA) + 48, 80, 0xEE),
          "increment in 2, out 4: one byte per element, gathered and spread");

    memset(mem(DST_EA), 0xEE, 128);
    m2mf(MAIN, MAIN, SRC_IO, DST_IO, 20, 5, 5, 1, 0x0104);    /* input every 4th byte, output packed */
    run();
    int ok = 1;
    for (u32 c = 0; c < 5; c++) ok &= mem(DST_EA)[c] == mem(SRC_EA)[c * 4];
    check(ok && all(mem(DST_EA) + 5, 123, 0xEE), "increment in 4, out 1: a strided gather into a packed run");
}

static void copy_overlap(void)
{
    u8 orig[128], exp[128];
    /* A plain block copy to a higher address inside its own source: as if the source were read first. */
    pat(mem(SRC_EA), 128, 9);
    memcpy(orig, mem(SRC_EA), 128);
    memcpy(exp, orig, 128);
    memcpy(exp + 16, orig, 64);
    m2mf(MAIN, MAIN, SRC_IO, SRC_IO + 16, 64, 64, 64, 1, 0x0101);
    run();
    check(!memcmp(mem(SRC_EA), exp, 128), "overlapping block copy (dst = src + 16): the source was read before it was written");

    /* Rows whose destination overlaps the NEXT source row: each row still gets its own source row. */
    pat(mem(SRC_EA), 128, 17);
    memcpy(orig, mem(SRC_EA), 128);
    memcpy(exp, orig, 128);
    for (u32 r = 0; r < 4; r++) memcpy(exp + 8 + 12 * r, orig + 12 * r, 8);
    m2mf(MAIN, MAIN, SRC_IO, SRC_IO + 8, 12, 12, 8, 4, 0x0101);
    run();
    check(!memcmp(mem(SRC_EA), exp, 128), "overlapping rows (dst row r overlaps src row r+1): every row copies the original");

    /* Increments above 1: the elements move one at a time and see each other's writes (RPCS3's order). */
    pat(mem(SRC_EA), 128, 23);
    memcpy(exp, mem(SRC_EA), 128);
    for (u32 c = 0; c < 16; c++) exp[c * 2] = exp[c];
    m2mf(MAIN, MAIN, SRC_IO, SRC_IO, 32, 32, 16, 1, 0x0201);
    run();
    check(!memcmp(mem(SRC_EA), exp, 128), "overlapping strided copy: elements in order, a later one reads an earlier one's write");
}

static void copy_negative_pitch(void)
{
    pat(mem(SRC_EA), 64, 29);
    memset(mem(DST_EA), 0xEE, 64);
    /* Three rows read bottom-up: the input pitch is -16 and the first row is the last in memory. */
    m2mf(MAIN, MAIN, SRC_IO + 32, DST_IO, (u32)-16, 8, 8, 3, 0x0101);
    run();
    int ok = 1;
    for (u32 r = 0; r < 3; r++) ok &= !memcmp(mem(DST_EA) + 8 * r, mem(SRC_EA) + 32 - 16 * r, 8);
    check(ok && all(mem(DST_EA) + 24, 40, 0xEE), "a negative input pitch walks the source upwards, row by row");
}

static void copy_locations(void)
{
    pat(mem(LOCAL_EA + 0x100), 32, 31);
    memset(mem(DST_EA), 0xEE, 64);
    m2mf(LOCAL, MAIN, 0x100, DST_IO, 32, 32, 32, 1, 0x0101);
    run();
    check(!memcmp(mem(DST_EA), mem(LOCAL_EA + 0x100), 32), "local source, main destination");
    pat(mem(SRC_EA), 32, 37);
    memset(mem(LOCAL_EA + 0x400), 0xEE, 64);
    m2mf(MAIN, LOCAL, SRC_IO, 0x400, 32, 32, 32, 1, 0x0101);
    run();
    check(!memcmp(mem(LOCAL_EA + 0x400), mem(SRC_EA), 32) && all(mem(LOCAL_EA + 0x400) + 32, 32, 0xEE),
          "main source, local destination");
}

/* A subchannel's class is its last SET_OBJECT: NV0039 methods stop being M2MF the moment it is rebound. */
static void rebind(void)
{
    pat(mem(SRC_EA), 32, 41);
    memset(mem(DST_EA), 0xEE, 64);
    g_3d_n = 0;
    put1(1, 0, RSX_OBJ_NV4097);
    put1(1, 0x1808, 5);
    m2mf(MAIN, MAIN, SRC_IO, DST_IO, 32, 32, 32, 1, 0x0101);   /* now 3D methods on subchannel 1 */
    run();
    int seen_3d = 0, seen_trigger = 0;
    for (u32 i = 0; i < g_3d_n; i++) { seen_3d |= g_3d[i] == 0x1808u; seen_trigger |= g_3d[i] == 0x328u; }
    check(seen_3d && seen_trigger, "bound to NV4097, subchannel 1 routes to the 3D state machine (0x1808 and 0x328 arrive)");
    check(all(mem(DST_EA), 64, 0xEE), "...and 0x328 copies nothing there");
    put1(1, 0, RSX_OBJ_NV0039);
    m2mf(MAIN, MAIN, SRC_IO, DST_IO, 32, 32, 32, 1, 0x0101);
    run();
    check(!memcmp(mem(DST_EA), mem(SRC_EA), 32), "rebound to NV0039, the same methods copy again");
    g_3d_n = 0;
    put1(1, 0x30C, 0x1234);
    run();
    check(g_3d_n == 0, "while bound to NV0039 none of its methods reaches the 3D state machine");
}

/* ---- verdicts -------------------------------------------------------------------------------- */

/* Verdicts over the allow-listed and value-pinned methods. An unknown method is a named stop now, so
 * this fixture exercises the classes that do NOT stop: handled, inert, and the known gaps (missing).
 * The stop itself is test_rsx_method_census.c's forked case. */
static void verdicts(int with_missing)
{
    gcm_method_stats_reset();
    put1(0, 0, RSX_OBJ_NV4097);
    put1(1, 0, RSX_OBJ_NV0039);
    put1(0, 0x1808, 1);          /* handled */
    put1(0, 0x0100, 0);          /* NO_OPERATION: inert, allow-listed */
    put1(1, 0x0180, 0);          /* NV0039 DMA_NOTIFY: inert, allow-listed */
    if (with_missing) {
        put1(0, 0x1D88, 0x12D0); /* shader window: an S2 gap, missing for its value */
        put1(0, 0x1D88, 0x1000); /* ...twice, one distinct pair */
        put1(0, 0x0300, 0);      /* dither pinned to enabled=1: a different value is missing */
    }
    run();
    GcmMethodStats st;
    gcm_method_stats(&st);
    GcmUnknownMethod u[8];
    const u32 n = gcm_unknown_methods(u, 8);
    printf("[verdicts] handled=%llu inert=%llu missing=%llu unknown=%llu unbound=%llu\n",
           st.handled, st.inert, st.missing, st.unknown, st.unbound);
    /* NO_OPERATION is also marked EXEC by the dispatcher's class table (its explicit no-op), so a
     * consumer takes it and the allow-list entry is the second, redundant citation; the NV0039 notify
     * has no consumer and is counted inert. */
    check(st.inert == 1, "NV0039 DMA_NOTIFY counts as inert (NO_OPERATION is consumed as a no-op)");
    if (with_missing) {
        check(st.unknown == 0 && n == 0, "missing states are not unknown: the frame stays valid");
        check(st.missing == 3, "three writes are classified missing");
        GcmUnknownMethod m[8];
        const u32 mn = gcm_missing_methods(m, 8);
        check(mn == 2 && m[0].cls == RSX_CLASS_NV4097 && m[0].method == 0x300 && m[0].count == 1 &&
              m[0].data == 0 && m[1].method == 0x1D88 && m[1].count == 2 && m[1].data == 0x12D0,
              "counted per pair: the off-value dither once, the shader window twice, with first data words");
        check(!strcmp(gcm_method_class_name(m[1].cls), "NV4097"), "the class has a name");
    } else {
        check(st.unknown == 0 && n == 0, "handled and allow-listed methods leave the unknown count at zero");
    }
}

static void frame(void)
{
    static rsx_dispatch regs;           /* an untouched register file */
    static const u8 px[4] = { 0, 0, 0, 255 };
    rsx_cap_frame fr;
    memset(&fr, 0, sizeof fr);
    fr.flip_id = 0; fr.guest_frame = 0; fr.display_buffer = 1; fr.present_seq = 0;
    fr.rsx = &regs;
    fr.readback = 0;
    fr.format = "rgba8-unorm"; fr.channel_order = "rgba"; fr.colorspace = "linear";
    fr.width = 1; fr.height = 1; fr.row_pitch = 4; fr.bytes = px;
    s_cap->frame(&fr);
}

/* ---- stops ----------------------------------------------------------------------------------- */

typedef struct {
    const char* name;
    u32 dma_in, dma_out, in_off, out_off, pin, pout, len, cnt, fmt;
    const char* kind;
    const char* word;
} stop_case;

static const stop_case k_stops[] = {
    { "increment 3 on the input",   MAIN, MAIN, SRC_IO, DST_IO, 64, 64, 64, 1, 0x0103, "gcm-unmodelled", "m2mf-increment" },
    { "increment 8 on the output",  MAIN, MAIN, SRC_IO, DST_IO, 64, 64, 64, 1, 0x0801, "gcm-unmodelled", "m2mf-increment" },
    { "increment 0",                MAIN, MAIN, SRC_IO, DST_IO, 64, 64, 64, 1, 0x0000, "gcm-unmodelled", "m2mf-increment" },
    { "a DMA object that is neither", 0x12345678u, MAIN, SRC_IO, DST_IO, 64, 64, 64, 1, 0x0101, "gcm-unmodelled", "m2mf-dma" },
    { "a source span that wraps the offset space", MAIN, MAIN, 0xFFFFFF00u, DST_IO, 0x200, 0x200, 0x200, 1, 0x0101,
      "hle-guest-range", "m2mf-source" },
    { "a destination span that wraps", MAIN, MAIN, SRC_IO, 0xFFFFFFF0u, 0x20, 0x20, 0x20, 1, 0x0101,
      "hle-guest-range", "m2mf-destination" },
    { "rows whose spread overflows 32 bits", MAIN, MAIN, SRC_IO, DST_IO, 16, 16, 4, 0x40000000u, 0x0101,
      "hle-guest-range", "m2mf-source" },
    { "a local span past guest memory", LOCAL, MAIN, 0x20000000u, DST_IO, 64, 64, 64, 1, 0x0101,
      "hle-guest-range", "m2mf-source" },
    { "a negative pitch that walks below offset 0", MAIN, MAIN, 0x10, DST_IO, (u32)-0x100, 64, 64, 4, 0x0101,
      "hle-guest-range", "m2mf-source" },
};

/* The child's stderr is read through a pipe; the destination is shared memory the parent inspects. */
static int run_stop_case(const stop_case* sc)
{
    memset(mem(DST_EA), 0xEE, 256);
    pat(mem(SRC_EA), 256, 1);
    memset(mem(0), 0, 64);
    int fds[2];
    if (pipe(fds)) return 0;
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        m2mf(sc->dma_in, sc->dma_out, sc->in_off, sc->out_off, sc->pin, sc->pout, sc->len, sc->cnt, sc->fmt);
        run();
        _exit(0);                        /* reaching here means the copy did not stop */
    }
    close(fds[1]);
    char buf[8192];
    size_t n = 0;
    for (ssize_t r; n < sizeof buf - 1 && (r = read(fds[0], buf + n, sizeof buf - 1 - n)) > 0;) n += (size_t)r;
    buf[n] = 0;
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    char want[64];
    snprintf(want, sizeof want, "kind=%s", sc->kind);
    const int stopped = WIFEXITED(status) && WEXITSTATUS(status) == 70;
    const int named = strstr(buf, want) && strstr(buf, sc->word);
    const int untouched = all(mem(DST_EA), 256, 0xEE) && all(mem(0), 64, 0);
    char what[160];
    snprintf(what, sizeof what, "%s: a named stop (%s, %s), no destination byte moved", sc->name, sc->kind, sc->word);
    check(stopped && named && untouched, what);
    if (!(stopped && named && untouched))
        printf("  status=%d stopped=%d named=%d untouched=%d\n  child stderr: %.300s\n", status, stopped, named, untouched, buf);
    return stopped && named && untouched;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: %s run [dir] | clean <dir> | stops\n", argv[0]); return 2; }
    const char* mode = argv[1];
    const char* dir = argc > 2 ? argv[2] : NULL;
    if (!strcmp(mode, "stops")) {
        setup(NULL);
        for (size_t i = 0; i < sizeof k_stops / sizeof k_stops[0]; i++) run_stop_case(&k_stops[i]);
        return g_fail ? 1 : 0;
    }
    if (!strcmp(mode, "clean") && !dir) { fprintf(stderr, "clean needs a capture directory\n"); return 2; }
    setup(dir);
    copy_block();
    copy_pitch();
    copy_increments();
    copy_overlap();
    copy_negative_pitch();
    copy_locations();
    rebind();
    GcmMethodStats st;
    gcm_method_stats(&st);
    check(st.m2mf_copies == 11, "each of the 11 BUFFER_NOTIFYs that met an NV0039 binding ran one copy, counted by the walker");
    verdicts(strcmp(mode, "clean") != 0);
    if (dir) {
        frame();
        rsx_capture_finish();
    }
    printf("[nv0039 %s] %s\n", mode, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}

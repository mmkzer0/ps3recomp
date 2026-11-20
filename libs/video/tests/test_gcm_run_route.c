/* One verdict per word, an unknown method stops at its own word, and the draw engine sees each word
 * before its verdict -- for the run of 3D methods the walker takes in one step (cellGcmSys.c, the
 * "run" route) as well as for the word-by-word route.
 *
 * The run route used to be off whenever the register-file draw engine was on, so a frame's state
 * words all took the word-by-word route. It now serves the engine too, which must change nothing
 * observable: the same words reach the engine in the same order with the same values, the census
 * counts the same verdicts, the legacy state machine sees the same words, and an unknown method in
 * the middle of a packet ends the pass at that word: the words before it counted and mirrored, the
 * words after it neither.
 *
 * The fixture builds one stream (argv[1]) and derives what must happen from the verdict contract
 * (docs/RSX_GRAPHICS.md "Method classification": the class table, the allow-list rows and the walker's
 * own consumers), not from the walker. tools/test_gcm_run_route.py runs it twice: as shipped, and with
 * GCM_OBJDBG=1, which keeps the word-by-word route (a per-word diagnostic switch is the one thing that
 * does), and requires the two runs to print the same. A stream that must stop ends in the fixture's
 * unknown-method stub (exit status 70), whose at-exit hook does the checking.
 *
 * cc -std=gnu17 -I include -I libs/video libs/video/tests/test_gcm_run_route.c
 *   runtime/memory/label_window.c runtime/diag/host_exit.c runtime/diag/ps3_log.c
 *   tests/ps3_log_mirror_stub.c libs/video/gcm_default_state.c
 *   -ffunction-sections -fdata-sections -Wl,-dead_strip   (gcc: -Wl,--gc-sections -pthread -lm)
 */
#include "../cellGcmSys.c"
#include "gcm_fixture_stubs.h"   /* ps3_config_get and the named stops cellGcmSys.c ends in */
#include "../rsx_tiling.c"
#include "../rsx_dispatch.c"     /* the real class table: rsx_dsp_method_class */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int spu_taskset_round_busy(uint32_t* ts, uint32_t* tid) { (void)ts; (void)tid; return 0; }
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
rsx_backend* rsx_get_backend(void) { return NULL; }
int  rsx_draw_engine_enabled(void) { return 1; }
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }

/* What the walker hands the engine and the legacy state machine, in order. */
typedef struct { u32 m, v; } word_t;
#define MAX_WORDS 4096
static word_t engine_seen[MAX_WORDS], legacy_seen[MAX_WORDS];
static u32 n_engine, n_legacy;
void rsx_draw_engine_method(u32 method, u32 arg)
{
    if (n_engine < MAX_WORDS) { engine_seen[n_engine].m = method; engine_seen[n_engine].v = arg; }
    n_engine++;
}
int rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state;
    if (n_legacy < MAX_WORDS) { legacy_seen[n_legacy].m = method; legacy_seen[n_legacy].v = data; }
    n_legacy++;
    return 0;                     /* taken */
}

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

/* ---- the contract, from the tables ------------------------------------------------------------ */

typedef enum { K_HANDLED, K_INERT, K_MISSING, K_UNKNOWN } kind_t;

/* What the walker's verdict for an NV4097 word must be. The legacy state machine is not live (the
 * engine is on, no vtable backend, no RSX_TRACE), so only the methods rsx_process_method acts on beyond
 * its state reach it. A word the walker consumes itself, or the class table claims, is handled; any other
 * is the allow-list's to classify (rsx_method_class.h), and no row means unknown. */
static int has_legacy_effect(u32 m)
{
    return m == 0x0050u || m == 0x0104u || m == 0x0180u || m == 0x01A4u || m == 0x1D6Cu || m == 0x1D70u ||
           m == 0x1D74u;
}
static kind_t expect_kind(u32 m, u32 v)
{
    if (has_legacy_effect(m) || m == 0x1A8u || m == 0x1800u) return K_HANDLED;
    if (m >= 0x100u && rsx_dsp_method_class(m) != RSX_DSP_CLASS_TODO) return K_HANDLED;
    const rsx_class_id cls = m < 0x100u ? RSX_CLASS_NV406E : RSX_CLASS_NV4097;
    const rsx_method_rule* r = rsx_method_rule_find(cls, m);
    if (!r) return K_UNKNOWN;
    if (r->verdict == RSX_METHOD_HANDLED) return K_HANDLED;
    if (r->verdict == RSX_METHOD_INERT)
        return (!r->mask || (v & r->mask) == (r->data & r->mask)) ? K_INERT : K_MISSING;
    return K_MISSING;
}

/* ---- the stream ------------------------------------------------------------------------------- */

static struct { u32 io, m, v, hdr_io; int bind; } stream[MAX_WORDS];
static u32 n_stream, io;

/* One packet: `count` words, the first at method `method`. Type 0 increments, type 2 does not. */
static void packet(u32 type, u32 subch, u32 method, u32 count, const u32* words)
{
    const u32 hdr = io;
    gw32(IO_ADDR + io, (type == 2 ? 0x40000000u : 0u) | (count << 18) | (subch << 13) | method);
    io += 4;
    for (u32 i = 0; i < count; i++) {
        gw32(IO_ADDR + io, words[i]);
        stream[n_stream].io = io;
        stream[n_stream].hdr_io = hdr;
        stream[n_stream].m = type == 2 ? method : method + i * 4;
        stream[n_stream].v = words[i];
        stream[n_stream].bind = method == 0u;     /* SET_OBJECT: the walker's own, never mirrored */
        n_stream++;
        io += 4;
    }
}
static void ramp(u32 type, u32 subch, u32 method, u32 count, u32 seed)
{
    u32 w[0x800];
    for (u32 i = 0; i < count; i++) w[i] = seed + i * 0x01010101u;
    packet(type, subch, method, count, w);
}

/* A method of each kind the census tells apart, found from the tables. */
static u32 find_method(kind_t want, u32 lo)
{
    for (u32 m = lo; m < 0x2000u; m += 4)
        if (expect_kind(m, 0) == want && (want != K_HANDLED || (m >= 0x100u && !has_legacy_effect(m) &&
            m != 0x1A8u && m != 0x1800u && rsx_dsp_method_class(m) != RSX_DSP_CLASS_TODO))) return m;
    return 0;
}
static u32 find_inert_any_value(void)
{
    for (u32 m = 0x100u; m < 0x2000u; m += 4) {
        const rsx_method_rule* r = rsx_method_rule_find(RSX_CLASS_NV4097, m);
        if (r && r->verdict == RSX_METHOD_INERT && !r->mask && rsx_dsp_method_class(m) == RSX_DSP_CLASS_TODO)
            return m;
    }
    return 0;
}
/* The first word of a packet of three where the third is unknown and the two before it are not. */
static u32 find_unknown_third(void)
{
    for (u32 u = 0x108u; u < 0x1FF0u; u += 4)
        if (expect_kind(u, 1) == K_UNKNOWN && expect_kind(u - 4, 1) != K_UNKNOWN &&
            expect_kind(u - 8, 1) != K_UNKNOWN)
            return u - 8;
    return 0;
}

static void build_prefix(void)
{
    /* Enough ordinary traffic that the stop is not at the start of the ring. */
    ramp(0, 0, 0x0A74u, 1, 1u);
    ramp(0, 0, 0x0A20u, 6, 0x3F800000u);
    ramp(2, 0, 0x1818u, 4, 0x3F000000u);
}

static void build_mixed(void)
{
    build_prefix();
    ramp(0, 0, 0x1EFCu, 1, 4u);                                 /* constant load id */
    ramp(0, 0, 0x1F00u, 32, 0x40000000u);                       /* constant window, a long run */
    ramp(0, 0, 0x0B80u, 32, 0x00001000u);                       /* program window */
    ramp(0, 0, 0x1680u, 16, 0x1000u);                           /* vertex array offsets */
    ramp(0, 0, 0x1A00u, 8, 0x2000u);                            /* a texture unit's block */
    const u32 missing = find_method(K_MISSING, 0x100u);
    const u32 inert = find_inert_any_value();
    if (missing) ramp(0, 0, missing, 1, 2u);                    /* a state the renderer ignores */
    if (inert) ramp(0, 0, inert, 1, 0x55u);                     /* one nothing depends on */
    ramp(0, 0, 0x0180u, 1, 0xFEED0000u);                        /* the legacy state machine's methods */
    ramp(0, 0, 0x1D6Cu, 3, 0x10u);
    ramp(0, 0, 0x01A4u, 1, 0x66616661u);
    ramp(0, 0, 0x01A8u, 1, 0xFEED0000u);                        /* SET_CONTEXT_DMA_REPORT (RSX local reports) */
    ramp(0, 0, 0x1800u, 1, (1u << 24) | 0x40u);                 /* GET_REPORT */
    ramp(0, 0, 0x0050u, 1, 7u);                                 /* channel control: the word-by-word route */
    ramp(0, 0, 0x1FF8u, 1, 3u);                                 /* the last method of the field */
    ramp(0, 0, 0x0A74u, 2, 9u);                                 /* an incrementing pair on a state register */
    /* A second slot bound to NV4097: the route goes by the slot's class, not its number. */
    u32 bind[1] = { RSX_OBJ_NV4097 };
    packet(0, 2, 0, 1, bind);
    ramp(0, 2, 0x0A20u, 4, 0x3F800000u);
    ramp(2, 2, 0x1818u, 3, 0x3E000000u);
    ramp(0, 0, 0x0A74u, 1, 0u);
}

static void build_unknown_mid(void)
{
    build_prefix();
    const u32 first = find_unknown_third();
    if (!first) { puts("[FAIL] no method triple in the tables to stop on"); exit(1); }
    /* known, known, unknown, then two words that must never run */
    ramp(0, 0, first, 5, 0x77u);
    ramp(0, 0, 0x0A74u, 1, 1u);
}

static void build_ni_unknown(void)
{
    build_prefix();
    const u32 unknown = find_method(K_UNKNOWN, 0x100u);
    if (!unknown) { puts("[FAIL] no unknown method in the tables"); exit(1); }
    ramp(2, 0, unknown, 3, 0x77u);                              /* the same word three times: the first stops */
    ramp(0, 0, 0x0A74u, 1, 1u);
}

/* An incrementing packet that runs off the 13-bit method field: the word past it is a different
 * method (0x2000 is not a register of the 3D engine) and is unknown, whatever the run route would
 * have made of it. */
static void build_overflow(void)
{
    build_prefix();
    ramp(0, 0, 0x1FF8u, 4, 0x77u);
    ramp(0, 0, 0x0A74u, 1, 1u);
}

/* ---- the check, at the end of the run (also on the stop's exit) ---------------------------------- */

static int fails;
static void check(int ok, const char* what)
{
    printf("[CHECK %s] %s\n", ok ? "ok" : "FAIL", what);
    fails += !ok;
}

static int reported;
static void report(void)
{
    if (reported) return;
    reported = 1;
    /* What the contract says happened: the words up to and including the first unknown one. */
    u32 consumed = n_stream, stop_at = n_stream;
    for (u32 i = 0; i < n_stream; i++)
        if (!stream[i].bind && expect_kind(stream[i].m, stream[i].v) == K_UNKNOWN) { stop_at = i; consumed = i + 1; break; }
    unsigned long long h = 0, in = 0, mi = 0;
    u32 want_legacy = 0;
    for (u32 i = 0; i < consumed; i++) {
        if (stream[i].bind) { h++; continue; }        /* a bind is consumed and counted handled */
        const kind_t k = expect_kind(stream[i].m, stream[i].v);
        h += k == K_HANDLED; in += k == K_INERT; mi += k == K_MISSING;
        want_legacy += has_legacy_effect(stream[i].m);
    }

    GcmMethodStats st;
    gcm_method_stats(&st);
    printf("census handled=%llu inert=%llu missing=%llu unknown=%llu\n", st.handled, st.inert, st.missing, st.unknown);
    printf("engine words=%u legacy words=%u get=0x%X\n", n_engine, n_legacy, s_fifo_getoff);
    for (u32 i = 0; i < n_engine && i < MAX_WORDS; i++) printf("E %04X %08X\n", engine_seen[i].m, engine_seen[i].v);
    for (u32 i = 0; i < n_legacy && i < MAX_WORDS; i++) printf("L %04X %08X\n", legacy_seen[i].m, legacy_seen[i].v);

    check(st.handled == h, "the census counts every handled word once, and none past a stop");
    check(st.inert == in, "the census counts the inert words");
    check(st.missing == mi, "the census counts the missing words");
    check(st.unknown == (stop_at < n_stream ? 1u : 0u),
          "an unknown word is counted once, and only the one that stopped the pass");
    u32 want_engine = 0;
    int engine_ok = 1;
    for (u32 i = 0; i < consumed; i++) {
        if (stream[i].bind || stream[i].m < 0x100u) continue;    /* channel control is the puller's, not mirrored */
        if (want_engine >= n_engine || engine_seen[want_engine].m != stream[i].m ||
            engine_seen[want_engine].v != stream[i].v) engine_ok = 0;
        want_engine++;
    }
    check(engine_ok && n_engine == want_engine,
          "the engine saw each word, in order, with its value, up to and including the stop word, and no later one");
    int legacy_ok = n_legacy == want_legacy;
    u32 li = 0;
    for (u32 i = 0; legacy_ok && i < consumed; i++)
        if (!stream[i].bind && has_legacy_effect(stream[i].m)) {
            legacy_ok = legacy_seen[li].m == stream[i].m && legacy_seen[li].v == stream[i].v;
            li++;
        }
    check(legacy_ok, "the legacy state machine saw only the words it acts on, in order");
    if (stop_at < n_stream)
        printf("expect stop fifo=0x%08X word=0x%08X\n", stream[stop_at].hdr_io, stream[stop_at].io);
    else
        check(s_fifo_getoff == io, "the pass ran to the end of the stream");
    printf("[run_route] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    fflush(stdout);
}

int main(int argc, char** argv)
{
    const char* which = argc > 1 ? argv[1] : "mixed";
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    if (!strcmp(which, "mixed")) build_mixed();
    else if (!strcmp(which, "unknown-mid")) build_unknown_mid();
    else if (!strcmp(which, "ni-unknown")) build_ni_unknown();
    else if (!strcmp(which, "overflow")) build_overflow();
    else { printf("FAIL: unknown stream '%s'\n", which); return 1; }
    if (n_stream >= MAX_WORDS || io >= IO_SIZE) { puts("FAIL: stream too big"); return 1; }
    atexit(report);              /* the unknown-method stub ends the process: report on the way out */

    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, io);
    cellGcm_rsx_process_fifo();
    report();
    free(vm_base);
    return fails ? 1 : 0;
}

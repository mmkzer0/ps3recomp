/* The handler mask word at driver_info + 0x12C0, as FW 4.93 libgcm_sys keeps it (dossier
 * GCM-CALLBACK-OPD-CONTRACT.md §1, §2, §6 T6).
 *
 * The firmware initialises the word to 4 when its bootstrap has created the interrupt thread
 * (0x02008240/0x02008244, a plain store), and each Set*Handler then does a read-modify-write of ITS OWN
 * bit: set for a handler, cleared for NULL, every other bit left alone. Mode 1-2 (the slots the HLE models,
 * see cellGcmSys.c): vblank 0x2, flip 0x10, second vblank 0x800, queue 0x40, user 0x80. SetGraphicsHandler
 * stores its pointer but touches no bit; 0x4 is only the start value. The words are guest memory, so big
 * endian, and the stores are plain `stw`: a registration racing another keeps both bits only if the RMW
 * is atomic on the host.
 *
 *   mask   T6: the start value, each setter's own bit (set, re-register, NULL), foreign bits surviving,
 *          graphics never setting 0x4, a second init not rewriting the word, no driver info = no store
 *   race   two threads registering different handlers at once keep every bit, and the foreign ones
 *
 * Built by tools/test_gcm_handler_mask.py (the race mode with -fsanitize=thread where the compiler has it).
 */
#include "../cellGcmSys.c"
#include "gcm_fixture_stubs.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
/* The rest of the walker's world: no graphics backend, no lifted program. */
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
int  rsx_draw_engine_enabled(void) { return 0; }
rsx_backend* rsx_get_backend(void) { return NULL; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
u32  rsx_dsp_method_class(u32 method) { (void)method; return RSX_DSP_CLASS_TODO; }
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

/* The firmware's bits, mode 1-2 (dossier §1), and the word's start value. Spelled out here, not taken from
 * the module, so a wrong constant in the module cannot agree with its own test. */
#define BIT_VBLANK  0x00000002u
#define BIT_GRAPHICS 0x00000004u    /* the start value; no setter sets it */
#define BIT_FLIP    0x00000010u
#define BIT_QUEUE   0x00000040u
#define BIT_USER    0x00000080u
#define BIT_SECONDV 0x00000800u
#define START       0x00000004u
/* Bits no mode-1/2 setter owns: arbitrary ones, and the mode-0 slots' bits (vblank 0x1, flip 0x8, queue
 * 0x20, second vblank 0x400), which the mode-1/2 setters must not touch either. */
#define FOREIGN     (0xC0000100u | 0x1u | 0x8u | 0x20u | 0x400u)

#define DI          0x00600000u
#define WORD_EA     (DI + 0x12C0u)
#define OPD_A       0x00000100u
#define OPD_B       0x00000200u

/* The word as a reader on another thread sees it: one atomic load of the big-endian cell. */
static uint32_t word(void)
{
    return ps3_bswap32(__atomic_load_n((uint32_t*)(vm_base + WORD_EA), __ATOMIC_RELAXED));
}
static void seed(uint32_t v) { vm_write32(WORD_EA, v); }

static void set_flip(u32 opd)    { cellGcmSetFlipHandler((CellGcmFlipHandler)(uintptr_t)opd); }
static void set_vblank(u32 opd)  { cellGcmSetVBlankHandler((CellGcmVBlankHandler)(uintptr_t)opd); }
static void set_secondv(u32 opd) { cellGcmSetSecondVHandler((CellGcmSecondVHandler)(uintptr_t)opd); }
static void set_user(u32 opd)    { cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)opd); }
static void set_queue(u32 opd)   { cellGcmSetQueueHandler((CellGcmQueueHandler)(uintptr_t)opd); }
static void set_graphics(u32 opd){ cellGcmSetGraphicsHandler((CellGcmGraphicsHandler)(uintptr_t)opd); }

static int fails;
static void check(int ok, const char* what, uint32_t got, uint32_t want)
{
    if (ok) printf("[PASS] %s\n", what);
    else    printf("[FAIL] %s: word is 0x%08X, wanted 0x%08X\n", what, got, want);
    fails += !ok;
}
#define CHECK_WORD(want, what) check(word() == (want), what, word(), (want))

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00400000u
#define CMD_SIZE  0x00080000u
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

static int run_mask(void)
{
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    /* Driver info exists before the init body runs (lv2 hands it out), and the word holds anything. */
    g_gcm_driver_info_ea = DI;
    seed(0xFFFFFFFFu);
    (void)cellGcmSetupContext(host_alloc(4, 4), CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    CHECK_WORD(START, "after init the word is the start value 4");

    /* A second init is refused (the firmware's thread-already-created gate) and writes nothing. */
    seed(START | FOREIGN);
    check(cellGcmInit(0x10000, 0, 0) == CELL_GCM_ERROR_FAILURE, "a second init is refused", 0, 0);
    CHECK_WORD(START | FOREIGN, "a refused init leaves the word alone");

    static const struct { const char* name; void (*set)(u32); uint32_t bit; } h[] = {
        { "vblank",  set_vblank,  BIT_VBLANK  },
        { "flip",    set_flip,    BIT_FLIP    },
        { "secondv", set_secondv, BIT_SECONDV },
        { "queue",   set_queue,   BIT_QUEUE   },
        { "user",    set_user,    BIT_USER    },
    };
    char what[96];
    for (unsigned i = 0; i < sizeof h / sizeof h[0]; i++) {
        const uint32_t base = START | FOREIGN;
        seed(base);
        h[i].set(OPD_A);
        snprintf(what, sizeof what, "%s: registering raises its bit 0x%X only", h[i].name, h[i].bit);
        CHECK_WORD(base | h[i].bit, what);
        h[i].set(OPD_B);
        snprintf(what, sizeof what, "%s: registering again keeps the bit, foreign bits untouched", h[i].name);
        CHECK_WORD(base | h[i].bit, what);
        h[i].set(0);
        snprintf(what, sizeof what, "%s: NULL clears its bit only", h[i].name);
        CHECK_WORD(base, what);
        h[i].set(0);
        snprintf(what, sizeof what, "%s: NULL again changes nothing", h[i].name);
        CHECK_WORD(base, what);

        /* The start value is a bit like any other to a setter that does not own it. */
        seed(FOREIGN);
        h[i].set(OPD_A);
        h[i].set(0);
        snprintf(what, sizeof what, "%s: a word without the start value never gains 0x4", h[i].name);
        CHECK_WORD(FOREIGN, what);
    }

    /* The graphics handler is stored but owns no bit: not set, not cleared, 0x4 stays what it was. */
    seed(START | FOREIGN);
    set_graphics(OPD_A);
    CHECK_WORD(START | FOREIGN, "graphics: registering changes no bit");
    set_graphics(0);
    CHECK_WORD(START | FOREIGN, "graphics: NULL clears no bit (0x4 stays)");
    seed(FOREIGN);
    set_graphics(OPD_A);
    CHECK_WORD(FOREIGN, "graphics: registering does not set 0x4");
    set_graphics(0);

    /* All five at once, cleared in another order: every step moves only its own bit. */
    seed(START | FOREIGN);
    uint32_t want = START | FOREIGN;
    for (unsigned i = 0; i < 5; i++) { h[i].set(OPD_A); want |= h[i].bit; }
    CHECK_WORD(want, "all five registered: start value, foreign and all five bits");
    for (int i = 4; i >= 0; i--) {
        h[i].set(0);
        want &= ~h[i].bit;
        snprintf(what, sizeof what, "all five: clearing %s leaves the rest", h[i].name);
        CHECK_WORD(want, what);
    }
    CHECK_WORD(START | FOREIGN, "all cleared: start value and foreign bits as they were");

    /* No driver info published: the setters touch no guest word at all. */
    seed(START | FOREIGN);
    g_gcm_driver_info_ea = 0;
    set_flip(OPD_A);
    set_user(OPD_A);
    g_gcm_driver_info_ea = DI;
    CHECK_WORD(START | FOREIGN, "without driver info a setter stores nothing");

    printf("[gcm_handler_mask] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}

/* Two threads, different handlers, no ordering between them. Each thread checks, right after every one of
 * its own registrations, that its bit is as that call left it and that the foreign bits are intact: a
 * non-atomic read-modify-write loses an update across the window between its load and its store, and the
 * thread whose bit was overwritten is the one that sees it. No sleep, no yield: the threads start off one
 * atomic flag and run flat out. */
enum { ROUNDS = 20000 };
static atomic_int g_go, g_bad;
static uint32_t g_a_bits, g_b_bits;

static void verify(uint32_t bit, int want_set)
{
    const uint32_t w = word();
    if (((w & bit) != 0) != want_set || (w & FOREIGN) != FOREIGN) atomic_store(&g_bad, (int)w | 1);
}
static void* thread_a(void* arg)
{
    (void)arg;
    while (!atomic_load(&g_go)) {}
    for (int r = 0; r < ROUNDS; r++) {
        set_flip(OPD_A);  verify(BIT_FLIP, 1);
        set_queue(OPD_A); verify(BIT_QUEUE, 1);
        set_user(OPD_A);  verify(BIT_USER, 1);
        set_user(0);      verify(BIT_USER, 0);
        set_queue(0);     verify(BIT_QUEUE, 0);
        set_flip(0);      verify(BIT_FLIP, 0);
    }
    set_flip(OPD_A); set_queue(OPD_A); set_user(OPD_A);
    return NULL;
}
static void* thread_b(void* arg)
{
    (void)arg;
    while (!atomic_load(&g_go)) {}
    for (int r = 0; r < ROUNDS; r++) {
        set_vblank(OPD_B);  verify(BIT_VBLANK, 1);
        set_secondv(OPD_B); verify(BIT_SECONDV, 1);
        set_secondv(0);     verify(BIT_SECONDV, 0);
        set_vblank(0);      verify(BIT_VBLANK, 0);
    }
    set_vblank(OPD_B); set_secondv(OPD_B);
    return NULL;
}

static int run_race(void)
{
    vm_base = (uint8_t*)calloc(1, 0x00800000u);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = 0x00800000u;
    g_gcm_driver_info_ea = DI;
    seed(START | FOREIGN);
    g_a_bits = BIT_FLIP | BIT_QUEUE | BIT_USER;
    g_b_bits = BIT_VBLANK | BIT_SECONDV;

    pthread_t a, b;
    if (pthread_create(&a, NULL, thread_a, NULL) || pthread_create(&b, NULL, thread_b, NULL)) {
        puts("FAIL: pthread_create");
        return 1;
    }
    atomic_store(&g_go, 1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    check(!atomic_load(&g_bad), "no registration ever lost another thread's bit or a foreign bit",
          (uint32_t)atomic_load(&g_bad) & ~1u, 0);
    CHECK_WORD(START | FOREIGN | g_a_bits | g_b_bits, "after the race every bit of both threads is set");
    printf("[gcm_handler_mask race] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    int rc = 2;
    if (argc >= 2 && !strcmp(argv[1], "mask")) rc = run_mask();
    else if (argc >= 2 && !strcmp(argv[1], "race")) rc = run_race();
    else fputs("usage: test_gcm_handler_mask mask|race\n", stderr);
    fflush(stdout);
    return rc;
}

/* The HLE's flip and vblank handler delivery against the firmware's OPD contract
 * (FW 4.93 libgcm_sys, dossier GCM-CALLBACK-OPD-CONTRACT.md §2, §4, §6 T1-T3).
 *
 * The firmware keeps the registered OPD pointer and dereferences {code, toc} from guest memory at every
 * delivery (0x0200851C..0x02008548 for the flip slot); it has no repair path, so a descriptor that was
 * changed after registration is a defect of the writer, not something delivery papers over. The HLE
 * delivers through ppu_guest_call, which reads the descriptor at each call too and ends in the named
 * stop unresolved-guest-callback for a code word no lifted function starts at. This fixture drives that
 * through cellGcmSys.c's own pump, against the real runtime/ppu/ppu_loader.cpp, with a synthetic lifted
 * function registered by hand. No title or firmware bytes.
 *
 *   intact      T1: both handlers run, r2 = the OPD's own toc, the arguments are as implemented today
 *   zero-flip   T2: the flip OPD's code word zeroed after registration: the stop, never a skip or a call
 *   zero-vblank T2: the same for the vblank OPD
 *   swap-flip   T3: {toc, code} swapped (ppu.opd_swap_fixup off): the stop with code = the toc, never
 *               the swap redirect
 *   swap-vblank T3: the same for the vblank OPD
 *
 * Each stop mode ends by the stop (exit status 70, one PS3STOP line); tools/test_gcm_handler_contract.py
 * checks the line's fields. A mode that comes back from the pump prints RETURNED and fails.
 *
 * Built by tools/test_gcm_handler_contract.py with ppu_loader.cpp and test_gcm_handler_contract_stubs.cpp. */
#include "../cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

uint8_t* vm_base;
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
/* The walker's other consumers: no graphics backend. */
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }
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

extern void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern uint64_t ppu_guest_call(uint32_t opd, uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t, uint64_t);

/* Above the null page (0..0xFFFF is a named stop of its own). tools/test_gcm_handler_contract.py
 * expects exactly these. */
#define OPD_FLIP     0x00020000u
#define OPD_VBLANK   0x00020010u
#define CODE_FLIP    0x00030000u
#define CODE_VBLANK  0x00030010u
#define TOC_FLIP     0x00028000u
#define TOC_VBLANK   0x00029000u

enum { H_FLIP, H_VBLANK, H_COUNT };
static struct { unsigned calls; uint32_t r2; uint64_t r3; } rec[H_COUNT];

/* The synthetic lifted handlers: they record what the delivery handed them. */
static void lifted(int which, ppu_context* ctx)
{
    rec[which].calls++;
    rec[which].r2 = (uint32_t)ctx->gpr[2];
    rec[which].r3 = ctx->gpr[3];
    printf("HANDLER ran %s\n", which == H_FLIP ? "flip" : "vblank");
}
static void lifted_flip(ppu_context* ctx) { lifted(H_FLIP, ctx); }
static void lifted_vblank(ppu_context* ctx) { lifted(H_VBLANK, ctx); }

static void deliver(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                    uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

/* Registration as a title does it: an OPD in guest memory, the lifted function behind its code word. */
static void register_handlers(void)
{
    vm_base = mmap(NULL, 0x100010000ull, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (vm_base == MAP_FAILED) { perror("mmap"); exit(1); }
    g_ps3_guest_caller = deliver;
    vm_write32(OPD_FLIP, CODE_FLIP);     vm_write32(OPD_FLIP + 4, TOC_FLIP);
    vm_write32(OPD_VBLANK, CODE_VBLANK); vm_write32(OPD_VBLANK + 4, TOC_VBLANK);
    ppu_register_function(CODE_FLIP, lifted_flip);
    ppu_register_function(CODE_VBLANK, lifted_vblank);
    cellGcmSetFlipHandler((CellGcmFlipHandler)(uintptr_t)OPD_FLIP);
    cellGcmSetVBlankHandler((CellGcmVBlankHandler)(uintptr_t)OPD_VBLANK);
    s_display_buffer_set[1] = 1;
    /* ppu.opd_swap_fixup is off in every test: T3 is about the HLE having no repair of its own. */
    if (ps3_cfg_ppu_opd_swap_fixup()) { puts("FAIL: ppu.opd_swap_fixup is on"); exit(1); }
}

/* T1 */
static int run_intact(void)
{
    register_handlers();

    /* A flip requested outside the stream runs the handler at once with r3 = 0, as implemented
     * (cellGcmSetFlipCommand, #69). The firmware never calls the flip handler from the request: only the
     * dispatcher does, from the completion event (dossier §2), so this call has no firmware counterpart. */
    check(cellGcmSetFlipCommand(1) == CELL_OK, "a flip request is accepted");
    check(rec[H_FLIP].calls == 1 && rec[H_FLIP].r3 == 0, "the request-time flip callback gets r3 = 0");
    check(rec[H_FLIP].r2 == TOC_FLIP, "and r2 = the flip OPD's toc");

    /* The tick raises vblank and completes the flip; the pump delivers both. */
    cellGcmTickVBlank();
    cellGcmTickFlip();
    ppu_gcm_pump();
    check(rec[H_FLIP].calls == 2, "the pump runs the flip handler for the completed flip");
    check(rec[H_FLIP].r2 == TOC_FLIP, "flip: r2 = the flip OPD's toc");
    /* The pump's completion delivery passes r3 = 1: the firmware dispatcher's 0x10 flag, which calls the
     * slot at state+0x1C with r3 = 1 (dossier §2, 0x10 -> +0x1C; the mode 1-2 slot the HLE models, see
     * cellGcmSys.c). */
    check(rec[H_FLIP].r3 == 1, "flip: the pump's completion delivery gets r3 = 1 (dispatcher, 0x10 slot)");
    check(rec[H_VBLANK].calls == 1, "the pump runs the vblank handler once for the one vblank");
    check(rec[H_VBLANK].r2 == TOC_VBLANK, "vblank: r2 = the vblank OPD's toc");
    /* #69: the HLE passes the vblank count; the firmware's dispatcher passes the slot's index, 1 for the
     * 0x2 flag (dossier §2, §4 divergence 2). The two agree only because one vblank has been raised, so
     * this is pinned as implemented until #69 decides. */
    check(rec[H_VBLANK].r3 == 1, "vblank: r3 is the vblank count (as implemented; #69)");
    printf("[gcm_handler_contract] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

/* T2, T3: change the descriptor after registration, raise the cause, pump. */
static int run_corrupt(const char* mode)
{
    register_handlers();
    const int flip = strstr(mode, "flip") != NULL;
    const uint32_t opd = flip ? OPD_FLIP : OPD_VBLANK;
    const uint32_t code = flip ? CODE_FLIP : CODE_VBLANK;
    const uint32_t toc = flip ? TOC_FLIP : TOC_VBLANK;
    /* The title requests its flip while the descriptor is still good (the handler runs once, at request
     * time); the clobbering store comes after, and the completion is what the pump must not deliver. */
    if (flip && cellGcmSetFlipCommand(1) != CELL_OK) { puts("FAIL: flip request"); return 1; }
    if (!strncmp(mode, "zero", 4)) {
        vm_write32(opd, 0);                       /* the code word, as a clobbering store leaves it */
    } else {
        vm_write32(opd, toc);                     /* {toc, code}: the swapped shape */
        vm_write32(opd + 4, code);
    }
    if (flip) {
        cellGcmTickFlip();
    } else {
        cellGcmTickVBlank();
    }
    printf("[gcm_handler_contract] %s: pumping\n", mode);
    ppu_gcm_pump();
    printf("RETURNED handler calls flip=%u vblank=%u\n", rec[H_FLIP].calls, rec[H_VBLANK].calls);
    return 1;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fputs("usage: test_gcm_handler_contract <mode>\n", stderr); return 2; }
    if (!strcmp(argv[1], "intact")) return run_intact();
    if (!strcmp(argv[1], "zero-flip") || !strcmp(argv[1], "zero-vblank") ||
        !strcmp(argv[1], "swap-flip") || !strcmp(argv[1], "swap-vblank"))
        return run_corrupt(argv[1]);
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}

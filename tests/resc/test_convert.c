/*
 * ps3recomp - tests/resc/test_convert.c
 *
 * cellRescSetConvertAndFlip(ctx, idx): the FIFO stream it appends to the command context, built
 * from RESC state (libresc FW 4.93, body 0x02004D4C) and the libgcm_sys flip command (0x02008948).
 *
 * Synthetic configuration, nothing recorded: a 1920x1080 display mode, default Dsts records,
 * a 1920x1080 source in format 0x85, buffers at made-up RSX offsets. The expected stream is
 * 185 words: every field was worked out from the firmware's formulas for this configuration
 * (log2 of the size in the surface format, the viewport floats, the texture words, the three
 * buffer offsets), and the structure (method order, reservation points) is the firmware's.
 * The vertex program's 12 microcode words are checked as instructions, not as bytes.
 *
 *   ctest --test-dir build/dev -R resc_convert --output-on-failure
 */
#include "cellResc.h"
#include "cellGcmSys.h"
#include "resc_convert.h"
#include "ps3emu/guest_call.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

extern uint32_t ppu_vm_size;
extern uint32_t ppu_hle_inject_base;
extern void vm_write32(uint64_t addr, uint32_t v);
extern uint32_t vm_read32(uint64_t addr);
extern u32 cellGcm_display_buffer_count(void);
extern void cellGcm_rsx_process_fifo(void);

uint8_t* vm_base = NULL;

#define CFG_EA     0x2000u
#define SRC_EA     0x3000u
#define IO_ADDR    0x00100000u              /* IO window, io offset 0 */
#define IO_SIZE    0x00400000u
#define RING       (IO_ADDR + 0x1000u)      /* the context's buffer: io 0x1000 */
#define RING_BYTES 0x00020000u
#define VERTEX_EA  (IO_ADDR + 0x80000u)     /* io offset 0x80000 */
#define COLOR_EA   0xC0200000u              /* local offset 0x200000 */
#define FRAG_EA    0xC0600000u              /* local offset 0x600000 */
#define HEAP       0x00800000u

static int g_fail;
#define CHECK(c, ...)                                                          \
    do {                                                                       \
        if (!(c)) {                                                            \
            g_fail++;                                                          \
            printf("[FAIL] %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static uint32_t gr32(uint32_t ea) { return vm_read32(ea); }
static void gw32(uint32_t ea, uint32_t v) { vm_write32(ea, v); }

static uint32_t g_heap = HEAP;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

static uint32_t g_ctx;   /* the CellGcmContextData EA */

/* ---- the scene ------------------------------------------------------------------------------ */

static void scene_init(u32 support, u32 ratio, u32 pal, u32 interlace, u32 flip)
{
    cellRescExit();
    gw32(CFG_EA + 0, 28);   gw32(CFG_EA + 4, 2);      gw32(CFG_EA + 8, support);
    gw32(CFG_EA + 12, ratio); gw32(CFG_EA + 16, pal); gw32(CFG_EA + 20, interlace);
    gw32(CFG_EA + 24, flip);
    CHECK(cellRescInit((const CellRescInitConfig*)(uintptr_t)CFG_EA) == 0, "Init");
}

static void set_src(s32 idx, u32 format, u32 pitch, u32 w, u32 h, u32 offset)
{
    gw32(SRC_EA + 0, format);
    gw32(SRC_EA + 4, pitch);
    vm_write32(SRC_EA + 8, (w << 16) | h);   /* u16 width, u16 height, big-endian */
    gw32(SRC_EA + 12, offset);
    CHECK(cellRescSetSrc(idx, (const CellRescSrc*)(uintptr_t)SRC_EA) == 0, "SetSrc");
}

/* Fresh GCM context with the firmware's layout: the ring at io 0x1000, a RET at io 0 (the driver
 * default-state subroutine the flip command CALLs), get == put at the ring start. */
static void scene_gcm(void)
{
    cellGcmTerminate();
    u32 ctx_out = host_alloc(4, 4);
    g_ctx = cellGcmSetupContext(ctx_out, RING_BYTES, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    gw32(IO_ADDR, 0x00020000u);
    gw32(g_ctx + 0, RING);
    gw32(g_ctx + 4, RING + RING_BYTES);
    gw32(g_ctx + 8, RING);
    cellGcm_syscall_set_fifo(0x1000u, 0x1000u);
}

static void scene_1080p(void)
{
    scene_gcm();
    scene_init(CELL_RESC_1920x1080, 0, 0, 0, 0);
    CHECK(cellRescSetDisplayMode(CELL_RESC_1920x1080) == 0, "SetDisplayMode");
    CHECK(cellRescSetBufferAddress((void*)(uintptr_t)COLOR_EA, (void*)(uintptr_t)VERTEX_EA,
                                   (void*)(uintptr_t)FRAG_EA) == 0, "SetBufferAddress");
    set_src(2, 0x85, 0x1E00, 1920, 1080, 0x100000);
}

/* The 185 words of frame 0. Words 132..143 are the vertex program (checked separately). */
#define VP_FIRST 132
#define VP_COUNT 12
static const uint32_t k_frame0[185] = {
    0x00040324, 0x01010101, 0x00040A70, 0x00000000, 0x00040304, 0x00000000,
    0x00040310, 0x00000000, 0x0004036C, 0x00000000, 0x00040374, 0x00000000,
    0x0004183C, 0x00000000, 0x00040380, 0x00000000, 0x00040A74, 0x00000000,
    0x00040A68, 0x00000000, 0x00040328, 0x00000000, 0x00040348, 0x00000000,
    0x00041EE4, 0x00000000, 0x00041EE8, 0x00000000, 0x00040300, 0x00000001,
    0x00040368, 0x00001D01, 0x00041FC0, 0x00000000, 0x000808C0, 0x07800000,
    0x04380000, 0x00080A00, 0x07800000, 0x04380000, 0x00080394, 0x00000000,
    0x3F800000, 0x00200A20, 0x44700000, 0x44070000, 0x3F000000, 0x00000000,
    0x44700000, 0xC4070000, 0x3F000000, 0x00000000, 0x00200A20, 0x44700000,
    0x44070000, 0x3F000000, 0x00000000, 0x44700000, 0xC4070000, 0x3F000000,
    0x00000000, 0x00041740, 0x00001822, 0x00041680, 0x00080000, 0x00041760,
    0x00001822, 0x000416A0, 0x00080008, 0x00041710, 0x00000000, 0x00041714,
    0x00000000, 0x00041714, 0x00000000, 0x00041714, 0x00000000, 0x00040194,
    0xFEED0000, 0x0004018C, 0xFEED0000, 0x000801B4, 0xFEED0000, 0xFEED0000,
    0x00040198, 0xFEED0000, 0x00180208, 0x0A0A0148, 0x00002000, 0x00200000,
    0x00000000, 0x00000000, 0x00000040, 0x0004022C, 0x00000040, 0x00100280,
    0x00000040, 0x00000040, 0x00000000, 0x00000000, 0x00040220, 0x00000001,
    0x000402B8, 0x00000000, 0x00080200, 0x07800000, 0x04380000, 0x00041D88,
    0x00001438, 0x00081A00, 0x00100000, 0x00018529, 0x00041A18, 0x07800438,
    0x00041840, 0x00101E00, 0x00041A10, 0x0000AAE4, 0x00041A0C, 0x80000780,
    0x00041A08, 0x10050404, 0x00041A14, 0x02022000, 0x00041A1C, 0x00000000,
    0x00041FD8, 0x00000001, 0x00081E9C, 0x00000000, 0x00000000, 0x00300B80,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00041FF0, 0x00000301, 0x00041EF8, 0x0020FFFF, 0x000408E4, 0x00600001,
    0x00041FF4, 0x00004020, 0x00040B40, 0x00000001, 0x00041D60, 0x02000440,
    0x400C1714, 0x00000000, 0x00000000, 0x00000000, 0x00041808, 0x00000008,
    0x00041814, 0x03000000, 0x00041808, 0x00000000, 0x0004E944, 0x00000000,
    0x00040060, 0x56616661, 0x00040064, 0x00000030, 0x0004006C, 0x00000000,
    0x00040064, 0x00000030, 0x00040068, 0x00000001, 0x00000002, 0x00040064,
    0x00000010, 0x0004006C, 0xFFFFFFFF, 0x0004E924, 0x8000010F,
};

/* The word indices that differ between frame 0 and frame 1 (the colour buffer toggles). */
#define W_COLOR_OFFSET 89
#define W_FLIP_BUFFER  167

/* Compare `n` words of guest memory at `at` with the expected frame (the vertex program aside). */
static int compare_stream(uint32_t at, const uint32_t* want, unsigned n, const char* what)
{
    int bad = 0;
    for (unsigned i = 0; i < n; i++) {
        if (i >= VP_FIRST && i < VP_FIRST + VP_COUNT)
            continue;
        const uint32_t got = gr32(at + i * 4u);
        if (got != want[i]) {
            if (bad++ < 6)
                printf("[FAIL] %s: word %u = 0x%08X, want 0x%08X\n", what, i, got, want[i]);
        }
    }
    if (bad) g_fail++;
    return bad == 0;
}

/* The three vertex-program instructions: MOV o[dst].mask, v[input]; only the last ends. */
static void check_vertex_program(uint32_t at, const char* what)
{
    static const u32 dst[3] = { 0, 7, 8 }, mask[3] = { 0xF, 0xC, 0xC }, input[3] = { 0, 8, 9 };
    for (int i = 0; i < 3; i++) {
        const uint32_t d1 = gr32(at + (VP_FIRST + i * 4 + 1) * 4u), d3 = gr32(at + (VP_FIRST + i * 4 + 3) * 4u);
        CHECK(((d1 >> 22) & 0x1F) == 1u, "%s: instr %d vector opcode %u (MOV)", what, i, (d1 >> 22) & 0x1F);
        CHECK(((d1 >> 8) & 0xF) == input[i], "%s: instr %d input %u", what, i, (d1 >> 8) & 0xF);
        CHECK(((d3 >> 2) & 0x1F) == dst[i], "%s: instr %d dst %u", what, i, (d3 >> 2) & 0x1F);
        CHECK(((d3 >> 13) & 0xF) == mask[i], "%s: instr %d mask 0x%X", what, i, (d3 >> 13) & 0xF);
        CHECK((d3 & 1u) == (i == 2 ? 1u : 0u), "%s: instr %d end bit", what, i);
    }
}

static void test_stream_and_toggle(void)
{
    scene_1080p();

    /* SetBufferAddress: the colour buffers and what it registered with GCM. */
    CellGcmDisplayInfo* d0 = cellGcmGetDisplayBufferByFlipIndex(0);
    CellGcmDisplayInfo* d1 = cellGcmGetDisplayBufferByFlipIndex(1);
    CHECK(d0->offset == 0x200000u && d1->offset == 0xA70000u, "display buffer offsets 0x%X 0x%X", d0->offset, d1->offset);
    CHECK(d0->pitch == 0x2000u && d0->width == 1920u && d0->height == 1080u, "display buffer 0 geometry");
    CHECK(cellGcm_display_buffer_count() == 2u, "two display buffers, got %u", cellGcm_display_buffer_count());

    /* Frame 0. */
    const uint32_t at0 = gr32(g_ctx + 8);
    cellRescResetFlipStatus();
    CHECK(cellRescSetConvertAndFlip((void*)(uintptr_t)g_ctx, 2) == 0, "ConvertAndFlip frame 0");
    CHECK(cellRescGetFlipStatus() == 1, "queueing the flip does not complete it: status %d, want WAITING (1)",
          cellRescGetFlipStatus());
    const uint32_t cur0 = gr32(g_ctx + 8);
    CHECK(cur0 - at0 == 185u * 4u, "frame 0 appended %u bytes, want %u", cur0 - at0, 185u * 4u);
    compare_stream(at0, k_frame0, 185, "frame 0");
    check_vertex_program(at0, "frame 0");

    /* Frame 1: the other colour buffer and queue entry; nothing else changes. */
    uint32_t want[185];
    memcpy(want, k_frame0, sizeof want);
    want[W_COLOR_OFFSET] = 0xA70000u;
    want[W_FLIP_BUFFER] = 1u;
    CHECK(cellRescSetConvertAndFlip((void*)(uintptr_t)g_ctx, 2) == 0, "ConvertAndFlip frame 1");
    CHECK(gr32(g_ctx + 8) - cur0 == 185u * 4u, "frame 1 length");
    compare_stream(cur0, want, 185, "frame 1");

    /* Frame 2: back to buffer 0, the stream of frame 0. */
    const uint32_t cur1 = gr32(g_ctx + 8);
    CHECK(cellRescSetConvertAndFlip((void*)(uintptr_t)g_ctx, 2) == 0, "ConvertAndFlip frame 2");
    compare_stream(cur1, k_frame0, 185, "frame 2");
}

/* The walker takes each frame through (the CALL returns to the stream, 0xE924 presents what
 * 0xE944 queued), one pass per frame: buffer 0 after frame 0, buffer 1 after frame 1, ... */
static void test_present_order(void)
{
    scene_1080p();
    uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    for (unsigned frame = 0; frame < 4; frame++) {
        CHECK(cellRescSetConvertAndFlip((void*)(uintptr_t)g_ctx, 2) == 0, "ConvertAndFlip %u", frame);
        const uint32_t put = gr32(g_ctx + 8) - IO_ADDR;
        vm_write32(ctrl + 0, put);
        for (int pass = 0; pass < 8 && vm_read32(ctrl + 4) != put; pass++)
            cellGcm_rsx_process_fifo();
        CHECK(vm_read32(ctrl + 4) == put, "frame %u drained (get 0x%X, put 0x%X)", frame, vm_read32(ctrl + 4), put);
        CHECK(cellGcmGetCurrentDisplayBufferId() == (frame & 1u), "frame %u presented buffer %u, want %u",
              frame, cellGcmGetCurrentDisplayBufferId(), frame & 1u);
    }
}

/* A source in a float format is sampled unfiltered and its format word follows the source. */
static void test_float_source(void)
{
    scene_1080p();
    set_src(5, 0x9B, 0x3C00, 1920, 1080, 0x4000);
    const uint32_t at = gr32(g_ctx + 8);
    CHECK(cellRescSetConvertAndFlip((void*)(uintptr_t)g_ctx, 5) == 0, "ConvertAndFlip");
    CHECK(gr32(at + 110 * 4) == 0x4000u, "texture offset");
    CHECK(gr32(at + 111 * 4) == 0x00019B29u, "texture format word 0x%08X", gr32(at + 111 * 4));
    CHECK(gr32(at + 115 * 4) == 0x00103C00u, "texture pitch word");
    CHECK(gr32(at + 123 * 4) == 0x01012000u, "float format filter word 0x%08X", gr32(at + 123 * 4));
}

/* The buffer-full callback fires at the firmware's reservation points: with room for 70 words the
 * first group that does not fit is the 8-word vertex-cache group, after word 68. */
static int g_cb_calls;
static uint32_t g_cb_ctx, g_cb_words;
static uint32_t g_seg2;
static void fake_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)opd; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    g_cb_calls++;
    g_cb_ctx = (uint32_t)a0;
    g_cb_words = (uint32_t)a1;
    gw32(g_cb_ctx + 0, g_seg2);
    gw32(g_cb_ctx + 4, g_seg2 + 0x10000u);
    gw32(g_cb_ctx + 8, g_seg2);
}

static void test_callback_points(void)
{
    scene_1080p();
    const uint32_t opd = host_alloc(8, 8);
    gw32(opd, 0x00123400u);      /* a title's own callback: not the runtime's sentinel */
    gw32(opd + 4, 0);
    gw32(g_ctx + 0xC, opd);
    gw32(g_ctx + 4, RING + 70u * 4u);
    g_seg2 = IO_ADDR + 0x40000u;
    g_cb_calls = 0;
    g_ps3_guest_caller = fake_caller;

    CHECK(cellRescSetConvertAndFlip((void*)(uintptr_t)g_ctx, 2) == 0, "ConvertAndFlip");
    g_ps3_guest_caller = NULL;
    CHECK(g_cb_calls == 1, "callback called %d time(s), want 1", g_cb_calls);
    CHECK(g_cb_ctx == g_ctx && g_cb_words == 8u, "callback(ctx 0x%X, words %u), want (0x%X, 8)", g_cb_ctx, g_cb_words, g_ctx);
    CHECK(gr32(g_ctx + 8) - g_seg2 == (185u - 69u) * 4u, "second segment holds %u words", (gr32(g_ctx + 8) - g_seg2) / 4u);
    /* The first segment holds words 0..68, the second 69..184: together the stream. */
    int same = 1;
    for (unsigned i = 0; i < 185; i++) {
        if (i >= VP_FIRST && i < VP_FIRST + VP_COUNT)
            continue;
        const uint32_t got = i < 69 ? gr32(RING + i * 4u) : gr32(g_seg2 + (i - 69) * 4u);
        if (got != k_frame0[i]) same = 0;
    }
    CHECK(same, "segments joined equal the stream");
}

/* ---- the quad and the vertex program ------------------------------------------------------ */

static void quad(u32 mode, u32 ratio, u32 size, float h, float v, const u32 want[24], const char* what)
{
    u32 got[24];
    resc_quad_vertices(mode, ratio, size, h, v, got);
    for (int i = 0; i < 24; i++)
        if (got[i] != want[i]) {
            g_fail++;
            printf("[FAIL] quad %s: float %d = 0x%08X, want 0x%08X\n", what, i, got[i], want[i]);
            return;
        }
}

static void test_quad(void)
{
    const u32 M1 = 0xBF800000u, P1 = 0x3F800000u, Z = 0;
    /* Fullscreen at unit aspect: the whole texture on the whole screen. */
    const u32 plain[24] = { M1, P1, Z, Z, Z, Z,   P1, P1, P1, Z, P1, Z,
                            P1, M1, P1, P1, P1, P1,   M1, M1, Z, P1, Z, P1 };
    quad(CELL_RESC_1280x720, CELL_RESC_FULLSCREEN, 28, 1.0f, 1.0f, plain, "720p");
    quad(CELL_RESC_1920x1080, CELL_RESC_LETTERBOX, 28, 1.0f, 1.0f, plain, "1080p ignores the ratio mode");
    /* AdjustAspectRatio(2.0, 0.5): u 0.25..0.75, v -0.5..1.5. */
    {
        const u32 ulo = 0x3E800000u, uhi = 0x3F400000u, vlo = 0xBF000000u, vhi = 0x3FC00000u;
        const u32 w[24] = { M1, P1, ulo, vlo, Z, Z,   P1, P1, uhi, vlo, P1, Z,
                            P1, M1, uhi, vhi, P1, P1,   M1, M1, ulo, vhi, Z, P1 };
        quad(CELL_RESC_1280x720, CELL_RESC_FULLSCREEN, 28, 2.0f, 0.5f, w, "aspect 2.0/0.5");
    }
    /* 576p letterbox: v spans 0.5 -+ 0.66666669 (config size 28), or that over 1.0620916 (size 20). */
    {
        const u32 vhi = 0x3F955556u, vlo = 0xBE2AAAACu;
        const u32 w[24] = { M1, P1, Z, vlo, Z, Z,   P1, P1, P1, vlo, P1, Z,
                            P1, M1, P1, vhi, P1, P1,   M1, M1, Z, vhi, Z, P1 };
        quad(CELL_RESC_720x576, CELL_RESC_LETTERBOX, 28, 1.0f, 1.0f, w, "576 letterbox size 28");
    }
    {
        const u32 vhi = 0x3F905838u, vlo = 0xBE02C1C4u;
        const u32 w[24] = { M1, P1, Z, vlo, Z, Z,   P1, P1, P1, vlo, P1, Z,
                            P1, M1, P1, vhi, P1, P1,   M1, M1, Z, vhi, Z, P1 };
        quad(CELL_RESC_720x576, CELL_RESC_LETTERBOX, 20, 1.0f, 1.0f, w, "576 letterbox size 20");
    }
    /* 480p panscan: u spans 0.5 -+ 0.375. */
    {
        const u32 ulo = 0x3E000000u, uhi = 0x3F600000u;
        const u32 w[24] = { M1, P1, ulo, Z, Z, Z,   P1, P1, uhi, Z, P1, Z,
                            P1, M1, uhi, P1, P1, P1,   M1, M1, ulo, P1, Z, P1 };
        quad(CELL_RESC_720x480, CELL_RESC_PANSCAN, 28, 1.0f, 1.0f, w, "480 panscan");
    }
}

static void test_vertex_buffer_and_aspect(void)
{
    scene_1080p();
    /* SetBufferAddress wrote the quad into the vertex buffer, big-endian. */
    CHECK(gr32(VERTEX_EA + 0) == 0xBF800000u && gr32(VERTEX_EA + 4) == 0x3F800000u, "quad record 0 position");
    CHECK(gr32(VERTEX_EA + 24 + 8) == 0x3F800000u && gr32(VERTEX_EA + 72 + 20) == 0x3F800000u, "quad records 1 and 3");
    /* AdjustAspectRatio: range [0.5, 2.0] on both; an out-of-range value changes nothing. */
    CHECK((u32)cellRescAdjustAspectRatio(0.4f, 1.0f) == 0x80210304u, "h below range");
    CHECK((u32)cellRescAdjustAspectRatio(1.0f, 2.1f) == 0x80210304u, "v above range");
    CHECK((u32)cellRescAdjustAspectRatio(__builtin_nanf(""), 1.0f) == 0x80210304u, "NaN");
    CHECK(gr32(VERTEX_EA + 8) == 0u, "rejected adjust left the quad alone");
    CHECK(cellRescAdjustAspectRatio(2.0f, 2.0f) == 0, "h 2.0 v 2.0 accepted");
    CHECK(gr32(VERTEX_EA + 8) == 0x3E800000u, "quad rewritten: u0 0x%08X", gr32(VERTEX_EA + 8));
}

/* libresc 02006C70: SetBufferAddress copies the current convert program's microcode into the
 * fragment buffer. The stream names that buffer by offset only, so a buffer left as the title had it
 * (here 0xDEADBEEF; zeros in a real title) is a convert draw that samples nothing. The DC program is
 * one TEX instruction: opcode 0x17 after the RSX's halfword swap. */
static void test_fragment_microcode(void)
{
    for (u32 i = 0; i < 6; i++) gw32(FRAG_EA + i * 4u, 0xDEADBEEFu);
    scene_1080p();
    const u32 w0 = gr32(FRAG_EA);
    const u32 swapped = ((w0 & 0xFFFFu) << 16) | (w0 >> 16);
    CHECK(((swapped >> 24) & 0x3Fu) == 0x17u, "word 0 is a TEX instruction: 0x%08X", w0);
    CHECK(gr32(FRAG_EA + 4) != 0xDEADBEEFu && gr32(FRAG_EA + 8) != 0xDEADBEEFu &&
          gr32(FRAG_EA + 12) != 0xDEADBEEFu, "all four words of the program were written");
    CHECK(gr32(FRAG_EA + 16) == 0xDEADBEEFu, "the copy is the program's 0x10 bytes and no more");
    /* The DC program only: the interlaced E4 program has more microcode than is held, and its
     * SetConvertAndFlip stops, so the buffer is left alone. */
    for (u32 i = 0; i < 6; i++) gw32(FRAG_EA + i * 4u, 0xDEADBEEFu);
    scene_gcm();
    scene_init(CELL_RESC_1280x720, 0, 0, 1, 0);
    CHECK(cellRescSetDisplayMode(CELL_RESC_1280x720) == 0, "SetDisplayMode interlaced");
    CHECK(cellRescSetBufferAddress((void*)(uintptr_t)COLOR_EA, (void*)(uintptr_t)VERTEX_EA,
                                   (void*)(uintptr_t)FRAG_EA) == 0, "SetBufferAddress interlaced");
    CHECK(gr32(FRAG_EA) == 0xDEADBEEFu, "E4 microcode is not held, the buffer is untouched");
}

static void test_vertex_program_words(void)
{
    const u8 attr[3] = { 3, 9, 12 };
    u32 w[12];
    resc_vertex_program_words(attr, w);
    for (int i = 0; i < 3; i++) {
        CHECK(((w[i * 4 + 1] >> 8) & 0xF) == attr[i], "input slot of instruction %d", i);
        CHECK(((w[i * 4 + 1] >> 22) & 0x1F) == 1u, "MOV");
    }
    CHECK((w[3] & 1) == 0 && (w[7] & 1) == 0 && (w[11] & 1) == 1, "end bit");
}

/* ---- errors and stops ---------------------------------------------------------------------- */

static void test_errors(void)
{
    cellRescExit();
    CHECK((u32)cellRescSetBufferAddress((void*)(uintptr_t)COLOR_EA, (void*)(uintptr_t)VERTEX_EA,
                                        (void*)(uintptr_t)FRAG_EA) == 0x80210301u, "SetBufferAddress before init");
    CHECK((u32)cellRescAdjustAspectRatio(1.0f, 1.0f) == 0x80210301u, "AdjustAspectRatio before init");
    scene_gcm();
    scene_init(CELL_RESC_1280x720, 0, 0, 0, 0);
    CHECK(cellRescSetDisplayMode(CELL_RESC_1280x720) == 0, "SetDisplayMode");
    void* c = (void*)(uintptr_t)COLOR_EA; void* v = (void*)(uintptr_t)VERTEX_EA; void* f = (void*)(uintptr_t)FRAG_EA;
    CHECK((u32)cellRescSetBufferAddress(NULL, v, f) == 0x80210304u, "NULL colour");
    CHECK((u32)cellRescSetBufferAddress(c, NULL, f) == 0x80210304u, "NULL vertex");
    CHECK((u32)cellRescSetBufferAddress(c, v, NULL) == 0x80210304u, "NULL fragment");
    CHECK((u32)cellRescSetBufferAddress((void*)(uintptr_t)(COLOR_EA + 0x40), v, f) == 0x80210303u, "colour not 128-aligned");
    CHECK((u32)cellRescSetBufferAddress(c, (void*)(uintptr_t)(VERTEX_EA + 2), f) == 0x80210303u, "vertex not 4-aligned");
    CHECK((u32)cellRescSetBufferAddress(c, v, (void*)(uintptr_t)(FRAG_EA + 0x20)) == 0x80210303u, "fragment not 64-aligned");
    /* NULL beats misalignment: the firmware checks every pointer before any alignment. */
    CHECK((u32)cellRescSetBufferAddress((void*)(uintptr_t)(COLOR_EA + 0x40), v, NULL) == 0x80210304u, "NULL before alignment");
    CHECK(cellRescSetBufferAddress(c, v, f) == 0, "aligned");

    CHECK((u32)cellRescSetConvertAndFlip((void*)(uintptr_t)g_ctx, 8) == 0x80210304u, "source index 8");
    CHECK((u32)cellRescSetConvertAndFlip((void*)(uintptr_t)g_ctx, -1) == 0x80210304u, "source index -1");
    CHECK(gr32(g_ctx + 8) == RING, "a rejected call appended nothing");
}

static int run_child(const char* self, const char* mode, char* out, size_t cap)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "'%s' %s 2>&1", self, mode);
    FILE* p = popen(cmd, "r");
    if (!p) return -1;
    size_t n = fread(out, 1, cap - 1, p);
    out[n] = 0;
    int st = pclose(p);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -2;
}

static void check_stop(const char* self, const char* mode, const char* feature)
{
    char out[8192];
    int rc = run_child(self, mode, out, sizeof out);
    CHECK(rc == 70, "%s: exit status %d, want 70", mode, rc);
    char want[256];
    snprintf(want, sizeof want, "PS3STOP v=1 kind=gcm-unmodelled func=");
    CHECK(strstr(out, want) != NULL, "%s: no gcm-unmodelled stop in '%s'", mode, out);
    snprintf(want, sizeof want, "feature=%s", feature);
    CHECK(strstr(out, want) != NULL, "%s: no '%s' in '%s'", mode, want, out);
}

static void child_scene(u32 support, u32 mode, u32 pal, u32 interlace, u32 flip)
{
    scene_gcm();
    scene_init(support, 0, pal, interlace, flip);
    cellRescSetDisplayMode(mode);
    cellRescSetBufferAddress((void*)(uintptr_t)COLOR_EA, (void*)(uintptr_t)VERTEX_EA, (void*)(uintptr_t)FRAG_EA);
    set_src(0, 0x85, 0x1400, 1280, 720, 0);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Guest memory out to the fragment buffer: SetBufferAddress writes the convert program there. */
    const uint32_t vm_size = FRAG_EA + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { printf("[FAIL] guest VM alloc\n"); return 1; }
    ppu_vm_size = vm_size;

    /* Child modes: the call under test must stop. */
    if (argc == 2) {
        if (!strcmp(argv[1], "interlace")) {
            child_scene(CELL_RESC_1280x720, CELL_RESC_1280x720, 0, 1, 0);
            cellRescSetConvertAndFlip((void*)(uintptr_t)g_ctx, 0);
            return 0;
        }
        if (!strcmp(argv[1], "pal")) {
            child_scene(CELL_RESC_720x576, CELL_RESC_720x576, CELL_RESC_PAL_60_DROP, 0, 0);
            cellRescSetConvertAndFlip((void*)(uintptr_t)g_ctx, 0);
            return 0;
        }
        if (!strcmp(argv[1], "full")) {
            /* A callback that leaves no room: the stream cannot continue. */
            child_scene(CELL_RESC_1280x720, CELL_RESC_1280x720, 0, 0, 0);
            const uint32_t opd = host_alloc(8, 8);
            gw32(opd, 0x00123400u);
            gw32(g_ctx + 0xC, opd);
            gw32(g_ctx + 4, RING + 40u);
            cellRescSetConvertAndFlip((void*)(uintptr_t)g_ctx, 0);
            return 0;
        }
        if (!strcmp(argv[1], "nobuffer")) {
            /* No SetBufferAddress: no vertex count, the draw cannot be built. */
            scene_gcm();
            scene_init(CELL_RESC_1280x720, 0, 0, 0, 0);
            cellRescSetDisplayMode(CELL_RESC_1280x720);
            set_src(0, 0x85, 0x1400, 1280, 720, 0);
            cellRescSetConvertAndFlip((void*)(uintptr_t)g_ctx, 0);
            return 0;
        }
        return 2;
    }

    test_stream_and_toggle();
    test_present_order();
    test_float_source();
    test_callback_points();
    test_quad();
    test_vertex_buffer_and_aspect();
    test_vertex_program_words();
    test_fragment_microcode();
    test_errors();

    check_stop(argv[0], "interlace", "interlace-mode");
    check_stop(argv[0], "pal", "pal-temporal-mode");
    check_stop(argv[0], "full", "buffer-full-callback");
    check_stop(argv[0], "nobuffer", "vertex-count");

    printf("[resc_convert] %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    free(vm_base);
    return g_fail ? 1 : 0;
}

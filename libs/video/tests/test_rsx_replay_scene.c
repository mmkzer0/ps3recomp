/* A synthetic "live" run for tools/test_rsx_replay.py: the real FIFO walker, register-file draw engine and
 * headless Metal backend over a command ring this fixture writes, with an RSX capture open
 * (rsx_capture.h). It leaves the capture in the directory argv[1]; rsx_replay replays it.
 *
 * Two frames, 64x64, every draw on the guest's own programs:
 *   flip 0   a clear, a triangle (the state set up in a CALLed subroutine that RETs), a NV0039 copy of a texture
 *            from one main-memory window to another, a semaphore ACQUIRE (read) and RELEASE (write), then the
 *            flip and a JUMP to a second segment;
 *   flip 1   after SET_OBJECT rebinds subchannel 1, a triangle strip textured from the copied bytes, and the flip.
 * The second main-memory window is mapped with cellGcmMapEaIoAddress, so a gcm-state event is in the capture.
 *
 * Extra arguments add what the walker cannot classify yet (the capture then counts unknown methods, and a
 * replay must refuse it by name or take --accept-unknown-methods):
 *   unknown  a method no object class handles (NV4097 0x1F80, outside the constant window and the
 *            allow-list; 0x0394 used to be one until S1 implemented the depth range);
 *   report   a GET_REPORT: the walker handles it in place, the method census still counts SET_REPORT_DMA and
 *            GET_REPORT as unknown. Its 16-byte write carries a host-clock timer.
 *
 * Exit 77 when there is no Metal device or no shader translation (CTest skips), 1 on a failed check.
 */
#include "cellGcmSys.h"
#include "rsx_capture.h"
#include "rsx_metal_backend.h"
#include "rsx_method_class.h"
#include "rsx_test_programs.h"

#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void cellGcm_rsx_process_fifo(void);   /* the walker: libs/video/cellGcmSys.c has no header entry for it */

uint8_t* vm_base;
extern uint32_t ppu_vm_size;
extern uint32_t ppu_hle_inject_base;
void vm_write32(uint64_t addr, uint32_t v);
uint32_t vm_read32(uint64_t addr);

#define VM_SIZE    0xE0000000u
#define IO_EA      0x10000000u
#define IO_SIZE    0x00400000u
#define CMD_SIZE   0x00080000u
#define WIN2_EA    0x20000000u
#define WIN2_IO    0x00400000u          /* the second window sits right after the first */
#define W          64u
#define H          64u
#define SURF_OFF   0x00040000u          /* local memory */
#define CONTROL    (ppu_hle_inject_base + 0x2000u)
#define LABEL_EA(i) (ppu_hle_inject_base + (i) * 0x10u)

#define VTX_TRI    0x00100000u          /* IO offsets, window 1 */
#define FP_COL     0x00110000u
#define REPORT     0x00120000u
#define SUB_AT     0x00020000u          /* the CALLed subroutine */
#define FAR_AT     0x00021000u          /* the JUMP target */
#define VTX_QUAD   (WIN2_IO + 0x1000u)  /* window 2 */
#define FP_TEX     (WIN2_IO + 0x2000u)
#define TEX_SRC    (WIN2_IO + 0x3000u)
#define TEX_DST    (WIN2_IO + 0x4000u)

static int g_fail;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    g_fail += !ok;
}

static u32 g_heap = 0x00800000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    const u32 r = g_heap;
    g_heap += size;
    return r;
}
static void host_w32(u32 ea, u32 v) { vm_write32(ea, v); }

/* The ring cursor, an IO offset; window 1 and window 2 are both reachable through io. */
static u32 g_at;
static u32 io_ea(u32 io) { return io < IO_SIZE ? IO_EA + io : WIN2_EA + (io - WIN2_IO); }
static void word(u32 v) { vm_write32(io_ea(g_at), v); g_at += 4; }
static void pk(u32 subch, u32 method, u32 n, const u32* d)
{
    word((n << 18) | (subch << 13) | method);
    for (u32 i = 0; i < n; i++) word(d[i]);
}
static void m(u32 method, u32 v)
{
    if (method >= 0xE000u) pk(7, method & 0x1FFCu, 1, &v);   /* driver methods carry their subchannel */
    else pk(0, method, 1, &v);
}
static void m1(u32 method, u32 v) { pk(1, method, 1, &v); }
static void bf(u32 ea, float f) { u32 v; memcpy(&v, &f, 4); vm_write32(ea, v); }

/* One interleaved vertex: position, colour and texture coordinate, four floats each (48 bytes). */
static void vertex(u32 ea, const float* p, const float* c, const float* t)
{
    for (int i = 0; i < 4; i++) { bf(ea + 4u * i, p[i]); bf(ea + 16u + 4u * i, c[i]); bf(ea + 32u + 4u * i, t[i]); }
}

/* The vertex program: position, colour and texture coordinate passed through. */
static void upload_vp(void)
{
    u8 vp[48];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 12; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    m(0x1EA0, 0);
}

static void vertex_arrays(u32 io)
{
    m(0x1740, 2u | (4u << 4) | (48u << 8)); m(0x1680, 0x80000000u | io);
    m(0x1744, 2u | (4u << 4) | (48u << 8)); m(0x1684, 0x80000000u | (io + 0x10u));
    m(0x1760, 2u | (4u << 4) | (48u << 8)); m(0x16A0, 0x80000000u | (io + 0x20u));
}

static void run_to(u32 put)
{
    vm_write32(CONTROL + 0, put);
    for (int pass = 0; pass < 64 && vm_read32(CONTROL + 4) != put; pass++) cellGcm_rsx_process_fifo();
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: %s <capture dir> [unknown] [report]\n", argv[0]); return 2; }
    int unknown = 0, report = 0;
    for (int i = 2; i < argc; i++) { unknown |= !strcmp(argv[i], "unknown"); report |= !strcmp(argv[i], "report"); }

    void* p = mmap(NULL, VM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { puts("FAIL: guest VM"); return 1; }
    vm_base = (uint8_t*)p;
    ppu_vm_size = VM_SIZE;
    ppu_hle_inject_base = 0x40300000u;   /* not the library default: a replay has to find it from the labels */

    setenv("PS3RECOMP_METAL_HEADLESS", "1", 1);
    if (rsx_metal_backend_init(1280, 720, "rsx_replay scene") != 0) { puts("SKIP: no Metal device"); return 77; }
    if (!(rsx_metal_backend_status() & RSX_METAL_GUEST_SHADERS) || !(rsx_metal_backend_status() & RSX_METAL_ENGINE_PATH)) {
        puts("SKIP: guest shader translation or the draw engine is unavailable");
        return 77;
    }
    rsx_capture_environment env;
    rsx_metal_backend_capture_environment(&env);
    rsx_capture_options opt = { argv[1], 0 };
    (void)rsx_capture_install(&env, &opt);

    const u32 ctx_out = host_alloc(4, 4);
    check(cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_EA, host_alloc, host_w32) != 0, "cellGcmSetupContext");
    check(cellGcmSetDisplayBuffer(1, SURF_OFF, W * 4, W, H) == 0, "display buffer 1 is the colour surface");
    check(cellGcmMapEaIoAddress(WIN2_EA, WIN2_IO, 0x100000u) == 0, "a second main-memory window is mapped");

    /* ---- guest data, written before the RSX consumes any of it ---- */
    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1}, white[4] = {1, 1, 1, 1};
    const float z[4] = {0, 0, 0, 0};
    {
        const float a[4] = {-1, -1, .5f, 1}, b[4] = {3, -1, .5f, 1}, c[4] = {-1, 3, .5f, 1};
        vertex(IO_EA + VTX_TRI, a, red, z);
        vertex(IO_EA + VTX_TRI + 48, b, green, z);
        vertex(IO_EA + VTX_TRI + 96, c, blue, z);
    }
    {
        const float a[4] = {-.75f, -.75f, .5f, 1}, b[4] = {.75f, -.75f, .5f, 1}, c[4] = {-.75f, .75f, .5f, 1}, d[4] = {.75f, .75f, .5f, 1};
        const float ta[4] = {0, 1, 0, 1}, tb[4] = {1, 1, 0, 1}, tc[4] = {0, 0, 0, 1}, td[4] = {1, 0, 0, 1};
        vertex(WIN2_EA + 0x1000u, a, white, ta);
        vertex(WIN2_EA + 0x1000u + 48, b, white, tb);
        vertex(WIN2_EA + 0x1000u + 96, c, white, tc);
        vertex(WIN2_EA + 0x1000u + 144, d, white, td);
    }
    rsx_test_fp_mov_r0_col0(vm_base + IO_EA + FP_COL, RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_tex_r0_tc0(vm_base + WIN2_EA + (FP_TEX - WIN2_IO), 0);
    for (u32 i = 0; i < 16; i++) {          /* 4x4 A8R8G8B8: a gradient with distinct texels */
        const u32 argb = 0xFF000000u | ((i * 16u) << 16) | (((15u - i) * 16u) << 8) | ((i & 3u) * 64u);
        vm_write32(WIN2_EA + (TEX_SRC - WIN2_IO) + 4u * i, argb);
    }
    vm_write32(LABEL_EA(6), 9u);            /* a label the RSX will ACQUIRE at 9 */

    /* ---- the subroutine: surface, scissor, clear colour, no depth ---- */
    g_at = SUB_AT;
    m(0x194, 0xFEED0000u);
    m(0x210, SURF_OFF); m(0x20C, W * 4);
    m(0x200, W << 16); m(0x204, H << 16);
    m(0x208, 0x148);
    m(0x8C0, W << 16); m(0x8C4, H << 16);
    m(0x1D90, 0xFF102030u);
    m(0xA74, 0); m(0xA70, 0);
    word(0x00020000u);                      /* RET */

    /* ---- segment 2, flip 1: rebind, textured strip, flip ---- */
    g_at = FAR_AT;
    m1(0, RSX_OBJ_NV3062);                  /* subchannel 1 leaves NV0039: a rebind */
    m(0x1D94, 0xF0);
    upload_vp();
    vertex_arrays(VTX_QUAD);
    m(0x8E4, FP_TEX | 2u); m(0x1D60, 64);
    m(0x1A00, TEX_DST);                     /* unit 0: the bytes the NV0039 copy wrote */
    m(0x1A04, 2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
    m(0x1A0C, 0x80000000u);
    m(0x1A10, 0xAAE4u);
    m(0x1A18, (4u << 16) | 4u);
    m(0x1808, 6 /* TRIANGLE_STRIP */); m(0x1814, 3u << 24); m(0x1808, 0);
    m(0xE944, 1); m(0xE924, 0x8000010Fu);
    const u32 far_end = g_at;

    /* ---- the main segment ---- */
    g_at = 0x1000;
    m(0, RSX_OBJ_NV4097);
    m1(0, RSX_OBJ_NV0039);
    word(SUB_AT | 2u);                      /* CALL */
    m(0x1D94, 0xF0);                        /* clear */
    upload_vp();
    vertex_arrays(VTX_TRI);
    m(0x8E4, FP_COL | 2u); m(0x1D60, 64);
    m(0x1808, 5 /* TRIANGLES */); m(0x1814, 2u << 24); m(0x1808, 0);
    if (unknown) m(0x1F80, 1);
    /* NV0039: 64 bytes, window 2 source to window 2 destination */
    m1(0x184, 0xFEED0001u); m1(0x188, 0xFEED0001u);
    m1(0x30C, TEX_SRC); m1(0x310, TEX_DST);
    m1(0x314, 16); m1(0x318, 16); m1(0x31C, 16); m1(0x320, 4); m1(0x324, 0x0101u);
    m1(0x328, 0);
    m(0x64, 6 * 0x10u); m(0x68, 9u);        /* ACQUIRE label 6 at 9: consumed at once */
    m(0x64, 5 * 0x10u); m(0x6C, 7u);        /* RELEASE label 5 = 7 */
    if (report) { m(0x1A8, 0xFEED0001u); m(0x1800, (1u << 24) | REPORT); }
    m(0xE944, 1); m(0xE924, 0x8000010Fu);
    word(0x20000000u | FAR_AT);             /* JUMP */
    (void)far_end;

    run_to(far_end);

    rsx_capture_finish();
    rsx_metal_backend_shutdown();

    check(vm_read32(LABEL_EA(5)) == 7u, "the RELEASE reached the label");
    check(vm_read32(WIN2_EA + (TEX_DST - WIN2_IO)) == vm_read32(WIN2_EA + (TEX_SRC - WIN2_IO)), "the NV0039 copy reached the destination");
    if (report) check(vm_read32(IO_EA + REPORT + 8) == 0xFFFFu, "the report holds its value word");
    return g_fail ? 1 : 0;
}

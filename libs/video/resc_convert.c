/*
 * ps3recomp - cellResc convert-and-flip command stream
 *
 * See resc_convert.h. Every group below is one firmware reservation (`current + n > end` ->
 * callback), in the order libresc FW 4.93 issues them; the helper each came from is named at
 * its group. Method headers are `count << 18 | subchannel << 13 | method`.
 */

#include "resc_convert.h"
#include "cellGcmSys.h"
#include "ps3emu/stop.h"
#include <string.h>

#define HDR(count, method) (((u32)(count) << 18) | (u32)(method))

static u32 f2u(float f)
{
    u32 u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* Index of the highest set bit as the firmware computes it: 31 - cntlzw(x), so 0 gives -1. */
static u32 log2_floor(u32 x)
{
    u32 clz = 32;
    for (int b = 31; b >= 0; b--)
        if ((x >> b) & 1u) { clz = (u32)(31 - b); break; }
    return 31u - clz;
}

/* ---- the vertex program ---------------------------------------------------------------------
 *
 * NV40 vertex-program instruction, the layout libs/video/rsx_vp_decompiler.c decodes:
 *   D0: CC swizzle [2:9], cond [10:12], dst_tmp [15:20], vec_result [30]
 *   D1: src0h [0:7], input_src [8:11], vec_opcode [22:26]
 *   D2: src2h [0:5], src1 [6:22], src0l [23:31]
 *   D3: end [0], dst [2:6], sca_dst_tmp [7:12], vec write mask [13:16], src2l [21:31]
 * Each of the library's three instructions is `MOV o[dst].mask, v[input].swz`. The library's Cg
 * compiler leaves sources 1 and 2 at the same xyzw input operand as the first instruction's. */
static void vp_mov(u32 o[4], u32 input, u32 dst, u32 mask, u32 src0, int end)
{
    const u32 src12 = 0x1B02u;   /* input operand, swizzle xyzw */
    o[0] = (0x1Bu << 2) | (7u << 10) | (0x3Fu << 15) | (1u << 30);
    o[1] = (1u << 22) | (input << 8) | (src0 >> 9);
    o[2] = ((src0 & 0x1FFu) << 23) | (src12 << 6) | (src12 >> 11);
    o[3] = ((src12 & 0x7FFu) << 21) | (mask << 13) | (dst << 2) | (0x3Fu << 7) | (end ? 1u : 0u);
}

void resc_vertex_program_words(const u8 attr[3], u32 out[12])
{
    const u32 xyzw = 0x1B02u;    /* input operand, swizzle xyzw */
    const u32 xyxx = 0x1002u;    /* input operand, swizzle xyxx (a 2-float texcoord) */
    vp_mov(out + 0, attr[0], 0, 0xF, xyzw, 0);   /* o[0] position */
    vp_mov(out + 4, attr[1], 7, 0xC, xyxx, 0);   /* o[7] texcoord 0, .xy */
    vp_mov(out + 8, attr[2], 8, 0xC, xyxx, 1);   /* o[8] texcoord 1, .xy; last instruction */
}

/* ---- the quad -------------------------------------------------------------------------------- */

void resc_quad_vertices(u32 display_mode, u32 ratio_mode, u32 cfg_size,
                        float aspect_h, float aspect_v, u32 out[24])
{
    const float half = 0.5f;
    const float f13 = half / aspect_v;
    const float vhi_plain = f13 + half, vlo_plain = half - f13;
    const float u0 = half / aspect_h;
    float ulo = half - u0, uhi = u0 + half;
    float vlo = vlo_plain, vhi = vhi_plain;

    if (display_mode == CELL_RESC_720x480 || display_mode == CELL_RESC_720x576) {
        if (ratio_mode == CELL_RESC_LETTERBOX) {
            const u32 k_two_thirds = 0x3F2AAAABu, k_ntsc_wide = 0x3F87F29Eu;
            float a, b;
            memcpy(&a, &k_two_thirds, 4);
            memcpy(&b, &k_ntsc_wide, 4);
            float g = a / aspect_v;
            if (cfg_size != 28)
                g = g / b;
            vhi = g + half;
            vlo = half - g;
        } else if (ratio_mode == CELL_RESC_PANSCAN) {
            const u32 k_three_eighths = 0x3EC00000u;
            float t;
            memcpy(&t, &k_three_eighths, 4);
            t = t / aspect_h;
            ulo = half - t;
            uhi = t + half;
        }
    }

    const float one = 1.0f, mone = -1.0f, zero = 0.0f;
    const float v[4][6] = {
        { mone,  one, ulo, vlo, zero, zero },
        {  one,  one, uhi, vlo,  one, zero },
        {  one, mone, uhi, vhi,  one,  one },
        { mone, mone, ulo, vhi, zero,  one },
    };
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 6; c++)
            out[r * 6 + c] = f2u(v[r][c]);
}

/* ---- the stream ------------------------------------------------------------------------------- */

/* libresc 0x02002978: default state, the scissor (the display mode's size), the viewport. */
static void emit_state(u32 ctx, const RescConvertIn* in)
{
    static const u32 zero_methods[] = { 0x0A70, 0x0304, 0x0310, 0x036C, 0x0374, 0x183C,
                                        0x0380, 0x0A74, 0x0A68, 0x0328, 0x0348 };
    u32 w[4];

    w[0] = HDR(1, 0x0324); w[1] = 0x01010101u;
    cellGcm_ctx_emit(ctx, w, 2);
    for (u32 i = 0; i < sizeof zero_methods / sizeof zero_methods[0]; i++) {
        w[0] = HDR(1, zero_methods[i]); w[1] = 0;
        cellGcm_ctx_emit(ctx, w, 2);
    }
    w[0] = HDR(1, 0x1EE4); w[1] = 0; w[2] = HDR(1, 0x1EE8); w[3] = 0;
    cellGcm_ctx_emit(ctx, w, 4);
    w[0] = HDR(1, 0x0300); w[1] = 1;
    cellGcm_ctx_emit(ctx, w, 2);
    w[0] = HDR(1, 0x0368); w[1] = 0x1D01;
    cellGcm_ctx_emit(ctx, w, 2);
    w[0] = HDR(1, 0x1FC0); w[1] = 0;
    cellGcm_ctx_emit(ctx, w, 2);

    /* NV4097_SET_SCISSOR_HORIZONTAL/VERTICAL: x | w << 16, y | h << 16 with the origin at 0. */
    const u32 sc[3] = { HDR(2, 0x08C0), in->width << 16, in->height << 16 };
    cellGcm_ctx_emit(ctx, sc, 3);

    /* libresc 0x02007DF0(ctx, 0, 0, w, h): viewport, depth range, offset and scale (twice). */
    {
        const float hw = (float)in->width * 0.5f, hh = (float)in->height * 0.5f;
        u32 v[24] = {
            HDR(2, 0x0A00), in->width << 16, in->height << 16,
            HDR(2, 0x0394), f2u(0.0f), f2u(1.0f),
        };
        const u32 vp[9] = {
            HDR(8, 0x0A20),
            f2u(hw), f2u(hh), f2u(0.5f), f2u(0.0f),     /* offset */
            f2u(hw), f2u(-hh), f2u(0.5f), f2u(0.0f),    /* scale */
        };
        memcpy(v + 6, vp, sizeof vp);
        memcpy(v + 15, vp, sizeof vp);
        cellGcm_ctx_emit(ctx, v, 24);
    }
}

/* libresc 0x02002680: the two attribute arrays, then the vertex-cache invalidates. */
static void emit_vertex_arrays(u32 ctx, const RescConvertIn* in)
{
    for (int a = 0; a < 2; a++) {
        const u32 slot = (u32)in->attr[a] & 0xFFu;
        const u32 w[4] = {
            HDR(1, 0x1740 + slot * 4), 0x1822u,   /* 2 floats, stride 24 */
            HDR(1, 0x1680 + slot * 4), in->vertex_offset + (a ? 8u : 0u),
        };
        cellGcm_ctx_emit(ctx, w, 4);
    }
    const u32 inv[8] = { HDR(1, 0x1710), 0, HDR(1, 0x1714), 0, HDR(1, 0x1714), 0, HDR(1, 0x1714), 0 };
    cellGcm_ctx_emit(ctx, inv, 8);
}

/* libresc 0x02000D00 -> 0x020009DC (the one-target case): DMA contexts, the surface, the clip. */
static void emit_surface(u32 ctx, const RescConvertIn* in)
{
    const u32 dma = 0xFEED0000u;
    const u32 fmt = (in->dst_format & 0xFFu) | 0x140u |
                    (log2_floor(in->width) << 16) | (log2_floor(in->height) << 24);
    const u32 h = in->height & 0xFFFFu;
    const u32 w[32] = {
        HDR(1, 0x0194), dma,
        HDR(1, 0x018C), dma,
        HDR(2, 0x01B4), dma, dma,
        HDR(1, 0x0198), dma,
        HDR(6, 0x0208), fmt, in->pitch, in->color_offset, 0, 0, 0x40,
        HDR(1, 0x022C), 0x40,
        HDR(4, 0x0280), 0x40, 0x40, 0, 0,
        HDR(1, 0x0220), 1,
        HDR(1, 0x02B8), 0,
        HDR(2, 0x0200), in->width << 16, h << 16,
        HDR(1, 0x1D88), (h - ((h >> 12) & 1u)) | 0x1000u,
    };
    cellGcm_ctx_emit(ctx, w, 32);
}

/* libresc 0x02000D88: bind the SetSrc record to texture unit `tex_unit` (non-interlaced). */
static void emit_texture(u32 ctx, const RescConvertIn* in)
{
    const u32 u = in->tex_unit;
    const u32 fmt = in->src.format & 0xBFu;
    /* Float formats (0x9A, 0x9B, with the layout bits cleared) are sampled unfiltered. */
    const int is_float = (((fmt & ~0x60u) + 0x66u) & 0xFFu) <= 1u;
    const u32 min_mag = is_float ? 1u : 2u, wrap = 1u;
    const u32 filter = ((min_mag << 16) & 0x70000u) | ((wrap << 13) & 0x6000u) |
                       ((min_mag << 24) & 0xFF000000u);
    const u32 base = (u << 5) & 0x1FE0u;

    const u32 a[9] = {
        HDR(2, 0x1A00 + ((u & 0xFFu) << 5)), in->src.offset, ((fmt << 8) & 0xFF00u) | 0x10000u | 0x29u,
        HDR(1, 0x1A18 + ((u & 0xFFu) << 5)), ((u32)in->src.width << 16) | in->src.height,
        HDR(1, 0x1840 + ((u & 0xFFu) << 2)), in->src.pitch | 0x100000u,
        HDR(1, 0x1A10 + ((u & 0xFFu) << 5)), 0xAAE4u,
    };
    cellGcm_ctx_emit(ctx, a, 9);
    const u32 b[2] = { HDR(1, 0x1A0C + base), 0x80000780u };
    cellGcm_ctx_emit(ctx, b, 2);
    const u32 c[2] = { HDR(1, 0x1A08 + base), 0x10050404u };
    cellGcm_ctx_emit(ctx, c, 2);
    const u32 d[2] = { HDR(1, 0x1A14 + base), filter };
    cellGcm_ctx_emit(ctx, d, 2);
    const u32 e[2] = { HDR(1, 0x1A1C + base), 0 };
    cellGcm_ctx_emit(ctx, e, 2);
}

/* libresc 0x02006D18 (program object +0xD8): the vertex program upload. The library's program has
 * 3 instructions, start slot 0, input mask 0x301 and no constants. */
static void emit_vertex_program(u32 ctx, const RescConvertIn* in)
{
    const u32 start = 0, n_instr = 3, input_mask = 0x301;
    u32 prog[12];
    resc_vertex_program_words(in->attr, prog);

    const u32 batches = n_instr >> 3, rem_words = (n_instr & 7u) * 4u;
    if (batches)
        ps3_stop_gcm_unmodelled("cellRescSetConvertAndFlip", "vertex-program-batches",
                                "the library's vertex program has 3 instructions");
    u32 w[32], n = 0;
    w[n++] = HDR(2, 0x1E9C); w[n++] = start; w[n++] = start;   /* TRANSFORM_PROGRAM_LOAD / START */
    w[n++] = HDR(rem_words, 0x0B80);
    for (u32 i = 0; i < rem_words; i++)
        w[n++] = prog[i];
    w[n++] = HDR(1, 0x1FF0); w[n++] = input_mask;               /* VERTEX_ATTRIB_INPUT_MASK */
    w[n++] = HDR(1, 0x1EF8); w[n++] = 0x20FFFFu;                /* TRANSFORM_TIMEOUT */
    cellGcm_ctx_emit(ctx, w, n);
}

/* libresc 0x020069F0 (program object +0xFC): the fragment program binding. */
static void emit_fragment_program(u32 ctx, const RescConvertIn* in)
{
    const RescFragProgram* fp = &in->fp;
    const u32 head[4] = {
        HDR(1, 0x08E4), (fp->ucode_offset & 0x1FFFFFFFu) | 1u,   /* SHADER_PROGRAM: offset | location + 1 */
        HDR(1, 0x1FF4), fp->attr_in_mask | 0x20u,                /* VERTEX_ATTRIB_OUTPUT_MASK */
    };
    cellGcm_ctx_emit(ctx, head, 4);

    /* One TEX_COORD_CONTROL per texcoord the program reads: bit 0 = 2D, bit 4 = centroid. */
    u32 left = fp->tex_coords_mask, two_d = fp->tex_coords_2d, cent = fp->tex_coords_centroid;
    for (u32 i = 0; left; i++, left >>= 1, two_d >>= 1, cent >>= 1) {
        if (!(left & 1u))
            continue;
        const u32 w[2] = { HDR(1, 0x0B40 + i * 4), (two_d & 1u) | ((cent << 4) & 0x10u) };
        cellGcm_ctx_emit(ctx, w, 2);
    }

    /* SHADER_CONTROL: register count in bits 24..31 (2 when the program reports fewer), bit 6
     * when the output is taken from R0, 0x400 | 0xE when depth is replaced, bit 7 pixel kill. */
    const u32 regs = fp->num_regs > 1 ? (u32)fp->num_regs << 24 : 0x02000000u;
    const u32 ctl = regs | (fp->output_from_h0 == 0 ? 0x40u : 0u) |
                    (fp->depth_replace == 0 ? 0x400u : 0x40Eu) | (fp->pixel_kill ? 0x80u : 0u);
    const u32 w[2] = { HDR(1, 0x1D60), ctl };
    cellGcm_ctx_emit(ctx, w, 2);
}

/* libresc 0x02004F04: the quad draw. */
static void emit_draw(u32 ctx, const RescConvertIn* in)
{
    if (in->vertex_count == 0 || in->vertex_count > 256)
        ps3_stop_gcm_unmodelled("cellRescSetConvertAndFlip", "vertex-count",
                                "the draw is built for 1..256 vertices (one DRAW_ARRAYS word)");
    const u32 w[8] = {
        HDR(3, 0x1714 | 0x40000000u), 0, 0, 0,         /* non-incrementing: the cache invalidates */
        HDR(1, 0x1808), 8,                              /* BEGIN_END: QUADS */
        HDR(1, 0x1814), ((in->vertex_count - 1u) & 0xFFu) << 24,
    };
    cellGcm_ctx_emit(ctx, w, 8);
    const u32 e[2] = { HDR(1, 0x1808), 0 };             /* BEGIN_END: end */
    cellGcm_ctx_emit(ctx, e, 2);
}

void resc_emit_convert_stream(u32 ctx, const RescConvertIn* in)
{
    emit_state(ctx, in);
    emit_vertex_arrays(ctx, in);
    emit_surface(ctx, in);
    emit_texture(ctx, in);
    const u32 sync[2] = { HDR(1, 0x1FD8), 1 };
    cellGcm_ctx_emit(ctx, sync, 2);
    emit_vertex_program(ctx, in);
    emit_fragment_program(ctx, in);
    emit_draw(ctx, in);
}

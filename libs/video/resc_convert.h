/*
 * ps3recomp - cellResc convert-and-flip command stream
 *
 * The RSX command words cellRescSetConvertAndFlip appends to the title's command context: the
 * full-screen quad that converts the source buffer into the display buffer. Built from the
 * library's configuration state, as libresc builds it (FW 4.93 `libresc.prx`, body 0x02004D4C):
 * nothing here is a recorded stream.
 *
 * Scope: the progressive, non-PAL-interpolating path (the library's `+0x14` interlace mode and
 * the 720x576 PAL-interpolation display modes take other branches the runtime does not
 * reproduce; cellRescSetConvertAndFlip stops on them).
 */

#ifndef PS3RECOMP_RESC_CONVERT_H
#define PS3RECOMP_RESC_CONVERT_H

#include "ps3emu/ps3types.h"
#include "cellResc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One of the library's convert fragment programs, by the Cg-binary header fields the emitter
 * reads (CgBinaryFragmentProgram: +0x04 attribute input mask, +0x0C/+0x0E/+0x10 texcoord
 * masks, +0x12 register count, +0x13..+0x15 flags) and the RSX offset its microcode was copied
 * to (library program object +0x10, set by cellRescSetBufferAddress). */
typedef struct RescFragProgram {
    u32 attr_in_mask;
    u16 tex_coords_mask;
    u16 tex_coords_2d;
    u16 tex_coords_centroid;
    u8  num_regs;
    u8  output_from_h0;
    u8  depth_replace;
    u8  pixel_kill;
    u32 ucode_offset;
} RescFragProgram;

/* The library state one convert-and-flip reads. Field comments give the library object offset. */
typedef struct RescConvertIn {
    u32 width, height, pitch;      /* +0x114, +0x118, +0x11C: the display mode's size and pitch */
    u32 dst_format;                /* word 0 of the mode's Dsts record (read through +0xCC) */
    u32 color_offset;              /* +0x128 + 4 * frame toggle: this frame's colour buffer */
    u32 vertex_offset;             /* RSX offset of +0x104, the vertex buffer */
    u32 vertex_count;              /* +0x140 */
    u8  attr[3];                   /* +0x152..+0x154: vertex attribute slots (position, 2 x texcoord) */
    u8  tex_unit;                  /* +0x155: the source texture unit */
    CellRescSrc src;               /* +0x1C + 16 * index: the SetSrc record */
    RescFragProgram fp;            /* +0xFC: the current convert fragment program */
} RescConvertIn;

/* The vertex program's three pass-through instructions (position, texcoord, texcoord), as the
 * library's embedded vertex program holds them (+0xD8), 4 words each. `attr` are the input slots. */
void resc_vertex_program_words(const u8 attr[3], u32 out[12]);

/* The quad the convert pass draws, four records of six floats (position, texcoord, texcoord),
 * big-endian-ready bit patterns, as the library fills the vertex buffer (0x02001884). Inputs
 * are the display mode (CELL_RESC_*), the init config's ratio mode and size, and the two aspect
 * values cellRescAdjustAspectRatio set (+0x148 horizontal, +0x14C vertical). */
void resc_quad_vertices(u32 display_mode, u32 ratio_mode, u32 cfg_size,
                        float aspect_h, float aspect_v, u32 out[24]);

/* Append the convert stream (everything before the flip) to the context at guest EA `ctx`, one
 * group per firmware reservation so the buffer-full callback fires at the firmware's points. */
void resc_emit_convert_stream(u32 ctx, const RescConvertIn* in);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RESC_CONVERT_H */

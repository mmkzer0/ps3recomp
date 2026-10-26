/*
 * ps3recomp - the object class a FIFO subchannel is bound to, and what a method on it may mean
 *
 * A subchannel is a binding slot, not an engine: SET_OBJECT (method 0) names the object that the
 * slot's methods drive, and the object's class decides what a method number is. Every method the
 * walker (cellGcmSys.c) or the register-file dispatch (rsx_dispatch.c) sees is exactly one of
 *
 *   handled   a consumer reads it: it changes what an executed command does,
 *   inert     an allow-list entry below, each with the evidence that nothing observable depends on it,
 *   missing   a state this renderer ignores, with an S2 row (docs/RSX_GRAPHICS.md) or an implementation,
 *   unknown   none of the above. Counted per (class, method); a capture frame it belongs to is invalid
 *             and the live walker stops with kind rsx-unknown-method.
 *
 * The table below is the single source of truth for all three classes: tools/test_rsx_methods.py
 * checks it against docs/RSX_GRAPHICS.md (one row per (class, method), a citation in each), and the
 * walker enforces it (cellGcmSys.c gcm_method_verdict). Rows a consumer reads are only documented
 * here; the walker's `handled` flag comes from the consumer itself, never from this table, so a
 * consumer that stops reading a register cannot be masked by a stale row (test_rsx_method_class.c
 * checks that every HANDLED row is consumed on the register-file path).
 *
 * Values: an INERT row carries the register value it is inert for (reset_value/reset_mask). The
 * driver's default-state routine (gcm_default_state.c) writes each of these registers once per flip,
 * so "the method is on the allow-list" would otherwise also allow a title to change the state
 * silently: a write that does not match the row counts as missing instead.
 *
 * Header-only on purpose: rsx_commands.c and rsx_dispatch.c are compiled alone by CI fixtures.
 */
#ifndef PS3RECOMP_RSX_METHOD_CLASS_H
#define PS3RECOMP_RSX_METHOD_CLASS_H

#include <stddef.h>
#include <stdint.h>

/* Object handles libgcm_sys 4.93 binds in its default-state subroutine (gcm_default_state.c: the
 * SET_OBJECT words on subchannels 0, 1, 3, 4, 5, 6 and 7). NV4097, NV0039 and NV3062 are the values
 * RPCS3 resets its method registers to (Emu/RSX/rsx_methods.cpp, NV4097_SET_OBJECT / NV0039_SET_OBJECT /
 * NV3062_SET_OBJECT). [INFERENCE] NV308A and NV309E are named from the methods the default state and a
 * title send to them: 0x184-0x19C and 0x2FC are NV308A's contexts and operation, a bare 0x184 is
 * NV309E's image DMA. */
#define RSX_OBJ_NV4097  0x31337000u
#define RSX_OBJ_NV0039  0x31337303u
#define RSX_OBJ_NV3062  0x313371C3u
#define RSX_OBJ_NV308A  0x31337808u
#define RSX_OBJ_NV309E  0x31337A73u
#define RSX_OBJ_NV3089  0x3137AF00u
#define RSX_OBJ_DRIVER  0xCAFEBABEu

typedef enum {
    RSX_CLASS_UNBOUND = 0,    /* nothing has bound this subchannel since the context was set up */
    RSX_CLASS_UNREGISTERED,   /* bound to a handle this table does not know */
    RSX_CLASS_NV406E,         /* channel control: methods below 0x100 on any subchannel */
    RSX_CLASS_NV4097,
    RSX_CLASS_NV0039,
    RSX_CLASS_NV3062,
    RSX_CLASS_NV308A,
    RSX_CLASS_NV309E,
    RSX_CLASS_NV3089,
    RSX_CLASS_DRIVER,         /* libgcm's own object: flip, queue and user-command words */
    RSX_CLASS_COUNT
} rsx_class_id;

static inline const char* rsx_class_name(rsx_class_id c)
{
    static const char* const names[RSX_CLASS_COUNT] = {
        "unbound", "unregistered", "NV406E", "NV4097", "NV0039", "NV3062", "NV308A", "NV309E", "NV3089",
        "driver",
    };
    return (unsigned)c < RSX_CLASS_COUNT ? names[c] : "invalid";
}

/* The class a SET_OBJECT handle names. 0 is the reset value, not a binding. */
static inline rsx_class_id rsx_class_from_handle(uint32_t handle)
{
    switch (handle) {
    case 0:              return RSX_CLASS_UNBOUND;
    case RSX_OBJ_NV4097: return RSX_CLASS_NV4097;
    case RSX_OBJ_NV0039: return RSX_CLASS_NV0039;
    case RSX_OBJ_NV3062: return RSX_CLASS_NV3062;
    case RSX_OBJ_NV308A: return RSX_CLASS_NV308A;
    case RSX_OBJ_NV309E: return RSX_CLASS_NV309E;
    case RSX_OBJ_NV3089: return RSX_CLASS_NV3089;
    case RSX_OBJ_DRIVER: return RSX_CLASS_DRIVER;
    default:             return RSX_CLASS_UNREGISTERED;
    }
}

typedef enum {
    RSX_METHOD_HANDLED = 0,   /* a consumer reads it; `consumer` names the file and function */
    RSX_METHOD_INERT,         /* allow-listed: `data`/`mask` is the value nothing depends on */
    RSX_METHOD_MISSING,       /* a state this renderer ignores: `consumer` is the S2 row or the fix */
    RSX_METHOD_UNKNOWN        /* no row: the walker's last resort, and a named stop */
} rsx_method_verdict;

/* Evidence tags, kept short so a table row stays readable; each is expanded in docs/RSX_GRAPHICS.md.
 *
 *   oracle-null     RPCS3 at d00d8023 binds no handler AND has no register decoder or accessor for it
 *                   (grep of rsx_methods.cpp / rsx_methods.h / rsx_decode.h), and the reason it can is
 *                   in the row. "The reference renderer ignores the register too."
 *   oracle-value    The same, but the oracle reads the register and what it selects is the hardware
 *                   reset default, which this renderer implements as its fixed behaviour (rsx_state::
 *                   init() in rsx_methods.cpp:134 equals gcm_default_state.c). The row pins the value.
 *   order           A stall, invalidate or ordering hint. The walker is a synchronous in-order
 *                   interpreter: everything before the method has already executed when it is read.
 *   redundant       The information is carried by bits the renderer does read (a location bit, the
 *                   one destination surface the 2D path owns, ...).
 *   absent-stage    The register drives a stage a shader-only NV40 pipeline does not have (the VP and
 *                   FP replace T&L and the register combiners), and the oracle ignores it too.
 *   no-transfer     The 2D object never performs a transfer in this stream, so the register is an
 *                   input to work that never happens (the census pair table is the evidence: no
 *                   COLOR/SIZE write for the object appears in it).
 */
typedef enum {
    RSX_EV_ORACLE_NULL = 0,
    RSX_EV_ORACLE_VALUE,
    RSX_EV_ORDER,
    RSX_EV_REDUNDANT,
    RSX_EV_ABSENT_STAGE,
    RSX_EV_NO_TRANSFER,
    RSX_EV_CONSUMER,
    RSX_EV_MISSING
} rsx_evidence;

static inline const char* rsx_evidence_name(rsx_evidence e)
{
    static const char* const names[] = {
        "oracle-null", "oracle-value", "order", "redundant", "absent-stage", "no-transfer",
        "consumer", "missing",
    };
    return (unsigned)e < sizeof names / sizeof names[0] ? names[e] : "invalid";
}

typedef struct rsx_method_rule {
    uint8_t  cls;          /* rsx_class_id */
    uint8_t  verdict;      /* rsx_method_verdict */
    uint8_t  evidence;     /* rsx_evidence */
    uint8_t  pad;
    uint16_t method;       /* first method number of the row (within the class) */
    uint16_t count;        /* consecutive dword methods covered; an array is one row */
    uint32_t data;         /* INERT: the value the row is inert for (see mask) */
    uint32_t mask;         /* INERT: bits compared; 0 = any value */
    const char* name;      /* rnndb / psdevwiki name (docs/rsxfifo.txt) */
    const char* consumer;  /* HANDLED: who reads it; INERT/MISSING: why, or the S2 row */
} rsx_method_rule;

/* One row per register, or per array register (count > 1). Ordered by class, then method; the
 * python test checks both. `data`/`mask` are 0 for rows that are value-independent. */
static const rsx_method_rule rsx_method_rules[] = {
/* Each row is one register, or one array register when `count` > 1. Rows are sorted by class then
 * method; the probe in libs/video/tests/rsx_method_probe.cpp checks that, and that no two rows overlap.
 * `data`/`mask` pin an INERT row to the value nothing depends on; mask 0 means every value. */
#define HANDLED(c, m, n, ev, nm, cs)  { (c), RSX_METHOD_HANDLED, (ev), 0, (m), (n), 0, 0, (nm), (cs) }
#define INERT(c, m, n, ev, nm, cs) { (c), RSX_METHOD_INERT, (ev), 0, (m), (n), 0, 0, (nm), (cs) }
#define INERT_V(c, m, n, v, mk, ev, nm, cs) \
    { (c), RSX_METHOD_INERT, (ev), 0, (m), (n), (v), (mk), (nm), (cs) }
#define MISSING(c, m, n, nm, cs)   { (c), RSX_METHOD_MISSING, RSX_EV_MISSING, 0, (m), (n), 0, 0, (nm), (cs) }

    INERT(RSX_CLASS_NV406E, 0x0060, 1, RSX_EV_ORACLE_NULL, "NV406E_SET_CONTEXT_DMA_SEMAPHORE",
          "the DMA object the semaphore offset addresses; RPCS3 binds nullptr and addresses the label "
          "window by offset alone, as gcm_rsx_process_fifo does (GCM_LABEL_GUEST_BASE + offset); the two "
          "handles libgcm writes (0x66616661/0x56616661) name that same report area "
          "(docs/rsxfifo.txt \"Semaphores\")"),

/* ---- NV4097 3D engine ----------------------------------------------------------------------- */
    HANDLED(RSX_CLASS_NV4097, 0x0000, 1, RSX_EV_CONSUMER, "NV4097_SET_OBJECT",
            "cellGcmSys.c gcm_bind_object: the binding table every later method on the slot decodes against"),
    INERT(RSX_CLASS_NV4097, 0x0100, 1, RSX_EV_ORACLE_NULL, "NV4097_NO_OPERATION",
          "RPCS3 resets the register and binds no handler to it; rsx_dispatch.c carries it as an explicit "
          "no-op (mark_class EXEC)"),
    INERT(RSX_CLASS_NV4097, 0x0110, 1, RSX_EV_ORDER, "NV4097_WAIT_FOR_IDLE",
          "libgcm's SetWaitForIdle / SetWriteBackEndLabelForConditional step (docs/rsxfifo.txt): the engine "
          "must have completed everything issued before it. The walker executes in order and returns only "
          "after the method it consumed, so the idle condition already holds"),
    HANDLED(RSX_CLASS_NV4097, 0x0180, 1, RSX_EV_CONSUMER, "NV4097_SET_CONTEXT_DMA_NOTIFIES",
            "rsx_commands.c rsx_process_method: the NOTIFY (0x0104) path writes the notify slot of this context"),
    INERT(RSX_CLASS_NV4097, 0x0184, 2, RSX_EV_REDUNDANT, "NV4097_SET_CONTEXT_DMA_A/B",
          "the texture DMA objects; the location of a texture is the format word's DMA bits and the 31-bit "
          "offset (rsx_dsp_get_texture), so the handle adds nothing RPCS3 does not also ignore (no decoder)"),
    HANDLED(RSX_CLASS_NV4097, 0x018C, 1, RSX_EV_CONSUMER, "NV4097_SET_CONTEXT_DMA_COLOR_B",
            "rsx_dsp_get_surface: colour target B's location (dma_to_location)"),
    INERT(RSX_CLASS_NV4097, 0x0190, 1, RSX_EV_ORACLE_NULL, "NV4097_SET_CONTEXT_DMA_STATE",
          "a state-object DMA handle for the fixed-function T&L state rung; RPCS3 has no decoder and the "
          "engine's state arrives as method writes, not as a fetched object"),
    HANDLED(RSX_CLASS_NV4097, 0x0194, 2, RSX_EV_CONSUMER, "NV4097_SET_CONTEXT_DMA_COLOR_A/ZETA",
            "rsx_dsp_get_surface: colour target A and zeta locations"),
    INERT(RSX_CLASS_NV4097, 0x019C, 2, RSX_EV_REDUNDANT, "NV4097_SET_CONTEXT_DMA_VERTEX_A/B",
          "vertex-array DMA objects; the location is the offset word's bit 31 (rsx_dsp_get_vertex_attr), "
          "which is what both the SDK and RPCS3 use"),
    HANDLED(RSX_CLASS_NV4097, 0x01A4, 1, RSX_EV_CONSUMER, "NV4097_SET_CONTEXT_DMA_SEMAPHORE",
            "rsx_commands.c rsx_process_method: chooses the main-memory or label-window byte swap of 0x1D70/0x1D74"),
    HANDLED(RSX_CLASS_NV4097, 0x01A8, 1, RSX_EV_CONSUMER, "NV4097_SET_CONTEXT_DMA_REPORT",
            "cellGcmSys.c gcm_rsx_process_fifo_unlocked: s_report_dma, the GET_REPORT destination"),
    INERT(RSX_CLASS_NV4097, 0x01AC, 2, RSX_EV_ORACLE_NULL, "NV4097_SET_CONTEXT_DMA_CLIP_ID/CULL_DATA",
          "DMA handles for the clip-id and cull-data objects (conditional rendering). RPCS3 binds nullptr "
          "with no decoder; this renderer models neither feature (see 0x1DA4)"),
    HANDLED(RSX_CLASS_NV4097, 0x01B4, 2, RSX_EV_CONSUMER, "NV4097_SET_CONTEXT_DMA_COLOR_C/D",
            "rsx_dsp_get_surface: MRT targets C and D locations"),
    HANDLED(RSX_CLASS_NV4097, 0x0200, 12, RSX_EV_CONSUMER, "NV4097_SET_SURFACE_* (0x200..0x22C)",
            "rsx_commands.c process_surface_method into rsx_state, and rsx_dsp_get_surface on the "
            "register-file path: clip, format, pitches, offsets, colour target, zeta"),
    INERT(RSX_CLASS_NV4097, 0x0230, 1, RSX_EV_ABSENT_STAGE, "NV4097_HIERZ_OFFSET (rnndb)",
          "the hierarchical-Z compression block. Compression is a bandwidth optimization: the depth values "
          "the zeta surface holds are unchanged, and the engine reads the zeta surface itself "
          "(rsx_dsp_get_surface). RPCS3 models no hierz either"),
    INERT(RSX_CLASS_NV4097, 0x0238, 2, RSX_EV_ORACLE_NULL, "NV4097_SET_CYLINDRICAL_WRAP[0:1]",
          "NV30-era cylindrical texture wrap; RPCS3 binds nullptr with no decoder; the sampler decodes wrap "
          "from TEX_ADDRESS (rsx_dsp_get_texture.wrap), which is the field libgcm writes"),
    INERT(RSX_CLASS_NV4097, 0x0240, 4, RSX_EV_ABSENT_STAGE, "NV4097_SET_TEX_MATRIX_ENABLE[0:3] (rnndb)",
          "fixed-function texture matrices, a T&L stage the VP replaces; RPCS3 has no decoder"),
    HANDLED(RSX_CLASS_NV4097, 0x0280, 4, RSX_EV_CONSUMER, "NV4097_SET_SURFACE_PITCH_C/D, COLOR_C/DOFFSET",
            "the same two consumers for the MRT targets C and D"),
    MISSING(RSX_CLASS_NV4097, 0x02B8, 1, "NV4097_SET_WINDOW_OFFSET",
            "S2: the shader-window offset every window coordinate is translated by. The engine's epilogue "
            "hard-codes offset 0 (rsx_draw_engine.c xf[]), the value the census sees, but the register is "
            "not read, so a title that sets it would move geometry silently. RPCS3 decodes it (window_offset_x/y)"),
    INERT(RSX_CLASS_NV4097, 0x02BC, 1, RSX_EV_ORACLE_NULL, "NV4097_SET_WINDOW_CLIP_TYPE",
          "selects whether the window clip rectangles below discard or only clip; RPCS3 has no decoder for "
          "any of the window-clip registers"),
    INERT(RSX_CLASS_NV4097, 0x02C0, 0x10, RSX_EV_ORACLE_NULL, "NV4097_SET_WINDOW_CLIP_HORIZONTAL/VERTICAL[0:7]",
          "eight window clip rectangles (0x2C0..0x2FC); RPCS3 decodes none of them; the engine clips with "
          "the surface clip (rsx_dsp_get_surface) and the scissor, which is what the title sets"),
    INERT_V(RSX_CLASS_NV4097, 0x0300, 1, 1, 1, RSX_EV_ORACLE_VALUE, "NV4097_SET_DITHER_ENABLE",
            "enabled at reset (rsx_state::init 0x300=1). A dither matrix maps a value the ROP already holds "
            "at the target's precision (8-bit for the A8R8G8B8 targets the title uses), so the 4x4 ordered "
            "table at 0x440 (also written by the default state) is the identity on it; RPCS3 models no "
            "dither either. The row pins enable=1, the only value written"),
    MISSING(RSX_CLASS_NV4097, 0x031C, 1, "NV4097_SET_BLEND_COLOR",
            "S2: the constant blend colour. The engine builds every blend from the factors and equation "
            "and never binds a constant colour, so a draw whose factor names BLEND_CONSTANT uses the "
            "host default instead of the guest's value"),
    HANDLED(RSX_CLASS_NV4097, 0x032C, 1, RSX_EV_CONSUMER, "NV4097_SET_STENCIL_MASK",
            "rsx_draw_engine.c build_render_state: rs->s_write_mask (the front stencil write mask)"),
    HANDLED(RSX_CLASS_NV4097, 0x0348, 1, RSX_EV_CONSUMER, "NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE",
            "rsx_draw_engine.c build_render_state: rs->stencil_two_sided"),
    MISSING(RSX_CLASS_NV4097, 0x034C, 1, "NV4097_SET_BACK_STENCIL_MASK",
            "S2: the back-face stencil write mask. The engine reads the front mask only; Metal's stencil "
            "descriptor has per-face masks, so the fix is to carry the back three through "
            "rsx_be_render_state's stencil fields"),
    HANDLED(RSX_CLASS_NV4097, 0x0350, 1, RSX_EV_CONSUMER, "NV4097_SET_BACK_STENCIL_FUNC",
            "rsx_draw_engine.c build_render_state: rs->bs_func"),
    MISSING(RSX_CLASS_NV4097, 0x0354, 1, "NV4097_SET_BACK_STENCIL_FUNC_REF",
            "S2: with two-sided stencil the back faces compare against this reference; the engine passes one "
            "reference (front) to the encoder. Metal shares the reference value between faces, so a "
            "two-sided state that needs a different back reference cannot be expressed without emulation"),
    MISSING(RSX_CLASS_NV4097, 0x0358, 1, "NV4097_SET_BACK_STENCIL_FUNC_MASK",
            "S2: back-face compare mask, same row as 0x0354 (front mask is read, back is not)"),
    HANDLED(RSX_CLASS_NV4097, 0x035C, 3, RSX_EV_CONSUMER, "NV4097_SET_BACK_STENCIL_OP_*",
            "rsx_draw_engine.c build_render_state: rs->bs_fail/bs_zfail/bs_zpass"),
    INERT_V(RSX_CLASS_NV4097, 0x0368, 1, 0x1D01, 0xFFFF, RSX_EV_ORACLE_VALUE, "NV4097_SET_SHADE_MODE",
            "GL_SMOOTH at reset (0x1D01), which is what the MSL vertex outputs interpolate as. A flat shade "
            "mode would need an interpolation qualifier per varying; the row pins the smooth value"),
    HANDLED(RSX_CLASS_NV4097, 0x036C, 1, RSX_EV_CONSUMER, "NV4097_SET_BLEND_ENABLE_MRT",
            "rsx_draw_engine.c build_render_state: rs->blend_mrt"),
    HANDLED(RSX_CLASS_NV4097, 0x0370, 1, RSX_EV_CONSUMER, "NV4097_SET_COLOR_MASK_MRT",
            "rsx_draw_engine.c build_render_state: rs->color_mask_mrt"),
    INERT_V(RSX_CLASS_NV4097, 0x0374, 1, 0, 0x1, RSX_EV_ORACLE_VALUE, "NV4097_SET_LOGIC_OP_ENABLE",
            "logic op disabled at reset (enable 0); the engine has no logic-op stage (Metal has none either)"),
    INERT_V(RSX_CLASS_NV4097, 0x0378, 1, 0x00001503, 0xFFFFFFFF, RSX_EV_ORACLE_VALUE, "NV4097_SET_LOGIC_OP",
            "the logic operation itself (copy at reset); it cannot be applied while the enable above is 0, "
            "and the row pins the reset value so a change is reported"),
    INERT_V(RSX_CLASS_NV4097, 0x037C, 1, 0, 0xFFFFFFFF, RSX_EV_ORACLE_VALUE, "NV4097_SET_BLEND_COLOR2",
            "the second blend constant, 0 at reset; only a blend factor naming the constant-2 source reads "
            "it, and the engine implements the factors the title uses from BLEND_FUNC_*; RPCS3 decodes it "
            "but the value 0 makes it identical to the constant not being bound"),
    HANDLED(RSX_CLASS_NV4097, 0x0380, 3, RSX_EV_CONSUMER, "NV4097_SET_DEPTH_BOUNDS_*",
            "rsx_draw_engine.c build_render_state rs->depth_bounds_test and the per-draw set_depth_bounds "
            "(0x0384/0x0388 as raw floats)"),
    HANDLED(RSX_CLASS_NV4097, 0x0394, 1, RSX_EV_CONSUMER, "NV4097_SET_CLIP_MIN",
            "rsx_process_method rsx_state.clip_min / rsx_dsp_depth_range: the near end of the depth range "
            "passed to the Metal viewport (rsx_draw_engine.c draw, rsx_metal_backend.m)"),
    HANDLED(RSX_CLASS_NV4097, 0x0398, 1, RSX_EV_CONSUMER, "NV4097_SET_CLIP_MAX",
            "...the far end, same consumers"),
    INERT_V(RSX_CLASS_NV4097, 0x03B0, 1, 0x00100000, 0x00100000, RSX_EV_ORACLE_VALUE, "NV4097_SET_CONTROL0",
            "the mip-rounding mode bit (rnndb MIPMAP_ROUNDING, bit 20) is set at reset; it chooses which mip "
            "level a coordinate exactly between two levels samples, and the host sampler's own selection is "
            "what runs. The row pins the reset value so a title that flips it is reported missing"),
    INERT_V(RSX_CLASS_NV4097, 0x03B8, 1, 8, 0xFFFFFFFF, RSX_EV_ORACLE_VALUE, "NV4097_SET_LINE_WIDTH",
            "line width 8 = 1/8-pixel units = 1.0 px at reset; the engine draws the host's 1-px lines, "
            "which is what that value asks for"),
    INERT_V(RSX_CLASS_NV4097, 0x03BC, 1, 0, 0x1, RSX_EV_ORACLE_VALUE, "NV4097_SET_LINE_SMOOTH_ENABLE",
            "line smoothing off at reset; the engine draws unsmoothed lines, as the reset value asks"),
    INERT(RSX_CLASS_NV4097, 0x03C0, 1, RSX_EV_ORACLE_NULL, "NV4097_SET_ANISO_SPREAD",
          "the anisotropic LOD spread table; RPCS3 binds nullptr with no decoder and its samplers take the "
          "anisotropy from TEX_FILTER alone (rsx_dsp_get_texture.filter)"),
    INERT(RSX_CLASS_NV4097, 0x03C4, 0x0F, RSX_EV_ABSENT_STAGE, "NV4097 fixed-function TEX_MATRIX/TEX_GEN block",
          "0x3C4..0x3FC is the NV30 fixed-function texture-matrix/generator rung (rnndb gives it no NV40_3D "
          "variant); the VP replaces it and RPCS3 ignores the range"),
    INERT(RSX_CLASS_NV4097, 0x0400, 0x10, RSX_EV_ORACLE_NULL, "NV4097_UNK0400[0:15] (nv40.h NV40TCL_UNK0400)",
          "16 words of 0x7421 the default state writes; RPCS3 stores the same reset values under "
          "registers[0x400] with no name and no decoder, so not even the register's purpose is established; "
          "nothing this renderer has reads it"),
    INERT(RSX_CLASS_NV4097, 0x0440, 0x10, RSX_EV_ORACLE_NULL, "NV4097 dither matrix (rnndb DITHER_MATRIX)",
          "the 4x4 ordered dither table the default state writes; see 0x0300: the ROP output and the target "
          "are both 8-bit, where this table is the identity; RPCS3 stores it unnamed and never reads it"),
    INERT_V(RSX_CLASS_NV4097, 0x08CC, 1, 0x800, 0xFFFF, RSX_EV_ORACLE_VALUE, "NV4097_SET_FOG_MODE",
            "CELL_GCM_FOG_MODE_EXP at reset; the fog stage applies the FP's fog coordinate (o5.x) with this "
            "mode, and this renderer has no fog stage: RPCS3 decodes the mode but its fragment programs "
            "receive no fog term either when the title's FP writes the final colour. Value-pinned to the "
            "driver default because a non-default mode would need one"),
    INERT_V(RSX_CLASS_NV4097, 0x08D0, 3, 0, 0xFFFFFFFF, RSX_EV_ORACLE_VALUE, "NV4097_SET_FOG_PARAMS",
            "the fog equation constants, all 0 at reset (a zero exponential constant makes the fog factor "
            "0); same stage as 0x08CC"),
    INERT_V(RSX_CLASS_NV4097, 0x0A0C, 1, 0, 0xFFFF, RSX_EV_ORACLE_NULL, "NV4097_SET_POINT_CENTER_MODE",
            "whether a point sprite is centred on the pixel; RPCS3 binds nullptr with no decoder and the "
            "engine draws POINTS with the host's convention, value 0 = the default"),
    INERT_V(RSX_CLASS_NV4097, 0x0A60, 3, 0, 0x1, RSX_EV_ORACLE_VALUE, "NV4097_SET_POLY_OFFSET_*_ENABLE",
            "polygon-offset enables, all 0 at reset: no offset is applied, which is what the engine does "
            "(it sets no depth bias). A title that enables one needs RSX_BE depth bias, see the S2 rows"),
    INERT_V(RSX_CLASS_NV4097, 0x0A78, 2, 0, 0xFFFFFFFF, RSX_EV_ORACLE_VALUE, "NV4097_SET_POLYGON_OFFSET_*",
            "offset factor and bias, both 0 at reset; see 0x0A60"),
    INERT(RSX_CLASS_NV4097, 0x0B00, 0x10, RSX_EV_ORACLE_NULL, "NV4097_SET_TEXTURE_CONTROL2[0:15]",
          "the per-unit trilinear/aniso optimization hints (rnndb TEX_FILTER_OPTIMIZATION); RPCS3 binds "
          "nullptr with no decoder: a quality/bandwidth hint, not a filter selection "
          "(TEX_FILTER at 0x1A14 is the filter the sampler reads)"),
    INERT(RSX_CLASS_NV4097, 0x0B40, 0x0A, RSX_EV_ORACLE_NULL, "NV4097_SET_TEX_COORD_CONTROL[0:9]",
          "per-texcoord interpolation mode (2D/linear, centroid). RPCS3 binds nullptr with no decoder and "
          "interpolates perspective-correct always, as this engine does; the RESC convert stream sets "
          "2D=1 for a clip-space quad of constant w, where the two modes agree"),
    INERT(RSX_CLASS_NV4097, 0x1428, 1, RSX_EV_ABSENT_STAGE, "NV4097_SET_SPECULAR_ENABLE",
          "fixed-function lighting: whether the specular term is added after texturing; the VP computes "
          "colours. RPCS3 has no decoder"),
    INERT(RSX_CLASS_NV4097, 0x142C, 1, RSX_EV_ABSENT_STAGE, "NV4097_SET_TWO_SIDE_LIGHT_EN",
          "fixed-function two-sided lighting (front/back colour select). With a VP the back-face colours "
          "are outputs o3/o4, which no consumer of this renderer binds; RPCS3 reads the flag in its VP "
          "translation only for titles that use the fixed-function path. Value 0 at reset"),
    INERT(RSX_CLASS_NV4097, 0x1450, 1, RSX_EV_ORACLE_NULL, "NV4097_SET_PERFORMANCE_PARAMS",
          "PGRAPH performance counters/thresholds; RPCS3 has no decoder and nothing in the pipeline reads a "
          "performance register"),
    INERT(RSX_CLASS_NV4097, 0x1454, 1, RSX_EV_ABSENT_STAGE, "NV4097_SET_FLAT_SHADE_OP",
          "fixed-function flat shading of the primary/secondary colour; RPCS3 has no decoder (it gets flat "
          "shading from the shader's own interpolation qualifiers)"),
    INERT(RSX_CLASS_NV4097, 0x145C, 1, RSX_EV_ABSENT_STAGE, "NV4097_SET_EDGE_FLAG",
          "fixed-function edge flag; RPCS3 has no decoder"),
    INERT(RSX_CLASS_NV4097, 0x1478, 1, RSX_EV_ORACLE_NULL, "NV4097_SET_USER_CLIP_PLANE_CONTROL",
          "which of the VP's clip distances (o[9..14] on NV30 numbering) the fixed-function clipper tests; "
          "this renderer clips with the host's clip volume only. RPCS3 decodes the enable mask for its VP "
          "translation; value 0 = no user clip plane, the reset value"),
    INERT(RSX_CLASS_NV4097, 0x147C, 1, RSX_EV_ABSENT_STAGE, "NV4097_SET_POLYGON_STIPPLE",
          "polygon stipple, a fragment-stage mask of the fixed-function ROP; RPCS3 decodes it but the engine "
          "has no stipple stage; value 0 = disabled, the reset value"),
    INERT(RSX_CLASS_NV4097, 0x1710, 1, RSX_EV_ORDER, "NV4097_INVALIDATE_VERTEX_CACHE_FILE",
          "drop the post-transform vertex cache; the engine fetches every vertex from guest memory for every "
          "draw (rsx_vertex_fetch.c), so no cache exists to invalidate, and the invalidate is issued before "
          "the draw it protects"),
    HANDLED(RSX_CLASS_NV4097, 0x1714, 1, RSX_EV_CONSUMER, "NV4097_INVALIDATE_VERTEX_FILE",
            "rsx_dispatch.c dsp_fill_classes marks it EXEC: the engine keeps no post-transform vertex "
            "cache, so the invalidate has nothing to drop and every draw re-fetches"),
    HANDLED(RSX_CLASS_NV4097, 0x1738, 2, RSX_EV_CONSUMER, "NV4097_SET_VERTEX_DATA_BASE_OFFSET/INDEX",
            "rsx_dsp_vertex_data_base_offset/index: added to every vertex fetch and indexed draw"),
    INERT_V(RSX_CLASS_NV4097, 0x17CC, 1, 0, 0x1, RSX_EV_ORACLE_VALUE, "NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE",
            "zpass counting enable, 0 at reset; RPCS3 binds set_zcull_pixel_count_enable, this renderer has "
            "no occlusion query (GET_REPORT answers 0xFFFF for a ZPASS query, cellGcmSys.c)"),
    HANDLED(RSX_CLASS_NV4097, 0x1800, 1, RSX_EV_CONSUMER, "NV4097_GET_REPORT",
            "cellGcmSys.c gcm_rsx_process_fifo_unlocked: writes the report slot (or main-memory report "
            "area) named by SET_CONTEXT_DMA_REPORT, with the chunk's timestamp"),
    INERT(RSX_CLASS_NV4097, 0x1828, 2, RSX_EV_ORACLE_NULL, "NV4097_SET_FRONT/BACK_POLYGON_MODE",
          "fill/line/point rasterization mode; RPCS3 binds nullptr with a decoder it never acts on, and the "
          "engine rasterizes triangles: the title writes the fill mode"),
    INERT(RSX_CLASS_NV4097, 0x1838, 1, RSX_EV_ORACLE_NULL, "NV4097_SET_POLY_SMOOTH_ENABLE",
          "polygon antialiasing via coverage; RPCS3 decodes it and no host pipeline models coverage AA; the "
          "engine's MSAA comes from the surface's antialias field (rsx_dsp_get_surface.antialias)"),
    INERT(RSX_CLASS_NV4097, 0x1D64, 1, RSX_EV_ORACLE_NULL, "NV4097_SET_INDEXED_CONSTANT_READ_LIMITS",
          "bounds the constant indices a VP may read (0x02000000 at reset); RPCS3 has no decoder, and the "
          "decompiler bounds every constant read to RSX_DSP_NUM_CONSTANTS (rsx_dispatch.c)"),
    INERT_V(RSX_CLASS_NV4097, 0x1D78, 1, 1, 0xFFFF, RSX_EV_ORACLE_VALUE, "NV4097_SET_ZMIN_MAX_CONTROL",
            "depth-test direction/rounding controls; the engine reads the engine-visible depth state from "
            "DEPTH_FUNC/MASK/TEST (0x0A6C..0x0A74); RPCS3 binds set_zmin_max_control for the same purpose"),
    INERT_V(RSX_CLASS_NV4097, 0x1D7C, 1, 0xFFFF0000, 0xFFFFFFFF, RSX_EV_ORACLE_VALUE,
            "NV4097_SET_ANTI_ALIASING_CONTROL",
            "AA sample placement; the engine takes the sample grid from the surface format's antialias field "
            "(rsx_dsp_get_surface), which is what libgcm sets together with this register; value = reset"),
    INERT_V(RSX_CLASS_NV4097, 0x1D80, 1, 3, 0xFFFFFFFF, RSX_EV_ORACLE_NULL, "NV4097_SET_SURFACE_COMPRESSION",
            "zeta/colour compression enable; no compression in this renderer (the surface is read and "
            "written uncompressed), RPCS3 has no decoder; value 3 = the reset value"),
    MISSING(RSX_CLASS_NV4097, 0x1D88, 1, "NV4097_SET_SHADER_WINDOW",
            "S2: window height, origin (top/bottom) and pixel centre. The engine's epilogue hard-codes a "
            "top-left origin (rsx_draw_engine.c xf[1]/xf[5]) while the register says origin=BOTTOM at reset "
            "(0x1000); whether the two agree depends on the convention, which only a reference-frame "
            "comparison settles (M3 non-goal). RPCS3 decodes all three fields"),
    INERT_V(RSX_CLASS_NV4097, 0x1D98, 2, 0x0FFF0000, 0xFFFFFFFF, RSX_EV_ORACLE_VALUE,
            "NV4097_SET_CLEAR_RECT_HORIZONTAL/VERTICAL",
            "the rectangle CLEAR_SURFACE covers; 0x0FFF0000/0x0FFF0000 at reset, i.e. the full 4096x4096 "
            "window, so clearing the attachment is the same clear. The engine clears the bound target "
            "(rsx_draw_engine.c eng_clear), which is the reset rectangle's meaning for every surface it owns"),
    INERT_V(RSX_CLASS_NV4097, 0x1DA4, 1, 0, 1, RSX_EV_ORACLE_NULL, "NV4097_SET_CLIP_ID_TEST_ENABLE",
            "clip-id conditional rendering enable, 0 at reset; RPCS3 has no decoder and this renderer draws "
            "every batch"),
    INERT(RSX_CLASS_NV4097, 0x1DB4, 1, RSX_EV_ORACLE_NULL, "NV4097_SET_LINE_STIPPLE",
          "line stipple enable/pattern; RPCS3 has no decoder; the engine draws lines solid, value 0"),
    INERT(RSX_CLASS_NV4097, 0x1E94, 1, RSX_EV_ABSENT_STAGE, "NV4097_SET_TRANSFORM_EXECUTION_MODE",
          "VPE execution mode (fixed-function vs program). The engine always runs the VP "
          "(rsx_dsp_vp_start / TRANSFORM_PROGRAM_LOAD) because libgcm titles always load one; RPCS3 has no "
          "decoder"),
    INERT_V(RSX_CLASS_NV4097, 0x1E98, 1, 0x01000000, 0xFFFFFFFF, RSX_EV_ORACLE_VALUE, "NV4097_SET_RENDER_ENABLE",
            "the VPE render-enable word; the engine runs the vertex program unconditionally (libgcm titles "
            "always bind one, and RPCS3's set_render_mode only tracks which pipe emits). Value = reset"),
    INERT_V(RSX_CLASS_NV4097, 0x1EE0, 1, 0x3F800000, 0xFFFFFFFF, RSX_EV_ORACLE_VALUE, "NV4097_SET_POINT_SIZE",
            "point size 1.0 at reset; the engine draws POINTS as 1-px points, which is 1.0"),
    INERT_V(RSX_CLASS_NV4097, 0x1EE4, 2, 0, 0xFFFFFFFF, RSX_EV_ORACLE_VALUE, "NV4097_SET_POINT_PARAMS_ENABLE",
            "point parameters/sprite enable, both 0 at reset; point sprites need a sprite coordinate the "
            "engine does not generate, and the reset value asks for none"),
    INERT_V(RSX_CLASS_NV4097, 0x1EF8, 1, 0x0020FFFF, 0xFFFFFFFF, RSX_EV_ORACLE_NULL, "NV4097_SET_TRANSFORM_TIMEOUT",
            "the VPE's timeout and watchdog threshold: a hang detector for the transform engine, not state a "
            "draw reads. RPCS3 binds nullptr with no decoder"),
    INERT(RSX_CLASS_NV4097, 0x1FC4, 5, RSX_EV_ABSENT_STAGE, "NV4097_SET_ATTRIB_COLOR/TEX_COORD/TEX_COORD_EX/UCLIP0/UCLIP1",
          "the fixed-function attribute *routing* (which VP output carries COL0/TEX0/...). The VP decompiler "
          "emits the hardware's reset routing directly (o0=HPOS, o1=COL0, o7..o15=TEX0..8; "
          "rsx_vp_decompiler.c header), which is the value the driver writes; RPCS3 has no decoder for these "
          "because it names the VP's outputs itself"),
    INERT(RSX_CLASS_NV4097, 0x1FE0, 1, RSX_EV_ORACLE_NULL, "NV4097_SET_REDUCE_DST_COLOR",
          "whether the reduce stage may read the destination; RPCS3 has no decoder; value 1 = reset"),
    INERT(RSX_CLASS_NV4097, 0x1FE8, 1, RSX_EV_ORACLE_NULL, "NV4097_SET_NO_PARANOID_TEXTURE_FETCHES",
          "relaxes texture-fetch bounds checks; RPCS3 has no decoder. The engine validates every texture "
          "span before sampling (rsx_texture_layout.c, rsx_draw_engine.c sink_bind_textures)"),
    HANDLED(RSX_CLASS_NV4097, 0x1FEC, 1, RSX_EV_CONSUMER, "NV4097_SET_SHADER_PACKER",
            "rsx_draw_engine.c fragment-pipeline key (the register is read with the fragment program and "
            "SHADER_CONTROL to decide the output packing)"),

/* ---- NV0039 M2MF ---------------------------------------------------------------------------- */
    HANDLED(RSX_CLASS_NV0039, 0x0000, 1, RSX_EV_CONSUMER, "NV0039_SET_OBJECT",
            "cellGcmSys.c gcm_bind_object / gcm_nv0039_method case 0"),
    INERT(RSX_CLASS_NV0039, 0x0180, 1, RSX_EV_ORACLE_NULL, "NV0039_SET_CONTEXT_DMA_NOTIFIES",
          "the notifier object the hardware would write on BUFFER_NOTIFY; RPCS3's nv0039::buffer_notify "
          "never reads it (the notify argument only reaches a trace line) and this walker moves the same "
          "bytes without raising a notify"),
    HANDLED(RSX_CLASS_NV0039, 0x0184, 1, RSX_EV_CONSUMER, "NV0039_SET_CONTEXT_DMA_BUFFER_IN",
            "cellGcmSys.c gcm_nv0039_method: the copy source location"),
    HANDLED(RSX_CLASS_NV0039, 0x0188, 1, RSX_EV_CONSUMER, "NV0039_SET_CONTEXT_DMA_BUFFER_OUT",
            "cellGcmSys.c gcm_nv0039_method: the copy destination location"),
    HANDLED(RSX_CLASS_NV0039, 0x030C, 1, RSX_EV_CONSUMER, "NV0039_OFFSET_IN", "gcm_nv0039_method / gcm_nv0039_copy"),
    HANDLED(RSX_CLASS_NV0039, 0x0310, 1, RSX_EV_CONSUMER, "NV0039_OFFSET_OUT", "gcm_nv0039_method / gcm_nv0039_copy"),
    HANDLED(RSX_CLASS_NV0039, 0x0314, 5, RSX_EV_CONSUMER, "NV0039_PITCH_IN..FORMAT", "gcm_nv0039_method / gcm_nv0039_copy"),
    HANDLED(RSX_CLASS_NV0039, 0x0328, 1, RSX_EV_CONSUMER, "NV0039_BUFFER_NOTIFY", "gcm_nv0039_method: the copy trigger"),

/* ---- NV3062 context surfaces 2D ------------------------------------------------------------- */
    HANDLED(RSX_CLASS_NV3062, 0x0000, 1, RSX_EV_CONSUMER, "NV3062_SET_OBJECT",
            "cellGcmSys.c gcm_bind_object: routes the slot's 2D methods"),
    INERT(RSX_CLASS_NV3062, 0x0180, 1, RSX_EV_ORACLE_NULL, "NV3062_SET_CONTEXT_DMA_NOTIFIES",
          "the 2D object's notify context; gcm_2d_method never raises a notify (the copy runs inline, "
          "cellGcmSys.c) and RPCS3 binds nullptr with no decoder"),
    HANDLED(RSX_CLASS_NV3062, 0x0300, 1, RSX_EV_CONSUMER, "NV3062_SET_COLOR_FORMAT", "gcm_2d_method: s_gcm2d.color_fmt"),
    HANDLED(RSX_CLASS_NV3062, 0x0304, 1, RSX_EV_CONSUMER, "NV3062_SET_PITCH", "gcm_2d_method: s_gcm2d.pitch"),
    HANDLED(RSX_CLASS_NV3062, 0x030C, 1, RSX_EV_CONSUMER, "NV3062_SET_OFFSET_DESTIN", "gcm_2d_method: s_gcm2d.dst_offset"),

/* ---- NV308A image from CPU ------------------------------------------------------------------ */
    HANDLED(RSX_CLASS_NV308A, 0x0000, 1, RSX_EV_CONSUMER, "NV308A_SET_OBJECT",
            "cellGcmSys.c gcm_bind_object: routes the slot's 2D methods"),
    INERT(RSX_CLASS_NV308A, 0x0180, 1, RSX_EV_ORACLE_NULL, "NV308A_SET_CONTEXT_DMA_NOTIFIES",
          "see NV3062 0x0180: the 2D path raises no notify and RPCS3 has no decoder"),
    INERT(RSX_CLASS_NV308A, 0x0184, 6, RSX_EV_NO_TRANSFER, "NV308A_SET_CONTEXT_COLOR_KEY..BETA4",
          "the inline-transfer raster-op context (colour key, clip rectangle, pattern, ROP, beta). Odin "
          "sends no NV308A_COLOR (0x400) and no SIZE_OUT/SIZE_IN write in the census, so this object "
          "performs no transfer and applies no raster op; gcm_2d_method writes the COLOR words as a plain "
          "copy when one does arrive"),
    INERT(RSX_CLASS_NV308A, 0x019C, 1, RSX_EV_REDUNDANT, "NV308A_SET_CONTEXT_SURFACE",
          "names the destination surface object; the 2D path has exactly one destination state, the NV3062 "
          "object's (s_gcm2d), which is the object this handle names (value 0x313371C3)"),
    INERT(RSX_CLASS_NV308A, 0x02FC, 1, RSX_EV_NO_TRANSFER, "NV308A_SET_OPERATION",
          "the transfer's blend operation; see 0x0184: no transfer is issued by this object in the census"),

/* ---- NV309E context surface swizzled -------------------------------------------------------- */
    HANDLED(RSX_CLASS_NV309E, 0x0000, 1, RSX_EV_CONSUMER, "NV309E_SET_OBJECT",
            "cellGcmSys.c gcm_bind_object: routes the slot's 2D methods"),
    INERT(RSX_CLASS_NV309E, 0x0180, 1, RSX_EV_ORACLE_NULL, "NV309E_SET_CONTEXT_DMA_NOTIFIES",
          "see NV3062 0x0180"),
    INERT_V(RSX_CLASS_NV309E, 0x0184, 1, 0xFEED0000, 0xFFFFFFFF, RSX_EV_REDUNDANT,
            "NV309E_SET_CONTEXT_DMA_IMAGE",
            "the destination image object; the swizzled path writes local memory unconditionally "
            "(gcm_2d_method: cellGcmResolveLocated(1, s_nv309e.offset)), and the value here says local"),

/* ---- NV3089 scaled image from memory -------------------------------------------------------- */
    HANDLED(RSX_CLASS_NV3089, 0x0000, 1, RSX_EV_CONSUMER, "NV3089_SET_OBJECT",
            "cellGcmSys.c gcm_bind_object: routes the slot's 2D methods"),
    INERT(RSX_CLASS_NV3089, 0x0180, 1, RSX_EV_ORACLE_NULL, "NV3089_SET_CONTEXT_DMA_NOTIFIES",
          "see NV3062 0x0180"),
    HANDLED(RSX_CLASS_DRIVER, 0x0000, 1, RSX_EV_CONSUMER, "SCE_DRIVER_SET_OBJECT",
            "cellGcmSys.c gcm_bind_object: the driver object makes 0xE9xx/0xEBxx words on the slot driver commands"),

/* ---- NV406E channel control (methods < 0x100 are this class on every subchannel) ------------ */
};
#undef HANDLED
#undef INERT
#undef INERT_V
#undef MISSING

#define RSX_METHOD_RULE_COUNT (sizeof rsx_method_rules / sizeof rsx_method_rules[0])

/* The row a (class, method) falls in, or NULL when nothing classifies it. Arrays are stored once. */
static inline const rsx_method_rule* rsx_method_rule_find(rsx_class_id c, uint32_t method)
{
    for (unsigned i = 0; i < RSX_METHOD_RULE_COUNT; i++) {
        const rsx_method_rule* r = &rsx_method_rules[i];
        if (r->cls != (uint8_t)c || method < r->method) continue;
        /* count is a number of consecutive dword methods (an array register is one row) */
        if (method < (uint32_t)r->method + (uint32_t)r->count * 4u) return r;
    }
    return NULL;
}

/* Value-aware verdict: an INERT row only applies to the value it pins (mask 0 = any value). A method
 * with no row is UNKNOWN: the caller's consumer already said it does not read it. */
static inline rsx_method_verdict rsx_method_check(rsx_class_id c, uint32_t method, uint32_t data,
                                                  const rsx_method_rule** out_rule)
{
    const rsx_method_rule* r = rsx_method_rule_find(c, method);
    if (out_rule) *out_rule = r;
    if (!r) return RSX_METHOD_UNKNOWN;
    if (r->verdict == RSX_METHOD_INERT && r->mask && (data & r->mask) != (r->data & r->mask))
        return RSX_METHOD_MISSING;
    return (rsx_method_verdict)r->verdict;
}

/* The allow-list predicate rsx_commands.c uses (it has no register values to compare): true when the
 * method is allow-listed for the value it pins, i.e. some value is inert. */
static inline int rsx_method_inert(rsx_class_id c, uint32_t method)
{
    const rsx_method_rule* r = rsx_method_rule_find(c, method);
    return r && r->verdict == RSX_METHOD_INERT;
}

#endif

/*
 * ps3recomp - cellResc HLE implementation
 *
 * The library's configuration state (init config, display mode, Dsts and Src records, buffer
 * addresses, convert programs) and the command stream cellRescSetConvertAndFlip appends to the
 * title's GCM context (resc_convert.c). Behaviour follows libresc FW 4.93; firmware addresses are
 * cited where a rule comes from them. Rendering is the RSX backend's: this module only emits
 * the same commands the firmware does.
 */

#include "cellResc.h"
#include "ps3emu/stop.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: translate + byte-swap */
#include "../../runtime/memory/vm.h"        /* vm_range_ok, vm_guest_limit */
#include "ps3emu/guest_call.h"   /* ps3_invoke_guest: handlers are guest OPDs */
#include "cellGcmSys.h"          /* display buffers, address->offset, the flip command */
#include "resc_convert.h"        /* the convert-and-flip command stream */
#include <stdio.h>
#include <string.h>
#include "ps3emu/log_first.h"

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_initialized = 0;
static CellRescInitConfig s_config;
static u32 s_display_mode = CELL_RESC_1280x720;
static CellRescSrc s_src[8]; /* up to 8 color buffers */
static CellRescDsts s_dsts[4]; /* one per display mode */
static float s_aspect_h = 1.0f;
static float s_aspect_v = 1.0f;
static float s_pal_ratio = 0.5f;

/* Library object +0x124: the destination buffer size cellRescSetDisplayMode took from the mode's
 * Dsts record (see resc_dsts_buffer_size). */
static u32 s_dst_buffer_size = 0;

/* The SDK version lv2 syscall 25 reports: the title's PROC_PARAM, loaded by ppu_load_elf
 * (runtime/syscalls/sdk_version.c). */
extern u32 ps3_process_sdk_version(void);

/* True when the library keeps its own flip flag instead of asking libgcm (firmware 020044D0, 02004534,
 * 02003704, 02003794, 02004590): the stored display mode (+0xD0) is 2 (720x576) and the PAL temporal
 * mode (+0x10) is neither 0 nor 5. Every other configuration, 720p included, is a plain call to the
 * libgcm export. The library flag (+0x1D8) is set by the library's own flip handler and not modelled:
 * that configuration stops instead of answering. */
static int resc_has_own_flip_flag(void)
{
    return s_display_mode == CELL_RESC_720x576 && s_config.palTemporalMode != 0 && s_config.palTemporalMode != 5;
}

static int resc_dsts_index(u32 mode);
static u32 resc_dsts_buffer_size(u32 mode, const CellRescDsts* rec);

/* The library's four convert fragment programs, one per (PAL interpolate mode,
 * interlace) case. Firmware 02003A2C..02003B10 loads them into the library
 * object at +0xDC, +0xE0, +0xE4 and +0xE8 from embedded Cg binaries; +0xFC
 * points at the current one, and cellRescSetRegisterCount patches the register
 * count byte of that program's header (blob header + 0x12, 02008B3C).
 *
 * The defaults are the byte at +0x12 of each embedded fragment program's header
 * (profile 0x1B5C, 7004): 2, 3, 4, 4. The counts live in the library's data,
 * not in the per-Init state, so they are not reset by cellRescInit/Exit
 * [INFERENCE: the blobs are patched in place; nothing reloads them]. */
enum { RESC_PROG_DC, RESC_PROG_E0, RESC_PROG_E4, RESC_PROG_E8, RESC_PROG_COUNT };
static u8 s_prog_regcount[RESC_PROG_COUNT] = {2, 3, 4, 4};
static int s_cur_prog = RESC_PROG_DC;

/* The library's convert fragment programs by the header fields the emitter reads (see
 * RescFragProgram). Embedded Cg binaries (profile 0x1B5C, FW 4.93): the attribute input mask at
 * +0x04, the texcoord masks at +0x0C/+0x0E/+0x10, no output-from-R0/depth/kill flags. The
 * register count comes from s_prog_regcount (cellRescSetRegisterCount patches it). */
static const RescFragProgram k_prog_desc[RESC_PROG_COUNT] = {
    /* DC */ { 0x4000, 0x0001, 0xFFFF, 0, 2, 0, 0, 0, 0 },
    /* E0 */ { 0xC000, 0x0003, 0xFFFF, 0, 3, 0, 0, 0, 0 },
    /* E4 */ { 0x4000, 0x0001, 0xFFFF, 0, 4, 0, 0, 0, 0 },
    /* E8 */ { 0xC000, 0x0003, 0xFFFF, 0, 4, 0, 0, 0, 0 },
};

/* More of the library object the convert stream reads (firmware offsets in comments). The mode
 * size and pitch are snapshots cellRescSetDisplayMode takes: a later SetDsts changes the Dsts
 * record but not these. */
#define RESC_MAX_COLOR_BUFFERS 6
static u32 s_mode_width, s_mode_height, s_mode_pitch;   /* +0x114, +0x118, +0x11C */
static u32 s_buf_color_ea, s_buf_vertex_ea, s_buf_fragment_ea;   /* +0x100, +0x104, +0x108 */
static u32 s_color_offset[RESC_MAX_COLOR_BUFFERS];     /* +0x128 + 4 * i */
static u32 s_vertex_count;                              /* +0x140 */
static u32 s_frame_toggle;                              /* +0x110: this frame's colour buffer */
static u32 s_prog_ucode_offset[RESC_PROG_COUNT];        /* program object +0x10, -1 until copied */
static u8  s_attr[3];                                   /* +0x152..+0x154 vertex attribute slots */
static u8  s_tex_unit;                                  /* +0x155 source texture unit */

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellRescInit(const CellRescInitConfig* initConfig)
{
    printf("[cellResc] Init()\n");

    if (s_initialized)
        return (s32)CELL_RESC_ERROR_REINITIALIZED;

    if (!initConfig)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* Decoded to host order, as the firmware does (memset 28 bytes, then copy
     * `size` bytes into the library object): fields past `size` stay 0. */
    {
        const u32 ea = (u32)(uintptr_t)initConfig;
        u32 f[7] = {0};
        const u32 size = vm_read32(ea);
        for (u32 i = 0; i < 7 && i * 4 < size; i++)
            f[i] = vm_read32(ea + i * 4);
        s_config.size = f[0];
        s_config.resourcePolicy = f[1];
        s_config.supportModes = f[2];
        s_config.ratioMode = f[3];
        s_config.palTemporalMode = f[4];
        s_config.interlaceMode = f[5];
        s_config.flipMode = f[6];
    }
    memset(s_src, 0, sizeof(s_src));
    /* Firmware 02003680..020036DC: each mode's Dsts record starts as {A8R8G8B8 (8), the tiled
     * pitch of width * 4, height alignment 8}, so a title that never calls SetDsts still has a
     * usable destination (and the size SetDisplayMode derives from it is the plain frame size). */
    {
        static const u32 k_mode_width[4] = { 720, 720, 1280, 1920 };
        for (int i = 0; i < 4; i++) {
            s_dsts[i].format = 8;
            s_dsts[i].pitch = cellGcmGetTiledPitchSize(k_mode_width[i] * 4u);
            s_dsts[i].heightAlign = 8;
        }
    }
    s_aspect_h = 1.0f;
    s_aspect_v = 1.0f;
    s_cur_prog = RESC_PROG_DC;   /* 02003A2C: +0xFC = +0xDC */
    s_mode_width = s_mode_height = s_mode_pitch = 0;
    s_dst_buffer_size = 0;
    s_buf_color_ea = s_buf_vertex_ea = s_buf_fragment_ea = 0;
    memset(s_color_offset, 0, sizeof s_color_offset);
    s_vertex_count = 0;
    s_frame_toggle = 0;
    for (int i = 0; i < RESC_PROG_COUNT; i++)
        s_prog_ucode_offset[i] = 0xFFFFFFFFu;   /* program object ctor 02007AC8: +0x10 = -1 */
    s_attr[0] = 0; s_attr[1] = 8; s_attr[2] = 9;   /* 02002490: the vertex program's input slots */
    s_tex_unit = 0xFF;                              /* 02003678: unresolved until SetBufferAddress */
    s_initialized = 1;
    return CELL_OK;
}

void cellRescExit(void)
{
    printf("[cellResc] Exit()\n");
    s_initialized = 0;
}

/* Firmware 02004774. In order: not initialised -> NOT_INITIALIZED; a mode
 * outside the config's supportModes, or not one of 1/2/4/8 -> BAD_ARGUMENT;
 * the mode is stored (+0xD0); for 720x576 a PAL temporal mode that the flip
 * mode forbids -> BAD_COMBINATION (PAL_60_DROP .. _DROP_FLEXIBLE with flipMode
 * 1, or palTemporalMode 5 with flipMode 0). Then the convert program is
 * picked (02004900..02004A28): 720x576 with PAL_60_INTERPOLATE..._FLEXIBLE
 * uses E8 when interlaceMode is 1 and E0 otherwise; every other case E4 when
 * interlaceMode is 1 and DC otherwise. The rest of the firmware function
 * programs the RSX through cellGcm and is not modelled here. */
s32 cellRescSetDisplayMode(u32 displayMode)
{
    printf("[cellResc] SetDisplayMode(0x%x)\n", displayMode);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if ((displayMode & s_config.supportModes) == 0)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;
    if (displayMode != CELL_RESC_720x480 && displayMode != CELL_RESC_720x576 &&
        displayMode != CELL_RESC_1280x720 && displayMode != CELL_RESC_1920x1080)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    s_display_mode = displayMode;

    const u32 pal = s_config.palTemporalMode;
    if (displayMode == CELL_RESC_720x576) {
        if (pal - 2u <= 2u || pal == CELL_RESC_PAL_60_DROP) {
            if (s_config.flipMode == 1)
                return (s32)CELL_RESC_ERROR_BAD_COMBINATION;
        } else if (pal == 5) {
            if (s_config.flipMode == 0)
                return (s32)CELL_RESC_ERROR_BAD_COMBINATION;
        }
    }

    /* 02004878..020048DC: the destination buffer size, from the record SetDsts stored for this
     * mode so far (zero if there is none yet: the title may call SetDsts afterwards, and the
     * firmware does not recompute). Kept for cellRescGetBufferSize's constant-VRAM case. */
    s_dst_buffer_size = resc_dsts_buffer_size(displayMode, &s_dsts[resc_dsts_index(displayMode)]);
    {   /* 020048AC..020048DC: size and pitch snapshots (the heights are 02007CB8's) */
        const int ri = resc_dsts_index(displayMode);
        s_mode_width  = displayMode == CELL_RESC_1920x1080 ? 1920 : displayMode == CELL_RESC_1280x720 ? 1280 : 720;
        s_mode_height = displayMode == CELL_RESC_1920x1080 ? 1080 : displayMode == CELL_RESC_1280x720 ? 720 :
                        displayMode == CELL_RESC_720x576 ? 576 : 480;
        s_mode_pitch  = s_dsts[ri].pitch;
    }

    const int interlaced = (s_config.interlaceMode == 1);
    if (displayMode == CELL_RESC_720x576 && pal - 2u <= 2u)
        s_cur_prog = interlaced ? RESC_PROG_E8 : RESC_PROG_E0;
    else
        s_cur_prog = interlaced ? RESC_PROG_E4 : RESC_PROG_DC;
    return CELL_OK;
}

/* Firmware 020003A4 (void): patches the register count byte of the current
 * convert fragment program (02008B3C). Before cellRescInit, and after Exit
 * (02003B3C clears +0xFC), the firmware dereferences a NULL program pointer, so
 * there is no faithful result: named stop. */
void cellRescSetRegisterCount(u8 regCount)
{
    if (!s_initialized)
        ps3_stop_hle_uninitialized("cellRescSetRegisterCount", "cellResc",
                                   "the firmware reads the current program through the library object's +0xFC pointer, which only cellRescInit sets");
    s_prog_regcount[s_cur_prog] = regCount;
}

/* Firmware 020003D8: the register count byte of the current convert fragment
 * program (02008AD8), zero-extended into an s32. Same stop before Init. */
s32 cellRescGetRegisterCount(void)
{
    if (!s_initialized)
        ps3_stop_hle_uninitialized("cellRescGetRegisterCount", "cellResc",
                                   "the firmware reads the current program through the library object's +0xFC pointer, which only cellRescInit sets");
    return (s32)s_prog_regcount[s_cur_prog];
}

/* Host accessor for the convert-and-flip emission: the register count the
 * current convert fragment program carries, 0 while uninitialised. Not a
 * firmware export (the `cellResc_` shape keeps it out of the NID table). */
u32 cellResc_convert_register_count(void)
{
    return s_initialized ? s_prog_regcount[s_cur_prog] : 0;
}

/* Returns the number of colour buffers (the count is the return value, not an
 * out-parameter). Firmware 02000194: no initialisation check; a non-zero
 * `reserved` is BAD_ARGUMENT; only the 720x576 destination mode depends on the
 * PAL temporal mode (DROP -> 3, INTERPOLATE / _30_DROP / _FLEXIBLE -> 6),
 * every other destination gets 2. */
s32 cellRescGetNumColorBuffers(u32 dstMode, u32 palTemporalMode, u32 reserved)
{
    if (reserved != 0)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    if (dstMode != CELL_RESC_720x576)
        return 2;

    switch (palTemporalMode) {
    case CELL_RESC_PAL_60_INTERPOLATE:
    case CELL_RESC_PAL_60_INTERPOLATE_30_DROP:
    case CELL_RESC_PAL_60_INTERPOLATE_DROP_FLEXIBLE:
        return 6;
    case CELL_RESC_PAL_60_DROP:
        return 3;
    default:
        return 2;
    }
}

/* Firmware 02000278 -> 0200089C: NULL output -> BAD_ARGUMENT; the VideoOut
 * resolution ids 1 (1080), 2 (720), 4 (480), 5 (576) map to the RESC buffer
 * modes 8, 4, 1, 2; every other id (3 included) is BAD_ARGUMENT with the output
 * untouched. No initialisation check: titles call this before cellRescInit. */
s32 cellRescVideoOutResolutionId2RescBufferMode(u32 resolutionId, u32* bufferMode)
{
    if (!bufferMode)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    u32 mode;
    switch (resolutionId) {
    case 1: mode = CELL_RESC_1920x1080; break;
    case 2: mode = CELL_RESC_1280x720;  break;
    case 4: mode = CELL_RESC_720x480;   break;
    case 5: mode = CELL_RESC_720x576;   break;
    default:
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;
    }
    vm_write32((uint32_t)(uintptr_t)bufferMode, mode);
    return CELL_OK;
}

/* Firmware 020002B0 -> 02003EF0. Offsets into the guest CellGcmSurface
 * (type@0 antialias@1 colorFormat@2 colorOffset[4]@8 colorPitch[4]@0x18
 * width@0x34 height@0x36) and the 16-byte CellRescSrc it fills
 * (format@0 pitch@4 width@8 height@0xA offset@0xC).
 *
 * No initialisation check. Either pointer NULL -> BAD_ARGUMENT. The format is
 * the NV4097 texture format byte of colour target 0 (A8R8G8B8 0x85,
 * F_W16Z16Y16X16 0x9A, +0x20 for a pitch-layout surface; 02007D3C), 0xFF for
 * any other surface format or layout. width/height are the surface's, scaled
 * for the anti-aliasing mode only when the init config's `size` is 24 or 28:
 * antialias 3 -> 2x1, 4 and 5 -> 2x2, everything else 1x1. */
#define GCM_SURFACE_TYPE_PITCH   1
#define GCM_SURFACE_TYPE_SWIZZLE 2

static u32 resc_surface_texture_format(u32 color_format, u32 type)
{
    u32 base;
    if (color_format == 8)
        base = 0x85;
    else if (color_format == 11)
        base = 0x9A;
    else
        return 0xFF;
    if (type == GCM_SURFACE_TYPE_PITCH)
        return base | 0x20;
    if (type == GCM_SURFACE_TYPE_SWIZZLE)
        return base;
    return 0xFF;
}

s32 cellRescGcmSurface2RescSrc(const void* gcmSurface, void* rescSrc)
{
    if (!gcmSurface || !rescSrc)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    const u32 sf = (u32)(uintptr_t)gcmSurface;
    const u32 dst = (u32)(uintptr_t)rescSrc;

    u32 mul_w = 1, mul_h = 1;
    if (s_config.size == 24 || s_config.size == 28) {
        const u32 aa = vm_read8(sf + 1);
        if (aa == 3) {
            mul_w = 2;
        } else if (aa >= 4 && aa <= 5) {
            mul_w = 2;
            mul_h = 2;
        }
    }

    vm_write32(dst + 0, resc_surface_texture_format(vm_read8(sf + 2), vm_read8(sf + 0)));
    vm_write32(dst + 4, vm_read32(sf + 0x18));
    vm_write16(dst + 8, (uint16_t)(mul_w * vm_read16(sf + 0x34)));
    vm_write16(dst + 10, (uint16_t)(mul_h * vm_read16(sf + 0x36)));
    vm_write32(dst + 12, vm_read32(sf + 8));
    return CELL_OK;
}

static int resc_dsts_index(u32 mode);

/* The size of one destination buffer of `mode` (firmware 02007D98): its height rounded up to the
 * record's heightAlign, times the record's pitch, in 32-bit arithmetic. The heights are those
 * of 02007CB8 (480, 576, 720, 1080). */
static u32 resc_dsts_buffer_size(u32 mode, const CellRescDsts* rec)
{
    u32 h;
    switch (mode) {
    case CELL_RESC_720x480:   h = 480;  break;
    case CELL_RESC_720x576:   h = 576;  break;
    case CELL_RESC_1280x720:  h = 720;  break;
    case CELL_RESC_1920x1080: h = 1080; break;
    default:                  h = 0;    break;
    }
    return ((rec->heightAlign + h - 1u) & (0u - rec->heightAlign)) * rec->pitch;
}

/* Size in bytes of the library's convert fragment-program microcode, per program: the size word
 * (`+0x18`) of each embedded Cg binary, which the library keeps at program object `+0xC`. */
static const u32 k_prog_ucode_size[RESC_PROG_COUNT] = {0x10, 0x60, 0x1E0, 0x240};

/* Firmware 02004B48. Three out-sizes, each optional:
 *   colour    resourcePolicy bit 0 (constant VRAM): the destination buffer size SetDisplayMode
 *             took from the mode's record, times the colour-buffer count of the current mode
 *             (PAL 576 with an interpolating PAL mode 6, with PAL_60_DROP 3, else 2);
 *             otherwise (020022A0): the largest, over the supported modes, of that mode's
 *             buffer size times cellRescGetNumColorBuffers(mode, palTemporalMode), from the
 *             records SetDsts has stored;
 *   vertex    384;
 *   fragment  constant VRAM: the current program's microcode size. Otherwise 768 when the
 *             title's SDK version is above 0x27FFFF, else by the init config's size:
 *             20 -> the current program's microcode size, 24 or 28 -> interlaced: 640 when the
 *             display mode is 576 with a non-zero PAL mode, else 512; progressive: 96 when 576
 *             is supported and the PAL mode is 2..4, else 16; any other size -> 0.
 * The SDK version is what lv2 syscall 25 reports (the title's PROC_PARAM). */
s32 cellRescGetBufferSize(u32* colorBufSize, u32* vertexBufSize, u32* fragmentBufSize)
{
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    const u32 pal = s_config.palTemporalMode;
    u32 color, fragment;
    if (s_config.resourcePolicy & 1u) {
        u32 n = 2;
        if (s_display_mode == CELL_RESC_720x576)
            n = (pal - 2u <= 2u) ? 6 : (pal == CELL_RESC_PAL_60_DROP ? 3 : 2);
        color = s_dst_buffer_size * n;
        fragment = k_prog_ucode_size[s_cur_prog];
    } else {
        int32_t max = 0;
        for (u32 mode = 1; mode <= 8; mode <<= 1) {
            if (!(s_config.supportModes & mode))
                continue;
            const int32_t size = (int32_t)(resc_dsts_buffer_size(mode, &s_dsts[resc_dsts_index(mode)]) *
                                           (u32)cellRescGetNumColorBuffers(mode, pal, 0));
            if (size >= max)
                max = size;
        }
        color = (u32)max;
        if (ps3_process_sdk_version() > 0x27FFFFu) {
            fragment = 768;
        } else if (s_config.size == 24 || s_config.size == 28) {
            if (s_config.interlaceMode == 1)
                fragment = (s_display_mode == CELL_RESC_720x576 && pal != 0) ? 640 : 512;
            else
                fragment = ((s_config.supportModes & CELL_RESC_720x576) && pal - 2u <= 2u) ? 96 : 16;
        } else if (s_config.size == 20) {
            fragment = k_prog_ucode_size[s_cur_prog];
        } else {
            fragment = 0;
        }
    }

    if (colorBufSize)
        vm_write32((uint32_t)(uintptr_t)colorBufSize, color);
    if (vertexBufSize)
        vm_write32((uint32_t)(uintptr_t)vertexBufSize, 384);
    if (fragmentBufSize)
        vm_write32((uint32_t)(uintptr_t)fragmentBufSize, fragment);
    return CELL_OK;
}

/* The colour buffers one display mode cycles through: 6 for 720x576 with a PAL interpolating
 * mode, 3 for PAL_60_DROP, else 2 (firmware 02006858..02006918; the same count as
 * cellRescGetNumColorBuffers). */
static u32 resc_color_buffer_count(void)
{
    if (s_display_mode == CELL_RESC_720x576) {
        const u32 pal = s_config.palTemporalMode;
        if (pal - 2u <= 2u)
            return 6;
        if (pal == CELL_RESC_PAL_60_DROP)
            return 3;
    }
    return 2;
}

/* Firmware 02001884: the quad's four records, written big-endian into the vertex buffer. */
static void resc_fill_vertex_buffer(void)
{
    u32 v[24];
    resc_quad_vertices(s_display_mode, s_config.ratioMode, s_config.size, s_aspect_h, s_aspect_v, v);
    for (u32 i = 0; i < 24; i++)
        vm_write32(s_buf_vertex_ea + i * 4u, v[i]);
    s_vertex_count = 4;
}

/* RSX offset of a guest address through cellGcmAddressToOffset. The firmware ignores that call's
 * result (the offset stays whatever the stack held); an address GCM cannot map is a stop here. */
static u32 resc_offset_of(u32 ea, const char* func, const char* what)
{
    u32 off = 0;
    if (cellGcm_address_to_offset(ea, &off) != CELL_OK)
        ps3_stop_gcm_unmodelled(func, what, "cellGcmAddressToOffset failed; the firmware would use an uninitialised stack word");
    return off;
}

/* The DC convert program's microcode: one TEX instruction (texture unit 0, texcoord 0, result to the
 * colour output, end) in the RSX's in-memory word order -- the plain `tex2D` of a full-screen blit,
 * the 0x10 bytes k_prog_ucode_size[DC] names (libresc embedded Cg binary: ucode at program object
 * +0x8, size +0xC). Written big-endian so guest memory holds the bytes the RSX reads. */
static const u32 k_dc_ucode[4] = { 0x9E011700u, 0xC8011C9Du, 0xC8000001u, 0xC8003FE1u };

/* Firmware 02006C70: the copy of the current convert program's microcode into the fragment buffer.
 * The E0/E4/E8 programs serve interlaced and PAL-interpolating configurations, and every one of
 * those stops by name in cellRescSetConvertAndFlip, the only consumer of the buffer's content; their
 * microcode is therefore not held and nothing can run on the uncopied buffer. */
static void resc_copy_convert_program(u32 fragment_ea)
{
    if (s_cur_prog != RESC_PROG_DC)
        return;
    if (!vm_range_ok(fragment_ea, sizeof k_dc_ucode))
        ps3_stop_guest_range("cellRescSetBufferAddress", "fragmentBuf", fragment_ea, sizeof k_dc_ucode,
                             vm_guest_limit());
    for (u32 i = 0; i < 4; i++)
        vm_write32(fragment_ea + i * 4u, k_dc_ucode[i]);
}

/* Firmware 02006778. In order: not initialised -> NOT_INITIALIZED; a NULL buffer -> BAD_ARGUMENT;
 * colour not 128-byte, vertex not 4-byte or fragment not 64-byte aligned -> BAD_ALIGNMENT. The
 * buffers are stored; the colour buffers' RSX offsets are `offset(colorBuf) + i * <size
 * SetDisplayMode derived>` (02006894), and each is registered with cellGcmSetDisplayBuffer as
 * buffer i (the first error is returned). Then the quad is written into the vertex buffer
 * (progressive configurations: the interlaced variant takes another path) and the current convert
 * fragment program's RSX offset is set.
 *
 * The library also copies the current program's microcode into the fragment buffer (02006C70,
 * resc_copy_convert_program). The stream refers to that buffer by offset only: without the copy the
 * RSX runs whatever the title left there (zeros), the draw samples nothing and writes black. */
s32 cellRescSetBufferAddress(void* colorBuf, void* vertexBuf, void* fragmentBuf)
{
    printf("[cellResc] SetBufferAddress()\n");

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    const u32 color = (u32)(uintptr_t)colorBuf, vertex = (u32)(uintptr_t)vertexBuf,
              fragment = (u32)(uintptr_t)fragmentBuf;
    if (!color || !vertex || !fragment)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;
    if ((color & 0x7Fu) || (vertex & 3u) || (fragment & 0x3Fu))
        return (s32)CELL_RESC_ERROR_BAD_ALIGNMENT;

    s_buf_color_ea = color;
    s_buf_vertex_ea = vertex;
    s_buf_fragment_ea = fragment;

    const u32 base = resc_offset_of(color, "cellRescSetBufferAddress", "colorBuf");
    const u32 n = resc_color_buffer_count();
    for (u32 i = 0; i < n; i++)
        s_color_offset[i] = i * s_dst_buffer_size + base;
    for (u32 i = 0; i < n; i++) {
        const s32 rc = cellGcmSetDisplayBuffer(i, s_color_offset[i], s_mode_pitch, s_mode_width,
                                               s_mode_height);
        if (rc != CELL_OK)
            return rc;
    }

    if (s_config.interlaceMode != 1)
        resc_fill_vertex_buffer();
    s_prog_ucode_offset[s_cur_prog] = resc_offset_of(fragment, "cellRescSetBufferAddress", "fragmentShader");
    resc_copy_convert_program(fragment);
    s_tex_unit = 0;   /* 020023B8: the program's sampler resolves to unit 0 */
    return CELL_OK;
}

s32 cellRescSetSrc(s32 index, const CellRescSrc* src)
{
    printf("[cellResc] SetSrc(index=%d)\n", index);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (!src || index < 0 || index >= 8)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* Decoded to host order: the guest struct is big-endian. */
    const u32 ea = (u32)(uintptr_t)src;
    s_src[index].format = vm_read32(ea + 0);
    s_src[index].pitch  = vm_read32(ea + 4);
    s_src[index].width  = vm_read16(ea + 8);
    s_src[index].height = vm_read16(ea + 10);
    s_src[index].offset = vm_read32(ea + 12);
    printf("[cellResc] SetSrc(%d): offset=0x%X pitch=%u %ux%u fmt=%u\n", index,
           s_src[index].offset, s_src[index].pitch, s_src[index].width,
           s_src[index].height, s_src[index].format);
    return CELL_OK;
}

/* Dsts record index of a destination mode (firmware 020006B4): 480 -> 0, 576 -> 1, 720 -> 2,
 * 1080 -> 3, anything else -1. */
static int resc_dsts_index(u32 mode)
{
    switch (mode) {
    case CELL_RESC_720x480:   return 0;
    case CELL_RESC_720x576:   return 1;
    case CELL_RESC_1280x720:  return 2;
    case CELL_RESC_1920x1080: return 3;
    default:                  return -1;
    }
}

/* Firmware 020006B4: not initialised -> NOT_INITIALIZED; NULL dsts -> BAD_ARGUMENT; a mode that
 * is not 1, 2, 4 or 8 -> BAD_ARGUMENT. The record ({format, pitch, heightAlign}) is stored as
 * given: nothing in it is validated here, and nothing is recomputed (the destination buffer
 * size is taken from the record by cellRescSetDisplayMode and cellRescGetBufferSize). */
s32 cellRescSetDsts(u32 displayMode, const CellRescDsts* dsts)
{
    printf("[cellResc] SetDsts(mode=0x%x)\n", displayMode);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (!dsts)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    const int idx = resc_dsts_index(displayMode);
    if (idx < 0)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* Decoded to host order: the guest struct is big-endian. */
    const u32 ea = (u32)(uintptr_t)dsts;
    s_dsts[idx].format      = vm_read32(ea + 0);
    s_dsts[idx].pitch       = vm_read32(ea + 4);
    s_dsts[idx].heightAlign = vm_read32(ea + 8);
    return CELL_OK;
}

/* Firmware 02004D4C (export 020004E0). In order: not initialised -> NOT_INITIALIZED; a source
 * index above 7 -> BAD_ARGUMENT. Then the convert stream is appended to the command context at
 * guest EA `context` (resc_convert.c, one group per firmware reservation), followed by
 * `_cellGcmSetFlipCommand` for display buffer `+0x110 & 0xFF` (020051CC, the flip result is not
 * checked), and the frame toggle flips. Always CELL_OK once past the checks.
 *
 * The interlaced configuration (`+0x14` == 1: a pre-pass at 02001AC0 and a second texture) and
 * 720x576 with PAL temporal mode 1..4 (an interpolating draw or another flip path, 020050F8..)
 * take branches that are not reproduced: a named stop, never a stream without them. */
s32 cellRescSetConvertAndFlip(void* context, s32 index)
{
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;
    if ((u32)index > 7u)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    if (s_config.interlaceMode == 1)
        ps3_stop_gcm_unmodelled("cellRescSetConvertAndFlip", "interlace-mode",
                                "interlaceMode 1 adds a pre-pass (02001AC0) and a second source texture");
    if (s_display_mode == CELL_RESC_720x576 && s_config.palTemporalMode - 1u <= 3u)
        ps3_stop_gcm_unmodelled("cellRescSetConvertAndFlip", "pal-temporal-mode",
                                "720x576 with PAL temporal mode 1..4 draws more than one pass or flips differently");

    const u32 ctx = (u32)(uintptr_t)context;
    RescConvertIn in;
    memset(&in, 0, sizeof in);
    in.width = s_mode_width;
    in.height = s_mode_height;
    in.pitch = s_mode_pitch;
    in.dst_format = s_dsts[resc_dsts_index(s_display_mode)].format;
    in.color_offset = s_color_offset[s_frame_toggle % RESC_MAX_COLOR_BUFFERS];
    in.vertex_offset = s_buf_vertex_ea ? resc_offset_of(s_buf_vertex_ea, "cellRescSetConvertAndFlip", "vertexArray") : 0;
    in.vertex_count = s_vertex_count;
    memcpy(in.attr, s_attr, sizeof in.attr);
    in.tex_unit = s_tex_unit;
    in.src = s_src[index];
    in.fp = k_prog_desc[s_cur_prog];
    in.fp.num_regs = s_prog_regcount[s_cur_prog];
    in.fp.ucode_offset = s_prog_ucode_offset[s_cur_prog];
    resc_emit_convert_stream(ctx, &in);

    cellGcm_emit_flip_command(ctx, s_frame_toggle & 0xFFu);
    s_frame_toggle ^= 1u;


    { static int n = 0;
      if (PS3_LOG_FIRST(n, 4))
          printf("[cellResc] SetConvertAndFlip(ctx=0x%08X, src=%d) -> convert stream + flip of buffer %u\n",
                 ctx, index, s_frame_toggle ^ 1u); }


    return CELL_OK;
}

/* cellRescSetWaitFlip(context) -- firmware 020045E8 (export 02000488, NID 0x0D3C22CE). SDK `void`.
 * Appends one 4-word group to the command context at guest EA `context`, which holds the RSX until
 * the previous flip has been executed:
 *
 *     NV406E SEMAPHORE_OFFSET (0x0064) = <label offset>,  NV406E SEMAPHORE_ACQUIRE (0x0068) = 0
 *
 * The label is 0x10, the flip-done label `_cellGcmSetFlipCommand` releases as 0xFFFFFFFF just before
 * its flip head and the driver sets back to 0 when it completes the flip (cellGcmTickVBlank). The
 * one other case is 720x576 (the stored display mode, +0xD0, is 2) with a non-zero PAL temporal mode
 * (+0x10) other than 5: the label is 18 << 4 (0x120), or 19 << 4 when +0x1DC == 1. The firmware only
 * ever clears +0x1DC (stores at 02003B30 and 02003BC0 write 0, nothing writes 1), so it is 18.
 *
 * The group is reserved like every group of the library (`current + 16 > end` -> callback(ctx, 4); a
 * callback that returns non-zero abandons the group, which cellGcm_ctx_reserve reports as the
 * gcm-unmodelled stop). It does not check that cellRescInit ran. The firmware's r3 on return is the
 * context when no callback ran and the callback's 0 otherwise; nothing reads it. */
s32 cellRescSetWaitFlip(void* context)
{
    const u32 ctx = (u32)(uintptr_t)context;
    u32 label = 0x10;
    if (s_display_mode == CELL_RESC_720x576 && s_config.palTemporalMode != 0 && s_config.palTemporalMode != 5)
        label = 18u << 4;
    const u32 w[4] = { 0x00040064u, label, 0x00040068u, 0u };
    const int callback = vm_read32(ctx + 8) + 16u > vm_read32(ctx + 4);
    cellGcm_ctx_emit(ctx, w, 4);
    return callback ? 0 : (s32)ctx;
}

/* cellRescSetFlipHandler / cellRescSetVBlankHandler: firmware 02003704 / 02003794 (exports 020002E8 /
 * 02000314). With the library initialised and a display mode set, any mode but 720x576 hands the
 * handler to libgcm (cellGcmSetFlipHandler 0200A56C, NID 0xA41EF7E8; cellGcmSetVBlankHandler 0200A5AC)
 * and keeps none itself, so the driver's flip and vblank deliver it. [INFERENCE: before a display mode
 * is set the firmware parks the handler in a library global for its own handler to call; the title
 * is not told apart here and gets the same delivery.] */
s32 cellRescSetFlipHandler(void (*handler)(u32))
{
    if (resc_has_own_flip_flag())
        ps3_stop_gcm_unmodelled("cellRescSetFlipHandler", "pal-flip-flag",
                                "720x576 with a PAL temporal mode routes the handler through the library's own flip handler");
    printf("[cellResc] SetFlipHandler(opd=0x%08X)\n", (u32)(uintptr_t)handler);
    cellGcmSetFlipHandler(handler);
    return CELL_OK;
}

s32 cellRescSetVBlankHandler(void (*handler)(u32))
{
    if (resc_has_own_flip_flag())
        ps3_stop_gcm_unmodelled("cellRescSetVBlankHandler", "pal-flip-flag",
                                "720x576 with a PAL temporal mode routes the handler through the library's own handler");
    printf("[cellResc] SetVBlankHandler(opd=0x%08X)\n", (u32)(uintptr_t)handler);
    cellGcmSetVBlankHandler(handler);
    return CELL_OK;
}

s32 cellRescGetDisplayMode(u32* displayMode)
{
    if (!displayMode)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    vm_write32((uint32_t)(uintptr_t)displayMode, s_display_mode);
    return CELL_OK;
}

/* cellRescGetLastFlipTime: without the library flag the firmware (02004590, export 0200045C) returns
 * cellGcmGetLastFlipTime (0200A52C, NID 0x63387071). */
s32 cellRescGetLastFlipTime(u64* time)
{
    if (!time)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;
    if (resc_has_own_flip_flag())
        ps3_stop_gcm_unmodelled("cellRescGetLastFlipTime", "pal-flip-flag",
                                "720x576 with a PAL temporal mode keeps the flip time in the library object (+0x1B0)");

    vm_write64((uint32_t)(uintptr_t)time, cellGcmGetLastFlipTime());
    return CELL_OK;
}

/* cellRescResetFlipStatus: firmware 02004534 (export 02000438, NID 0x129922A0). Without the library
 * flag it is cellGcmResetFlipStatus (0200455C -> the stub at 0200A5EC, NID 0xB2E761D4). */
void cellRescResetFlipStatus(void)
{
    if (resc_has_own_flip_flag())
        ps3_stop_gcm_unmodelled("cellRescResetFlipStatus", "pal-flip-flag",
                                "720x576 with a PAL temporal mode keeps the flip flag in the library object (+0x1D8)");
    cellGcmResetFlipStatus();
}

/* cellRescGetFlipStatus: firmware 020044D0 (export 0200040C, NID 0xC47C5C22). Without the library flag
 * it is cellGcmGetFlipStatus (020044F8 -> 0200A54C, NID 0x72A577CE): WAITING until the flip the RSX
 * executed has been completed by the driver. Nothing completes it here, and a poll never does. */
s32 cellRescGetFlipStatus(void)
{
    if (resc_has_own_flip_flag())
        ps3_stop_gcm_unmodelled("cellRescGetFlipStatus", "pal-flip-flag",
                                "720x576 with a PAL temporal mode keeps the flip flag in the library object (+0x1D8)");
    return (s32)cellGcmGetFlipStatus();
}

s32 cellRescSetPalInterpolateDropFlexRatio(float ratio)
{
    printf("[cellResc] SetPalInterpolateDropFlexRatio(%.2f)\n", ratio);
    s_pal_ratio = ratio;
    return CELL_OK;
}

s32 cellRescCreateInterlaceTable(void* buf, float ea, u32 tableLen, s32 depth)
{
    (void)ea;
    (void)depth;

    printf("[cellResc] CreateInterlaceTable(len=%u, depth=%d)\n", tableLen, depth);

    if (!buf)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* Fill with simple linear interpolation weights */
    float* table = (float*)buf;
    for (u32 i = 0; i < tableLen; i++)
        table[i] = (float)i / (float)(tableLen > 1 ? tableLen - 1 : 1);

    return CELL_OK;
}

/* Firmware 02004194. Not initialised -> NOT_INITIALIZED; either value outside [0.5, 2.0] ->
 * BAD_ARGUMENT, nothing stored. Otherwise stored (+0x148 horizontal, +0x14C vertical), and the
 * quad is rewritten when the vertex buffer is set and the configuration is progressive. */
s32 cellRescAdjustAspectRatio(float horizontal, float vertical)
{
    /* Titles call this every frame with unchanged values; logging each call buried every other
     * line of a session log. Log only a changed request (per thread: diagnostic state only). */
    { static _Thread_local float last_h = -1.0f, last_v = -1.0f;
      if (horizontal != last_h || vertical != last_v) {
          printf("[cellResc] AdjustAspectRatio(h=%.2f, v=%.2f)\n", horizontal, vertical);
          last_h = horizontal;
          last_v = vertical;
      } }

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (!(horizontal >= 0.5f) || horizontal > 2.0f || !(vertical >= 0.5f) || vertical > 2.0f)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    s_aspect_h = horizontal;
    s_aspect_v = vertical;
    if (s_buf_vertex_ea && s_config.interlaceMode != 1)
        resc_fill_vertex_buffer();
    return CELL_OK;
}

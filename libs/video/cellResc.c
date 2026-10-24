/*
 * ps3recomp - cellResc HLE implementation
 *
 * Resolution scaling stub. Since the recompiled game renders through
 * the host GPU, actual rescaling is handled by the host graphics API.
 * This module tracks state so game code that queries RESC works correctly.
 */

#include "cellResc.h"
#include "ps3emu/stop.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: translate + byte-swap */
#include "ps3emu/guest_call.h"   /* ps3_invoke_guest: handlers are guest OPDs */
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
/* Guest OPD addresses, not host function pointers: what a title passes is a
 * guest function descriptor, and calling it as a host pointer is a crash
 * waiting for the first title that sets one. Invoked through ps3_invoke_guest,
 * the same way cellGcmSetFlipHandler's is. */
static u32 s_flip_handler_opd = 0;
static u32 s_vblank_handler_opd = 0;
static u32 s_flip_target = 0;   /* display buffer the next convert-and-flip presents */
static s32 s_flip_status = 0;
static u64 s_last_flip_time = 0;
static float s_aspect_h = 1.0f;
static float s_aspect_v = 1.0f;
static float s_pal_ratio = 0.5f;

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
    memset(s_dsts, 0, sizeof(s_dsts));
    s_flip_handler_opd = 0;
    s_vblank_handler_opd = 0;
    s_flip_target = 0;
    s_flip_status = 0;
    s_last_flip_time = 0;
    s_aspect_h = 1.0f;
    s_aspect_v = 1.0f;
    s_cur_prog = RESC_PROG_DC;   /* 02003A2C: +0xFC = +0xDC */
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

s32 cellRescGetBufferSize(u32* colorBufSize, u32* vertexBufSize, u32* fragmentBufSize)
{
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    /* Provide reasonable buffer sizes for state tracking */
    if (colorBufSize)
        vm_write32((uint32_t)(uintptr_t)colorBufSize, 1920 * 1080 * 4); /* RGBA 1080p */
    if (vertexBufSize)
        vm_write32((uint32_t)(uintptr_t)vertexBufSize, 64 * 1024); /* 64KB vertex buffer */
    if (fragmentBufSize)
        vm_write32((uint32_t)(uintptr_t)fragmentBufSize, 256 * 1024); /* 256KB fragment shader */

    return CELL_OK;
}

s32 cellRescSetBufferAddress(void* colorBuf, void* vertexBuf, void* fragmentBuf)
{
    (void)colorBuf;
    (void)vertexBuf;
    (void)fragmentBuf;

    printf("[cellResc] SetBufferAddress()\n");

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    /* In host-GPU mode we don't use these buffers directly */
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

s32 cellRescSetDsts(u32 displayMode, const CellRescDsts* dsts)
{
    printf("[cellResc] SetDsts(mode=0x%x)\n", displayMode);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (!dsts)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* Map display mode to index */
    int idx = 0;
    if (displayMode & CELL_RESC_720x576) idx = 1;
    else if (displayMode & CELL_RESC_1280x720) idx = 2;
    else if (displayMode & CELL_RESC_1920x1080) idx = 3;

    s_dsts[idx] = *GUEST_PTR(dsts, const CellRescDsts*);
    return CELL_OK;
}

s32 cellRescSetConvertAndFlip(s32 index)
{
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    /* This is a real flip, not bookkeeping. A title that presents through RESC
     * -- Virtua Fighter 5 does -- renders into an off-screen surface and asks
     * RESC to scale it into a display buffer and show it. Tracking a counter
     * here and returning CELL_OK meant no flip ever reached cellGcm, so the
     * title's frame loop never closed: nothing retired, and it kept appending
     * to a command buffer it could never reclaim.
     *
     * ponytail: the convert half is deliberately not emitted -- the backend
     * already presents at the output resolution, so RESC's scale is a no-op for
     * us. What must happen is the flip. Rotate through the display buffers the
     * title registered, the way RESC's own double/triple buffering does. Emit a
     * real conversion draw if a title ever needs RESC's PAL/interlace modes
     * rather than a plain scale. */
    extern s32 cellGcmSetFlipCommand(u32 bufferId);
    extern u32 cellGcm_display_buffer_count(void);
    u32 nbuf = cellGcm_display_buffer_count();
    if (nbuf) {
        cellGcmSetFlipCommand(s_flip_target % nbuf);
        s_flip_target = (u32)((s_flip_target + 1) % nbuf);
    }

    s_flip_status = 0;      /* flip complete */
    s_last_flip_time++;

    { static int n = 0;
      if (PS3_LOG_FIRST(n, 4))
          printf("[cellResc] SetConvertAndFlip(src=%d) -> flip, %u display buffer(s)\n",
                 index, nbuf); }

    if (s_flip_handler_opd)
        ps3_invoke_guest(s_flip_handler_opd, 1, 0, 0, 0, 0, 0, 0, 0);

    return CELL_OK;
}

s32 cellRescSetFlipHandler(void (*handler)(u32))
{
    s_flip_handler_opd = (u32)(uintptr_t)handler;
    printf("[cellResc] SetFlipHandler(opd=0x%08X)\n", s_flip_handler_opd);
    return CELL_OK;
}

s32 cellRescSetVBlankHandler(void (*handler)(u32))
{
    s_vblank_handler_opd = (u32)(uintptr_t)handler;
    printf("[cellResc] SetVBlankHandler(opd=0x%08X)\n", s_vblank_handler_opd);
    return CELL_OK;
}

s32 cellRescGetDisplayMode(u32* displayMode)
{
    if (!displayMode)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    vm_write32((uint32_t)(uintptr_t)displayMode, s_display_mode);
    return CELL_OK;
}

s32 cellRescGetLastFlipTime(u64* time)
{
    if (!time)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    vm_write64((uint32_t)(uintptr_t)time, s_last_flip_time);
    return CELL_OK;
}

void cellRescResetFlipStatus(void)
{
    s_flip_status = 1; /* waiting for next flip */
}

s32 cellRescGetFlipStatus(void)
{
    /* Auto-complete pending flips when polled, so games don't stall
     * waiting for events in the null renderer. */
    if (s_flip_status == 1) {
        s_flip_status = 0;
        s_last_flip_time++;
        if (s_flip_handler_opd)
            ps3_invoke_guest(s_flip_handler_opd, 1, 0, 0, 0, 0, 0, 0, 0);
        if (s_vblank_handler_opd)
            ps3_invoke_guest(s_vblank_handler_opd, 1, 0, 0, 0, 0, 0, 0, 0);
    }
    return s_flip_status;
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

s32 cellRescAdjustAspectRatio(float horizontal, float vertical)
{
    printf("[cellResc] AdjustAspectRatio(h=%.2f, v=%.2f)\n", horizontal, vertical);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    s_aspect_h = horizontal;
    s_aspect_v = vertical;
    return CELL_OK;
}

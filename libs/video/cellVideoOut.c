/*
 * ps3recomp - cellVideoOut HLE implementation
 *
 * Video output configuration: resolution, display mode, device info.
 * Defaults to 1280x720 (720p).
 */

#include "cellVideoOut.h"
#include "ps3emu/endian.h"
#include "ps3emu/config.h"        /* video.resolutions */
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* The generic HLE adapter passes GUEST addresses for pointer args; translate to
 * a host pointer. (Values written through it stay big-endian via ps3_bswap*.) */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static u8  s_resolution_id  = CELL_VIDEO_OUT_RESOLUTION_720;
static u8  s_color_format   = CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8;
static u8  s_aspect         = CELL_VIDEO_OUT_ASPECT_16_9;
static u32 s_pitch          = 1280 * 4;

static void get_resolution_wh(u32 resId, u16* w, u16* h);

/* ---------------------------------------------------------------------------
 * The display's resolution set (video.resolutions, docs/CONFIG.md)
 *
 * Empty (the default) is "no restriction": every id is available and the device
 * lists 480, 576, 720, 1080. A configured list is the set the display reports;
 * availability, the device's mode list and the resolution GetState reports all
 * answer from it.
 * -----------------------------------------------------------------------*/
static const u8 k_known_resolutions[] = {
    CELL_VIDEO_OUT_RESOLUTION_480,  CELL_VIDEO_OUT_RESOLUTION_576,
    CELL_VIDEO_OUT_RESOLUTION_720,  CELL_VIDEO_OUT_RESOLUTION_1080,
    CELL_VIDEO_OUT_RESOLUTION_1600x1080, CELL_VIDEO_OUT_RESOLUTION_1440x1080,
    CELL_VIDEO_OUT_RESOLUTION_1280x1080, CELL_VIDEO_OUT_RESOLUTION_960x1080,
};

static int resolutions_restricted(void)
{
    return ps3_cfg_video_resolutions()[0] != '\0';
}

/* The list was validated and normalised at load: ids 1,2,4,5,10..13 joined by ','. */
static int resolution_listed(u32 id)
{
    const char* p = ps3_cfg_video_resolutions();
    while (*p) {
        u32 v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10u + (u32)(*p++ - '0');
        if (v == id)
            return 1;
        if (*p == ',')
            p++;
    }
    return 0;
}

static int resolution_available(u32 id)
{
    return resolutions_restricted() ? resolution_listed(id) : 1;
}

/* What GetState and GetConfiguration report until the title configures one:
 * 720, or the first listed id when 720 is not. */
static u8 initial_resolution(void)
{
    if (!resolutions_restricted() || resolution_listed(CELL_VIDEO_OUT_RESOLUTION_720))
        return CELL_VIDEO_OUT_RESOLUTION_720;
    for (unsigned i = 0; i < sizeof k_known_resolutions; i++)
        if (resolution_listed(k_known_resolutions[i]))
            return k_known_resolutions[i];
    return CELL_VIDEO_OUT_RESOLUTION_720;   /* unreachable: a non-empty list names a known id */
}

static int s_configured = 0;   /* Configure or cellVideoOut_set_resolution has set a resolution */
static u8 current_resolution(void)
{
    return s_configured ? s_resolution_id : initial_resolution();
}

static u32 current_pitch(void)
{
    if (s_configured)
        return s_pitch;
    u16 w, h;
    get_resolution_wh(initial_resolution(), &w, &h);
    return (u32)w * 4;
}

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

void cellVideoOut_set_resolution(u8 resolutionId)
{
    s_resolution_id = resolutionId;
    s_configured = 1;

    /* Update pitch based on resolution (host-endian helper; the public
     * cellVideoOutGetResolution writes guest big-endian now) */
    u16 w, h;
    get_resolution_wh(resolutionId, &w, &h);
    s_pitch = (u32)w * 4;
}

/* ---------------------------------------------------------------------------
 * Helpers
 * -----------------------------------------------------------------------*/

static void get_resolution_wh(u32 resId, u16* w, u16* h)
{
    switch (resId) {
    case CELL_VIDEO_OUT_RESOLUTION_1080:
        *w = 1920; *h = 1080; break;
    case CELL_VIDEO_OUT_RESOLUTION_720:
        *w = 1280; *h = 720;  break;
    case CELL_VIDEO_OUT_RESOLUTION_480:
        *w = 720;  *h = 480;  break;
    case CELL_VIDEO_OUT_RESOLUTION_576:
        *w = 720;  *h = 576;  break;
    case CELL_VIDEO_OUT_RESOLUTION_1600x1080:
        *w = 1600; *h = 1080; break;
    case CELL_VIDEO_OUT_RESOLUTION_1440x1080:
        *w = 1440; *h = 1080; break;
    case CELL_VIDEO_OUT_RESOLUTION_1280x1080:
        *w = 1280; *h = 1080; break;
    case CELL_VIDEO_OUT_RESOLUTION_960x1080:
        *w = 960;  *h = 1080; break;
    default:
        *w = 1280; *h = 720;  break;
    }
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellVideoOutGetState(u32 videoOut, u32 deviceIndex, CellVideoOutState* state)
{
    printf("[cellVideoOut] GetState(videoOut=%u, deviceIndex=%u)\n",
           videoOut, deviceIndex);

    if (!state)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    if (videoOut != CELL_VIDEO_OUT_PRIMARY && videoOut != CELL_VIDEO_OUT_SECONDARY)
        return CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT;

    state = GUEST_PTR(state, CellVideoOutState*);
    memset(state, 0, sizeof(CellVideoOutState));

    if (videoOut == CELL_VIDEO_OUT_PRIMARY) {
        state->state      = 0; /* CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED (was 2=PREPARING; PhyreEngine gates render-target config on ENABLED) */
        state->colorSpace = CELL_VIDEO_OUT_COLOR_SPACE_RGB;

        /* Fill the 8-byte displayMode struct byte-wise (endian-safe): the
         * guest reads resolutionId/scanMode/aspect as individual bytes. */
        (void)0;
        state->displayMode.resolutionId = current_resolution();
        state->displayMode.scanMode     = CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE;
        state->displayMode.conversion   = 0;
        state->displayMode.aspect       = CELL_VIDEO_OUT_ASPECT_16_9;
        state->displayMode.refreshRates = ps3_bswap16(0x0001); /* 59.94Hz flag */
    } else {
        state->state = 0; /* disabled */
    }

    return CELL_OK;
}

s32 cellVideoOutGetResolution(u32 resolutionId, CellVideoOutResolution* resolution)
{
    if (!resolution)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    /* The out-struct lives in guest memory: fields are big-endian.
     * (Host-endian writes here fed the game byte-swapped width/height —
     * 0xD002x0xE001 instead of 720x480 — which wrecked its display-buffer
     * setup.) */
    u16 w, h;
    get_resolution_wh(resolutionId, &w, &h);
    resolution = GUEST_PTR(resolution, CellVideoOutResolution*);
    resolution->width  = ps3_bswap16(w);
    resolution->height = ps3_bswap16(h);

    printf("[cellVideoOut] GetResolution(id=%u) -> %ux%u\n", resolutionId, w, h);

    return CELL_OK;
}

s32 cellVideoOutConfigure(u32 videoOut, CellVideoOutConfiguration* config,
                            void* option, u32 waitForEvent)
{
    printf("[cellVideoOut] Configure(videoOut=%u)\n", videoOut);

    if (!config)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    if (videoOut != CELL_VIDEO_OUT_PRIMARY)
        return CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT;

    config = GUEST_PTR(config, CellVideoOutConfiguration*);
    s_resolution_id = config->resolutionId;
    s_color_format  = config->format;
    s_aspect        = config->aspect;

    /* config is a guest BE struct: byte-swap multi-byte reads */
    if (ps3_bswap32(config->pitch) > 0) {
        s_pitch = ps3_bswap32(config->pitch);
    } else {
        u16 w, h;
        get_resolution_wh(s_resolution_id, &w, &h);
        s_pitch = (u32)w * 4;
    }

    s_configured = 1;
    printf("[cellVideoOut] Configure: resId=%u, format=%u, aspect=%u, pitch=%u\n",
           s_resolution_id, s_color_format, s_aspect, s_pitch);

    return CELL_OK;
}

s32 cellVideoOutGetConfiguration(u32 videoOut, CellVideoOutConfiguration* config,
                                   void* option)
{
    printf("[cellVideoOut] GetConfiguration(videoOut=%u)\n", videoOut);

    if (!config)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    if (videoOut != CELL_VIDEO_OUT_PRIMARY)
        return CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT;

    config = GUEST_PTR(config, CellVideoOutConfiguration*);
    memset(config, 0, sizeof(CellVideoOutConfiguration));
    config->resolutionId = current_resolution();
    config->format       = s_color_format;
    config->aspect       = s_aspect;
    config->pitch        = ps3_bswap32(current_pitch());  /* guest BE struct */

    return CELL_OK;
}

s32 cellVideoOutGetDeviceInfo(u32 videoOut, u32 deviceIndex,
                               CellVideoOutDeviceInfo* info)
{
    printf("[cellVideoOut] GetDeviceInfo(videoOut=%u, deviceIndex=%u)\n",
           videoOut, deviceIndex);

    if (!info)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    if (videoOut != CELL_VIDEO_OUT_PRIMARY)
        return CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT;

    info = GUEST_PTR(info, CellVideoOutDeviceInfo*);
    memset(info, 0, sizeof(CellVideoOutDeviceInfo));

    info->portType       = CELL_VIDEO_OUT_OUTPUT_HDMI;
    info->colorSpace     = CELL_VIDEO_OUT_COLOR_SPACE_RGB;
    info->latency        = 0;
    info->state          = 2; /* connected */
    info->rgbOutputRange = 1;

    /* Report supported modes: the four standard ones when video.resolutions is empty,
     * else exactly the listed ids. */
    int idx = 0;
    for (unsigned i = 0; i < sizeof k_known_resolutions; i++) {
        const u8 id = k_known_resolutions[i];
        if (resolutions_restricted() ? !resolution_listed(id) : i >= 4)
            continue;
        info->availableModes[idx].resolutionId = id;
        info->availableModes[idx].scanMode     = CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE;
        info->availableModes[idx].aspect       = CELL_VIDEO_OUT_ASPECT_16_9;
        idx++;
    }
    info->availableModeCount = (u8)idx;

    return CELL_OK;
}

s32 cellVideoOutGetNumberOfDevice(u32 videoOut)
{
    printf("[cellVideoOut] GetNumberOfDevice(videoOut=%u)\n", videoOut);

    if (videoOut == CELL_VIDEO_OUT_PRIMARY)
        return 1;

    return 0;
}

/* Is (resolutionId, aspect) available on this output? The primary output answers from
 * video.resolutions: every id when it is empty (a 0 here makes a game skip its
 * display-buffer/tile setup, leaving a null render object it later dereferences), else
 * 1 for a listed id and 0 for any other. The aspect is not part of the answer. */
s32 cellVideoOutGetResolutionAvailability(u32 videoOut, u32 resolutionId,
                                          u32 aspect, u32 option)
{
    (void)aspect; (void)option;
    const s32 r = (videoOut == CELL_VIDEO_OUT_PRIMARY) ? resolution_available(resolutionId) : 0;
    printf("[cellVideoOut] GetResolutionAvailability(out=%u, res=%u) -> %d\n",
           videoOut, resolutionId, (int)r);
    return r;
}

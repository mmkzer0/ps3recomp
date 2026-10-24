/*
 * ps3recomp - cellResc HLE
 *
 * Resolution scaling/conversion module. Handles scaling of render targets
 * to display resolution (e.g., rendering at 720p, output at 1080p).
 */

#ifndef PS3RECOMP_CELL_RESC_H
#define PS3RECOMP_CELL_RESC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
/* Numbering from the firmware library's return sites (FW 4.93 libresc):
 *   0x301  library not initialised (02000848, 02004064, 02004BB0, ...)
 *   0x302  init flag already set (cellRescInit, 0200391C)
 *   0x303  buffer address not 128-byte aligned (0200680C)
 *   0x304  NULL pointer / unknown id / bad value (0200089C, 02003970, ...)
 *   0x305  an internal allocation failed (0x2007B58 returned 0; 02003A68..02003B08)
 *   0x306  flip queue still full after the retry loop (02005168, 02005620)
 *   0x307  no usable combination of modes (020040D0, 02004A3C, 02004B3C)
 * 0x308 (02004C18) is the result of lv2 syscall 25 (sys_process_get_sdk_version)
 * failing; it has no SDK name. */
#define CELL_RESC_ERROR_NOT_INITIALIZED    0x80210301
#define CELL_RESC_ERROR_REINITIALIZED      0x80210302
#define CELL_RESC_ERROR_BAD_ALIGNMENT      0x80210303
#define CELL_RESC_ERROR_BAD_ARGUMENT       0x80210304
#define CELL_RESC_ERROR_LESS_MEMORY        0x80210305
#define CELL_RESC_ERROR_GCM_FLIP_QUE_FULL  0x80210306
#define CELL_RESC_ERROR_BAD_COMBINATION    0x80210307

/* ---------------------------------------------------------------------------
 * Display buffer modes
 * -----------------------------------------------------------------------*/
#define CELL_RESC_720x480                  0x01
#define CELL_RESC_720x576                  0x02
#define CELL_RESC_1280x720                 0x04
#define CELL_RESC_1920x1080                0x08

/* ---------------------------------------------------------------------------
 * Conversion modes
 * -----------------------------------------------------------------------*/
#define CELL_RESC_FULLSCREEN               0
#define CELL_RESC_LETTERBOX                1
#define CELL_RESC_PANSCAN                  2

/* ---------------------------------------------------------------------------
 * Pal temporal mode
 * -----------------------------------------------------------------------*/
#define CELL_RESC_PAL_50                   0
#define CELL_RESC_PAL_60_DROP              1
#define CELL_RESC_PAL_60_INTERPOLATE       2
#define CELL_RESC_PAL_60_INTERPOLATE_30_DROP   3
#define CELL_RESC_PAL_60_INTERPOLATE_DROP_FLEXIBLE 4

/* ---------------------------------------------------------------------------
 * Ratio conversion mode
 * -----------------------------------------------------------------------*/
#define CELL_RESC_INTERLACE_FILTER         0
#define CELL_RESC_NORMAL_BILINEAR          1
#define CELL_RESC_ELEMENT_BILINEAR         2

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

typedef struct CellRescInitConfig {
    u32 size;
    u32 resourcePolicy;
    u32 supportModes;
    u32 ratioMode;
    u32 palTemporalMode;
    u32 interlaceMode;
    u32 flipMode;
} CellRescInitConfig;

typedef struct CellRescSrc {   /* SDK resc.h: 16 bytes, width/height are u16 */
    u32 format;
    u32 pitch;
    u16 width;
    u16 height;
    u32 offset;
} CellRescSrc;

typedef struct CellRescDsts {
    u32 format;
    u32 pitch;
    u32 heightAlign;
} CellRescDsts;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellRescInit(const CellRescInitConfig* initConfig);
void cellRescExit(void);

s32 cellRescSetDisplayMode(u32 displayMode);
s32 cellRescGetNumColorBuffers(u32 dstMode, u32 palTemporalMode, u32 reserved);

s32 cellRescGetBufferSize(u32* colorBufSize, u32* vertexBufSize, u32* fragmentBufSize);
s32 cellRescSetBufferAddress(void* colorBuf, void* vertexBuf, void* fragmentBuf);
void cellRescSetRegisterCount(u8 regCount);
s32 cellRescGetRegisterCount(void);
u32 cellResc_convert_register_count(void);

s32 cellRescSetSrc(s32 index, const CellRescSrc* src);
s32 cellRescSetDsts(u32 displayMode, const CellRescDsts* dsts);
s32 cellRescGcmSurface2RescSrc(const void* gcmSurface, void* rescSrc);
s32 cellRescSetConvertAndFlip(void* context, s32 index);

s32 cellRescSetFlipHandler(void (*handler)(u32));
s32 cellRescSetVBlankHandler(void (*handler)(u32));

s32 cellRescGetDisplayMode(u32* displayMode);
s32 cellRescGetLastFlipTime(u64* time);
void cellRescResetFlipStatus(void);
s32 cellRescGetFlipStatus(void);

s32 cellRescVideoOutResolutionId2RescBufferMode(u32 resolutionId, u32* bufferMode);

s32 cellRescSetPalInterpolateDropFlexRatio(float ratio);
s32 cellRescCreateInterlaceTable(void* buf, float ea, u32 tableLen, s32 depth);
s32 cellRescAdjustAspectRatio(float horizontal, float vertical);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_RESC_H */

/*
 * ps3recomp - cellGcmSys HLE module
 *
 * RSX (Reality Synthesizer) graphics system interface.
 * Manages initialization, display buffers, flip control, command buffer
 * control, tile/zcull configuration, IO memory mapping, report/label
 * areas, and address-to-offset translation for the GPU.
 *
 * Note: Full RSX command buffer processing (the NV47xx methods, fragment/
 * vertex programs, texture setup, etc.) is a separate massive undertaking
 * handled by the RSX command processor module, not this file.
 */

#ifndef PS3RECOMP_CELL_GCM_SYS_H
#define PS3RECOMP_CELL_GCM_SYS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

/* Flip modes */
#define CELL_GCM_DISPLAY_HSYNC          1
#define CELL_GCM_DISPLAY_VSYNC          2

/* Flip status */
#define CELL_GCM_FLIP_STATUS_DONE       0
#define CELL_GCM_FLIP_STATUS_WAITING    1

/* Surface color formats */
#define CELL_GCM_SURFACE_X1R5G5B5_Z1R5G5B5   1
#define CELL_GCM_SURFACE_X1R5G5B5_O1R5G5B5   2
#define CELL_GCM_SURFACE_R5G6B5               3
#define CELL_GCM_SURFACE_X8R8G8B8_Z8R8G8B8   4
#define CELL_GCM_SURFACE_X8R8G8B8             5
#define CELL_GCM_SURFACE_A8R8G8B8             8
#define CELL_GCM_SURFACE_B8                   9
#define CELL_GCM_SURFACE_G8B8                 10
#define CELL_GCM_SURFACE_F_W16Z16Y16X16       11
#define CELL_GCM_SURFACE_F_W32Z32Y32X32       12
#define CELL_GCM_SURFACE_F_X32                13
#define CELL_GCM_SURFACE_X8B8G8R8_Z8B8G8R8   14
#define CELL_GCM_SURFACE_X8B8G8R8_O8B8G8R8   15
#define CELL_GCM_SURFACE_A8B8G8R8             16

/* Depth buffer formats */
#define CELL_GCM_SURFACE_Z16                  1
#define CELL_GCM_SURFACE_Z24S8                2

/* Max display buffers */
#define CELL_GCM_MAX_DISPLAY_BUFFER_NUM 8

/* Tile / Zcull limits */
#define CELL_GCM_MAX_TILE_COUNT         15
#define CELL_GCM_MAX_ZCULL_COUNT        8

/* Report / label limits */
#define CELL_GCM_MAX_REPORT_COUNT       65536   /* the 1 MB report area, 16 bytes each */
#define CELL_GCM_MAX_LABEL_COUNT        256

/* Report data size (timestamp u64 + value u32 + pad u32 = 16 bytes) */
#define CELL_GCM_REPORT_DATA_SIZE       16

/* Location */
#define CELL_GCM_LOCATION_LOCAL         0  /* video memory */
#define CELL_GCM_LOCATION_MAIN          1  /* main memory */

/* IO mapping limits. libgcm has no small fixed slot count:
 * cellGcmMapMainMemory fails when the IO address space is full. Killzone 3
 * maps up to 38 separate 1 MB chunks at level load and keeps vertex buffers in
 * them; with 16 slots the later calls failed and those draws read unmapped
 * offsets. 512 slots (16 bytes each) is an assumed bound well above that;
 * build with -DCELL_GCM_MAX_IO_MAPPINGS=N to change it. */
#ifndef CELL_GCM_MAX_IO_MAPPINGS
#define CELL_GCM_MAX_IO_MAPPINGS        512
#endif
#define CELL_GCM_IOMAP_FLAG_STRICT_ORDERING 2u

/* Debug output levels */
#define CELL_GCM_DEBUG_LEVEL0           0
#define CELL_GCM_DEBUG_LEVEL1           1
#define CELL_GCM_DEBUG_LEVEL2           2

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/* Command buffer control registers (mapped to RSX FIFO control area) */
typedef struct CellGcmControl {
    volatile u32 put;       /* write pointer (offset into cmd buffer) */
    volatile u32 get;       /* read pointer (offset into cmd buffer) */
    volatile u32 ref;       /* reference value (written by RSX on completion) */
} CellGcmControl;

/* GCM context data — the game reads/writes this to submit RSX commands.
 * On PS3, gCellGcmCurrentContext points to one of these.
 * All pointer fields are guest addresses (u32) in the recomp.
 * Layout verified against compiled SDK inline code (Yakuza: Dead Souls
 * EBOOT, command-write helper at 0xEBC0C8) and RPCS3's CellGcmContextData:
 * begin/end/current/callback, with callback LAST (offset 0xC). The fields
 * are big-endian in guest memory — byte-swap on host access. */
typedef struct CellGcmContextData {
    u32 begin;      /* start of command buffer (guest addr) */
    u32 end;        /* end of command buffer (guest addr) */
    u32 current;    /* current write position (guest addr) */
    u32 callback;   /* overflow callback function OPD (guest addr) */
} CellGcmContextData;

/* Display buffer configuration */
typedef struct CellGcmDisplayInfo {
    u32 offset;         /* offset in local memory */
    u32 pitch;          /* pitch in bytes */
    u32 width;
    u32 height;
} CellGcmDisplayInfo;

/* Offset table for address translation */
typedef struct CellGcmOffsetTable {
    u16* ioAddress;     /* main memory -> IO offset mapping */
    u16* eaAddress;     /* IO offset -> main memory mapping */
} CellGcmOffsetTable;

/* GCM configuration returned by cellGcmInit */
typedef struct CellGcmConfig {
    u32 localAddress;       /* start of local (video) memory in guest space */
    u32 ioAddress;          /* start of IO-mapped main memory */
    u32 localSize;          /* size of local memory */
    u32 ioSize;             /* size of IO-mapped region */
    u32 memoryFrequency;    /* RSX memory clock */
    u32 coreFrequency;      /* RSX core clock */
} CellGcmConfig;

/* Tile configuration */
typedef struct CellGcmTileInfo {
    u32 tile;           /* tile register value */
    u32 limit;          /* tile limit address */
    u32 pitch;          /* pitch in bytes */
    u32 format;         /* compression format */
    u32 offset;         /* base offset in local memory */
    u32 size;           /* tile region size in bytes */
    u32 base;           /* base address for compression */
    u32 bank;           /* memory bank */
    u8  bound;          /* 1 if bound to RSX, 0 if unbound */
} CellGcmTileInfo;

/* Zcull configuration */
typedef struct CellGcmZcullInfo {
    u32 offset;         /* offset in local memory */
    u32 width;          /* width in pixels */
    u32 height;         /* height in pixels */
    u32 cullStart;      /* zcull start address */
    u32 zFormat;        /* depth format */
    u32 aaFormat;       /* anti-alias format */
    u32 zcullDir;       /* direction */
    u32 zcullFormat;    /* zcull format register */
    u32 sFunc;          /* stencil function */
    u32 sRef;           /* stencil reference */
    u32 sMask;          /* stencil mask */
    u8  bound;          /* 1 if bound to RSX, 0 if unbound */
} CellGcmZcullInfo;

/* Report data (16 bytes, matching RSX report format) */
typedef struct CellGcmReportData {
    u64 timestamp;      /* RSX timestamp */
    u32 value;          /* report value */
    u32 pad;            /* padding to 16 bytes */
} CellGcmReportData;

/* Notify data (same layout as report) */
typedef struct CellGcmNotifyData {
    u64 timestamp;
    u64 zero;           /* always 0 on notification */
} CellGcmNotifyData;

/* Callback types */
typedef void (*CellGcmFlipHandler)(u32 head);
typedef void (*CellGcmVBlankHandler)(u32 head);
typedef void (*CellGcmUserHandler)(u32 cause);
typedef void (*CellGcmSecondVHandler)(u32 head);
typedef s32  (*CellGcmContextCallback)(void* context, u32 count);
typedef void (*CellGcmGraphicsHandler)(u32 val);
typedef void (*CellGcmQueueHandler)(u32 head);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* Initialization */

/* NID: 0xB477F06A */
s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress);

/* Reusable core of _cellGcmInitBody (NID 0x15BAE46B) — the function the SDK's
 * cellGcmInit() macro actually calls, imported by virtually every PS3 game.
 *
 * It can't be a self-contained HLE function in this library because it must
 * (a) allocate a guest CellGcmContextData and (b) write the game's
 * context-out pointer in guest memory — both of which require the owning game
 * project's vm. So the project's bridge supplies those as callbacks and this
 * helper performs the proven, layout-correct setup:
 *   - calls cellGcmInit(cmdSize, ioSize, ioAddress)
 *   - allocates a 16-byte CellGcmContextData via `galloc`
 *   - fills begin@+0, end@+4, current@+8, callback@+0xC (the layout verified in
 *     the shipping Simpsons port; a wrong layout makes the game read `current`
 *     as the callback OPD and stall) via `gwrite32` (which must byte-swap to BE)
 *   - writes the context's guest address to *ctx_out_addr
 * Returns the guest address of the new CellGcmContextData.
 *
 * Game bridge usage (gpr3=ctx_out, gpr4=cmdSize, gpr5=ioSize, gpr6=ioAddress):
 *   ctx->gpr[3] = cellGcmSetupContext(gpr3, gpr4, gpr5, gpr6,
 *                                     my_guest_alloc, my_vm_write32) ? 0 : err;
 */
typedef u32  (*CellGcmGuestAlloc)(u32 size, u32 align);
typedef void (*CellGcmGuestWrite32)(u32 guest_addr, u32 value);
u32 cellGcmSetupContext(u32 ctx_out_addr, u32 cmdSize, u32 ioSize, u32 ioAddress,
                        CellGcmGuestAlloc galloc, CellGcmGuestWrite32 gwrite32);

s32 cellGcmGetConfiguration(CellGcmConfig* config);

/* Command buffer control */

/* NID: 0xA547ADDE */
CellGcmControl* cellGcmGetControlRegister(void);

/* Display / flip */

/* NID: 0xC8F3BD09 */
u32 cellGcmGetCurrentField(void);

/* NID: 0x4AE8D215 */
void cellGcmSetFlipMode(u32 mode);

/* NID: 0x983FB9AA */
void cellGcmSetWaitFlip(void);
void cellGcmSetWaitFlipUnsafe(void);
u32 cellGcmGetReport(u32 type, u32 index);

/* NID: 0xB2E761D4 */
void cellGcmResetFlipStatus(void);

/* NID: 0x72A577CE */
u32 cellGcmGetFlipStatus(void);

/* --- sys_rsx_* bridge (libs/video/sys_rsx.c) -----------------------------
 * A firmware module links libgcm statically and reaches RSX through the lv2
 * syscalls, so no cellGcm* import ever runs. These let the syscall layer drive
 * the same state the HLE path sets up. */
u32  cellGcm_syscall_bringup(u32 local_size);   /* -> RSX local memory base */
u32  cellGcm_control_guest_addr(void);          /* put/get/ref triple EA */
void cellGcm_syscall_iomap(u32 ea, u32 io, u32 size);
void cellGcm_syscall_iounmap(u32 io, u32 size);
void cellGcm_syscall_set_fifo(u32 put, u32 get);
/* Hand the display buffers the title has registered so far to the draw
 * engine, for an engine that comes up after cellGcmSetDisplayBuffer ran. */
void cellGcm_replay_display_buffers_to_engine(void);

/* NID: 0xA53D12AE */
s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height);

/* NID: 0x5C770579 */
s32 cellGcmSetFlipCommand(u32 bufferId);

/* NID: 0x2B827690 */
s32 cellGcmSetFlipCommandWithWaitLabel(u32 bufferId, u32 labelIndex, u32 labelValue);

/* Host-side command-context primitives for library emitters (cellResc): not firmware exports.
 *   cellGcm_ctx_reserve: libgcm's `current + 4*words > end -> callback(ctx, words)`; returns 1
 *     with room, else stops (gcm-unmodelled) when the callback left none.
 *   cellGcm_ctx_emit: reserve `n` words, write them at `current`, advance `current`.
 *   cellGcm_emit_flip_command: `_cellGcmSetFlipCommand` as libgcm_sys writes it (19 words: driver
 *     queue 0xE944, NV406E label handshake, CALL 0, driver flip head 0xE924); returns
 *     CELL_GCM_ERROR_FAILURE for a buffer id above 7. `ctx` is the guest context EA. */
int  cellGcm_ctx_reserve(u32 ctx, u32 words);
void cellGcm_ctx_emit(u32 ctx, const u32* words, u32 n);
s32  cellGcm_emit_flip_command(u32 ctx, u32 bufferId);
s32  cellGcm_address_to_offset(u32 address, u32* offset_out);

/* NID: 0x0B4B62D5 */
s32 cellGcmSetPrepareFlip(void* ctx, u32 bufferId);

/* NID: 0x371674CF */
CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index);

/* NID: 0x93806525 */
u32 cellGcmGetCurrentDisplayBufferId(void);

/* NID: 0xA41EF7E8 */
void cellGcmSetFlipHandler(CellGcmFlipHandler handler);

/* NID: 0xA91B0402 */
void cellGcmSetVBlankHandler(CellGcmVBlankHandler handler);

/* Host-side ticks. Call from the game's host driver to fire any
 * registered guest VBlank/Flip handlers via the g_ps3_guest_caller
 * mechanism (see ps3emu/guest_call.h). Many games drive their
 * title-screen state machine from the VBlank handler. */
void cellGcmTickVBlank(void);
void cellGcmTickFlip(void);

/* NID: 0xDC494430 */
void cellGcmSetSecondVHandler(CellGcmSecondVHandler handler);

/* NID: 0x06EDEA9E */
void cellGcmSetUserHandler(CellGcmUserHandler handler);

/* NID: 0x63387071 */
u64 cellGcmGetLastFlipTime(void);

/* Address translation / IO mapping */

/* NID: 0x2922AED0 */
s32 cellGcmGetOffsetTable(CellGcmOffsetTable* table);

/* NID: 0x21AC3697 */
s32 cellGcmAddressToOffset(u32 address, u32* offset);

/* NID: 0xA114EC67 */
s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset);

/* NID: 0x63441CB4 */
s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size);

/* NID: 0x626E8518 */
s32 cellGcmMapEaIoAddressWithFlags(u32 ea, u32 io, u32 size, u32 flags);

/* NID: 0xEFD00F54 (disambiguation by context) */
s32 cellGcmUnmapEaIoAddress(u32 ea);

/* NID: 0xDB23E867 */
s32 cellGcmUnmapIoAddress(u32 io);

/* NID: 0x2A6FBA9C */
s32 cellGcmIoOffsetToAddress(u32 ioOffset, u32* ea);

/* Label / report / timestamp */

/* NID: 0xF80196C1 */
u32* cellGcmGetLabelAddress(u8 index);

/* NID: 0x9A0159AF */
CellGcmReportData* cellGcmGetReportDataAddress(u32 index);

/* NID: 0x5A41C10F */
u64 cellGcmGetTimeStamp(u32 index);

/* Tile / Zcull */

/* NID: 0xD0B1D189 */
s32 cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size,
                   u32 pitch, u8 comp, u16 base, u8 bank);

/* NID: 0xD34A420D */
s32 cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height,
                    u32 cullStart, u32 zFormat, u32 aaFormat,
                    u32 zcullDir, u32 zcullFormat,
                    u32 sFunc, u32 sRef, u32 sMask);

/* NID: 0x4524CCCD */
s32 cellGcmBindTile(u8 index);

/* NID: 0x9DC04436 */
s32 cellGcmBindZcull(u8 index);

/* NID: 0xD9B7653E */
s32 cellGcmUnbindTile(u8 index);

/* NID: 0xA75640E8 */
s32 cellGcmUnbindZcull(u8 index);

/* Misc */

/* NID: 0x055BD74D */
u32 cellGcmGetTiledPitchSize(u32 size);

/* NID: 0x51C9D62B */
void cellGcmSetDebugOutputLevel(u32 level);

/* --- Additional functions needed by Tokyo Jungle --- */

/* Tile info (alternative to cellGcmSetTile) */
s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size,
                       u32 pitch, u8 comp, u16 base, u8 bank);

/* Notify data area (similar to report data) */
void* cellGcmGetNotifyDataAddress(u32 index);

/* Timestamp location query */
/* location is CELL_GCM_LOCATION_LOCAL/_MAIN, as in the Report variants. */
u64 cellGcmGetTimeStampLocation(u32 index, u32 location);

/* Default FIFO size configuration */
s32 cellGcmSetDefaultFifoSize(u32 size);

/* Internal flip commands (called by game code directly) */
s32 _cellGcmSetFlipCommand(void* ctx, u32 bufferId);
s32 _cellGcmSetFlipCommandWithWaitLabel(void* ctx, u32 bufferId, u32 labelIndex, u32 labelValue);

/* --- Additional functions (RPCS3 parity) --- */

/* FIFO command buffer callback */
s32 cellGcmCallback(void* context, u32 count);

/* Map RSX local memory */
s32 cellGcmMapLocalMemory(u32* address, u32* size);

/* Shutdown RSX */
void cellGcmTerminate(void);

/* IO map size queries */
u32 cellGcmGetMaxIoMapSize(void);
s32 cellGcmReserveIoMapSize(u32 size);
s32 cellGcmUnreserveIoMapSize(u32 size);

/* VBlank counter */
u32 cellGcmGetVBlankCount(void);

/* Tile / Zcull info getters */
CellGcmTileInfo* cellGcmGetTileInfo(u8 index);
CellGcmZcullInfo* cellGcmGetZcullInfo(u8 index);

/* Display info by index */
CellGcmDisplayInfo* cellGcmGetDisplayInfo(u32 index);

/* Default FIFO mode / command buffer */
void cellGcmInitDefaultFifoMode(s32 mode);
void cellGcmSetDefaultCommandBuffer(void);

/* Debug dump (no-op) */
void cellGcmDumpGraphicsError(void);

/* Default word sizes */
u32 cellGcmGetDefaultCommandWordSize(void);
u32 cellGcmGetDefaultSegmentWordSize(void);

/* Immediate flip, and the context/buffer form a title imports directly. */
s32 cellGcmSetFlip(void* context, u32 bufferId);
s32 cellGcmSetFlipImmediate(u32 bufferId);

/* Flip status setter */
void cellGcmSetFlipStatus(u32 status);

/* Graphics / queue handler setters */
void cellGcmSetGraphicsHandler(CellGcmGraphicsHandler handler);
void cellGcmSetQueueHandler(CellGcmQueueHandler handler);

/* Frequency / VBlank configuration */
void cellGcmSetSecondVFrequency(u32 freq);
void cellGcmSetVBlankFrequency(u32 freq);

/* User command */
void cellGcmSetUserCommand(u32 cmd);
/* Host FIFO consumer: queue the retired command, in retirement order and never coalesced, for
 * ppu_gcm_pump to deliver (AM-27). gcm_user_ring_drops counts commands lost to a full ring. */
void cellGcmQueueUserCommand(u32 cmd);
u32  gcm_user_ring_drops(void);

/* Driver events the FIFO walker retires, for C3's event comparison. `seq` counts from 1 in
 * retirement order. The observer runs on the walker thread, inside the drain; it must not block. */
typedef enum GcmDriverEventKind {
    GCM_DRV_EVENT_QUEUE_FLIP   = 0,   /* GCM_DRIVER_QUEUE 0xE944; arg = display buffer id */
    GCM_DRV_EVENT_FLIP_HEAD    = 1,   /* GCM_FLIP_HEAD 0xE924 with bit 31: the present; arg = buffer id */
    GCM_DRV_EVENT_USER_COMMAND = 2,   /* GCM_SET_USER_COMMAND 0xEB00; arg = the command */
    GCM_DRV_EVENT_KINDS        = 3
} GcmDriverEventKind;
typedef struct GcmDriverEvent { u32 kind; u32 arg; unsigned long long seq; } GcmDriverEvent;
typedef void (*GcmDriverObserver)(const GcmDriverEvent* ev);
void gcm_set_driver_observer(GcmDriverObserver fn);
unsigned long long gcm_driver_event_count(u32 kind);

/* What the FIFO walker did with every method it consumed (rsx_method_class.h): each ends as handled,
 * inert (allow-listed for the value it pins), missing (a state this renderer ignores, with an S2 row in
 * docs/RSX_GRAPHICS.md), or unknown (no row at all: the live walker stops with rsx-unknown-method and a
 * capture frame is invalid). `unbound` counts methods dropped because the slot had no object bound,
 * which is what the console does. The counters are cumulative until gcm_method_stats_reset(); they may
 * be read from any thread. */
typedef struct GcmMethodStats {
    unsigned long long handled, inert, missing, unknown, unbound;
    unsigned long long m2mf_copies;   /* NV0039 BUFFER_NOTIFY copies executed */
} GcmMethodStats;
typedef struct GcmUnknownMethod {
    u32 cls;                          /* rsx_class_id: name it with gcm_method_class_name() */
    u32 method;
    unsigned long long count;
    u32 subch, data;                  /* the first occurrence's subchannel and data word */
} GcmUnknownMethod;
void gcm_method_stats(GcmMethodStats* out);
/* Fills up to `max` entries, ordered by class then method; returns the number of distinct pairs. */
u32  gcm_unknown_methods(GcmUnknownMethod* out, u32 max);
/* The same for the pairs classified missing: the known gaps, per (class, method). */
u32  gcm_missing_methods(GcmUnknownMethod* out, u32 max);
void gcm_method_stats_reset(void);
const char* gcm_method_class_name(u32 cls);

/* One recorded FIFO step, for rsx_replay (docs/RSX_GRAPHICS.md, "Replay"). `put` is what the live pass saw;
 * `end` is where the step leaves the walker (a branch's target, the next word otherwise). A GET_REPORT in the
 * step takes the next of `report_ts` (the capture's timers: the live ones came from the host clock). */
typedef struct GcmReplayStep {
    u32 get, call_ret;
    u32 put, end;
    const u64* report_ts;
    u32 n_report_ts;
} GcmReplayStep;
enum {
    GCM_REPLAY_NOT_AT_STEP = 1,        /* got = {get, call_ret} the walker stood at */
    GCM_REPLAY_LANDED_ELSEWHERE = 2,   /* got = {get, call_ret} it left the step at */
    GCM_REPLAY_REPORT_COUNT = 3        /* got[0] = reports the walker wrote */
};
int cellGcm_rsx_replay_step(const GcmReplayStep* s, u32 got[2]);

/* Invalidate tile */
s32 cellGcmSetInvalidateTile(u8 index);

/* EA IO address remap (stub) */
void cellGcmSortRemapEaIoAddress(void);

/* Report data with location */
CellGcmReportData* cellGcmGetReportDataAddressLocation(u32 index, u32 location);
u32 cellGcmGetReportDataLocation(u32 index, u32 location);


#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GCM_SYS_H */

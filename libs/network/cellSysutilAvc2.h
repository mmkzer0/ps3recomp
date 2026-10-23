/*
 * ps3recomp - cellSysutilAvc2 HLE (in-game voice chat)
 *
 * Declares the entry points cellSysutilAvc2.c exports. Voice chat is accepted
 * and silent: load/unload/join/leave succeed and report through the title's
 * Avc2 callback (libs/system/cellSysutil.c).
 */

#ifndef PS3RECOMP_CELL_SYSUTIL_AVC2_H
#define PS3RECOMP_CELL_SYSUTIL_AVC2_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

s32 cellSysutilAvc2InitParam(u16 version, u32 option);
s32 cellSysutilAvc2LoadAsync(u16 ctxId, u32 container, u32 callback,
                             u32 userdata, u32 initParam);
s32 cellSysutilAvc2UnloadAsync(void);
s32 cellSysutilAvc2JoinChatRequest(u32 roomId);
s32 cellSysutilAvc2LeaveChatRequest(void);
s32 cellSysutilAvc2StartStreaming(void);
s32 cellSysutilAvc2StopStreaming(void);
s32 cellSysutilAvc2EnableVoiceDetection(u8 enable, u8 level);
s32 cellSysutilAvc2IsMicAttached(u8 deviceType, u32 status);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SYSUTIL_AVC2_H */

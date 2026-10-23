/*
 * ps3recomp - sceNpScore HLE (leaderboards over psnr)
 *
 * Declares the entry points sceNpScore.c exports. The ranking calls take more
 * than eight arguments and are context-aware handlers (registered from
 * ppu_sysprx.cpp); the rest are plain HLE handlers whose guest pointer
 * arguments are 32-bit effective addresses.
 */

#ifndef PS3RECOMP_SCE_NP_SCORE_H
#define PS3RECOMP_SCE_NP_SCORE_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

s32 sceNpScoreCreateTitleCtx(u32 commId, u32 passphrase, u32 selfNpId);
s32 sceNpScoreDestroyTitleCtx(s32 titleCtxId);
s32 sceNpScoreCreateTransactionCtx(s32 titleCtxId);
s32 sceNpScoreDestroyTransactionCtx(s32 transId);
s32 sceNpScoreSetTimeout(s32 id, u32 timeout);
s32 sceNpScoreSetPlayerCharacterId(s32 id, s32 pcId);
s32 sceNpScoreAbortTransaction(s32 transId);
s32 sceNpScorePollAsync(s32 transId, u32 result);
s32 sceNpScoreWaitAsync(s32 transId, u32 result);
s32 sceNpScoreRecordScoreAsync(s32 transId, u32 boardId, s64 score, u32 comment,
                               u32 gameInfo, u32 tmpRank, s32 prio, u32 option);
s32 sceNpScoreRecordScore(s32 transId, u32 boardId, s64 score, u32 comment,
                          u32 gameInfo, u32 tmpRank, u32 option);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_SCORE_H */

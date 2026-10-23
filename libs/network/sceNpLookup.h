/*
 * ps3recomp - sceNpLookup HLE (player profiles and avatars)
 *
 * Declares the entry points sceNpLookup.c exports: title/transaction contexts
 * and the profile lookup that is answered from the NP ID itself. Guest pointer
 * arguments are 32-bit effective addresses (GUEST_PTR).
 */

#ifndef PS3RECOMP_SCE_NP_LOOKUP_H
#define PS3RECOMP_SCE_NP_LOOKUP_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

s32 sceNpLookupInit(void);
s32 sceNpLookupTerm(void);
s32 sceNpLookupCreateTitleCtx(u32 commId, u32 selfNpId);
s32 sceNpLookupDestroyTitleCtx(s32 titleCtxId);
s32 sceNpLookupCreateTransactionCtx(s32 titleCtxId);
s32 sceNpLookupDestroyTransactionCtx(s32 transId);
s32 sceNpLookupAbortTransaction(s32 transId);
s32 sceNpLookupPollAsync(s32 transId, u32 result);
s32 sceNpLookupWaitAsync(s32 transId, u32 result);
s32 sceNpLookupUserProfileAsync(s32 transId, u32 npId, u32 userInfo, u32 aboutMe,
                                u32 languages, u32 countryCode, u32 avatarImage,
                                s32 prio);
s32 sceNpLookupUserProfile(s32 transId, u32 npId, u32 userInfo, u32 aboutMe,
                           u32 languages, u32 countryCode, u32 avatarImage);
s32 sceNpLookupAvatarImageAsync(s32 transId, u32 avatarUrl, u32 avatarImage,
                                s32 prio, u32 option);
s32 sceNpLookupAvatarImage(s32 transId, u32 avatarUrl, u32 avatarImage, u32 option);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_LOOKUP_H */

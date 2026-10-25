/*
 * ps3recomp - sceNpTrophy HLE
 *
 * Trophy/achievement system: create contexts, unlock trophies, query progress.
 */

#ifndef PS3RECOMP_SCE_NP_TROPHY_H
#define PS3RECOMP_SCE_NP_TROPHY_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "sceNp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 *
 * The firmware's values, 0x800229xx. FW 4.93 libsysutil_np_trophy confirms
 * 0x80022901 (already initialized), 02 (not initialized), 03 (not supported),
 * 06 (invalid argument), 0A (unknown context), 0F (processing aborted) and 11
 * (unknown handle); the rest are RPCS3's sceNpTrophy.h at e53ac18f. A title
 * compares against these through its SDK header. The context/handle a call
 * names that the library does not hold is UNKNOWN_*; INVALID_CONTEXT and
 * INVALID_HANDLE are different codes.
 * -----------------------------------------------------------------------*/
#define SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED          0x80022901
#define SCE_NP_TROPHY_ERROR_NOT_INITIALIZED              0x80022902
#define SCE_NP_TROPHY_ERROR_NOT_SUPPORTED                0x80022903
#define SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED       0x80022904
#define SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY                0x80022905
#define SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT             0x80022906
#define SCE_NP_TROPHY_ERROR_EXCEEDS_MAX                  0x80022907
#define SCE_NP_TROPHY_ERROR_INSUFFICIENT                 0x80022909
#define SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT              0x8002290A
#define SCE_NP_TROPHY_ERROR_INVALID_FORMAT               0x8002290B
#define SCE_NP_TROPHY_ERROR_BAD_RESPONSE                 0x8002290C
#define SCE_NP_TROPHY_ERROR_INVALID_GRADE                0x8002290D
#define SCE_NP_TROPHY_ERROR_INVALID_CONTEXT              0x8002290E
#define SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED           0x8002290F
#define SCE_NP_TROPHY_ERROR_ABORT                        0x80022910
#define SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE               0x80022911
#define SCE_NP_TROPHY_ERROR_LOCKED                       0x80022912
#define SCE_NP_TROPHY_ERROR_HIDDEN                       0x80022913
#define SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM       0x80022914
#define SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED             0x80022915
#define SCE_NP_TROPHY_ERROR_INVALID_TYPE                 0x80022916
#define SCE_NP_TROPHY_ERROR_INVALID_HANDLE               0x80022917
#define SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID           0x80022918
#define SCE_NP_TROPHY_ERROR_UNKNOWN_NP_COMM_ID           0x80022919
#define SCE_NP_TROPHY_ERROR_DISC_IO                      0x8002291A
#define SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST          0x8002291B
#define SCE_NP_TROPHY_ERROR_UNSUPPORTED_FORMAT           0x8002291C
#define SCE_NP_TROPHY_ERROR_ALREADY_INSTALLED            0x8002291D
#define SCE_NP_TROPHY_ERROR_BROKEN_DATA                  0x8002291E
#define SCE_NP_TROPHY_ERROR_VERIFICATION_FAILURE         0x8002291F
#define SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID            0x80022920
#define SCE_NP_TROPHY_ERROR_UNKNOWN_TROPHY_ID            0x80022921
#define SCE_NP_TROPHY_ERROR_UNKNOWN_TITLE                0x80022922
#define SCE_NP_TROPHY_ERROR_UNKNOWN_FILE                 0x80022923
#define SCE_NP_TROPHY_ERROR_DISC_NOT_MOUNTED             0x80022924
#define SCE_NP_TROPHY_ERROR_SHUTDOWN                     0x80022925
#define SCE_NP_TROPHY_ERROR_TITLE_ICON_NOT_FOUND         0x80022926
#define SCE_NP_TROPHY_ERROR_TROPHY_ICON_NOT_FOUND        0x80022927
#define SCE_NP_TROPHY_ERROR_INSUFFICIENT_DISK_SPACE      0x80022928
#define SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE               0x8002292A
#define SCE_NP_TROPHY_ERROR_SAVEDATA_USER_DOES_NOT_MATCH 0x8002292B
#define SCE_NP_TROPHY_ERROR_TROPHY_ID_DOES_NOT_EXIST     0x8002292C
#define SCE_NP_TROPHY_ERROR_SERVICE_UNAVAILABLE          0x8002292D
#define SCE_NP_TROPHY_ERROR_UNKNOWN                      0x800229FF

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define SCE_NP_TROPHY_INVALID_CONTEXT       ((SceNpTrophyContext)0)
#define SCE_NP_TROPHY_INVALID_HANDLE        ((SceNpTrophyHandle)0)
#define SCE_NP_TROPHY_INVALID_TROPHY_ID     ((SceNpTrophyId)-1)

#define SCE_NP_TROPHY_MAX_NUM_TROPHIES      128
#define SCE_NP_TROPHY_NAME_MAX_SIZE         128
#define SCE_NP_TROPHY_DESC_MAX_SIZE         256
#define SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE   128
#define SCE_NP_TROPHY_GAME_DESC_MAX_SIZE    1024

#define SCE_NP_TROPHY_MAX_CONTEXTS          4
#define SCE_NP_TROPHY_MAX_HANDLES           4

/* Trophy grades */
#define SCE_NP_TROPHY_GRADE_UNKNOWN         0
#define SCE_NP_TROPHY_GRADE_PLATINUM        1
#define SCE_NP_TROPHY_GRADE_GOLD            2
#define SCE_NP_TROPHY_GRADE_SILVER          3
#define SCE_NP_TROPHY_GRADE_BRONZE          4

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef s32 SceNpTrophyContext;
typedef s32 SceNpTrophyHandle;
typedef s32 SceNpTrophyId;

typedef struct SceNpTrophyDetails {
    u32  trophyId;
    u32  trophyGrade;
    char name[SCE_NP_TROPHY_NAME_MAX_SIZE];
    char description[SCE_NP_TROPHY_DESC_MAX_SIZE];
    u8   hidden;
    u8   padding[3];
} SceNpTrophyDetails;

typedef struct SceNpTrophyData {
    u64  timestamp;     /* UNIX timestamp when unlocked, 0 if locked */
    u32  trophyId;
    u8   unlocked;
    u8   padding[3];
} SceNpTrophyData;

typedef struct SceNpTrophyGameDetails {
    u32  numTrophies;
    u32  numPlatinum;
    u32  numGold;
    u32  numSilver;
    u32  numBronze;
    char title[SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE];
    char description[SCE_NP_TROPHY_GAME_DESC_MAX_SIZE];
} SceNpTrophyGameDetails;

typedef struct SceNpTrophyGameData {
    u32  unlockedTrophies;
    u32  unlockedPlatinum;
    u32  unlockedGold;
    u32  unlockedSilver;
    u32  unlockedBronze;
} SceNpTrophyGameData;

/* Bitfield for trophy unlock state: 1 bit per trophy */
typedef struct SceNpTrophyFlagArray {
    u32 flag[SCE_NP_TROPHY_MAX_NUM_TROPHIES / 32];
} SceNpTrophyFlagArray;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options);
s32 sceNpTrophyTerm(void);

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options);
s32 sceNpTrophyDestroyContext(SceNpTrophyContext context);

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle);
s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle);

/* Real ABI: (context, handle, statusCb, arg, options).  statusCb is a guest
 * OPD for SceNpTrophyStatusCallback(context, status, completed, total, arg);
 * the game blocks on registration-complete which is signalled via this cb. */
s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               u32 statusCb,
                               u32 arg,
                               u64 options);

/* SceNpTrophyStatus values fired to the status callback */
#define SCE_NP_TROPHY_STATUS_UNKNOWN            0
#define SCE_NP_TROPHY_STATUS_NOT_INSTALLED      1
#define SCE_NP_TROPHY_STATUS_DATA_CORRUPT       2
#define SCE_NP_TROPHY_STATUS_INSTALLED          3
#define SCE_NP_TROPHY_STATUS_REQUIRES_UPDATE    4
#define SCE_NP_TROPHY_STATUS_PROCESSING_SETUP    5
#define SCE_NP_TROPHY_STATUS_PROCESSING_PROGRESS 6
#define SCE_NP_TROPHY_STATUS_PROCESSING_FINALIZE 7
#define SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE 8

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options);

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data);

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data);

s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId);

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count);

s32 sceNpTrophyGetGameProgress(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               s32* percentage);

/* Set the trophy storage directory (defaults to "gamedata/trophies") */
void sceNpTrophySetStoragePath(const char* path);

/* Host-side: how many sceNpTrophyRegisterContext calls returned at the wait cap
 * (trophy.register_wait_cap_vblanks) instead of after the completion was
 * consumed. A counter for a run's report; the cap is also logged. */
u32 sceNpTrophy_register_wait_cap_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_TROPHY_H */

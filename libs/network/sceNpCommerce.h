/*
 * ps3recomp - sceNpCommerce HLE
 *
 * PlayStation Store / in-game commerce: product browsing, purchasing,
 * and entitlement checking. Offline stub.
 */

#ifndef PS3RECOMP_SCE_NP_COMMERCE_H
#define PS3RECOMP_SCE_NP_COMMERCE_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED     0x80024301
#define SCE_NP_COMMERCE2_ERROR_ALREADY_INITIALIZED 0x80024302
#define SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT    0x80024303
#define SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED       0x80024304
#define SCE_NP_COMMERCE2_ERROR_INSUFFICIENT        0x80024305

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u32 SceNpCommerce2Ctx;

typedef struct SceNpCommerce2CategoryInfo {
    char categoryId[64];
    char categoryName[128];
    u32  countOfSubCategory;
    u32  countOfProduct;
} SceNpCommerce2CategoryInfo;

typedef struct SceNpCommerce2ProductInfo {
    char productId[64];
    char productName[128];
    char priceStr[32];
    u32  priceCents;
    u8   isPurchased;
    u8   reserved[3];
} SceNpCommerce2ProductInfo;

/* ---------------------------------------------------------------------------
 * Functions
 *
 * This header declares only the entry points sceNpCommerce.c defines. Init,
 * Term, CreateCtx, DestroyCtx, GetCategoryContentsStart and
 * DoCheckoutFinishAsync are sceNpCommerce2.c's (see sceNpCommerce2.h); the
 * stale copies that stood here had different signatures (a decoded context
 * pointer vs the SDK's u32 handle), and a translation unit that includes both
 * headers could not compile.
 * -----------------------------------------------------------------------*/

s32 sceNpCommerce2GetCategoryInfo(SceNpCommerce2Ctx ctx,
                                    SceNpCommerce2CategoryInfo* info);
s32 sceNpCommerce2GetProductInfo(SceNpCommerce2Ctx ctx, u32 index,
                                   SceNpCommerce2ProductInfo* info);

/* Store overlay */
s32 sceNpCommerce2InitStoreRequest(SceNpCommerce2Ctx ctx);
s32 sceNpCommerce2DoCheckoutStartAsync(SceNpCommerce2Ctx ctx,
                                         const char* productId);
s32 sceNpCommerce2DoCheckoutFinishAsync(SceNpCommerce2Ctx ctx);
s32 sceNpCommerce2GetResult(SceNpCommerce2Ctx ctx, s32* result);

/* Entitlements */
s32 sceNpCommerce2DoEntitlementListStartAsync(SceNpCommerce2Ctx ctx);
s32 sceNpCommerce2DoEntitlementListFinishAsync(SceNpCommerce2Ctx ctx);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_COMMERCE_H */

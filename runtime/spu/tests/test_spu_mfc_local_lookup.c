/* MFC lookup, first-use creation and release stay off shared registry locks. */
#include "../../platform/win32_compat.h"
#include <stdatomic.h>
#include <stdlib.h>

static atomic_uint g_lock_acquisitions;

static void counted_shared(SRWLOCK* lock)
{
    atomic_fetch_add_explicit(&g_lock_acquisitions, 1, memory_order_relaxed);
    AcquireSRWLockShared(lock);
}

static void counted_exclusive(SRWLOCK* lock)
{
    atomic_fetch_add_explicit(&g_lock_acquisitions, 1, memory_order_relaxed);
    AcquireSRWLockExclusive(lock);
}

#define AcquireSRWLockShared counted_shared
#define AcquireSRWLockExclusive counted_exclusive
#define main contexts_previous_main
#include "test_spu_mfc_contexts.c"
#undef main
#undef AcquireSRWLockShared
#undef AcquireSRWLockExclusive

int main(void)
{
    unsetenv("SPU_PCSAMPLE");
    unsetenv("SPU_MFC_STATS");
    spu_context* ctx = (spu_context*)calloc(1, sizeof(*ctx));
    CHECK(ctx != NULL);
    spu_context_init(ctx, 0);

    mfc_engine* engine = mfc_for(ctx);
    CHECK(engine == &ctx->mfc);
    CHECK(spu_mfc_has_engine(ctx));
    CHECK(atomic_load_explicit(&g_lock_acquisitions, memory_order_relaxed) == 0);
    engine->queue_count = 7;
    engine->tag_completed = 0x12345678;
    for (unsigned i = 0; i < 100000; ++i)
        CHECK(mfc_for(ctx) == engine);
    CHECK(engine->queue_count == 7 && engine->tag_completed == 0x12345678);
    CHECK(atomic_load_explicit(&g_lock_acquisitions, memory_order_relaxed) == 0);

    spu_mfc_release(ctx);
    CHECK(!spu_mfc_has_engine(ctx));
    CHECK(atomic_load_explicit(&g_lock_acquisitions, memory_order_relaxed) == 0);
    engine = mfc_for(ctx);
    CHECK(engine == &ctx->mfc);
    CHECK(engine->queue_count == 0 && engine->tag_completed == 0xFFFFFFFFu);
    spu_mfc_release(ctx);
    spu_mfc_forget_range((uintptr_t)ctx, (uintptr_t)ctx + sizeof(*ctx));
    CHECK(atomic_load_explicit(&g_lock_acquisitions, memory_order_relaxed) == 0);
    free(ctx);

    printf("SPU MFC local lookup: %d passed, %d failed; 100000 lookups used no shared lock\n",
           g_pass, g_fail);
    return g_fail ? 1 : 0;
}

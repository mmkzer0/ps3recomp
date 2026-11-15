/* Context-owned MFC state must not alias a copied context, and the optional
 * PC-sample registry must retire abandoned stack owners without reading them. */
#include "../../platform/win32_compat.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define main contexts_previous_main
#include "test_spu_mfc_contexts.c"
#undef main


static unsigned diagnostic_count(void)
{
    unsigned count = 0;
    AcquireSRWLockShared(&s_mfc_pcsample_lock);
    for (spu_mfc_slot* slot = s_mfc_pcsample_contexts; slot; slot = slot->next)
        ++count;
    ReleaseSRWLockShared(&s_mfc_pcsample_lock);
    return count;
}

int main(void)
{
    unsetenv("SPU_PCSAMPLE");
    unsetenv("SPU_MFC_STATS");
    spu_context* owner = (spu_context*)calloc(1, sizeof(*owner));
    spu_context* copy = (spu_context*)malloc(sizeof(*copy));
    CHECK(owner != NULL && copy != NULL);
    if (!owner || !copy) return 1;
    spu_context_init(owner, 0);
    CHECK(!spu_mfc_has_engine(owner));

    mfc_engine* original = mfc_for(owner);
    CHECK(original == &owner->mfc);
    original->queue_count = 7;
    original->tag_completed = 0x12345678;
    memcpy(copy, owner, sizeof(*copy));
    mfc_engine* duplicate = mfc_for(copy);
    CHECK(duplicate == &copy->mfc && duplicate != original);
    CHECK(duplicate->queue_count == 7 && duplicate->tag_completed == 0x12345678);
    duplicate->queue_count = 19;
    CHECK(original->queue_count == 7);
    spu_mfc_release(copy);
    free(copy);
    CHECK(mfc_for(owner) == original);
    CHECK(original->queue_count == 7 && original->tag_completed == 0x12345678);

    spu_mfc_release(owner);
    CHECK(!spu_mfc_has_engine(owner));
    mfc_engine* fresh = mfc_for(owner);
    CHECK(fresh == original);
    CHECK(fresh->queue_count == 0 && fresh->tag_completed == 0xFFFFFFFFu);
    spu_mfc_release(owner);


    /* Enable only the registry state: no sampler thread or timing is needed
     * to verify that range cleanup compares saved addresses, not stack bytes. */
    atomic_store_explicit(&s_mfc_pcsample_state, MFC_PCSAMPLE_ON, memory_order_release);
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        CHECK(0);
        free(owner);
        return 1;
    }
    const size_t used = 3 * sizeof(spu_context);
    const size_t mapped = (used + (size_t)page - 1) / (size_t)page * (size_t)page;
    spu_context* abandoned = (spu_context*)mmap(NULL, mapped, PROT_READ | PROT_WRITE,
                                                MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(abandoned != MAP_FAILED);
    if (abandoned == MAP_FAILED) {
        free(owner);
        return 1;
    }
    for (unsigned i = 0; i < 3; ++i) {
        spu_context_init(&abandoned[i], i);
        (void)mfc_for(&abandoned[i]);
    }
    spu_context* outside = (spu_context*)calloc(1, sizeof(*outside));
    CHECK(outside != NULL);
    if (!outside) {
        spu_mfc_forget_range((uintptr_t)abandoned,
                             (uintptr_t)abandoned + 3 * sizeof(spu_context));
        munmap(abandoned, mapped);
        free(owner);
        return 1;
    }
    spu_context_init(outside, 0);
    mfc_engine* outside_engine = mfc_for(outside);
    CHECK(diagnostic_count() == 4);
    CHECK(mprotect(abandoned, mapped, PROT_NONE) == 0);
    spu_mfc_forget_range((uintptr_t)abandoned,
                         (uintptr_t)abandoned + 3 * sizeof(spu_context));
    CHECK(munmap(abandoned, mapped) == 0);
    CHECK(diagnostic_count() == 1);
    CHECK(s_mfc_pcsample_contexts && s_mfc_pcsample_contexts->ctx == outside &&
          !s_mfc_pcsample_contexts->next);
    CHECK(mfc_for(outside) == outside_engine);
    spu_mfc_release(outside);
    CHECK(diagnostic_count() == 0);
    free(outside);

    free(owner);
    printf("SPU MFC context lifetime: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* spu_mfc_has_engine is read by shadow/served checks while the context owner
 * may create or retire its engine on another host thread. */
#include "../../platform/win32_compat.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#define main contexts_previous_main
#include "test_spu_mfc_contexts.c"
#undef main

typedef struct {
    spu_context* ctx;
    atomic_int stop;
    atomic_int invalid;
} reader_request;

static void* has_engine_reader(void* opaque)
{
    reader_request* req = opaque;
    while (!atomic_load_explicit(&req->stop, memory_order_acquire)) {
        const int active = spu_mfc_has_engine(req->ctx);
        if (active != 0 && active != 1)
            atomic_store_explicit(&req->invalid, 1, memory_order_relaxed);
    }
    return NULL;
}

int main(void)
{
    unsetenv("SPU_PCSAMPLE");
    unsetenv("SPU_MFC_STATS");
    spu_context* ctx = (spu_context*)calloc(1, sizeof(*ctx));
    CHECK(ctx != NULL);
    if (!ctx) return 1;
    spu_context_init(ctx, 0);
    CHECK(!spu_mfc_has_engine(ctx));

    reader_request req = { .ctx = ctx };
    atomic_init(&req.stop, 0);
    atomic_init(&req.invalid, 0);
    pthread_t reader;
    if (pthread_create(&reader, NULL, has_engine_reader, &req) != 0) {
        CHECK(0);
        free(ctx);
        return 1;
    }
    for (unsigned i = 0; i < 10000; ++i) {
        mfc_engine* engine = mfc_for(ctx);
        CHECK(engine == &ctx->mfc);
        spu_mfc_release(ctx);
    }
    atomic_store_explicit(&req.stop, 1, memory_order_release);
    CHECK(pthread_join(reader, NULL) == 0);
    CHECK(!atomic_load_explicit(&req.invalid, memory_order_relaxed));
    CHECK(!spu_mfc_has_engine(ctx));
    free(ctx);

    printf("SPU MFC engine presence: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

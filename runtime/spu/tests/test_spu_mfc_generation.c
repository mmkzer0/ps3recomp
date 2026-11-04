/* spu.mfc_lookup_cache: a host thread's remembered engine is never returned
 * once its slot has been released, forgotten or handed to another context.
 *
 * The contract is mfc_for(ctx) == the engine the registry holds for ctx, in
 * both modes. The cache is only allowed to make that cheaper, and what lets it
 * do so safely is that every removal of a slot publishes a new generation
 * before the slot is freed. A thread that remembered an engine, with the
 * removal done by another thread, cannot learn of it any other way, so each
 * case below is three threads in a fixed order:
 *
 *   holder   looks the context up (so its cache holds it) and parks
 *   mutator  removes, and perhaps re-creates, slots, then ends
 *   holder   looks the context up again, and the result must be the live slot
 *
 * Each case runs from three starting generations: a fresh one, the one just
 * below saturation (the removal saturates it) and the saturated one (hits are
 * off for good). A stale hit shows as a context that has no slot after its
 * lookup, or as an engine that now belongs to a different context. The checks
 * read the registry directly, so they do not depend on what the allocator
 * hands out next.
 *
 * Steps are ordered with gates, never with sleeps. */
#include "../../platform/win32_compat.h"
#include <pthread.h>
#include <stdatomic.h>
#define main generation_existing_main
#include "test_spu_mfc_slots.c"
#undef main

static int s_expect_cache;   /* the harness's "on"/"off" argument */

typedef struct { pthread_mutex_t m; pthread_cond_t c; int open; } gate;
static void gate_init(gate* g)
{
    pthread_mutex_init(&g->m, NULL);
    pthread_cond_init(&g->c, NULL);
    g->open = 0;
}
static void gate_open(gate* g)
{
    pthread_mutex_lock(&g->m);
    g->open = 1;
    pthread_cond_broadcast(&g->c);
    pthread_mutex_unlock(&g->m);
}
static void gate_wait(gate* g)
{
    pthread_mutex_lock(&g->m);
    while (!g->open) pthread_cond_wait(&g->c, &g->m);
    pthread_mutex_unlock(&g->m);
}

/* The engine the registry holds for ctx, or NULL. Does not go through mfc_for,
 * so it is the same in both modes. */
static mfc_engine* registered_engine(spu_context* ctx)
{
    mfc_engine* found = NULL;
    AcquireSRWLockShared(&s_mfc_claim_lock);
    for (spu_mfc_slot* slot = s_mfc_slots; slot; slot = slot->next)
        if (slot->ctx == ctx) found = &slot->mfc;
    ReleaseSRWLockShared(&s_mfc_claim_lock);
    return found;
}

static spu_context pool[3];
#define OWNER  (&pool[0])   /* the context the holder keeps looking up */
#define PEER   (&pool[1])   /* a second live context                   */
#define NEWCOM (&pool[2])   /* a context created after a release       */

typedef struct {
    const char* name;
    void (*mutate)(void);
    int owner_survives;      /* OWNER's slot is still the same one afterwards */
} scenario;

static void forget(spu_context* ctx)
{
    spu_mfc_forget_range((uintptr_t)ctx, (uintptr_t)ctx + sizeof(*ctx));
}

static void mutate_release_owner(void) { spu_mfc_release(OWNER); }
static void mutate_forget_owner(void)  { forget(OWNER); }
static void mutate_release_peer(void)  { spu_mfc_release(PEER); }
static void mutate_forget_peer(void)   { forget(PEER); }
/* The slot's memory goes to a different context, and the old one comes back
 * with a new slot: a stale pointer would now be someone else's engine. */
static void mutate_reuse(void)
{
    spu_mfc_release(OWNER);
    (void)mfc_for(NEWCOM);
    (void)mfc_for(OWNER);
}

static const scenario scenarios[] = {
    { "release the owner",                  mutate_release_owner, 0 },
    { "forget the owner",                   mutate_forget_owner,  0 },
    { "release, reuse for another, reclaim", mutate_reuse,        0 },
    { "release another context",            mutate_release_peer,  1 },
    { "forget another context",             mutate_forget_peer,   1 },
};

typedef struct {
    const scenario* sc;
    gate populated, mutated;
    mfc_engine* before;
    int hit_precondition;
} run;

static void* holder_thread(void* opaque)
{
    run* r = opaque;
    (void)mfc_for(PEER);
    r->before = mfc_for(OWNER);
    /* With the cache on, the thread must really be holding OWNER here, or
     * the case would pass without testing anything. */
    r->hit_precondition = !s_expect_cache ||
                          (s_mfc_recent.ctx == OWNER && s_mfc_recent.engine == r->before);
    gate_open(&r->populated);
    gate_wait(&r->mutated);

    mfc_engine* got = mfc_for(OWNER);
    mfc_engine* live = registered_engine(OWNER);
    CHECK(live != NULL);
    CHECK(got == live);
    CHECK(got != registered_engine(NEWCOM));   /* never another context's engine */
    if (r->sc->owner_survives) CHECK(got == r->before);
    for (unsigned i = 0; i < 100; ++i) CHECK(mfc_for(OWNER) == live);
    return NULL;
}

static void* mutator_thread(void* opaque)
{
    ((run*)opaque)->sc->mutate();
    return NULL;
}

static void run_scenario(const scenario* sc, uint64_t start)
{
    CHECK(s_mfc_slots == NULL);
    atomic_store(&s_mfc_cache_generation, start);
    run r = { .sc = sc };
    gate_init(&r.populated);
    gate_init(&r.mutated);

    pthread_t holder, mutator;
    CHECK(pthread_create(&holder, NULL, holder_thread, &r) == 0);
    gate_wait(&r.populated);
    CHECK(r.hit_precondition);

    uint64_t g0 = atomic_load(&s_mfc_cache_generation);
    CHECK(pthread_create(&mutator, NULL, mutator_thread, &r) == 0);
    CHECK(pthread_join(mutator, NULL) == 0);
    uint64_t g1 = atomic_load(&s_mfc_cache_generation);
    /* Exactly one slot was removed: one new generation, or the saturated one
     * for good. It never wraps to a value a stale thread could match. */
    CHECK(g1 == (g0 == UINT64_MAX ? UINT64_MAX : g0 + 1));

    gate_open(&r.mutated);
    CHECK(pthread_join(holder, NULL) == 0);

    spu_mfc_release(OWNER);
    spu_mfc_release(PEER);
    spu_mfc_release(NEWCOM);
    CHECK(s_mfc_slots == NULL);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* The expected mode comes from the harness, not from the configuration: with
     * the option unset the holder must really hold the engine (the shipped
     * default is on), which reading the option back would not check. */
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        fprintf(stderr, "usage: %s on|off\n", argv[0]);
        return 2;
    }
    s_expect_cache = !strcmp(argv[1], "on");
    static const uint64_t starts[] = { 1, UINT64_MAX - 1, UINT64_MAX };
    for (unsigned s = 0; s < sizeof(starts) / sizeof(starts[0]); ++s)
        for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i)
            run_scenario(&scenarios[i], starts[s]);
    printf("SPU MFC cache generation (cache %d): %d passed, %d failed\n",
           s_expect_cache, g_pass, g_fail);
    return g_fail ? 1 : 0;
}

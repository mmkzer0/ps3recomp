/* rsx_tiling.c: the bound-tile mask under threads (#131).
 *
 * s_rsx_tiles_bound is written by the PPU thread (cellGcmSetTile -> rsx_tiling_set) and read without the lock by the FIFO
 * walker and the draw engine (rsx_tiling_active, the first test of rsx_tiling_find). It was `volatile LONG`, which is
 * no synchronisation: a data race against the interlocked store. It is an atomic with release stores and acquire loads
 * now; the tile entries stay under the lock.
 *
 * Two threads bind, unbind and rebind disjoint tiles (the mask is read-modified-written by both) while a third reads
 * through the lock-free path. Under ThreadSanitizer (ctest --preset tsan -R rsx_tiling_threads) the old volatile read is a
 * reported race on its first overlap, whatever the interleaving; without it the checks below catch a hit that is not a
 * whole tile and a mask that does not end up empty once every tile is unbound.
 *
 * clang -std=gnu17 -I include -I libs/video libs/video/tests/test_rsx_tiling_threads.c libs/video/rsx_tiling.c -pthread
 */
#include "rsx_tiling.h"
#include "rsx_dispatch.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#define TILES 8u
#define ITER 20000
#define TILE_SIZE 0x10000u
#define TILE_OFF(i) ((i) * 0x100000u)

static atomic_int g_go;
static atomic_int g_done;
static atomic_long g_hits, g_bad;

static void bind(u32 i) { rsx_tiling_set(i, RSX_LOCATION_MAIN, TILE_OFF(i), TILE_SIZE, 1024u, i & 3u); }

/* Writer w owns tiles 4w .. 4w+3: unbind and rebind them in turn, leave them bound. */
static void* writer(void* arg)
{
    const u32 base = (u32)(uintptr_t)arg * 4u;
    while (!atomic_load_explicit(&g_go, memory_order_acquire)) { }
    for (int it = 0; it < ITER; it++)
        for (u32 k = 0; k < 4; k++) {
            const u32 i = base + k;
            if ((it + (int)k) & 1) rsx_tiling_set(i, 0, 0, 0, 0, 0);
            else bind(i);
        }
    for (u32 k = 0; k < 4; k++) bind(base + k);
    return NULL;
}

/* The walker's side: the lock-free test, then the lookup. A hit is a whole tile of the table or nothing. */
static void* reader(void* arg)
{
    (void)arg;
    while (!atomic_load_explicit(&g_go, memory_order_acquire)) { }
    u32 x = 12345;
    while (!atomic_load_explicit(&g_done, memory_order_acquire)) {
        x = x * 1664525u + 1013904223u;
        const u32 i = (x >> 16) % TILES;
        rsx_tile t;
        (void)rsx_tiling_active();
        if (rsx_tiling_find(RSX_LOCATION_MAIN, TILE_OFF(i) + 100, 1, &t)) {
            atomic_fetch_add_explicit(&g_hits, 1, memory_order_relaxed);
            if (t.offset != TILE_OFF(i) || t.size != TILE_SIZE || t.pitch != 1024u || t.bank != (i & 3u))
                atomic_fetch_add_explicit(&g_bad, 1, memory_order_relaxed);
        }
    }
    return NULL;
}

int main(void)
{
    pthread_t w0, w1, r;
    assert(pthread_create(&w0, NULL, writer, (void*)(uintptr_t)0) == 0);
    assert(pthread_create(&w1, NULL, writer, (void*)(uintptr_t)1) == 0);
    assert(pthread_create(&r, NULL, reader, NULL) == 0);
    atomic_store_explicit(&g_go, 1, memory_order_release);
    assert(pthread_join(w0, NULL) == 0);
    assert(pthread_join(w1, NULL) == 0);
    atomic_store_explicit(&g_done, 1, memory_order_release);
    assert(pthread_join(r, NULL) == 0);
    assert(atomic_load(&g_bad) == 0);

    /* Every tile ends bound, so the mask says so and each lookup finds its own tile. */
    assert(rsx_tiling_active());
    for (u32 i = 0; i < TILES; i++) {
        rsx_tile t;
        assert(rsx_tiling_find(RSX_LOCATION_MAIN, TILE_OFF(i), TILE_SIZE, &t) && t.offset == TILE_OFF(i));
    }
    /* The mask is per tile: it stays set until the last tile is unbound, and is then empty (a bit that two writers
     * left behind, or cleared for a tile that is bound, shows here). */
    for (u32 i = 0; i + 1 < TILES; i++) {
        rsx_tiling_set(i, 0, 0, 0, 0, 0);
        assert(rsx_tiling_active());
    }
    rsx_tiling_set(TILES - 1, 0, 0, 0, 0, 0);
    assert(!rsx_tiling_active());
    rsx_tile none;
    assert(!rsx_tiling_find(RSX_LOCATION_MAIN, 0, 0xFFFFFFFFu, &none));
    printf("test_rsx_tiling_threads: ok (%ld lock-free hits seen by the reader)\n", (long)atomic_load(&g_hits));
    return 0;
}

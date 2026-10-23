/* The SPU_SMC_WATCH gate (g_spu_smc_watch, spu_context.h) is -1 until the first
 * local-store write reads the environment. Two SPU workers can make their first
 * write at once, so the gate and the probe's own lazily parsed settings must be
 * race-free. Two threads wait on a spin barrier and then each does its first
 * spu_ls_write128; built with -fsanitize=thread (tools/test_spu_smc_gate.py) a
 * racy gate is a ThreadSanitizer report and exit status 66.
 *
 * argv[1] = "off": SPU_SMC_WATCH unset, the gate settles at 0.
 *           "on":  SPU_SMC_WATCH=5 with SPU_SMC_LO/HI around the write: the gate
 *                  settles at 1 and the probe logs the hit (each thread has its
 *                  own context, image 5).
 * No other symbol is needed: the gate is a weak definition in the header. */
#include "../spu_helpers.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The LS watchpoint list is off, as when SPU_LS_WATCH is unset. */
int g_spu_ls_watch_n = 0;
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr)
{
    (void)l; (void)w; (void)p; (void)pc; (void)lr;
    abort();
}

#define WRITES 64
static spu_context g_ctx[2];
static atomic_int g_arrived;

static void* worker(void* arg)
{
    spu_context* c = (spu_context*)arg;
    atomic_fetch_add(&g_arrived, 1);
    while (atomic_load(&g_arrived) < 2) { /* spin: both first writes together */ }
    for (int i = 0; i < WRITES; i++) {
        u128 v;
        for (int w = 0; w < 4; w++) v._u32[w] = 0x01020304u * (uint32_t)(i + 1);
        spu_ls_write128(c, 0x1000u + 16u * (uint32_t)(i & 7), v);
    }
    return NULL;
}

int main(int argc, char** argv)
{
    const int on = argc > 1 && !strcmp(argv[1], "on");
    for (int i = 0; i < 2; i++) {
        spu_context_init(&g_ctx[i], 0);
        g_ctx[i].image_id = 5;
    }
    pthread_t t[2];
    for (int i = 0; i < 2; i++) pthread_create(&t[i], NULL, worker, &g_ctx[i]);
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);

    int bad = 0;
    for (int i = 0; i < 2; i++) {
        const uint8_t* p = g_ctx[i].ls + 0x1000;
        /* the last write to line (WRITES-1)&7 */
        const uint32_t want = 0x01020304u * (uint32_t)WRITES;
        const uint32_t got = ((uint32_t)p[16 * ((WRITES - 1) & 7)] << 24) | ((uint32_t)p[16 * ((WRITES - 1) & 7) + 1] << 16) |
                             ((uint32_t)p[16 * ((WRITES - 1) & 7) + 2] << 8) | p[16 * ((WRITES - 1) & 7) + 3];
        if (got != want) { printf("FAIL: ctx %d line holds %08X, want %08X\n", i, got, want); bad = 1; }
    }
    extern int g_spu_smc_watch;
    const int gate = __atomic_load_n(&g_spu_smc_watch, __ATOMIC_RELAXED);
    if (gate != (on ? 1 : 0)) { printf("FAIL: the gate settled at %d\n", gate); bad = 1; }
    if (bad) return 1;
    printf("PASS: smc gate %s\n", on ? "on" : "off");
    return 0;
}

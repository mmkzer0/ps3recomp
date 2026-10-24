/* Probe: spu_pchist_tick (runtime/spu/spu_context.h) is entered by every SPU
 * worker on its first trampoline hop. With SPU_PCHIST unset its gate is still
 * lazily written, and a process-global gate is a data race between workers.
 * Two threads call the tick together; tools/test_spu_trace_file.py builds this
 * with -fsanitize=thread, where a race is a report and exit status 66. */
#include "../spu_context.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static spu_context g_ctx[2];
static atomic_int  g_ready;

static void* worker(void* arg)
{
    spu_context* c = (spu_context*)arg;
    atomic_fetch_add_explicit(&g_ready, 1, memory_order_acq_rel);
    while (atomic_load_explicit(&g_ready, memory_order_acquire) < 2) { /* spin */ }
    for (int i = 0; i < 1000; i++) {
        c->pc = (uint32_t)i * 16u;
        spu_pchist_tick(c);
    }
    return NULL;
}

int main(void)
{
    for (int i = 0; i < 2; i++) spu_context_init(&g_ctx[i], (uint32_t)i);
    pthread_t t[2];
    for (int i = 0; i < 2; i++)
        if (pthread_create(&t[i], NULL, worker, &g_ctx[i]) != 0) return 3;
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);
    printf("PASS: pchist tick under two workers\n");
    return 0;
}

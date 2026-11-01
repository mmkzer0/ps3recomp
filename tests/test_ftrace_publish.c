/*
 * test_ftrace_publish - the recorder's state is published to threads that did not initialise it.
 *
 * ps3_ft_init (include/ps3emu/ps3_ftrace.h) writes buf, cap, start, end and min, then stores on = 2 with
 * RELEASE. The hooks read `on` first with a relaxed load, so a thread that sees 2 has to acquire before it
 * touches that state: on arm64 it can otherwise read a stale (NULL) buf.
 *
 * One thread runs the first PS3_FT_ON() (the initialisation). The emitters never call it until they have
 * seen on == 2 through a relaxed load of the word itself, which gives them no ordering with the initialiser
 * (they were created before it, and nothing else synchronises them with it). Then they emit spans. Under
 * ThreadSanitizer the unordered reads of start/end/buf are a reported race, so the test fails there when the
 * acquire in ps3_ft_recording() is removed; everywhere it also checks that every span landed.
 */
#include "ps3emu/ps3_ftrace.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

enum { EMITTERS = 3, EACH = 2000 };

static void* emitter(void* arg)
{
    const uint32_t id = (uint32_t)(uintptr_t)arg;
    while (__atomic_load_n(&g_ps3_ft.on, __ATOMIC_RELAXED) != 2) sched_yield();
    for (uint32_t i = 0; i < EACH; i++) {
        if (!PS3_FT_ON()) continue;
        const uint64_t t = ps3_ft_now();
        ps3_ft_span(PS3_FT_SYSCALL, id, i, 0, t, t);
    }
    return NULL;
}

static void* initialiser(void* arg)
{
    (void)arg;
    return PS3_FT_ON() ? NULL : (void*)1;
}

int main(void)
{
    setenv("PS3_FTRACE", "0:100000:/dev/null", 1);
    setenv("PS3_FTRACE_CAP", "65536", 1);

    pthread_t e[EMITTERS], init;
    for (uintptr_t t = 0; t < EMITTERS; t++)
        if (pthread_create(&e[t], NULL, emitter, (void*)(t + 1)) != 0) return 2;
    if (pthread_create(&init, NULL, initialiser, NULL) != 0) return 2;
    void* init_failed = NULL;
    pthread_join(init, &init_failed);
    for (int t = 0; t < EMITTERS; t++) pthread_join(e[t], NULL);

    int failures = 0;
    if (init_failed) { printf("FAIL: the first PS3_FT_ON() did not start recording\n"); failures++; }
    if (!g_ps3_ft.buf) { printf("FAIL: no buffer\n"); return 1; }
    if (g_ps3_ft.n != (uint32_t)EMITTERS * EACH || g_ps3_ft.dropped != 0) {
        printf("FAIL: %u spans (%u dropped), want %d\n", g_ps3_ft.n, g_ps3_ft.dropped, EMITTERS * EACH);
        failures++;
    }
    unsigned per[EMITTERS + 1] = { 0 };
    for (uint32_t i = 0; i < g_ps3_ft.n && i < g_ps3_ft.cap; i++) {
        const ps3_ft_rec* r = &g_ps3_ft.buf[i];
        if (r->kind != PS3_FT_SYSCALL || r->who < 1 || r->who > EMITTERS) { failures++; continue; }
        per[r->who]++;
    }
    for (int t = 1; t <= EMITTERS; t++)
        if (per[t] != EACH) { printf("FAIL: emitter %d recorded %u spans, want %d\n", t, per[t], EACH); failures++; }
    printf("%s ftrace_publish\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

/* ps3_thread_name (include/ps3emu/thread_name.h) on POSIX hosts: it names the
 * calling thread and only that one, formats its arguments, and on Linux keeps
 * the 15 bytes the kernel keeps. The names are what per-thread CPU and energy
 * tables (and the run log's thread labels) group by. */
#include "ps3emu/thread_name.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#if defined(__linux__)
#  include <sys/prctl.h>
#endif

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

static void own_name(char out[64])
{
    memset(out, 0, 64);
#if defined(__linux__)
    prctl(PR_GET_NAME, (unsigned long)out, 0, 0, 0);
#else
    pthread_getname_np(pthread_self(), out, 64);
#endif
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_named;

static void* worker(void* arg)
{
    (void)arg;
    ps3_thread_name("jq2 q%u w%u %s", 3u, 12u, "with-a-long-tail-past-fifteen");
    char got[64];
    own_name(got);
#if defined(__linux__)
    CHECK(strcmp(got, "jq2 q3 w12 with") == 0);   /* 15 bytes, cut as the kernel would */
#else
    CHECK(strcmp(got, "jq2 q3 w12 with-a-long-tail-past-fifteen") == 0);
#endif
    pthread_mutex_lock(&g_mu);
    g_named = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

int main(void)
{
#if !defined(__APPLE__) && !defined(__linux__)
    printf("thread_name: no naming on this host, skipped\n");
    return 77;
#else
    char before[64];
    own_name(before);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, worker, NULL) == 0);
    pthread_mutex_lock(&g_mu);
    while (!g_named) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    pthread_join(t, NULL);

    char after[64];
    own_name(after);
    CHECK(strcmp(before, after) == 0);           /* the main thread was not renamed */

    ps3_thread_name("ppu %s", "main");
    own_name(after);
    CHECK(strcmp(after, "ppu main") == 0);
    if (!g_fail) printf("thread_name: ok\n");
    return g_fail;
#endif
}

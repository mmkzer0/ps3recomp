/* One scenario of ps3_log per process: configure is once-only and reads the environment,
 * so every case that changes the configuration is a process of its own. test_log.py sets
 * PS3RECOMP_LOG_* in the environment, runs a scenario and reads stderr and the log file. */
#include "ps3emu/config.h"
#include "ps3emu/ps3_log.h"
#include "ps3emu/host_exit.h"
#include "ps3emu/stop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
typedef HANDLE probe_thread;
#else
#  include <pthread.h>
#  include <sched.h>
typedef pthread_t probe_thread;
#endif

static int g_evaluated;
static int side_effect(void) { return __atomic_add_fetch(&g_evaluated, 1, __ATOMIC_SEQ_CST); }

/* The same lines twice: as the fprintf calls the runtime has today, and as migrated call sites. */
static void legacy(void)
{
    fprintf(stderr, "[spu] x %d %s\n", 5, "y");
    fprintf(stderr, "[hle] value 0x%08x\n", 0xBEEFu);
    fprintf(stderr, "plain line %d\n", 1);
    fprintf(stderr, "[p] partial");
    static char big[3001];
    memset(big, 'a', 3000);
    fprintf(stderr, "[long] %s\n", big);
    fprintf(stderr, "[e] error level\n");
    fprintf(stderr, "[w] warn level\n");
    for (int n = 1015; n <= 1030; ++n) fprintf(stderr, "[b] %.*s\n", n, big);
}

static void migrated(void)
{
    PS3_LOG(PS3_LOG_INFO, "spu", "x %d %s\n", 5, "y");
    PS3_LOG(PS3_LOG_INFO, "hle", "value 0x%08x\n", 0xBEEFu);
    PS3_LOG(PS3_LOG_INFO, "", "plain line %d\n", 1);
    PS3_LOG(PS3_LOG_INFO, "p", "partial");
    static char big[3001];
    memset(big, 'a', 3000);
    PS3_LOG(PS3_LOG_INFO, "long", "%s\n", big);
    PS3_LOGE("e", "error level\n");
    PS3_LOGW("w", "warn level\n");
    /* Lines whose total length straddles the 1 KiB stack buffer. */
    for (int n = 1015; n <= 1030; ++n) PS3_LOG(PS3_LOG_INFO, "b", "%.*s\n", n, big);
}

static void say_same(void)  { PS3_LOG(PS3_LOG_INFO, "rep", "same\n"); }
static void say_other(void) { PS3_LOG(PS3_LOG_INFO, "rep", "other\n"); }
static void twin_a(void)    { PS3_LOG(PS3_LOG_INFO, "rep", "twin\n"); }
static void twin_b(void)    { PS3_LOG(PS3_LOG_INFO, "rep", "twin\n"); }

static void levels(void)
{
    PS3_LOGE("lv", "error line\n");
    PS3_LOGW("lv", "warn line\n");
    PS3_LOGI("lv", "info line\n");
    PS3_LOGD("lv", "debug line\n");
}

static void tags(void)
{
    PS3_LOGI("noisy", "from noisy\n");
    PS3_LOGI("quiet", "from quiet\n");
    PS3_LOGI("noisy.sub", "from noisy.sub\n");
}

#define THREADS 8
#define LINES   1500
#define LIMIT_ROUNDS 20000

static int g_ready;

static void probe_yield(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

static void* thread_main(void* arg)
{
    const int id = (int)(intptr_t)arg;
    static const char pad[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char big[1601];
    memset(big, 'z', 1600);
    big[1600] = '\0';
    for (int i = 0; i < LINES; ++i) {
        /* Every seventh line outgrows the stack buffer: the heap path must not interleave either. */
        if (i % 7 == 0) PS3_LOG(PS3_LOG_INFO, "thr", "t%d n%05d %s|\n", id, i, big);
        else PS3_LOG(PS3_LOG_INFO, "thr", "t%d n%05d %.*s|\n", id, i, (i * 13) % (int)(sizeof pad - 1), pad);
        PS3_LOG_N(100, PS3_LOG_INFO, "capped", "capped t%d n%05d\n", id, i);
    }
    return NULL;
}

typedef struct { void* (*fn)(void*); void* arg; } thread_start;

#ifdef _WIN32
static DWORD WINAPI thread_entry(LPVOID p)
{
    const thread_start* s = (const thread_start*)p;
    s->fn(s->arg);
    return 0;
}
#endif

/* A thread nobody joins: the process ends under it (an exit or a stop), and a joinable thread left behind
 * would be a thread leak under TSan. Detached. */
static void spawn(void* (*fn)(void*), void* arg)
{
    static thread_start slots[4];
    static int used;
    thread_start* s = &slots[used++];
    s->fn = fn;
    s->arg = arg;
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, thread_entry, s, 0, NULL);
    if (h) CloseHandle(h);
#else
    pthread_attr_t attr;
    pthread_t t;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
#endif
}

static void* limit_main(void* arg)
{
    const int id = (int)(intptr_t)arg;
    /* Start together, then nothing between the sites: the threads reach the limit at once, which is the race.
     * The argument has a side effect: only a call that was admitted may evaluate it. */
    __atomic_add_fetch(&g_ready, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_ready, __ATOMIC_SEQ_CST) < THREADS) probe_yield();
    for (int i = 0; i < LIMIT_ROUNDS; ++i)
        PS3_LOG_N(100, PS3_LOG_INFO, "hot", "hot t%d n%05d e%d\n", id, i, side_effect());
    return NULL;
}

static void run_threads(void* (*fn)(void*))
{
    probe_thread t[THREADS];
    thread_start start[THREADS];
    for (int i = 0; i < THREADS; ++i) {
        start[i].fn = fn;
        start[i].arg = (void*)(intptr_t)i;
#ifdef _WIN32
        t[i] = CreateThread(NULL, 0, thread_entry, &start[i], 0, NULL);
#else
        pthread_create(&t[i], NULL, fn, start[i].arg);
#endif
    }
    for (int i = 0; i < THREADS; ++i) {
#ifdef _WIN32
        WaitForSingleObject(t[i], INFINITE);
        CloseHandle(t[i]);
#else
        pthread_join(t[i], NULL);
#endif
    }
}

/* In flight at the end of the process: see "inflight-exit". */
static int g_w_inside, g_release, g_w_done;
static ps3_log_site g_probe_site = { "probe", __FILE__, __LINE__, 0u, 0u, 0u, NULL, 0 };

static int worker_arg(void)
{
    __atomic_store_n(&g_w_inside, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_release, __ATOMIC_ACQUIRE)) probe_yield();
    return 42;
}

static void* worker_main(void* arg)
{
    (void)arg;
    PS3_LOG(PS3_LOG_INFO, "inflight", "worker line %d\n", worker_arg());
    __atomic_store_n(&g_w_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* ps3_log_site_admit refuses once the closer has shut the gate: that refusal is the signal. */
static void* releaser_main(void* arg)
{
    (void)arg;
    while (!__atomic_load_n(&g_w_inside, __ATOMIC_ACQUIRE)) probe_yield();
    for (;;) {
        if (!ps3_log_site_admit(&g_probe_site, PS3_LOG_INFO)) break;
        ps3_log_emit_admitted(&g_probe_site, PS3_LOG_INFO, "probe\n");
        probe_yield();
    }
    __atomic_store_n(&g_release, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int g_hook_mode;   /* 0 none; 1 park the flusher at its first tick; 2 hold the log lock inside its flush */
static int g_parked, g_hook_release, g_final_done, g_flush_after_final, g_flusher_exit_seen, g_level_evals;

static ps3_log_level counted_level(ps3_log_level l)
{
    __atomic_add_fetch(&g_level_evals, 1, __ATOMIC_SEQ_CST);
    return l;
}

/* Waits (bounded by iterations, not by time) until *flag is set. */
static void wait_flag(int* flag)
{
    for (long i = 0; i < 20000000L && !__atomic_load_n(flag, __ATOMIC_ACQUIRE); ++i) probe_yield();
}

static void release_worker_and_wait(void)
{
    __atomic_store_n(&g_release, 1, __ATOMIC_RELEASE);
    wait_flag(&g_w_done);   /* the worker has finished its attempt to write: dropped, or not */
}

static void late_handler(void) { release_worker_and_wait(); }

static void late_mirror(const char* bytes, size_t n)
{
    ps3_log_mirror(bytes, n);       /* the log's own mirror ran: the banner is on stderr and in the file */
    release_worker_and_wait();
}

static void test_hook(int phase)
{
    if (phase == PS3_LOG_TEST_FLUSHER_TICK) {
        if (__atomic_load_n(&g_hook_mode, __ATOMIC_ACQUIRE) == 1 && !__atomic_exchange_n(&g_parked, 1, __ATOMIC_ACQ_REL))
            while (!__atomic_load_n(&g_hook_release, __ATOMIC_ACQUIRE)) probe_yield();
    } else if (phase == PS3_LOG_TEST_FLUSHER_FLUSH) {
        if (__atomic_load_n(&g_hook_mode, __ATOMIC_ACQUIRE) == 2) {
            __atomic_store_n(&g_parked, 1, __ATOMIC_RELEASE);
            for (;;) probe_yield();   /* never returns: the log lock stays held */
        }
        if (__atomic_load_n(&g_final_done, __ATOMIC_ACQUIRE)) __atomic_add_fetch(&g_flush_after_final, 1, __ATOMIC_SEQ_CST);
    } else if (phase == PS3_LOG_TEST_FLUSHER_EXIT) {
        __atomic_store_n(&g_flusher_exit_seen, 1, __ATOMIC_RELEASE);
    }
}

static void flusher_handler(void)
{
    __atomic_store_n(&g_final_done, 1, __ATOMIC_RELEASE);   /* every handler of the log has run */
    __atomic_store_n(&g_hook_release, 1, __ATOMIC_RELEASE);
    wait_flag(&g_flusher_exit_seen);
    printf("flush_after_final=%d exit_seen=%d\n", __atomic_load_n(&g_flush_after_final, __ATOMIC_SEQ_CST),
           __atomic_load_n(&g_flusher_exit_seen, __ATOMIC_SEQ_CST));
    fflush(stdout);
}

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    const char* sc = argv[1];

    if (!strcmp(sc, "legacy"))           { ps3_config_load(NULL); legacy(); return 0; }
    if (!strcmp(sc, "migrated"))         { ps3_config_load(NULL); migrated(); return 0; }
    if (!strcmp(sc, "migrated-noconfig")) { migrated(); return 0; }

    if (!strcmp(sc, "limit")) {
        ps3_config_load(NULL);
        for (int i = 0; i < 10; ++i) PS3_LOG_N(3, PS3_LOG_INFO, "cap", "capped line %d\n", i);
        /* A silenced line does not evaluate its arguments: PS3_LOG_FIRST's guard never did. */
        for (int i = 0; i < 10; ++i) PS3_LOG_N(4, PS3_LOG_INFO, "lazy", "lazy %d\n", side_effect());
        printf("evaluated=%d\n", g_evaluated);
        return 0;
    }
    if (!strcmp(sc, "config-only")) { ps3_config_load(NULL); return 0; }
    if (!strcmp(sc, "levels"))      { ps3_config_load(NULL); levels(); return 0; }
    if (!strcmp(sc, "tags"))        { ps3_config_load(NULL); tags(); return 0; }

    if (!strcmp(sc, "collapse")) {
        ps3_config_load(NULL);
        for (int i = 0; i < 5; ++i) say_same();
        say_other();
        for (int i = 0; i < 3; ++i) say_same();
        for (int i = 0; i < 4; ++i) { twin_a(); twin_b(); }   /* same text, two sites: nothing to collapse */
        for (int i = 0; i < 2; ++i) say_same();               /* a run that is still open at exit */
        return 0;
    }

    if (!strcmp(sc, "pre")) {
        /* Before the configuration is loaded: stderr as ever, the file once it opens. */
        for (int i = 0; i < 5; ++i) PS3_LOG(PS3_LOG_INFO, "early", "before configure %d\n", i);
        ps3_config_load(NULL);
        PS3_LOG(PS3_LOG_INFO, "late", "after configure\n");
        return 0;
    }
    if (!strcmp(sc, "pre-overflow")) {
        for (int i = 0; i < 2000; ++i)
            PS3_LOG(PS3_LOG_INFO, "early", "pre %04d ................................................................\n", i);
        ps3_config_load(NULL);
        return 0;
    }
    if (!strcmp(sc, "cap")) {
        ps3_config_load(NULL);
        char big[801];
        memset(big, 'c', 800);
        big[800] = '\0';
        for (int i = 0; i < 3000; ++i) PS3_LOG(PS3_LOG_INFO, "fill", "%05d %s\n", i, big);
        return 0;
    }
    if (!strcmp(sc, "threads"))       { ps3_config_load(NULL); run_threads(thread_main); return 0; }
    if (!strcmp(sc, "threads-limit")) {
        ps3_config_load(NULL);
        run_threads(limit_main);
        printf("evaluated=%d\n", __atomic_load_n(&g_evaluated, __ATOMIC_SEQ_CST));
        return 0;
    }

    if (!strcmp(sc, "quick-exit")) {
        /* How every guest exit ends (ps3_host_exit): quick_exit runs at_quick_exit handlers, not atexit ones. */
        ps3_config_load(NULL);
        for (int i = 0; i < 5; ++i) PS3_LOG_N(2, PS3_LOG_INFO, "capped", "capped %d\n", i);
        for (int i = 0; i < 3; ++i) say_same();
        ps3_host_exit(0);
    }
    /* A worker is inside a logging operation (blocked evaluating an argument) when the process ends. The
     * releaser frees it only once it sees the gate shut, so the line can reach the file only if the closer
     * waits for operations already admitted. No sleeps: the order is the gate's own. */
    if (!strcmp(sc, "inflight-exit") || !strcmp(sc, "inflight-stop")) {
        ps3_config_load(NULL);
        PS3_LOG(PS3_LOG_INFO, "before", "a line before the exit\n");
        for (int i = 0; i < 5; ++i) PS3_LOG_N(2, PS3_LOG_INFO, "capped", "capped %d\n", i);
        spawn(worker_main, NULL);
        spawn(releaser_main, NULL);
        while (!__atomic_load_n(&g_w_inside, __ATOMIC_ACQUIRE)) probe_yield();
        if (!strcmp(sc, "inflight-exit")) ps3_host_exit(0);
        ps3_stop(PS3_STOP_CONFIG_INVALID, "a probe stop", "none", NULL, 0, NULL);
    }

    /* An operation admitted before shutdown that is released only after the drain gave up (inflight-* releases
     * it as soon as the gate shuts, which is inside the drain). Nothing may follow the summary or the banner:
     * the last handler (after the log's own) releases the worker and waits until it has tried to write. */
    if (!strcmp(sc, "late-exit") || !strcmp(sc, "late-stop")) {
        if (!strcmp(sc, "late-exit")) at_quick_exit(late_handler);   /* registered first: runs after the log's */
        ps3_config_load(NULL);
        if (!strcmp(sc, "late-stop")) ps3_stop_set_mirror(ps3_log_quiesce, late_mirror);
        PS3_LOG(PS3_LOG_INFO, "before", "a line before the exit\n");
        for (int i = 0; i < 5; ++i) PS3_LOG_N(2, PS3_LOG_INFO, "capped", "capped %d\n", i);
        spawn(worker_main, NULL);
        while (!__atomic_load_n(&g_w_inside, __ATOMIC_ACQUIRE)) probe_yield();
        if (!strcmp(sc, "late-exit")) ps3_host_exit(0);
        ps3_stop(PS3_STOP_CONFIG_INVALID, "a probe stop", "none", NULL, 0, NULL);
    }

    /* The log's flusher is parked at a tick when the process ends. The last handler releases it and waits
     * for it to leave: it must not flush the file after the final flush. */
    if (!strcmp(sc, "flusher-exit") || !strcmp(sc, "flusher-quick")) {
        g_hook_mode = 1;
        ps3_log_set_test_hook(test_hook);
        if (!strcmp(sc, "flusher-exit")) atexit(flusher_handler); else at_quick_exit(flusher_handler);
        ps3_config_load(NULL);
        PS3_LOG(PS3_LOG_INFO, "before", "a line before the exit\n");
        while (!__atomic_load_n(&g_parked, __ATOMIC_ACQUIRE)) probe_yield();
        if (!strcmp(sc, "flusher-exit")) return 0;
        ps3_host_exit(0);
    }

    /* A flush that never returns, holding the log lock (a stalled disk): the ending must still be bounded. */
    if (!strcmp(sc, "stall-exit") || !strcmp(sc, "stall-stop")) {
        ps3_log_set_test_hook(test_hook);
        ps3_config_load(NULL);
        PS3_LOG(PS3_LOG_INFO, "before", "a line before the stall\n");
        __atomic_store_n(&g_hook_mode, 2, __ATOMIC_RELEASE);   /* only now: the line above must not meet the held lock */
        while (!__atomic_load_n(&g_parked, __ATOMIC_ACQUIRE)) probe_yield();
        if (!strcmp(sc, "stall-exit")) ps3_host_exit(0);
        ps3_stop(PS3_STOP_CONFIG_INVALID, "a probe stop", "none", NULL, 0, NULL);
    }

    /* The level expression runs once, before admission: the filtered level is the printed one. */
    if (!strcmp(sc, "level-once")) {
        ps3_config_load(NULL);
        for (int i = 0; i < 3; ++i) PS3_LOG(counted_level(PS3_LOG_INFO), "lvl", "info line\n");
        for (int i = 0; i < 3; ++i) PS3_LOG(counted_level(PS3_LOG_DEBUG), "lvl", "debug line\n");
        printf("evals=%d\n", __atomic_load_n(&g_level_evals, __ATOMIC_SEQ_CST));
        return 0;
    }

    /* Configured, a line logged, then nothing: the test kills the process once the line is in the file. */
    if (!strcmp(sc, "kill-wait")) {
        ps3_config_load(NULL);
        PS3_LOG(PS3_LOG_INFO, "startup", "line before ppu_run\n");
        printf("ready\n");
        fflush(stdout);
        (void)getchar();
        return 0;
    }

    if (!strcmp(sc, "stop")) {
        ps3_config_load(NULL);
        PS3_LOG(PS3_LOG_INFO, "before", "a line before the stop\n");
        for (int i = 0; i < 5; ++i) PS3_LOG_N(2, PS3_LOG_INFO, "capped", "capped %d\n", i);
        for (int i = 0; i < 3; ++i) say_same();
        ps3_stop(PS3_STOP_CONFIG_INVALID, "a probe stop", "none", NULL, 0, NULL);
    }
    return 2;
}

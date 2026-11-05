/* sys_timer_usleep on macOS waits on a NOTE_CRITICAL kevent timer (ppu.precise_usleep) instead of nanosleep, whose
 * timer leeway oversleeps by a quarter of the request or more. Contract held here, none of it by a timing threshold
 * except `timing`, which is a stress tier measurement:
 *
 *   state       a precise sleep leaves the calling thread as it was: no time-constraint policy, the same QoS class,
 *               relative priority and sched priority, for threads set up through the runtime's own priority path
 *               (SetThreadPriority) and through plain QoS (what a pthread_create'd guest thread has). The state is
 *               read after the sleep, by the sleeping thread, and during it, by another thread.
 *   path        the counter sys_timer_usleep_precise_count() says which path a sleep took: one tick per sleep of
 *               20 us or more with the option on, none below that, none with it off.
 *   fallback    a host that refuses the timer (no fd for the kqueue) still sleeps the full time, names it once on
 *               stderr, does not count the sleep as precise, and the next sleep is precise again.
 *   fds         64 threads that sleep once each leave the process's open fd count where it was.
 *   concurrent  N threads sleep random lengths for a while; after every sleep each checks its own state and that it
 *               did not wake early.
 *   timing      N sleeps of 10 ms, overshoot printed; with the option on its p90 must stay under a bound.
 *
 * Usage: test_usleep_precise <state|fallback|fds|concurrent|timing> <on|off> [seconds-or-count]
 * The mode's second word is what the run expects of ppu.precise_usleep; the environment (ctest) sets the option to
 * match, so a changed default fails the "on" run. */
#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#endif
#include "sys_timer.h"
#include "platform/win32_compat.h"

/* The runtime defines vm_base in ppu_loader.cpp, which a host without a lifted title does not link. Nothing here
 * touches guest memory: sys_timer_usleep reads only its register argument. */
uint8_t* vm_base = NULL;

static _Atomic int g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { atomic_fetch_add(&g_fails, 1); \
    fprintf(stderr, "FAIL %s:%d: %s ", __FILE__, __LINE__, #cond); fprintf(stderr, "" __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int  g_expect_on;        /* the run expects ppu.precise_usleep on */
static int  g_precise_host;     /* a host with the precise path at all */

/* ---- thread state ------------------------------------------------------------------------------------------- */

typedef struct {
    int      tc;          /* in THREAD_TIME_CONSTRAINT_POLICY */
    unsigned qos;         /* pthread_get_qos_class_np */
    int      rel;
    int      sched_prio;
} tstate;

static tstate snap(pthread_t t)
{
    tstate s; memset(&s, 0, sizeof s);
    int pol = 0; struct sched_param sp; memset(&sp, 0, sizeof sp);
    if (pthread_getschedparam(t, &pol, &sp) == 0) s.sched_prio = sp.sched_priority;
#ifdef __APPLE__
    qos_class_t q = QOS_CLASS_UNSPECIFIED; int rel = 0;
    if (pthread_get_qos_class_np(t, &q, &rel) == 0) { s.qos = (unsigned)q; s.rel = rel; }
    thread_time_constraint_policy_data_t p; mach_msg_type_number_t n = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t is_default = FALSE;
    if (thread_policy_get(pthread_mach_thread_np(t), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, &n,
                          &is_default) == KERN_SUCCESS)
        s.tc = !is_default;
#endif
    return s;
}

static int same(const tstate* a, const tstate* b)
{
    return a->tc == b->tc && a->qos == b->qos && a->rel == b->rel && a->sched_prio == b->sched_prio;
}

static void show(const char* what, const tstate* s)
{
    fprintf(stderr, "  %s: tc=%d qos=0x%02x rel=%d sched_prio=%d\n", what, s->tc, s->qos, s->rel, s->sched_prio);
}

/* The ways a guest thread comes by its host scheduling state. The first four are the runtime's own path (the
 * SetThreadPriority shim the audio thread and the PPU thread create use); the rest are plain QoS, which a
 * pthread_create'd guest thread inherits from its creator. */
typedef struct { const char* name; int win_level; int qos; int qos_rel; } setup_t;
static const setup_t k_setups[] = {
    { "SetThreadPriority(TIME_CRITICAL)", THREAD_PRIORITY_TIME_CRITICAL, 0, 0 },
    { "SetThreadPriority(ABOVE_NORMAL)",  THREAD_PRIORITY_ABOVE_NORMAL,  0, 0 },
    { "SetThreadPriority(BELOW_NORMAL)",  THREAD_PRIORITY_BELOW_NORMAL,  0, 0 },
    { "SetThreadPriority(IDLE)",          THREAD_PRIORITY_IDLE,          0, 0 },
#ifdef __APPLE__
    { "QoS USER_INTERACTIVE",             THREAD_PRIORITY_ERROR_RETURN, QOS_CLASS_USER_INTERACTIVE, 0 },
    { "QoS USER_INITIATED",               THREAD_PRIORITY_ERROR_RETURN, QOS_CLASS_USER_INITIATED,   0 },
    { "QoS UTILITY",                      THREAD_PRIORITY_ERROR_RETURN, QOS_CLASS_UTILITY,          0 },
    { "QoS BACKGROUND",                   THREAD_PRIORITY_ERROR_RETURN, QOS_CLASS_BACKGROUND,       0 },
    { "QoS USER_INITIATED rel -5",        THREAD_PRIORITY_ERROR_RETURN, QOS_CLASS_USER_INITIATED,  -5 },
#endif
};
#define N_SETUPS ((int)(sizeof k_setups / sizeof k_setups[0]))

/* Applies setup `i` to the calling thread. */
static void apply_setup(int i)
{
    const setup_t* s = &k_setups[i];
    if (s->win_level != THREAD_PRIORITY_ERROR_RETURN) {
        CHECK(SetThreadPriority(GetCurrentThread(), s->win_level), "%s", s->name);
        return;
    }
#ifdef __APPLE__
    CHECK(pthread_set_qos_class_self_np((qos_class_t)s->qos, s->qos_rel) == 0, "%s", s->name);
#endif
}

/* ---- sleeping through the handler --------------------------------------------------------------------------- */

static uint64_t g_tid_seq = 0x100;   /* contexts in no thread-table slot: ppu_thread_note_block ignores them */

typedef struct { ppu_context ctx; } sleeper;

static void sleeper_init(sleeper* s)
{
    memset(&s->ctx, 0, sizeof s->ctx);
    s->ctx.thread_id = __atomic_add_fetch(&g_tid_seq, 1, __ATOMIC_RELAXED);
}

/* Sleeps `usec` through sys_timer_usleep; returns the nanoseconds it took. */
static uint64_t do_usleep(sleeper* s, uint64_t usec)
{
    s->ctx.gpr[3] = usec;
    const uint64_t t0 = host_monotonic_ns();
    const int64_t r = sys_timer_usleep(&s->ctx);
    const uint64_t dt = host_monotonic_ns() - t0;
    CHECK(r == 0, "sys_timer_usleep(%llu) = %lld", (unsigned long long)usec, (long long)r);
    /* nanosleep and the timer never return early without a signal; this is a lower bound, so no host speed
     * can break it. */
    CHECK(dt >= usec * 1000u, "usleep(%llu us) took %llu ns", (unsigned long long)usec, (unsigned long long)dt);
    return dt;
}

/* The first 60 calls of sys_timer_usleep print a [WAIT] line, counted by a plain static, and the
 * PS3_POLLTOP / PS3_WAIT_OBJ / POLLSITE switches read their variables into plain statics on the first call. Use the
 * counter up once, with stderr silenced, so the threads below only ever read those statics: the test is for the
 * sleep, and TSan reports nothing for state that no thread writes any more. */
static void warm_up(void)
{
    const int saved = dup(2);
    const int null = open("/dev/null", O_WRONLY);
    if (saved >= 0 && null >= 0) dup2(null, 2);
    sleeper s; sleeper_init(&s);
    for (int i = 0; i < 64; i++) { s.ctx.gpr[3] = 1; (void)sys_timer_usleep(&s.ctx); }
    if (saved >= 0) { dup2(saved, 2); close(saved); }
    if (null >= 0) close(null);
}

/* ---- state / path ------------------------------------------------------------------------------------------- */

typedef struct {
    int           setup;
    pthread_t     thread;
    _Atomic int   sleeping;     /* 1 from just before the long sleep until it returns */
    _Atomic int   done;
    tstate        before, after;
} state_job;

static void* state_thread(void* arg)
{
    state_job* j = arg;
    apply_setup(j->setup);
    j->before = snap(pthread_self());
    sleeper s; sleeper_init(&s);

    uint64_t before_count = sys_timer_usleep_precise_count();
    do_usleep(&s, 0);       /* nothing to sleep: no path */
    do_usleep(&s, 10);      /* below the threshold: nanosleep */
    CHECK(sys_timer_usleep_precise_count() == before_count, "a sub-threshold sleep took the precise path");
    tstate mid = snap(pthread_self());
    CHECK(same(&mid, &j->before), "after a nanosleep, setup %d", j->setup);

    atomic_store(&j->sleeping, 1);
    do_usleep(&s, 30000);   /* the observer reads the thread while this runs */
    atomic_store(&j->sleeping, 0);
    j->after = snap(pthread_self());
    const uint64_t delta = sys_timer_usleep_precise_count() - before_count;
    CHECK(delta == (uint64_t)(g_precise_host && g_expect_on ? 1 : 0),
          "precise sleeps counted: %llu (host precise=%d, expected on=%d)", (unsigned long long)delta,
          g_precise_host, g_expect_on);
    for (int k = 0; k < 3; k++) do_usleep(&s, 200u << k);
    tstate again = snap(pthread_self());
    CHECK(same(&again, &j->before), "after 3 more sleeps, setup %d", j->setup);
    atomic_store(&j->done, 1);
    return NULL;
}

static int run_state(void)
{
    tstate first, last;
    memset(&first, 0, sizeof first); memset(&last, 0, sizeof last);
    for (int i = 0; i < N_SETUPS; i++) {
        state_job j; memset(&j, 0, sizeof j); j.setup = i;
        pthread_t t;
        if (pthread_create(&t, NULL, state_thread, &j) != 0) { CHECK(0, "pthread_create"); continue; }
        j.thread = t;
        while (!atomic_load(&j.sleeping) && !atomic_load(&j.done)) sched_yield();
        /* Another thread's view while the sleeper waits. State that did not change cannot differ whenever this
         * read lands, so where it lands is no matter. */
        tstate mid = snap(t);
        pthread_join(t, NULL);
        CHECK(same(&mid, &j.before), "state read mid-sleep, setup %d (%s)", i, k_setups[i].name);
        CHECK(same(&j.after, &j.before), "state after sleep, setup %d (%s)", i, k_setups[i].name);
        CHECK(!j.after.tc, "left in the time-constraint policy, setup %d", i);
        if (!same(&mid, &j.before) || !same(&j.after, &j.before)) {
            show("before", &j.before); show("mid   ", &mid); show("after ", &j.after);
        }
        if (i == 0) first = j.before;
        if (i == 3) last = j.before;
    }
    /* The runtime's priority path reaches the host (the audio thread depends on it); if two levels read back
     * the same, the equality above would be vacuous for the sched priority. */
#ifdef __APPLE__
    CHECK(first.sched_prio != last.sched_prio, "TIME_CRITICAL and IDLE read back the same sched priority (%d)",
          first.sched_prio);
#else
    (void)first; (void)last;
#endif
    return 0;
}

/* ---- fallback ----------------------------------------------------------------------------------------------- */

static int count_lines_with(FILE* f, const char* needle)
{
    char line[1024]; int n = 0;
    rewind(f);
    while (fgets(line, sizeof line, f)) if (strstr(line, needle)) n++;
    return n;
}

static int run_fallback(void)
{
    if (!g_precise_host || !g_expect_on) { fprintf(stderr, "fallback: nothing to refuse on this configuration\n"); return 0; }
    sleeper s; sleeper_init(&s);
    FILE* log = tmpfile();
    CHECK(log != NULL, "tmpfile");
    if (!log) return 1;
    const int saved = dup(2);
    dup2(fileno(log), 2);

    /* No descriptor above the lowest free one can be made: kqueue() fails with EMFILE. */
    struct rlimit old;
    CHECK(getrlimit(RLIMIT_NOFILE, &old) == 0, "getrlimit");
    const int probe = dup(0);
    struct rlimit tight = old; tight.rlim_cur = (rlim_t)probe;
    close(probe);
    CHECK(setrlimit(RLIMIT_NOFILE, &tight) == 0, "setrlimit: %s", strerror(errno));

    const uint64_t c0 = sys_timer_usleep_precise_count();
    do_usleep(&s, 5000);
    do_usleep(&s, 5000);
    const uint64_t c1 = sys_timer_usleep_precise_count();
    setrlimit(RLIMIT_NOFILE, &old);
    do_usleep(&s, 5000);
    const uint64_t c2 = sys_timer_usleep_precise_count();

    fflush(stderr);
    dup2(saved, 2); close(saved);
    const int named = count_lines_with(log, "precise-usleep-fallback");
    fclose(log);
    CHECK(c1 == c0, "a sleep with no fd for the kqueue was counted as precise (%llu -> %llu)",
          (unsigned long long)c0, (unsigned long long)c1);
    CHECK(c2 == c0 + 1, "the sleep after the refusal was not precise (%llu)", (unsigned long long)c2);
    CHECK(named == 1, "the fallback was named %d times, not once", named);
    return 0;
}

/* ---- fds ---------------------------------------------------------------------------------------------------- */

static int open_fds(void)
{
    int n = 0;
    for (int fd = 0; fd < 4096; fd++) if (fcntl(fd, F_GETFD) != -1) n++;
    return n;
}

static void* one_sleep(void* arg)
{
    (void)arg;
    sleeper s; sleeper_init(&s);
    do_usleep(&s, 1500);
    return NULL;
}

static int run_fds(void)
{
    const int before = open_fds();
    const uint64_t c0 = sys_timer_usleep_precise_count();
    enum { N = 64 };
    pthread_t t[N];
    for (int i = 0; i < N; i++) CHECK(pthread_create(&t[i], NULL, one_sleep, NULL) == 0, "pthread_create");
    for (int i = 0; i < N; i++) pthread_join(t[i], NULL);
    const int after = open_fds();
    CHECK(before == after, "open fds %d before, %d after 64 threads that slept once", before, after);
    CHECK(sys_timer_usleep_precise_count() - c0 == (uint64_t)(g_precise_host && g_expect_on ? N : 0),
          "precise sleeps counted: %llu", (unsigned long long)(sys_timer_usleep_precise_count() - c0));
    return 0;
}

/* ---- concurrent --------------------------------------------------------------------------------------------- */

typedef struct { int idx; double seconds; _Atomic uint64_t sleeps; } conc_job;

static uint64_t xorshift(uint64_t* s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }

static void* conc_thread(void* arg)
{
    conc_job* j = arg;
    apply_setup(j->idx % N_SETUPS);
    const tstate want = snap(pthread_self());
    sleeper s; sleeper_init(&s);
    uint64_t rng = 0x9E3779B97F4A7C15ull * (uint64_t)(j->idx + 1);
    const uint64_t end = host_monotonic_ns() + (uint64_t)(j->seconds * 1e9);
    while (host_monotonic_ns() < end) {
        /* 0 .. 30 ms, a third of the draws under 100 us where the path changes at 20 us. */
        const uint64_t r = xorshift(&rng);
        const uint64_t usec = (r % 3 == 0) ? (r >> 8) % 100 : (r >> 8) % 30000;
        do_usleep(&s, usec);
        const tstate now = snap(pthread_self());
        if (!same(&now, &want)) {
            CHECK(0, "thread %d: state changed by a %llu us sleep", j->idx, (unsigned long long)usec);
            show("want", &want); show("now ", &now);
            break;
        }
        atomic_fetch_add(&j->sleeps, 1);
    }
    return NULL;
}

static int run_concurrent(double seconds)
{
    enum { N = 16 };
    pthread_t t[N]; conc_job job[N];
    for (int i = 0; i < N; i++) { job[i].idx = i; job[i].seconds = seconds; atomic_init(&job[i].sleeps, 0); }
    for (int i = 0; i < N; i++) CHECK(pthread_create(&t[i], NULL, conc_thread, &job[i]) == 0, "pthread_create");
    uint64_t total = 0;
    for (int i = 0; i < N; i++) { pthread_join(t[i], NULL); total += atomic_load(&job[i].sleeps); }
    fprintf(stderr, "concurrent: %d threads, %.1f s, %llu sleeps\n", N, seconds, (unsigned long long)total);
    CHECK(total > 0, "no sleeps ran");
    return 0;
}

/* ---- timing ------------------------------------------------------------------------------------------------- */

static int cmp_u64(const void* a, const void* b)
{
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

/* The bound sits between the two paths. Pooled over 800 sleeps on the measuring host (scratch/m4/usleep/MEASURE.md in
 * the recon tree), the overshoot of a 10 ms sleep is p90 2.52 ms with nanosleep and p90 0.055 ms with the precise
 * path, 0.049 ms with 8 busy threads (0.18 ms at 33 ms). 1 ms clears that by 5x and stays under half of the leeway the
 * option removes, so a run that falls back to nanosleep fails it. The p99 under load is not bounded: it reaches the
 * leeway's size on a saturated host. */
#define TIMING_P90_BOUND_NS 1000000u

static int run_timing(int n)
{
    uint64_t* over = malloc((size_t)n * sizeof *over);
    sleeper s; sleeper_init(&s);
    for (int i = 0; i < n; i++) over[i] = do_usleep(&s, 10000) - 10000u * 1000u;
    qsort(over, (size_t)n, sizeof *over, cmp_u64);
    fprintf(stderr, "timing: 10 ms x %d, precise_usleep=%s: overshoot p50 %.3f ms  p90 %.3f ms  p99 %.3f ms  max %.3f ms\n",
            n, g_expect_on ? "on" : "off", over[n / 2] / 1e6, over[n * 9 / 10] / 1e6, over[n * 99 / 100] / 1e6,
            over[n - 1] / 1e6);
    if (g_precise_host && g_expect_on)
        CHECK(over[n * 9 / 10] < TIMING_P90_BOUND_NS, "p90 overshoot %.3f ms is not under %.1f ms",
              over[n * 9 / 10] / 1e6, TIMING_P90_BOUND_NS / 1e6);
    free(over);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off"))) {
        fprintf(stderr, "usage: %s <state|fallback|fds|concurrent|timing> <on|off> [seconds|count]\n", argv[0]);
        return 2;
    }
    g_expect_on = !strcmp(argv[2], "on");
#ifdef __APPLE__
    g_precise_host = 1;
#endif
    CHECK(ps3_cfg_ppu_precise_usleep() == (bool)g_expect_on, "ppu.precise_usleep is %d, the run expects %d",
          (int)ps3_cfg_ppu_precise_usleep(), g_expect_on);
    warm_up();
    const char* mode = argv[1];
    const double arg = argc > 3 ? atof(argv[3]) : 0;
    if      (!strcmp(mode, "state"))      run_state();
    else if (!strcmp(mode, "fallback"))   run_fallback();
    else if (!strcmp(mode, "fds"))        run_fds();
    else if (!strcmp(mode, "concurrent")) run_concurrent(arg > 0 ? arg : 10.0);
    else if (!strcmp(mode, "timing"))     run_timing(arg > 0 ? (int)arg : 200);
    else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }
    const int fails = atomic_load(&g_fails);
    fprintf(stderr, "%s %s %s: %s\n", argv[0], mode, argv[2], fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}

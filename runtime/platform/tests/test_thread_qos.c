/*
 * ps3recomp - SetThreadPriority and the QoS class it leaves a thread with
 *
 * On Apple Silicon the QoS class decides P-core vs E-core, and a thread whose
 * sched params were ever set (pthread_setschedparam, even to the value it
 * already has) loses its class and can never take one again. This test holds
 * the shim to the class it promises for each THREAD_PRIORITY_* level, for a
 * thread that sets its own level, for one that is given a level before
 * ResumeThread, and for the two requests that cannot be honoured (a running
 * other thread, a thread whose class is locked), which must fail and say so
 * once on stderr.
 *
 * Elsewhere there is no class to read; the platform-neutral part (the level
 * round-trips through GetThreadPriority) still runs and the rest skips.
 *
 * Build (any POSIX host):
 *   cc -std=gnu17 -Wall -Wextra -I runtime/platform \
 *      runtime/platform/tests/test_thread_qos.c runtime/platform/win32_compat.c -lpthread
 *
 * Exit status is the number of failed checks.
 */
#include "../win32_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#  include <pthread/qos.h>
#endif

/* The waits have no bound of their own: a thread at the IDLE level runs at the
 * background QoS class, which a saturated host (the full suite at -j8) can
 * starve for longer than 10 s. ctest's TIMEOUT bounds the run instead. */
#define WAIT_MS INFINITE

static int g_pass, g_fail;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKF(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

typedef struct { const char* name; int level; unsigned qos; int rel; } level_t;
#ifdef __APPLE__
/* The contract, written out: a level keeps its class however it was set, and
 * levels that share a class stay ordered by the relative priority. */
static const level_t k_levels[] = {
    { "TIME_CRITICAL", THREAD_PRIORITY_TIME_CRITICAL, QOS_CLASS_USER_INTERACTIVE,  0 },
    { "HIGHEST",       THREAD_PRIORITY_HIGHEST,       QOS_CLASS_USER_INTERACTIVE, -1 },
    { "ABOVE_NORMAL",  THREAD_PRIORITY_ABOVE_NORMAL,  QOS_CLASS_USER_INTERACTIVE, -2 },
    { "NORMAL",        THREAD_PRIORITY_NORMAL,        QOS_CLASS_USER_INITIATED,    0 },
    { "BELOW_NORMAL",  THREAD_PRIORITY_BELOW_NORMAL,  QOS_CLASS_UTILITY,           0 },
    { "LOWEST",        THREAD_PRIORITY_LOWEST,        QOS_CLASS_UTILITY,          -1 },
    { "IDLE",          THREAD_PRIORITY_IDLE,          QOS_CLASS_BACKGROUND,        0 },
};
#else
static const level_t k_levels[] = {
    { "TIME_CRITICAL", THREAD_PRIORITY_TIME_CRITICAL, 0, 0 },
    { "NORMAL",        THREAD_PRIORITY_NORMAL,        0, 0 },
    { "IDLE",          THREAD_PRIORITY_IDLE,          0, 0 },
};
#endif
#define N_LEVELS ((int)(sizeof k_levels / sizeof k_levels[0]))

typedef struct { unsigned qos; int rel; } qos_state;

static qos_state read_qos(void)
{
    qos_state s = { 0, 0 };
#ifdef __APPLE__
    qos_class_t q = QOS_CLASS_UNSPECIFIED; int rel = 0;
    if (pthread_get_qos_class_np(pthread_self(), &q, &rel) == 0) { s.qos = (unsigned)q; s.rel = rel; }
#endif
    return s;
}

/* ---- a thread sets its own level ---------------------------------------- */
typedef struct { int level; BOOL ok; qos_state after; int prio; } self_job;

static DWORD WINAPI self_worker(LPVOID arg)
{
    self_job* j = (self_job*)arg;
    j->ok    = SetThreadPriority(GetCurrentThread(), j->level);
    j->after = read_qos();
    j->prio  = GetThreadPriority(GetCurrentThread());
    return 0;
}

static void test_self(void)
{
    for (int i = 0; i < N_LEVELS; i++) {
        self_job j = { k_levels[i].level, FALSE, { 0, 0 }, 0 };
        HANDLE h = CreateThread(NULL, 0, self_worker, &j, 0, NULL);
        CHECK(h != NULL);
        CHECK(WaitForSingleObject(h, WAIT_MS) == WAIT_OBJECT_0);
        CloseHandle(h);
        CHECKF(j.ok, "SetThreadPriority(self, %s) succeeds", k_levels[i].name);
        CHECKF(j.prio == k_levels[i].level, "%s: GetThreadPriority %d", k_levels[i].name, j.prio);
#ifdef __APPLE__
        CHECKF(j.after.qos == k_levels[i].qos && j.after.rel == k_levels[i].rel,
               "%s: self ends with qos 0x%02x rel %d, wants 0x%02x rel %d", k_levels[i].name,
               j.after.qos, j.after.rel, k_levels[i].qos, k_levels[i].rel);
#endif
    }
}

/* ---- a level given before ResumeThread ---------------------------------- */
typedef struct { qos_state start; int prio; } susp_job;

static DWORD WINAPI susp_worker(LPVOID arg)
{
    susp_job* j = (susp_job*)arg;
    j->start = read_qos();          /* first thing the thread does: what it was started with */
    j->prio  = GetThreadPriority(GetCurrentThread());
    return 0;
}

static void test_suspended(void)
{
    for (int i = 0; i < N_LEVELS; i++) {
        /* Given a level, then another before the resume: the last one is what it starts with. */
        const int other = (i + 1) % N_LEVELS;
        susp_job j = { { 0, 0 }, 0 };
        HANDLE h = CreateThread(NULL, 0, susp_worker, &j, CREATE_SUSPENDED, NULL);
        CHECK(h != NULL);
        CHECK(SetThreadPriority(h, k_levels[other].level));
        CHECK(SetThreadPriority(h, k_levels[i].level));
        CHECK(ResumeThread(h) == 1);
        CHECK(WaitForSingleObject(h, WAIT_MS) == WAIT_OBJECT_0);
        CloseHandle(h);
        CHECKF(j.prio == k_levels[i].level, "%s: GetThreadPriority %d", k_levels[i].name, j.prio);
#ifdef __APPLE__
        CHECKF(j.start.qos == k_levels[i].qos && j.start.rel == k_levels[i].rel,
               "%s: suspended thread starts with qos 0x%02x rel %d, wants 0x%02x rel %d", k_levels[i].name,
               j.start.qos, j.start.rel, k_levels[i].qos, k_levels[i].rel);
#endif
    }
}

/* A thread nobody gave a level keeps what it inherited. */
static void test_untouched(const qos_state* creator)
{
    susp_job j = { { 0, 0 }, 0 };
    HANDLE h = CreateThread(NULL, 0, susp_worker, &j, CREATE_SUSPENDED, NULL);
    CHECK(h != NULL);
    CHECK(ResumeThread(h) == 1);
    CHECK(WaitForSingleObject(h, WAIT_MS) == WAIT_OBJECT_0);
    CloseHandle(h);
    CHECK(j.prio == THREAD_PRIORITY_NORMAL);
    CHECKF(j.start.qos == creator->qos && j.start.rel == creator->rel,
           "untouched thread has qos 0x%02x rel %d, its creator had 0x%02x rel %d",
           j.start.qos, j.start.rel, creator->qos, creator->rel);
}

#ifdef __APPLE__
/* ---- requests that cannot be honoured ----------------------------------- */
typedef struct { HANDLE ready, go; qos_state before, after; } run_job;

static DWORD WINAPI parked_worker(LPVOID arg)
{
    run_job* j = (run_job*)arg;
    j->before = read_qos();
    SetEvent(j->ready);
    WaitForSingleObject(j->go, WAIT_MS);
    j->after = read_qos();
    return 0;
}

static void test_running_other(void)
{
    run_job j = { CreateEventW(NULL, TRUE, FALSE, NULL), CreateEventW(NULL, TRUE, FALSE, NULL), { 0, 0 }, { 0, 0 } };
    HANDLE h = CreateThread(NULL, 0, parked_worker, &j, 0, NULL);
    CHECK(h != NULL);
    CHECK(WaitForSingleObject(j.ready, WAIT_MS) == WAIT_OBJECT_0);

    /* Its class cannot be set from here, and the sched-param route would cost it the class. */
    SetLastError(0);
    CHECK(!SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL));
    CHECK(GetLastError() == ERROR_NOT_SUPPORTED);
    CHECK(!SetThreadPriority(h, THREAD_PRIORITY_IDLE));     /* a second refusal, still one line */
    CHECK(GetThreadPriority(h) == THREAD_PRIORITY_NORMAL);  /* the record says what the thread has */

    SetEvent(j.go);
    CHECK(WaitForSingleObject(h, WAIT_MS) == WAIT_OBJECT_0);
    CloseHandle(h);
    CHECKF(j.before.qos != 0 && j.after.qos == j.before.qos && j.after.rel == j.before.rel,
           "a refused request leaves the class alone: before 0x%02x rel %d, after 0x%02x rel %d",
           j.before.qos, j.before.rel, j.after.qos, j.after.rel);
    CloseHandle(j.ready); CloseHandle(j.go);
}

/* A thread whose sched params were set can no longer take a class. */
typedef struct { int skipped; BOOL ok; DWORD err; int prio; } locked_job;

static DWORD WINAPI locked_worker(LPVOID arg)
{
    locked_job* j = (locked_job*)arg;
    struct sched_param sp; int pol;
    pthread_getschedparam(pthread_self(), &pol, &sp);
    pthread_setschedparam(pthread_self(), pol, &sp);
    if (pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0) == 0) { j->skipped = 1; return 0; }
    SetLastError(0);
    j->ok   = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    j->err  = GetLastError();
    j->prio = GetThreadPriority(GetCurrentThread());
    return 0;
}

/* Returns 1 if the host did not lock the class, so there was nothing to refuse. */
static int test_locked(void)
{
    locked_job j = { 0, TRUE, 0, 0 };
    HANDLE h = CreateThread(NULL, 0, locked_worker, &j, 0, NULL);
    CHECK(h != NULL);
    CHECK(WaitForSingleObject(h, WAIT_MS) == WAIT_OBJECT_0);
    CloseHandle(h);
    if (j.skipped) { fprintf(stderr, "SKIP: this host lets a thread with sched params set take a QoS class\n"); return 1; }
    CHECK(!j.ok);
    CHECK(j.err == ERROR_GEN_FAILURE);
    CHECK(j.prio == THREAD_PRIORITY_NORMAL);
    return 0;
}

static int count_lines(const char* text, const char* needle)
{
    int n = 0;
    for (const char* p = text; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}
#endif

int main(void)
{
#ifdef __APPLE__
    /* Not a class any level maps to, so no check passes by inheritance. */
    CHECK(pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0) == 0);
#endif
    const qos_state creator = read_qos();

#ifdef __APPLE__
    /* Refusals go to an unnamed temporary file so the once-per-process line can be counted. */
    FILE* cap = tmpfile();
    int tmp = cap ? fileno(cap) : -1;
    int saved = tmp >= 0 ? dup(2) : -1;
    CHECK(tmp >= 0 && saved >= 0);
    if (tmp >= 0 && saved >= 0) { fflush(stderr); dup2(tmp, 2); }
#endif

    test_self();
    test_suspended();
    test_untouched(&creator);
#ifdef __APPLE__
    test_running_other();
    const int lock_skipped = test_locked();

    if (tmp >= 0 && saved >= 0) {
        char buf[8192]; size_t n = 0; ssize_t r;
        fflush(stderr);
        lseek(tmp, 0, SEEK_SET);
        while (n < sizeof buf - 1 && (r = read(tmp, buf + n, sizeof buf - 1 - n)) > 0) n += (size_t)r;
        buf[n] = 0;
        dup2(saved, 2); close(saved);
        fputs(buf, stderr);
        const int other = count_lines(buf, "can only be set by that thread");
        const int qos   = count_lines(buf, "pthread_set_qos_class_self_np(");
        CHECKF(other == 1, "a refused request on a running thread is reported once, saw %d", other);
        CHECKF(qos == (lock_skipped ? 0 : 1), "a refused QoS call is reported once, saw %d", qos);
    }
#else
    fprintf(stderr, "SKIP: no QoS classes on this host; levels round-trip only\n");
#endif
    printf("thread_qos: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

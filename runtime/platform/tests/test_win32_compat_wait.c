/*
 * The Win32 shim's waits (win32_compat.c / win32_compat.h) on the monotonic
 * budget, deterministically.
 *
 * The shim is the REAL one. It is linked against host_wait_fake.c, which is the
 * real host_wait.c with a scripted native wait and, here, also the only clock:
 * the shim's deadlines and timer due times and the wait loop's budget read the
 * same scripted time. No sleeps, no host time.
 *
 * Pinned here: a single wait goes to its deadline in one native request and
 * keeps that deadline across wakes; a timer due before the caller's deadline
 * wakes the wait at the due time and is then consumed, one due after it does
 * not; zero is immediate and INFINITE is infinite; the multi-object, address
 * and condition-variable paths do the same; a host refusal is WAIT_FAILED or
 * FALSE with ERROR_GEN_FAILURE, never a timeout. The shim's static
 * multi-object condition variable is initialised once, whichever thread comes
 * first, and a failed initialisation is the same answer for every later wait.
 *
 * usage: test_win32_compat_wait [init-race | init-fail]
 *   (no argument)  every case but the two that must be the first to touch the
 *                  multi-object variable, each in a process of its own.
 * Built twice by CMake (win32_compat_wait_abs, _rel).
 */
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../win32_compat.h"
#include "host_wait_fake.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define MS 1000000ull
#define SEC 1000000000ull
#define BASE (1000ull * SEC)
#define WALL (1700000000ull * SEC)

static void begin(void) { hw_fake_reset(BASE, WALL); }

static void finish(void)
{
    CHECK(hw_fake_failures() == 0);
    CHECK(hw_fake_steps_left() == 0);
}

static void push(int kind, int rc, uint64_t advance, uint64_t want_rel, hw_fake_hook hook, void* arg)
{
    hw_fake_step s;
    memset(&s, 0, sizeof s);
    s.kind = kind; s.rc = rc; s.advance_ns = advance; s.want_rel_ns = want_rel;
    s.hook = hook; s.hook_arg = arg;
    hw_fake_push(&s);
}

static unsigned timed_calls(void)
{
    hw_fake_stats st;
    hw_fake_get_stats(&st);
    return st.timed_calls;
}

static unsigned infinite_calls(void)
{
    hw_fake_stats st;
    hw_fake_get_stats(&st);
    return st.infinite_calls;
}

static void set_event_hook(void* h) { SetEvent((HANDLE)h); }

static LARGE_INTEGER rel_ms(long long ms)
{
    LARGE_INTEGER due;
    due.QuadPart = -ms * 10000LL;        /* relative, 100 ns units */
    return due;
}

/* ------------------------------------------------------------------------- */
static void case_single_object(void)
{
    HANDLE e;

    /* A deadline of 10 ms goes to the native wait as 10 ms, and expires there. */
    begin();
    e = CreateEventA(NULL, FALSE, FALSE, NULL);
    push(HW_FAKE_TIMED, ETIMEDOUT, 10 * MS, 10 * MS, NULL, NULL);
    CHECK(WaitForSingleObject(e, 10) == WAIT_TIMEOUT);
    CHECK(GetLastError() == ERROR_TIMEOUT);
    CHECK(hw_fake_now() - BASE == 10 * MS);
    finish();

    /* The deadline survives a spurious wake: 3 ms in, the next request is the remaining 7 ms. */
    begin();
    push(HW_FAKE_TIMED, 0, 3 * MS, 10 * MS, NULL, NULL);
    push(HW_FAKE_TIMED, ETIMEDOUT, 7 * MS, 7 * MS, NULL, NULL);
    CHECK(WaitForSingleObject(e, 10) == WAIT_TIMEOUT);
    CHECK(hw_fake_now() - BASE == 10 * MS);
    finish();

    /* A SetEvent while the wait sleeps (the hook runs with the shim's lock released) wakes it and is consumed. */
    begin();
    push(HW_FAKE_TIMED, 0, 4 * MS, 10 * MS, set_event_hook, e);
    CHECK(WaitForSingleObject(e, 10) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(e, 0) == WAIT_TIMEOUT);                    /* auto-reset: taken */
    finish();

    /* Zero is immediate, INFINITE is the infinite native wait. */
    begin();
    CHECK(WaitForSingleObject(e, 0) == WAIT_TIMEOUT);
    CHECK(timed_calls() == 0 && infinite_calls() == 0);
    push(HW_FAKE_INFINITE, 0, 1000ull * SEC, HW_FAKE_ANY, set_event_hook, e);
    CHECK(WaitForSingleObject(e, INFINITE) == WAIT_OBJECT_0);
    CHECK(timed_calls() == 0 && infinite_calls() == 1);
    finish();

    /* A host refusal is not a wake and not a timeout. */
    begin();
    push(HW_FAKE_TIMED, EINVAL, 0, 5 * MS, NULL, NULL);
    CHECK(WaitForSingleObject(e, 5) == WAIT_FAILED && GetLastError() == ERROR_GEN_FAILURE);
    finish();
    CloseHandle(e);
}

static void case_timers(void)
{
    HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE t = CreateWaitableTimerA(NULL, FALSE, NULL);

    /* Timer due at 3 ms, caller deadline 10 ms: one request for 3 ms, then the timer is taken. */
    begin();
    LARGE_INTEGER due = rel_ms(3);
    CHECK(SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE));
    push(HW_FAKE_TIMED, ETIMEDOUT, 3 * MS, 3 * MS, NULL, NULL);
    CHECK(WaitForSingleObject(t, 10) == WAIT_OBJECT_0);
    CHECK(timed_calls() == 1);
    CHECK(WaitForSingleObject(t, 0) == WAIT_TIMEOUT);                    /* one-shot: consumed */
    finish();

    /* Timer due at 20 ms, caller deadline 10 ms: the deadline wins, the timer is still pending after. */
    begin();
    due = rel_ms(20);
    CHECK(SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE));
    push(HW_FAKE_TIMED, ETIMEDOUT, 10 * MS, 10 * MS, NULL, NULL);
    CHECK(WaitForSingleObject(t, 10) == WAIT_TIMEOUT);
    finish();
    begin();
    push(HW_FAKE_TIMED, ETIMEDOUT, 20 * MS, 20 * MS, NULL, NULL);       /* an INFINITE wait still wakes at the due time */
    CHECK(WaitForSingleObject(t, INFINITE) == WAIT_OBJECT_0);
    finish();

    /* Multi-object wait over an event and that timer: the earliest due time is the request. */
    begin();
    HANDLE hs[2] = { e, t };
    due = rel_ms(3);
    CHECK(SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE));
    push(HW_FAKE_TIMED, ETIMEDOUT, 3 * MS, 3 * MS, NULL, NULL);
    CHECK(WaitForMultipleObjects(2, hs, FALSE, 10) == WAIT_OBJECT_0 + 1);
    finish();

    /* ... a spurious wake keeps the caller's deadline; a signal ends it. */
    begin();
    push(HW_FAKE_TIMED, 0, 2 * MS, 10 * MS, NULL, NULL);
    push(HW_FAKE_TIMED, 0, 2 * MS, 8 * MS, set_event_hook, e);
    CHECK(WaitForMultipleObjects(2, hs, FALSE, 10) == WAIT_OBJECT_0);
    CHECK(hw_fake_now() - BASE == 4 * MS);
    finish();

    /* ... all objects, timing out; zero immediate; INFINITE infinite; a refusal is WAIT_FAILED. */
    begin();
    push(HW_FAKE_TIMED, ETIMEDOUT, 10 * MS, 10 * MS, NULL, NULL);
    CHECK(WaitForMultipleObjects(2, hs, TRUE, 10) == WAIT_TIMEOUT && GetLastError() == ERROR_TIMEOUT);
    CHECK(WaitForMultipleObjects(2, hs, TRUE, 0) == WAIT_TIMEOUT);
    push(HW_FAKE_INFINITE, 0, 5 * SEC, HW_FAKE_ANY, set_event_hook, e);
    CHECK(WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0);
    push(HW_FAKE_TIMED, EINVAL, 0, 5 * MS, NULL, NULL);
    CHECK(WaitForMultipleObjects(2, hs, FALSE, 5) == WAIT_FAILED && GetLastError() == ERROR_GEN_FAILURE);
    finish();

    CloseHandle(t);
    CloseHandle(e);
}

/* ------------------------------------------------------------------------- */
static volatile LONG g_value;

static void change_value_hook(void* a) { (void)a; g_value = 2; }
static void wake_address_hook(void* a) { (void)a; WakeByAddressAll((PVOID)&g_value); }

static void case_wait_on_address(void)
{
    LONG expect = 1;
    g_value = 1;

    begin();
    push(HW_FAKE_TIMED, ETIMEDOUT, 10 * MS, 10 * MS, NULL, NULL);
    CHECK(!WaitOnAddress(&g_value, &expect, sizeof expect, 10) && GetLastError() == ERROR_TIMEOUT);
    CHECK(hw_fake_now() - BASE == 10 * MS);
    finish();

    begin();                                                               /* timed out, but the value moved: success */
    push(HW_FAKE_TIMED, ETIMEDOUT, 10 * MS, 10 * MS, change_value_hook, NULL);
    CHECK(WaitOnAddress(&g_value, &expect, sizeof expect, 10));
    finish();

    g_value = 1;
    begin();                                                               /* a wake on the bucket: success */
    push(HW_FAKE_TIMED, 0, 3 * MS, 10 * MS, wake_address_hook, NULL);
    CHECK(WaitOnAddress(&g_value, &expect, sizeof expect, 10));
    finish();

    begin();                                                               /* zero: immediate; INFINITE: infinite */
    CHECK(!WaitOnAddress(&g_value, &expect, sizeof expect, 0) && GetLastError() == ERROR_TIMEOUT);
    CHECK(timed_calls() == 0 && infinite_calls() == 0);
    push(HW_FAKE_INFINITE, 0, 100 * SEC, HW_FAKE_ANY, wake_address_hook, NULL);
    CHECK(WaitOnAddress(&g_value, &expect, sizeof expect, INFINITE));
    CHECK(infinite_calls() == 1);
    finish();

    begin();
    push(HW_FAKE_TIMED, EINVAL, 0, 5 * MS, NULL, NULL);
    CHECK(!WaitOnAddress(&g_value, &expect, sizeof expect, 5) && GetLastError() == ERROR_GEN_FAILURE);
    finish();
}

/* ------------------------------------------------------------------------- */
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static CRITICAL_SECTION   g_cs;
static SRWLOCK            g_srw = SRWLOCK_INIT;

static void wake_cv_hook(void* a) { (void)a; WakeConditionVariable(&g_cv); }

static void case_condition_variable(void)
{
    InitializeCriticalSection(&g_cs);
    EnterCriticalSection(&g_cs);

    begin();                                                               /* a spurious wake, then a real one */
    push(HW_FAKE_TIMED, 0, 3 * MS, 10 * MS, NULL, NULL);                   /* generation unchanged: sleep again */
    push(HW_FAKE_TIMED, 0, 2 * MS, 7 * MS, wake_cv_hook, NULL);            /* with 7 ms left, not 10 */
    CHECK(SleepConditionVariableCS(&g_cv, &g_cs, 10));
    CHECK(hw_fake_now() - BASE == 5 * MS);
    finish();

    begin();                                                               /* timeout */
    push(HW_FAKE_TIMED, ETIMEDOUT, 10 * MS, 10 * MS, NULL, NULL);
    CHECK(!SleepConditionVariableCS(&g_cv, &g_cs, 10) && GetLastError() == ERROR_TIMEOUT);
    finish();

    begin();                                                               /* zero: immediate */
    CHECK(!SleepConditionVariableCS(&g_cv, &g_cs, 0) && GetLastError() == ERROR_TIMEOUT);
    CHECK(timed_calls() == 0 && infinite_calls() == 0);
    finish();

    begin();                                                               /* INFINITE */
    push(HW_FAKE_INFINITE, 0, 100 * SEC, HW_FAKE_ANY, wake_cv_hook, NULL);
    CHECK(SleepConditionVariableCS(&g_cv, &g_cs, INFINITE));
    finish();

    begin();                                                               /* a host refusal is not a timeout */
    push(HW_FAKE_TIMED, EINVAL, 0, 5 * MS, NULL, NULL);
    CHECK(!SleepConditionVariableCS(&g_cv, &g_cs, 5) && GetLastError() == ERROR_GEN_FAILURE);
    finish();
    LeaveCriticalSection(&g_cs);

    AcquireSRWLockExclusive(&g_srw);
    begin();
    push(HW_FAKE_TIMED, ETIMEDOUT, 10 * MS, 10 * MS, NULL, NULL);
    CHECK(!SleepConditionVariableSRW(&g_cv, &g_srw, 10, 0) && GetLastError() == ERROR_TIMEOUT);
    finish();
    ReleaseSRWLockExclusive(&g_srw);
    DeleteCriticalSection(&g_cs);
}

/* ------------------------------------------------------------------------- */
enum { NTHREADS = 8 };
static _Atomic unsigned g_ready;
static _Atomic int g_go;
static _Atomic unsigned g_timeouts;
static _Atomic unsigned g_done;
static HANDLE g_pair[2];

static DWORD WINAPI first_multi_wait(LPVOID arg)
{
    (void)arg;
    atomic_fetch_add(&g_ready, 1);
    while (!atomic_load(&g_go)) { }
    if (WaitForMultipleObjects(2, g_pair, FALSE, 0) == WAIT_TIMEOUT) atomic_fetch_add(&g_timeouts, 1);
    atomic_fetch_add(&g_done, 1);
    return 0;
}

/* The multi-object variable's first use, from eight threads at once, is one initialisation. */
static void case_init_race(void)
{
    hw_fake_init_stats before, after;
    begin();
    g_pair[0] = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_pair[1] = CreateEventA(NULL, FALSE, FALSE, NULL);

    HANDLE th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) { th[i] = CreateThread(NULL, 0, first_multi_wait, NULL, 0, NULL); CHECK(th[i]); }
    while (atomic_load(&g_ready) != NTHREADS) { }
    hw_fake_init_get(&before);          /* every shim object the threads need exists by now */
    atomic_store(&g_go, 1);
    while (atomic_load(&g_done) != NTHREADS) { }   /* not WaitForSingleObject: the native wait here is the fake */
    for (int i = 0; i < NTHREADS; i++) CloseHandle(th[i]);
    hw_fake_init_get(&after);
    CHECK(atomic_load(&g_timeouts) == NTHREADS);
    CHECK(after.cond_init - before.cond_init == 1);     /* the multi-object variable, once, whoever came first */
    CloseHandle(g_pair[0]);
    CloseHandle(g_pair[1]);
}

/* A failed first initialisation of the multi-object variable is the answer every later multi-object wait gets;
 * single-object waits, which do not use it, are unaffected. */
static void case_init_fail(void)
{
    begin();
    HANDLE a = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE b = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE hs[2] = { a, b };
    CHECK(a && b);

    hw_fake_init_ctl ctl;
    memset(&ctl, 0, sizeof ctl);
    ctl.cond_init_rc = EAGAIN;
    hw_fake_init_set(&ctl);
    CHECK(WaitForMultipleObjects(2, hs, FALSE, 0) == WAIT_FAILED && GetLastError() == ERROR_NOT_ENOUGH_MEMORY);
    memset(&ctl, 0, sizeof ctl);
    hw_fake_init_set(&ctl);                                              /* the host recovers; the answer stays */
    CHECK(WaitForMultipleObjects(2, hs, FALSE, 0) == WAIT_FAILED && GetLastError() == ERROR_NOT_ENOUGH_MEMORY);

    SetEvent(a);
    CHECK(WaitForSingleObject(a, 0) == WAIT_OBJECT_0);
    CloseHandle(a);
    CloseHandle(b);

    /* a condition variable that cannot be built is no object at all */
    memset(&ctl, 0, sizeof ctl);
    ctl.cond_init_rc = EAGAIN;
    hw_fake_init_set(&ctl);
    CHECK(CreateEventA(NULL, FALSE, FALSE, NULL) == NULL && GetLastError() == ERROR_NOT_ENOUGH_MEMORY);
    CHECK(CreateWaitableTimerA(NULL, FALSE, NULL) == NULL);
    CHECK(CreateSemaphoreA(NULL, 0, 1, NULL) == NULL);
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "init-race"))      case_init_race();
    else if (!strcmp(mode, "init-fail")) case_init_fail();
    else {
        case_single_object();
        case_timers();
        case_wait_on_address();
        case_condition_variable();
    }
    printf("PASS: win32 shim waits%s%s on the monotonic budget, %s backend\n", *mode ? " " : "", mode,
           hw_fake_backend_is_relative() ? "relative" : "absolute");
    return 0;
}

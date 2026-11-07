/*
 * A scripted clock and scripted native waits behind the REAL host_wait.c.
 *
 * host_wait_fake.c compiles runtime/platform/host_wait.c with the
 * PS3_HOST_WAIT_TEST_SEAMS macros pointing here. The production budget, slice
 * and retry loop therefore run unchanged; only the two things a deterministic
 * test cannot own are replaced, the monotonic clock and the native sleep. A
 * test links this object INSTEAD of host_wait.c, together with whatever real
 * consumer (sys_event.c, sys_cond.c, ...) it wants to drive through an
 * expiry. Which backend the real loop takes is fixed per build by
 * PS3_HW_RELATIVE (0: absolute CLOCK_MONOTONIC deadline, Linux's; 1: relative
 * duration, macOS's), so both run on any host.
 *
 * Every wrong thing the loop does is recorded, not asserted here:
 * hw_fake_failures() counts them and each prints a line. A native wait nobody
 * scripted aborts the process (a test that would otherwise hang).
 */
#ifndef PS3RECOMP_HOST_WAIT_FAKE_H
#define PS3RECOMP_HOST_WAIT_FAKE_H

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HW_FAKE_TIMED = 1, HW_FAKE_INFINITE = 2 };
#define HW_FAKE_ANY UINT64_MAX

/* Runs while the native wait has the caller's lock RELEASED, as a real
 * condition wait does: a test signals, cancels or destroys from here. */
typedef void (*hw_fake_hook)(void* arg);

typedef struct hw_fake_step {
    int          kind;         /* the native operation the loop must call */
    int          rc;           /* what it returns */
    uint64_t     advance_ns;   /* the fake clock moves by this during the wait */
    uint64_t     want_rel_ns;  /* timed: the duration the loop must ask for, or HW_FAKE_ANY */
    hw_fake_hook hook;
    void*        hook_arg;
} hw_fake_step;

typedef struct hw_fake_stats {
    unsigned timed_calls, infinite_calls;
    uint64_t last_rel_ns;                  /* duration of the last timed request */
    int64_t  last_abs_sec;                 /* absolute backend: its timespec */
    int64_t  last_abs_nsec;
    unsigned realtime_reads;               /* clock_gettime(CLOCK_REALTIME) from the code under test */
    unsigned monotonic_gettime_reads;      /* clock_gettime(CLOCK_MONOTONIC): the loop reads HW_NOW_NS only */
    unsigned stray_native;                 /* a native wait that bypassed the seam */
    unsigned clock_reads;                  /* HW_NOW_NS calls */
} hw_fake_stats;

void     hw_fake_reset(uint64_t mono_now_ns, uint64_t wall_now_ns);
void     hw_fake_push(const hw_fake_step* step);
unsigned hw_fake_steps_left(void);
unsigned hw_fake_failures(void);
void     hw_fake_get_stats(hw_fake_stats* out);
uint64_t hw_fake_now(void);
void     hw_fake_set_now(uint64_t mono_now_ns);
void     hw_fake_set_wall(uint64_t wall_now_ns);

/* The tries the timed mutex makes. By default they reach the real
 * pthread_mutex_trylock; a script of results replaces that. A sleep or a yield
 * advances the fake clock (a yield by hw_fake_set_yield_cost_ns, 1 us unless
 * changed) so a timed lock always converges. */
void     hw_fake_mutex_script(const int* rcs, unsigned count);   /* 0 = acquired (really locks) */
void     hw_fake_set_yield_cost_ns(uint64_t ns);
unsigned hw_fake_mutex_sleeps(const uint64_t** ns_out);          /* sleeps (nanoseconds each) */
unsigned hw_fake_mutex_yields(void);
unsigned hw_fake_mutex_locks(void);                              /* infinite pthread_mutex_lock calls */
unsigned hw_fake_mutex_tries(void);

/* Initialisation seams. Results are 0 unless set. */
typedef struct hw_fake_init_ctl {
    int attr_init_rc, setclock_rc, cond_init_rc, attr_destroy_rc;
} hw_fake_init_ctl;
typedef struct hw_fake_init_stats {
    unsigned attr_init, setclock, cond_init, cond_init_with_attr, attr_destroy, cond_destroy;
    int      clock_id;                     /* the last clock given to setclock, -1 if never */
} hw_fake_init_stats;
void hw_fake_init_set(const hw_fake_init_ctl* ctl);
void hw_fake_init_get(hw_fake_init_stats* out);
int  hw_fake_backend_is_relative(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_HOST_WAIT_FAKE_H */

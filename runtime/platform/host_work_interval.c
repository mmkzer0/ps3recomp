/*
 * ps3recomp - host work interval (include/ps3emu/host_work_interval.h)
 *
 * macOS: time-constraint policy + an audio interval workgroup, one interval per
 * unit of work. Every other host: join stops, begin/end are empty.
 */
#include "ps3emu/host_work_interval.h"

#include "ps3emu/config.h"
#include "ps3emu/ps3_log.h"
#include "ps3emu/stop.h"

#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)

#include <AudioToolbox/AudioWorkInterval.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>   /* not mach.h: its vm_protect clashes with the toolkit's */
#include <mach/thread_policy.h>
#include <os/object.h>
#include <os/workgroup.h>
#include <pthread.h>
#include <stdint.h>

/* The time-constraint shape the interval needs to be acted on. Period is one
 * 59.94 Hz frame: the work recurs once per frame. Computation (4 ms) is a
 * generous bound on one pass, constraint (16 ms) just under the period. The
 * policy alone does not raise the clock (it is the same shape with no
 * interval, measured), so these values are not a tuning knob: they only make
 * the thread eligible. */
enum { TC_PERIOD_US = 16683, TC_COMPUTATION_US = 4000, TC_CONSTRAINT_US = 16000 };

/* Thread-local: an interval belongs to the thread that joined the workgroup, and
 * a pass that runs on another thread must not start or finish it. */
static _Thread_local os_workgroup_interval_t t_wg;
static _Thread_local os_workgroup_join_token_s t_token;
static _Thread_local uint64_t t_deadline_ticks;   /* host.work_interval_us in mach absolute units */
static _Thread_local int t_open;                  /* an interval is started and not yet finished */

static void interval_error(const char* what, int err)
{
    /* A hint the host declined mid-run is worth a line, not a flood: one pass per
     * frame would repeat it every frame. */
    PS3_LOG_N(4, PS3_LOG_INFO, "work-interval", "%s failed: %d (%s); this pass runs outside an interval\n",
              what, err, strerror(err));
}

/* 0 on success; otherwise a logged reason, `step` and `code` say what the host refused. */
static int join_apple(const char* name, const char** step, int* code)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const double to_abs = (double)tb.denom / (double)tb.numer * 1000.0;   /* us -> mach absolute units */

    thread_time_constraint_policy_data_t pol;
    pol.period = (uint32_t)(TC_PERIOD_US * to_abs);
    pol.computation = (uint32_t)(TC_COMPUTATION_US * to_abs);
    pol.constraint = (uint32_t)(TC_CONSTRAINT_US * to_abs);
    pol.preemptible = 1;
    const kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                               (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) {
        *step = "thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY)";
        *code = (int)kr;
        return 1;
    }

    os_workgroup_interval_t wg = AudioWorkIntervalCreate(name, OS_CLOCK_MACH_ABSOLUTE_TIME, NULL);
    if (!wg) {
        *step = "AudioWorkIntervalCreate";
        *code = 0;
        return 1;
    }
    const int jr = os_workgroup_join(wg, &t_token);
    if (jr != 0) {
        os_release(wg);
        *step = "os_workgroup_join";
        *code = jr;
        return 1;
    }

    t_wg = wg;
    t_deadline_ticks = (uint64_t)ps3_cfg_host_work_interval_us() * 1000u * tb.denom / tb.numer;
    t_open = 0;
    return 0;
}

void ps3_host_work_interval_join(const char* name)
{
    if (!ps3_cfg_host_work_interval() || t_wg) return;
    const char* step = "";
    int code = 0;
    if (join_apple(name, &step, &code) != 0) {
        PS3_LOG(PS3_LOG_ERROR, "work-interval", "%s: %s failed: %d (%s)\n", name, step, code,
                code ? strerror(code) : "no workgroup returned");
        char c[16];
        snprintf(c, sizeof c, "%d", code);
        const ps3_stop_field f[] = { {"key", "host.work_interval"}, {"thread", name}, {"step", step}, {"code", c} };
        ps3_stop(PS3_STOP_CONFIG_INVALID, "host.work_interval is on and the host refused the interval workgroup join",
                 "Set host.work_interval = false (PS3RECOMP_HOST_WORK_INTERVAL=0), or run on a host that grants the "
                 "time-constraint policy and audio workgroups (docs/CONFIG.md).",
                 f, 4, NULL);
    }
    PS3_LOG(PS3_LOG_INFO, "work-interval", "%s: joined an interval workgroup, time-constraint policy applied "
            "(period %d us, computation %d us, constraint %d us), deadline %u us per interval\n",
            name, TC_PERIOD_US, TC_COMPUTATION_US, TC_CONSTRAINT_US, (unsigned)ps3_cfg_host_work_interval_us());
}

void ps3_host_work_interval_begin(void)
{
    if (!t_wg) return;
    const uint64_t now = mach_absolute_time();
    const int r = os_workgroup_interval_start(t_wg, now, now + t_deadline_ticks, NULL);
    if (r != 0) {
        interval_error("os_workgroup_interval_start", r);
        return;
    }
    t_open = 1;
}

void ps3_host_work_interval_end(void)
{
    if (!t_open) return;
    t_open = 0;
    const int r = os_workgroup_interval_finish(t_wg, NULL);
    if (r != 0) interval_error("os_workgroup_interval_finish", r);
}

#else /* !__APPLE__ */

void ps3_host_work_interval_join(const char* name)
{
    if (!ps3_cfg_host_work_interval()) return;
    PS3_LOG(PS3_LOG_ERROR, "work-interval", "%s: no interval workgroup on this host\n", name);
    const ps3_stop_field f[] = { {"key", "host.work_interval"}, {"thread", name},
                                 {"step", "interval workgroup"}, {"code", "unsupported"} };
    ps3_stop(PS3_STOP_CONFIG_INVALID, "host.work_interval is on and this host has no interval workgroup",
             "Set host.work_interval = false (PS3RECOMP_HOST_WORK_INTERVAL=0); the option exists on macOS only "
             "(docs/CONFIG.md).",
             f, 4, NULL);
}

void ps3_host_work_interval_begin(void) {}
void ps3_host_work_interval_end(void) {}

#endif

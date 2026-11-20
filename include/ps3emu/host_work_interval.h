/*
 * ps3recomp - host work interval (option host.work_interval)
 *
 * A host thread that does one bounded unit of work per pass, on the path from a
 * guest input to the screen, can tell the host's performance controller about it:
 * the controller then sees the work and its deadline and holds the cluster at a
 * higher clock while the work runs. On Apple Silicon the unit of work is an
 * "interval" of an audio workgroup (os_workgroup_interval_start/finish), and
 * the thread must be in the time-constraint band for the controller to take it
 * into account. A time-constraint policy alone moved nothing in the same
 * measurement: the interval with a deadline is what the controller acts on.
 *
 * Use, on the thread that runs the work:
 *
 *     ps3_host_work_interval_join("rsx walker");     // once, first thing on the thread
 *     for (;;) {
 *         ps3_host_work_interval_begin();
 *         one_pass();
 *         ps3_host_work_interval_end();
 *     }
 *
 * The state is thread-local: begin and end act on the workgroup the calling
 * thread joined, so they must run on the thread that called join. A thread that
 * did not join (the option is off, or join was never called) makes both calls
 * no-ops that touch no host API.
 *
 * Off (host.work_interval = false, the default) nothing here calls a host
 * framework. On: only macOS has the mechanism. A join the host refuses, and any
 * other host, end the run with kind config-invalid naming host.work_interval,
 * so a run that asked for the behaviour never silently runs without it.
 */
#ifndef PS3EMU_HOST_WORK_INTERVAL_H
#define PS3EMU_HOST_WORK_INTERVAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* With host.work_interval on: put the calling thread in the Mach time-constraint
 * band (period one 59.94 Hz frame, computation 4 ms, constraint 16 ms,
 * preemptible), create an interval workgroup named `name` on the Mach absolute
 * clock and join it, then log one line saying so. A refused step is logged with
 * its reason and ends the run (config-invalid, key host.work_interval); it does
 * not return. A second call on a thread that already joined does nothing. With
 * the option off, does nothing. Call it from the thread itself, before its first
 * ps3_host_work_interval_begin. */
void ps3_host_work_interval_join(const char* name);

/* Start one interval whose deadline is host.work_interval_us after now. No-op on
 * a thread that has not joined. A refusal by the host is logged (the first few
 * per process) and the pass runs outside an interval: after a successful join
 * the interval is a scheduling hint, not a correctness contract. */
void ps3_host_work_interval_begin(void);

/* Finish the interval ps3_host_work_interval_begin started. No-op when none is
 * open on this thread. */
void ps3_host_work_interval_end(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_HOST_WORK_INTERVAL_H */

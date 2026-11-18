/*
 * ps3recomp - iOS host pad backend (GameController framework)
 *
 * cellPad's iOS backend. pad_gc.m owns the controllers; cellPad.c only asks,
 * once per 4 ms poll, what each host port last read (pad_gc_read) and maps
 * the snapshot into the guest's pad state (pad_gc_map).
 *
 * Threading: connect, disconnect and every value report are delivered on ONE
 * private serial queue inside pad_gc.m, and the per-port snapshots are
 * published under a small lock for readers on the pad poll thread. That is
 * what keeps a report queued behind a disconnect from re-pressing a port
 * (IOS-LEVELING-PLAN.md finding 3): the value handler checks that the
 * controller reporting is still the one bound to its port before writing.
 *
 * The mapping and the report layout live in pad_gc_map.h / pad_gc_map.c,
 * which are pure C and tested on every host.
 */

#ifndef PS3RECOMP_PAD_GC_H
#define PS3RECOMP_PAD_GC_H

#include "pad_gc_map.h"

/* One slot per PS3 host port; cellPad's PAD_MAX_HOST_PORTS is 4. */
#define PAD_GC_PORTS 4

#ifdef __cplusplus
extern "C" {
#endif

/* Create the queue and observers, then adopt the controllers already
 * connected. Idempotent. Not safe to call concurrently with itself or
 * pad_gc_stop: cellPad calls both under its poll lock. */
void pad_gc_start(void);

/* Remove the observers and unbind everything. Safe to call twice; after it a
 * port reads as unbound (pad_gc_read returns 0) until the next start. */
void pad_gc_stop(void);

/* Latest snapshot for a host port. 1 = a controller is bound to it and *out
 * holds its state; 0 = no controller, *out is not touched. Callable from any
 * thread (the pad poll thread). */
int pad_gc_read(unsigned port, pad_gc_report* out);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_PAD_GC_H */

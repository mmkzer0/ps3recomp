/*
 * ps3recomp - the per-thread host-stack high-water report for guest-code worker roles.
 * (PS3_HOST_STACK_HWM, docs/DIAGNOSTICS.md). Guest-created PPU threads use their existing
 * table (runtime/syscalls/sys_ppu_thread.c); all other configured roles use this reporter.
 *
 * Header-only, one ps3_hs_reporter per translation unit and role, so a fixture that includes the .c file needs
 * nothing more than the file itself. Off (the default), a thread costs one cached environment lookup at its start
 * and one at its end. On, a thread prints its high-water mark when it ends, and the process-exit summary reports
 * the threads still running: group members and policy-module workers can live for the whole run.
 *
 *     static ps3_hs_reporter s_hs = PS3_HS_REPORTER_INIT("spu");
 *     static void s_hs_summary(void) { ps3_hs_summary(&s_hs); }
 *     ...on the thread:   const int hs = ps3_hs_begin(&s_hs, id, reserved_bytes, s_hs_summary);
 *     ...                 ps3_hs_end(&s_hs, hs);
 *
 * Linux needs _GNU_SOURCE before the first system header (host_stack.h).
 */
#ifndef PS3EMU_HOST_STACK_REPORT_H
#define PS3EMU_HOST_STACK_REPORT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "ps3emu/host_exit.h"
#include "ps3emu/host_stack.h"
#include "ps3emu/ps3_env_once.h"

#define PS3_HS_LIVE 256 /* includes every supported SPU group member */

typedef struct ps3_hs_reporter {
    const char* label;                 /* "spu", "spu-dispatch": the word after [HOSTSTACK] */
    struct { uintptr_t lo; size_t size; size_t reserved; uint64_t id; } live[PS3_HS_LIVE];   /* lo == 0: free */
    unsigned    reported;              /* measurements printed (atomic) */
    size_t      max;                   /* the largest of them (atomic) */
    int         registered;            /* the exit handlers are installed */
    unsigned dropped;                 /* table overflow, retained in every summary */
#ifdef _WIN32
    SRWLOCK     lock;
#else
    pthread_mutex_t lock;
#endif
} ps3_hs_reporter;

#ifdef _WIN32
#  define PS3_HS_REPORTER_INIT(label_) { (label_), { { 0, 0, 0, 0 } }, 0u, 0u, 0, 0u, SRWLOCK_INIT }
#  define PS3_HS_LOCK(r)   AcquireSRWLockExclusive(&(r)->lock)
#  define PS3_HS_UNLOCK(r) ReleaseSRWLockExclusive(&(r)->lock)
#else
#  define PS3_HS_REPORTER_INIT(label_) { (label_), { { 0, 0, 0, 0 } }, 0u, 0u, 0, 0u, PTHREAD_MUTEX_INITIALIZER }
#  define PS3_HS_LOCK(r)   pthread_mutex_lock(&(r)->lock)
#  define PS3_HS_UNLOCK(r) pthread_mutex_unlock(&(r)->lock)
#endif

static inline void ps3_hs_publish(ps3_hs_reporter* r, uint64_t id, const char* state, size_t reserved, int known,
                                  size_t hw)
{
    if (known) {
        fprintf(stderr, "[HOSTSTACK] %s id=%llu %s reserved=%zu high_water=%zu (%.1f%%)\n", r->label,
                (unsigned long long)id, state, reserved, hw, reserved ? 100.0 * (double)hw / (double)reserved : 0.0);
        if (hw > r->max) r->max = hw;
    } else {
        fprintf(stderr, "[HOSTSTACK] %s id=%llu %s reserved=%zu high_water=unknown\n", r->label,
                (unsigned long long)id, state, reserved);
    }
    ++r->reported;
}

/* at_quick_exit/atexit: the threads still running, then one line for the role. */
static inline void ps3_hs_summary(ps3_hs_reporter* r)
{
    PS3_HS_LOCK(r);
    for (int i = 0; i < PS3_HS_LIVE; i++) {
        if (!r->live[i].lo) continue;
        size_t hw = 0;
        const int known = host_stack_high_water(r->live[i].lo, r->live[i].size, &hw);
        ps3_hs_publish(r, r->live[i].id, "live", r->live[i].reserved, known, hw);
    }
    fprintf(stderr, "[HOSTSTACK] %s summary: threads=%u max_high_water=%zu dropped=%u\n", r->label,
            r->reported, r->max, r->dropped);
    PS3_HS_UNLOCK(r);
    fflush(stderr);
}

/* On the thread, at its start. `summary` is the translation unit's handler that calls ps3_hs_summary on this
 * reporter. Returns the slot of the live table, or -1 when nothing is tracked. */
static inline int ps3_hs_begin(ps3_hs_reporter* r, uint64_t id, size_t reserved, void (*summary)(void))
{
    /* PS3_HOST_STACK_HWM=1: report each thread's host-stack high-water mark (the span from the top of its stack to
     * the lowest page it touched) as "[HOSTSTACK] <role> id=.. reserved=.. high_water=.." when the thread ends,
     * and the threads still running in a summary at process exit; the guest PPU threads report the same way
     * (sys_ppu_thread.c). Unset, nothing is measured. Size ppu.host_stack_mb, spu.host_stack_mb and
     * spu.dispatch_stack_mb from it. */
    if (!ps3_env_once("PS3_HOST_STACK_HWM")) return -1;
    uintptr_t lo;
    size_t size;
    if (!host_stack_range_self(&lo, &size)) return -1;
    int slot = -1;
    PS3_HS_LOCK(r);
    if (!r->registered) {
        r->registered = 1;
        atexit(summary);
        ps3_host_at_quick_exit(summary);
    }
    for (int i = 0; i < PS3_HS_LIVE; i++)
        if (!r->live[i].lo) {
            r->live[i].lo = lo; r->live[i].size = size; r->live[i].reserved = reserved; r->live[i].id = id;
            slot = i;
            break;
        }
    if (slot < 0) ++r->dropped;
    PS3_HS_UNLOCK(r);
    if (slot < 0)
        fprintf(stderr, "[HOSTSTACK] %s: more than %d threads at once, one is not tracked\n", r->label, PS3_HS_LIVE);
    return slot;
}

/* On the thread, before it ends: report its mark and drop it from the live table. */
static inline void ps3_hs_end(ps3_hs_reporter* r, int slot)
{
    if (slot < 0) return;
    size_t hw = 0;
    PS3_HS_LOCK(r);
    const int known = host_stack_high_water(r->live[slot].lo, r->live[slot].size, &hw);
    ps3_hs_publish(r, r->live[slot].id, "exited", r->live[slot].reserved, known, hw);
    r->live[slot].lo = 0;
    PS3_HS_UNLOCK(r);
}

#endif /* PS3EMU_HOST_STACK_REPORT_H */

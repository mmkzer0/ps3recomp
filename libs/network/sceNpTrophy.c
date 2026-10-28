/*
 * ps3recomp - sceNpTrophy HLE implementation
 *
 * Trophy management with persistent storage.  Unlocked trophies are saved
 * to a JSON file at gamedata/trophies/{commId}.json.
 *
 * JSON format (hand-written, no external dependency):
 * { "trophies": [ { "id": N, "timestamp": T }, ... ] }
 */

#include "sceNpTrophy.h"
#include "../system/cellSysutil.h"
#include "ps3emu/guest_call.h"
#include "ps3emu/stop.h"
#include "ps3emu/config.h"   /* trophy.register_pump_timeout_ms */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
#include "ps3emu/endian.h"                   /* ps3_bswap64 -- guest is big-endian */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */
/* HLE args arrive as guest effective addresses; translate before deref. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

/* The guest-call bridge that returns r3 (runtime/ppu/ppu_loader.cpp). The
 * g_ps3_guest_caller hook is void and the firmware's inline status callback
 * is judged by its return value, so that one call goes through this; same
 * precedent as sys_spu_printf.c. */
extern u64 ppu_guest_call(u32 opd_addr,
                          u64 a0, u64 a1, u64 a2, u64 a3,
                          u64 a4, u64 a5, u64 a6, u64 a7);

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define mkdir_p(path) _mkdir(path)
#define PATH_SEP '\\'
#else
#include <pthread.h>
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#define PATH_SEP '/'
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_trophy_initialized = 0;
static char s_storage_path[512] = "gamedata/trophies";
static int  s_storage_path_set = 0;

/* $PS3_GAMEDATA_ROOT (see cellSaveData.c) moves the default next to the saves;
 * an explicit sceNpTrophySetStoragePath still wins. */
static void trophy_adopt_gamedata_root(void)
{
    static int s_done = 0;
    if (s_done || s_storage_path_set) return;
    s_done = 1;
    const char* env = getenv("PS3_GAMEDATA_ROOT");
    if (env && *env)
        snprintf(s_storage_path, sizeof(s_storage_path), "%s/trophies", env);
}

/* Per-context data */
typedef struct {
    int                      in_use;
    int                      registered;
    SceNpCommunicationId     commId;
    u8                       unlocked[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u64                      unlock_time[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u32                      total_trophies;
    /* Registration lifecycle, guarded by s_life_lock. life_serial is non-zero
     * while the lifecycle is owned by this context; destroying or re-registering
     * the context zeroes or replaces it, which cancels the rest. v0 is the
     * vblank it started at, posted how many statuses the "VSH" has queued,
     * delivered how many a poll has run. complete_seen/complete_v mark the
     * completion's consumption. */
    u64                      life_serial;
    u32                      life_cb;
    u32                      life_arg;
    u32                      life_v0;
    u32                      life_posted;
    u32                      life_delivered;
    int                      life_complete_seen;
    u32                      life_complete_v;
    u64                      life_complete_poll;
    /* The RegisterContext call blocked in trophy_run_lifecycle on this
     * context names this handle (0: none) and owns lifecycle serial
     * life_wait_serial, so a call that was superseded does not clear the
     * newer one's entry; abort_hit is set by an AbortHandle that woke it.
     * All under s_life_lock. */
    s32                      life_wait_handle;
    u64                      life_wait_serial;
    int                      life_abort_hit;
} TrophyContext;

/* aborted is guarded by s_life_lock (see "Handle abort" below); in_use keeps
 * the module's unguarded convention. */
typedef struct {
    int in_use;
    int aborted;
} TrophyHandle;

/* ID zero is the firmware invalid sentinel; retain all four usable slots. */
static TrophyContext s_contexts[SCE_NP_TROPHY_MAX_CONTEXTS + 1];
static TrophyHandle  s_handles[SCE_NP_TROPHY_MAX_HANDLES + 1];

/* ---------------------------------------------------------------------------
 * Registration lifecycle: synchronisation
 *
 * sceNpTrophyRegisterContext blocks on the registration thread while the
 * status events are delivered by whichever thread calls
 * cellSysutilCheckCallback, so the lifecycle fields are shared across guest
 * threads. They live behind one lock and one condition variable; the lock is
 * never held across a guest callback or a sleep (the condition wait releases
 * it). The rest of this module is not guarded and is not made to be.
 * -----------------------------------------------------------------------*/

#ifdef _WIN32
static SRWLOCK s_life_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_life_cv = CONDITION_VARIABLE_INIT;
typedef DWORD trophy_tid;
static trophy_tid trophy_self(void) { return GetCurrentThreadId(); }
static int trophy_tid_eq(trophy_tid a, trophy_tid b) { return a == b; }
static void life_lock(void)   { AcquireSRWLockExclusive(&s_life_lock); }
static void life_unlock(void) { ReleaseSRWLockExclusive(&s_life_lock); }
static void life_wake(void)   { WakeAllConditionVariable(&s_life_cv); }
static void life_wait(u64 us)
{
    DWORD ms = (DWORD)((us + 999) / 1000);
    SleepConditionVariableSRW(&s_life_cv, &s_life_lock, ms ? ms : 1, 0);
}
static u64 trophy_clock_real(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (u64)(c.QuadPart / f.QuadPart) * 1000000u +
           (u64)(c.QuadPart % f.QuadPart) * 1000000u / (u64)f.QuadPart;
}
#else
static pthread_mutex_t s_life_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_life_cv   = PTHREAD_COND_INITIALIZER;
typedef pthread_t trophy_tid;
static trophy_tid trophy_self(void) { return pthread_self(); }
static int trophy_tid_eq(trophy_tid a, trophy_tid b) { return pthread_equal(a, b); }
static void life_lock(void)   { pthread_mutex_lock(&s_life_lock); }
static void life_unlock(void) { pthread_mutex_unlock(&s_life_lock); }
static void life_wake(void)   { pthread_cond_broadcast(&s_life_cv); }
static void life_wait(u64 us)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(us / 1000000u);
    ts.tv_nsec += (long)(us % 1000000u) * 1000;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&s_life_cv, &s_life_lock, &ts);
}
static u64 trophy_clock_real(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000u + (u64)ts.tv_nsec / 1000u;
}
#endif

static u64       s_life_next_serial = 1;
static u32       s_register_wait_caps;   /* RegisterContext calls that returned at the cap */
static int       s_life_delivering;      /* a status callback is running */
static trophy_tid s_life_deliverer;      /* ... on this thread */
static u64       s_life_polls;           /* polls that began with no callback running */

/* ---------------------------------------------------------------------------
 * Persistent storage helpers
 * -----------------------------------------------------------------------*/

static void trophy_ensure_dir(void)
{
    char tmp[512];
    char* p;

    trophy_adopt_gamedata_root();
    strncpy(tmp, s_storage_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Create directories recursively */
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            mkdir_p(tmp);
            *p = PATH_SEP;
        }
    }
    mkdir_p(tmp);
}

static void trophy_get_filepath(const SceNpCommunicationId* commId,
                                 char* buf, size_t bufsize)
{
    trophy_adopt_gamedata_root();
    snprintf(buf, bufsize, "%s/%s.json", s_storage_path, commId->data);
}

static int trophy_save(TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp;
    int first = 1;

    trophy_ensure_dir();
    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));

    fp = fopen(filepath, "w");
    if (!fp) {
        printf("[sceNpTrophy] WARNING: Could not save trophies to %s\n",
               filepath);
        return -1;
    }

    fprintf(fp, "{\n  \"trophies\": [");
    for (u32 i = 0; i < ctx->total_trophies; i++) {
        if (ctx->unlocked[i]) {
            if (!first) fprintf(fp, ",");
            fprintf(fp, "\n    { \"id\": %u, \"timestamp\": %llu }",
                    i, (unsigned long long)ctx->unlock_time[i]);
            first = 0;
        }
    }
    fprintf(fp, "\n  ]\n}\n");
    return fclose(fp) ? -1 : 0;
}

static void trophy_load(TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp;
    char line[256];

    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));

    fp = fopen(filepath, "r");
    if (!fp)
        return; /* no saved data, all locked */

    /*
     * Simple JSON parser -- look for "id": N and "timestamp": T patterns.
     * This avoids any external JSON library dependency.
     */
    while (fgets(line, sizeof(line), fp)) {
        const char* id_pos = strstr(line, "\"id\":");
        if (id_pos) {
            u32 tid = 0;
            u64 ts = 0;
            const char* ts_pos;

            if (sscanf(id_pos, "\"id\": %u", &tid) == 1 &&
                tid < SCE_NP_TROPHY_MAX_NUM_TROPHIES) {
                ts_pos = strstr(line, "\"timestamp\":");
                if (ts_pos)
                    sscanf(ts_pos, "\"timestamp\": %llu",
                           (unsigned long long*)&ts);
                ctx->unlocked[tid] = 1;
                ctx->unlock_time[tid] = ts;
            }
        }
    }
    fclose(fp);

    printf("[sceNpTrophy] Loaded trophy data from %s\n", filepath);
}

static u32 trophy_count_unlocked(TrophyContext* ctx)
{
    u32 count = 0;
    for (u32 i = 0; i < ctx->total_trophies; i++)
        if (ctx->unlocked[i]) count++;
    return count;
}

/* The trophy installation state, from what the toolkit keeps: the store file
 * written for the community id. RPCS3 sceNpTrophy.cpp
 * sceNpTrophyRegisterContext/GetRequiredDiskSpace take it from the existence
 * of the installed trophy directory; the firmware leaves it to the VSH
 * (FW 4.93 libsysutil_np_trophy RegisterContext 0x02001614, status of the
 * first reply). A store path that exists but cannot be classified (not a
 * regular file, unreadable directory) leaves the state unknown: refuse
 * instead of guessing INSTALLED. */
static int trophy_install_state(const SceNpCommunicationId* commId, u32* status)
{
    char filepath[1024];
    struct stat st;

    trophy_get_filepath(commId, filepath, sizeof(filepath));
    if (stat(filepath, &st) == 0) {
        if ((st.st_mode & S_IFMT) == S_IFREG) {
            *status = SCE_NP_TROPHY_STATUS_INSTALLED;
            return 0;
        }
        printf("[sceNpTrophy] trophy store %s is not a regular file\n", filepath);
        return -1;
    }
    if (errno == ENOENT) {
        *status = SCE_NP_TROPHY_STATUS_NOT_INSTALLED;
        return 0;
    }
    printf("[sceNpTrophy] cannot establish the install state of %s (errno %d)\n",
           filepath, errno);
    return -1;
}

/* ---------------------------------------------------------------------------
 * Registration lifecycle
 *
 * Provenance of each part:
 *  - FW 4.93 libsysutil_np_trophy / libsysutil, as lifted in
 *    TS-FW-PACKETREAD-opus.md (A2, A3): RegisterContext blocks in its second
 *    round trip to the VSH (0x02001614..0x02001B68; the wait loop
 *    0x02003F30..0x02003F58 calls only cellSysutilPacketRead, which sleeps on
 *    a condition variable the VSH signals and never runs a dispatcher). The
 *    VSH posts the status events to the process's sysutil event queue
 *    (capacity 127) as id 299, data2 = ctx<<32|status, data3 =
 *    completed<<32|total. They run only inside cellSysutilCheckCallback, on
 *    whichever thread calls it, which drains everything posted so far in one
 *    call (0x02004E98); the dispatcher (0x020005E4 -> 0x0200049C) calls
 *    cb(ctx, status, completed, total, arg) and discards its return value.
 *    Never on the blocked thread. A call made on the thread that pumps does
 *    not deadlock: the wait does not depend on a poll, the events wait in the
 *    queue.
 *  - RPCS3-oracle (sceNpTrophy.cpp sceNpTrophyRegisterContext, pinned
 *    e53ac18f): the sequence and the completed/total counts. The firmware
 *    does not show them (they are VSH-side): SETUP 0..3 of 3, PROGRESS
 *    0..N-1 of N-1, FINALIZE 0..max(N-1,9)-5 of the same, COMPLETE 0 of 0,
 *    N the trophy count; one event queued per step of 2 s / events, the step
 *    waiting until a CheckCallback consumed the previous event or 50 ms more;
 *    never less than about 2 s over the whole sequence.
 *  - Heuristic (RPCS3 style; whether the console's VSH waits for consumption
 *    is not known): the call returns only after a CheckCallback has consumed
 *    COMPLETE, a later CheckCallback has begun (the consumer polls its
 *    mailbox after each one, on the same thread, so the previous frame's
 *    reading is done) and one step of vblanks has passed, as RPCS3's step waits
 *    for the consumer. A title that retires its registration thread the moment
 *    the call returns and clears the mailbox its status callback fills has by
 *    then seen the completion; returning earlier is the measured failure behind
 *    #37. A vblank edge alone is not enough: the completion can land a
 *    microsecond before one.
 *
 * Pacing is counted in vblanks (cellGcmGetVBlankCount), the guest-visible time
 * base, not in wall-clock time: a deterministic rendering of RPCS3's rules, so
 * a run and its replay do not depend on the host's speed. 2 s at the
 * console's 59.94 Hz is 120 vblanks over the events (one vblank a step with
 * the usual few hundred events); RPCS3's 50 ms grace is 3 vblanks.
 *
 * Mechanism: a queue and a clock, no thread of our own. posted counts the
 * events the "VSH" has queued: event k is due at vblank v0 + k * step, and
 * whichever thread looks first (the blocked caller, or a poll) queues what is
 * due, never more than the queue holds (a full queue holds the VSH back: the
 * firmware's behaviour on overflow is unknown, so none is invented). The poll
 * hook, run from cellSysutilCheckCallback on the polling thread, drains the
 * queue in order, one callback at a time process-wide. The caller only
 * waits (condition variable, lock released); the wait is bounded by
 * trophy.register_wait_cap_vblanks (RPCS3's worst case), and at the cap the
 * call returns success, as the firmware does when it does not hang, counting
 * and logging it; the lifecycle stays owned by the context and a later poll
 * delivers the rest. An unticking vblank counter would otherwise be a silent
 * hang: that is the gcm-wait-timeout stop.
 * -----------------------------------------------------------------------*/

extern u32 cellGcmGetVBlankCount(void);

#define TROPHY_PACE_TOTAL_VBLANKS   120u   /* RPCS3-oracle: 2 s minimum at 59.94 Hz */
#define TROPHY_EVENT_QUEUE_CAP      127u   /* FW 4.93 sysutil event queue (A2.6) */
#define TROPHY_WAIT_SLICE_US       4000u   /* re-check the vblank counter at least this often */

#define TROPHY_STATUS_SEGMENTS 4

/* RPCS3-oracle counts; see the block comment above. */
static void life_segments(u32 trophies, u32 seg[TROPHY_STATUS_SEGMENTS])
{
    const u32 n = trophies ? trophies - 1 : 0;
    seg[0] = 3;                        /* SETUP */
    seg[1] = n;                        /* PROGRESS */
    seg[2] = (n > 9 ? n : 9) - 5;      /* FINALIZE */
    seg[3] = 0;                        /* COMPLETE */
}

static u32 life_step_vblanks(u32 events)
{
    const u32 p = (TROPHY_PACE_TOTAL_VBLANKS + events - 1) / events;
    return p ? p : 1;
}

static u32 life_event_count(u32 trophies)
{
    u32 seg[TROPHY_STATUS_SEGMENTS], n = 0;
    life_segments(trophies, seg);
    for (int i = 0; i < TROPHY_STATUS_SEGMENTS; i++) n += seg[i] + 1;
    return n;
}

static void life_event_at(u32 trophies, u32 idx,
                          u32* status, u32* completed, u32* total)
{
    u32 seg[TROPHY_STATUS_SEGMENTS];
    life_segments(trophies, seg);
    for (int i = 0; i < TROPHY_STATUS_SEGMENTS; i++) {
        if (idx <= seg[i]) {
            *status = SCE_NP_TROPHY_STATUS_PROCESSING_SETUP + (u32)i;
            *completed = idx;
            *total = seg[i];
            return;
        }
        idx -= seg[i] + 1;
    }
    /* idx is always below life_event_count(); keep the last event on a slip. */
    *status = SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE;
    *completed = 0;
    *total = 0;
}

static void trophy_require_bridge(const char* who)
{
    /* RPCS3 sceNpTrophy.cpp sceNpTrophyRegisterContext() dispatches statusCb;
     * this HLE has no guest callback bridge, so it is a named stop. */
    if (!g_ps3_guest_caller)
        ps3_stop(PS3_STOP_UNRESOLVED_GUEST_CALLBACK, who,
                 "set g_ps3_guest_caller before calling the trophy API",
                 NULL, 0, NULL);
}

/* Lock held. Ends the context's lifecycle: queued statuses are dropped and
 * the registration thread, if it is waiting, is woken to return. */
static void life_cancel_locked(TrophyContext* c)
{
    c->life_serial = 0;
    c->life_posted = 0;
    c->life_delivered = 0;
    c->life_complete_seen = 0;
    life_wake();
}

/* Lock held. After a context is cancelled, a status callback already running
 * on another thread must finish before the caller returns, so no callback
 * runs after Destroy/Term returns. A callback that destroys its own context
 * runs on this thread and is not waited for. */
static void life_quiesce_locked(void)
{
    while (s_life_delivering && !trophy_tid_eq(s_life_deliverer, trophy_self()))
        life_wait(TROPHY_WAIT_SLICE_US);
}

/* ---------------------------------------------------------------------------
 * Handle abort
 *
 * The library keeps no abort state. FW 4.93 libsysutil_np_trophy
 * sceNpTrophyAbortHandle 0x020014C4 checks the handle (0x0200025C), sends the
 * VSH an "abort" request {mem_context = handle} (PacketBegin channel 0, type
 * 0x127, 0x02001500..0x020015A8), waits for the reply (0x020015C0 ->
 * 0x02003DE4) and returns its "result" when negative, else 0 (0x02003EC8..
 * 0x02003F00). It stores nothing in the library's tables.
 *
 * The state is the VSH's (vsh.elf FW 4.93). Its abort handler 0x001909DC
 * resolves the handle with 0x0018DC94 and stores 1 in the entry's flag byte
 * (+8, 0x00190A2C); the result it answers is 0. Every handle-taking request
 * resolves the handle through the same 0x0018DC94, whose callee 0x0018DC0C
 * reads that byte and answers 0x80022910 (ABORT) while it is set (0x0018DC4C..
 * 0x0018DC6C). The destroy request ('9', 0x0018F110 -> 0x0018DDC0) resolves
 * the entry itself, never reads the byte, and frees the entry and its flag
 * (0x0018DD28, 0x0018DD94). So until the handle is destroyed, every operation
 * naming it answers ABORT, a second AbortHandle included, and DestroyHandle
 * still succeeds. RPCS3 d00d8023 sceNpTrophy.cpp sceNpTrophyAbortHandle sets
 * trophy_handle_t::is_aborted and get_context_ex() answers ABORT from it, the
 * same, except that RPCS3's second AbortHandle answers CELL_OK.
 *
 * Proven by the VSH walk (dossier ABORT-HANDLE.md 3.3, 3.4): an abort before
 * RegisterContext's second round trip (tag 'i', handler 0x001902A4: lookup
 * 0x0018DC94 at 0x00190360, error path 0x001905C8 storing 0x80022910 at
 * 0x001905D8) answers ABORT, and the library returns that result as is
 * (0x02003F00, 0x020019C4..0x020019CC). An abort while the call waits in that
 * round trip runs the cancel step 0x0018E22C: the session is held then, so it
 * signals the handle's object (0x0018E288), which the 'i' handler sleeps on,
 * and clears the byte again (0x0018E290), so the handle is not left marked.
 * What the woken call returns is NOT determined: nothing re-checks the byte
 * after the wake and the result is the install engine's return value
 * (0x00190598..0x001905BC, callee 0x00172CAC), which could be ABORT through
 * the mapper at 0x0018F6DC or CELL_OK. The toolkit does not invent it: the
 * woken wait is the named stop np-trophy-unmodelled. The slot-299 status event
 * the handler posts itself after the wake (0x0018F1EC) is not delivered: the
 * stop comes first. The cancel step's other trigger, a short session-holding
 * request in flight (CreateHandle and friends), is not modelled: those calls
 * hold no session here.
 *
 * The flag is read by threads other than the one that sets it (a registration
 * thread blocked in RegisterContext), so it lives under s_life_lock.
 * -----------------------------------------------------------------------*/

/* FW 4.93 libsysutil_np_trophy 0x0200025C: an id outside 1..4 is
 * INVALID_ARGUMENT (0x02000264..0x02000288), one the table does not hold
 * UNKNOWN_HANDLE (0x0200028C..0x020002B0). It tests no init flag: the table
 * is empty before Init and after Term (0x02000BC8 clears it), so ids 1..4 are
 * UNKNOWN_HANDLE there. RPCS3 sceNpTrophyAbortHandle/DestroyHandle split the
 * same way. */
static s32 trophy_handle_lookup(SceNpTrophyHandle handle)
{
    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;
    return CELL_OK;
}

/* ABORT when `handle` names a live handle AbortHandle marked (VSH 0x0018DC0C),
 * else CELL_OK. The operations that call this do not validate the handle
 * themselves (they ignore it), so an id that names nothing is the caller's
 * business and answers CELL_OK here: only an aborted handle changes their
 * answer. */
static s32 trophy_handle_abort_rc(SceNpTrophyHandle handle)
{
    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES)
        return CELL_OK;
    life_lock();
    const int aborted = s_handles[handle].in_use && s_handles[handle].aborted;
    life_unlock();
    return aborted ? (s32)SCE_NP_TROPHY_ERROR_ABORT : CELL_OK;
}

/* Lock held. Queue what is due at vblank `vb` and fits in the queue. */
static void life_post_locked(TrophyContext* c, u32 vb)
{
    const u32 events = life_event_count(c->total_trophies);
    const u32 step = life_step_vblanks(events);
    u64 due = 1u + (u64)(u32)(vb - c->life_v0) / step;
    if (due > events) due = events;
    const u32 room = TROPHY_EVENT_QUEUE_CAP - (c->life_posted - c->life_delivered);
    u32 n = due > c->life_posted ? (u32)(due - c->life_posted) : 0;
    if (n > room) n = room;
    c->life_posted += n;
}

/* Run by every cellSysutilCheckCallback (cellSysutil_register_poll_hook), on
 * the polling thread: queue what is due, then drain the queue in order, one
 * callback at a time process-wide. A poll made from inside a status callback
 * finds one running and delivers nothing. */
static void trophy_poll(void)
{
    life_lock();
    if (s_life_delivering) { life_unlock(); return; }
    s_life_polls++;
    for (;;) {
        TrophyContext* c = NULL;
        s32 id = 0;
        const u32 vb = cellGcmGetVBlankCount();
        for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
            TrophyContext* k = &s_contexts[i];
            if (!k->in_use || !k->life_serial) continue;
            life_post_locked(k, vb);
            if (!c && k->life_delivered < k->life_posted) { c = k; id = i; }
        }
        if (!c) break;
        u32 status, completed, total;
        life_event_at(c->total_trophies, c->life_delivered,
                      &status, &completed, &total);
        const u64 serial = c->life_serial;
        const u32 cb = c->life_cb, arg = c->life_arg;
        s_life_delivering = 1;
        s_life_deliverer = trophy_self();
        life_unlock();

        trophy_require_bridge("a trophy status is due but there is no guest callback bridge");
        /* FW dispatch 0x0200049C discards the callback's return value here. */
        g_ps3_guest_caller(cb, (u32)id, status, completed, total, arg, 0, 0, 0);
        /* Milestones for a run's log: the first status and the completion. */
        if (status == SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE ||
            (status == SCE_NP_TROPHY_STATUS_PROCESSING_SETUP && !completed))
            printf("[sceNpTrophy] status %u delivered to ctx=%d at vblank %u\n",
                   status, id, cellGcmGetVBlankCount());

        life_lock();
        s_life_delivering = 0;
        if (c->life_serial == serial) {
            c->life_delivered++;
            if (c->life_delivered == life_event_count(c->total_trophies)) {
                c->life_complete_seen = 1;
                c->life_complete_v = cellGcmGetVBlankCount();
                c->life_complete_poll = s_life_polls;
            }
        }
        life_wake();
    }
    life_unlock();
}

/* Lock held. The waiting RegisterContext of lifecycle `serial` leaves its
 * wait; a newer call on the same context keeps its own entry. */
static void life_leave_wait_locked(TrophyContext* c, u64 serial)
{
    if (c->life_wait_serial == serial) {
        c->life_wait_handle = 0;
        c->life_wait_serial = 0;
    }
}

/* The abort woke a RegisterContext that was waiting: the VSH cancel step
 * signalled the object the registration sleeps on (0x0018E288) and the result
 * the call then returns is the install engine's (0x001905BC), which the
 * firmware does not show. Never reached with s_life_lock held. */
static PS3_NORETURN void trophy_stop_abort_during_registration(s32 context,
                                                                SceNpTrophyHandle handle)
{
    char h[16], c[16];
    snprintf(h, sizeof h, "%d", handle);
    snprintf(c, sizeof c, "%d", context);
    const ps3_stop_field f[] = {
        { "func", "sceNpTrophyRegisterContext" },
        { "handle", h },
        { "context", c },
        { "reason", "abort-during-registration" },
    };
    ps3_stop(PS3_STOP_NP_TROPHY_UNMODELLED,
             "sceNpTrophyAbortHandle woke a sceNpTrophyRegisterContext that was waiting for the VSH's install, and what the firmware then returns to that call is not determined",
             "Do not abort a handle while its registration runs, or decode the VSH install engine's result (vsh 0x001905BC) and model it.",
             f, sizeof f / sizeof f[0], NULL);
}

/* The wait of sceNpTrophyRegisterContext: until the completion has been
 * consumed and one step has passed, or the cap. Returns CELL_OK, ABORT when
 * the handle was already marked as the wait began, or the error of a context
 * that was destroyed or re-registered meanwhile. An AbortHandle that arrives
 * while it waits is the named stop (see "Handle abort"). */
static s32 trophy_run_lifecycle(s32 context, SceNpTrophyHandle handle, u64 serial)
{
    TrophyContext* c = &s_contexts[context];
    const u32 cap = ps3_cfg_trophy_register_wait_cap_vblanks();
    const u32 gcm_timeout_ms = ps3_cfg_gcm_wait_timeout_ms();
    life_lock();
    c->life_wait_handle = handle;
    c->life_wait_serial = serial;
    c->life_abort_hit = 0;
    const u32 events = life_event_count(c->total_trophies);
    const u32 step = life_step_vblanks(events);
    const u32 start_vb = cellGcmGetVBlankCount();
    u32 seen_vb = start_vb;
    u64 seen_at = trophy_clock_real();
    for (;;) {
        if (c->life_serial != serial) {
            const s32 rc = c->in_use ? (s32)SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED
                                     : (s32)SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
            life_leave_wait_locked(c, serial);
            life_unlock();
            return rc;
        }
        if (c->life_abort_hit) {
            life_leave_wait_locked(c, serial);
            c->life_abort_hit = 0;
            life_unlock();
            trophy_stop_abort_during_registration(context, handle);
        }
        /* The handle was marked before the wait began: the request of round
         * trip 2 meets the mark (VSH 0x00190360) and answers ABORT. The
         * registration fails as the firmware does on any failure
         * (0x02001B10..0x02001B4C zeroes the context's callback and argument):
         * the lifecycle ends and no status runs after the call returns. */
        if (s_handles[handle].in_use && s_handles[handle].aborted) {
            life_cancel_locked(c);
            c->registered = 0;
            life_leave_wait_locked(c, serial);
            life_quiesce_locked();
            life_unlock();
            return (s32)SCE_NP_TROPHY_ERROR_ABORT;
        }
        const u32 vb = cellGcmGetVBlankCount();
        life_post_locked(c, vb);
        if (c->life_complete_seen && s_life_polls > c->life_complete_poll &&
            (u32)(vb - c->life_complete_v) >= step)
            break;
        if ((u32)(vb - start_vb) >= cap) {
            s_register_wait_caps++;
            const u32 posted = c->life_posted, delivered = c->life_delivered;
            life_leave_wait_locked(c, serial);
            life_unlock();
            printf("[sceNpTrophy] register-wait-cap ctx=%d posted=%u consumed=%u events=%u "
                   "waited_vblanks=%u cap=%u: returning success, a later poll delivers the rest\n",
                   context, posted, delivered, events, (u32)(vb - start_vb), cap);
            return CELL_OK;
        }
        /* A vblank counter that stands still would hold this thread for ever:
         * that is an RSX wait nobody ended. */
        const u64 now = trophy_clock_real();
        if (vb != seen_vb) { seen_vb = vb; seen_at = now; }
        else if (now - seen_at >= (u64)gcm_timeout_ms * 1000u) {
            life_unlock();
            ps3_stop_gcm_wait_timeout("sceNpTrophyRegisterContext", "trophy-vblank", 0,
                                      (u32)context, vb, (u32)((now - seen_at) / 1000u));
        }
        life_wait(TROPHY_WAIT_SLICE_US);
    }
    life_leave_wait_locked(c, serial);
    life_unlock();
    return CELL_OK;
}

u32 sceNpTrophy_register_wait_cap_count(void)
{
    life_lock();
    const u32 n = s_register_wait_caps;
    life_unlock();
    return n;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

void sceNpTrophySetStoragePath(const char* path)
{
    path = GUEST_PTR(path, const char*);
    if (path) {
        strncpy(s_storage_path, path, sizeof(s_storage_path) - 1);
        s_storage_path[sizeof(s_storage_path) - 1] = '\0';
        s_storage_path_set = 1;
    }
}

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options)
{
    (void)poolPtr; (void)poolSize; (void)containerId; (void)options;

    printf("[sceNpTrophy] Init()\n");

    /* RPCS3 sceNpTrophy.cpp sceNpTrophyInit: re-init ->
     * ALREADY_INITIALIZED. */
    if (s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED;

    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 1;
    return CELL_OK;
}

s32 sceNpTrophyTerm(void)
{
    printf("[sceNpTrophy] Term()\n");

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* Save all registered contexts */
    for (int i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (s_contexts[i].in_use && s_contexts[i].registered)
            trophy_save(&s_contexts[i]);
    }

    life_lock();
    for (int i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++)
        life_cancel_locked(&s_contexts[i]);
    life_quiesce_locked();
    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 0;
    life_unlock();
    return CELL_OK;
}

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options)
{
    (void)options;

    /* RPCS3 sceNpTrophy.cpp sceNpTrophyCreateContext checks commSign before
     * the init state; the firmware's INVALID_ARGUMENT is 0x80022906. */
    if (!commSign)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    /* RPCS3 sceNpTrophy.cpp sceNpTrophyCreateContext: not initialized ->
     * NOT_INITIALIZED. */
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* RPCS3 sceNpTrophy.cpp sceNpTrophyCreateContext: !context || !commId ->
     * INVALID_ARGUMENT. */
    if (!context || !commId)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    const SceNpCommunicationId* commId_h = GUEST_PTR(commId, const SceNpCommunicationId*);

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (!s_contexts[i].in_use) {
            memset(&s_contexts[i], 0, sizeof(TrophyContext));
            s_contexts[i].in_use = 1;
            s_contexts[i].commId = *commId_h;
            s_contexts[i].total_trophies = SCE_NP_TROPHY_MAX_NUM_TROPHIES;
            vm_write32(GUEST_EA(context), (u32)i);
            printf("[sceNpTrophy] CreateContext(commId=\"%s\") -> ctx=%d\n",
                   commId_h->data, i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTrophyDestroyContext(SceNpTrophyContext context)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* RPCS3 sceNpTrophyDestroyContext: an id outside the table is
     * INVALID_ARGUMENT, one the table does not hold UNKNOWN_CONTEXT. */
    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;

    if (s_contexts[context].registered)
        trophy_save(&s_contexts[context]);

    life_lock();
    life_cancel_locked(&s_contexts[context]);
    s_contexts[context].in_use = 0;
    life_quiesce_locked();
    life_unlock();
    printf("[sceNpTrophy] DestroyContext(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle)
{
    /* RPCS3 sceNpTrophy.cpp sceNpTrophyCreateHandle: !init -> NOT_INITIALIZED. */
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* RPCS3 sceNpTrophy.cpp sceNpTrophyCreateHandle: !handle ->
     * INVALID_ARGUMENT. */
    if (!handle)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) {
            s_handles[i].in_use = 1;
            vm_write32(GUEST_EA(handle), (u32)i);
            printf("[sceNpTrophy] CreateHandle() -> handle=%d\n", i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* RPCS3 sceNpTrophyDestroyHandle: the same split for the handle. */
    const s32 rc = trophy_handle_lookup(handle);
    if (rc != CELL_OK)
        return rc;

    /* The VSH frees the entry and its abort flag (0x0018DD28, 0x0018DD94). */
    life_lock();
    s_handles[handle].in_use = 0;
    s_handles[handle].aborted = 0;
    life_unlock();
    printf("[sceNpTrophy] DestroyHandle(handle=%d)\n", handle);
    return CELL_OK;
}

s32 sceNpTrophyAbortHandle(SceNpTrophyHandle handle)
{
    /* FW 4.93 libsysutil_np_trophy 0x020014C4 goes straight to the handle
     * check 0x0200025C; unlike RegisterContext (0x02001678) and CreateHandle
     * (0x02000D44) it never reads the init flag 0x020001A4. RPCS3 leaves the
     * init check out of sceNpTrophyAbortHandle too (a TODO there). */
    const s32 rc = trophy_handle_lookup(handle);
    if (rc != CELL_OK)
        return rc;

    life_lock();
    if (s_handles[handle].aborted) {
        /* VSH 0x0018DC94 -> 0x0018DC0C answers ABORT for a handle that is
         * already marked, and the abort request returns that as its result
         * (0x00190A4C); RPCS3 answers CELL_OK here. */
        life_unlock();
        return SCE_NP_TROPHY_ERROR_ABORT;
    }
    /* VSH 0x00190A2C marks, then the cancel step 0x0018E22C runs: with a
     * session in flight (here: a RegisterContext waiting) it signals the
     * handle's object and clears the mark again (0x0018E290), else the mark
     * stays. A woken registration is the named stop (see "Handle abort"). */
    int inflight = 0;
    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++)
        if (s_contexts[i].life_wait_handle) inflight = 1;
    if (inflight) {
        for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++)
            if (s_contexts[i].life_wait_handle == handle) s_contexts[i].life_abort_hit = 1;
    } else {
        s_handles[handle].aborted = 1;
    }
    life_wake();                         /* a registration thread re-reads it */
    life_unlock();
    printf("[sceNpTrophy] AbortHandle(handle=%d)\n", handle);
    return CELL_OK;                      /* VSH result 0, 0x00190A40 */
}

s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               u32 statusCb,
                               u32 arg,
                               u64 options)
{
    /* RPCS3 sceNpTrophy.cpp sceNpTrophyRegisterContext: !init -> NOT_INITIALIZED. */
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* RPCS3 sceNpTrophy.cpp get_context_ex(): a context outside [1, MAX] ->
     * INVALID_ARGUMENT; an idm miss -> UNKNOWN_CONTEXT (FW 4.93
     * RegisterContext: 0x80022906 / 0x8002290A). */
    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    /* The table miss: SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT (0x8002290A). */
    if (!s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;

    /* RPCS3 sceNpTrophy.cpp get_context_ex(): the same split for the handle. */
    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    /* The table miss: SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE (0x80022911). */
    if (!s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;

    /* RPCS3 sceNpTrophy.cpp sceNpTrophyRegisterContext: !statusCb ->
     * INVALID_ARGUMENT. */
    if (!statusCb) return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    /* Only options 0 and 1 (show the error and exit on an abort) exist: FW 4.93
     * RegisterContext 0x02001664 area, RPCS3 options > SHOW_ERROR_EXIT. */
    if (options > 1) return SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;

    /* An aborted handle is the VSH's answer to the request, after every check
     * the library makes itself (0x0018DC0C inside the VSH lookup); RPCS3
     * get_context_ex() answers it before the callback and options checks. */
    {
        const s32 arc = trophy_handle_abort_rc(handle);
        if (arc) return arc;
    }

    trophy_require_bridge("sceNpTrophyRegisterContext has no guest callback bridge");

    /* The status the first callback reports follows the install state
     * (RPCS3: trophy directory exists -> INSTALLED, else NOT_INSTALLED). An
     * unknown state is an I/O error, never an assumed INSTALLED. */
    u32 status0;
    if (trophy_install_state(&s_contexts[context].commId, &status0) != 0)
        return SCE_NP_TROPHY_ERROR_DISC_IO;

    /* Load any previously saved trophy data */
    trophy_load(&s_contexts[context]);
    s_contexts[context].registered = 1;

    {
        char record[1024];
        trophy_get_filepath(&s_contexts[context].commId, record, sizeof(record));
        printf("[sceNpTrophy] RegisterContext(ctx=%d, handle=%d, statusCb=0x%08X, "
               "arg=0x%08X) status0=%s: install record %s %s\n",
               context, handle, statusCb, arg,
               status0 == SCE_NP_TROPHY_STATUS_INSTALLED ? "INSTALLED" : "NOT_INSTALLED",
               record, status0 == SCE_NP_TROPHY_STATUS_INSTALLED ? "exists" : "absent");
    }

    /* The inline "ask permission" callback, on the caller thread. FW 4.93
     * RegisterContext 0x020018E0..0x020018EC / RPCS3 statusCb(...) < 0: a
     * negative return clears the registration and fails the call. */
    const s32 cb_rc = (s32)(u32)ppu_guest_call(statusCb, (u32)context, status0,
                                               0, 0, arg, 0, 0, 0);
    if (cb_rc < 0) {
        if (s_contexts[context].in_use) s_contexts[context].registered = 0;
        return SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED;
    }
    /* RPCS3 sceNpTrophy.cpp sceNpTrophyRegisterContext() re-resolves the
     * context and handle after the callback (:660/:666); the codes are
     * 0x8002290A / 0x80022911. */
    if (!s_contexts[context].in_use) return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
    if (!s_handles[handle].in_use) return SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;
    /* ... and AbortHandle called from the callback (RPCS3 :647-653, "such as
     * if AbortHandle was called"). The registration did not complete. */
    {
        const s32 arc = trophy_handle_abort_rc(handle);
        if (arc) {
            if (s_contexts[context].in_use) s_contexts[context].registered = 0;
            return arc;
        }
    }

    /* The install: the store file now exists, so the next registration of
     * this community id reports INSTALLED. RPCS3 maps a failed install to
     * ILLEGAL_UPDATE. */
    if (status0 != SCE_NP_TROPHY_STATUS_INSTALLED &&
        trophy_save(&s_contexts[context]) != 0) {
        s_contexts[context].registered = 0;
        return SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE;
    }

    /* Take ownership of a fresh lifecycle; one still in flight for this
     * context is superseded and its registration thread returns. */
    life_lock();
    const u64 serial = s_life_next_serial++;
    life_cancel_locked(&s_contexts[context]);
    s_contexts[context].life_serial = serial;
    s_contexts[context].life_cb = statusCb;
    s_contexts[context].life_arg = arg;
    s_contexts[context].life_v0 = cellGcmGetVBlankCount();
    life_unlock();
    if (cellSysutil_register_poll_hook(trophy_poll) != CELL_OK) {
        life_lock();
        life_cancel_locked(&s_contexts[context]);
        life_unlock();
        s_contexts[context].registered = 0;
        return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
    }

    const s32 rc = trophy_run_lifecycle(context, handle, serial);
    printf("[sceNpTrophy] RegisterContext(ctx=%d) -> 0x%08X\n", context, (u32)rc);
    return rc;
}

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options)
{
    (void)options;

    /* RPCS3 sceNpTrophy.cpp sceNpTrophyGetRequiredDiskSpace checks reqspace
     * before the init state. */
    if (!reqSpace)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    /* RPCS3 sceNpTrophy.cpp sceNpTrophyGetRequiredDiskSpace: !init ->
     * NOT_INITIALIZED. */
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* RPCS3 sceNpTrophy.cpp get_context_ex(): range -> INVALID_ARGUMENT. */
    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    /* The table miss: SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT (0x8002290A). */
    if (!s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
    {
        const s32 arc = trophy_handle_abort_rc(handle);   /* see "Handle abort" */
        if (arc) return arc;
    }

    reqSpace = GUEST_PTR(reqSpace, u64*);

    /* RPCS3 sceNpTrophy.cpp sceNpTrophyGetRequiredDiskSpace: 0 once the trophy
     * data is installed; otherwise the pack size, for which the toolkit has
     * no TRP and keeps its fixed 1 MB. The guest is big-endian and reads this
     * u64 back in BE order, so the value MUST be byte-swapped on write --
     * otherwise a host-LE 0x100000 is read as 0x0010000000000000 (2^44). LBP
     * feeds this straight into its save-data free-space accounting
     * (sub_379EC0: errB = budget - usedKB - reqSpaceKB); a garbage 2^44 makes
     * errB underflow negative -> cellGame GameData check returns ERROR-B ->
     * boot bails. (Same LE-write class as the cellUserInfo fix.) */
    u32 install;
    if (trophy_install_state(&s_contexts[context].commId, &install) != 0)
        return SCE_NP_TROPHY_ERROR_DISC_IO;
    const u64 space = install == SCE_NP_TROPHY_STATUS_INSTALLED ? 0 : 1024 * 1024;
    *reqSpace = ps3_bswap64(space);
    printf("[sceNpTrophy] GetRequiredDiskSpace(ctx=%d) -> %llu\n", context,
           (unsigned long long)space);
    return CELL_OK;
}

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
    {
        const s32 arc = trophy_handle_abort_rc(handle);   /* see "Handle abort" */
        if (arc) return arc;
    }

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
    details = GUEST_PTR(details, SceNpTrophyGameDetails*);
    data = GUEST_PTR(data, SceNpTrophyGameData*);

    if (details) {
        memset(details, 0, sizeof(SceNpTrophyGameDetails));
        details->numTrophies = 32; /* default trophy set */
        details->numPlatinum = 1;
        details->numGold     = 2;
        details->numSilver   = 8;
        details->numBronze   = 21;
        strncpy(details->title, "PS3 Game",
                SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE - 1);
        strncpy(details->description, "Trophy set",
                SCE_NP_TROPHY_GAME_DESC_MAX_SIZE - 1);
    }

    if (data) {
        memset(data, 0, sizeof(SceNpTrophyGameData));
        data->unlockedTrophies = trophy_count_unlocked(&s_contexts[context]);
        /* Count by grade would require per-trophy grade data;
         * return conservative estimates */
        data->unlockedPlatinum = 0;
        data->unlockedGold     = 0;
        data->unlockedSilver   = 0;
        data->unlockedBronze   = data->unlockedTrophies;
    }

    printf("[sceNpTrophy] GetGameInfo(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
    {
        const s32 arc = trophy_handle_abort_rc(handle);   /* see "Handle abort" */
        if (arc) return arc;
    }

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (trophyId < 0 || (u32)trophyId >= s_contexts[context].total_trophies)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    details = GUEST_PTR(details, SceNpTrophyDetails*);
    data = GUEST_PTR(data, SceNpTrophyData*);

    if (details) {
        memset(details, 0, sizeof(SceNpTrophyDetails));
        details->trophyId = (u32)trophyId;
        details->trophyGrade = SCE_NP_TROPHY_GRADE_BRONZE;
        snprintf(details->name, SCE_NP_TROPHY_NAME_MAX_SIZE,
                 "Trophy %d", trophyId);
        snprintf(details->description, SCE_NP_TROPHY_DESC_MAX_SIZE,
                 "Trophy #%d description", trophyId);
        details->hidden = 0;
    }

    if (data) {
        memset(data, 0, sizeof(SceNpTrophyData));
        data->trophyId = (u32)trophyId;
        data->unlocked = s_contexts[context].unlocked[trophyId];
        data->timestamp = s_contexts[context].unlock_time[trophyId];
    }

    return CELL_OK;
}

s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
    {
        const s32 arc = trophy_handle_abort_rc(handle);   /* see "Handle abort" */
        if (arc) return arc;
    }

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (trophyId < 0 || (u32)trophyId >= s_contexts[context].total_trophies)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    if (s_contexts[context].unlocked[trophyId])
        return SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED;

    /* Unlock the trophy */
    s_contexts[context].unlocked[trophyId] = 1;
    s_contexts[context].unlock_time[trophyId] = (u64)time(NULL);

    printf("************************************************************\n");
    printf("*  TROPHY UNLOCKED!  Trophy #%d                            \n",
           trophyId);
    printf("*  Context: %d  CommId: %s                                 \n",
           context, s_contexts[context].commId.data);
    printf("************************************************************\n");

    /* Save immediately */
    trophy_save(&s_contexts[context]);

    /* Check if all non-platinum trophies are unlocked -> platinum */
    if (platinumId) {
        vm_write32((u32)(uintptr_t)platinumId, (u32)SCE_NP_TROPHY_INVALID_TROPHY_ID);
        /* Simplified: don't auto-award platinum without grade info */
    }

    return CELL_OK;
}

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count)
{
    /* RPCS3 sceNpTrophy.cpp sceNpTrophyGetTrophyUnlockState checks the output
     * pointers before the init state. */
    if (!flags || !count)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    /* RPCS3 sceNpTrophy.cpp sceNpTrophyGetTrophyUnlockState: !init ->
     * NOT_INITIALIZED. */
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* RPCS3 sceNpTrophy.cpp get_context_ex(): range -> INVALID_ARGUMENT. */
    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    /* The table miss: SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT (0x8002290A). */
    if (!s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;

    {
        const s32 arc = trophy_handle_abort_rc(handle);   /* see "Handle abort" */
        if (arc) return arc;
    }

    /* SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED (0x80022904). */
    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    u32 flags_ea = (u32)(uintptr_t)flags;
    for (u32 o = 0; o < sizeof(SceNpTrophyFlagArray); o += 4)
        vm_write32(flags_ea + o, 0);

    for (u32 i = 0; i < s_contexts[context].total_trophies; i++) {
        if (s_contexts[context].unlocked[i])
            {
            u32 word_ea = flags_ea + (i / 32) * 4;
            vm_write32(word_ea, vm_read32(word_ea) | (1u << (i % 32)));
        }
    }

    vm_write32((u32)(uintptr_t)count, (u32)s_contexts[context].total_trophies);
    return CELL_OK;
}

s32 sceNpTrophyGetGameProgress(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               s32* percentage)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
    {
        const s32 arc = trophy_handle_abort_rc(handle);   /* see "Handle abort" */
        if (arc) return arc;
    }

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!percentage)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 total = s_contexts[context].total_trophies;
    u32 unlocked = trophy_count_unlocked(&s_contexts[context]);

    s32 pct = total > 0 ? (s32)((unlocked * 100) / total) : 0;
    vm_write32(GUEST_EA(percentage), (u32)pct);

    printf("[sceNpTrophy] GetGameProgress(ctx=%d) -> %d%%\n",
           context, pct);
    return CELL_OK;
}

/* clang -std=gnu17 -I include libs/network/tests/test_trophy_registration.c
 * libs/system/cellSysutil.c -Wl,-dead_strip -pthread -o /tmp/test_trophy_registration
 *
 * Trophy registration contract, as a title meets it:
 *   - a registration thread calls sceNpTrophyRegisterContext, which blocks;
 *   - a separate thread (the "main" thread) pumps cellSysutilCheckCallback
 *     once a vblank and reads a guest mailbox {valid, status, completed,
 *     total} the status callback fills, as a title does;
 *   - the registration thread clears the mailbox's valid byte when the call
 *     returns (a title retiring its registration worker).
 * The first status follows the install state; a negative first-callback return
 * aborts; the processing statuses (setup, progress, finalize, complete) are
 * queued gradually, paced by vblanks, and run only from cellSysutilCheckCallback
 * on the polling thread, in order; the call returns only after the completion
 * was consumed; a wait no poll serves ends at the counted cap with success and
 * the events are drained later, in order; destroying the context ends the
 * delivery; a vblank counter that never ticks is the named stop gcm-wait-timeout.
 *
 * The guest is a synthetic callback and the vblank counter a stub the test
 * advances, so no case sleeps to order events. Run with a case name ("status0",
 * "abort", "paced", "short", "interleave", "drain", "samethread", "destroy",
 * "threads", "novblank", "guards") to run one.
 */
#undef NDEBUG
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../sceNpTrophy.c"
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void cellMsgDialog_tick(void) {}
void np_psnr_pump(void) {}
void ps3_stop(ps3_stop_kind k, const char* w, const char* r,
              const ps3_stop_field* f, size_t n, const ps3_stop_site* s)
{ (void)k; (void)w; (void)r; (void)f; (void)n; (void)s; abort(); }

/* The vblank counter. g_vb_auto > 0 makes every read advance it, for a thread
 * that waits with nobody ticking. */
static _Atomic u32 g_vblank, g_vb_auto;
u32 cellGcmGetVBlankCount(void)
{ return atomic_fetch_add(&g_vblank, atomic_load(&g_vb_auto)); }
static void vblank_tick(void) { atomic_fetch_add(&g_vblank, 1); }

static _Atomic u32 g_cap_vblanks = 1024, g_gcm_timeout_ms = 10000;
const ps3_config* ps3_config_get(void)
{
    static ps3_config cfg;
    cfg.trophy_register_wait_cap_vblanks = atomic_load(&g_cap_vblanks);
    cfg.gcm_wait_timeout_ms = atomic_load(&g_gcm_timeout_ms);
    return &cfg;
}

/* A stop on the registration thread ends that thread, as the process-ending
 * stop would, but leaves the fixture to check what it said. */
static _Atomic int g_stopped;
static char g_stop_func[64], g_stop_what[32];
void ps3_stop_gcm_wait_timeout(const char* func, const char* what, uint32_t offset,
                               uint32_t value, uint32_t current, uint32_t waited_ms)
{
    (void)offset; (void)value; (void)current; (void)waited_ms;
    snprintf(g_stop_func, sizeof g_stop_func, "%s", func);
    snprintf(g_stop_what, sizeof g_stop_what, "%s", what);
    atomic_store(&g_stopped, 1);
    pthread_exit(NULL);
}

/* ---- the synthetic guest ------------------------------------------------ */

#define CB_OPD 0x300u
#define CB_ARG 0xCAFEu
/* The statuses the lifecycle produces for a 128-trophy context: setup 0..3 of
 * 3, progress 0..127 of 127, finalize 0..122 of 122, complete 0 of 0; plus the
 * one synchronous status. */
#define N_EVENTS (4 + 128 + 123 + 1)

typedef struct { u32 ctx, status, completed, total; u32 vblank; pthread_t tid; } Ev;
static Ev g_log[N_EVENTS + 8];
static _Atomic unsigned g_nlog;
static _Atomic int g_in_cb, g_max_in_cb;
static unsigned g_max_pending;
static s32 g_sync_ret;               /* what the callback returns for status 1..4 */

/* The title's mailbox. valid is the byte the registration thread clears. */
static struct { _Atomic int valid; u32 status, completed, total; } g_mbox;

static u64 guest_dispatch(u32 opd, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                          u64 a5, u64 a6, u64 a7)
{
    assert(opd == CB_OPD && a4 == CB_ARG && !a5 && !a6 && !a7);
    int now = atomic_fetch_add(&g_in_cb, 1) + 1;
    int mx = atomic_load(&g_max_in_cb);
    while (now > mx && !atomic_compare_exchange_weak(&g_max_in_cb, &mx, now)) {}
    unsigned i = atomic_load(&g_nlog);
    assert(i < sizeof(g_log) / sizeof(g_log[0]));
    g_log[i] = (Ev){(u32)a0, (u32)a1, (u32)a2, (u32)a3, atomic_load(&g_vblank), pthread_self()};
    /* The queue never holds more than the firmware's 127 events. */
    life_lock();
    unsigned pending = s_contexts[1].life_posted - s_contexts[1].life_delivered;
    life_unlock();
    if (pending > g_max_pending) g_max_pending = pending;
    g_mbox.status = (u32)a1; g_mbox.completed = (u32)a2; g_mbox.total = (u32)a3;
    atomic_store(&g_mbox.valid, 1);
    atomic_fetch_sub(&g_in_cb, 1);
    atomic_store(&g_nlog, i + 1);
    /* The processing statuses' return value is discarded by the firmware: a
     * guest that returns negative there must not stop the lifecycle. */
    return a1 >= SCE_NP_TROPHY_STATUS_PROCESSING_SETUP ? (u64)(s64)-1 : (u64)(s64)g_sync_ret;
}
u64 ppu_guest_call(u32 opd, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7)
{ return guest_dispatch(opd, a0, a1, a2, a3, a4, a5, a6, a7); }
static void void_caller(u32 opd, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7)
{ (void)guest_dispatch(opd, a0, a1, a2, a3, a4, a5, a6, a7); }

static char g_root[64];
static void store_path(const char* comm, char* out, size_t n)
{ snprintf(out, n, "%s/%s.json", g_root, comm); }
static int store_exists(const char* comm)
{ char p[256]; store_path(comm, p, sizeof(p)); return access(p, F_OK) == 0; }
static void store_remove(const char* comm)
{ char p[256]; store_path(comm, p, sizeof(p)); unlink(p); rmdir(p); }
static void store_create(const char* comm)
{
    char p[256]; store_path(comm, p, sizeof(p));
    FILE* f = fopen(p, "w"); assert(f); fputs("{\n  \"trophies\": [\n  ]\n}\n", f); fclose(f);
}

static int g_last_valid_status;    /* the last status seen with the valid byte set */
static void wait_log(unsigned n);
/* Until the registration thread has made its inline callback and taken
 * ownership of its lifecycle. */
static void wait_started(void)
{
    wait_log(1);
    for (unsigned i = 0; ; i++) {
        assert(i < 50000000u);
        life_lock();
        const int started = s_contexts[1].life_serial != 0;
        life_unlock();
        if (started) return;
        sched_yield();
    }
}

static void begin(const char* comm)
{
    memset(g_log, 0, sizeof(g_log)); atomic_store(&g_nlog, 0);
    g_mbox.status = g_mbox.completed = g_mbox.total = 0; atomic_store(&g_mbox.valid, 0);
    g_sync_ret = 1; g_max_pending = 0; g_last_valid_status = -1;
    atomic_store(&g_max_in_cb, 0);
    atomic_store(&g_stopped, 0);
    atomic_store(&g_vblank, 1000); atomic_store(&g_vb_auto, 0);
    atomic_store(&g_cap_vblanks, 100000); atomic_store(&g_gcm_timeout_ms, 10000);
    memset(vm_base + 0x100, 0, 0x40);
    memcpy(vm_base + 0x100, comm, strlen(comm) + 1);
    g_ps3_guest_caller = void_caller;
    assert(sceNpTrophyInit(NULL, 0, 0, 0) == CELL_OK);
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, (void*)0x108, 0) == CELL_OK);
    assert(sceNpTrophyCreateHandle((void*)0x204) == CELL_OK);
}
static void end(void) { assert(sceNpTrophyTerm() == CELL_OK); }

/* RegisterContext blocks, so it runs on its own thread; the test thread is the
 * one that polls. Like a title retiring its registration worker, the thread
 * clears the mailbox's valid byte the moment the call returns. */
typedef struct { s32 ctx, handle; s32 rc; _Atomic int done; pthread_t th; } Reg;
static void* reg_main(void* p)
{
    Reg* r = p;
    r->rc = sceNpTrophyRegisterContext(r->ctx, r->handle, CB_OPD, CB_ARG, 0);
    atomic_store(&g_mbox.valid, 0);
    atomic_store(&r->done, 1);
    return NULL;
}
static void reg_start(Reg* r)
{
    r->ctx = 1; r->handle = 1; r->rc = -1; atomic_store(&r->done, 0);
    assert(pthread_create(&r->th, NULL, reg_main, r) == 0);
}
static void reg_join(Reg* r) { assert(pthread_join(r->th, NULL) == 0); }

static unsigned poll_once(void)
{
    unsigned before = atomic_load(&g_nlog);
    assert(cellSysutilCheckCallback() == CELL_OK);
    return atomic_load(&g_nlog) - before;
}
static void wait_log(unsigned n)       /* until the registration thread made n callbacks */
{
    for (unsigned i = 0; atomic_load(&g_nlog) < n; i++) { assert(i < 50000000u); sched_yield(); }
}
/* One vblank, one poll: until the log holds n callbacks. */
static void frames_until_log(unsigned n)
{
    for (unsigned i = 0; atomic_load(&g_nlog) < n; i++) {
        assert(i < 50000000u);
        vblank_tick(); poll_once(); sched_yield();
    }
}

/* The consumer: what a title's progress poll does after each CheckCallback,
 * on the pumping thread. */
/* g_last_valid_status is declared above. */
static void consume(void)
{
    if (atomic_load(&g_mbox.valid)) {
        g_last_valid_status = (int)g_mbox.status;
        atomic_store(&g_mbox.valid, 0);
    }
}
/* A "frame" per vblank until the registration call returned: the vblank
 * ticks, the title polls, then reads its mailbox. */
static void drive(Reg* r, void (*frame)(void))
{
    for (unsigned i = 0; !atomic_load(&r->done); i++) {
        assert(i < 50000000u);
        vblank_tick();
        poll_once();
        if (frame) frame(); else consume();
        sched_yield();
    }
}

static void expect_sequence(unsigned total_logged, u32 trophies, const pthread_t* reg_thread)
{
    /* log[0] is the synchronous status, on the registering thread; log[1..] the
     * lifecycle, only ever from a poll on another thread. */
    const u32 n = trophies - 1, fin = (n > 9 ? n : 9) - 5;
    assert(total_logged == 1 + (4 + (n + 1) + (fin + 1) + 1));
    unsigned k = 1;
    for (u32 c = 0; c <= 3; c++, k++)
        assert(g_log[k].status == 5 && g_log[k].completed == c && g_log[k].total == 3);
    for (u32 c = 0; c <= n; c++, k++)
        assert(g_log[k].status == 6 && g_log[k].completed == c && g_log[k].total == n);
    for (u32 c = 0; c <= fin; c++, k++)
        assert(g_log[k].status == 7 && g_log[k].completed == c && g_log[k].total == fin);
    assert(g_log[k].status == 8 && !g_log[k].completed && !g_log[k].total);
    for (unsigned i = 0; i < total_logged; i++) assert(g_log[i].ctx == 1);
    if (reg_thread) {
        assert(pthread_equal(g_log[0].tid, *reg_thread));
        for (unsigned i = 1; i < total_logged; i++) assert(!pthread_equal(g_log[i].tid, *reg_thread));
    }
}

/* ---- cases -------------------------------------------------------------- */

/* The synchronous status follows the install state; an install state that
 * cannot be established is an error, not INSTALLED. */
static void case_status0(void)
{
    store_remove("NPWR00000"); store_remove("NPWR00001"); store_remove("NPWR00002");

    /* No store file: NOT_INSTALLED. A negative return stops the call before
     * any processing, so the case needs no lifecycle. */
    begin("NPWR00000");
    g_sync_ret = -1;
    assert(sceNpTrophyRegisterContext(1, 1, CB_OPD, CB_ARG, 0) ==
           (s32)SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED);
    assert(atomic_load(&g_nlog) == 1 && g_log[0].status == SCE_NP_TROPHY_STATUS_NOT_INSTALLED);
    assert(g_log[0].completed == 0 && g_log[0].total == 0);
    u64 space = 0;
    assert(sceNpTrophyGetRequiredDiskSpace(1, 1, (u64*)(uintptr_t)0x300, 0) == CELL_OK);
    memcpy(&space, vm_base + 0x300, 8);
    assert(ps3_bswap64(space) == 1024 * 1024);
    end();

    /* A store file: INSTALLED, and nothing more is required of the disk. */
    store_create("NPWR00001");
    begin("NPWR00001");
    g_sync_ret = -1;
    assert(sceNpTrophyRegisterContext(1, 1, CB_OPD, CB_ARG, 0) ==
           (s32)SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED);
    assert(atomic_load(&g_nlog) == 1 && g_log[0].status == SCE_NP_TROPHY_STATUS_INSTALLED);
    assert(sceNpTrophyGetRequiredDiskSpace(1, 1, (u64*)(uintptr_t)0x300, 0) == CELL_OK);
    memcpy(&space, vm_base + 0x300, 8);
    assert(space == 0);
    end();

    /* A store path that exists but is not a file: the state is unknown. The
     * call fails before the callback, with a firmware I/O error. */
    char p[256]; store_path("NPWR00002", p, sizeof(p));
    assert(mkdir(p, 0755) == 0);
    begin("NPWR00002");
    assert(sceNpTrophyRegisterContext(1, 1, CB_OPD, CB_ARG, 0) ==
           (s32)SCE_NP_TROPHY_ERROR_DISC_IO);
    assert(atomic_load(&g_nlog) == 0 && !s_contexts[1].registered);
    assert(sceNpTrophyGetRequiredDiskSpace(1, 1, (u64*)(uintptr_t)0x300, 0) ==
           (s32)SCE_NP_TROPHY_ERROR_DISC_IO);
    end();
    store_remove("NPWR00001"); store_remove("NPWR00002");
}

/* A negative return from the first callback aborts the registration: no
 * processing follows and nothing is installed. */
static void case_abort(void)
{
    store_remove("NPWR00003");
    begin("NPWR00003");
    g_sync_ret = -1;
    assert(sceNpTrophyRegisterContext(1, 1, CB_OPD, CB_ARG, 0) ==
           (s32)SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED);
    assert(!s_contexts[1].registered && !store_exists("NPWR00003"));
    for (int i = 0; i < 100; i++) { vblank_tick(); assert(poll_once() == 0); }
    assert(atomic_load(&g_nlog) == 1);
    /* Zero is not negative. */
    g_sync_ret = 0;
    Reg r; reg_start(&r); wait_started(); drive(&r, NULL); reg_join(&r);
    assert(r.rc == CELL_OK);
    end();
    store_remove("NPWR00003");
}

/* The title's pattern: the registration thread blocks, the main thread pumps
 * once a vblank and consumes, the registration thread clears the valid byte as
 * it returns. */
static void case_paced(void)
{
    store_remove("NPWR00004");
    begin("NPWR00004");
    Reg r; reg_start(&r);
    wait_started();
    assert(g_log[0].status == SCE_NP_TROPHY_STATUS_NOT_INSTALLED);
    /* The statuses are queued gradually: with the vblank standing still, polls
     * deliver the first one and nothing else. */
    unsigned n0 = 0;
    for (int i = 0; i < 50; i++) n0 += poll_once();
    assert(n0 == 1 && g_log[1].status == 5 && g_log[1].completed == 0);
    assert(!atomic_load(&r.done));

    /* The completion is delivered and read by the consumer; vblanks pass but no
     * later poll comes. The call must still wait for that poll: a vblank edge
     * alone can land a microsecond after the completion, before the consumer
     * has looked. (A negative check: the worker's wait re-reads the counter every
     * few milliseconds, so thirty of them pass.) */
    while (atomic_load(&g_nlog) < 1 + N_EVENTS) { vblank_tick(); poll_once(); consume(); sched_yield(); }
    atomic_fetch_add(&g_vblank, 5);
    { struct timespec ts = {0, 30 * 1000 * 1000}; nanosleep(&ts, NULL); }
    assert(!atomic_load(&r.done));
    drive(&r, NULL);

    /* The call returned: the completion, and everything before it, was consumed. */
    assert(r.rc == CELL_OK);
    assert(atomic_load(&g_nlog) == 1 + N_EVENTS);
    expect_sequence(atomic_load(&g_nlog), 128, &r.th);
    assert(atomic_load(&g_max_in_cb) == 1);
    /* One status a vblank; the completion comes 255 vblanks after the first
     * (2 s at 59.94 Hz over the 256 events is under one vblank a step, so the
     * rate is the vblank's), and never in the poll that delivered the first. */
    for (unsigned k = 2; k <= N_EVENTS; k++) assert(g_log[k].vblank == g_log[k - 1].vblank + 1);
    assert(g_log[N_EVENTS].vblank - g_log[1].vblank == N_EVENTS - 1);
    /* The consumer's last valid status is the completion, though the
     * registration thread has cleared the byte since. */
    assert(g_last_valid_status == 8);
    assert(sceNpTrophy_register_wait_cap_count() == 0);
    assert(store_exists("NPWR00004"));   /* installed */
    reg_join(&r);
    /* No more once complete. */
    for (int i = 0; i < 50; i++) { vblank_tick(); assert(poll_once() == 0); }
    end();

    /* The install persisted: the same community id now starts INSTALLED. */
    begin("NPWR00004");
    g_sync_ret = -1;
    assert(sceNpTrophyRegisterContext(1, 1, CB_OPD, CB_ARG, 0) ==
           (s32)SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED);
    assert(g_log[0].status == SCE_NP_TROPHY_STATUS_INSTALLED);
    end();
    store_remove("NPWR00004");
}

/* With few trophies there are few statuses, so the 2 s the sequence takes at
 * 59.94 Hz (120 vblanks) is spread over them: each waits for its step. */
static void case_short(void)
{
    store_remove("NPWR00010");
    begin("NPWR00010");
    s_contexts[1].total_trophies = 1;      /* setup 4, progress 1, finalize 5, complete 1 */
    Reg r; reg_start(&r);
    wait_started();
    poll_once();                           /* the first status, at the starting vblank */
    drive(&r, NULL);
    assert(r.rc == CELL_OK);
    expect_sequence(atomic_load(&g_nlog), 1, &r.th);
    const unsigned events = atomic_load(&g_nlog) - 1;
    assert(events == 11);
    const u32 step = (120u + events - 1) / events;       /* 11 vblanks */
    for (unsigned i = 2; i <= events; i++) assert(g_log[i].vblank - g_log[i - 1].vblank == step);
    assert(g_last_valid_status == 8);
    reg_join(&r);
    end();
    store_remove("NPWR00010");
}

/* A consumer that resets its mailbox-valid byte at a phase boundary after the
 * first statuses still gets a valid completion afterwards. */
static unsigned g_reset_at;
static int g_saw_complete;
static void interleave_frame(void)
{
    if (!g_reset_at && atomic_load(&g_nlog) >= 2) {   /* the phase boundary */
        atomic_store(&g_mbox.valid, 0);
        g_reset_at = atomic_load(&g_nlog);
    }
    if (atomic_load(&g_mbox.valid)) {                 /* the consumer's poll */
        if (g_mbox.status == SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE) {
            assert(g_reset_at && atomic_load(&g_nlog) > g_reset_at);
            g_saw_complete = 1;
        }
        atomic_store(&g_mbox.valid, 0);
    }
}
static void case_interleave(void)
{
    store_remove("NPWR00005");
    begin("NPWR00005");
    g_reset_at = 0; g_saw_complete = 0;
    Reg r; reg_start(&r);
    wait_started();
    drive(&r, interleave_frame);
    reg_join(&r);
    assert(g_reset_at && g_saw_complete);
    end();
    store_remove("NPWR00005");
}

/* CheckCallback drains the whole queue in one call; a consumer that stalled
 * finds what was posted meanwhile, in order, never more than the 127 slots of
 * the firmware's queue at once. */
static void case_drain(void)
{
    store_remove("NPWR00011");
    begin("NPWR00011");
    Reg r; reg_start(&r);
    wait_started();
    /* 200 vblanks pass with no poll. */
    atomic_fetch_add(&g_vblank, 200);
    const unsigned got = poll_once();
    assert(got == 201);                    /* due: 1 + 200 */
    assert(g_max_pending <= 127);
    for (unsigned k = 1; k <= got; k++) assert(g_log[k].status >= 5 && g_log[k].status <= 7);
    assert(g_log[1].status == 5 && g_log[4].status == 5 && g_log[5].status == 6 && g_log[5].completed == 0);
    assert(!atomic_load(&r.done));
    drive(&r, NULL);
    reg_join(&r);
    assert(r.rc == CELL_OK);
    expect_sequence(atomic_load(&g_nlog), 128, &r.th);
    assert(sceNpTrophy_register_wait_cap_count() == 0);
    end();
    store_remove("NPWR00011");
}

/* The same thread registers and pumps. The call does not deadlock (the
 * firmware's wait does not depend on a poll): it returns at the counted cap
 * with success, and the events, which waited in the queue, are drained in order
 * by the first poll after. */
static void case_samethread(void)
{
    store_remove("NPWR00012");
    begin("NPWR00012");
    atomic_store(&g_cap_vblanks, 200);
    atomic_store(&g_vb_auto, 8);           /* time passes while the thread waits */
    assert(sceNpTrophyRegisterContext(1, 1, CB_OPD, CB_ARG, 0) == CELL_OK);
    assert(sceNpTrophy_register_wait_cap_count() == 1);
    assert(atomic_load(&g_nlog) == 1);                 /* nothing ran on the blocked thread */
    assert(s_contexts[1].life_posted == 127 && s_contexts[1].life_delivered == 0);
    const unsigned got = poll_once();                  /* one poll drains it all, in order */
    assert(got == N_EVENTS);
    expect_sequence(atomic_load(&g_nlog), 128, NULL);
    /* The poll thread is not the registering thread here, so check the order only. */
    assert(g_max_pending <= 127);
    assert(poll_once() == 0);
    assert(sceNpTrophy_register_wait_cap_count() == 1);
    end();
    store_remove("NPWR00012");
}

/* Destroying the context mid-lifecycle ends the call and every callback. */
static void case_destroy(void)
{
    store_remove("NPWR00006");
    begin("NPWR00006");
    Reg r; reg_start(&r);
    wait_started();
    frames_until_log(7);
    assert(atomic_load(&g_nlog) == 7);
    assert(sceNpTrophyDestroyContext(1) == CELL_OK);
    reg_join(&r);
    assert(r.rc == (s32)SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT);
    const unsigned frozen = atomic_load(&g_nlog);
    for (int i = 0; i < 500; i++) { atomic_fetch_add(&g_vblank, 100); assert(poll_once() == 0); }
    assert(atomic_load(&g_nlog) == frozen);
    end();

    /* Term cancels the same way. */
    begin("NPWR00006");
    reg_start(&r);
    wait_started();
    frames_until_log(4);
    assert(sceNpTrophyTerm() == CELL_OK);
    reg_join(&r);
    assert(r.rc == (s32)SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT);
    const unsigned after_term = atomic_load(&g_nlog);
    for (int i = 0; i < 100; i++) { atomic_fetch_add(&g_vblank, 100); assert(poll_once() == 0); }
    assert(atomic_load(&g_nlog) == after_term);
    store_remove("NPWR00006");
}

/* Several threads polling at once still see one status at a time, in order. */
static _Atomic int g_stop_pollers;
static void* poller_main(void* p)
{
    (void)p;
    unsigned last = 0;
    while (!atomic_load(&g_stop_pollers)) {
        cellSysutilCheckCallback();
        const unsigned n = atomic_load(&g_nlog);
        if (n != last) { last = n; vblank_tick(); } else sched_yield();
    }
    return NULL;
}
static void case_threads(void)
{
    store_remove("NPWR00007");
    begin("NPWR00007");
    atomic_store(&g_stop_pollers, 0);
    pthread_t pollers[3];
    Reg r; reg_start(&r);
    for (int i = 0; i < 3; i++) assert(pthread_create(&pollers[i], NULL, poller_main, NULL) == 0);
    for (unsigned i = 0; !atomic_load(&r.done); i++) { assert(i < 50000000u); sched_yield(); }
    atomic_store(&g_stop_pollers, 1);
    for (int i = 0; i < 3; i++) assert(pthread_join(pollers[i], NULL) == 0);
    reg_join(&r);
    assert(r.rc == CELL_OK);
    assert(atomic_load(&g_nlog) == 1 + N_EVENTS);
    expect_sequence(atomic_load(&g_nlog), 128, &r.th);
    assert(atomic_load(&g_max_in_cb) == 1);
    end();
    store_remove("NPWR00007");
}

/* A vblank counter that never ticks would hold the call for ever: that is an
 * RSX wait nobody ended, the gcm-wait-timeout stop, not a silent hang. */
static void case_novblank(void)
{
    store_remove("NPWR00009");
    begin("NPWR00009");
    atomic_store(&g_gcm_timeout_ms, 100);
    Reg r; reg_start(&r);
    reg_join(&r);                       /* the stop ended the thread */
    assert(atomic_load(&g_stopped));
    assert(!atomic_load(&r.done));      /* it did not return a result */
    assert(!strcmp(g_stop_func, "sceNpTrophyRegisterContext") && !strcmp(g_stop_what, "trophy-vblank"));
    assert(atomic_load(&g_nlog) == 1);  /* only the synchronous status ran */
    end();
    store_remove("NPWR00009");
}

/* Argument and state checks that precede the lifecycle. */
static void case_guards(void)
{
    begin("NPWR00008");
    /* RPCS3 get_context_ex(): an out-of-range id is INVALID_ARGUMENT, a table
     * miss is UNKNOWN_CONTEXT/UNKNOWN_HANDLE (the SDK's INVALID_*). */
    assert(sceNpTrophyRegisterContext(0, 1, CB_OPD, CB_ARG, 0) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyRegisterContext(5, 1, CB_OPD, CB_ARG, 0) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyRegisterContext(1, 0, CB_OPD, CB_ARG, 0) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyRegisterContext(1, 5, CB_OPD, CB_ARG, 0) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyRegisterContext(1, 2, CB_OPD, CB_ARG, 0) == SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE);
    assert(sceNpTrophyRegisterContext(1, 1, 0, CB_ARG, 0) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyGetRequiredDiskSpace(0, 1, (u64*)(uintptr_t)0x300, 0) ==
           SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyGetRequiredDiskSpace(1, 1, NULL, 0) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyGetTrophyUnlockState(1, 1, NULL, (u32*)(uintptr_t)0x304) ==
           SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyGetTrophyUnlockState(0, 1, (void*)(uintptr_t)0x300, (u32*)(uintptr_t)0x304) ==
           SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(atomic_load(&g_nlog) == 0);
    end();
}

int main(int argc, char** argv)
{
    static const struct { const char* name; void (*fn)(void); } cases[] = {
        {"status0", case_status0}, {"abort", case_abort}, {"paced", case_paced},
        {"short", case_short}, {"interleave", case_interleave}, {"drain", case_drain},
        {"samethread", case_samethread}, {"destroy", case_destroy},
        {"threads", case_threads}, {"novblank", case_novblank}, {"guards", case_guards},
    };
    strcpy(g_root, "/tmp/ps3recomp-trophy-XXXXXX");
    assert(mkdtemp(g_root));
    vm_base = calloc(1, 65536); assert(vm_base);
    strcpy((char*)vm_base + 0x400, g_root);
    sceNpTrophySetStoragePath((void*)0x400);
    int ran = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && strcmp(argv[1], cases[i].name)) continue;
        cases[i].fn(); ran++;
        printf("case %s ok\n", cases[i].name);
    }
    assert(ran);
    rmdir(g_root); free(vm_base);
    puts("Trophy registration follows the install state and queues paced processing statuses");
    return 0;
}

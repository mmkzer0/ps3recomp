/* The usleep-site seam (ppu.poll_sleep_coalesce, runtime/syscalls/sys_timer.{h,c}): a title registers the return
 * address of a usleep poll and a callback that returns the deadline the poll ends at; sys_timer_usleep from that
 * site then sleeps once, to the deadline. No case asserts an upper bound on wall time: where a case needs "the
 * handler ran during the sleep" it waits for events (the deadline callback ran, a delivery happened, the sleeper
 * returned), never for a window a loaded runner can miss (cf. toolkit #146). The only measurements are `timing`
 * (stress tier) and the CPU-time modes, whose margins are wide.
 *
 *   site      option on, default spin. A matching call: the callback runs once, the call returns with the host
 *             clock (and the guest clock, host_monotonic_ns()/1000) at or past the deadline, the seam counts it.
 *             A call from another lr: the callback is not consulted and the ordinary usleep runs. A callback
 *             answer of 0, a deadline already passed, or one beyond the registered bound: the ordinary usleep, and
 *             the last of these names itself once on stderr, however many times it happens.
 *   wake      a GCM notification posted while the seam sleeps is pumped before the sleep returns, and the sleep
 *             then runs on to the deadline; work published before the seam armed is pumped on entry; posts at
 *             varied instants after the call began are never left undelivered. Gated on the deadline callback
 *             (the call has begun) and on the delivery itself.
 *   guard     a notification that wakes the sleeper while another thread's pump holds the delivery guard: the
 *             sleeper's pump fails (gcm_pump_contended counts it), the owner then releases the guard, and the work
 *             is delivered before the sleeper's deadline without another post or HLE call.
 *   guard-ring the same with a user command (ring, no pending bit; the title's 0xEB00 path) posted while the
 *             owner is parked past its last pop (gcm_set_pump_tail_hook): only the owner's work-generation check
 *             after it released the guard can wake the sleeper.
 *   contract  the seam serves one sleeping call at a time: with one held inside its deadline callback, a second
 *             call at the site takes the ordinary usleep without running its callback and names the contention
 *             once; after the first returns the site works again.
 *   off       option off: a registered site is ignored, the callback never runs, nothing is pumped.
 *   spin0     option on, ppu.poll_sleep_spin_us=0: a pure sleep, whose thread CPU time stays far below the span.
 *   spin2000  spin 2000 us: the last 2 ms are spent spinning, so the thread CPU time is not far below it.
 *   register  the registration table's refusals (lr 0, bound 0, no callback, a duplicate, a ninth site).
 *   timing    stress: 200 sleeps to a 16.68 ms deadline; overshoot printed, p90 bounded.
 *
 * The GCM side is the real one (cellGcmSys.c is in the runtime library): the test installs a guest caller and
 * handlers and posts with cellGcmQueueUserCommand and cellGcm_vblank_signal, so the waker and the pump the seam
 * uses are the ones a title runs.
 *
 * Usage: test_usleep_site <site|wake|guard|contract|off|spin0|spin2000|register|timing>; ctest sets the options in
 * the environment (every test names them: the on-runs set them, the others unset them). */
#define _DARWIN_C_SOURCE 1
#ifndef _WIN32
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "sys_timer.h"
#include "ps3emu/config.h"
#include "ps3emu/guest_call.h"
#include "cellGcmSys.h"

void cellGcm_vblank_signal(void);   /* cellGcmSys.c: the vblank itself, as the title's clock thread raises it */
void ppu_gcm_pump(void);

/* The runtime defines vm_base in ppu_loader.cpp, which a host without a lifted title does not link. The vblank
 * handler's registration reads its descriptor word, so there is a zeroed window to read; nothing else uses it. */
uint8_t* vm_base = NULL;

static _Atomic int g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { atomic_fetch_add(&g_fails, 1); \
    fprintf(stderr, "FAIL %s:%d: %s ", __FILE__, __LINE__, #cond); fprintf(stderr, "" __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- the real GCM side ------------------------------------------------------------------------------------------
 * A user command queued by cellGcmQueueUserCommand (the path the title's FIFO takes, ~128 a second) or a vblank
 * raised by cellGcm_vblank_signal is published, the installed waker is called, and ppu_gcm_pump delivers it through
 * g_ps3_guest_caller to the handler registered with cellGcmSetUserHandler / cellGcmSetVBlankHandler. The "guest"
 * below counts those deliveries. */
extern ps3_guest_caller_fn g_ps3_guest_caller;

enum { USER_OPD = 0x100, VBLANK_OPD = 0x200 };

static atomic_uint g_posts;
static atomic_uint g_deliveries;          /* user handler runs */
static atomic_uint g_vblanks;             /* vblank handler runs */
static atomic_int  g_hold_vblank;         /* the next vblank handler run blocks until g_release */
static atomic_int  g_in_handler;
static atomic_int  g_release;

static void guest_caller(uint32_t opd, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                         uint64_t h, uint64_t i)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)h; (void)i;
    if (opd == VBLANK_OPD) {
        atomic_fetch_add(&g_vblanks, 1);
        if (atomic_exchange(&g_hold_vblank, 0)) {
            atomic_store(&g_in_handler, 1);
            while (!atomic_load(&g_release)) sched_yield();
        }
        return;
    }
    atomic_fetch_add(&g_deliveries, 1);
}

static void post(void)
{
    cellGcmQueueUserCommand(atomic_fetch_add(&g_posts, 1) + 1);
}

/* ---- the site ------------------------------------------------------------------------------------------------ */

enum { SITE_LR = 0x00123458u, OTHER_LR = 0x00123460u, SITE_BOUND_MS = 1000 };
static atomic_uint g_cb_calls;
static atomic_int  g_in_call;             /* set by the callback: the seam call has begun (it holds the seam) */
static atomic_int  g_returned;            /* set by the test after the call returned */
static atomic_int  g_gate_on;             /* the first caller's callback blocks (holding the seam) until cleared */
static atomic_int  g_gate_taken;

/* The title's callback reads guest words; this one reads the "deadline" a test left in r4. */
static uint64_t site_deadline(ppu_context* ctx)
{
    atomic_fetch_add(&g_cb_calls, 1);
    atomic_store(&g_in_call, 1);
    /* Only the first caller blocks, so a broken seam fails the checks instead of deadlocking the test. */
    if (atomic_load(&g_gate_on) && atomic_exchange(&g_gate_taken, 1) == 0)
        while (atomic_load(&g_gate_on)) sched_yield();
    return ctx->gpr[4];
}

static uint64_t g_tid_seq = 0x200;   /* contexts in no thread-table slot: ppu_thread_note_block ignores them */

typedef struct { ppu_context ctx; } sleeper;

static void sleeper_init(sleeper* s)
{
    memset(&s->ctx, 0, sizeof s->ctx);
    s->ctx.thread_id = __atomic_add_fetch(&g_tid_seq, 1, __ATOMIC_RELAXED);
}

/* A usleep(usec) from `lr` whose callback answers `deadline`. */
static void call(sleeper* s, uint32_t lr, uint64_t usec, uint64_t deadline)
{
    s->ctx.lr = lr;
    s->ctx.gpr[3] = usec;
    s->ctx.gpr[4] = deadline;
    const int64_t r = sys_timer_usleep(&s->ctx);
    CHECK(r == 0, "sys_timer_usleep = %lld", (long long)r);
}

/* The title's own deadline arithmetic: the guest clock is host_monotonic_ns()/1000 and its exit test is
 * `now - last > interval`, so the first passing read is last + interval + 1. */
static uint64_t deadline_in_us(uint64_t us)
{
    return (host_monotonic_ns() / 1000u + us + 1u) * 1000u;
}

static uint64_t thread_cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* The first 60 calls of sys_timer_usleep print a [WAIT] line and its diagnostic switches read their variables into
 * plain statics on the first call: use that up once with stderr silenced, from a lr no site owns. */
static void warm_up(void)
{
    const int saved = dup(2);
    const int null = open("/dev/null", O_WRONLY);
    if (saved >= 0 && null >= 0) dup2(null, 2);
    sleeper s; sleeper_init(&s);
    for (int i = 0; i < 64; i++) call(&s, OTHER_LR + 0x100u, 1, 0);
    if (saved >= 0) { dup2(saved, 2); close(saved); }
    if (null >= 0) close(null);
    atomic_store(&g_cb_calls, 0);
}

static void register_sites(void)
{
    CHECK(ps3_usleep_site_register(SITE_LR - 8u, 1000000u, site_deadline) == 0, "register a decoy");
    CHECK(ps3_usleep_site_register(SITE_LR, (uint64_t)SITE_BOUND_MS * 1000000u, site_deadline) == 0, "register");
}

static int count_lines_with(FILE* f, const char* needle)
{
    char line[1024]; int n = 0;
    rewind(f);
    while (fgets(line, sizeof line, f)) if (strstr(line, needle)) n++;
    return n;
}

/* ---- site ---------------------------------------------------------------------------------------------------- */

static void case_match(void)
{
    sleeper s; sleeper_init(&s);
    const uint64_t c0 = atomic_load(&g_cb_calls), s0 = sys_timer_usleep_site_count();
    const uint64_t t0 = host_monotonic_ns();
    const uint64_t d = deadline_in_us(5000);
    call(&s, SITE_LR, 30, d);
    const uint64_t t1 = host_monotonic_ns();
    CHECK(t1 >= d, "returned %llu ns before the deadline", (unsigned long long)(d - t1));
    CHECK(t1 / 1000u >= d / 1000u, "the guest clock reads %llu us, the deadline is %llu us",
          (unsigned long long)(t1 / 1000u), (unsigned long long)(d / 1000u));
    CHECK(t1 - t0 >= 4000000u, "slept %llu us for a 5 ms deadline", (unsigned long long)((t1 - t0) / 1000u));
    CHECK(atomic_load(&g_cb_calls) - c0 == 1, "the callback ran %u times", atomic_load(&g_cb_calls) - (unsigned)c0);
    CHECK(sys_timer_usleep_site_count() - s0 == 1, "the seam counted %llu sleeps",
          (unsigned long long)(sys_timer_usleep_site_count() - s0));
}

static void case_ordinary(void)
{
    sleeper s; sleeper_init(&s);
    uint64_t c0 = atomic_load(&g_cb_calls), s0 = sys_timer_usleep_site_count();
    uint64_t t0 = host_monotonic_ns();
    call(&s, OTHER_LR, 30, deadline_in_us(500000));            /* a site nobody registered */
    CHECK(host_monotonic_ns() - t0 >= 30000u, "usleep(30) from an unregistered lr slept less than 30 us");
    CHECK(atomic_load(&g_cb_calls) == c0, "the callback ran for an unregistered lr");
    CHECK(sys_timer_usleep_site_count() == s0, "the seam took an unregistered lr");

    c0 = atomic_load(&g_cb_calls);
    t0 = host_monotonic_ns();
    call(&s, SITE_LR, 30, 0);                                   /* the callback has no opinion */
    CHECK(host_monotonic_ns() - t0 >= 30000u, "usleep(30) after a 0 deadline slept less than 30 us");
    CHECK(atomic_load(&g_cb_calls) - c0 == 1, "the callback ran %u times for a 0 deadline", atomic_load(&g_cb_calls) - (unsigned)c0);
    CHECK(sys_timer_usleep_site_count() == s0, "the seam took a 0 deadline");

    t0 = host_monotonic_ns();
    call(&s, SITE_LR, 30, t0 - 1000000u);                       /* already passed: the ordinary 30 us */
    CHECK(host_monotonic_ns() - t0 >= 30000u, "usleep(30) after a past deadline slept less than 30 us");
    CHECK(sys_timer_usleep_site_count() == s0, "the seam took a deadline in the past");
}

static void case_bound(void)
{
    FILE* log = tmpfile();
    CHECK(log != NULL, "tmpfile");
    if (!log) return;
    const int saved = dup(2);
    dup2(fileno(log), 2);
    sleeper s; sleeper_init(&s);
    const uint64_t s0 = sys_timer_usleep_site_count();
    for (int i = 0; i < 3; i++) {
        const uint64_t t0 = host_monotonic_ns();
        call(&s, SITE_LR, 30, deadline_in_us(2000000));         /* 2 s away, the bound is 1 s */
        CHECK(host_monotonic_ns() - t0 >= 30000u, "usleep(30) beyond the bound slept less than 30 us");
    }
    fflush(stderr);
    dup2(saved, 2); close(saved);
    const int named = count_lines_with(log, "poll-sleep-bound");
    fclose(log);
    CHECK(sys_timer_usleep_site_count() == s0, "the seam took a deadline beyond its bound");
    CHECK(named == 1, "the bound was named %d times, not once", named);
}

static void case_sleep_until(void)
{
    uint64_t t0 = host_monotonic_ns();
    ps3_host_sleep_until_ns(t0 - 1000000u, 100000u);            /* passed: returns at once (no hang is the check) */
    t0 = host_monotonic_ns();
    const uint64_t d = t0 + 3000000u;
    ps3_host_sleep_until_ns(d, 100000u);
    CHECK(host_monotonic_ns() >= d, "ps3_host_sleep_until_ns returned early");
    const uint64_t d2 = host_monotonic_ns() + 2000000u;
    ps3_host_sleep_until_ns(d2, 5000000u);                      /* a spin longer than the whole wait: all spin */
    CHECK(host_monotonic_ns() >= d2, "ps3_host_sleep_until_ns returned early with spin > wait");
}

static int run_site(void)
{
    CHECK(ps3_cfg_ppu_poll_sleep_coalesce(), "ppu.poll_sleep_coalesce is off, the run expects on");
    register_sites();
    case_sleep_until();
    case_ordinary();
    case_match();
    case_bound();
    return 0;
}

/* ---- wake ---------------------------------------------------------------------------------------------------- */

/* The observer/poster of one round. It starts the moment the seam call has begun (the callback ran: the call holds
 * the seam and is about to arm), waits `spin` iterations to vary where in the call's start-up the post lands,
 * posts (or, with do_post 0, only watches work the test published before the call), and then waits for the
 * delivery or for the call to return. `before` is the delivery count when the round started. */
typedef struct { uint32_t spin; int do_post; unsigned before; int skipped; int delivered; uint64_t post_ns; } round_t;

static void* round_thread(void* arg)
{
    round_t* r = arg;
    while (!atomic_load(&g_in_call)) sched_yield();
    for (uint32_t i = 0; i < r->spin; i++) __asm__ volatile("" ::: "memory");
    if (atomic_load(&g_returned)) { r->skipped = 1; return NULL; }
    if (r->do_post) { r->post_ns = host_monotonic_ns(); post(); }
    while (atomic_load(&g_deliveries) == r->before && !atomic_load(&g_returned)) sched_yield();
    r->delivered = atomic_load(&g_deliveries) != r->before;
    return NULL;
}

/* One round: a seam call to a deadline `dl_us` away, with a post after it began (pre_post 0) or before it (1). The
 * handler must have run before the call returned. A post that the round's own thread made within 20 ms of the
 * deadline (that thread was not scheduled until then) is not asked to be delivered: only the sleeper's wake is
 * under test, not the poster's scheduling. */
static void wake_round(uint32_t spin, int pre_post, uint64_t dl_us, int idx)
{
    sleeper s; sleeper_init(&s);
    atomic_store(&g_in_call, 0);
    atomic_store(&g_returned, 0);
    round_t r = { .spin = spin, .do_post = !pre_post, .before = atomic_load(&g_deliveries) };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, round_thread, &r) == 0, "pthread_create");
    if (pre_post) post();
    const uint64_t d = deadline_in_us(dl_us);
    call(&s, SITE_LR, 30, d);
    const uint64_t t1 = host_monotonic_ns();
    atomic_store(&g_returned, 1);
    pthread_join(th, NULL);
    CHECK(t1 >= d, "round %d: the sleep ended %llu us before its deadline after a wake", idx,
          (unsigned long long)((d - t1) / 1000u));
    if (!r.skipped && (pre_post || r.post_ns + 20000000u <= d))
        CHECK(r.delivered, "round %d: %s, spin %u: not delivered before the call returned", idx,
              pre_post ? "work published before the call" : "a post after the call began", r.spin);
    ppu_gcm_pump();   /* whatever a late poster left, so the next round starts clean */
}

static int run_wake(void)
{
    CHECK(ps3_cfg_ppu_poll_sleep_coalesce(), "ppu.poll_sleep_coalesce is off, the run expects on");
    register_sites();
    wake_round(0, 1, 200000, -1);        /* published before the seam armed: the entry pump takes it */
    wake_round(0, 0, 300000, 0);         /* posted at the start of a 300 ms sleep: delivered, then the sleep runs on */
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    for (int i = 1; i <= 16; i++) {      /* varied instants of the call's start-up, Dekker's pairing as the target */
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        wake_round((uint32_t)((rng >> 8) % 100000u), 0, 150000, i);
    }
    /* The user-handler delivery counter (the R2 measure of PACER-DESIGN 3.2) saw every delivery the guest saw. */
    CHECK(atomic_load(&g_deliveries) >= 17, "only %u deliveries ran", atomic_load(&g_deliveries));
    CHECK(gcm_user_handler_deliveries() == atomic_load(&g_deliveries), "the counter says %u, the handler ran %u times",
          gcm_user_handler_deliveries(), atomic_load(&g_deliveries));
    return 0;
}

/* ---- guard --------------------------------------------------------------------------------------------------- */

static void* pump_owner_thread(void* arg)
{
    (void)arg;
    ppu_gcm_pump();      /* takes vblank 1, runs its handler, which holds the guard until the test releases it */
    return NULL;
}

static void* guard_sleeper_thread(void* arg)
{
    (void)arg;
    sleeper s; sleeper_init(&s);
    call(&s, SITE_LR, 30, deadline_in_us(900000));
    atomic_store(&g_returned, 1);
    return NULL;
}

/* The interleaving: A is delivering vblank 1 (guard held). Vblank 2 is posted. The sleeper B (the seam) pumps, and
 * fails the guard: the work stays pending. A then releases the guard. Nothing posts again and no HLE call
 * pumps: B has to be told by A's release (the pump's re-notification) or vblank 2 waits for the deadline. */
static int run_guard(void)
{
    CHECK(ps3_cfg_ppu_poll_sleep_coalesce(), "ppu.poll_sleep_coalesce is off, the run expects on");
    register_sites();
    atomic_store(&g_hold_vblank, 1);
    cellGcm_vblank_signal();
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, pump_owner_thread, NULL) == 0, "pthread_create");
    while (!atomic_load(&g_in_handler)) sched_yield();          /* A is inside the vblank handler: guard held */

    cellGcm_vblank_signal();                                    /* vblank 2: pending, and the guard is taken */
    const unsigned contended0 = gcm_pump_contended();
    CHECK(pthread_create(&b, NULL, guard_sleeper_thread, NULL) == 0, "pthread_create");
    while (gcm_pump_contended() == contended0) sched_yield();   /* B's pump found the guard held and left */
    CHECK(atomic_load(&g_vblanks) == 1, "vblank 2 was delivered while A held the guard");

    atomic_store(&g_release, 1);                                /* A leaves its handler and releases the guard */
    pthread_join(a, NULL);
    while (atomic_load(&g_vblanks) < 2 && !atomic_load(&g_returned)) sched_yield();
    CHECK(atomic_load(&g_vblanks) == 2, "vblank 2 was not delivered before the sleeper's deadline (the wake that "
          "found the guard held was consumed)");
    pthread_join(b, NULL);
    return 0;
}

/* ---- guard-ring ---------------------------------------------------------------------------------------------- */

static atomic_int g_hold_tail;

/* Runs inside ppu_gcm_pump after its last delivery, before the guard is released: the window in which a post is
 * invisible to the pump that holds the guard. Parks the first pump that gets here. */
static void tail_hook(void)
{
    if (atomic_exchange(&g_hold_tail, 0)) {
        atomic_store(&g_in_handler, 1);
        while (!atomic_load(&g_release)) sched_yield();
    }
}

/* The same interleaving as `guard`, with a user command (the ring, which sets no pending bit: the title's 0xEB00
 * path) posted while A is parked past its last pop. Nothing posts again and no HLE call pumps, so only the
 * pump's generation check, after it released the guard, can tell the sleeper. */
static int run_guard_ring(void)
{
    CHECK(ps3_cfg_ppu_poll_sleep_coalesce(), "ppu.poll_sleep_coalesce is off, the run expects on");
    register_sites();
    gcm_set_pump_tail_hook(tail_hook);
    atomic_store(&g_hold_tail, 1);
    post();                                                     /* command 1: A's pump delivers it */
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, pump_owner_thread, NULL) == 0, "pthread_create");
    while (!atomic_load(&g_in_handler)) sched_yield();          /* A delivered command 1 and is parked in the tail */
    CHECK(atomic_load(&g_deliveries) == 1, "command 1 was not delivered by A");

    post();                                                     /* command 2: the ring is empty to A's last pop */
    const unsigned contended0 = gcm_pump_contended();
    CHECK(pthread_create(&b, NULL, guard_sleeper_thread, NULL) == 0, "pthread_create");
    while (gcm_pump_contended() == contended0) sched_yield();   /* B's pump found the guard held and left */
    CHECK(atomic_load(&g_deliveries) == 1, "command 2 was delivered while A held the guard");

    atomic_store(&g_release, 1);
    pthread_join(a, NULL);
    while (atomic_load(&g_deliveries) < 2 && !atomic_load(&g_returned)) sched_yield();
    CHECK(atomic_load(&g_deliveries) == 2, "user command 2 was not delivered before the sleeper's deadline (the wake "
          "that found the guard held was consumed, and the owner did not re-notify)");
    pthread_join(b, NULL);
    gcm_set_pump_tail_hook(NULL);
    return 0;
}

/* ---- contract ------------------------------------------------------------------------------------------------ */

static void* held_thread(void* arg)
{
    (void)arg;
    sleeper s; sleeper_init(&s);
    call(&s, SITE_LR, 30, deadline_in_us(20000));
    return NULL;
}

static int run_contract(void)
{
    CHECK(ps3_cfg_ppu_poll_sleep_coalesce(), "ppu.poll_sleep_coalesce is off, the run expects on");
    register_sites();
    FILE* log = tmpfile();
    CHECK(log != NULL, "tmpfile");
    if (!log) return 1;

    atomic_store(&g_gate_on, 1);                    /* the first call stays inside its callback, holding the seam */
    pthread_t th;
    CHECK(pthread_create(&th, NULL, held_thread, NULL) == 0, "pthread_create");
    while (!atomic_load(&g_in_call)) sched_yield();
    const uint64_t c0 = atomic_load(&g_cb_calls), s0 = sys_timer_usleep_site_count();

    const int saved = dup(2);
    dup2(fileno(log), 2);
    sleeper s; sleeper_init(&s);
    for (int i = 0; i < 2; i++) {
        const uint64_t t0 = host_monotonic_ns();
        call(&s, SITE_LR, 30, deadline_in_us(100000));
        CHECK(host_monotonic_ns() - t0 >= 30000u, "usleep(30) of the second caller slept less than 30 us");
    }
    fflush(stderr);
    dup2(saved, 2); close(saved);
    CHECK(atomic_load(&g_cb_calls) == c0, "the second caller's callback ran while the seam was held");
    CHECK(sys_timer_usleep_site_count() == s0, "the second caller was served by the seam");
    CHECK(count_lines_with(log, "poll-sleep-contended") == 1, "the contention was named %d times, not once",
          count_lines_with(log, "poll-sleep-contended"));
    fclose(log);

    atomic_store(&g_gate_on, 0);
    pthread_join(th, NULL);
    CHECK(sys_timer_usleep_site_count() - s0 == 1, "the first caller was not served by the seam");

    const uint64_t c1 = atomic_load(&g_cb_calls), s1 = sys_timer_usleep_site_count();
    call(&s, SITE_LR, 30, deadline_in_us(3000));    /* the seam is free again */
    CHECK(atomic_load(&g_cb_calls) - c1 == 1 && sys_timer_usleep_site_count() - s1 == 1,
          "the seam did not serve the next call after the first returned");
    return 0;
}

/* ---- off ----------------------------------------------------------------------------------------------------- */

static int run_off(void)
{
    /* Something for a seam sleep to pump, if one ran. */
    post();
    CHECK(!ps3_cfg_ppu_poll_sleep_coalesce(), "ppu.poll_sleep_coalesce is on, the run expects off");
    register_sites();
    sleeper s; sleeper_init(&s);
    for (int i = 0; i < 3; i++) {
        const uint64_t t0 = host_monotonic_ns();
        call(&s, SITE_LR, 30, deadline_in_us(500000));
        CHECK(host_monotonic_ns() - t0 >= 30000u, "usleep(30) slept less than 30 us");
    }
    CHECK(atomic_load(&g_cb_calls) == 0, "the callback ran %u times with the option off", atomic_load(&g_cb_calls));
    CHECK(sys_timer_usleep_site_count() == 0, "the seam slept with the option off");
    CHECK(gcm_user_handler_deliveries() == 0, "a handler was delivered with the option off");
    CHECK(atomic_load(&g_deliveries) == 0, "the seam pumped with the option off");
    return 0;
}

/* ---- spin ---------------------------------------------------------------------------------------------------- */

static int run_spin(unsigned spin_us)
{
    CHECK(ps3_cfg_ppu_poll_sleep_spin_us() == spin_us, "ppu.poll_sleep_spin_us is %u, the run expects %u",
          (unsigned)ps3_cfg_ppu_poll_sleep_spin_us(), spin_us);
    register_sites();
    sleeper s; sleeper_init(&s);
    call(&s, SITE_LR, 30, deadline_in_us(1000));                /* warm the seam's first-use state */
    /* Thread CPU over a 10 ms sleep: a spin of S us costs about S us of it, a pure sleep almost none. Preemption
     * on a loaded host only lowers a sample (dev-ci 44277410272, macos-15 release: a 5-sample mean of 295 us
     * for a 2000 us spin), so a spin is proven by the largest sample and a pure sleep by the mean. */
    uint64_t cpu_total = 0, cpu_max = 0;
    enum { N = 10 };
    for (int i = 0; i < N; i++) {
        const uint64_t c0 = thread_cpu_ns();
        const uint64_t d = deadline_in_us(10000);
        call(&s, SITE_LR, 30, d);
        const uint64_t c = thread_cpu_ns() - c0;
        cpu_total += c;
        if (c > cpu_max) cpu_max = c;
        CHECK(host_monotonic_ns() >= d, "returned before the deadline");
    }
    const uint64_t cpu = cpu_total / N;
    fprintf(stderr, "spin %u us: %.0f us mean, %.0f us max of thread CPU per 10 ms sleep\n", spin_us, cpu / 1e3,
            cpu_max / 1e3);
    if (spin_us == 0)
        CHECK(cpu < 1500000u, "a pure sleep used %.0f us of CPU in 10 ms", cpu / 1e3);
    else
        CHECK(cpu_max > 400000u, "a %u us spin used at most %.0f us of CPU in %d sleeps", spin_us, cpu_max / 1e3,
              (int)N);
    return 0;
}

/* ---- register ------------------------------------------------------------------------------------------------ */

static int run_register(void)
{
    CHECK(ps3_usleep_site_register(0, 1000, site_deadline) == -1, "lr 0 accepted");
    CHECK(ps3_usleep_site_register(0x100, 0, site_deadline) == -1, "a bound of 0 accepted");
    CHECK(ps3_usleep_site_register(0x100, 1000, NULL) == -1, "a null callback accepted");
    for (uint32_t i = 1; i <= 8; i++)
        CHECK(ps3_usleep_site_register(0x1000u + i * 4u, 1000, site_deadline) == 0, "site %u refused", i);
    CHECK(ps3_usleep_site_register(0x1004, 1000, site_deadline) == -1, "a duplicate lr accepted");
    CHECK(ps3_usleep_site_register(0x2000, 1000, site_deadline) == -1, "a ninth site accepted");
    return 0;
}

/* ---- timing -------------------------------------------------------------------------------------------------- */

static int cmp_u64(const void* a, const void* b)
{
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

/* The design's frame-start jitter claim (PACER-DESIGN 3.2): with the 100 us margin the exit is at the deadline,
 * p99 10-56 us on the measuring host. p90 under 2 ms is the stress-tier bound (the 2 ms of section 4.1); an
 * ordinary host sits two orders below it. */
static int run_timing(int n)
{
    register_sites();
    uint64_t* over = malloc((size_t)n * sizeof *over);
    sleeper s; sleeper_init(&s);
    for (int i = 0; i < n; i++) {
        const uint64_t d = deadline_in_us(16683);
        call(&s, SITE_LR, 30, d);
        const uint64_t t = host_monotonic_ns();
        over[i] = t > d ? t - d : 0;
    }
    qsort(over, (size_t)n, sizeof *over, cmp_u64);
    fprintf(stderr, "timing: 16.68 ms x %d: overshoot p50 %.1f us  p90 %.1f us  p99 %.1f us  max %.1f us\n", n,
            over[n / 2] / 1e3, over[n * 9 / 10] / 1e3, over[n * 99 / 100] / 1e3, over[n - 1] / 1e3);
    CHECK(over[n * 9 / 10] < 2000000u, "p90 overshoot %.1f us is not under 2 ms", over[n * 9 / 10] / 1e3);
    free(over);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <site|wake|guard|contract|off|spin0|spin2000|register|timing> [count]\n", argv[0]);
        return 2;
    }
    vm_base = calloc(1, 1u << 20);
    warm_up();
    g_ps3_guest_caller = guest_caller;
    cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)USER_OPD);
    cellGcmSetVBlankHandler((CellGcmVBlankHandler)(uintptr_t)VBLANK_OPD);
    const char* mode = argv[1];
    if      (!strcmp(mode, "site"))     run_site();
    else if (!strcmp(mode, "wake"))     run_wake();
    else if (!strcmp(mode, "guard"))    run_guard();
    else if (!strcmp(mode, "guard-ring")) run_guard_ring();
    else if (!strcmp(mode, "contract")) run_contract();
    else if (!strcmp(mode, "off"))      run_off();
    else if (!strcmp(mode, "spin0"))    run_spin(0);
    else if (!strcmp(mode, "spin2000")) run_spin(2000);
    else if (!strcmp(mode, "register")) run_register();
    else if (!strcmp(mode, "timing"))   run_timing(argc > 2 ? atoi(argv[2]) : 200);
    else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }
    const int fails = atomic_load(&g_fails);
    fprintf(stderr, "%s %s: %s\n", argv[0], mode, fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
#else
int main(void) { return 0; }   /* Windows keeps the poll: there is no seam to test (sys_timer.h) */
#endif

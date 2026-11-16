/* The Metal backend's BC emulation fails closed. Through the backend's own entry points, on a real device:
 *
 *   1. A forced rsx.bc_path (cpu, gpu, tiered) on the vtable route, whether PS3RECOMP_RSX_ENGINE=vtable chose it
 *      or the draw engine did not start, is a named config-invalid stop that names the key and the route, and
 *      `auto` on that route goes on, natively. The draw-engine route accepts every value.
 *   2. A GPU-tier decode whose command buffer ends in an error (a test hook, compiled only into this fixture,
 *      makes the next completion fail) stops the RSX thread at the next frame commit with host-gpu-command,
 *      naming the texture (format, size, level, slice, z) and the error, before that frame's command buffer is
 *      created: no commit, no present. A flush (no present) is held back the same way.
 *   3. A decode that completes late, with an error, across shutdown and a new init: shutdown waits for it, the
 *      failure stays with the decoder that ran it, and the new run commits without a stop. Run under ThreadSanitizer
 *      this is also the check that the completion thread and the RSX thread share no unsynchronised state.
 *   4. The settle before a frame commit is bounded: a decode that never completes (a test hook holds its
 *      completion) stops with host-gpu-command / decode-timeout within the bound, naming the pending decode; the
 *      bound counts active time, so a held decode outlasts it with the gate suspended and the commit goes through
 *      after the resume; a completion released as the bound runs out is a completed decode (or its decode-error),
 *      never a timeout naming nothing; every settle leaves a BC_SETTLE span. Shutdown blocks on a held decode
 *      until it is released. Every ordering is a hook or a count (the waiter's blocked slices, the gate's parked
 *      threads), not a sleep.
 *   5. The lifecycle drain (rsx_metal_backend_gpu_suspend) waits for the scheduling of the last command buffer
 *      committed on the queue, the BC decode's or the frame's (a hook records what it waited on).
 *
 * Synthetic blocks only. Skips (77) without a Metal device. */
#define RSX_METAL_BC_TEST_HOOKS 1
#include "../rsx_metal_backend.m"
#include "rsx_bc_test_corpus.h"

#include <setjmp.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

/* The draw engine as the backend sees it: only what the routes need. */
static int s_engine_enabled = 1;
static int s_engine_init_fails;
static const char* s_policy_path = "auto";
static int s_vtable_registered;

float cellAvconfExt_display_gamma(void) { return 1.0f; }
void rsx_set_backend(rsx_backend* b) { s_vtable_registered = b != NULL; }
void rsx_draw_engine_set_backend(const rsx_draw_backend* b)
{
    /* What the real engine does: hand the registry's policy to the backend when it is set. */
    if (b && b->set_bc_policy) b->set_bc_policy(b->user, s_policy_path, 1024);
}
void rsx_draw_engine_set_default(int on) { (void)on; }
int  rsx_draw_engine_enabled(void) { return s_engine_enabled; }
int  rsx_draw_engine_init(u32 w, u32 h) { return s_engine_init_fails ? -1 : eng_init(NULL, w, h); }
void rsx_draw_engine_shutdown(void) { eng_shutdown(NULL); }
void rsx_draw_engine_present(void) {}
u32  rsx_draw_engine_readback_center(void) { return 0; }
u32  rsx_draw_engine_guest_draws(void) { return 0; }
int  cellGcmOffsetIsDisplay(u32 off) { (void)off; return 1; }
u32  cellGcmResolveLocated(int location, u32 offset) { (void)location; return offset; }
u32  cellGcmResolveOffset(u32 offset) { return offset; }
void ps3_host_quit(const char* why) { (void)why; }
u8*  vm_base;
int  rsx_draw_engine_frame_identity(rsx_frame_identity* out) { (void)out; return 0; }
u32  rsx_draw_engine_present_diag(void) { return 0; }
void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4])
{ (void)st; (void)idx; (void)vi; out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; }
int  rsx_capture_install_from_config(const rsx_capture_environment* env) { (void)env; return 0; }
void cellGcm_set_host_pause_clock(unsigned long long (*paused_ms)(void)) { (void)paused_ms; }

/* ps3_stop, recorded and left by longjmp so the test can look at the stop and carry on. The backend finds it by
 * name (dlsym), so the volatile pointer keeps it from being dead-stripped. */
static jmp_buf s_jmp;
static int s_stop_armed;
static struct {
    int kind;
    char what[320];
    size_t n;
    char key[8][32], value[8][320];
} s_stop;

void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy, const ps3_stop_field* fields, size_t nfields,
              const ps3_stop_site* site)
{
    (void)remedy; (void)site;
    memset(&s_stop, 0, sizeof s_stop);
    s_stop.kind = (int)kind;
    snprintf(s_stop.what, sizeof s_stop.what, "%s", what);
    for (size_t i = 0; i < nfields && i < 8; ++i) {
        snprintf(s_stop.key[i], sizeof s_stop.key[i], "%s", fields[i].key);
        snprintf(s_stop.value[i], sizeof s_stop.value[i], "%s", fields[i].value);
        s_stop.n = i + 1;
    }
    if (s_stop_armed) longjmp(s_jmp, 1);
    fprintf(stderr, "unexpected stop: %s\n", what);
    _Exit(70);
}
static void* volatile s_keep_ps3_stop = (void*)ps3_stop;

static unsigned s_failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        s_failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
    } \
} while (0)

static const char* stop_field(const char* key)
{
    for (size_t i = 0; i < s_stop.n; ++i)
        if (!strcmp(s_stop.key[i], key)) return s_stop.value[i];
    return "";
}

/* Run `body`; 1 when it ended in a stop (recorded in s_stop), 0 when it returned. */
#define STOPS_IN(body) ({ memset(&s_stop, 0, sizeof s_stop); s_stop_armed = 1; \
                          volatile int stopped_ = 1; \
                          if (setjmp(s_jmp) == 0) { body; stopped_ = 0; } \
                          s_stop_armed = 0; stopped_; })

static void setup_policy(const char* path, int engine_enabled, int engine_init_fails)
{
    s_policy_path = path;
    s_engine_enabled = engine_enabled;
    s_engine_init_fails = engine_init_fails;
    s_vtable_registered = 0;
}

static int backend_init(void) { return rsx_metal_backend_init(64, 64, "bc fail closed"); }

static void test_vtable_route(void)
{
    static const char* const forced[] = { "cpu", "gpu", "tiered" };
    for (unsigned i = 0; i < 3; ++i) {
        for (int engine_fails = 0; engine_fails < 2; ++engine_fails) {
            /* Chosen by PS3RECOMP_RSX_ENGINE=vtable (engine disabled), or by an engine that did not start. */
            setup_policy(forced[i], engine_fails ? 1 : 0, engine_fails);
            const int stopped = STOPS_IN(backend_init());
            CHECK(stopped, "%s on the vtable route (%s) did not stop", forced[i],
                  engine_fails ? "engine init failed" : "engine disabled");
            CHECK(s_stop.kind == PS3_STOP_CONFIG_INVALID, "%s: stop kind %d, want config-invalid", forced[i],
                  s_stop.kind);
            CHECK(!strcmp(stop_field("key"), "rsx.bc_path") && !strcmp(stop_field("value"), forced[i]) &&
                  !strcmp(stop_field("route"), "vtable"), "%s: the stop names the key (%s), the value (%s) and the route (%s)",
                  forced[i], stop_field("key"), stop_field("value"), stop_field("route"));
            CHECK(!s_vtable_registered, "%s: the vtable backend was registered despite the refusal", forced[i]);
            rsx_metal_backend_shutdown();
        }
    }
    /* auto on that route is native BC, as before. */
    setup_policy("auto", 0, 0);
    int rc = -1;
    const int stopped = STOPS_IN(rc = backend_init());
    CHECK(!stopped && rc == 0 && s_vtable_registered, "auto on the vtable route goes on (stopped %d, rc %d, registered %d)",
          stopped, rc, s_vtable_registered);
    CHECK(texfmt_to_metal(RSX_TEXFMT_BC3) == bc_native_format(3), "the vtable route holds BC natively");
    rsx_metal_backend_shutdown();
    /* The draw-engine route honours every value. */
    for (unsigned i = 0; i < 3; ++i) {
        setup_policy(forced[i], 1, 0);
        rc = -1;
        const int s = STOPS_IN(rc = backend_init());
        CHECK(!s && rc == 0 && !s_vtable_registered && s_eng_active, "%s on the engine route is accepted (stopped %d rc %d)",
              forced[i], s, rc);
        rsx_metal_backend_shutdown();
    }
}

static u32 make_bc3_texture(const char* what, int upload, uint32_t* rng)
{
    const u32 handle = eng_texture_create(NULL, RSX_BE_FMT_BC3, 64, 64, 1, 1, 0xAAE4, 0xA8);
    CHECK(handle != 0, "%s: create", what);
    if (upload) {
        rsx_bc_test_image im;
        CHECK(rsx_bc_test_make_random_image(&im, RSX_BC_FORMAT_BC3, 64, 64, rng), "%s: image", what);
        eng_texture_upload(NULL, handle, 0, 0, 64, 64, im.bytes, (u32)im.row_bytes, im.blocks_h);
        rsx_bc_test_image_free(&im);
    }
    return handle;
}

static void test_failed_decode_stops_the_frame(void)
{
    setup_policy("gpu", 1, 0);
    CHECK(backend_init() == 0, "init (engine route, gpu)");
    uint32_t rng = 0x6D2B79F5u;
    RsxMetalBc* bc = eng_bc_gpu();
    CHECK(bc != nil, "the GPU decoder builds");
    const u32 target = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 64, 64, NULL, 0);
    id<MTLTexture> surface = eng_obj(target);

    /* A healthy decode commits and presents: nothing stops. */
    u32 tex = make_bc3_texture("healthy", 1, &rng);
    const u32 serial0 = s_eng_commit_serial;
    int stopped = STOPS_IN(eng_encode_and_commit(surface));
    CHECK(!stopped && s_eng_commit_serial == serial0 + 1 && s_eng_shown == surface,
          "a healthy decode lets the frame through (stopped %d)", stopped);
    eng_obj_release(NULL, tex);

    /* Failing decodes: windowed-style present (a surface shown) and flush (none), same choke point. */
    for (int present = 1; present >= 0; --present) {
        static const char* const kinds[] = { "flush", "present" };
        /* Reset what a present of this surface would change, so "not presented" is a difference. */
        s_eng_shown = nil;
        [bc testInjectNextDecodeFailure:YES delayMicroseconds:20000];
        tex = make_bc3_texture(kinds[present], 1, &rng);
        const u32 serial = s_eng_commit_serial;
        const unsigned tier_gpu = s_eng_bc_tier_uploads[2];
        CHECK(tier_gpu > 0, "the failing decode ran on the GPU tier");
        stopped = STOPS_IN(eng_encode_and_commit(present ? surface : nil));
        CHECK(stopped, "%s: a failed decode did not stop the frame", kinds[present]);
        CHECK(s_stop.kind == PS3_STOP_HOST_GPU_COMMAND, "%s: stop kind %d, want host-gpu-command", kinds[present],
              s_stop.kind);
        CHECK(!strcmp(stop_field("texture"), "BC3 64x64 level 0 slice 0 z 0") && !strcmp(stop_field("command"), "bc-decode") &&
              strstr(stop_field("error"), "injected") != NULL,
              "%s: the stop names the texture ('%s') and the error ('%s')", kinds[present], stop_field("texture"),
              stop_field("error"));
        CHECK(s_eng_commit_serial == serial, "%s: a frame was committed after the failure (serial %u -> %u)",
              kinds[present], serial, s_eng_commit_serial);
        CHECK(s_eng_shown == nil, "%s: the frame was presented", kinds[present]);
        CHECK([bc failureCount] >= 1, "the decoder counted the failure");
        /* The failure stays recorded: the run would have ended, and a second look says the same. */
        stopped = STOPS_IN(eng_encode_and_commit(nil));
        CHECK(stopped, "%s: the failure is sticky", kinds[present]);
        eng_obj_release(NULL, tex);
        /* The record is per decoder: drop it so the next iteration starts clean (what a new run does). */
        rsx_metal_backend_shutdown();
        setup_policy("gpu", 1, 0);
        CHECK(backend_init() == 0, "re-init");
        bc = eng_bc_gpu();
        CHECK(bc != nil && [bc failureCount] == 0, "a new run has a decoder with no failure");
    }
    rsx_metal_backend_shutdown();
}

/* The bound of eng_bc_settle, shortened for the tests. Elapsed time is read on the monotonic clock the backend
 * itself uses. */
#define TEST_BUDGET_MS 150u

static double elapsed_ms(uint64_t t0) { return (double)(ps3_ft_now() - t0) / 1e6; }

static void* commit_thread(void* arg)
{
    (void)arg;
    eng_encode_and_commit(nil);   /* a timeout here would end the process: ps3_stop is not armed on this thread */
    return NULL;
}

static int s_shutdown_done;
static void* shutdown_thread(void* arg)
{
    (void)arg;
    rsx_metal_backend_shutdown();
    __atomic_store_n(&s_shutdown_done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* Spins (no sleep) until a waiter has been blocked on an unfinished decode for `slices` slices of its own, or
 * 30 s passed. */
static int wait_for_slices(RsxMetalBc* bc, unsigned slices)
{
    const uint64_t t0 = ps3_ft_now();
    while ([bc testBlockedSlices] < slices) {
        if (elapsed_ms(t0) > 30000.0) return 0;
        sched_yield();
    }
    return 1;
}

static void check_suspend_waited_for(id<MTLCommandBuffer> cb, unsigned waits, const char* what)
{
    rsx_metal_backend_gpu_suspend();
    CHECK(s_test_scheduled_waits == waits && s_test_scheduled_buffer == cb &&
          s_test_scheduled_status >= (long)MTLCommandBufferStatusScheduled,
          "suspend waited for the %s's buffer (waits %u, same buffer %d, status %ld)", what, s_test_scheduled_waits,
          s_test_scheduled_buffer == cb, s_test_scheduled_status);
    rsx_metal_backend_gpu_resume();
}

static void test_late_completion_across_shutdown(void)
{
    setup_policy("gpu", 1, 0);
    CHECK(backend_init() == 0, "init (engine route, gpu)");
    uint32_t rng = 0x9E3779B9u;
    RsxMetalBc* first = eng_bc_gpu();
    CHECK(first != nil, "the GPU decoder builds");
    /* The completion is held, will be an error, and the RSX thread goes on to shut down: the shutdown has to be
     * blocked on it (the waiter's slices count), and returns only after the release. No timing involved. */
    [first testInjectNextDecodeFailure:YES delayMicroseconds:0];
    [first testHoldNextDecodeCompletion];
    make_bc3_texture("late", 1, &rng);
    __atomic_store_n(&s_shutdown_done, 0, __ATOMIC_SEQ_CST);
    pthread_t sd;
    CHECK(pthread_create(&sd, NULL, shutdown_thread, NULL) == 0, "shutdown thread");
    CHECK(wait_for_slices(first, 3), "shutdown is waiting on the held decode");
    CHECK(!__atomic_load_n(&s_shutdown_done, __ATOMIC_SEQ_CST) && [first inFlight] == 1,
          "shutdown did not return while the decode was in flight");
    [first testReleaseHeldCompletion];
    pthread_join(sd, NULL);
    CHECK(__atomic_load_n(&s_shutdown_done, __ATOMIC_SEQ_CST), "shutdown returned after the release");
    CHECK([first failureCount] == 1 && [first failureTexture] != nil,
          "the late failure was recorded on the decoder that ran it (%u)", [first failureCount]);
    CHECK(s_eng_bc_gpu == nil, "shutdown dropped the decoder");

    /* A new run: its own decoder, no inherited failure, and its frame commits. */
    setup_policy("gpu", 1, 0);
    CHECK(backend_init() == 0, "re-init");
    RsxMetalBc* second = eng_bc_gpu();
    CHECK(second != nil && second != first && [second failureCount] == 0 && [second failureTexture] == nil,
          "the new run starts with a clean decoder");
    const u32 tex = make_bc3_texture("second run", 1, &rng);
    const u32 serial = s_eng_commit_serial;
    const int stopped = STOPS_IN(eng_encode_and_commit(nil));
    CHECK(!stopped && s_eng_commit_serial == serial + 1, "the new run's frame commits without a stop (stopped %d)", stopped);
    CHECK(!s_eng_encoding_failed, "the old failure did not mark the new run failed");
    eng_obj_release(NULL, tex);
    rsx_metal_backend_shutdown();
}


/* A decode whose command buffer never ends stops the RSX thread with decode-timeout, within the bound. */
static void test_settle_timeout(void)
{
    setup_policy("gpu", 1, 0);
    CHECK(backend_init() == 0, "init");
    uint32_t rng = 0x1234567u;
    RsxMetalBc* bc = eng_bc_gpu();
    s_eng_bc_settle_budget_ms = TEST_BUDGET_MS;
    [bc testHoldNextDecodeCompletion];
    const u32 tex = make_bc3_texture("held", 1, &rng);
    CHECK([bc inFlight] == 1, "the held decode is in flight (%u)", [bc inFlight]);
    const u32 serial = s_eng_commit_serial;
    const uint64_t t0 = ps3_ft_now();
    const int stopped = STOPS_IN(eng_encode_and_commit(nil));
    const double ms = elapsed_ms(t0);
    CHECK(stopped && s_stop.kind == PS3_STOP_HOST_GPU_COMMAND, "a decode that never completes stops the run (stopped %d, kind %d)",
          stopped, s_stop.kind);
    CHECK(!strcmp(stop_field("reason"), "decode-timeout") && !strcmp(stop_field("command"), "bc-decode") &&
          strstr(stop_field("pending"), "BC3 64x64 level 0 slice 0 z 0") != NULL,
          "the stop says decode-timeout and names the pending decode ('%s', '%s')", stop_field("reason"), stop_field("pending"));
    CHECK(strlen(stop_field("pending")) > 0, "a timeout never names nothing");
    CHECK(ms >= TEST_BUDGET_MS - 1 && ms < TEST_BUDGET_MS + 1500, "the stop came after %.0f ms, bound %u ms", ms, TEST_BUDGET_MS);
    CHECK(s_eng_commit_serial == serial, "no frame was committed after the timeout");
    /* Let the held completion run; the decoder is then idle and clean (a hang is not a failed decode). */
    [bc testReleaseHeldCompletion];
    CHECK([bc waitIdleWithin:5000] == nil && [bc failureCount] == 0, "the released decode completed cleanly");
    const int again = STOPS_IN(eng_encode_and_commit(nil));
    CHECK(!again, "the next commit goes through once the decode has completed");
    s_eng_bc_settle_budget_ms = ENG_BC_SETTLE_BUDGET_MS;
    eng_obj_release(NULL, tex);
    rsx_metal_backend_shutdown();
}


/* A completion that lands as the bound runs out is not a timeout. The held decode is released from inside the
 * waiter, after its budget is spent and before it reads what is pending: the report must not be an empty
 * "pending" list, so the waiter finds the decode done (and an error it ended in is the caller's decode-error). */
static void test_completion_at_the_deadline(void)
{
    setup_policy("gpu", 1, 0);
    CHECK(backend_init() == 0, "init");
    uint32_t rng = 0xBADC0DEu;
    RsxMetalBc* bc = eng_bc_gpu();
    __weak RsxMetalBc* weak_bc = bc;
    void (^release_at_deadline)(void) = ^{
        RsxMetalBc* b = weak_bc;
        [b testReleaseHeldCompletion];
        for (unsigned i = 0; i < 400000000u && [b inFlight] != 0; i++) sched_yield();   /* its entry is gone */
    };

    /* Clean completion at the deadline. */
    [bc testHoldNextDecodeCompletion];
    u32 tex = make_bc3_texture("deadline", 1, &rng);
    [bc testOnBudgetExhausted:release_at_deadline];
    NSString* pending = [bc waitIdleWithin:50];
    CHECK(pending == nil && [bc inFlight] == 0 && [bc failureCount] == 0,
          "a decode that completed at the deadline is not a timeout (report '%s')", pending ? [pending UTF8String] : "nil");
    eng_obj_release(NULL, tex);

    /* A failed completion at the deadline is a decode-error, not a timeout with nothing to name. */
    s_eng_bc_settle_budget_ms = 50;
    [bc testInjectNextDecodeFailure:YES delayMicroseconds:0];
    [bc testHoldNextDecodeCompletion];
    tex = make_bc3_texture("deadline error", 1, &rng);
    [bc testOnBudgetExhausted:release_at_deadline];
    const int stopped = STOPS_IN(eng_bc_settle());
    CHECK(stopped && s_stop.kind == PS3_STOP_HOST_GPU_COMMAND && !strcmp(stop_field("reason"), "decode-error") &&
          strstr(stop_field("texture"), "BC3 64x64 level 0 slice 0 z 0") != NULL,
          "a failure at the deadline is reported as decode-error naming the texture (stopped %d, reason '%s', texture '%s')",
          stopped, stop_field("reason"), stop_field("texture"));
    s_eng_bc_settle_budget_ms = ENG_BC_SETTLE_BUDGET_MS;
    eng_obj_release(NULL, tex);
    rsx_metal_backend_shutdown();
}

static int s_settle_done, s_settle_stopped;
static void* settle_thread(void* arg)
{
    (void)arg;
    s_settle_stopped = STOPS_IN(eng_bc_settle());   /* the stop, if any, is left in s_stop for the test to read after the join */
    __atomic_store_n(&s_settle_done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* The deadline falls between a decode's published outcome and the return of its slot. The handler is paused
 * there (outcome published: failure record and end of the pending entry, slot not yet back), the settle runs on
 * a thread with a 50 ms budget, and the test watches the waiter's blocked slices: one budget is 5 slices, and
 * the waiter must return within it, without waiting for the slot (it is never extended: a second budget would
 * show as more slices, and the handler is resumed only after, so a waiter that waits for the slot cannot return
 * before). A clean decode is no stop; a failed one is a decode-error naming the texture, not a timeout. */
static void test_slot_handoff_at_the_deadline(int fail)
{
    setup_policy("gpu", 1, 0);
    CHECK(backend_init() == 0, "init");
    uint32_t rng = 0x5107B0A7u;
    RsxMetalBc* bc = eng_bc_gpu();
    enum { BUDGET_MS = 50, SLICE_BOUND = BUDGET_MS / 10 + 2 };   /* one budget's slices (10 ms each) and slack */
    s_eng_bc_settle_budget_ms = BUDGET_MS;
    if (fail) [bc testInjectNextDecodeFailure:YES delayMicroseconds:0];
    [bc testHoldNextDecodeCompletion];
    [bc testPauseNextDecodeBeforeSlotReturn];
    const u32 tex = make_bc3_texture("handoff", 1, &rng);
    [bc testReleaseHeldCompletion];
    [bc testWaitUntilHandlerPaused];
    CHECK([bc inFlight] == 0, "handoff: the outcome is published, the decode is no longer pending");
    CHECK(([bc failureCount] == 1) == (fail != 0), "handoff: the failure is on record exactly when the decode failed (%u)",
          [bc failureCount]);
    const unsigned base = [bc testBlockedSlices];
    __atomic_store_n(&s_settle_done, 0, __ATOMIC_SEQ_CST);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, settle_thread, NULL) == 0, "settle thread");
    const uint64_t t0 = ps3_ft_now();
    unsigned slices = 0;
    while (!__atomic_load_n(&s_settle_done, __ATOMIC_SEQ_CST) && (slices = [bc testBlockedSlices] - base) <= SLICE_BOUND) {
        if (elapsed_ms(t0) > 30000.0) break;
        sched_yield();
    }
    const int returned = __atomic_load_n(&s_settle_done, __ATOMIC_SEQ_CST);
    CHECK(returned, "handoff: the settle returned on its one budget, without waiting for the slot (%u slices, bound %d)",
          slices, (int)SLICE_BOUND);
    [bc testResumePausedHandler];   /* always: a waiter stuck on the slot must be able to end */
    pthread_join(t, NULL);
    slices = [bc testBlockedSlices] - base;
    CHECK(slices <= SLICE_BOUND, "handoff: %u blocked slices for a %d ms budget (bound %d)", slices, (int)BUDGET_MS,
          (int)SLICE_BOUND);
    if (fail)
        CHECK(s_settle_stopped && s_stop.kind == PS3_STOP_HOST_GPU_COMMAND && !strcmp(stop_field("reason"), "decode-error") &&
              strstr(stop_field("texture"), "BC3 64x64 level 0 slice 0 z 0") != NULL,
              "handoff: a failed decode is decode-error naming the texture (stopped %d, reason '%s', texture '%s')",
              s_settle_stopped, stop_field("reason"), stop_field("texture"));
    else
        CHECK(!s_settle_stopped, "handoff: a clean decode is no stop (reason '%s')", stop_field("reason"));
    CHECK([bc waitIdleWithin:5000] == nil, "handoff: the slot came back");
    s_eng_bc_settle_budget_ms = ENG_BC_SETTLE_BUDGET_MS;
    eng_obj_release(NULL, tex);
    rsx_metal_backend_shutdown();
}

/* The bound counts active time. A decode whose completion the test holds back keeps the waiter blocked; the
 * gate is suspended before the commit thread starts, and the test lets the waiter's own slices run past the
 * bound (60 slices of 10 ms against 150 ms) before it releases the decode. With the pause charged to the
 * budget the waiter would stop at slice 15, deterministically, and the unarmed ps3_stop ends the process. No
 * sleeps: each step waits for the state it needs (slices counted by the waiter, the gate's parked-thread count). */
static void test_settle_waits_out_a_suspended_gate(void)
{
    setup_policy("gpu", 1, 0);
    CHECK(backend_init() == 0, "init");
    uint32_t rng = 0x7654321u;
    RsxMetalBc* bc = eng_bc_gpu();
    s_eng_bc_settle_budget_ms = TEST_BUDGET_MS;
    [bc testHoldNextDecodeCompletion];
    const u32 tex = make_bc3_texture("suspended", 1, &rng);
    CHECK([bc inFlight] == 1, "the decode is held in flight");
    const u32 serial = s_eng_commit_serial;
    rsx_metal_backend_gpu_suspend();
    pthread_t t;
    CHECK(pthread_create(&t, NULL, commit_thread, NULL) == 0, "commit thread");
    const unsigned want = 4u * TEST_BUDGET_MS / 10u;   /* slices of kRsxBcWaitSliceMs: 4x the bound */
    CHECK(wait_for_slices(bc, want), "the waiter blocked for %u slices", want);
    CHECK(host_gpu_gate_waiters(&s_gpu_gate) == 0, "the thread is still in the settle, not at the gate");
    CHECK([bc inFlight] == 1 && s_eng_commit_serial == serial, "the decode is still held and nothing was committed");
    [bc testReleaseHeldCompletion];
    for (unsigned i = 0; i < 400000000u && host_gpu_gate_waiters(&s_gpu_gate) != 1; i++) sched_yield();
    CHECK(host_gpu_gate_waiters(&s_gpu_gate) == 1, "the settle ended and the thread parked at the suspended gate");
    CHECK(s_eng_commit_serial == serial, "nothing was committed while the gate was suspended");
    rsx_metal_backend_gpu_resume();
    pthread_join(t, NULL);
    CHECK(s_eng_commit_serial == serial + 1, "the commit went through after the resume");
    s_eng_bc_settle_budget_ms = ENG_BC_SETTLE_BUDGET_MS;
    eng_obj_release(NULL, tex);
    rsx_metal_backend_shutdown();
}

/* The lifecycle drain waits for the scheduling of the last buffer committed on the queue (macOS never
 * backgrounds, so the hook records what suspend waited on and the status after): a BC decode's, then the
 * frame's. */
static void test_suspend_waits_for_scheduling(void)
{
    setup_policy("gpu", 1, 0);
    CHECK(backend_init() == 0, "init");
    uint32_t rng = 0x13572468u;
    RsxMetalBc* bc = eng_bc_gpu();
    s_test_scheduled_waits = 0;
    s_test_scheduled_buffer = nil;
    id<MTLCommandBuffer> before = s_last_committed;
    const u32 tex = make_bc3_texture("sched", 1, &rng);
    id<MTLCommandBuffer> decode_cb = s_last_committed;
    CHECK(decode_cb != nil && decode_cb != before, "the decoder's commit was recorded as the last on the queue");
    check_suspend_waited_for(decode_cb, 1, "decode");
    CHECK([bc waitIdleWithin:5000] == nil, "the decode completed");
    CHECK(!STOPS_IN(eng_encode_and_commit(nil)), "the frame commits");
    id<MTLCommandBuffer> frame_cb = s_last_committed;
    CHECK(frame_cb != nil && frame_cb != decode_cb, "the frame's commit is the last on the queue");
    check_suspend_waited_for(frame_cb, 2, "frame");
    eng_obj_release(NULL, tex);
    rsx_metal_backend_shutdown();
    CHECK(s_last_committed == nil, "shutdown dropped the record of the dropped queue");
}

/* One BC_SETTLE span per commit while the GPU tier exists, with the decodes in flight and the outcome. */
static void test_settle_spans(void)
{
    setup_policy("gpu", 1, 0);
    CHECK(backend_init() == 0, "init");
    uint32_t rng = 0x2468ACEu;
    RsxMetalBc* bc = eng_bc_gpu();
    (void)PS3_FT_ON();
    uint32_t first = g_ps3_ft.n;
    s_eng_bc_settle_budget_ms = TEST_BUDGET_MS;
    eng_encode_and_commit(nil);                                 /* nothing in flight */
    [bc testHoldNextDecodeCompletion];
    const u32 tex = make_bc3_texture("span", 1, &rng);
    (void)STOPS_IN(eng_encode_and_commit(nil));                 /* times out */
    [bc testReleaseHeldCompletion];
    CHECK([bc waitIdleWithin:5000] == nil, "span: released");
    unsigned seen = 0, done = 0, timed_out = 0;
    for (uint32_t i = first; i < g_ps3_ft.n; ++i) {
        const ps3_ft_rec* r = &g_ps3_ft.buf[i];
        if (r->kind != PS3_FT_BC_SETTLE) continue;
        seen++;
        CHECK(r->t1 >= r->t0, "span: time runs forward");
        if (r->b == 0 && r->a == 0) done++;
        if (r->b == 2 && r->a == 1) timed_out++;
    }
    CHECK(seen == 2 && done == 1 && timed_out == 1,
          "span: %u BC_SETTLE span(s): %u idle, %u timed out with one decode in flight", seen, done, timed_out);
    s_eng_bc_settle_budget_ms = ENG_BC_SETTLE_BUDGET_MS;
    eng_obj_release(NULL, tex);
    rsx_metal_backend_shutdown();
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> probe = MTLCreateSystemDefaultDevice();
        if (!probe) { puts("no Metal device: skipped"); return 77; }
        setenv("PS3RECOMP_METAL_HEADLESS", "1", 1);
        setenv("PS3_FTRACE", "0:100000:/dev/null", 1);   /* read once, at the first hook: test_settle_spans reads the buffer */
        (void)s_keep_ps3_stop;
        test_vtable_route();
        test_failed_decode_stops_the_frame();
        test_late_completion_across_shutdown();
        test_settle_timeout();
        test_completion_at_the_deadline();
        test_slot_handoff_at_the_deadline(0);
        test_slot_handoff_at_the_deadline(1);
        test_settle_waits_out_a_suspended_gate();
        test_suspend_waits_for_scheduling();
        test_settle_spans();
        printf("[metal_bc_failclosed] failures=%u\n", s_failures);
    }
    return s_failures ? 1 : 0;
}

/*
 * ps3recomp - what the RSX driver's flip words mean (#101)
 *
 * Synthetic FIFO streams through the real dispatcher and the real draw engine, against the
 * recording stub backend. Evidence for the semantics under test:
 *
 *   firmware  libgcm_sys _cellGcmSetFlipCommand emits 0xE944 = bufferId, the NV406E semaphore
 *             handshake and CALL 0, then 0xE924 = 0x8000010F, a constant. The head word never names
 *             the buffer; the preceding queue word does.
 *   RPCS3     pin d00d8023: gcm_enums.h GCM_FLIP_HEAD 0xE920:0xE924 (head 0 or 1) and
 *             GCM_DRIVER_QUEUE 0xE940:0xE95C (only the first two indices prepare display buffers);
 *             sys_rsx.cpp 0x103 (queue) records the head's last queued buffer and does not flip, 0x102
 *             (flip) takes the low nibble of a bit-31 word (8..15 = the head's last queued buffer) or
 *             matches a display-buffer offset.
 *
 * Every scenario runs in its own forked child, so the engine's process-wide state starts clean and a
 * scenario that must end in a named stop can end the process: the parent reads the child's exit status
 * (70) and its PS3STOP line. The child's backend prints PRESENT for every present, so "stops before
 * presenting anything" is observable.
 *
 * Needs runtime/diag/stop.cpp (the real stop), not the stub.
 */
#define STUB_REAL_STOP
#include "rsx_draw_engine_test_stub.h"
#include "rsx_capture.h"
#include "ps3emu/stop.h"

#include <sys/wait.h>
#include <unistd.h>

#define NSURF 3
/* Three display buffers, each its own 64x64 surface, and one offscreen target the draws end on. */
static const u32 k_offset[NSURF] = { 0x40000u, 0x60000u, 0x80000u };
#define OFFSCREEN_OFFSET 0xA0000u

static u32 g_handle[NSURF];       /* the backend handle of each display buffer's surface */
static u32 g_offscreen;           /* ... and of the offscreen target that is bound when a flip arrives */
static int g_child_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("[PASS] " __VA_ARGS__); printf("\n");                      \
        } else {                                                              \
            printf("[FAIL] " __VA_ARGS__);                                    \
            printf("   (line %d)\n", __LINE__);                               \
            g_child_fail++;                                                   \
        }                                                                     \
    } while (0)

static rsx_draw_backend g_be;

static void be_present(void* u, u32 surface)
{
    printf("PRESENT handle=%u\n", surface);
    fflush(stdout);
    stub_present(u, surface);
}

/* The driver's words, with the subchannel bits the walker leaves on them. */
#define M_QUEUE0 0xE940u
#define M_QUEUE1 0xE944u
#define M_FLIP0  0xE920u
#define M_FLIP1  0xE924u
#define LAST_QUEUED 0x8000010Fu     /* the word libgcm_sys writes */

static void draw_into(u32 offset)
{
    sm(M_COLOR_A_OFFSET, offset);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);
}

/* An engine with display buffers 0..2 registered, each with a surface rendered into it, and the
 * offscreen target left bound: a flip that fell back to "whatever is bound" would show in the handle. */
static void up(int render, int register_buffers)
{
    stub_engine_up(0);               /* the stub backend, the vertex format and the vertex buffer */
    /* The same backend, but a present says so: the child's output is the parent's evidence that a
     * scenario that must stop did so before presenting anything. */
    g_be = stub_backend;
    g_be.present = be_present;
    rsx_draw_engine_set_backend(&g_be);
    sm(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
    sm(M_SURFACE_CLIP_H, 64u << 16);
    sm(M_SURFACE_CLIP_V, 64u << 16);
    if (register_buffers)
        for (u32 i = 0; i < NSURF; i++)
            rsx_draw_engine_set_display_buffer(i, RSX_LOCATION_LOCAL, k_offset[i], 64u * 4u, 64u, 64u);
    if (render) {
        for (u32 i = 0; i < NSURF; i++) {
            draw_into(k_offset[i]);
            g_handle[i] = stub.bound_targets[0];
        }
        draw_into(OFFSCREEN_OFFSET);
        g_offscreen = stub.bound_targets[0];
    }
}

static struct { u32 flip, guest, buffer, seq; int n; } g_cap;

static void cap_frame(const rsx_cap_frame* f)
{
    g_cap.flip = f->flip_id; g_cap.guest = f->guest_frame; g_cap.buffer = f->display_buffer;
    g_cap.seq = f->present_seq; g_cap.n++;
}

static int ident(rsx_frame_identity* id) { return rsx_draw_engine_frame_identity(id); }

/* ---- scenarios that must run to the end ---------------------------------- */

/* The libgcm_sys stream: 0xE944 = 1, then 0xE924 = 0x8000010F. One present, of buffer 1's surface. */
static void sc_queue_then_head(void)
{
    up(1, 1);
    CHECK(g_offscreen && g_offscreen != g_handle[1] && g_handle[0] != g_handle[1] && g_handle[1] != g_handle[2],
          "four distinct surfaces: three display buffers and the bound offscreen target");
    static rsx_capture_sink sink;
    sink.frame = cap_frame;
    rsx_draw_engine_set_capture_sink(&sink);
    const int p0 = stub.n_present;

    sm(M_QUEUE1, 1u);
    rsx_frame_identity id;
    CHECK(stub.n_present == p0 && !ident(&id), "the queue word presents nothing and publishes no identity");

    sm(M_FLIP1, LAST_QUEUED);
    CHECK(stub.n_present == p0 + 1, "the head word is exactly one present (%d)", stub.n_present - p0);
    CHECK(stub.last_present_surface == g_handle[1],
          "it shows display buffer 1's surface (%u, want %u), not the bound target %u",
          stub.last_present_surface, g_handle[1], g_offscreen);
    CHECK(ident(&id) && id.flip_id == 0 && id.guest_frame == 0 && id.display_buffer == 1 && id.present_seq == 0,
          "identity: flip %u guest frame %u buffer %u present %u", id.flip_id, id.guest_frame,
          id.display_buffer, id.present_seq);
    CHECK(g_cap.n == 1 && g_cap.flip == 0 && g_cap.guest == 0 && g_cap.buffer == 1 && g_cap.seq == 0,
          "the capture frame is the head's: flip %u guest %u buffer %u present %u (%d frame)",
          g_cap.flip, g_cap.guest, g_cap.buffer, g_cap.seq, g_cap.n);
    rsx_draw_engine_set_capture_sink(NULL);
}

/* Alternating buffers across flips: the present ordinal and the flip ordinal each advance by one a flip. */
static void sc_alternating(void)
{
    up(1, 1);
    static rsx_capture_sink sink;
    sink.frame = cap_frame;
    rsx_draw_engine_set_capture_sink(&sink);
    int ok = 1;
    for (u32 k = 0; k < 6; k++) {
        const u32 b = k & 1u;
        const int p0 = stub.n_present;
        stub_flip(b);                          /* 0xE944 = b, then 0xE924 = 0x8000010F */
        rsx_frame_identity id;
        const int good = stub.n_present == p0 + 1 && stub.last_present_surface == g_handle[b] && ident(&id) &&
                         id.flip_id == k && id.guest_frame == k && id.display_buffer == b && id.present_seq == k;
        if (!good) printf("  flip %u: presents %d surface %u (want %u) identity %u/%u/%u/%u\n", k,
                          stub.n_present - p0, stub.last_present_surface, g_handle[b], id.flip_id,
                          id.guest_frame, id.display_buffer, id.present_seq);
        ok &= good;
    }
    CHECK(ok, "six flips alternating buffers 0 and 1: one present each, of that buffer, ordinals +1");
    CHECK(g_cap.n == 6 && g_cap.flip == 5 && g_cap.seq == 5, "one capture frame per flip (%d, last flip %u)",
          g_cap.n, g_cap.flip);
    rsx_draw_engine_set_capture_sink(NULL);
}

/* The low nibble names the buffer; it does not consume what was queued. */
static void sc_explicit_nibble(void)
{
    up(1, 1);
    sm(M_QUEUE1, 1u);
    static const u32 order[3] = { 0u, 2u, 1u };
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        sm(M_FLIP1, 0x80000100u | order[i]);
        rsx_frame_identity id;
        const int good = stub.last_present_surface == g_handle[order[i]] && ident(&id) &&
                         id.display_buffer == order[i];
        if (!good) printf("  idx %u: surface %u want %u\n", order[i], stub.last_present_surface, g_handle[order[i]]);
        ok &= good;
    }
    CHECK(ok, "0x80000100|idx presents display buffer idx (0, 2, 1) whatever was queued");
    sm(M_FLIP1, LAST_QUEUED);
    CHECK(stub.last_present_surface == g_handle[1], "an 8..15 nibble afterwards still takes the queued buffer 1");
}

/* Bit 31 clear: the word is a display-buffer offset. */
static void sc_offset_form(void)
{
    up(1, 1);
    sm(M_QUEUE1, 0u);
    int ok = 1;
    for (u32 i = 0; i < NSURF; i++) {
        sm(M_FLIP1, k_offset[NSURF - 1u - i]);
        const u32 want = g_handle[NSURF - 1u - i];
        const int good = stub.last_present_surface == want;
        if (!good) printf("  offset 0x%X: surface %u want %u\n", k_offset[NSURF - 1u - i], stub.last_present_surface, want);
        ok &= good;
    }
    CHECK(ok, "an offset word presents the display buffer that carries it, not the queued buffer 0");
}

/* The queue and the flip are per head: head 0's flip takes head 0's queue and head 1's takes its own. */
static void sc_two_heads(void)
{
    up(1, 1);
    sm(M_QUEUE0, 2u);
    sm(M_QUEUE1, 1u);
    sm(M_FLIP0, LAST_QUEUED);
    CHECK(stub.last_present_surface == g_handle[2], "head 0 flips the buffer queued on head 0 (2)");
    sm(M_FLIP1, LAST_QUEUED);
    CHECK(stub.last_present_surface == g_handle[1], "head 1 flips the buffer queued on head 1 (1)");
    sm(M_QUEUE0, 0u);
    sm(M_FLIP1, LAST_QUEUED);
    CHECK(stub.last_present_surface == g_handle[1], "a later queue on head 0 does not move head 1's buffer");
}

/* A queue word does not change what is on screen: a host present between it and its head shows the
 * buffer the previous flip chose, and names the previous flip. */
static void sc_queue_keeps_scanout(void)
{
    up(1, 1);
    rsx_frame_identity id;
    sm(M_QUEUE1, 1u);
    rsx_draw_engine_present();
    CHECK(stub.last_present_surface == g_handle[0] && ident(&id) && id.flip_id == RSX_FRAME_NO_FLIP,
          "before its head, the queued buffer 1 is not shown: a host present shows buffer 0 and names no flip");
    sm(M_FLIP1, LAST_QUEUED);
    sm(M_QUEUE1, 2u);
    rsx_draw_engine_present();
    CHECK(stub.last_present_surface == g_handle[1] && ident(&id) && id.flip_id == 0 && id.display_buffer == 1,
          "after flip 0 and before flip 1's head, a host present still shows buffer 1 and names flip 0");
}

/* The host present is pacing, not a flip (#94): with display buffers registered and nothing rendered
 * at the buffer, it presents nothing; with none registered it shows the current target. */
static void sc_host_present_without_scanout(void)
{
    up(0, 1);
    rsx_draw_engine_present();
    CHECK(stub.n_present == 0, "display buffers registered, nothing rendered: a host present shows nothing (%d)",
          stub.n_present);
}

static void sc_host_present_unregistered(void)
{
    up(0, 0);
    draw_into(OFFSCREEN_OFFSET);
    const u32 bound = stub.bound_targets[0];
    rsx_draw_engine_present();
    CHECK(stub.n_present == 1 && stub.last_present_surface == bound,
          "no display buffer registered at all: a host present shows the current target (%u, want %u)",
          stub.last_present_surface, bound);
}

/* ---- scenarios that must end in the named stop --------------------------- */

static void st_nothing_queued(void)   { up(1, 1); sm(M_FLIP1, LAST_QUEUED); }
static void st_other_head(void)       { up(1, 1); sm(M_QUEUE1, 1u); sm(M_FLIP0, LAST_QUEUED); }
static void st_unregistered_idx(void) { up(1, 1); sm(M_QUEUE1, 1u); sm(M_FLIP1, 0x80000105u); }
static void st_offset_unmatched(void) { up(1, 1); sm(M_QUEUE1, 1u); sm(M_FLIP1, 0x00012340u); }
static void st_queue_index_2(void)    { up(1, 1); sm(0xE948u, 3u); }
static void st_queue_index_7(void)    { up(1, 1); sm(0xE95Cu, 0x80000000u); }
static void st_queued_above_7(void)   { up(1, 1); sm(M_QUEUE1, 9u); sm(M_FLIP1, LAST_QUEUED); }

/* A registered display buffer the engine rendered nothing at has no scanout to show. */
static void st_no_surface(void)
{
    up(1, 1);
    rsx_draw_engine_set_display_buffer(3, RSX_LOCATION_LOCAL, 0xE0000u, 64u * 4u, 64u, 64u);
    sm(M_QUEUE1, 3u);
    sm(M_FLIP1, LAST_QUEUED);
}

static void st_present_buffer(void)   { up(1, 1); rsx_draw_engine_present_buffer(5); }

/* ---- the harness --------------------------------------------------------- */

static int g_failures;

static int run_child(void (*fn)(void), char* out, size_t cap, int* status)
{
    int fds[2];
    if (pipe(fds)) return 0;
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        fn();
        stub_engine_down();
        fflush(stdout);
        _exit(g_child_fail ? 1 : 0);
    }
    close(fds[1]);
    size_t n = 0;
    for (ssize_t r; n < cap - 1 && (r = read(fds[0], out + n, cap - 1 - n)) > 0;) n += (size_t)r;
    out[n] = 0;
    close(fds[0]);
    waitpid(pid, status, 0);
    return 1;
}

static void expect_runs(const char* name, void (*fn)(void))
{
    static char out[1 << 16];
    int status = 0;
    const int ran = run_child(fn, out, sizeof out, &status);
    const int ok = ran && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        g_failures++;
        printf("  status %d; child output:\n%s\n", status, out);
    }
}

/* The child ends with the stop status, a PS3STOP line carrying exactly `fields`, and no present before it. */
static void expect_stop(const char* name, void (*fn)(void), const char* fields)
{
    static char out[1 << 16];
    int status = 0;
    const int ran = run_child(fn, out, sizeof out, &status);
    char want[256];
    snprintf(want, sizeof want, "PS3STOP v=1 kind=rsx-driver-method %s", fields);
    const int stopped = ran && WIFEXITED(status) && WEXITSTATUS(status) == PS3_STOP_EXIT_CODE;
    const int named = strstr(out, want) != NULL;
    const int quiet = strstr(out, "PRESENT") == NULL;
    const int ok = stopped && named && quiet;
    printf("[%s] stop: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        g_failures++;
        printf("  stopped=%d named=%d no-present=%d; wanted \"%s\"\n  child output:\n%s\n", stopped, named, quiet,
               want, out);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    stub_guest_init();

    expect_runs("queue 1, head 0x8000010F: one present of buffer 1, identity and capture frame", sc_queue_then_head);
    expect_runs("alternating buffers across flips", sc_alternating);
    expect_runs("explicit low nibble presents that buffer", sc_explicit_nibble);
    expect_runs("offset form presents the matching display buffer", sc_offset_form);
    expect_runs("queue and flip are per head", sc_two_heads);
    expect_runs("a queue word does not change the scanout", sc_queue_keeps_scanout);
    expect_runs("host present with no rendered scanout presents nothing", sc_host_present_without_scanout);
    expect_runs("host present with no display buffers shows the current target", sc_host_present_unregistered);

    expect_stop("a flip with nothing queued", st_nothing_queued,
                "method=0xE924 head=1 arg=0x8000010F buffer=none reason=nothing-queued");
    expect_stop("a queue on head 1 is not what a flip on head 0 takes", st_other_head,
                "method=0xE920 head=0 arg=0x8000010F buffer=none reason=nothing-queued");
    expect_stop("an explicit buffer that is not registered", st_unregistered_idx,
                "method=0xE924 head=1 arg=0x80000105 buffer=5 reason=unregistered-buffer");
    expect_stop("an offset no display buffer carries", st_offset_unmatched,
                "method=0xE924 head=1 arg=0x00012340 buffer=none reason=offset-unmatched");
    expect_stop("queue index 2 (0xE948)", st_queue_index_2,
                "method=0xE948 head=2 arg=0x00000003 buffer=none reason=queue-index-unmodelled");
    expect_stop("queue index 7 (0xE95C)", st_queue_index_7,
                "method=0xE95C head=7 arg=0x80000000 buffer=none reason=queue-index-unmodelled");
    expect_stop("a queued buffer above 7", st_queued_above_7,
                "method=0xE924 head=1 arg=0x8000010F buffer=9 reason=unregistered-buffer");
    expect_stop("a registered buffer nothing was rendered at", st_no_surface,
                "method=0xE924 head=1 arg=0x8000010F buffer=3 reason=no-surface");
    expect_stop("a runner naming an unregistered buffer", st_present_buffer,
                "method=0x0000 head=0 arg=0x00000005 buffer=5 reason=unregistered-buffer");

    printf(g_failures ? "\n%d scenario(s) FAILED\n" : "\nall scenarios passed\n", g_failures);
    free(stub_guest);
    return g_failures ? 1 : 0;
}

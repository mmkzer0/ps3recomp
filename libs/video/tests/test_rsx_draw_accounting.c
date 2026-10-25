/*
 * ps3recomp - the draw engine's accounting: M3 gaps 3e (dropped draws), 3g
 * (pipeline failures) and 3d (frame identity)
 *
 * The engine is driven through rsx_draw_engine_method() as the FIFO walker
 * drives it, against a recording stub backend and a synthetic guest memory.
 * What is asserted is the contract a gated frame relies on:
 *
 *   - every draw group is one submitted draw, and it is either executed or
 *     counted under exactly one reason: submitted = executed + sum(dropped);
 *   - a reason's counter and its log both exist, so no drop path is silent;
 *   - pipeline failures are counted per fragment program, not only logged;
 *   - the identity records the flip ordinal and the present ordinal separately,
 *     because about three presents share one flip and neither can be derived
 *     from the other.
 */
#include "rsx_draw_engine_test_stub.h"

static int g_failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("[PASS] " __VA_ARGS__); printf("\n");                      \
        } else {                                                              \
            printf("[FAIL] " __VA_ARGS__);                                    \
            printf("   (%s:%d)\n", __FILE__, __LINE__);                       \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

/* The identity every accounting result has to satisfy. */
static void check_sum(const char* when)
{
    rsx_draw_engine_stats st;
    rsx_draw_engine_draw_stats(&st);
    CHECK(st.submitted == st.executed + st.dropped,
          "%s: submitted %llu = executed %llu + dropped %llu",
          when, st.submitted, st.executed, st.dropped);
}

static void test_executed_draw(void)
{
    printf("-- an executed draw\n");
    stub_engine_up(1);
    rsx_draw_engine_reset_draw_stats();
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);

    rsx_draw_engine_stats st;
    rsx_draw_engine_draw_stats(&st);
    CHECK(st.submitted == 1 && st.executed == 1 && st.dropped == 0,
          "one submitted, one executed, none dropped (%llu/%llu/%llu)",
          st.submitted, st.executed, st.dropped);
    CHECK(stub.n_draw == 1, "the backend saw the draw");
    check_sum("executed draw");
    stub_engine_down();
}

static void test_no_packets(void)
{
    printf("-- BEGIN_END with no draw packet\n");
    stub_engine_up(1);
    rsx_draw_engine_reset_draw_stats();
    sm(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    sm(M_BEGIN_END, 0);

    rsx_draw_engine_stats st;
    rsx_draw_engine_draw_stats(&st);
    CHECK(st.by_reason[RSX_DROP_NO_PACKETS] == 1, "counted no-packets (%llu)",
          st.by_reason[RSX_DROP_NO_PACKETS]);
    CHECK(st.submitted == 1 && st.executed == 0 && st.dropped == 1,
          "submitted 1, executed 0, dropped 1 (%llu/%llu/%llu)",
          st.submitted, st.executed, st.dropped);
    CHECK(strcmp(rsx_drop_reason_name(RSX_DROP_NO_PACKETS), "no-packets") == 0,
          "the reason has a name");
    check_sum("no packets");
    stub_engine_down();
}

static void test_no_targets(void)
{
    printf("-- a draw with no colour target\n");
    stub_engine_up(1);
    rsx_draw_engine_reset_draw_stats();
    stub.fail_color = 1;                 /* the backend cannot make a target */
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);

    rsx_draw_engine_stats st;
    rsx_draw_engine_draw_stats(&st);
    CHECK(st.by_reason[RSX_DROP_NO_TARGETS] == 1, "counted no-targets (%llu)",
          st.by_reason[RSX_DROP_NO_TARGETS]);
    CHECK(st.executed == 0 && st.dropped == 1, "not executed, one drop");
    check_sum("no targets");
    stub_engine_down();
}

static void test_fetch_failed(void)
{
    printf("-- a draw whose vertex span is not guest memory\n");
    stub_engine_up(1);
    rsx_draw_engine_reset_draw_stats();
    /* Main-memory offset past the end of the synthetic VM: the span check
     * rejects it before any vertex is decoded. */
    sm(M_VTXBUF_OFFSET, 0x80000000u | 0x00FFFFF8u);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);

    rsx_draw_engine_stats st;
    rsx_draw_engine_draw_stats(&st);
    CHECK(st.by_reason[RSX_DROP_FETCH_FAILED] == 1, "counted fetch-failed (%llu)",
          st.by_reason[RSX_DROP_FETCH_FAILED]);
    CHECK(st.executed == 0 && st.dropped == 1, "not executed, one drop");
    check_sum("fetch failed");
    stub_engine_down();
}

static void test_pipeline_failure(void)
{
    printf("-- a pipeline that will not build\n");
    stub_engine_up(1);
    rsx_draw_engine_reset_draw_stats();
    stub.fail_pipelines = 1;
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);

    rsx_draw_engine_stats st;
    rsx_draw_engine_draw_stats(&st);
    CHECK(st.by_reason[RSX_DROP_PIPELINE_FAILED] == 1, "counted pipeline-failed (%llu)",
          st.by_reason[RSX_DROP_PIPELINE_FAILED]);
    CHECK(st.executed == 0 && st.dropped == 1, "not executed, one drop");

    rsx_pipeline_failure fails[8];
    const u32 n = rsx_draw_engine_pipeline_failures(fails, 8);
    unsigned long long total = 0;
    for (u32 i = 0; i < n && i < 8; i++) total += fails[i].count;
    CHECK(n == 1 && total == 1, "one program failed once (%u program(s), %llu failure(s))",
          n, total);
    check_sum("pipeline failed");
    stub_engine_down();
}

static void test_frame_identity(void)
{
    printf("-- the frame identity\n");
    stub_engine_up(1);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);   /* a current surface to present */
    rsx_draw_engine_reset_draw_stats();

    rsx_frame_identity fid;
    CHECK(rsx_draw_engine_frame_identity(&fid) == 0,
          "no identity before the first present");

    /* The queue word alone presents, but is not a flip head. */
    sm(M_GCM_QUEUE_FLIP, 2u);
    CHECK(rsx_draw_engine_frame_identity(&fid) == 1, "a present has an identity");
    CHECK(fid.present_seq == 0, "the first present is seq 0 (%u)", fid.present_seq);
    CHECK(fid.flip_id == RSX_FRAME_NO_FLIP,
          "no flip head yet: flip_id is none, not an invented 0 (%u)", fid.flip_id);
    CHECK(fid.display_buffer == 2, "the queued buffer is recorded (%u)", fid.display_buffer);

    /* The head is the present that retires a flip. */
    sm(M_GCM_FLIP_HEAD, 0x8000010Fu);
    CHECK(rsx_draw_engine_frame_identity(&fid) == 1, "the head present has an identity");
    CHECK(fid.present_seq == 1, "the head present is seq 1 (%u)", fid.present_seq);
    CHECK(fid.flip_id == 0, "the head present carries flip 0 (%u)", fid.flip_id);
    CHECK(fid.display_buffer == 2, "the head keeps the queued buffer (%u)", fid.display_buffer);

    /* A second flip: the counters advance independently. */
    stub_flip(3u);
    CHECK(rsx_draw_engine_frame_identity(&fid) == 1 &&
          fid.present_seq == 3 && fid.flip_id == 1 && fid.display_buffer == 3,
          "second flip: seq %u flip %u buffer %u", fid.present_seq, fid.flip_id,
          fid.display_buffer);
    CHECK(stub.n_present == 4, "four presents for two flips (%d)", stub.n_present);
    check_sum("frame identity");
    stub_engine_down();
}

/* One session with several outcomes: the identity is aggregate, not per case. */
static void test_mixed_session(void)
{
    printf("-- a mixed session\n");
    stub_engine_up(1);
    rsx_draw_engine_reset_draw_stats();

    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);       /* executed */
    sm(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);      /* no packets */
    sm(M_BEGIN_END, 0);
    sm(M_VTXBUF_OFFSET, 0x80000000u | 0x00FFFFF8u);/* fetch fails */
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);

    rsx_draw_engine_stats st;
    rsx_draw_engine_draw_stats(&st);
    CHECK(st.submitted == 3 && st.executed == 1 && st.dropped == 2,
          "3 submitted, 1 executed, 2 dropped (%llu/%llu/%llu)",
          st.submitted, st.executed, st.dropped);
    CHECK(st.by_reason[RSX_DROP_NO_PACKETS] == 1 &&
          st.by_reason[RSX_DROP_FETCH_FAILED] == 1,
          "one drop of each reason");
    check_sum("mixed session");
    stub_engine_down();
}

int main(void)
{
    stub_guest_init();
    test_executed_draw();
    test_no_packets();
    test_no_targets();
    test_fetch_failed();
    test_pipeline_failure();
    test_frame_identity();
    test_mixed_session();

    free(stub_guest);
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n",
           g_failures);
    return g_failures ? 1 : 0;
}

/*
 * ps3recomp - what rsx_dispatch_method does beyond storing the register, method by method
 *
 * rsx_dispatch_method answers "does this method do anything besides the register store" from a
 * per-method table, so a method missing from that table would silently lose its effect: a draw not
 * issued, a constant not uploaded, a vertex attribute not folded. This pins the effects from the
 * outside, against the documented method numbers (rsx_dispatch.c header, rsx_dispatch.h) and not
 * against the table:
 *
 *   - every other method, over the whole method space, stores its register and changes nothing else
 *     (no callback, no program or constant write, no other register);
 *   - each method that does something does it, and does it once: the execution callbacks, the two
 *     upload windows, the vertex-attribute encodings that fold into the current-value registers, and
 *     the eight words that complete an immediate-mode position (only inside BEGIN);
 *   - a word past the register file is dropped.
 */
#include "rsx_dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static struct {
    u32 clear, begin, end, draw_arrays, draw_index, inline_array, queue, flip, unmodelled, immediate;
    u32 last_arg0, last_arg1;
    u32 inline_bytes;
} calls;

static void s_clear(void* u, const rsx_dispatch* r, u32 mask) { (void)u; (void)r; calls.clear++; calls.last_arg0 = mask; }
static void s_begin(void* u, const rsx_dispatch* r, u32 prim) { (void)u; (void)r; calls.begin++; calls.last_arg0 = prim; }
static void s_end(void* u, const rsx_dispatch* r) { (void)u; (void)r; calls.end++; }
static void s_draw_arrays(void* u, const rsx_dispatch* r, u32 first, u32 count)
{ (void)u; (void)r; calls.draw_arrays++; calls.last_arg0 = first; calls.last_arg1 = count; }
static void s_draw_index(void* u, const rsx_dispatch* r, u32 first, u32 count)
{ (void)u; (void)r; calls.draw_index++; calls.last_arg0 = first; calls.last_arg1 = count; }
static void s_inline(void* u, const rsx_dispatch* r, const u8* data, u32 bytes)
{ (void)u; (void)r; (void)data; calls.inline_array++; calls.inline_bytes = bytes; }
static void s_queue(void* u, const rsx_dispatch* r, u32 head, u32 buffer)
{ (void)u; (void)r; calls.queue++; calls.last_arg0 = head; calls.last_arg1 = buffer; }
static void s_flip(void* u, const rsx_dispatch* r, u32 head, u32 arg)
{ (void)u; (void)r; calls.flip++; calls.last_arg0 = head; calls.last_arg1 = arg; }
static void s_unmodelled(void* u, const rsx_dispatch* r, u32 method, u32 arg)
{ (void)u; (void)r; calls.unmodelled++; calls.last_arg0 = method; calls.last_arg1 = arg; }
static void s_immediate(void* u, const rsx_dispatch* r) { (void)u; (void)r; calls.immediate++; }

static u32 total_calls(void)
{
    return calls.clear + calls.begin + calls.end + calls.draw_arrays + calls.draw_index +
           calls.inline_array + calls.queue + calls.flip + calls.unmodelled + calls.immediate;
}

static rsx_dispatch* new_dispatcher(void)
{
    static const rsx_dispatch_sink sink = {
        NULL, s_clear, s_begin, s_end, s_draw_arrays, s_draw_index, s_inline, s_queue, s_flip,
        s_unmodelled, s_immediate,
    };
    rsx_dispatch* d = (rsx_dispatch*)malloc(sizeof *d);
    if (!d) { puts("FAIL: allocation"); exit(1); }
    rsx_dispatch_init(d, &sink);
    memset(&calls, 0, sizeof calls);
    return d;
}

/* The methods with an effect beyond the store, from the documented numbers. */
static int in_range(u32 m, u32 lo, u32 hi) { return m >= lo && m < hi; }
static int is_fold(u32 m)
{
    return in_range(m, 0x1500, 0x1A00) || in_range(m, 0x1E40, 0x1E80) || in_range(m, 0x0A80, 0x0B00);
}
static int is_vp_window(u32 m) { return in_range(m, 0x0B80, 0x0C00) || in_range(m, 0x1F00, 0x1F80); }
static int is_exec(u32 m)
{
    return m == 0x1D94 || m == 0x1808 || m == 0x1818 || m == 0x1814 || m == 0x1824 ||
           in_range(m, 0xE920, 0xE928) || in_range(m, 0xE940, 0xE960);
}
static int is_immediate_word(u32 m)
{
    return m == 0x1508 || m == 0x1C0C || m == 0x1884 || m == 0x1E40 || m == 0x1900 || m == 0x1940 ||
           m == 0x1984 || m == 0x0A84;
}

static u32 f_bits(float f) { u32 v; memcpy(&v, &f, 4); return v; }

/* Every other method stores its register and nothing else, with and without BEGIN open. */
static void test_plain_methods(void)
{
    rsx_dispatch* d = new_dispatcher();
    rsx_dispatch* before = (rsx_dispatch*)malloc(sizeof *before);
    unsigned checked = 0, bad = 0;
    for (int in_begin = 0; in_begin < 2; in_begin++) {
        if (in_begin) rsx_dispatch_method(d, 0x1808, 5u);          /* BEGIN_END(TRIANGLES) */
        for (u32 m = 0; m < 0x10000u; m += 4) {
            if (is_fold(m) || is_vp_window(m) || is_exec(m) || is_immediate_word(m)) continue;
            memcpy(before, d, sizeof *d);
            const u32 calls_before = total_calls();
            rsx_dispatch_method(d, m, 0x12345678u + m);
            const u32 idx = m >> 2;
            int ok = total_calls() == calls_before && d->regs[idx] == 0x12345678u + m &&
                     d->seen[idx] == before->seen[idx] + 1u &&
                     d->constants_epoch == before->constants_epoch &&
                     !memcmp(d->vp, before->vp, sizeof d->vp) &&
                     !memcmp(d->constants, before->constants, sizeof d->constants) &&
                     d->in_begin_end == before->in_begin_end && d->inline_len == before->inline_len;
            /* no other register moved: put the stored one back and compare the whole file */
            if (ok) {
                const u32 stored = d->regs[idx];
                d->regs[idx] = before->regs[idx];
                ok = !memcmp(d->regs, before->regs, sizeof d->regs);
                d->regs[idx] = stored;
            }
            if (!ok && bad++ < 4) printf("   plain method 0x%04X changed more than its register\n", m);
            checked++;
        }
    }
    CHECK(!bad, "%u plain method writes (outside and inside BEGIN) store the register and nothing else", checked);
    free(before);
    free(d);
}

static void test_execution_methods(void)
{
    rsx_dispatch* d = new_dispatcher();

    rsx_dispatch_method(d, 0x1D94, 0xF1u);
    CHECK(calls.clear == 1 && calls.last_arg0 == 0xF1u && total_calls() == 1, "CLEAR_BUFFERS calls clear once with its mask");

    memset(&calls, 0, sizeof calls);
    rsx_dispatch_method(d, 0x1808, 5u);
    CHECK(calls.begin == 1 && calls.last_arg0 == 5u && d->in_begin_end && total_calls() == 1,
          "BEGIN_END(prim) opens a primitive and calls begin once");
    rsx_dispatch_method(d, 0x1814, (7u << 24) | 0x40u);
    CHECK(calls.draw_arrays == 1 && calls.last_arg0 == 0x40u && calls.last_arg1 == 8u,
          "DRAW_ARRAYS hands over first and count (the field is count - 1)");
    rsx_dispatch_method(d, 0x1824, (3u << 24) | 0x10u);
    CHECK(calls.draw_index == 1 && calls.last_arg0 == 0x10u && calls.last_arg1 == 4u,
          "DRAW_INDEX_ARRAY hands over first and count");
    rsx_dispatch_method(d, 0x1818, 0x01020304u);
    rsx_dispatch_method(d, 0x1818, 0x05060708u);
    CHECK(d->inline_len == 8u && d->inline_data[0] == 1u && d->inline_data[7] == 8u && calls.inline_array == 0,
          "INLINE_ARRAY words accumulate in guest byte order and are not delivered until END");
    memset(&calls, 0, sizeof calls);
    rsx_dispatch_method(d, 0x1808, 0u);
    CHECK(calls.inline_array == 1 && calls.inline_bytes == 8u && calls.end == 1 && !d->in_begin_end &&
          d->inline_len == 0,
          "BEGIN_END(0) delivers the inline stream, then ends the primitive");

    memset(&calls, 0, sizeof calls);
    rsx_dispatch_method(d, 0xE920, 0x80000100u);
    CHECK(calls.flip == 1 && calls.last_arg0 == 0u && calls.last_arg1 == 0x80000100u && total_calls() == 1,
          "GCM_FLIP_HEAD 0 reaches flip with head 0");
    memset(&calls, 0, sizeof calls);
    rsx_dispatch_method(d, 0xE924, 0x8000010Fu);
    CHECK(calls.flip == 1 && calls.last_arg0 == 1u && calls.last_arg1 == 0x8000010Fu && total_calls() == 1,
          "GCM_FLIP_HEAD 1 reaches flip with head 1");
    memset(&calls, 0, sizeof calls);
    rsx_dispatch_method(d, 0xE940, 3u);
    CHECK(calls.queue == 1 && calls.last_arg0 == 0u && calls.last_arg1 == 3u && d->flip_queues.valid[0] &&
          d->flip_queues.buffer[0] == 3u && total_calls() == 1,
          "GCM_DRIVER_QUEUE 0 notes the head's buffer and reaches queue");
    memset(&calls, 0, sizeof calls);
    rsx_dispatch_method(d, 0xE944, 6u);
    CHECK(calls.queue == 1 && calls.last_arg0 == 1u && calls.last_arg1 == 6u && d->flip_queues.valid[1] &&
          d->flip_queues.buffer[1] == 6u && total_calls() == 1,
          "GCM_DRIVER_QUEUE 1 notes the head's buffer and reaches queue");
    unsigned unmodelled_ok = 0;
    for (u32 m = 0xE948; m <= 0xE95C; m += 4) {
        memset(&calls, 0, sizeof calls);
        rsx_dispatch_method(d, m, m);
        unmodelled_ok += calls.unmodelled == 1 && calls.last_arg0 == m && calls.last_arg1 == m && total_calls() == 1;
    }
    CHECK(unmodelled_ok == 6, "GCM_DRIVER_QUEUE indices 2..7 reach driver_unmodelled each, once");
    memset(&calls, 0, sizeof calls);
    rsx_dispatch_method(d, 0xE960, 1u);
    rsx_dispatch_method(d, 0xE91C, 1u);
    CHECK(total_calls() == 0, "the words either side of the driver blocks do nothing");
    free(d);
}

static void test_upload_windows(void)
{
    rsx_dispatch* d = new_dispatcher();

    /* The program window: word k of a burst goes to slot load * 4 + (k % 4), and the load pointer
     * advances after each completed vec4. */
    d->regs[0x1E9C >> 2] = 2u;
    for (u32 k = 0; k < 8; k++) rsx_dispatch_method(d, 0x0B80 + k * 4, 0xA0u + k);
    int program_ok = d->regs[0x1E9C >> 2] == 4u;
    for (u32 k = 0; k < 8; k++) program_ok &= d->vp[2 * 4 + k] == 0xA0u + k;
    CHECK(program_ok, "VP_UPLOAD_INST writes the program at the load pointer and advances it per vec4");
    const u32 vp_before = d->vp[0];
    rsx_dispatch_method(d, 0x0B7C, 0x77u);
    rsx_dispatch_method(d, 0x0C00, 0x77u);
    CHECK(d->vp[0] == vp_before && d->regs[0x1E9C >> 2] == 4u, "the words just outside the program window do not upload");

    /* The constant window: word i goes to constants[id + i / 4][i % 4]; the epoch moves for a write
     * that lands and the load id does not move. */
    d->regs[0x1EFC >> 2] = 10u;
    const u64 epoch = d->constants_epoch;
    for (u32 i = 0; i < 8; i++) rsx_dispatch_method(d, 0x1F00 + i * 4, 0xC0u + i);
    int const_ok = d->regs[0x1EFC >> 2] == 10u && d->constants_epoch == epoch + 8u;
    for (u32 i = 0; i < 8; i++) const_ok &= d->constants[10 + i / 4][i % 4] == 0xC0u + i;
    CHECK(const_ok, "VP_UPLOAD_CONST writes constants[id + i/4][i mod 4] and bumps the epoch per write");
    d->regs[0x1EFC >> 2] = RSX_DSP_NUM_CONSTANTS;
    const u64 epoch2 = d->constants_epoch;
    rsx_dispatch_method(d, 0x1F00, 1u);
    CHECK(d->constants_epoch == epoch2, "a constant write that lands nowhere does not move the epoch");
    d->regs[0x1EFC >> 2] = 3u;              /* an id a window write would land on */
    rsx_dispatch_method(d, 0x1F80, 0x99u);
    CHECK(d->constants_epoch == epoch2, "the word just past the constant window does not upload");
    free(d);
}

static void test_vertex_folds(void)
{
    rsx_dispatch* d = new_dispatcher();
    float v[4];

    rsx_dispatch_method(d, 0x1E40 + 4u * 2u, f_bits(2.5f));                       /* DATA1F, attribute 2 */
    rsx_dsp_vertex_default(d, 2, v);
    CHECK(v[0] == 2.5f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 1.0f, "DATA1F folds to (x,0,0,1)");

    rsx_dispatch_method(d, 0x1880 + 8u * 3u, f_bits(1.5f));                        /* DATA2F x, attribute 3 */
    rsx_dispatch_method(d, 0x1884 + 8u * 3u, f_bits(-4.0f));                       /* DATA2F y */
    rsx_dsp_vertex_default(d, 3, v);
    CHECK(v[0] == 1.5f && v[1] == -4.0f && v[2] == 0.0f && v[3] == 1.0f, "DATA2F folds to (x,y,0,1)");

    rsx_dispatch_method(d, 0x1500 + 16u * 4u, f_bits(7.0f));                       /* DATA3F x, attribute 4 */
    rsx_dispatch_method(d, 0x1504 + 16u * 4u, f_bits(8.0f));
    rsx_dispatch_method(d, 0x1508 + 16u * 4u, f_bits(9.0f));
    rsx_dsp_vertex_default(d, 4, v);
    CHECK(v[0] == 7.0f && v[1] == 8.0f && v[2] == 9.0f && v[3] == 1.0f, "DATA3F folds to (x,y,z,1)");

    rsx_dispatch_method(d, 0x1900 + 4u * 5u, 0xFFFE0003u);                         /* DATA2S, attribute 5: (3, -2) */
    rsx_dsp_vertex_default(d, 5, v);
    CHECK(v[0] == 3.0f && v[1] == -2.0f && v[2] == 0.0f && v[3] == 1.0f, "DATA2S folds the signed halves unnormalised");

    rsx_dispatch_method(d, 0x1940 + 4u * 6u, 0xFF008000u);                         /* DATA4UB, attribute 6 */
    rsx_dsp_vertex_default(d, 6, v);
    CHECK(v[0] == 0.0f && v[1] == 128.0f / 255.0f && v[2] == 0.0f && v[3] == 1.0f, "DATA4UB folds the bytes normalised");

    rsx_dispatch_method(d, 0x1980 + 8u * 7u, 0x00020001u);                         /* DATA4S, attribute 7 */
    rsx_dispatch_method(d, 0x1984 + 8u * 7u, 0xFFFC0003u);
    rsx_dsp_vertex_default(d, 7, v);
    CHECK(v[0] == 1.0f && v[1] == 2.0f && v[2] == 3.0f && v[3] == -4.0f, "DATA4S folds two words of two halves");

    rsx_dispatch_method(d, 0x0A80 + 8u * 8u, 0x7FFF0000u);                         /* SCALED4S, attribute 8 */
    rsx_dsp_vertex_default(d, 8, v);
    CHECK(v[0] == 0.0f && v[1] > 0.99999f && v[1] < 1.00001f, "DATA_SCALED4S folds normalised");
    free(d);
}

static void test_immediate_vertex(void)
{
    rsx_dispatch* d = new_dispatcher();
    static const u32 completing[] = { 0x1508, 0x1C0C, 0x1884, 0x1E40, 0x1900, 0x1940, 0x1984, 0x0A84 };
    unsigned outside = 0, inside = 0;
    for (unsigned i = 0; i < sizeof completing / sizeof completing[0]; i++) {
        rsx_dispatch_method(d, completing[i], 0u);
        outside += calls.immediate;
    }
    CHECK(outside == 0, "no immediate vertex outside BEGIN");
    rsx_dispatch_method(d, 0x1808, 5u);
    for (unsigned i = 0; i < sizeof completing / sizeof completing[0]; i++) {
        const u32 before = calls.immediate;
        rsx_dispatch_method(d, completing[i], 0u);
        inside += calls.immediate == before + 1u;
    }
    CHECK(inside == sizeof completing / sizeof completing[0],
          "each word that completes a position emits one immediate vertex inside BEGIN");
    const u32 before = calls.immediate;
    rsx_dispatch_method(d, 0x1504, 0u);     /* DATA3F Y: not the completing word */
    rsx_dispatch_method(d, 0x1C00, 0u);     /* DATA4F X */
    rsx_dispatch_method(d, 0x1880, 0u);     /* DATA2F X */
    rsx_dispatch_method(d, 0x1980, 0u);     /* DATA4S first word */
    CHECK(calls.immediate == before, "the words before the completing one emit nothing");

    /* with no callback registered the dispatcher still stores and folds */
    rsx_dispatch* nocb = (rsx_dispatch*)malloc(sizeof *nocb);
    rsx_dispatch_init(nocb, NULL);
    rsx_dispatch_method(nocb, 0x1808, 5u);
    rsx_dispatch_method(nocb, 0x1508, 0u);
    CHECK(nocb->regs[0x1508 >> 2] == 0u && nocb->seen[0x1508 >> 2] == 1u, "a missing sink callback is not called");
    free(nocb);
    free(d);
}

static void test_out_of_range(void)
{
    rsx_dispatch* d = new_dispatcher();
    rsx_dispatch* before = (rsx_dispatch*)malloc(sizeof *before);
    memcpy(before, d, sizeof *d);
    rsx_dispatch_method(d, 0x10000u, 1u);
    /* The method is masked to 20 bits first: 0xFFFFC is past the register file too. */
    rsx_dispatch_method(d, 0xFFFFCu, 1u);
    CHECK(!memcmp(d, before, sizeof *d), "a method past the register file is dropped without a trace");
    free(before);
    free(d);
}

int main(void)
{
    test_plain_methods();
    test_execution_methods();
    test_upload_windows();
    test_vertex_folds();
    test_immediate_vertex();
    test_out_of_range();
    printf("test_rsx_dispatch_special: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}

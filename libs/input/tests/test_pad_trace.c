/* pad_trace.c: what a recording writes, and what a replay answers.
 *
 *   test_pad_trace <case>              checks of the writer, the reader and the lookup
 *   test_pad_trace stop <name>         one malformed trace, which must end the run with kind pad-trace
 *
 * Synthetic data only: the states are made up, the flip count is a variable the test moves, and the files
 * live in a scratch directory under the working directory. A stop case ends the process (exit 70, one
 * PS3STOP line), so ctest matches that line and each case names its own file.
 */
#include "../pad_trace.h"
#include "ps3emu/replay_sync.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("[FAIL] %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            g_fail = 1;                                                        \
        }                                                                      \
    } while (0)

static uint32_t g_flip;
static uint32_t flip_now(void) { return g_flip; }

static char g_dir[64];
static char g_files[64][160];
static int  g_nfiles;

static const char* scratch(const char* name)
{
    static char path[16][200];
    static int n;
    char* p = path[n++ % 16];
    if (!g_dir[0]) {
        snprintf(g_dir, sizeof g_dir, "pad_trace_test.XXXXXX");
        if (!mkdtemp(g_dir)) { perror("mkdtemp"); exit(2); }
    }
    snprintf(p, 200, "%s/%s", g_dir, name);
    if (g_nfiles < 64) snprintf(g_files[g_nfiles++], sizeof g_files[0], "%s", p);
    return p;
}

static void cleanup(void)
{
    for (int i = 0; i < g_nfiles; i++) unlink(g_files[i]);
    if (g_dir[0]) rmdir(g_dir);
}

static void write_text(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    if (!f || fputs(text, f) < 0 || fclose(f)) { perror(path); exit(2); }
}

static char* read_text(const char* path)
{
    FILE* f = fopen(path, "rb");
    char* b = malloc(1 << 20);
    size_t n;
    if (!f || !b) { perror(path); exit(2); }
    n = fread(b, 1, (1 << 20) - 1, f);
    b[n] = 0;
    fclose(f);
    return b;
}

static int count_sub(const char* hay, const char* needle)
{
    int n = 0;
    for (const char* p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

#define HEADER "{\"format\":\"ps3recomp-pad-trace\",\"version\":3,\"clock\":\"guest-polls\",\"ports\":7}\n"
/* A press at seq 0, the same words with the host's len 0 at seq 2, and the release at seq 5. */
#define L_STATE_0 "{\"seq\":0,\"port\":0,\"len\":8,\"words\":[0,116,0,64,128,128,128,128],\"flip\":3}\n"
#define L_STATE_2 "{\"seq\":2,\"port\":0,\"len\":0,\"words\":[0,116,0,64,128,128,128,128],\"flip\":3}\n"
#define L_STATE_5 "{\"seq\":5,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":7}\n"

static PadTraceState mk(int len, unsigned d1, unsigned d2, unsigned lx)
{
    PadTraceState s;
    memset(&s, 0, sizeof s);
    s.len = len;
    if (len) {
        s.words[1] = (uint16_t)(0x70 | (len / 2));
        s.words[2] = (uint16_t)d1;
        s.words[3] = (uint16_t)d2;
        s.words[4] = s.words[5] = s.words[7] = 128;
        s.words[6] = (uint16_t)lx;
    }
    return s;
}

static int same(const PadTraceState* a, const PadTraceState* b)
{
    return a->len == b->len && memcmp(a->words, b->words, sizeof a->words) == 0;
}

/* A replay with an explicit pad.replay_repeat; NULL (and *e filled) when the
 * trace refuses it. */
static PadTrace* open_trace_repeat(const char* path, uint32_t repeat, PadTraceError* e)
{
    PadTraceOptions o = { NULL, path, flip_now, repeat };
    PadTrace* t = NULL;
    memset(e, 0, sizeof *e);
    if (pad_trace_create(&o, &t, e) != 0) return NULL;
    return t;
}

static PadTrace* open_trace(const char* rec, const char* rep)
{
    PadTraceOptions o = { rec, rep, flip_now, 1u };
    PadTrace* t = NULL;
    PadTraceError e;
    memset(&e, 0, sizeof e);
    if (pad_trace_create(&o, &t, &e) != 0 || !t) {
        printf("[FAIL] cannot open a trace: line %u reason %s: %s\n", e.line, e.reason, e.detail);
        exit(1);
    }
    return t;
}

static const PadTraceState k_neutral_marker = { 8, { 0xAAAA, 0xBBBB } };

/* One poll of `port` at the current flip; returns what the guest gets. */
static PadTraceState poll(PadTrace* t, uint32_t port, const PadTraceState* live)
{
    PadTraceState st = *live;
    pad_trace_poll(t, port, &st, &k_neutral_marker);
    return st;
}

/* ---- round trip ------------------------------------------------------------------------------------- */

static void case_roundtrip(void)
{
    const char* path = scratch("roundtrip.trace");
    const PadTraceState A = mk(8, 0, 0, 128), B = mk(8, 0, 0x40, 128), C = mk(8, 0x08, 0x20, 200), D = mk(0, 0, 0, 0);
    PadTrace* t;
    PadTraceStats s;
    uint8_t on[PAD_TRACE_MAX_PORTS];
    char* text;

    /* Record: port 0 polls 5 times, port 1 twice, three info calls. */
    t = open_trace(path, NULL);
    g_flip = 5;
    poll(t, 0, &A); poll(t, 0, &A); poll(t, 0, &B);
    memset(on, 0, sizeof on); on[0] = 1;
    pad_trace_conn(t, on); pad_trace_conn(t, on);
    g_flip = 6;
    poll(t, 0, &B);
    g_flip = 9;
    poll(t, 0, &C);
    poll(t, 1, &D); poll(t, 1, &D);
    on[1] = 1;
    pad_trace_conn(t, on);
    pad_trace_destroy(t);

    text = read_text(path);
    CHECK(strncmp(text, HEADER, strlen(HEADER)) == 0, "the file starts with the version-3 header");
    CHECK(count_sub(text, "\"seq\":") == 5, "four state records and the end record, got %d", count_sub(text, "\"seq\":"));
    CHECK(count_sub(text, "\"info\":") == 2, "a connection record only when the connection changes, got %d", count_sub(text, "\"info\":"));
    CHECK(count_sub(text, "\"polls\":") == 0, "no per-flip poll counts in version 3");
    CHECK(strstr(text, "\"seq\":0,\"port\":0,\"len\":8,") != NULL, "a state record is keyed by the port's poll ordinal");
    CHECK(strstr(text, "\"end\":true,\"seq\":[5,2,0,0,0,0,0]}") != NULL,
          "the end record holds each port's poll count");
    CHECK(strchr(text, '/') == NULL && strstr(text, g_dir) == NULL, "no host path in the file");
    free(text);

    /* Replay: the same poll sequence, the same flips. */
    t = open_trace(NULL, path);
    g_flip = 5;
    { PadTraceState r = poll(t, 0, &D); CHECK(same(&r, &A), "poll 0: A, the record's own seq"); }
    { PadTraceState r = poll(t, 0, &D); CHECK(same(&r, &A), "poll 1: A held"); }
    { PadTraceState r = poll(t, 0, &D); CHECK(same(&r, &B), "poll 2: B, the change"); }
    memset(on, 1, sizeof on);
    pad_trace_conn(t, on);
    CHECK(on[0] == 1 && on[1] == 0 && on[6] == 0, "info call 0 is port 0 only");
    memset(on, 0, sizeof on);
    pad_trace_conn(t, on);
    CHECK(on[0] == 1 && on[1] == 0, "info call 1 holds it");
    g_flip = 6;
    { PadTraceState r = poll(t, 0, &D); CHECK(same(&r, &B), "poll 3: B held (no record at this ordinal)"); }
    g_flip = 9;
    { PadTraceState r = poll(t, 0, &D); CHECK(same(&r, &C), "poll 4: C"); }
    { PadTraceState r = poll(t, 1, &A); CHECK(same(&r, &D), "port 1 poll 0: the recorded empty packet"); }
    { PadTraceState r = poll(t, 1, &A); CHECK(same(&r, &D), "port 1 poll 1: held"); }
    memset(on, 0, sizeof on);
    pad_trace_conn(t, on);
    CHECK(on[0] == 1 && on[1] == 1 && on[2] == 0, "info call 2 adds port 1");
    pad_trace_stats(t, &s);
    CHECK(s.polls == 7 && s.exact == 4 && s.held == 3, "polls %llu exact %llu held %llu", (unsigned long long)s.polls,
          (unsigned long long)s.exact, (unsigned long long)s.held);
    CHECK(s.records == 4 && s.records_reached == 4, "records %llu reached %llu", (unsigned long long)s.records,
          (unsigned long long)s.records_reached);
    CHECK(s.have_drift && s.drift_min == 0 && s.drift_max == 0,
          "the same poll sequence under the same flips drifts by nothing (%lld:%lld)",
          (long long)s.drift_min, (long long)s.drift_max);
    CHECK(s.end_seq[0] == 5 && s.end_seq[1] == 2, "the end record holds each port's own count (%llu, %llu)",
          (unsigned long long)s.end_seq[0], (unsigned long long)s.end_seq[1]);
    CHECK(!s.ended, "not past either port's end yet");
    g_flip = 10;
    { PadTraceState r = poll(t, 0, &D); CHECK(same(&r, &k_neutral_marker), "poll 5 is the end of the trace: neutral"); }
    pad_trace_stats(t, &s);
    CHECK(s.ended, "the trace is reported as ended");
    pad_trace_destroy(t);
}

/* ---- the ordinal key across a different flip clock --------------------------------------------------- */

/* The recording's second poll sat in a flip of its own (polls at flips 10, 11 and 12), so the recording's
 * step loop caught up wall time; the replay's did not (the same polls at flips 10, 12 and 13). Every poll
 * still gets the input the recording's poll got, and the summary reports how far the flip clock moved: the
 * guest-time drift no input key can remove. */
static void case_catchup(void)
{
    const char* path = scratch("catchup.trace");
    const PadTraceState press = mk(8, 0, 0x40, 128), release = mk(8, 0, 0, 128);
    PadTrace* t;
    PadTraceStats s;

    t = open_trace(path, NULL);
    g_flip = 10; poll(t, 0, &press);     /* seq 0 */
    g_flip = 11; poll(t, 0, &press);     /* seq 1, unchanged: not written */
    g_flip = 12; poll(t, 0, &release);   /* seq 2 */
    pad_trace_destroy(t);

    /* The same three polls, the flip clock one ahead of the recording from the second on. */
    t = open_trace(NULL, path);
    g_flip = 10;
    { PadTraceState r = poll(t, 0, &release); CHECK(same(&r, &press), "poll 0 (flip 10): the press"); }
    g_flip = 12;
    { PadTraceState r = poll(t, 0, &release); CHECK(same(&r, &press), "poll 1 (flip 12): still the press"); }
    g_flip = 13;
    { PadTraceState r = poll(t, 0, &release); CHECK(same(&r, &release), "poll 2 (flip 13): the release"); }
    pad_trace_stats(t, &s);
    CHECK(s.records == 2 && s.records_reached == 2, "both records were reached (%llu/%llu)",
          (unsigned long long)s.records_reached, (unsigned long long)s.records);
    CHECK(s.have_drift && s.drift_min == 0 && s.drift_max == 1,
          "the drift the key absorbed is reported (%lld:%lld)", (long long)s.drift_min, (long long)s.drift_max);
    pad_trace_destroy(t);
}

/* ---- each port's own end count ---------------------------------------------------------------------- */

/* Capture what opening a trace prints on stdout (its start-up lines), then restore stdout. */
static char* capture_create(const PadTraceOptions* o, PadTrace** out, PadTraceError* err)
{
    const char* path = scratch("stdout.txt");
    int saved = dup(fileno(stdout));
    int rc;
    fflush(stdout);
    if (saved < 0 || !freopen(path, "wb", stdout)) { perror(path); exit(2); }
    rc = pad_trace_create(o, out, err);
    fflush(stdout);
    if (dup2(saved, fileno(stdout)) < 0) { perror("dup2"); exit(2); }
    close(saved);
    (void)rc;
    return read_text(path);
}

/* The end record holds one poll count per port, and a replay compares a port's ordinal with its own: a
 * port polled more often than port 0 is not cut off at port 0's count, nor one polled less often. */
static void case_ends(void)
{
    const char* more = scratch("ends-more.trace");
    const char* less = scratch("ends-less.trace");
    const char* killed = scratch("ends-killed.trace");
    const PadTraceState a = mk(8, 0, 0x40, 128), b = mk(8, 0, 0, 128);
    const PadTraceState c = mk(8, 0x08, 0, 128), d = mk(8, 0x04, 0, 128);
    const PadTraceState any = mk(8, 0xFF, 0xFF, 1);
    PadTrace* t;
    PadTraceStats s;

    /* Port 1 polled four times against port 0's two. */
    write_text(more, HEADER
               "{\"seq\":0,\"port\":0,\"len\":8,\"words\":[0,116,0,64,128,128,128,128],\"flip\":1}\n"
               "{\"seq\":1,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":2}\n"
               "{\"seq\":0,\"port\":1,\"len\":8,\"words\":[0,116,8,0,128,128,128,128],\"flip\":1}\n"
               "{\"seq\":1,\"port\":1,\"len\":8,\"words\":[0,116,4,0,128,128,128,128],\"flip\":1}\n"
               "{\"seq\":2,\"port\":1,\"len\":8,\"words\":[0,116,2,0,128,128,128,128],\"flip\":2}\n"
               "{\"seq\":3,\"port\":1,\"len\":8,\"words\":[0,116,1,0,128,128,128,128],\"flip\":2}\n"
               "{\"end\":true,\"seq\":[2,4,0,0,0,0,0]}\n");
    t = open_trace(NULL, more);
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &a), "port 0 poll 0: its press"); }
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &b), "port 0 poll 1: its release"); }
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &k_neutral_marker), "port 0 poll 2 is its own end: neutral"); }
    { PadTraceState r = poll(t, 1, &any); CHECK(r.len == 8 && r.words[2] == 8, "port 1 poll 0 survives port 0's end"); }
    { PadTraceState r = poll(t, 1, &any); CHECK(r.words[2] == 4, "port 1 poll 1: its second record"); }
    { PadTraceState r = poll(t, 1, &any); CHECK(r.words[2] == 2, "port 1 poll 2: its third record"); }
    { PadTraceState r = poll(t, 1, &any); CHECK(r.words[2] == 1, "port 1 poll 3: its last record"); }
    { PadTraceState r = poll(t, 1, &any); CHECK(same(&r, &k_neutral_marker), "port 1 poll 4 is its own end: neutral"); }
    pad_trace_stats(t, &s);
    CHECK(s.have_end && s.end_seq[0] == 2 && s.end_seq[1] == 4, "the counts are per port (%llu, %llu)",
          (unsigned long long)s.end_seq[0], (unsigned long long)s.end_seq[1]);
    CHECK(s.records_reached == 6, "all six records were reached (%llu)", (unsigned long long)s.records_reached);
    pad_trace_destroy(t);

    /* Port 1 polled twice against port 0's four: the other way round. */
    write_text(less, HEADER
               "{\"seq\":0,\"port\":0,\"len\":8,\"words\":[0,116,0,64,128,128,128,128],\"flip\":1}\n"
               "{\"seq\":1,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":2}\n"
               "{\"seq\":2,\"port\":0,\"len\":8,\"words\":[0,116,8,0,128,128,128,128],\"flip\":3}\n"
               "{\"seq\":3,\"port\":0,\"len\":8,\"words\":[0,116,4,0,128,128,128,128],\"flip\":4}\n"
               "{\"seq\":0,\"port\":1,\"len\":8,\"words\":[0,116,8,0,128,128,128,128],\"flip\":1}\n"
               "{\"seq\":1,\"port\":1,\"len\":8,\"words\":[0,116,4,0,128,128,128,128],\"flip\":2}\n"
               "{\"end\":true,\"seq\":[4,2,0,0,0,0,0]}\n");
    t = open_trace(NULL, less);
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &a), "port 0 poll 0"); }
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &b), "port 0 poll 1"); }
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &c), "port 0 poll 2"); }
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &d), "port 0 poll 3: its last record"); }
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &k_neutral_marker), "port 0 poll 4 is its own end: neutral"); }
    { PadTraceState r = poll(t, 1, &any); CHECK(same(&r, &c), "port 1 poll 0: its first record"); }
    { PadTraceState r = poll(t, 1, &any); CHECK(same(&r, &d), "port 1 poll 1: its last record"); }
    { PadTraceState r = poll(t, 1, &any); CHECK(same(&r, &k_neutral_marker), "port 1 poll 2 is its own end: neutral"); }
    pad_trace_destroy(t);

    /* A killed run: no end record. Every recorded poll is real, so the last record is held, the file
     * still opens, and it says so once. */
    write_text(killed, HEADER
               "{\"seq\":0,\"port\":0,\"len\":8,\"words\":[0,116,0,64,128,128,128,128],\"flip\":1}\n");
    {
        PadTraceOptions o = { NULL, killed, flip_now, 1u };
        PadTraceError e;
        PadTrace* k = NULL;
        char* out;
        memset(&e, 0, sizeof e);
        out = capture_create(&o, &k, &e);
        CHECK(k != NULL, "a trace without an end record still opens: %s %s", e.reason, e.detail);
        CHECK(strstr(out, "no end record") != NULL && strstr(out, "held") != NULL,
              "opening it says the last state of each port is held: %s", out);
        free(out);
        if (k) {
            { PadTraceState r = poll(k, 0, &any); CHECK(same(&r, &a), "poll 0: the press"); }
            { PadTraceState r = poll(k, 0, &any); CHECK(same(&r, &a), "poll 1 holds it: no end count to cross"); }
            pad_trace_stats(k, &s);
            CHECK(!s.have_end && !s.ended, "the replay reports no end record and stays open-ended");
            pad_trace_destroy(k);
        }
    }
}

/* ---- lookup semantics ------------------------------------------------------------------------------- */

static void case_lookup(void)
{
    const char* path = scratch("lookup.trace");
    const PadTraceState press = mk(8, 0, 0x40, 128), release = mk(8, 0, 0, 128);
    const PadTraceState any = mk(8, 0xFF, 0xFF, 1);
    PadTrace* t;
    PadTraceStats s;
    uint8_t on[PAD_TRACE_MAX_PORTS];

    write_text(path, HEADER L_STATE_0 L_STATE_2 L_STATE_5
               "{\"info\":0,\"conn\":[1,1,0,0,0,0,0]}\n"
               "{\"info\":2,\"conn\":[1,0,0,0,0,0,0]}\n"
               "{\"end\":true,\"seq\":[6,0,0,0,0,0,0]}\n");
    t = open_trace(NULL, path);

    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &press), "poll 0 is a record's own seq"); }
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &press), "poll 1 holds the seq-0 record, len 8"); }
    { PadTraceState r = poll(t, 0, &any);
      CHECK(r.len == 0 && !memcmp(r.words, press.words, sizeof r.words), "poll 2 is the recorded drained poll: len 0, same words"); }
    { PadTraceState r = poll(t, 0, &any);
      CHECK(r.len == 0 && !memcmp(r.words, press.words, sizeof r.words), "poll 3 holds the drained record"); }
    { PadTraceState r = poll(t, 0, &any);
      CHECK(r.len == 0 && !memcmp(r.words, press.words, sizeof r.words), "poll 4 holds it"); }
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &release), "poll 5 is the release"); }
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &k_neutral_marker), "poll 6 is past the end: neutral"); }
    { PadTraceState r = poll(t, 1, &any); CHECK(same(&r, &k_neutral_marker), "a port with no record is neutral"); }

    /* The connection is keyed on the info-call ordinal. */
    memset(on, 0, sizeof on);
    pad_trace_conn(t, on);
    CHECK(on[0] == 1 && on[1] == 1, "info call 0: both ports");
    memset(on, 0, sizeof on);
    pad_trace_conn(t, on);
    CHECK(on[0] == 1 && on[1] == 1, "info call 1 holds it");
    memset(on, 0, sizeof on);
    pad_trace_conn(t, on);
    CHECK(on[0] == 1 && on[1] == 0, "info call 2 unplugs port 1");
    pad_trace_stats(t, &s);
    CHECK(s.records == 3 && s.records_reached == 3, "all three records were reached (%llu/%llu)",
          (unsigned long long)s.records_reached, (unsigned long long)s.records);
    CHECK(s.exact == 3, "three polls landed on a record of their own (%llu)", (unsigned long long)s.exact);
    CHECK(s.ended, "the trace is reported as ended");
    pad_trace_destroy(t);
}

/* ---- frame repeat (pad.replay_repeat) --------------------------------------------------------------- */

/* Capture what closing a trace prints on stdout (its replay summary), then restore stdout. */
static char* capture_close(PadTrace* t)
{
    const char* path = scratch("stdout.txt");
    int saved = dup(fileno(stdout));
    char* out;
    fflush(stdout);
    if (saved < 0 || !freopen(path, "wb", stdout)) { perror(path); exit(2); }
    pad_trace_close(t);
    fflush(stdout);
    if (dup2(saved, fileno(stdout)) < 0) { perror("dup2"); exit(2); }
    close(saved);
    out = read_text(path);
    return out;
}

/* Two recorded frames of a draining title: frame 0's changes are the packet
 * (len 8) and its drain (len 0), frame 1's is the release. Only a change is
 * written, but for a drain loop every poll of the frame is a change, so each
 * group is the frame's full poll sequence. */
static const char k_repeat_trace_text[] =
    HEADER
    "{\"seq\":0,\"port\":0,\"len\":8,\"words\":[0,116,0,64,128,128,128,128],\"flip\":10}\n"
    "{\"seq\":1,\"port\":0,\"len\":0,\"words\":[0,116,0,64,128,128,128,128],\"flip\":10}\n"
    "{\"seq\":2,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":11}\n"
    "{\"end\":true,\"seq\":[3,0,0,0,0,0,0]}\n";

/* The recording pad.replay_repeat is checked against: a packet (ordinal 0), its len 0 drained poll (1), three
 * ordinals with no record of their own (2-4: the held len 0), then one changed len 0 poll (5). The end
 * count is 6. */
static const char k_gap_trace_text[] =
    HEADER
    "{\"seq\":0,\"port\":0,\"len\":8,\"words\":[0,116,0,64,128,128,128,128],\"flip\":10}\n"
    "{\"seq\":1,\"port\":0,\"len\":0,\"words\":[0,116,0,64,128,128,128,128],\"flip\":10}\n"
    "{\"seq\":5,\"port\":0,\"len\":0,\"words\":[0,116,0,0,128,128,128,128],\"flip\":40}\n"
    "{\"end\":true,\"seq\":[6,0,0,0,0,0,0]}\n";

static void case_repeat(void)
{
    const char* path = scratch("repeat.trace");
    const PadTraceState press = mk(8, 0, 0x40, 128), release = mk(8, 0, 0, 128);
    const PadTraceState any = mk(8, 0xFF, 0xFF, 1);
    PadTraceState drain = press;
    PadTrace* t;
    PadTraceStats s;
    PadTraceError e;
    char* out;

    drain.len = 0;   /* the frame's drained poll: the same words, len 0 */
    write_text(path, k_repeat_trace_text);

    /* k = 1: the recorded poll sequence, and the summary string this code printed
     * before the option existed, byte for byte. */
    t = open_trace(NULL, path);
    g_flip = 10;
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &press), "k=1 poll 0: the packet"); }
    g_flip = 11;
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &drain), "k=1 poll 1: the drain"); }
    g_flip = 12;
    { PadTraceState r = poll(t, 0, &any); CHECK(same(&r, &release), "k=1 poll 2: the release"); }
    out = capture_close(t);
    CHECK(!strcmp(out, "[cellPad] pad replay summary: polls=3 exact=3 held=0 recorded_changes=3 "
                       "reached_exactly=3 flip_drift=0:1\n"),
          "k=1 prints the unchanged summary, got: %s", out);
    free(out);
    pad_trace_destroy(t);

    /* k = 2: guest poll p is answered with what k = 1 serves at ordinal floor(p / 2), the unrecorded
     * ordinals (their held state) included. The flip clock is moved arbitrarily: it plays no part. */
    write_text(path, k_gap_trace_text);
    t = open_trace_repeat(path, 2u, &e);
    CHECK(t != NULL, "k=2 opens the v3 trace: %s %s", e.reason, e.detail);
    if (!t) return;
    {
        PadTraceState drain2 = release;   /* the changed poll: other words, len 0 */
        const PadTraceState* want[13];
        drain2.len = 0;
        want[0] = want[1] = &press;                                       /* ordinal 0 */
        want[2] = want[3] = &drain;                                       /* ordinal 1 */
        want[4] = want[5] = want[6] = want[7] = want[8] = want[9] = &drain;   /* ordinals 2-4: held */
        want[10] = want[11] = &drain2;                                    /* ordinal 5 */
        want[12] = &k_neutral_marker;                                     /* ordinal 6: the end count */
        for (int i = 0; i < 13; i++) {
            PadTraceState r;
            g_flip = 100u + (uint32_t)((i * 7) % 5);
            r = poll(t, 0, &any);
            CHECK(same(&r, want[i]), "k=2 poll %d: wrong state (len %d)", i, (int)r.len);
        }
    }
    pad_trace_stats(t, &s);
    CHECK(s.repeat == 2, "the stats name the repeat (%u)", s.repeat);
    CHECK(s.polls == 13 && s.exact == 6 && s.held == 7,
          "6 of 13 polls served an ordinal with a record of its own (%llu polls, %llu exact, %llu held)",
          (unsigned long long)s.polls, (unsigned long long)s.exact, (unsigned long long)s.held);
    CHECK(s.records == 3 && s.records_reached == 3, "each recorded change counts once, at its first serve (%llu/%llu)",
          (unsigned long long)s.records_reached, (unsigned long long)s.records);
    CHECK(s.have_drift, "the drift is read at the first serve of each change");
    CHECK(s.ended, "the trace is reported as ended");
    out = capture_close(t);
    CHECK(strstr(out, "reached_exactly=3 ") != NULL && strstr(out, " repeat=2\n") != NULL,
          "the summary keeps reached_exactly and names the repeat: %s", out);
    CHECK(strstr(out, "frames") == NULL, "no frame counter in the summary: %s", out);
    free(out);
    pad_trace_destroy(t);

    /* Every factor in range follows the same rule: poll p is ordinal p / k. */
    for (uint32_t k = 3; k <= PAD_TRACE_REPEAT_MAX; k++) {
        PadTraceState drain2 = release;
        drain2.len = 0;
        t = open_trace_repeat(path, k, &e);
        if (!t) return;
        for (uint32_t p = 0; p <= 6u * k; p++) {
            const uint32_t o = p / k;
            const PadTraceState* want = o == 0 ? &press : o < 5 ? &drain : o == 5 ? &drain2 : &k_neutral_marker;
            PadTraceState r = poll(t, 0, &any);
            CHECK(same(&r, want), "k=%u poll %u (ordinal %u): wrong state", k, p, o);
        }
        pad_trace_stats(t, &s);
        CHECK(s.records_reached == s.records && s.exact == 3u * k, "k=%u: reached %llu/%llu, exact %llu", k,
              (unsigned long long)s.records_reached, (unsigned long long)s.records, (unsigned long long)s.exact);
        pad_trace_destroy(t);
    }

    /* Past the recording nothing is required to stop or change: a killed trace holds its last
     * record and a finished one serves the neutral pad, for any number of polls. */
    write_text(path, HEADER L_STATE_0);
    t = open_trace_repeat(path, 2u, &e);
    if (!t) return;
    for (int i = 0; i < 200; i++) {
        PadTraceState r = poll(t, 0, &any);
        CHECK(same(&r, &press), "killed trace, poll %d: the held last record", i);
    }
    pad_trace_destroy(t);
    write_text(path, HEADER L_STATE_0 "{\"end\":true,\"seq\":[2,0,0,0,0,0,0]}\n");
    t = open_trace_repeat(path, 2u, &e);
    if (!t) return;
    for (int i = 0; i < 200; i++) {
        PadTraceState r = poll(t, 0, &any);
        CHECK(same(&r, i < 4 ? &press : &k_neutral_marker), "finished trace, poll %d", i);
    }
    pad_trace_destroy(t);

    /* k is bounded: 0 and 9 are refused, named, before any file is read. */
    for (uint32_t bad = 0; bad <= 9u; bad += 9u) {
        PadTrace* r = open_trace_repeat(path, bad, &e);
        CHECK(r == NULL, "repeat %u must be refused", bad);
        CHECK(e.role == PAD_TRACE_ROLE_REPLAY && e.line == 0 && !strcmp(e.reason, "repeat"),
              "repeat %u: wanted replay line 0 repeat, got role %d line %u %s (%s)", bad, (int)e.role, e.line,
              e.reason, e.detail);
    }

    /* No flip-based key exists in versions 1 and 2, so a repeat of either is
     * refused with the version's own reason, and the detail says why. */
    {
        static const char* const versions[2] = {
            "{\"format\":\"ps3recomp-pad-trace\",\"version\":1,\"ports\":7}\n",
            "{\"format\":\"ps3recomp-pad-trace\",\"version\":2,\"clock\":\"guest-flip-requests\",\"ports\":7}\n",
        };
        for (int i = 0; i < 2; i++) {
            const char* vpath = scratch("repeat-v.trace");
            PadTrace* r;
            write_text(vpath, versions[i]);
            r = open_trace_repeat(vpath, 2u, &e);
            CHECK(r == NULL, "version %d with repeat 2 must be refused", i + 1);
            CHECK(e.role == PAD_TRACE_ROLE_REPLAY && e.line == 1 && !strcmp(e.reason, "version"),
                  "version %d: wanted replay line 1 version, got role %d line %u %s (%s)", i + 1, (int)e.role,
                  e.line, e.reason, e.detail);
            CHECK(strstr(e.detail, "pad.replay_repeat 2") != NULL,
                  "version %d's refusal names the repeat: %s", i + 1, e.detail);
        }
    }
}

/* ---- change-only writing ---------------------------------------------------------------------------- */

static void case_changes(void)
{
    const char* path = scratch("changes.trace");
    const PadTraceState a = mk(8, 0, 0, 128), b = mk(8, 0, 0x40, 128);
    PadTraceState drained = a;
    PadTrace* t = open_trace(path, NULL);
    char* text;

    drained.len = 0;   /* the same words, a poll the host had no report for */
    for (g_flip = 0; g_flip < 3; g_flip++) poll(t, 0, &a);   /* one record */
    poll(t, 0, &drained);   /* len changed */
    poll(t, 0, &b);         /* words changed */
    poll(t, 0, &drained);   /* len changed back */
    pad_trace_destroy(t);
    text = read_text(path);
    CHECK(count_sub(text, "\"seq\":") == 5, "three unchanged polls, then len, words, len: four records and the end, got %d",
          count_sub(text, "\"seq\":"));
    CHECK(strstr(text, "\"seq\":0,\"port\":0,\"len\":8,") != NULL, "the first poll of the port is a record");
    CHECK(strstr(text, "\"seq\":3,\"port\":0,\"len\":0,") != NULL, "a change of len alone is a record");
    CHECK(strstr(text, "\"seq\":4,\"port\":0,\"len\":8,") != NULL, "a change of the words is a record");
    free(text);
}

/* ---- recording while replaying ------------------------------------------------------------------------ */

static void case_record_replay(void)
{
    const char* src = scratch("src.trace");
    const char* out = scratch("out.trace");
    const PadTraceState press = mk(8, 0, 0x40, 128), release = mk(8, 0, 0, 128);
    const PadTraceState live = mk(8, 0xFF, 0xFF, 1);   /* what a host pad would say; must not be recorded */
    PadTrace* t;

    write_text(src, HEADER L_STATE_0
               "{\"seq\":2,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":4}\n"
               "{\"end\":true,\"seq\":[4,0,0,0,0,0,0]}\n");
    t = open_trace(out, src);
    poll(t, 0, &live); poll(t, 0, &live); poll(t, 0, &live); poll(t, 0, &live);
    pad_trace_destroy(t);

    t = open_trace(NULL, out);
    { PadTraceState r = poll(t, 0, &live); CHECK(same(&r, &press), "the copy has the replayed press, not the live state"); }
    { PadTraceState r = poll(t, 0, &live); CHECK(same(&r, &press), "held"); }
    { PadTraceState r = poll(t, 0, &live); CHECK(same(&r, &release), "the release"); }
    { PadTraceState r = poll(t, 0, &live); CHECK(same(&r, &release), "held to the end"); }
    pad_trace_destroy(t);
}

/* ---- several guest threads ------------------------------------------------------------------------------ */

static _Atomic uint32_t g_tick;
static uint32_t flip_ticking(void) { return atomic_fetch_add(&g_tick, 1) / 11; }

typedef struct { PadTrace* t; uint32_t port; } Worker;

static void* worker(void* p)
{
    Worker* w = p;
    for (unsigned i = 0; i < 4000; i++) {
        PadTraceState st = mk(8, i % 5 ? 0 : 0x08, 0, 100 + (i % 7));
        uint8_t on[PAD_TRACE_MAX_PORTS] = {1};
        pad_trace_poll(w->t, w->port, &st, NULL);
        if (i % 50 == 0) pad_trace_conn(w->t, on);
    }
    return NULL;
}

/* A file written by threads polling at once must still read back: the ordinals are handed out under the
 * trace's lock, so seq never repeats or goes backwards. */
static void case_threads(void)
{
    const char* path = scratch("threads.trace");
    PadTraceOptions o = { path, NULL, flip_ticking, 1u };
    PadTrace* t = NULL;
    PadTraceError e;
    pthread_t th[4];
    Worker w[4];
    PadTraceStats s;

    memset(&e, 0, sizeof e);
    CHECK(pad_trace_create(&o, &t, &e) == 0, "open: %s", e.detail);
    for (unsigned i = 0; i < 4; i++) {
        w[i].t = t;
        w[i].port = i % 2;
        pthread_create(&th[i], NULL, worker, &w[i]);
    }
    for (unsigned i = 0; i < 4; i++) pthread_join(th[i], NULL);
    pad_trace_destroy(t);

    t = open_trace(NULL, path);
    pad_trace_stats(t, &s);
    CHECK(s.records > 100, "the threads' changes were recorded: %llu records", (unsigned long long)s.records);
    pad_trace_destroy(t);
}

/* ---- malformed files -------------------------------------------------------------------------------------- */

typedef struct {
    const char* name;
    const char* text;
    uint32_t    line;
    const char* reason;
} Bad;

static const Bad k_bad[] = {
    { "empty",           "", 1, "header" },
    { "garbage-header",  "hello\n", 1, "header" },
    { "wrong-format",    "{\"format\":\"other\",\"version\":3,\"clock\":\"guest-polls\",\"ports\":7}\n", 1, "header" },
    { "bad-ports",       "{\"format\":\"ps3recomp-pad-trace\",\"version\":3,\"clock\":\"guest-polls\",\"ports\":8}\n", 1, "header" },
    { "no-clock",        "{\"format\":\"ps3recomp-pad-trace\",\"version\":3,\"ports\":7}\n", 1, "header" },
    { "wrong-clock",     "{\"format\":\"ps3recomp-pad-trace\",\"version\":3,\"clock\":\"guest-flip-requests\",\"ports\":7}\n", 1, "header" },
    { "version-1",       "{\"format\":\"ps3recomp-pad-trace\",\"version\":1,\"ports\":7}\n", 1, "version" },
    { "version-2",       "{\"format\":\"ps3recomp-pad-trace\",\"version\":2,\"clock\":\"guest-flip-requests\",\"ports\":7}\n", 1, "version" },
    { "version-4",       "{\"format\":\"ps3recomp-pad-trace\",\"version\":4,\"clock\":\"guest-polls\",\"ports\":7}\n", 1, "version" },
    { "truncated",       HEADER "{\"seq\":1,\"port\":0,\"len\":8,\"words\":[0", 2, "truncated" },
    { "syntax",          HEADER "{\"seq\":1,\"port\":0,\"len\":8,\"words\":[0,1}\n", 2, "syntax" },
    { "blank-line",      HEADER "\n", 2, "syntax" },
    { "unknown-key",     HEADER "{\"seq\":0,\"poll\":0,\"port\":0,\"len\":8,\"words\":[],\"flip\":1}\n", 2, "unknown-key" },
    { "duplicate-key",   HEADER "{\"seq\":0,\"seq\":1}\n", 2, "duplicate-key" },
    { "value-len",       HEADER "{\"seq\":0,\"port\":0,\"len\":999,\"words\":[],\"flip\":1}\n", 2, "value" },
    { "value-word",      HEADER "{\"seq\":0,\"port\":0,\"len\":8,\"words\":[70000],\"flip\":1}\n", 2, "value" },
    { "conn-length",     HEADER "{\"info\":0,\"conn\":[1,0]}\n", 2, "value" },
    { "record-kind",     HEADER "{\"seq\":0,\"port\":0}\n", 2, "record-kind" },
    { "state-no-seq",    HEADER "{\"port\":0,\"len\":8,\"words\":[],\"flip\":1}\n", 2, "value" },
    { "state-no-flip",   HEADER "{\"seq\":0,\"port\":0,\"len\":8,\"words\":[]}\n", 2, "value" },
    { "conn-no-info",    HEADER "{\"conn\":[1,0,0,0,0,0,0]}\n", 2, "value" },
    { "end-scalar",      HEADER "{\"end\":true,\"seq\":1}\n", 2, "value" },
    { "end-length",      HEADER "{\"end\":true,\"seq\":[1,2]}\n", 2, "value" },
    { "state-seq-array", HEADER "{\"seq\":[0],\"port\":0,\"len\":8,\"words\":[],\"flip\":1}\n", 2, "value" },
    { "unreachable",     HEADER "{\"seq\":2,\"port\":0,\"len\":8,\"words\":[],\"flip\":1}\n"
                                "{\"end\":true,\"seq\":[2,0,0,0,0,0,0]}\n", 2, "value" },
    { "seq-backwards",   HEADER "{\"seq\":5,\"port\":0,\"len\":8,\"words\":[],\"flip\":1}\n"
                                "{\"seq\":5,\"port\":0,\"len\":0,\"words\":[],\"flip\":2}\n", 3, "keys-backwards" },
    { "info-backwards",  HEADER "{\"info\":5,\"conn\":[1,0,0,0,0,0,0]}\n"
                                "{\"info\":5,\"conn\":[0,0,0,0,0,0,0]}\n", 3, "keys-backwards" },
    { "port-range",      "{\"format\":\"ps3recomp-pad-trace\",\"version\":3,\"clock\":\"guest-polls\",\"ports\":2}\n"
                         "{\"seq\":0,\"port\":2,\"len\":8,\"words\":[],\"flip\":1}\n", 2, "port-range" },
    { "after-end",       HEADER "{\"end\":true,\"seq\":[1,0,0,0,0,0,0]}\n{\"info\":1,\"conn\":[1,0,0,0,0,0,0]}\n", 3, "after-end" },
    { "sync-kind",       HEADER "{\"sync\":\"mystery\",\"key\":1,\"port\":0,\"seq\":3}\n", 2, "sync-kind" },
    { "sync-no-key",     HEADER "{\"sync\":\"pipeline\",\"port\":0,\"seq\":3}\n", 2, "record-kind" },
    { "sync-seq-array",  HEADER "{\"sync\":\"pipeline\",\"key\":1,\"port\":0,\"seq\":[3]}\n", 2, "value" },
    { "sync-backwards",  HEADER "{\"sync\":\"pipeline\",\"key\":1,\"port\":0,\"seq\":5}\n"
                                "{\"sync\":\"pipeline\",\"key\":2,\"port\":0,\"seq\":5}\n", 3, "keys-backwards" },
    { "sync-unreachable", HEADER "{\"sync\":\"pipeline\",\"key\":1,\"port\":0,\"seq\":2}\n"
                                 "{\"end\":true,\"seq\":[2,0,0,0,0,0,0]}\n", 2, "value" },
    { "sync-port-range", "{\"format\":\"ps3recomp-pad-trace\",\"version\":3,\"clock\":\"guest-polls\",\"ports\":2}\n"
                         "{\"sync\":\"pipeline\",\"key\":1,\"port\":2,\"seq\":3}\n", 2, "port-range" },
};

static void case_errors(void)
{
    for (size_t i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++) {
        const char* path = scratch("bad.trace");
        PadTraceOptions o = { NULL, path, flip_now, 1u };
        PadTrace* t = NULL;
        PadTraceError e;
        memset(&e, 0, sizeof e);
        write_text(path, k_bad[i].text);
        CHECK(pad_trace_create(&o, &t, &e) != 0 && t == NULL, "%s: the file must be refused", k_bad[i].name);
        CHECK(e.role == PAD_TRACE_ROLE_REPLAY && e.line == k_bad[i].line && !strcmp(e.reason, k_bad[i].reason),
              "%s: wanted replay line %u %s, got role %d line %u %s (%s)", k_bad[i].name, k_bad[i].line, k_bad[i].reason,
              (int)e.role, e.line, e.reason, e.detail);
        if (!strncmp(k_bad[i].name, "version-", 8) && strcmp(k_bad[i].name, "version-4") != 0)
            CHECK(strstr(e.detail, "re-record") != NULL,
                  "the %s refusal says the old file must be re-recorded: %s", k_bad[i].name, e.detail);
    }
    {   /* A NUL byte in a line is not text, and must not be reported as an over-long line. */
        const char* path = scratch("nul.trace");
        static const char pre[] = "{\"seq\":0,\"port\":0,\"l";
        static const char post[] = "en\":8,\"words\":[],\"flip\":1}\n";
        PadTraceOptions o = { NULL, path, flip_now, 1u };
        PadTrace* t = NULL;
        PadTraceError e;
        FILE* f = fopen(path, "wb");
        if (!f) { perror(path); exit(2); }
        fputs(HEADER, f);
        fwrite(pre, 1, sizeof pre - 1, f);
        fputc('\0', f);
        fwrite(post, 1, sizeof post - 1, f);
        if (fclose(f)) { perror(path); exit(2); }
        memset(&e, 0, sizeof e);
        CHECK(pad_trace_create(&o, &t, &e) != 0 && t == NULL, "a NUL byte in a line is refused");
        CHECK(e.role == PAD_TRACE_ROLE_REPLAY && e.line == 2 && !strcmp(e.reason, "syntax"),
              "a NUL byte is a syntax error on its own line, not an over-long line: got line %u %s (%s)",
              e.line, e.reason, e.detail);
    }
}

/* ---- sync records ------------------------------------------------------------------------------------ */

/* The press at recorded 10..13 and the press at 30..33 are what the guest must meet at the right distance from
 * its own events: event 100 happened when the recording had made 20 polls, event 200 at 32. */
static const char k_sync_trace_text[] =
    HEADER
    "{\"seq\":0,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":1}\n"
    "{\"seq\":10,\"port\":0,\"len\":8,\"words\":[0,116,0,64,128,128,128,128],\"flip\":10}\n"
    "{\"seq\":14,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":14}\n"
    "{\"sync\":\"pipeline\",\"key\":100,\"port\":0,\"seq\":20}\n"
    "{\"seq\":30,\"port\":0,\"len\":8,\"words\":[0,116,0,32,128,128,128,128],\"flip\":30}\n"
    "{\"sync\":\"pipeline\",\"key\":200,\"port\":0,\"seq\":32}\n"
    "{\"seq\":34,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":34}\n"
    "{\"end\":true,\"seq\":[60,0,0,0,0,0,0]}\n";

static unsigned btn(const PadTraceState* s) { return s->words[3]; }

/* What the recording handed at recorded ordinal `ord`. */
static unsigned want_btn(uint64_t ord) { return (ord >= 10 && ord <= 13) ? 64u : (ord >= 30 && ord <= 33) ? 32u : 0u; }

static void case_sync(void)
{
    const char* path = scratch("sync.trace");
    const PadTraceState live = mk(8, 0, 0, 128);
    PadTraceState st;
    PadTraceStats ps;
    PadTraceError e;
    PadTrace* t;
    write_text(path, k_sync_trace_text);

    /* A guest that is late: it has not reached event 100 when the recording had. The clock follows the poll
     * ordinals up to the event's ordinal, holds there, and is re-based on the event when the run reports it. */
    t = open_trace(NULL, path);
    CHECK(pad_trace_sync_count(t) == 2, "the trace carries two sync records");
    for (uint64_t p = 0; p < 25; p++) {
        st = poll(t, 0, &live);
        CHECK(btn(&st) == want_btn(p < 20 ? p : 20), "poll %llu before the event: button %u", (unsigned long long)p, btn(&st));
    }
    pad_trace_stats(t, &ps);
    CHECK(ps.sync_waited == 4 && ps.exact == 3 && ps.sync_hits == 0,
          "four polls waited at ordinal 20 and none counted as landing on a record: waited %llu exact %llu hits %llu",
          (unsigned long long)ps.sync_waited, (unsigned long long)ps.exact, (unsigned long long)ps.sync_hits);
    pad_trace_sync_event(t, PS3_REPLAY_SYNC_PIPELINE, 999, 0u);                /* not in the trace */
    pad_trace_sync_event(t, PS3_REPLAY_SYNC_PIPELINE + 1u, 100, 0u);           /* another kind */
    pad_trace_stats(t, &ps);
    CHECK(ps.sync_hits == 0, "an event the trace does not name changes nothing");
    pad_trace_sync_event(t, PS3_REPLAY_SYNC_PIPELINE, 100, 0u);                /* the next poll is ordinal 25 */
    for (uint64_t p = 25; p < 40; p++) {
        const uint64_t ord = p - 5 < 32 ? p - 5 : 32;                          /* shift 5, held at event 200's 32 */
        st = poll(t, 0, &live);
        CHECK(btn(&st) == want_btn(ord), "poll %llu after event 100: recorded %llu, button %u", (unsigned long long)p,
              (unsigned long long)ord, btn(&st));
    }
    pad_trace_sync_event(t, PS3_REPLAY_SYNC_PIPELINE, 200, 0u);                /* the next poll is ordinal 40 */
    for (uint64_t p = 40; p < 68; p++) {
        st = poll(t, 0, &live);
        CHECK(btn(&st) == want_btn(p - 8), "poll %llu after event 200: recorded %llu, button %u", (unsigned long long)p,
              (unsigned long long)(p - 8), btn(&st));
    }
    st = poll(t, 0, &live);
    CHECK(same(&st, &k_neutral_marker), "the re-based clock reaches the end count at poll 68 and then serves the neutral pad");
    pad_trace_stats(t, &ps);
    CHECK(ps.sync_hits == 2 && ps.sync_missed == 0 && ps.sync_waited == 6 && ps.sync_records == 2,
          "both events hit, none missed, six polls waited: hits %llu missed %llu waited %llu", (unsigned long long)ps.sync_hits,
          (unsigned long long)ps.sync_missed, (unsigned long long)ps.sync_waited);
    {
        char* out = capture_close(t);
        CHECK(strstr(out, " sync=2/2 sync_missed=0 sync_waited=6\n") != NULL, "the summary names the sync counts: %s", out);
        free(out);
    }
    pad_trace_destroy(t);

    /* A guest that is early, and events that come out of order: event 200 first re-bases the clock forward and
     * passes event 100 (missed); event 100 reported afterwards has no pending record and is ignored. The jump
     * from recorded 12 to 32 does not skip what lies between: the press at 10..13 keeps its length, the idle
     * stretch from 14 is cut to the next record, and the press at 30..33 is served whole before the clock
     * reaches the re-based ordinal (poll p = recorded p + 20) at poll 20. */
    t = open_trace(NULL, path);
    for (uint64_t p = 0; p < 12; p++) {
        st = poll(t, 0, &live);
        CHECK(btn(&st) == want_btn(p), "early run, poll %llu: button %u", (unsigned long long)p, btn(&st));
    }
    pad_trace_sync_event(t, PS3_REPLAY_SYNC_PIPELINE, 200, 0u);                /* re-based: poll p is recorded p + 20 */
    {
        static const uint64_t served[] = { 12, 13, 14, 30, 31, 32, 33, 34, 40, 41 };   /* polls 12..21 */
        for (uint64_t p = 12; p < 22; p++) {
            st = poll(t, 0, &live);
            CHECK(btn(&st) == want_btn(served[p - 12]), "early run, poll %llu: recorded %llu, button %u",
                  (unsigned long long)p, (unsigned long long)served[p - 12], btn(&st));
        }
    }
    pad_trace_sync_event(t, PS3_REPLAY_SYNC_PIPELINE, 100, 0u);
    pad_trace_stats(t, &ps);
    CHECK(ps.sync_hits == 1 && ps.sync_missed == 1 && ps.sync_waited == 0,
          "one hit, event 100 passed over, no wait: hits %llu missed %llu waited %llu", (unsigned long long)ps.sync_hits,
          (unsigned long long)ps.sync_missed, (unsigned long long)ps.sync_waited);
    pad_trace_destroy(t);

    /* An event reported late for an earlier frame: the guest made its 24th flip request after 24 polls, the draw
     * engine reports at poll 30. The event is where the guest was (24), not where the report arrives (30), so the
     * input after the event is not delayed by the engine's lag: shift 24 - 20 = 4. */
    t = open_trace(NULL, path);
    for (uint64_t p = 0; p < 30; p++) {
        g_flip = (uint32_t)p;                                                  /* flip request f is made after f polls */
        st = poll(t, 0, &live);
    }
    pad_trace_sync_event(t, PS3_REPLAY_SYNC_PIPELINE, 100, 24u);
    for (uint64_t p = 30; p < 38; p++) {
        g_flip = (uint32_t)p;
        st = poll(t, 0, &live);
        CHECK(btn(&st) == want_btn(p - 4 < 32 ? p - 4 : 32), "late report, poll %llu: button %u", (unsigned long long)p, btn(&st));
    }
    pad_trace_destroy(t);
    g_flip = 0;

    /* A late report that moves the clock across a tap: the guest waits at event 7 (recorded 5) from poll 6, made
     * the event's flip request at poll 2 and reports it at poll 10, so the re-based clock wants recorded 13 at
     * poll 10. The tap at recorded 8..9 lies inside that jump; it is served for its two polls, the idle stretch
     * after it absorbs the jump, and the press at 20..23 comes at the re-based distance (poll p = recorded p + 3). */
    {
        const char* tap_path = scratch("sync-tap.trace");
        static const unsigned want[] = { 64, 64, 0, 0, 0, 0, 0, 32, 32, 32, 32, 0 };   /* polls 10..21 */
        write_text(tap_path,
            HEADER
            "{\"seq\":0,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":0}\n"
            "{\"sync\":\"pipeline\",\"key\":7,\"port\":0,\"seq\":5}\n"
            "{\"seq\":8,\"port\":0,\"len\":8,\"words\":[0,116,0,64,128,128,128,128],\"flip\":8}\n"
            "{\"seq\":10,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":10}\n"
            "{\"seq\":20,\"port\":0,\"len\":8,\"words\":[0,116,0,32,128,128,128,128],\"flip\":20}\n"
            "{\"seq\":24,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":24}\n"
            "{\"end\":true,\"seq\":[40,0,0,0,0,0,0]}\n");
        t = open_trace(NULL, tap_path);
        for (uint64_t p = 0; p < 10; p++) {
            g_flip = (uint32_t)p;
            st = poll(t, 0, &live);
        }
        pad_trace_sync_event(t, PS3_REPLAY_SYNC_PIPELINE, 7, 2u);
        for (uint64_t p = 10; p < 22; p++) {
            g_flip = (uint32_t)p;
            st = poll(t, 0, &live);
            CHECK(btn(&st) == want[p - 10], "tap across the jump, poll %llu: button %u, want %u", (unsigned long long)p,
                  btn(&st), want[p - 10]);
        }
        pad_trace_destroy(t);
        g_flip = 0;
    }

    /* An event may pass over at most PAD_TRACE_SYNC_SKIP_MAX pending records: one four records ahead is taken for
     * a key built again, not for the course, and changes nothing; one three ahead re-bases. */
    {
        const char* skip_path = scratch("sync-skip.trace");
        write_text(skip_path,
            HEADER
            "{\"seq\":0,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":0}\n"
            "{\"sync\":\"pipeline\",\"key\":1,\"port\":0,\"seq\":2}\n"
            "{\"sync\":\"pipeline\",\"key\":2,\"port\":0,\"seq\":3}\n"
            "{\"sync\":\"pipeline\",\"key\":3,\"port\":0,\"seq\":4}\n"
            "{\"sync\":\"pipeline\",\"key\":4,\"port\":0,\"seq\":5}\n"
            "{\"sync\":\"pipeline\",\"key\":5,\"port\":0,\"seq\":6}\n"
            "{\"end\":true,\"seq\":[20,0,0,0,0,0,0]}\n");
        t = open_trace(NULL, skip_path);
        pad_trace_sync_event(t, PS3_REPLAY_SYNC_PIPELINE, 5, 0u);
        pad_trace_stats(t, &ps);
        CHECK(ps.sync_hits == 0 && ps.sync_missed == 0, "an event four pending records ahead is ignored: hits %llu missed %llu",
              (unsigned long long)ps.sync_hits, (unsigned long long)ps.sync_missed);
        pad_trace_sync_event(t, PS3_REPLAY_SYNC_PIPELINE, 4, 0u);
        pad_trace_stats(t, &ps);
        CHECK(ps.sync_hits == 1 && ps.sync_missed == 3, "an event three pending records ahead re-bases: hits %llu missed %llu",
              (unsigned long long)ps.sync_hits, (unsigned long long)ps.sync_missed);
        pad_trace_destroy(t);
    }

    /* The re-based ordinal and pad.replay_repeat do not compose: the trace is refused. */
    memset(&e, 0, sizeof e);
    CHECK(open_trace_repeat(path, 2u, &e) == NULL && !strcmp(e.reason, "repeat") && e.line == 5,
          "sync records with pad.replay_repeat 2 are refused on the first record: line %u %s (%s)", e.line, e.reason, e.detail);
}

static void case_record_refusals(void)
{
    const char* path = scratch("exists.trace");
    PadTraceOptions o = { path, NULL, flip_now, 1u };
    PadTrace* t = NULL;
    PadTraceError e;

    write_text(path, "precious\n");
    memset(&e, 0, sizeof e);
    CHECK(pad_trace_create(&o, &t, &e) != 0 && e.role == PAD_TRACE_ROLE_RECORD && !strcmp(e.reason, "exists"),
          "an existing record file is refused (%s)", e.reason);
    { char* text = read_text(path); CHECK(!strcmp(text, "precious\n"), "and left alone"); free(text); }

    o.record_path = "pad_trace_no_such_dir/x.trace";
    memset(&e, 0, sizeof e);
    CHECK(pad_trace_create(&o, &t, &e) != 0 && e.role == PAD_TRACE_ROLE_RECORD && !strcmp(e.reason, "io"),
          "an uncreatable record file is refused (%s)", e.reason);

    o.record_path = ""; o.replay_path = NULL;
    CHECK(pad_trace_create(&o, &t, &e) == 0 && t == NULL, "no path set opens nothing");
}

/* ---- stop cases -------------------------------------------------------------------------------------------- */

static void run_stop(const char* name)
{
    for (size_t i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++) {
        if (strcmp(name, k_bad[i].name)) continue;
        char path[96];
        snprintf(path, sizeof path, "pad_trace_stop_%s.trace", name);
        write_text(path, k_bad[i].text);
        PadTraceOptions o = { NULL, path, flip_now, 1u };
        pad_trace_open(&o);
        printf("[FAIL] %s: the malformed file did not stop the run\n", name);
        exit(1);
    }
    if (!strcmp(name, "record-exists")) {
        write_text("pad_trace_stop_record_exists.trace", "precious\n");
        PadTraceOptions o = { "pad_trace_stop_record_exists.trace", NULL, flip_now, 1u };
        pad_trace_open(&o);
        printf("[FAIL] record-exists: did not stop\n");
        exit(1);
    }
    if (!strcmp(name, "record-io")) {
        PadTraceOptions o = { "pad_trace_no_such_dir/x.trace", NULL, flip_now, 1u };
        pad_trace_open(&o);
        printf("[FAIL] record-io: did not stop\n");
        exit(1);
    }
    printf("[FAIL] unknown stop case %s\n", name);
    exit(2);
}

int main(int argc, char** argv)
{
    if (argc == 3 && !strcmp(argv[1], "stop")) run_stop(argv[2]);
    if (argc != 2) { fprintf(stderr, "usage: test_pad_trace <case>\n"); return 2; }
    atexit(cleanup);
    if      (!strcmp(argv[1], "roundtrip"))       case_roundtrip();
    else if (!strcmp(argv[1], "catchup"))         case_catchup();
    else if (!strcmp(argv[1], "ends"))            case_ends();
    else if (!strcmp(argv[1], "lookup"))          case_lookup();
    else if (!strcmp(argv[1], "repeat"))          case_repeat();
    else if (!strcmp(argv[1], "changes"))         case_changes();
    else if (!strcmp(argv[1], "record-replay"))   case_record_replay();
    else if (!strcmp(argv[1], "threads"))         case_threads();
    else if (!strcmp(argv[1], "errors"))          case_errors();
    else if (!strcmp(argv[1], "record-refusals")) case_record_refusals();
    else if (!strcmp(argv[1], "sync"))            case_sync();
    else { fprintf(stderr, "unknown case %s\n", argv[1]); return 2; }
    if (!g_fail) printf("PASS test_pad_trace %s\n", argv[1]);
    return g_fail ? 1 : 0;
}

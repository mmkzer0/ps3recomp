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

static PadTrace* open_trace(const char* rec, const char* rep)
{
    PadTraceOptions o = { rec, rep, flip_now };
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
    CHECK(strstr(text, "\"end\":true,\"seq\":5}") != NULL, "the end record names port 0's poll count");
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
    CHECK(!s.ended && s.end_seq == 5, "not past the end yet (end seq %llu)", (unsigned long long)s.end_seq);
    g_flip = 10;
    { PadTraceState r = poll(t, 0, &D); CHECK(same(&r, &k_neutral_marker), "poll 5 is the end of the trace: neutral"); }
    pad_trace_stats(t, &s);
    CHECK(s.ended, "the trace is reported as ended");
    pad_trace_destroy(t);
}

/* ---- the ordinal key across a different flip clock --------------------------------------------------- */

/* A recording replayed with one more flip between two of its polls -- a step loop that caught up wall time
 * where the recording did not. Every poll still gets the input the recording's poll got, and the summary
 * reports how far the flip clock moved: the guest-time drift no input key can remove. */
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

    /* The same three polls; the flip clock takes a catch-up tick between the second and the third. */
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
               "{\"end\":true,\"seq\":6}\n");
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
               "{\"end\":true,\"seq\":4}\n");
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
    PadTraceOptions o = { path, NULL, flip_ticking };
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
    { "seq-backwards",   HEADER "{\"seq\":5,\"port\":0,\"len\":8,\"words\":[],\"flip\":1}\n"
                                "{\"seq\":5,\"port\":0,\"len\":0,\"words\":[],\"flip\":2}\n", 3, "keys-backwards" },
    { "info-backwards",  HEADER "{\"info\":5,\"conn\":[1,0,0,0,0,0,0]}\n"
                                "{\"info\":5,\"conn\":[0,0,0,0,0,0,0]}\n", 3, "keys-backwards" },
    { "port-range",      "{\"format\":\"ps3recomp-pad-trace\",\"version\":3,\"clock\":\"guest-polls\",\"ports\":2}\n"
                         "{\"seq\":0,\"port\":2,\"len\":8,\"words\":[],\"flip\":1}\n", 2, "port-range" },
    { "after-end",       HEADER "{\"end\":true,\"seq\":1}\n{\"info\":1,\"conn\":[1,0,0,0,0,0,0]}\n", 3, "after-end" },
};

static void case_errors(void)
{
    for (size_t i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++) {
        const char* path = scratch("bad.trace");
        PadTraceOptions o = { NULL, path, flip_now };
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
}

static void case_record_refusals(void)
{
    const char* path = scratch("exists.trace");
    PadTraceOptions o = { path, NULL, flip_now };
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
        PadTraceOptions o = { NULL, path, flip_now };
        pad_trace_open(&o);
        printf("[FAIL] %s: the malformed file did not stop the run\n", name);
        exit(1);
    }
    if (!strcmp(name, "record-exists")) {
        write_text("pad_trace_stop_record_exists.trace", "precious\n");
        PadTraceOptions o = { "pad_trace_stop_record_exists.trace", NULL, flip_now };
        pad_trace_open(&o);
        printf("[FAIL] record-exists: did not stop\n");
        exit(1);
    }
    if (!strcmp(name, "record-io")) {
        PadTraceOptions o = { "pad_trace_no_such_dir/x.trace", NULL, flip_now };
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
    else if (!strcmp(argv[1], "lookup"))          case_lookup();
    else if (!strcmp(argv[1], "changes"))         case_changes();
    else if (!strcmp(argv[1], "record-replay"))   case_record_replay();
    else if (!strcmp(argv[1], "threads"))         case_threads();
    else if (!strcmp(argv[1], "errors"))          case_errors();
    else if (!strcmp(argv[1], "record-refusals")) case_record_refusals();
    else { fprintf(stderr, "unknown case %s\n", argv[1]); return 2; }
    if (!g_fail) printf("PASS test_pad_trace %s\n", argv[1]);
    return g_fail ? 1 : 0;
}

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

#define HEADER "{\"format\":\"ps3recomp-pad-trace\",\"version\":1,\"ports\":7}\n"
#define STATE_CROSS    "{\"flip\":3,\"poll\":0,\"port\":0,\"len\":8,\"words\":[0,116,0,64,128,128,128,128]}\n"
#define STATE_RELEASED "{\"flip\":3,\"poll\":2,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128]}\n"
#define STATE_START    "{\"flip\":7,\"poll\":0,\"port\":0,\"len\":8,\"words\":[0,116,8,0,128,128,128,128]}\n"

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
static PadTraceState poll(PadTrace* t, uint32_t port, const PadTraceState* live, int* fresh)
{
    PadTraceState st = *live;
    int f;
    pad_trace_poll(t, port, &st, &k_neutral_marker, &f);
    if (fresh) *fresh = f;
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
    int fresh;

    /* Record. */
    t = open_trace(path, NULL);
    g_flip = 5;
    poll(t, 0, &A, NULL); poll(t, 0, &A, NULL); poll(t, 0, &B, NULL);
    memset(on, 0, sizeof on); on[0] = 1;
    pad_trace_conn(t, on); pad_trace_conn(t, on);
    g_flip = 6;
    poll(t, 0, &B, NULL);
    g_flip = 9;
    poll(t, 0, &C, NULL);
    poll(t, 1, &D, NULL); poll(t, 1, &D, NULL);
    on[1] = 1;
    pad_trace_conn(t, on);
    pad_trace_destroy(t);

    text = read_text(path);
    CHECK(strncmp(text, HEADER, strlen(HEADER)) == 0, "the file starts with the version-1 header");
    CHECK(count_sub(text, "\"poll\":") == 4, "one state record per change and per port's first poll, got %d", count_sub(text, "\"poll\":"));
    CHECK(count_sub(text, "\"conn\":") == 2, "a connection record only when the connection changes, got %d", count_sub(text, "\"conn\":"));
    CHECK(strstr(text, "{\"end\":true,\"flip\":9}\n") != NULL, "the end record names the last flip");
    CHECK(strchr(text, '/') == NULL && strstr(text, g_dir) == NULL, "no host path in the file");
    free(text);

    /* Replay answers every poll with what was recorded. */
    t = open_trace(NULL, path);
    g_flip = 5;
    { PadTraceState r = poll(t, 0, &D, &fresh); CHECK(same(&r, &A) && fresh, "flip 5 poll 0: A, fresh"); }
    { PadTraceState r = poll(t, 0, &D, &fresh); CHECK(same(&r, &A) && !fresh, "flip 5 poll 1: A held, not fresh"); }
    { PadTraceState r = poll(t, 0, &D, &fresh); CHECK(same(&r, &B) && fresh, "flip 5 poll 2: B, a change is fresh"); }
    memset(on, 1, sizeof on);
    pad_trace_conn(t, on);
    CHECK(on[0] == 1 && on[1] == 0 && on[6] == 0, "flip 5 connection is port 0 only");
    g_flip = 6;
    { PadTraceState r = poll(t, 0, &D, &fresh); CHECK(same(&r, &B) && fresh, "flip 6 poll 0: B, first poll of a flip is fresh"); }
    g_flip = 9;
    { PadTraceState r = poll(t, 0, &D, &fresh); CHECK(same(&r, &C), "flip 9 port 0: C"); }
    { PadTraceState r = poll(t, 1, &A, &fresh); CHECK(same(&r, &D), "flip 9 port 1: the recorded empty packet"); }
    memset(on, 0, sizeof on);
    pad_trace_conn(t, on);
    CHECK(on[0] == 1 && on[1] == 1 && on[2] == 0, "flip 9 connection adds port 1");
    pad_trace_stats(t, &s);
    CHECK(s.polls == 6 && s.exact == 4 && s.held == 2, "polls %llu exact %llu held %llu", (unsigned long long)s.polls,
          (unsigned long long)s.exact, (unsigned long long)s.held);
    CHECK(s.records == 4 && s.records_reached == 4, "records %llu reached %llu", (unsigned long long)s.records,
          (unsigned long long)s.records_reached);
    CHECK(s.first_div_flip == -1, "the same polls never diverge (flip %lld)", (long long)s.first_div_flip);
    pad_trace_destroy(t);
}

/* ---- lookup semantics ------------------------------------------------------------------------------- */

static void case_lookup(void)
{
    const char* path = scratch("lookup.trace");
    const PadTraceState cross = mk(8, 0, 0x40, 128), released = mk(8, 0, 0, 128), start = mk(8, 0x08, 0, 128);
    const PadTraceState any = mk(8, 0xFF, 0xFF, 1);
    PadTrace* t;
    PadTraceStats s;
    int fresh;

    write_text(path, HEADER STATE_CROSS STATE_RELEASED STATE_START "{\"end\":true,\"flip\":9}\n");
    t = open_trace(NULL, path);

    for (g_flip = 0; g_flip < 3; g_flip++) {
        PadTraceState r = poll(t, 0, &any, &fresh);
        CHECK(same(&r, &k_neutral_marker), "flip %u is before port 0's first record: neutral", g_flip);
    }
    { PadTraceState r = poll(t, 1, &any, NULL); CHECK(same(&r, &k_neutral_marker), "a port with no record is neutral"); }

    g_flip = 3;
    { PadTraceState r = poll(t, 0, &any, &fresh); CHECK(same(&r, &cross), "(3,0) is the record's own key"); }
    { PadTraceState r = poll(t, 0, &any, &fresh); CHECK(same(&r, &cross) && !fresh, "(3,1) is held at the state at or before it"); }
    { PadTraceState r = poll(t, 0, &any, &fresh); CHECK(same(&r, &released) && fresh, "(3,2) takes the release"); }
    { PadTraceState r = poll(t, 0, &any, &fresh); CHECK(same(&r, &released) && !fresh, "(3,3) holds it"); }
    g_flip = 4;
    { PadTraceState r = poll(t, 0, &any, &fresh); CHECK(same(&r, &released) && fresh, "(4,0) holds it; the first poll of a flip is fresh"); }
    g_flip = 7;
    { PadTraceState r = poll(t, 0, &any, &fresh); CHECK(same(&r, &start), "(7,0) is START"); }
    g_flip = 9;
    { PadTraceState r = poll(t, 0, &any, &fresh); CHECK(same(&r, &start), "the end flip itself still holds the last state"); }
    pad_trace_stats(t, &s);
    CHECK(!s.ended && s.end_flip == 9, "not past the end yet (end_flip %u)", s.end_flip);
    g_flip = 10;
    { PadTraceState r = poll(t, 0, &any, &fresh); CHECK(same(&r, &k_neutral_marker), "after the end of the trace: neutral"); }
    g_flip = 500;
    { PadTraceState r = poll(t, 0, &any, &fresh); CHECK(same(&r, &k_neutral_marker), "and it stays neutral"); }
    pad_trace_stats(t, &s);
    CHECK(s.ended, "the trace is reported as ended");
    pad_trace_destroy(t);
}

/* ---- change-only writing ---------------------------------------------------------------------------- */

static void case_changes(void)
{
    const char* path = scratch("changes.trace");
    const PadTraceState a = mk(8, 0, 0, 128), b = mk(8, 0, 0x40, 128);
    PadTrace* t = open_trace(path, NULL);
    char* text;

    for (g_flip = 0; g_flip < 5; g_flip++)
        for (int k = 0; k < 3; k++) poll(t, 0, &a, NULL);
    CHECK(count_sub(text = read_text(path), "\"poll\":") <= 1, "identical consecutive states write one record");
    free(text);
    poll(t, 0, &b, NULL);
    poll(t, 0, &b, NULL);
    poll(t, 0, &a, NULL);
    pad_trace_destroy(t);
    text = read_text(path);
    CHECK(count_sub(text, "\"poll\":") == 3, "a, b, a is three records, got %d", count_sub(text, "\"poll\":"));
    CHECK(strstr(text, "\"flip\":0,\"poll\":0,\"port\":0") != NULL, "the first poll of the port is a record");
    CHECK(strstr(text, "\"flip\":5,\"poll\":0,\"port\":0") != NULL, "the change is keyed by the poll that saw it");
    CHECK(strstr(text, "\"flip\":5,\"poll\":2,\"port\":0") != NULL, "and so is the change back");
    free(text);
}

/* ---- poll counts: what the writer says and what the replayer compares it with ------------------------- */

static void polls_at(PadTrace* t, uint32_t flip, int n)
{
    const PadTraceState a = mk(8, 0, 0, 128);
    g_flip = flip;
    for (int i = 0; i < n; i++) poll(t, 0, &a, NULL);
}

static void case_poll_counts(void)
{
    const char* path = scratch("counts.trace");
    PadTrace* t = open_trace(path, NULL);
    PadTraceStats s;

    /* 3 per flip, then 2, a gap of two flips with no polls, then 5. */
    polls_at(t, 1, 3); polls_at(t, 2, 3); polls_at(t, 3, 2); polls_at(t, 4, 2); polls_at(t, 7, 5); polls_at(t, 8, 5);
    pad_trace_destroy(t);

    t = open_trace(NULL, path);   /* the same polling replays without divergence */
    polls_at(t, 1, 3); polls_at(t, 2, 3); polls_at(t, 3, 2); polls_at(t, 4, 2); polls_at(t, 7, 5); polls_at(t, 8, 5);
    pad_trace_stats(t, &s);
    CHECK(s.first_div_flip == -1, "same polling: no divergence, got flip %lld", (long long)s.first_div_flip);
    pad_trace_destroy(t);

    t = open_trace(NULL, path);   /* one poll too few in flip 3 */
    polls_at(t, 1, 3); polls_at(t, 2, 3); polls_at(t, 3, 1); polls_at(t, 4, 2);
    pad_trace_stats(t, &s);
    CHECK(s.first_div_flip == 3, "a short flip is reported at that flip, got %lld", (long long)s.first_div_flip);
    pad_trace_destroy(t);

    t = open_trace(NULL, path);   /* polling in a flip the recording skipped */
    polls_at(t, 1, 3); polls_at(t, 2, 3); polls_at(t, 3, 2); polls_at(t, 4, 2); polls_at(t, 5, 1); polls_at(t, 7, 5);
    pad_trace_stats(t, &s);
    CHECK(s.first_div_flip == 5, "a poll in an unpolled flip is reported, got %lld", (long long)s.first_div_flip);
    pad_trace_destroy(t);

    t = open_trace(NULL, path);   /* starting later than the recording */
    polls_at(t, 2, 3); polls_at(t, 3, 2); polls_at(t, 4, 2);
    pad_trace_stats(t, &s);
    CHECK(s.first_div_flip == 1, "a flip the recording polled and this run did not, got %lld", (long long)s.first_div_flip);
    pad_trace_destroy(t);
}

/* ---- recording while replaying ------------------------------------------------------------------------ */

static void case_record_replay(void)
{
    const char* src = scratch("src.trace");
    const char* out = scratch("out.trace");
    const PadTraceState cross = mk(8, 0, 0x40, 128), released = mk(8, 0, 0, 128), start = mk(8, 0x08, 0, 128);
    const PadTraceState live = mk(8, 0xFF, 0xFF, 1);   /* what a host pad would say; must not be recorded */
    PadTrace* t;

    write_text(src, HEADER STATE_CROSS STATE_RELEASED STATE_START "{\"end\":true,\"flip\":9}\n");
    t = open_trace(out, src);
    g_flip = 3;
    poll(t, 0, &live, NULL); poll(t, 0, &live, NULL); poll(t, 0, &live, NULL);
    g_flip = 7;
    poll(t, 0, &live, NULL);
    pad_trace_destroy(t);

    t = open_trace(NULL, out);
    g_flip = 3;
    { PadTraceState r = poll(t, 0, &live, NULL); CHECK(same(&r, &cross), "the copy has the replayed press, not the live state"); }
    { PadTraceState r = poll(t, 0, &live, NULL); CHECK(same(&r, &cross), "held"); }
    { PadTraceState r = poll(t, 0, &live, NULL); CHECK(same(&r, &released), "the release"); }
    g_flip = 7;
    { PadTraceState r = poll(t, 0, &live, NULL); CHECK(same(&r, &start), "START"); }
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
        int fresh;
        pad_trace_poll(w->t, w->port, &st, NULL, &fresh);
        if (i % 50 == 0) pad_trace_conn(w->t, on);
    }
    return NULL;
}

/* A file written by threads polling at once must still read back: keys never go backwards because the
 * flip count is read inside the trace's lock. */
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
    { "wrong-format",    "{\"format\":\"other\",\"version\":1,\"ports\":7}\n", 1, "header" },
    { "bad-ports",       "{\"format\":\"ps3recomp-pad-trace\",\"version\":1,\"ports\":8}\n", 1, "header" },
    { "version-2",       "{\"format\":\"ps3recomp-pad-trace\",\"version\":2,\"ports\":7}\n", 1, "version" },
    { "truncated",       HEADER "{\"flip\":1,\"poll\":0,\"port\":0,\"len\":8,\"words\":[0", 2, "truncated" },
    { "syntax",          HEADER "{\"flip\":1,\"poll\":0,\"port\":0,\"len\":8,\"words\":[0,1}\n", 2, "syntax" },
    { "blank-line",      HEADER "\n", 2, "syntax" },
    { "unknown-key",     HEADER "{\"flip\":1,\"colour\":3}\n", 2, "unknown-key" },
    { "duplicate-key",   HEADER "{\"flip\":1,\"flip\":2}\n", 2, "duplicate-key" },
    { "value-len",       HEADER "{\"flip\":1,\"poll\":0,\"port\":0,\"len\":999,\"words\":[]}\n", 2, "value" },
    { "value-word",      HEADER "{\"flip\":1,\"poll\":0,\"port\":0,\"len\":8,\"words\":[70000]}\n", 2, "value" },
    { "conn-length",     HEADER "{\"flip\":1,\"conn\":[1,0]}\n", 2, "value" },
    { "record-kind",     HEADER "{\"flip\":1,\"poll\":0}\n", 2, "record-kind" },
    { "poll-backwards",  HEADER "{\"flip\":3,\"poll\":2,\"port\":0,\"len\":8,\"words\":[]}\n"
                                "{\"flip\":3,\"poll\":1,\"port\":0,\"len\":8,\"words\":[]}\n", 3, "keys-backwards" },
    { "same-key",        HEADER "{\"flip\":3,\"poll\":0,\"port\":0,\"len\":8,\"words\":[]}\n"
                                "{\"flip\":3,\"poll\":0,\"port\":0,\"len\":8,\"words\":[]}\n", 3, "keys-backwards" },
    { "flip-backwards",  HEADER "{\"flip\":5,\"poll\":0,\"port\":0,\"len\":8,\"words\":[]}\n"
                                "{\"flip\":4,\"poll\":0,\"port\":1,\"len\":8,\"words\":[]}\n", 3, "keys-backwards" },
    { "step-backwards",  HEADER "{\"flip\":5,\"port\":0,\"polls\":2}\n{\"flip\":3,\"port\":0,\"polls\":0}\n", 3, "keys-backwards" },
    { "end-backwards",   HEADER "{\"flip\":5,\"poll\":0,\"port\":0,\"len\":8,\"words\":[]}\n{\"end\":true,\"flip\":4}\n", 3, "keys-backwards" },
    { "port-range",      "{\"format\":\"ps3recomp-pad-trace\",\"version\":1,\"ports\":2}\n"
                         "{\"flip\":1,\"poll\":0,\"port\":2,\"len\":8,\"words\":[]}\n", 2, "port-range" },
    { "after-end",       HEADER "{\"end\":true,\"flip\":1}\n{\"flip\":2,\"conn\":[1,0,0,0,0,0,0]}\n", 3, "after-end" },
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
    else if (!strcmp(argv[1], "lookup"))          case_lookup();
    else if (!strcmp(argv[1], "changes"))         case_changes();
    else if (!strcmp(argv[1], "poll-counts"))     case_poll_counts();
    else if (!strcmp(argv[1], "record-replay"))   case_record_replay();
    else if (!strcmp(argv[1], "threads"))         case_threads();
    else if (!strcmp(argv[1], "errors"))          case_errors();
    else if (!strcmp(argv[1], "record-refusals")) case_record_refusals();
    else { fprintf(stderr, "unknown case %s\n", argv[1]); return 2; }
    if (!g_fail) printf("PASS test_pad_trace %s\n", argv[1]);
    return g_fail ? 1 : 0;
}

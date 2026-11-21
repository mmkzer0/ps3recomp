/*
 * ps3recomp - pad trace writer, reader and lookup (see pad_trace.h)
 *
 * File format, version 3: UTF-8 text, one JSON object per line, no host paths
 * and no timestamps.
 *
 *   {"format":"ps3recomp-pad-trace","version":3,"clock":"guest-polls","ports":7}   header, line 1
 *   {"seq":S,"port":N,"len":L,"words":[...],"flip":F}      pad state
 *   {"info":I,"conn":[c0,...,c6]}                            connection
 *   {"end":true,"seq":S}                                     last line
 *   {"sync":"pipeline","key":K,"port":N,"seq":S}             optional, replay only: a guest event and the poll
 *                                                            count the recording had made when it happened
 *
 * A state record gives what poll S of port N was handed: `len` and `words`
 * exactly as the guest read them, `len` = 0 for a poll the host had no new
 * report for. It holds until the next record of that port, so a replay poll n
 * is served the latest record of its port with seq <= n. `flip` is the guest
 * flip count at that poll, for the summary's drift only: it is not a key.
 * A connection record says which ports the info call with ordinal I reported
 * connected. The end record carries port 0's poll count: a replay poll whose
 * ordinal reaches it is past the recording and gets a neutral pad. A killed run
 * has no end record and keeps serving the last record.
 *
 * `seq` is the port's 0-based cellPadGetData ordinal, and the only key. The
 * flip count cannot be one: a title's step loop runs one to three steps per
 * tick to catch up wall time, so how many polls happen inside one flip depends
 * on the host. Version 2 keyed on (flip, poll) and turned a one-step press
 * recorded at (F,0)->(F+1,0) into a two-step press when a replay took a
 * catch-up tick where the recording did not. Version 1 keyed on the walker's
 * flip count, one step further from the guest. Both are refused: neither key
 * can be re-derived from the other.
 *
 * Ordering: `seq` strictly increases per port and `info` strictly increases.
 * `flip` is annotation, so the reader does not order on it.
 */

/* Strict -std=c17 hides clock_gettime and pthread on glibc. Apple's headers go the other
 * way: defining a POSIX level there hides at_quick_exit. */
#if !defined(_POSIX_C_SOURCE) && !defined(__APPLE__)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "pad_trace.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* One lock per trace and one around the process-wide trace. Neither is ever taken recursively, so an SRW
 * lock serves on Windows, which has no pthread.h. */
#ifdef _WIN32
#  include <windows.h>
typedef SRWLOCK trace_lock;
#  define TRACE_LOCK_INIT          SRWLOCK_INIT
#  define trace_lock_init(l)       InitializeSRWLock(l)
#  define trace_lock_destroy(l)    ((void)(l))
#  define trace_lock_acquire(l)    AcquireSRWLockExclusive(l)
#  define trace_lock_release(l)    ReleaseSRWLockExclusive(l)
#else
#  include <pthread.h>
typedef pthread_mutex_t trace_lock;
#  define TRACE_LOCK_INIT          PTHREAD_MUTEX_INITIALIZER
#  define trace_lock_init(l)       pthread_mutex_init((l), NULL)
#  define trace_lock_destroy(l)    pthread_mutex_destroy(l)
#  define trace_lock_acquire(l)    pthread_mutex_lock(l)
#  define trace_lock_release(l)    pthread_mutex_unlock(l)
#endif

#include "ps3emu/host_exit.h"
#include "ps3emu/stop.h"
#include "ps3emu/pc_counters.h"
#include "ps3emu/replay_sync.h"

/* ---------------------------------------------------------------------------
 * Types
 * -------------------------------------------------------------------------*/

#define LINE_MAX_BYTES   4096u
#define WBUF_BYTES       16384u
/* A record can sit in the buffer this long before it reaches the file; a killed
 * run loses at most this much. Checked at every poll, not only on a write. */
#define FLUSH_INTERVAL_MS 100u
#define LEN_MAX          256u

typedef struct {
    uint64_t      seq;            /* the port's GetData ordinal: the record's key */
    uint32_t      flip;           /* the flip clock at that poll, annotation only */
    uint32_t      line;           /* the replay file's line, for a load error */
    uint8_t       hit;            /* a replayed poll landed on this record's own seq */
    PadTraceState st;             /* len and words exactly as the guest read them */
} Rec;

typedef struct { uint64_t info; uint8_t on[PAD_TRACE_MAX_PORTS]; } ConnRec;

/* A sync record: the guest event (kind, key) happened when the port had made `seq` polls in the recording. */
typedef struct { uint64_t seq; uint64_t key; uint32_t kind; uint32_t line; } SyncRec;

struct PadTrace {
    trace_lock      lock;
    uint32_t      (*flip_fn)(void);
    /* Each port's GetData ordinal since its first poll, and the info-call
     * ordinal: the key the next record is written with, and the ordinal a
     * replay serves from. Both roles count every call, so equal call sequences
     * give equal ordinals. */
    uint64_t        seq[PAD_TRACE_MAX_PORTS];
    uint64_t        info_seq;

    /* writer */
    FILE*           wf;
    char*           record_path;
    char*           wbuf;
    size_t          wlen;
    int             wdirty;
    uint64_t        w_flush_ms;
    int             wclosed;
    PadTraceState   w_last[PAD_TRACE_MAX_PORTS];
    uint8_t         w_have[PAD_TRACE_MAX_PORTS];
    uint8_t         w_conn[PAD_TRACE_MAX_PORTS];
    int             w_have_conn;

    /* reader */
    int             replaying;
    char*           replay_path;
    uint32_t        ports;        /* the header's */
    /* pad.replay_repeat: guest poll p of a port is answered with the k = 1
     * answer at recorded ordinal p / repeat; 1 replays by poll ordinal. */
    uint32_t        repeat;
    Rec*            recs[PAD_TRACE_MAX_PORTS];
    size_t          nrecs[PAD_TRACE_MAX_PORTS];
    size_t          caprecs[PAD_TRACE_MAX_PORTS];
    ConnRec*        conns;
    size_t          nconns, capconns;
    uint64_t        end_seq[PAD_TRACE_MAX_PORTS];  /* the end record's counts */
    int             have_end;
    int             end_logged;
    int             summary_logged;
    /* sync records (pad_trace.h): per port the pending-event list in seq order, the index of the first record
     * the run has not reported yet, and the shift that maps the port's poll ordinal to the recorded ordinal
     * (recorded = ordinal - shift) */
    SyncRec*        syncs[PAD_TRACE_MAX_PORTS];
    size_t          nsyncs[PAD_TRACE_MAX_PORTS];
    size_t          capsyncs[PAD_TRACE_MAX_PORTS];
    size_t          sync_next[PAD_TRACE_MAX_PORTS];
    int64_t         sync_shift[PAD_TRACE_MAX_PORTS];
    /* the lowest recorded ordinal no poll of the port has been served yet: a re-base never jumps over a record
     * (sync_ordinal) */
    uint64_t        sync_from[PAD_TRACE_MAX_PORTS];
    /* flip_poll[port][f - 1]: the polls the port had made when the guest made its f-th flip request. Kept only for
     * a trace with sync records: it turns "the frame the draw engine was working on" into a guest poll count. */
    uint64_t*       flip_poll[PAD_TRACE_MAX_PORTS];
    size_t          nflip[PAD_TRACE_MAX_PORTS];
    size_t          capflip[PAD_TRACE_MAX_PORTS];
    PadTraceStats   stats;
};

/* ---------------------------------------------------------------------------
 * Small helpers
 * -------------------------------------------------------------------------*/

static void set_err(PadTraceError* e, PadTraceRole role, uint32_t line, const char* reason,
                    const char* fmt, ...)
{
    va_list ap;
    if (!e) return;
    e->role = role;
    e->line = line;
    snprintf(e->reason, sizeof e->reason, "%s", reason);
    va_start(ap, fmt);
    vsnprintf(e->detail, sizeof e->detail, fmt, ap);
    va_end(ap);
}

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static int state_equal(const PadTraceState* a, const PadTraceState* b)
{
    return a->len == b->len && memcmp(a->words, b->words, sizeof a->words) == 0;
}

/* Pad data words, as cellPad lays them out (libs/input/cellPad.h CELL_PAD_BTN_OFFSET_*): 2 and 3 the digital
 * buttons, 4 to 7 the two sticks, 0x80 at centre. */
enum { PAD_WORD_DIGITAL1 = 2, PAD_WORD_DIGITAL2 = 3, PAD_WORD_STICK_FIRST = 4, PAD_WORD_STICK_LAST = 7 };
#define PAD_STICK_CENTRE 0x80
/* A stick this close to centre is at rest: a hand-held stick sits a few counts off it. */
#define PAD_STICK_REST 8

/* No button down and both sticks at rest: an interval the replay may shorten without changing any input the
 * guest acts on. */
static int state_idle(const PadTraceState* s)
{
    if (s->len <= PAD_WORD_DIGITAL2) return 1;
    if (s->words[PAD_WORD_DIGITAL1] || s->words[PAD_WORD_DIGITAL2]) return 0;
    for (int i = PAD_WORD_STICK_FIRST; i <= PAD_WORD_STICK_LAST && i < s->len; i++) {
        const int d = (int)s->words[i] - PAD_STICK_CENTRE;
        if (d > PAD_STICK_REST || d < -PAD_STICK_REST) return 0;
    }
    return 1;
}

static char* dup_str(const char* s)
{
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void* grow(void* p, size_t* cap, size_t need, size_t elem)
{
    size_t nc = *cap ? *cap : 16;
    while (nc < need) nc *= 2;
    if (nc == *cap) return p;
    void* q = realloc(p, nc * elem);
    if (!q) return NULL;
    *cap = nc;
    return q;
}

static const char* role_name(PadTraceRole r) { return r == PAD_TRACE_ROLE_RECORD ? "record" : "replay"; }

/* ---------------------------------------------------------------------------
 * Line parser: the subset of JSON the writer emits. Objects of non-negative
 * integers, one string, `true`, and arrays of integers; no escapes, no nesting.
 * -------------------------------------------------------------------------*/

enum {
    K_FORMAT = 1u << 0, K_VERSION = 1u << 1, K_CLOCK = 1u << 2, K_PORTS = 1u << 3,
    K_SEQ    = 1u << 4, K_PORT    = 1u << 5, K_LEN   = 1u << 6, K_WORDS = 1u << 7,
    K_FLIP   = 1u << 8, K_INFO    = 1u << 9, K_CONN  = 1u << 10, K_END  = 1u << 11,
    K_SYNC   = 1u << 12, K_KEY    = 1u << 13,
};

#define MASK_HEADER (K_FORMAT | K_VERSION | K_CLOCK | K_PORTS)
#define MASK_STATE  (K_SEQ | K_PORT | K_LEN | K_WORDS | K_FLIP)
#define MASK_CONN   (K_INFO | K_CONN)
#define MASK_END    (K_END | K_SEQ)
#define MASK_SYNC   (K_SYNC | K_KEY | K_PORT | K_SEQ)

typedef struct {
    unsigned seen;
    char     format[32];
    char     clock[32];
    char     sync[24];
    uint32_t version, ports, port, len, flip;
    uint64_t seq, info, key;
    /* `seq` is a scalar in a state record and the per-port poll-count array in
     * the end record; the two are told apart by the shape the file uses. */
    uint64_t seqv[PAD_TRACE_MAX_PORTS];
    uint32_t nseq;
    int      seq_array;
    uint32_t nwords;
    uint16_t words[PAD_TRACE_MAX_WORDS];
    uint32_t nconn;
    uint8_t  conn[PAD_TRACE_MAX_PORTS];
} Line;

typedef struct { const char* s; const char* p; PadTraceError* e; } Cur;

#define PFAIL(c, reason, ...) \
    do { set_err((c)->e, PAD_TRACE_ROLE_REPLAY, 0, (reason), __VA_ARGS__); return -1; } while (0)

static void skip_ws(Cur* c) { while (*c->p == ' ' || *c->p == '\t') c->p++; }

static int expect_ch(Cur* c, char ch)
{
    skip_ws(c);
    if (*c->p != ch) PFAIL(c, "syntax", "expected '%c' at column %d", ch, (int)(c->p - c->s) + 1);
    c->p++;
    return 0;
}

static int parse_string(Cur* c, char* out, size_t cap)
{
    size_t n = 0;
    if (expect_ch(c, '"')) return -1;
    while (*c->p != '"') {
        unsigned char ch = (unsigned char)*c->p;
        if (ch == 0 || ch < 0x20 || ch == '\\')
            PFAIL(c, "syntax", "unterminated string or escape at column %d", (int)(c->p - c->s) + 1);
        if (n + 1 >= cap) PFAIL(c, "value", "string longer than %zu characters", cap - 1);
        out[n++] = (char)ch;
        c->p++;
    }
    c->p++;
    out[n] = 0;
    return 0;
}

/* Base 10, no sign, no leading zero, at most `max`. */
static int parse_uint(Cur* c, uint64_t* v, uint64_t max)
{
    uint64_t acc = 0;
    skip_ws(c);
    if (*c->p < '0' || *c->p > '9') PFAIL(c, "syntax", "expected a number at column %d", (int)(c->p - c->s) + 1);
    if (c->p[0] == '0' && c->p[1] >= '0' && c->p[1] <= '9')
        PFAIL(c, "syntax", "leading zero at column %d", (int)(c->p - c->s) + 1);
    while (*c->p >= '0' && *c->p <= '9') {
        const uint64_t d = (uint64_t)(*c->p - '0');
        if (acc > (max - d) / 10u)
            PFAIL(c, "value", "number above %llu at column %d", (unsigned long long)max, (int)(c->p - c->s) + 1);
        acc = acc * 10u + d;
        c->p++;
    }
    *v = acc;
    return 0;
}

static int parse_u32(Cur* c, uint32_t* v)
{
    uint64_t wide;
    if (parse_uint(c, &wide, 0xFFFFFFFFu)) return -1;
    *v = (uint32_t)wide;
    return 0;
}

/* `max_val` bounds each element, `cap` the element count. */
static int parse_array(Cur* c, uint32_t* out, size_t cap, uint32_t max_val, uint32_t* n)
{
    uint32_t count = 0;
    if (expect_ch(c, '[')) return -1;
    skip_ws(c);
    if (*c->p == ']') { c->p++; *n = 0; return 0; }
    for (;;) {
        uint32_t v;
        if (parse_u32(c, &v)) return -1;
        if (v > max_val) PFAIL(c, "value", "array element %u above %u", v, max_val);
        if (count >= cap) PFAIL(c, "value", "array longer than %zu elements", cap);
        out[count++] = v;
        skip_ws(c);
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == ']') { c->p++; break; }
        PFAIL(c, "syntax", "expected ',' or ']' at column %d", (int)(c->p - c->s) + 1);
    }
    *n = count;
    return 0;
}

/* `cap` bounds the element count; the elements are poll counts, so u64. */
static int parse_array_u64(Cur* c, uint64_t* out, size_t cap, uint32_t* n)
{
    uint32_t count = 0;
    if (expect_ch(c, '[')) return -1;
    skip_ws(c);
    if (*c->p == ']') { c->p++; *n = 0; return 0; }
    for (;;) {
        uint64_t v;
        if (parse_uint(c, &v, UINT64_MAX)) return -1;
        if (count >= cap) PFAIL(c, "value", "array longer than %zu elements", cap);
        out[count++] = v;
        skip_ws(c);
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == ']') { c->p++; break; }
        PFAIL(c, "syntax", "expected ',' or ']' at column %d", (int)(c->p - c->s) + 1);
    }
    *n = count;
    return 0;
}

static int parse_line(const char* s, Line* L, PadTraceError* e)
{
    Cur cur = { s, s, e };
    Cur* c = &cur;
    memset(L, 0, sizeof *L);
    if (expect_ch(c, '{')) return -1;
    skip_ws(c);
    if (*c->p == '}') { c->p++; goto tail; }
    for (;;) {
        char key[16];
        unsigned bit = 0;
        enum { T_STR, T_U32, T_U64, T_SEQ, T_WORDS, T_CONN, T_TRUE } type = T_U32;
        uint32_t* dst = NULL;
        uint64_t* dst64 = NULL;
        if (parse_string(c, key, sizeof key)) return -1;
        if (expect_ch(c, ':')) return -1;
        if      (!strcmp(key, "format"))  { bit = K_FORMAT;  type = T_STR; }
        else if (!strcmp(key, "version")) { bit = K_VERSION; dst = &L->version; }
        else if (!strcmp(key, "clock"))   { bit = K_CLOCK;   type = T_STR; }
        else if (!strcmp(key, "ports"))   { bit = K_PORTS;   dst = &L->ports; }
        else if (!strcmp(key, "seq"))     { bit = K_SEQ;     type = T_SEQ; }
        else if (!strcmp(key, "port"))    { bit = K_PORT;    dst = &L->port; }
        else if (!strcmp(key, "len"))     { bit = K_LEN;     dst = &L->len; }
        else if (!strcmp(key, "flip"))    { bit = K_FLIP;    dst = &L->flip; }
        else if (!strcmp(key, "info"))    { bit = K_INFO;    type = T_U64; dst64 = &L->info; }
        else if (!strcmp(key, "words"))   { bit = K_WORDS;   type = T_WORDS; }
        else if (!strcmp(key, "conn"))    { bit = K_CONN;    type = T_CONN; }
        else if (!strcmp(key, "end"))     { bit = K_END;     type = T_TRUE; }
        else if (!strcmp(key, "sync"))    { bit = K_SYNC;    type = T_STR; }
        else if (!strcmp(key, "key"))     { bit = K_KEY;     type = T_U64; dst64 = &L->key; }
        else PFAIL(c, "unknown-key", "key \"%s\" is not part of the format", key);
        if (L->seen & bit) PFAIL(c, "duplicate-key", "key \"%s\" appears twice", key);
        skip_ws(c);
        switch (type) {
        case T_STR:
            if (bit == K_CLOCK ? parse_string(c, L->clock, sizeof L->clock)
                : bit == K_SYNC ? parse_string(c, L->sync, sizeof L->sync)
                                : parse_string(c, L->format, sizeof L->format))
                return -1;
            break;
        case T_U32:
            if (parse_u32(c, dst)) return -1;
            break;
        case T_U64:
            if (parse_uint(c, dst64, UINT64_MAX)) return -1;
            break;
        case T_SEQ:
            /* A state record keys one poll ordinal; the end record lists each port's poll count. */
            skip_ws(c);
            if (*c->p == '[') {
                L->seq_array = 1;
                if (parse_array_u64(c, L->seqv, PAD_TRACE_MAX_PORTS, &L->nseq)) return -1;
            } else if (parse_uint(c, &L->seq, UINT64_MAX)) {
                return -1;
            }
            break;
        case T_WORDS: {
            uint32_t tmp[PAD_TRACE_MAX_WORDS];
            if (parse_array(c, tmp, PAD_TRACE_MAX_WORDS, 0xFFFFu, &L->nwords)) return -1;
            for (uint32_t i = 0; i < L->nwords; i++) L->words[i] = (uint16_t)tmp[i];
            break;
        }
        case T_CONN: {
            uint32_t tmp[PAD_TRACE_MAX_PORTS];
            if (parse_array(c, tmp, PAD_TRACE_MAX_PORTS, 1u, &L->nconn)) return -1;
            for (uint32_t i = 0; i < L->nconn; i++) L->conn[i] = (uint8_t)tmp[i];
            break;
        }
        case T_TRUE:
            if (strncmp(c->p, "true", 4) != 0) PFAIL(c, "syntax", "expected true at column %d", (int)(c->p - c->s) + 1);
            c->p += 4;
            break;
        }
        L->seen |= bit;
        skip_ws(c);
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == '}') { c->p++; break; }
        PFAIL(c, "syntax", "expected ',' or '}' at column %d", (int)(c->p - c->s) + 1);
    }
tail:
    skip_ws(c);
    if (*c->p) PFAIL(c, "syntax", "text after the closing brace at column %d", (int)(c->p - c->s) + 1);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Reader
 * -------------------------------------------------------------------------*/

#define RFAIL(line, reason, ...) \
    do { set_err(e, PAD_TRACE_ROLE_REPLAY, (line), (reason), __VA_ARGS__); goto fail; } while (0)

static int replay_load(PadTrace* t, const char* path, PadTraceError* e)
{
    FILE* f = fopen(path, "rb");
    char line[LINE_MAX_BYTES];
    uint32_t n = 0;
    uint64_t last_seq[PAD_TRACE_MAX_PORTS] = {0};
    uint8_t have_seq[PAD_TRACE_MAX_PORTS] = {0};
    uint64_t last_sync[PAD_TRACE_MAX_PORTS] = {0};
    uint8_t have_sync[PAD_TRACE_MAX_PORTS] = {0};
    uint64_t last_info = 0;
    int have_info = 0;
    int ended = 0;
    Line L;

    if (!f) {
        set_err(e, PAD_TRACE_ROLE_REPLAY, 0, "io", "cannot open the replay file: %s", strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        n++;
        if (len == 0 || line[len - 1] != '\n') {
            if (feof(f)) RFAIL(n, "truncated", "the last line has no newline: the file was cut off while it was written");
            /* fgets stops at a newline or a full buffer, never at a NUL: a short read with no newline
             * is a NUL byte, which is not text and not an over-long line. */
            if (len < LINE_MAX_BYTES - 1u) RFAIL(n, "syntax", "a NUL byte at column %zu: the trace is text", len + 1u);
            RFAIL(n, "syntax", "line longer than %u bytes", LINE_MAX_BYTES - 1u);
        }
        line[--len] = 0;
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (ended) RFAIL(n, "after-end", "a line follows the end record");
        if (len == 0) RFAIL(n, "syntax", "blank line");

        if (parse_line(line, &L, e)) {
            e->line = n;
            if (n == 1) snprintf(e->reason, sizeof e->reason, "header");
            goto fail;
        }

        if (n == 1) {
            if (!(L.seen & K_FORMAT) || strcmp(L.format, PAD_TRACE_FORMAT) != 0)
                RFAIL(n, "header", "the first line is not a {\"format\":\"" PAD_TRACE_FORMAT "\",...} header");
            /* The version decides before the key set: version 1 had no `clock` key and version 2 named
             * another clock, so the reason to refuse either is its version, not the shape of its header. */
            if (!(L.seen & K_VERSION)) RFAIL(n, "header", "the header has no \"version\"");
            if (L.version == 1u || L.version == 2u) {
                if (t->repeat > 1u)
                    RFAIL(n, "version",
                          "version %u keyed its records on the flip clock, not on the poll ordinal, so "
                          "pad.replay_repeat %u cannot serve it; set pad.replay_repeat 1 or re-record it as "
                          "version %u", L.version, t->repeat, PAD_TRACE_VERSION);
                RFAIL(n, "version",
                      "version %u keyed its records on the flip clock, which moves between a recording and "
                      "its replay when the guest catches up wall time; re-record it as version %u",
                      L.version, PAD_TRACE_VERSION);
            }
            if (L.version != PAD_TRACE_VERSION)
                RFAIL(n, "version", "version %u; this runtime reads version %u", L.version, PAD_TRACE_VERSION);
            if (L.seen != MASK_HEADER)
                RFAIL(n, "header", "the first line is not a {...\"clock\":\"%s\",\"ports\":..} header", PAD_TRACE_CLOCK);
            if (strcmp(L.clock, PAD_TRACE_CLOCK) != 0)
                RFAIL(n, "header", "clock \"%s\"; this runtime keys traces on \"%s\"", L.clock, PAD_TRACE_CLOCK);
            if (L.ports < 1 || L.ports > PAD_TRACE_MAX_PORTS)
                RFAIL(n, "header", "ports %u is outside 1..%u", L.ports, PAD_TRACE_MAX_PORTS);
            t->ports = L.ports;
            continue;
        }

        if (L.seen == MASK_STATE) {
            Rec* r;
            if (L.seq_array) RFAIL(n, "value", "a state record's \"seq\" is one poll ordinal, not an array");
            if (L.port >= t->ports) RFAIL(n, "port-range", "port %u, but the header declares %u ports", L.port, t->ports);
            if (L.len > LEN_MAX) RFAIL(n, "value", "len %u above %u", L.len, LEN_MAX);
            if (have_seq[L.port] && L.seq <= last_seq[L.port])
                RFAIL(n, "keys-backwards", "port %u: seq %llu does not follow seq %llu", L.port,
                      (unsigned long long)L.seq, (unsigned long long)last_seq[L.port]);
            t->recs[L.port] = (Rec*)grow(t->recs[L.port], &t->caprecs[L.port], t->nrecs[L.port] + 1, sizeof(Rec));
            if (!t->recs[L.port]) RFAIL(n, "io", "out of memory");
            r = &t->recs[L.port][t->nrecs[L.port]++];
            memset(r, 0, sizeof *r);
            r->seq = L.seq; r->flip = L.flip; r->line = n;
            r->st.len = (int32_t)L.len;
            memcpy(r->st.words, L.words, L.nwords * sizeof(uint16_t));
            last_seq[L.port] = L.seq; have_seq[L.port] = 1;
            t->stats.records++;
        } else if (L.seen == MASK_CONN) {
            ConnRec* c;
            if (L.nconn != t->ports) RFAIL(n, "value", "conn has %u entries, the header declares %u ports", L.nconn, t->ports);
            if (have_info && L.info <= last_info)
                RFAIL(n, "keys-backwards", "conn info %llu does not follow info %llu",
                      (unsigned long long)L.info, (unsigned long long)last_info);
            t->conns = (ConnRec*)grow(t->conns, &t->capconns, t->nconns + 1, sizeof(ConnRec));
            if (!t->conns) RFAIL(n, "io", "out of memory");
            c = &t->conns[t->nconns++];
            memset(c, 0, sizeof *c);
            c->info = L.info;
            memcpy(c->on, L.conn, L.nconn);
            last_info = L.info; have_info = 1;
        } else if (L.seen == MASK_END) {
            if (!L.seq_array) RFAIL(n, "value", "the end record's \"seq\" is the array of per-port poll counts");
            if (L.nseq != t->ports)
                RFAIL(n, "value", "the end record has %u poll counts, the header declares %u ports", L.nseq, t->ports);
            ended = 1;
            for (uint32_t p = 0; p < t->ports; p++) t->end_seq[p] = L.seqv[p];
            t->have_end = 1;
        } else if (L.seen == MASK_SYNC) {
            SyncRec* s;
            uint32_t kind = PS3_REPLAY_SYNC_NONE;
            if (L.seq_array) RFAIL(n, "value", "a sync record's \"seq\" is one poll ordinal, not an array");
            if (L.port >= t->ports) RFAIL(n, "port-range", "port %u, but the header declares %u ports", L.port, t->ports);
            if (!strcmp(L.sync, PS3_REPLAY_SYNC_NAME_PIPELINE)) kind = PS3_REPLAY_SYNC_PIPELINE;
            if (kind == PS3_REPLAY_SYNC_NONE)
                RFAIL(n, "sync-kind", "sync event \"%s\" is not one this runtime reports (known: \"%s\")", L.sync,
                      PS3_REPLAY_SYNC_NAME_PIPELINE);
            /* A sync re-bases the input clock, and k > 1 serves an ordinal several polls: the two do not compose */
            if (t->repeat > 1u)
                RFAIL(n, "repeat", "sync records re-base the poll ordinal and cannot be combined with pad.replay_repeat %u",
                      t->repeat);
            if (have_sync[L.port] && L.seq <= last_sync[L.port])
                RFAIL(n, "keys-backwards", "port %u: sync seq %llu does not follow sync seq %llu", L.port,
                      (unsigned long long)L.seq, (unsigned long long)last_sync[L.port]);
            t->syncs[L.port] = (SyncRec*)grow(t->syncs[L.port], &t->capsyncs[L.port], t->nsyncs[L.port] + 1, sizeof(SyncRec));
            if (!t->syncs[L.port]) RFAIL(n, "io", "out of memory");
            s = &t->syncs[L.port][t->nsyncs[L.port]++];
            s->seq = L.seq; s->key = L.key; s->kind = kind; s->line = n;
            last_sync[L.port] = L.seq; have_sync[L.port] = 1;
            t->stats.sync_records++;
        } else if ((L.seen | K_SEQ) == MASK_STATE) {
            RFAIL(n, "value", "a state record without \"seq\": version 3 keys on the port's poll ordinal");
        } else if ((L.seen | K_FLIP) == MASK_STATE) {
            RFAIL(n, "value", "a state record without \"flip\": version 3 writes the flip clock as an annotation");
        } else if ((L.seen | K_INFO) == MASK_CONN) {
            RFAIL(n, "value", "a connection record without \"info\": version 3 keys it on the info-call ordinal");
        } else {
            RFAIL(n, "record-kind", "the keys of this line match no record of the format");
        }
    }
    if (ferror(f)) { set_err(e, PAD_TRACE_ROLE_REPLAY, n, "io", "read error: %s", strerror(errno)); goto fail; }
    if (n == 0) RFAIL(1, "header", "the file is empty");
    /* The end record is last, so reachability is checked once all of it is known: a record at or past
     * its port's end count can never be served by a replay. The error names the record's own line. */
    if (t->have_end) {
        for (uint32_t p = 0; p < t->ports; p++)
            for (size_t i = 0; i < t->nrecs[p]; i++)
                if (t->recs[p][i].seq >= t->end_seq[p])
                    RFAIL(t->recs[p][i].line, "value",
                          "port %u: the record at seq %llu is at or past the end count %llu and can never be reached",
                          p, (unsigned long long)t->recs[p][i].seq, (unsigned long long)t->end_seq[p]);
        for (uint32_t p = 0; p < t->ports; p++)
            for (size_t i = 0; i < t->nsyncs[p]; i++)
                if (t->syncs[p][i].seq >= t->end_seq[p])
                    RFAIL(t->syncs[p][i].line, "value",
                          "port %u: the sync record at seq %llu is at or past the end count %llu and can never be reached",
                          p, (unsigned long long)t->syncs[p][i].seq, (unsigned long long)t->end_seq[p]);
    }
    fclose(f);
    memcpy(t->stats.end_seq, t->end_seq, sizeof t->end_seq);
    t->stats.have_end = t->have_end;
    return 0;
fail:
    fclose(f);
    return -1;
}

static const Rec* rec_at_or_before(const PadTrace* t, uint32_t port, uint64_t seq, size_t* idx)
{
    size_t lo = 0, hi = t->nrecs[port];   /* first record with a seq above `seq` */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (t->recs[port][mid].seq <= seq) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return NULL;
    *idx = lo - 1;
    return &t->recs[port][lo - 1];
}

static void drift_note(PadTrace* t, uint32_t flip, const Rec* r)
{
    const int64_t d = (int64_t)flip - (int64_t)r->flip;
    if (!t->stats.have_drift) {
        t->stats.have_drift = 1;
        t->stats.drift_min = t->stats.drift_max = d;
    } else {
        if (d < t->stats.drift_min) t->stats.drift_min = d;
        if (d > t->stats.drift_max) t->stats.drift_max = d;
    }
}

/* ---------------------------------------------------------------------------
 * Writer
 * -------------------------------------------------------------------------*/

static void trace_stop(const PadTrace* t, PadTraceRole role, uint32_t line, const char* reason, const char* detail)
{
    ps3_stop_pad_trace(role_name(role), role == PAD_TRACE_ROLE_RECORD ? t->record_path : t->replay_path, line,
                       reason, detail);
}

static void wflush(PadTrace* t)
{
    if (!t->wf || !t->wlen) { t->wdirty = 0; return; }
    /* The stream is unbuffered, so this is one write of whole lines: a kill
     * leaves the file ending at a line boundary. */
    if (fwrite(t->wbuf, 1, t->wlen, t->wf) != t->wlen || fflush(t->wf) != 0)
        trace_stop(t, PAD_TRACE_ROLE_RECORD, 0, "io", strerror(errno));
    t->wlen = 0;
    t->wdirty = 0;
    t->w_flush_ms = now_ms();
}

static void wline(PadTrace* t, const char* fmt, ...)
{
    char tmp[LINE_MAX_BYTES];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp - 1) trace_stop(t, PAD_TRACE_ROLE_RECORD, 0, "io", "record line does not fit its buffer");
    tmp[n++] = '\n';
    if (t->wlen + (size_t)n > WBUF_BYTES) wflush(t);
    memcpy(t->wbuf + t->wlen, tmp, (size_t)n);
    t->wlen += (size_t)n;
    t->wdirty = 1;
}

static void w_state(PadTrace* t, uint64_t seq, uint32_t port, uint32_t flip, const PadTraceState* st)
{
    char words[PAD_TRACE_MAX_WORDS * 6 + 4];
    size_t n = PAD_TRACE_MAX_WORDS, off = 0;
    while (n && st->words[n - 1] == 0) n--;
    words[off++] = '[';
    for (size_t i = 0; i < n; i++)
        off += (size_t)snprintf(words + off, sizeof words - off, i ? ",%u" : "%u", (unsigned)st->words[i]);
    words[off++] = ']';
    words[off] = 0;
    wline(t, "{\"seq\":%llu,\"port\":%u,\"len\":%d,\"words\":%s,\"flip\":%u}", (unsigned long long)seq, port,
          (int)st->len, words, flip);
}

static void flush_if_due(PadTrace* t)
{
    if (t->wdirty && now_ms() - t->w_flush_ms >= FLUSH_INTERVAL_MS) wflush(t);
}

/* ---------------------------------------------------------------------------
 * Summary
 * -------------------------------------------------------------------------*/

static void summary(const PadTrace* t, char* b, size_t n)
{
    char drift[48];
    if (!t->stats.have_drift) snprintf(drift, sizeof drift, "none");
    else snprintf(drift, sizeof drift, "%lld:%lld", (long long)t->stats.drift_min, (long long)t->stats.drift_max);
    snprintf(b, n,
             "polls=%llu exact=%llu held=%llu recorded_changes=%llu reached_exactly=%llu flip_drift=%s",
             (unsigned long long)t->stats.polls, (unsigned long long)t->stats.exact, (unsigned long long)t->stats.held,
             (unsigned long long)t->stats.records, (unsigned long long)t->stats.records_reached, drift);
    /* k = 1 keeps the historical summary byte for byte; the repeat is named
     * only when one was asked for. */
    if (t->repeat != 1u) {
        const size_t used = strlen(b);
        if (used < n)
            snprintf(b + used, n - used, " repeat=%u", t->repeat);
    }
    /* a trace without sync records keeps the summary byte for byte */
    if (t->stats.sync_records) {
        const size_t used = strlen(b);
        if (used < n)
            snprintf(b + used, n - used, " sync=%llu/%llu sync_missed=%llu sync_waited=%llu",
                     (unsigned long long)t->stats.sync_hits, (unsigned long long)t->stats.sync_records,
                     (unsigned long long)t->stats.sync_missed, (unsigned long long)t->stats.sync_waited);
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

int pad_trace_replaying(const PadTrace* t) { return t && t->replaying; }

static void destroy_storage(PadTrace* t)
{
    for (unsigned i = 0; i < PAD_TRACE_MAX_PORTS; i++) free(t->recs[i]);
    for (unsigned i = 0; i < PAD_TRACE_MAX_PORTS; i++) free(t->syncs[i]);
    for (unsigned i = 0; i < PAD_TRACE_MAX_PORTS; i++) free(t->flip_poll[i]);
    free(t->conns);
    free(t->wbuf);
    free(t->record_path);
    free(t->replay_path);
    if (t->wf) fclose(t->wf);
    trace_lock_destroy(&t->lock);
    free(t);
}

int pad_trace_create(const PadTraceOptions* o, PadTrace** out, PadTraceError* err)
{
    const int rec = o->record_path && o->record_path[0];
    const int rep = o->replay_path && o->replay_path[0];
    PadTrace* t;
    PadTraceError local;
    if (!err) err = &local;

    *out = NULL;
    if (!rec && !rep) return 0;
    if (o->replay_repeat < 1u || o->replay_repeat > PAD_TRACE_REPEAT_MAX) {
        set_err(err, PAD_TRACE_ROLE_REPLAY, 0, "repeat", "pad.replay_repeat %u is outside 1..%u",
                o->replay_repeat, PAD_TRACE_REPEAT_MAX);
        return -1;
    }
    t = (PadTrace*)calloc(1, sizeof *t);
    if (!t) { set_err(err, PAD_TRACE_ROLE_RECORD, 0, "io", "out of memory"); return -1; }
    trace_lock_init(&t->lock);
    t->flip_fn = o->flip;
    t->repeat = o->replay_repeat;
    t->stats.repeat = t->repeat;

    if (rep) {
        t->replay_path = dup_str(o->replay_path);
        if (!t->replay_path || replay_load(t, o->replay_path, err)) goto fail;
        t->replaying = 1;
        /* A killed run has no end record: the writer flushes at most 100 ms behind, so the last
         * interval is lost, but every record in the file is reachable and was really served. Holding
         * each port's last record is the recorded truth up to the kill; refusing would throw the whole
         * recording away for a kill the recorder is built to survive. Say it once instead. */
        if (!t->have_end) {
            printf("[cellPad] pad replay: the trace has no end record; the last state of each port is held\n");
            fflush(stdout);
        }
    }
    if (rec) {
        char header[160];
        t->record_path = dup_str(o->record_path);
        t->wbuf = (char*)malloc(WBUF_BYTES);
        if (!t->record_path || !t->wbuf) { set_err(err, PAD_TRACE_ROLE_RECORD, 0, "io", "out of memory"); goto fail; }
        /* "x": fail if the file exists. A recording is never overwritten. */
        t->wf = fopen(o->record_path, "wbx");
        if (!t->wf) {
            set_err(err, PAD_TRACE_ROLE_RECORD, 0, errno == EEXIST ? "exists" : "io", "cannot create the record file: %s",
                    strerror(errno));
            goto fail;
        }
        setvbuf(t->wf, NULL, _IONBF, 0);
        t->w_flush_ms = now_ms();
        snprintf(header, sizeof header,
                 "{\"format\":\"" PAD_TRACE_FORMAT "\",\"version\":%u,\"clock\":\"" PAD_TRACE_CLOCK "\",\"ports\":%u}",
                 PAD_TRACE_VERSION, PAD_TRACE_MAX_PORTS);
        wline(t, "%s", header);
        wflush(t);
    }
    *out = t;
    return 0;
fail:
    destroy_storage(t);
    return -1;
}

PadTrace* pad_trace_open(const PadTraceOptions* o)
{
    PadTrace* t;
    PadTraceError e;
    PadTraceRole role = PAD_TRACE_ROLE_REPLAY;
    memset(&e, 0, sizeof e);
    if (pad_trace_create(o, &t, &e) == 0) return t;
    role = e.role;
    ps3_stop_pad_trace(role_name(role), role == PAD_TRACE_ROLE_RECORD ? o->record_path : o->replay_path, e.line, e.reason,
                       e.detail);
}

static PadTrace* g_trace;
static trace_lock g_lock = TRACE_LOCK_INIT;
static int g_opened;

static void exit_hook(void)
{
    PadTrace* t = g_trace;
    if (t) pad_trace_close(t);
}

PadTrace* pad_trace_global(const PadTraceOptions* o)
{
    PadTrace* t;
    trace_lock_acquire(&g_lock);
    if (!g_opened) {
        g_trace = pad_trace_open(o);
        g_opened = 1;
        if (g_trace) {
            atexit(exit_hook);
            ps3_host_at_quick_exit(exit_hook);
        }
    }
    t = g_trace;
    trace_lock_release(&g_lock);
    return t;
}

/* What the ordinal replay serves a port at recorded ordinal `ord`: the latest
 * record at or before it, a neutral pad before the port's first record, and a
 * neutral pad from the end count on (said once). Returns the record the answer
 * came from, NULL for the neutral pad. A record is counted reached, and its
 * drift read, when a poll first serves it at its own ordinal. */
static const Rec* replay_serve(PadTrace* t, uint32_t port, uint64_t ord, int waiting, uint32_t flip,
                               PadTraceState* st, const PadTraceState* neutral)
{
    size_t idx = 0;
    const Rec* r = NULL;
    if (t->have_end && ord >= t->end_seq[port]) {
        if (!t->end_logged) {
            char sum[384];
            t->end_logged = 1;
            t->stats.ended = 1;
            summary(t, sum, sizeof sum);
            printf("[cellPad] pad replay: the trace ended (port 0 polled %llu times); holding a neutral pad. %s\n",
                   (unsigned long long)t->end_seq[0], sum);
            fflush(stdout);
            /* The end of the input is the end of the measured scenario: a --pc-counters lift writes its
             * PS3_PC_COUNTERS_OUT dump here, so two runs of one trace count the same input, and a
             * headless run killed later by its timeout still leaves the file (no exit handler runs). */
            ps3_pc_counters_dump_now();
        }
    } else {
        r = rec_at_or_before(t, port, ord, &idx);
    }
    if (r) {
        *st = r->st;
        /* A poll served while the clock waits at a pending event holds the state at that ordinal again and
         * again: it is not the poll that lands on the record, and it must not count as one. */
        if (r->seq == ord && !waiting) {
            t->stats.exact++;
            /* The flip is annotation: how far it moved between the recording and this replay is the
             * guest-time drift the ordinal key absorbs, and what the summary reports. */
            if (!t->recs[port][idx].hit) {
                t->recs[port][idx].hit = 1;
                t->stats.records_reached++;
                drift_note(t, flip, r);
            }
        }
    } else {
        *st = *neutral;
    }
    t->stats.held = t->stats.polls - t->stats.exact;
    return r;
}

/* Diagnostic: PS3_PC_COUNTERS_DUMP_AT=<ord>[,<ord>...] writes the --pc-counters table to
 * "$PS3_PC_COUNTERS_OUT.at<ord>" when port 0 first reaches recorded ordinal <ord> (poll ord * repeat), so a base
 * run and a repeat run count the same recorded input up to the same point, before they can diverge. Read once, at
 * the first replayed poll; called under the trace lock. */
#define DUMP_AT_MAX 64u
static uint64_t g_dump_at[DUMP_AT_MAX];
static size_t g_dump_at_n;
static int g_dump_at_read;

static void dump_at_check(const PadTrace* t, uint64_t seq)
{
    if (!g_dump_at_read) {
        const char* v = getenv("PS3_PC_COUNTERS_DUMP_AT");
        g_dump_at_read = 1;
        while (v && *v && g_dump_at_n < DUMP_AT_MAX) {
            char* end;
            g_dump_at[g_dump_at_n++] = strtoull(v, &end, 10);
            if (end == v) { g_dump_at_n--; break; }
            v = *end == ',' ? end + 1 : end;
            if (*end != ',') break;
        }
    }
    if (seq % t->repeat) return;
    for (size_t i = 0; i < g_dump_at_n; i++) {
        const char* out = getenv("PS3_PC_COUNTERS_OUT");
        char path[1024];
        if (g_dump_at[i] != seq / t->repeat || !out || !*out) continue;
        snprintf(path, sizeof path, "%s.at%llu", out, (unsigned long long)g_dump_at[i]);
        ps3_pc_counters_snapshot(path);
    }
}

/* Note the flip requests the guest made since the port's last poll: each was made after `seq` polls of the port.
 * Bounded, so a garbage flip count cannot ask for gigabytes; past the bound the table stops growing and an event
 * for a later frame is taken at the poll that reports it. Called under the lock. */
#define FLIP_NOTE_MAX (1u << 24)
static void flip_note(PadTrace* t, uint32_t port, uint64_t seq, uint32_t flip)
{
    while (t->nflip[port] < flip && t->nflip[port] < FLIP_NOTE_MAX) {
        t->flip_poll[port] = (uint64_t*)grow(t->flip_poll[port], &t->capflip[port], t->nflip[port] + 1, sizeof(uint64_t));
        if (!t->flip_poll[port]) trace_stop(t, PAD_TRACE_ROLE_REPLAY, 0, "io", "out of memory");
        t->flip_poll[port][t->nflip[port]++] = seq;
    }
}
/* The recorded ordinal a poll of `port` is served when the port has sync records. The clock runs one ordinal per
 * poll from the last re-base on (recorded = ordinal - shift) and does not pass the ordinal of the next pending
 * event: the recording made the input after the event only once the event had happened, so the replay does not
 * hand it to a guest that has not got there.
 *
 * A re-base can move the clock forward by many ordinals at once (the event is placed at the guest's flip request,
 * before the poll that learns of it; or the guest got there before the recording did). Jumping would drop every
 * record in between: a tap there is never pressed, a hold is cut. The clock therefore closes the gap only over
 * idle input: inside a press or a stick deflection it advances one ordinal per poll, so the input keeps its
 * length, and from an idle interval it jumps at most to the next record. Called under the lock. */
static uint64_t sync_ordinal(PadTrace* t, uint32_t port, uint64_t seq, int* waiting)
{
    int64_t target = (int64_t)seq - t->sync_shift[port];
    const size_t i = t->sync_next[port];
    int held = 0;
    if (i < t->nsyncs[port] && target > (int64_t)t->syncs[port][i].seq) {
        target = (int64_t)t->syncs[port][i].seq;
        held = 1;
    }
    uint64_t rec = target < 0 ? 0u : (uint64_t)target;
    const uint64_t from = t->sync_from[port];
    if (rec > from) {
        size_t idx = 0;
        const Rec* cur = from ? rec_at_or_before(t, port, from - 1u, &idx) : NULL;
        if (cur && !state_idle(&cur->st)) {
            rec = from;
        } else {
            const size_t next = cur ? idx + 1u : 0u;
            if (next < t->nrecs[port] && t->recs[port][next].seq < rec) rec = t->recs[port][next].seq;
        }
    }
    if (held && rec == (uint64_t)target) {
        *waiting = 1;
        t->stats.sync_waited++;
    }
    if (rec + 1u > t->sync_from[port]) t->sync_from[port] = rec + 1u;
    return rec;
}

void pad_trace_poll(PadTrace* t, uint32_t port, PadTraceState* st, const PadTraceState* neutral)
{
    uint64_t seq;
    uint32_t flip;
    if (port >= PAD_TRACE_MAX_PORTS) {
        if (t->replaying) *st = *neutral;
        return;
    }
    trace_lock_acquire(&t->lock);
    seq = t->seq[port]++;
    flip = t->flip_fn();
    if (t->replaying && t->nsyncs[port]) flip_note(t, port, seq, flip);

    if (t->replaying) {
        t->stats.polls++;
        if (port == 0) dump_at_check(t, seq);
        /* pad.replay_repeat: for a title that polls once per simulation step, each recorded step is served
         * k guest steps. Poll p is answered exactly as the k = 1 replay answers recorded ordinal p / k:
         * an ordinal with no record of its own holds the latest state, the end count is reached when
         * p / k >= end_seq, and a record is reached (its drift read) when a poll first serves it. */
        {
            uint64_t ord = seq / t->repeat;
            int waiting = 0;
            if (t->nsyncs[port]) ord = sync_ordinal(t, port, seq, &waiting);
            replay_serve(t, port, ord, waiting, flip, st, neutral);
        }
    }

    if (t->wf && !t->wclosed) {
        if (!t->w_have[port] || !state_equal(&t->w_last[port], st)) {
            w_state(t, seq, port, flip, st);
            t->w_last[port] = *st;
            t->w_have[port] = 1;
        }
        flush_if_due(t);
    }
    trace_lock_release(&t->lock);
}

void pad_trace_conn(PadTrace* t, uint8_t on[PAD_TRACE_MAX_PORTS])
{
    uint64_t info;
    trace_lock_acquire(&t->lock);
    info = t->info_seq++;
    if (t->replaying && t->nconns) {
        size_t lo = 0, hi = t->nconns;   /* first record above `info` */
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (t->conns[mid].info <= info) lo = mid + 1; else hi = mid;
        }
        if (lo) memcpy(on, t->conns[lo - 1].on, t->ports);
    }
    if (t->wf && !t->wclosed) {
        if (!t->w_have_conn || memcmp(t->w_conn, on, PAD_TRACE_MAX_PORTS) != 0) {
            char arr[PAD_TRACE_MAX_PORTS * 2 + 2];
            size_t off = 0;
            arr[off++] = '[';
            for (unsigned i = 0; i < PAD_TRACE_MAX_PORTS; i++) {
                if (i) arr[off++] = ',';
                arr[off++] = on[i] ? '1' : '0';
            }
            arr[off++] = ']';
            arr[off] = 0;
            wline(t, "{\"info\":%llu,\"conn\":%s}", (unsigned long long)info, arr);
            memcpy(t->w_conn, on, PAD_TRACE_MAX_PORTS);
            t->w_have_conn = 1;
        }
        flush_if_due(t);
    }
    trace_lock_release(&t->lock);
}

uint64_t pad_trace_sync_count(const PadTrace* t) { return t ? t->stats.sync_records : 0; }

void pad_trace_sync_event(PadTrace* t, uint32_t kind, uint64_t key, uint32_t frame)
{
    if (!t || !t->replaying || !t->stats.sync_records) return;
    trace_lock_acquire(&t->lock);
    for (uint32_t p = 0; p < t->ports; p++) {
        const size_t n = t->nsyncs[p];
        /* An event may pass over a few pending records whose events never came (a pipeline the run already had);
         * one further ahead is far more likely the same key built again than the course jumping forward. */
        const size_t lim = n - t->sync_next[p] > PAD_TRACE_SYNC_SKIP_MAX + 1u ? t->sync_next[p] + PAD_TRACE_SYNC_SKIP_MAX + 1u : n;
        size_t i = t->sync_next[p];
        while (i < lim && !(t->syncs[p][i].kind == kind && t->syncs[p][i].key == key)) i++;
        if (i == lim) continue;   /* not an event this port waits for now (or already passed) */
        /* The guest event happened when the guest made its `frame`-th flip request: after the polls noted for it, or
         * just now when the guest has not polled since (or the frame is unknown). Not when this call arrives: the
         * draw engine reports a frame later than the guest made it, by as much as the engine lags the guest. */
        const uint64_t at = (frame >= 1u && frame <= t->nflip[p]) ? t->flip_poll[p][frame - 1u] : t->seq[p];
        t->stats.sync_missed += i - t->sync_next[p];
        t->sync_next[p] = i + 1u;
        t->sync_shift[p] = (int64_t)at - (int64_t)t->syncs[p][i].seq;
        t->stats.sync_hits++;
        printf("[cellPad] pad replay sync: event %u key %016llx port %u at poll %llu (reported at %llu) re-based to "
               "recorded poll %llu (shift %lld)\n",
               kind, (unsigned long long)key, p, (unsigned long long)at, (unsigned long long)t->seq[p],
               (unsigned long long)t->syncs[p][i].seq, (long long)t->sync_shift[p]);
        fflush(stdout);
    }
    trace_lock_release(&t->lock);
}

void pad_trace_flush(PadTrace* t)
{
    if (!t) return;
    trace_lock_acquire(&t->lock);
    if (t->wf && !t->wclosed) wflush(t);
    trace_lock_release(&t->lock);
}

void pad_trace_close(PadTrace* t)
{
    char sum[384];
    int say = 0;
    if (!t) return;
    trace_lock_acquire(&t->lock);
    if (t->wf && !t->wclosed) {
        /* The end marker is each port's poll count, the unit a replay counts in. */
        char counts[PAD_TRACE_MAX_PORTS * 21 + 4];
        size_t off = 0;
        counts[off++] = '[';
        for (unsigned i = 0; i < PAD_TRACE_MAX_PORTS; i++)
            off += (size_t)snprintf(counts + off, sizeof counts - off, i ? ",%llu" : "%llu",
                                    (unsigned long long)t->seq[i]);
        counts[off++] = ']';
        counts[off] = 0;
        wline(t, "{\"end\":true,\"seq\":%s}", counts);
        wflush(t);
        t->wclosed = 1;
        fclose(t->wf);
        t->wf = NULL;
    }
    if (t->replaying && !t->summary_logged) {
        t->summary_logged = 1;
        summary(t, sum, sizeof sum);
        say = 1;
    }
    trace_lock_release(&t->lock);
    if (say) {
        printf("[cellPad] pad replay summary: %s\n", sum);
        fflush(stdout);
    }
}

void pad_trace_destroy(PadTrace* t)
{
    if (!t) return;
    pad_trace_close(t);
    destroy_storage(t);
}

void pad_trace_stats(PadTrace* t, PadTraceStats* out)
{
    trace_lock_acquire(&t->lock);
    *out = t->stats;
    trace_lock_release(&t->lock);
}

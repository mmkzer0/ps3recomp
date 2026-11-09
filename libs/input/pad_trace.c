/*
 * ps3recomp - pad trace writer, reader and lookup (see pad_trace.h)
 *
 * File format, version 1: UTF-8 text, one JSON object per line, no host paths
 * and no timestamps.
 *
 *   {"format":"ps3recomp-pad-trace","version":1,"ports":7}          header, line 1
 *   {"flip":F,"poll":P,"port":N,"len":L,"words":[...]}              pad state
 *   {"flip":F,"conn":[c0,...,c6]}                                    connection
 *   {"flip":F,"port":N,"polls":C}                                    poll count
 *   {"end":true,"flip":F}                                            last line
 *
 * A state record gives the state of port N from poll P of flip F on, until the
 * next record of that port. `words` stops at its last non-zero word; the rest
 * are zero. A connection record says which ports GetInfo reported connected.
 * A poll-count record is a run-length step: from flip F, port N is polled C
 * times per flip until the next step of that port (C = 0 for flips the guest
 * did not poll). They exist so a replay can say where the guest's polling
 * stopped matching the recording; they never change what a poll returns. The
 * end record carries the last flip of the run; a killed run has none and ends
 * at its last record.
 *
 * Ordering: `flip` never decreases over state, connection and end records, and
 * (flip, poll) strictly increases per port. Poll-count steps are written when a
 * flip is complete, so they are exempt from the first rule and only increase in
 * flip per port.
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

#include "ps3emu/stop.h"

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
    uint32_t      flip, poll;
    uint8_t       hit;            /* a replayed poll matched this key exactly */
    PadTraceState st;
} Rec;

typedef struct { uint32_t flip; uint8_t on[PAD_TRACE_MAX_PORTS]; } ConnRec;
typedef struct { uint32_t flip, polls; } Step;

typedef struct { uint32_t cur_flip, cur_count; int any; } Tracker;

struct PadTrace {
    trace_lock      lock;
    uint32_t      (*flip_fn)(void);
    uint32_t        max_flip;     /* highest flip any call has seen */
    Tracker         trk[PAD_TRACE_MAX_PORTS];

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
    uint32_t        w_next[PAD_TRACE_MAX_PORTS];   /* first flip not yet accounted in a step */
    uint32_t        w_rle[PAD_TRACE_MAX_PORTS];    /* count the last step declared */

    /* reader */
    int             replaying;
    char*           replay_path;
    uint32_t        ports;        /* the header's */
    Rec*            recs[PAD_TRACE_MAX_PORTS];
    size_t          nrecs[PAD_TRACE_MAX_PORTS];
    size_t          caprecs[PAD_TRACE_MAX_PORTS];
    Step*           steps[PAD_TRACE_MAX_PORTS];
    size_t          nsteps[PAD_TRACE_MAX_PORTS];
    size_t          capsteps[PAD_TRACE_MAX_PORTS];
    ConnRec*        conns;
    size_t          nconns, capconns;
    uint32_t        end_flip;
    uint32_t        r_next[PAD_TRACE_MAX_PORTS];   /* first flip not yet compared with the steps */
    PadTraceState   served[PAD_TRACE_MAX_PORTS];
    uint8_t         have_served[PAD_TRACE_MAX_PORTS];
    int             end_logged;
    int             summary_logged;
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

static uint64_t key_of(uint32_t flip, uint32_t poll) { return ((uint64_t)flip << 32) | poll; }

static int state_equal(const PadTraceState* a, const PadTraceState* b)
{
    return a->len == b->len && memcmp(a->words, b->words, sizeof a->words) == 0;
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
    K_FORMAT = 1u << 0, K_VERSION = 1u << 1, K_PORTS = 1u << 2,
    K_FLIP   = 1u << 3, K_POLL    = 1u << 4, K_PORT  = 1u << 5, K_LEN = 1u << 6, K_WORDS = 1u << 7,
    K_CONN   = 1u << 8, K_POLLS   = 1u << 9, K_END   = 1u << 10,
};

#define MASK_HEADER (K_FORMAT | K_VERSION | K_PORTS)
#define MASK_STATE  (K_FLIP | K_POLL | K_PORT | K_LEN | K_WORDS)
#define MASK_CONN   (K_FLIP | K_CONN)
#define MASK_STEP   (K_FLIP | K_PORT | K_POLLS)
#define MASK_END    (K_END | K_FLIP)

typedef struct {
    unsigned seen;
    char     format[32];
    uint32_t version, ports, flip, poll, port, len, polls;
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

static int parse_u32(Cur* c, uint32_t* v)
{
    uint64_t acc = 0;
    skip_ws(c);
    if (*c->p < '0' || *c->p > '9') PFAIL(c, "syntax", "expected a number at column %d", (int)(c->p - c->s) + 1);
    if (c->p[0] == '0' && c->p[1] >= '0' && c->p[1] <= '9')
        PFAIL(c, "syntax", "leading zero at column %d", (int)(c->p - c->s) + 1);
    while (*c->p >= '0' && *c->p <= '9') {
        acc = acc * 10u + (uint64_t)(*c->p - '0');
        if (acc > 0xFFFFFFFFull) PFAIL(c, "value", "number above 4294967295 at column %d", (int)(c->p - c->s) + 1);
        c->p++;
    }
    *v = (uint32_t)acc;
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
        enum { T_STR, T_U32, T_WORDS, T_CONN, T_TRUE } type = T_U32;
        uint32_t* dst = NULL;
        if (parse_string(c, key, sizeof key)) return -1;
        if (expect_ch(c, ':')) return -1;
        if      (!strcmp(key, "format"))  { bit = K_FORMAT;  type = T_STR; }
        else if (!strcmp(key, "version")) { bit = K_VERSION; dst = &L->version; }
        else if (!strcmp(key, "ports"))   { bit = K_PORTS;   dst = &L->ports; }
        else if (!strcmp(key, "flip"))    { bit = K_FLIP;    dst = &L->flip; }
        else if (!strcmp(key, "poll"))    { bit = K_POLL;    dst = &L->poll; }
        else if (!strcmp(key, "port"))    { bit = K_PORT;    dst = &L->port; }
        else if (!strcmp(key, "len"))     { bit = K_LEN;     dst = &L->len; }
        else if (!strcmp(key, "polls"))   { bit = K_POLLS;   dst = &L->polls; }
        else if (!strcmp(key, "words"))   { bit = K_WORDS;   type = T_WORDS; }
        else if (!strcmp(key, "conn"))    { bit = K_CONN;    type = T_CONN; }
        else if (!strcmp(key, "end"))     { bit = K_END;     type = T_TRUE; }
        else PFAIL(c, "unknown-key", "key \"%s\" is not part of the format", key);
        if (L->seen & bit) PFAIL(c, "duplicate-key", "key \"%s\" appears twice", key);
        skip_ws(c);
        switch (type) {
        case T_STR:
            if (parse_string(c, L->format, sizeof L->format)) return -1;
            break;
        case T_U32:
            if (parse_u32(c, dst)) return -1;
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
    uint32_t n = 0, last_flip = 0;
    uint64_t last_key[PAD_TRACE_MAX_PORTS] = {0};
    uint8_t have_key[PAD_TRACE_MAX_PORTS] = {0};
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
            if (L.seen != MASK_HEADER || strcmp(L.format, PAD_TRACE_FORMAT) != 0)
                RFAIL(n, "header", "the first line is not a {\"format\":\"" PAD_TRACE_FORMAT "\",\"version\":..,\"ports\":..} header");
            if (L.version != PAD_TRACE_VERSION)
                RFAIL(n, "version", "version %u; this runtime reads version %u", L.version, PAD_TRACE_VERSION);
            if (L.ports < 1 || L.ports > PAD_TRACE_MAX_PORTS)
                RFAIL(n, "header", "ports %u is outside 1..%u", L.ports, PAD_TRACE_MAX_PORTS);
            t->ports = L.ports;
            continue;
        }

        if (L.seen == MASK_STATE) {
            Rec* r;
            uint64_t key = key_of(L.flip, L.poll);
            if (L.port >= t->ports) RFAIL(n, "port-range", "port %u, but the header declares %u ports", L.port, t->ports);
            if (L.len > LEN_MAX) RFAIL(n, "value", "len %u above %u", L.len, LEN_MAX);
            if (L.flip < last_flip) RFAIL(n, "keys-backwards", "flip %u after flip %u", L.flip, last_flip);
            if (have_key[L.port] && key <= last_key[L.port])
                RFAIL(n, "keys-backwards", "port %u: (flip %u, poll %u) does not follow (flip %u, poll %u)", L.port,
                      L.flip, L.poll, (uint32_t)(last_key[L.port] >> 32), (uint32_t)last_key[L.port]);
            t->recs[L.port] = (Rec*)grow(t->recs[L.port], &t->caprecs[L.port], t->nrecs[L.port] + 1, sizeof(Rec));
            if (!t->recs[L.port]) RFAIL(n, "io", "out of memory");
            r = &t->recs[L.port][t->nrecs[L.port]++];
            memset(r, 0, sizeof *r);
            r->flip = L.flip; r->poll = L.poll;
            r->st.len = (int32_t)L.len;
            memcpy(r->st.words, L.words, L.nwords * sizeof(uint16_t));
            last_flip = L.flip; last_key[L.port] = key; have_key[L.port] = 1;
            t->stats.records++;
        } else if (L.seen == MASK_CONN) {
            ConnRec* c;
            if (L.nconn != t->ports) RFAIL(n, "value", "conn has %u entries, the header declares %u ports", L.nconn, t->ports);
            if (L.flip < last_flip) RFAIL(n, "keys-backwards", "flip %u after flip %u", L.flip, last_flip);
            t->conns = (ConnRec*)grow(t->conns, &t->capconns, t->nconns + 1, sizeof(ConnRec));
            if (!t->conns) RFAIL(n, "io", "out of memory");
            c = &t->conns[t->nconns++];
            memset(c, 0, sizeof *c);
            c->flip = L.flip;
            memcpy(c->on, L.conn, L.nconn);
            last_flip = L.flip;
        } else if (L.seen == MASK_STEP) {
            Step* s;
            if (L.port >= t->ports) RFAIL(n, "port-range", "port %u, but the header declares %u ports", L.port, t->ports);
            if (t->nsteps[L.port] && L.flip <= t->steps[L.port][t->nsteps[L.port] - 1].flip)
                RFAIL(n, "keys-backwards", "port %u: poll-count step at flip %u does not follow flip %u", L.port, L.flip,
                      t->steps[L.port][t->nsteps[L.port] - 1].flip);
            t->steps[L.port] = (Step*)grow(t->steps[L.port], &t->capsteps[L.port], t->nsteps[L.port] + 1, sizeof(Step));
            if (!t->steps[L.port]) RFAIL(n, "io", "out of memory");
            s = &t->steps[L.port][t->nsteps[L.port]++];
            s->flip = L.flip; s->polls = L.polls;
            if (L.flip > t->end_flip) t->end_flip = L.flip;
        } else if (L.seen == MASK_END) {
            if (L.flip < last_flip) RFAIL(n, "keys-backwards", "end flip %u is before flip %u", L.flip, last_flip);
            last_flip = L.flip;
            ended = 1;
        } else {
            RFAIL(n, "record-kind", "the keys of this line match no record of the format");
        }
        if (last_flip > t->end_flip) t->end_flip = last_flip;
    }
    if (ferror(f)) { set_err(e, PAD_TRACE_ROLE_REPLAY, n, "io", "read error: %s", strerror(errno)); goto fail; }
    if (n == 0) RFAIL(1, "header", "the file is empty");
    fclose(f);
    t->stats.end_flip = t->end_flip;
    return 0;
fail:
    fclose(f);
    return -1;
}

static const Rec* rec_at_or_before(const PadTrace* t, uint32_t port, uint64_t key, size_t* idx)
{
    size_t lo = 0, hi = t->nrecs[port];   /* first record with a key above `key` */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (key_of(t->recs[port][mid].flip, t->recs[port][mid].poll) <= key) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return NULL;
    *idx = lo - 1;
    return &t->recs[port][lo - 1];
}

/* The poll count the recording had for `flip`. */
static uint32_t expected_polls(const PadTrace* t, uint32_t port, uint32_t flip)
{
    size_t lo = 0, hi = t->nsteps[port];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (t->steps[port][mid].flip <= flip) lo = mid + 1; else hi = mid;
    }
    return lo ? t->steps[port][lo - 1].polls : 0;
}

/* The first flip in [a, b] with a non-zero recorded count, or -1. */
static int64_t first_polled_flip(const PadTrace* t, uint32_t port, uint32_t a, uint32_t b)
{
    size_t lo = 0, hi = t->nsteps[port];
    if (expected_polls(t, port, a)) return a;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (t->steps[port][mid].flip <= a) lo = mid + 1; else hi = mid;
    }
    for (; lo < t->nsteps[port] && t->steps[port][lo].flip <= b; lo++)
        if (t->steps[port][lo].polls) return t->steps[port][lo].flip;
    return -1;
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

static void w_state(PadTrace* t, uint32_t port, uint32_t flip, uint32_t poll, const PadTraceState* st)
{
    char words[PAD_TRACE_MAX_WORDS * 6 + 4];
    size_t n = PAD_TRACE_MAX_WORDS, off = 0;
    while (n && st->words[n - 1] == 0) n--;
    words[off++] = '[';
    for (size_t i = 0; i < n; i++)
        off += (size_t)snprintf(words + off, sizeof words - off, i ? ",%u" : "%u", (unsigned)st->words[i]);
    words[off++] = ']';
    words[off] = 0;
    wline(t, "{\"flip\":%u,\"poll\":%u,\"port\":%u,\"len\":%d,\"words\":%s}", flip, poll, port, (int)st->len, words);
}

static void w_poll_done(PadTrace* t, uint32_t port, uint32_t flip, uint32_t count)
{
    /* Flips between the last accounted one and this one had no polls. */
    if (flip > t->w_next[port] && t->w_rle[port] != 0) {
        wline(t, "{\"flip\":%u,\"port\":%u,\"polls\":0}", t->w_next[port], port);
        t->w_rle[port] = 0;
    }
    if (count != t->w_rle[port]) {
        wline(t, "{\"flip\":%u,\"port\":%u,\"polls\":%u}", flip, port, count);
        t->w_rle[port] = count;
    }
    t->w_next[port] = flip + 1u;
}

/* A flip's polls for `port` are complete: account it for the file and for the
 * divergence report. `final` is the process ending: the replay side then has no
 * complete flip to compare. */
static void flip_complete(PadTrace* t, uint32_t port, uint32_t flip, uint32_t count, int final)
{
    if (t->wf && !t->wclosed) w_poll_done(t, port, flip, count);
    if (t->replaying && !final && flip <= t->end_flip) {
        int64_t div = -1;
        if (flip > t->r_next[port]) div = first_polled_flip(t, port, t->r_next[port], flip - 1u);
        if (div < 0 && expected_polls(t, port, flip) != count) div = flip;
        if (div >= 0 && t->stats.first_div_flip < 0) t->stats.first_div_flip = div;
        t->r_next[port] = flip + 1u;
    }
}

/* The ordinal of this call among the port's polls since the flip count last changed. */
static uint32_t advance_poll(PadTrace* t, uint32_t port, uint32_t flip)
{
    Tracker* k = &t->trk[port];
    if (flip > t->max_flip) t->max_flip = flip;
    if (!k->any || k->cur_flip != flip) {
        if (k->any) flip_complete(t, port, k->cur_flip, k->cur_count, 0);
        k->any = 1;
        k->cur_flip = flip;
        k->cur_count = 0;
    }
    return k->cur_count++;
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
    char div[40];
    if (t->stats.first_div_flip < 0) snprintf(div, sizeof div, "none");
    else snprintf(div, sizeof div, "flip %lld", (long long)t->stats.first_div_flip);
    snprintf(b, n,
             "polls=%llu exact=%llu held=%llu recorded_changes=%llu reached_exactly=%llu first_poll_count_divergence=%s",
             (unsigned long long)t->stats.polls, (unsigned long long)t->stats.exact, (unsigned long long)t->stats.held,
             (unsigned long long)t->stats.records, (unsigned long long)t->stats.records_reached, div);
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

int pad_trace_replaying(const PadTrace* t) { return t && t->replaying; }

static void destroy_storage(PadTrace* t)
{
    for (unsigned i = 0; i < PAD_TRACE_MAX_PORTS; i++) { free(t->recs[i]); free(t->steps[i]); }
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
    t = (PadTrace*)calloc(1, sizeof *t);
    if (!t) { set_err(err, PAD_TRACE_ROLE_RECORD, 0, "io", "out of memory"); return -1; }
    trace_lock_init(&t->lock);
    t->flip_fn = o->flip;
    t->stats.first_div_flip = -1;

    if (rep) {
        t->replay_path = dup_str(o->replay_path);
        if (!t->replay_path || replay_load(t, o->replay_path, err)) goto fail;
        t->replaying = 1;
    }
    if (rec) {
        char header[96];
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
        snprintf(header, sizeof header, "{\"format\":\"" PAD_TRACE_FORMAT "\",\"version\":%u,\"ports\":%u}", PAD_TRACE_VERSION,
                 PAD_TRACE_MAX_PORTS);
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
            at_quick_exit(exit_hook);
        }
    }
    t = g_trace;
    trace_lock_release(&g_lock);
    return t;
}

void pad_trace_poll(PadTrace* t, uint32_t port, PadTraceState* st, const PadTraceState* neutral, int* fresh)
{
    uint32_t flip, poll;
    *fresh = 1;
    if (port >= PAD_TRACE_MAX_PORTS) {
        if (t->replaying) *st = *neutral;
        return;
    }
    trace_lock_acquire(&t->lock);
    flip = t->flip_fn();
    poll = advance_poll(t, port, flip);

    if (t->replaying) {
        size_t idx = 0;
        const Rec* r = NULL;
        t->stats.polls++;
        if (flip > t->end_flip) {
            if (!t->end_logged) {
                char sum[256];
                t->end_logged = 1;
                t->stats.ended = 1;
                summary(t, sum, sizeof sum);
                printf("[cellPad] pad replay: the trace ended at flip %u; holding a neutral pad. %s\n", t->end_flip, sum);
                fflush(stdout);
            }
        } else {
            r = rec_at_or_before(t, port, key_of(flip, poll), &idx);
        }
        if (r) {
            *st = r->st;
            if (r->flip == flip && r->poll == poll) {
                t->stats.exact++;
                if (!t->recs[port][idx].hit) { t->recs[port][idx].hit = 1; t->stats.records_reached++; }
            }
        } else {
            *st = *neutral;
        }
        t->stats.held = t->stats.polls - t->stats.exact;
        *fresh = poll == 0 || !t->have_served[port] || !state_equal(&t->served[port], st);
        t->served[port] = *st;
        t->have_served[port] = 1;
    }

    if (t->wf && !t->wclosed) {
        if (!t->w_have[port] || !state_equal(&t->w_last[port], st)) {
            w_state(t, port, flip, poll, st);
            t->w_last[port] = *st;
            t->w_have[port] = 1;
        }
        flush_if_due(t);
    }
    trace_lock_release(&t->lock);
}

void pad_trace_conn(PadTrace* t, uint8_t on[PAD_TRACE_MAX_PORTS])
{
    uint32_t flip;
    trace_lock_acquire(&t->lock);
    flip = t->flip_fn();
    if (flip > t->max_flip) t->max_flip = flip;
    if (t->replaying && t->nconns) {
        size_t lo = 0, hi = t->nconns;   /* first record above `flip` */
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (t->conns[mid].flip <= flip) lo = mid + 1; else hi = mid;
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
            wline(t, "{\"flip\":%u,\"conn\":%s}", flip, arr);
            memcpy(t->w_conn, on, PAD_TRACE_MAX_PORTS);
            t->w_have_conn = 1;
        }
        flush_if_due(t);
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
    char sum[256];
    int say = 0;
    if (!t) return;
    trace_lock_acquire(&t->lock);
    if (t->wf && !t->wclosed) {
        uint32_t end = t->flip_fn();
        if (end < t->max_flip) end = t->max_flip;
        for (uint32_t p = 0; p < PAD_TRACE_MAX_PORTS; p++)
            if (t->trk[p].any) flip_complete(t, p, t->trk[p].cur_flip, t->trk[p].cur_count, 1);
        wline(t, "{\"end\":true,\"flip\":%u}", end);
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

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
    uint8_t       hit;            /* a replayed poll landed on this record's own seq */
    PadTraceState st;             /* len and words exactly as the guest read them */
} Rec;

typedef struct { uint64_t info; uint8_t on[PAD_TRACE_MAX_PORTS]; } ConnRec;

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
    Rec*            recs[PAD_TRACE_MAX_PORTS];
    size_t          nrecs[PAD_TRACE_MAX_PORTS];
    size_t          caprecs[PAD_TRACE_MAX_PORTS];
    ConnRec*        conns;
    size_t          nconns, capconns;
    uint64_t        end_seq;      /* the end record's poll count */
    int             have_end;
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
    K_FORMAT = 1u << 0, K_VERSION = 1u << 1, K_CLOCK = 1u << 2, K_PORTS = 1u << 3,
    K_SEQ    = 1u << 4, K_PORT    = 1u << 5, K_LEN   = 1u << 6, K_WORDS = 1u << 7,
    K_FLIP   = 1u << 8, K_INFO    = 1u << 9, K_CONN  = 1u << 10, K_END  = 1u << 11,
};

#define MASK_HEADER (K_FORMAT | K_VERSION | K_CLOCK | K_PORTS)
#define MASK_STATE  (K_SEQ | K_PORT | K_LEN | K_WORDS | K_FLIP)
#define MASK_CONN   (K_INFO | K_CONN)
#define MASK_END    (K_END | K_SEQ)

typedef struct {
    unsigned seen;
    char     format[32];
    char     clock[32];
    uint32_t version, ports, port, len, flip;
    uint64_t seq, info;
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
        enum { T_STR, T_U32, T_U64, T_WORDS, T_CONN, T_TRUE } type = T_U32;
        uint32_t* dst = NULL;
        uint64_t* dst64 = NULL;
        if (parse_string(c, key, sizeof key)) return -1;
        if (expect_ch(c, ':')) return -1;
        if      (!strcmp(key, "format"))  { bit = K_FORMAT;  type = T_STR; }
        else if (!strcmp(key, "version")) { bit = K_VERSION; dst = &L->version; }
        else if (!strcmp(key, "clock"))   { bit = K_CLOCK;   type = T_STR; }
        else if (!strcmp(key, "ports"))   { bit = K_PORTS;   dst = &L->ports; }
        else if (!strcmp(key, "seq"))     { bit = K_SEQ;     type = T_U64; dst64 = &L->seq; }
        else if (!strcmp(key, "port"))    { bit = K_PORT;    dst = &L->port; }
        else if (!strcmp(key, "len"))     { bit = K_LEN;     dst = &L->len; }
        else if (!strcmp(key, "flip"))    { bit = K_FLIP;    dst = &L->flip; }
        else if (!strcmp(key, "info"))    { bit = K_INFO;    type = T_U64; dst64 = &L->info; }
        else if (!strcmp(key, "words"))   { bit = K_WORDS;   type = T_WORDS; }
        else if (!strcmp(key, "conn"))    { bit = K_CONN;    type = T_CONN; }
        else if (!strcmp(key, "end"))     { bit = K_END;     type = T_TRUE; }
        else PFAIL(c, "unknown-key", "key \"%s\" is not part of the format", key);
        if (L->seen & bit) PFAIL(c, "duplicate-key", "key \"%s\" appears twice", key);
        skip_ws(c);
        switch (type) {
        case T_STR:
            if (parse_string(c, bit == K_CLOCK ? L->clock : L->format,
                             bit == K_CLOCK ? sizeof L->clock : sizeof L->format))
                return -1;
            break;
        case T_U32:
            if (parse_u32(c, dst)) return -1;
            break;
        case T_U64:
            if (parse_uint(c, dst64, UINT64_MAX)) return -1;
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
            /* The version is decided before the key set: a version-1 or version-2 header has no `clock`
             * key, and the reason to refuse it is its key, not the missing clock. */
            if (!(L.seen & K_VERSION)) RFAIL(n, "header", "the header has no \"version\"");
            if (L.version == 1u || L.version == 2u)
                RFAIL(n, "version",
                      "version %u keyed its records on the flip clock, which moves between a recording and "
                      "its replay when the guest catches up wall time; re-record it as version %u",
                      L.version, PAD_TRACE_VERSION);
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
            if (L.port >= t->ports) RFAIL(n, "port-range", "port %u, but the header declares %u ports", L.port, t->ports);
            if (L.len > LEN_MAX) RFAIL(n, "value", "len %u above %u", L.len, LEN_MAX);
            if (have_seq[L.port] && L.seq <= last_seq[L.port])
                RFAIL(n, "keys-backwards", "port %u: seq %llu does not follow seq %llu", L.port,
                      (unsigned long long)L.seq, (unsigned long long)last_seq[L.port]);
            t->recs[L.port] = (Rec*)grow(t->recs[L.port], &t->caprecs[L.port], t->nrecs[L.port] + 1, sizeof(Rec));
            if (!t->recs[L.port]) RFAIL(n, "io", "out of memory");
            r = &t->recs[L.port][t->nrecs[L.port]++];
            memset(r, 0, sizeof *r);
            r->seq = L.seq; r->flip = L.flip;
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
            ended = 1;
            t->end_seq = L.seq;
            t->have_end = 1;
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
    fclose(f);
    t->stats.end_seq = t->end_seq;
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
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

int pad_trace_replaying(const PadTrace* t) { return t && t->replaying; }

static void destroy_storage(PadTrace* t)
{
    for (unsigned i = 0; i < PAD_TRACE_MAX_PORTS; i++) free(t->recs[i]);
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

    if (rep) {
        t->replay_path = dup_str(o->replay_path);
        if (!t->replay_path || replay_load(t, o->replay_path, err)) goto fail;
        t->replaying = 1;
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

    if (t->replaying) {
        size_t idx = 0;
        const Rec* r = NULL;
        t->stats.polls++;
        if (t->have_end && seq >= t->end_seq) {
            if (!t->end_logged) {
                char sum[384];
                t->end_logged = 1;
                t->stats.ended = 1;
                summary(t, sum, sizeof sum);
                printf("[cellPad] pad replay: the trace ended after %llu polls; holding a neutral pad. %s\n",
                       (unsigned long long)t->end_seq, sum);
                fflush(stdout);
            }
        } else {
            r = rec_at_or_before(t, port, seq, &idx);
        }
        if (r) {
            *st = r->st;
            if (r->seq == seq) {
                t->stats.exact++;
                if (!t->recs[port][idx].hit) { t->recs[port][idx].hit = 1; t->stats.records_reached++; }
                /* The flip is annotation: how far it moved between the recording and this replay is the
                 * guest-time drift the ordinal key absorbs, and what the summary reports. */
                const int64_t d = (int64_t)flip - (int64_t)r->flip;
                if (!t->stats.have_drift) {
                    t->stats.have_drift = 1;
                    t->stats.drift_min = t->stats.drift_max = d;
                } else {
                    if (d < t->stats.drift_min) t->stats.drift_min = d;
                    if (d > t->stats.drift_max) t->stats.drift_max = d;
                }
            }
        } else {
            *st = *neutral;
        }
        t->stats.held = t->stats.polls - t->stats.exact;
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
        /* The end marker is port 0's poll count: the recording's length in the only unit the replay has. */
        wline(t, "{\"end\":true,\"seq\":%llu}", (unsigned long long)t->seq[0]);
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

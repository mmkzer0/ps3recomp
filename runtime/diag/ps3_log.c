/*
 * ps3recomp - tagged, deduplicated, rotated run log (contract: docs/LOGGING.md,
 * API: include/ps3emu/ps3_log.h)
 *
 * stderr gets exactly the bytes the fprintf it replaced printed. The file gets
 * the same line behind `<seconds> <thread> <level> `, plus what only a file can
 * hold: collapsed repeats, what a per-site cap swallowed, the stop banner.
 *
 * Locking: formatting and the thread label happen outside the lock. One
 * process-wide lock covers every write (stderr, then the file) and all the state
 * below it, so lines from different threads never interleave and the two sinks
 * see the same order. Nothing in here calls ps3_stop while holding it: a stop
 * mirrors its banner back through ps3_log_mirror, which would deadlock.
 *
 * Shutdown: the process ends under live threads (ps3_host_exit is quick_exit; a
 * stop is _Exit), so the exit summary has to account for work that is in
 * flight. Every logging operation holds the gate from admission to the end of
 * its output. The closer (exit or stop) shuts the gate, so no new operation is
 * admitted, waits a bounded time for those already inside, and only then takes
 * the summary and the final flush. A line that arrives once the gate is shut is
 * dropped from both sinks: it raced the end of the process.
 *
 * Link: config.cpp calls ps3_log_configure, so a title, rsx_replay and any
 * fixture that links config.cpp links this file too. stop.cpp knows the log only
 * through the mirror hook ps3_stop_set_mirror, set at configure: a fixture that
 * has stop.cpp and no config needs nothing.
 *
 * Atomics are the __atomic builtins, as in runtime/platform/ps3_stdio.c.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#include "ps3emu/ps3_log.h"
#include "ps3emu/config.h"
#include "ps3emu/host_exit.h"
#include "ps3emu/stop.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <pthread.h>
#  include <sched.h>
#  include <sys/stat.h>
#  include <time.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/prctl.h>
#    include <sys/syscall.h>
#  endif
#endif

#ifndef EISDIR
#  define EISDIR 21   /* MSVC's errno.h has no EISDIR; the POSIX value, so stop lines agree across hosts */
#endif

#define LOG_STACK_BYTES   1024u
#define LOG_PRE_BYTES     (64u * 1024u)
#define LOG_FILE_BUF      (64u * 1024u)
#define LOG_PATH_MAX      1024u   /* host buffer for the path and its ".prev" twin */
#define LOG_LABEL_MAX     48u
#define LOG_MUTE_MAX      256u    /* log.mute is at most 255 characters */
#define LOG_FLUSH_MS      50u     /* ps3_stdio's default interval: a killed run loses at most this much */
#define LOG_DRAIN_NS      250000000ull   /* how long a closer waits for operations already admitted */
#define LOG_FLUSHER_NS    250000000ull   /* ...then for the flusher to leave */
#define LOG_LOCK_NS       250000000ull   /* ...then for the logger lock: a shutdown never waits unboundedly */

enum { COLLAPSE_OFF = 0, COLLAPSE_FILE = 1, COLLAPSE_ALL = 2 };

/* ---- lock ---------------------------------------------------------------- */

#ifdef _WIN32
static SRWLOCK g_lock = SRWLOCK_INIT;
static void lock(void)   { AcquireSRWLockExclusive(&g_lock); }
static void unlock(void) { ReleaseSRWLockExclusive(&g_lock); }
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void)   { pthread_mutex_lock(&g_lock); }
static void unlock(void) { pthread_mutex_unlock(&g_lock); }
#endif

static int try_lock(void)
{
#ifdef _WIN32
    return TryAcquireSRWLockExclusive(&g_lock) != 0;
#else
    return pthread_mutex_trylock(&g_lock) == 0;
#endif
}

/* ---- state --------------------------------------------------------------- */

/* Read without the lock. */
static int      g_level = PS3_LOG_INFO;
static int      g_label_needed = 1;      /* a file, or the pre-configure buffer, will take the line */
static int      g_claimed;               /* ps3_log_configure: first caller wins */
static ps3_log_site* g_sites;            /* head of the registry of limited sites that swallowed lines */
static unsigned g_gate;                  /* bit 0: shut; the rest: 2 per operation in flight */
static int      g_shut;                  /* the drain is over: an operation that reaches output now is dropped */
static uint64_t g_close_ns;              /* when the first closer began; every shutdown deadline hangs off it */
static int      g_lock_timeout_noted;    /* the "lock held past the deadline" notice goes out once */
static int      g_flusher_stop, g_flusher_exited, g_flusher_started;
static ps3_log_test_hook_fn g_test_hook; /* ordering seam for tests (ps3_log.h) */

/* Under g_lock. */
static int      g_configured;
static FILE*    g_file;
static char     g_file_buf[LOG_FILE_BUF];
static int      g_flush_each;            /* no flusher thread could be started: flush every line */
static uint64_t g_bytes;                 /* written to the file so far */
static uint64_t g_cap;                   /* log.max_mb in bytes */
static int      g_capped;
static uint64_t g_cap_dropped;           /* lines the cap kept out of the file */
static int      g_cap_reported;
static int      g_summary_done;
static int      g_collapse;              /* effective mode: a file-only collapse with no file is off */
static char     g_mute[LOG_MUTE_MAX];
static uint64_t g_t0_ns;

static char     g_pre[LOG_PRE_BYTES];    /* lines before configure: written to the file once it opens */
static size_t   g_pre_len;
static uint64_t g_pre_dropped;

static const ps3_log_site* g_last_site;  /* the last line written, for collapsing */
static char*    g_last;
static size_t   g_last_len, g_last_cap;
static int      g_last_muted;
static unsigned g_repeat;                /* identical lines since g_last was written */

static ps3_log_site g_internal = { "log", __FILE__, __LINE__, 0u, 0u, 0u, NULL, 0 };

/* ---- clock and thread ---------------------------------------------------- */

static uint64_t now_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    const uint64_t freq = (uint64_t)f.QuadPart, ticks = (uint64_t)c.QuadPart;
    return (ticks / freq) * 1000000000ull + (ticks % freq) * 1000000000ull / freq;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void yield_cpu(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

/* Seconds since the first line. Under g_lock, so timestamps never run backwards
 * through the file. */
static double stamp_locked(void)
{
    const uint64_t t = now_ns();
    if (!g_t0_ns) g_t0_ns = t;
    return (double)(t - g_t0_ns) / 1e9;
}

static unsigned long long numeric_thread_id(void)
{
#if defined(_WIN32)
    return GetCurrentThreadId();
#elif defined(__APPLE__)
    uint64_t t = 0;
    pthread_threadid_np(NULL, &t);
    return t;
#elif defined(__linux__)
    return (unsigned long long)syscall(SYS_gettid);
#else
    return (unsigned long long)(uintptr_t)pthread_self();
#endif
}

/* The host thread's name when it has one, else its numeric id. A name never
 * holds whitespace, so the file's columns stay splittable. */
static void thread_label(char out[LOG_LABEL_MAX])
{
    out[0] = '\0';
#if defined(_WIN32)
    /* GetThreadDescription is Windows 10 1607 and later: looked up, not linked. */
    typedef HRESULT (WINAPI *get_desc_fn)(HANDLE, PWSTR*);
    static get_desc_fn fn;
    static int looked_up;
    if (!__atomic_load_n(&looked_up, __ATOMIC_ACQUIRE)) {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        __atomic_store_n(&fn, k ? (get_desc_fn)(void*)GetProcAddress(k, "GetThreadDescription") : NULL,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&looked_up, 1, __ATOMIC_RELEASE);
    }
    get_desc_fn f = __atomic_load_n(&fn, __ATOMIC_RELAXED);
    if (f) {
        PWSTR w = NULL;
        if (SUCCEEDED(f(GetCurrentThread(), &w)) && w) {
            if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)LOG_LABEL_MAX, NULL, NULL) == 0)
                out[0] = '\0';
            LocalFree(w);
        }
    }
#elif defined(__linux__)
    /* pthread_getname_np reads /proc on glibc; PR_GET_NAME is the same name for one syscall. */
    if (prctl(PR_GET_NAME, (unsigned long)out, 0, 0, 0) != 0) out[0] = '\0';
#else
    if (pthread_getname_np(pthread_self(), out, LOG_LABEL_MAX) != 0) out[0] = '\0';
#endif
    out[LOG_LABEL_MAX - 1] = '\0';
    if (!out[0]) {
        snprintf(out, LOG_LABEL_MAX, "%llu", numeric_thread_id());
        return;
    }
    for (char* p = out; *p; ++p)
        if ((unsigned char)*p <= 0x20 || *p == 0x7f) *p = '_';
}

/* ---- the gate ------------------------------------------------------------ */

/* An operation is in flight from its admission to the end of its output: the
 * window in which it may still register a site, count a suppression or write. */
static int gate_enter(void)
{
    const unsigned v = __atomic_fetch_add(&g_gate, 2u, __ATOMIC_ACQ_REL);
    if (v & 1u) {
        __atomic_fetch_sub(&g_gate, 2u, __ATOMIC_ACQ_REL);
        return 0;
    }
    return 1;
}

static void gate_leave(void)
{
    __atomic_fetch_sub(&g_gate, 2u, __ATOMIC_ACQ_REL);
}

static void flusher_stop_and_wait(uint64_t deadline_ns);   /* below */

static uint64_t close_ns(void)
{
    return __atomic_load_n(&g_close_ns, __ATOMIC_ACQUIRE);
}

/* Shut the gate and wait for the operations already inside. Bounded: one that
 * never returns (an argument that blocks) must not hold up the end of the process.
 * Once the drain is over g_shut cancels what is left: an operation admitted before
 * the gate shut that reaches output after this point is dropped, under the output
 * lock (output_line), from both sinks. One already writing holds that lock, so it
 * finishes before shutdown_lock gets it. */
static void gate_close_and_drain(void)
{
    uint64_t none = 0;
    __atomic_compare_exchange_n(&g_close_ns, &none, now_ns(), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    __atomic_fetch_or(&g_gate, 1u, __ATOMIC_ACQ_REL);
    const uint64_t deadline = close_ns() + LOG_DRAIN_NS;
    while ((__atomic_load_n(&g_gate, __ATOMIC_ACQUIRE) >> 1) != 0 && now_ns() < deadline) yield_cpu();
    __atomic_store_n(&g_shut, 1, __ATOMIC_RELEASE);
}

static int lock_until(uint64_t deadline_ns)
{
    for (;;) {
        if (try_lock()) return 1;
        if (now_ns() >= deadline_ns) return 0;
        yield_cpu();
    }
}

/* A raw write: stderr's own stream lock may be the thing that is stuck. */
static void note_lock_timeout(void)
{
    if (__atomic_exchange_n(&g_lock_timeout_noted, 1, __ATOMIC_ACQ_REL)) return;
    static const char msg[] = "[log] the log lock was still held at the shutdown deadline; "
                              "the run log's final summary and flush were skipped\n";
#ifdef _WIN32
    _write(2, msg, (unsigned)(sizeof msg - 1));
#else
    const ssize_t r = write(2, msg, sizeof msg - 1);
    (void)r;
#endif
}

/* The first half of every ending, shared by exit and stop: refuse new operations, let the admitted ones finish,
 * cancel the stragglers, stop the flusher. All bounded. */
static void shutdown_quiesce(void)
{
    gate_close_and_drain();
    flusher_stop_and_wait(close_ns() + LOG_DRAIN_NS + LOG_FLUSHER_NS);
}

/* The second half: 1 with g_lock held, or 0 (after one notice on stderr) when it was not free by the deadline:
 * the final file-only work is skipped and the process ends anyway. */
static int shutdown_lock(void)
{
    if (lock_until(close_ns() + LOG_DRAIN_NS + LOG_FLUSHER_NS + LOG_LOCK_NS)) return 1;
    note_lock_timeout();
    return 0;
}

/* ---- registry ------------------------------------------------------------ */

static void register_site(ps3_log_site* s)
{
    if (__atomic_load_n(&s->registered, __ATOMIC_RELAXED)) return;
    if (__atomic_exchange_n(&s->registered, 1, __ATOMIC_ACQ_REL)) return;
    ps3_log_site* head = __atomic_load_n(&g_sites, __ATOMIC_RELAXED);
    do {
        s->next = head;
    } while (!__atomic_compare_exchange_n(&g_sites, &head, s, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

static void count_suppressed(ps3_log_site* s)
{
    register_site(s);
    unsigned n = __atomic_load_n(&s->suppressed, __ATOMIC_RELAXED);
    while (n != UINT_MAX &&
           !__atomic_compare_exchange_n(&s->suppressed, &n, n + 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

/* PS3_LOG_FIRST's contract with atomics: exactly `limit` lines get through, and
 * `emitted` stops there instead of counting on, so it cannot wrap. The slot is
 * taken here, before the caller evaluates its arguments, so a call that loses the
 * race never evaluates them. */
static int reserve(ps3_log_site* s)
{
    if (!s->limit) return 1;
    unsigned n = __atomic_load_n(&s->emitted, __ATOMIC_RELAXED);
    for (;;) {
        if (n >= s->limit) {
            count_suppressed(s);
            return 0;
        }
        if (__atomic_compare_exchange_n(&s->emitted, &n, n + 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return 1;
    }
}

/* ---- sinks (all under g_lock) -------------------------------------------- */

static int tag_muted(const char* tag)
{
    if (!g_mute[0] || !tag || !*tag) return 0;
    const size_t tl = strlen(tag);
    for (const char* p = g_mute; *p;) {
        const char* end = strchr(p, ',');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == tl && memcmp(p, tag, tl) == 0) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

static void stderr_put(const ps3_log_site* site, const char* line, size_t n)
{
    if (tag_muted(site->tag)) return;
    fwrite(line, 1, n, stderr);
}

/* One line of the file: prefix, message, and a newline when the message lacks one
 * (a partial line would otherwise run into the next). `force` writes past the cap:
 * the cap's own report. */
static void file_line_locked(char lvl, double ts, const char* label, const char* msg, size_t n, int force)
{
    char pfx[96];
    const int pl = snprintf(pfx, sizeof pfx, "%.6f %s %c ", ts, label ? label : "?", lvl);
    const size_t plen = pl > 0 ? (size_t)pl : 0;
    const int need_nl = !(n && msg[n - 1] == '\n');
    const size_t total = plen + n + (need_nl ? 1u : 0u);

    if (!g_configured) {
        if (g_pre_len + total > LOG_PRE_BYTES) { g_pre_dropped++; return; }
        memcpy(g_pre + g_pre_len, pfx, plen);
        memcpy(g_pre + g_pre_len + plen, msg, n);
        if (need_nl) g_pre[g_pre_len + plen + n] = '\n';
        g_pre_len += total;
        return;
    }
    if (!g_file) return;
    if (!force) {
        if (g_capped || g_bytes + total > g_cap) {
            g_capped = 1;
            g_cap_dropped++;
            return;
        }
    }
    fwrite(pfx, 1, plen, g_file);
    fwrite(msg, 1, n, g_file);
    if (need_nl) fputc('\n', g_file);
    g_bytes += total;
    if (g_flush_each) fflush(g_file);
}

static void file_line_text_locked(char lvl, double ts, const char* label, const char* text)
{
    file_line_locked(lvl, ts, label, text, strlen(text), 0);
}

static void flush_repeats_locked(double ts, const char* label)
{
    if (!g_repeat) return;
    char text[96];
    snprintf(text, sizeof text, "[log] previous line repeated %u times\n", g_repeat);
    if (g_collapse == COLLAPSE_ALL && !g_last_muted) fputs(text, stderr);
    file_line_text_locked('I', ts, label, text);
    g_repeat = 0;
}

static void remember_locked(const ps3_log_site* site, const char* line, size_t n)
{
    if (n > g_last_cap) {
        char* p = (char*)realloc(g_last, n);
        if (!p) { g_last_site = NULL; return; }   /* cannot compare: this line just is not collapsible */
        g_last = p;
        g_last_cap = n;
    }
    memcpy(g_last, line, n);
    g_last_site = site;
    g_last_len = n;
    g_last_muted = tag_muted(site->tag);
}

static void output_line(const ps3_log_site* site, ps3_log_level lvl, const char* line, size_t n,
                        const char* label)
{
    lock();
    if (__atomic_load_n(&g_shut, __ATOMIC_ACQUIRE)) {   /* admitted before shutdown, too late for it: neither sink */
        unlock();
        return;
    }
    const double ts = stamp_locked();
    if (g_collapse != COLLAPSE_OFF && g_last_site == site && g_last_len == n &&
        memcmp(g_last, line, n) == 0) {
        g_repeat++;
        if (g_collapse == COLLAPSE_FILE) stderr_put(site, line, n);
        unlock();
        return;
    }
    flush_repeats_locked(ts, label);
    if (g_collapse != COLLAPSE_OFF) remember_locked(site, line, n);
    stderr_put(site, line, n);
    file_line_locked("EWID"[lvl > PS3_LOG_DEBUG ? PS3_LOG_DEBUG : lvl], ts, label, line, n, 0);
    unlock();
}

/* A runtime diagnostic about the log itself, through the normal path. */
static void internal_line(const char* text, const char* label)
{
    output_line(&g_internal, PS3_LOG_WARN, text, strlen(text), label);
}

static void summary_locked(double ts, const char* label)
{
    if (g_summary_done) return;
    g_summary_done = 1;
    if (!g_file || g_capped) return;   /* at the cap the file gets its one report and nothing else */
    for (ps3_log_site* s = __atomic_load_n(&g_sites, __ATOMIC_ACQUIRE); s; s = s->next) {
        const unsigned dropped = __atomic_load_n(&s->suppressed, __ATOMIC_RELAXED);
        if (!dropped) continue;
        char text[1024];
        snprintf(text, sizeof text, "[log] suppressed %u lines after the first %u at %s:%d [%s]\n", dropped,
                 s->limit, s->file ? s->file : "?", s->line, s->tag ? s->tag : "");
        file_line_text_locked('I', ts, label, text);
    }
}

static void cap_report_locked(double ts, const char* label)
{
    if (!g_file || !g_capped || g_cap_reported) return;
    g_cap_reported = 1;
    char text[96];
    snprintf(text, sizeof text, "[log] size cap reached, %llu further lines not written\n",
             (unsigned long long)g_cap_dropped);
    file_line_locked('I', ts, label, text, strlen(text), 1);
}

/* ---- host file: rotation and open ---------------------------------------- */

static void stop_log_file(const char* op, int error, size_t path_len)
{
    ps3_stop_host_log_file(op, error, path_len);
}

static void stop_path_limit(size_t need)
{
    char need_s[24], limit_s[24];
    snprintf(need_s, sizeof need_s, "%zu", need);
    snprintf(limit_s, sizeof limit_s, "%u", LOG_PATH_MAX - 1u);
    const ps3_stop_field f[] = { {"func", "ps3_log_configure"}, {"need", need_s}, {"limit", limit_s} };
    ps3_stop(PS3_STOP_HOST_PATH_LIMIT, "the log file path (or its .prev twin) does not fit its buffer",
             "shorten log.file", f, 3, NULL);
}

/* The existing file becomes <path>.prev (replacing an older one), then a fresh
 * file is created. A path that exists but is not a regular file is refused, not
 * rotated: renaming a directory aside would be the worst reading of "log.file". */
static FILE* rotate_and_open(const char* path)
{
    const size_t len = strlen(path);
    if (len + 5u >= LOG_PATH_MAX) stop_path_limit(len + 5u);
    char prev[LOG_PATH_MAX];
    memcpy(prev, path, len);
    memcpy(prev + len, ".prev", 6);

#ifdef _WIN32
    wchar_t wpath[LOG_PATH_MAX], wprev[LOG_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, (int)LOG_PATH_MAX) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, prev, -1, wprev, (int)LOG_PATH_MAX))
        stop_log_file("open", (int)GetLastError(), len);
    const DWORD attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) stop_log_file("open", (int)e, len);
    } else {
        if (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE))
            stop_log_file("open", EISDIR, len);
        if (!MoveFileExW(wpath, wprev, MOVEFILE_REPLACE_EXISTING)) {
            const DWORD e = GetLastError();
            if (e != ERROR_FILE_NOT_FOUND) stop_log_file("rename", (int)e, len);
        }
    }
    FILE* f = _wfopen(wpath, L"wb");
#else
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) stop_log_file("open", S_ISDIR(st.st_mode) ? EISDIR : EINVAL, len);
        if (rename(path, prev) != 0 && errno != ENOENT) stop_log_file("rename", errno, len);
    } else if (errno != ENOENT) {
        stop_log_file("open", errno, len);
    }
    FILE* f = fopen(path, "wb");
#endif
    if (!f) stop_log_file("open", errno, len);
    setvbuf(f, g_file_buf, _IOFBF, sizeof g_file_buf);
    return f;
}

static int parse_level(const char* s)
{
    if (!strcmp(s, "error")) return PS3_LOG_ERROR;
    if (!strcmp(s, "warn"))  return PS3_LOG_WARN;
    if (!strcmp(s, "debug")) return PS3_LOG_DEBUG;
    return PS3_LOG_INFO;
}

/* ---- flusher ------------------------------------------------------------- */

/* The file is fully buffered, and a host loads its config (which opens the file)
 * long before ppu_run() starts ps3_stdio's flusher and crash hooks. A run that
 * dies in between would lose the whole buffer. ps3_stdio_init is not an option
 * from here: it is first-call-wins, it also takes over stdout and stderr, and its
 * thread and win32_compat dependencies would reach every fixture that links
 * config.cpp. So the log owns a flusher of its own, started when it opens a file.
 * It bounds the loss of a killed or crashed run to LOG_FLUSH_MS, whatever
 * ps3_stdio does later (PS3_STDIO_UNBUFFERED=1 included).
 *
 * It must not outlive the final flush: normal exit closes the stream after the
 * handlers, and a flush then would hit a closing FILE. The closer signals it and
 * waits (bounded) for it to leave; a tick re-checks the signal under the output
 * lock, so even a flusher that is still running cannot flush after the final
 * flush, which is taken under the same lock after the signal. */
static void test_hook(int phase)
{
    ps3_log_test_hook_fn h = __atomic_load_n(&g_test_hook, __ATOMIC_ACQUIRE);
    if (h) h(phase);
}

/* One wake of the flusher: 0 when it has been told to stop. */
static int flusher_tick(void)
{
    test_hook(PS3_LOG_TEST_FLUSHER_TICK);
    if (__atomic_load_n(&g_flusher_stop, __ATOMIC_ACQUIRE)) return 0;
    lock();
    if (__atomic_load_n(&g_flusher_stop, __ATOMIC_ACQUIRE)) {
        unlock();
        return 0;
    }
    test_hook(PS3_LOG_TEST_FLUSHER_FLUSH);
    if (g_file) fflush(g_file);
    unlock();
    return 1;
}

#ifdef _WIN32
static HANDLE g_fl_thread, g_fl_event;

static DWORD WINAPI flusher_main(LPVOID arg)
{
    (void)arg;
    while (WaitForSingleObject(g_fl_event, LOG_FLUSH_MS) == WAIT_TIMEOUT)
        if (!flusher_tick()) break;
    test_hook(PS3_LOG_TEST_FLUSHER_EXIT);
    __atomic_store_n(&g_flusher_exited, 1, __ATOMIC_RELEASE);
    return 0;
}

static int start_flusher(void)
{
    g_fl_event = CreateEventW(NULL, TRUE, FALSE, NULL);   /* manual reset: the stop signal stays set */
    if (!g_fl_event) return 0;
    g_fl_thread = CreateThread(NULL, 0, flusher_main, NULL, 0, NULL);
    if (!g_fl_thread) {
        CloseHandle(g_fl_event);
        g_fl_event = NULL;
        return 0;
    }
    __atomic_store_n(&g_flusher_started, 1, __ATOMIC_RELEASE);
    return 1;
}

static void flusher_stop_and_wait(uint64_t deadline_ns)
{
    if (!__atomic_load_n(&g_flusher_started, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&g_flusher_stop, 1, __ATOMIC_RELEASE);
    SetEvent(g_fl_event);
    const uint64_t now = now_ns();
    const DWORD ms = now >= deadline_ns ? 0 : (DWORD)((deadline_ns - now) / 1000000ull);
    WaitForSingleObject(g_fl_thread, ms);   /* the handle is kept for this: a closed one cannot be waited on */
}
#else
static pthread_mutex_t g_fl_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_fl_cv = PTHREAD_COND_INITIALIZER;

/* pthread_cond_timedwait takes an absolute CLOCK_REALTIME time (macOS has no monotonic condition clock). */
static void realtime_after(struct timespec* ts, uint64_t ns)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += (time_t)(ns / 1000000000ull);
    ts->tv_nsec += (long)(ns % 1000000000ull);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static void* flusher_main(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&g_fl_mu);
    while (!__atomic_load_n(&g_flusher_stop, __ATOMIC_ACQUIRE)) {
        struct timespec ts;
        realtime_after(&ts, (uint64_t)LOG_FLUSH_MS * 1000000ull);
        pthread_cond_timedwait(&g_fl_cv, &g_fl_mu, &ts);
        if (__atomic_load_n(&g_flusher_stop, __ATOMIC_ACQUIRE)) break;
        pthread_mutex_unlock(&g_fl_mu);
        const int go = flusher_tick();
        pthread_mutex_lock(&g_fl_mu);
        if (!go) break;
    }
    pthread_mutex_unlock(&g_fl_mu);
    test_hook(PS3_LOG_TEST_FLUSHER_EXIT);
    pthread_mutex_lock(&g_fl_mu);
    __atomic_store_n(&g_flusher_exited, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_fl_cv);
    pthread_mutex_unlock(&g_fl_mu);
    return NULL;
}

static int start_flusher(void)
{
    pthread_attr_t attr;
    pthread_t t;
    if (pthread_attr_init(&attr) != 0) return 0;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    const int rc = pthread_create(&t, &attr, flusher_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) return 0;
    __atomic_store_n(&g_flusher_started, 1, __ATOMIC_RELEASE);
    return 1;
}

/* Detached, so the wait is on a flag the flusher sets as its last act that touches shared state. */
static void flusher_stop_and_wait(uint64_t deadline_ns)
{
    if (!__atomic_load_n(&g_flusher_started, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&g_fl_mu);
    __atomic_store_n(&g_flusher_stop, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_fl_cv);
    while (!__atomic_load_n(&g_flusher_exited, __ATOMIC_ACQUIRE)) {
        const uint64_t now = now_ns();
        if (now >= deadline_ns) break;
        struct timespec ts;
        realtime_after(&ts, deadline_ns - now);
        pthread_cond_timedwait(&g_fl_cv, &g_fl_mu, &ts);
    }
    pthread_mutex_unlock(&g_fl_mu);
}
#endif

/* ---- exit ---------------------------------------------------------------- */

/* Repeats and the suppression summary: the start of the file's last words. g_lock held. */
static double shutdown_begin_locked(const char* label)
{
    const double ts = stamp_locked();
    flush_repeats_locked(ts, label);
    summary_locked(ts, label);
    return ts;
}

static void shutdown_end_locked(double ts, const char* label)
{
    cap_report_locked(ts, label);
    if (g_file) fflush(g_file);
    fflush(stderr);   /* a collapsed run's last count, in `all` mode, is written to the buffered stream */
    unlock();
}

static void at_exit(void)
{
    char label[LOG_LABEL_MAX];
    thread_label(label);
    shutdown_quiesce();
    if (!shutdown_lock()) return;
    const double ts = shutdown_begin_locked(label);
    shutdown_end_locked(ts, label);
}

/* ---- public -------------------------------------------------------------- */

void ps3_log_configure(void)
{
    if (__atomic_exchange_n(&g_claimed, 1, __ATOMIC_ACQ_REL)) return;

    const ps3_config* c = ps3_config_get();
    FILE* f = c->log_file[0] ? rotate_and_open(c->log_file) : NULL;   /* may stop: no lock is held */

    char label[LOG_LABEL_MAX];
    thread_label(label);
    lock();
    const double ts = stamp_locked();
    __atomic_store_n(&g_level, parse_level(c->log_level), __ATOMIC_RELAXED);
    memcpy(g_mute, c->log_mute, sizeof g_mute < sizeof c->log_mute ? sizeof g_mute : sizeof c->log_mute);
    g_mute[LOG_MUTE_MAX - 1] = '\0';
    g_cap = (uint64_t)c->log_max_mb << 20;
    if (!strcmp(c->log_collapse, "all"))       g_collapse = COLLAPSE_ALL;
    else if (!strcmp(c->log_collapse, "file")) g_collapse = f ? COLLAPSE_FILE : COLLAPSE_OFF;
    else                                       g_collapse = COLLAPSE_OFF;

    g_file = f;
    g_configured = 1;
    if (f) {
        fwrite(g_pre, 1, g_pre_len, f);
        g_bytes += g_pre_len;
        if (g_pre_dropped) {
            char text[160];
            snprintf(text, sizeof text,
                     "[log] %llu lines printed before the log was configured did not fit the %u KiB "
                     "buffer and are not in this file\n",
                     (unsigned long long)g_pre_dropped, LOG_PRE_BYTES / 1024u);
            file_line_locked('W', ts, label, text, strlen(text), 1);
        }
        if (!start_flusher()) g_flush_each = 1;   /* no thread: flush every line rather than lose the buffer */
        fflush(f);
    }
    g_pre_len = 0;
    g_pre_dropped = 0;
    __atomic_store_n(&g_label_needed, f != NULL, __ATOMIC_RELAXED);
    unlock();

    /* A stop quiesces the log before its banner and mirrors the banner after it; stop.cpp does not link the log. */
    ps3_stop_set_mirror(ps3_log_quiesce, ps3_log_mirror);
    /* ps3_host_exit ends every guest exit with quick_exit, which runs these and not the atexit ones. */
    atexit(at_exit);
    ps3_host_at_quick_exit(at_exit);
}

/* A spent site is the common case on a hot path (the old PS3_LOG_FIRST was a thread-local increment there):
 * one relaxed load of `emitted`, which stops at the limit, then one relaxed fetch_add on the site's count.
 * No gate, no reservation. A call that sees a free slot goes the slow way and may still lose the race for
 * it; the CAS in reserve() keeps the limit exact. A suppression counted here after the closer's snapshot is a
 * line that raced the end of the process, like any other. */
int ps3_log_site_admit(ps3_log_site* site, ps3_log_level lvl)
{
    if ((int)lvl > __atomic_load_n(&g_level, __ATOMIC_RELAXED)) return 0;
    if (site->limit && __atomic_load_n(&site->emitted, __ATOMIC_RELAXED) >= site->limit) {
        register_site(site);
        if (__atomic_fetch_add(&site->suppressed, 1u, __ATOMIC_RELAXED) == UINT_MAX)
            __atomic_store_n(&site->suppressed, UINT_MAX, __ATOMIC_RELAXED);   /* saturate, never wrap */
        return 0;
    }
    if (!gate_enter()) return 0;
    if (!reserve(site)) {
        gate_leave();
        return 0;
    }
    return 1;
}

static void emit_va(ps3_log_site* site, ps3_log_level lvl, const char* fmt, va_list ap)
{
    char stack[LOG_STACK_BYTES];
    char* buf = stack;
    const size_t tl = (site->tag && *site->tag) ? strlen(site->tag) : 0;
    const size_t plen = tl ? tl + 3u : 0u;   /* "[tag] " */
    if (tl && plen < sizeof stack) {
        stack[0] = '[';
        memcpy(stack + 1, site->tag, tl);
        stack[tl + 1] = ']';
        stack[tl + 2] = ' ';
    }

    va_list ap2;
    va_copy(ap2, ap);
    const int r = vsnprintf(plen < sizeof stack ? stack + plen : NULL,
                            plen < sizeof stack ? sizeof stack - plen : 0, fmt, ap);

    char label[LOG_LABEL_MAX] = "?";
    if (__atomic_load_n(&g_label_needed, __ATOMIC_RELAXED)) thread_label(label);

    if (r < 0) {
        va_end(ap2);
        char text[192];
        snprintf(text, sizeof text, "[log] a line at %s:%d could not be formatted and was not printed\n",
                 site->file ? site->file : "?", site->line);
        internal_line(text, label);
        return;
    }
    const size_t total = plen + (size_t)r;
    if (total >= sizeof stack) {
        /* Longer than the stack buffer: formatted again into a heap buffer of the exact size. */
        buf = (char*)malloc(total + 1u);
        if (!buf) {
            va_end(ap2);
            char text[192];
            snprintf(text, sizeof text, "[log] a line of %zu bytes at %s:%d was not printed: out of memory\n",
                     total, site->file ? site->file : "?", site->line);
            internal_line(text, label);
            return;
        }
        if (tl) {
            buf[0] = '[';
            memcpy(buf + 1, site->tag, tl);
            buf[tl + 1] = ']';
            buf[tl + 2] = ' ';
        }
        vsnprintf(buf + plen, (size_t)r + 1u, fmt, ap2);
    }
    va_end(ap2);

    output_line(site, lvl, buf, total, label);
    if (buf != stack) free(buf);
}

void ps3_log_emit_admitted(ps3_log_site* site, ps3_log_level lvl, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit_va(site, lvl, fmt, ap);
    va_end(ap);
    gate_leave();
}

void ps3_log_emit(ps3_log_site* site, ps3_log_level lvl, const char* fmt, ...)
{
    if (!ps3_log_site_admit(site, lvl)) return;
    va_list ap;
    va_start(ap, fmt);
    emit_va(site, lvl, fmt, ap);
    va_end(ap);
    gate_leave();
}

void ps3_log_quiesce(void)
{
    shutdown_quiesce();
    /* Barrier: a write that was already running finishes before the caller goes on (a stop writes its
     * banner next, and it stays the last thing on stderr). */
    if (shutdown_lock()) unlock();
}

void ps3_log_set_test_hook(ps3_log_test_hook_fn fn)
{
    __atomic_store_n(&g_test_hook, fn, __ATOMIC_RELEASE);
}

void ps3_log_mirror(const char* bytes, size_t n)
{
    char label[LOG_LABEL_MAX];
    thread_label(label);
    shutdown_quiesce();
    if (!shutdown_lock()) return;
    const double ts = shutdown_begin_locked(label);

    if (!g_configured) {
        if (g_pre_len + n <= LOG_PRE_BYTES) { memcpy(g_pre + g_pre_len, bytes, n); g_pre_len += n; }
        else g_pre_dropped++;
    } else if (g_file) {
        if (g_capped || g_bytes + n > g_cap) {
            g_capped = 1;
            g_cap_dropped++;
        } else {
            fwrite(bytes, 1, n, g_file);
            g_bytes += n;
        }
    }
    shutdown_end_locked(ts, label);
}

void ps3_log_flush(void)
{
    lock();
    if (g_file) fflush(g_file);
    unlock();
}

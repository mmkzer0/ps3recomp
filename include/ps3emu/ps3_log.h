/*
 * ps3recomp - tagged, deduplicated, rotated run log (contract: docs/LOGGING.md)
 *
 * A call site prints `[tag] text` on stderr exactly as the fprintf it replaces
 * did; the same line also goes to the run's log file (config `log.file`) with a
 * timestamp, thread and level. Only the file carries anything new: stderr bytes
 * stay what tools and tests grep.
 *
 * Not to be confused with runtime/ps3_log.h (the PS3_VERBOSE gate for chatty
 * per-wait logging), which is a different header with a different job.
 *
 * Atomics are the __atomic builtins, as in runtime/platform/win32_compat.h: this
 * header is included from C and C++ and both must compile it.
 */
#ifndef PS3RECOMP_PS3_LOG_H
#define PS3RECOMP_PS3_LOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PS3_LOG_ERROR = 0, PS3_LOG_WARN, PS3_LOG_INFO, PS3_LOG_DEBUG } ps3_log_level;

/* One per call site: static storage, constant-initialised by the macros below
 * (PS3_LOG, PS3_LOG_N), never by hand. */
typedef struct ps3_log_site {
    const char* tag;      /* printed as "[tag] "; "" prints no prefix */
    const char* file;
    int         line;
    unsigned    limit;       /* 0: unlimited; else lines printed before suppression */
    unsigned    emitted;     /* __atomic: lines let through; stops at limit, so it cannot wrap */
    unsigned    suppressed;  /* __atomic: lines swallowed after the limit (saturates) */
    struct ps3_log_site* next;   /* lock-free registry, read by the exit summary */
    int         registered;
} ps3_log_site;

#if defined(__GNUC__) || defined(__clang__)
#  define PS3_LOG_PRINTF_CHECK __attribute__((format(printf, 3, 4)))
#else
#  define PS3_LOG_PRINTF_CHECK
#endif

/* Level filter, the site's limit and the shutdown gate, then format and write stderr and the file under
 * one lock. `fmt` is the old fprintf format after the "[tag] ". */
void ps3_log_emit(ps3_log_site* site, ps3_log_level lvl, const char* fmt, ...) PS3_LOG_PRINTF_CHECK;

/* The two halves of ps3_log_emit, for the macros: admit takes the site's slot and enters the shutdown gate
 * before the caller evaluates its arguments, so a call that loses the race for the last slot never
 * evaluates them (PS3_LOG_FIRST's guard never did). A non-zero result obliges the caller to call
 * ps3_log_emit_admitted exactly once, which leaves the gate. */
int  ps3_log_site_admit(ps3_log_site* site, ps3_log_level lvl);
void ps3_log_emit_admitted(ps3_log_site* site, ps3_log_level lvl, const char* fmt, ...) PS3_LOG_PRINTF_CHECK;

/* Shut the gate: refuse new logging operations, wait (bounded) for the ones already admitted to finish, then
 * cancel the stragglers (one admitted earlier that reaches output now is dropped from both sinks), stop the
 * flusher and make sure a write that was running has finished. A stop calls this before it writes its banner,
 * an exit before its summary. Every wait is bounded; a log lock that is still held at the deadline is noted
 * once on stderr and the file's final work is skipped. */
void ps3_log_quiesce(void);

/* Raw bytes to the file only, after the repeats and the suppression summary, then a flush: the PS3STOP
 * banner. Quiesces too. stop.cpp reaches both through ps3_stop_set_mirror, set by ps3_log_configure. */
void ps3_log_mirror(const char* bytes, size_t n);

/* Flush the file's buffer. The log's own flusher thread and ps3_stdio's (crash path) call it. */
void ps3_log_flush(void);

/* Read `log.*`, rotate and open the file. The first call wins; ps3_config_load()
 * makes it. A file that cannot be rotated or opened is a named stop
 * (host-log-file). */
void ps3_log_configure(void);

#define PS3_LOG_SITE_INIT_(tag, limit) { (tag), __FILE__, __LINE__, (limit), 0u, 0u, NULL, 0 }

/* `lvl` is evaluated once, before admission, so the level that is filtered is the level that is printed. */

/* Unlimited. */
#define PS3_LOG(lvl, tag, ...)                                                          \
    do {                                                                                \
        static ps3_log_site ps3_log_site_ = PS3_LOG_SITE_INIT_(tag, 0u);                \
        const ps3_log_level ps3_log_lvl_ = (lvl);                                       \
        if (ps3_log_site_admit(&ps3_log_site_, ps3_log_lvl_))                           \
            ps3_log_emit_admitted(&ps3_log_site_, ps3_log_lvl_, __VA_ARGS__);           \
    } while (0)

/* The first `limit` lines (limit > 0, a constant), then none: PS3_LOG_FIRST's
 * contract, kept exactly. What the cap swallowed is counted and reported at exit. */
#define PS3_LOG_N(limit, lvl, tag, ...)                                                 \
    do {                                                                                \
        static ps3_log_site ps3_log_site_ = PS3_LOG_SITE_INIT_(tag, limit);             \
        const ps3_log_level ps3_log_lvl_ = (lvl);                                       \
        if (ps3_log_site_admit(&ps3_log_site_, ps3_log_lvl_))                           \
            ps3_log_emit_admitted(&ps3_log_site_, ps3_log_lvl_, __VA_ARGS__);           \
    } while (0)

/* Ordering seam for tests (tests/gates/probe_log.c): the hook is called at the named points of the
 * log's own flusher thread, and may block there. Nothing in the runtime sets one. */
enum {
    PS3_LOG_TEST_FLUSHER_TICK = 1,   /* a wake of the flusher, before it looks at its stop signal; no lock held */
    PS3_LOG_TEST_FLUSHER_FLUSH,      /* under the log lock, just before the flusher flushes the file */
    PS3_LOG_TEST_FLUSHER_EXIT        /* the flusher is leaving */
};
typedef void (*ps3_log_test_hook_fn)(int phase);
void ps3_log_set_test_hook(ps3_log_test_hook_fn fn);

#define PS3_LOGE(tag, ...) PS3_LOG(PS3_LOG_ERROR, tag, __VA_ARGS__)
#define PS3_LOGW(tag, ...) PS3_LOG(PS3_LOG_WARN,  tag, __VA_ARGS__)
#define PS3_LOGI(tag, ...) PS3_LOG(PS3_LOG_INFO,  tag, __VA_ARGS__)
#define PS3_LOGD(tag, ...) PS3_LOG(PS3_LOG_DEBUG, tag, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_PS3_LOG_H */

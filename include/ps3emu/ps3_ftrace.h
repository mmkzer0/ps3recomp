/*
 * ps3_ftrace -- a timeline of who waits for whom, frame by frame.
 *
 * PS3_FTRACE=<start s>:<seconds>:<path> records timestamped spans from the PPU
 * syscall dispatcher, the SPU channel waits, the RSX FIFO walker and the Metal
 * backend for <seconds> starting <start s> after the first hook ran (~boot),
 * then writes them to <path> as text for tools/ftrace_report.py:
 *   # ps3_ftrace start_us=<n> end_us=<n> spans=<n> dropped=<n>
 *   T <tid> <name>                         PPU thread names
 *   <kind> <who> <a hex> <b hex> <t0 us> <t1 us>
 *
 * A sampling profile says where CPU time goes; it cannot say why a frame takes
 * 130 ms when no thread is busy for 130 ms. That is a latency chain -- the PPU
 * waiting on SPU tasks waiting on the RSX waiting on the GPU -- and only a
 * timeline shows it.
 *
 * Header-only on purpose: the hooked files are also compiled on their own by
 * self-contained tests, and a new link dependency would break each of them.
 * The state is one weak/selectany object the linker folds into a single copy;
 * the functions are static inline. Off (the default), every hook is a load
 * and a predictable branch. Knobs: PS3_FTRACE_MIN_US (shortest span kept,
 * default 20), PS3_FTRACE_MERGE_US (usleep polls of one thread at one call
 * site closer than this become one span, default 300), PS3_FTRACE_CAP (spans
 * kept, default 4M = 128 MB, allocated only when recording).
 */
#ifndef PS3EMU_PS3_FTRACE_H
#define PS3EMU_PS3_FTRACE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum ps3_ft_kind {
    PS3_FT_SYSCALL     = 1,  /* who = PPU tid, a = syscall number, b = guest lr        */
    PS3_FT_USLEEP      = 2,  /* who = PPU tid, a = usleep calls merged, b = guest lr   */
    PS3_FT_SPU_WAIT    = 3,  /* who = SPU id,  a = channel,      b = LS pc             */
    PS3_FT_SPU_BUSY    = 4,  /* who = SPU id,  a = lifted image, b = SPURS wid (LS 0x1DC) */
    PS3_FT_SEMA        = 5,  /* RSX blocked on ACQUIRE: a = label offset, b = value    */
    PS3_FT_SEMA_GAVEUP = 6,  /* same, ended by a bounded acquire giving up            */
    PS3_FT_DRAIN       = 7,  /* one RSX FIFO drain pass, a = 1 if it ended on an ACQUIRE */
    PS3_FT_FLIP_FIFO   = 8,  /* instant: the RSX reached a flip, a = buffer            */
    PS3_FT_FLIP_REQ    = 9,  /* instant: the guest asked for a flip, a = buffer        */
    PS3_FT_PRESENT     = 10, /* the host presented a guest frame                       */
    PS3_FT_GPU_ENCODE  = 11, /* Metal: CPU encode of the recorded draws, a = 1 present / 0 flush, b = records */
    PS3_FT_GPU_WAIT    = 12, /* Metal: CPU waits for the GPU, a = 1 present / 0 flush / 2 in-flight bound, b = flush reason */
    PS3_FT_READBACK    = 13, /* Metal: render target read back to the CPU, a = w, b = h */
    PS3_FT_GPU_EXEC    = 14, /* Metal: a command buffer's own GPU time, ending when the CPU saw it
                              * complete; who = render passes, a = depth resolves | colour copies << 16,
                              * b = 1 present / 0 flush */
    PS3_FT_HLE         = 15, /* who = PPU tid, a = NID, b = guest lr: an HLE import call */
    PS3_FT_DRAWABLE    = 16, /* Metal: the wait inside [CAMetalLayer nextDrawable], never filtered by PS3_FTRACE_MIN_US;
                              * who = 1 engine present / 2 legacy present,
                              * a bit 0 = nextDrawable returned nil, bit 1 = the surface shown is not the one
                              * shown last (engine only), bits 8-31 = rsx_draw_engine_present_diag() (engine only),
                              * b = records encoded or still open since the last display blit (legacy: this frame's) */
    PS3_FT_LABEL_WAIT  = 17, /* the guest-visible label-store wait in the GCM label writer, never filtered;
                              * who = PPU tid (0 = host callback or unknown), a = label offset,
                              * b = min(spins, 0xFFFF) | 1 << 16 if the RSX was mid-pass when it began */
    PS3_FT_AUDIO_MIX   = 18, /* one mixer iteration that produced a block, never filtered;
                              * who = 0, a = blocks mixed this iteration,
                              * b = backend queue depth in frames right after queueing
                              * (SDL queued frames; WASAPI padding; 0 if no backend) */
    PS3_FT_AUDIO_PACE  = 19, /* the mixer's pacing wait (sleep or backend wait), never filtered;
                              * who = 0, a = reason code (1 queue full, 2 clock pacing, 3 backend wait),
                              * b = queue depth in frames before the wait */
    PS3_FT_AUDIO_UNDERRUN = 20, /* instant (t0 == t1): queue depth was below one block right before
                              * queueing, never filtered; who = 0,
                              * a = depth in frames, b = blocks queued since the previous underrun */
    PS3_FT_AUDIO_STALE    = 21, /* instant (t0 == t1): port block was not refilled after consumption
                              * while audio was playing, never filtered; who = port index,
                              * a = block index read, b = run length of consecutive not-refilled blocks */
    /* SPURS job queue (jq2) spans, never filtered by PS3_FTRACE_MIN_US. Identities: q = host queue index in
     * creation order (8 bits, 255 saturates), w = worker index inside the queue (8 bits), seq = per-queue job
     * sequence number (32 bits, first job 1, assigned at push under the queue lock), waiter = the Port2Sync
     * event-flag waiter id (0..127), tid = PPU thread id. A PushSync emits no PUSH and takes no seq; a push
     * without the port-accounting flag has a PUSH and a JOB but no CREDIT. */
    PS3_FT_JQ2_PUSH       = 22, /* caller (PPU), entry of the host enqueue -> job visible to workers (after the kick);
                              * who = tid, a = seq, b = q | depth << 8, depth = jobs of this queue pushed and not yet
                              * admitted after this push, saturated at 2^24-1. A busy worker can admit the job between
                              * the ring write and the kick, so a JOB.t0 slightly before this t1 is possible: a
                              * consumer clamps the queue latency at 0 and counts the cases */
    PS3_FT_JQ2_JOB        = 23, /* worker, admitted by the worker -> job body returned, before the credit;
                              * who = q << 8 | w, a = seq, b = thread CPU time of the job in us
                              * (CLOCK_THREAD_CPUTIME_ID delta over the same interval), saturated at 2^32-1 */
    PS3_FT_JQ2_CREDIT     = 24, /* worker, start of the completion credit -> wake delivered (evf_set or fiber notify
                              * returned), or credit done when nobody waits; who = q << 8 | w, a = seq,
                              * b = waiter | wake << 8 | due << 16: wake 0 none, 1 event flag, 2 fiber; due 1 when
                              * this credit woke a waiter; waiter 0xFF when none and for a fiber (a fiber has no
                              * waiter id, so fiber waits cannot be joined on waiter) */
    PS3_FT_JQ2_SYNC_WAIT  = 25, /* the blocked PPU thread, entry of the blocking wait -> the thread runs again (only the
                              * blocking part of Port2Sync; a sync that completes without blocking emits nothing);
                              * who = tid, a = waiter | wake << 8 | mode << 16 (wake 1 event flag, 2 fiber; waiter
                              * 0xFF for a fiber; mode = the sync mode), b = q */
    PS3_FT_JQ2_IDLE       = 26, /* worker, parks (cv wait in) -> wakes; who = q << 8 | w,
                              * a = sat(depth at park, 2^24-1) | reason << 24, b = depth at wake, depth as in
                              * PUSH. reason, first match wins: 0 unrecorded (a trace written before this byte
                              * existed), 5 paused (no pass ran), 4 no free slot, 1 empty (nothing
                              * outstanding), 2 unwritten (the pass stopped at a slot the writer has not
                              * finished), 3 held (every entry the pass reached was held by a barrier),
                              * 15 unclassified (expected never; a consumer counts it) */
    PS3_FT_JQ2_HELD       = 27, /* worker, a job became visible to workers (the ring write in push_unless_shut)
                              * -> it became admissible (the release of the barrier on its tag, the visible
                              * time itself when it was never held); who = q << 8 | w, a = seq,
                              * b = tag | cause << 4 | mask << 8: cause 0 never held, 1 held by a barrier;
                              * mask = the SYNC mask of that barrier (0 when cause 0), bits 24-31 are 0.
                              * t1 <= JOB.t0: the release was recorded in an earlier critical section than
                              * the admission. No span when tracing was off at the push */
    PS3_FT_JQ2_BARRIER    = 28, /* worker, the end of the intake pass that consumed a SYNC and left its tags
                              * barred -> the release of those tags in complete(); who = q << 8 | w of the
                              * releasing worker, a = tags | mask << 16 (the tags this SYNC barred, released
                              * together; its SYNC mask), b = q | sat(waited, 2^24-1) << 8, waited = jobs in
                              * flight on the mask's tags when the barrier was set. Tags released together
                              * with one set time and mask form one span; a barrier set before tracing
                              * started emits none */
};

enum ps3_ft_audio_pace_reason {
    PS3_FT_AUDIO_PACE_QUEUE_FULL   = 1, /* queue above target depth / waiting for room */
    PS3_FT_AUDIO_PACE_CLOCK        = 2, /* clock pacing (no backend or pacing against clock) */
    PS3_FT_AUDIO_PACE_BACKEND_WAIT = 3, /* waiting on backend / short sleep when below target depth */
};

#if defined(_MSC_VER)
#  define PS3_FT_SHARED __declspec(selectany)
#else
#  define PS3_FT_SHARED __attribute__((weak))
#endif

typedef struct { uint64_t t0, t1; uint32_t a, b, who, kind; } ps3_ft_rec;

enum { PS3_FT_TIDS = 256 };

typedef struct {
    int on;              /* 0 unknown, 1 initialising, 2 recording, 3 off/done */
    int dumped;
    uint32_t n, cap, dropped;
    uint64_t base, start, end, min, merge;
    ps3_ft_rec* buf;
    char path[1024];
    struct { uint64_t t0, t1; uint32_t lr, n; } us[PS3_FT_TIDS];   /* usleep run being merged */
    char names[PS3_FT_TIDS][28];
} ps3_ft_state;

PS3_FT_SHARED ps3_ft_state g_ps3_ft = { 0 };

static inline uint64_t ps3_ft_now(void)
{
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static inline void ps3_ft_dump(void)
{
    ps3_ft_state* s = &g_ps3_ft;
    if (__atomic_exchange_n(&s->dumped, 1, __ATOMIC_ACQ_REL)) return;
    __atomic_store_n(&s->on, 3, __ATOMIC_RELEASE);
    /* Close the open usleep runs; their threads may still be in them. */
    for (uint32_t t = 0; t < PS3_FT_TIDS; t++) {
        if (!s->us[t].n) continue;
        const uint32_t i = __atomic_fetch_add(&s->n, 1, __ATOMIC_RELAXED);
        if (i >= s->cap) break;
        ps3_ft_rec r = { s->us[t].t0, s->us[t].t1, s->us[t].n, s->us[t].lr, t, PS3_FT_USLEEP };
        s->buf[i] = r;
    }
    uint32_t n = __atomic_load_n(&s->n, __ATOMIC_ACQUIRE);
    if (n > s->cap) n = s->cap;
    FILE* f = fopen(s->path, "w");
    if (!f) { fprintf(stderr, "[ftrace] cannot write %s\n", s->path); return; }
    fprintf(f, "# ps3_ftrace start_us=%.3f end_us=%.3f spans=%u dropped=%u\n",
            (double)(s->start - s->base) / 1e3, (double)(s->end - s->base) / 1e3, n, s->dropped);
    for (uint32_t t = 0; t < PS3_FT_TIDS; t++)
        if (s->names[t][0]) fprintf(f, "T %u %s\n", t, s->names[t]);
    for (uint32_t i = 0; i < n; i++) {
        const ps3_ft_rec* r = &s->buf[i];
        if (!r->kind) continue;   /* claimed but never filled: a writer raced the dump */
        fprintf(f, "%u %u %x %x %.3f %.3f\n", r->kind, r->who, r->a, r->b,
                (double)(r->t0 - s->base) / 1e3, (double)(r->t1 - s->base) / 1e3);
    }
    fclose(f);
    fprintf(stderr, "[ftrace] wrote %u spans (%u dropped) to %s\n", n, s->dropped, s->path);
    /* quick_exit does not flush stdio; a handler flushes its own output. */
    fflush(NULL);
}

static inline void ps3_ft_dump_at_exit(void) { ps3_ft_dump(); }

/* First hook: read PS3_FTRACE once. Hooks see "off" while another thread is
 * still setting up, which only loses spans before the window anyway. */
static inline int ps3_ft_init(void)
{
    ps3_ft_state* s = &g_ps3_ft;
    int expect = 0;
    if (!__atomic_compare_exchange_n(&s->on, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return expect == 2;
    int on = 3;
    const char* e = getenv("PS3_FTRACE");
    double start = 0, secs = 0;
    int used = 0;
    if (e && *e) {
        if (sscanf(e, "%lf:%lf:%n", &start, &secs, &used) < 2 || !used || secs <= 0) {
            fprintf(stderr, "[ftrace] PS3_FTRACE=<start s>:<seconds>:<path>, got '%s'\n", e);
        } else {
            snprintf(s->path, sizeof s->path, "%s", e + used);
            const char* m = getenv("PS3_FTRACE_MIN_US");
            s->min = (m ? strtoull(m, 0, 10) : 20ull) * 1000ull;
            const char* g = getenv("PS3_FTRACE_MERGE_US");
            s->merge = (g ? strtoull(g, 0, 10) : 300ull) * 1000ull;
            const char* c = getenv("PS3_FTRACE_CAP");
            s->cap = c ? (uint32_t)strtoul(c, 0, 10) : 4u << 20;
            s->buf = (ps3_ft_rec*)calloc(s->cap, sizeof(ps3_ft_rec));
            if (!s->buf) {
                fprintf(stderr, "[ftrace] no memory for %u spans\n", s->cap);
            } else {
                s->base = ps3_ft_now();
                s->start = s->base + (uint64_t)(start * 1e9);
                s->end = s->start + (uint64_t)(secs * 1e9);
                /* Both ways a process ends: quick_exit() for a guest exit and
                 * exit() for a harness that returned from main. The dump only
                 * runs on the one that happens. */
                atexit(ps3_ft_dump_at_exit);
                at_quick_exit(ps3_ft_dump_at_exit);
                fprintf(stderr, "[ftrace] recording %.1f s from t=%.1f s into %s\n", secs, start, s->path);
                on = 2;
            }
        }
    }
    __atomic_store_n(&s->on, on, __ATOMIC_RELEASE);
    return on == 2;
}

/* Recording? ps3_ft_init writes buf, cap, start, end, min and merge and then publishes on = 2 with a RELEASE
 * store. A thread that read 2 with a relaxed load is not ordered after those writes, and on arm64 may read a
 * stale buf. So the off path stays one relaxed load, and only a thread that read 2 pays for an ACQUIRE load of
 * the same word, which pairs with that release store. (A fence would order it on hardware, but
 * ThreadSanitizer does not model fences.) */
static inline int ps3_ft_recording(void)
{
    if (__builtin_expect(__atomic_load_n(&g_ps3_ft.on, __ATOMIC_RELAXED) != 2, 1)) return 0;
    return __atomic_load_n(&g_ps3_ft.on, __ATOMIC_ACQUIRE) == 2;
}

/* The hook guard: one load when off. */
static inline int ps3_ft_on(void)
{
    const int on = __atomic_load_n(&g_ps3_ft.on, __ATOMIC_RELAXED);
    if (__builtin_expect(on == 2, 0)) return ps3_ft_recording();
    if (__builtin_expect(on != 0, 1)) return 0;
    return ps3_ft_init();
}
#define PS3_FT_ON() ps3_ft_on()

static inline uint64_t ps3_ft_min_ns(void) { return g_ps3_ft.min; }

static inline void ps3_ft_span(int kind, uint32_t who, uint32_t a, uint32_t b,
                               uint64_t t0, uint64_t t1)
{
    ps3_ft_state* s = &g_ps3_ft;
    if (!ps3_ft_recording()) return;
    if (t1 < s->start) return;
    if (t0 > s->end) { ps3_ft_dump(); return; }
    const uint32_t i = __atomic_fetch_add(&s->n, 1, __ATOMIC_RELAXED);
    if (i >= s->cap) { __atomic_fetch_add(&s->dropped, 1, __ATOMIC_RELAXED); return; }
    ps3_ft_rec r = { t0, t1, a, b, who, (uint32_t)kind };
    s->buf[i] = r;
}

/* sys_timer_usleep polls come tens of thousands a second; consecutive sleeps of
 * one thread from one call site within PS3_FTRACE_MERGE_US become one span.
 * Only the sleeping thread touches its slot. */
static inline void ps3_ft_usleep(uint32_t tid, uint32_t lr, uint64_t t0, uint64_t t1)
{
    ps3_ft_state* s = &g_ps3_ft;
    if (tid >= PS3_FT_TIDS || !ps3_ft_recording()) return;
    if (s->us[tid].n && s->us[tid].lr == lr && t0 - s->us[tid].t1 <= s->merge) {
        s->us[tid].t1 = t1; s->us[tid].n++;
        return;
    }
    if (s->us[tid].n)
        ps3_ft_span(PS3_FT_USLEEP, tid, s->us[tid].n, s->us[tid].lr, s->us[tid].t0, s->us[tid].t1);
    s->us[tid].t0 = t0; s->us[tid].t1 = t1; s->us[tid].lr = lr; s->us[tid].n = 1;
}

/* A PPU thread's name, for the report's labels. */
static inline void ps3_ft_name(uint32_t tid, const char* name)
{
    if (tid >= PS3_FT_TIDS || !name || !PS3_FT_ON()) return;
    snprintf(g_ps3_ft.names[tid], sizeof g_ps3_ft.names[tid], "%s", name);
}

#ifdef __cplusplus
}
#endif

#endif

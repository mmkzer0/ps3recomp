/*
 * ps3_ftrace -- a timeline of who waits for whom, frame by frame.
 *
 * PS3_FTRACE=<start s>:<seconds>:<path> records timestamped spans from the PPU
 * syscall dispatcher, the SPU channel waits, the RSX FIFO walker and the Metal
 * backend for <seconds> starting <start s> after the first hook ran (~boot),
 * then writes them to <path> as text for tools/ftrace_report.py (a <path> ending in .zst
 * is written as a zstd stream instead; see ps3_ft_zstd_begin):
 *   # ps3_ftrace start_us=<n> end_us=<n> spans=<n> dropped=<n> jq2_images=<n> jq2_image_overflow=<n>
 *   T <tid> <name>                         PPU thread names
 *   J <index> <fp hex16> <ea hex8> <bytes> jq2 job image table (kind 29's b), ascending index
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
 * kept, default 4M = 128 MB, allocated only when recording). A .zst path needs the
 * `zstd` command on PATH, checked when recording starts; the stream is built in
 * <path>.partial and renamed to <path> only when zstd exits 0.
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
#  include <errno.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  ifdef __APPLE__
#    include <crt_externs.h>
#  endif
/* The platforms where the zstd child is spawned with every other descriptor closed (see ps3_ft_zstd_begin);
 * tests read this to know whether to expect it. */
#  ifdef __APPLE__
#    define PS3_FT_ZSTD_CLOSES_FDS 1
#  elif defined(__GLIBC__) && defined(_GNU_SOURCE)
#    if __GLIBC_PREREQ(2, 34)
#      define PS3_FT_ZSTD_CLOSES_FDS 1
#    endif
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
/* Inside extern "C": g++ defines _GNU_SOURCE, so glibc's unistd.h has already declared it with C linkage. */
extern char** environ;
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
                              * who = 0, a = reason code (1 guard skip, 2 clock wait, 3 backend wait [retired], 4 catch-up/re-anchor),
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
    PS3_FT_JQ2_IMAGE      = 29, /* worker, start -> end of the job's image identification, inside its JOB span (after
                              * staging, before the first instruction); who = q << 8 | w, a = seq,
                              * b = image table index (J lines), 0xFFFFFFFF when the table was full; one per job
                              * that has a JOB span, never filtered */
    /* cellAudio port ring, both sides, never filtered by PS3_FTRACE_MIN_US. The two kinds share one packing of a:
     * a = (tag & 0xFFFFFF) << 8 | (nblock - 1) << 3 | port. tag is the port's monotonic read counter
     * (AudioPortSlot.read_index), nblock the port's nBlock (a power of two, 2..32, so it divides 2^24 and
     * (a >> 8) % nblock == tag % nblock across the 24-bit wrap), port 0..7. b = filled mask: bit i is set when
     * block i of the ring holds a non-zero 32-bit word. The test reads raw guest words (no float compare, no
     * byte swap) and stops at a block's first non-zero word; exact silence written by a producer reads as empty. */
    PS3_FT_AUDIO_RING     = 30, /* instant (t0 == t1): the mixer is about to read a port's block, under the audio
                              * mutex; who = port, tag = read_index before the increment (the block read is
                              * tag % nblock), mask as the ring stood before the read */
    PS3_FT_AUDIO_RING_VIEW = 31, /* the guest's cellAudioGetPortConfig on a port that is open, started and has a
                              * buffer, entry -> return of the success path; who = the caller's PPU trace tid (0: no
                              * guest thread), tag and mask taken under the audio mutex. The index is as of the call:
                              * a mixer tick before the guest's own read of readIndexAddr is a kind 30 between them */
};

enum ps3_ft_audio_pace_reason {
    PS3_FT_AUDIO_PACE_QUEUE_FULL   = 1, /* guard skip: queue >= HI, skipping period */
    PS3_FT_AUDIO_PACE_CLOCK        = 2, /* clock wait: pacing against clock deadline */
    PS3_FT_AUDIO_PACE_BACKEND_WAIT = 3, /* retired (reserved; previously SDL_Delay(1) wait) */
    PS3_FT_AUDIO_PACE_CATCHUP      = 4, /* guard catch-up / re-anchor: queue < LO or stall re-anchor */
};

/* selectany (a COMDAT "any": one definition kept, every object sees the same
 * variable) under MSVC and MinGW. MinGW's weak definition is a PE weak external
 * whose default is a per-object `.weak.<name>.<object>` symbol; linking C++
 * objects that reach it through the .refptr indirection ended with the symbol
 * undefined. */
#if defined(_MSC_VER) || defined(__MINGW32__)
#  define PS3_FT_SHARED __declspec(selectany)
#else
#  define PS3_FT_SHARED __attribute__((weak))
#endif

typedef struct { uint64_t t0, t1; uint32_t a, b, who, kind; } ps3_ft_rec;

enum { PS3_FT_TIDS = 256, PS3_FT_JQ2_IMAGES = 1024 };

typedef struct {
    int on;              /* 0 unknown, 1 initialising, 2 recording, 3 off/done */
    int dumped;
    uint32_t n, cap, dropped;
    uint64_t base, start, end, min, merge;
    ps3_ft_rec* buf;
    char path[1024];
    int zst;             /* the path ends in .zst: written through a zstd child */
    char zstd[1024];     /* the zstd executable found on PATH at init (zst only) */
    struct { uint64_t t0, t1; uint32_t lr, n; } us[PS3_FT_TIDS];   /* usleep run being merged */
    char names[PS3_FT_TIDS][28];
    /* jq2 job images: each slot has one writer (the registry assigns an index once), published by `set`. */
    struct { uint64_t fp; uint32_t ea, bytes; int set; } jq2_img[PS3_FT_JQ2_IMAGES];
    uint32_t jq2_image_overflow;
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

/* True when the recorder was asked for a zstd file: the path ends in ".zst". */
static inline int ps3_ft_is_zst(const char* path)
{
    const size_t n = strlen(path);
    return n >= 4 && memcmp(path + n - 4, ".zst", 4) == 0;
}

#ifdef _WIN32
/* No .zst on Windows: the child would need a command line built for CreateProcess, whose quoting
 * rules (and the shell-injection risk of getting them wrong with a path from the environment) cannot
 * be exercised by the POSIX tests here. A named refusal at init is safer than an untested spawn. */
static inline int ps3_ft_zstd_check(ps3_ft_state* s)
{
    (void)s;
    fprintf(stderr, "[ftrace] .zst output is not supported on Windows; use a text path\n");
    return 0;
}
#else
/* The zstd stream is made by a `zstd` child fed through a pipe, so this header needs no libzstd (see
 * the top comment). The child is run by argv with no shell, from the PATH entry found at init: what
 * was checked is what runs, and nothing from PS3_FTRACE is ever parsed by a shell. */

static inline char** ps3_ft_environ(void)
{
#  ifdef __APPLE__
    return *_NSGetEnviron();   /* a shared library has no direct `environ` */
#  else
    return environ;
#  endif
}

/* The first executable regular file `zstd` on PATH (an empty entry is the current directory), into out. */
static inline int ps3_ft_find_zstd(char* out, size_t n)
{
    const char* p = getenv("PATH");
    if (!p || !*p) return 0;
    for (;;) {
        const char* end = strchr(p, ':');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        const int w = len ? snprintf(out, n, "%.*s/zstd", (int)len, p) : snprintf(out, n, "./zstd");
        struct stat st;
        if (w > 0 && (size_t)w < n && stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0)
            return 1;
        if (!end) return 0;
        p = end + 1;
    }
}

/* Init-time check, so a run that cannot write its trace says so before the game starts and not at exit. */
static inline int ps3_ft_zstd_check(ps3_ft_state* s)
{
    if (strlen(s->path) + sizeof ".partial" > sizeof s->path) {
        fprintf(stderr, "[ftrace] path too long for a .zst output (%zu bytes, limit %zu)\n", strlen(s->path),
                sizeof s->path - sizeof ".partial");
        return 0;
    }
    if (!ps3_ft_find_zstd(s->zstd, sizeof s->zstd)) {
        fprintf(stderr, "[ftrace] .zst output needs the zstd command on PATH and none was found; "
                        "recording is off (install zstd or use a text path)\n");
        return 0;
    }
    return 1;
}

/* A copy of fd that is close-on-exec and at least 3, with the original closed (-1 on failure, fd closed
 * either way). Above 2 so the dup2 into the child's 0 and 1 never meets a descriptor already there. */
static inline int ps3_ft_fd_high(int fd)
{
    const int hi = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    close(fd);
    return hi;
}

static inline void ps3_ft_reap(pid_t pid)
{
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
}

/* Starts `zstd -q -T0 -c` with stdin a pipe and stdout `partial`; returns the pipe's write end, or NULL
 * after naming the failure (nothing is left behind). SIGPIPE is ignored until ps3_ft_zstd_end restores it:
 * the dump runs from atexit or at_quick_exit, and a child that died must surface as a write error here, not
 * kill the process before it can say so.
 *
 * The child gets exactly fds 0 (the pipe), 1 (the output) and 2 (stderr, for zstd's own messages). Every
 * descriptor of ours is close-on-exec; the others of the process are closed in the child by
 * POSIX_SPAWN_CLOEXEC_DEFAULT (macOS) or addclosefrom_np (glibc with _GNU_SOURCE). Elsewhere the child
 * inherits whatever the process left inheritable, which is all POSIX lets it do. */
static inline FILE* ps3_ft_zstd_begin(const ps3_ft_state* s, const char* partial, pid_t* child,
                                      struct sigaction* old_pipe)
{
    int out = open(partial, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (out < 0) {
        fprintf(stderr, "[ftrace] cannot write %s: %s\n", partial, strerror(errno));
        return NULL;
    }
    int p[2];
    if ((out = ps3_ft_fd_high(out)) < 0 || pipe(p) != 0) {
        fprintf(stderr, "[ftrace] cannot set up the zstd pipe: %s\n", strerror(errno));
        if (out >= 0) close(out);
        unlink(partial);
        return NULL;
    }
    p[0] = ps3_ft_fd_high(p[0]);
    p[1] = ps3_ft_fd_high(p[1]);
    int rc = (p[0] < 0 || p[1] < 0) ? errno : 0;
    if (!rc) {
        posix_spawn_file_actions_t fa;
        posix_spawnattr_t at;
        rc = posix_spawn_file_actions_init(&fa);
        if (!rc && (rc = posix_spawnattr_init(&at)) != 0) posix_spawn_file_actions_destroy(&fa);
        if (!rc) {
            rc = posix_spawn_file_actions_adddup2(&fa, p[0], 0);
            if (!rc) rc = posix_spawn_file_actions_adddup2(&fa, out, 1);
#  ifdef __APPLE__
            if (!rc) rc = posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);
            if (!rc) rc = posix_spawn_file_actions_addinherit_np(&fa, 2);
#  elif defined(PS3_FT_ZSTD_CLOSES_FDS)
            if (!rc) rc = posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#  endif
            if (!rc) {
                char* argv[] = { (char*)"zstd", (char*)"-q", (char*)"-T0", (char*)"-c", NULL };
                rc = posix_spawn(child, s->zstd, &fa, &at, argv, ps3_ft_environ());
            }
            posix_spawn_file_actions_destroy(&fa);
            posix_spawnattr_destroy(&at);
        }
    }
    if (p[0] >= 0) close(p[0]);
    close(out);
    if (rc) {
        fprintf(stderr, "[ftrace] cannot start %s: %s\n", s->zstd, strerror(rc));
        if (p[1] >= 0) close(p[1]);
        unlink(partial);
        return NULL;
    }
    FILE* f = fdopen(p[1], "w");
    if (!f) {
        fprintf(stderr, "[ftrace] cannot open the zstd pipe: %s\n", strerror(errno));
        close(p[1]);   /* EOF for the child, then reap it */
        ps3_ft_reap(*child);
        unlink(partial);
        return NULL;
    }
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    struct sigaction ign;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, old_pipe);
    return f;
}

/* Closes the pipe (the child's end of input), restores SIGPIPE, waits for the child and publishes `partial`
 * as `final` only when zstd exited 0 and every write reached it. Otherwise it names the status and removes
 * `partial`; `final` is not touched. Returns 1 when `final` was written. */
static inline int ps3_ft_zstd_end(FILE* f, pid_t child, const struct sigaction* old_pipe, const char* partial,
                                  const char* final)
{
    int bad = ferror(f) != 0;
    if (fclose(f) != 0) bad = 1;
    sigaction(SIGPIPE, old_pipe, NULL);
    int st = 0;
    pid_t r;
    while ((r = waitpid(child, &st, 0)) < 0 && errno == EINTR) {}
    if (r < 0) {
        fprintf(stderr, "[ftrace] cannot wait for zstd (%s); %s not written\n", strerror(errno), final);
    } else if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
        fprintf(stderr, "[ftrace] zstd exited with status %d; %s not written\n", WEXITSTATUS(st), final);
    } else if (!WIFEXITED(st)) {
        fprintf(stderr, "[ftrace] zstd was killed by signal %d; %s not written\n",
                WIFSIGNALED(st) ? WTERMSIG(st) : 0, final);
    } else if (bad) {
        fprintf(stderr, "[ftrace] writing the trace to zstd failed; %s not written\n", final);
    } else if (rename(partial, final) != 0) {
        fprintf(stderr, "[ftrace] cannot rename %s to %s: %s\n", partial, final, strerror(errno));
    } else {
        return 1;
    }
    unlink(partial);
    return 0;
}
#endif

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
    FILE* f;
#ifndef _WIN32
    char partial[sizeof s->path + 16];
    pid_t child = -1;
    struct sigaction old_pipe;
    if (s->zst) {
        /* Cannot truncate (partial has 16 bytes over a NUL-terminated path); the check keeps GCC's
         * -Wformat-truncation quiet in every includer and fails closed if the sizes ever change. */
        const int w = snprintf(partial, sizeof partial, "%s.partial", s->path);
        f = (w > 0 && (size_t)w < sizeof partial) ? ps3_ft_zstd_begin(s, partial, &child, &old_pipe) : NULL;
        if (w <= 0 || (size_t)w >= sizeof partial) fprintf(stderr, "[ftrace] path too long for %s.partial\n", s->path);
    } else
#endif
    f = fopen(s->path, "w");
    if (!f) {
        if (!s->zst) fprintf(stderr, "[ftrace] cannot write %s\n", s->path);   /* zst: the begin step named the cause */
        return;
    }
    uint32_t images = 0;
    for (uint32_t i = 0; i < PS3_FT_JQ2_IMAGES; i++)
        if (__atomic_load_n(&s->jq2_img[i].set, __ATOMIC_ACQUIRE)) images++;
    fprintf(f, "# ps3_ftrace start_us=%.3f end_us=%.3f spans=%u dropped=%u jq2_images=%u jq2_image_overflow=%u\n",
            (double)(s->start - s->base) / 1e3, (double)(s->end - s->base) / 1e3, n, s->dropped, images,
            __atomic_load_n(&s->jq2_image_overflow, __ATOMIC_RELAXED));
    for (uint32_t t = 0; t < PS3_FT_TIDS; t++)
        if (s->names[t][0]) fprintf(f, "T %u %s\n", t, s->names[t]);
    for (uint32_t i = 0; i < PS3_FT_JQ2_IMAGES; i++) {
        if (!__atomic_load_n(&s->jq2_img[i].set, __ATOMIC_ACQUIRE)) continue;
        fprintf(f, "J %u %016llx %08x %u\n", i, (unsigned long long)s->jq2_img[i].fp, s->jq2_img[i].ea,
                s->jq2_img[i].bytes);
    }
    for (uint32_t i = 0; i < n; i++) {
        const ps3_ft_rec* r = &s->buf[i];
        if (!r->kind) continue;   /* claimed but never filled: a writer raced the dump */
        fprintf(f, "%u %u %x %x %.3f %.3f\n", r->kind, r->who, r->a, r->b,
                (double)(r->t0 - s->base) / 1e3, (double)(r->t1 - s->base) / 1e3);
    }
    int wrote = 1;
#ifndef _WIN32
    if (s->zst)
        wrote = ps3_ft_zstd_end(f, child, &old_pipe, partial, s->path);
    else
#endif
        fclose(f);
    if (wrote) fprintf(stderr, "[ftrace] wrote %u spans (%u dropped) to %s\n", n, s->dropped, s->path);
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
            /* A cut path names another file: refuse it rather than record somewhere else (host-path-limit). */
            const int w = snprintf(s->path, sizeof s->path, "%s", e + used);
            s->zst = (w > 0 && (size_t)w < sizeof s->path) ? ps3_ft_is_zst(s->path) : 0;
            if (w <= 0 || (size_t)w >= sizeof s->path) {
                fprintf(stderr, "[ftrace] PS3_FTRACE path is empty or longer than %zu bytes; recording is off\n",
                        sizeof s->path - 1);
            } else if (s->zst && !ps3_ft_zstd_check(s)) {
                /* The named error is printed; recording stays off, so the run is not left to fail at exit. */
            } else {
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

/* One job image table entry; the caller assigns each index once. An index past the table is counted, not
 * stored: the consumer then knows the table is incomplete. Not window-gated: an image first seen before the
 * window still names the jobs inside it. */
static inline void ps3_ft_jq2_image(uint32_t index, uint64_t fp, uint32_t ea, uint32_t bytes)
{
    ps3_ft_state* s = &g_ps3_ft;
    if (!ps3_ft_recording()) return;
    if (index >= PS3_FT_JQ2_IMAGES) { __atomic_fetch_add(&s->jq2_image_overflow, 1, __ATOMIC_RELAXED); return; }
    s->jq2_img[index].fp = fp;
    s->jq2_img[index].ea = ea;
    s->jq2_img[index].bytes = bytes;
    __atomic_store_n(&s->jq2_img[index].set, 1, __ATOMIC_RELEASE);
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

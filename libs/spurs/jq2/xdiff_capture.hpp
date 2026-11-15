/*
 * ps3recomp - recording a JobQueue2 job into an xdiff fixture, on the worker
 *
 * Commit 10a of docs/SPU_XDIFF.md (section 16): with `spu.xdiff_capture` = N and
 * `spu.xdiff_dir` set, the first N jobs of each registered image are recorded by
 * a tap on the effect port (runtime/spu/xdiff/spu_xdiff_recorder.h) and written,
 * synchronously, as `<dir>/<fingerprint>-q<queue>-s<seq>.spujob`. No shadow runs
 * (that is commit 10b); the job's guest-visible results are those of a run
 * without capture, because the recorder only listens.
 *
 * The executor calls capture_configure when a queue is created, capture_job_start
 * from the loader's start hook (after the job is staged, before its first
 * instruction) and capture_job_end from the end hook, the last two on the worker
 * thread that runs the job. A worker runs one job at a time, so everything per job
 * lives in thread-local state: the fixture buffer (two 256 KiB local stores,
 * allocated at a worker's first recording) is never shared between threads.
 *
 * Which jobs are recorded. The decision is made from what the image's registration
 * told the runtime (spu_workload_set_inventory, section 12.3), never from the job:
 *   - an image with no inventory is UNKNOWN and is not recorded (fail closed);
 *   - one whose inventory lists event branches is not recorded either: they use the
 *     event facility without a channel read, which no port mediates. Unresolved
 *     `wrch MFC_Cmd` sites do not refuse an image (section 12.3: same-program replay
 *     does not depend on a command's class); the fixtures of such an image are
 *     counted apart (captured_with_unresolved) for the report;
 *   - the body that runs must be the one the fixture will name: the interpreter
 *     (T0), or the body registered for the image's fingerprint (T1). Any other body
 *     (a forced test body, a different registration) records nothing;
 *   - an eligible job is recorded until N attempts of its image have been made.
 * An inventory is an upper bound on a run and never excuses a check at run time:
 * the recorder still reports an effect it has no record for.
 *
 * Bounded state. A process whose staged bytes vary (a loader that restages an
 * address with a new binary for every job) must not grow tables or logs without
 * bound. A worker remembers, per (EA, size), the last four contents it saw with
 * their fingerprints and verdicts (LRU): alternating buffers stay cheap and
 * recordable, and contents that never repeat cost one hash and one fingerprint a
 * job and nothing else. An image slot (at most 256) is allocated only for an image
 * whose registration carries an inventory; every other job is counted together.
 * At most 32 capture-ineligible lines are printed in a process, then one line says
 * the rest are suppressed. Failure lines are not budgeted: each consumes an attempt.
 *
 * What goes wrong goes wrong loudly and alone: a recorder or write failure is one
 * `[jq2-xdiff] capture-failed` line (image, queue, seq, reason, detail), the
 * attempt counts against N (so a persistent failure cannot print forever) and the
 * run goes on. A directory that does not exist or cannot be written is found once,
 * when the first queue is created: one line, and capture is off for the process.
 */
#ifndef PS3RECOMP_SPURS_JQ2_XDIFF_CAPTURE_HPP
#define PS3RECOMP_SPURS_JQ2_XDIFF_CAPTURE_HPP

#include "jq2/executor.hpp"

#include <cstdint>
#include <optional>

struct spu_context;
struct spu_job_layout;

namespace ps3recomp::spurs::jq2 {

/* What the tap needs of a job, handed to the loader's hooks by pointer. `queue` is the queue's creation
 * number (the q of a fixture's name; not the saturating trace index), `entry` the body about to run and
 * `interpreted` whether that is the SPU interpreter. */
struct CaptureJob {
    const JobStart* job;
    std::uint32_t queue;
    JobEntryFn entry;
    bool interpreted;
};

/* Reads spu.xdiff_capture and spu.xdiff_dir, once for the process (the configuration is immutable), and
 * checks the directory: it must exist, be a directory and be writable, or capture is off and one line says
 * why. Called by queue creation, before any worker exists. */
void capture_configure();

/* Starts recording the job on this worker thread when capture is on and the job qualifies; a no-op
 * otherwise. Must be called with `ctx` at its entry state, as the loader's start hook has it. */
void capture_job_start(const CaptureJob& job, ::spu_context* ctx, const ::spu_job_layout* layout);

/* Ends the recording this thread started for its job, if any, and writes the fixture. Call it first in
 * the end hook, before the executor applies its policy to the end: a job that is about to stop the
 * process still leaves its fixture. */
void capture_job_end(::spu_context* ctx);

/* What the run counted for one image under capture. `ineligible` counts jobs; its reasons overlap (an
 * image with event branches and unknown flag bits is in both). */
struct CaptureCounters {
    std::uint64_t captured = 0;                 /* fixtures written */
    std::uint64_t captured_with_unresolved = 0; /* of those, fixtures of an image whose inventory lists unresolved MFC sites */
    std::uint64_t failed = 0;                   /* attempts the recorder or the file write failed */
    std::uint64_t ineligible = 0;               /* jobs not recorded because of what is known of the image or its body */
    std::uint64_t ineligible_no_inventory = 0;  /* registered without an inventory, or with flag bits this build does not know */
    std::uint64_t ineligible_event_branches = 0;
    std::uint64_t ineligible_unknown_body = 0;  /* the body that ran is neither the interpreter nor the image's registered body */
    std::uint64_t ineligible_table_full = 0;    /* only in capture_unattributed() */
    std::uint64_t fingerprint_cache_misses = 0; /* only in capture_unattributed() */
    std::uint64_t ineligible_collision = 0;     /* the staged bytes did not hash to the remembered fingerprint */
};

/* The counters of `fingerprint`, or nullopt when no job of that image was looked at under capture (or the
 * image arrived after the table of 256 was full). Safe from any thread. */
[[nodiscard]] std::optional<CaptureCounters> capture_counters(std::uint64_t fingerprint);

/* The jobs no image slot was kept for: those of images with no inventory (the table of 256 is for images a
 * registration cleared to be considered, never for the unknown) and those of images past the table's bound.
 * `ineligible`, `ineligible_no_inventory` and `ineligible_table_full` count jobs; `fingerprint_cache_misses`
 * counts the byte-serial fingerprints the workers computed because their per-address cache missed. */
[[nodiscard]] CaptureCounters capture_unattributed();

/* A test seam: the bound on one fixture's payload bytes (default 64 MiB). A recording that would pass it
 * stops, counts as failed (reason capture-limit) and the job runs on. Process-wide; set it before the jobs. */
void set_capture_payload_limit(std::uint32_t bytes);

/* A test seam: XOR `mask` into every fingerprint a worker remembers (0: off). The recorder's own
 * fingerprint of the entry local store then disagrees with it, which is the collision branch. Process-wide;
 * set it before the jobs. */
void set_capture_fingerprint_fault(std::uint64_t mask);

}  // namespace ps3recomp::spurs::jq2

#endif  // PS3RECOMP_SPURS_JQ2_XDIFF_CAPTURE_HPP

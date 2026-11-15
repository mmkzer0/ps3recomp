/*
 * ps3recomp - in-run xdiff: sampled shadows of JobQueue2 jobs, on one audit thread
 *
 * Commit 10b of docs/SPU_XDIFF.md (sections 8.4, 9, 10, 11 and 14). With `spu.xdiff` = sample or all, the worker
 * that runs a job records it exactly as the capture does (xdiff_capture.hpp: one recorder, one content cache,
 * no second hash), credits the job, and only then hands the recording to a queue. One audit thread takes it
 * from there: it builds a shadow context from the recording alone, runs the OTHER tier on it with every effect
 * served from the transcript (spu_xdiff_replay), and compares. The authority is the body that ran
 * (spu.job_body, row 6); T0 is shadowed by the image's registered body through its budgeted twin and T1 by the
 * interpreter, under the instruction budget. The authority's result is what the guest saw before the shadow
 * ever started: a divergence is reported, counted and optionally written as a fixture, and ends the run only
 * when spu.xdiff_on_divergence / spu.xdiff_on_latent say stop.
 *
 * The queue is bounded: at most SPU_XDIFF_LIVE_QUEUE_SLOTS samples and SPU_XDIFF_LIVE_QUEUE_BYTES in total, one
 * sample at most SPU_XDIFF_MAX_LIVE_SAMPLE_BYTES. A larger recording is a void / capture-limit verdict and never
 * compared. `sample` never waits for the queue: a full queue drops the sample and counts it. `all` waits for room.
 * Shutdown (audit_shutdown, also run at process exit) drops what is queued and counts it, and never joins the
 * audit thread: a shadow in flight must not delay an exit.
 *
 * Everything the audit prints starts with `[jq2-xdiff]`. What a shadow run reaches inside the SPU runtime is
 * prefixed `[shadow] ` (spu_diag_shadow_set), and a named stop raised there is void / host-fault for that
 * sample (ps3_stop_divert_begin; spu_xdiff_replay), never a stop of the run.
 *
 * Threading. The state is a leaked heap object, as the capture's: the workers and the audit thread may still run
 * while the process exits and a static destructor must not race them. The settings are published once by
 * audit_configure, which queue creation calls before any worker exists. Every counter is a statistic nobody
 * orders other data by: relaxed. The queue and the fixture pool are under one mutex.
 */
#ifndef PS3RECOMP_SPURS_JQ2_XDIFF_AUDIT_HPP
#define PS3RECOMP_SPURS_JQ2_XDIFF_AUDIT_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "spu/xdiff/spu_xdiff_fixture.h"

namespace ps3recomp::spurs::jq2 {

/* What became of a job's sample, in the words of ps3_ftrace.h kind 32 (bits 8..15 of b). */
enum Disposition : std::uint8_t {
    kNotSampled = 0,  /* the job was not chosen, or nothing was recorded for the audit */
    kQueued = 1,      /* handed to the audit thread */
    kDropped = 2,     /* the queue was full (sample), or the audit had shut down */
    kNotEligible = 3  /* cannot be compared: no inventory, event branches, an unknown body, no twin for a lifted
                         shadow, no registered body, or a recording the audit refuses (over the bound, unsupported) */
};

/* spu_xdiff_reason ids are below this (the largest is no-body, 65). */
inline constexpr std::size_t kReasonSlots = 66;

/* The most fixtures the audit writes for one image, differences and void / unlifted-branch together, and so the most
 * distinct subject pcs of a void it remembers and names: one address that can be replayed, one fixture
 * (docs/SPU_XDIFF.md section 11). */
inline constexpr std::size_t kMaxFixturesPerImage = 16;

/* The distinct local store addresses of a void / unlifted-branch seen for one image: the coverage frontier a lifted
 * subject reached, where no lifted code covers the bytes the authority ran. Only the audit thread touches this --
 * a replay verdict is reached on no other thread -- so it is plain state with no atomics. The bound is the fixture
 * bound: an address past it could not have a fixture, and naming it would only grow the log. */
struct VoidPcs {
    std::array<std::uint32_t, kMaxFixturesPerImage> pc{};
    std::size_t count = 0;
    bool named_over = false; /* the one line that says further addresses are counted and not named */
};

/* What the audit counts for one image. Nodes live as long as the process; the audit thread and the workers update them
 * (void_pcs is the audit thread's alone). */
struct ImageAudit {
    std::atomic<std::uint64_t> jobs{0};          /* jobs of the image counted for the period: the first is job 0 */
    std::atomic<std::uint64_t> sampled{0};       /* chosen by the period (every job under `all`) */
    std::atomic<std::uint64_t> queued{0};        /* handed to the audit thread */
    std::atomic<std::uint64_t> dropped{0};       /* a full queue, or a queue already shut down */
    std::atomic<std::uint64_t> not_eligible{0};  /* jobs of the image that cannot be shadowed, all reasons */
    std::atomic<std::uint64_t> no_body{0};       /* ... of which: a T0 authority and no registered body */
    std::atomic<std::uint64_t> no_twin{0};       /* ... of which: a T0 authority and no budgeted twin */
    std::atomic<std::uint64_t> shadowed{0};      /* shadow runs that ended in a verdict */
    std::atomic<std::uint64_t> shutdown_dropped{0}; /* queued and dropped by the shutdown */
    std::atomic<std::uint64_t> verdict[8]{};     /* by spu_xdiff_verdict id (0 equal, 2 malformed, 3 not-found, 4 differ, 5 void) */
    std::atomic<std::uint64_t> verdict_other{0}; /* a verdict id outside the table above */
    std::atomic<std::uint64_t> reason[kReasonSlots]{}; /* by spu_xdiff_reason id, of every verdict (equal is `none`) */
    std::atomic<std::uint64_t> differ_visible{0}, differ_latent{0};
    std::atomic<std::uint64_t> fixtures{0};      /* fixtures written: differences and void / unlifted-branch */
    std::atomic<std::uint64_t> fixtures_over{0}; /* samples past the per-image bound of kMaxFixturesPerImage, counted
                                                    and not written */
    std::atomic<std::uint64_t> fixtures_failed{0};
    std::atomic<bool> named_no_body{false}, named_no_twin{false};
    std::atomic<bool> named_void[kReasonSlots]{}; /* a void of this reason has been named for the image */
    VoidPcs void_pcs; /* the audit thread's: the pcs of the image's void / unlifted-branch samples */
};

/* A plain copy of an ImageAudit, for a report or a test. */
struct AuditCounters {
    std::uint64_t jobs = 0, sampled = 0, queued = 0, dropped = 0, not_eligible = 0, no_body = 0, no_twin = 0;
    std::uint64_t shadowed = 0, shutdown_dropped = 0;
    std::uint64_t equal = 0, differ = 0, voided = 0, malformed = 0, not_found = 0, verdict_other = 0;
    std::array<std::uint64_t, kReasonSlots> reason{};
    std::uint64_t differ_visible = 0, differ_latent = 0;
    std::uint64_t fixtures = 0, fixtures_over = 0, fixtures_failed = 0;
};

enum class AuditMode : std::uint8_t { off, sample, all };

/* Reads spu.xdiff, spu.xdiff_period, spu.xdiff_on_divergence, spu.xdiff_on_latent and spu.xdiff_budget once for the
 * process and starts the audit thread when the mode is not off. Mode off stays off, with one line, when the process
 * runs under a knob that changes what the interpreter computes (spu_xdiff_refused_knob): a shadow under it would
 * compare another program. An unusable spu.xdiff_dir is one line and no fixtures; the audit goes on. Called by
 * queue creation, before any worker exists. */
void audit_configure();

/* True when the audit is on for the process (mode sample or all, not refused). Valid once audit_configure has run. */
[[nodiscard]] bool audit_active();

[[nodiscard]] AuditMode audit_mode();

/* Whether the job `n` (0-based, per image) of an image is chosen. */
[[nodiscard]] bool audit_sampled(std::uint64_t n);

/* Why a job of an image the capture cleared cannot be shadowed, or none. A T0 authority is shadowed by the
 * registered body, and by it only through its budgeted twin: an unbounded lifted shadow could hang the audit thread. */
enum class ShadowCheck : std::uint8_t { ok, no_body, no_twin };
[[nodiscard]] ShadowCheck audit_shadow_check(std::uint64_t fingerprint, bool authority_interpreted);

/* One recording on its way to the audit thread. */
struct Sample {
    spu_xdiff_fixture fixture{};   /* owned; the audit thread returns it to the pool */
    ImageAudit* image = nullptr;
    std::uint64_t fingerprint = 0;
    std::uint32_t queue = 0;       /* the queue's creation number: the q of a fixture's name */
    std::uint32_t seq = 0;
    std::uint32_t who = 0;         /* q << 8 | w of the job, the trace identity (saturating q) */
    std::uint8_t authority_tier = 0; /* spu_job_tier of the body that ran */
    std::size_t bytes = 0;         /* what the queue charges: the allocated size of the recording (audit_sample_charge) */

    Sample() = default;
    Sample(const Sample&) = delete;
    Sample& operator=(const Sample&) = delete;
    ~Sample();
};

/* The bytes the queue accounts for a recording. */
[[nodiscard]] std::size_t audit_sample_bytes(const spu_xdiff_fixture& f);

/* What a recording holds in memory: both local stores and register files plus the allocated capacity of its transcript and
 * payload stores, which is what the queue charges and what a worker retains. */
[[nodiscard]] std::size_t audit_sample_charge(const spu_xdiff_fixture& f);

/* The most a recording may hold before it is a void / capture-limit; the largest payload store that fits under it,
 * for a recorder that has nothing else to be limited by. */
[[nodiscard]] std::size_t audit_sample_limit();
[[nodiscard]] std::uint32_t audit_blob_limit();

/* Hands the recording of a job, after the job was credited, to the audit thread. Returns kQueued (`s` is moved from),
 * or kDropped (`s` stays with the caller): the queue is full and the mode is `sample`, or the audit has shut down.
 * Under `all` it waits for room first. */
[[nodiscard]] Disposition audit_submit(std::unique_ptr<Sample>& s);

/* Counts the verdict of a recording that was never compared (the recorder failed, or the recording is over the bound)
 * and names it once per image and reason. `reason` is the spu_xdiff_reason id the recorder gave, or capture-limit. */
void audit_note_void(ImageAudit& image, std::uint64_t fingerprint, std::uint32_t queue, std::uint32_t seq,
                     std::uint32_t reason, const char* detail);

/* Counts a job that cannot be shadowed. `why` names it once per image when it is no-body or no-twin; `ok` only counts
 * (the capture's own lines name an image that cannot be recorded at all). */
void audit_note_not_eligible(ImageAudit& image, std::uint64_t fingerprint, ShadowCheck why);

/* The fixture buffers of the workers: a recording is taken from, and a handed-over one returns to, a small pool,
 * so a sampled job does not allocate two local stores. */
[[nodiscard]] bool audit_fixture_take(spu_xdiff_fixture* f);

/* Stops the audit: what is queued is dropped and counted, a worker waiting for room is released (its sample is
 * dropped), the audit thread ends after the shadow it is running, and later samples are dropped. Does not join.
 * Registered with atexit and at_quick_exit at configure; a test calls it to check what shutdown counts. */
void audit_shutdown();

/* A plain copy of an image's counters. */
[[nodiscard]] AuditCounters audit_snapshot(const ImageAudit& image);

/* The counters of `fingerprint` (nullopt when no job of that image was looked at under the audit or the capture, or
 * the image arrived after the table of 256 was full), and of the jobs no image slot was kept for: those of images
 * with no inventory and those past the bound. Safe from any thread; the table is the capture's (xdiff_capture.hpp). */
[[nodiscard]] std::optional<AuditCounters> audit_counters(std::uint64_t fingerprint);
[[nodiscard]] AuditCounters audit_unattributed();

/* ---- test seams (set before the jobs; process-wide) -------------------------------------------------------- */

/* Which sample an audit hook is called for. */
struct AuditEvent {
    std::uint64_t fingerprint = 0;
    std::uint32_t queue = 0;
    std::uint32_t seq = 0;
};

/* The audit's synchronisation points, so a test orders the worker and the audit thread by hooks and never by sleeping:
 *   on_wait   a worker found the queue full under `all` and is about to wait for room (runs without the queue lock;
 *             the audit thread alone can make room, so a test that holds it here knows the worker cannot go on);
 *   on_start  the audit thread took the sample and has not yet run the shadow;
 *   on_done   the verdict is counted and reported (and the fixture written), before the stop a divergence may raise. */
struct AuditHooks {
    void* user = nullptr;
    void (*on_wait)(void* user, const AuditEvent&) = nullptr;
    void (*on_start)(void* user, const AuditEvent&) = nullptr;
    void (*on_done)(void* user, const AuditEvent&, std::uint32_t verdict, std::uint32_t reason) = nullptr;
};
void set_audit_hooks(const AuditHooks& hooks);

/* The bound on one sample, in bytes (default SPU_XDIFF_MAX_LIVE_SAMPLE_BYTES): a test lowers it to make an oversized
 * recording without a gigantic payload. */
void set_audit_sample_limit(std::uint32_t bytes);

/* Makes the allocation of the audit's shadow context fail at configure: the audit is then off, with one line, and no
 * producer records (set before the first queue). */
void set_audit_shadow_alloc_fail(bool fail);

/* How many samples are queued right now (not the one the audit thread is running). */
[[nodiscard]] std::size_t audit_queued();

}  // namespace ps3recomp::spurs::jq2

#endif  // PS3RECOMP_SPURS_JQ2_XDIFF_AUDIT_HPP

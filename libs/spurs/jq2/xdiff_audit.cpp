/*
 * ps3recomp - in-run xdiff: the audit queue and the audit thread. The design and the decisions are in
 * xdiff_audit.hpp and docs/SPU_XDIFF.md (sections 8.4, 9, 10, 11, 14 and 16, commit 10b).
 *
 * Memory orders. Every counter is a statistic nobody orders other data by: relaxed. A recording changes hands
 * through the queue's mutex, which orders its bytes. The settings and the hooks are published once, before any
 * worker exists, with release and read with acquire. The state is a leaked heap object: see the header.
 */

#include "jq2/xdiff_audit.hpp"
#include "ps3emu/ps3_log.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <process.h> /* _beginthreadex: the audit runs lifted code, which needs the workers' stack size std::thread cannot set */
#else
#include <pthread.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#endif

#include "jq2/xdiff_capture.hpp"
#include "ps3emu/config.h"
#include "ps3emu/ps3_ftrace.h"
#include "ps3emu/stop.h"
#include "ps3emu/thread_name.h"
#include "spu/spu_context.h"
#include "spu/spu_workload.h"
#include "spu/xdiff/spu_xdiff_replay.h"

namespace ps3recomp::spurs::jq2 {

namespace {

constexpr auto kRelaxed = std::memory_order_relaxed;

constexpr std::size_t kPoolFixtures = 4;                 /* fixture buffers kept for the workers */
constexpr unsigned kAuditStack = 8u << 20;               /* the shadow runs lifted code; as a worker's stack */

/* ---- settings ---------------------------------------------------------------- */

struct Settings {
    AuditMode mode = AuditMode::off;
    std::uint32_t period = 64;
    bool stop_visible = false, stop_latent = false;
    std::uint32_t budget = 0;
    std::string dir; /* difference fixtures; empty: none */
    spu_xdiff_shadow* shadow = nullptr; /* the audit thread's one shadow context, allocated before the thread exists */
};
std::once_flag g_configured;
std::atomic<const Settings*> g_settings{nullptr}; /* published by audit_configure; never freed */
std::atomic<bool> g_shadow_alloc_fail{false}; /* a test seam */
std::atomic<const AuditHooks*> g_hooks{nullptr};
std::atomic<std::uint32_t> g_sample_limit{SPU_XDIFF_MAX_LIVE_SAMPLE_BYTES};

const Settings* settings() { return g_settings.load(std::memory_order_acquire); }

/* ---- the queue and the pool ------------------------------------------------------ */

struct State {
    std::mutex mu;
    std::condition_variable work; /* the audit thread waits here for a sample */
    std::condition_variable room; /* a worker under `all` waits here for room */
    std::deque<std::unique_ptr<Sample>> items;
    std::size_t bytes = 0;
    bool stopped = false;
    std::vector<spu_xdiff_fixture> pool;
};

State& state()
{
    static State* s = new State; /* leaked on purpose */
    return *s;
}

/* The recording's bytes that a queue accounts: both local stores and register files, the transcript and the payloads. */
constexpr std::size_t kFixedBytes = 2 * std::size_t{SPU_XDIFF_LS_BYTES} + 2 * std::size_t{SPU_XDIFF_REG_BYTES};

bool pooled_size_ok(const spu_xdiff_fixture& f)
{
    /* What a worker may retain is what a sample may hold. */
    return static_cast<std::size_t>(f.blob_cap) + static_cast<std::size_t>(f.effect_cap) * sizeof(spu_xdiff_effect) <=
           g_sample_limit.load(kRelaxed);
}

/* A fixture the audit thread is done with: cleared and kept for a worker when it is not oversized, else freed. */
void give_back(spu_xdiff_fixture& f)
{
    if (f.ls0 == nullptr) return;
    if (pooled_size_ok(f)) {
        spu_xdiff_fixture_clear(&f);
        State& st = state();
        std::lock_guard<std::mutex> lk(st.mu);
        if (st.pool.size() < kPoolFixtures) {
            st.pool.push_back(f);
            std::memset(&f, 0, sizeof f);
            return;
        }
    }
    spu_xdiff_fixture_free(&f);
}

/* ---- diagnostics -------------------------------------------------------------- */

std::string fingerprint_text(std::uint64_t fingerprint)
{
    char b[24];
    std::snprintf(b, sizeof b, "0x%016llX", static_cast<unsigned long long>(fingerprint));
    return b;
}

const char* tier_name(std::uint32_t tier)
{
    switch (tier & SPU_TIER_MASK) {
    case SPU_TIER_INTERP: return "interp";
    case SPU_TIER_LIFTED: return "lifted";
    case SPU_TIER_PARTIAL: return "partial";
    case SPU_TIER_NATIVE: return "native";
    default: return "unknown";
    }
}

/* ---- counting ------------------------------------------------------------------- */

/* The verdict of one shadow run, counted on its image. */
void count_verdict(ImageAudit& image, std::uint32_t verdict, std::uint32_t reason)
{
    if (verdict < 8) image.verdict[verdict].fetch_add(1, kRelaxed);
    else image.verdict_other.fetch_add(1, kRelaxed);
    if (reason < kReasonSlots) image.reason[reason].fetch_add(1, kRelaxed);
}

/* A verdict that is not a difference and not equal says so once per image and reason: it proves nothing either way and
 * must not be silent, and it must not print once per sample either. */
void name_unfinished(ImageAudit& image, std::uint64_t fingerprint, std::uint32_t queue, std::uint32_t seq,
                     std::uint32_t verdict, std::uint32_t reason, const char* detail)
{
    if (reason >= kReasonSlots || image.named_void[reason].exchange(true, kRelaxed)) return;
    PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "%s image=%s queue=%u seq=%u reason=%s%s%s\n", spu_xdiff_verdict_id(verdict), fingerprint_text(fingerprint).c_str(), queue, seq, spu_xdiff_reason_id(reason), detail != nullptr ? " detail=" : "", detail != nullptr ? detail : "");
}

/* ---- the audit thread ------------------------------------------------------------------ */

/* Writes the sample's fixture with its verdict record under the image's bound, which differences and void /
 * unlifted-branch share; returns the path, or "none". The name is the sample's (section 7.2), so the fixture a void
 * leaves replays offline to the same verdict at the same subject pc. */
std::string write_fixture(const Settings& cfg, Sample& s, const spu_xdiff_verdict_record& rec)
{
    ImageAudit& image = *s.image;
    if (cfg.dir.empty()) return "none";
    if (image.fixtures.load(kRelaxed) >= kMaxFixturesPerImage) {
        image.fixtures_over.fetch_add(1, kRelaxed);
        return "none";
    }

    char name[64];
    spu_xdiff_fixture_name(name, sizeof name, s.fingerprint, s.queue, s.seq);
    std::string file = cfg.dir;
    if (file.back() != '/') file += '/';
    file += name;
    s.fixture.has_verdict = 1;
    s.fixture.verdict = rec;
    spu_xdiff_error err{};
    if (spu_xdiff_fixture_write(&s.fixture, file.c_str(), &err) == 0) {
        image.fixtures.fetch_add(1, kRelaxed);
        return file;
    }
    image.fixtures_failed.fetch_add(1, kRelaxed);
    PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "fixture-failed image=%s queue=%u seq=%u reason=%s detail=%s\n", fingerprint_text(s.fingerprint).c_str(), s.queue, s.seq, spu_xdiff_reason_id(err.reason), err.detail);
    return "none";
}

/* A difference: the named line and the fixture. Returns the fixture's path, or "none". The verdict is counted and the
 * fixture written before any stop, so a stopped run leaves the same evidence as one that goes on. */
std::string report_divergence(const Settings& cfg, Sample& s, const spu_xdiff_verdict_record& rec, std::uint32_t subject_tier)
{
    ImageAudit& image = *s.image;
    const std::uint32_t klass = spu_xdiff_differ_class_of(rec.reason, s.fixture.unit.profile);
    (klass == static_cast<std::uint32_t>(SPU_XDIFF_CLASS_LATENT) ? image.differ_latent : image.differ_visible)
        .fetch_add(1, kRelaxed);

    const std::string path = write_fixture(cfg, s, rec);

    char effect[16] = "none", where[16] = "none";
    if (rec.effect_index != SPU_XDIFF_NO_EFFECT) std::snprintf(effect, sizeof effect, "%u", rec.effect_index);
    if (rec.where != SPU_XDIFF_NO_WHERE) std::snprintf(where, sizeof where, "0x%X", rec.where);
    /* One call, one line: the workers print too. */
    PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "differ image=%s queue=%u seq=%u reason=%s class=%s effect=%s where=%s authority=%s "
                 "subject=%s fixture=%s\n", fingerprint_text(s.fingerprint).c_str(), s.queue, s.seq, spu_xdiff_reason_id(rec.reason), spu_xdiff_differ_class_id(klass), effect, where, tier_name(s.authority_tier), tier_name(subject_tier), path.c_str());
    std::fflush(stderr);
    return path;
}

/* A void / unlifted-branch: the subject reached a local store address no lifted code covers, which is the image's
 * coverage frontier and the one void worth a fixture. The first address of an image is named by the reason's line (once
 * per image and reason), every further distinct one by a line of its own, and each gets one fixture attempt -- at most
 * one per address and kMaxFixturesPerImage per image across differences and voids. The set is bounded by the same
 * number: an address past it can get no fixture, so it is not remembered, its sample is counted by the verdict's
 * reason (and by fixtures_over once the image's fixture bound is spent), and one line per image says so. The audit
 * thread alone reaches a replay verdict, so void_pcs is plain state. */
void report_void_branch(const Settings& cfg, Sample& s, const spu_xdiff_verdict_record& rec)
{
    ImageAudit& image = *s.image;
    VoidPcs& pcs = image.void_pcs;
    if (pcs.count == pcs.pc.size()) {
        /* The set is full, so this sample's address can be neither remembered nor compared with the remembered ones,
         * and one fixture per address forbids writing for it. When the image's fixture bound is spent (the normal
         * case: the set fills at the same number), the sample is counted as past the bound, as a difference would be. */
        if (!cfg.dir.empty() && image.fixtures.load(kRelaxed) >= kMaxFixturesPerImage)
            image.fixtures_over.fetch_add(1, kRelaxed);
        if (!pcs.named_over) {
            pcs.named_over = true;
            PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "void-pc-over image=%s reason=%s detail=%zu distinct subject pcs are already "
                         "named for the image; further gaps are counted, not named\n", fingerprint_text(s.fingerprint).c_str(), spu_xdiff_reason_id(rec.reason), pcs.count);
        }
        return;
    }
    bool fresh = true;
    for (std::size_t i = 0; i < pcs.count; ++i)
        if (pcs.pc[i] == rec.subject_pc) fresh = false;
    if (!fresh) return;
    pcs.pc[pcs.count++] = rec.subject_pc;

    if (!image.named_void[rec.reason].exchange(true, kRelaxed)) {
        PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "void image=%s queue=%u seq=%u reason=%s subject_pc=0x%05X\n", fingerprint_text(s.fingerprint).c_str(), s.queue, s.seq, spu_xdiff_reason_id(rec.reason), rec.subject_pc);
    } else {
        PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "void-pc image=%s queue=%u seq=%u reason=%s subject_pc=0x%05X\n", fingerprint_text(s.fingerprint).c_str(), s.queue, s.seq, spu_xdiff_reason_id(rec.reason), rec.subject_pc);
    }
    write_fixture(cfg, s, rec);
}

/* True when the class of a difference is set to stop the run. */
bool stops_run(const Settings& cfg, const spu_xdiff_verdict_record& rec, std::uint32_t profile)
{
    const std::uint32_t klass = spu_xdiff_differ_class_of(rec.reason, profile);
    return klass == static_cast<std::uint32_t>(SPU_XDIFF_CLASS_LATENT) ? cfg.stop_latent : cfg.stop_visible;
}

[[noreturn]] void stop_run(const Sample& s, const spu_xdiff_verdict_record& rec, std::uint32_t subject_tier,
                           const std::string& fixture)
{
    char image[24], effect[16] = "none", queue[16], seq[16];
    std::snprintf(image, sizeof image, "0x%016llX", static_cast<unsigned long long>(s.fingerprint));
    if (rec.effect_index != SPU_XDIFF_NO_EFFECT) std::snprintf(effect, sizeof effect, "%u", rec.effect_index);
    std::snprintf(queue, sizeof queue, "%u", s.queue);
    std::snprintf(seq, sizeof seq, "%u", s.seq);
    const ps3_stop_field fields[] = {
        {"image", image},
        {"reason", spu_xdiff_reason_id(rec.reason)},
        {"effect", effect},
        {"authority", tier_name(s.authority_tier)},
        {"subject", tier_name(subject_tier)},
        {"queue", queue},
        {"seq", seq},
        {"fixture", fixture.c_str()},
    };
    ps3_stop(PS3_STOP_SPU_XDIFF_DIVERGENCE, "an xdiff shadow tier disagreed with the authority tier on a recorded job",
             "The job was already credited and the guest saw the authority's result. Replay the fixture with spu_job_replay "
             "to reproduce the difference; set spu.xdiff_on_divergence / spu.xdiff_on_latent to report to keep running.",
             fields, sizeof fields / sizeof fields[0], nullptr);
}

void process(const Settings& cfg, spu_xdiff_shadow* shadow, Sample& s)
{
    ImageAudit& image = *s.image;
    const AuditEvent ev{s.fingerprint, s.queue, s.seq};
    const AuditHooks* hooks = g_hooks.load(std::memory_order_acquire);
    if (hooks != nullptr && hooks->on_start != nullptr) hooks->on_start(hooks->user, ev);

    spu_xdiff_replay_options opt{};
    opt.subject_tier = (s.authority_tier & SPU_TIER_MASK) == SPU_TIER_INTERP ? SPU_TIER_LIFTED : SPU_TIER_INTERP;
    opt.budget = cfg.budget;
    spu_xdiff_verdict_record rec{};
    const std::uint64_t ft0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    spu_diag_shadow_set(1);
    const std::uint32_t verdict = spu_xdiff_replay(&s.fixture, &opt, shadow, &rec);
    spu_diag_shadow_set(0);
    if (ft0 != 0)
        ps3_ft_span(PS3_FT_JQ2_XDIFF, s.who, s.seq, verdict | rec.reason << 8 | opt.subject_tier << 16, ft0, ps3_ft_now());

    image.shadowed.fetch_add(1, kRelaxed);
    count_verdict(image, verdict, rec.reason);
    bool stop = false;
    std::string fixture = "none";
    if (verdict == static_cast<std::uint32_t>(SPU_XDIFF_DIFFER)) {
        fixture = report_divergence(cfg, s, rec, opt.subject_tier);
        stop = stops_run(cfg, rec, s.fixture.unit.profile);
    } else if (verdict == static_cast<std::uint32_t>(SPU_XDIFF_VOID) && rec.reason == SPU_XDIFF_R_UNLIFTED_BRANCH) {
        report_void_branch(cfg, s, rec); /* the coverage frontier: named by pc, with a fixture */
    } else if (verdict != static_cast<std::uint32_t>(SPU_XDIFF_EQUAL)) {
        name_unfinished(image, s.fingerprint, s.queue, s.seq, verdict, rec.reason, nullptr);
    }
    if (hooks != nullptr && hooks->on_done != nullptr) hooks->on_done(hooks->user, ev, verdict, rec.reason);
    if (stop) stop_run(s, rec, opt.subject_tier, fixture);
}

void lower_priority()
{
    /* The audit must never take the cores the game's own threads need: `sample` runs below normal priority. */
    bool ok = false;
    const char* detail = "no portable way to lower a thread's priority is built for this host";
#ifdef _WIN32
    ok = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) != 0;
    detail = "SetThreadPriority failed";
#elif defined(__APPLE__)
    ok = pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0) == 0;
    detail = "pthread_set_qos_class_self_np failed";
#elif defined(__linux__)
    ok = setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10) == 0;
    detail = "setpriority failed";
#endif
    if (!ok)
        PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "audit-priority reason=not-lowered detail=%s; the audit runs at normal priority\n", detail);
}

void audit_main(const Settings* cfg)
{
    ps3_thread_name("jq2 xdiff");
    if (cfg->mode == AuditMode::sample) lower_priority();
    spu_xdiff_shadow* shadow = cfg->shadow;
    State& st = state();
    for (;;) {
        std::unique_ptr<Sample> s;
        {
            std::unique_lock<std::mutex> lk(st.mu);
            st.work.wait(lk, [&st] { return st.stopped || !st.items.empty(); });
            if (st.stopped) return;
            s = std::move(st.items.front());
            st.items.pop_front();
            st.bytes -= s->bytes;
        }
        st.room.notify_one();
        process(*cfg, shadow, *s);
        give_back(s->fixture);
    }
}

#ifdef _WIN32
unsigned __stdcall audit_entry(void* p)
{
    audit_main(static_cast<const Settings*>(p));
    return 0;
}
#else
void* audit_entry(void* p)
{
    audit_main(static_cast<const Settings*>(p));
    return nullptr;
}
#endif

bool start_thread(const Settings* cfg)
{
#ifdef _WIN32
    const auto t = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, kAuditStack, &audit_entry, const_cast<Settings*>(cfg), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    if (!t) return false;
    CloseHandle(t); /* never joined: a shadow in flight must not delay an exit */
    return true;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kAuditStack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    const int rc = pthread_create(&t, &attr, &audit_entry, const_cast<Settings*>(cfg));
    pthread_attr_destroy(&attr);
    return rc == 0;
#endif
}

void shutdown_at_exit() { audit_shutdown(); }

}  // namespace

/* ---- the process --------------------------------------------------------------------- */

void audit_configure()
{
    std::call_once(g_configured, [] {
        auto* s = new Settings; /* leaked: the audit thread reads it until the process ends */
        const char* mode = ps3_cfg_spu_xdiff();
        s->mode = std::strcmp(mode, "all") == 0 ? AuditMode::all : std::strcmp(mode, "sample") == 0 ? AuditMode::sample : AuditMode::off;
        s->period = ps3_cfg_spu_xdiff_period();
        s->stop_visible = std::strcmp(ps3_cfg_spu_xdiff_on_divergence(), "stop") == 0;
        s->stop_latent = std::strcmp(ps3_cfg_spu_xdiff_on_latent(), "stop") == 0;
        s->budget = ps3_cfg_spu_xdiff_budget();
        if (s->mode != AuditMode::off) {
            if (const char* knob = spu_xdiff_refused_knob()) {
                PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "audit-off reason=refused-knob detail=%s is set; a shadow under it would compare "
                             "another program, so nothing is shadowed\n", knob);
                s->mode = AuditMode::off;
            }
        }
        if (s->mode != AuditMode::off) {
            s->dir = ps3_cfg_spu_xdiff_dir();
            std::string why;
            if (!s->dir.empty() && !xdiff_dir_usable(s->dir, why)) {
                PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "fixtures-off reason=bad-dir detail=spu.xdiff_dir %s %s; differences are reported "
                             "without a fixture\n", s->dir.c_str(), why.c_str());
                s->dir.clear();
            }
            /* The shadow context is allocated here, before the thread and before any producer: a host that cannot
             * allocate it never records a sample. */
            s->shadow = g_shadow_alloc_fail.load(kRelaxed) ? nullptr : spu_xdiff_shadow_new();
            if (s->shadow == nullptr) {
                PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "audit-off reason=no-memory detail=the shadow context could not be allocated; "
                                     "nothing is shadowed\n");
                s->mode = AuditMode::off;
            } else if (!start_thread(s)) {
                PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "audit-off reason=thread detail=the host refused the audit thread; nothing is shadowed\n");
                spu_xdiff_shadow_free(s->shadow);
                s->shadow = nullptr;
                s->mode = AuditMode::off;
            } else {
                std::atexit(&shutdown_at_exit);
#ifndef _WIN32
                at_quick_exit(&shutdown_at_exit);
#endif
                capture_publish_audit_recording(); /* producers record from here on */
            }
        }
        g_settings.store(s, std::memory_order_release);
    });
}

bool audit_active()
{
    const Settings* s = settings();
    return s != nullptr && s->mode != AuditMode::off;
}

AuditMode audit_mode()
{
    const Settings* s = settings();
    return s != nullptr ? s->mode : AuditMode::off;
}

bool audit_sampled(std::uint64_t n)
{
    const Settings* s = settings();
    if (s == nullptr || s->mode == AuditMode::off) return false;
    return s->mode == AuditMode::all || n % s->period == 0;
}

ShadowCheck audit_shadow_check(std::uint64_t fingerprint, bool authority_interpreted)
{
    if (!authority_interpreted) return ShadowCheck::ok; /* T1 is shadowed by the interpreter, which runs any bytes */
    int id = 0;
    if (spu_workload_find_img(fingerprint, &id) == nullptr) return ShadowCheck::no_body;
    /* A lifted shadow runs only under the twin's budget: without it a loop with no effects would hang the audit thread. */
    if (spu_workload_find_twin(fingerprint, &id) == nullptr) return ShadowCheck::no_twin;
    return ShadowCheck::ok;
}

Sample::~Sample() { spu_xdiff_fixture_free(&fixture); }

std::size_t audit_sample_bytes(const spu_xdiff_fixture& f)
{
    return kFixedBytes + static_cast<std::size_t>(f.effect_count) * sizeof(spu_xdiff_effect) + f.blob_bytes;
}

std::size_t audit_sample_limit() { return g_sample_limit.load(kRelaxed); }

std::uint32_t audit_blob_limit()
{
    const std::size_t limit = audit_sample_limit();
    return limit > kFixedBytes ? static_cast<std::uint32_t>(limit - kFixedBytes) : 1u;
}

std::size_t audit_sample_charge(const spu_xdiff_fixture& f)
{
    return kFixedBytes + static_cast<std::size_t>(f.blob_cap) + static_cast<std::size_t>(f.effect_cap) * sizeof(spu_xdiff_effect);
}

void set_audit_shadow_alloc_fail(bool fail) { g_shadow_alloc_fail.store(fail, kRelaxed); }

void set_audit_sample_limit(std::uint32_t bytes) { g_sample_limit.store(bytes, kRelaxed); }

void set_audit_hooks(const AuditHooks& hooks)
{
    g_hooks.store(new AuditHooks(hooks), std::memory_order_release); /* a test seam: set once, never freed */
}

Disposition audit_submit(std::unique_ptr<Sample>& s)
{
    const Settings* cfg = settings();
    State& st = state();
    ImageAudit& image = *s->image;
    const std::size_t bytes = s->bytes;
    const AuditEvent ev{s->fingerprint, s->queue, s->seq};
    const AuditHooks* hooks = g_hooks.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> lk(st.mu);
    const auto has_room = [&st, bytes] {
        return st.items.size() < SPU_XDIFF_LIVE_QUEUE_SLOTS && st.bytes + bytes <= SPU_XDIFF_LIVE_QUEUE_BYTES;
    };
    if (!st.stopped && !has_room() && cfg != nullptr && cfg->mode == AuditMode::all) {
        if (hooks != nullptr && hooks->on_wait != nullptr) {
            lk.unlock();
            hooks->on_wait(hooks->user, ev);
            lk.lock();
        }
        st.room.wait(lk, [&st, &has_room] { return st.stopped || has_room(); });
    }
    Disposition disposition;
    if (st.stopped || !has_room()) {
        lk.unlock();
        image.dropped.fetch_add(1, kRelaxed);
        disposition = kDropped;
    } else {
        st.bytes += bytes;
        st.items.push_back(std::move(s));
        lk.unlock();
        image.queued.fetch_add(1, kRelaxed);
        st.work.notify_one();
        disposition = kQueued;
    }
    if (hooks != nullptr && hooks->on_disposed != nullptr) hooks->on_disposed(hooks->user, ev, disposition);
    return disposition;
}

void audit_note_void(ImageAudit& image, std::uint64_t fingerprint, std::uint32_t queue, std::uint32_t seq,
                     std::uint32_t reason, const char* detail)
{
    image.not_eligible.fetch_add(1, kRelaxed);
    const std::uint32_t verdict = spu_xdiff_reason_verdict(reason);
    count_verdict(image, verdict, reason);
    name_unfinished(image, fingerprint, queue, seq, verdict, reason, detail);
}

void audit_note_not_eligible(ImageAudit& image, std::uint64_t fingerprint, ShadowCheck why)
{
    image.not_eligible.fetch_add(1, kRelaxed);
    if (why == ShadowCheck::no_body) {
        image.no_body.fetch_add(1, kRelaxed);
        if (!image.named_no_body.exchange(true, kRelaxed))
            PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "audit-ineligible image=%s reason=no-body detail=the image has an inventory but no "
                         "registered body to shadow the interpreter with\n", fingerprint_text(fingerprint).c_str());
    } else if (why == ShadowCheck::no_twin) {
        image.no_twin.fetch_add(1, kRelaxed);
        if (!image.named_no_twin.exchange(true, kRelaxed))
            PS3_LOG(PS3_LOG_INFO, "jq2-xdiff", "audit-ineligible image=%s reason=no-twin detail=the registered body has no budgeted "
                         "twin, and an unbounded lifted shadow could hang the audit thread\n", fingerprint_text(fingerprint).c_str());
    }
}

bool audit_fixture_take(spu_xdiff_fixture* f)
{
    State& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.pool.empty()) return false;
    *f = st.pool.back();
    st.pool.pop_back();
    return true;
}

void audit_shutdown()
{
    State& st = state();
    std::deque<std::unique_ptr<Sample>> dropped;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        st.stopped = true;
        dropped.swap(st.items);
        st.bytes = 0;
    }
    st.room.notify_all();
    st.work.notify_all();
    for (const std::unique_ptr<Sample>& s : dropped) s->image->shutdown_dropped.fetch_add(1, kRelaxed);
}

std::size_t audit_queued()
{
    State& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.items.size();
}

AuditCounters audit_snapshot(const ImageAudit& image)
{
    AuditCounters c;
    c.jobs = image.jobs.load(kRelaxed);
    c.sampled = image.sampled.load(kRelaxed);
    c.queued = image.queued.load(kRelaxed);
    c.dropped = image.dropped.load(kRelaxed);
    c.not_eligible = image.not_eligible.load(kRelaxed);
    c.no_body = image.no_body.load(kRelaxed);
    c.no_twin = image.no_twin.load(kRelaxed);
    c.shadowed = image.shadowed.load(kRelaxed);
    c.shutdown_dropped = image.shutdown_dropped.load(kRelaxed);
    c.equal = image.verdict[static_cast<std::size_t>(SPU_XDIFF_EQUAL)].load(kRelaxed);
    c.differ = image.verdict[static_cast<std::size_t>(SPU_XDIFF_DIFFER)].load(kRelaxed);
    c.voided = image.verdict[static_cast<std::size_t>(SPU_XDIFF_VOID)].load(kRelaxed);
    c.malformed = image.verdict[static_cast<std::size_t>(SPU_XDIFF_MALFORMED)].load(kRelaxed);
    c.not_found = image.verdict[static_cast<std::size_t>(SPU_XDIFF_NOT_FOUND)].load(kRelaxed);
    c.verdict_other = image.verdict_other.load(kRelaxed);
    for (std::size_t i = 0; i < kReasonSlots; ++i) c.reason[i] = image.reason[i].load(kRelaxed);
    c.differ_visible = image.differ_visible.load(kRelaxed);
    c.differ_latent = image.differ_latent.load(kRelaxed);
    c.fixtures = image.fixtures.load(kRelaxed);
    c.fixtures_over = image.fixtures_over.load(kRelaxed);
    c.fixtures_failed = image.fixtures_failed.load(kRelaxed);
    return c;
}

}  // namespace ps3recomp::spurs::jq2

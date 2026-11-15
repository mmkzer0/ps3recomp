/*
 * ps3recomp - recording a JobQueue2 job into an xdiff fixture, on the worker.
 * The design and the decisions are in xdiff_capture.hpp and docs/SPU_XDIFF.md
 * (sections 4, 7, 11, 12.3 and 16, commit 10a).
 *
 * Threading: a job's state is thread-local, because a worker runs one job at a
 * time between its start and its end hook. What several workers share is the
 * per-image table, whose nodes are never freed and whose counters are atomics;
 * the table's mutex is taken when a worker first meets an image the registration
 * cleared and when a line is named for the first time. The table is a leaked heap
 * object: workers may still be running when the process exits, and a static
 * destructor must not race them. The inventory table of spu_workload.c is written
 * by registration before any job and read here without a lock; the settings are
 * published once by capture_configure, which queue creation calls before it starts
 * a worker.
 *
 * Memory orders. Every counter and the line budget are statistics or budgets
 * nobody orders other data by: relaxed. The reservation of an attempt is a
 * compare-and-swap on a count: it publishes nothing, so relaxed is enough too
 * (the fixture buffer a worker then fills is its own). The settings are published
 * with release and read with acquire.
 */

#include "jq2/xdiff_capture.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "ps3emu/config.h"
#include "ps3emu/lifter_abi.h"
#include "spu/spu_context.h"
#include "spu/spu_workload.h"
#include "spu/spurs_job.h"
#include "spu/xdiff/spu_xdiff_recorder.h"

/* The loader's classification goes into the fixture's END_ section as it is: the two sets are one. */
#define XDIFF_END_PAIR(name)                                                                                    \
    static_assert(static_cast<int>(SPU_JOB_END_##name) == static_cast<int>(SPU_XDIFF_END_##name),             \
                  "the loader's and the fixture's end classes are one set")
XDIFF_END_PAIR(RETURN);
XDIFF_END_PAIR(STOP);
XDIFF_END_PAIR(HALT);
XDIFF_END_PAIR(JM_CALL);
XDIFF_END_PAIR(OTHER);
#undef XDIFF_END_PAIR

namespace ps3recomp::spurs::jq2 {

namespace {

constexpr auto kRelaxed = std::memory_order_relaxed;

constexpr std::size_t kMaxImages = 256;        /* image slots; the rest are counted together */
constexpr std::size_t kMaxNamed = 1024;        /* distinct (address, size, reason) the ineligible line has named */
constexpr std::uint32_t kMaxLines = 32;        /* capture-ineligible lines per process */
constexpr std::size_t kMaxAddresses = 256;     /* per worker; the table is cleared, not grown, past this */
constexpr std::size_t kContents = 4;           /* contents remembered per address */
constexpr std::size_t kKeepFixtureBytes = 8u << 20; /* a worker's fixture buffer above this is freed after the job */

/* ---- settings -------------------------------------------------------------- */

struct Settings {
    std::uint32_t limit = 0; /* spu.xdiff_capture; 0: off, whatever else is set */
    std::string dir;
};
std::once_flag g_configured;
std::atomic<const Settings*> g_settings{nullptr}; /* published by capture_configure; never freed */
std::atomic<std::uint32_t> g_payload_limit{64u << 20};
std::atomic<std::uint64_t> g_fingerprint_fault{0};

/* ---- per image ----------------------------------------------------------- */

struct ImageCapture {
    /* Fixtures attempted (written or failed): the first N of an image, by reservation, so that several
     * workers on one image never write more than N between them. */
    std::atomic<std::uint64_t> attempted{0};
    std::atomic<std::uint64_t> captured{0}, captured_unresolved{0}, failed{0}, ineligible{0};
    std::atomic<std::uint64_t> no_inventory{0}, event_branches{0}, unknown_body{0}, table_full{0}, collision{0};
    std::atomic<std::uint64_t> cache_misses{0};
};

struct Table {
    std::mutex mu;
    std::map<std::uint64_t, std::unique_ptr<ImageCapture>> images; /* nodes are never erased; at most kMaxImages */
    ImageCapture unattributed;                                     /* images with no inventory or past the bound */
    std::set<std::string> named;                                   /* (address, size, reasons) already named; under mu */
};

Table& table()
{
    static Table* t = new Table; /* leaked on purpose: see the file comment */
    return *t;
}

/* The image's counters, or null when the table is full and this fingerprint is not in it. Called only for an
 * image whose registration carries an inventory. */
ImageCapture* image_of(std::uint64_t fingerprint)
{
    Table& t = table();
    std::lock_guard<std::mutex> lk(t.mu);
    const auto it = t.images.find(fingerprint);
    if (it != t.images.end()) return it->second.get();
    if (t.images.size() >= kMaxImages) return nullptr;
    return (t.images[fingerprint] = std::make_unique<ImageCapture>()).get();
}

/* ---- what is known of a staged content --------------------------------------- */

/* Why a job is not recorded, in the words of the `capture-ineligible` line; the index is a bit of
 * AddrCache::named. */
enum Reason : unsigned { kNoInventory, kTableFull, kEventBranches, kEventAndFlags, kInventoryFlags, kUnknownBody, kReasons };
constexpr const char* kReasonText[kReasons] = {"no-inventory",          "image-table-full", "event-branches",
                                               "event-branches,inventory-flags", "inventory-flags", "unknown-body"};

enum class Verdict : std::uint8_t {
    no_inventory, /* the registration says nothing about this image */
    table_full,   /* it has an inventory, the table of images is full */
    refused,      /* its inventory lists event branches, or flag bits this build does not know */
    eligible
};

/* The fingerprint of an image is the byte-serial FNV over its staged bytes, about a millisecond for a
 * large one, and every job would pay it. A worker therefore remembers it by (EA, size) and content hash, and
 * with it everything the registration says about that fingerprint (docs/SPU_XDIFF.md section 4). The hit is a
 * claim, not a proof: a fixture is only written after the recorder's own byte-serial fingerprint of the entry
 * local store agrees with it. */
struct Content {
    bool valid = false;
    std::uint64_t content = 0;
    std::uint64_t fingerprint = 0;
    std::uint64_t used = 0;          /* LRU tick */
    Verdict verdict = Verdict::no_inventory;
    Reason reason = kNoInventory;    /* of a verdict that is not eligible */
    ImageCapture* image = nullptr;   /* null unless the registration cleared the image to be considered */
    spu_workload_inventory inv{};
};

/* The last kContents contents staged at one (EA, size). A loader that alternates buffers keeps them all;
 * contents that never repeat only ever cost a hash and a fingerprint. `named` is the reasons this worker
 * has already named for the address, so a steady stream of ineligible jobs takes no global lock. */
struct AddrCache {
    std::array<Content, kContents> contents;
    std::uint64_t tick = 0;
    unsigned named = 0;
};

struct WorkerCapture {
    spu_xdiff_fixture fixture{}; /* zeroed until the first recording; two local stores, never shared */
    spu_xdiff_recorder recorder{};
    bool active = false;
    bool unresolved = false;     /* the recorded image's inventory lists unresolved MFC sites */
    ImageCapture* image = nullptr;
    std::uint64_t fingerprint = 0;
    std::uint32_t queue = 0, seq = 0;
    std::unordered_map<std::uint64_t, AddrCache> seen;

    WorkerCapture() = default;
    WorkerCapture(const WorkerCapture&) = delete;
    WorkerCapture& operator=(const WorkerCapture&) = delete;
    ~WorkerCapture() { spu_xdiff_fixture_free(&fixture); }
};
thread_local WorkerCapture t_capture;

/* ---- diagnostics ---------------------------------------------------------- */

std::atomic<std::uint32_t> g_lines{0};

/* One of the process's kMaxLines capture-ineligible lines. The one after the last says the rest are not
 * printed; the counters keep counting. Failure lines are not budgeted: each consumes one of an image's N
 * attempts, so they are bounded by N per image and the budget must never silence them. */
bool line_budget()
{
    const std::uint32_t n = g_lines.fetch_add(1, kRelaxed);
    if (n == kMaxLines)
        std::fprintf(stderr,
                     "[jq2-xdiff] capture-lines-suppressed detail=more than %u capture-ineligible lines; "
                     "the counters keep counting\n",
                     kMaxLines);
    return n < kMaxLines;
}

std::string fingerprint_text(std::uint64_t fingerprint)
{
    char b[24];
    std::snprintf(b, sizeof b, "0x%016llX", static_cast<unsigned long long>(fingerprint));
    return b;
}

/* One line, one call, so lines of several workers never interleave. */
void fail(const WorkerCapture& w, ImageCapture& image, std::uint64_t fingerprint, std::uint32_t reason, const char* detail)
{
    image.failed.fetch_add(1, kRelaxed);
    std::fprintf(stderr, "[jq2-xdiff] capture-failed image=%s queue=%u seq=%u reason=%s detail=%s\n",
                 fingerprint_text(fingerprint).c_str(), w.queue, w.seq, spu_xdiff_reason_id(reason), detail);
}

/* Names why the job at (address, size) is not recorded: once per worker, address and reason, then once per
 * process, address, size and reason, within the cap on lines. */
void name_ineligible(AddrCache& a, Reason reason, const spu_job_layout* l, std::uint64_t fingerprint)
{
    if ((a.named & (1u << reason)) != 0) return;
    a.named |= 1u << reason;
    char key[96];
    std::snprintf(key, sizeof key, "%08X:%u:%s", l->image_ea, l->image_bytes, kReasonText[reason]);
    {
        Table& t = table();
        std::lock_guard<std::mutex> lk(t.mu);
        if (t.named.size() >= kMaxNamed || !t.named.insert(key).second) return;
    }
    if (!line_budget()) return;
    std::fprintf(stderr, "[jq2-xdiff] capture-ineligible image=%s reason=%s ea=0x%08X bytes=%u\n",
                 fingerprint_text(fingerprint).c_str(), kReasonText[reason], l->image_ea, l->image_bytes);
}

/* ---- the image of a job ---------------------------------------------------- */

/* What the registration says of `fingerprint`, as a verdict; takes an image slot only for an image it has an
 * inventory for. */
void judge(Content& c)
{
    spu_workload_inventory inv{};
    if (!spu_workload_find_inventory(c.fingerprint, &inv)) {
        c.verdict = Verdict::no_inventory;
        c.reason = kNoInventory;
        return;
    }
    c.inv = inv;
    c.image = image_of(c.fingerprint);
    if (c.image == nullptr) {
        c.verdict = Verdict::table_full;
        c.reason = kTableFull;
        return;
    }
    const std::uint32_t known = SPU_WORKLOAD_INV_EVENT_BRANCHES | SPU_WORKLOAD_INV_UNRESOLVED_MFC;
    const bool events = (inv.flags & SPU_WORKLOAD_INV_EVENT_BRANCHES) != 0;
    const bool unknown_bits = (inv.flags & ~known) != 0;
    /* Event branches use the event facility without a channel read. Bits this build does not know say
     * something it cannot act on. Unresolved MFC sites are not a reason: same-program replay does not
     * depend on a command's class (section 12.3). */
    if (events || unknown_bits) {
        c.verdict = Verdict::refused;
        c.reason = events && unknown_bits ? kEventAndFlags : events ? kEventBranches : kInventoryFlags;
        return;
    }
    c.verdict = Verdict::eligible;
}

/* The content at this job's (address, size): remembered, or hashed, fingerprinted and judged now. */
Content& seen_content(AddrCache& a, const spu_context* ctx, const spu_job_layout* l)
{
    const std::uint8_t* bytes = ctx->ls + l->image_ls;
    const std::uint64_t content = spu_workload_content_hash(bytes, l->image_bytes);
    Content* victim = &a.contents[0];
    for (Content& c : a.contents) {
        if (c.valid && c.content == content) {
            c.used = ++a.tick;
            return c;
        }
        if (!c.valid) victim = &c; /* an empty slot beats the least recently used */
        else if (victim->valid && c.used < victim->used) victim = &c;
    }
    Content& c = *victim;
    c = Content{};
    c.valid = true;
    c.content = content;
    c.used = ++a.tick;
    c.fingerprint = spu_workload_fingerprint(bytes, l->image_bytes) ^ g_fingerprint_fault.load(kRelaxed);
    table().unattributed.cache_misses.fetch_add(1, kRelaxed);
    judge(c);
    return c;
}

/* Counts a job on the shared bucket of jobs no image slot was kept for. */
void count_unattributed(bool no_inventory)
{
    ImageCapture& u = table().unattributed;
    u.ineligible.fetch_add(1, kRelaxed);
    (no_inventory ? u.no_inventory : u.table_full).fetch_add(1, kRelaxed);
}

bool directory_is_usable(const std::string& dir, std::string& why)
{
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        why = "does not exist";
        return false;
    }
    if (!std::filesystem::is_directory(dir, ec)) {
        why = "is not a directory";
        return false;
    }
#ifdef _WIN32
    const bool writable = _access(dir.c_str(), 2) == 0;
#else
    const bool writable = access(dir.c_str(), W_OK | X_OK) == 0;
#endif
    if (!writable) {
        why = "is not writable";
        return false;
    }
    return true;
}

/* A buffer that grew for one pathological recording (a spinning poll job) is not kept. */
bool oversized(const spu_xdiff_fixture& f)
{
    return static_cast<std::size_t>(f.blob_cap) + static_cast<std::size_t>(f.effect_cap) * sizeof(spu_xdiff_effect) >
           kKeepFixtureBytes;
}

}  // namespace

/* ---- the process ------------------------------------------------------------ */

void capture_configure()
{
    std::call_once(g_configured, [] {
        auto* s = new Settings; /* leaked: workers read it until the process ends */
        s->limit = ps3_cfg_spu_xdiff_capture();
        if (s->limit != 0) {
            s->dir = ps3_cfg_spu_xdiff_dir();
            std::string why;
            if (s->dir.empty()) {
                std::fprintf(stderr,
                             "[jq2-xdiff] capture-off reason=no-dir detail=spu.xdiff_capture=%u but spu.xdiff_dir is empty; "
                             "nothing is recorded\n",
                             s->limit);
                s->limit = 0;
            } else if (!directory_is_usable(s->dir, why)) {
                std::fprintf(stderr,
                             "[jq2-xdiff] capture-off reason=bad-dir detail=spu.xdiff_dir %s %s; nothing is recorded\n",
                             s->dir.c_str(), why.c_str());
                s->limit = 0;
            }
        }
        g_settings.store(s, std::memory_order_release);
    });
}

void set_capture_payload_limit(std::uint32_t bytes) { g_payload_limit.store(bytes, kRelaxed); }
void set_capture_fingerprint_fault(std::uint64_t mask) { g_fingerprint_fault.store(mask, kRelaxed); }

/* ---- the job --------------------------------------------------------------- */

void capture_job_start(const CaptureJob& job, ::spu_context* ctx, const ::spu_job_layout* l)
{
    const Settings* settings = g_settings.load(std::memory_order_acquire);
    if (settings == nullptr || settings->limit == 0) return;
    const std::uint32_t limit = settings->limit;
    const JobStart& js = *job.job;

    WorkerCapture& w = t_capture;
    /* A job that began recording always reaches its end hook or ends the process; if it did not, its
     * context is gone and there is nothing to remove from it. */
    w.active = false;
    w.queue = job.queue;
    w.seq = js.seq;

    if (static_cast<std::uint64_t>(l->image_ls) + l->image_bytes > SPU_LS_SIZE) {
        /* The loader validated the layout; this names a loader that stopped doing so. It consumes no attempt,
         * so unlike the other failure lines it is budgeted. */
        if (line_budget())
            std::fprintf(stderr,
                         "[jq2-xdiff] capture-failed image=unknown queue=%u seq=%u reason=%s detail=staged image [0x%X, +%u) is "
                         "outside local store\n",
                         w.queue, js.seq, spu_xdiff_reason_id(SPU_XDIFF_R_FIXTURE_FORMAT), l->image_ls, l->image_bytes);
        return;
    }
    const std::uint64_t key = (static_cast<std::uint64_t>(l->image_ea) << 32) | l->image_bytes;
    if (w.seen.size() >= kMaxAddresses && w.seen.find(key) == w.seen.end()) w.seen.clear();
    AddrCache& addr = w.seen[key];
    Content& content = seen_content(addr, ctx, l);
    const std::uint64_t fingerprint = content.fingerprint;

    switch (content.verdict) {
    case Verdict::no_inventory:
        count_unattributed(true);
        name_ineligible(addr, content.reason, l, fingerprint);
        return;
    case Verdict::table_full:
        count_unattributed(false);
        name_ineligible(addr, content.reason, l, fingerprint);
        return;
    case Verdict::refused: {
        ImageCapture& image = *content.image;
        image.ineligible.fetch_add(1, kRelaxed);
        if (content.reason != kInventoryFlags) image.event_branches.fetch_add(1, kRelaxed);
        if (content.reason != kEventBranches) image.no_inventory.fetch_add(1, kRelaxed); /* flag bits of another build */
        name_ineligible(addr, content.reason, l, fingerprint);
        return;
    }
    case Verdict::eligible: break;
    }

    ImageCapture& image = *content.image;
    const spu_workload_inventory& inv = content.inv;
    if (image.attempted.load(kRelaxed) >= limit) return; /* this image has its N */

    /* The body that runs is the authority the fixture names: T0 for the interpreter, T1 only for the
     * body registered under this fingerprint (section 3), with the lift identity of its registration. */
    spu_xdiff_body_id authority{};
    if (job.interpreted) {
        authority.tier = static_cast<std::uint8_t>(SPU_TIER_INTERP);
    } else if (job.entry != nullptr && spu_workload_find_img(fingerprint, nullptr) == job.entry) {
        authority.tier = static_cast<std::uint8_t>(SPU_TIER_LIFTED);
        authority.lifter_abi = SPU_RUNTIME_LIFTER_ABI;
        authority.code_end = inv.code_end;
        authority.lift_options = inv.lift_options;
    } else {
        image.ineligible.fetch_add(1, kRelaxed);
        image.unknown_body.fetch_add(1, kRelaxed);
        name_ineligible(addr, kUnknownBody, l, fingerprint);
        return;
    }

    /* Reserve one of the N attempts: several workers on one image race here. The count publishes nothing
     * (the buffer a winner fills is its own), so the CAS needs atomicity and no ordering. */
    for (std::uint64_t n = image.attempted.load(kRelaxed);;) {
        if (n >= limit) return;
        if (image.attempted.compare_exchange_weak(n, n + 1, kRelaxed, kRelaxed)) break;
    }

    if (w.fixture.ls0 == nullptr && spu_xdiff_fixture_init(&w.fixture) != 0) {
        fail(w, image, fingerprint, SPU_XDIFF_R_CAPTURE_LIMIT, "out of memory for the worker's fixture buffer");
        return;
    }

    spu_xdiff_unit unit{};
    unit.kind = SPU_XDIFF_UNIT_JOB;
    unit.profile = SPU_XDIFF_PROFILE_JQ2;
    unit.entry_pc = ctx->pc;
    unit.return_ls = SPU_JOB_RETURN_LINK_LS; /* what the loader plants in r0 (spurs_job.c) */
    unit.job_rt_ls = ctx->job_rt_ls;
    unit.job_rt_ls2 = ctx->job_rt_ls2;
    unit.image_ea = l->image_ea;
    unit.image_ls = l->image_ls;
    unit.image_bytes = l->image_bytes;
    unit.bss_bytes = l->bss_bytes;
    unit.desc_ls = l->desc_ls;
    unit.desc_bytes = l->desc_bytes;
    unit.io_ls = l->io_ls;
    unit.io_bytes = l->io_bytes;
    unit.out_ls = l->out_ls;
    unit.out_bytes = l->out_bytes;
    unit.scratch_ls = l->scratch_ls;
    unit.scratch_bytes = l->scratch_bytes;
    unit.stack_bytes = l->stack_bytes;
    unit.r1 = l->r1;
    unit.ctx_ls = l->ctx_ls;
    unit.origin_queue = job.queue;
    unit.origin_worker = js.worker;
    unit.origin_seq = js.seq;
    unit.origin_descriptor_ea = js.descriptor_ea;

    if (spu_xdiff_recorder_begin(&w.recorder, &w.fixture, ctx, &unit, &authority, nullptr, nullptr,
                                 g_payload_limit.load(kRelaxed)) != 0) {
        fail(w, image, fingerprint, w.recorder.error.reason, w.recorder.error.detail);
        spu_xdiff_fixture_free(&w.fixture);
        return;
    }
    if (w.fixture.identity.image_fingerprint != fingerprint) {
        /* The remembered fingerprint was not the one of these bytes: a collision of the content hash. The
         * fixture is dropped, never written under a fingerprint that is not its own, and the memory is
         * evicted so the next job of this content hashes again (section 4). Not budgeted: it consumed an
         * attempt. */
        spu_xdiff_recorder_end(&w.recorder, ctx, SPU_XDIFF_END_OTHER, 0); /* removes the tap; the fixture is dropped */
        const std::uint64_t actual = w.fixture.identity.image_fingerprint;
        content = Content{};
        image.ineligible.fetch_add(1, kRelaxed);
        image.collision.fetch_add(1, kRelaxed);
        std::fprintf(stderr,
                     "[jq2-xdiff] capture-ineligible image=%s reason=fingerprint-collision ea=0x%08X bytes=%u "
                     "detail=the staged bytes hash to %s, not to the remembered fingerprint; nothing recorded\n",
                     fingerprint_text(fingerprint).c_str(), l->image_ea, l->image_bytes, fingerprint_text(actual).c_str());
        return;
    }
    w.active = true;
    w.unresolved = (inv.flags & SPU_WORKLOAD_INV_UNRESOLVED_MFC) != 0;
    w.image = &image;
    w.fingerprint = fingerprint;
}

void capture_job_end(::spu_context* ctx)
{
    /* With capture off the thread-local state is not even constructed. */
    const Settings* settings = g_settings.load(std::memory_order_acquire);
    if (settings == nullptr || settings->limit == 0) return;
    WorkerCapture& w = t_capture;
    if (!w.active) return;
    w.active = false;
    ImageCapture& image = *w.image;

    /* The loader's classification, the one the executor applies its policy to (spu_job_classify_end). */
    const spu_job_end end = spu_job_classify_end(ctx);
    const std::uint32_t failed = spu_xdiff_recorder_end(&w.recorder, ctx, end.end_class, end.stop_code);
    if (failed != static_cast<std::uint32_t>(SPU_XDIFF_R_NONE)) {
        fail(w, image, w.fingerprint, failed, w.recorder.error.detail);
        spu_xdiff_fixture_free(&w.fixture); /* re-initialised by the next recording */
        return;
    }

    char name[64];
    spu_xdiff_fixture_name(name, sizeof name, w.fingerprint, w.queue, w.seq);
    std::string path = settings->dir;
    if (path.back() != '/') path += '/';
    path += name;
    spu_xdiff_error err{};
    const bool written = spu_xdiff_fixture_write(&w.fixture, path.c_str(), &err) == 0;
    if (!written) {
        fail(w, image, w.fingerprint, err.reason, err.detail);
    } else {
        image.captured.fetch_add(1, kRelaxed);
        if (w.unresolved) image.captured_unresolved.fetch_add(1, kRelaxed);
        std::fprintf(stderr, "[jq2-xdiff] captured image=%s queue=%u seq=%u end=%s path=%s\n",
                     fingerprint_text(w.fingerprint).c_str(), w.queue, w.seq, spu_xdiff_end_id(end.end_class), path.c_str());
    }
    if (!written || oversized(w.fixture)) spu_xdiff_fixture_free(&w.fixture);
}

namespace {

/* Statistics read for a report or a test: relaxed, there is nothing to order them against. */
CaptureCounters snapshot(const ImageCapture& image)
{
    CaptureCounters c;
    c.captured = image.captured.load(kRelaxed);
    c.captured_with_unresolved = image.captured_unresolved.load(kRelaxed);
    c.failed = image.failed.load(kRelaxed);
    c.ineligible = image.ineligible.load(kRelaxed);
    c.ineligible_no_inventory = image.no_inventory.load(kRelaxed);
    c.ineligible_event_branches = image.event_branches.load(kRelaxed);
    c.ineligible_unknown_body = image.unknown_body.load(kRelaxed);
    c.ineligible_table_full = image.table_full.load(kRelaxed);
    c.fingerprint_cache_misses = image.cache_misses.load(kRelaxed);
    c.ineligible_collision = image.collision.load(kRelaxed);
    return c;
}

}  // namespace

std::optional<CaptureCounters> capture_counters(std::uint64_t fingerprint)
{
    Table& t = table();
    const ImageCapture* image;
    {
        std::lock_guard<std::mutex> lk(t.mu);
        const auto it = t.images.find(fingerprint);
        if (it == t.images.end()) return std::nullopt;
        image = it->second.get();
    }
    return snapshot(*image);
}

CaptureCounters capture_unattributed() { return snapshot(table().unattributed); }

}  // namespace ps3recomp::spurs::jq2

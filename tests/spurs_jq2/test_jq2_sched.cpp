/*
 * T0: the Jq2 reference scheduler (libs/spurs/jq2/sched.hpp).
 *
 *   - property tests for C1', C2, C3', C4 over seeded random interleavings of
 *     push / intake_pass / complete (2 PM instances x 2 job slots),
 *   - the seven OQ-A cases of M1R-REPORT 3 (F1-F5, K1-K2) as scenarios that
 *     assert dispatch order and the read-index trajectory (tick numbers are not
 *     modelled),
 *   - the jq-boot main-port pattern,
 *   - ring bookkeeping (phase, wrap, capacity), the out-of-order mark and the
 *     wrap with flags&1 = 0 and 1 (OQ-F closed).
 *
 * Synthetic descriptors only; no guest or firmware bytes.
 */

#include <jq2/ring.hpp>
#include <jq2/sched.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <vector>

using namespace ps3recomp::spurs::jq2;

static int g_failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                          \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

namespace {

constexpr std::uint32_t kPortEa = 0x40000080u;
constexpr std::uint32_t kDescBase = 0x80000000u;

/* `flag` is the payload's flags&1: the port EA is present iff it is set. */
JobEntry make_job(unsigned tag, unsigned label, bool flag = true)
{
    return JobEntry{flag ? kPortEa : 0u, kDescBase + std::uint32_t(label) * 0x10u,
                    static_cast<std::uint8_t>(tag), SizeCode::bytes256};
}

unsigned label_of(const JobEntry& job)
{
    return (job.descriptor_ea - kDescBase) / 0x10u;
}

/* A scheduler plus the bookkeeping a driver needs: jobs in flight by label and
 * the dispatch order. */
struct Rig {
    Scheduler sched;
    std::map<unsigned, Dispatch> live;
    std::vector<unsigned> order; /* labels in dispatch order */

    explicit Rig(std::uint16_t depth, unsigned instances = 2, unsigned slots = 2)
        : sched(*Scheduler::create(SchedConfig{depth, instances, slots}))
    {
    }

    PushResult job(unsigned tag, unsigned label, bool flag = true)
    {
        return sched.push(make_job(tag, label, flag));
    }
    PushResult sync(std::uint16_t mask) { return sched.push(SyncEntry{mask}); }

    PassResult pass(unsigned instance)
    {
        PassResult r = sched.intake_pass(instance);
        if (r.dispatched) {
            const unsigned label = label_of(r.dispatched->job);
            CHECK(live.find(label) == live.end());
            live[label] = *r.dispatched;
            order.push_back(label);
        }
        return r;
    }

    bool done(unsigned label)
    {
        const auto found = live.find(label);
        CHECK(found != live.end());
        if (found == live.end()) return false;
        const bool ok = sched.complete(found->second.id);
        CHECK(ok);
        live.erase(found);
        return ok;
    }

    bool running(unsigned label) const { return live.find(label) != live.end(); }
    bool dispatched(unsigned label) const
    {
        for (unsigned l : order)
            if (l == label) return true;
        return false;
    }

    /* Run passes on `instance` until one makes no progress. */
    void settle_instance(unsigned instance)
    {
        for (;;) {
            const PassResult r = pass(instance);
            if (!r.dispatched && r.read_after == r.read_before && r.syncs_consumed == 0) return;
        }
    }

    /* Drive every instance to quiescence, completing the oldest in-flight job
     * whenever the PM cannot progress. */
    void drain()
    {
        for (;;) {
            bool progress = false;
            for (unsigned i = 0; i < sched.instances(); ++i) {
                const PassResult r = pass(i);
                if (r.dispatched || r.read_after != r.read_before || r.syncs_consumed != 0)
                    progress = true;
            }
            if (progress) continue;
            if (live.empty()) return;
            done(live.begin()->first);
        }
    }
};

/* splitmix64: fixed seed, no library PRNG differences between hosts. */
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed) {}
    std::uint64_t next()
    {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    unsigned below(unsigned n) { return static_cast<unsigned>(next() % n); }
};

/* ------------------------------------------------------------------------ */
/* OQ-A scenarios (M1R-REPORT 3)                                             */
/* ------------------------------------------------------------------------ */

struct OqaCase {
    const char* name;
    unsigned tag_a;
    unsigned tag_b;
    std::uint16_t mask;
    unsigned tag_c;
    bool b_first;           /* which of A/B is released first */
    unsigned read_after;    /* read index after the first release's pass: 3 or 4 */
    bool c_before_second;   /* C dispatches before the second release */
    bool c_in_first_pass;   /* ... in that very pass (else in the next one) */
};

void run_oqa(const OqaCase& c)
{
    constexpr unsigned A = 1, B = 2, C = 3;
    Rig rig(16);
    CHECK(rig.job(c.tag_a, A).ok());
    CHECK(rig.job(c.tag_b, B).ok());
    CHECK(rig.sync(c.mask).ok());
    CHECK(rig.job(c.tag_c, C).ok());

    /* Both PMs enter at once: one JOB per pass, one SPU each. */
    const PassResult p0 = rig.pass(0);
    CHECK(p0.end == PassEnd::job_dispatched);
    CHECK(p0.dispatched && label_of(p0.dispatched->job) == A && p0.dispatched->instance == 0);
    const PassResult p1 = rig.pass(1);
    CHECK(p1.end == PassEnd::job_dispatched);
    CHECK(p1.dispatched && label_of(p1.dispatched->job) == B && p1.dispatched->instance == 1);

    /* Read index 2 while A and B run, SYNC and C still in the ring. */
    CHECK(rig.sched.read_index() == 2);
    CHECK(rig.sched.write_index() == 4);

    const unsigned first = c.b_first ? B : A;
    const unsigned second = c.b_first ? A : B;
    const unsigned first_inst = c.b_first ? 1 : 0;

    CHECK(rig.done(first));
    const PassResult q = rig.pass(first_inst);
    CHECK(rig.sched.read_index() == c.read_after);
    CHECK(q.read_after == c.read_after);
    if (c.c_in_first_pass) {
        CHECK(q.end == PassEnd::job_dispatched);
        CHECK(q.dispatched && label_of(q.dispatched->job) == C);
    } else {
        CHECK(!q.dispatched);
        CHECK(q.end == PassEnd::sync_incomplete);
        CHECK(q.syncs_consumed == 1);
    }
    CHECK(rig.running(second)); /* the second job is still in flight */

    rig.settle_instance(first_inst);
    if (c.c_before_second) {
        CHECK(rig.running(C));
        CHECK(rig.sched.read_index() == 4);
        CHECK(rig.running(second));
        CHECK((rig.order == std::vector<unsigned>{A, B, C}));
    } else {
        /* C carries a masked tag: retained in the ring, not consumed. */
        CHECK(!rig.dispatched(C));
        CHECK(rig.sched.read_index() == 3);
        CHECK(rig.sched.outstanding() == 1);
        const auto bytes = encode(make_job(c.tag_c, C), true);
        CHECK(bytes.ok() && rig.sched.ring_entry(3) == bytes.bytes);
        CHECK(rig.sched.barred_mask() != 0);

        CHECK(rig.done(second));
        const PassResult r = rig.pass(first_inst);
        CHECK(r.dispatched && label_of(r.dispatched->job) == C);
        CHECK(rig.sched.read_index() == 4);
        CHECK((rig.order == std::vector<unsigned>{A, B, C}));
    }

    rig.drain();
    CHECK(rig.sched.read_index() == rig.sched.write_index());
    CHECK(rig.sched.read_index() == 4);
    CHECK(rig.live.empty());
    CHECK(rig.sched.barred_mask() == 0);
    for (unsigned t = 0; t < kTagCount; ++t) CHECK(rig.sched.inflight(t) == 0);
    if (g_failures != 0) std::fprintf(stderr, "(oqa case %s)\n", c.name);
}

void test_oqa_cases()
{
    /* A tag1 long on spu0, B tag0 long on spu1, ring A B SYNC C. Mask bit t is
     * tag t, so SYNC(1) masks tag 0. */
    static const OqaCase cases[] = {
        /* name  tagA tagB mask tagC b_first read C-before-2nd C-in-1st-pass */
        {"F1", 1, 0, 0x1, 1, true, 4, true, true},    /* falsifier: not a drain */
        {"F2", 1, 0, 0x1, 1, false, 3, true, false},  /* masked B busy on the other SPU */
        {"F3", 1, 0, 0x2, 1, true, 3, false, false},  /* C waits for A */
        {"F4", 1, 0, 0x2, 1, false, 4, true, true},
        {"F5", 1, 0, 0x3, 1, true, 3, false, false},
        {"K1", 0, 1, 0x1, 1, true, 3, true, false},   /* inverse control */
        {"K2", 0, 1, 0x1, 1, false, 4, true, true},
    };
    for (const OqaCase& c : cases) run_oqa(c);
}

/* ------------------------------------------------------------------------ */
/* jq-boot main-port pattern                                                 */
/* ------------------------------------------------------------------------ */

void test_main_port_pattern()
{
    /* JOB tag0, SYNC mask1, JOB tag0, SYNC mask1, repeated: the PM keeps up
     * after every push, then once per frame. The ring ends each settle with
     * read == write; depth 8 wraps the ring many times. */
    for (const std::uint16_t depth : {std::uint16_t(8), std::uint16_t(128)}) {
        for (const bool settle_each_push : {true, false}) {
            Rig rig(depth);
            unsigned label = 0;
            for (unsigned frame = 0; frame < 64; ++frame) {
                for (unsigned part = 0; part < 4; ++part) {
                    const PushResult p = (part % 2 == 0) ? rig.job(0, label++) : rig.sync(1);
                    CHECK(p.ok());
                    if (settle_each_push) {
                        rig.drain();
                        CHECK(rig.sched.read_index() == rig.sched.write_index());
                    }
                }
                rig.drain();
                CHECK(rig.sched.read_index() == rig.sched.write_index());
                CHECK(rig.sched.outstanding() == 0);
                CHECK(rig.live.empty());
                CHECK(rig.sched.barred_mask() == 0);
            }
            CHECK(rig.order.size() == 128);
            for (unsigned i = 0; i < rig.order.size(); ++i) CHECK(rig.order[i] == i);
        }
    }

    /* Hand-driven first frame: the second tag-0 JOB and its SYNC are retained
     * until the first JOB ends. */
    Rig rig(16);
    CHECK(rig.job(0, 1).ok());
    CHECK(rig.sync(1).ok());
    CHECK(rig.job(0, 2).ok());
    CHECK(rig.sync(1).ok());

    PassResult r = rig.pass(0);
    CHECK(r.dispatched && label_of(r.dispatched->job) == 1);
    CHECK(rig.sched.read_index() == 1);
    r = rig.pass(1); /* SYNC: tag 0 busy -> consumed, barrier, pass ends */
    CHECK(!r.dispatched && r.end == PassEnd::sync_incomplete && r.syncs_consumed == 1);
    CHECK(rig.sched.read_index() == 2);
    CHECK(rig.sched.barred_mask() == 0x1 && rig.sched.barrier_mask(0) == 0x1);
    r = rig.pass(1); /* JOB 2 retained; the second SYNC re-arms later */
    CHECK(!r.dispatched && r.end == PassEnd::ring_not_written);
    CHECK(r.entries_retained == 2 && r.read_after == 2);
    CHECK(rig.done(1));
    CHECK(rig.sched.barred_mask() == 0);
    r = rig.pass(1);
    CHECK(r.dispatched && label_of(r.dispatched->job) == 2);
    CHECK(rig.sched.read_index() == 3);
    r = rig.pass(1);
    CHECK(r.end == PassEnd::sync_incomplete && rig.sched.read_index() == 4);
    CHECK(rig.done(2));
    CHECK(rig.sched.barred_mask() == 0);
    CHECK(rig.sched.read_index() == rig.sched.write_index());
}

/* ------------------------------------------------------------------------ */
/* Property tests                                                            */
/* ------------------------------------------------------------------------ */

struct ShadowEntry {
    bool sync = false;
    unsigned tag = 0;
    std::uint16_t mask = 0;
    bool dispatched = false;
    bool completed = false;
    bool consumed = false;       /* SYNC: consumed at admission */
    std::uint16_t fresh = 0;     /* SYNC: tags it newly barred */
};

struct Property {
    Rig rig;
    std::vector<ShadowEntry> log; /* index == push sequence == job label */
    std::uint64_t last_read_abs = 0;
    std::uint64_t seed;

    Property(std::uint16_t depth, std::uint64_t s) : rig(depth), seed(s) {}

    bool fail(const char* what)
    {
        std::fprintf(stderr, "FAIL property seed %llu: %s\n",
                     static_cast<unsigned long long>(seed), what);
        ++g_failures;
        return false;
    }

    std::uint64_t read_abs() const { return log.size() - rig.sched.outstanding(); }

    /* Record which SYNCs the last pass consumed and what each one barred. The
     * window starts at the read index before the pass: a consumed SYNC is
     * still readable in its slot until the writer reuses it. */
    void observe_syncs(std::uint64_t from)
    {
        const Scheduler& s = rig.sched;
        for (std::uint64_t seq = from; seq < log.size(); ++seq) {
            ShadowEntry& e = log[seq];
            if (!e.sync || e.consumed) continue;
            const std::uint16_t fresh = s.sync_barred_tags(static_cast<std::uint16_t>(seq % s.depth()));
            if (fresh == 0) continue;
            e.consumed = true;
            e.fresh = fresh;
        }
    }

    bool on_dispatch(const Dispatch& d)
    {
        const unsigned label = label_of(d.job);
        if (label >= log.size() || log[label].sync) return fail("dispatch of unknown label");
        ShadowEntry& job = log[label];
        if (job.dispatched) return fail("job dispatched twice");
        const unsigned tag = job.tag;
        for (unsigned q = 0; q < label; ++q) {
            const ShadowEntry& e = log[q];
            /* C1'/C3': same-tag order preserved. */
            if (!e.sync && e.tag == tag && !e.dispatched)
                return fail("same-tag job overtaken");
        }
        for (unsigned s = 0; s < label; ++s) {
            if (!log[s].sync || !log[s].consumed || (log[s].fresh & (1u << tag)) == 0) continue;
            /* C2/C3': a SYNC barred this tag at admission, so no later job of
             * the tag runs while a job admitted before the SYNC is in flight.
             * (A tag the SYNC found already barred is held by the older
             * barrier only; see the sched.hpp note.) */
            for (unsigned q = 0; q < s; ++q) {
                const ShadowEntry& e = log[q];
                if (!e.sync && e.tag == tag && !e.completed)
                    return fail("barred-tag job dispatched behind an unfinished job");
            }
        }
        job.dispatched = true;
        return true;
    }

    /* After a pass that ran to the write index nothing admissible is left. */
    bool check_no_admissible_left()
    {
        const Scheduler& s = rig.sched;
        for (unsigned k = 0; k < s.outstanding(); ++k) {
            const auto index = static_cast<std::uint16_t>((s.read_index() + k) % s.depth());
            if (s.consumed_out_of_order(index)) continue;
            const Decoded d = decode(s.ring_entry(index));
            if (const auto* job = std::get_if<JobEntry>(&d.entry)) {
                if ((s.barred_mask() & (1u << job->tag)) == 0)
                    return fail("unbarred-tag job left in the ring (waited on a barrier)");
            } else if (const auto* sync = std::get_if<SyncEntry>(&d.entry)) {
                if ((sync->mask & ~s.barred_mask()) != 0)
                    return fail("admissible SYNC left in the ring");
            } else {
                return fail("non-JOB/SYNC entry in the ring");
            }
        }
        return true;
    }

    bool check_state()
    {
        const Scheduler& s = rig.sched;
        /* read index monotonic (absolute) and never past an undispatched job */
        const std::uint64_t r = read_abs();
        if (r < last_read_abs) return fail("read index went backwards");
        last_read_abs = r;
        for (std::uint64_t i = 0; i < r; ++i)
            if (!log[i].sync && !log[i].dispatched)
                return fail("read index passed an undispatched (retained) job");
        if (s.read_index() != r % s.depth()) return fail("read index / outstanding mismatch");

        /* per-tag in-flight counts match the shadow */
        std::array<unsigned, kTagCount> inflight{};
        for (const ShadowEntry& e : log)
            if (!e.sync && e.dispatched && !e.completed) ++inflight[e.tag];
        for (unsigned t = 0; t < kTagCount; ++t)
            if (s.inflight(t) != inflight[t]) return fail("in-flight count mismatch");

        /* slots: free + resident == slots per instance */
        std::array<unsigned, 8> resident{};
        for (const auto& kv : rig.live) ++resident[kv.second.instance];
        for (unsigned i = 0; i < s.instances(); ++i)
            if (s.free_slots(i) + resident[i] != s.slots_per_instance())
                return fail("slot accounting");

        /* a barred tag has a recorded barrier covering it; others have none */
        for (unsigned t = 0; t < kTagCount; ++t) {
            const bool barred = (s.barred_mask() & (1u << t)) != 0;
            if (barred != (s.barrier_mask(t) != 0)) return fail("barrier bookkeeping");
            if (barred && (s.barrier_mask(t) & (1u << t)) == 0)
                return fail("barrier does not cover its own tag");
        }
        return true;
    }

    bool run(unsigned ops, unsigned ntags)
    {
        Rng rng(seed * 0x1000003ull + 17);
        for (unsigned op = 0; op < ops; ++op) {
            const unsigned pick = rng.below(100);
            if (pick < 40) {
                const bool is_sync = rng.below(100) < 30;
                PushResult p;
                ShadowEntry e;
                if (is_sync) {
                    std::uint16_t mask = static_cast<std::uint16_t>(rng.next() & ((1u << ntags) - 1u));
                    if (mask == 0) mask = static_cast<std::uint16_t>(1u << rng.below(ntags));
                    e.sync = true;
                    e.mask = mask;
                    p = rig.sync(mask);
                } else {
                    e.tag = rng.below(ntags);
                    p = rig.job(e.tag, static_cast<unsigned>(log.size()));
                }
                if (p.error == PushError::ring_full) {
                    if (rig.sched.outstanding() != rig.sched.depth()) return fail("spurious ring_full");
                } else if (!p.ok()) {
                    return fail("push rejected");
                } else {
                    log.push_back(e);
                }
            } else if (pick < 75) {
                const unsigned inst = rng.below(rig.sched.instances());
                std::uint32_t inflight_before = 0;
                for (unsigned t = 0; t < kTagCount; ++t) inflight_before += rig.sched.inflight(t);
                const std::uint64_t read_before = read_abs();
                const PassResult r = rig.sched.intake_pass(inst);
                observe_syncs(read_before);
                std::uint32_t inflight_after = 0;
                for (unsigned t = 0; t < kTagCount; ++t) inflight_after += rig.sched.inflight(t);
                if (inflight_after - inflight_before != (r.dispatched ? 1u : 0u))
                    return fail("more than one job per pass");
                if (r.dispatched) {
                    rig.live[label_of(r.dispatched->job)] = *r.dispatched;
                    rig.order.push_back(label_of(r.dispatched->job));
                    if (!on_dispatch(*r.dispatched)) return false;
                }
                if (r.end == PassEnd::ring_not_written && !check_no_admissible_left()) return false;
                if (r.end == PassEnd::unsupported_entry || r.end == PassEnd::invalid_instance)
                    return fail("unexpected pass end");
            } else if (!rig.live.empty()) {
                auto it = rig.live.begin();
                std::advance(it, rng.below(static_cast<unsigned>(rig.live.size())));
                const unsigned label = it->first;
                if (!rig.sched.complete(it->second.id)) return fail("complete failed");
                log[label].completed = true;
                rig.live.erase(it);
            }
            if (!check_state()) return false;
        }

        /* Drain: every pushed JOB is dispatched exactly once and completes. */
        for (;;) {
            bool progress = false;
            for (unsigned i = 0; i < rig.sched.instances(); ++i) {
                const std::uint64_t read_before = read_abs();
                const PassResult r = rig.sched.intake_pass(i);
                observe_syncs(read_before);
                if (r.dispatched) {
                    rig.live[label_of(r.dispatched->job)] = *r.dispatched;
                    if (!on_dispatch(*r.dispatched)) return false;
                }
                if (r.dispatched || r.read_after != r.read_before || r.syncs_consumed != 0)
                    progress = true;
                if (!check_state()) return false;
            }
            if (progress) continue;
            if (rig.live.empty()) break;
            auto it = rig.live.begin();
            if (!rig.sched.complete(it->second.id)) return fail("complete failed");
            log[it->first].completed = true;
            rig.live.erase(it);
        }
        if (rig.sched.outstanding() != 0 || rig.sched.read_index() != rig.sched.write_index())
            return fail("ring did not drain");
        if (rig.sched.barred_mask() != 0) return fail("barrier left after drain");
        for (const ShadowEntry& e : log)
            if (!e.sync && (!e.dispatched || !e.completed)) return fail("job lost");
        return true;
    }
};

void test_properties()
{
    for (std::uint64_t seed = 1; seed <= 400; ++seed) {
        const auto depth = static_cast<std::uint16_t>(2 + seed % 11);
        const unsigned ntags = (seed % 3 == 0) ? 16 : (seed % 3 == 1 ? 2 : 4);
        Property p(depth, seed);
        if (!p.run(500, ntags)) break;
    }
}

/* ------------------------------------------------------------------------ */
/* Ring bookkeeping and the assumption points                                */
/* ------------------------------------------------------------------------ */

void test_ring_bookkeeping()
{
    CHECK(!Scheduler::create(SchedConfig{0, 2, 2}));
    CHECK(!Scheduler::create(SchedConfig{4, 0, 2}));
    CHECK(!Scheduler::create(SchedConfig{4, 2, 0}));

    Rig rig(3);
    CHECK(rig.sched.write_phase() && rig.sched.read_phase());

    /* The writer stores its phase in w1 bit 0: 1 on the first lap. */
    CHECK(rig.job(0, 0).ok());
    CHECK(rig.sync(1).ok());
    CHECK(rig.job(1, 1).ok());
    CHECK(!rig.sched.write_phase()); /* wrapped */
    CHECK(rig.sched.write_index() == 0);
    for (std::uint16_t i = 0; i < 3; ++i) CHECK(decode(rig.sched.ring_entry(i)).writer_bit);
    CHECK(rig.job(2, 2).ok() == false);
    CHECK(rig.job(2, 2).error == PushError::ring_full);

    /* Rejections: COPY, unknown, SYNC(0), bad tag. */
    CHECK(rig.sched.push(CopyEntry{kPortEa, kDescBase, 0, SizeCode::bytes64}).error ==
          PushError::unsupported_entry);
    CHECK(rig.sched.push(UnknownEntry{}).error == PushError::unsupported_entry);
    CHECK(rig.sched.push(SyncEntry{0}).error == PushError::empty_sync_mask);
    CHECK(rig.sched.push(JobEntry{kPortEa, kDescBase, 16, SizeCode::bytes64}).error ==
          PushError::encode_failed);

    /* Drain one lap; the next entries are written with phase 0. */
    rig.drain();
    CHECK(rig.sched.outstanding() == 0);
    CHECK(!rig.sched.read_phase() && !rig.sched.write_phase());
    CHECK(rig.job(0, 3).ok());
    CHECK(!decode(rig.sched.ring_entry(0)).writer_bit);
    CHECK(rig.sync(1).ok());
    CHECK(!decode(rig.sched.ring_entry(1)).writer_bit);
    rig.drain();
    CHECK(rig.sched.read_index() == rig.sched.write_index());
    CHECK(rig.order.size() == 3);

    /* A pass on a bad instance or with no free slot examines nothing. */
    CHECK(rig.sched.intake_pass(2).end == PassEnd::invalid_instance);
    Rig busy(8, 1, 1);
    CHECK(busy.job(0, 0).ok());
    CHECK(busy.job(1, 1).ok());
    CHECK(busy.pass(0).end == PassEnd::job_dispatched);
    const PassResult full = busy.pass(0);
    CHECK(full.end == PassEnd::no_free_slot && !full.dispatched && full.read_after == 1);
    CHECK(!busy.sched.complete(9999));
}

/* mark_consumed_out_of_order: an entry dispatched behind a retained one is
 * remembered; the read index stops at the retained entry, and a later pass
 * moves it over the marked ones once they sit at the frontier. */
void test_out_of_order_consumption()
{
    Rig rig(16);
    CHECK(rig.job(0, 0).ok());   /* 0: A, tag 0 */
    CHECK(rig.sync(1).ok());     /* 1: SYNC(tag 0) */
    CHECK(rig.job(0, 2).ok());   /* 2: X, tag 0, barred */
    CHECK(rig.job(1, 3).ok());   /* 3: Y, tag 1 */
    CHECK(rig.job(2, 4).ok());   /* 4: Z, tag 2 */

    CHECK(rig.pass(0).dispatched.has_value()); /* A */
    PassResult r = rig.pass(0);                /* SYNC consumed, incomplete */
    CHECK(r.end == PassEnd::sync_incomplete && rig.sched.read_index() == 2);

    r = rig.pass(1); /* X retained, Y dispatched behind it */
    CHECK(r.dispatched && label_of(r.dispatched->job) == 3);
    CHECK(r.entries_retained == 1);
    CHECK(rig.sched.read_index() == 2); /* stops at the retained X */
    CHECK(rig.sched.consumed_out_of_order(3));
    CHECK(!rig.sched.consumed_out_of_order(2));
    CHECK(rig.sched.outstanding() == 3);

    r = rig.pass(1); /* Z dispatched behind X too; Y is not re-dispatched */
    CHECK(r.dispatched && label_of(r.dispatched->job) == 4);
    CHECK(rig.sched.read_index() == 2);
    CHECK(rig.sched.consumed_out_of_order(4));
    CHECK((rig.order == std::vector<unsigned>{0, 3, 4}));

    CHECK(rig.done(0));
    r = rig.pass(0); /* X now unbarred: dispatched in order */
    CHECK(r.dispatched && label_of(r.dispatched->job) == 2);
    CHECK(rig.sched.read_index() == 3); /* the marks are not looked ahead */
    CHECK(rig.sched.consumed_out_of_order(3) && rig.sched.consumed_out_of_order(4));
    r = rig.pass(0); /* the next pass skips Y and Z at the frontier */
    CHECK(!r.dispatched);
    CHECK(rig.sched.read_index() == 5);
    CHECK(rig.sched.outstanding() == 0);
    CHECK(!rig.sched.consumed_out_of_order(3) && !rig.sched.consumed_out_of_order(4));
}

/* Golden case 1 (firmware PM on the m1r harness, 2 SPUs): ring A(tag1, long),
 * SYNC(mask 2), C(tag1), D(tag0). D dispatches while C is retained; the read
 * index goes 0 -> 1 -> 2 -> (held at 2 while D runs and ends) -> 3 -> 4; the
 * ring bytes are not rewritten. */
void test_golden_out_of_order()
{
    constexpr unsigned A = 1, C = 3, D = 4;
    Rig rig(16);
    CHECK(rig.job(1, A).ok());
    CHECK(rig.sync(0x2).ok());
    CHECK(rig.job(1, C).ok());
    CHECK(rig.job(0, D).ok());
    const auto c_bytes = encode(make_job(1, C), true);
    const auto d_bytes = encode(make_job(0, D), true);

    CHECK(rig.sched.read_index() == 0);
    PassResult r = rig.pass(0);
    CHECK(r.dispatched && label_of(r.dispatched->job) == A && r.dispatched->instance == 0);
    CHECK(rig.sched.read_index() == 1);

    r = rig.pass(1); /* SYNC consumed, tag 1 busy: barrier, pass ends */
    CHECK(!r.dispatched && r.end == PassEnd::sync_incomplete);
    CHECK(rig.sched.read_index() == 2);
    r = rig.pass(1); /* C retained; D (tag 0) dispatched behind it */
    CHECK(r.dispatched && label_of(r.dispatched->job) == D && r.dispatched->instance == 1);
    CHECK(rig.sched.read_index() == 2);
    CHECK((rig.order == std::vector<unsigned>{A, D}));

    CHECK(rig.done(D)); /* D runs and ends; the read index is held at 2 */
    r = rig.pass(1);
    CHECK(!r.dispatched && rig.sched.read_index() == 2);
    CHECK(rig.sched.barred_mask() == 0x2);

    CHECK(rig.done(A));
    r = rig.pass(1); /* C dispatches ... */
    CHECK(r.dispatched && label_of(r.dispatched->job) == C);
    CHECK(rig.sched.read_index() == 3);
    r = rig.pass(1); /* ... and the next pass skips the already dispatched D */
    CHECK(!r.dispatched);
    CHECK(rig.sched.read_index() == 4);
    CHECK((rig.order == std::vector<unsigned>{A, D, C}));

    /* No ring entry was rewritten. */
    CHECK(c_bytes.ok() && rig.sched.ring_entry(2) == c_bytes.bytes);
    CHECK(d_bytes.ok() && rig.sched.ring_entry(3) == d_bytes.bytes);
}

/* Golden case 1b: C(tag1) and D(tag1) behind the barrier are both retained
 * while A runs; after A ends they dispatch strictly in FIFO order. */
void test_golden_same_tag_behind_barrier()
{
    constexpr unsigned A = 1, C = 3, D = 4;
    Rig rig(16);
    CHECK(rig.job(1, A).ok());
    CHECK(rig.sync(0x2).ok());
    CHECK(rig.job(1, C).ok());
    CHECK(rig.job(1, D).ok());

    CHECK(rig.pass(0).dispatched.has_value());
    CHECK(rig.pass(1).end == PassEnd::sync_incomplete);
    const PassResult held = rig.pass(1);
    CHECK(!held.dispatched && held.entries_retained == 2);
    CHECK(rig.sched.read_index() == 2);

    CHECK(rig.done(A));
    PassResult r = rig.pass(1);
    CHECK(r.dispatched && label_of(r.dispatched->job) == C);
    CHECK(rig.sched.read_index() == 3);
    r = rig.pass(1);
    CHECK(r.dispatched && label_of(r.dispatched->job) == D);
    CHECK(rig.sched.read_index() == 4);
    CHECK((rig.order == std::vector<unsigned>{A, C, D}));
}

/* One SPU, three unbarred jobs (firmware PM on the m1r harness): at most one
 * JOB per intake pass, the read index advances by one per pass, and the job
 * slots rotate (JobContext2 at 0x68D0, 0x6900, 0x68D0, ...). */
void test_single_spu_unbarred()
{
    Rig rig(16, 1, 2);
    CHECK(rig.job(0, 0).ok());
    CHECK(rig.job(1, 1).ok());
    CHECK(rig.job(2, 2).ok());

    PassResult r = rig.pass(0);
    CHECK(r.dispatched && r.dispatched->slot == 0 && r.read_after == 1);
    CHECK(rig.done(0));
    r = rig.pass(0);
    CHECK(r.dispatched && r.dispatched->slot == 1 && r.read_after == 2);
    CHECK(rig.done(1));
    r = rig.pass(0);
    CHECK(r.dispatched && r.dispatched->slot == 0 && r.read_after == 3);
    CHECK(rig.done(2));
    CHECK(rig.sched.read_index() == rig.sched.write_index());

    /* Both slots busy: the PM takes no further job until one ends. */
    Rig busy(16, 1, 2);
    for (unsigned i = 0; i < 3; ++i) CHECK(busy.job(i, i).ok());
    CHECK(busy.pass(0).dispatched.has_value());
    CHECK(busy.pass(0).dispatched.has_value());
    const PassResult none = busy.pass(0);
    CHECK(none.end == PassEnd::no_free_slot && !none.dispatched && none.read_after == 2);
}

/* Wrap (OQ-F closed, m1r-017). PM side, measured on the firmware PM: the write
 * index wraps to 0 and the phase toggles 1 -> 0; an entry whose w1 bit 0
 * differs from the expected phase ends the pass. Writer side: w1 bit 0 is the
 * phase only, so JOBs with flags&1 = 0 and 1 (port EA absent / present) both
 * pass a wrap, at depths 4, 8 and 16. */
void test_wrap_with_flag()
{
    CHECK(pm_accepts_phase(true, true));
    CHECK(pm_accepts_phase(false, false));
    CHECK(!pm_accepts_phase(true, false));
    CHECK(!pm_accepts_phase(false, true));

    for (const std::uint16_t depth : {std::uint16_t(4), std::uint16_t(8), std::uint16_t(16)}) {
        for (const bool flag : {false, true}) {
            Rig rig(depth);
            CHECK(rig.sched.write_phase() && rig.sched.read_phase());
            for (unsigned i = 0; i < depth; ++i) CHECK(rig.job(i % 16, i, flag).ok());
            CHECK(rig.sched.write_index() == 0 && !rig.sched.write_phase());
            CHECK(rig.sched.read_phase()); /* the PM has not passed the wrap yet */
            rig.drain();
            CHECK(rig.sched.read_index() == 0 && !rig.sched.read_phase());
            CHECK(rig.sched.outstanding() == 0);

            /* Lap 2, slot 0, phase 0: bit 0 is the phase whatever the flag. */
            CHECK(rig.job(2, 100, flag).ok());
            CHECK(!decode(rig.sched.ring_entry(0)).writer_bit);
            const PassResult r = rig.pass(0);
            CHECK(r.dispatched && label_of(r.dispatched->job) == 100);
            CHECK(rig.sched.read_index() == 1);
            CHECK(r.dispatched && r.dispatched->job.api_flag() == flag);
            rig.drain();
            CHECK(rig.dispatched(100));

            /* Two more laps, alternating flags, all admitted in order. */
            for (unsigned i = 0; i < 2u * depth; ++i) {
                CHECK(rig.job(i % 16, 200 + i, (i % 2) == 0).ok());
                rig.drain();
            }
            CHECK(rig.sched.read_index() == rig.sched.write_index());
            CHECK(rig.order.size() == depth + 1u + 2u * depth);
        }
    }
}

}  // namespace

int main()
{
    test_ring_bookkeeping();
    test_oqa_cases();
    test_main_port_pattern();
    test_out_of_order_consumption();
    test_golden_out_of_order();
    test_golden_same_tag_behind_barrier();
    test_single_spu_unbarred();
    test_wrap_with_flag();
    test_properties();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("jq2 scheduler: all checks passed\n");
    return 0;
}

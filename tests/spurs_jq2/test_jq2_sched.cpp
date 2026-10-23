/*
 * T0: the Jq2 reference scheduler (libs/spurs/jq2/sched.hpp).
 *
 *   - property tests for C1', C2, C3', C4 over seeded random interleavings of
 *     push / intake_pass / complete / writer-stall (2 PM instances x 2 job
 *     slots, ring depths 1-12) against an independent oracle: a second model
 *     that derives the read position, barred tags, what each SYNC barred, the
 *     out-of-order marks and the writer's phase bits from the pushed sequence
 *     and the dispatch/complete events alone (no Scheduler getter feeds it),
 *     compared with every Scheduler observer after every operation,
 *   - wrong-phase intake: an entry whose w1 bit 0 mismatches the expected phase
 *     ends the pass and is not dispatched until rewritten with the right phase,
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
#include <variant>
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

/* The oracle: a second model of the PM's queue-level behaviour, written from
 * the contract in sched.hpp and sharing no state or code with Scheduler. It
 * numbers entries by push sequence (absolute positions: the ring index is
 * position % depth, the phase is the initial phase flipped once per lap), keeps
 * its own per-tag in-flight counts and barrier set, and derives everything the
 * Scheduler reports from the pushed sequence and the dispatch/complete events
 * alone: the read position, the barred tags, what a SYNC barred, which entries
 * sit consumed behind a retained one, the phase bit the writer stored. The
 * property tests drive both and demand they agree after every operation; no
 * oracle value is read back from a Scheduler getter. */
struct RefEntry {
    Entry entry;
    bool sync = false;
    unsigned tag = 0;
    std::uint16_t mask = 0;
    bool written = true;    /* w1 bit 0 equals the phase the PM expects at this position */
    bool consumed = false;  /* JOB dispatched / SYNC admitted */
    bool dispatched = false;
    bool completed = false;
    std::uint16_t fresh = 0; /* SYNC: tags it newly barred */
    explicit RefEntry(const Entry& e) : entry(e) {}
};

struct RefPass {
    int label = -1; /* push sequence of the dispatched JOB, -1: none */
    unsigned instance = 0;
    unsigned slot = 0;
    PassEnd end = PassEnd::ring_not_written;
    unsigned syncs = 0;
    unsigned retained = 0;
    std::uint64_t read_before = 0;
    std::uint64_t read_after = 0;
};

struct RefModel {
    unsigned depth, instances, slots;
    std::vector<RefEntry> seq; /* every pushed entry; index == push sequence == JOB label */
    std::uint64_t read = 0;    /* absolute read position */
    std::array<unsigned, kTagCount> inflight{};
    std::uint16_t barred = 0;
    std::array<std::uint16_t, kTagCount> barrier{};
    std::vector<std::vector<bool>> busy; /* [instance][slot] */
    std::vector<unsigned> rotor;

    RefModel(unsigned d, unsigned i, unsigned s)
        : depth(d), instances(i), slots(s), busy(i, std::vector<bool>(s, false)), rotor(i, 0)
    {
    }

    std::uint64_t outstanding() const { return seq.size() - read; }
    bool phase_at(std::uint64_t pos) const { return kInitialPhase != (((pos / depth) & 1u) != 0); }

    void push(const Entry& e, bool written)
    {
        RefEntry r(e);
        if (const auto* sy = std::get_if<SyncEntry>(&e)) {
            r.sync = true;
            r.mask = sy->mask;
        } else {
            r.tag = std::get<JobEntry>(e).tag;
        }
        r.written = written;
        seq.push_back(r);
    }

    /* The oldest pushed entry whose write has not completed, or -1. */
    long unwritten() const
    {
        for (std::uint64_t s = read; s < seq.size(); ++s)
            if (!seq[s].written) return static_cast<long>(s);
        return -1;
    }

    /* The bytes the writer left in a pushed position. */
    std::array<std::uint8_t, kEntryBytes> stored(std::uint64_t pos) const
    {
        const bool phase = seq[pos].written ? phase_at(pos) : !phase_at(pos);
        return encode(seq[pos].entry, phase).bytes;
    }

    /* The newest pushed position that lives in ring slot `index`, or -1. */
    long latest_in_slot(unsigned index) const
    {
        if (seq.size() <= index) return -1;
        return static_cast<long>(index + ((seq.size() - 1 - index) / depth) * depth);
    }

    RefPass pass(unsigned instance)
    {
        RefPass r;
        r.read_before = r.read_after = read;
        if (instance >= instances) { r.end = PassEnd::invalid_instance; return r; }
        unsigned free_slots = 0;
        for (bool b : busy[instance]) free_slots += b ? 0u : 1u;
        if (free_slots == 0) { r.end = PassEnd::no_free_slot; return r; }

        bool behind_retained = false;
        std::uint64_t advance_to = read;
        r.end = PassEnd::ring_not_written;
        for (std::uint64_t s = read; s < seq.size(); ++s) {
            RefEntry& e = seq[s];
            if (!e.written) break; /* stall: the writer has not finished this entry */
            if (e.consumed) {      /* consumed by an earlier pass, behind a since-resolved retained entry */
                if (!behind_retained) advance_to = s + 1;
                continue;
            }
            if (!e.sync) {
                if ((barred & (1u << e.tag)) != 0) {
                    behind_retained = true;
                    ++r.retained;
                    continue;
                }
                unsigned slot = rotor[instance];
                while (busy[instance][slot]) slot = (slot + 1) % slots;
                rotor[instance] = (slot + 1) % slots;
                busy[instance][slot] = true;
                ++inflight[e.tag];
                e.consumed = e.dispatched = true;
                if (!behind_retained) advance_to = s + 1;
                r.label = static_cast<int>(s);
                r.instance = instance;
                r.slot = slot;
                r.end = PassEnd::job_dispatched;
                break;
            }
            const std::uint16_t fresh = static_cast<std::uint16_t>(e.mask & ~barred);
            if (fresh == 0) { /* every masked tag already barred: retained, re-armed later */
                behind_retained = true;
                ++r.retained;
                continue;
            }
            const bool covered_barred = (e.mask & barred) != 0;
            for (unsigned t = 0; t < kTagCount; ++t)
                if ((fresh & (1u << t)) != 0) barrier[t] = e.mask;
            barred = static_cast<std::uint16_t>(barred | fresh);
            e.fresh = fresh;
            bool complete_now = true;
            for (unsigned t = 0; t < kTagCount; ++t)
                if ((e.mask & (1u << t)) != 0 && (inflight[t] != 0 || barrier[t] != e.mask))
                    complete_now = false;
            if (complete_now) {
                barred = static_cast<std::uint16_t>(barred & ~e.mask);
                for (unsigned t = 0; t < kTagCount; ++t)
                    if ((e.mask & (1u << t)) != 0) barrier[t] = 0;
            }
            e.consumed = true;
            ++r.syncs;
            if (!behind_retained) advance_to = s + 1;
            if (!complete_now || covered_barred) { r.end = PassEnd::sync_incomplete; break; }
        }
        read = advance_to;
        r.read_after = read;
        return r;
    }

    void complete(unsigned label, unsigned instance, unsigned slot)
    {
        RefEntry& e = seq[label];
        e.completed = true;
        --inflight[e.tag];
        busy[instance][slot] = false;
        for (unsigned t = 0; t < kTagCount; ++t) {
            if ((barred & (1u << t)) == 0) continue;
            bool idle = true;
            for (unsigned u = 0; u < kTagCount; ++u)
                if ((barrier[t] & (1u << u)) != 0 && inflight[u] != 0) idle = false;
            if (idle) {
                barred = static_cast<std::uint16_t>(barred & ~(1u << t));
                barrier[t] = 0;
            }
        }
    }
};

struct Property {
    Rig rig;
    RefModel ref;
    std::uint64_t seed;

    Property(std::uint16_t depth, std::uint64_t s) : rig(depth), ref(depth, 2, 2), seed(s) {}

    bool fail(const char* what)
    {
        std::fprintf(stderr, "FAIL property seed %llu depth %u: %s\n",
                     static_cast<unsigned long long>(seed), ref.depth, what);
        ++g_failures;
        return false;
    }

    /* What the Scheduler said about one pass, against the oracle. */
    bool pass_matches(const PassResult& got, const RefPass& want)
    {
        if (got.end != want.end) return fail("pass end differs from the oracle");
        if (got.syncs_consumed != want.syncs) return fail("SYNCs consumed differ from the oracle");
        if (got.entries_retained != want.retained) return fail("retained entries differ from the oracle");
        if (got.read_before != want.read_before % ref.depth ||
            got.read_after != want.read_after % ref.depth)
            return fail("read index trajectory differs from the oracle");
        if (got.dispatched.has_value() != (want.label >= 0)) return fail("dispatch differs from the oracle");
        if (got.dispatched) {
            const Dispatch& d = *got.dispatched;
            if (label_of(d.job) != static_cast<unsigned>(want.label)) return fail("dispatched the wrong job");
            if (d.instance != want.instance || d.slot != want.slot) return fail("instance/slot differ from the oracle");
            if (d.ring_index != static_cast<unsigned>(want.label) % ref.depth)
                return fail("dispatched from the wrong ring slot");
        }
        return true;
    }

    /* Every Scheduler observer against the oracle. */
    bool state_matches()
    {
        const Scheduler& s = rig.sched;
        const std::uint64_t pushed = ref.seq.size();
        if (s.read_index() != ref.read % ref.depth) return fail("read index differs from the oracle");
        if (s.write_index() != pushed % ref.depth) return fail("write index differs from the oracle");
        if (s.read_phase() != ref.phase_at(ref.read)) return fail("read phase differs from the oracle");
        if (s.write_phase() != ref.phase_at(pushed)) return fail("write phase differs from the oracle");
        if (s.outstanding() != ref.outstanding()) return fail("outstanding differs from the oracle");
        if (s.barred_mask() != ref.barred) return fail("barred tags differ from the oracle");
        for (unsigned t = 0; t < kTagCount; ++t) {
            if (s.inflight(t) != ref.inflight[t]) return fail("in-flight count differs from the oracle");
            const bool barred = (ref.barred & (1u << t)) != 0;
            if (s.barrier_mask(t) != (barred ? ref.barrier[t] : 0u))
                return fail("barrier mask differs from the oracle");
        }
        for (unsigned i = 0; i < s.instances(); ++i) {
            unsigned free_slots = 0;
            for (bool b : ref.busy[i]) free_slots += b ? 0u : 1u;
            if (s.free_slots(i) != free_slots) return fail("free slots differ from the oracle");
        }
        for (unsigned index = 0; index < ref.depth; ++index) {
            const long pos = ref.latest_in_slot(index);
            const auto idx = static_cast<std::uint16_t>(index);
            const bool sits_consumed = pos >= 0 && static_cast<std::uint64_t>(pos) >= ref.read &&
                                       ref.seq[pos].consumed;
            if (s.consumed_out_of_order(idx) != sits_consumed)
                return fail("out-of-order mark differs from the oracle");
            const std::uint16_t fresh = pos >= 0 ? ref.seq[pos].fresh : std::uint16_t(0);
            if (s.sync_barred_tags(idx) != fresh) return fail("SYNC barred tags differ from the oracle");
            if (pos >= 0 && s.ring_entry(idx) != ref.stored(static_cast<std::uint64_t>(pos)))
                return fail("ring bytes differ from what the writer stored");
        }
        /* structural properties of the oracle's own trajectory */
        for (std::uint64_t i = 0; i < ref.read; ++i)
            if (!ref.seq[i].sync && !ref.seq[i].dispatched)
                return fail("read index passed an undispatched (retained) job");
        return true;
    }

    bool on_dispatch(unsigned label)
    {
        const unsigned tag = ref.seq[label].tag;
        for (unsigned q = 0; q < label; ++q) {
            const RefEntry& e = ref.seq[q];
            /* C1'/C3': same-tag order preserved. */
            if (!e.sync && e.tag == tag && !e.dispatched) return fail("same-tag job overtaken");
        }
        for (unsigned s = 0; s < label; ++s) {
            const RefEntry& sy = ref.seq[s];
            if (!sy.sync || !sy.consumed || (sy.fresh & (1u << tag)) == 0) continue;
            /* C2/C3': a SYNC barred this tag at admission, so no later job of
             * the tag runs while a job admitted before the SYNC is in flight.
             * (A tag the SYNC found already barred is held by the older
             * barrier only; see the sched.hpp note.) */
            for (unsigned q = 0; q < s; ++q) {
                const RefEntry& e = ref.seq[q];
                if (!e.sync && e.tag == tag && !e.completed)
                    return fail("barred-tag job dispatched behind an unfinished job");
            }
        }
        return true;
    }

    /* After a pass that ran to the write index (or to an unwritten entry) nothing
     * admissible is left before it. */
    bool check_no_admissible_left()
    {
        for (std::uint64_t s = ref.read; s < ref.seq.size(); ++s) {
            const RefEntry& e = ref.seq[s];
            if (!e.written) break;
            if (e.consumed) continue;
            if (!e.sync) {
                if ((ref.barred & (1u << e.tag)) == 0)
                    return fail("unbarred-tag job left in the ring (waited on a barrier)");
            } else if ((e.mask & ~ref.barred) != 0) {
                return fail("admissible SYNC left in the ring");
            }
        }
        return true;
    }

    PassResult last; /* the Scheduler's report for the most recent do_pass */

    bool do_pass(unsigned instance)
    {
        const RefPass want = ref.pass(instance);
        const PassResult got = rig.sched.intake_pass(instance);
        last = got;
        if (!pass_matches(got, want)) return false;
        if (got.dispatched) {
            rig.live[want.label] = *got.dispatched;
            rig.order.push_back(static_cast<unsigned>(want.label));
            if (!on_dispatch(static_cast<unsigned>(want.label))) return false;
        }
        if (got.end == PassEnd::ring_not_written && !check_no_admissible_left()) return false;
        if (got.end == PassEnd::unsupported_entry || got.end == PassEnd::invalid_instance)
            return fail("unexpected pass end");
        return true;
    }

    bool do_complete(unsigned label)
    {
        const Dispatch d = rig.live.at(label);
        if (!rig.sched.complete(d.id)) return fail("complete failed");
        ref.complete(label, d.instance, d.slot);
        rig.live.erase(label);
        return true;
    }

    /* The writer finishes the oldest write the PM saw as not yet written. */
    bool finish_write()
    {
        const long s = ref.unwritten();
        if (s < 0) return true;
        const auto index = static_cast<std::uint16_t>(static_cast<std::uint64_t>(s) % ref.depth);
        if (!rig.sched.rewrite_entry(index, ref.seq[s].entry, ref.phase_at(static_cast<std::uint64_t>(s))))
            return fail("rewrite of a pushed, unconsumed entry refused");
        ref.seq[s].written = true;
        return true;
    }

    /* rewrite_entry is for pushed entries nobody consumed yet, nothing else. */
    bool check_rewrite_refusals(Rng& rng)
    {
        if (ref.outstanding() < ref.depth) {
            const auto free_index = static_cast<std::uint16_t>(ref.seq.size() % ref.depth);
            if (rig.sched.rewrite_entry(free_index, make_job(0, 0), true))
                return fail("rewrite of a slot that is not outstanding accepted");
        }
        for (std::uint64_t s = ref.read; s < ref.seq.size(); ++s)
            if (ref.seq[s].consumed) {
                const auto index = static_cast<std::uint16_t>(s % ref.depth);
                if (rig.sched.rewrite_entry(index, make_job(rng.below(16), 0), ref.phase_at(s)))
                    return fail("rewrite of a consumed entry accepted");
                break;
            }
        return true;
    }

    bool do_push(Rng& rng, unsigned ntags)
    {
        const bool is_sync = rng.below(100) < 30;
        const bool stalled = rng.below(100) < 12; /* the writer has stored the wrong phase so far */
        Entry entry = make_job(0, 0);
        if (is_sync) {
            std::uint16_t mask = static_cast<std::uint16_t>(rng.next() & ((1u << ntags) - 1u));
            if (mask == 0) mask = static_cast<std::uint16_t>(1u << rng.below(ntags));
            entry = SyncEntry{mask};
        } else {
            entry = make_job(rng.below(ntags), static_cast<unsigned>(ref.seq.size()));
        }
        const std::uint64_t pos = ref.seq.size();
        const bool phase = ref.phase_at(pos) != stalled;
        const PushResult p = rig.sched.push_with_phase(entry, phase);
        if (ref.outstanding() == ref.depth) {
            if (p.error != PushError::ring_full) return fail("push into a full ring not refused");
            return true;
        }
        if (!p.ok()) return fail("push rejected");
        if (p.index != pos % ref.depth) return fail("push stored at the wrong slot");
        ref.push(entry, !stalled);
        return true;
    }

    bool run(unsigned ops, unsigned ntags)
    {
        Rng rng(seed * 0x1000003ull + 17);
        for (unsigned op = 0; op < ops; ++op) {
            const unsigned pick = rng.below(100);
            if (pick < 40) {
                if (!do_push(rng, ntags)) return false;
            } else if (pick < 75) {
                if (!do_pass(rng.below(rig.sched.instances()))) return false;
            } else if (pick < 84) {
                if (!finish_write() || !check_rewrite_refusals(rng)) return false;
            } else if (!rig.live.empty()) {
                auto it = rig.live.begin();
                std::advance(it, rng.below(static_cast<unsigned>(rig.live.size())));
                if (!do_complete(it->first)) return false;
            }
            if (!state_matches()) return false;
        }

        /* Drain: every write finishes, every pushed JOB is dispatched exactly once and completes. */
        while (ref.unwritten() >= 0)
            if (!finish_write() || !state_matches()) return false;
        for (;;) {
            bool progress = false;
            for (unsigned i = 0; i < rig.sched.instances(); ++i) {
                if (!do_pass(i) || !state_matches()) return false;
                if (last.dispatched || last.read_after != last.read_before || last.syncs_consumed != 0)
                    progress = true;
            }
            if (progress) continue;
            if (rig.live.empty()) break;
            if (!do_complete(rig.live.begin()->first) || !state_matches()) return false;
        }
        if (ref.outstanding() != 0 || rig.sched.outstanding() != 0 ||
            rig.sched.read_index() != rig.sched.write_index())
            return fail("ring did not drain");
        if (ref.barred != 0 || rig.sched.barred_mask() != 0) return fail("barrier left after drain");
        for (const RefEntry& e : ref.seq)
            if (!e.sync && (!e.dispatched || !e.completed)) return fail("job lost");
        return true;
    }
};

void test_properties()
{
    for (std::uint64_t seed = 1; seed <= 400; ++seed) {
        const auto depth = static_cast<std::uint16_t>(1 + seed % 12);
        const unsigned ntags = (seed % 3 == 0) ? 16 : (seed % 3 == 1 ? 2 : 4);
        Property p(depth, seed);
        if (!p.run(500, ntags)) break;
    }
}

/* ------------------------------------------------------------------------ */
/* Wrong-phase intake (the PM's phase rule, M1R-REPORT 8)                    */
/* ------------------------------------------------------------------------ */

/* push() always stores the expected phase, so the PM's mismatch path needs
 * the push_with_phase / rewrite_entry hooks: an entry whose w1 bit 0 differs
 * from the phase the PM expects ends the pass, is not dispatched or consumed
 * and is not stepped over, and is picked up once the writer's bit is right. */
void test_wrong_phase_intake()
{
    constexpr bool kRight = kInitialPhase;
    constexpr bool kWrong = !kInitialPhase;

    /* A lone JOB with the wrong phase: every pass ends at it. */
    {
        Rig rig(4);
        CHECK(rig.sched.push_with_phase(make_job(0, 0), kWrong).ok());
        CHECK(decode(rig.sched.ring_entry(0)).writer_bit == kWrong);
        for (unsigned k = 0; k < 3; ++k) {
            const PassResult r = rig.pass(k % 2);
            CHECK(r.end == PassEnd::ring_not_written && !r.dispatched);
            CHECK(r.read_before == 0 && r.read_after == 0 && r.syncs_consumed == 0);
            CHECK(rig.sched.outstanding() == 1 && rig.sched.inflight(0) == 0);
        }
        CHECK(!rig.dispatched(0));
        CHECK(rig.sched.rewrite_entry(0, make_job(0, 0), kRight));
        CHECK(decode(rig.sched.ring_entry(0)).writer_bit == kRight);
        const PassResult r = rig.pass(0);
        CHECK(r.end == PassEnd::job_dispatched && r.dispatched && label_of(r.dispatched->job) == 0);
        CHECK(r.read_after == 1);
    }

    /* In the middle of the ring: A ok, B wrong, C ok. C is valid but behind B,
     * so it is not examined; the read index and the dispatch order stop at B. */
    {
        Rig rig(8);
        CHECK(rig.job(0, 0).ok());
        CHECK(rig.sched.push_with_phase(make_job(1, 1), kWrong).ok());
        CHECK(rig.job(2, 2).ok());
        CHECK(rig.pass(0).end == PassEnd::job_dispatched);
        for (unsigned k = 0; k < 4; ++k) {
            const PassResult r = rig.pass(k % 2);
            CHECK(r.end == PassEnd::ring_not_written && !r.dispatched);
            CHECK(r.read_after == 1);
        }
        CHECK(rig.order.size() == 1 && !rig.dispatched(1) && !rig.dispatched(2));
        CHECK(!rig.sched.consumed_out_of_order(2));
        CHECK(rig.sched.rewrite_entry(1, make_job(1, 1), kRight));
        CHECK(rig.pass(1).end == PassEnd::job_dispatched);
        CHECK(rig.pass(1).end == PassEnd::job_dispatched);
        CHECK((rig.order == std::vector<unsigned>{0, 1, 2}));
    }

    /* A SYNC with the wrong phase is not admitted: it bars nothing and is not consumed. */
    {
        Rig rig(8);
        CHECK(rig.job(0, 0).ok());
        CHECK(rig.sched.push_with_phase(SyncEntry{1}, kWrong).ok());
        CHECK(rig.job(0, 2).ok());
        CHECK(rig.pass(0).end == PassEnd::job_dispatched);
        const PassResult r = rig.pass(1);
        CHECK(r.end == PassEnd::ring_not_written && r.syncs_consumed == 0 && !r.dispatched);
        CHECK(rig.sched.barred_mask() == 0 && rig.sched.sync_barred_tags(1) == 0);
        CHECK(rig.sched.rewrite_entry(1, SyncEntry{1}, kRight));
        const PassResult s = rig.pass(1);   /* tag 0 is in flight: consumed, incomplete */
        CHECK(s.end == PassEnd::sync_incomplete && s.syncs_consumed == 1);
        CHECK(rig.sched.barred_mask() == 1 && rig.sched.sync_barred_tags(1) == 1);
    }

    /* Second lap: the expected phase has flipped, so the previous lap's bit is stale. */
    {
        Rig rig(3);
        for (unsigned i = 0; i < 3; ++i) CHECK(rig.job(i, i).ok());
        rig.drain();
        CHECK(!rig.sched.read_phase() && !rig.sched.write_phase());
        CHECK(rig.sched.push_with_phase(make_job(3, 3), true).ok());   /* lap 1's bit at slot 0 */
        const PassResult r = rig.pass(0);
        CHECK(r.end == PassEnd::ring_not_written && !r.dispatched && r.read_after == 0);
        CHECK(rig.sched.rewrite_entry(0, make_job(3, 3), false));
        CHECK(rig.pass(0).end == PassEnd::job_dispatched);
    }

    /* Behind a retained entry: A(tag1) runs, SYNC(2) bars tag 1, B(tag1) is
     * retained, C(tag0) has the wrong phase. The scan goes past B (counting it
     * retained), ends at C; once C's phase is right it dispatches behind B. */
    {
        Rig rig(16);
        CHECK(rig.job(1, 0).ok());
        CHECK(rig.sync(2).ok());
        CHECK(rig.job(1, 1).ok());
        CHECK(rig.sched.push_with_phase(make_job(0, 2), kWrong).ok());
        CHECK(rig.pass(0).end == PassEnd::job_dispatched);                  /* A */
        CHECK(rig.pass(1).end == PassEnd::sync_incomplete);                 /* SYNC bars tag 1 */
        const PassResult r = rig.pass(1);
        CHECK(r.end == PassEnd::ring_not_written && r.entries_retained == 1 && !r.dispatched);
        CHECK(r.read_after == 2 && !rig.dispatched(2));
        CHECK(rig.sched.rewrite_entry(3, make_job(0, 2), kRight));
        const PassResult c = rig.pass(1);
        CHECK(c.end == PassEnd::job_dispatched && c.dispatched && label_of(c.dispatched->job) == 2);
        CHECK(c.read_after == 2 && rig.sched.consumed_out_of_order(3));
        rig.done(0);
        CHECK(rig.pass(0).end == PassEnd::job_dispatched);                  /* B, released */
        CHECK((rig.order == std::vector<unsigned>{0, 2, 1}));
    }

    /* The hook's refusals: not an outstanding slot, a consumed entry, an unstorable entry. */
    {
        Rig rig(4);
        CHECK(rig.job(0, 0).ok());
        CHECK(!rig.sched.rewrite_entry(1, make_job(0, 1), kRight));         /* not pushed */
        CHECK(!rig.sched.rewrite_entry(4, make_job(0, 1), kRight));         /* off the ring */
        CHECK(!rig.sched.rewrite_entry(0, SyncEntry{0}, kRight));
        CHECK(!rig.sched.rewrite_entry(0, CopyEntry{kPortEa, kDescBase, 0, SizeCode::bytes64}, kRight));
        CHECK(!rig.sched.rewrite_entry(0, make_job(16, 0), kRight));        /* tag out of range */
        CHECK(rig.sched.push_with_phase(SyncEntry{0}, kRight).error == PushError::empty_sync_mask);
        CHECK(rig.sched.outstanding() == 1);
        CHECK(rig.pass(0).end == PassEnd::job_dispatched);
        CHECK(!rig.sched.rewrite_entry(0, make_job(0, 0), kRight));         /* already passed */
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
    test_wrong_phase_intake();
    test_properties();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("jq2 scheduler: all checks passed\n");
    return 0;
}

/* Cases for the synthetic firmware-free fixture. Every case compares the
 * synthetic HLE (synth_hle.cpp) against the lifted synthetic module
 * (module.c / module_cpp.cpp / module_asm.S). The wrong-NID handlers exist to
 * prove one comparator dimension each; tools/diff_harness/test_synth.py also
 * runs the binary with that dimension disabled and requires the case to go
 * undetected, so the dimension cannot rot silently. */
#include "dh_engine.h"
#include "ppu/ppu_context.h"

using namespace dh;

namespace {
constexpr uint32_t N_HELPER      = 0x91000100;
constexpr uint32_t N_RET_OK      = 0x91000001;
constexpr uint32_t N_WRITE_OK    = 0x91000002;
constexpr uint32_t N_IMPORT_OK   = 0x91000003;
constexpr uint32_t N_FAULT_OK    = 0x91000004;
constexpr uint32_t N_HANG        = 0x91000005;
constexpr uint32_t N_ASM_ADD     = 0x91000006;
constexpr uint32_t N_CPP_MUL     = 0x91000007;
constexpr uint32_t N_NO_HLE      = 0x91000008;
constexpr uint32_t N_RET_WRONG   = 0x91000011;
constexpr uint32_t N_WRITE_MISS  = 0x91000012;
constexpr uint32_t N_PAD_EXTRA   = 0x91000013;
constexpr uint32_t N_IMPORT_SWAP = 0x91000014;
constexpr uint32_t N_FAULT_SPAN  = 0x91000015;
constexpr uint32_t N_FAULT_RET   = 0x91000016;
constexpr uint32_t N_FAULT_ZERO  = 0x91000017;
constexpr uint32_t N_MASK_ADJ    = 0x91000018;
constexpr uint32_t N_OVR_OK      = 0x91000021;
constexpr uint32_t DST = 0x00040000, SRC = 0x00041000;
constexpr uint32_t FAULT_EA = 0x90000000u;

std::vector<Case>* g_out;

void add(const char* id, const char* func, uint32_t nid, std::initializer_list<uint64_t> args,
         Expect e, const char* cause = "", const char* note = "", std::function<void()> setup = nullptr,
         int timeout = 10)
{
    Case c;
    c.id = id; c.func = func; c.nid = nid; c.expect = e; c.expect_cause = cause; c.note = note;
    c.setup = std::move(setup);
    c.timeout_s = timeout;
    int i = 0;
    for (uint64_t a : args) if (i < 8) c.args[i++] = a;
    g_out->push_back(std::move(c));
}

/* Replaces the lifted module's internal function: the firmware twin computes 2a, this answers
 * 3a, which is what makes the HLE's 3a+1 and the lifted export's helper(a)+1 agree. With the
 * test-only "mutate-override" flag it answers 3a+1 instead, which must flip the verdict. */
bool ovr_internal(void* v)
{
    ppu_context* c = (ppu_context*)v;
    c->gpr[3] = (uint32_t)(c->gpr[3] * 3u + (case_flag("mutate-override") ? 1u : 0u));
    return true;
}
}  // namespace

namespace dh {
ImportPolicy import_policy()
{
    ImportPolicy p;
    p.dflt = Policy::Stop;
    p.by_nid[N_HELPER] = Policy::Forward;   /* the helper is the same HLE handler on both sides */
    return p;
}

void register_cases(std::vector<Case>& out)
{
    g_out = &out;
    override_symbol("synth.internal_91000021", ovr_internal);
    if (case_flag("bogus-override")) override_symbol("synth.no_such", ovr_internal);   /* test_synth.py: fail closed */

    /* matched executions */
    add("ret_match", "synth_ret", N_RET_OK, {7}, Expect::Match, "return");
    add("write_match", "synth_write", N_WRITE_OK, {DST}, Expect::Match, "return");
    add("import_match", "synth_import", N_IMPORT_OK, {3, 9}, Expect::Match, "return",
        "matches only after a forwarded import; the report must mark it common_mode");
    add("asm_match", "synth_asm_add", N_ASM_ADD, {20, 22}, Expect::Match, "return");
    add("cpp_match", "synth_cpp_mul", N_CPP_MUL, {6, 7}, Expect::Match, "return");
    add("nohle", "synth_no_hle", N_NO_HLE, {1}, Expect::NoHle, "",
        "the module exports the NID, the HLE does not register a handler");

    /* one deliberate mutation per comparator dimension */
    add("ret_wrong", "synth_ret_wrong", N_RET_WRONG, {7}, Expect::Diverge, "return");
    add("write_missing", "synth_write_missing", N_WRITE_MISS, {DST}, Expect::Diverge, "memory");
    add("pad_extra", "synth_pad_extra", N_PAD_EXTRA, {DST, SRC}, Expect::Diverge, "memory",
        "8 copied bytes agree; the HLE adds 24 zero bytes the firmware never writes", [] {
            for (int i = 0; i < 8; ++i) wr8(SRC + i, (uint8_t)(0x11 + i));
        });
    add("import_swap", "synth_import_swap", N_IMPORT_SWAP, {3, 9}, Expect::Diverge, "imports",
        "the helper returns 0 and touches nothing; only the argument order differs");
    add("mask_adjacent", "synth_mask_adjacent", N_MASK_ADJ, {DST, SRC}, Expect::Diverge, "memory",
        "the case masks DST+8..+16 only; the HLE's zero padding at +16..+32 must still diverge", [] {
            for (int i = 0; i < 8; ++i) wr8(SRC + i, (uint8_t)(0x21 + i));
        });
    /* mask only the first 8 bytes of the 24 the HLE pads */
    out.back().mask = {{DST + 8, 8}};
    add("fault_match", "synth_fault", N_FAULT_OK, {FAULT_EA}, Expect::Match, "fault",
        "HLE range stop and lifted guest-fault at the same EA");
    add("fault_span", "synth_fault_span", N_FAULT_SPAN, {FAULT_EA}, Expect::Inconclusive, "fault-span",
        "two guest faults at the same step whose spans do not overlap");
    add("override_match", "synth_override", N_OVR_OK, {7}, Expect::Match, "return",
        "matches only because the internal helper is overridden: the module alone computes 2a+1");
    add("fault_zero", "synth_fault_zero", N_FAULT_ZERO, {FAULT_EA}, Expect::Inconclusive, "fault-span",
        "a zero-length HLE range covers nothing and cannot pair with the lifted fault");
    add("fault_return", "synth_fault_return", N_FAULT_RET, {FAULT_EA}, Expect::Diverge, "status",
        "the HLE returns an error where the firmware faults");
    add("hang", "synth_hang", N_HANG, {0}, Expect::Inconclusive, "hang",
        "both sides time out; two timeouts are inconclusive, never a match", nullptr, 1);

    /* Causal witnesses: every case is a single call, and a memory divergence
     * must start in the span its mutation writes. */
    for (auto& c : out) c.expect_step = 0;
    for (auto& c : out) {
        if (c.id == "write_missing") { c.expect_range_lo = DST; c.expect_range_hi = DST + 4; }
        if (c.id == "pad_extra") { c.expect_range_lo = DST + 8; c.expect_range_hi = DST + 32; }
        if (c.id == "mask_adjacent") { c.expect_range_lo = DST + 16; c.expect_range_hi = DST + 32; }
    }
}
}  // namespace dh

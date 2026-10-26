/* Cases for the synthetic module (syn_spec.h): the same table runs against
 * every implementation of the spec -- C, C++ and assembly lifted from a
 * cross-compiled image, and the buggy C one -- with ref_native.c as the HLE side.
 *
 * Built with SYN_VARIANT_BUG the table is the same, but the cases marked `bug`
 * are declared to diverge: they are exactly the ones that reach one of the three
 * deliberate bugs of syn_c_bug.c, and every other case has to stay a match. The
 * witness names the cause and, for a memory divergence, the field the bug writes.
 *
 * Built with SYN_VARIANT_JT_GAP (images compiled so that the switch of synObjCommand is
 * dispatched through a table shape tools/ppu_lifter.py does not recognise yet, issue #45)
 * the cases marked `sw` -- those that reach that switch -- are declared lifted-blocked: the
 * lifted code stops at an indirect call to a case label that was never lifted. Each carries
 * the observed blocker and step so it can neither pass for another reason nor hide. The
 * declaration turns into an "unexpected" the day #45 is fixed, which is the point: flip
 * these cases to Match then. */
#include "dh_engine.h"
#include "syn_nids.h"
#include "syn_spec.h"
#include <cstring>

using namespace dh;

namespace {
constexpr uint32_t OBJ = 0x30000, NAME = 0x31000, BUF = 0x40000, OUT = 0x38000;
constexpr uint32_t BAD_EA = 0xFFFFFFF0u;
#ifdef SYN_VARIANT_BUG
constexpr bool kBugBuild = true;
#else
constexpr bool kBugBuild = false;
#endif
#ifdef SYN_VARIANT_JT_GAP
constexpr bool kJtGap = true;
#else
constexpr bool kJtGap = false;
#endif

std::vector<Case>* g_out;

/* An initialised object, written field by field from the spec (not by calling either side). */
void put_obj(uint32_t ea, uint16_t flags, uint32_t cap, uint32_t count, uint64_t stamp, uint32_t magic = SYN_MAGIC)
{
    wr32(ea + SYN_O_MAGIC, magic); wr16(ea + SYN_O_VERSION, SYN_VERSION); wr16(ea + SYN_O_FLAGS, flags);
    wr32(ea + SYN_O_CAPACITY, cap); wr32(ea + SYN_O_COUNT, count); wr64(ea + SYN_O_STAMP, stamp);
}
void put_str(uint32_t ea, const char* s) { memcpy(mem + ea, s, strlen(s) + 1); }   /* NUL, then the arena's garbage */

const char* const B_GAP = "known lifter gap #45: the switch dispatch (table base + index then lwz, "
                          "or an addis/addi TOC base) is not recognised, so the case labels are not lifted";
void declare(Case& c, bool bug, bool sw, const char* note, int sw_step)
{
    c.expect = Expect::Match;
    if (bug && kBugBuild) { c.expect = Expect::Diverge; c.note = note; }
    if (sw && kJtGap) {
        c.expect = Expect::Blocked; c.note = B_GAP;
        c.expect_cause = "blocked"; c.expect_step = sw_step;
        c.blocker = "unresolved-jump-table"; c.blocker_step = sw_step;
    }
}
void add_case(const char* id, uint32_t nid, const char* func, std::initializer_list<uint64_t> a,
              std::function<void()> setup, bool bug, const char* note, bool sw = false)
{
    Case c; c.id = id; c.func = func; c.nid = nid; c.setup = std::move(setup);
    int i = 0; for (uint64_t v : a) c.args[i++] = v;
    declare(c, bug, sw, note, 0);
    g_out->push_back(std::move(c));
}
using Setup = std::function<void()>;
void init(const char* id, uint32_t obj, uint32_t name, uint32_t flags, uint32_t cap, Setup s = nullptr,
          bool bug = false, const char* note = "")
{ add_case(id, SYN_NID_synObjInit, "synObjInit", {obj, name, flags, cap}, std::move(s), bug, note); }
void cmd(const char* id, uint32_t obj, uint32_t c, uint32_t arg, Setup s = nullptr, bool bug = false, const char* note = "")
{ add_case(id, SYN_NID_synObjCommand, "synObjCommand", {obj, c, arg}, std::move(s), bug, note); }
/* scmd/spush: the call reaches the switch of synObjCommand (valid object, command 0..7) */
void scmd(const char* id, uint32_t obj, uint32_t c, uint32_t arg, Setup s = nullptr, bool bug = false, const char* note = "")
{ add_case(id, SYN_NID_synObjCommand, "synObjCommand", {obj, c, arg}, std::move(s), bug, note, true); }
void push(const char* id, uint32_t obj, uint32_t n, Setup s = nullptr, bool sw = false)
{ add_case(id, SYN_NID_synObjPush, "synObjPush", {obj, n}, std::move(s), false, "", sw); }
void cksum(const char* id, uint32_t buf, uint32_t words, uint32_t out, Setup s = nullptr)
{ add_case(id, SYN_NID_synBufChecksum, "synBufChecksum", {buf, words, out}, std::move(s), false, ""); }

/* a 64-bit-result call: the whole of r3 is compared */
void bits(const char* id, uint64_t a, uint64_t b, uint32_t c)
{
    Case k; k.id = id; k.func = "synBits"; k.nid = SYN_NID_synBits;
    Step s = step("synBits", SYN_NID_synBits, {Arg(a), Arg(b), Arg(c)}); s.ret64 = true;
    k.steps.push_back(s);
    g_out->push_back(std::move(k));
}
Step st(const char* func, uint32_t nid, std::initializer_list<Arg> a) { return step(func, nid, a); }

void seq(const char* id, std::vector<Step> steps, Setup s = nullptr, bool bug = false, const char* note = "", bool sw = true)
{
    Case c; c.id = id; c.func = steps.back().func; c.nid = steps.back().nid; c.steps = std::move(steps); c.setup = std::move(s);
    declare(c, bug, sw, note, 1);   /* step 0 is always a synObjInit; the switch is reached at step 1 */
    g_out->push_back(std::move(c));
}

constexpr const char* B_ORDER = "BUG: capacity checked before flags (wrong error for flags AND capacity bad)";
constexpr const char* B_LIMIT = "BUG: capacity 0x1000 rejected (off by one)";
constexpr const char* B_WIDE  = "BUG: PACK stores a 32-bit product in the 64-bit field";

/* Witnesses for the declared non-Match verdicts (V1): the comparator cause and
 * step, the memory range the first differing byte must lie in, and for a
 * lifted block its reason. A declared witness that does not reproduce makes
 * the case unexpected, so a bug or a lifter gap cannot pass for another reason. */
void witness(const char* id, const char* cause, int step, uint32_t lo = 0, uint32_t hi = 0)
{
    for (auto& c : *g_out)
        if (c.id == id) { c.expect_cause = cause; c.expect_step = step; c.expect_range_lo = lo; c.expect_range_hi = hi; return; }
}
void witness_blocker(const char* id, const char* blocker, int step)
{
    for (auto& c : *g_out)
        if (c.id == id) { c.expect_cause = "blocked"; c.expect_step = step; c.blocker = blocker; c.blocker_step = step; return; }
}
}  // namespace

namespace dh {
/* The one import (synHostMemset) is a pure host helper: run for real, through the
 * registered handler, on the lifted side too. Anything else would stop the call. */
ImportPolicy import_policy()
{
    ImportPolicy p;
    p.dflt = Policy::Stop;
    p.by_nid[SYN_NID_synHostMemset] = Policy::Forward;
    return p;
}

void register_cases(std::vector<Case>& out)
{
    g_out = &out;

    /* ---- synObjInit: argument checks and their order ---------------------- */
    init("init01_null_obj",           0,         NAME, 0,    4);
    init("init02_misaligned_1",       OBJ + 1,   NAME, 0,    4);
    init("init03_misaligned_8",       OBJ + 8,   NAME, 0,    4);
    init("init04_align_before_flags", OBJ + 4,   NAME, 0x40, 0);
    init("init05_null_before_flags",  0,         NAME, 0x40, 0);
    init("init06_flags_before_cap",   OBJ,       NAME, 0x40, 0, nullptr, true, B_ORDER);
    init("init07_flags_high_bit",     OBJ,       0,    0x80000000u, 4);
    init("init08_flags_3f_ok",        OBJ,       0,    0x3F, 4);
    init("init09_flags_40",           OBJ,       0,    0x40, 4);
    init("init10_cap_0",              OBJ,       0,    0,    0);
    init("init11_cap_1",              OBJ,       0,    1,    1);
    init("init12_cap_1000_max",       OBJ,       0,    0,    SYN_CAP_MAX, nullptr, true, B_LIMIT);
    init("init13_cap_1001",           OBJ,       0,    0,    SYN_CAP_MAX + 1);
    init("init14_cap_ffffffff",       OBJ,       0,    0,    0xFFFFFFFFu);
    init("init15_cap_bad_flags_ok",   OBJ,       0,    0x15, 0xFFFFu);

    /* ---- synObjInit: what it writes --------------------------------------- */
    init("init20_fields_stamp",       OBJ,       0,    0x15, 0x123);
    init("init21_flags_zero_stamp",   OBJ,       0,    0,    0x800);
    init("init22_16_aligned_other",   OBJ + 0x10, 0,   0x2A, 7);
    init("init23_name_null",          OBJ,       0,    1,    3);
    init("init24_name_short_pad",     OBJ,       NAME, 1,    3, [] { put_str(NAME, "ab"); });
    init("init25_name_empty",         OBJ,       NAME, 1,    3, [] { put_str(NAME, ""); });
    init("init26_name_23_chars",      OBJ,       NAME, 1,    3, [] { put_str(NAME, "ABCDEFGHIJKLMNOPQRSTUVW"); });
    init("init27_name_24_chars",      OBJ,       NAME, 1,    3, [] { memcpy(mem + NAME, "ABCDEFGHIJKLMNOPQRSTUVWX", 24); });
    init("init28_name_long",          OBJ,       NAME, 1,    3, [] { put_str(NAME, "this name is far longer than the field can take"); });
    init("init29_name_high_bytes",    OBJ,       NAME, 1,    3, [] { memcpy(mem + NAME, "\x80\xFF\xA5\x01z", 6); });
    init("init30_reserved_zeroed",    OBJ,       NAME, 2,    9, [] { put_str(NAME, "x"); fill(OBJ + 0x30, 0x20, 0xEE); });
    init("init31_beyond_object",      OBJ,       NAME, 2,    9, [] { put_str(NAME, "y"); fill(OBJ - 16, 16, 0x77); fill(OBJ + 0x50, 16, 0x77); });
    init("init32_over_old_object",    OBJ,       NAME, 3,    5, [] { put_obj(OBJ, 0x3F, 99, 77, ~0ull); put_str(NAME, "new"); });

    /* ---- synObjCommand: checks and their order ---------------------------- */
    auto good = [](uint16_t fl = 0x05, uint32_t cap = 10, uint32_t count = 3, uint64_t stamp = 0x0000000A00000003ull) {
        return [=] { put_obj(OBJ, fl, cap, count, stamp); };
    };
    cmd("cmd01_null_obj",             0,         0, 0);
    cmd("cmd02_misaligned",           OBJ + 4,   0, 0, good());
    cmd("cmd03_align_before_state",   OBJ + 8,   0, 0);
    cmd("cmd04_state_garbage",        OBJ,       0, 0);
    cmd("cmd05_state_before_cmd",     OBJ,       99, 0);
    cmd("cmd06_bad_magic_cmd_ok",     OBJ,       1, 1, [] { put_obj(OBJ, 0, 10, 0, 0, SYN_MAGIC ^ 1); });
    cmd("cmd07_cmd_8",                OBJ,       8, 0, good());
    cmd("cmd08_cmd_ffffffff",         OBJ,       0xFFFFFFFFu, 0, good());
    cmd("cmd09_obj_oob",              BAD_EA,    0, 0);

    /* ---- synObjCommand: the jump table, every case and its edges ---------- */
    scmd("cmd10_query",                OBJ,       SYN_CMD_QUERY, 123, good(0x05, 10, 5));
    scmd("cmd11_add_ok",               OBJ,       SYN_CMD_ADD, 4, good());
    scmd("cmd12_add_to_capacity",      OBJ,       SYN_CMD_ADD, 7, good());
    scmd("cmd13_add_over_capacity",    OBJ,       SYN_CMD_ADD, 8, good());
    scmd("cmd14_add_u32_wrap",         OBJ,       SYN_CMD_ADD, 0xFFFFFFFFu, good(0x05, 0xFFFFFFF0u, 5));
    scmd("cmd15_add_corrupt_count",    OBJ,       SYN_CMD_ADD, 0, good(0, 0x1000, 0x2000));
    scmd("cmd16_add_stamp_carry",      OBJ,       SYN_CMD_ADD, 1, good(0, 10, 3, 0x00000000FFFFFFFFull));
    scmd("cmd17_add_stamp_wrap64",     OBJ,       SYN_CMD_ADD, 2, good(0, 10, 3, 0xFFFFFFFFFFFFFFFFull));
    scmd("cmd18_add_zero",             OBJ,       SYN_CMD_ADD, 0, good());
    scmd("cmd20_sub_ok",               OBJ,       SYN_CMD_SUB, 2, good());
    scmd("cmd21_sub_to_zero",          OBJ,       SYN_CMD_SUB, 3, good());
    scmd("cmd22_sub_underflow",        OBJ,       SYN_CMD_SUB, 4, good());
    scmd("cmd23_sub_huge",             OBJ,       SYN_CMD_SUB, 0x80000000u, good());
    scmd("cmd30_clear",                OBJ,       SYN_CMD_CLEAR, 55, good(0x05, 10, 6, 0x1122334455667788ull));
    scmd("cmd40_setflags_ok",          OBJ,       SYN_CMD_SETFLAGS, 0x2A, good(0x15));
    scmd("cmd41_setflags_40",          OBJ,       SYN_CMD_SETFLAGS, 0x40, good(0x15));
    scmd("cmd42_setflags_10000",       OBJ,       SYN_CMD_SETFLAGS, 0x10000, good(0x15));
    scmd("cmd43_setflags_old_ffff",    OBJ,       SYN_CMD_SETFLAGS, 0, good(0xFFFF));
    scmd("cmd50_stamphi",              OBJ,       SYN_CMD_STAMPHI, 0, good(0, 10, 3, 0x1234567890ABCDEFull));
    scmd("cmd51_stamphi_sign",         OBJ,       SYN_CMD_STAMPHI, 0, good(0, 10, 3, 0xFFFFFFF012345678ull));
    scmd("cmd60_pack_wide",            OBJ,       SYN_CMD_PACK, 0x12345678u, good(), true, B_WIDE);
    scmd("cmd61_pack_one",             OBJ,       SYN_CMD_PACK, 1, good());       /* K < 2^32: the bug cannot show */
    scmd("cmd62_pack_zero",            OBJ,       SYN_CMD_PACK, 0, good());
    scmd("cmd63_pack_ffffffff",        OBJ,       SYN_CMD_PACK, 0xFFFFFFFFu, good(), true, B_WIDE);
    scmd("cmd70_rotate_0",             OBJ,       SYN_CMD_ROTATE, 0, good(0, 10, 3, 0x8000000000000001ull));
    scmd("cmd71_rotate_1",             OBJ,       SYN_CMD_ROTATE, 1, good(0, 10, 3, 0x8000000000000001ull));
    scmd("cmd72_rotate_13",            OBJ,       SYN_CMD_ROTATE, 13, good(0, 10, 3, 0x0123456789ABCDEFull));
    scmd("cmd73_rotate_63",            OBJ,       SYN_CMD_ROTATE, 63, good(0, 10, 3, 0x0123456789ABCDEFull));
    scmd("cmd74_rotate_64_is_0",       OBJ,       SYN_CMD_ROTATE, 64, good(0, 10, 3, 0x0123456789ABCDEFull));
    scmd("cmd75_rotate_c5",            OBJ,       SYN_CMD_ROTATE, 0xFFFFFFC5u, good(0, 10, 3, 0x0123456789ABCDEFull));

    /* ---- synObjPush: calls synObjCommand (another export) ----------------- */
    push("push01_ok",                 OBJ,       4, good(), true);
    push("push02_over_capacity",      OBJ,       8, good(), true);
    push("push03_bad_state",          OBJ,       1);
    push("push04_null",               0,         1);
    push("push05_misaligned",         OBJ + 2,   1, good());
    push("push06_seq_counter",        OBJ,       2, [] { put_obj(OBJ, 0, 10, 0, 0); wr32(OBJ + SYN_O_PUSH_SEQ, 0xFFFFFFFFu); }, true);

    /* ---- sequences: state carried from one call to the next --------------- */
    seq("seq01_init_push_push_over",
        {st("synObjInit", SYN_NID_synObjInit, {Arg(OBJ), Arg(NAME), Arg(1), Arg(4)}),
         st("synObjPush", SYN_NID_synObjPush, {Arg(OBJ), Arg(2)}),
         st("synObjPush", SYN_NID_synObjPush, {Arg(OBJ), Arg(2)}),
         st("synObjPush", SYN_NID_synObjPush, {Arg(OBJ), Arg(1)}),
         st("synObjCommand", SYN_NID_synObjCommand, {Arg(OBJ), Arg(SYN_CMD_QUERY), Arg(0)})},
        [] { put_str(NAME, "seq"); });
    seq("seq02_init_max_cap_fill",
        {st("synObjInit", SYN_NID_synObjInit, {Arg(OBJ), Arg(0), Arg(0), Arg(SYN_CAP_MAX)}),
         st("synObjCommand", SYN_NID_synObjCommand, {Arg(OBJ), Arg(SYN_CMD_ADD), Arg(SYN_CAP_MAX)}),
         st("synObjCommand", SYN_NID_synObjCommand, {Arg(OBJ), Arg(SYN_CMD_ADD), Arg(1)}),
         st("synObjCommand", SYN_NID_synObjCommand, {Arg(OBJ), Arg(SYN_CMD_STAMPHI), Arg(0)})},
        nullptr, true, B_LIMIT);
    seq("seq03_pack_rotate_clear",
        {st("synObjInit", SYN_NID_synObjInit, {Arg(OBJ), Arg(0), Arg(0x3F), Arg(0x40)}),
         st("synObjCommand", SYN_NID_synObjCommand, {Arg(OBJ), Arg(SYN_CMD_PACK), Arg(0xCAFEF00Du)}),
         st("synObjCommand", SYN_NID_synObjCommand, {Arg(OBJ), Arg(SYN_CMD_ROTATE), Arg(21)}),
         st("synObjCommand", SYN_NID_synObjCommand, {Arg(OBJ), Arg(SYN_CMD_CLEAR), Arg(0)}),
         st("synObjCommand", SYN_NID_synObjCommand, {Arg(OBJ), Arg(SYN_CMD_STAMPHI), Arg(0)})},
        nullptr, true, B_WIDE);
    seq("seq04_reinit_over_used",
        {st("synObjInit", SYN_NID_synObjInit, {Arg(OBJ), Arg(NAME), Arg(2), Arg(8)}),
         st("synObjPush", SYN_NID_synObjPush, {Arg(OBJ), Arg(5)}),
         st("synObjInit", SYN_NID_synObjInit, {Arg(OBJ), Arg(0), Arg(4), Arg(2)}),
         st("synObjCommand", SYN_NID_synObjCommand, {Arg(OBJ), Arg(SYN_CMD_QUERY), Arg(0)})},
        [] { put_str(NAME, "first"); });

    /* ---- synBufChecksum: a loop, 64-bit sums ------------------------------ */
    auto pattern = [](uint32_t words) {
        return [=] { uint32_t x = 0x9E3779B9u; for (uint32_t i = 0; i < words; ++i) { x = x * 1664525u + 1013904223u; wr32(BUF + 4 * i, x); } };
    };
    cksum("ck01_null_buf",            0,         4, OUT);
    cksum("ck02_null_out",            BUF,       4, 0);
    cksum("ck03_null_before_align",   0,         4, OUT + 4);
    cksum("ck04_null_before_range",   BUF,       0x4001, 0);
    cksum("ck05_buf_misaligned",      BUF + 2,   4, OUT);
    cksum("ck06_out_misaligned",      BUF,       4, OUT + 4);
    cksum("ck07_align_before_range",  BUF + 1,   0x4001, OUT);
    cksum("ck08_range_4001",          BUF,       0x4001, OUT);
    cksum("ck09_words_0",             BUF,       0, OUT);
    cksum("ck10_words_1",             BUF,       1, OUT, pattern(1));
    cksum("ck11_words_2",             BUF,       2, OUT, pattern(2));
    cksum("ck12_words_7_odd",         BUF,       7, OUT, pattern(7));
    cksum("ck13_words_1000",          BUF,       1000, OUT, pattern(1000));
    cksum("ck14_words_4000_max",      BUF,       SYN_WORDS_MAX, OUT, pattern(SYN_WORDS_MAX));
    cksum("ck15_all_ones_carry",      BUF,       100, OUT, [] { for (int i = 0; i < 100; ++i) wr32(BUF + 4 * i, 0xFFFFFFFFu); });
    cksum("ck16_out_inside_buf",      BUF,       16, BUF + 0x20, pattern(16));
    cksum("ck17_buf_oob",             BAD_EA,    4, OUT);

    /* ---- synBits: 64-bit registers in, 64-bit register out; a bit-op zoo ---- */
    bits("bits01_zero",               0, 0, 0);
    bits("bits02_ones",               1, 1, 1);
    bits("bits03_allbits",            0xFFFFFFFFFFFFFFFFull, 3, 5);
    bits("bits04_big",                0x123456789ABCDEF0ull, 0xFEDCBA9876543210ull, 0xFFFFFFFFu);
    bits("bits05_b_all_ones",         7, 0xFFFFFFFFFFFFFFFEull, 7);
    bits("bits06_a_high_bit",         0x8000000000000000ull, 2, 0);
    bits("bits07_small",              100, 200, 300);
    bits("bits08_negative_shifts",    0x8000000000000001ull, 5, 0xFFFFFFFFu);
    bits("bits09_sign_extends",       0x0000000000000080ull, 0x8000, 0x80000000u);
    bits("bits10_clz_1",              1, 0, 0);
    bits("bits11_clz_mid",            0x0000000100000000ull, 0x31, 0x7F);
    bits("bits12_c_int_min",          0x7F, 3, 0x80000000u);
    bits("bits13_rotate_31",          0xDEADBEEFCAFEBABEull, 31, 31);
    bits("bits14_rotate_32",          0xDEADBEEFCAFEBABEull, 32, 32);
    bits("bits15_signed_cmp",         0xFFFFFFFFFFFFFFFFull, 1, 1);
    bits("bits16_cmp_equal",          5, 5, 5);
    bits("bits17_mulh_max",           0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFu);
    bits("bits18_int64_min",          0x8000000000000000ull, 1, 1);
    bits("bits19_divisors",           0x00000000FFFFFFFFull, 0x7FFE, 0xFFFE);
    bits("bits20_word_products",      0xFFFFFFFF80000000ull, 0x00000000FFFFFFFFull, 3);

    /* ---- synFpu: floats and doubles through guest memory ----------------- */
    constexpr uint32_t FIN = 0x50000, FOUT = 0x51000;
    auto fin = [](double a, double b, float c, float d) {
        return [=] { uint64_t u; memcpy(&u, &a, 8); wr64(FIN, u); memcpy(&u, &b, 8); wr64(FIN + 8, u);
                     uint32_t w; memcpy(&w, &c, 4); wr32(FIN + 16, w); memcpy(&w, &d, 4); wr32(FIN + 20, w); };
    };
    auto fpu = [&](const char* id, uint32_t in, uint32_t out, Setup s) {
        add_case(id, SYN_NID_synFpu, "synFpu", {in, out}, std::move(s), false, ""); };
    fpu("fpu01_null_in",              0,       FOUT,     nullptr);
    fpu("fpu02_null_out",             FIN,     0,        fin(1, 2, 3, 4));
    fpu("fpu03_in_misaligned",        FIN + 4, FOUT,     nullptr);
    fpu("fpu04_out_misaligned",       FIN,     FOUT + 4, fin(1, 2, 3, 4));
    fpu("fpu05_null_before_align",    0,       FOUT + 4, nullptr);
    fpu("fpu06_b_zero",               FIN,     FOUT,     fin(1.5, 0.0, 1, 1));
    fpu("fpu07_b_negative_zero",      FIN,     FOUT,     fin(1.5, -0.0, 1, 1));
    fpu("fpu08_a_2p31",               FIN,     FOUT,     fin(2147483648.0, 3, 1, 1));
    fpu("fpu09_a_below_2p31",         FIN,     FOUT,     fin(2147483647.5, 3, 1, 1));
    fpu("fpu10_a_minus_2p31",         FIN,     FOUT,     fin(-2147483648.0, 3, 1, 1));
    fpu("fpu11_a_minus_below",        FIN,     FOUT,     fin(-2147483647.9, 3, 1, 1));
    fpu("fpu12_basic",                FIN,     FOUT,     fin(1.5, 2.25, 3.5f, -0.25f));
    fpu("fpu13_negative_mixed",       FIN,     FOUT,     fin(-7.125, 0.3, -1000.5f, 0.1f));
    fpu("fpu14_fma_is_fused",         FIN,     FOUT,     fin(1.0 + 0x1p-27, 1.0 + 0x1p-27, -1.0f, 0));
    fpu("fpu15_trunc_negative",       FIN,     FOUT,     fin(-3.9, 1, 0, 0));
    fpu("fpu16_trunc_positive",       FIN,     FOUT,     fin(3.9, 1, 0, 0));
    fpu("fpu17_a_zero",               FIN,     FOUT,     fin(0.0, -5.5, 0, 0));
    fpu("fpu18_cmp_less",             FIN,     FOUT,     fin(-1, 1, 0, 0));
    fpu("fpu19_cmp_equal",            FIN,     FOUT,     fin(2.5, 2.5, 0, 0));
    fpu("fpu20_cmp_greater",          FIN,     FOUT,     fin(9, 1, 0, 0));
    fpu("fpu21_single_rounding",      FIN,     FOUT,     fin(1, 1, 16777216.0f, 1.0f));
    fpu("fpu22_product_to_single",    FIN,     FOUT,     fin(1.0000001, 1.0000001, 1.0000001f, 1.0000001f));
    fpu("fpu23_huge_exponents",       FIN,     FOUT,     fin(1e300, 1e-300, 3e38f, 3e-38f));
    fpu("fpu24_out_inside_in",        FIN,     FIN + 8,  fin(1.5, 2.5, 3.5f, 4.5f));

    if (kBugBuild) {
        witness("init06_flags_before_cap", "return", 0);
        witness("init12_cap_1000_max", "return", 0);
        witness("cmd60_pack_wide", "memory", 0, OBJ + SYN_O_PACK_WIDE, OBJ + SYN_O_PACK_WIDE + 8);
        witness("cmd63_pack_ffffffff", "memory", 0, OBJ + SYN_O_PACK_WIDE, OBJ + SYN_O_PACK_WIDE + 8);
        witness("seq02_init_max_cap_fill", "return", 0);
        witness("seq03_pack_rotate_clear", "memory", 1, OBJ + SYN_O_PACK_WIDE, OBJ + SYN_O_PACK_WIDE + 8);
    }
}
}  // namespace dh

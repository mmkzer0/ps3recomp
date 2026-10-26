/* Cases for the cellSync2 semaphore entry points (libs/sync/cellSync2.c vs the
 * firmware's libsync2). Argument signatures follow the HLE prototypes, which are
 * the RPCS3 ones with guest pointers as u32:
 *
 *   s32 _cellSync2SemaphoreAttributeInitialize(u32 attr, u32 sdkVersion)
 *   s32 cellSync2SemaphoreEstimateBufferSize(u32 attr, u32 bufferSizeOut)   (no HLE yet)
 *   s32 cellSync2SemaphoreInitialize(u32 sem, u32 workBuffer, s32 initialValue, u32 attr)
 *
 * Attribute block layout (the HLE's, cellSync2.c:368): +0 sdk, +4 threadTypes
 * (u16), +6 maxWaiters (u16), +8 name[]. */
#include "dh_engine.h"
#include <cstring>

using namespace dh;

namespace {
constexpr uint32_t NID_ATTR = 0x2D77FE17, NID_EST = 0x74C2780F, NID_INIT = 0xC5DEE254;
constexpr uint32_t ATTR = 0x20000, SEM = 0x21000, OUT = 0x23000, BUF = 0x80000;
constexpr uint32_t BAD_EA = 0xFFFFFFF0u;   /* far outside the arena */

/* An attribute block as _cellSync2SemaphoreAttributeInitialize leaves it, then
 * with the given fields overridden. tail_garbage leaves 0x5A after the name's NUL
 * (instead of zeros) to expose strncpy vs memcpy of the name field. */
void put_attr(uint32_t ea, uint32_t sdk, uint16_t mask, uint16_t max, bool tail_garbage = false)
{
    fill(ea, 128, 0);
    wr32(ea, sdk); wr16(ea + 4, mask); wr16(ea + 6, max);
    memcpy(mem + ea + 8, "CellSync2Semaphore", 19);
    if (tail_garbage) fill(ea + 8 + 19, 31 - 19, 0x5A);
}

std::vector<Case>* g_out;
void add(const char* id, const char* fn, uint32_t nid, std::initializer_list<uint64_t> args,
         std::function<void()> setup = nullptr, Expect e = Expect::Match, const char* note = "")
{
    Case c; c.id = id; c.func = fn; c.nid = nid; c.setup = std::move(setup); c.expect = e; c.note = note;
    int i = 0; for (uint64_t a : args) c.args[i++] = a;
    g_out->push_back(std::move(c));
}
void attr_init(const char* id, uint64_t attr, uint64_t sdk, Expect e = Expect::Match, const char* note = "")
{ add(id, "_cellSync2SemaphoreAttributeInitialize", NID_ATTR, {attr, sdk}, nullptr, e, note); }
void estimate(const char* id, uint64_t attr, uint64_t out, std::function<void()> s)
{ add(id, "cellSync2SemaphoreEstimateBufferSize", NID_EST, {attr, out}, std::move(s), Expect::NoHle, "HLE does not implement this export"); }
void init(const char* id, uint64_t sem, uint64_t buf, int32_t iv, uint64_t attr, std::function<void()> s,
          Expect e = Expect::Match, const char* note = "")
{ add(id, "cellSync2SemaphoreInitialize", NID_INIT, {sem, buf, (uint64_t)(uint32_t)iv, attr}, std::move(s), e, note); }
}  // namespace

namespace dh {
/* The two pure memory helpers libsync2 calls are run for real (through the HLE
 * handlers, same code on both sides); any other import stops the lifted call with
 * a named reason. */
ImportPolicy import_policy()
{
    ImportPolicy p;
    p.dflt = Policy::Stop;
    p.by_nid[0x68B9B011] = Policy::Forward;   /* _sys_memset  */
    p.by_nid[0xD3039D4D] = Policy::Forward;   /* _sys_strncpy */
    return p;
}

void register_cases(std::vector<Case>& out)
{
    g_out = &out;

    /* ---- _cellSync2SemaphoreAttributeInitialize (S01) --------------------- */
    attr_init("attr01_null",            0,        0x270000);
    attr_init("attr02_sdk_270000",      ATTR,     0x270000);
    attr_init("attr03_sdk_300123",      ATTR,     0x300123);
    attr_init("attr04_sdk_0",           ATTR,     0);
    attr_init("attr05_sdk_ffffffff",    ATTR,     0xFFFFFFFFu);
    attr_init("attr06_sdk_26ffff",      ATTR,     0x26FFFF);
    attr_init("attr07_sdk_330001",      ATTR,     0x330001);
    attr_init("attr08_misaligned_+1",   ATTR + 1, 0x270000);
    attr_init("attr09_misaligned_+7",   ATTR + 7, 0x300123);
    attr_init("attr10_oob_pointer",     BAD_EA,   0x270000, Expect::Match,
              "both stop identically, but only because the lifted code's forwarded _sys_memset hits the same HLE range stop");

    /* ---- cellSync2SemaphoreEstimateBufferSize (no HLE: lifted behaviour is recorded) --- */
    estimate("est01_valid_max1",   ATTR,     OUT, [] { put_attr(ATTR, 0x270000, 0x0001, 1); });
    estimate("est02_valid_max4",   ATTR,     OUT, [] { put_attr(ATTR, 0x270000, 0x0001, 4); });
    estimate("est03_max_8000",     ATTR,     OUT, [] { put_attr(ATTR, 0x270000, 0x0001, 0x8000); });
    estimate("est04_max_8001",     ATTR,     OUT, [] { put_attr(ATTR, 0x270000, 0x0001, 0x8001); });
    estimate("est05_max_0",        ATTR,     OUT, [] { put_attr(ATTR, 0x270000, 0x0001, 0); });
    estimate("est06_sdk_330001",   ATTR,     OUT, [] { put_attr(ATTR, 0x330001, 0x0001, 4); });
    estimate("est07_null_attr",    0,        OUT, nullptr);
    estimate("est08_null_out",     ATTR,     0,   [] { put_attr(ATTR, 0x270000, 0x0001, 4); });

    /* ---- cellSync2SemaphoreInitialize (S02..S12) --------------------------- */
    auto valid = [](uint32_t sdk, uint16_t mask, uint16_t max) { return [=] { put_attr(ATTR, sdk, mask, max); }; };

    init("init01_null_sem",         0,       0,   0, ATTR, valid(0x270000, 1, 1));
    init("init02_null_attr",        SEM,     0,   0, 0,    nullptr);
    init("init03_null_both",        0,       0,   0, 0,    nullptr);
    init("init04_sem_misaligned",   SEM + 8, 0,   0, ATTR, valid(0x270000, 1, 1));
    init("init05_sem_+0x40",        SEM + 0x40, 0, 0, ATTR, valid(0x270000, 1, 1));
    init("init06_buf_misaligned",   SEM,     BUF + 4, 0, ATTR, valid(0x270000, 1, 4));
    init("init07_both_misaligned",  SEM + 4, BUF + 4, 0, ATTR, valid(0x270000, 1, 4));
    init("init08_null_sem_vs_align", 0,      BUF + 4, 0, ATTR, valid(0x270000, 1, 4));
    init("init09_sdk_26ffff",       SEM,     0,   0, ATTR, valid(0x26FFFF, 1, 1));
    init("init10_sdk_270000",       SEM,     0,   0, ATTR, valid(0x270000, 1, 1));
    init("init11_sdk_330000",       SEM,     0,   0, ATTR, valid(0x330000, 1, 1));
    init("init12_sdk_330001",       SEM,     0,   0, ATTR, valid(0x330001, 1, 1));
    init("init13_sdk_340000",       SEM,     0,   0, ATTR, valid(0x340000, 1, 1));
    init("init14_max_0",            SEM,     0,   0, ATTR, valid(0x270000, 1, 0));
    init("init15_max_1_nobuf",      SEM,     0,   0, ATTR, valid(0x270000, 1, 1));
    init("init16_max_1_with_buf",   SEM,     BUF, 0, ATTR, valid(0x270000, 1, 1));
    init("init17_max_2_nobuf",      SEM,     0,   0, ATTR, valid(0x270000, 1, 2));
    init("init18_max_2_buf",        SEM,     BUF, 1, ATTR, valid(0x270000, 1, 2));
    init("init19_max_8000_buf",     SEM,     BUF, 0, ATTR, valid(0x270000, 1, 0x8000));
    init("init20_max_8001_buf",     SEM,     BUF, 0, ATTR, valid(0x270000, 1, 0x8001));
    init("init21_max_7fff_buf",     SEM,     BUF, 0, ATTR, valid(0x270000, 1, 0x7FFF));
    init("init22_mask_0",           SEM,     0,   0, ATTR, valid(0x270000, 0x0000, 1));
    init("init23_mask_0010",        SEM,     0,   0, ATTR, valid(0x270000, 0x0010, 1));
    init("init24_mask_0020",        SEM,     0,   0, ATTR, valid(0x270000, 0x0020, 1));
    init("init25_mask_0200",        SEM,     0,   0, ATTR, valid(0x270000, 0x0200, 1));
    init("init26_mask_2000",        SEM,     0,   0, ATTR, valid(0x270000, 0x2000, 1));
    init("init27_mask_3e30",        SEM,     0,   0, ATTR, valid(0x270000, 0x3E30, 1));
    init("init28_mask_0001",        SEM,     0,   0, ATTR, valid(0x270000, 0x0001, 1));
    init("init29_mask_0002",        SEM,     0,   0, ATTR, valid(0x270000, 0x0002, 1));
    init("init30_mask_010f",        SEM,     0,   0, ATTR, valid(0x270000, 0x010F, 1));
    init("init31_mask_c1ff_hi",     SEM,     0,   0, ATTR, valid(0x270000, 0xC1CF, 1));
    init("init32_valid_max4_iv3",   SEM,     BUF, 3, ATTR, valid(0x300123, 0x010F, 4));
    init("init33_valid_max1_iv_neg", SEM,    0,  -1, ATTR, valid(0x270000, 0x010F, 1));
    init("init34_iv_7fffff",        SEM,     0,   0x7FFFFF, ATTR, valid(0x270000, 1, 1));
    init("init35_iv_800000",        SEM,     0,   0x800000, ATTR, valid(0x270000, 1, 1));
    init("init36_iv_neg_800000",    SEM,     0,  -0x800000, ATTR, valid(0x270000, 1, 1));
    init("init37_iv_neg_800001",    SEM,     0,  -0x800001, ATTR, valid(0x270000, 1, 1));
    init("init38_garbage_attr",     SEM,     0,   0, ATTR, nullptr);
    init("init39_name_tail_garbage", SEM,    0,   0, ATTR, [] { put_attr(ATTR, 0x270000, 0x010F, 1, true); },
         Expect::Diverge, "S12: HLE memcpy()s 31 name bytes, the firmware strncpy()s (zero-pads after the NUL)");
    init("init40_sdk_bad_and_max_0", SEM,    0,   0, ATTR, valid(0x26FFFF, 0, 0));
    init("init41_order_max1buf_vs_mask", SEM, BUF, 0, ATTR, valid(0x270000, 0x0010, 1));
    init("init42_order_nobuf_vs_mask",  SEM,  0,  0, ATTR, valid(0x270000, 0x0010, 4));
    init("init43_attr_oob",         SEM,     0,   0, BAD_EA, nullptr, Expect::Match,
         "stage 1 reported a divergence (HLE named stop vs host SIGSEGV); fault normalisation pairs them as guest-fault");

    /* Why a non-obvious verdict is expected: init39 must diverge on the copied
     * tail bytes, and attr10/init43 must pair a fault (same step, overlapping
     * spans), not match for some other reason. */
    for (auto& c : out) {
        if (c.id == "init39_name_tail_garbage") {
            c.expect_cause = "memory"; c.expect_step = 0;
            c.expect_range_lo = SEM + 0x28; c.expect_range_hi = SEM + 0x47;
        }
        if (c.id == "attr10_oob_pointer") { c.expect_cause = "fault"; c.expect_step = 0; }
        if (c.id == "init43_attr_oob") { c.expect_cause = "fault"; c.expect_step = 0; }
    }
}
}  // namespace dh

/* test_spu_xdiff_record.c - the xdiff recorder (spu_xdiff_recorder.h,
 * docs/SPU_XDIFF.md sections 6 and 7.5).
 *
 * The synthetic job of test_spu_effect_port.c (gen_xdiff_port_job.py: GET, tag wait, a two-element
 * list GET, PUT, SDCRZ, a three-element list whose first element stalls and is resumed by
 * MFC_WrListStallAck, GETLLAR/PUTLLC, rchcnt, mailbox write) runs interpreted and lifted against the
 * real channel, DMA and interpreter runtime, each under a recorder:
 *
 *   (a) the transcript equals an expected table record by record (kind, channel, value, ea, lsa,
 *       size, tag, dir, status), with the payload bytes of every GET, PUT, list, parked list and
 *       atomic line, and the registers and local store at entry and end are the ones the job had;
 *   (b) the two tiers' transcripts, entry and end states are equal except `pc`, which the
 *       interpreter records exactly;
 *   (c) the fixture survives encode -> decode unchanged, and an image byte flipped after recording
 *       is answered with FIXTURE_IDENTITY;
 *   (d) a recorder whose payload bound is too small ends with CAPTURE_LIMIT, stops appending, and
 *       the job itself still completes in the state an unrecorded run reaches;
 *   (e) begin refuses a context that has a port, is not at its entry state, or has an image outside
 *       local store, and installs nothing.
 *
 * Nothing sleeps and no thread starts.
 */
#include "../spu_context.h"
#include "../spu_dma.h"
#include "../spu_interp.h"
#include "../spu_coherency.h"
#include "../spurs_job.h"
#include "../spu_workload.h"
#include "../xdiff/spu_xdiff_census.h"
#include "../xdiff/spu_xdiff_recorder.h"
#include "ps3emu/lifter_abi.h"
#include "xdiff_port_job.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The DMA guard resolves EAs through vm_base; a static arena is committed. */
static uint8_t g_mem[0x40000];
uint8_t* vm_base = g_mem;
uint32_t ppu_vm_size = sizeof g_mem;
const uint32_t ppu_lifter_abi = PPU_RUNTIME_LIFTER_ABI;   /* this program links no PPU lift */

/* The SPURS runtime sources the channel runtime links against reach these; a job
 * of this test touches none of them. */
uint32_t g_barrier_sync_watch = 0;
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
unsigned long long ps3_ms_now(void) { return 0; }
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx);
void spu_recomp_register(void);
void spu_func_00000000(spu_context* ctx);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);

static int g_fail;
#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            g_fail++;                                                        \
            printf("FAIL: ");                                                \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

/* ---- the world ----------------------------------------------------------- */
/* The byte of the world at guest address `a`: what fill_world puts under every range the job reads. */
static uint8_t world_byte(uint32_t a) { return (uint8_t)(a * 131u + 7u); }

static void fill_world(uint8_t* w)
{
    static const struct { uint32_t ea, n; } r[] = {
        { XJ_EA_GET, XJ_GET_SIZE }, { XJ_EA_LIST0, XJ_LIST0_SIZE },
        { XJ_EA_LIST1, XJ_LIST1_SIZE }, { XJ_EA_ATOM, 128 },
        { XJ_EA_SL0, 3 * XJ_SL_SIZE }, { XJ_EA_ZERO, XJ_ZERO_SIZE } };
    memset(w, 0, sizeof g_mem);
    for (unsigned k = 0; k < sizeof r / sizeof r[0]; k++)
        for (uint32_t i = 0; i < r[k].n; i++) w[r[k].ea + i] = world_byte(r[k].ea + i);
}

static void put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint8_t put_source_byte(uint32_t i) { return (uint8_t)(i * 5u + 3u); }

/* ---- running a tier ---------------------------------------------------------- */
static void interp_entry(spu_context* c) { spu_interp_run(c, c->pc); }
static void (*entry_for(int lifted))(spu_context*)
{
    return lifted ? spu_func_00000000 : interp_entry;
}

/* A register the job never touches, so the entry and end register files carry a recognisable value. */
#define PROBE_REG 100u
static const uint32_t kProbe[4] = { 0x01020304u, 0x05060708u, 0x090A0B0Cu, 0x0D0E0F10u };

static spu_context g_ctx;
static spu_context g_scratch;   /* spu_ls_write128 is the definition of the register byte order: it converts them here */

/* Both tiers get the staged image in local store: the lift ignores the bytes, but the entry state a
 * fixture holds (and its fingerprint) is the same whichever body runs, and the end states compare. */
static void ctx_setup(spu_context* c, int lifted)
{
    spu_context_init(c, 0);
    c->image_id = lifted ? 0 : -1;   /* -1: the interpreter must not rejoin a lift */
    c->pc = XJ_ENTRY;
    memcpy(c->ls, xj_code, XJ_CODE_BYTES);
    put_be32(c->ls + XJ_LS_LIST, XJ_LIST0_SIZE);
    put_be32(c->ls + XJ_LS_LIST + 4, XJ_EA_LIST0);
    put_be32(c->ls + XJ_LS_LIST + 8, XJ_LIST1_SIZE);
    put_be32(c->ls + XJ_LS_LIST + 12, XJ_EA_LIST1);
    put_be32(c->ls + XJ_LS_SLIST, 0x80000000u | XJ_SL_SIZE);   /* the first element stalls */
    put_be32(c->ls + XJ_LS_SLIST + 4, XJ_EA_SL0);
    put_be32(c->ls + XJ_LS_SLIST + 8, XJ_SL_SIZE);
    put_be32(c->ls + XJ_LS_SLIST + 12, XJ_EA_SL1);
    put_be32(c->ls + XJ_LS_SLIST + 16, XJ_SL_SIZE);
    put_be32(c->ls + XJ_LS_SLIST + 20, XJ_EA_SL2);
    for (uint32_t i = 0; i < XJ_PUT_SIZE; i++) c->ls[XJ_LS_PUT + i] = put_source_byte(i);
    for (int i = 0; i < 4; i++) c->gpr[PROBE_REG]._u32[i] = kProbe[i];
}

static void ctx_teardown(spu_context* c)
{
    spu_coh_unregister(c);
    spu_mfc_release(c);
}

static int run_job(spu_context* c, int lifted)
{
    return spu_run_with_halt(entry_for(lifted), c);
}

/* The registers as the file holds them: byte k of register r is what a stqd of r stores at offset k. */
static void regs_as_stored(uint8_t out[SPU_XDIFF_REG_BYTES], const spu_context* c)
{
    for (uint32_t r = 0; r < 128; r++) {
        spu_ls_write128(&g_scratch, 0, c->gpr[r]);
        memcpy(out + r * 16, g_scratch.ls, 16);
    }
}

/* ---- the unobserved run of each tier ------------------------------------------ */
typedef struct {
    uint8_t ls[SPU_LS_SIZE];
    uint8_t reg[SPU_XDIFF_REG_BYTES];
    uint32_t status, stop_code;
} end_state;
static end_state g_base[2];
static uint8_t g_entry_ls[SPU_LS_SIZE];
static uint8_t g_entry_reg[SPU_XDIFF_REG_BYTES];

static void baseline_run(int lifted)
{
    spu_context* c = &g_ctx;
    fill_world(g_mem);
    ctx_setup(c, lifted);
    const int halted = run_job(c, lifted);
    CHECK(!halted && c->status == SPU_STATUS_STOPPED_BY_STOP && c->stop_code == XJ_STOP_CODE,
          "%s: the unobserved run did not stop at the job's stop (halted %d status %u code 0x%X)",
          lifted ? "lifted" : "interpreted", halted, c->status, c->stop_code);
    memcpy(g_base[lifted].ls, c->ls, SPU_LS_SIZE);
    regs_as_stored(g_base[lifted].reg, c);
    g_base[lifted].status = c->status;
    g_base[lifted].stop_code = c->stop_code;
    ctx_teardown(c);
}

/* ---- the expected transcript -------------------------------------------------- */
enum { PL_NONE, PL_WORLD, PL_PUT, PL_LIST, PL_SLIST, PL_SLIST_REST };
typedef struct {
    uint8_t kind, channel, tag, dir;
    uint32_t value, status;
    uint64_t ea;
    uint32_t lsa, size;
    int payload;
} xe;

#define WR(ch, v)  { SPU_FX_WRCH,   ch, 0, 0, v, 0, 0, 0, 0, PL_NONE }
#define RD(ch, v)  { SPU_FX_RDCH,   ch, 0, 0, v, 0, 0, 0, 0, PL_NONE }
#define RC(ch, v)  { SPU_FX_RCHCNT, ch, 0, 0, v, 0, 0, 0, 0, PL_NONE }
/* MFC_CMD: the command word, EAH:EAL, the latched LSA and Size, and the tag. */
#define CMD(v, ea_, lsa_, n_, tag_, pl) { SPU_FX_MFC_CMD, MFC_Cmd, tag_, 0, v, 0, ea_, lsa_, n_, pl }
#define XF(op, ea_, lsa_, n_, tag_, dir_, st_, pl) { SPU_FX_XFER, 0, tag_, dir_, op, st_, ea_, lsa_, n_, pl }

/* In the order the job issues them. The latch writes (channels 16 to 20) are absent: their values
 * are the requests of the MFC_CMD records. Two latches are left over from earlier commands, which is
 * the point of recording them: the SDCRZ keeps the PUT's LSA, and the atomics keep the stalled
 * list's Size of 24. */
static const xe kExpected[] = {
    CMD(0x40, XJ_EA_GET, XJ_LS_GET, XJ_GET_SIZE, XJ_TAG, PL_NONE),
    XF(0x40, XJ_EA_GET, XJ_LS_GET, XJ_GET_SIZE, XJ_TAG, SPU_XFER_TO_LS, 0, PL_WORLD),
    WR(MFC_WrTagMask, 1u << XJ_TAG),
    WR(MFC_WrTagUpdate, 2),
    RD(MFC_RdTagStat, 1u << XJ_TAG),
    /* a list command: EAL is the list's local-store address, Size its length, and the list is the payload */
    CMD(0x44, XJ_LS_LIST, XJ_LS_LIST_DEST, 16, XJ_TAG, PL_LIST),
    XF(0x40, XJ_EA_LIST0, XJ_LS_LIST_DEST, XJ_LIST0_SIZE, XJ_TAG, SPU_XFER_TO_LS, 0, PL_WORLD),
    XF(0x40, XJ_EA_LIST1, XJ_LS_LIST_DEST + 16, XJ_LIST1_SIZE, XJ_TAG, SPU_XFER_TO_LS, 0, PL_WORLD),
    CMD(0x20, XJ_EA_PUT, XJ_LS_PUT, XJ_PUT_SIZE, XJ_TAG, PL_NONE),
    XF(0x20, XJ_EA_PUT, XJ_LS_PUT, XJ_PUT_SIZE, XJ_TAG, SPU_XFER_FROM_LS, 0, PL_PUT),
    CMD(0x89, XJ_EA_ZERO, XJ_LS_PUT, XJ_ZERO_SIZE, XJ_TAG, PL_NONE),
    XF(0x89, XJ_EA_ZERO, 0, XJ_ZERO_SIZE, XJ_TAG, SPU_XFER_ZERO_EA, 0, PL_NONE),
    /* the stalled list: its first element moves, then the list parks */
    CMD(0x44, XJ_LS_SLIST, XJ_LS_SLIST_DEST, 24, XJ_TAG2, PL_SLIST),
    XF(0x40, XJ_EA_SL0, XJ_LS_SLIST_DEST, XJ_SL_SIZE, XJ_TAG2, SPU_XFER_TO_LS, 0, PL_WORLD),
    RD(MFC_RdListStallStat, 1u << XJ_TAG2),
    /* the acknowledgement carries the elements still parked: lsa the next element, size 8 per element */
    { SPU_FX_WRCH, MFC_WrListStallAck, 0, 0, XJ_TAG2, 0, 0, XJ_LS_SLIST + 8, 16, PL_SLIST_REST },
    XF(0x40, XJ_EA_SL1, XJ_LS_SLIST_DEST + 16, XJ_SL_SIZE, XJ_TAG2, SPU_XFER_TO_LS, 0, PL_WORLD),
    XF(0x40, XJ_EA_SL2, XJ_LS_SLIST_DEST + 32, XJ_SL_SIZE, XJ_TAG2, SPU_XFER_TO_LS, 0, PL_WORLD),
    CMD(0xD0, XJ_EA_ATOM, XJ_LS_ATOM, 24, XJ_TAG, PL_NONE),
    XF(0xD0, XJ_EA_ATOM, XJ_LS_ATOM, 128, XJ_TAG, SPU_XFER_TO_LS, 4, PL_WORLD),
    RD(MFC_RdAtomicStat, 4),
    CMD(0xB4, XJ_EA_ATOM, XJ_LS_ATOM, 24, XJ_TAG, PL_NONE),
    XF(0xB4, XJ_EA_ATOM, XJ_LS_ATOM, 128, XJ_TAG, SPU_XFER_FROM_LS, 0, PL_WORLD),
    RD(MFC_RdAtomicStat, 0),
    RC(SPU_WrOutMbox, 1),
    WR(SPU_WrOutMbox, XJ_MBOX_VALUE),
};
#define NX ((uint32_t)(sizeof kExpected / sizeof kExpected[0]))

/* The payload bytes record `w` must carry; returns their count. */
static uint32_t expected_payload(const xe* w, uint8_t buf[256])
{
    static const uint32_t list[4] = { XJ_LIST0_SIZE, XJ_EA_LIST0, XJ_LIST1_SIZE, XJ_EA_LIST1 };
    static const uint32_t slist[6] = { 0x80000000u | XJ_SL_SIZE, XJ_EA_SL0, XJ_SL_SIZE, XJ_EA_SL1, XJ_SL_SIZE,
                                       XJ_EA_SL2 };
    uint32_t n = 0;
    switch (w->payload) {
    case PL_WORLD:
        for (; n < w->size; n++) buf[n] = world_byte((uint32_t)w->ea + n);
        break;
    case PL_PUT:
        for (; n < w->size; n++) buf[n] = put_source_byte(n);
        break;
    case PL_LIST:
        for (; n < 4; n++) put_be32(buf + 4 * n, list[n]);
        return 16;
    case PL_SLIST:
        for (; n < 6; n++) put_be32(buf + 4 * n, slist[n]);
        return 24;
    case PL_SLIST_REST:   /* what is still parked once the first element has moved */
        for (; n < 4; n++) put_be32(buf + 4 * n, slist[2 + n]);
        return 16;
    default:
        break;
    }
    return n;
}

static void check_records(const char* tier, const spu_xdiff_fixture* f)
{
    CHECK(f->effect_count == NX, "%s: %u records, expected %u", tier, f->effect_count, NX);
    for (uint32_t i = 0; i < f->effect_count && i < NX; i++) {
        const spu_xdiff_effect* g = &f->effects[i];
        const xe* w = &kExpected[i];
        uint8_t want[256];
        const uint32_t want_n = expected_payload(w, want);
        CHECK(g->kind == w->kind && g->channel == w->channel && g->tag == w->tag && g->dir == w->dir &&
                  g->value == w->value && g->status == w->status && g->ea == w->ea && g->lsa == w->lsa &&
                  g->size == w->size && g->reserved == 0,
              "%s: record %u is kind %u ch %u tag %u dir %u value 0x%X status %u ea 0x%llX lsa 0x%X size %u;"
              " expected kind %u ch %u tag %u dir %u value 0x%X status %u ea 0x%llX lsa 0x%X size %u",
              tier, i, g->kind, g->channel, g->tag, g->dir, g->value, g->status, (unsigned long long)g->ea, g->lsa,
              g->size, w->kind, w->channel, w->tag, w->dir, w->value, w->status, (unsigned long long)w->ea, w->lsa,
              w->size);
        if (w->payload == PL_NONE) {
            CHECK(g->blob == SPU_XDIFF_NO_BLOB, "%s: record %u has a payload (blob %u) and should have none", tier, i,
                  g->blob);
            continue;
        }
        CHECK(want_n == w->size, "%s: the test's own payload for record %u is %u bytes, not %u", tier, i, want_n,
              w->size);
        CHECK(g->blob != SPU_XDIFF_NO_BLOB && (uint64_t)g->blob + g->size <= f->blob_bytes,
              "%s: record %u: payload %u+%u is not inside the %u-byte blob", tier, i, g->blob, g->size, f->blob_bytes);
        if (g->blob != SPU_XDIFF_NO_BLOB && (uint64_t)g->blob + g->size <= f->blob_bytes)
            CHECK(!memcmp(f->blob + g->blob, want, want_n), "%s: record %u: the payload bytes are wrong", tier, i);
    }
}

/* ---- recording one tier --------------------------------------------------------- */
static spu_xdiff_unit make_unit(void)
{
    spu_xdiff_unit u;
    memset(&u, 0, sizeof u);
    u.kind = SPU_XDIFF_UNIT_JOB;
    u.profile = SPU_XDIFF_PROFILE_JOBCHAIN;
    u.entry_pc = XJ_ENTRY;
    u.return_ls = SPU_JOB_RETURN_LINK_LS;
    u.image_ea = 0x20000u;
    u.image_ls = 0;
    u.image_bytes = XJ_CODE_BYTES;
    u.desc_ls = XJ_LS_LIST;
    u.desc_bytes = 16;
    u.io_ls = XJ_LS_GET;
    u.io_bytes = XJ_GET_SIZE;
    u.origin_seq = 7;
    return u;
}

static spu_xdiff_body_id make_body(int lifted)
{
    spu_xdiff_body_id b;
    memset(&b, 0, sizeof b);
    b.tier = lifted ? SPU_TIER_LIFTED : SPU_TIER_INTERP;
    if (lifted) b.lifter_abi = SPU_RUNTIME_LIFTER_ABI;
    return b;
}

typedef struct {
    int begin_rc, halted;
    uint32_t failed;       /* what end() returned */
    spu_xdiff_error error; /* the recorder's error after end() */
    uint32_t status, stop_code, pc;
    spu_job_end classified;
} run_result;

/* One recorded run of the job. */
static void record_tier(int lifted, spu_xdiff_fixture* f, uint32_t max_blob, run_result* out)
{
    const char* tier = lifted ? "lifted" : "interpreted";
    spu_xdiff_recorder r;
    spu_context* c = &g_ctx;
    const spu_xdiff_unit unit = make_unit();
    const spu_xdiff_body_id body = make_body(lifted);

    memset(out, 0, sizeof *out);
    fill_world(g_mem);
    ctx_setup(c, lifted);
    memcpy(g_entry_ls, c->ls, SPU_LS_SIZE);
    regs_as_stored(g_entry_reg, c);
    out->begin_rc = spu_xdiff_recorder_begin(&r, f, c, &unit, &body, NULL, NULL, max_blob);
    CHECK(out->begin_rc == 0, "%s: begin refused: %s", tier, r.error.detail);
    if (out->begin_rc != 0) {
        out->failed = r.failed;
        ctx_teardown(c);
        return;
    }
    CHECK(spu_port_get(c) == &r.port && r.port.tap == &r.tap && r.port.serve == NULL,
          "%s: begin did not install the recorder's tap-only port", tier);

    out->halted = run_job(c, lifted);
    out->classified = spu_job_classify_end(c);
    out->failed = spu_xdiff_recorder_end(&r, c, out->classified.end_class, out->classified.stop_code);
    out->error = r.error;
    out->status = c->status;
    out->stop_code = c->stop_code;
    out->pc = c->pc;
    CHECK(spu_port_get(c) == NULL, "%s: end left the port installed", tier);
    CHECK(!out->halted, "%s: the recorded run halted", tier);
    ctx_teardown(c);
}

/* What a fixture of the full run holds besides its transcript. */
static void check_state(const char* tier, int lifted, const spu_xdiff_fixture* f, const run_result* run)
{
    const spu_xdiff_unit unit = make_unit();
    static const uint8_t zero20[20];
    uint8_t zero_reg[16];

    CHECK(run->failed == SPU_XDIFF_R_NONE, "%s: end() returned reason %u (%s): %s", tier, run->failed,
          spu_xdiff_reason_id(run->failed), run->error.detail);

    /* IDEN */
    CHECK(f->identity.image_fingerprint == spu_workload_fingerprint(xj_code, XJ_CODE_BYTES),
          "%s: the fingerprint is not that of the staged image", tier);
    CHECK(f->identity.authority.tier == (lifted ? SPU_TIER_LIFTED : SPU_TIER_INTERP) &&
              f->identity.authority.lifter_abi == (lifted ? SPU_RUNTIME_LIFTER_ABI : 0u) &&
              f->identity.authority.code_end == 0 && f->identity.authority.lift_options == 0 &&
              f->identity.authority.native_rev == 0,
          "%s: the authority body id is not the one given", tier);
    CHECK(f->identity.effect_port_abi == SPU_EFFECT_PORT_ABI, "%s: effect_port_abi %u", tier,
          f->identity.effect_port_abi);
    CHECK(f->identity.context_census == spu_xdiff_context_census(), "%s: context_census 0x%X", tier,
          f->identity.context_census);
    CHECK(f->identity.host_arch == SPU_XDIFF_HOST_ARM64 || f->identity.host_arch == SPU_XDIFF_HOST_X86_64,
          "%s: host_arch %u", tier, f->identity.host_arch);
    /* An x86-64 baseline build (no -mssse3 and up, no scalar switch) compiles none of the optional helper
     * paths, so 0 is a valid word there; what the word must never do is name a path of the other
     * architecture or a bit the header does not define. */
    {
        const uint32_t known = SPU_XDIFF_ISA_NEON | SPU_XDIFF_ISA_SSSE3 | SPU_XDIFF_ISA_SSE41 | SPU_XDIFF_ISA_AVX2 |
                               SPU_XDIFF_ISA_FMA | SPU_XDIFF_ISA_SCALAR;
        const uint32_t x86 = SPU_XDIFF_ISA_SSSE3 | SPU_XDIFF_ISA_SSE41 | SPU_XDIFF_ISA_AVX2;
        const uint32_t isa = f->identity.host_isa;
        CHECK((isa & ~known) == 0, "%s: host_isa 0x%X has undefined bits", tier, isa);
        CHECK(f->identity.host_arch == SPU_XDIFF_HOST_ARM64 ? (isa & x86) == 0 : (isa & SPU_XDIFF_ISA_NEON) == 0,
              "%s: host_isa 0x%X names a path of the other architecture", tier, isa);
#if defined(__ARM_NEON)
        CHECK((isa & SPU_XDIFF_ISA_NEON) != 0, "%s: a NEON build recorded host_isa 0x%X", tier, isa);
#endif
    }
    CHECK(f->identity.float_rz == (spu_float_rz_mode() ? 1 : 0), "%s: float_rz %u", tier, f->identity.float_rz);
    CHECK(!memcmp(f->identity.toolkit_commit, zero20, 20) && f->identity.toolkit_dirty == 0 &&
              f->identity.reserved[0] == 0 && f->identity.reserved[1] == 0,
          "%s: toolkit_commit/dirty/reserved are not zero", tier);

    /* UNIT, copied whole */
    CHECK(!memcmp(&f->unit, &unit, sizeof unit), "%s: UNIT is not the unit given", tier);

    /* REG0 and LS_0: the entry state, registers as stqd stores them */
    CHECK(!memcmp(f->reg0, g_entry_reg, SPU_XDIFF_REG_BYTES), "%s: REG0 is not the entry register file", tier);
    memset(zero_reg, 0, sizeof zero_reg);
    for (uint32_t k = 0; k < 16; k++)
        CHECK(f->reg0[PROBE_REG * 16 + k] == k + 1, "%s: REG0[%u] byte %u is 0x%02X, expected 0x%02X (guest byte order)",
              tier, PROBE_REG, k, f->reg0[PROBE_REG * 16 + k], k + 1);
    for (uint32_t r = 0; r < 128; r++)
        if (r != PROBE_REG)
            CHECK(!memcmp(f->reg0 + r * 16, zero_reg, 16), "%s: REG0[%u] is not zero", tier, r);
    CHECK(!memcmp(f->ls0, g_entry_ls, SPU_LS_SIZE), "%s: LS_0 is not the entry local store", tier);
    CHECK(!memcmp(f->ls0, xj_code, XJ_CODE_BYTES), "%s: LS_0 does not start with the staged image", tier);

    /* REG1, LS_1: the end state an unobserved run reaches (recording changes nothing) */
    CHECK(!memcmp(f->reg1, g_base[lifted].reg, SPU_XDIFF_REG_BYTES), "%s: REG1 is not the unobserved end registers",
          tier);
    CHECK(!memcmp(f->ls1, g_base[lifted].ls, SPU_LS_SIZE), "%s: LS_1 is not the unobserved end local store", tier);
    CHECK(f->reg1[PROBE_REG * 16] == 0x01 && f->reg1[PROBE_REG * 16 + 15] == 0x10,
          "%s: REG1[%u] lost its value", tier, PROBE_REG);
    CHECK(run->status == g_base[lifted].status && run->stop_code == g_base[lifted].stop_code,
          "%s: the recorded run ended as status %u code 0x%X, the unobserved one as %u 0x%X", tier, run->status,
          run->stop_code, g_base[lifted].status, g_base[lifted].stop_code);

    /* END_: the loader's classification and the raw fields */
    CHECK(run->classified.end_class == SPU_JOB_END_STOP && run->classified.stop_code == XJ_STOP_CODE,
          "%s: the job did not classify as a stop with its code (class %u code 0x%X)", tier, run->classified.end_class,
          run->classified.stop_code);
    CHECK(f->end.end_class == SPU_XDIFF_END_STOP && f->end.stop_code == XJ_STOP_CODE &&
              f->end.pc == (run->pc & SPU_LS_MASK) && f->end.status == SPU_STATUS_STOPPED_BY_STOP,
          "%s: END_ is class %u code 0x%X pc 0x%X status %u", tier, f->end.end_class, f->end.stop_code, f->end.pc,
          f->end.status);
}

/* The interpreter stores pc before every channel instruction, and the straight-line job issues them in
 * program order: its records carry the instruction's address, and a transfer carries its command's. */
static void check_interpreted_pcs(const spu_xdiff_fixture* f)
{
    uint32_t last = 0;
    for (uint32_t i = 0; i < f->effect_count; i++) {
        const uint32_t pc = f->effects[i].pc;
        CHECK(pc % 4 == 0 && pc < XJ_CODE_BYTES && pc >= last, "interpreted: record %u has pc 0x%X after 0x%X", i, pc,
              last);
        if (f->effects[i].kind == SPU_FX_XFER && i > 0)
            CHECK(pc == f->effects[i - 1].pc, "interpreted: transfer %u has pc 0x%X, its command 0x%X", i, pc,
                  f->effects[i - 1].pc);
        last = pc;
    }
    /* the GET block is six constants and six latch/command writes: MFC_Cmd is its twelfth instruction */
    CHECK(f->effect_count > 0 && f->effects[0].pc == 11 * 4, "interpreted: the first MFC_Cmd is at pc 0x%X, expected 0x2C",
          f->effect_count ? f->effects[0].pc : 0);
}

/* The two tiers run the same program: same records, payloads, entry and end state, but for `pc`. */
static void check_tiers_equal(const spu_xdiff_fixture* a, const spu_xdiff_fixture* b)
{
    CHECK(a->effect_count == b->effect_count, "tiers: %u records interpreted, %u lifted", a->effect_count,
          b->effect_count);
    for (uint32_t i = 0; i < a->effect_count && i < b->effect_count; i++) {
        spu_xdiff_effect x = a->effects[i], y = b->effects[i];
        x.pc = y.pc = 0;
        CHECK(!memcmp(&x, &y, sizeof x), "tiers: record %u differs beyond pc", i);
    }
    CHECK(a->blob_bytes == b->blob_bytes && !memcmp(a->blob, b->blob, a->blob_bytes), "tiers: the payload stores differ");
    CHECK(a->identity.image_fingerprint == b->identity.image_fingerprint, "tiers: fingerprints differ");
    CHECK(!memcmp(a->reg0, b->reg0, SPU_XDIFF_REG_BYTES) && !memcmp(a->ls0, b->ls0, SPU_LS_SIZE),
          "tiers: entry states differ");
    CHECK(!memcmp(a->reg1, b->reg1, SPU_XDIFF_REG_BYTES), "tiers: REG1 differs");
    CHECK(!memcmp(a->ls1, b->ls1, SPU_LS_SIZE), "tiers: LS_1 differs");
    CHECK(a->end.end_class == b->end.end_class && a->end.stop_code == b->end.stop_code,
          "tiers: END_ class/code differ (%u/0x%X against %u/0x%X)", a->end.end_class, a->end.stop_code, b->end.end_class,
          b->end.stop_code);
}

/* ---- the file form ------------------------------------------------------------------ */
static void check_roundtrip(const char* tier, const spu_xdiff_fixture* f)
{
    uint8_t* bytes = NULL;
    size_t n = 0;
    spu_xdiff_error err;
    spu_xdiff_fixture g;
    memset(&err, 0, sizeof err);
    memset(&g, 0, sizeof g);
    int rc = spu_xdiff_fixture_encode(f, &bytes, &n, &err);
    CHECK(rc == 0, "%s: encode failed (%s): %s", tier, spu_xdiff_reason_id(err.reason), err.detail);
    if (rc != 0) return;
    rc = spu_xdiff_fixture_decode(bytes, n, &g, &err);
    CHECK(rc == 0, "%s: decode of the encoded fixture failed (%s): %s", tier, spu_xdiff_reason_id(err.reason), err.detail);
    if (rc == 0) {
        CHECK(!memcmp(&g.identity, &f->identity, sizeof g.identity), "%s: round trip: IDEN differs", tier);
        CHECK(!memcmp(&g.unit, &f->unit, sizeof g.unit), "%s: round trip: UNIT differs", tier);
        CHECK(!memcmp(g.reg0, f->reg0, SPU_XDIFF_REG_BYTES), "%s: round trip: REG0 differs", tier);
        CHECK(!memcmp(g.ls0, f->ls0, SPU_LS_SIZE), "%s: round trip: LS_0 differs", tier);
        CHECK(g.effect_count == f->effect_count &&
                  !memcmp(g.effects, f->effects, (size_t)f->effect_count * sizeof *f->effects),
              "%s: round trip: the transcript differs (%u records, %u recorded)", tier, g.effect_count, f->effect_count);
        CHECK(g.blob_bytes == f->blob_bytes && !memcmp(g.blob, f->blob, f->blob_bytes), "%s: round trip: BLOB differs",
              tier);
        CHECK(!memcmp(g.reg1, f->reg1, SPU_XDIFF_REG_BYTES), "%s: round trip: REG1 differs", tier);
        CHECK(!memcmp(g.ls1, f->ls1, SPU_LS_SIZE), "%s: round trip: LS_1 differs", tier);
        CHECK(!memcmp(&g.end, &f->end, sizeof g.end), "%s: round trip: END_ differs", tier);
        CHECK(g.has_verdict == f->has_verdict, "%s: round trip: a verdict appeared or vanished", tier);
        spu_xdiff_fixture_free(&g);
    }
    free(bytes);
}

/* A byte of the staged image changed in the file: the encoder never writes what the decoder rejects, so the
 * flip is made on the encoded bytes (found through the section table), and the checksum is recomputed so the
 * file is well-formed and only the image no longer hashes to the fingerprint it carries. */
static void check_identity_flip(const spu_xdiff_fixture* f)
{
    uint8_t* bytes = NULL;
    size_t n = 0;
    spu_xdiff_error err;
    spu_xdiff_fixture g;
    memset(&err, 0, sizeof err);
    memset(&g, 0, sizeof g);
    int rc = spu_xdiff_fixture_encode(f, &bytes, &n, &err);
    CHECK(rc == 0, "identity: encode failed (%s): %s", spu_xdiff_reason_id(err.reason), err.detail);
    if (rc != 0) return;

    spu_xdiff_file_header h;
    memcpy(&h, bytes, sizeof h);
    int flipped = 0;
    for (uint32_t i = 0; i < h.section_count && !flipped; i++) {
        spu_xdiff_section s;
        spu_xdiff_ls_extent e;
        memcpy(&s, bytes + h.header_bytes + (size_t)i * h.section_bytes, sizeof s);
        if (s.tag != SPU_XDIFF_SEC_LS0) continue;
        /* the first extent's data lies at the extent header's end; flip its first byte when that byte is image */
        if (s.bytes < sizeof e + 8 || s.offset + s.bytes > n) break;
        memcpy(&e, bytes + s.offset, sizeof e);
        if (e.bytes == 0 || e.lsa >= XJ_CODE_BYTES) break;
        bytes[s.offset + sizeof e] ^= 0x10;
        flipped = 1;
    }
    CHECK(flipped, "identity: no LS_0 extent starts inside the staged image");
    if (flipped) {
        uint64_t sum;
        memset(bytes + offsetof(spu_xdiff_file_header, checksum), 0, sizeof h.checksum);
        sum = spu_workload_fingerprint(bytes, (size_t)h.file_bytes);
        memcpy(bytes + offsetof(spu_xdiff_file_header, checksum), &sum, sizeof sum);
        rc = spu_xdiff_fixture_decode(bytes, n, &g, &err);
        CHECK(rc != 0 && err.reason == SPU_XDIFF_R_FIXTURE_IDENTITY,
              "identity: a flipped image byte was answered with rc %d reason %u (%s): %s, expected FIXTURE_IDENTITY",
              rc, err.reason, spu_xdiff_reason_id(err.reason), err.detail);
        if (rc == 0) spu_xdiff_fixture_free(&g);
    }
    free(bytes);
}

/* ---- the capture bound ---------------------------------------------------------------- */
static void check_capture_limit(const char* what, int lifted, uint32_t max_blob, uint32_t want_records,
                                spu_xdiff_fixture* f)
{
    run_result run;
    record_tier(lifted, f, max_blob, &run);
    if (run.begin_rc != 0) return;
    CHECK(run.failed == SPU_XDIFF_R_CAPTURE_LIMIT && run.error.reason == SPU_XDIFF_R_CAPTURE_LIMIT &&
              run.error.detail[0] != 0,
          "%s: end() returned %u (%s), expected CAPTURE_LIMIT with a detail", what, run.failed,
          spu_xdiff_reason_id(run.failed));
    CHECK(f->effect_count == want_records,
          "%s: %u records, expected %u: recording must stop at the record that did not fit", what, f->effect_count,
          want_records);
    /* the check that matters to the guest: the job still completed, in the unobserved run's state */
    CHECK(run.status == SPU_STATUS_STOPPED_BY_STOP && run.stop_code == XJ_STOP_CODE,
          "%s: the job did not complete under a failed recorder (status %u code 0x%X)", what, run.status, run.stop_code);
    CHECK(!memcmp(f->ls1, g_base[lifted].ls, SPU_LS_SIZE) && !memcmp(f->reg1, g_base[lifted].reg, SPU_XDIFF_REG_BYTES),
          "%s: the end state of the job changed under a failed recorder", what);
    /* the part of the transcript that was kept is still the right one */
    for (uint32_t i = 0; i < f->effect_count && i < NX; i++)
        CHECK(f->effects[i].kind == kExpected[i].kind && f->effects[i].value == kExpected[i].value,
              "%s: kept record %u is not the job's", what, i);
    /* an incomplete fixture is not a fixture: END_ class 0, which neither the encoder nor the file writer accepts */
    CHECK(f->end.end_class == 0, "%s: a failed recording left END_ class %u", what, f->end.end_class);
    {
        uint8_t* bytes = NULL;
        size_t n = 0;
        spu_xdiff_error err;
        const char* path = "xdiff_record_failed.spujob";
        memset(&err, 0, sizeof err);
        if (spu_xdiff_fixture_encode(f, &bytes, &n, &err) == 0) {
            CHECK(0, "%s: a failed recording encoded", what);
            free(bytes);
        }
        memset(&err, 0, sizeof err);
        if (spu_xdiff_fixture_write(f, path, &err) == 0) {
            CHECK(0, "%s: a failed recording was written", what);
            remove(path);
        }
    }
}

/* ---- refusals ------------------------------------------------------------------------- */
static void dummy_wrch(void* u, const spu_context* c, uint32_t ch, uint32_t v)
{
    (void)u; (void)c; (void)ch; (void)v;
}
static const spu_effect_tap kDummyTap = { dummy_wrch, NULL, NULL, NULL };

static void check_refusals(spu_xdiff_fixture* f)
{
    spu_xdiff_recorder r;
    spu_context* c = &g_ctx;
    spu_effect_port other;
    spu_xdiff_unit unit = make_unit();
    const spu_xdiff_body_id body = make_body(0);

    other.serve = NULL; other.serve_user = NULL; other.tap = &kDummyTap; other.tap_user = NULL;

    /* a context that already has a port */
    spu_xdiff_fixture_clear(f);
    ctx_setup(c, 0);
    spu_port_set(c, &other);
    CHECK(spu_xdiff_recorder_begin(&r, f, c, &unit, &body, NULL, NULL, 0) != 0, "refusal: begin took a context with a port");
    CHECK(r.error.reason == SPU_XDIFF_R_UNSUPPORTED_EFFECT && r.error.detail[0] != 0 && r.failed == r.error.reason,
          "refusal: port: reason %u (%s), detail \"%s\"", r.error.reason, spu_xdiff_reason_id(r.error.reason),
          r.error.detail);
    CHECK(spu_port_get(c) == &other, "refusal: begin replaced the context's port");
    CHECK(f->effect_count == 0 && f->identity.image_fingerprint == 0 && f->end.end_class == 0,
          "refusal: begin wrote into the fixture");
    CHECK(spu_xdiff_recorder_end(&r, c, SPU_XDIFF_END_STOP, 0) != SPU_XDIFF_R_NONE && spu_port_get(c) == &other,
          "refusal: end() on a recorder that never began answered NONE or removed the other port");
    spu_port_set(c, NULL);
    ctx_teardown(c);

    /* a context that is not at its entry state */
    spu_xdiff_fixture_clear(f);
    ctx_setup(c, 0);
    c->pc = 4;
    CHECK(spu_xdiff_recorder_begin(&r, f, c, &unit, &body, NULL, NULL, 0) != 0, "refusal: begin took a context past its entry pc");
    CHECK(r.error.reason == SPU_XDIFF_R_FIXTURE_FORMAT && r.error.detail[0] != 0, "refusal: pc: reason %u (%s)",
          r.error.reason, spu_xdiff_reason_id(r.error.reason));
    CHECK(spu_port_get(c) == NULL && f->effect_count == 0, "refusal: a refused begin installed a port");
    ctx_teardown(c);

    /* an image that is not inside local store */
    ctx_setup(c, 0);
    unit.image_ls = SPU_LS_SIZE - 4;
    unit.image_bytes = 16;
    CHECK(spu_xdiff_recorder_begin(&r, f, c, &unit, &body, NULL, NULL, 0) != 0, "refusal: begin took an image outside local store");
    CHECK(r.error.reason == SPU_XDIFF_R_FIXTURE_FORMAT && spu_port_get(c) == NULL, "refusal: image: reason %u",
          r.error.reason);
    ctx_teardown(c);
}

/* A GETLLAR after a stale, huge MFC_Size latch (the atomic ignores Size): the run is ordinary, and the
 * MFC_CMD record holds the latches verbatim although lsa + size lies far past local store. Driven by
 * direct channel writes: the synthetic job's latches are fixed and its expectations stay untouched. */
static void check_stale_latches(spu_xdiff_fixture* f)
{
    spu_xdiff_recorder r;
    spu_context* c = &g_ctx;
    const spu_xdiff_unit unit = make_unit();
    const spu_xdiff_body_id body = make_body(0);
    u128 v;

    fill_world(g_mem);
    ctx_setup(c, 0);
    CHECK(spu_xdiff_recorder_begin(&r, f, c, &unit, &body, NULL, NULL, 0) == 0, "stale latches: begin refused: %s",
          r.error.detail);
    if (r.ctx == NULL) {
        ctx_teardown(c);
        return;
    }
    memset(&v, 0, sizeof v);
#define LATCH(ch, val) do { v._u32[0] = (val); spu_wrch(c, (ch), v); } while (0)
    LATCH(MFC_LSA, XJ_LS_ATOM);
    LATCH(MFC_EAH, 0);
    LATCH(MFC_EAL, XJ_EA_ATOM);
    LATCH(MFC_Size, 0x40000u);
    LATCH(MFC_TagID, XJ_TAG);
    LATCH(MFC_Cmd, 0xD0);
#undef LATCH
    const uint32_t failed = spu_xdiff_recorder_end(&r, c, SPU_XDIFF_END_OTHER, 0);
    CHECK(failed == SPU_XDIFF_R_NONE, "stale latches: end() returned %u (%s): %s", failed, spu_xdiff_reason_id(failed),
          r.error.detail);
    CHECK(f->effect_count == 2, "stale latches: %u records, expected the command and its transfer", f->effect_count);
    if (f->effect_count == 2) {
        const spu_xdiff_effect* m = &f->effects[0];
        const spu_xdiff_effect* x = &f->effects[1];
        CHECK(m->kind == SPU_FX_MFC_CMD && m->value == 0xD0 && m->ea == XJ_EA_ATOM && m->lsa == XJ_LS_ATOM &&
                  m->size == 0x40000u && m->tag == XJ_TAG && m->blob == SPU_XDIFF_NO_BLOB,
              "stale latches: the MFC_CMD record is kind %u value 0x%X ea 0x%llX lsa 0x%X size 0x%X tag %u", m->kind,
              m->value, (unsigned long long)m->ea, m->lsa, m->size, m->tag);
        CHECK(x->kind == SPU_FX_XFER && x->dir == SPU_XFER_TO_LS && x->value == 0xD0 && x->status == 4 &&
                  x->ea == XJ_EA_ATOM && x->lsa == XJ_LS_ATOM && x->size == 128 && x->tag == XJ_TAG &&
                  x->blob != SPU_XDIFF_NO_BLOB && (uint64_t)x->blob + 128 <= f->blob_bytes,
              "stale latches: the XFER record is kind %u dir %u value 0x%X status %u size %u", x->kind, x->dir, x->value,
              x->status, x->size);
        if (x->blob != SPU_XDIFF_NO_BLOB && (uint64_t)x->blob + 128 <= f->blob_bytes)
            for (uint32_t i = 0; i < 128; i++)
                if (f->blob[x->blob + i] != world_byte(XJ_EA_ATOM + i)) {
                    CHECK(0, "stale latches: the line payload differs at byte %u", i);
                    break;
                }
    }
    check_roundtrip("stale latches", f);
    ctx_teardown(c);
}

static void wrch32(spu_context* c, uint32_t ch, uint32_t val)
{
    u128 v;
    memset(&v, 0, sizeof v);
    v._u32[0] = val;
    spu_wrch(c, ch, v);
}

/* A payload of `n` bytes at record `e` equals `want`. */
static int payload_is(const spu_xdiff_fixture* f, const spu_xdiff_effect* e, const uint8_t* want, uint32_t n)
{
    return e->blob != SPU_XDIFF_NO_BLOB && e->size == n && (uint64_t)e->blob + n <= f->blob_bytes &&
           !memcmp(f->blob + e->blob, want, n);
}

/* MFC_WrListStallAck, driven by direct channel writes. `rewrite`: a two-element list whose first element
 * stalls, and whose second is rewritten between the stall and the ack; the ack's payload must be the element
 * as it is at the ack (the runtime reads it again then), not as it was at issue. Otherwise a one-element list
 * whose only element stalls: the parked list has nothing left, so the ack carries lsa 0, size 0 and no payload
 * (the decoder requires lsa 0 with size 0). */
static void check_stall_ack(spu_xdiff_fixture* f, int rewrite)
{
    const char* what = rewrite ? "stall ack (rewritten element)" : "stall ack (nothing left)";
    const uint32_t list_ls = 0x2000, dest = 0x2400, tag = rewrite ? 5 : 6, elems = rewrite ? 2 : 1;
    spu_xdiff_recorder r;
    spu_context* c = &g_ctx;
    const spu_xdiff_unit unit = make_unit();
    const spu_xdiff_body_id body = make_body(0);
    uint8_t issued[16], at_ack[8], fresh[16];

    fill_world(g_mem);
    ctx_setup(c, 0);
    put_be32(c->ls + list_ls, 0x80000000u | XJ_SL_SIZE);   /* stall-and-notify */
    put_be32(c->ls + list_ls + 4, XJ_EA_SL0);
    if (rewrite) {
        put_be32(c->ls + list_ls + 8, XJ_SL_SIZE);
        put_be32(c->ls + list_ls + 12, XJ_EA_SL1);
    }
    memcpy(issued, c->ls + list_ls, elems * 8);
    CHECK(spu_xdiff_recorder_begin(&r, f, c, &unit, &body, NULL, NULL, 0) == 0, "%s: begin refused: %s", what,
          r.error.detail);
    if (r.ctx == NULL) {
        ctx_teardown(c);
        return;
    }
    wrch32(c, MFC_LSA, dest);
    wrch32(c, MFC_EAH, 0);
    wrch32(c, MFC_EAL, list_ls);
    wrch32(c, MFC_Size, elems * 8);
    wrch32(c, MFC_TagID, tag);
    wrch32(c, MFC_Cmd, 0x44);
    if (rewrite) {   /* the stall handler rewrites the parked element: another source EA */
        put_be32(c->ls + list_ls + 12, XJ_EA_SL2);
        memcpy(at_ack, c->ls + list_ls + 8, 8);
    }
    wrch32(c, MFC_WrListStallAck, tag);
    const uint32_t failed = spu_xdiff_recorder_end(&r, c, SPU_XDIFF_END_OTHER, 0);
    CHECK(failed == SPU_XDIFF_R_NONE, "%s: end() returned %u (%s): %s", what, failed, spu_xdiff_reason_id(failed),
          r.error.detail);
    CHECK(f->effect_count == (rewrite ? 4u : 3u), "%s: %u records", what, f->effect_count);
    if (f->effect_count == (rewrite ? 4u : 3u)) {
        const spu_xdiff_effect* cmd = &f->effects[0];
        const spu_xdiff_effect* ack = &f->effects[2];
        CHECK(cmd->kind == SPU_FX_MFC_CMD && cmd->value == 0x44 && cmd->ea == list_ls && cmd->lsa == dest &&
                  cmd->size == elems * 8 && cmd->tag == tag && payload_is(f, cmd, issued, elems * 8),
              "%s: the list command record is wrong (the list as issued is its payload)", what);
        CHECK(f->effects[1].kind == SPU_FX_XFER && f->effects[1].ea == XJ_EA_SL0 && f->effects[1].lsa == dest,
              "%s: the first element's transfer is wrong", what);
        CHECK(ack->kind == SPU_FX_WRCH && ack->channel == MFC_WrListStallAck && ack->value == tag && ack->tag == 0 &&
                  ack->ea == 0 && ack->status == 0 && ack->dir == 0,
              "%s: the acknowledgement record is wrong", what);
        if (rewrite) {
            const spu_xdiff_effect* x = &f->effects[3];
            put_be32(fresh, XJ_SL_SIZE);
            put_be32(fresh + 4, XJ_EA_SL2);
            CHECK(ack->lsa == list_ls + 8 && ack->size == 8 && payload_is(f, ack, at_ack, 8) &&
                      payload_is(f, ack, fresh, 8),
                  "%s: the ack payload is not the element as it is at the ack (lsa 0x%X size %u)", what, ack->lsa,
                  ack->size);
            CHECK(!payload_is(f, ack, issued + 8, 8), "%s: the ack payload is the element as it was at issue", what);
            CHECK(x->kind == SPU_FX_XFER && x->ea == XJ_EA_SL2 && x->lsa == dest + 16 && x->size == XJ_SL_SIZE,
                  "%s: the resumed transfer did not follow the rewritten element", what);
        } else {
            CHECK(ack->lsa == 0 && ack->size == 0 && ack->blob == SPU_XDIFF_NO_BLOB,
                  "%s: an ack with nothing parked must be lsa 0 size 0 no payload (lsa 0x%X size %u blob 0x%X)", what,
                  ack->lsa, ack->size, ack->blob);
        }
    }
    check_roundtrip(what, f);
    ctx_teardown(c);
}

int main(void)
{
    static spu_xdiff_fixture fx[2], fx_limit;
    run_result run[2];

    spu_recomp_register();
    spu_context_init(&g_scratch, 0);
    for (int i = 0; i < 2; i++) CHECK(spu_xdiff_fixture_init(&fx[i]) == 0, "fixture %d: out of memory", i);
    CHECK(spu_xdiff_fixture_init(&fx_limit) == 0, "limit fixture: out of memory");

    baseline_run(0);
    baseline_run(1);

    for (int lifted = 0; lifted < 2; lifted++) {
        const char* tier = lifted ? "lifted" : "interpreted";
        record_tier(lifted, &fx[lifted], 0, &run[lifted]);
        if (run[lifted].begin_rc != 0) continue;
        check_records(tier, &fx[lifted]);
        check_state(tier, lifted, &fx[lifted], &run[lifted]);
        check_roundtrip(tier, &fx[lifted]);
    }
    if (run[0].begin_rc == 0) check_interpreted_pcs(&fx[0]);
    if (run[0].begin_rc == 0 && run[1].begin_rc == 0) check_tiers_equal(&fx[0], &fx[1]);
    if (run[0].begin_rc == 0) check_identity_flip(&fx[0]);

    /* 16 bytes cannot hold the first GET's 32: record 1 is the first that does not fit, so one record is kept */
    check_capture_limit("capture limit (first payload)", 0, 16, 1, &fx_limit);
    /* 40 bytes hold the GET's 32 but not the list's 16 on top: the MFC_CMD of the list is the one that fails, after the
     * five records before it, and the many later records without a payload are not appended behind it */
    check_capture_limit("capture limit (list payload)", 0, 40, 5, &fx_limit);
    check_capture_limit("capture limit (lifted)", 1, 16, 1, &fx_limit);

    check_stale_latches(&fx_limit);
    check_stall_ack(&fx_limit, 1);
    check_stall_ack(&fx_limit, 0);
    check_refusals(&fx_limit);

    for (int i = 0; i < 2; i++) spu_xdiff_fixture_free(&fx[i]);
    spu_xdiff_fixture_free(&fx_limit);
    if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
    printf("OK: xdiff recorder (interpreted and lifted)\n");
    return 0;
}

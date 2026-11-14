/* test_spu_effect_port.c - the effect port seam of the channel runtime
 * (spu_effect_port.h, docs/SPU_XDIFF.md section 5).
 *
 * One synthetic job (gen_xdiff_port_job.py: GET, tag wait, a two-element list
 * GET, GETLLAR/PUTLLC, rchcnt, mailbox write) runs interpreted and lifted
 * against the real channel, DMA and interpreter runtime:
 *
 *   (a) a tap sees every effect in order, list elements, the atomic and
 *       MFC_WrTagMask included, and no latch write; an unobserved run ends in
 *       the same state;
 *   (b) a served context completes with vm_base == NULL and no MFC engine, in
 *       the state the real run reached, with the same transcript;
 *   (c) a served lifted context that branches to local store no lift holds ends
 *       with lift_miss == 1 and nothing interpreted (an unserved one
 *       interprets it, so the check can fail);
 *   (d) a serve callback that cannot answer ends the run under
 *       spu_run_with_halt, and without a landing pad the process aborts (child
 *       processes: this program re-executes itself with an argument).
 */
#include "../spu_context.h"
#include "../spu_dma.h"
#include "../spu_interp.h"
#include "../spu_coherency.h"
#include "ps3emu/lifter_abi.h"
#include "xdiff_port_job.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
void spu_recomp_register(void);
void spu_func_00000000(spu_context* ctx);
void spu_func_00000400(spu_context* ctx);

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
static void fill_world(uint8_t* w)
{
    static const struct { uint32_t ea, n; } r[] = {
        { XJ_EA_GET, XJ_GET_SIZE }, { XJ_EA_LIST0, XJ_LIST0_SIZE },
        { XJ_EA_LIST1, XJ_LIST1_SIZE }, { XJ_EA_ATOM, 128 },
        { XJ_EA_SL0, 3 * XJ_SL_SIZE }, { XJ_EA_ZERO, XJ_ZERO_SIZE } };
    memset(w, 0, sizeof g_mem);
    for (unsigned k = 0; k < sizeof r / sizeof r[0]; k++)
        for (uint32_t i = 0; i < r[k].n; i++) w[r[k].ea + i] = (uint8_t)((r[k].ea + i) * 131u + 7u);
}

static void put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* ---- a recording tap ------------------------------------------------------ */
enum { EV_WRCH = 1, EV_RDCH, EV_RCHCNT, EV_XFER };
typedef struct {
    int kind;
    uint32_t ch, value, lsa, size, cmd, tag, dir, status;
    uint64_t ea;
    int landed;   /* the bytes at lsa equal the world's at ea when the report is made */
} ev;
typedef struct {
    ev e[64];
    int n;
    const spu_context* ctx;
    const uint8_t* world;
    int wrong_ctx;
} transcript;

static ev* tr_add(transcript* t, const spu_context* ctx, int kind)
{
    if (ctx != t->ctx) t->wrong_ctx++;
    if (t->n >= 64) { g_fail++; printf("FAIL: transcript overflow\n"); return &t->e[63]; }
    ev* e = &t->e[t->n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    return e;
}
static void tap_wrch(void* u, const spu_context* c, uint32_t ch, uint32_t v)
{
    ev* e = tr_add((transcript*)u, c, EV_WRCH); e->ch = ch; e->value = v;
}
static void tap_rdch(void* u, const spu_context* c, uint32_t ch, uint32_t v)
{
    ev* e = tr_add((transcript*)u, c, EV_RDCH); e->ch = ch; e->value = v;
}
static void tap_rchcnt(void* u, const spu_context* c, uint32_t ch, uint32_t n)
{
    ev* e = tr_add((transcript*)u, c, EV_RCHCNT); e->ch = ch; e->value = n;
}
static uint8_t put_source_byte(uint32_t i) { return (uint8_t)(i * 5u + 3u); }
static void tap_xfer(void* u, const spu_context* c, const spu_port_xfer* x)
{
    transcript* t = (transcript*)u;
    ev* e = tr_add(t, c, EV_XFER);
    e->ea = x->ea; e->lsa = x->lsa; e->size = x->size; e->cmd = x->cmd; e->tag = x->tag;
    e->dir = x->dir; e->status = x->status;
    /* What each direction promises at report time: a GET's bytes have landed, a PUT's source is
     * still in local store (and is what the test planted), a zeroed range is zero. */
    if (x->dir == SPU_XFER_ZERO_EA) {
        e->landed = 1;
        for (uint32_t i = 0; i < x->size; i++) e->landed &= t->world[x->ea + i] == 0;
    } else {
        e->landed = x->lsa + x->size <= SPU_LS_SIZE && !memcmp(c->ls + x->lsa, t->world + x->ea, x->size);
        if (x->cmd == 0x20)
            for (uint32_t i = 0; i < x->size; i++) e->landed &= c->ls[x->lsa + i] == put_source_byte(i);
    }
}
static const spu_effect_tap kTap = { tap_wrch, tap_rdch, tap_rchcnt, tap_xfer };

/* What the job does, in order. The latch writes (channels 16 to 20) are absent. */
#define W(ch, v)              { EV_WRCH, ch, v, 0, 0, 0, 0, 0, 0, 0, 0 }
#define R(ch, v)              { EV_RDCH, ch, v, 0, 0, 0, 0, 0, 0, 0, 0 }
#define XT(ea_, lsa_, n_, cmd_, tag_, dir_, st_) \
    { EV_XFER, 0, 0, lsa_, n_, cmd_, tag_, dir_, st_, ea_, 1 }
#define X(ea_, lsa_, n_, cmd_, dir_, st_) XT(ea_, lsa_, n_, cmd_, XJ_TAG, dir_, st_)
static const ev kExpected[] = {
    W(MFC_Cmd, 0x40),
    X(XJ_EA_GET, XJ_LS_GET, XJ_GET_SIZE, 0x40, SPU_XFER_TO_LS, 0),
    W(MFC_WrTagMask, 1u << XJ_TAG),
    W(MFC_WrTagUpdate, 2),
    R(MFC_RdTagStat, 1u << XJ_TAG),
    W(MFC_Cmd, 0x44),
    X(XJ_EA_LIST0, XJ_LS_LIST_DEST, XJ_LIST0_SIZE, 0x40, SPU_XFER_TO_LS, 0),
    X(XJ_EA_LIST1, XJ_LS_LIST_DEST + 16, XJ_LIST1_SIZE, 0x40, SPU_XFER_TO_LS, 0),
    W(MFC_Cmd, 0x20),
    X(XJ_EA_PUT, XJ_LS_PUT, XJ_PUT_SIZE, 0x20, SPU_XFER_FROM_LS, 0),
    W(MFC_Cmd, 0x89),
    X(XJ_EA_ZERO, 0, XJ_ZERO_SIZE, 0x89, SPU_XFER_ZERO_EA, 0),
    W(MFC_Cmd, 0x44),
    XT(XJ_EA_SL0, XJ_LS_SLIST_DEST, XJ_SL_SIZE, 0x40, XJ_TAG2, SPU_XFER_TO_LS, 0),
    R(MFC_RdListStallStat, 1u << XJ_TAG2),
    W(MFC_WrListStallAck, XJ_TAG2),
    XT(XJ_EA_SL1, XJ_LS_SLIST_DEST + 16, XJ_SL_SIZE, 0x40, XJ_TAG2, SPU_XFER_TO_LS, 0),
    XT(XJ_EA_SL2, XJ_LS_SLIST_DEST + 32, XJ_SL_SIZE, 0x40, XJ_TAG2, SPU_XFER_TO_LS, 0),
    W(MFC_Cmd, 0xD0),
    X(XJ_EA_ATOM, XJ_LS_ATOM, 128, 0xD0, SPU_XFER_TO_LS, 4),
    R(MFC_RdAtomicStat, 4),
    W(MFC_Cmd, 0xB4),
    X(XJ_EA_ATOM, XJ_LS_ATOM, 128, 0xB4, SPU_XFER_FROM_LS, 0),
    R(MFC_RdAtomicStat, 0),
    { EV_RCHCNT, SPU_WrOutMbox, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    W(SPU_WrOutMbox, XJ_MBOX_VALUE),
};
#define NEXPECTED ((int)(sizeof kExpected / sizeof kExpected[0]))

static int ev_matches(const ev* g, const ev* w)
{
    if (g->kind != w->kind) return 0;
    if (g->kind != EV_XFER) return g->ch == w->ch && g->value == w->value;
    return g->ea == w->ea && g->lsa == w->lsa && g->size == w->size && g->cmd == w->cmd &&
           g->tag == w->tag && g->dir == w->dir && g->status == w->status && g->landed == w->landed;
}

static void check_transcript(const char* what, const transcript* t)
{
    CHECK(t->wrong_ctx == 0, "%s: a callback got another context (%d times)", what, t->wrong_ctx);
    CHECK(t->n == NEXPECTED, "%s: %d effects, expected %d", what, t->n, NEXPECTED);
    for (int i = 0; i < t->n && i < NEXPECTED; i++)
        CHECK(ev_matches(&t->e[i], &kExpected[i]),
              "%s: effect %d is kind %d ch %u value 0x%X ea 0x%llX lsa 0x%X size %u cmd 0x%X dir %u st %u landed %d;"
              " expected kind %d ch %u value 0x%X ea 0x%llX lsa 0x%X size %u cmd 0x%X dir %u st %u",
              what, i, t->e[i].kind, t->e[i].ch, t->e[i].value, (unsigned long long)t->e[i].ea,
              t->e[i].lsa, t->e[i].size, t->e[i].cmd, t->e[i].dir, t->e[i].status, t->e[i].landed,
              kExpected[i].kind, kExpected[i].ch, kExpected[i].value, (unsigned long long)kExpected[i].ea,
              kExpected[i].lsa, kExpected[i].size, kExpected[i].cmd, kExpected[i].dir, kExpected[i].status);
}

/* ---- a synthetic serve table: the world is a model, not guest memory --------- */
typedef struct {
    uint8_t world[sizeof g_mem];
    uint32_t mask, completed, tag_status, atomic_stat, out, out_n, stall_stat;
    struct { int valid; uint32_t tag, elem, remaining, dest; } parked;
    int refuse_tag_stat;   /* cannot answer a MFC_RdTagStat read */
    char reason[128];
} model;
static model g_model;

static SPU_PORT_NORETURN void m_cannot(model* m, spu_context* ctx, const char* what, uint32_t ch)
{
    snprintf(m->reason, sizeof m->reason, "%s of channel %u is not in the model", what, ch);
    spu_port_serve_halt(ctx, m->reason);
}
static void m_xfer(spu_context* ctx, uint64_t ea, uint32_t lsa, uint32_t n, uint32_t cmd, uint32_t dir,
                   uint32_t st)
{
    spu_port_xfer x;
    x.ea = ea; x.lsa = lsa; x.size = n; x.cmd = cmd; x.tag = ctx->mfc_tag; x.dir = dir; x.status = st;
    spu_port_report_xfer(ctx, &x);
}
/* Run list elements from `el`: the serve table's own list engine. An element with bit 31
 * stalls the list: the table keeps the rest until the guest acknowledges the tag. */
static void m_list(model* m, spu_context* ctx, uint32_t el, uint32_t left, uint32_t dest, uint32_t tag)
{
    for (uint32_t i = 0; i < left; i++, el += 8) {
        const uint32_t w0 = spu_ls_read32(ctx, el), sz = w0 & 0x7FFF, eal = spu_ls_read32(ctx, el + 4),
                       to = (dest & ~15u) | (eal & 15u);
        if (sz) {
            memcpy(ctx->ls + to, m->world + eal, sz);
            m_xfer(ctx, eal, to, sz, 0x40, SPU_XFER_TO_LS, 0);
            dest = (dest & ~15u) + ((sz + 15u) & ~15u);
        }
        if (w0 >> 31) {
            m->parked.valid = 1; m->parked.tag = tag; m->parked.elem = el + 8;
            m->parked.remaining = left - i - 1; m->parked.dest = dest;
            m->stall_stat |= 1u << tag;
            return;
        }
    }
    m->completed |= 1u << tag;
}
static void m_cmd(model* m, spu_context* ctx, uint32_t cmd)
{
    const uint32_t ea = ctx->mfc_eal, lsa = ctx->mfc_lsa & SPU_LS_MASK, n = ctx->mfc_size;
    switch (cmd) {
    case 0x40:
        memcpy(ctx->ls + lsa, m->world + ea, n);
        m_xfer(ctx, ea, lsa, n, 0x40, SPU_XFER_TO_LS, 0);
        break;
    case 0x20:
        memcpy(m->world + ea, ctx->ls + lsa, n);
        m_xfer(ctx, ea, lsa, n, 0x20, SPU_XFER_FROM_LS, 0);
        break;
    case 0x89:
        memset(m->world + ea, 0, n);
        m_xfer(ctx, ea, 0, n, 0x89, SPU_XFER_ZERO_EA, 0);
        break;
    case 0x44:
        m_list(m, ctx, ea & SPU_LS_MASK, n / 8, lsa, ctx->mfc_tag & 31u);
        return;
    case 0xD0: {
        const uint32_t l = lsa & ~127u, e = ea & ~127u;
        memcpy(ctx->ls + l, m->world + e, 128);
        m->atomic_stat = 4;
        m_xfer(ctx, e, l, 128, 0xD0, SPU_XFER_TO_LS, 4);
        return;
    }
    case 0xB4: {
        const uint32_t l = lsa & ~127u, e = ea & ~127u;
        memcpy(m->world + e, ctx->ls + l, 128);
        m->atomic_stat = 0;
        m_xfer(ctx, e, l, 128, 0xB4, SPU_XFER_FROM_LS, 0);
        return;
    }
    default:
        m_cannot(m, ctx, "an MFC command", cmd);
    }
    m->completed |= 1u << (ctx->mfc_tag & 31u);
}
static void sv_wrch(void* u, spu_context* ctx, uint32_t ch, uint32_t v)
{
    model* m = (model*)u;
    switch (ch) {
    case MFC_Cmd:         m_cmd(m, ctx, v); break;
    case MFC_WrTagMask:   m->mask = v; break;
    case MFC_WrTagUpdate: m->tag_status = m->completed & m->mask; break;
    case SPU_WrOutMbox:   m->out = v; m->out_n++; break;
    case MFC_WrListStallAck:
        if (m->parked.valid && m->parked.tag == (v & 31u)) {
            m->parked.valid = 0;
            m_list(m, ctx, m->parked.elem, m->parked.remaining, m->parked.dest, v & 31u);
        }
        break;
    default:              m_cannot(m, ctx, "a write", ch);
    }
}
static uint32_t sv_rdch(void* u, spu_context* ctx, uint32_t ch)
{
    model* m = (model*)u;
    if (ch == MFC_RdTagStat && !m->refuse_tag_stat) return m->tag_status;
    if (ch == MFC_RdAtomicStat) return m->atomic_stat;
    if (ch == MFC_RdListStallStat) { const uint32_t s = m->stall_stat; m->stall_stat = 0; return s; }
    m_cannot(m, ctx, "a read", ch);
}
static uint32_t sv_rchcnt(void* u, spu_context* ctx, uint32_t ch)
{
    model* m = (model*)u;
    if (ch == SPU_WrOutMbox) return m->out_n ? 0u : 1u;
    m_cannot(m, ctx, "a count", ch);
}
static const spu_effect_serve kServe = { sv_wrch, sv_rdch, sv_rchcnt };

/* ---- running a tier ---------------------------------------------------------- */
static void interp_entry(spu_context* c) { spu_interp_run(c, c->pc); }
static void (*entry_for(int lifted, uint32_t entry))(spu_context*)
{
    if (!lifted) return interp_entry;
    return entry == XJ_MISS_ENTRY ? spu_func_00000400 : spu_func_00000000;
}

static spu_context g_ctx;
static void ctx_setup(spu_context* c, int lifted, uint32_t entry)
{
    spu_context_init(c, 0);
    c->image_id = lifted ? 0 : -1;   /* -1: the interpreter must not rejoin a lift */
    c->pc = entry;
    if (!lifted) memcpy(c->ls, xj_code, XJ_CODE_BYTES);
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
}
static void ctx_teardown(spu_context* c)
{
    spu_coh_unregister(c);
    spu_mfc_release(c);
}

static spu_context g_base[2];   /* the unobserved run of each tier */

static void same_end_state(const char* what, const spu_context* a, const spu_context* b)
{
    CHECK(!memcmp(a->gpr, b->gpr, sizeof a->gpr), "%s: registers differ", what);
    CHECK(!memcmp(a->ls_store, b->ls_store, SPU_LS_SIZE), "%s: local store differs", what);
    CHECK(a->status == b->status && a->stop_code == b->stop_code, "%s: ended as status %u code 0x%X, expected %u 0x%X",
          what, a->status, a->stop_code, b->status, b->stop_code);
    /* mfc_tag_mask is not compared: a served context's mask lives in the serve table */
}

static int run_job(spu_context* c, int lifted, uint32_t entry)
{
    return spu_run_with_halt(entry_for(lifted, entry), c);
}

static void test_tier(int lifted)
{
    const char* tier = lifted ? "lifted" : "interpreted";
    char what[64];
    transcript tr;
    spu_effect_port port;
    spu_context* c = &g_ctx;
    int halted;

    /* the unobserved run: today's path */
    fill_world(g_mem);
    ctx_setup(c, lifted, XJ_ENTRY);
    CHECK(c->port == NULL && c->lift_miss == 0, "%s: a fresh context has a port or a miss", tier);
    halted = run_job(c, lifted, XJ_ENTRY);
    CHECK(!halted && c->status == SPU_STATUS_STOPPED_BY_STOP && c->stop_code == XJ_STOP_CODE,
          "%s: the unobserved run did not stop at the job's stop (halted %d status %u code 0x%X)", tier, halted,
          c->status, c->stop_code);
    CHECK(c->mfc_tag_mask == 1u << XJ_TAG, "%s: MFC_WrTagMask did not store", tier);
    g_base[lifted] = *c;
    ctx_teardown(c);

    /* (a) a tap sees every effect, in order, and changes nothing */
    fill_world(g_mem);
    memset(&tr, 0, sizeof tr);
    ctx_setup(c, lifted, XJ_ENTRY);
    port.serve = NULL; port.serve_user = NULL; port.tap = &kTap; port.tap_user = &tr;
    spu_port_set(c, &port);
    CHECK(spu_port_get(c) == &port, "%s: spu_port_get", tier);
    tr.ctx = c; tr.world = g_mem;
    halted = run_job(c, lifted, XJ_ENTRY);
    CHECK(!halted, "%s: the tapped run halted", tier);
    snprintf(what, sizeof what, "%s tap", tier);
    check_transcript(what, &tr);
    same_end_state(what, c, &g_base[lifted]);
    CHECK(c->mfc_tag_mask == g_base[lifted].mfc_tag_mask, "%s: tag mask 0x%X", what, c->mfc_tag_mask);
    ctx_teardown(c);

    /* (b) a served context: no guest memory, no MFC engine, the same end state and transcript */
    memset(&g_model, 0, sizeof g_model);
    fill_world(g_model.world);
    memset(g_mem, 0xEE, sizeof g_mem);   /* nothing may read this either */
    memset(&tr, 0, sizeof tr);
    ctx_setup(c, lifted, XJ_ENTRY);
    port.serve = &kServe; port.serve_user = &g_model; port.tap = &kTap; port.tap_user = &tr;
    spu_port_set(c, &port);
    tr.ctx = c; tr.world = g_model.world;
    vm_base = NULL;
    halted = run_job(c, lifted, XJ_ENTRY);
    vm_base = g_mem;
    snprintf(what, sizeof what, "%s served", tier);
    CHECK(!halted, "%s: the served run halted (%s)", what, g_model.reason);
    CHECK(!spu_mfc_has_engine(c), "%s: an MFC engine was created for a served context", what);
    CHECK(c->coh_listed == 0, "%s: the served context joined the reservation list", what);
    CHECK(g_model.out == XJ_MBOX_VALUE && g_model.out_n == 1, "%s: the model did not get the mailbox word", what);
    check_transcript(what, &tr);
    same_end_state(what, c, &g_base[lifted]);
    CHECK(g_model.mask == 1u << XJ_TAG && c->mfc_tag_mask == 0, "%s: the tag mask is not the model's alone", what);
    ctx_teardown(c);
    fill_world(g_mem);
}

static uint32_t w_il(int rt, uint32_t imm16) { return (0x081u << 23) | ((imm16 & 0xFFFFu) << 7) | (uint32_t)rt; }

static void plant_miss_target(spu_context* c)
{
    put_be32(c->ls + XJ_MISS_TARGET, w_il(10, XJ_MISS_MARK));
    put_be32(c->ls + XJ_MISS_TARGET + 4, XJ_MISS_STOP_CODE & 0x3FFFu);   /* stop */
}

static void test_miss(void)
{
    spu_effect_port port;
    spu_context* c = &g_ctx;
    int halted;

    /* unserved: the resolver interprets the live bytes, so the served check below can fail */
    fill_world(g_mem);
    ctx_setup(c, 1, XJ_MISS_ENTRY);
    plant_miss_target(c);
    halted = run_job(c, 1, XJ_MISS_ENTRY);
    CHECK(!halted && c->lift_miss == 0 && c->gpr[10]._u32[0] == XJ_MISS_MARK &&
          c->stop_code == XJ_MISS_STOP_CODE,
          "unserved lifted miss: the real resolver did not interpret the target (halted %d r10 0x%X code 0x%X)",
          halted, c->gpr[10]._u32[0], c->stop_code);
    ctx_teardown(c);

    /* (c) served and lifted: counted, ended, nothing interpreted */
    memset(&g_model, 0, sizeof g_model);
    ctx_setup(c, 1, XJ_MISS_ENTRY);
    plant_miss_target(c);
    port.serve = &kServe; port.serve_user = &g_model; port.tap = NULL; port.tap_user = NULL;
    spu_port_set(c, &port);
    vm_base = NULL;
    halted = run_job(c, 1, XJ_MISS_ENTRY);
    vm_base = g_mem;
    CHECK(halted == 1, "served lifted miss: the run did not end through the landing pad");
    CHECK(c->lift_miss == 1, "served lifted miss: lift_miss is %u, expected 1", c->lift_miss);
    CHECK(c->gpr[10]._u32[0] == 0, "served lifted miss: the unlifted bytes ran (r10 0x%X)", c->gpr[10]._u32[0]);
    CHECK(c->status != SPU_STATUS_STOPPED_BY_STOP, "served lifted miss: the unlifted stop ran");
    CHECK(!spu_mfc_has_engine(c), "served lifted miss: an MFC engine exists");
    ctx_teardown(c);

    /* served and interpreted: there is no lift to miss; the interpreter runs the bytes itself */
    ctx_setup(c, 0, XJ_MISS_ENTRY);
    plant_miss_target(c);
    spu_port_set(c, &port);
    halted = run_job(c, 0, XJ_MISS_ENTRY);
    CHECK(!halted && c->lift_miss == 0 && c->gpr[10]._u32[0] == XJ_MISS_MARK && c->stop_code == XJ_MISS_STOP_CODE,
          "served interpreted: lift_miss %u r10 0x%X code 0x%X", c->lift_miss, c->gpr[10]._u32[0], c->stop_code);
    ctx_teardown(c);
}

static void test_cannot_answer(int lifted)
{
    transcript tr;
    spu_effect_port port;
    spu_context* c = &g_ctx;
    const char* tier = lifted ? "lifted" : "interpreted";

    memset(&g_model, 0, sizeof g_model);
    fill_world(g_model.world);
    g_model.refuse_tag_stat = 1;
    memset(&tr, 0, sizeof tr);
    ctx_setup(c, lifted, XJ_ENTRY);
    port.serve = &kServe; port.serve_user = &g_model; port.tap = &kTap; port.tap_user = &tr;
    spu_port_set(c, &port);
    tr.ctx = c; tr.world = g_model.world;
    vm_base = NULL;
    const int halted = run_job(c, lifted, XJ_ENTRY);
    vm_base = g_mem;
    CHECK(halted == 1, "%s: a serve callback that cannot answer did not end the run", tier);
    CHECK(strstr(g_model.reason, "channel 24") != NULL, "%s: the serve table lost its reason", tier);
    CHECK(c->gpr[11]._u32[0] == 0, "%s: the context went on with a value (r11 0x%X)", tier, c->gpr[11]._u32[0]);
    CHECK(c->status != SPU_STATUS_STOPPED_BY_STOP, "%s: the job ran to its stop", tier);
    CHECK(tr.n == 4 && tr.e[3].kind == EV_WRCH && tr.e[3].ch == MFC_WrTagUpdate,
          "%s: the tap heard %d effects up to the refused read, expected 4 and no rdch", tier, tr.n);
    ctx_teardown(c);
}

static void test_tagmask_inline(void)
{
    transcript tr;
    spu_effect_port port;
    spu_context* c = &g_ctx;
    u128 v;

    memset(&v, 0, sizeof v);
    ctx_setup(c, 1, XJ_ENTRY);
    v._u32[0] = 5;
    spu_wrch(c, MFC_WrTagMask, v);
    CHECK(c->mfc_tag_mask == 5, "no port: MFC_WrTagMask did not store");
    CHECK(!spu_mfc_has_engine(c), "no port: MFC_WrTagMask reached the MFC engine table");

    memset(&tr, 0, sizeof tr);
    port.serve = NULL; port.serve_user = NULL; port.tap = &kTap; port.tap_user = &tr;
    spu_port_set(c, &port);
    tr.ctx = c; tr.world = g_mem;
    v._u32[0] = 6;
    spu_wrch(c, MFC_WrTagMask, v);
    v._u32[0] = 0x100;
    spu_wrch(c, MFC_LSA, v);
    CHECK(c->mfc_tag_mask == 6 && c->mfc_lsa == 0x100, "tap: the writes did not take effect");
    CHECK(tr.n == 1 && tr.e[0].kind == EV_WRCH && tr.e[0].ch == MFC_WrTagMask && tr.e[0].value == 6,
          "tap: heard %d effects; expected only the tag mask write, not the latch", tr.n);
    ctx_teardown(c);
}

/* ---- child processes: a serve that cannot answer, with no landing pad --------- */
static int child_main(const char* mode)
{
    spu_effect_port port;
    spu_effect_serve bad = kServe;
    spu_context* c = &g_ctx;
#if !defined(_WIN32)
    struct rlimit rl = { 0, 0 };
    setrlimit(RLIMIT_CORE, &rl);
#endif
    if (!strcmp(mode, "--child-bad-serve")) {
        bad.rchcnt = NULL;
        port.serve = &bad; port.serve_user = &g_model; port.tap = NULL; port.tap_user = NULL;
        ctx_setup(c, 1, XJ_ENTRY);
        spu_port_set(c, &port);
        return 3;   /* spu_port_set aborts first */
    }
    const int lifted = !strcmp(mode, "--child-no-pad-lifted");
    memset(&g_model, 0, sizeof g_model);
    fill_world(g_model.world);
    g_model.refuse_tag_stat = 1;
    port.serve = &kServe; port.serve_user = &g_model; port.tap = NULL; port.tap_user = NULL;
    ctx_setup(c, lifted, XJ_ENTRY);
    spu_port_set(c, &port);
    vm_base = NULL;
    if (lifted) spu_func_00000000(c);   /* no spu_run_with_halt: no landing pad */
    else spu_interp_run(c, XJ_ENTRY);
    return 3;   /* it went on: the parent expects an abort, not an exit */
}

static void expect_abort(const char* what, const char* argv0, const char* arg, const char* diagnostic)
{
#if defined(_WIN32)
    (void)argv0; (void)arg; (void)diagnostic;
    printf("SKIP: %s (the child-process check needs fork/exec)\n", what);
#else
    int fds[2];
    char buf[4096];
    size_t len = 0;
    int st = 0;
    if (pipe(fds) != 0) { g_fail++; printf("FAIL: %s: pipe\n", what); return; }
    const pid_t pid = fork();
    if (pid < 0) { g_fail++; printf("FAIL: %s: fork\n", what); return; }
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]); close(fds[1]);
        execl(argv0, argv0, arg, (char*)NULL);
        _exit(127);
    }
    close(fds[1]);
    for (ssize_t n; len < sizeof buf - 1 && (n = read(fds[0], buf + len, sizeof buf - 1 - len)) > 0;) len += (size_t)n;
    close(fds[0]);
    buf[len] = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT, "%s: the child did not end by abort (status 0x%X)", what, st);
    CHECK(strstr(buf, diagnostic) != NULL, "%s: the child's diagnostic lacks \"%s\": %s", what, diagnostic, buf);
#endif
}

int main(int argc, char** argv)
{
    if (argc > 1) return child_main(argv[1]);
    spu_recomp_register();
    test_tagmask_inline();
    test_tier(0);
    test_tier(1);
    test_miss();
    test_cannot_answer(0);
    test_cannot_answer(1);
    expect_abort("no landing pad, lifted", argv[0], "--child-no-pad-lifted", "no halt landing pad is armed");
    expect_abort("no landing pad, interpreted", argv[0], "--child-no-pad-interp", "no halt landing pad is armed");
    expect_abort("serve table with a missing member", argv[0], "--child-bad-serve", "lacks rchcnt");
    if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
    printf("OK: effect port seam (interpreted and lifted)\n");
    return 0;
}

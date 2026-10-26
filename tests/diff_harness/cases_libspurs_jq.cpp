/* Cases for libspurs_jq, SPURS JobQueue2 (libs/spurs/cellSpursJq.c and libs/spurs/jq2 vs the firmware's
 * libspurs_jq). Argument checks and guest-memory effects only: no SPU job ever runs. Row ids (C06, S05,
 * N03, ...) are the firmware verdicts of re_jq2.md / issue #19.
 *
 * V1 harness: every non-Match case declares a causal witness (expect_cause, expect_step, and
 * for a memory divergence the first differing byte's range; for a lifted block its blocker and
 * step), so a case cannot pass for an unrelated reason. The sequences run per side, and the
 * HLE side's workers are paused after a successful create (post hook) so nothing drains the ring.
 *
 * Prototypes (guest pointers are u32, r3..r10 then the caller's parameter save area):
 *   _cellSpursCreateJobQueueWithJobDescriptorPool(rev, sdk, spurs, queue, attr, pool, pool_attr, name, ring, depth, workers, extra)
 *   _cellSpursCreateJobQueue(rev, sdk, spurs, queue, attr, name, ring, depth, workers, extra)
 *   cellSpursShutdownJobQueue(queue)      cellSpursJoinJobQueue(queue, exit_code*)
 *   cellSpursJobQueuePort2Create(port, queue)   cellSpursJobQueuePort2Destroy(port)
 *   _cellSpursJobQueuePort2PushJobBody(port, desc, size, tag, flags)
 *   _cellSpursJobQueuePort2CopyPushJobBody(port, src, size, copy_size, tag, flags)
 *   cellSpursJobQueuePort2PushSync(port, tag_mask, flags)   cellSpursJobQueuePort2Sync(port, mode)
 *   cellSpursJobQueueAttribute{Initialize,SetMaxSizeJobDescriptor,SetGrabParameters,SetSubmitWithEntryLock,
 *                              SetDoBusyWaiting,SetIsHaltOnError,SetIsJobTypeMemoryCheck}, ...SetMaxGrab (FW only)
 *
 * What stands in for what the module imports (policy at the bottom of this file):
 *   libsre cellSpurs*   a minimal workload model: Add returns wid 0, Shutdown marks it, Wait succeeds only after
 *                       Shutdown (otherwise the lifted call stops: libsre's answer depends on SPURS state that does
 *                       not exist here); everything else answers 0. No SPURS instance exists.
 *   lv2 syscalls        event flags and the page-attribute query (351) are answered here; a blocking wait (85)
 *                       blocks for good (the case timeout turns it into an outcome).
 *   sysPrxForUser       _sys_memset, _sys_strncmp, sys_process_get_paramsfo run through the HLE; lwmutex/lwcond
 *                       create/destroy are modelled (they write nothing: the HLE has no such objects either).
 * On the HLE side the worker threads are paused right after a successful create (intake paused), so nothing
 * drains the ring: pushes then leave exactly the bytes the push wrote. */
#include "dh_engine.h"
#include "ppu/ppu_context.h"
#include "syscalls/sys_event.h"
#include "spurs/jq2/executor.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

using namespace dh;

namespace {
constexpr uint32_t N_ATTRINIT = 0x43DDAB4F, N_SMAXDESC = 0x1686957E, N_SGRAB = 0xE70F874E, N_SLOCK = 0x0F03F712,
                   N_SBUSY = 0x0582338A, N_SHALT = 0xFF03CC79, N_SMEMCHK = 0x742CEC0D, N_SMAXGRAB = 0x5FB9B05D,
                   N_CREATE = 0xF244E799, N_CREATEPOOL = 0x983BE7FC, N_SHUT = 0x8C97A96C, N_JOIN = 0x8CE4CFFA,
                   N_PCREATE = 0xF11FE0F1, N_PDESTROY = 0xC485D207, N_PUSHJOB = 0xCF89F218, N_COPYPUSH = 0xC39173A4,
                   N_PUSHSYNC = 0xFC526B72, N_SYNC = 0xFACB3CED;

constexpr uint32_t SPURS = 0x20000, ATTR = 0x21000, PATTR = 0x21100, NAME = 0x21200, OUT = 0x21300,
                   QUEUE = 0x30000, QUEUE2 = 0x32000, RING = 0x40000, RING2 = 0x44000, RINGBIG = 0x50000,
                   POOL = 0x60000, PORT = 0x70000, PORT2 = 0x70080, DESC = 0x78000, SDK = 0x330000,
                   BAD_EA = 0xFFFFFFF0u;

std::vector<Case>* g_out;

/* ---- harness state the models read (set in a case's setup, before the fork) ---- */
int  g_pageattr = 0;       /* 0 ANY(0xF) | 1 syscall fails (EFAULT) | 2 access_right 0x8 (what the runtime's own handler says) | 3 access_right 0 */
bool g_wl_shutdown = false;

Step S(const char* f, uint32_t nid, std::initializer_list<Arg> a, std::function<void()> pre = nullptr, std::function<void()> post = nullptr)
{ Step s = step(f, nid, a); s.pre = std::move(pre); s.post = std::move(post); return s; }

/* Stop the HLE worker threads from draining the ring (only the HLE side has any). */
void pause_q(uint32_t q) { if (on_hle_side()) ps3recomp::spurs::jq2::set_intake_paused(q, true); }

/* ---- guest structures ---- */
void put_attr(uint32_t ea, uint32_t classes = 4, uint32_t maxdesc = 256, uint8_t lock = 0, uint8_t busy = 0, uint8_t halt = 0, uint8_t chk = 0, uint32_t grab = 255)
{
    fill(ea, 0x20, 0);
    wr32(ea + 4, classes); wr32(ea + 8, maxdesc);
    wr8(ea + 0xC, lock); wr8(ea + 0xD, busy); wr8(ea + 0xE, halt); wr8(ea + 0xF, chk);
    wr32(ea + 0x10, grab);
}
/* pool attribute: eight u32 block counts, class k holds 64/128/256/384/512/640/768/896-byte blocks */
void put_pattr(uint32_t ea, std::initializer_list<uint32_t> counts)
{
    fill(ea, 0x40, 0); int i = 0; for (uint32_t c : counts) { if (i < 8) wr32(ea + 4 * i, c); ++i; }
}
void put_name() { memcpy(mem + NAME, "jq2test\0", 8); }

struct CA {
    Arg rev = Arg(1), sdk = Arg(SDK), spurs = Arg(SPURS), queue = Arg(QUEUE), attr = Arg(ATTR), pool = Arg(0), pattr = Arg(0),
        name = Arg(NAME), ring = Arg(RING), depth = Arg(16), workers = Arg(2), extra = Arg(0);
    bool poolfn = false;
};
Step create(std::function<void(CA&)> f = nullptr)
{
    CA a; if (f) f(a);
    const uint32_t q = a.queue.slot < 0 ? (uint32_t)a.queue.lit : 0;
    std::function<void()> post = [q] { if (q) pause_q(q); };
    if (a.poolfn)
        return S("_cellSpursCreateJobQueueWithJobDescriptorPool", N_CREATEPOOL,
                 {a.rev, a.sdk, a.spurs, a.queue, a.attr, a.pool, a.pattr, a.name, a.ring, a.depth, a.workers, a.extra}, nullptr, post);
    return S("_cellSpursCreateJobQueue", N_CREATE,
             {a.rev, a.sdk, a.spurs, a.queue, a.attr, a.name, a.ring, a.depth, a.workers, a.extra}, nullptr, post);
}
Step create_pool(std::function<void(CA&)> f = nullptr)
{ return create([f](CA& a) { a.poolfn = true; a.pool = Arg(POOL); a.pattr = Arg(PATTR); if (f) f(a); }); }
Step shut(Arg q = QUEUE) { return S("cellSpursShutdownJobQueue", N_SHUT, {q}); }
Step join(Arg q = QUEUE, Arg out = OUT) { return S("cellSpursJoinJobQueue", N_JOIN, {q, out}); }
Step pcreate(Arg port = PORT, Arg q = QUEUE) { return S("cellSpursJobQueuePort2Create", N_PCREATE, {port, q}); }
Step pdestroy(Arg port = PORT) { return S("cellSpursJobQueuePort2Destroy", N_PDESTROY, {port}); }
Step push(Arg port = PORT, Arg desc = DESC, Arg size = 256, Arg tag = 0, Arg flags = 0)
{ return S("_cellSpursJobQueuePort2PushJobBody", N_PUSHJOB, {port, desc, size, tag, flags}); }
Step cpush(Arg port = PORT, Arg src = DESC, Arg size = 256, Arg copy = 256, Arg tag = 0, Arg flags = 0)
{ return S("_cellSpursJobQueuePort2CopyPushJobBody", N_COPYPUSH, {port, src, size, copy, tag, flags}); }
Step psync(Arg port = PORT, Arg mask = 1, Arg flags = 0) { return S("cellSpursJobQueuePort2PushSync", N_PUSHSYNC, {port, mask, flags}); }
Step sync(Arg port = PORT, Arg mode = 4) { return S("cellSpursJobQueuePort2Sync", N_SYNC, {port, mode}); }

/* A created queue with one port: the usual start of a push sequence. */
std::vector<Step> qp(std::function<void(CA&)> f = nullptr, bool pool = false)
{ return {pool ? create_pool(f) : create(f), pcreate()}; }
std::vector<Step> cat(std::vector<Step> a, std::initializer_list<Step> b) { for (auto& s : b) a.push_back(s); return a; }

/* ---- masks --------------------------------------------------------------- */
/* What the firmware's CreateCommon (and Port2Create) write into guest memory that the HLE does not model, found by
 * running the unmasked create case (c00) and read back against the disassembly. Every range below is listed in the
 * JSON of each case that uses it ("masked") and in the stage-3 notes. */
const std::vector<std::pair<uint32_t, uint32_t>> kMaskNone = {};
const std::vector<std::pair<uint32_t, uint32_t>> kMaskQueue = {
    /* filled in after the unmasked run, see add_masks() */
};
std::vector<std::pair<uint32_t, uint32_t>> g_default_mask;
/* Port2Destroy: the FW clears only the init byte (+0x20), leaving the queue link in port+0x08 and port+0x18; the HLE zeroes both
 * (p15_destroy_valid shows it unmasked). Cases that destroy a port for another reason mask just these two words. */
bool g_destroy_mask = false;

void add(const char* id, const char* row, std::vector<Step> steps, Expect e = Expect::Match, const char* note = "",
         std::function<void()> setup = nullptr, int timeout = 10, bool masked = true)
{
    Case c; c.id = id; c.row = row; c.steps = std::move(steps); c.expect = e; c.note = note; c.timeout_s = timeout;
    if (masked) c.mask = g_default_mask;
    if (masked && g_destroy_mask) for (uint32_t p : {PORT, PORT2, PORT + 0x100}) { c.mask.push_back({p + 0x08, 4}); c.mask.push_back({p + 0x18, 4}); }
    c.setup = [setup] {
        g_pageattr = 0; g_wl_shutdown = false;
        put_attr(ATTR); put_pattr(PATTR, {0, 0, 4}); put_name();
        fill(OUT, 16, 0);
        if (setup) setup();
    };
    g_out->push_back(std::move(c));
}
auto SA = [](uint32_t classes, uint32_t maxdesc) { return [=] { put_attr(ATTR, classes, maxdesc); }; };

/* ---- causal witnesses for every declared non-Match verdict ---------------- */
/* Filled in from the first full run: cause kind, the step the verdict turns on
 * (a sequence's 0-based step, or the completed-step count when a side ends
 * inside a step), the memory range the first differing byte must lie in, and
 * for a lifted block its reason. A declared witness that stops reproducing is
 * unexpected, so a case cannot keep passing for an unrelated reason. */
struct Witness { const char* id; const char* cause; int step; uint32_t lo, hi; const char* blocker; };
const Witness kWitness[] = {
    /* memory: the raw create diff and the port stores that precede an error */
    {"c00_unmasked_valid_create", "memory", 0, QUEUE + 0x0D, QUEUE + 0x0E, nullptr},
    {"p06_create_queue_not_created", "memory", 0, PORT, PORT + 1, nullptr},
    {"p09_create_after_shutdown", "memory", 2, PORT, PORT + 1, nullptr},
    {"p15_destroy_valid", "memory", 2, PORT + 0x08, PORT + 0x0C, nullptr},
    /* status: the HLE stops by name (or returns) where the firmware returns (or stops) */
    {"c34_depth_0", "status", 0, 0, 0, nullptr},
    {"c38_workers_0", "status", 0, 0, 0, nullptr},
    {"s05_size_0", "status", 2, 0, 0, nullptr},
    {"s14c_pushjob_flag_bit1_2", "status", 2, 0, 0, nullptr},
    {"s14c_pushjob_flag_bit1_3", "status", 2, 0, 0, nullptr},
    {"s14c_pushjob_flag_bit1_6", "status", 2, 0, 0, nullptr},
    {"s14c_pushjob_flag_bit1_7", "status", 2, 0, 0, nullptr},
    {"s14g_copypush_flag_bit1_2", "status", 2, 0, 0, nullptr},
    {"s14g_copypush_flag_bit1_3", "status", 2, 0, 0, nullptr},
    /* return: both sides returned and disagreed on r3 at this step */
    {"c50_pageattr_syscall_fails", "return", 0, 0, 0, nullptr},
    {"c51_pageattr_access_right_8", "return", 0, 0, 0, nullptr},
    {"c52_pageattr_access_right_0", "return", 0, 0, 0, nullptr},
    {"c60_pool_count0_odd", "return", 0, 0, 0, nullptr},
    {"c61_pool_count0_odd_3", "return", 0, 0, 0, nullptr},
    {"c70_create_twice_same_queue", "return", 1, 0, 0, nullptr},
    {"c71_create_shutdown_create", "return", 2, 0, 0, nullptr},
    {"c73_create_twice_other_ring", "return", 1, 0, 0, nullptr},
    {"q09_shutdown_garbage_queue_block", "return", 0, 0, 0, nullptr},
    {"q10_join_garbage_queue_block", "return", 0, 0, 0, nullptr},
    {"q12_shutdown_twice", "return", 2, 0, 0, nullptr},
    {"q13_shutdown_three_times", "return", 2, 0, 0, nullptr},
    {"q17_join_twice", "return", 3, 0, 0, nullptr},
    {"q18_shutdown_after_join", "return", 3, 0, 0, nullptr},
    {"q21_wid_field_over_1f", "return", 1, 0, 0, nullptr},
    {"q22_join_wid_field_over_1f", "return", 2, 0, 0, nullptr},
    {"p20_destroy_after_join", "return", 4, 0, 0, nullptr},
    {"s01b_order_desc_null_vs_uninit_port", "return", 1, 0, 0, nullptr},
    {"s09_no_pool_bad_copy_size", "return", 2, 0, 0, nullptr},
    {"s09c_no_pool_copy_1024", "return", 2, 0, 0, nullptr},
    {"s09d_no_pool_copy_65", "return", 2, 0, 0, nullptr},
    {"n03a_pushjob_desc_at_pool_base", "return", 2, 0, 0, nullptr},
    {"n03b_pushjob_desc_in_pool", "return", 2, 0, 0, nullptr},
    {"n03c_pushjob_desc_last_block", "return", 2, 0, 0, nullptr},
    {"n07a_pushjob_slot_not_allocated", "return", 2, 0, 0, nullptr},
    {"n07b_pushsync_slot_not_allocated", "return", 2, 0, 0, nullptr},
    {"n07d_copypush_slot_not_allocated", "return", 2, 0, 0, nullptr},
    {"r06a_pushjob_null_port", "return", 1, 0, 0, nullptr},
    {"r06b_pushjob_misaligned_port", "return", 1, 0, 0, nullptr},
    {"r06e_pushsync_uninit_port_zero", "return", 1, 0, 0, nullptr},
    {"r06f_pushsync_uninit_port_garbage", "return", 1, 0, 0, nullptr},
    {"r06g_copypush_null_port", "return", 1, 0, 0, nullptr},
    {"y01a_sync_mode_bad_null_port", "return", 1, 0, 0, nullptr},
    {"y01c_sync_mode_bad_misaligned_port", "return", 1, 0, 0, nullptr},
    {"y01d_sync_mode_bad_uninit_port", "return", 1, 0, 0, nullptr},
    {"y04_sync_uninit_port_zero", "return", 1, 0, 0, nullptr},
    {"y05_sync_after_destroy", "return", 3, 0, 0, nullptr},
    /* no-hle: the firmware-only export */
    {"a11_setmaxgrab_null", "no-hle", 0, 0, 0, nullptr},
    {"a12_setmaxgrab_valid", "no-hle", 0, 0, 0, nullptr},
    {"a13_setmaxgrab_bounds", "no-hle", 0, 0, 0, nullptr},
    {"n05a_pooled_push_desc_in_pool", "no-hle", 2, 0, 0, nullptr},
    {"n05b_pooled_push_desc_outside_pool", "no-hle", 2, 0, 0, nullptr},
    {"n05c_pooled_push_flags_8", "no-hle", 2, 0, 0, nullptr},
    /* the intentionally blocking pushes: both sides wait for good -> timeout */
    {"s11b_pool_four_blocks_then_exhausted", "hang", 6, 0, 0, nullptr},
    {"s15d_full_ring_blocking_push_after_4", "hang", 6, 0, 0, nullptr},
    {"s15d_full_ring_blocking_push_after_5", "hang", 7, 0, 0, nullptr},
    {"s15d_full_ring_blocking_push_after_6", "hang", 8, 0, 0, nullptr},
    {"s15d_full_ring_blocking_push_after_7", "hang", 9, 0, 0, nullptr},
    {"s15d_full_ring_blocking_push_after_8", "hang", 10, 0, 0, nullptr},
    {"s15e_flags1_pushes_block_when_full", "hang", 6, 0, 0, nullptr},
    {"s15f_pushsync_blocks_when_full", "hang", 6, 0, 0, nullptr},
    {"s15g_copypush_blocks_when_full", "hang", 6, 0, 0, nullptr},
    /* lifted blocks: a model that must stop, and the firmware's assertion trap */
    {"q16_join_before_shutdown", "blocked", 1, 0, 0, "model-stop"},
    {"n07c_destroy_slot_not_allocated", "blocked", 2, 0, 0, "import-policy"},
};

void apply_witnesses()
{
    for (const Witness& w : kWitness) {
        bool found = false;
        for (auto& c : *g_out) {
            if (c.id != w.id) continue;
            c.expect_cause = w.cause;
            c.expect_step = w.step;
            if (w.hi > w.lo) { c.expect_range_lo = w.lo; c.expect_range_hi = w.hi; }
            if (w.blocker) { c.blocker = w.blocker; c.blocker_step = w.step; }
            found = true;
            break;
        }
        if (!found) {
            fprintf(stderr, "cases_libspurs_jq: witness for unknown case %s\n", w.id);
            abort();
        }
    }
}
}  // namespace

namespace dh {
/* ---- import and syscall models ------------------------------------------- */
#define CTXP(v) ppu_context* c = (ppu_context*)(v)
#define SYSCALL_MODEL(name, fn) static void name(void* v) { ppu_context* c = (ppu_context*)v; c->gpr[3] = (uint64_t)fn(c); }
SYSCALL_MODEL(sc_evf_create, sys_event_flag_create)
SYSCALL_MODEL(sc_evf_destroy, sys_event_flag_destroy)
SYSCALL_MODEL(sc_evf_set, sys_event_flag_set)
static void sc_evf_wait(void*) { model_block("sys_event_flag_wait (syscall 85): the ring is full or a Sync waits"); }
/* sys_memory_get_page_attribute(addr, sys_page_attribute_t*): struct = {u64 attribute; u64 access_right; u32 page_size; u32 pad}.
 * CreateCommon tests access_right & 2 (SPU thread). The runtime's own handler (runtime/syscalls/sys_memory.c) reports
 * access_right 8, which fails that test; the harness answers 0xF like lv2 does for ordinary user memory. */
static void sc_page_attr(void* v)
{
    CTXP(v);
    if (g_pageattr == 1) { c->gpr[3] = 0x8001000Dull /*EFAULT*/; return; }
    const uint32_t out = (uint32_t)c->gpr[4];
    wr64(out, 0x40000); wr64(out + 8, g_pageattr == 2 ? 8 : g_pageattr == 3 ? 0 : 0xF); wr32(out + 0x10, 0x10000); wr32(out + 0x14, 0);
    c->gpr[3] = 0;
}
static void m_zero(void* v) { ((ppu_context*)v)->gpr[3] = 0; }
static void m_wl_add(void* v) { CTXP(v); if (c->gpr[4]) wr32((uint32_t)c->gpr[4], 0); g_wl_shutdown = false; c->gpr[3] = 0; }
static void m_wl_shutdown(void* v) { g_wl_shutdown = true; ((ppu_context*)v)->gpr[3] = 0; }
static void m_wl_wait(void* v)
{
    if (!g_wl_shutdown) model_stop("cellSpursWaitForWorkloadShutdown before ShutdownWorkload: libsre's answer depends on SPURS workload state (not modelled)");
    ((ppu_context*)v)->gpr[3] = 0;
}
ImportPolicy import_policy()
{
    ImportPolicy p;
    p.dflt = Policy::Stop;
    p.by_nid[0x68B9B011] = Policy::Forward;   /* _sys_memset */
    p.by_nid[0x04E83D2C] = Policy::Forward;   /* _sys_strncmp */
    p.by_nid[0x9F04F7AF] = Policy::Forward;   /* _sys_printf: the FW's assertion message (stderr); the _sys_trap_process that follows stops the call */
    p.by_nid[0xE75C40F2] = Policy::Forward;   /* sys_process_get_paramsfo */
    p.by_nid[0x5D9A7034] = Policy::Forward;   /* cellFiberPpuSelf: NULL outside a fiber, as for any title thread */
    const uint32_t zero_models[] = {0x4A5EAB63 /*WorkloadAttributeSetName*/, 0x57E4DEC3 /*RemoveWorkload*/, 0x7E4EA023 /*WakeUp*/,
                                    0xD2E23FA9 /*SetExceptionEventHandler*/, 0xEFEB2679 /*_WorkloadAttributeInitialize*/,
                                    0x1D2BCA4B /*SendWorkloadSignal*/,
                                    0x2F85C0EF /*sys_lwmutex_create*/, 0xC3476D0C /*sys_lwmutex_destroy*/,
                                    0xDA0EB71A /*sys_lwcond_create*/, 0x1C9A942C /*sys_lwcond_destroy*/};
    for (uint32_t n : zero_models) { p.by_nid[n] = Policy::Model; p.models[n] = m_zero; }
    p.by_nid[0xC0158D8B] = Policy::Model; p.models[0xC0158D8B] = m_wl_add;          /* AddWorkloadWithAttribute */
    p.by_nid[0x98D5B343] = Policy::Model; p.models[0x98D5B343] = m_wl_shutdown;     /* ShutdownWorkload */
    p.by_nid[0x5FD43FE4] = Policy::Model; p.models[0x5FD43FE4] = m_wl_wait;         /* WaitForWorkloadShutdown */
    p.syscalls[82] = sc_evf_create; p.syscalls[83] = sc_evf_destroy; p.syscalls[87] = sc_evf_set;
    p.syscalls[85] = sc_evf_wait;
    p.syscalls[351] = sc_page_attr;
    return p;
}


/* helpers local to the case table */
static Step A_init(Arg a) { return S("cellSpursJobQueueAttributeInitialize", N_ATTRINIT, {a}); }
static Step A_maxsize(Arg a, Arg v) { return S("cellSpursJobQueueAttributeSetMaxSizeJobDescriptor", N_SMAXDESC, {a, v}); }
static Step A_grab(Arg a, Arg g, Arg m) { return S("cellSpursJobQueueAttributeSetGrabParameters", N_SGRAB, {a, g, m}); }
static Step A_byte(const char* f, uint32_t nid, Arg a, Arg v) { return S(f, nid, {a, v}); }
static Step A_maxgrab(Arg a, Arg v) { return S("cellSpursJobQueueAttributeSetMaxGrab", N_SMAXGRAB, {a, v}); }

void register_cases(std::vector<Case>& out)
{
    g_out = &out;
    const uint32_t A01 = 0x80410A01u; (void)A01;

    /* ---- the raw create diff (task 3): nothing masked --------------------------------------------------------- */
    g_default_mask.clear();
    add("c00_unmasked_valid_create", "C-ORD", {create()}, Expect::Diverge,
        "raw diff: the FW stores seven bytes of bookkeeping in the queue block that the HLE does not model (see the masks)", nullptr, 10, false);

    /* masks used by every other case: queue-relative, found by c00 and read against the lifted code */
    g_default_mask = {
        {QUEUE + 0x0D, 2},     /* bytes 1,1 stored at 0x020007D0 / 0x020007D8 (per-queue state bytes, no HLE counterpart) */
        {QUEUE + 0x60, 8},     /* the FW flag doubleword: running bit 0x800 (set last, 0x02000AF4), halt-on-error / busy-wait bits from the attr, and the
                                * byte at +0x65 = attr maxGrab. The HLE keeps "running" in its host registry (behaviour compared by return codes) */
        {QUEUE + 0x7A, 2},     /* +0x7A attr.submitWithEntryLock, +0x7B attr.grab top byte: copied by the FW, unused by the HLE */
        {QUEUE + 0x9C, 4},     /* revision copy (the HLE keeps it at +0xFFC only; the FW stores it at +0x9C as well) */
        {QUEUE + 0x1C4, 4},    /* constant 1 stored at 0x020007D4 */
        {QUEUE + 0xF18, 1},    /* attr.isJobTypeMemoryCheck, stored as 0/4 */
        /* found by the port cases (p07, p10): layouts the HLE chose differently, both compared by behaviour instead (BUSY at Shutdown, slot numbers in port+0x1C) */
        {QUEUE + 0x70, 4},     /* attached-port count: FW keeps a u16 in the HIGH half (+0x70..+0x71, Port2Create adds 0x10000), HLE a u32 (count in +0x73) */
        {QUEUE + 0x580, 16},   /* port-slot bitmap: FW allocates MSB first (cntlzw/srw 0x80000000, 0x02002F04..0F), HLE LSB first (ctz); same slot numbers */
    };

    /* ---- attributes (A01..A05) ------------------------------------------------------------------------------- */
    add("a01_init_null", "A01", {A_init(0u)});
    add("a02_init_garbage_attr", "A-init", {A_init(ATTR)}, Expect::Match, "", [] { fill(ATTR, 0x40, 0xA5); });
    add("a03_init_misaligned", "A01", {A_init(ATTR + 1)}, Expect::Match, "no alignment test in either");
    add("a04_init_oob", "A01", {A_init(BAD_EA)}, Expect::Match, "HLE named range stop, FW host fault: paired as guest-fault");
    add("a05_setmaxsize_null", "A01", {A_maxsize(0u, 256)});
    add("a06_setmaxsize_sweep", "A02", {A_maxsize(ATTR, 0), A_maxsize(ATTR, 128), A_maxsize(ATTR, 255), A_maxsize(ATTR, 256), A_maxsize(ATTR, 257),
        A_maxsize(ATTR, 384), A_maxsize(ATTR, 896), A_maxsize(ATTR, 1023), A_maxsize(ATTR, 1024), A_maxsize(ATTR, 0xFFFFFF80u), A_maxsize(ATTR, 512)});
    add("a07_setgrab_null", "A03", {A_grab(0u, 255, 4)});
    add("a08_setgrab_sweep", "A03", {A_grab(ATTR, 255, 4), A_grab(ATTR, 256, 4), A_grab(ATTR, 0, 0), A_grab(ATTR, 0, 1), A_grab(ATTR, 7, 16), A_grab(ATTR, 0, 17),
        A_grab(ATTR, 1, 0xFFFFFFFFu), A_grab(ATTR, 300, 0), A_grab(ATTR, 0xFFFFFFFFu, 2)});
    {
        struct F { const char* n; uint32_t nid; } fs[] = {{"cellSpursJobQueueAttributeSetSubmitWithEntryLock", N_SLOCK}, {"cellSpursJobQueueAttributeSetDoBusyWaiting", N_SBUSY},
                                                          {"cellSpursJobQueueAttributeSetIsHaltOnError", N_SHALT}, {"cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck", N_SMEMCHK}};
        std::vector<Step> nulls, vals;
        for (auto& f : fs) nulls.push_back(A_byte(f.n, f.nid, 0u, 1));
        add("a09_byte_setters_null", "A01", nulls);
        for (auto& f : fs) for (uint32_t v : {0u, 1u, 0x1FFu, 0x12345678u, 0xFFFFFFFFu}) vals.push_back(A_byte(f.n, f.nid, ATTR, v));
        add("a10_byte_setters_values", "A05", vals, Expect::Match, "low byte stored unmodified");
    }
    add("a11_setmaxgrab_null", "N05", {A_maxgrab(0u, 4)}, Expect::NoHle, "SetMaxGrab (0x5FB9B05D) is exported by the FW, the HLE has no handler");
    add("a12_setmaxgrab_valid", "N05", {A_maxgrab(ATTR, 4)}, Expect::NoHle, "FW: writes attr+4");
    add("a13_setmaxgrab_bounds", "N05", {A_maxgrab(ATTR, 0), A_maxgrab(ATTR, 17), A_maxgrab(ATTR, 16)}, Expect::NoHle, "FW: (v-1) > 15 -> A02");

    /* ---- queue create (C01..C09, N01, N02) -------------------------------------------------------------------- */
    add("c01_valid", "C-ORD", {create()});
    add("c02_valid_pool_export_no_pool", "C03", {create([](CA& a) { a.poolfn = true; })});
    add("c03_valid_pool", "P-class", {create_pool()});
    add("c04_valid_pool_all_classes", "P-class", {create_pool()}, Expect::Match, "", [] { put_pattr(PATTR, {2, 2, 2, 2, 2, 2, 2, 2}); });
    add("c05_valid_old_sdk_all_attr_flags", "C-sdk", {create([](CA& a) { a.sdk = Arg(0x30FFFF); })}, Expect::Match,
        "sdk <= 0x30FFFF takes the FW's older-SDK branch (flags copied differently); the HLE ignores the sdk. Differences land in masked bytes only",
        [] { put_attr(ATTR, 4, 256, 1, 1, 1, 1, 7); });
    add("c06_valid_new_sdk_all_attr_flags", "C-sdk", {create()}, Expect::Match, "", [] { put_attr(ATTR, 4, 256, 1, 1, 1, 1, 7); });
    add("c07_valid_unaligned_name_and_extra", "C-ORD", {create([](CA& a) { a.name = Arg(NAME + 3); a.extra = Arg(0x1234); a.rev = Arg(0x12345678u); })});

    add("c10_null_spurs", "C01", {create([](CA& a) { a.spurs = Arg(0); })});
    add("c11_null_queue", "C01", {create([](CA& a) { a.queue = Arg(0); })});
    add("c12_null_attr", "C01", {create([](CA& a) { a.attr = Arg(0); })});
    add("c13_null_ring", "C01", {create([](CA& a) { a.ring = Arg(0); })});
    add("c14_null_all", "C01", {create([](CA& a) { a.spurs = Arg(0); a.queue = Arg(0); a.attr = Arg(0); a.ring = Arg(0); })});
    add("c15_pool_set_pattr_clear", "C03a", {create_pool([](CA& a) { a.pattr = Arg(0); })});
    add("c16_pool_clear_pattr_set", "C03b", {create_pool([](CA& a) { a.pool = Arg(0); })});
    add("c17_pool_misaligned_pattr_clear", "C03a", {create_pool([](CA& a) { a.pool = Arg(POOL + 0x40); a.pattr = Arg(0); })}, Expect::Match, "xor test before the alignment tests");
    add("c18_pool_attr_unaligned_ok", "C02", {create_pool([](CA& a) { a.pattr = Arg(PATTR + 4); })}, Expect::Match, "pool_attr is never alignment-checked",
        [] { put_pattr(PATTR + 4, {0, 0, 4}); });
    add("c19_null_spurs_vs_xor_vs_align", "C-ORD", {create_pool([](CA& a) { a.spurs = Arg(0); a.pattr = Arg(0); a.ring = Arg(RING + 0x40); })});

    add("c20_spurs_misaligned", "C02", {create([](CA& a) { a.spurs = Arg(SPURS + 0x40); })});
    add("c21_queue_misaligned", "C02", {create([](CA& a) { a.queue = Arg(QUEUE + 0x40); })});
    add("c22_ring_misaligned", "C02", {create([](CA& a) { a.ring = Arg(RING + 0x40); })});
    add("c23_pool_misaligned", "C07", {create_pool([](CA& a) { a.pool = Arg(POOL + 0x40); })});
    add("c24_spurs_align_7", "C02", {create([](CA& a) { a.spurs = Arg(SPURS + 7); })});
    add("c25_order_spurs_vs_queue_vs_ring_align", "C-ORD", {create([](CA& a) { a.spurs = Arg(SPURS + 1); a.queue = Arg(QUEUE + 2); a.ring = Arg(RING + 4); })});
    add("c26_order_align_vs_depth", "C-ORD", {create([](CA& a) { a.ring = Arg(RING + 0x40); a.depth = Arg(0x9000); })});
    add("c27_order_pool_align_vs_depth", "C07", {create_pool([](CA& a) { a.pool = Arg(POOL + 0x40); a.depth = Arg(0x9000); })});
    add("c28_order_align_vs_workers", "C-ORD", {create([](CA& a) { a.queue = Arg(QUEUE + 0x40); a.workers = Arg(9); })});

    add("c30_depth_8001", "C04a", {create([](CA& a) { a.depth = Arg(0x8001); a.ring = Arg(RINGBIG); })});
    add("c31_depth_8000", "C04a", {create([](CA& a) { a.depth = Arg(0x8000); a.ring = Arg(RINGBIG); })}, Expect::Match, "ring memset of 0x40800 bytes");
    add("c32_depth_ffffffff", "C04a", {create([](CA& a) { a.depth = Arg(0xFFFFFFFFu); })});
    add("c33_depth_1", "C04a", {create([](CA& a) { a.depth = Arg(1); })});
    add("c34_depth_0", "C08", {create([](CA& a) { a.depth = Arg(0); })}, Expect::Diverge, "FW has no depth==0 test (OK); HLE stops by name jq2-unmodelled (policy)");
    add("c35_workers_9", "C04b", {create([](CA& a) { a.workers = Arg(9); })});
    add("c36_workers_8", "C04b", {create([](CA& a) { a.workers = Arg(8); })});
    add("c37_workers_1", "C04b", {create([](CA& a) { a.workers = Arg(1); })});
    add("c38_workers_0", "C08", {create([](CA& a) { a.workers = Arg(0); })}, Expect::Diverge, "FW has no workers==0 test (OK); HLE stops by name (policy)");
    add("c39_workers_ffffffff", "C04b", {create([](CA& a) { a.workers = Arg(0xFFFFFFFFu); })});
    add("c3a_depth_and_workers_bad", "C04", {create([](CA& a) { a.depth = Arg(0x8001); a.workers = Arg(9); })});

    add("c40_classes_0", "C06", {create()}, Expect::Match, "", SA(0, 256));
    add("c41_classes_1", "C06", {create()}, Expect::Match, "", SA(1, 256));
    add("c42_classes_16", "C06", {create()}, Expect::Match, "", SA(16, 256));
    add("c43_classes_17", "C06", {create()}, Expect::Match, "", SA(17, 256));
    add("c44_classes_80000001", "C06", {create()}, Expect::Match, "", SA(0x80000001u, 256));
    add("c45_maxdesc_255", "C06", {create()}, Expect::Match, "", SA(4, 255));
    add("c46_maxdesc_128", "C06", {create()}, Expect::Match, "", SA(4, 128));
    add("c47_maxdesc_384", "C06", {create()}, Expect::Match, "", SA(4, 384));
    add("c48_maxdesc_896", "C06", {create()}, Expect::Match, "", SA(4, 896));
    add("c49_maxdesc_1023", "C06", {create()}, Expect::Match, "", SA(4, 1023));
    add("c4a_maxdesc_1024", "C06", {create()}, Expect::Match, "", SA(4, 1024));
    add("c4b_maxdesc_448_not_128_multiple", "C06", {create()}, Expect::Match, "", SA(4, 448));
    add("c4c_maxdesc_ffffff80", "C06", {create()}, Expect::Match, "", SA(4, 0xFFFFFF80u));
    add("c4d_classes_bad_and_maxdesc_bad", "C06", {create()}, Expect::Match, "", SA(0, 0));

    add("c50_pageattr_syscall_fails", "N01", {create()}, Expect::Diverge, "FW: any syscall-351 failure -> A02; the HLE does not model page attributes (flat VM)",
        [] { g_pageattr = 1; });
    add("c51_pageattr_access_right_8", "N01", {create()}, Expect::Diverge,
        "FW: access_right & 2 clear -> A02. 8 is what runtime/syscalls/sys_memory.c reports today", [] { g_pageattr = 2; });
    add("c52_pageattr_access_right_0", "N01", {create()}, Expect::Diverge, "FW: access_right & 2 clear -> A02", [] { g_pageattr = 3; });
    add("c53_pageattr_fail_vs_depth_ok", "N01", {create([](CA& a) { a.depth = Arg(0x8001); a.ring = Arg(RINGBIG); })}, Expect::Match,
        "depth is tested before the syscall, both A02", [] { g_pageattr = 1; });

    add("c60_pool_count0_odd", "N02", {create_pool()}, Expect::Diverge, "FW: pool_attr word 0 bit 0 set -> A02 (0x02000A90); HLE has no such test",
        [] { put_pattr(PATTR, {1, 0, 4}); });
    add("c61_pool_count0_odd_3", "N02", {create_pool()}, Expect::Diverge, "same, count 3", [] { put_pattr(PATTR, {3, 0, 4}); });
    add("c62_pool_count0_even", "N02", {create_pool()}, Expect::Match, "", [] { put_pattr(PATTR, {2, 0, 4}); });
    add("c63_pool_count0_odd_but_attr_bad", "N02", {create_pool()}, Expect::Match, "the attr predicates come first, both A02", [] { put_pattr(PATTR, {1, 0, 4}); put_attr(ATTR, 0, 256); });
    add("c64_pool_other_counts_odd", "N02", {create_pool()}, Expect::Match, "only word 0 is tested", [] { put_pattr(PATTR, {2, 1, 3, 1}); });
    add("c65_pool_all_zero_counts", "P-class", {create_pool()}, Expect::Match, "", [] { put_pattr(PATTR, {}); });

    add("c70_create_twice_same_queue", "C09", {create(), create()}, Expect::Diverge,
        "FW never reads queue state before its memset: the second create re-initialises (OK); HLE returns STAT from its registry");
    add("c71_create_shutdown_create", "C09", {create(), shut(), create()}, Expect::Diverge, "same: HLE keeps the queue registered until Join -> STAT");
    add("c72_create_shutdown_join_create", "C09", {create(), shut(), join(), create()}, Expect::Match, "after Join the HLE forgets the queue; both accept");
    add("c73_create_twice_other_ring", "C09", {create(), create([](CA& a) { a.ring = Arg(RING2); })}, Expect::Diverge, "as c70, with a different ring");

    add("c80_oob_queue", "C10", {create([](CA& a) { a.queue = Arg(0xFFFFFF80u); })}, Expect::Match, "HLE range stop vs the FW's forwarded memset reaching the same HLE stop");
    add("c81_oob_ring", "C10", {create([](CA& a) { a.ring = Arg(0xFFFFFF80u); })}, Expect::Match, "same, ring");
    add("c82_oob_attr", "C05", {create([](CA& a) { a.attr = Arg(BAD_EA); })}, Expect::Match, "HLE range stop vs host fault: paired as guest-fault");
    add("c83_create_10arg_vs_12arg_null_name", "C-ORD", {create([](CA& a) { a.name = Arg(0); })});

    /* ---- Shutdown / Join (Q01..Q07, N09) ---------------------------------------------------------------------- */
    add("q01_shutdown_null", "Q01", {shut(0u)});
    add("q02_shutdown_misaligned", "Q05", {shut(QUEUE + 0x40)});
    add("q03_join_null_queue", "Q04", {join(0u, OUT)});
    add("q04_join_null_exit", "Q04", {join(QUEUE, 0u)});
    add("q05_join_both_null", "Q04", {join(0u, 0u)});
    add("q06_join_misaligned", "Q05", {join(QUEUE + 0x40, OUT)});
    add("q07_join_order_null_exit_vs_align", "Q07", {join(QUEUE + 0x40, 0u)});
    add("q08_shutdown_never_created_zeroed", "Q02", {shut()}, Expect::Match, "running flag clear -> A0F in both", [] { fill(QUEUE, 0x1000, 0); });
    add("q09_shutdown_garbage_queue_block", "N09", {shut()}, Expect::Diverge, "FW: queue+0x6C & 0x7F > 0x1F -> A02 first; HLE: not a registered queue -> A0F");
    add("q10_join_garbage_queue_block", "N09", {join()}, Expect::Diverge, "FW: wid > 0x1F -> A02; HLE: STAT");
    add("q11_create_shutdown", "Q01", {create(), shut()});
    add("q12_shutdown_twice", "Q02", {create(), shut(), shut()}, Expect::Diverge, "FW: running flag cleared by the first -> A0F; HLE: begin_shutdown() is idempotent -> OK");
    add("q13_shutdown_three_times", "Q02", {create(), shut(), shut(), shut()}, Expect::Diverge, "as q12; the third call too");
    add("q14_shutdown_join", "Q01", {create(), shut(), join()});
    add("q15_join_exit_value", "Q01", {create(), shut(), S("cellSpursJoinJobQueue", N_JOIN, {QUEUE, OUT}, [] { wr32(QUEUE + 0xF04, 0x1234ABCD); })}, Expect::Match,
        "*exit = queue+0xF04");
    add("q16_join_before_shutdown", "Q06", {create(), join()}, Expect::Blocked,
        "FW: the A0F comes from libsre's cellSpursWaitForWorkloadShutdown (SPURS state, not modelled): the lifted call stops there. HLE: STAT");
    add("q17_join_twice", "Q06", {create(), shut(), join(), join()}, Expect::Diverge, "FW: joined queue has w6C = spurs|0x20 -> wid 0x20 > 0x1F -> A02; HLE: STAT");
    add("q18_shutdown_after_join", "Q02", {create(), shut(), join(), shut()}, Expect::Diverge, "FW: A02 (joined marker); HLE: STAT (queue forgotten)");
    add("q19_port_attached_shutdown_busy", "Q03", {create(), pcreate(), shut()});
    g_destroy_mask = true;
    add("q20_port_destroyed_then_shutdown", "Q03", {create(), pcreate(), pdestroy(), shut(), join()});
    g_destroy_mask = false;
    add("q21_wid_field_over_1f", "N09", {create(), S("cellSpursShutdownJobQueue", N_SHUT, {QUEUE}, [] { wr32(QUEUE + 0x6C, SPURS | 0x3F); })}, Expect::Diverge,
        "created queue whose queue+0x6C & 0x7F is 0x3F: FW A02 before anything else; HLE does not read it");
    add("q22_join_wid_field_over_1f", "N09", {create(), shut(), S("cellSpursJoinJobQueue", N_JOIN, {QUEUE, OUT}, [] { wr32(QUEUE + 0x6C, SPURS | 0x3F); })}, Expect::Diverge, "same for Join");
    add("q23_shutdown_port_count_over_running_flag", "Q03", {create(), pcreate(), shut(), shut()}, Expect::Match, "BUSY is tested before the running flag, twice");

    /* ---- Port2Create / Destroy (R01..R08, N07) ---------------------------------------------------------------- */
    add("p01_create_null_port", "R01", {create(), pcreate(0u, QUEUE)});
    add("p02_create_null_queue", "R01", {create(), pcreate(PORT, 0u)});
    add("p03_create_port_misaligned", "R01", {create(), pcreate(PORT + 8, QUEUE)});
    add("p04_create_queue_misaligned", "R01", {create(), pcreate(PORT, QUEUE + 8)});
    add("p05_create_null_vs_align", "R01", {create(), pcreate(PORT + 8, 0u)});
    add("p06_create_queue_not_created", "R02", {pcreate()}, Expect::Diverge,
        "same A0F, but the FW has already memset the port, stored the queue link and the init byte (stores precede the allocator); the HLE leaves the port untouched",
        [] { fill(QUEUE, 0x1000, 0); });
    add("p07_create_valid", "R01", {create(), pcreate()});
    add("p08_create_twice_same_port", "R05", {create(), pcreate(), pcreate()});
    add("p09_create_after_shutdown", "R02", {create(), shut(), pcreate()}, Expect::Diverge, "same residual port state on A0F as p06");
    add("p10_create_three_ports", "R04", {create(), pcreate(PORT), pcreate(PORT2), pcreate(PORT + 0x100)});
    add("p11_destroy_null", "R06", {create(), pdestroy(0u)});
    add("p12_destroy_misaligned", "R06", {create(), pdestroy(PORT + 8)});
    add("p13_destroy_uninit_zero", "R06", {create(), pdestroy(PORT)}, Expect::Match, "", [] { fill(PORT, 0x80, 0); });
    add("p14_destroy_uninit_garbage", "R06", {create(), pdestroy(PORT)});
    add("p15_destroy_valid", "R06", {create(), pcreate(), pdestroy()}, Expect::Diverge,
        "return equal; the FW leaves port+0x08 and +0x18 holding the queue EA (only +0x20 is cleared), the HLE zeroes both", nullptr, 10, true);
    g_destroy_mask = true;
    add("p16_destroy_twice", "R07", {create(), pcreate(), pdestroy(), pdestroy()});
    g_destroy_mask = false;
    add("p17_destroy_with_accounted_push_busy", "R08", {create(), pcreate(), push(PORT, DESC, 256, 0, 1), pdestroy()});
    g_destroy_mask = true;
    add("p18_destroy_on_shutdown_queue", "R06", {create(), pcreate(), S("cellSpursShutdownJobQueue", N_SHUT, {QUEUE}, [] { wr32(QUEUE + 0x70, 0); }), pdestroy()}, Expect::Match,
        "the port count is zeroed by hand so that Shutdown succeeds with the port still attached; Destroy never tests the running flag in the FW, nor does the HLE before Join");
    g_destroy_mask = false;
    add("p19_destroy_state_byte_21", "R07", {create(), pcreate(), S("cellSpursJobQueuePort2Destroy", N_PDESTROY, {PORT}, [] { mem[PORT + 0x21] = 1; })}, Expect::Match, "+0x21 != 0 -> A0F");
    g_destroy_mask = true;
    add("p20_destroy_after_join", "R06", {create(), pcreate(), S("cellSpursShutdownJobQueue", N_SHUT, {QUEUE}, [] { wr32(QUEUE + 0x70, 0); }), join(), pdestroy()}, Expect::Diverge,
        "FW Destroy needs no live queue and works on a joined one; the HLE has forgotten the queue after Join -> A0F");
    g_destroy_mask = false;

    /* ---- pushes: front checks (S01..S04, S06, S08) ------------------------------------------------------------ */
    add("s01_desc_null", "S01", cat(qp(), {push(PORT, 0u)}));
    add("s01b_order_desc_null_vs_uninit_port", "S01", {create(), push(PORT, 0u)}, Expect::Diverge,
        "FW tests the descriptor before it touches the port (A11); HLE resolves the port first (A0F, uninitialised)");
    add("s01c_order_desc_null_vs_null_port", "S01", {create(), push(0u, 0u)}, Expect::Match, "A11 either way");
    add("s02_desc_misaligned_1", "S02", cat(qp(), {push(PORT, DESC + 1)}));
    add("s02_desc_misaligned_8", "S02", cat(qp(), {push(PORT, DESC + 8)}));
    add("s02_desc_aligned_16", "S02", cat(qp(), {push(PORT, DESC + 0x10)}));
    add("s02b_order_misaligned_vs_tag", "S02", cat(qp(), {push(PORT, DESC + 8, 256, 99)}));
    add("s02c_order_misaligned_vs_size", "S02", cat(qp(), {push(PORT, DESC + 8, 1000)}));
    add("s03_copy_src_misaligned", "S03", cat(qp(nullptr, true), {cpush(PORT, DESC + 8, 256, 256)}));
    add("s03_copy_src_null", "S01", cat(qp(nullptr, true), {cpush(PORT, 0u, 256, 256)}));
    add("s04_tag_16_and_more", "S04", cat(qp(), {push(PORT, DESC, 256, 16), push(PORT, DESC, 256, 0xFFFFFFFFu), push(PORT, DESC, 256, 15), push(PORT, DESC, 256, 0x10000)}));
    add("s04_tag_16_copy", "S04", cat(qp(nullptr, true), {cpush(PORT, DESC, 256, 256, 16), cpush(PORT, DESC, 256, 256, 15)}));
    add("s04b_order_tag_vs_size", "S04", cat(qp(), {push(PORT, DESC, 1000, 16)}));
    add("s06_size_over_max_256", "S06", cat(qp(), {push(PORT, DESC, 384), push(PORT, DESC, 256), push(PORT, DESC, 128)}));
    add("s08_copy_size_less_than_size", "S08", cat(qp(nullptr, true), {cpush(PORT, DESC, 256, 128), cpush(PORT, DESC, 128, 128)}));
    add("s08b_copy_size_less_than_size_no_pool", "S08", cat(qp(), {cpush(PORT, DESC, 256, 128)}), Expect::Match, "size > copy_size is tested before the pool");

    /* ---- S05: descriptor sizes against maxDescriptor 896 ------------------------------------------------------- */
    for (uint32_t sz : {64u, 128u, 256u, 384u, 512u, 640u, 768u, 896u, 100u, 1024u, 192u, 32u, 960u, 65u}) {
        static std::vector<std::string> keep;
        char id[40]; snprintf(id, sizeof id, "s05_size_%u", sz); keep.push_back(id);
        add(keep.back().c_str(), "S05", cat(qp(nullptr, false), {push(PORT, DESC, sz)}), Expect::Match, "", SA(4, 896));
    }
    add("s05_size_0", "S05", cat(qp(), {push(PORT, DESC, 0u)}), Expect::Diverge,
        "FW accepts 0 (0 & 127 == 0, class 0, entry as a 64-byte one); HLE stops by name (jq2-unmodelled, deliberate)", SA(4, 896));
    add("s05_all_sizes_sequence", "S05", cat(qp(), {push(PORT, DESC, 64), push(PORT, DESC, 128), push(PORT, DESC, 256), push(PORT, DESC, 384), push(PORT, DESC, 512),
        push(PORT, DESC, 640), push(PORT, DESC, 768), push(PORT, DESC, 896), push(PORT, DESC, 900), push(PORT, DESC, 1024)}), Expect::Match, "ring bytes of every class", SA(4, 896));
    add("s05_sizes_vs_max_384", "S06", cat(qp(), {push(PORT, DESC, 384), push(PORT, DESC, 512), push(PORT, DESC, 256), push(PORT, DESC, 64)}), Expect::Match, "", SA(4, 384));
    add("s05_copy_sizes_with_pool", "S05", cat(qp(nullptr, true), {cpush(PORT, DESC, 64, 64), cpush(PORT, DESC, 128, 128), cpush(PORT, DESC, 256, 256), cpush(PORT, DESC, 384, 384),
        cpush(PORT, DESC, 512, 512), cpush(PORT, DESC, 640, 640), cpush(PORT, DESC, 768, 768), cpush(PORT, DESC, 896, 896)}), Expect::Match, "pool with two blocks of every class",
        [] { put_attr(ATTR, 4, 896); put_pattr(PATTR, {2, 2, 2, 2, 2, 2, 2, 2}); });

    /* ---- S09..S11: CopyPush classes and order ------------------------------------------------------------------- */
    add("s09_no_pool_bad_copy_size", "S09", cat(qp(), {cpush(PORT, DESC, 64, 100)}), Expect::Diverge,
        "FW: copy_size shape (A02) before the class-configured test (A11); HLE tests the pool first (A11)");
    add("s09b_no_pool_valid_copy_size", "S09", cat(qp(), {cpush(PORT, DESC, 64, 128), cpush(PORT, DESC, 64, 64)}), Expect::Match, "valid shape, no pool: A11 in both");
    add("s09c_no_pool_copy_1024", "S09", cat(qp(), {cpush(PORT, DESC, 64, 1024)}), Expect::Diverge, "as s09, 1024");
    add("s09d_no_pool_copy_65", "S09", cat(qp(), {cpush(PORT, DESC, 64, 65), cpush(PORT, DESC, 64, 127)}), Expect::Diverge, "copy_size 65 / 127: FW shape test fails (A02); HLE A11");
    add("s10_pool_copy_size_sweep", "S10", cat(qp(nullptr, true), {cpush(PORT, DESC, 64, 100), cpush(PORT, DESC, 64, 1024), cpush(PORT, DESC, 64, 1023), cpush(PORT, DESC, 64, 0),
        cpush(PORT, DESC, 64, 192), cpush(PORT, DESC, 64, 127), cpush(PORT, DESC, 64, 128)}), Expect::Match, "pool has class 2 only: the last is class 1 -> A11",
        SA(4, 896));
    add("s11_class_unconfigured", "S11", cat(qp(nullptr, true), {cpush(PORT, DESC, 64, 64), cpush(PORT, DESC, 128, 384), cpush(PORT, DESC, 256, 256)}));
    add("s11b_pool_four_blocks_then_exhausted", "S11", cat(qp(nullptr, true), {cpush(PORT, DESC, 256, 256), cpush(PORT, DESC, 256, 256), cpush(PORT, DESC, 256, 256), cpush(PORT, DESC, 256, 256),
        cpush(PORT, DESC, 256, 256)}), Expect::Inconclusive, "class 2 holds four blocks: the fifth acquire waits for a block (both sides block, timeout)", [] { put_pattr(PATTR, {0, 0, 4}); }, 2);
    add("s11d_pool_four_blocks_ok", "S11", cat(qp(nullptr, true), {cpush(PORT, DESC, 256, 256), cpush(PORT, DESC, 256, 256), cpush(PORT, DESC, 256, 256), cpush(PORT, DESC, 256, 256)}));
    add("s11c_copy_content_in_pool", "S10", cat(qp(nullptr, true), {S("_cellSpursJobQueuePort2CopyPushJobBody", N_COPYPUSH, {PORT, DESC, 128, 256, 3, 0},
        [] { for (int i = 0; i < 256; ++i) mem[DESC + i] = (uint8_t)i; })}), Expect::Match, "the snapshot bytes land in the pool block");

    /* ---- S13: PushSync ----------------------------------------------------------------------------------------- */
    add("s13a_pushsync_tag_masks", "S13a", cat(qp(), {psync(PORT, 0), psync(PORT, 0x10000), psync(PORT, 0xFFFFFFFFu), psync(PORT, 1), psync(PORT, 0xFFFF), psync(PORT, 0x8000)}));
    for (uint32_t fl : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 0x10u, 0x80000000u}) {
        static std::vector<std::string> keep;
        char id[40]; snprintf(id, sizeof id, "s13b_pushsync_flags_%X", fl); keep.push_back(id);
        add(keep.back().c_str(), "S13b", cat(qp(), {psync(PORT, 1, fl)}));
    }
    add("s13c_pushsync_flags_before_port", "S13b", {psync(0u, 1, 1)}, Expect::Match, "FW: flags (A02) before the null port (A11); HLE the same since 7ec6155");
    add("s13d_pushsync_order_port_null_vs_mask", "S13a", {psync(0u, 0, 0)}, Expect::Match, "port null (A11) before the tag mask");
    add("s13e_pushsync_misaligned_port", "R06", {create(), psync(PORT + 8, 1, 0)});
    add("s13f_pushsync_null_port", "R06", {create(), psync(0u, 1, 0)});

    /* ---- S14: flags of PushJob and CopyPush -------------------------------------------------------------------- */
    add("s14a_pushjob_flags_valid_no_bit1", "S14", cat(qp(), {push(PORT, DESC, 256, 0, 0), push(PORT, DESC, 256, 1, 1), push(PORT, DESC, 256, 2, 4), push(PORT, DESC, 256, 3, 5)}),
        Expect::Match, "flags 0,1,4,5: ring bytes (tag, accounting bit, size class) and the port accounting");
    add("s14b_pushjob_flags_invalid", "S14", cat(qp(), {push(PORT, DESC, 256, 0, 8), push(PORT, DESC, 256, 0, 0x10), push(PORT, DESC, 256, 0, 0xF), push(PORT, DESC, 256, 0, 0x80000000u),
        push(PORT, DESC, 256, 0, 0xFFFFFFF8u)}));
    for (uint32_t fl : {2u, 3u, 6u, 7u}) {
        static std::vector<std::string> keep;
        char id[40]; snprintf(id, sizeof id, "s14c_pushjob_flag_bit1_%X", fl); keep.push_back(id);
        add(keep.back().c_str(), "S14", cat(qp(), {push(PORT, DESC, 256, 0, fl)}), Expect::Diverge,
            "FW stores flag bit 1 in entry w0 bit 2 (for the PM) and returns OK; the HLE stops by name after all checks (FIXED-DIFFERENTLY in 7ec6155, deliberate)");
    }
    add("s14d_pushjob_flag_bit1_fails_checks_first", "S14", cat(qp(), {push(PORT, DESC, 256, 99, 2), push(PORT, DESC + 8, 256, 0, 2), push(PORT, DESC, 1024, 0, 2)}), Expect::Match,
        "bit 1 set but another check fails first: A02/A10/A02 in both");
    add("s14e_copypush_flags_valid", "S14", cat(qp(nullptr, true), {cpush(PORT, DESC, 256, 256, 0, 0), cpush(PORT, DESC, 256, 256, 1, 1)}));
    add("s14f_copypush_flags_invalid", "S14", cat(qp(nullptr, true), {cpush(PORT, DESC, 256, 256, 0, 4), cpush(PORT, DESC, 256, 256, 0, 5), cpush(PORT, DESC, 256, 256, 0, 8),
        cpush(PORT, DESC, 256, 256, 0, 0x10), cpush(PORT, DESC, 256, 256, 0, 0xFFFFFFFCu)}), Expect::Match, "bit 2 does not exist for CopyPush: A02 even with the pool ready");
    for (uint32_t fl : {2u, 3u}) {
        static std::vector<std::string> keep;
        char id[40]; snprintf(id, sizeof id, "s14g_copypush_flag_bit1_%X", fl); keep.push_back(id);
        add(keep.back().c_str(), "S14", cat(qp(nullptr, true), {cpush(PORT, DESC, 256, 256, 0, fl)}), Expect::Diverge,
            "as s14c: FW stores bit 1 and returns OK; HLE named stop");
    }
    add("s14h_flags_bad_before_desc_null", "S14", cat(qp(), {push(PORT, 0u, 256, 0, 8), cpush(PORT, 0u, 256, 256, 0, 4)}), Expect::Match, "flags are the very first test: A02 even for a null descriptor");

    /* ---- S15: full ring ------------------------------------------------------------------------------------------ */
    {
        std::vector<Step> v = qp([](CA& a) { a.depth = Arg(4); });
        for (int i = 0; i < 8; ++i) v.push_back(push(PORT, DESC, 256, i, 4));
        add("s15a_full_ring_nonblocking_push", "S15", v, Expect::Match, "flags bit 2 set: A01 once the ring is full (depth 4)");
    }
    {
        std::vector<Step> v = qp([](CA& a) { a.depth = Arg(4); });
        for (int i = 0; i < 8; ++i) v.push_back(psync(PORT, 1, 4));
        add("s15b_full_ring_nonblocking_pushsync", "S15", v, Expect::Match, "PushSync flags 4: A01 on a full ring");
    }
    {
        std::vector<Step> v = qp([](CA& a) { a.depth = Arg(4); });
        for (int i = 0; i < 12; ++i) v.push_back(push(PORT, DESC, 256, i, 5));
        add("s15c_full_ring_nonblocking_accounted", "S15", v, Expect::Match, "flags 5 = accounted + non-blocking; an A01 must roll the accounting back");
    }
    for (int fill_n : {4, 5, 6, 7, 8}) {
        static std::vector<std::string> keep;
        char id[56]; snprintf(id, sizeof id, "s15d_full_ring_blocking_push_after_%d", fill_n); keep.push_back(id);
        std::vector<Step> v = qp([](CA& a) { a.depth = Arg(4); });
        for (int i = 0; i < fill_n; ++i) v.push_back(push(PORT, DESC, 256, 0, 4));
        v.push_back(push(PORT, DESC, 256, 0, 0));
        add(keep.back().c_str(), "S15", v, Expect::Inconclusive,
            "flags bit 2 clear: the push blocks (FW waits on the queue event flag, HLE on its condition variable); nothing wakes either, so both time out", nullptr, 2);
    }
    {
        std::vector<Step> v = qp([](CA& a) { a.depth = Arg(4); });
        for (int i = 0; i < 12; ++i) v.push_back(push(PORT, DESC, 256, 0, 1));
        add("s15e_flags1_pushes_block_when_full", "S15", v, Expect::Inconclusive, "Odin's flags (1): a full ring blocks; both sides time out at the same step", nullptr, 2);
    }
    {
        std::vector<Step> v = qp([](CA& a) { a.depth = Arg(4); });
        for (int i = 0; i < 12; ++i) v.push_back(psync(PORT, 1, 0));
        add("s15f_pushsync_blocks_when_full", "S15", v, Expect::Inconclusive, "PushSync flags 0 on a full ring blocks", nullptr, 2);
    }
    {
        std::vector<Step> v = qp([](CA& a) { a.depth = Arg(4); }, true);
        for (int i = 0; i < 12; ++i) v.push_back(cpush(PORT, DESC, 256, 256, 0, 0));
        add("s15g_copypush_blocks_when_full", "S15", v, Expect::Inconclusive, "CopyPush has no non-blocking mode: a full ring blocks (pool has four blocks, the ring is the limit)", nullptr, 2);
    }

    /* ---- N03, N07: checks only the firmware has ------------------------------------------------------------------ */
    for (auto& t : std::vector<std::pair<const char*, uint32_t>>{{"n03a_pushjob_desc_at_pool_base", POOL}, {"n03b_pushjob_desc_in_pool", POOL + 0x100},
                                                                   {"n03c_pushjob_desc_last_block", POOL + 0x300}}) {
        static std::vector<std::string> keep; keep.push_back(t.first);
        add(keep.back().c_str(), "N03", cat(qp(nullptr, true), {push(PORT, t.second, 256, 0, 0)}), Expect::Diverge,
            "FW: PushJob with a descriptor inside the queue's pool block area -> A02 (use the pooled-descriptor export); HLE accepts");
    }
    add("n03d_pushjob_desc_after_pool_blocks", "N03", cat(qp(nullptr, true), {push(PORT, POOL + 0x400, 256, 0, 0)}), Expect::Match, "block area is [base, base+4*256): +0x400 is outside");
    add("n03e_pushjob_desc_before_pool", "N03", cat(qp(nullptr, true), {push(PORT, POOL - 0x10, 256, 0, 0)}), Expect::Match, "outside");
    add("n03f_pushjob_desc_in_pool_no_pool_queue", "N03", cat(qp(), {push(PORT, POOL + 0x100, 256, 0, 0)}), Expect::Match, "queue without a pool: no pool-area test applies");
    add("n03g_copypush_src_in_pool", "N03", cat(qp(nullptr, true), {cpush(PORT, POOL + 0x100, 256, 256)}), Expect::Match, "CopyPush has no pool-area test (mode 1)");
    add("n07a_pushjob_slot_not_allocated", "N07", {create(), pcreate(), S("_cellSpursJobQueuePort2PushJobBody", N_PUSHJOB, {PORT, DESC, 256, 0, 0}, [] { fill(QUEUE + 0x580, 16, 0); })},
        Expect::Diverge, "port block intact but its slot bit cleared in queue+0x580: FW ring writer -> A02; HLE push never consults the bitmap and accepts");
    add("n07b_pushsync_slot_not_allocated", "N07", {create(), pcreate(), S("cellSpursJobQueuePort2PushSync", N_PUSHSYNC, {PORT, 1, 0}, [] { fill(QUEUE + 0x580, 16, 0); })},
        Expect::Diverge, "same for PushSync: FW A02, HLE accepts");
    add("n07c_destroy_slot_not_allocated", "N07", {create(), pcreate(), S("cellSpursJobQueuePort2Destroy", N_PDESTROY, {PORT}, [] { fill(QUEUE + 0x580, 16, 0); })},
        Expect::Blocked, "Destroy with the slot bit already clear: the HLE stops by name (jq2-state); the FW asserts (_sys_printf, then _sys_trap_process, which the harness stops at): both abort");
    add("n07d_copypush_slot_not_allocated", "N07", {create_pool(), pcreate(), S("_cellSpursJobQueuePort2CopyPushJobBody", N_COPYPUSH, {PORT, DESC, 256, 256, 0, 0}, [] { fill(QUEUE + 0x580, 16, 0); })},
        Expect::Diverge, "CopyPush: FW acquires a pool block, the ring writer fails (A02) and the block goes back to the free list; HLE accepts");

    /* ---- FW-only exports (N05): the pooled-descriptor push; the lifted code records what the HLE would have to do ---- */
    {
        auto push_pooled = [](Arg port, Arg desc, Arg size, Arg tag, Arg flags) { return S("<fw-only 0xE1731DF3 PushJobPooled>", 0xE1731DF3, {port, desc, size, tag, flags}); };
        add("n05a_pooled_push_desc_in_pool", "N05", cat(qp(nullptr, true), {push_pooled(PORT, POOL + 0x100, 256, 0, 0)}), Expect::NoHle, "FW-only export 0xE1731DF3 (mode 2): descriptor must lie in the pool area");
        add("n05b_pooled_push_desc_outside_pool", "N05", cat(qp(nullptr, true), {push_pooled(PORT, DESC, 256, 0, 0)}), Expect::NoHle, "FW-only: outside the pool area");
        add("n05c_pooled_push_flags_8", "N05", cat(qp(nullptr, true), {push_pooled(PORT, POOL + 0x100, 256, 0, 8)}), Expect::NoHle, "FW-only: flags & ~7");
    }

    /* ---- R06: port checks per API ------------------------------------------------------------------------------- */
    add("r06a_pushjob_null_port", "R06", {create(), push(0u, DESC)}, Expect::Diverge, "FW has no port null test: it reads port+0x18 (address 0x18 = 0) -> A0F; HLE A11");
    add("r06b_pushjob_misaligned_port", "R06", {create(), push(PORT + 8, DESC)}, Expect::Diverge, "FW: no port alignment test, link read at +0x20 is 0xA5.. -> A0F; HLE A10");
    add("r06c_pushjob_uninit_port_zero", "R06", {create(), push(PORT, DESC)}, Expect::Match, "zeroed port: link 0 -> A0F in both", [] { fill(PORT, 0x80, 0); });
    add("r06d_pushjob_uninit_port_garbage", "R06", {create(), push(PORT, DESC)}, Expect::Match, "link 0xA5A5A5A5 is not a queue -> A0F in both");
    add("r06e_pushsync_uninit_port_zero", "R06", {create(), psync(PORT, 1, 0)}, Expect::Diverge, "FW: queue link null -> A11 (inside the worker); HLE A0F", [] { fill(PORT, 0x80, 0); });
    add("r06f_pushsync_uninit_port_garbage", "R06", {create(), psync(PORT, 1, 0)}, Expect::Diverge, "garbage link 0xA5A5A5A5: the FW worker tests its alignment (A10); HLE: unknown queue -> A0F");
    add("r06g_copypush_null_port", "R06", {create_pool(), cpush(0u, DESC)}, Expect::Diverge, "as r06a");
    add("r06h_copypush_uninit_port_zero", "R06", {create_pool(), cpush(PORT, DESC)}, Expect::Match, "", [] { fill(PORT, 0x80, 0); });

    /* ---- Port2Sync (Y01..Y03) ------------------------------------------------------------------------------------ */
    add("y01a_sync_mode_bad_null_port", "Y01", {create(), sync(0u, 1)}, Expect::Diverge, "FW tests the mode first (A02); HLE resolves the port first (A11)");
    add("y01b_sync_mode_bad_valid_port", "Y01", cat(qp(), {sync(PORT, 1), sync(PORT, 2), sync(PORT, 3), sync(PORT, 5), sync(PORT, 8), sync(PORT, 0xFFFFFFFFu), sync(PORT, 0x14)}));
    add("y01c_sync_mode_bad_misaligned_port", "Y01", {create(), sync(PORT + 8, 1)}, Expect::Diverge, "FW A02 first; HLE A10");
    add("y01d_sync_mode_bad_uninit_port", "Y01", {create(), sync(PORT, 8)}, Expect::Diverge, "FW A02 (mode first); HLE A0F (uninitialised port)", [] { fill(PORT, 0x80, 0); });
    add("y01e_sync_null_port_valid_mode", "Y01", {create(), sync(0u, 4), sync(0u, 0)});
    add("y01f_sync_misaligned_valid_mode", "Y01", {create(), sync(PORT + 8, 4)});
    add("y02a_poll_idle_port", "Y02", cat(qp(), {sync(PORT, 4)}));
    add("y02b_block_idle_port", "Y02", cat(qp(), {sync(PORT, 0)}), Expect::Match, "nothing outstanding: no wait", nullptr, 3);
    add("y03_poll_after_accounted_push", "Y03", cat(qp(), {push(PORT, DESC, 256, 0, 1), sync(PORT, 4)}), Expect::Match, "A < B -> A01 (poll)");
    add("y03b_poll_after_unaccounted_push", "Y03", cat(qp(), {push(PORT, DESC, 256, 0, 0), sync(PORT, 4)}));
    add("y04_sync_uninit_port_zero", "R06", {create(), sync(PORT, 4)}, Expect::Diverge, "FW has no init/link test in Port2Sync and runs the sync on a zeroed block; HLE A0F",
        [] { fill(PORT, 0x80, 0); });
    g_destroy_mask = true;
    add("y05_sync_after_destroy", "R06", {create(), pcreate(), pdestroy(), sync(PORT, 4)}, Expect::Diverge, "destroyed port: FW still runs the sync; HLE A0F");
    g_destroy_mask = false;

    apply_witnesses();
}
}  // namespace dh

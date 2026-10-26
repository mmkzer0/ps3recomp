/* Cases for libfiber (libs/spurs/cellFiber.c vs the firmware's libfiber): the
 * scheduler / fiber lifecycle checks that need no guest context switch. Row ids
 * (F03, F09, ...) are the firmware verdicts of re_fiber_sync2.md / issue #28.
 *
 * Prototypes (HLE; guest pointers as u32, r3..r10):
 *   _cellFiberPpu{Scheduler,}AttributeInitialize(attr, sdk)
 *   cellFiberPpuInitializeScheduler(sched, attr)    cellFiberPpuFinalizeScheduler(sched)
 *   cellFiberPpuCreateFiber(sched, fiber, entry, u64 arg, prio, stack, size, attr)
 *   cellFiberPpuRunFibers(sched)    cellFiberPpuJoinFiber(fiber, status*)
 *   _cellFiberPpuInitialize(anchor)  -> offset = anchor - r13 becomes the TLS offset
 * Every sequence that needs the "system initialised" state starts with the latter. */
#include "dh_engine.h"
#include "ppu/ppu_context.h"
#include "syscalls/sys_timer.h"
#include "syscalls/sys_semaphore.h"
#include <cstring>

using namespace dh;

namespace {
constexpr uint32_t N_INIT = 0x55870804, N_SATTR = 0x9E25C72D, N_FATTR = 0xC11F8056, N_WATTR = 0x68BA4568,
                   N_INITSCHED = 0xEE3B604D, N_FINSCHED = 0x8B6BAA01, N_CREATE = 0x7C2F4034, N_RUN = 0x12B1ACF0,
                   N_JOIN = 0xA6004249, N_SELF = 0x5D9A7034, N_WAITSIG = 0x6C164B3B, N_SENDSIG = 0x8AFB8356,
                   N_YIELD = 0x0C44F441, N_EXIT = 0xFA8D5F95, N_CTXATTR = 0x31252EC3;
constexpr uint32_t ATTR = 0x20000, FATTR = 0x20400, STATUS = 0x23000, ENTRY = 0x25000,
                   SCHED = 0x30000, SCHED2 = 0x30400, FIB = 0x32000, FIB2 = 0x32400,
                   STACK = 0x40000, STACK2 = 0x50000, SSIZE = 0x4000, BAD_EA = 0xFFFFFFF0u;
constexpr uint32_t SDK = 0x330000;

std::vector<Case>* g_out;

Step S(const char* f, uint32_t nid, std::initializer_list<Arg> a, std::function<void()> pre = nullptr, bool r64 = false)
{ Step s = step(f, nid, a); s.pre = std::move(pre); s.ret64 = r64; return s; }
Step init0()   { return S("_cellFiberPpuInitialize", N_INIT, {TLS_R13 + 0x40}, nullptr, true); }
Step sattr(uint32_t ea = ATTR, uint32_t sdk = SDK) { return S("_cellFiberPpuSchedulerAttributeInitialize", N_SATTR, {ea, sdk}); }
Step fattr(uint32_t ea = FATTR, uint32_t sdk = SDK) { return S("_cellFiberPpuAttributeInitialize", N_FATTR, {ea, sdk}); }
Step isched(Arg sched = SCHED, Arg attr = ATTR) { return S("cellFiberPpuInitializeScheduler", N_INITSCHED, {sched, attr}); }
Step fsched(Arg sched = SCHED) { return S("cellFiberPpuFinalizeScheduler", N_FINSCHED, {sched}); }
Step create(Arg sched = SCHED, Arg fib = FIB, Arg entry = ENTRY, Arg prio = 1, Arg stack = STACK, Arg size = SSIZE, Arg attr = (uint32_t)0,
            std::function<void()> pre = nullptr)
{ return S("cellFiberPpuCreateFiber", N_CREATE, {sched, fib, entry, (uint64_t)0x1122334455667788ull, prio, stack, size, attr}, std::move(pre)); }
Step runf(Arg sched = SCHED) { return S("cellFiberPpuRunFibers", N_RUN, {sched}); }
Step join(Arg fib = FIB, Arg status = STATUS) { return S("cellFiberPpuJoinFiber", N_JOIN, {fib, status}); }

/* InitializeScheduler in the firmware does three things the HLE does not model; each leaves bytes in the
 * scheduler storage that the HLE leaves zero (understood, see is00_unmasked_valid_init). The ranges are
 * exactly the words the evidence names -- two lv2 semaphore ids (+0xA4, +0xC4) and the debugger-table
 * registration word (+0x160) -- not the slack around them:
 *   +0xA4, +0xC4  ids of two lv2 semaphores it creates (syscall 90)
 *   +0x160        8 bytes written by the debugger-table registration (sys_prx_get_module_id_by_name lookup)
 * Cases that are about something else mask them so the later steps stay comparable. */
const std::vector<std::pair<uint32_t, uint32_t>> kSchedMask = {
    {SCHED + 0xA4, 4}, {SCHED + 0xC4, 4}, {SCHED + 0x160, 8},
    {SCHED2 + 0xA4, 4}, {SCHED2 + 0xC4, 4}, {SCHED2 + 0x160, 8},
    /* CreateFiber also writes firmware bookkeeping the HLE does not model: words in fiber+0x80..0xA8, the
     * name copy and its neighbours in fiber+0x2A0..0x2E0, and a 16-byte marker at the bottom of the stack */
    {FIB + 0x80, 0x28}, {FIB + 0x2A0, 0x40}, {FIB2 + 0x80, 0x28}, {FIB2 + 0x2A0, 0x40}, {STACK, 0x10}, {STACK2, 0x10}};
/* Scheduler-only subset, for the cases that never create a fiber. */
const std::vector<std::pair<uint32_t, uint32_t>> kSchedOnlyMask = {
    {SCHED + 0xA4, 4}, {SCHED + 0xC4, 4}, {SCHED + 0x160, 8},
    {SCHED2 + 0xA4, 4}, {SCHED2 + 0xC4, 4}, {SCHED2 + 0x160, 8}};
bool g_nomask = false;
bool g_schedonly = false;
void add(const char* id, const char* row, std::vector<Step> steps, Expect e = Expect::Match, const char* note = "",
         std::function<void()> setup = nullptr, int timeout = 10)
{
    Case c; c.id = id; c.row = row; c.steps = std::move(steps); c.expect = e; c.note = note; c.timeout_s = timeout;
    if (g_schedonly) c.mask = kSchedOnlyMask;
    else if (!g_nomask) c.mask = kSchedMask;
    /* guest BSS-like storage: scheduler / fiber / stack areas start zeroed, so that the HLE's whole-object
     * memset and the firmware's field-by-field init are comparable on the bytes that matter */
    c.setup = [setup] { fill(SCHED, 0x800, 0); fill(FIB, 0x800, 0); fill(STACK, 0x20000, 0);
                        wr32(ENTRY, 0x00A00000); wr32(ENTRY + 4, 0); if (setup) setup(); };
    /* libfiber 4.93 _cellFiberPpuInitialize (0x02003D18) stores the r13-relative
     * offset through a module global (pointer at toc-0x7F90, target
     * image+0x66F0); the HLE keeps the same value in host state, so only this
     * 4-byte firmware cache word differs. Mask exactly it, and only in the
     * sequences that call the initializer. */
    for (const Step& s : c.steps)
        if (s.nid == N_INIT) { c.mask_image.push_back({0x66F0, 4}); break; }
    g_out->push_back(std::move(c));
}
/* The machine-checkable witness of a non-Match (or non-obvious Match) verdict:
 * the comparator's cause_kind (memory, return, status, fault, abi, ...) and,
 * for Expect::Blocked, the harness reason and step. A declared cause that is
 * not reproduced is unexpected. */
void witness_cause(const char* id, const char* cause)
{
    for (auto& c : *g_out) if (c.id == id) { c.expect_cause = cause; return; }
}
void witness_blocker(const char* id, const char* blocker, int step)
{
    for (auto& c : *g_out) if (c.id == id) { c.blocker = blocker; c.blocker_step = step; return; }
}
void witness_step(const char* id, int step)
{
    for (auto& c : *g_out) if (c.id == id) { c.expect_step = step; return; }
}
void witness_range(const char* id, uint32_t lo, uint32_t len)
{
    for (auto& c : *g_out) if (c.id == id) { c.expect_range_lo = lo; c.expect_range_hi = lo + len; return; }
}
void witness_stop(const char* id, const char* kind)
{
    for (auto& c : *g_out) if (c.id == id) { c.expect_stop_kind = kind; return; }
}
/* Hand-built attributes (bypassing the Initialize functions) so single bytes can be controlled. */
void put_sattr(uint32_t ea, uint32_t sdk = SDK) { fill(ea, 0x100, 0); wr64(ea, 0x44007515864E5F4Cull); wr32(ea + 8, sdk); }
void put_fattr(uint32_t ea, uint32_t sdk = SDK) { fill(ea, 0x100, 0); wr64(ea, 0xF784E782F9EA9359ull); wr32(ea + 8, sdk); memcpy(mem + ea + 0x10, "*NO NAME*", 10); }
}  // namespace

namespace dh {
/* Forwarded to the HLE: the pure memory helpers. Modelled by the harness: the calling
 * thread's id (the HLE registers it in ppu_sysprx.cpp, which this fixture does not link).
 * Everything else, including the libprof hooks, stops the lifted call with a named reason. */
static void model_timebase_frequency(void* vctx)
{ ppu_context* c = (ppu_context*)vctx; c->gpr[3] = (uint64_t)sys_time_get_timebase_frequency(c); }
/* The scheduler owns an lv2 semaphore (created in InitializeScheduler, destroyed in Finalize). The
 * runtime's own handlers back it; a blocking wait (92) is not modelled and stops the call. */
#define SYSCALL_MODEL(name, fn) static void name(void* v) { ppu_context* c = (ppu_context*)v; c->gpr[3] = (uint64_t)fn(c); }
SYSCALL_MODEL(model_sem_create, sys_semaphore_create)
SYSCALL_MODEL(model_sem_destroy, sys_semaphore_destroy)
SYSCALL_MODEL(model_sem_post, sys_semaphore_post)
static void model_get_thread_id(void* vctx)
{
    ppu_context* c = (ppu_context*)vctx;
    wr64((uint32_t)c->gpr[3], 1);   /* *id = thread 1 */
    c->gpr[3] = 0;
}
ImportPolicy import_policy()
{
    ImportPolicy p;
    p.dflt = Policy::Stop;
    p.by_nid[0x68B9B011] = Policy::Forward;   /* _sys_memset  */
    p.by_nid[0x6BF66EA7] = Policy::Forward;   /* _sys_memcpy  */
    p.by_nid[0xD3039D4D] = Policy::Forward;   /* _sys_strncpy */
    p.by_nid[0x350D454E] = Policy::Model;     /* sys_ppu_thread_get_id */
    p.by_nid[0xE0998DBF] = Policy::Forward;   /* sys_prx_get_module_id_by_name: logged, answered by the HLE */
    p.models[0x350D454E] = model_get_thread_id;
    p.syscalls[147] = model_timebase_frequency;
    p.syscalls[90] = model_sem_create; p.syscalls[91] = model_sem_destroy; p.syscalls[94] = model_sem_post;   /* sys_time_get_timebase_frequency: the runtime's own handler */
    return p;
}

void register_cases(std::vector<Case>& out)
{
    g_out = &out;
    register_guest_fn(0x00A00000, [](void* v) { ((ppu_context*)v)->gpr[3] = 7; });   /* a fiber entry that returns 7 */

    /* ---- attribute-initialise exports (F01, F02, F29) ---------------------- */
    for (int w = 0; w < 3; ++w) {
        const char* fn = w == 0 ? "_cellFiberPpuSchedulerAttributeInitialize" : w == 1 ? "_cellFiberPpuAttributeInitialize" : "_cellFiberPpuUtilWorkerControlAttributeInitialize";
        const uint32_t nid = w == 0 ? N_SATTR : w == 1 ? N_FATTR : N_WATTR;
        const char* p = w == 0 ? "sattr" : w == 1 ? "fattr" : "wattr";
        auto id = [&](const char* s) { return std::string(p) + "_" + s; };
        auto ad = [&](const char* s, std::initializer_list<Arg> a, const char* row = "F01") {
            static std::vector<std::string> keep; keep.push_back(id(s));
            Case c; c.id = keep.back(); c.row = row; c.steps = {S(fn, nid, a)}; c.expect = Expect::Match; out.push_back(std::move(c)); };
        ad("null", {0, SDK}, "F01");
        ad("valid_sdk", {ATTR, SDK});
        ad("valid_sdk_0", {ATTR, 0});
        ad("valid_sdk_270000", {ATTR, 0x270000});
        ad("misaligned_4", {ATTR + 4, SDK}, "F02");
        ad("misaligned_1", {ATTR + 1, SDK}, "F02");
        ad("align8_not128", {ATTR + 8, SDK}, "F02");
    }
    /* the Context attribute initialiser exists in the firmware only */
    { Case c; c.id = "ctxattr_valid"; c.row = "-"; c.steps = {S("_cellFiberPpuContextAttributeInitialize", N_CTXATTR, {ATTR, SDK})};
      c.expect = Expect::NoHle; c.note = "HLE does not implement the cellFiberPpuContext* family"; out.push_back(std::move(c)); }

    /* ---- InitializeScheduler (F04 F07 F09 F10) ------------------------------ */
    g_nomask = true;
    add("is00_unmasked_valid_init", "F05", {init0(), sattr(), isched()}, Expect::Diverge,
        "FW InitializeScheduler creates two lv2 semaphores (ids at sched+0xA4/+0xC4) and registers with a debugger table (+0x160); the HLE leaves those bytes zero");
    g_nomask = false;
    add("is01_valid_attr", "F07", {init0(), sattr(), isched()});
    add("is02_valid_null_attr", "F07", {init0(), isched(SCHED, (uint32_t)0)});
    add("is03_null_sched", "F10", {init0(), sattr(), isched(0u, ATTR)});
    add("is04_sched_misaligned", "F10", {init0(), sattr(), isched(SCHED + 0x40, ATTR)});
    add("is05_attr_misaligned", "F07", {init0(), isched(SCHED, ATTR + 4)}, Expect::Match, "", [] { put_sattr(ATTR + 4); });
    add("is06_attr_magic_mismatch", "F04", {init0(), isched()}, Expect::Match, "", [] { put_sattr(ATTR); wr64(ATTR, 0x1122334455667788ull); });
    add("is07_no_sysinit", "F07", {sattr(), isched()});
    add("is08_order_sched_align_vs_magic", "F07", {init0(), isched(SCHED + 0x40, ATTR)}, Expect::Match, "", [] { put_sattr(ATTR); wr64(ATTR, 0); });
    add("is09_order_attr_align_vs_magic", "F07", {init0(), isched(SCHED, ATTR + 4)}, Expect::Match, "", [] { put_sattr(ATTR + 4); wr64(ATTR + 4, 0); });
    add("is10_order_nosysinit_vs_magic", "F07", {isched()}, Expect::Match, "", [] { put_sattr(ATTR); wr64(ATTR, 0); });
    add("is11_init_twice_same_storage", "F09", {init0(), sattr(), isched(), isched()}, Expect::Diverge,
        "FW memsets 0x200 bytes and re-initialises (no STAT path); HLE returns STAT");
    add("is12_init_twice_after_finalize", "F09", {init0(), sattr(), isched(), fsched(), isched()});
    add("is13_autocheck_flags", "F05", {init0(), isched()}, Expect::Diverge,
        "FW stores autoCheckFlags and uses it in the dispatch loop; HLE stops loudly when it is non-zero (unmodelled)",
        [] { put_sattr(ATTR); wr8(ATTR + 0x10, 1); });
    add("is14_interval_usec", "F05", {init0(), isched()}, Expect::Match, "", [] { put_sattr(ATTR); wr32(ATTR + 0x14, 5000); });

    /* ---- FinalizeScheduler (F11 F12) ---------------------------------------- */
    add("fs01_null", "F11", {init0(), fsched(0u)});
    add("fs02_misaligned", "F11", {init0(), sattr(), isched(), fsched(SCHED + 0x40)}, Expect::Diverge, "FW: ALIGN (&127); HLE: INVAL, no ALIGN test");
    add("fs03_uninitialised", "F11", {init0(), fsched(SCHED)}, Expect::Diverge, "FW: STAT (byte +0x41 != 1); HLE: INVAL (unknown scheduler)");
    add("fs04_no_sysinit", "F11", {fsched(SCHED)}, Expect::Diverge, "FW: NOSYSINIT before STAT; HLE has no NOSYSINIT here and says INVAL");
    add("fs05_valid", "F11", {init0(), sattr(), isched(), fsched()});
    add("fs06_double_finalize", "F11", {init0(), sattr(), isched(), fsched(), fsched()}, Expect::Diverge, "second Finalize: FW STAT (+0x41 cleared); HLE INVAL (scheduler gone)");
    add("fs07_busy_fiber_listed", "F12", {init0(), sattr(), isched(), create(), fsched()});
    add("fs08_busy_after_capture", "F12", {init0(), sattr(), isched(), [] { Step s = create(); s.caps = {{0, Capture::Mem32, FIB + 8}}; return s; }(), fsched(Arg::slotv(0))},
        Expect::Match, "sequence with a captured value: Finalize(*(fiber+8))");

    /* ---- CreateFiber (F03 F06 F14-F21) -------------------------------------- */
    auto pre3 = [] { /* sched + fiber attr, hand-built */ };
    (void)pre3;
    const std::vector<Step> base = {init0(), sattr(), isched()};
    auto seq = [&](std::initializer_list<Step> more) { std::vector<Step> v = base; for (auto& s : more) v.push_back(s); return v; };
    g_schedonly = true;
    add("cf00_unmasked_valid_create", "F21", seq({create()}), Expect::Diverge,
        "FW-HAS-EXTRA: CreateFiber also writes bookkeeping words (fiber+0x83..0xA3), copies the name (+0x2A8) and marks the stack bottom; the HLE models only the fields its queues read");
    g_schedonly = false;
    add("cf01_valid_no_attr", "F18", seq({create()}));
    add("cf02_valid_attr", "F18", seq({fattr(), create(SCHED, FIB, ENTRY, 1, STACK, SSIZE, FATTR)}));
    add("cf03_attr_misaligned", "F03", seq({create(SCHED, FIB, ENTRY, 1, STACK, SSIZE, FATTR + 4)}), Expect::Diverge,
        "plain CreateFiber has no attr&7 test in the FW; HLE returns ALIGN", [] { put_fattr(FATTR + 4); });
    add("cf04_reserved_attr_byte_50", "F06", seq({create(SCHED, FIB, ENTRY, 1, STACK, SSIZE, FATTR)}), Expect::Diverge,
        "FW ignores attr bytes it does not read; HLE stops loudly (attribute-field)", [] { put_fattr(FATTR); wr8(FATTR + 0x50, 1); });
    add("cf05_reserved_attr_byte_0c", "F06", seq({create(SCHED, FIB, ENTRY, 1, STACK, SSIZE, FATTR)}), Expect::Diverge,
        "same, +0x0C", [] { put_fattr(FATTR); wr8(FATTR + 0x0C, 1); });
    add("cf06_attr_bad_magic", "F04", seq({create(SCHED, FIB, ENTRY, 1, STACK, SSIZE, FATTR)}), Expect::Match, "", [] { put_fattr(FATTR); wr64(FATTR, 1); });
    add("cf07_null_sched", "F14", seq({create(0u)}));
    add("cf08_null_fiber", "F14", seq({create(SCHED, 0u)}));
    add("cf09_null_entry", "F14", seq({create(SCHED, FIB, 0u)}));
    add("cf10_null_stack", "F14", seq({create(SCHED, FIB, ENTRY, 1, 0u)}));
    add("cf11_size_zero", "F14", seq({create(SCHED, FIB, ENTRY, 1, STACK, 0u)}));
    add("cf12_sched_misaligned", "F15", seq({create(SCHED + 0x40)}));
    add("cf13_fiber_misaligned", "F15", seq({create(SCHED, FIB + 0x40)}));
    add("cf14_stack_misaligned_8", "F16", seq({create(SCHED, FIB, ENTRY, 1, STACK + 8)}));
    add("cf15_size_misaligned_8", "F16", seq({create(SCHED, FIB, ENTRY, 1, STACK, SSIZE + 8)}));
    add("cf16_prio_3", "F17", seq({create(SCHED, FIB, ENTRY, 3)}));
    add("cf17_prio_4", "F17", seq({create(SCHED, FIB, ENTRY, 4)}));
    add("cf18_prio_ffffffff", "F17", seq({create(SCHED, FIB, ENTRY, 0xFFFFFFFFu)}));
    add("cf19_order_align_vs_prio", "F16", seq({create(SCHED, FIB, ENTRY, 9, STACK + 8)}));
    add("cf20_order_prio_vs_magic", "F17", seq({create(SCHED, FIB, ENTRY, 9, STACK, SSIZE, FATTR)}), Expect::Match, "", [] { put_fattr(FATTR); wr64(FATTR, 1); });
    add("cf21_order_null_vs_align", "F14", seq({create(SCHED + 0x40, FIB, 0u)}));
    add("cf22_no_sysinit", "F18", {sattr(), create()});
    add("cf23_order_prio_vs_nosysinit", "F18", {create(SCHED, FIB, ENTRY, 9)});
    add("cf24_order_magic_vs_nosysinit", "F18", {create(SCHED, FIB, ENTRY, 1, STACK, SSIZE, FATTR)}, Expect::Match, "", [] { put_fattr(FATTR); wr64(FATTR, 1); });
    add("cf25_create_twice_no_run", "F21", seq({create(), create()}), Expect::Diverge,
        "FW never reads the old storage and relinks unconditionally; HLE returns STAT from its registry");
    add("cf26_create_two_distinct", "F21", seq({create(), create(SCHED, FIB2, ENTRY, 1, STACK2)}));
    add("cf27_sched_uninitialised", "F19", {init0(), create(SCHED2)}, Expect::Diverge,
        "FW dereferences the scheduler blindly (no validity test; zeroed storage accepted, OK); HLE returns INVAL");

    /* ---- RunFibers without a fiber to switch to (F22) ----------------------- */
    add("rf01_null", "F22", {init0(), runf(0u)});
    add("rf02_misaligned", "F22", {init0(), runf(SCHED + 0x40)});
    add("rf03_no_sysinit", "F22", {runf(SCHED)});
    add("rf04_unknown_sched", "F22", {init0(), runf(SCHED)}, Expect::Diverge, "FW: STAT (byte +0x41 != 1); HLE: INVAL");
    add("rf05_empty_scheduler", "F22", seq({runf()}), Expect::Inconclusive,
        "r3 and memory agree, but the lifted RunFibers returns with SP=0x450, LR=0: it executes the context-restore routine even with no fiber, so its result is accidental");

    /* ---- running a fiber: the HLE can, the lifted code cannot (see the report) ---- */
    add("run01_create_run_join", "F22", seq({create(), runf(), join(FIB, STATUS)}), Expect::Blocked,
        "needs a guest stack/context switch inside lifted code (setjmp/longjmp-style routines at fw 0x02000008/0x02000128)", nullptr, 4);
    add("run02_create_run_join_create_again", "F21", seq({create(), runf(), join(FIB, STATUS), create()}), Expect::Blocked,
        "F21b: re-create after the fiber ran and was joined; same obstacle", nullptr, 4);

    /* ---- JoinFiber without running (F28) ------------------------------------- */
    add("jn01_null", "F28", {init0(), join(0u)});
    add("jn02_status_null", "F28", seq({create(), join(FIB, 0u)}), Expect::Diverge,
        "FW: NULL_POINTER for status; HLE accepts NULL (then waits for a fiber that cannot run)", nullptr, 2);
    add("jn03_fiber_misaligned", "F28", seq({join(FIB + 0x40)}), Expect::Diverge, "FW: ALIGN; HLE has no ALIGN test (unknown fiber -> INVAL)");
    add("jn04_no_sysinit", "F28", {join(FIB)}, Expect::Diverge, "FW: NOSYSINIT; HLE: INVAL");
    add("jn05_unknown_fiber_zeroed", "F28", seq({join(FIB)}), Expect::Diverge, "FW: STAT (fiber state word 0); HLE: INVAL");
    add("jn05b_unknown_fiber_garbage", "F28", seq({join(FIB)}), Expect::Match, "state word is 0xA5A5A5A5: the FW treats it as a live fiber and the HLE as unknown, both end in the same code",
        [] { fill(FIB, 0x380, 0xA5); });
    add("jn06_null_before_sysinit", "F28", {join(0u)});

    /* ---- other entry points that do not need a running fiber ----------------- */
    add("ws01_no_sysinit", "F23", {S("cellFiberPpuWaitSignal", N_WAITSIG, {})});
    add("ws02_not_in_fiber", "F23", {init0(), S("cellFiberPpuWaitSignal", N_WAITSIG, {})});
    add("yl01_no_sysinit", "F27", {S("cellFiberPpuYield", N_YIELD, {})}, Expect::Diverge, "FW: NOSYSINIT precedes PERM; HLE: PERM");
    add("yl02_not_in_fiber", "F27", {init0(), S("cellFiberPpuYield", N_YIELD, {})});
    add("ex01_no_sysinit", "F27", {S("cellFiberPpuExit", N_EXIT, {0})}, Expect::Diverge, "FW: NOSYSINIT precedes PERM; HLE: PERM");
    add("ex02_not_in_fiber", "F27", {init0(), S("cellFiberPpuExit", N_EXIT, {0})});
    add("ss01_null", "F24", {init0(), S("cellFiberPpuSendSignal", N_SENDSIG, {0u, 0u})});
    add("ss02_misaligned", "F24", {init0(), S("cellFiberPpuSendSignal", N_SENDSIG, {FIB + 0x40, 0u})});
    add("ss03_unstarted_fiber", "F25", seq({create(), S("cellFiberPpuSendSignal", N_SENDSIG, {FIB, STATUS})}));
    add("self01_no_sysinit", "F08", {S("cellFiberPpuSelf", N_SELF, {})}, Expect::Match, "", nullptr, 10);
    add("self02_after_init", "F08", {init0(), S("cellFiberPpuSelf", N_SELF, {})});

    /* Why each non-Match verdict is expected; the note on the case says what
     * the firmware does. The comparator must report this witness, not just
     * some divergence. */
    witness_cause("is00_unmasked_valid_init", "memory");
    witness_cause("is11_init_twice_same_storage", "return");
    witness_cause("is13_autocheck_flags", "status");
    witness_cause("fs02_misaligned", "return");
    witness_cause("fs03_uninitialised", "return");
    witness_cause("fs04_no_sysinit", "return");
    witness_cause("fs06_double_finalize", "return");
    witness_cause("cf00_unmasked_valid_create", "memory");
    witness_cause("cf03_attr_misaligned", "return");
    witness_cause("cf04_reserved_attr_byte_50", "status");
    witness_cause("cf05_reserved_attr_byte_0c", "status");
    witness_cause("cf25_create_twice_no_run", "return");
    witness_cause("cf27_sched_uninitialised", "return");
    witness_cause("rf04_unknown_sched", "return");
    witness_cause("rf05_empty_scheduler", "abi");
    witness_cause("jn02_status_null", "status");
    witness_cause("jn03_fiber_misaligned", "return");
    witness_cause("jn04_no_sysinit", "return");
    witness_cause("jn05_unknown_fiber_zeroed", "return");
    witness_cause("yl01_no_sysinit", "return");
    witness_cause("ex01_no_sysinit", "return");
    witness_step("is00_unmasked_valid_init", 2);
    witness_range("is00_unmasked_valid_init", SCHED, 0x200);
    witness_step("is11_init_twice_same_storage", 3);
    witness_step("is13_autocheck_flags", 1);
    witness_stop("is13_autocheck_flags", "fiber-unsupported");
    witness_step("fs02_misaligned", 3);
    witness_step("fs03_uninitialised", 1);
    witness_step("fs04_no_sysinit", 0);
    witness_step("fs06_double_finalize", 4);
    witness_step("cf00_unmasked_valid_create", 3);
    witness_range("cf00_unmasked_valid_create", FIB, 0x300);
    witness_step("cf03_attr_misaligned", 3);
    witness_step("cf04_reserved_attr_byte_50", 3);
    witness_stop("cf04_reserved_attr_byte_50", "fiber-unsupported");
    witness_step("cf05_reserved_attr_byte_0c", 3);
    witness_stop("cf05_reserved_attr_byte_0c", "fiber-unsupported");
    witness_step("cf25_create_twice_no_run", 4);
    witness_step("cf27_sched_uninitialised", 1);
    witness_step("rf04_unknown_sched", 1);
    witness_step("rf05_empty_scheduler", 3);
    witness_step("jn02_status_null", 4);
    witness_step("jn03_fiber_misaligned", 3);
    witness_step("jn04_no_sysinit", 0);
    witness_step("jn05_unknown_fiber_zeroed", 3);
    witness_step("yl01_no_sysinit", 0);
    witness_step("ex01_no_sysinit", 0);
    /* The lifted RunFibers reaches a zero word (an unknown instruction) at the
     * context-switch routine; that specific harness gap is the declared block,
     * so a different lifter gap cannot hide behind "Expect::Blocked". */
    witness_blocker("run01_create_run_join", "unknown-instruction", 4);
    witness_blocker("run02_create_run_join_create_again", "unknown-instruction", 4);
}
}  // namespace dh

/*
 * test_jq2_executor - the JobQueue2 HLE executor (libs/spurs/jq2/executor.cpp)
 * through its C surface (jq2.h), running synthetic SPU job images on the SPU
 * interpreter. One mode per process (argv[1]); a run prints PASS or FAIL lines
 * and exits 0 / 1. Nothing here is a game or firmware byte: the job images are
 * built from hand-encoded SPU words.
 *
 * A job image is a JOBCRT Ver13 header and a 16-instruction program: read the
 * descriptor's user words {out_ea, in_ea, add, serial} from its LS copy, GET 16
 * bytes from in_ea, add the four words {add, serial, out_ea, in_ea} lane by lane,
 * PUT the result to out_ea and return through the link register. The result
 * therefore names the descriptor bytes the job saw (add, serial) and proves it
 * ran (a DMA PUT landed).
 *
 * Modes:
 *   premature   S2: a store to the descriptor after push and before admission
 *               reaches the job; a store after admission does not
 *   copy        S3: CopyPush snapshots at push; the pool slot returns; blocking
 *               acquire on an exhausted class; a pool-less queue refuses
 *   credit      1000 jobs, 2 workers, 3 tags, PushSync barriers: at every Sync
 *               dispatched == executed == credited, every output right, A/B
 *   wrap        a depth-4 ring wraps (phase flips) 30 times under 2 workers
 *   barrier     SYNC order: same-tag jobs behind a SYNC wait for the earlier
 *               ones, other tags overtake, the out-of-order mark is set (in the
 *               executor's state: the PM's record area in the queue stays
 *               untouched) and the read index holds at the retained entry
 *   sync        Port2Sync mode 0 blocks and returns 0, mode 4 AGAIN, a second
 *               Sync and a push meet BUSY, every other mode INVAL with the
 *               port line unchanged
 *   join        Shutdown/Join: Join returns only after the outstanding jobs
 *               finished (L1); guest buffers stay the caller's (L2)
 *   observer    jq2_set_job_observer: the worker's spu_context and the loader's
 *               layout at job start, r3 at job end, nothing once removed
 *   validate    every guard and firmware status code of the surface
 *   lifted      the same transform as a lifted-style body (the C an SPU lifter emits: channel
 *               writes, `bi $lr` as SPU_RET): it returns normally, is executed and credited
 *               once, and its output is right
 *   stop-halt   a lifted-style body that halts (the lifter's halt-on-condition: status HALT,
 *               spu_halt longjmp) before its output write: named stop jq2-job-end reason=halt,
 *               no output, no credit
 *   stop-job    a job that executes `stop 0x2222`: named stop jq2-job-end, the
 *               port is not credited
 *   stop-crt    a job image with another CRT: the loader's named stop, no credit
 *   stop-ring   an entry the executor does not model: named stop jq2-ring-entry
 *   sync-concurrent  several PPU threads block in mode 0 on different ports at
 *               once, each woken by its own credits
 *   stop-waiter-id   a credit finds an event-flag waiter with id 64: jq2-wake
 *               reason=waiter-id
 *   waiter-64-nowake id 64 allocated, credit arrives before registration or Sync
 *               completes without blocking: no stop, id 64 cleared, returns 0
 *   stop-waiter-64-wake 64th concurrent Sync registers kind 2 with id 64 and blocks,
 *               a credit triggers jq2-wake reason=waiter-id id=64
 *   stop-wait   an event-flag wait that fails (ESRCH, e.g. destroyed before wait): jq2-wake reason=wait
 *   stop-push-shutdown, stop-copy-push-shutdown  a push that passed its checks
 *               and reaches the ring after Shutdown: jq2-state
 *               reason=push-after-shutdown
 *   desc-sizes  a maxDescriptor 896 queue runs every firmware-accepted descriptor size (64,
 *               128..896) end to end; the ring word carries the 3-bit class
 *   stop-push-size0, stop-copy-push-size0  the firmware accepts size 0; the HLE stops:
 *               jq2-unmodelled
 */
#include "jq2/executor.hpp"
#include "jq2/jq2.h"
#include "jq2/port.hpp"

#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_observe.h"
#include "spu_coherency.h"
#include "syscalls/sys_event.h"
#include "memory/reservation.h"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
uint8_t* vm_base;
extern uint32_t ppu_vm_size; /* runtime/host/host_stubs.c */
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
u128 spu_rdch(spu_context* ctx, uint32_t channel);
void spu_halt(spu_context* ctx);
}

using namespace ps3recomp::spurs::jq2;

namespace {

int g_fail = 0;
#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            ++g_fail;                             \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);             \
            std::printf("\n");                    \
        }                                         \
    } while (0)

/* ---- guest memory ---------------------------------------------------------- */

constexpr std::uint32_t kMem = 16u << 20;

/* Shared, so that a forked child's stop leaves its guest memory to the parent. */
void guest_init()
{
    void* m = mmap(nullptr, kMem, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) {
        std::perror("mmap");
        std::exit(2);
    }
    vm_base = static_cast<std::uint8_t*>(m);
    ppu_vm_size = kMem;
}

enum : std::uint32_t {
    SPURS = 0x60000,
    ATTR = 0x50000,
    QUEUE = 0x10000,
    RING = 0x20000,
    POOL = 0x30000,
    POOL_ATTR = 0x40000,
    PORT = 0x70000,     /* + 0x80 per port */
    IMAGE = 0x100000,   /* + 0x1000 per image */
    DESC = 0x200000,    /* + 0x100 per job */
    IN = 0x400000,      /* + 0x10 per job */
    OUT = 0x500000,     /* + 0x10 per job */
};

std::uint32_t be32(std::uint32_t v) { return __builtin_bswap32(v); }
std::uint32_t get32(std::uint32_t ea)
{
    return be32(std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(vm_base + ea)).load());
}
void put32(std::uint32_t ea, std::uint32_t v)
{
    std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(vm_base + ea)).store(be32(v));
}
void put16(std::uint32_t ea, std::uint16_t v)
{
    v = __builtin_bswap16(v);
    std::memcpy(vm_base + ea, &v, 2);
}
void put64(std::uint32_t ea, std::uint64_t v)
{
    put32(ea, static_cast<std::uint32_t>(v >> 32));
    put32(ea + 4, static_cast<std::uint32_t>(v));
}
std::uint16_t get16(std::uint32_t ea)
{
    std::uint16_t v;
    std::memcpy(&v, vm_base + ea, 2);
    return __builtin_bswap16(v);
}

template <class P>
bool wait_for(P pred, int ms = 60000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

/* ---- SPU encoders ------------------------------------------------------------ */

std::uint32_t e_il(int rt, int imm) { return (0x81u << 23) | ((std::uint32_t(imm) & 0xFFFFu) << 7) | std::uint32_t(rt); }
std::uint32_t e_ila(int rt, std::uint32_t imm) { return (0x21u << 25) | ((imm & 0x3FFFFu) << 7) | std::uint32_t(rt); }
std::uint32_t e_ilhu(int rt, std::uint32_t imm) { return (0x82u << 23) | ((imm & 0xFFFFu) << 7) | std::uint32_t(rt); }
std::uint32_t e_iohl(int rt, std::uint32_t imm) { return (0xC1u << 23) | ((imm & 0xFFFFu) << 7) | std::uint32_t(rt); }
std::uint32_t e_and(int rt, int ra, int rb) { return (0xC1u << 21) | (std::uint32_t(rb) << 14) | (std::uint32_t(ra) << 7) | std::uint32_t(rt); }
std::uint32_t e_ori0(int rt, int ra) { return (0x04u << 24) | (std::uint32_t(ra) << 7) | std::uint32_t(rt); }
std::uint32_t e_bisl(int rt, int ra) { return (0x1A9u << 21) | (std::uint32_t(ra) << 7) | std::uint32_t(rt); }
std::uint32_t e_a(int rt, int ra, int rb) { return (0x0C0u << 21) | (std::uint32_t(rb) << 14) | (std::uint32_t(ra) << 7) | std::uint32_t(rt); }
std::uint32_t e_lqd(int rt, int ra, int off) { return (0x34u << 24) | (((std::uint32_t(off) / 16) & 0x3FFu) << 14) | (std::uint32_t(ra) << 7) | std::uint32_t(rt); }
std::uint32_t e_lqa(int rt, std::uint32_t lsa) { return (0x61u << 23) | (((lsa >> 2) & 0xFFFFu) << 7) | std::uint32_t(rt); }
std::uint32_t e_stqa(int rt, std::uint32_t lsa) { return (0x41u << 23) | (((lsa >> 2) & 0xFFFFu) << 7) | std::uint32_t(rt); }
std::uint32_t e_rotqbyi(int rt, int ra, int n) { return (0x1FCu << 21) | ((std::uint32_t(n) & 0x7Fu) << 14) | (std::uint32_t(ra) << 7) | std::uint32_t(rt); }
std::uint32_t e_wrch(std::uint32_t ch, int rt) { return (0x10Du << 21) | (ch << 7) | std::uint32_t(rt); }
std::uint32_t e_rdch(int rt, std::uint32_t ch) { return (0x00Du << 21) | (ch << 7) | std::uint32_t(rt); }
std::uint32_t e_br(std::uint32_t pc, std::uint32_t target) { return (0x64u << 23) | ((((std::int32_t(target) - std::int32_t(pc)) / 4) & 0xFFFFu) << 7); }
std::uint32_t e_stop(std::uint32_t code) { return code & 0x3FFFu; }
constexpr std::uint32_t kBiLr = 0x35000000u; /* bi $lr */

constexpr std::uint32_t kCode = 0x300;
constexpr std::uint32_t kImageSize = 0x1000;
constexpr std::uint32_t kBssQuads = 0x100;     /* the buffers below live in the bss */
/* The JQ2 job profile puts the image at 0x7C00 (docs/JQ2.md); the job's two
 * 16-byte buffers sit in its bss, 0x1800 and 0x1810 into the image. */
constexpr std::uint32_t kBase = 0x7C00;
constexpr std::uint32_t kLsIn = kBase + 0x1800, kLsOut = kBase + 0x1810;

enum class Prog { transform, stop_early, bad_crt, ret_only, jm_wait, jm_call };

/* The jm_wait program's list: two elements for a GETL on tag 1, the first with
 * the stall-and-notify bit. It is data in the image, at kBase + kList. */
constexpr std::uint32_t kList = 0x700;

void emit_dma(std::vector<std::uint32_t>& c, std::uint32_t lsa, int eal_reg, std::uint32_t cmd)
{
    c.push_back(e_ila(5, lsa));
    c.push_back(e_wrch(MFC_LSA, 5));
    c.push_back(e_il(5, 0));
    c.push_back(e_wrch(MFC_EAH, 5));
    c.push_back(e_wrch(MFC_EAL, eal_reg));
    c.push_back(e_il(5, 16));
    c.push_back(e_wrch(MFC_Size, 5));
    c.push_back(e_il(5, 1));
    c.push_back(e_wrch(MFC_TagID, 5));
    c.push_back(e_il(5, int(cmd)));
    c.push_back(e_wrch(MFC_Cmd, 5));
    c.push_back(e_il(5, 2)); /* tag 1 */
    c.push_back(e_wrch(MFC_WrTagMask, 5));
    c.push_back(e_il(5, 2)); /* wait for all */
    c.push_back(e_wrch(MFC_WrTagUpdate, 5));
    c.push_back(e_rdch(5, MFC_RdTagStat));
}

/* r84 = jm interface word 0 or 1, found the way the CRT preflight finds it:
 * the low 28 bits of ctx+0x14 name the table. */
void emit_jm_lookup(std::vector<std::uint32_t>& c, bool word1)
{
    c.push_back(e_lqd(5, 3, 0x10));
    c.push_back(e_rotqbyi(5, 5, 4));
    c.push_back(e_ilhu(6, 0x0FFF));
    c.push_back(e_iohl(6, 0xFFFF));
    c.push_back(e_and(7, 5, 6));
    c.push_back(e_lqd(8, 7, 0));
    c.push_back(word1 ? e_rotqbyi(84, 8, 4) : e_ori0(84, 8));
}

void build_image(std::uint32_t ea, Prog prog)
{
    std::uint8_t* img = vm_base + ea;
    std::memset(img, 0, kImageSize);
    std::memcpy(img + 0x30, prog == Prog::bad_crt ? "JOBCRT Ver12" : "JOBCRT Ver13", 12);
    std::vector<std::uint32_t> c;
    if (prog == Prog::stop_early) {
        c.push_back(e_stop(0x2222));
    } else if (prog == Prog::ret_only) {
        c.push_back(kBiLr);
    } else if (prog == Prog::jm_call) {
        emit_jm_lookup(c, false);
        c.push_back(e_il(3, 0x1234)); /* the request word */
        c.push_back(e_bisl(0, 84));   /* call jm word 0 */
        c.push_back(e_stop(0));
    } else if (prog == Prog::jm_wait) {
        /* GETL a stalled two-element list, then call jm word 1 (the tag wait)
         * and use the SECOND element's data, which the MFC moves only after
         * the stall is acknowledged. */
        c.push_back(e_lqd(10, 4, 0x30));
        c.push_back(e_rotqbyi(12, 10, 8));
        emit_jm_lookup(c, true);
        c.push_back(e_ori0(90, 0)); /* the link, for after the call */
        c.push_back(e_ila(5, kLsIn));
        c.push_back(e_wrch(MFC_LSA, 5));
        c.push_back(e_il(5, 0));
        c.push_back(e_wrch(MFC_EAH, 5));
        c.push_back(e_ila(5, kBase + kList));
        c.push_back(e_wrch(MFC_EAL, 5));
        c.push_back(e_il(5, 16));
        c.push_back(e_wrch(MFC_Size, 5));
        c.push_back(e_il(5, 1));
        c.push_back(e_wrch(MFC_TagID, 5));
        c.push_back(e_il(5, int(MFC_GETL_CMD)));
        c.push_back(e_wrch(MFC_Cmd, 5));
        c.push_back(e_bisl(0, 84));
        c.push_back(e_ori0(0, 90));
        c.push_back(e_lqa(13, kLsOut)); /* the second element: kLsIn + 16 */
        c.push_back(e_a(13, 13, 12));
        c.push_back(e_stqa(13, kLsOut));
        emit_dma(c, kLsOut, 10, MFC_PUT_CMD);
        c.push_back(kBiLr);
        put32(ea + kList, 0x80000010u); /* stall-and-notify, 16 bytes from the first input */
        put32(ea + kList + 4, IN);
        put32(ea + kList + 8, 0x10u); /* 16 bytes from the second */
        put32(ea + kList + 12, IN + 0x10);
    } else {
        c.push_back(e_lqd(10, 4, 0x30));   /* r10 = {out_ea, in_ea, add, serial} */
        c.push_back(e_rotqbyi(11, 10, 4)); /* r11 word 0 = in_ea */
        c.push_back(e_rotqbyi(12, 10, 8)); /* r12 = {add, serial, out_ea, in_ea} */
        emit_dma(c, kLsIn, 11, MFC_GET_CMD);
        c.push_back(e_lqa(13, kLsIn));
        c.push_back(e_a(13, 13, 12));
        c.push_back(e_stqa(13, kLsOut));
        emit_dma(c, kLsOut, 10, MFC_PUT_CMD);
        c.push_back(kBiLr);
    }
    put32(ea + 0, e_br(0, kCode));
    for (std::size_t i = 0; i < c.size(); ++i) put32(ea + kCode + 4 * std::uint32_t(i), c[i]);
}

/* A 64-byte descriptor at `ea` for image `image`. */
void build_desc(std::uint32_t ea, std::uint32_t image, std::uint32_t out, std::uint32_t in, std::uint32_t add,
                std::uint32_t serial)
{
    std::memset(vm_base + ea, 0, 0x100);
    put64(ea + 0x00, (std::uint64_t{kBssQuads} << 32) | (image | 1u)); /* bit 0: cache inhibit */
    put16(ea + 0x08, kImageSize >> 4);
    put16(ea + 0x0A, 0);        /* no input DMA list */
    put16(ea + 0x1C, 0x40);     /* stack, quadwords */
    put16(ea + 0x1E, 0x10);     /* scratch, quadwords */
    put32(ea + 0x30, out);
    put32(ea + 0x34, in);
    put32(ea + 0x38, add);
    put32(ea + 0x3C, serial);
}

void set_in(std::uint32_t ea, std::uint32_t seed)
{
    for (std::uint32_t i = 0; i < 4; ++i) put32(ea + 4 * i, seed * 0x9E3779B1u + i * 0x01010101u);
}

/* What the transform program must have stored. */
bool out_is(std::uint32_t out, std::uint32_t in, std::uint32_t add, std::uint32_t serial, std::uint32_t out_ea,
            std::uint32_t in_ea)
{
    const std::uint32_t k[4] = {add, serial, out_ea, in_ea};
    for (std::uint32_t i = 0; i < 4; ++i)
        if (get32(out + 4 * i) != get32(in + 4 * i) + k[i]) return false;
    return true;
}

/* ---- the harness ------------------------------------------------------------- */

struct Probe {
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<std::uint64_t> clock{0};
    std::map<std::uint32_t, std::uint64_t> admit, done;  /* serial -> clock */
    std::map<std::uint32_t, std::uint32_t> dispatch;     /* serial -> dispatch id */
    std::function<void(const JobStart&)> on_admit;
};
Probe g_probe;

std::uint32_t serial_of(const JobStart& js)
{
    std::uint32_t v;
    std::memcpy(&v, js.descriptor.data() + 0x3C, 4);
    return __builtin_bswap32(v);
}

void hook_admit(void*, const JobStart& js)
{
    const std::uint32_t s = serial_of(js);
    std::function<void(const JobStart&)> extra;
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.admit[s] = ++g_probe.clock;
        g_probe.dispatch[s] = js.dispatch_id;
        extra = g_probe.on_admit;
    }
    if (extra) extra(js);
}
void hook_done(void*, const JobStart& js)
{
    std::lock_guard<std::mutex> lk(g_probe.mu);
    g_probe.done[serial_of(js)] = ++g_probe.clock;
}

/* The job bodies a test selects; 0 leaves the interpreter. */
enum LiftedKind { kNoLifted = 0, kLiftedNormal, kLiftedHalt, kLiftedJmCall };
int g_lifted = kNoLifted;

/* A lifted-style body, written the way tools/spu_lifter.py emits code: channel
 * writes through the context, the halt-on-condition as `status = HALT;
 * spu_halt(ctx); return;` and `bi $lr` at depth 0 as SPU_RET. It does the
 * transform program's work: GET 16 bytes from the descriptor's in_ea, add
 * {add, serial, out_ea, in_ea}, PUT the result to out_ea. The halting kind
 * halts after the GET and before the output is stored or written. */
void lifted_dma(spu_context* c, std::uint32_t lsa, std::uint32_t ea, std::uint32_t cmd)
{
    spu_wrch(c, MFC_LSA, spu_splat_u32(lsa));
    spu_wrch(c, MFC_EAH, spu_splat_u32(0));
    spu_wrch(c, MFC_EAL, spu_splat_u32(ea));
    spu_wrch(c, MFC_Size, spu_splat_u32(16));
    spu_wrch(c, MFC_TagID, spu_splat_u32(1));
    spu_wrch(c, MFC_Cmd, spu_splat_u32(cmd));
    spu_wrch(c, MFC_WrTagMask, spu_splat_u32(2));
    spu_wrch(c, MFC_WrTagUpdate, spu_splat_u32(2));
    (void)spu_rdch(c, MFC_RdTagStat);
}

std::uint32_t ls_word(const spu_context* c, std::uint32_t lsa)
{
    std::uint32_t v;
    std::memcpy(&v, c->ls + lsa, 4);
    return be32(v);
}

void lifted_job(spu_context* c, bool halt)
{
    const std::uint32_t d = c->gpr[4]._u32[0];
    const std::uint32_t out = ls_word(c, d + 0x30), in = ls_word(c, d + 0x34);
    const std::uint32_t k[4] = {ls_word(c, d + 0x38), ls_word(c, d + 0x3C), out, in};
    lifted_dma(c, kLsIn, in, MFC_GET_CMD);
    if (halt) {
        c->status = SPU_STATUS_STOPPED_BY_HALT;
        spu_halt(c);
        return;
    }
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint32_t v = be32(ls_word(c, kLsIn + 4 * i) + k[i]);
        std::memcpy(c->ls + kLsOut + 4 * i, &v, 4);
    }
    lifted_dma(c, kLsOut, out, MFC_PUT_CMD);
    SPU_RET(c);
}
void lifted_normal_entry(spu_context* c) { lifted_job(c, false); }
void lifted_halt_entry(spu_context* c) { lifted_job(c, true); }

/* A lifted body that calls jm word 0 the way lifted code calls through a
 * register (the bisl bracket): the resolver finds no lifted function at 0x1500
 * and interprets the planted stop word. */
void lifted_jm_call_entry(spu_context* c)
{
    const std::uint32_t iface = ls_word(c, c->gpr[3]._u32[0] + 0x14) & 0x0FFFFFFFu;
    constexpr std::uint32_t kRet = 0x2F0;
    c->gpr[84] = spu_splat_u32(ls_word(c, iface));
    c->gpr[3] = spu_splat_u32(0x1234);
    const std::uint32_t target = c->gpr[84]._u32[0];
    c->gpr[0] = spu_link(kRet);
    const std::int32_t saved = c->image_id;
    c->pc = target;
    c->host_depth++;
    spu_indirect_branch(c);
    spu_drain_call(c, kRet);
    c->host_depth--;
    spu_img_restore(c, saved);
}

JobBody hook_select(void*, const JobStart&)
{
    switch (g_lifted) {
    case kLiftedHalt: return JobBody{&lifted_halt_entry, -1};
    case kLiftedJmCall: return JobBody{&lifted_jm_call_entry, -1};
    default: return JobBody{&lifted_normal_entry, -1};
    }
}

/* The push held by hook_push_checked (push_shutdown_scenario). Process-wide and
 * never destroyed: its pusher is detached and the process ends with the stop it
 * provokes. */
struct PushGate {
    std::atomic<bool> armed{false}, reached{false}, release{false}, returned{false};
};
PushGate g_gate;

void hook_push_checked(void*, std::uint32_t)
{
    if (!g_gate.armed) return;
    g_gate.reached = true;
    while (!g_gate.release) std::this_thread::sleep_for(std::chrono::microseconds(100));
}

void install_hooks()
{
    ExecutorHooks h;
    h.on_admit = &hook_admit;
    h.on_push_checked = &hook_push_checked;
    h.on_done = &hook_done;
    if (g_lifted != kNoLifted) h.select_body = &hook_select;
    set_executor_hooks(h);
}

struct Opts {
    std::uint32_t depth = 128;
    std::uint32_t workers = 2;
    bool pool = false;
    std::uint32_t max_desc = 256; /* the attribute's maxSizeJobDescriptor: D0 = 0x3F400 - 4 * max_desc */
};

/* A queue with one port, the guest buffers laid out above. */
struct Rig {
    std::uint32_t queue = QUEUE, port = PORT;

    explicit Rig(const Opts& o = {})
    {
        put32(ATTR + 4, 8);
        put32(ATTR + 8, o.max_desc);
        if (o.pool) {
            for (std::uint32_t c = 0; c < 8; ++c) put32(POOL_ATTR + 4 * c, 0);
            put32(POOL_ATTR + 4 * 0, 2);  /* class 0 (64 B): two blocks */
            put32(POOL_ATTR + 4 * 2, 3);  /* class 2 (256 B): three blocks */
        }
        const std::int32_t rc = jq2_queue_create(1, 0x360001, SPURS, QUEUE, ATTR, o.pool ? POOL : 0,
                                                 o.pool ? POOL_ATTR : 0, 0, RING, o.depth, o.workers, 0);
        CHECK(rc == 0, "create rc=0x%X", unsigned(rc));
        CHECK(jq2_port_create(PORT, QUEUE) == 0, "port create");
    }

    std::int32_t push(std::uint32_t desc, std::uint32_t size, std::uint32_t tag, std::uint32_t flags = 1)
    {
        return jq2_port_push_job(port, desc, size, tag, flags);
    }
    /* Push until the ring has room. */
    void push_wait(std::uint32_t desc, std::uint32_t size, std::uint32_t tag, std::uint32_t flags = 1)
    {
        for (;;) {
            const std::int32_t rc = push(desc, size, tag, flags);
            if (rc == 0) return;
            CHECK(rc == JQ2_ERR_AGAIN, "push rc=0x%X", unsigned(rc));
            if (rc != JQ2_ERR_AGAIN) return;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    void sync_tags_wait(std::uint32_t mask)
    {
        for (;;) {
            const std::int32_t rc = jq2_port_push_sync(port, mask, 0);
            if (rc == 0) return;
            CHECK(rc == JQ2_ERR_AGAIN, "push sync rc=0x%X", unsigned(rc));
            if (rc != JQ2_ERR_AGAIN) return;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    jq2_stats stats()
    {
        jq2_stats s{};
        CHECK(jq2_queue_stats(queue, &s) == 0, "stats");
        return s;
    }
    void teardown()
    {
        CHECK(jq2_port_destroy(port) == 0, "port destroy");
        CHECK(jq2_queue_shutdown(queue) == 0, "shutdown");
        std::uint32_t exit_value = 0xFFFFFFFF;
        CHECK(jq2_queue_join(queue, &exit_value) == 0, "join");
    }
};

std::uint32_t port_a(std::uint32_t port = PORT) { return get32(port + 0x00); }
std::uint32_t port_b(std::uint32_t port = PORT) { return get32(port + 0x10); }
/* A's count, without the wait-kind bits a woken Sync leaves behind (port.hpp). */
std::uint32_t credit_of(std::uint32_t port = PORT) { return port_a(port) & 0x1FFFFFFFu; }
/* The executor never writes the PM's own record area, queue+0x180..+0x57F. */
bool queue_pm_area_clear()
{
    for (std::uint32_t o = 0x180; o < 0x580; ++o)
        if (vm_base[QUEUE + o] != 0) return false;
    return true;
}

/* Job j of a run: descriptor, in and out buffers. */
struct JobSpec {
    std::uint32_t desc, in, out, add, serial;
};
JobSpec spec(std::uint32_t j, std::uint32_t image = IMAGE)
{
    JobSpec s{DESC + 0x100 * j, IN + 0x10 * j, OUT + 0x10 * j, j * 7 + 1, j};
    set_in(s.in, j + 1);
    std::memset(vm_base + s.out, 0, 16);
    build_desc(s.desc, image, s.out, s.in, s.add, s.serial);
    return s;
}
bool job_ok(const JobSpec& s) { return out_is(s.out, s.in, s.add, s.serial, s.out, s.in); }

/* ---- modes -------------------------------------------------------------------- */

void mode_premature()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    Rig rig({128, 1, false});

    /* Admission has not happened: the job runs on what the descriptor holds when
     * a worker admits it, not on what it held at push (S2, as on the PM). */
    CHECK(set_intake_paused(QUEUE, true), "pause");
    const JobSpec a = spec(0);
    CHECK(rig.push(a.desc, 64, 0) == 0, "push a");
    put32(a.desc + 0x38, 555); /* the title reuses the descriptor after the push */
    put32(a.desc + 0x3C, 9);
    CHECK(set_intake_paused(QUEUE, false), "resume");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync a");
    CHECK(out_is(a.out, a.in, 555, 9, a.out, a.in), "the job ran on the overwritten descriptor");
    CHECK(!out_is(a.out, a.in, a.add, a.serial, a.out, a.in), "the push-time bytes did not reach the job");

    /* Admission has happened: a store after the descriptor was read is not seen
     * (the hook runs after the admission copy and before the job starts). */
    const JobSpec b = spec(1);
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = [&](const JobStart& js) {
            if (serial_of(js) == b.serial) {
                put32(b.desc + 0x38, 777);
                put32(b.desc + 0x3C, 8);
            }
        };
    }
    CHECK(rig.push(b.desc, 64, 0) == 0, "push b");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync b");
    CHECK(get32(b.desc + 0x38) == 777, "the descriptor was overwritten after admission");
    CHECK(job_ok(b), "the job ran on the admission-time descriptor (add %u serial %u)", b.add, b.serial);

    const jq2_stats s = rig.stats();
    CHECK(s.dispatched == 2 && s.executed == 2 && s.credited == 2, "counts %llu/%llu/%llu",
          (unsigned long long)s.dispatched, (unsigned long long)s.executed, (unsigned long long)s.credited);
    rig.teardown();
}

void mode_copy()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    Rig rig({128, 2, true});

    /* S3: the source is free to reuse the moment CopyPush returns. */
    CHECK(set_intake_paused(QUEUE, true), "pause");
    const JobSpec a = spec(0);
    CHECK(jq2_port_copy_push(PORT, a.desc, 64, 64, 0, 1) == 0, "copy push a");
    build_desc(a.desc, IMAGE, a.out, a.in, 999, 999); /* scribble the source */
    CHECK(set_intake_paused(QUEUE, false), "resume");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    CHECK(job_ok(a), "the job ran on the descriptor as it was at push");

    /* The class holds two 64-byte blocks: eight serial CopyPushes only complete
     * if every block comes back to the pool when its job has run, and the
     * pusher blocks in the acquire while both are out (paused queue). */
    CHECK(set_intake_paused(QUEUE, true), "pause");
    std::vector<JobSpec> js;
    for (std::uint32_t j = 1; j <= 2; ++j) {
        js.push_back(spec(j));
        CHECK(jq2_port_copy_push(PORT, js.back().desc, 64, 64, 1, 1) == 0, "copy push %u", j);
    }
    std::atomic<bool> third_done{false};
    std::thread t([&] {
        js.push_back(spec(3));
        CHECK(jq2_port_copy_push(PORT, js.back().desc, 64, 64, 1, 1) == 0, "copy push 3");
        third_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(!third_done, "the third CopyPush blocks while the class is empty");
    CHECK(set_intake_paused(QUEUE, false), "resume");
    t.join();
    CHECK(third_done, "the blocked CopyPush completes once a job returned its block");
    for (std::uint32_t j = 4; j <= 8; ++j) {
        js.push_back(spec(j));
        CHECK(jq2_port_copy_push(PORT, js.back().desc, 64, 64, 2, 1) == 0, "copy push %u", j);
    }
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync 2");
    for (const JobSpec& s : js) CHECK(job_ok(s), "copy job %u output", s.serial);
    const jq2_stats s = rig.stats();
    CHECK(s.dispatched == 9 && s.executed == 9 && s.credited == 9, "counts %llu/%llu/%llu",
          (unsigned long long)s.dispatched, (unsigned long long)s.executed, (unsigned long long)s.credited);

    /* A 256-byte class has three blocks; a class nobody configured is refused. */
    CHECK(jq2_port_copy_push(PORT, spec(20).desc, 64, 384, 0, 1) == JQ2_ERR_NULL_POINTER, "unconfigured class");
    CHECK(jq2_port_copy_push(PORT, spec(21).desc, 64, 100, 0, 1) == JQ2_ERR_INVAL, "bad copy size");
    CHECK(jq2_port_copy_push(PORT, spec(22).desc, 128, 64, 0, 1) == JQ2_ERR_INVAL, "size > copy size");
    rig.teardown();

    /* A queue without a pool has no classes. */
    Rig bare({128, 1, false});
    CHECK(jq2_port_copy_push(PORT, spec(30).desc, 64, 64, 0, 1) == JQ2_ERR_NULL_POINTER, "no pool");
    bare.teardown();
}

void mode_credit()
{
    constexpr std::uint32_t kJobs = 1000, kBatch = 100;
    for (int i = 0; i < 3; ++i) build_image(IMAGE + 0x1000 * i, Prog::transform);
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    Rig rig({128, 2, false});

    std::vector<JobSpec> specs;
    std::uint32_t pushed = 0;
    for (std::uint32_t batch = 0; batch < kJobs / kBatch; ++batch) {
        for (std::uint32_t k = 0; k < kBatch; ++k) {
            const std::uint32_t j = batch * kBatch + k;
            specs.push_back(spec(j, IMAGE + 0x1000 * (j % 3)));
            rig.push_wait(specs.back().desc, 64, j % 3);
            ++pushed;
            if (k % 10 == 9) rig.sync_tags_wait(1u << (j % 3));
        }
        /* Poll with mode 4 for a while, then block. */
        for (int p = 0; p < 3; ++p) {
            const std::int32_t rc = jq2_port_sync(PORT, 4);
            CHECK(rc == 0 || rc == JQ2_ERR_AGAIN, "sync poll rc=0x%X", unsigned(rc));
            if (rc == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (p == 2) CHECK(jq2_port_sync(PORT, 0) == 0, "sync wait");
        }
        if (batch > 0 && port_b() != 0) CHECK(jq2_port_sync(PORT, 0) == 0, "sync again");

        const jq2_stats s = rig.stats();
        CHECK(s.dispatched == pushed && s.executed == pushed && s.credited == pushed && s.uncredited == 0,
              "batch %u: %llu/%llu/%llu of %u", batch, (unsigned long long)s.dispatched,
              (unsigned long long)s.executed, (unsigned long long)s.credited, pushed);
        CHECK(credit_of() == 0 && port_b() == 0, "batch %u: A=0x%X B=%u", batch, port_a(), port_b());
        for (std::uint32_t j = batch * kBatch; j < (batch + 1) * kBatch; ++j)
            if (!job_ok(specs[j])) CHECK(false, "job %u output", j);
    }
    /* A job credits its port inside run_job, before its worker re-takes the queue lock to drop `running`,
     * so a Sync that has seen every credit can return while the last worker is still on its way out.
     * `running` is executor-internal; quiescence is eventual, not ordered against the Sync. */
    decltype(observe_queue(QUEUE)) view;
    CHECK(wait_for([&] {
        view = observe_queue(QUEUE);
        return view && view->outstanding == 0 && view->running == 0 && view->barred == 0;
    }), "quiescent: outstanding %u running %u barred 0x%X", view ? view->outstanding : 0, view ? view->running : 0,
          view ? view->barred : 0);
    rig.teardown();
}

void mode_wrap()
{
    constexpr std::uint32_t kJobs = 120;
    build_image(IMAGE, Prog::transform);
    install_hooks();
    Rig rig({4, 2, false});
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < kJobs; ++j) {
        specs.push_back(spec(j));
        rig.push_wait(specs.back().desc, 64, j & 1);
        if (j % 7 == 6) rig.sync_tags_wait(3);
        if (j % 16 == 15) CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    }
    CHECK(jq2_port_sync(PORT, 0) == 0, "final sync");
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    const jq2_stats s = rig.stats();
    CHECK(s.dispatched == kJobs && s.executed == kJobs && s.credited == kJobs, "counts");
    CHECK(get16(QUEUE + 0) == get16(QUEUE + 2), "read %u == write %u at quiescence", get16(QUEUE + 0),
          get16(QUEUE + 2));
    rig.teardown();
}

void mode_barrier()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    Rig rig({16, 2, false});

    /* ring: A(tag1) SYNC(tag1) C(tag1) D(tag0) C2(tag1), with A held inside its
     * admission hook so it stays in flight (the m1r-013 situation). */
    std::mutex mu;
    std::condition_variable cv;
    bool release_a = false;
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = [&](const JobStart& js) {
            if (serial_of(js) != 0) return;
            std::unique_lock<std::mutex> lk2(mu);
            cv.wait(lk2, [&] { return release_a; });
        };
    }
    CHECK(set_intake_paused(QUEUE, true), "pause");
    const JobSpec a = spec(0), c = spec(1), d = spec(2), c2 = spec(3);
    CHECK(rig.push(a.desc, 64, 1) == 0, "push A");
    CHECK(jq2_port_push_sync(PORT, 1u << 1, 0) == 0, "push SYNC");
    CHECK(rig.push(c.desc, 64, 1) == 0, "push C");
    CHECK(rig.push(d.desc, 64, 0) == 0, "push D");
    CHECK(rig.push(c2.desc, 64, 1) == 0, "push C2");
    std::uint8_t ring_before[40];
    std::memcpy(ring_before, vm_base + RING, sizeof ring_before);
    CHECK(set_intake_paused(QUEUE, false), "resume");

    /* D overtakes the retained C: dispatched behind it, marked in queue state. */
    CHECK(wait_for([&] {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        return g_probe.admit.count(d.serial) != 0;
    }), "D is admitted while A is in flight");
    {
        const auto v = observe_queue(QUEUE);
        CHECK(v.has_value(), "view");
        if (v) {
            CHECK(v->indices.read == 2, "read index holds at the retained C: %u", v->indices.read);
            CHECK(v->out_of_order == std::vector<std::uint16_t>{3}, "out-of-order marks: %zu", v->out_of_order.size());
            CHECK((v->barred & 2) != 0, "tag 1 is barred");
            CHECK(v->inflight[1] == 1 && v->inflight[0] <= 1, "in flight per tag");
        }
        CHECK(get16(QUEUE + 0) == 2, "guest read index %u", get16(QUEUE + 0));
        CHECK(queue_pm_area_clear(), "the PM's record area (+0x180..+0x57F) is never written");
        CHECK(std::memcmp(ring_before, vm_base + RING, sizeof ring_before) == 0, "the ring bytes were not rewritten");
        std::lock_guard<std::mutex> lk(g_probe.mu);
        CHECK(g_probe.admit.count(c.serial) == 0 && g_probe.admit.count(c2.serial) == 0,
              "C and C2 wait behind the SYNC while A runs");
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        release_a = true;
    }
    cv.notify_all();
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");

    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        CHECK(g_probe.admit[c.serial] > g_probe.done[a.serial], "C starts after A finished");
        CHECK(g_probe.admit[c2.serial] > g_probe.done[a.serial], "C2 starts after A finished");
        CHECK(g_probe.dispatch[c.serial] < g_probe.dispatch[c2.serial], "same tag, push order: C before C2");
        CHECK(g_probe.admit[d.serial] < g_probe.done[a.serial], "D (other tag) overtook the SYNC");
    }
    for (const JobSpec& s : {a, c, d, c2}) CHECK(job_ok(s), "job %u output", s.serial);
    const auto v = observe_queue(QUEUE);
    CHECK(v && v->indices.read == v->indices.write && v->out_of_order.empty() && v->barred == 0,
          "the marks and the barrier are gone, read %u write %u", v ? v->indices.read : 0, v ? v->indices.write : 0);
    CHECK(get16(QUEUE + 0) == 5 && get16(QUEUE + 2) == 5, "indices 5/5: %u/%u", get16(QUEUE + 0), get16(QUEUE + 2));
    CHECK(queue_pm_area_clear(), "the PM's record area is still clear");
    rig.teardown();
}

void mode_sync()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    Rig rig({128, 2, false});

    CHECK(jq2_port_sync(PORT, 0) == 0, "an idle port syncs at once");
    CHECK(jq2_port_sync(PORT, 4) == 0, "mode 4 on an idle port");
    /* PPU 0x020045E8 accepts only modes 0 and 4. */
    for (const std::uint32_t mode : {1u, 2u, 3u, 5u, 6u, 7u, 8u, 0xCu, 0x80000000u})
        CHECK(jq2_port_sync(PORT, mode) == JQ2_ERR_INVAL, "mode 0x%X on an idle port", mode);

    CHECK(set_intake_paused(QUEUE, true), "pause");
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < 3; ++j) {
        specs.push_back(spec(j));
        CHECK(rig.push(specs.back().desc, 64, 0) == 0, "push %u", j);
    }
    CHECK(port_b() == 3 && port_a() == 0, "A=%u B=%u", port_a(), port_b());
    CHECK(jq2_port_sync(PORT, 4) == JQ2_ERR_AGAIN, "mode 4 with jobs outstanding");
    CHECK(port_b() == 3 && port_a() == 0, "AGAIN leaves A and B");
    {
        /* Jobs outstanding (A < B): a mode that is neither 0 nor 4 is INVAL, does
         * not poll, and leaves A, B and the claim word as they were. Modes 1, 2
         * and 3 are covered on the model (test_jq2_port): accepted here they would
         * block this thread for good. */
        const std::uint32_t claim_word = get32(PORT + 0x14);
        for (const std::uint32_t mode : {5u, 6u, 7u, 8u, 0xCu, 0x80000000u}) {
            CHECK(jq2_port_sync(PORT, mode) == JQ2_ERR_INVAL, "mode 0x%X with jobs outstanding", mode);
            CHECK(port_a() == 0 && port_b() == 3 && get32(PORT + 0x14) == claim_word,
                  "mode 0x%X left A=0x%X B=%u claim=0x%X", mode, port_a(), port_b(), get32(PORT + 0x14));
        }
    }

    std::atomic<std::int32_t> blocked{-1};
    std::thread t([&] { blocked = jq2_port_sync(PORT, 0); });
    CHECK(wait_for([&] { return (port_a() & 0x80000000u) != 0; }), "the blocking Sync registers");
    CHECK(((get32(PORT + 0x14) >> 24) & 0xFF) == 1, "and holds the claim");
    CHECK(blocked == -1, "it is still blocked");
    CHECK(jq2_port_sync(PORT, 0) == JQ2_ERR_BUSY, "a second Sync");
    CHECK(jq2_port_sync(PORT, 4) == JQ2_ERR_BUSY, "a polling Sync");
    const JobSpec extra = spec(9);
    CHECK(rig.push(extra.desc, 64, 0) == JQ2_ERR_BUSY, "a push meets the claim");
    CHECK(port_b() == 3, "and leaves B");
    CHECK(jq2_port_push_sync(PORT, 1, 0) == 0, "push sync does not touch the port");

    CHECK(set_intake_paused(QUEUE, false), "resume");
    t.join();
    CHECK(blocked == 0, "the blocked Sync returns 0 (got 0x%X)", unsigned(blocked.load()));
    CHECK(credit_of() == 0 && port_b() == 0 && ((get32(PORT + 0x14) >> 24) & 0xFF) == 0, "A=0x%X B=%u claim cleared",
          port_a(), port_b());
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    CHECK(jq2_port_sync(PORT, 4) == 0, "nothing outstanding");
    const jq2_stats s = rig.stats();
    CHECK(s.dispatched == 3 && s.executed == 3 && s.credited == 3, "counts");
    rig.teardown();
}

void mode_join()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    std::mutex mu;
    std::condition_variable cv;
    bool release = false;
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = [&](const JobStart& js) {
            if (serial_of(js) != 0) return;
            std::unique_lock<std::mutex> lk2(mu);
            cv.wait(lk2, [&] { return release; });
        };
    }

    /* L2: what the caller owns outside the queue's blocks stays untouched. */
    std::memset(vm_base + 0x7000, 0xA5, 0x1000);        /* before the queue */
    std::memset(vm_base + 0x11000, 0xA5, 0x1000);       /* after the queue block */
    std::memset(vm_base + RING - 0x1000, 0xA5, 0x1000);
    Rig rig({128, 2, false});

    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < 12; ++j) {
        specs.push_back(spec(j));
        CHECK(rig.push(specs.back().desc, 64, j % 2) == 0, "push %u", j);
    }
    CHECK(jq2_queue_shutdown(QUEUE) == JQ2_ERR_BUSY, "a live port blocks Shutdown");
    CHECK(jq2_port_destroy(PORT) == JQ2_ERR_BUSY, "a port with submissions cannot be destroyed");
    CHECK(wait_for([&] { return rig.stats().dispatched >= 1; }), "job 0 admitted");
    /* Sync what can finish, then retire the port the way the game does. */
    std::uint32_t exit_value = 0;
    std::atomic<bool> joined{false};
    std::atomic<std::int32_t> join_rc{-1};
    {
        /* The port needs B == 0 to be destroyed; a Sync cannot return while job 0 is held. */
        std::thread s([&] { CHECK(jq2_port_sync(PORT, 0) == 0, "sync"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(rig.stats().credited < 12, "job 0 still holds the queue");
        {
            std::lock_guard<std::mutex> lk(mu);
            release = true;
        }
        cv.notify_all();
        s.join();
    }
    CHECK(jq2_port_destroy(PORT) == 0, "destroy");
    CHECK(jq2_queue_join(QUEUE, &exit_value) == JQ2_ERR_STAT, "Join before Shutdown");
    CHECK(jq2_queue_shutdown(QUEUE) == 0, "shutdown");
    CHECK(jq2_port_create(PORT, QUEUE) == JQ2_ERR_STAT, "no port after Shutdown");
    CHECK(jq2_port_push_job(PORT, specs[0].desc, 64, 0, 1) == JQ2_ERR_STAT, "no push through a destroyed port");
    std::thread jt([&] {
        join_rc = jq2_queue_join(QUEUE, &exit_value);
        joined = true;
    });
    jt.join();
    CHECK(joined && join_rc == 0, "join rc 0x%X", unsigned(join_rc.load()));
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    CHECK((get32(QUEUE + 0x6C) & 0x20) != 0, "joined bit");
    CHECK(jq2_queue_stats(QUEUE, nullptr) == JQ2_ERR_NULL_POINTER, "no stats for a null out");
    jq2_stats s{};
    CHECK(jq2_queue_stats(QUEUE, &s) == JQ2_ERR_STAT, "the queue is gone");
    for (std::uint32_t i = 0; i < 0x1000; ++i)
        if (vm_base[0x7000 + i] != 0xA5 || vm_base[0x11000 + i] != 0xA5 || vm_base[RING - 0x1000 + i] != 0xA5) {
            CHECK(false, "a byte outside the queue's blocks changed at +0x%X", i);
            break;
        }

    /* Join with jobs outstanding: it returns only after they ran. */
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    Rig rig2({128, 2, false});
    CHECK(set_intake_paused(QUEUE, true), "pause");
    std::vector<JobSpec> more;
    for (std::uint32_t j = 20; j < 50; ++j) {
        more.push_back(spec(j));
        CHECK(rig2.push(more.back().desc, 64, j % 3) == 0, "push %u", j);
    }
    /* Shutdown needs the port gone; the port needs B == 0: sync after resume. */
    std::atomic<bool> resumed{false};
    std::thread r([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        resumed = true;
        set_intake_paused(QUEUE, false);
    });
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    r.join();
    CHECK(jq2_port_destroy(PORT) == 0, "destroy");
    CHECK(jq2_queue_shutdown(QUEUE) == 0, "shutdown");
    CHECK(jq2_queue_join(QUEUE, &exit_value) == 0, "join");
    for (const JobSpec& m : more) CHECK(job_ok(m), "job %u output", m.serial);
}

/* The job observer: one worker's spu_context and the loader's layout at job
 * start, r3 at job end, and nothing at all once it is removed. */
struct ObsState {
    std::mutex mu;
    int starts = 0, ends = 0;
    jq2_job_info start_info{};
    std::uint8_t serial_bytes[4]{};
    void* start_ctx = nullptr;
    void* end_ctx = nullptr;
    std::uint32_t end_r3 = 0;
    struct Mfc { std::uint32_t cmd, lsa, size; std::uint64_t ea; };
    std::vector<Mfc> mfc;
};
ObsState g_obs;

void obs_on_mfc(void*, spu_context*, const spu_obs_mfc* c)
{
    std::lock_guard<std::mutex> lk(g_obs.mu);
    g_obs.mfc.push_back({c->cmd, c->lsa, c->size, c->ea});
}
const spu_observer g_spu_obs = {nullptr, nullptr, &obs_on_mfc, nullptr, nullptr};

void obs_start(void*, spu_context* ctx, const jq2_job_info* info)
{
    std::lock_guard<std::mutex> lk(g_obs.mu);
    ++g_obs.starts;
    g_obs.start_info = *info;
    std::memcpy(g_obs.serial_bytes, info->descriptor + 0x3C, 4);
    g_obs.start_ctx = ctx;
    spu_observer_set(ctx, &g_spu_obs);
}
void obs_end(void*, spu_context* ctx, std::uint32_t r3, const jq2_job_info*)
{
    std::lock_guard<std::mutex> lk(g_obs.mu);
    ++g_obs.ends;
    g_obs.end_ctx = ctx;
    g_obs.end_r3 = r3;
    spu_observer_set(ctx, nullptr);
}

void mode_observer()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    Rig rig({128, 1, false});
    CHECK(jq2_set_job_observer(QUEUE + 0x1000, nullptr) == JQ2_ERR_STAT, "no such queue");

    const jq2_job_observer obs{nullptr, &obs_start, &obs_end};
    CHECK(jq2_set_job_observer(QUEUE, &obs) == 0, "install");
    const JobSpec a = spec(0);
    CHECK(rig.push(a.desc, 64, 0) == 0, "push");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    CHECK(job_ok(a), "the observed job still ran");
    {
        std::lock_guard<std::mutex> lk(g_obs.mu);
        const jq2_job_info& i = g_obs.start_info;
        CHECK(g_obs.starts == 1 && g_obs.ends == 1, "start/end %d/%d", g_obs.starts, g_obs.ends);
        CHECK(g_obs.start_ctx != nullptr && g_obs.start_ctx == g_obs.end_ctx, "the same spu_context");
        CHECK(i.queue == QUEUE && i.port == PORT && i.descriptor_ea == a.desc && i.descriptor_size == 64 &&
                  i.tag == 0 && i.copy == 0 && i.worker == 0,
              "job identity");
        std::uint32_t serial;
        std::memcpy(&serial, g_obs.serial_bytes, 4);
        CHECK(__builtin_bswap32(serial) == a.serial, "the admission bytes");
        CHECK(i.image_ea == IMAGE && i.image_ls == kBase && i.image_bytes == kImageSize &&
                  i.bss_bytes == kBssQuads * 16,
              "image: ea 0x%X ls 0x%X bytes 0x%X bss 0x%X", i.image_ea, i.image_ls, i.image_bytes, i.bss_bytes);
        /* maxDescriptor 256: D0 = 0x3F400 - 4 * 256; the context of slot 0 */
        CHECK(i.desc_bytes == 64 && i.desc_ls == 0x3F000, "descriptor copy: ls 0x%X bytes %u", i.desc_ls,
              i.desc_bytes);
        /* image+bss end at 0x9C00 (A128((0x100 + 0x100) * 16) = 0x2000 from 0x7C00); no cache, io or out, so
         * the scratch block starts there: scratch 0x100, stack 0x400, r1 = T - 0x60 */
        CHECK(i.ctx_ls == 0x68D0 && i.scratch_ls == 0x9C00 && i.r1 == 0x9D00 + 0x400 - 0x60,
              "context 0x%X r1 0x%X scratch 0x%X", i.ctx_ls, i.r1, i.scratch_ls);
        CHECK(g_obs.end_r3 == i.ctx_ls, "r3 at the end: 0x%X", g_obs.end_r3);
        /* The observer installed in job_start saw the job's two MFC commands. */
        CHECK(g_obs.mfc.size() == 2, "mfc commands: %zu", g_obs.mfc.size());
        if (g_obs.mfc.size() == 2) {
            CHECK(g_obs.mfc[0].cmd == MFC_GET_CMD && g_obs.mfc[0].ea == a.in && g_obs.mfc[0].lsa == kLsIn &&
                      g_obs.mfc[0].size == 16,
                  "the GET");
            CHECK(g_obs.mfc[1].cmd == MFC_PUT_CMD && g_obs.mfc[1].ea == a.out && g_obs.mfc[1].lsa == kLsOut &&
                      g_obs.mfc[1].size == 16,
                  "the PUT");
        }
    }

    /* Removed: the next job runs on the plain path. */
    CHECK(jq2_set_job_observer(QUEUE, nullptr) == 0, "remove");
    const JobSpec b = spec(1);
    CHECK(rig.push(b.desc, 64, 0) == 0, "push b");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync b");
    CHECK(job_ok(b), "job b output");
    {
        std::lock_guard<std::mutex> lk(g_obs.mu);
        CHECK(g_obs.starts == 1 && g_obs.ends == 1 && g_obs.mfc.size() == 2, "no callbacks after removal");
    }
    rig.teardown();
}

void expect_stop(const std::function<void()>& child, const char* kind, std::initializer_list<const char*> needles);

void mode_validate()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    put32(ATTR + 4, 8);
    put32(ATTR + 8, 256);
    constexpr std::int32_t NIL = JQ2_ERR_NULL_POINTER, AL = JQ2_ERR_ALIGN, IN_ = JQ2_ERR_INVAL;
    const auto create = [&](std::uint32_t spurs, std::uint32_t queue, std::uint32_t attr, std::uint32_t pool,
                            std::uint32_t pattr, std::uint32_t ring, std::uint32_t depth, std::uint32_t workers) {
        return jq2_queue_create(1, 2, spurs, queue, attr, pool, pattr, 0, ring, depth, workers, 0);
    };
    CHECK(create(0, QUEUE, ATTR, 0, 0, RING, 128, 2) == NIL, "null spurs");
    CHECK(create(SPURS, 0, ATTR, 0, 0, RING, 128, 2) == NIL, "null queue");
    CHECK(create(SPURS, QUEUE, 0, 0, 0, RING, 128, 2) == NIL, "null attr");
    CHECK(create(SPURS, QUEUE, ATTR, 0, 0, 0, 128, 2) == NIL, "null ring");
    CHECK(create(SPURS + 16, QUEUE, ATTR, 0, 0, RING, 128, 2) == AL, "spurs alignment");
    CHECK(create(SPURS, QUEUE + 16, ATTR, 0, 0, RING, 128, 2) == AL, "queue alignment");
    CHECK(create(SPURS, QUEUE, ATTR, 0, 0, RING + 16, 128, 2) == AL, "ring alignment");
    CHECK(create(SPURS, QUEUE, ATTR, POOL, 0, RING, 128, 2) == NIL, "pool without attr");
    CHECK(create(SPURS, QUEUE, ATTR, 0, POOL_ATTR, RING, 128, 2) == NIL, "attr without pool");
    CHECK(create(SPURS, QUEUE, ATTR, POOL + 16, POOL_ATTR, RING, 128, 2) == AL, "pool alignment");
    CHECK(create(SPURS, QUEUE, ATTR, POOL + 16, POOL_ATTR, RING, 0x8001, 2) == AL,
          "misaligned pool with depth > 0x8000 gives ALIGN");
    /* The firmware never aligns poolAttr: a 16-byte-aligned stack attribute
     * must create the queue (it returned ALIGN here before). */
    put32(POOL_ATTR + 16 + 0x08, 1); /* class 2: one block, like POOL_ATTR */
    CHECK(create(SPURS, QUEUE, ATTR, POOL, POOL_ATTR + 16, RING, 128, 2) == 0, "16-aligned pool attribute");
    {
        std::uint32_t exit_value = 0;
        CHECK(jq2_queue_shutdown(QUEUE) == 0 && jq2_queue_join(QUEUE, &exit_value) == 0, "teardown");
    }
    CHECK(create(SPURS, QUEUE, ATTR, 0, 0, RING, 0x8001, 2) == IN_, "depth over 0x8000");
    expect_stop([&] { create(SPURS, QUEUE, ATTR, 0, 0, RING, 0, 2); }, "jq2-unmodelled",
                {"depth=0", "workers=2"});
    CHECK(create(SPURS, QUEUE, ATTR, 0, 0, RING, 128, 9) == IN_, "9 workers");
    expect_stop([&] { create(SPURS, QUEUE, ATTR, 0, 0, RING, 128, 0); }, "jq2-unmodelled",
                {"depth=128", "workers=0"});
    put32(ATTR + 8, 1024);
    CHECK(create(SPURS, QUEUE, ATTR, 0, 0, RING, 128, 2) == IN_, "max descriptor 1024");
    put32(ATTR + 8, 200);
    CHECK(create(SPURS, QUEUE, ATTR, 0, 0, RING, 128, 2) == IN_, "max descriptor 200");
    put32(ATTR + 8, 320);
    CHECK(create(SPURS, QUEUE, ATTR, 0, 0, RING, 128, 2) == IN_, "max descriptor unaligned");
    put32(ATTR + 8, 256);
    put32(ATTR + 4, 0);
    CHECK(create(SPURS, QUEUE, ATTR, 0, 0, RING, 128, 2) == IN_, "0 classes");
    put32(ATTR + 4, 17);
    CHECK(create(SPURS, QUEUE, ATTR, 0, 0, RING, 128, 2) == IN_, "17 classes");
    put32(ATTR + 4, 8);
    CHECK(create(SPURS, QUEUE, ATTR, 0, 0, RING, 128, 2) == 0, "create");
    CHECK(create(SPURS, QUEUE, ATTR, 0, 0, RING, 128, 2) == JQ2_ERR_STAT, "a live queue");

    CHECK(get32(QUEUE + 0x10) == RING && get16(QUEUE + 0x14) == 128, "ring and depth");
    CHECK(get32(QUEUE + 0x04) == 0x60 && get16(QUEUE + 0) == 0 && get16(QUEUE + 2) == 0, "index word");
    CHECK(get32(QUEUE + 0xF10) == SPURS && get32(QUEUE + 0xF14) == 256 && get32(QUEUE + 0xFF8) == 2 &&
              get32(QUEUE + 0xFFC) == 1,
          "spurs, max descriptor, sdk, revision");
    CHECK(get32(QUEUE + 0x80) != 0 && get32(QUEUE + 0x84) != 0 && get32(QUEUE + 0x80) != get32(QUEUE + 0x84),
          "two event flags");

    CHECK(jq2_port_create(0, QUEUE) == NIL && jq2_port_create(PORT, 0) == NIL, "port create nulls");
    CHECK(jq2_port_create(PORT + 16, QUEUE) == AL && jq2_port_create(PORT, QUEUE + 16) == AL, "port create alignment");
    CHECK(jq2_port_create(PORT, QUEUE + 0x1000) == JQ2_ERR_STAT, "no such queue");
    CHECK(jq2_port_create(PORT, QUEUE) == 0, "port create");
    CHECK(get32(PORT + 0x08) == QUEUE && get32(PORT + 0x18) == QUEUE && get32(PORT + 0x1C) == 0 &&
              get32(PORT + 0x20) == 0x01000000,
          "port block: link, queue, slot, initialised byte");
    CHECK(get32(QUEUE + 0x70) == 1 && (get32(QUEUE + 0x580) & 1) == 1, "port count and slot bit");
    CHECK(jq2_port_create(PORT + 0x80, QUEUE) == 0 && get32(PORT + 0x80 + 0x1C) == 1, "second port, slot 1");
    CHECK(jq2_port_destroy(PORT + 0x80) == 0 && get32(QUEUE + 0x70) == 1 && (get32(QUEUE + 0x580) & 2) == 0,
          "destroy frees the slot");
    CHECK(jq2_port_destroy(PORT + 0x80) == JQ2_ERR_STAT, "destroy of a destroyed port");
    CHECK(jq2_port_create(PORT + 0x80, QUEUE) == 0, "second port again");
    vm_base[PORT + 0x80 + 0x21] = 1; /* a port block whose state byte is set (0x02004890) */
    CHECK(jq2_port_destroy(PORT + 0x80) == JQ2_ERR_STAT, "destroy with +0x21 set");
    vm_base[PORT + 0x80 + 0x21] = 0;
    CHECK(jq2_port_destroy(PORT + 0x80) == 0, "destroy once +0x21 is clear");
    CHECK(jq2_port_destroy(0) == NIL && jq2_port_destroy(PORT + 16) == AL, "destroy guards");

    const JobSpec a = spec(0);
    CHECK(jq2_port_push_job(0, a.desc, 64, 0, 1) == NIL, "push null port");
    CHECK(jq2_port_push_job(PORT + 16, a.desc, 64, 0, 1) == AL, "push port alignment");
    CHECK(jq2_port_push_job(PORT + 0x100, a.desc, 64, 0, 1) == JQ2_ERR_STAT, "push through an uninitialised port");
    CHECK(jq2_port_push_job(PORT, 0, 64, 0, 1) == NIL, "null descriptor");
    CHECK(jq2_port_push_job(PORT, a.desc + 8, 64, 0, 1) == AL, "descriptor alignment");
    CHECK(jq2_port_push_job(PORT, a.desc, 64, 16, 1) == IN_, "tag 16");
    CHECK(jq2_port_push_job(PORT, a.desc, 100, 0, 1) == IN_, "size 100");
    CHECK(jq2_port_push_job(PORT, a.desc, 512, 0, 1) == IN_, "size 512");
    CHECK(jq2_port_push_job(PORT, a.desc, 256, 0, 1) == 0, "256 is the queue's maximum");
    CHECK(port_b() == 1, "B counts it");
    CHECK(jq2_port_push_sync(PORT, 0, 0) == IN_ && jq2_port_push_sync(PORT, 0x10000, 0) == IN_ &&
              jq2_port_push_sync(PORT, 1, 2) == IN_,
          "push sync arguments");
    CHECK(jq2_port_push_sync(0, 1, 0) == NIL && jq2_port_push_sync(PORT + 16, 1, 0) == AL, "push sync port guards");
    CHECK(jq2_port_sync(0, 0) == NIL && jq2_port_sync(PORT + 16, 0) == AL, "sync port guards");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");

    /* A pushed-without-port job runs, is never credited and never counted in B. */
    const JobSpec b = spec(1);
    CHECK(jq2_port_push_job(PORT, b.desc, 64, 0, 0) == 0, "push with flags 0");
    CHECK(port_b() == 0, "flags 0 leaves B");
    CHECK(wait_for([&] { jq2_stats st{}; jq2_queue_stats(QUEUE, &st); return st.executed == 2; }), "both ran");
    jq2_stats st{};
    jq2_queue_stats(QUEUE, &st);
    CHECK(st.uncredited == 1 && st.credited == 1 && st.dispatched == 2, "credited 1 uncredited 1");
    CHECK(job_ok(b), "the uncredited job's output");

    CHECK(jq2_queue_shutdown(0) == NIL && jq2_queue_shutdown(QUEUE + 16) == AL && jq2_queue_shutdown(QUEUE + 0x1000) == JQ2_ERR_STAT,
          "shutdown guards");
    std::uint32_t ev = 0;
    CHECK(jq2_queue_join(0, &ev) == NIL && jq2_queue_join(QUEUE, nullptr) == NIL && jq2_queue_join(QUEUE + 16, &ev) == AL,
          "join guards");
    CHECK(jq2_port_destroy(PORT) == 0 && jq2_queue_shutdown(QUEUE) == 0 && jq2_queue_join(QUEUE, &ev) == 0, "teardown");
}

/* ---- fork-based stops --------------------------------------------------------- */

/* Runs `child` in a forked process whose stderr is captured; returns its exit
 * status and what it printed. The guest memory is shared, so the parent reads
 * the port the child left behind. */
int run_child(const std::function<void()>& child, std::string& err)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        child();
        _exit(0);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) err.append(buf, std::size_t(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void stop_scenario(Prog prog, const char* kind, const char* needle, const std::function<void(const JobSpec&)>& patch = {})
{
    const std::function<void()> child = [prog, &patch] {
        build_image(IMAGE, prog);
        install_hooks();
        Rig rig({128, 1, false});
        const JobSpec a = spec(0);
        if (patch) patch(a);
        rig.push(a.desc, 64, 0);
        jq2_port_sync(PORT, 0); /* never returns: the worker stops the process */
        std::fprintf(stderr, "the stop did not happen\n");
    };
    std::string err;
    const int rc = run_child(child, err);
    CHECK(rc == 70, "exit status %d, wanted 70 (a named stop)", rc);
    const std::string want = std::string("kind=") + kind;
    CHECK(err.find(want) != std::string::npos, "stderr names %s", want.c_str());
    if (needle != nullptr) CHECK(err.find(needle) != std::string::npos, "stderr carries %s", needle);
    CHECK(err.find("the stop did not happen") == std::string::npos, "the Sync returned");
    /* The job did not return, so nothing was credited and the push still counts. */
    CHECK(port_a() == 0 || port_a() == 0xDFFFFFFFu, "port A is 0x%X: a credit would have made it 0 or cleared the waiter", port_a());
    CHECK(port_b() == 1, "port B is %u", port_b());
    std::memset(vm_base, 0, 0x1000);
}

void mode_stop_job() { stop_scenario(Prog::stop_early, "jq2-job-end", "stop_code=0x2222"); }
void mode_stop_crt() { stop_scenario(Prog::bad_crt, "spu-job-crt-version", nullptr); }

/* A lifted-style job that halts before its output write: the Sync never
 * returns, the output stays as it was and nothing is credited. */
void mode_stop_halt()
{
    g_lifted = kLiftedHalt;
    stop_scenario(Prog::transform, "jq2-job-end", "reason=halt");
    const JobSpec a = spec(0);
    for (std::uint32_t i = 0; i < 4; ++i)
        CHECK(get32(a.out + 4 * i) == 0, "the halted job wrote its output");
}

/* The normal return of a lifted-style body: executed and credited once. */
void mode_lifted()
{
    g_lifted = kLiftedNormal;
    build_image(IMAGE, Prog::transform);
    install_hooks();
    Rig rig({128, 1, false});
    const JobSpec a = spec(0);
    rig.push(a.desc, 64, 0);
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    CHECK(job_ok(a), "the lifted job's output");
    jq2_stats s{};
    CHECK(jq2_queue_stats(QUEUE, &s) == 0, "stats");
    CHECK(s.dispatched == 1 && s.executed == 1 && s.credited == 1, "counts %llu/%llu/%llu",
          (unsigned long long)s.dispatched, (unsigned long long)s.executed, (unsigned long long)s.credited);
    rig.teardown();
}

/* ---- the JQ2 job profile (docs/JQ2.md, "JQ2 job ABI") -------------------------- */

/* What a job's context holds at its first instruction: the whole local store,
 * the registers and pc, and the loader's layout report. Addresses and words in
 * the checks below are derived from the descriptor by hand, not read back from
 * the implementation. */
struct StartState {
    std::mutex mu;
    int starts = 0;
    jq2_job_info info{};
    std::vector<std::uint8_t> ls = std::vector<std::uint8_t>(SPU_LS_SIZE);
    u128 gpr[128]{};
    std::uint32_t pc = 0;
};
StartState g_start;

void state_start(void*, spu_context* ctx, const jq2_job_info* info)
{
    std::lock_guard<std::mutex> lk(g_start.mu);
    ++g_start.starts;
    g_start.info = *info;
    std::memcpy(g_start.ls.data(), ctx->ls, SPU_LS_SIZE);
    std::memcpy(g_start.gpr, ctx->gpr, sizeof g_start.gpr);
    g_start.pc = ctx->pc;
}
void state_end(void*, spu_context*, std::uint32_t, const jq2_job_info*) {}

std::uint32_t ls_be32(const std::vector<std::uint8_t>& ls, std::uint32_t a)
{
    return (std::uint32_t{ls[a]} << 24) | (std::uint32_t{ls[a + 1]} << 16) | (std::uint32_t{ls[a + 2]} << 8) | ls[a + 3];
}

bool ls_zero(const std::vector<std::uint8_t>& ls, std::uint32_t lo, std::uint32_t hi)
{
    for (std::uint32_t a = lo; a < hi; ++a)
        if (ls[a] != 0) return false;
    return true;
}

bool ls_equals_guest(const std::vector<std::uint8_t>& ls, std::uint32_t lsa, std::uint32_t ea, std::uint32_t n)
{
    return std::memcmp(ls.data() + lsa, vm_base + ea, n) == 0;
}

/* A descriptor with caches, an input list, io, out, scratch and stack:
 *   image 0x1000 + bss (0x100 quadwords): A128((0x100 + 0x100) * 16) = 0x2000 from 0x7C00 -> 0x9C00
 *   cache 0: A128(0x100) = 0x100 -> 0x9C00..0x9D00; cache 1: A128(0x150) = 0x180 -> 0x9D00..0x9E80
 *   io: A128(0x4F0) = 0x500 -> 0x9E80..0xA380; out: A128(0x2C0) = 0x300 -> 0xA380..0xA680
 *   scratch+stack: s = 0xA680, stack base = s + A128(0x400) = 0xAA80, T = + 0x800 = 0xB280
 *   r1 = T - 0x60 = 0xB220, r80 = 0xB240, r81 = 0xB250
 * maxDescriptor `max_desc`: D0 = 0x3F400 - 4 * max_desc (256: 0x3F000, 896: 0x3E600, the
 * descriptor copy address the firmware run shows). The workload id is the low bits of
 * queue+0x6C, which CreateCommon wrote as the SPURS EA | 0; the test sets `wid` there
 * and expects it in the kernel view at 0x1D8 and 0x1DC. */
void mode_layout(std::uint32_t max_desc, std::uint32_t wid)
{
    constexpr std::uint32_t CACHE0 = 0x600000, CACHE1 = 0x601000, BIG_IN = 0x602000;
    build_image(IMAGE, Prog::ret_only);
    install_hooks();
    Rig rig({128, 1, false, max_desc});
    const std::uint32_t d0 = 0x3F400 - 4 * max_desc;
    CHECK(get32(QUEUE + 0x6C) == SPURS, "CreateCommon leaves queue+0x6C = spurs | wid 0: 0x%X", get32(QUEUE + 0x6C));
    put32(QUEUE + 0x6C, SPURS | wid);
    const JobSpec a = spec(0);
    const std::uint32_t d = a.desc;
    std::memset(vm_base + d, 0, 0x100);
    put64(d + 0x00, (std::uint64_t{kBssQuads} << 32) | (IMAGE | 1u));
    put16(d + 0x08, kImageSize >> 4);
    put16(d + 0x0A, 8);      /* one input list element */
    put32(d + 0x14, 0x4F0);  /* sizeInOrInOut */
    put32(d + 0x18, 0x2C0);  /* sizeOut */
    put16(d + 0x1C, 0x80);   /* stack 0x800 */
    put16(d + 0x1E, 0x40);   /* scratch 0x400 */
    put32(d + 0x24, 16);     /* two cache entries */
    put64(d + 0x30, (std::uint64_t{0x80} << 32) | BIG_IN);
    put64(d + 0x38, (std::uint64_t{0x100} << 32) | CACHE0);
    put64(d + 0x40, (std::uint64_t{0x150} << 32) | CACHE1);
    for (std::uint32_t i = 0x48; i < 0x80; ++i) vm_base[d + i] = std::uint8_t(0xA0 + i);
    for (std::uint32_t i = 0; i < 0x80; ++i) vm_base[BIG_IN + i] = std::uint8_t(0x20 + i);
    for (std::uint32_t i = 0; i < 0x100; ++i) vm_base[CACHE0 + i] = std::uint8_t(0x40 + 3 * i);
    for (std::uint32_t i = 0; i < 0x150; ++i) vm_base[CACHE1 + i] = std::uint8_t(0x90 + 5 * i);

    const jq2_job_observer obs{nullptr, &state_start, &state_end};
    CHECK(jq2_set_job_observer(QUEUE, &obs) == 0, "install");
    CHECK(rig.push(d, 128, 0) == 0, "push");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");

    std::lock_guard<std::mutex> lk(g_start.mu);
    CHECK(g_start.starts == 1, "starts %d", g_start.starts);
    const std::vector<std::uint8_t>& ls = g_start.ls;
    const jq2_job_info& i = g_start.info;

    /* the loader's report */
    CHECK(i.image_ls == 0x7C00 && i.image_bytes == 0x1000 && i.bss_bytes == 0x1000, "image 0x%X+0x%X bss 0x%X",
          i.image_ls, i.image_bytes, i.bss_bytes);
    CHECK(i.desc_ls == d0 && i.desc_bytes == 128 && i.ctx_ls == 0x68D0, "desc 0x%X ctx 0x%X", i.desc_ls, i.ctx_ls);
    CHECK(i.io_ls == 0x9E80 && i.io_bytes == 0x4F0 && i.out_ls == 0xA380 && i.out_bytes == 0x2C0 &&
              i.scratch_ls == 0xA680 && i.scratch_bytes == 0x400 && i.stack_bytes == 0x800 && i.r1 == 0xB220,
          "io 0x%X out 0x%X scratch 0x%X r1 0x%X", i.io_ls, i.out_ls, i.scratch_ls, i.r1);

    /* image, bss, caches, input list, descriptor copy */
    CHECK(ls_equals_guest(ls, 0x7C00, IMAGE, 0x1000), "image bytes at 0x7C00");
    CHECK(ls_zero(ls, 0x8C00, 0x9C00), "bss is not copied");
    CHECK(ls_equals_guest(ls, 0x9C00, CACHE0, 0x100), "cache buffer 0");
    CHECK(ls_equals_guest(ls, 0x9D00, CACHE1, 0x150) && ls_zero(ls, 0x9E50, 0x9E80), "cache buffer 1 and its padding");
    CHECK(ls_equals_guest(ls, 0x9E80, BIG_IN, 0x80) && ls_zero(ls, 0x9F00, 0xA680), "input list staged at io, rest zero");
    CHECK(ls_equals_guest(ls, d0, d, 128), "descriptor copy at D0");

    /* JobContext2 at 0x68D0 */
    const std::uint32_t x = 0x68D0;
    const std::uint32_t ctx_words[12] = {0x9E80, 0x9C00, 0x9D00, 0, 0, 0x10007A10, 0x00010002, 0xA380, 0xA680, 0, 0, d};
    for (std::uint32_t k = 0; k < 12; ++k)
        CHECK(ls_be32(ls, x + 4 * k) == ctx_words[k], "ctx+0x%02X = 0x%08X, want 0x%08X", 4 * k, ls_be32(ls, x + 4 * k),
              ctx_words[k]);

    /* jm interface {0x1500, 0x5870, 0, 0}, the stop word and the poll routine */
    CHECK(ls_be32(ls, 0x7A10) == 0x1500 && ls_be32(ls, 0x7A14) == 0x5870 && ls_be32(ls, 0x7A18) == 0 &&
              ls_be32(ls, 0x7A1C) == 0,
          "jm table");
    CHECK(ls_be32(ls, 0x1500) == 0x3B0F, "jm word 0 is a stop 0x3B0F: 0x%08X", ls_be32(ls, 0x1500));
    static const std::uint32_t poll[8] = {0x40FFFF83u, 0x21A00B03u, 0x40800003u, 0x21A00B83u,
                                          0x01A00C03u, 0x7CFFC183u, 0x207FFD03u, 0x35000000u};
    for (std::uint32_t k = 0; k < 8; ++k)
        CHECK(ls_be32(ls, 0x5870 + 4 * k) == poll[k], "poll routine word %u", k);

    /* the kernel view: exactly these words, everything else in 0..0x2FF zero */
    const std::uint32_t spu_num = i.worker;
    for (std::uint32_t a2 = 0; a2 < 0x300; a2 += 4) {
        std::uint32_t want = 0;
        switch (a2) {
        case 0x1C4: want = SPURS; break;     /* spurs EA, low word of the u64 at 0x1C0 */
        case 0x1C8: want = spu_num; break;
        case 0x1CC: want = 8; break;
        case 0x1D4: want = 0xA00; break;     /* wklCurrentAddr low word */
        case 0x1D8: case 0x1DC: want = wid; break;   /* wklCurrentUniqueId, wklCurrentId */
        default: break;
        }
        CHECK(ls_be32(ls, a2) == want, "kernel view 0x%03X = 0x%08X, want 0x%08X", a2, ls_be32(ls, a2), want);
    }

    /* entry state: only these lanes are non-zero (r1 words 1..3: the stack bytes left, 0x800 - 0x60) */
    CHECK(g_start.pc == 0x7C00, "pc 0x%X", g_start.pc);
    for (unsigned r = 0; r < 128; ++r) {
        std::uint32_t want = 0;
        switch (r) {
        case 0: want = 0x3FF00; break;
        case 1: want = 0xB220; break;
        case 3: want = 0x68D0; break;
        case 4: want = d0; break;
        case 80: want = 0xB240; break;
        case 81: want = 0xB250; break;
        case 126: want = 0x7C00; break;
        default: break;
        }
        const std::uint32_t rest = r == 1 ? 0x800 - 0x60 : 0;
        CHECK(g_start.gpr[r]._u32[0] == want && g_start.gpr[r]._u32[1] == rest && g_start.gpr[r]._u32[2] == rest &&
                  g_start.gpr[r]._u32[3] == rest,
              "r%u = %08X %08X %08X %08X, want %08X %08X..", r, g_start.gpr[r]._u32[0], g_start.gpr[r]._u32[1],
              g_start.gpr[r]._u32[2], g_start.gpr[r]._u32[3], want, rest);
    }
    const jq2_stats s = rig.stats();
    CHECK(s.dispatched == 1 && s.executed == 1 && s.credited == 1, "counts");
    rig.teardown();
}

/* The jm word 1 routine waits for a tag group. The job GETLs a two-element list
 * whose first element stalls; the second element moves only once the stall is
 * acknowledged, which the test does after the routine has polled RdTagStat
 * three times. The job then calls the routine through the jm table and uses the
 * second element's data: a routine that returned early would have read zeros. */
struct WaitState {
    std::atomic<int> reads{0}, acks{0};
};
WaitState g_wait;

void wait_channel(void*, spu_context* ctx, int is_write, std::uint32_t ch, std::uint32_t)
{
    if (is_write) {
        if (ch == MFC_WrListStallAck) g_wait.acks.fetch_add(1);
        return;
    }
    if (ch != MFC_RdTagStat) return;
    if (g_wait.reads.fetch_add(1) + 1 == 4) spu_wrch(ctx, MFC_WrListStallAck, spu_splat_u32(1));
}
const spu_observer g_wait_obs = {nullptr, nullptr, nullptr, nullptr, &wait_channel};
void wait_start(void*, spu_context* ctx, const jq2_job_info*) { spu_observer_set(ctx, &g_wait_obs); }
void wait_end(void*, spu_context* ctx, std::uint32_t, const jq2_job_info*) { spu_observer_set(ctx, nullptr); }

void mode_jm_wait()
{
    build_image(IMAGE, Prog::jm_wait);
    install_hooks();
    Rig rig({128, 1, false});
    const jq2_job_observer obs{nullptr, &wait_start, &wait_end};
    CHECK(jq2_set_job_observer(QUEUE, &obs) == 0, "install");
    const JobSpec a = spec(0);
    const JobSpec second = spec(1); /* only its input, at IN + 0x10, is used */
    CHECK(rig.push(a.desc, 64, 0) == 0, "push");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    CHECK(g_wait.acks.load() == 1, "stall acknowledged %d times", g_wait.acks.load());
    CHECK(g_wait.reads.load() >= 5, "RdTagStat read %d times: the routine did not poll until the tag completed",
          g_wait.reads.load());
    CHECK(out_is(a.out, second.in, a.add, a.serial, a.out, a.in), "the job used the second element's data");
    const jq2_stats s = rig.stats();
    CHECK(s.executed == 1 && s.credited == 1, "counts");
    rig.teardown();
}

/* A job that calls jm word 0: interpreted, and lifted. */
void mode_stop_jm() { stop_scenario(Prog::jm_call, "jq2-jm-call", "request=0x1234"); }
void mode_stop_jm_lifted()
{
    g_lifted = kLiftedJmCall;
    stop_scenario(Prog::jm_call, "jq2-jm-call", "request=0x1234");
}

/* The heap runs from the image to D0 = 0x3F000. io 0x34F00 leaves exactly
 * 0x3F000 - 0x9C00 - 0x34F00 - 0x100 - 0x400 = 0: the job runs. 0x80 more and the
 * stack block ends at 0x3F080: spu-job-memory, region stack, limit D0. */
void mode_heap_fit()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    Rig rig({128, 1, false});
    const JobSpec a = spec(0);
    put32(a.desc + 0x14, 0x34F00);
    CHECK(rig.push(a.desc, 64, 0) == 0, "push");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    CHECK(job_ok(a), "the job that fills the heap exactly ran");
    const jq2_stats s = rig.stats();
    CHECK(s.executed == 1 && s.credited == 1, "counts");
    rig.teardown();
}

void mode_stop_heap()
{
    const auto over = [](const JobSpec& a) { put32(a.desc + 0x14, 0x34F80); };
    stop_scenario(Prog::transform, "spu-job-memory", "region=stack", over);
    stop_scenario(Prog::transform, "spu-job-memory", "p=0x3F080", over);
    stop_scenario(Prog::transform, "spu-job-memory", "limit=0x3F000", over);
}

/* What the PM's evidence does not decode is a named stop, not a guess: the
 * jobType byte's bit 1 (U1), the eaBinary low word's bit 1 (U6) and more than
 * the two cache entries the PM was observed with (U5). */
void mode_stop_unmodelled()
{
    stop_scenario(Prog::transform, "spu-job-dma-list", "reason=jobtype-bit1",
                  [](const JobSpec& a) { put32(a.desc + 0x2C, 0x06000000u); });
    stop_scenario(Prog::transform, "spu-job-dma-list", "reason=binary-bit1",
                  [](const JobSpec& a) { put32(a.desc + 4, IMAGE | 3u); });
    /* jobType 0x04 (bit 2: the ELF flag every Odin image carries) alone is fine; three cache entries are not */
    stop_scenario(Prog::transform, "spu-job-dma-list", "reason=cache-count", [](const JobSpec& a) {
        put32(a.desc + 0x2C, 0x04000000u);
        put32(a.desc + 0x24, 24);
    });
}

void mode_stop_ring()
{
    const std::function<void()> child = [] {
        build_image(IMAGE, Prog::transform);
        install_hooks();
        Rig rig({128, 1, false});
        CHECK(set_intake_paused(QUEUE, true), "pause");
        /* A JOBLIST command (type 1) the writer never produces. */
        put32(RING + 0, 0x00020001u);
        put32(RING + 4, 0x00001001u);
        std::uint64_t w = (std::uint64_t{0} << 48) | (std::uint64_t{1} << 32) | 0x60;
        std::uint64_t raw = __builtin_bswap64(w);
        std::memcpy(vm_base + QUEUE, &raw, 8);
        set_intake_paused(QUEUE, false);
        jq2_port_sync(PORT, 0); /* nothing pushed through the port: returns */
        std::this_thread::sleep_for(std::chrono::seconds(30));
    };
    std::string err;
    const int rc = run_child(child, err);
    CHECK(rc == 70, "exit status %d, wanted 70", rc);
    CHECK(err.find("kind=jq2-ring-entry") != std::string::npos, "stderr names kind=jq2-ring-entry");
}

/* ---- waiters, waits and Shutdown ------------------------------------------------ */

/* If the stop has not happened by the deadline the child says so and leaves with
 * status 1, so the parent's checks fail instead of the ctest timeout. */
void arm_child_watchdog(int seconds)
{
    std::thread([seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        std::fprintf(stderr, "the stop did not happen\n");
        std::fflush(stderr);
        _exit(1);
    }).detach();
}

void expect_stop(const std::function<void()>& child, const char* kind, std::initializer_list<const char*> needles)
{
    std::string err;
    const int rc = run_child(child, err);
    CHECK(rc == 70, "exit status %d, wanted 70 (a named stop)", rc);
    CHECK(err.find(std::string("kind=") + kind) != std::string::npos, "stderr names kind=%s", kind);
    for (const char* needle : needles) CHECK(err.find(needle) != std::string::npos, "stderr carries %s", needle);
    CHECK(err.find("the stop did not happen") == std::string::npos, "the child ran past the stop");
}

std::uint32_t bitmap_atomic(spu_context* ctx, std::uint32_t cmd)
{
    spu_wrch(ctx, MFC_LSA, spu_make_preferred_u32(0x1000));
    spu_wrch(ctx, MFC_EAH, spu_make_preferred_u32(0));
    spu_wrch(ctx, MFC_EAL, spu_make_preferred_u32(QUEUE + 0x80));
    spu_wrch(ctx, MFC_Cmd, spu_make_preferred_u32(cmd));
    return spu_rdch(ctx, MFC_RdAtomicStat)._u32[0];
}

/* Exercise the real Sync path twice: zero is allocated and leaked, not
 * pre-reserved, and the released event id remains stale in the port. */
void mode_waiter_fresh()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    Rig rig({128, 1, false});
    auto spu = std::make_unique<spu_context>();
    spu_context_init(spu.get(), 7);
    for (unsigned w = 0; w < 4; ++w) CHECK(get32(QUEUE + 0x88 + 4 * w) == 0, "fresh bitmap word %u", w);
    for (unsigned round = 0; round < 2; ++round) {
        CHECK(set_intake_paused(QUEUE, true), "pause");
        const JobSpec job = spec(round);
        CHECK(rig.push(job.desc, 64, 0) == 0, "push");
        ppu_context reservation{};
        resv_lwarx32(&reservation, QUEUE + 0x98);
        bitmap_atomic(spu.get(), MFC_GETLLAR_CMD);
        std::int32_t rc = -1;
        std::thread waiter([&] { rc = jq2_port_sync(PORT, 0); });
        CHECK(wait_for([] { return (port_a(PORT) & 0x80000000u) != 0; }), "registered");
        CHECK(get32(QUEUE + 0x88) == 0xC0000000u, "id0 leaked and id1 allocated");
        CHECK(get32(PORT + 4) == 1, "first wakeable id is 1");
        CHECK(resv_stcx32(&reservation, QUEUE + 0x98, 0) == 0, "allocation invalidates neighboring reservation");
        CHECK(bitmap_atomic(spu.get(), MFC_PUTLLC_CMD) != 0, "allocation kills SPU reservation");
        bitmap_atomic(spu.get(), MFC_GETLLAR_CMD);
        resv_lwarx32(&reservation, QUEUE + 0x98);
        CHECK(jq2_queue_shutdown(QUEUE) == JQ2_ERR_BUSY, "attached blocked port prevents Shutdown");
        CHECK(jq2_port_destroy(PORT) == JQ2_ERR_BUSY, "blocked port prevents Destroy");
        CHECK(set_intake_paused(QUEUE, false), "resume");
        waiter.join();
        CHECK(rc == 0 && job_ok(job), "Sync completes after real output");
        CHECK(get32(QUEUE + 0x88) == 0x80000000u, "only id0 remains allocated");
        CHECK(resv_stcx32(&reservation, QUEUE + 0x98, 0) == 0, "release invalidates neighboring reservation");
        CHECK(bitmap_atomic(spu.get(), MFC_PUTLLC_CMD) != 0, "release kills SPU reservation");
        CHECK(get32(PORT + 4) == 1 && (port_a(PORT) & 0x80000000u) == 0, "stale id retained after wake");
        CHECK(jq2_port_sync(PORT, 4) == 0 && get32(PORT + 4) == 1, "poll preserves stale id");
        for (unsigned w = 1; w < 4; ++w) CHECK(get32(QUEUE + 0x88 + 4 * w) == 0, "unused bitmap word %u", w);
    }
    spu_coh_unregister(spu.get());
    spu_mfc_release(spu.get());
    rig.teardown();
}

/* Pretend ids0..62 are owned by other ports, using the same guest reservation
 * transaction as a guest allocator. The last wakeable bit must be usable. */
int occupy_waiters(std::uint8_t*, void* user)
{
    const unsigned words = *static_cast<unsigned*>(user);
    for (unsigned w = 0; w < words; ++w) put32(QUEUE + 0x88 + 4 * w, 0xFFFFFFFFu);
    if (words == 1) put32(QUEUE + 0x8C, 0xFFFFFFFEu);
    return 1;
}

void mode_waiter_63()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    Rig rig({128, 1, false});
    unsigned words = 1;
    resv_host_rmw(QUEUE + 0x88, &occupy_waiters, &words);
    CHECK(set_intake_paused(QUEUE, true), "pause");
    const JobSpec job = spec(0);
    CHECK(rig.push(job.desc, 64, 0) == 0, "push");
    std::int32_t rc = -1;
    std::thread waiter([&] { rc = jq2_port_sync(PORT, 0); });
    CHECK(wait_for([] { return (port_a(PORT) & 0x80000000u) != 0; }), "registered");
    CHECK(get32(PORT + 4) == 63 && get32(QUEUE + 0x8C) == 0xFFFFFFFFu, "id63 uses word1 low bit");
    CHECK(set_intake_paused(QUEUE, false), "resume");
    waiter.join();
    CHECK(rc == 0 && job_ok(job), "flag bit63 wakes after output");
    CHECK(get32(QUEUE + 0x88) == 0xFFFFFFFFu && get32(QUEUE + 0x8C) == 0xFFFFFFFEu, "only id63 released");
    CHECK(get32(PORT + 4) == 63 && jq2_port_sync(PORT, 4) == 0 && get32(PORT + 4) == 63, "id63 stays stale");
    rig.teardown();
}

/* Several PPU threads blocked in Port2Sync mode 0 at the same time, one port
 * each. Each registers its own waiter id, all stay blocked while intake is
 * paused, and each is woken by the credit of its own job. */
void mode_sync_concurrent()
{
    constexpr unsigned kPorts = 4;
    build_image(IMAGE, Prog::transform);
    install_hooks();
    Rig rig({128, 2, false});
    CHECK(set_intake_paused(QUEUE, true), "pause");

    std::vector<JobSpec> specs;
    for (unsigned i = 0; i < kPorts; ++i) {
        const std::uint32_t port = PORT + 0x80 * i;
        if (i != 0) CHECK(jq2_port_create(port, QUEUE) == 0, "port %u create", i);
        specs.push_back(spec(i));
        CHECK(jq2_port_push_job(port, specs.back().desc, 64, 0, 1) == 0, "push on port %u", i);
    }

    /* Exercise the SPU-marked line lock path and competing PPU reservations,
     * without letting unrelated neighboring stores get overwritten. */
    resv_mark_spu(QUEUE + 0x88);
    put32(QUEUE + 0x9C, 0x12345678u);
    std::atomic<bool> touching{true};
    std::atomic<unsigned> touches{0};
    std::thread toucher([&] {
        ppu_context ctx{};
        while (touching.load(std::memory_order_acquire)) {
            const auto value = resv_lwarx32(&ctx, QUEUE + 0x98);
            if (resv_stcx32(&ctx, QUEUE + 0x98, value + 1)) touches.fetch_add(1);
        }
    });
    CHECK(wait_for([&] { return touches.load() != 0; }), "neighbor conditional store runs");
    std::atomic<unsigned> spu_touches{0};
    std::thread spu_toucher([&] {
        auto ctx = std::make_unique<spu_context>();
        spu_context_init(ctx.get(), 7);
        while (touching.load(std::memory_order_acquire)) {
            bitmap_atomic(ctx.get(), MFC_GETLLAR_CMD);
            if (bitmap_atomic(ctx.get(), MFC_PUTLLC_CMD) == 0) spu_touches.fetch_add(1);
        }
        spu_coh_unregister(ctx.get());
        spu_mfc_release(ctx.get());
    });
    CHECK(wait_for([&] { return spu_touches.load() != 0; }), "SPU conditional store runs");
    std::atomic<std::int32_t> result[kPorts];
    for (auto& r : result) r = -1;
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < kPorts; ++i)
        threads.emplace_back([&result, i] { result[i] = jq2_port_sync(PORT + 0x80 * i, 0); });

    bool all_registered = true;
    for (unsigned i = 0; i < kPorts; ++i)
        all_registered = wait_for([&] { return (port_a(PORT + 0x80 * i) & 0x80000000u) != 0; }) && all_registered;
    CHECK(all_registered, "every Sync registered its wait");
    std::uint64_t seen_ids = 0;
    for (unsigned i = 0; i < kPorts; ++i) {
        const std::uint32_t port = PORT + 0x80 * i;
        const std::uint32_t id = get32(port + 0x04);
        CHECK(id >= 1 && id < 64, "port %u waiter id %u is within the firmware's 1..63", i, id);
        CHECK(((port_a(port) >> 29) & 3) == 2, "port %u waits on an event flag (kind 2)", i);
        CHECK(id >= 64 || (seen_ids & (std::uint64_t{1} << id)) == 0, "port %u waiter id %u is not shared", i, id);
        if (id < 64) seen_ids |= std::uint64_t{1} << id;
        CHECK(((get32(port + 0x14) >> 24) & 0xFF) == 1, "port %u holds the claim", i);
    }
    CHECK(get32(QUEUE + 0x88) == 0xF8000000u && get32(QUEUE + 0x8C) == 0 &&
              get32(QUEUE + 0x90) == 0 && get32(QUEUE + 0x94) == 0, "exact concurrent guest bitmap");
    for (unsigned i = 0; i < kPorts; ++i) CHECK(result[i] == -1, "port %u's Sync is still blocked", i);

    CHECK(set_intake_paused(QUEUE, false), "resume");
    for (auto& t : threads) t.join();
    touching.store(false, std::memory_order_release);
    toucher.join();
    spu_toucher.join();
    CHECK(get32(QUEUE + 0x98) == touches.load() && get32(QUEUE + 0x9C) == 0x12345678u,
          "bitmap transactions preserve racing neighbor stores and guard");
    for (unsigned i = 0; i < kPorts; ++i) {
        const std::uint32_t port = PORT + 0x80 * i;
        CHECK(result[i] == 0, "port %u's Sync returned 0x%X", i, unsigned(result[i].load()));
        CHECK(credit_of(port) == 0 && port_b(port) == 0 && ((get32(port + 0x14) >> 24) & 0xFF) == 0,
              "port %u: A=0x%X B=%u claim=0x%X", i, port_a(port), port_b(port), get32(port + 0x14));
        CHECK(job_ok(specs[i]), "job %u output", i);
    }
    CHECK(get32(QUEUE + 0x88) == 0x80000000u && get32(QUEUE + 0x8C) == 0 &&
              get32(QUEUE + 0x90) == 0 && get32(QUEUE + 0x94) == 0, "concurrent release retains only leaked0");
    const jq2_stats s = rig.stats();
    CHECK(s.dispatched == kPorts && s.executed == kPorts && s.credited == kPorts, "counts");
    for (unsigned i = 1; i < kPorts; ++i) CHECK(jq2_port_destroy(PORT + 0x80 * i) == 0, "port %u destroy", i);
    rig.teardown();
}

/* The firmware's wake helper (0x6348) rejects ids above 63, so a credit that
 * finds an event-flag waiter registered with id 64 is a named stop. The line is
 * forged: A = -1 with bit 31 and kind 2, +0x04 = 64. */
void mode_stop_waiter_id()
{
    expect_stop(
        [] {
            build_image(IMAGE, Prog::transform);
            install_hooks();
            arm_child_watchdog(5);
            Rig rig({128, 1, false});
            put32(PORT + 0x00, 0xDFFFFFFFu);
            put32(PORT + 0x04, 64);
            const JobSpec a = spec(0);
            CHECK(rig.push(a.desc, 64, 0) == 0, "push");
            std::this_thread::sleep_for(std::chrono::seconds(30));
        },
        "jq2-wake", {"reason=waiter-id", "kind=2", "id=64"});
}

/* A credit finds a kind-0 waiter whose fiber EA names no fiber. The firmware's
 * PM would write whatever guest bytes sit there; the executor signals only
 * through the fiber library and stops when it refuses (status 3: not a live
 * fiber), instead of reporting a wake that never happened. */
void mode_stop_fiber_notify()
{
    expect_stop(
        [] {
            build_image(IMAGE, Prog::transform);
            install_hooks();
            arm_child_watchdog(5);
            Rig rig({128, 1, false});
            put32(PORT + 0x00, 0x9FFFFFFFu); /* waiting, kind 0, A - B = -1 once B is 1 */
            put32(PORT + 0x04, 0x1000);      /* 128-aligned, no such fiber */
            const JobSpec a = spec(0);
            CHECK(rig.push(a.desc, 64, 0) == 0, "push");
            std::this_thread::sleep_for(std::chrono::seconds(30));
        },
        "jq2-wake", {"reason=fiber-notify", "kind=0", "fiber=0x1000", "status=3"});
}

/* Id 64 is allocated (63 concurrent blocked Syncs hold ids 1..63). Credit arrives
 * before the Sync registers, so the Sync completes without blocking, releases
 * id 64 and returns 0 without stopping. */
struct NowakeWait {
    std::uint32_t queue_ea;
    std::uint32_t port_ea;
    Waiter acquire()
    {
        unsigned id = 128;
        resv_host_rmw(queue_ea + 0x88, [](std::uint8_t* line, void* user) {
            auto& allocated = *static_cast<unsigned*>(user);
            for (unsigned w = 0; w < 4; ++w) {
                auto word = std::atomic_ref<std::uint32_t>(
                    *reinterpret_cast<std::uint32_t*>(line + 8 + 4 * w));
                const std::uint32_t bits = ps3_bswap32(word.load(std::memory_order_relaxed));
                if (bits == UINT32_MAX) continue;
                const unsigned bit = static_cast<unsigned>(__builtin_clz(~bits));
                word.store(ps3_bswap32(bits | (0x80000000u >> bit)), std::memory_order_release);
                allocated = 32 * w + bit;
                return 1;
            }
            return 0;
        }, &id);
        GuestPortLine::at(port_ea)->update([](PortWords& w) {
            w.credit_completion();
            return true;
        });
        return {WaitKind::event_flag, id};
    }
    void block(const Waiter&) {}
    void release(const Waiter& w)
    {
        struct Release { unsigned id; } release{w.id};
        resv_host_rmw(queue_ea + 0x88, [](std::uint8_t* line, void* user) {
            const unsigned id = static_cast<Release*>(user)->id;
            auto word = std::atomic_ref<std::uint32_t>(
                *reinterpret_cast<std::uint32_t*>(line + 8 + 4 * (id / 32)));
            const std::uint32_t mask = 0x80000000u >> (id % 32);
            word.fetch_and(ps3_bswap32(~mask), std::memory_order_release);
            return 1;
        }, &release);
    }
};

void mode_waiter_64_nowake()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    Rig rig({128, 1, false});
    unsigned words = 2;
    resv_host_rmw(QUEUE + 0x88, &occupy_waiters, &words);
    const JobSpec s = spec(0);
    CHECK(rig.push(s.desc, 64, 0) == 0, "push");
    NowakeWait wait{QUEUE, PORT};
    auto line = GuestPortLine::at(PORT);
    CHECK(line.has_value(), "port line");
    BasicPort port(*line);
    const PortStatus st = port.sync(0, wait);
    CHECK(st == PortStatus::ok, "Sync returned 0 without blocking (got 0x%X)", static_cast<unsigned>(st));
    CHECK(get32(QUEUE + 0x88) == 0xFFFFFFFFu && get32(QUEUE + 0x8C) == 0xFFFFFFFFu &&
              get32(QUEUE + 0x90) == 0 && get32(QUEUE + 0x94) == 0,
          "id64 bit cleared again");
    CHECK((port_a(PORT) & 0x80000000u) == 0, "waiter never registered");
    rig.teardown();
}

/* The 64th concurrent Sync registers kind 2 with id 64 and blocks. A credit then
 * triggers the stop jq2-wake reason=waiter-id id=64 because the firmware PM wake
 * helper (0x6348) rejects unreduced ids > 63. */
void mode_stop_waiter_64_wake()
{
    expect_stop(
        [] {
            build_image(IMAGE, Prog::transform);
            install_hooks();
            arm_child_watchdog(5);
            Rig rig({128, 1, false});
            CHECK(set_intake_paused(QUEUE, true), "pause");
            unsigned words = 2;
            resv_host_rmw(QUEUE + 0x88, &occupy_waiters, &words);
            const JobSpec s = spec(0);
            CHECK(jq2_port_push_job(PORT, s.desc, 64, 0, 1) == 0, "push");
            std::thread([] { jq2_port_sync(PORT, 0); }).detach();
            CHECK(wait_for([] { return (port_a(PORT) & 0x80000000u) != 0; }), "registered");
            CHECK(get32(PORT + 4) == 64, "registered id 64");
            CHECK(set_intake_paused(QUEUE, false), "resume");
            wait_for([] { return false; }, 10000);
        },
        "jq2-wake", {"reason=waiter-id", "kind=2", "id=64", "fw=\"pm-wake-rejects-id>63\""});
}

/* Under lv2 semantics, sys_event_flag_destroy returns CELL_EBUSY if threads are
 * waiting, so a flag cannot be destroyed while a Sync is blocked on it.
 * The HLE stop reason=wait is reached when the wait returns an error such as
 * CELL_ESRCH (here, the flag was destroyed before the wait began). */
void mode_stop_wait()
{
    expect_stop(
        [] {
            build_image(IMAGE, Prog::transform);
            install_hooks();
            arm_child_watchdog(5);
            Rig rig({128, 1, false});
            CHECK(set_intake_paused(QUEUE, true), "pause");
            const JobSpec a = spec(0);
            CHECK(rig.push(a.desc, 64, 0) == 0, "push");
            ppu_context ctx;
            std::memset(&ctx, 0, sizeof ctx);
            ctx.gpr[3] = get32(QUEUE + 0x80); /* the queue's first event flag */
            CHECK(sys_event_flag_destroy(&ctx) == 0, "destroy the flag before wait (0 waiters)");
            jq2_port_sync(PORT, 0); /* registers, waits on destroyed flag -> ESRCH */
            std::fprintf(stderr, "the stop did not happen: the Sync returned\n");
        },
        "jq2-wake", {"reason=wait", "rc=0x80010005"});
}

/* A push that passed its checks while the port was destroyed and the queue shut
 * down and joined. ExecutorHooks::on_push_checked parks the pusher after
 * check_push and before its port-line update, so the order is established by the
 * hook's handshake, not by timing; it is then released into a ring nobody
 * drains. Both threads are detached because the pusher's named stop ends the
 * process, so nothing can join them. */
void push_shutdown_scenario(bool copy)
{
    expect_stop(
        [copy] {
            build_image(IMAGE, Prog::transform);
            install_hooks();
            arm_child_watchdog(10);
            Rig rig({128, 1, copy});
            const std::uint32_t desc = spec(0).desc;
            g_gate.armed = true;
            std::thread([copy, desc] {
                const std::int32_t rc = copy ? jq2_port_copy_push(PORT, desc, 64, 64, 0, 1)
                                             : jq2_port_push_job(PORT, desc, 64, 0, 1);
                std::fprintf(stderr, "the stop did not happen: the push returned 0x%X\n", unsigned(rc));
                g_gate.returned = true;
            }).detach();
            CHECK(wait_for([] { return g_gate.reached.load(); }), "the push passed its checks");
            CHECK(jq2_port_destroy(PORT) == 0, "port destroy");
            CHECK(jq2_queue_shutdown(QUEUE) == 0, "shutdown");
            std::uint32_t exit_value = 0;
            CHECK(jq2_queue_join(QUEUE, &exit_value) == 0, "join: the workers are gone");
            g_gate.release = true;
            wait_for([] { return g_gate.returned.load(); }, 10000); /* the stop ends the process first */
        },
        "jq2-state", {"reason=push-after-shutdown"});
}
void mode_stop_push_shutdown() { push_shutdown_scenario(false); }
void mode_stop_copy_push_shutdown() { push_shutdown_scenario(true); }

/* The firmware accepts a descriptor size of 64 or any multiple of 128 up to the queue's
 * maxDescriptor (re_jq2.md A.3), and the ring carries it as a 3-bit class in w1 bits 3..1. On a
 * maxDescriptor 896 queue every size travels ring -> admission -> loader intact: the ring word
 * holds the class, the worker fetches exactly that many bytes, and the loader stages them. */
struct SizesSeen {
    std::mutex mu;
    std::vector<jq2_job_info> info;
    std::vector<std::vector<std::uint8_t>> fetched, staged;
};
SizesSeen g_sizes;

void sizes_start(void*, spu_context* ctx, const jq2_job_info* info)
{
    std::lock_guard<std::mutex> lk(g_sizes.mu);
    g_sizes.info.push_back(*info);
    g_sizes.fetched.emplace_back(info->descriptor, info->descriptor + info->descriptor_size);
    g_sizes.staged.emplace_back(ctx->ls + info->desc_ls, ctx->ls + info->desc_ls + info->desc_bytes);
}

void mode_desc_sizes()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    Rig rig({128, 1, false, 896});
    const jq2_job_observer obs{nullptr, &sizes_start, &state_end};
    CHECK(jq2_set_job_observer(QUEUE, &obs) == 0, "install");

    const std::uint32_t sizes[8] = {64, 128, 256, 384, 512, 640, 768, 896};
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < 8; ++j) {
        JobSpec s = spec(j);
        s.desc = DESC + 0x10000 + 0x1000 * j; /* room for 896 bytes */
        build_desc(s.desc, IMAGE, s.out, s.in, s.add, s.serial);
        for (std::uint32_t i = 0x40; i < sizes[j]; ++i) vm_base[s.desc + i] = std::uint8_t(0x30 + 7 * j + 3 * i);
        specs.push_back(s);

        /* the ring word, with the worker held off: class in w1 bits 3..1, the phase in bit 0, w0 bit 2
         * clear (the writer stores no push flag bit 1) */
        if (j == 7) {
            CHECK(set_intake_paused(QUEUE, true), "pause");
            CHECK(rig.push(s.desc, sizes[j], 1) == 0, "push %u", sizes[j]);
            const std::uint32_t w1 = get32(RING + 7 * 8 + 4);
            CHECK((w1 & ~0xFu) == s.desc && (w1 & 0xE) == 14, "ring w1 0x%08X for size 896", w1);
            CHECK((get32(RING + 7 * 8) & 7) == 0, "w0 0x%08X", get32(RING + 7 * 8));
            CHECK(set_intake_paused(QUEUE, false), "resume");
            CHECK(jq2_port_sync(PORT, 0) == 0, "sync %u", sizes[j]);
        } else {
            CHECK(rig.push(s.desc, sizes[j], j & 3) == 0, "push %u", sizes[j]);
            CHECK(jq2_port_sync(PORT, 0) == 0, "sync %u", sizes[j]);
        }
        CHECK(job_ok(s), "the %u-byte job ran on its descriptor", sizes[j]);
    }

    std::lock_guard<std::mutex> lk(g_sizes.mu);
    CHECK(g_sizes.info.size() == 8, "%zu jobs started", g_sizes.info.size());
    for (std::size_t j = 0; j < g_sizes.info.size() && j < 8; ++j) {
        CHECK(g_sizes.info[j].descriptor_size == sizes[j] && g_sizes.info[j].desc_bytes == sizes[j],
              "job %zu: descriptor %u staged %u, want %u", j, g_sizes.info[j].descriptor_size,
              g_sizes.info[j].desc_bytes, sizes[j]);
        CHECK(g_sizes.fetched[j].size() == sizes[j] &&
                  std::memcmp(g_sizes.fetched[j].data(), vm_base + specs[j].desc, sizes[j]) == 0,
              "job %zu: the admission fetch is the descriptor", j);
        CHECK(g_sizes.staged[j].size() == sizes[j] &&
                  std::memcmp(g_sizes.staged[j].data(), vm_base + specs[j].desc, sizes[j]) == 0,
              "job %zu: the loader's copy is the descriptor", j);
    }
    rig.teardown();
}

/* The firmware accepts size 0 (its test is `size == 64 || size % 128 == 0`) and the HLE models no
 * 0-byte descriptor: a named stop, not a code the firmware does not return. */
void push_size0_scenario(bool copy)
{
    expect_stop(
        [copy] {
            build_image(IMAGE, Prog::transform);
            install_hooks();
            arm_child_watchdog(10);
            Rig rig({128, 1, copy});
            const std::uint32_t desc = spec(0).desc;
            const std::int32_t rc = copy ? jq2_port_copy_push(PORT, desc, 0, 64, 0, 1)
                                         : jq2_port_push_job(PORT, desc, 0, 0, 1);
            std::fprintf(stderr, "the stop did not happen: the push returned 0x%X\n", unsigned(rc));
        },
        "jq2-unmodelled", {"size=0", "0-byte descriptor"});
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: %s <mode>\n", argv[0]);
        return 2;
    }
    guest_init();
    const std::string mode = argv[1];
    if (mode == "premature") mode_premature();
    else if (mode == "copy") mode_copy();
    else if (mode == "credit") mode_credit();
    else if (mode == "wrap") mode_wrap();
    else if (mode == "barrier") mode_barrier();
    else if (mode == "sync") mode_sync();
    else if (mode == "join") mode_join();
    else if (mode == "observer") mode_observer();
    else if (mode == "validate") mode_validate();
    else if (mode == "lifted") mode_lifted();
    else if (mode == "stop-job") mode_stop_job();
    else if (mode == "stop-halt") mode_stop_halt();
    else if (mode == "stop-crt") mode_stop_crt();
    else if (mode == "layout") mode_layout(256, 5);
    else if (mode == "layout-896") mode_layout(896, 0x11);
    else if (mode == "stop-unmodelled") mode_stop_unmodelled();
    else if (mode == "jm-wait") mode_jm_wait();
    else if (mode == "heap-fit") mode_heap_fit();
    else if (mode == "stop-jm") mode_stop_jm();
    else if (mode == "stop-jm-lifted") mode_stop_jm_lifted();
    else if (mode == "stop-heap") mode_stop_heap();
    else if (mode == "stop-ring") mode_stop_ring();
    else if (mode == "sync-concurrent") mode_sync_concurrent();
    else if (mode == "waiter-fresh") mode_waiter_fresh();
    else if (mode == "waiter-63") mode_waiter_63();
    else if (mode == "stop-waiter-id") mode_stop_waiter_id();
    else if (mode == "waiter-64-nowake") mode_waiter_64_nowake();
    else if (mode == "stop-waiter-64-wake") mode_stop_waiter_64_wake();
    else if (mode == "stop-wait") mode_stop_wait();
    else if (mode == "stop-fiber-notify") mode_stop_fiber_notify();
    else if (mode == "stop-push-shutdown") mode_stop_push_shutdown();
    else if (mode == "stop-copy-push-shutdown") mode_stop_copy_push_shutdown();
    else if (mode == "desc-sizes") mode_desc_sizes();
    else if (mode == "stop-push-size0") push_size0_scenario(false);
    else if (mode == "stop-copy-push-size0") push_size0_scenario(true);
    else {
        std::printf("unknown mode %s\n", mode.c_str());
        return 2;
    }
    if (g_fail != 0) {
        std::printf("FAIL: %d check(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS %s\n", mode.c_str());
    return 0;
}

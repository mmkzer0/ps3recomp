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
 *   end-return, end-return-lifted, end-stop, end-halt, end-jm-call  the end class of docs/SPU_XDIFF.md 7.6 that
 *               ExecutorHooks::on_end reports before the executor's policy runs: return (interpreted and
 *               lifted, with and without an observer), stop with its code, halt, jm call (interpreted and
 *               lifted); the stopping ends still stop with the same kinds and fields
 *   end-classify  spu_job_classify_end on hand-built contexts: every class and the neighbours of each rule
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
 *   stop-push-flag1, stop-copy-push-flag1  flag bit 1 (stored for the PM, not decoded) on an
 *               otherwise valid push: jq2-unmodelled, after every firmware check
 *   push-block, pushsync-block, push-block-copy  a push without flags bit 2 on a full
 *               ring parks (on_push_blocked) until intake resumes, then lands in order
 *   push-nonblock  flags bit 2 on a full ring is AGAIN: nothing written, B rolled back
 *   push-flags  PushJob flags & ~7, CopyPush flags & ~3, PushSync flags & ~4: A02, first
 *   stop-push-block-shutdown  Shutdown while a pusher is parked: jq2-unmodelled
 *   trace-spans, trace-sync, trace-queues, trace-concurrent, trace-held, trace-off  the ftrace jq2 kinds 22-28
 *               (ps3_ftrace.h): PUSH/JOB/CREDIT per (q, seq), SYNC_WAIT keyed by waiter, IDLE and its reason
 *               byte, HELD and BARRIER for a job a SYNC held, the q of each queue and its creation line, seq
 *               unique under concurrent pushers; nothing recorded with tracing off
 *   trace-image-two, -same, -changed, -moved, -overflow, -concurrent, -off, -observer  the ftrace job image
 *               identity (kind 29, the J table): two images two entries, the same image the same index, bytes
 *               changed in place a new entry, the same bytes at a second EA the same fingerprint, the 1024-slot
 *               table and its dump, four workers racing on a shared table, nothing with tracing off, the
 *               observer's loader path
 *   xdiff-capture, -lifted, -observer, -concurrent, -stop, -event, -unresolved, -both, -unknown, -body-none, -body-other,
 *   -varying, -bounded, -baddir, -writefail, -payload, -queue256, -off, -nodir
 *               fixture capture on the worker (docs/SPU_XDIFF.md commit 10a; spu.xdiff_capture = N, spu.xdiff_dir):
 *               N+1 jobs write exactly N fixtures named per 7.2 that decode, carry the job's identity, effects and
 *               end, and replay `equal` on the interpreter, and leave guest memory as the same run without capture;
 *               a body registered for the image is recorded as T1 with its inventory's lift identity, any other
 *               body records nothing; the observer's path records too; three workers and two images, held at a
 *               barrier, write exactly N per image between them; a job that stops the process leaves its fixture
 *               (end class stop); an image whose inventory lists event branches, or has none, is counted
 *               ineligible, named once and not recorded, while unresolved MFC sites are recorded and counted
 *               apart; bytes that change every job and images past the table bound are counted together, with
 *               at most 32 lines; the 257th queue is q256 in the name; a payload over the bound and a directory
 *               that cannot be written are named failures and the run goes on; a directory missing at the first
 *               queue is one line and capture off; capture off, or on without a directory, records nothing
 */
#include "jq2/executor.hpp"
#include "jq2/jq2.h"
#include "jq2/port.hpp"

#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_observe.h"
#include "spu_coherency.h"
#include "spurs_job.h"
#include "spu_workload.h"
#include "spu_interp.h"
#include "syscalls/sys_event.h"
#include "ps3emu/ps3_ftrace.h"
#include "ps3emu/stop.h"
#include "syscalls/ppu_thread_id.h"
#include "memory/reservation.h"
#include "jq2/xdiff_capture.hpp"
#include "ps3emu/lifter_abi.h"
#include "xdiff/spu_xdiff_replay.h"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
uint8_t* vm_base;
extern uint32_t ppu_vm_size; /* runtime/host/host_stubs.c */
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx; /* runtime/host/host_stubs.c */
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

/* What ExecutorHooks::on_end reported, in guest memory: the arena is MAP_SHARED, so the record of a job that
 * ended in a named stop (a forked child) is still there for the parent to read. One job per run. */
constexpr std::uint32_t END_REC = 0xE00000;
struct EndRec {
    std::uint32_t count, end_class, stop_code, pc, status;
};
/* The image id the context carried at the end of each job, by the job's serial (spu.job_body tests). */
struct EndImages {
    std::mutex mu;
    std::map<std::uint32_t, std::int32_t> id;
};
EndImages g_end_images;
void hook_end(void*, const JobStart& js, const spu_context* ctx, const spu_job_end& e)
{
    {
        std::lock_guard<std::mutex> lk(g_end_images.mu);
        g_end_images.id[serial_of(js)] = ctx->image_id;
    }
    put32(END_REC + 4, e.end_class);
    put32(END_REC + 8, e.stop_code);
    put32(END_REC + 12, e.pc);
    put32(END_REC + 16, e.status);
    put32(END_REC, get32(END_REC) + 1);
}
EndRec end_rec()
{
    return {get32(END_REC), get32(END_REC + 4), get32(END_REC + 8), get32(END_REC + 12), get32(END_REC + 16)};
}
void end_rec_clear() { std::memset(vm_base + END_REC, 0, 32); }

/* The job bodies a test selects; 0 leaves the interpreter. */
enum LiftedKind { kNoLifted = 0, kLiftedNormal, kLiftedHalt, kLiftedJmCall, kLiftedBusy };
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

/* The transform's work in the lifted style, without the end: the GET, the four
 * words and the PUT. */
void lifted_transform(spu_context* c)
{
    const std::uint32_t d = c->gpr[4]._u32[0];
    const std::uint32_t out = ls_word(c, d + 0x30), in = ls_word(c, d + 0x34);
    const std::uint32_t k[4] = {ls_word(c, d + 0x38), ls_word(c, d + 0x3C), out, in};
    lifted_dma(c, kLsIn, in, MFC_GET_CMD);
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint32_t v = be32(ls_word(c, kLsIn + 4 * i) + k[i]);
        std::memcpy(c->ls + kLsOut + 4 * i, &v, 4);
    }
    lifted_dma(c, kLsOut, out, MFC_PUT_CMD);
}

void lifted_job(spu_context* c, bool halt)
{
    if (halt) {
        /* the halt lands between the GET and the output store */
        lifted_dma(c, kLsIn, ls_word(c, c->gpr[4]._u32[0] + 0x34), MFC_GET_CMD);
        c->status = SPU_STATUS_STOPPED_BY_HALT;
        spu_halt(c);
        return;
    }
    lifted_transform(c);
    SPU_RET(c);
}
void lifted_normal_entry(spu_context* c) { lifted_job(c, false); }
void lifted_halt_entry(spu_context* c) { lifted_job(c, true); }

/* The transform, then 2 ms of this thread's own CPU time: a job whose CPU time the JOB span must report. */
void lifted_busy_entry(spu_context* c)
{
    lifted_job(c, false);
    timespec t0;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
    for (;;) {
        timespec t;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
        if ((t.tv_sec - t0.tv_sec) * 1000000000ll + (t.tv_nsec - t0.tv_nsec) >= 2000000) break;
    }
}

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

/* A registered body that calls through r84 into two words it plants in local
 * store, where no lift holds them: the production resolver's microstep executes
 * the `il` and the `bi` and counts the arrival. The branch links through r84, not
 * r0, so the job's own return link stays what the loader planted. */
constexpr std::uint32_t kMissStub = 0x2000; /* between the jm areas, below the taskset and kernel ranges, outside the image */
constexpr std::uint32_t kMissLink = 0x2F0;  /* no lift holds it; the drain bracket compares it and returns to this host frame */
constexpr std::uint32_t kMissMark = 0x5A5A; /* positive, so `il`'s sign extension leaves this value in the word */
void lifted_miss_entry(spu_context* c)
{
    lifted_transform(c);
    const std::uint32_t stub[2] = {be32(e_il(10, kMissMark)), be32((0x1A8u << 21) | (84u << 7))}; /* il $r10 ; bi $r84 */
    std::memcpy(c->ls + kMissStub, stub, sizeof stub);
    c->gpr[84] = spu_link(kMissLink);
    c->pc = kMissStub;
    c->host_depth++;
    spu_indirect_branch(c);
    spu_drain_call(c, kMissLink);
    c->host_depth--;
    if (c->gpr[10]._u32[0] != kMissMark) {
        /* The stub did not run as interpreted code. End the job on a stop, not a
         * return, so the run fails loudly instead of reporting a tier. */
        c->status = SPU_STATUS_STOPPED_BY_STOP;
        c->stop_code = 0xBAD0u;
        spu_halt(c);
        return;
    }
    SPU_RET(c);
}

JobBody hook_select(void*, const JobStart&)
{
    switch (g_lifted) {
    case kLiftedHalt: return JobBody{&lifted_halt_entry, -1};
    case kLiftedBusy: return JobBody{&lifted_busy_entry, -1};
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

/* A push parked on a full ring (ExecutorHooks::on_push_blocked). */
struct PushParked {
    std::atomic<unsigned> blocked{0};
};
PushParked g_parked;
void hook_push_blocked(void*, std::uint32_t) { g_parked.blocked.fetch_add(1); }

void install_hooks()
{
    ExecutorHooks h;
    h.on_admit = &hook_admit;
    h.on_push_checked = &hook_push_checked;
    h.on_push_blocked = &hook_push_blocked;
    h.on_done = &hook_done;
    h.on_end = &hook_end;
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
    /* Push; a full ring is waited out (flags without bit 2). */
    void push_wait(std::uint32_t desc, std::uint32_t size, std::uint32_t tag, std::uint32_t flags = 1)
    {
        CHECK(push(desc, size, tag, flags) == 0, "push");
    }
    void sync_tags_wait(std::uint32_t mask) { CHECK(jq2_port_push_sync(port, mask, 0) == 0, "push sync"); }
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

/* ---- the end class (docs/SPU_XDIFF.md 7.6) -------------------------------------- */

/* One job runs to a return; the record ExecutorHooks::on_end left is the class and the context fields the tier
 * left. An interpreted job returns by executing the `stop 0` at the link (status STOP, pc link + 4); a lifted
 * one is halted by the resolver on the branch to the link (pc the link, neither STOP nor HALT). Both are the
 * one class `return`. A second job runs with an observer installed: the loader's observed path reports too. */
void end_return_scenario(int lifted)
{
    g_lifted = lifted;
    build_image(IMAGE, Prog::transform);
    install_hooks();
    Rig rig({128, 1, false});
    end_rec_clear();
    const JobSpec a = spec(0);
    rig.push(a.desc, 64, 0);
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    CHECK(job_ok(a), "the job's output");
    EndRec r = end_rec();
    CHECK(r.count == 1, "%u end reports, want 1", r.count);
    CHECK(r.end_class == 1, "class %u, want 1 (return)", r.end_class);
    CHECK(r.stop_code == 0, "stop code 0x%X", r.stop_code);
    if (lifted == kNoLifted) {
        CHECK(r.pc == 0x3FF04 && r.status == SPU_STATUS_STOPPED_BY_STOP, "interpreted end pc 0x%X status 0x%X",
              r.pc, r.status);
    } else {
        CHECK(r.pc == 0x3FF00 && (r.status & (SPU_STATUS_STOPPED_BY_STOP | SPU_STATUS_STOPPED_BY_HALT)) == 0,
              "lifted end pc 0x%X status 0x%X", r.pc, r.status);
    }

    const jq2_job_observer obs{nullptr, nullptr, nullptr};
    CHECK(jq2_set_job_observer(QUEUE, &obs) == 0, "observer");
    end_rec_clear();
    const JobSpec b = spec(1);
    rig.push(b.desc, 64, 0);
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync observed");
    CHECK(job_ok(b), "the observed job's output");
    r = end_rec();
    CHECK(r.count == 1 && r.end_class == 1, "observed: %u reports, class %u", r.count, r.end_class);
    jq2_stats s{};
    CHECK(jq2_queue_stats(QUEUE, &s) == 0, "stats");
    CHECK(s.executed == 2 && s.credited == 2, "counts %llu/%llu", (unsigned long long)s.executed,
          (unsigned long long)s.credited);
    rig.teardown();
}
void mode_end_return() { end_return_scenario(kNoLifted); }
void mode_end_return_lifted() { end_return_scenario(kLiftedNormal); }

/* A job that ends otherwise is a named stop as before, and the class was reported first: the record is in the
 * shared arena when the child dies. */
EndRec end_after_stop(Prog prog, const char* kind, const char* needle)
{
    end_rec_clear();
    stop_scenario(prog, kind, needle);
    return end_rec();
}

void mode_end_stop()
{
    const EndRec r = end_after_stop(Prog::stop_early, "jq2-job-end", "stop_code=0x2222");
    CHECK(r.count == 1, "%u end reports, want 1", r.count);
    CHECK(r.end_class == 2, "class %u, want 2 (stop)", r.end_class);
    CHECK(r.stop_code == 0x2222, "stop code 0x%X", r.stop_code);
    CHECK(r.status == SPU_STATUS_STOPPED_BY_STOP, "status 0x%X", r.status);
}

void mode_end_halt()
{
    g_lifted = kLiftedHalt;
    const EndRec r = end_after_stop(Prog::transform, "jq2-job-end", "reason=halt");
    CHECK(r.count == 1, "%u end reports, want 1", r.count);
    CHECK(r.end_class == 3, "class %u, want 3 (halt)", r.end_class);
    CHECK(r.status == SPU_STATUS_STOPPED_BY_HALT, "status 0x%X", r.status);
}

void mode_end_jm_call()
{
    for (const int lifted : {int(kNoLifted), int(kLiftedJmCall)}) {
        g_lifted = lifted;
        const EndRec r = end_after_stop(Prog::jm_call, "jq2-jm-call", "request=0x1234");
        CHECK(r.count == 1, "lifted=%d: %u end reports, want 1", lifted, r.count);
        CHECK(r.end_class == 4, "lifted=%d: class %u, want 4 (jm-call)", lifted, r.end_class);
        CHECK(r.stop_code == 0x3B0F && r.pc == 0x1504, "lifted=%d: stop code 0x%X pc 0x%X", lifted, r.stop_code, r.pc);
    }
}

/* The classifier alone, on contexts built by hand from the contract of section 7.6: every class, and the
 * neighbours of each rule that must not change class (a stop that is not at the link or the jm word, a halt at
 * the link, both status bits, the pc's local-store mask). It reads the context and nothing else. */
void mode_end_classify()
{
    constexpr std::uint32_t STOP = SPU_STATUS_STOPPED_BY_STOP, HALT = SPU_STATUS_STOPPED_BY_HALT;
    struct Case {
        const char* what;
        std::uint32_t status, code, pc, want;
    };
    const Case cases[] = {
        {"interpreted return", STOP, 0, 0x3FF04, 1},
        {"lifted return", 0, 0, 0x3FF00, 1},
        {"lifted return, pc beyond the local-store mask", 0, 0x77, 0x3FF00 | 0x40000, 1},
        {"stop 0 elsewhere", STOP, 0, 0x7C10, 2},
        {"stop 0 at the link itself", STOP, 0, 0x3FF00, 2},
        {"stop with a code at the link", STOP, 0x2222, 0x3FF04, 2},
        {"jm call", STOP, 0x3B0F, 0x1504, 4},
        {"the jm code elsewhere", STOP, 0x3B0F, 0x1508, 2},
        {"another code at the jm word", STOP, 0x3B0E, 0x1504, 2},
        {"halt", HALT, 0, 0x7C20, 3},
        {"halt at the link", HALT, 0, 0x3FF00, 3},
        {"halt just past the link", HALT, 0, 0x3FF04, 3},
        {"run ended elsewhere", 0, 0, 0x7C20, 5},
        {"stop and halt both set", STOP | HALT, 0, 0x3FF00, 5},
    };
    for (const Case& c : cases) {
        const std::unique_ptr<spu_context> ctx(new spu_context{});
        ctx->status = c.status;
        ctx->stop_code = c.code;
        ctx->pc = c.pc;
        const spu_job_end e = spu_job_classify_end(ctx.get());
        CHECK(e.end_class == c.want, "%s: class %u, want %u", c.what, e.end_class, c.want);
        CHECK(e.stop_code == c.code && e.status == c.status && e.pc == (c.pc & SPU_LS_MASK),
              "%s: record code 0x%X status 0x%X pc 0x%X", c.what, e.stop_code, e.status, e.pc);
    }
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
    /* jobType 0x04 (bit 2: the ELF flag every job image examined carries) alone is fine; three cache entries are not */
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

void push_flag1_scenario(bool copy)
{
    expect_stop(
        [copy] {
            build_image(IMAGE, Prog::transform);
            install_hooks();
            arm_child_watchdog(10);
            Rig rig({128, 1, copy});
            const std::uint32_t desc = spec(0).desc;
            const std::int32_t rc = copy ? jq2_port_copy_push(PORT, desc, 64, 64, 0, 3)
                                         : jq2_port_push_job(PORT, desc, 64, 0, 3);
            std::fprintf(stderr, "the stop did not happen: the push returned 0x%X\n", unsigned(rc));
        },
        "jq2-unmodelled", {"flags=0x3", "flag bit 1"});
}

/* ---- a full ring --------------------------------------------------------------- */

/* The firmware's ring writer waits on a full ring unless the push's flags have bit 2 (PushJob and
 * PushSync) and returns AGAIN only then (re_jq2.md A.2; 0x02001138). The ring is kept full by
 * pausing intake; ExecutorHooks::on_push_blocked says when a pusher is parked, so no test sleeps to
 * order anything. */
struct RingSnap {
    std::uint8_t entries[32];
    std::uint64_t index_word;
    bool operator==(const RingSnap& o) const
    {
        return std::memcmp(entries, o.entries, sizeof entries) == 0 && index_word == o.index_word;
    }
};
RingSnap ring_snap()
{
    RingSnap s{};
    std::memcpy(s.entries, vm_base + RING, sizeof s.entries);
    std::memcpy(&s.index_word, vm_base + QUEUE, 8);
    return s;
}

/* A depth-4 queue, one worker, intake paused, the ring full of jobs 0..3 pushed with `flags`. */
struct FullRing {
    Rig rig;
    std::vector<JobSpec> specs;
    explicit FullRing(bool pool = false, std::uint32_t flags = 1) : rig({4, 1, pool})
    {
        CHECK(set_intake_paused(QUEUE, true), "pause");
        for (std::uint32_t j = 0; j < 4; ++j) {
            specs.push_back(spec(j));
            CHECK(rig.push(specs.back().desc, 64, 0, flags) == 0, "fill %u", j);
        }
        const auto view = observe_queue(QUEUE);
        CHECK(view && view->outstanding == 4, "the ring is full");
    }
};

/* A push on a thread of its own. `resumed` is set before the queue resumes, so a push that returned
 * without waiting for the consumer sees it clear. */
struct Pusher {
    std::atomic<bool> done{false}, resumed{false}, resumed_at_return{false};
    std::atomic<std::int32_t> rc{-1};
    std::thread t;
    explicit Pusher(std::function<std::int32_t()> f)
        : t([this, f] {
              rc = f();
              resumed_at_return = resumed.load();
              done = true;
          })
    {
    }
    /* True once the push is parked on the ring; false if it returned instead. */
    bool parked()
    {
        wait_for([&] { return g_parked.blocked.load() >= 1 || done.load(); });
        return g_parked.blocked.load() >= 1 && !done.load();
    }
    void release()
    {
        resumed = true;
        CHECK(set_intake_paused(QUEUE, false), "resume");
        t.join();
    }
};

bool admitted_before(std::uint32_t first, std::uint32_t second)
{
    std::lock_guard<std::mutex> lk(g_probe.mu);
    return g_probe.admit.count(first) != 0 && g_probe.admit.count(second) != 0 &&
           g_probe.admit[first] < g_probe.admit[second];
}

/* (a) A job push with flags 1 on a full ring waits, B already counts it, and the entry lands after
 * the four in front of it once the worker has consumed one. */
void mode_push_block()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    FullRing f;
    const JobSpec e = spec(4);
    CHECK(port_b() == 4, "B %u", port_b());
    Pusher p([&] { return f.rig.push(e.desc, 64, 0, 1); });
    CHECK(p.parked(), "the push parks on the full ring (the firmware returns AGAIN only with flags bit 2)");
    CHECK(g_parked.blocked.load() == 1, "parked once");
    CHECK(port_b() == 5, "B is taken before the ring write: %u", port_b());
    CHECK(f.rig.stats().pushed == 4, "nothing was written");
    p.release();
    CHECK(p.rc.load() == 0, "rc 0x%X", unsigned(p.rc.load()));
    CHECK(p.resumed_at_return.load(), "the push returned only after the consumer resumed");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    for (const JobSpec& s : f.specs) CHECK(job_ok(s), "job %u", s.serial);
    CHECK(job_ok(e), "the pushed job ran");
    CHECK(admitted_before(3, 4), "the entry landed behind the four in front of it");
    CHECK(f.rig.stats().pushed == 5 && port_b() == 0, "pushed %llu B %u", (unsigned long long)f.rig.stats().pushed,
          port_b());
    f.rig.teardown();
}

/* (b) With flags bit 2 a full ring is AGAIN, nothing is written and B is as it was; a non-blocking
 * push with room writes. */
void mode_push_nonblock()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    FullRing f(false, 5); /* the ring is filled by non-blocking, accounted pushes */
    const JobSpec e = spec(4);
    const RingSnap before = ring_snap();
    CHECK(f.rig.push(e.desc, 64, 0, 5) == JQ2_ERR_AGAIN, "flags 5");
    CHECK(f.rig.push(e.desc, 64, 0, 4) == JQ2_ERR_AGAIN, "flags 4 (not accounted)");
    CHECK(jq2_port_push_sync(PORT, 1, 4) == JQ2_ERR_AGAIN, "PushSync flags 4");
    CHECK(port_b() == 4, "the accounted push is rolled back: B %u", port_b());
    CHECK(ring_snap() == before, "nothing was written to the ring");
    CHECK(f.rig.stats().pushed == 4 && g_parked.blocked.load() == 0, "nothing parked");
    CHECK(set_intake_paused(QUEUE, false), "resume");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    for (const JobSpec& s : f.specs) CHECK(job_ok(s), "job %u", s.serial);
    CHECK(f.rig.push(e.desc, 64, 0, 5) == 0, "a non-blocking push with room writes");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    CHECK(job_ok(e), "the job ran");
    f.rig.teardown();
}

/* (c) PushSync: flags 4 on a full ring is AGAIN and writes nothing; flags 0 waits and the SYNC lands. */
void mode_pushsync_block()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    FullRing f;
    const RingSnap before = ring_snap();
    CHECK(jq2_port_push_sync(PORT, 1, 4) == JQ2_ERR_AGAIN, "flags 4");
    CHECK(ring_snap() == before && f.rig.stats().pushed == 4, "nothing was written");
    Pusher p([] { return jq2_port_push_sync(PORT, 1, 0); });
    CHECK(p.parked(), "PushSync flags 0 parks on the full ring");
    CHECK(f.rig.stats().pushed == 4, "still nothing written");
    p.release();
    CHECK(p.rc.load() == 0 && p.resumed_at_return.load(), "rc 0x%X", unsigned(p.rc.load()));
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    CHECK(wait_for([] {
        const auto v = observe_queue(QUEUE);
        return v && v->outstanding == 0;
    }), "the SYNC was consumed");
    CHECK(get16(QUEUE + 2) == 1 && get16(QUEUE + 0) == 1, "write %u read %u: the SYNC took the fifth slot",
          get16(QUEUE + 2), get16(QUEUE + 0));
    CHECK(f.rig.stats().pushed == 5, "pushed");
    f.rig.teardown();
}

/* A CopyPush has no non-blocking mode: on a full ring it waits (holding its pool slot) like a job. */
void mode_push_block_copy()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    FullRing f(true);
    const JobSpec e = spec(4);
    Pusher p([&] { return jq2_port_copy_push(PORT, e.desc, 64, 64, 0, 1); });
    CHECK(p.parked(), "the CopyPush parks on the full ring");
    build_desc(e.desc, IMAGE, e.out, e.in, 999, 999); /* the source is free to reuse: the pool holds a snapshot */
    p.release();
    CHECK(p.rc.load() == 0 && p.resumed_at_return.load(), "rc 0x%X", unsigned(p.rc.load()));
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    CHECK(job_ok(e), "the snapshot ran");
    CHECK(admitted_before(3, 4), "in order");
    f.rig.teardown();
}

/* (d) The flag masks are the first test of each API (FW 4.93): PushJob flags & ~7, CopyPush flags &
 * ~3, PushSync flags & ~4, before the port is looked at. Nothing is touched by a refusal. */
void mode_push_flags()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    Rig rig({4, 1, false});
    CHECK(set_intake_paused(QUEUE, true), "pause");
    const JobSpec a = spec(0), b = spec(1);
    const RingSnap before = ring_snap();
    for (const std::uint32_t flags : {8u, 0x10u, 0x80000000u, 0xFFFFFFF8u}) {
        CHECK(jq2_port_push_job(PORT, a.desc, 64, 0, flags) == JQ2_ERR_INVAL, "PushJob flags 0x%X", unsigned(flags));
        CHECK(jq2_port_push_job(0, 0, 64, 0, flags) == JQ2_ERR_INVAL, "PushJob flags 0x%X before the port", unsigned(flags));
    }
    for (const std::uint32_t flags : {1u, 2u, 3u, 5u, 6u, 7u, 8u, 0x80000000u}) {
        CHECK(jq2_port_push_sync(PORT, 1, flags) == JQ2_ERR_INVAL, "PushSync flags 0x%X", unsigned(flags));
        CHECK(jq2_port_push_sync(0, 1, flags) == JQ2_ERR_INVAL, "PushSync flags 0x%X before the port", unsigned(flags));
    }
    for (const std::uint32_t flags : {4u, 5u, 8u, 0x80000000u}) {
        CHECK(jq2_port_copy_push(PORT, a.desc, 64, 64, 0, flags) == JQ2_ERR_INVAL, "CopyPush flags 0x%X", unsigned(flags));
        CHECK(jq2_port_copy_push(0, 0, 64, 64, 0, flags) == JQ2_ERR_INVAL, "CopyPush flags 0x%X before the port", unsigned(flags));
    }
    /* the mask is exactly ~3: flags 3 passes it (a queue with no pool then has no class) */
    CHECK(jq2_port_copy_push(PORT, a.desc, 64, 64, 0, 3) == JQ2_ERR_NULL_POINTER, "CopyPush flags 3");
    CHECK(ring_snap() == before && port_b() == 0 && rig.stats().pushed == 0 && g_parked.blocked.load() == 0,
          "a refused push touches nothing");

    /* the legal combinations without bit 1 are accepted (bit 1 stops: stop-push-flag1) */
    CHECK(jq2_port_push_job(PORT, a.desc, 64, 0, 0) == 0, "PushJob flags 0");
    CHECK(jq2_port_push_job(PORT, b.desc, 64, 0, 4) == 0, "PushJob flags 4 with room");
    CHECK(jq2_port_push_sync(PORT, 1, 4) == 0, "PushSync flags 4 with room");
    CHECK(get32(RING + 0) % 8 == 0 && get32(RING + 8) % 8 == 0, "w0 bit 2 stays clear: 0x%08X 0x%08X",
          get32(RING + 0), get32(RING + 8));
    CHECK(port_b() == 0, "no flags bit 0: nothing accounted");
    CHECK(set_intake_paused(QUEUE, false), "resume");
    CHECK(wait_for([&] { return rig.stats().executed == 2; }), "both jobs ran");
    CHECK(job_ok(a) && job_ok(b), "outputs");
    rig.teardown();
}

/* (f) A pusher parked on a full ring when the queue shuts down: what the firmware does is not
 * decoded, so it is a named stop. Only an unaccounted push can be parked at Shutdown: an accounted
 * one holds B, which keeps its port (and so Shutdown) from succeeding. The pusher is detached
 * because its stop ends the process. */
void mode_stop_push_block_shutdown()
{
    expect_stop(
        [] {
            build_image(IMAGE, Prog::transform);
            install_hooks();
            arm_child_watchdog(10);
            FullRing f(false, 0);
            const JobSpec e = spec(4);
            std::thread([e] {
                const std::int32_t rc = jq2_port_push_job(PORT, e.desc, 64, 0, 0);
                std::fprintf(stderr, "the stop did not happen: the push returned 0x%X\n", unsigned(rc));
            }).detach();
            CHECK(wait_for([] { return g_parked.blocked.load() >= 1; }), "the push parks");
            CHECK(jq2_port_destroy(PORT) == 0, "port destroy");
            CHECK(jq2_queue_shutdown(QUEUE) == 0, "shutdown");
            wait_for([] { return false; }, 10000); /* the stop ends the process first */
        },
        "jq2-unmodelled", {"reason=push-blocked-at-shutdown"});
}

/* ---- jq2 ftrace spans (ps3_ftrace.h kinds 22-28) ---------------------------- */

/* The recorder is read after every thread that can emit has been joined (Rig::teardown joins the
 * workers; a test joins its own threads), so the records need no synchronisation. */
std::vector<ps3_ft_rec> ft_kind(int kind)
{
    std::vector<ps3_ft_rec> out;
    const std::uint32_t n = std::min(g_ps3_ft.n, g_ps3_ft.cap);
    for (std::uint32_t i = 0; i < n; ++i)
        if (g_ps3_ft.buf[i].kind == static_cast<std::uint32_t>(kind)) out.push_back(g_ps3_ft.buf[i]);
    return out;
}

/* The queue a span belongs to. */
std::uint32_t ft_q(const ps3_ft_rec& r)
{
    switch (r.kind) {
    case PS3_FT_JQ2_PUSH: return r.b & 0xFFu;
    case PS3_FT_JQ2_SYNC_WAIT: return r.b;
    default: return r.who >> 8;
    }
}

using SeqMap = std::map<std::uint32_t, std::vector<ps3_ft_rec>>;
SeqMap by_seq(int kind, std::uint32_t q)
{
    SeqMap m;
    for (const ps3_ft_rec& r : ft_kind(kind))
        if (ft_q(r) == q) m[r.a].push_back(r);
    return m;
}

/* Every job pushed on queue `q` (n of them) has exactly one PUSH, JOB and CREDIT under one (q, seq), seq
 * runs 1..n, and the three spans follow each other in time. With `paused_pushes` every push finished before
 * any worker could admit, so the queue latency is not negative either. */
void check_job_spans(std::uint32_t q, std::uint32_t n, bool paused_pushes, std::uint32_t workers)
{
    const SeqMap push = by_seq(PS3_FT_JQ2_PUSH, q), job = by_seq(PS3_FT_JQ2_JOB, q),
                 credit = by_seq(PS3_FT_JQ2_CREDIT, q);
    CHECK(push.size() == n && job.size() == n && credit.size() == n, "q%u: PUSH %zu JOB %zu CREDIT %zu, want %u each",
          q, push.size(), job.size(), credit.size(), n);
    for (std::uint32_t seq = 1; seq <= n; ++seq) {
        const auto p = push.find(seq), j = job.find(seq), c = credit.find(seq);
        const bool one = p != push.end() && p->second.size() == 1 && j != job.end() && j->second.size() == 1 &&
                         c != credit.end() && c->second.size() == 1;
        CHECK(one, "q%u seq %u: not exactly one PUSH, JOB and CREDIT", q, seq);
        if (!one) continue;
        const ps3_ft_rec &pr = p->second[0], &jr = j->second[0], &cr = c->second[0];
        CHECK(pr.t1 >= pr.t0 && jr.t1 >= jr.t0 && cr.t1 >= cr.t0, "q%u seq %u: a span ends before it starts", q, seq);
        if (paused_pushes) CHECK(jr.t0 >= pr.t1, "q%u seq %u: JOB starts before PUSH ends", q, seq);
        CHECK(cr.t0 >= jr.t1, "q%u seq %u: CREDIT starts before JOB ends", q, seq);
        CHECK((jr.who & 0xFFu) < workers && cr.who == jr.who, "q%u seq %u: worker JOB %u CREDIT %u", q, seq, jr.who,
              cr.who);
    }
}

/* A thread's pushes in time order carry increasing seq: the number is assigned under the queue lock at the push.
 * Time order is the buffer order of the thread's own records (each claims its slot after the previous one), not t0:
 * macOS CLOCK_MONOTONIC ticks in microseconds, so back-to-back pushes share a t0 and a sort by it is a tie. */
void check_push_order(std::uint32_t tid, std::uint32_t q, std::uint32_t n)
{
    std::vector<ps3_ft_rec> mine;
    for (const ps3_ft_rec& r : ft_kind(PS3_FT_JQ2_PUSH))
        if (r.who == tid && ft_q(r) == q) mine.push_back(r);
    std::stable_sort(mine.begin(), mine.end(), [](const ps3_ft_rec& a, const ps3_ft_rec& b) { return a.t0 < b.t0; });
    CHECK(mine.size() == n, "tid %u: %zu PUSH spans, want %u", tid, mine.size(), n);
    for (std::size_t i = 1; i < mine.size(); ++i)
        CHECK(mine[i].a > mine[i - 1].a, "tid %u: seq %u after %u", tid, mine[i].a, mine[i - 1].a);
}

/* A CREDIT's wake fields agree with each other. */
void check_credit_fields(const ps3_ft_rec& r)
{
    const std::uint32_t waiter = r.b & 0xFFu, wake = (r.b >> 8) & 0xFFu, due = (r.b >> 16) & 0xFFu;
    if (due == 0) CHECK(wake == 0 && waiter == 0xFF, "q%u seq %u: no wake but b=0x%X", ft_q(r), r.a, r.b);
    else CHECK(wake == 1 && waiter >= 1 && waiter < 64, "q%u seq %u: due but b=0x%X", ft_q(r), r.a, r.b);
}

/* Six jobs pushed while intake is paused, then released: ordered PUSH -> JOB -> CREDIT per (q, seq), the
 * depth each push saw, the thread CPU time of the job against its wall time, and the idle spans of workers
 * that had parked with work pending. */
void mode_trace_spans()
{
    constexpr std::uint32_t kJobs = 6;
    g_lifted = kLiftedBusy;
    build_image(IMAGE, Prog::transform);
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    ppu_context ppu{};
    ppu.thread_id = ppu_tid_make(0, 0);
    g_active_ctx = &ppu;

    CHECK(PS3_FT_ON(), "tracing is on"); /* before any worker: a hook sees "off" while another thread initialises */
    Rig rig({128, 2, false});
    CHECK(set_intake_paused(QUEUE, true), "pause");
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < kJobs; ++j) {
        specs.push_back(spec(j));
        rig.push_wait(specs.back().desc, 64, j % 3);
    }
    /* Parked on the generation of the last push: the workers wake for the first time when intake resumes. */
    CHECK(wait_for([] {
              const auto v = observe_queue(QUEUE);
              return v && v->settled == 2;
          }),
          "both workers park with the jobs pending");
    CHECK(set_intake_paused(QUEUE, false), "resume");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    rig.teardown();
    g_active_ctx = nullptr;

    check_job_spans(0, kJobs, true, 2);
    check_push_order(ppu_tid_diag_index(ppu.thread_id), 0, kJobs);
    for (const ps3_ft_rec& r : ft_kind(PS3_FT_JQ2_PUSH)) {
        CHECK(r.who == ppu_tid_diag_index(ppu.thread_id), "PUSH tid %u", r.who);
        CHECK((r.b >> 8) == r.a, "PUSH seq %u saw depth %u; nothing was admitted yet", r.a, r.b >> 8);
    }
    for (const ps3_ft_rec& r : ft_kind(PS3_FT_JQ2_JOB)) {
        const std::uint64_t wall_us = (r.t1 - r.t0) / 1000;
        CHECK(r.b >= 2000 && r.b <= wall_us + 2000, "seq %u: CPU %u us over a wall of %llu us (the body spins 2 ms)", r.a,
              r.b, (unsigned long long)wall_us);
    }
    for (const ps3_ft_rec& r : ft_kind(PS3_FT_JQ2_CREDIT)) check_credit_fields(r);

    const std::vector<ps3_ft_rec> idle = ft_kind(PS3_FT_JQ2_IDLE);
    for (std::uint32_t w = 0; w < 2; ++w) {
        bool parked_with_work = false;
        for (const ps3_ft_rec& r : idle) {
            CHECK(r.who >> 8 == 0 && (r.who & 0xFFu) < 2 && r.t1 >= r.t0, "IDLE who 0x%X", r.who);
            if (r.who == w) parked_with_work = parked_with_work || ((r.a & 0xFFFFFFu) == kJobs && r.b <= kJobs);
        }
        CHECK(parked_with_work, "worker %u: no IDLE span that began with %u jobs pending", w, kJobs);
    }
    CHECK(ft_kind(PS3_FT_JQ2_SYNC_WAIT).size() <= 1, "at most the one blocking Sync");
}

/* A blocking Port2Sync emits one SYNC_WAIT, keyed by the waiter id the port line carries, and the CREDIT
 * that woke it names the same waiter. A Sync that does not block emits none. */
void mode_trace_sync()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    CHECK(PS3_FT_ON(), "tracing is on"); /* before any worker: a hook sees "off" while another thread initialises */
    Rig rig({128, 1, false});
    CHECK(set_intake_paused(QUEUE, true), "pause");
    const JobSpec a = spec(0);
    CHECK(rig.push(a.desc, 64, 0) == 0, "push");

    ppu_context waiter_ppu{};
    waiter_ppu.thread_id = ppu_tid_make(1, 2);
    std::atomic<std::int32_t> rc{-1};
    std::thread t([&] {
        g_active_ctx = &waiter_ppu;
        rc = jq2_port_sync(PORT, 0);
        g_active_ctx = nullptr;
    });
    CHECK(wait_for([] { return (port_a() & 0x80000000u) != 0; }), "the Sync registered its wait");
    const std::uint32_t id = get32(PORT + 4);
    CHECK(id >= 1 && id < 64, "waiter id %u", id);
    CHECK(set_intake_paused(QUEUE, false), "resume");
    t.join();
    CHECK(rc == 0, "sync rc 0x%X", unsigned(rc.load()));

    CHECK(jq2_port_sync(PORT, 0) == 0, "a Sync with nothing pending completes without blocking");
    CHECK(jq2_port_sync(PORT, 4) == 0, "a poll completes");
    rig.teardown();

    const std::vector<ps3_ft_rec> waits = ft_kind(PS3_FT_JQ2_SYNC_WAIT);
    CHECK(waits.size() == 1, "%zu SYNC_WAIT spans, want 1 (the Syncs that did not block emit none)", waits.size());
    std::vector<ps3_ft_rec> due;
    for (const ps3_ft_rec& r : ft_kind(PS3_FT_JQ2_CREDIT)) {
        check_credit_fields(r);
        if ((r.b >> 16) & 0xFFu) due.push_back(r);
    }
    CHECK(due.size() == 1, "%zu due CREDIT spans, want 1", due.size());
    if (waits.size() == 1 && due.size() == 1) {
        const ps3_ft_rec &w = waits[0], &c = due[0];
        CHECK(w.who == ppu_tid_diag_index(waiter_ppu.thread_id), "SYNC_WAIT tid %u", w.who);
        CHECK(w.a == (id | 1u << 8 | 0u << 16), "SYNC_WAIT waiter/wake/mode 0x%X, want waiter %u event flag mode 0", w.a, id);
        CHECK(w.b == 0, "SYNC_WAIT queue %u", w.b);
        CHECK((c.b & 0xFFu) == id && ((c.b >> 8) & 0xFFu) == 1, "the due CREDIT names waiter %u (b=0x%X)", id, c.b);
        /* The waiter resumes inside or just after the credit that woke it; it cannot resume before the credit began. */
        CHECK(w.t1 >= c.t0 && w.t1 >= w.t0, "the wait ended at %llu before the CREDIT began at %llu",
              (unsigned long long)w.t1, (unsigned long long)c.t0);
    }
}

/* stderr of `fn`, for the one-line diagnostics the executor prints. */
std::string capture_stderr(const std::function<void()>& fn)
{
    std::fflush(stderr);
    const int saved = dup(2);
    std::FILE* tmp = std::tmpfile();
    CHECK(saved >= 0 && tmp != nullptr, "capture stderr");
    dup2(fileno(tmp), 2);
    fn();
    std::fflush(stderr);
    dup2(saved, 2);
    close(saved);
    std::string text;
    std::rewind(tmp);
    char buf[512];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, tmp)) != 0;) text.append(buf, n);
    std::fclose(tmp);
    return text;
}

std::size_t count_of(const std::string& hay, const std::string& needle)
{
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}

/* Queues get q in creation order, each with its own seq from 1; the creation line names q, EA, workers and
 * depth; the 257th queue keeps 255 and says so. */
void mode_trace_queues()
{
    constexpr std::uint32_t QUEUE2 = 0x80000, RING2 = 0x90000, PORT2 = 0x74000;
    build_image(IMAGE, Prog::transform);
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }

    std::unique_ptr<Rig> rig;
    CHECK(PS3_FT_ON(), "tracing is on"); /* before any worker: a hook sees "off" while another thread initialises */
    std::string log = capture_stderr([&] { rig = std::make_unique<Rig>(Opts{128, 3, false}); });
    log += capture_stderr([&] {
        CHECK(jq2_queue_create(1, 0x360001, SPURS, QUEUE2, ATTR, 0, 0, 0, RING2, 64, 1, 0) == 0, "second queue");
    });
    CHECK(jq2_port_create(PORT2, QUEUE2) == 0, "second port");
    CHECK(log.find("[jq2] queue q=0 ea=0x00010000 workers=3 depth=128\n") != std::string::npos, "first creation line in:\n%s",
          log.c_str());
    CHECK(log.find("[jq2] queue q=1 ea=0x00080000 workers=1 depth=64\n") != std::string::npos, "second creation line in:\n%s",
          log.c_str());

    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < 5; ++j) specs.push_back(spec(j));
    for (std::uint32_t j = 0; j < 3; ++j) rig->push_wait(specs[j].desc, 64, 0);
    for (std::uint32_t j = 3; j < 5; ++j) CHECK(jq2_port_push_job(PORT2, specs[j].desc, 64, 0, 1) == 0, "push on q1");
    CHECK(jq2_port_sync(PORT, 0) == 0 && jq2_port_sync(PORT2, 0) == 0, "syncs");
    rig->teardown();
    CHECK(jq2_port_destroy(PORT2) == 0 && jq2_queue_shutdown(QUEUE2) == 0, "second queue shutdown");
    std::uint32_t exit_value = 0;
    CHECK(jq2_queue_join(QUEUE2, &exit_value) == 0, "second queue join");
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    check_job_spans(0, 3, false, 3);
    check_job_spans(1, 2, false, 1);

    /* Queues 3..257 of the process: only creation order counts, so create and join one at a time. */
    const std::string many = capture_stderr([&] {
        for (unsigned i = 0; i < 255; ++i) {
            CHECK(jq2_queue_create(1, 0x360001, SPURS, QUEUE2, ATTR, 0, 0, 0, RING2, 16, 1, 0) == 0, "queue %u", i);
            CHECK(jq2_queue_shutdown(QUEUE2) == 0 && jq2_queue_join(QUEUE2, &exit_value) == 0, "join %u", i);
        }
    });
    CHECK(count_of(many, "[jq2] queue q=255 ea=0x00080000 workers=1 depth=16\n") == 2, "the 256th and 257th queue are q=255");
    CHECK(count_of(many, "[jq2] queue q saturated at 255: ea=0x00080000 is queue number 257\n") == 1,
          "the 257th queue logs that q saturated:\n%s", many.c_str() + many.size() - std::min<std::size_t>(many.size(), 300));
    CHECK(count_of(many, "saturated") == 1, "only the 257th logs a saturation");
}

/* Four threads push at once onto two workers: seq stays unique and gapless, and every job keeps its own
 * PUSH, JOB and CREDIT under it. */
void mode_trace_concurrent()
{
    constexpr unsigned kThreads = 4, kEach = 40;
    constexpr std::uint32_t kJobs = kThreads * kEach;
    build_image(IMAGE, Prog::transform);
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    CHECK(PS3_FT_ON(), "tracing is on"); /* before any worker: a hook sees "off" while another thread initialises */
    Rig rig({128, 2, false});
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < kJobs; ++j) specs.push_back(spec(j));

    std::atomic<unsigned> refused{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> pushers;
    for (unsigned t = 0; t < kThreads; ++t) {
        pushers.emplace_back([&, t] {
            ppu_context ctx{};
            ctx.thread_id = ppu_tid_make(t, t);
            g_active_ctx = &ctx;
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned k = 0; k < kEach; ++k) {
                const JobSpec& s = specs[t * kEach + k];
                if (jq2_port_push_job(PORT, s.desc, 64, (t + k) % 3, 1) != 0) refused.fetch_add(1);
            }
            g_active_ctx = nullptr;
        });
    }
    go.store(true, std::memory_order_release);
    for (std::thread& t : pushers) t.join();
    CHECK(refused == 0, "%u pushes refused", refused.load());
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    rig.teardown();

    check_job_spans(0, kJobs, false, 2);
    for (unsigned t = 0; t < kThreads; ++t) check_push_order(ppu_tid_diag_index(ppu_tid_make(t, t)), 0, kEach);
}

/* A SYNC that reaches the ring while a job on its tag is in flight bars the tag, and the job pushed behind it
 * waits for the release. HELD names the tag, the cause and the SYNC's mask; BARRIER spans the end of the pass
 * that barred the tag to the release; a worker parks with reason 3 (held) and a depth above 0 while the
 * barrier is up, and one that parked on the empty ring before any push has reason 1 (empty). */
void mode_trace_held()
{
    g_lifted = kLiftedBusy;
    build_image(IMAGE, Prog::transform);
    install_hooks();
    /* The first job stays in flight until the other worker has consumed the SYNC and parked on the job behind
     * it, so the barrier is up before that job can finish: no step of this mode depends on a sleep. */
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = [](const JobStart& js) {
            if (serial_of(js) != 0) return;
            CHECK(wait_for(
                      [] {
                          const auto v = observe_queue(QUEUE);
                          return v && v->settled >= 1;
                      },
                      10000),
                  "the other worker parks on the held job");
        };
    }
    ppu_context ppu{};
    ppu.thread_id = ppu_tid_make(0, 0);
    g_active_ctx = &ppu;

    CHECK(PS3_FT_ON(), "tracing is on"); /* before any worker: a hook sees "off" while another thread initialises */
    Rig rig({128, 2, false});
    CHECK(wait_for([] {
              const auto v = observe_queue(QUEUE);
              return v && v->settled == 2;
          }),
          "both workers park on the empty ring");
    CHECK(set_intake_paused(QUEUE, true), "pause");
    const JobSpec a = spec(0), b = spec(1);
    rig.push_wait(a.desc, 64, 0); /* seq 1, tag 0 */
    rig.sync_tags_wait(1u << 0);  /* bars tag 0 while seq 1 is in flight */
    rig.push_wait(b.desc, 64, 0); /* seq 2, tag 0, behind the SYNC */
    CHECK(set_intake_paused(QUEUE, false), "resume");
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    CHECK(job_ok(a) && job_ok(b), "outputs");
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    rig.teardown();
    g_active_ctx = nullptr;

    const SeqMap held = by_seq(PS3_FT_JQ2_HELD, 0), job = by_seq(PS3_FT_JQ2_JOB, 0);
    CHECK(held.size() == 2 && job.size() == 2, "q0: %zu HELD spans over %zu JOB spans, want 2 each", held.size(),
          job.size());
    const auto h1 = held.find(1), j1 = job.find(1), h2 = held.find(2), j2 = job.find(2);
    const bool one1 = h1 != held.end() && h1->second.size() == 1 && j1 != job.end() && j1->second.size() == 1;
    const bool one2 = h2 != held.end() && h2->second.size() == 1 && j2 != job.end() && j2->second.size() == 1;
    CHECK(one1 && one2, "seq 1: %d, seq 2: %d, want one HELD and one JOB each", one1 ? 1 : 0, one2 ? 1 : 0);
    const std::uint64_t job1_end = one1 ? j1->second[0].t1 : 0;

    /* seq 1 was admitted before the SYNC: never held, no mask, a zero-length span. */
    if (one1) {
        const ps3_ft_rec& r = h1->second[0];
        CHECK(r.a == 1 && (r.who >> 8) == 0 && (r.who & 0xFFu) < 2, "seq 1: HELD a=%u who=0x%X", r.a, r.who);
        CHECK(r.b == 0, "seq 1: HELD b=0x%X, want tag 0, cause 0, mask 0", r.b);
        CHECK(r.t1 == r.t0, "seq 1: HELD is %llu ns long, want 0 (never held)", (unsigned long long)(r.t1 - r.t0));
    }
    /* seq 2 waited for the SYNC's barrier on tag 0: cause 1, mask 1 << 0, t1 the release, and t1 <= JOB.t0. */
    if (one2) {
        const ps3_ft_rec& r = h2->second[0];
        CHECK(r.a == 2 && (r.who >> 8) == 0 && (r.who & 0xFFu) < 2, "seq 2: HELD a=%u who=0x%X", r.a, r.who);
        CHECK((r.b & 0xFu) == 0, "seq 2: HELD tag %u, want 0", r.b & 0xFu);
        CHECK(((r.b >> 4) & 0xFu) == 1, "seq 2: HELD cause %u, want 1 (held by a barrier)", (r.b >> 4) & 0xFu);
        CHECK((r.b >> 8) == 0x0001u, "seq 2: HELD mask 0x%X, want 0x0001 (the SYNC's)", r.b >> 8);
        CHECK(r.t0 <= r.t1 && r.t1 <= j2->second[0].t0, "seq 2: HELD %llu-%llu is not inside [t0, JOB.t0 %llu]",
              (unsigned long long)r.t0, (unsigned long long)r.t1, (unsigned long long)j2->second[0].t0);
        CHECK(r.t1 >= job1_end, "seq 2 became admissible at %llu, before seq 1 ended at %llu",
              (unsigned long long)r.t1, (unsigned long long)job1_end);
    }

    /* One barrier: the pass that consumed the SYNC barred tag 0 with the SYNC's mask while seq 1 was in
     * flight, and the complete() of seq 1 released it. */
    const std::vector<ps3_ft_rec> barrier = ft_kind(PS3_FT_JQ2_BARRIER);
    CHECK(barrier.size() == 1, "%zu BARRIER spans, want 1", barrier.size());
    if (barrier.size() == 1) {
        const ps3_ft_rec& r = barrier[0];
        CHECK((r.who >> 8) == 0 && (r.who & 0xFFu) < 2, "BARRIER who=0x%X", r.who);
        CHECK((r.a & 0xFFFFu) == 0x0001u, "BARRIER tags=0x%X, want 0x0001 (tag 0)", r.a & 0xFFFFu);
        CHECK((r.a >> 16) == 0x0001u, "BARRIER mask=0x%X, want 0x0001", r.a >> 16);
        CHECK((r.b & 0xFFu) == 0, "BARRIER q=%u, want 0", r.b & 0xFFu);
        CHECK((r.b >> 8) == 1, "BARRIER waited=%u, want 1 (seq 1 was in flight)", r.b >> 8);
        CHECK(r.t0 <= r.t1, "BARRIER ends before it starts");
    }

    /* Parks: one on the empty ring (reason 1) and one on the held job (reason 3, depth 1) while the barrier
     * was up. */
    bool empty_park = false, held_park = false, held_park_while_up = false;
    for (const ps3_ft_rec& r : ft_kind(PS3_FT_JQ2_IDLE)) {
        CHECK((r.who >> 8) == 0 && (r.who & 0xFFu) < 2, "IDLE who=0x%X", r.who);
        const std::uint32_t reason = r.a >> 24, depth = r.a & 0xFFFFFFu;
        if (reason == 1 && depth == 0) empty_park = true;
        if (reason == 3 && depth > 0) {
            held_park = true;
            if (barrier.size() == 1)
                held_park_while_up = held_park_while_up || (r.t0 <= barrier[0].t1 && r.t1 >= barrier[0].t0);
        }
    }
    CHECK(empty_park, "no IDLE span with reason 1 (empty) and depth 0 on the empty ring");
    CHECK(held_park, "no IDLE span with reason 3 (held) and depth > 0");
    CHECK(held_park_while_up, "the held park does not overlap the barrier");

    for (const ps3_ft_rec& r : ft_kind(PS3_FT_JQ2_CREDIT)) check_credit_fields(r);
}

/* Tracing off: the same traffic records nothing. */
void mode_trace_off()
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    Rig rig({128, 2, false});
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < 8; ++j) {
        specs.push_back(spec(j));
        rig.push_wait(specs.back().desc, 64, j % 3);
    }
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    rig.teardown();
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    CHECK(!PS3_FT_ON(), "tracing is off");
    CHECK(g_ps3_ft.n == 0 && g_ps3_ft.buf == nullptr, "no span was recorded (n=%u)", g_ps3_ft.n);
    for (int kind = PS3_FT_JQ2_PUSH; kind <= PS3_FT_JQ2_BARRIER; ++kind) CHECK(ft_kind(kind).empty(), "kind %d recorded", kind);
}

/* ---- jq2 job image identity (ps3_ftrace.h kind 29, the J table) -------------- */

/* The images are the transform program with one byte of the zero gap between its header and its code set to
 * `variant`: the same behaviour, a different fingerprint. */
constexpr std::uint32_t kTweak = 0x800;
void build_variant(std::uint32_t ea, std::uint8_t variant)
{
    build_image(ea, Prog::transform);
    vm_base[ea + kTweak] = variant;
}

std::uint64_t fp_at(std::uint32_t ea) { return spu_workload_fingerprint(vm_base + ea, kImageSize); }

/* Tracing on, hooks in place, nothing extra on admission. */
void trace_prologue()
{
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    CHECK(PS3_FT_ON(), "tracing is on"); /* before any worker: a hook sees "off" while another thread initialises */
}

/* One job on the image at `ea`, pushed alone and waited out: the test owns the image bytes between jobs. */
void run_job_on(Rig& rig, std::uint32_t j, std::uint32_t ea)
{
    const JobSpec s = spec(j, ea);
    rig.push_wait(s.desc, 64, 0);
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync after job %u", j);
    CHECK(job_ok(s), "job %u output", j);
}

struct ImageEntry {
    std::uint64_t fp;
    std::uint32_t ea, bytes;
};
using ImageTable = std::map<std::uint32_t, ImageEntry>;

/* The recorder's image table, set slots only, read after the threads that write it are joined. */
ImageTable ft_images()
{
    ImageTable t;
    for (std::uint32_t i = 0; i < PS3_FT_JQ2_IMAGES; ++i)
        if (g_ps3_ft.jq2_img[i].set) t[i] = {g_ps3_ft.jq2_img[i].fp, g_ps3_ft.jq2_img[i].ea, g_ps3_ft.jq2_img[i].bytes};
    return t;
}

constexpr std::uint32_t kNoImage = 0xFFFFFFFEu;

/* Every job of queue `q` (seq 1..n) has exactly one IMAGE span, on its JOB's worker and inside the JOB. Returns
 * the table index each carries, by seq - 1 (kNoImage where the span is missing). */
std::vector<std::uint32_t> check_image_spans(std::uint32_t q, std::uint32_t n)
{
    const SeqMap job = by_seq(PS3_FT_JQ2_JOB, q), image = by_seq(PS3_FT_JQ2_IMAGE, q);
    CHECK(job.size() == n && image.size() == n, "q%u: JOB %zu IMAGE %zu, want %u each", q, job.size(), image.size(), n);
    std::vector<std::uint32_t> index(n, kNoImage);
    for (std::uint32_t seq = 1; seq <= n; ++seq) {
        const auto j = job.find(seq), i = image.find(seq);
        const bool one = j != job.end() && j->second.size() == 1 && i != image.end() && i->second.size() == 1;
        CHECK(one, "q%u seq %u: not exactly one JOB and one IMAGE", q, seq);
        if (!one) continue;
        const ps3_ft_rec &jr = j->second[0], &ir = i->second[0];
        CHECK(ir.who == jr.who, "q%u seq %u: IMAGE on worker 0x%X, JOB on 0x%X", q, seq, ir.who, jr.who);
        CHECK(jr.t0 <= ir.t0 && ir.t0 <= ir.t1 && ir.t1 <= jr.t1, "q%u seq %u: IMAGE [%llu, %llu] is not inside JOB [%llu, %llu]",
              q, seq, (unsigned long long)ir.t0, (unsigned long long)ir.t1, (unsigned long long)jr.t0,
              (unsigned long long)jr.t1);
        index[seq - 1] = ir.b;
    }
    return index;
}

/* The entry job `seq` named, with the table's own check that the index exists. */
const ImageEntry* entry_of(const ImageTable& t, std::uint32_t index)
{
    const auto it = t.find(index);
    CHECK(it != t.end(), "image index %u has no J entry", index);
    return it == t.end() ? nullptr : &it->second;
}

/* Two different images: two entries, each fingerprint is the FNV of the bytes the loader staged, and each job's
 * IMAGE names its own image. */
void mode_trace_image_two()
{
    constexpr std::uint32_t A = IMAGE, B = IMAGE + 0x1000;
    build_variant(A, 1);
    build_variant(B, 2);
    trace_prologue();
    Rig rig({128, 1, false});
    run_job_on(rig, 0, A);
    run_job_on(rig, 1, B);
    rig.teardown();

    const ImageTable table = ft_images();
    CHECK(table.size() == 2, "%zu image entries, want 2", table.size());
    CHECK(g_ps3_ft.jq2_image_overflow == 0, "overflow %u", g_ps3_ft.jq2_image_overflow);
    const std::vector<std::uint32_t> idx = check_image_spans(0, 2);
    CHECK(idx[0] == 0 && idx[1] == 1, "indices %u %u, want 0 1 (dense, first seen first)", idx[0], idx[1]);
    const std::uint32_t ea[2] = {A, B};
    for (unsigned k = 0; k < 2; ++k) {
        const ImageEntry* e = entry_of(table, idx[k]);
        if (e == nullptr) continue;
        CHECK(e->ea == ea[k] && e->bytes == kImageSize, "job %u: entry ea 0x%X bytes 0x%X", k, e->ea, e->bytes);
        CHECK(e->fp == fp_at(ea[k]), "job %u: fingerprint 0x%016llx, want 0x%016llx", k, (unsigned long long)e->fp,
              (unsigned long long)fp_at(ea[k]));
    }
    CHECK(fp_at(A) != fp_at(B), "the two images must differ");
}

/* The same image at the same EA, run again and again: one entry, the same index every time. */
void mode_trace_image_same()
{
    build_variant(IMAGE, 1);
    trace_prologue();
    Rig rig({128, 1, false});
    for (std::uint32_t j = 0; j < 4; ++j) run_job_on(rig, j, IMAGE);
    rig.teardown();

    const ImageTable table = ft_images();
    CHECK(table.size() == 1, "%zu image entries, want 1", table.size());
    const std::vector<std::uint32_t> idx = check_image_spans(0, 4);
    for (std::uint32_t j = 0; j < 4; ++j) CHECK(idx[j] == 0, "job %u: index %u, want 0", j, idx[j]);
    const ImageEntry* e = entry_of(table, 0);
    CHECK(e != nullptr && e->fp == fp_at(IMAGE) && e->ea == IMAGE && e->bytes == kImageSize, "the one entry");
}

/* The same EA and size does not mean the same code: bytes changed in place between jobs give a new entry with a
 * new fingerprint, and restoring them finds the first entry again. A job that follows without a change reuses
 * the entry it just got. */
void mode_trace_image_changed()
{
    build_variant(IMAGE, 1);
    const std::uint64_t fp1 = fp_at(IMAGE);
    trace_prologue();
    Rig rig({128, 1, false});
    run_job_on(rig, 0, IMAGE);
    vm_base[IMAGE + kTweak] = 2;
    const std::uint64_t fp2 = fp_at(IMAGE);
    run_job_on(rig, 1, IMAGE);
    run_job_on(rig, 2, IMAGE);
    vm_base[IMAGE + kTweak] = 1;
    run_job_on(rig, 3, IMAGE);
    rig.teardown();

    CHECK(fp1 != fp2, "the two byte states must differ");
    const ImageTable table = ft_images();
    CHECK(table.size() == 2, "%zu image entries, want 2", table.size());
    const std::vector<std::uint32_t> idx = check_image_spans(0, 4);
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 1 && idx[3] == 0, "indices %u %u %u %u, want 0 1 1 0", idx[0], idx[1],
          idx[2], idx[3]);
    const ImageEntry *e0 = entry_of(table, 0), *e1 = entry_of(table, 1);
    if (e0 != nullptr && e1 != nullptr) {
        CHECK(e0->ea == IMAGE && e1->ea == IMAGE && e0->bytes == kImageSize && e1->bytes == kImageSize,
              "both entries at the one EA and size");
        CHECK(e0->fp == fp1 && e1->fp == fp2, "fingerprints 0x%016llx 0x%016llx, want 0x%016llx 0x%016llx",
              (unsigned long long)e0->fp, (unsigned long long)e1->fp, (unsigned long long)fp1, (unsigned long long)fp2);
    }
}

/* The same bytes at a second EA: a new entry with the same fingerprint (the report groups by fingerprint). */
void mode_trace_image_moved()
{
    constexpr std::uint32_t A = IMAGE, B = IMAGE + 0x1000;
    build_variant(A, 3);
    build_variant(B, 3);
    trace_prologue();
    Rig rig({128, 1, false});
    run_job_on(rig, 0, A);
    run_job_on(rig, 1, B);
    run_job_on(rig, 2, A);
    run_job_on(rig, 3, B);
    rig.teardown();

    const ImageTable table = ft_images();
    CHECK(table.size() == 2, "%zu image entries, want 2", table.size());
    const std::vector<std::uint32_t> idx = check_image_spans(0, 4);
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 0 && idx[3] == 1, "indices %u %u %u %u, want 0 1 0 1", idx[0], idx[1],
          idx[2], idx[3]);
    const ImageEntry *e0 = entry_of(table, 0), *e1 = entry_of(table, 1);
    if (e0 != nullptr && e1 != nullptr) {
        CHECK(e0->ea == A && e1->ea == B, "entry EAs 0x%X 0x%X", e0->ea, e1->ea);
        CHECK(e0->fp == e1->fp && e0->fp == fp_at(A), "one fingerprint for both EAs: 0x%016llx 0x%016llx",
              (unsigned long long)e0->fp, (unsigned long long)e1->fp);
    }
}

std::string overflow_trace_path() { return "/tmp/jq2_image_overflow_" + std::to_string(getpid()) + ".ftrace"; }

/* The table is 1024 slots: index 1024 and beyond store nothing and count, and the dump has the 1024 J lines
 * and both header fields. Through the recorder directly: no queue is involved. */
void mode_trace_image_overflow()
{
    constexpr std::uint32_t kSlots = PS3_FT_JQ2_IMAGES, kExtra = 3;
    CHECK(PS3_FT_ON(), "tracing is on");
    auto fp_of = [](std::uint32_t i) { return 0xABCD000000000000ull | i; };
    auto ea_of = [](std::uint32_t i) { return 0x100000u + 0x10u * i; };
    for (std::uint32_t i = 0; i < kSlots; ++i) ps3_ft_jq2_image(i, fp_of(i), ea_of(i), 0x1000u + i);
    CHECK(g_ps3_ft.jq2_image_overflow == 0, "overflow %u before the table is full", g_ps3_ft.jq2_image_overflow);
    ps3_ft_jq2_image(kSlots, 0x1111, 0x2000, 8);
    ps3_ft_jq2_image(kSlots + 1, 0x2222, 0x3000, 8);
    ps3_ft_jq2_image(0x7FFFFFFFu, 0x3333, 0x4000, 8);
    CHECK(g_ps3_ft.jq2_image_overflow == kExtra, "overflow %u, want %u", g_ps3_ft.jq2_image_overflow, kExtra);
    CHECK(ft_images().size() == kSlots, "%zu slots set, want %u", ft_images().size(), kSlots);

    const std::string path = g_ps3_ft.path;
    ps3_ft_dump();
    std::ifstream in(path);
    CHECK(in.good(), "cannot read %s", path.c_str());
    std::string line;
    std::getline(in, line);
    const std::string want_tail = " jq2_images=1024 jq2_image_overflow=3";
    CHECK(line.rfind("# ps3_ftrace ", 0) == 0 && line.find(" dropped=0" + want_tail) != std::string::npos &&
              line.size() >= want_tail.size() && line.compare(line.size() - want_tail.size(), want_tail.size(), want_tail) == 0,
          "header: %s", line.c_str());
    std::uint32_t j_lines = 0;
    bool in_order = true;
    while (std::getline(in, line)) {
        if (line.rfind("J ", 0) != 0) continue;
        char want[64];
        std::snprintf(want, sizeof want, "J %u %016llx %08x %u", j_lines, (unsigned long long)fp_of(j_lines), ea_of(j_lines),
                      0x1000u + j_lines);
        in_order = in_order && line == want;
        if (line != want) CHECK(false, "J line %u: '%s', want '%s'", j_lines, line.c_str(), want);
        ++j_lines;
    }
    CHECK(j_lines == kSlots && in_order, "%u J lines, want %u in index order", j_lines, kSlots);
    in.close();
    std::remove(path.c_str());
}

/* Four workers race on a mix of shared and distinct images: every (fingerprint, ea, bytes) triple has exactly
 * one index and no index has two triples, whichever worker saw it first. Six EAs carry four distinct byte
 * states, so two fingerprints are shared by two EAs. */
void mode_trace_image_concurrent()
{
    constexpr std::uint32_t kJobs = 240, kImages = 6, kWorkers = 4;
    constexpr std::uint8_t kVariant[kImages] = {1, 2, 3, 4, 1, 2};
    for (std::uint32_t i = 0; i < kImages; ++i) build_variant(IMAGE + 0x1000 * i, kVariant[i]);
    trace_prologue();
    Rig rig({128, kWorkers, false});
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < kJobs; ++j) {
        specs.push_back(spec(j, IMAGE + 0x1000 * (j % kImages)));
        rig.push_wait(specs.back().desc, 64, j % 3);
    }
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    rig.teardown();
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);

    const ImageTable table = ft_images();
    CHECK(table.size() == kImages, "%zu image entries, want %u", table.size(), kImages);
    CHECK(g_ps3_ft.jq2_image_overflow == 0, "overflow %u", g_ps3_ft.jq2_image_overflow);
    for (std::uint32_t i = 0; i < kImages; ++i) CHECK(table.count(i) == 1, "index %u is not in the table (dense from 0)", i);
    for (auto a = table.begin(); a != table.end(); ++a)
        for (auto b = std::next(a); b != table.end(); ++b)
            CHECK(a->second.ea != b->second.ea, "indices %u and %u are the same triple", a->first, b->first);
    std::map<std::uint64_t, unsigned> per_fp;
    for (const auto& [index, e] : table) {
        CHECK(e.bytes == kImageSize && e.fp == fp_at(e.ea), "index %u: ea 0x%X fingerprint 0x%016llx", index, e.ea,
              (unsigned long long)e.fp);
        ++per_fp[e.fp];
    }
    CHECK(per_fp.size() == 4, "%zu distinct fingerprints, want 4", per_fp.size());

    /* seq is the push order of the one pushing thread, so job j is seq j + 1 on image j % kImages. */
    const std::vector<std::uint32_t> idx = check_image_spans(0, kJobs);
    std::map<std::uint32_t, std::uint32_t> index_of_image;
    for (std::uint32_t j = 0; j < kJobs; ++j) {
        const ImageEntry* e = entry_of(table, idx[j]);
        if (e == nullptr) continue;
        const std::uint32_t ea = IMAGE + 0x1000 * (j % kImages);
        CHECK(e->ea == ea, "job %u: IMAGE names ea 0x%X, the job ran 0x%X", j, e->ea, ea);
        const auto [it, fresh] = index_of_image.try_emplace(ea, idx[j]);
        CHECK(fresh || it->second == idx[j], "ea 0x%X carries indices %u and %u", ea, it->second, idx[j]);
    }
}

/* Tracing off: nothing is identified or recorded, and the jobs run as before. */
void mode_trace_image_off()
{
    build_variant(IMAGE, 1);
    build_variant(IMAGE + 0x1000, 2);
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    Rig rig({128, 2, false});
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < 8; ++j) {
        specs.push_back(spec(j, IMAGE + 0x1000 * (j % 2)));
        rig.push_wait(specs.back().desc, 64, j % 3);
    }
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    rig.teardown();
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    CHECK(!PS3_FT_ON(), "tracing is off");
    CHECK(ft_kind(PS3_FT_JQ2_IMAGE).empty(), "an IMAGE span was recorded");
    CHECK(ft_images().empty(), "an image table entry was recorded");
    CHECK(g_ps3_ft.jq2_image_overflow == 0 && g_ps3_ft.n == 0 && g_ps3_ft.buf == nullptr, "the recorder was touched");
}

/* The observer's loader path identifies the image too, and the observer still sees exactly what it did before:
 * its job_start gets the staged local store, whose fingerprint is the one the J line carries. */
struct Staged {
    std::vector<std::uint64_t> fp;
};
void staged_start(void* user, spu_context* ctx, const jq2_job_info* job)
{
    static_cast<Staged*>(user)->fp.push_back(spu_workload_fingerprint(ctx->ls + job->image_ls, job->image_bytes));
}

void mode_trace_image_observer()
{
    constexpr std::uint32_t A = IMAGE, B = IMAGE + 0x1000;
    build_variant(A, 1);
    build_variant(B, 2);
    trace_prologue();
    Rig rig({128, 1, false});
    run_job_on(rig, 0, A); /* the plain path */
    run_job_on(rig, 1, B);
    Staged staged;
    const jq2_job_observer obs{&staged, &staged_start, nullptr};
    CHECK(jq2_set_job_observer(QUEUE, &obs) == 0, "install the observer");
    run_job_on(rig, 2, A); /* the observer path */
    run_job_on(rig, 3, B);
    rig.teardown();

    const ImageTable table = ft_images();
    CHECK(table.size() == 2, "%zu image entries, want 2", table.size());
    const std::vector<std::uint32_t> idx = check_image_spans(0, 4);
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 0 && idx[3] == 1, "indices %u %u %u %u, want 0 1 0 1", idx[0], idx[1],
          idx[2], idx[3]);
    CHECK(staged.fp.size() == 2, "the observer saw %zu job starts, want 2", staged.fp.size());
    if (staged.fp.size() == 2) {
        const ImageEntry *e0 = entry_of(table, 0), *e1 = entry_of(table, 1);
        CHECK(e0 != nullptr && e1 != nullptr && staged.fp[0] == e0->fp && staged.fp[1] == e1->fp,
              "the staged fingerprints differ from the J lines");
    }
}

/* ---- fixture capture (docs/SPU_XDIFF.md section 16, commit 10a) ---------------------------------- */

namespace fs = std::filesystem;

/* The capture directory of this process. main() creates it and sets the two options in the environment
 * before anything reads the configuration, which is immutable after its first read: one mode, one
 * configuration. A mode that needs a reference run without capture forks it first. */
std::string g_xd_dir;
constexpr std::uint32_t kXdN = 3; /* spu.xdiff_capture of the modes that record, unless main() says otherwise */

/* The registration of the transform image: what the pipeline's generated registration would pass. */
void xd_register(std::uint32_t flags, std::uint32_t code_end = 0, std::uint64_t lift_options = 0)
{
    spu_workload_set_inventory(fp_at(IMAGE), flags, code_end, lift_options);
}

/* `jobs` transform jobs, job j with seq j + 1, on a queue of `workers` workers. With one worker each job is pushed
 * and synced alone, so the sequence and the guest memory it leaves are deterministic; with more they are pushed in a
 * row and synced once. `image_ea` names job j's image (default IMAGE) and `before_job` runs before job j is described
 * (to change the image's bytes). Every job's output and the queue's counters are checked. */
std::vector<JobSpec> xd_run(std::uint32_t jobs, std::uint32_t workers = 1, const std::function<void()>& after_create = {},
                            const std::function<std::uint32_t(std::uint32_t)>& image_ea = {},
                            const std::function<void(std::uint32_t)>& before_job = {})
{
    build_image(IMAGE, Prog::transform);
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    Rig rig({128, workers, false});
    if (after_create) after_create();
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < jobs; ++j) {
        if (before_job) before_job(j);
        specs.push_back(spec(j, image_ea ? image_ea(j) : std::uint32_t(IMAGE)));
        rig.push_wait(specs.back().desc, 64, 0);
        if (workers == 1) CHECK(jq2_port_sync(PORT, 0) == 0, "sync after job %u", j);
    }
    if (workers != 1) CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    const jq2_stats st = rig.stats();
    CHECK(st.dispatched == jobs && st.executed == jobs && st.credited == jobs, "counts %llu/%llu/%llu, wanted %u",
          (unsigned long long)st.dispatched, (unsigned long long)st.executed, (unsigned long long)st.credited, jobs);
    rig.teardown();
    return specs;
}

/* All of guest memory, the one thing a job and its capture could share. */
std::uint64_t xd_arena_digest() { return spu_workload_content_hash(vm_base, kMem); }

/* What the capture directory holds, sorted; empty when it is gone. */
std::vector<std::string> xd_files()
{
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(g_xd_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        names.push_back(it->path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

std::string join(const std::vector<std::string>& v)
{
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}

/* The file name of section 7.2, spelled out here rather than asked of the writer. */
std::string xd_name(std::uint64_t fp, std::uint32_t q, std::uint32_t seq)
{
    char b[96];
    std::snprintf(b, sizeof b, "%016llx-q%u-s%u.spujob", (unsigned long long)fp, q, seq);
    return b;
}

std::string xd_fp_hex(std::uint64_t fp)
{
    char b[24];
    std::snprintf(b, sizeof b, "0x%016llX", (unsigned long long)fp);
    return b;
}

struct Fx {
    spu_xdiff_fixture f{};
    ~Fx() { spu_xdiff_fixture_free(&f); }
};

bool xd_read(const std::string& name, Fx& fx)
{
    spu_xdiff_error err{};
    const std::string path = g_xd_dir + "/" + name;
    const int rc = spu_xdiff_fixture_read(path.c_str(), &fx.f, &err);
    CHECK(rc == 0, "decode %s: %s (%s)", name.c_str(), spu_xdiff_reason_id(err.reason), err.detail);
    return rc == 0;
}

/* A fixture of the transform job `s` on the image at `image_ea`, recorded as job `seq` of queue `queue` by a body of
 * tier `tier`. */
void xd_check_job(const spu_xdiff_fixture& f, const JobSpec& s, std::uint32_t seq, std::uint32_t tier,
                  std::uint32_t image_ea = IMAGE, std::uint32_t queue = 0)
{
    const spu_xdiff_unit& u = f.unit;
    CHECK(f.identity.image_fingerprint == fp_at(image_ea), "fingerprint 0x%llX", (unsigned long long)f.identity.image_fingerprint);
    CHECK(f.identity.authority.tier == tier, "authority tier %u, wanted %u", f.identity.authority.tier, tier);
    CHECK(f.identity.effect_port_abi == SPU_EFFECT_PORT_ABI, "port abi %u", f.identity.effect_port_abi);
    CHECK(u.kind == SPU_XDIFF_UNIT_JOB && u.profile == SPU_XDIFF_PROFILE_JQ2, "unit kind %u profile %u", u.kind, u.profile);
    CHECK(u.entry_pc == kBase && u.return_ls == kJobReturnLs, "entry 0x%X link 0x%X", u.entry_pc, u.return_ls);
    CHECK(u.image_ea == image_ea && u.image_ls == kBase && u.image_bytes == kImageSize && u.bss_bytes == kBssQuads * 16,
          "image ea 0x%X ls 0x%X bytes 0x%X bss 0x%X", u.image_ea, u.image_ls, u.image_bytes, u.bss_bytes);
    CHECK(u.desc_bytes == 64 && u.ctx_ls == SPU_JQ2_CTX_LS, "descriptor copy %u bytes, context 0x%X", u.desc_bytes, u.ctx_ls);
    CHECK(u.job_rt_ls == SPU_JQ2_JM_CALL_LS && u.job_rt_ls2 == SPU_JQ2_JM_SWAP_LS, "jm interface 0x%X 0x%X", u.job_rt_ls,
          u.job_rt_ls2);
    CHECK(u.origin_queue == queue && u.origin_worker == 0 && u.origin_seq == seq && u.origin_descriptor_ea == s.desc,
          "origin q%u w%u seq %u desc 0x%X, wanted q%u w0 seq %u desc 0x%X", u.origin_queue, u.origin_worker, u.origin_seq,
          u.origin_descriptor_ea, queue, seq, s.desc);

    /* The transcript: the job's GET and PUT, each followed by the bytes it moved. */
    std::vector<const spu_xdiff_effect*> cmds, xfers;
    for (std::uint32_t i = 0; i < f.effect_count; ++i) {
        if (f.effects[i].kind == SPU_FX_MFC_CMD) cmds.push_back(&f.effects[i]);
        if (f.effects[i].kind == SPU_FX_XFER) xfers.push_back(&f.effects[i]);
    }
    CHECK(cmds.size() == 2 && xfers.size() == 2, "transcript: %zu commands, %zu transfers", cmds.size(), xfers.size());
    if (cmds.size() != 2 || xfers.size() != 2) return;
    CHECK((cmds[0]->value & 0xFFFFu) == MFC_GET_CMD && cmds[0]->ea == s.in && cmds[0]->lsa == kLsIn && cmds[0]->size == 16,
          "the GET: cmd 0x%X ea 0x%llX lsa 0x%X size %u", cmds[0]->value, (unsigned long long)cmds[0]->ea, cmds[0]->lsa,
          cmds[0]->size);
    CHECK((cmds[1]->value & 0xFFFFu) == MFC_PUT_CMD && cmds[1]->ea == s.out && cmds[1]->lsa == kLsOut && cmds[1]->size == 16,
          "the PUT: cmd 0x%X ea 0x%llX lsa 0x%X size %u", cmds[1]->value, (unsigned long long)cmds[1]->ea, cmds[1]->lsa,
          cmds[1]->size);
    auto payload_is = [&f](const spu_xdiff_effect& e, std::uint32_t dir, std::uint32_t ea) {
        return e.dir == dir && e.ea == ea && e.size == 16 && e.blob != SPU_XDIFF_NO_BLOB &&
               std::uint64_t{e.blob} + e.size <= f.blob_bytes && std::memcmp(f.blob + e.blob, vm_base + ea, 16) == 0;
    };
    CHECK(payload_is(*xfers[0], SPU_XFER_TO_LS, s.in), "the GET's bytes are the input");
    CHECK(payload_is(*xfers[1], SPU_XFER_FROM_LS, s.out), "the PUT's bytes are the output");
}

/* The authoritative end of an interpreted job that returned: class return, the stop 0 at the link executed. */
void xd_check_interp_end(const spu_xdiff_fixture& f)
{
    CHECK(f.end.end_class == SPU_XDIFF_END_RETURN && f.end.stop_code == 0 && f.end.pc == kJobReturnLs + 4,
          "end class %u code %u pc 0x%X", f.end.end_class, f.end.stop_code, f.end.pc);
}

/* The interpreter against the transcript of `f`. */
void xd_replay_equal(const spu_xdiff_fixture& f, const std::string& what)
{
    spu_xdiff_shadow* shadow = spu_xdiff_shadow_new();
    CHECK(shadow != nullptr, "shadow");
    if (shadow == nullptr) return;
    spu_xdiff_replay_options opt{};
    opt.subject_tier = SPU_TIER_INTERP;
    spu_xdiff_verdict_record v{};
    const std::uint32_t verdict = spu_xdiff_replay(&f, &opt, shadow, &v);
    CHECK(verdict == SPU_XDIFF_EQUAL, "%s: replay is %s / %s", what.c_str(), spu_xdiff_verdict_id(verdict),
          spu_xdiff_reason_id(v.reason));
    spu_xdiff_shadow_free(shadow);
}

std::vector<std::string> xd_names(std::uint64_t fp, std::uint32_t first_seq, std::uint32_t n, std::uint32_t queue = 0)
{
    std::vector<std::string> names;
    for (std::uint32_t seq = first_seq; seq < first_seq + n; ++seq) names.push_back(xd_name(fp, queue, seq));
    return names;
}

/* N + 1 jobs under capture = N: exactly N fixtures, named per 7.2, each with the job's identity, effects and end,
 * each replaying equal on the interpreter, and the guest memory of the run is that of the same run without capture. */
void mode_xdiff_capture()
{
    constexpr std::uint32_t kJobs = kXdN + 1;
    build_image(IMAGE, Prog::transform);
    xd_register(0);
    const std::uint64_t fp = fp_at(IMAGE);

    /* The reference: the same sequence with capture off, in a child (forked before any thread exists and before
     * anything has read the configuration, which the child sets differently). It leaves its guest memory's digest
     * in a page both share, outside the arena. */
    auto* digest = static_cast<std::uint64_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    CHECK(digest != MAP_FAILED, "mmap");
    if (digest == MAP_FAILED) return;
    std::string child_log;
    const int rc = run_child(
        [digest] {
            setenv("PS3RECOMP_SPU_XDIFF_CAPTURE", "0", 1);
            xd_run(kJobs);
            *digest = xd_arena_digest();
            if (g_fail != 0) _exit(1);
        },
        child_log);
    CHECK(rc == 0, "the reference run without capture failed (status %d): %s", rc, child_log.c_str());
    CHECK(child_log.find("[jq2-xdiff]") == std::string::npos, "capture off said something: %s", child_log.c_str());
    CHECK(xd_files().empty(), "capture off wrote %s", join(xd_files()).c_str());
    std::memset(vm_base, 0, kMem); /* the arena as the child found it */

    const std::vector<JobSpec> specs = xd_run(kJobs);
    CHECK(xd_arena_digest() == *digest, "guest memory after the capturing run differs from the run without capture");

    CHECK(xd_files() == xd_names(fp, 1, kXdN), "fixtures: %s", join(xd_files()).c_str());
    const auto c = capture_counters(fp);
    CHECK(c && c->captured == kXdN && c->captured_with_unresolved == 0 && c->failed == 0 && c->ineligible == 0,
          "counters %llu/%llu/%llu", (unsigned long long)(c ? c->captured : 0), (unsigned long long)(c ? c->failed : 0),
          (unsigned long long)(c ? c->ineligible : 0));
    for (std::uint32_t seq = 1; seq <= kXdN; ++seq) {
        Fx fx;
        if (!xd_read(xd_name(fp, 0, seq), fx)) continue;
        xd_check_job(fx.f, specs[seq - 1], seq, SPU_TIER_INTERP);
        xd_check_interp_end(fx.f);
        CHECK(fx.f.identity.authority.lifter_abi == 0 && fx.f.identity.authority.code_end == 0 &&
                  fx.f.identity.authority.lift_options == 0,
              "the interpreter carries no lift identity");
        xd_replay_equal(fx.f, xd_name(fp, 0, seq));
    }
}

/* A body registered for the image's fingerprint is the authority T1, with the lift identity of its registration. */
void mode_xdiff_lifted()
{
    constexpr std::uint32_t kCodeEnd = 0x8000;
    constexpr std::uint64_t kOptions = 0x1122334455667788ull;
    g_lifted = kLiftedNormal;
    build_image(IMAGE, Prog::transform);
    xd_register(0, kCodeEnd, kOptions);
    spu_workload_register_img(fp_at(IMAGE), &lifted_normal_entry, 5, "xd-lifted");
    const std::uint64_t fp = fp_at(IMAGE);
    const std::vector<JobSpec> specs = xd_run(kXdN + 1);
    CHECK(xd_files() == xd_names(fp, 1, kXdN), "fixtures: %s", join(xd_files()).c_str());
    for (std::uint32_t seq = 1; seq <= kXdN; ++seq) {
        Fx fx;
        if (!xd_read(xd_name(fp, 0, seq), fx)) continue;
        xd_check_job(fx.f, specs[seq - 1], seq, SPU_TIER_LIFTED);
        const spu_xdiff_body_id& b = fx.f.identity.authority;
        CHECK(b.lifter_abi == SPU_RUNTIME_LIFTER_ABI && b.code_end == kCodeEnd && b.lift_options == kOptions,
              "lift identity abi %u code_end 0x%X options 0x%llX", b.lifter_abi, b.code_end, (unsigned long long)b.lift_options);
        CHECK(fx.f.end.end_class == SPU_XDIFF_END_RETURN, "end class %u", fx.f.end.end_class);
    }
}

/* A body that is neither the interpreter nor the one registered for the image records nothing: the fixture would
 * name a tier that did not run. `registered_other`: a different body is registered for the fingerprint; otherwise
 * none is. The job runs either way. */
void xd_unknown_body(bool registered_other)
{
    constexpr std::uint32_t kJobs = kXdN + 1;
    g_lifted = kLiftedNormal;
    build_image(IMAGE, Prog::transform);
    xd_register(0);
    if (registered_other) spu_workload_register_img(fp_at(IMAGE), &lifted_halt_entry, 5, "xd-other");
    const std::uint64_t fp = fp_at(IMAGE);
    const std::string log = capture_stderr([] { xd_run(kJobs); });
    CHECK(xd_files().empty(), "an unknown body wrote %s", join(xd_files()).c_str());
    const auto c = capture_counters(fp);
    CHECK(c && c->captured == 0 && c->failed == 0 && c->ineligible == kJobs && c->ineligible_unknown_body == kJobs &&
              c->ineligible_no_inventory == 0 && c->ineligible_event_branches == 0,
          "counters: captured %llu ineligible %llu unknown-body %llu", (unsigned long long)(c ? c->captured : 0),
          (unsigned long long)(c ? c->ineligible : 0), (unsigned long long)(c ? c->ineligible_unknown_body : 0));
    CHECK(count_of(log, "capture-ineligible") == 1 && log.find("image=" + xd_fp_hex(fp) + " reason=unknown-body") != std::string::npos,
          "one line names the image and the reason:\n%s", log.c_str());
    CHECK(count_of(log, "captured image=") == 0, "something was recorded");
}
void mode_xdiff_body_none() { xd_unknown_body(false); }
void mode_xdiff_body_other() { xd_unknown_body(true); }

/* The loader's observed path records too, and the observer still sees every command the job issues. */
void mode_xdiff_observer()
{
    constexpr std::uint32_t kJobs = 3, kRecorded = 2; /* main() sets spu.xdiff_capture = 2 */
    build_image(IMAGE, Prog::transform);
    xd_register(0);
    const std::uint64_t fp = fp_at(IMAGE);
    const jq2_job_observer obs{nullptr, &obs_start, &obs_end};
    const std::vector<JobSpec> specs =
        xd_run(kJobs, 1, [&obs] { CHECK(jq2_set_job_observer(QUEUE, &obs) == 0, "install observer"); });
    CHECK(xd_files() == xd_names(fp, 1, kRecorded), "fixtures: %s", join(xd_files()).c_str());
    {
        std::lock_guard<std::mutex> lk(g_obs.mu);
        CHECK(g_obs.starts == int(kJobs) && g_obs.ends == int(kJobs), "observer start/end %d/%d", g_obs.starts, g_obs.ends);
        CHECK(g_obs.mfc.size() == 2 * kJobs, "the observer saw %zu commands", g_obs.mfc.size());
    }
    for (std::uint32_t seq = 1; seq <= kRecorded; ++seq) {
        Fx fx;
        if (!xd_read(xd_name(fp, 0, seq), fx)) continue;
        xd_check_job(fx.f, specs[seq - 1], seq, SPU_TIER_INTERP);
        xd_check_interp_end(fx.f);
        xd_replay_equal(fx.f, xd_name(fp, 0, seq));
    }
}

/* Three workers, two images, N = 5 each. The first three jobs are held at admission until every worker holds one (a
 * barrier, not a sleep), so all three workers are in the executor's job path together; it does not force the
 * reservation race itself, which stays a matter of timing. What it checks does not depend on timing: between the
 * workers exactly N fixtures per image are written, each with its own job's identity, and more than one worker
 * recorded (the three held jobs are the first three attempts, on three different workers). */
void mode_xdiff_concurrent()
{
    constexpr std::uint32_t kJobs = 24, kWorkers = 3, kRecorded = 5; /* main() sets spu.xdiff_capture = 5 */
    constexpr std::uint32_t kImageA = IMAGE, kImageB = IMAGE + 0x1000;
    build_image(IMAGE, Prog::transform);
    build_variant(kImageB, 1);
    xd_register(0);
    spu_workload_set_inventory(fp_at(kImageB), 0, 0, 0);
    const std::uint64_t fpa = fp_at(kImageA), fpb = fp_at(kImageB);
    CHECK(fpa != fpb, "the two images have one fingerprint");
    auto image_of_job = [](std::uint32_t j) { return (j % 2) != 0 ? kImageB : kImageA; };

    struct Gate {
        std::mutex mu;
        std::condition_variable cv;
        unsigned arrived = 0;
    } gate;
    const std::vector<JobSpec> specs = xd_run(
        kJobs, kWorkers,
        [&gate] {
            std::lock_guard<std::mutex> lk(g_probe.mu);
            g_probe.on_admit = [&gate](const JobStart&) {
                std::unique_lock<std::mutex> lk(gate.mu);
                if (gate.arrived >= kWorkers) return;
                ++gate.arrived;
                gate.cv.notify_all();
                gate.cv.wait(lk, [&gate] { return gate.arrived >= kWorkers; });
            };
        },
        image_of_job);
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    CHECK(gate.arrived == kWorkers, "the barrier saw %u workers", gate.arrived);

    const std::vector<std::string> files = xd_files();
    CHECK(files.size() == 2 * kRecorded, "fixtures: %s", join(files).c_str());
    for (const std::uint64_t fp : {fpa, fpb}) {
        const auto c = capture_counters(fp);
        CHECK(c && c->captured == kRecorded && c->failed == 0 && c->ineligible == 0, "counters of 0x%llX: %llu/%llu",
              (unsigned long long)fp, (unsigned long long)(c ? c->captured : 0), (unsigned long long)(c ? c->failed : 0));
    }
    std::set<std::uint32_t> workers;
    std::set<std::uint32_t> seqs;
    for (const std::string& name : files) {
        Fx fx;
        if (!xd_read(name, fx)) continue;
        const std::uint32_t seq = fx.f.unit.origin_seq;
        CHECK(seq >= 1 && seq <= kJobs && fx.f.unit.origin_worker < kWorkers, "seq %u worker %u", seq, fx.f.unit.origin_worker);
        if (seq < 1 || seq > kJobs) continue;
        const std::uint32_t ea = image_of_job(seq - 1);
        CHECK(name == xd_name(fp_at(ea), 0, seq), "%s is not the name of seq %u on image 0x%X", name.c_str(), seq, ea);
        CHECK(seqs.insert(seq).second, "seq %u recorded twice", seq);
        workers.insert(fx.f.unit.origin_worker);
        /* xd_check_job expects worker 0; the worker is whichever admitted the job, so check the rest by hand. */
        spu_xdiff_fixture g = fx.f;
        g.unit.origin_worker = 0;
        xd_check_job(g, specs[seq - 1], seq, SPU_TIER_INTERP, ea);
        xd_check_interp_end(fx.f);
        xd_replay_equal(fx.f, name);
    }
    CHECK(seqs.size() == 2 * kRecorded, "%zu distinct jobs recorded", seqs.size());
    CHECK(workers.size() >= 2, "only %zu worker(s) recorded", workers.size());
}

/* A job that ends the process leaves its fixture first: end class stop, with its code. */
void mode_xdiff_stop()
{
    build_image(IMAGE, Prog::stop_early);
    const std::uint64_t fp = fp_at(IMAGE);
    std::string err;
    const int rc = run_child(
        [] {
            build_image(IMAGE, Prog::stop_early);
            xd_register(0);
            install_hooks();
            Rig rig({128, 1, false});
            const JobSpec a = spec(0);
            rig.push(a.desc, 64, 0);
            jq2_port_sync(PORT, 0); /* never returns: the worker stops the process */
            std::fprintf(stderr, "the stop did not happen\n");
        },
        err);
    CHECK(rc == 70, "exit status %d, wanted 70 (a named stop)", rc);
    CHECK(err.find("kind=jq2-job-end") != std::string::npos, "stderr names the stop");
    CHECK(err.find("the stop did not happen") == std::string::npos, "the Sync returned");
    CHECK(xd_files() == xd_names(fp, 1, 1), "fixtures: %s", join(xd_files()).c_str());
    Fx fx;
    if (xd_read(xd_name(fp, 0, 1), fx)) {
        CHECK(fx.f.end.end_class == SPU_XDIFF_END_STOP && fx.f.end.stop_code == 0x2222, "end class %u code 0x%X",
              fx.f.end.end_class, fx.f.end.stop_code);
        CHECK(fx.f.effect_count == 0, "a job with no channel access has %u effects", fx.f.effect_count);
    }
}

/* An image the registration does not clear is counted, named once and never recorded: `flags` is its inventory
 * (`registered` false: none was registered, which is unknown and not clean). */
void xd_ineligible(bool registered, std::uint32_t flags, const char* reason, std::uint64_t events, std::uint64_t unknown)
{
    constexpr std::uint32_t kJobs = kXdN + 1;
    build_image(IMAGE, Prog::transform);
    if (registered) xd_register(flags);
    const std::uint64_t fp = fp_at(IMAGE);
    const std::string log = capture_stderr([] { xd_run(kJobs); });
    CHECK(xd_files().empty(), "an ineligible image wrote %s", join(xd_files()).c_str());
    const auto c = capture_counters(fp);
    if (!registered) {
        /* No inventory, no slot: the jobs are counted in the shared bucket. */
        const CaptureCounters u = capture_unattributed();
        CHECK(!c.has_value(), "an unregistered image took a slot");
        CHECK(u.ineligible == kJobs && u.ineligible_no_inventory == kJobs, "unattributed: ineligible %llu no-inventory %llu",
              (unsigned long long)u.ineligible, (unsigned long long)u.ineligible_no_inventory);
    } else {
        CHECK(c.has_value(), "the image was never looked at");
    }
    if (c) {
        CHECK(c->captured == 0 && c->captured_with_unresolved == 0 && c->failed == 0, "captured %llu failed %llu",
              (unsigned long long)c->captured, (unsigned long long)c->failed);
        CHECK(c->ineligible == kJobs, "ineligible jobs %llu, wanted %u", (unsigned long long)c->ineligible, kJobs);
        CHECK(c->ineligible_event_branches == events && c->ineligible_no_inventory == 0 && c->ineligible_unknown_body == 0,
              "reasons: events %llu unknown %llu", (unsigned long long)c->ineligible_event_branches,
              (unsigned long long)c->ineligible_no_inventory);
    }
    /* One line for the image, whatever the number of jobs, and it names the image and why. */
    CHECK(count_of(log, "capture-ineligible") == 1, "%zu capture-ineligible lines", count_of(log, "capture-ineligible"));
    CHECK(log.find("image=" + xd_fp_hex(fp) + " reason=" + reason) != std::string::npos, "the line names image and reason: %s",
          log.c_str());
    CHECK(count_of(log, "captured image=") == 0 && count_of(log, "capture-failed") == 0, "nothing was recorded or failed");
}

void mode_xdiff_event() { xd_ineligible(true, SPU_WORKLOAD_INV_EVENT_BRANCHES, "event-branches", kXdN + 1, 0); }
/* Unresolved MFC sites are not a reason: the image is recorded, and its fixtures are counted apart (section 12.3). */
void mode_xdiff_unresolved()
{
    build_image(IMAGE, Prog::transform);
    xd_register(SPU_WORKLOAD_INV_UNRESOLVED_MFC);
    const std::uint64_t fp = fp_at(IMAGE);
    const std::vector<JobSpec> specs = xd_run(kXdN + 1);
    CHECK(xd_files() == xd_names(fp, 1, kXdN), "fixtures: %s", join(xd_files()).c_str());
    const auto c = capture_counters(fp);
    CHECK(c && c->captured == kXdN && c->captured_with_unresolved == kXdN && c->failed == 0 && c->ineligible == 0,
          "counters %llu/%llu/%llu/%llu", (unsigned long long)(c ? c->captured : 0),
          (unsigned long long)(c ? c->captured_with_unresolved : 0), (unsigned long long)(c ? c->failed : 0),
          (unsigned long long)(c ? c->ineligible : 0));
    for (std::uint32_t seq = 1; seq <= kXdN; ++seq) {
        Fx fx;
        if (!xd_read(xd_name(fp, 0, seq), fx)) continue;
        xd_check_job(fx.f, specs[seq - 1], seq, SPU_TIER_INTERP);
        xd_replay_equal(fx.f, xd_name(fp, 0, seq));
    }
}
/* Event branches refuse the image whatever else its inventory says; the unresolved flag is not a second reason. */
void mode_xdiff_both()
{
    xd_ineligible(true, SPU_WORKLOAD_INV_EVENT_BRANCHES | SPU_WORKLOAD_INV_UNRESOLVED_MFC, "event-branches", kXdN + 1, 0);
}
void mode_xdiff_unknown() { xd_ineligible(false, 0, "no-inventory", 0, kXdN + 1); }

/* A loader that alternates two buffers through one address (A, B, A, B, ...): each is fingerprinted once, then
 * remembered by its content, and both record, N fixtures per image. */
void mode_xdiff_varying()
{
    constexpr std::uint32_t kJobs = 12;
    build_variant(IMAGE, 1);
    const std::uint64_t fpa = fp_at(IMAGE);
    build_variant(IMAGE, 2);
    const std::uint64_t fpb = fp_at(IMAGE);
    spu_workload_set_inventory(fpa, 0, 0, 0);
    spu_workload_set_inventory(fpb, 0, 0, 0);
    xd_run(kJobs, 1, {}, {}, [](std::uint32_t j) { vm_base[IMAGE + kTweak] = (j % 2) != 0 ? 2 : 1; });
    /* Job j has seq j + 1: A on the odd seqs, B on the even ones, the first N of each. */
    std::vector<std::string> want;
    for (std::uint32_t k = 0; k < kXdN; ++k) {
        want.push_back(xd_name(fpa, 0, 2 * k + 1));
        want.push_back(xd_name(fpb, 0, 2 * k + 2));
    }
    std::sort(want.begin(), want.end());
    CHECK(xd_files() == want, "fixtures: %s", join(xd_files()).c_str());
    for (const std::uint64_t fp : {fpa, fpb}) {
        const auto c = capture_counters(fp);
        CHECK(c && c->captured == kXdN && c->failed == 0 && c->ineligible == 0, "counters of 0x%llX", (unsigned long long)fp);
    }
    CHECK(capture_unattributed().fingerprint_cache_misses == 2, "%llu fingerprints computed, wanted one per content",
          (unsigned long long)capture_unattributed().fingerprint_cache_misses);
    for (const std::string& name : xd_files()) {
        Fx fx;
        if (!xd_read(name, fx)) continue;
        CHECK(name == xd_name(fx.f.identity.image_fingerprint, 0, fx.f.unit.origin_seq), "%s names its own fingerprint and seq",
              name.c_str());
        xd_replay_equal(fx.f, name);
    }
}

/* Bytes that never repeat and no inventory: each job costs one hash and one fingerprint and nothing else. No image
 * slot is taken, the jobs are counted together, and the address is named once. */
void mode_xdiff_nonrepeating()
{
    constexpr std::uint32_t kJobs = 12;
    std::vector<std::uint64_t> fps;
    for (std::uint32_t j = 0; j < kJobs; ++j) {
        build_variant(IMAGE, std::uint8_t(j + 1));
        fps.push_back(fp_at(IMAGE));
    }
    const std::string log = capture_stderr([] {
        xd_run(kJobs, 1, {}, {}, [](std::uint32_t j) { vm_base[IMAGE + kTweak] = std::uint8_t(j + 1); });
    });
    CHECK(xd_files().empty(), "unregistered bytes wrote %s", join(xd_files()).c_str());
    for (std::uint32_t j = 0; j < kJobs; ++j)
        CHECK(!capture_counters(fps[j]).has_value(), "job %u: an unregistered image must not take a slot", j);
    const CaptureCounters u = capture_unattributed();
    CHECK(u.ineligible == kJobs && u.ineligible_no_inventory == kJobs && u.ineligible_table_full == 0 &&
              u.fingerprint_cache_misses == kJobs,
          "unattributed: ineligible %llu no-inventory %llu misses %llu", (unsigned long long)u.ineligible,
          (unsigned long long)u.ineligible_no_inventory, (unsigned long long)u.fingerprint_cache_misses);
    CHECK(count_of(log, "capture-ineligible image=") == 1 && count_of(log, "reason=no-inventory ea=0x00100000 bytes=4096") == 1,
          "the address is named once:\n%s", log.c_str());
}

constexpr std::uint32_t kXdFirst = 0x600000; /* images the capture modes stage one per job, 0x1000 apart */

/* `n` images at kXdFirst + 0x1000 * i, each with its own fingerprint. */
void xd_build_images(std::uint32_t n)
{
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t ea = kXdFirst + 0x1000 * i;
        build_image(ea, Prog::transform);
        vm_base[ea + kTweak] = std::uint8_t(i & 0xFF);
        vm_base[ea + kTweak + 1] = std::uint8_t(i >> 8);
        vm_base[ea + kTweak + 2] = 0x5A; /* never the bytes of the transform image at IMAGE */
    }
}

/* Images with no inventory take no slot: after 300 of them, an image the registration cleared still records. */
void mode_xdiff_starvation()
{
    constexpr std::uint32_t kUnknown = 300, kJobs = kUnknown + kXdN + 1;
    xd_build_images(kUnknown);
    build_image(IMAGE, Prog::transform);
    xd_register(0);
    const std::uint64_t fp = fp_at(IMAGE);
    capture_stderr([] {
        xd_run(kJobs, 1, {}, [](std::uint32_t j) { return j < kUnknown ? kXdFirst + 0x1000 * j : std::uint32_t(IMAGE); });
    });
    CHECK(xd_files() == xd_names(fp, kUnknown + 1, kXdN), "fixtures: %s", join(xd_files()).c_str());
    const auto c = capture_counters(fp);
    CHECK(c && c->captured == kXdN && c->failed == 0, "the registered image: captured %llu", (unsigned long long)(c ? c->captured : 0));
    const CaptureCounters u = capture_unattributed();
    CHECK(u.ineligible == kUnknown && u.ineligible_no_inventory == kUnknown && u.ineligible_table_full == 0,
          "unattributed: ineligible %llu no-inventory %llu table-full %llu", (unsigned long long)u.ineligible,
          (unsigned long long)u.ineligible_no_inventory, (unsigned long long)u.ineligible_table_full);
}

/* The 32-line budget is for capture-ineligible only: after 40 images have used it, a fixture that cannot be written
 * still prints its capture-failed line (every one of them consumes an attempt, so they are bounded by N). */
void mode_xdiff_failure_lines()
{
    constexpr std::uint32_t kUnknown = 40, kJobs = kUnknown + kXdN + 1, kLines = 32;
    xd_build_images(kUnknown);
    build_image(IMAGE, Prog::transform);
    xd_register(0);
    const std::string log = capture_stderr([] {
        xd_run(
            kJobs, 1,
            [] {
                std::error_code ec;
                fs::remove_all(g_xd_dir, ec); /* fine at the first queue, gone before the first fixture */
            },
            [](std::uint32_t j) { return j < kUnknown ? kXdFirst + 0x1000 * j : std::uint32_t(IMAGE); });
    });
    CHECK(count_of(log, "capture-ineligible image=") == kLines && count_of(log, "capture-lines-suppressed") == 1,
          "%zu ineligible lines, %zu suppression lines", count_of(log, "capture-ineligible image="),
          count_of(log, "capture-lines-suppressed"));
    CHECK(count_of(log, "capture-failed image=" + xd_fp_hex(fp_at(IMAGE))) == kXdN, "%zu capture-failed lines after the budget:\n%s",
          count_of(log, "capture-failed image="), log.c_str());
}

/* Images the registration cleared are tracked (256 slots, as many as the registry has inventories); the others are
 * counted together without a slot; the ineligible lines stop at 32 with one line that says so. */
void mode_xdiff_bounded()
{
    constexpr std::uint32_t kImages = 260, kTable = 256, kLines = 32;
    xd_build_images(kImages);
    for (std::uint32_t i = 0; i < kTable; ++i) /* the registry holds 256 inventories: the last four images have none */
        spu_workload_set_inventory(fp_at(kXdFirst + 0x1000 * i), SPU_WORKLOAD_INV_EVENT_BRANCHES, 0, 0);
    const std::string log = capture_stderr([] {
        xd_run(kImages, 1, {}, [](std::uint32_t j) { return kXdFirst + 0x1000 * j; });
    });
    CHECK(xd_files().empty(), "event-branch images wrote %s", join(xd_files()).c_str());
    std::uint32_t tracked = 0;
    for (std::uint32_t i = 0; i < kImages; ++i)
        if (capture_counters(fp_at(kXdFirst + 0x1000 * i))) ++tracked;
    CHECK(tracked == kTable, "%u fingerprints tracked, wanted %u", tracked, kTable);
    const CaptureCounters u = capture_unattributed();
    CHECK(u.ineligible == kImages - kTable && u.ineligible_no_inventory == kImages - kTable && u.ineligible_table_full == 0,
          "unattributed: ineligible %llu no-inventory %llu table-full %llu", (unsigned long long)u.ineligible,
          (unsigned long long)u.ineligible_no_inventory, (unsigned long long)u.ineligible_table_full);
    CHECK(count_of(log, "capture-ineligible image=") == kLines, "%zu capture-ineligible lines, wanted %u",
          count_of(log, "capture-ineligible image="), kLines);
    CHECK(count_of(log, "capture-lines-suppressed") == 1, "%zu suppression lines", count_of(log, "capture-lines-suppressed"));
}

/* The collision branch: the fingerprint a worker remembers is not the one of the staged bytes (a seam makes it so).
 * The recorder notices, the fixture is dropped, the attempt is spent, and the line is not budgeted. */
void mode_xdiff_collision()
{
    constexpr std::uint64_t kMask = 0x00FF00FF00FF00FFull;
    build_image(IMAGE, Prog::transform);
    const std::uint64_t fp = fp_at(IMAGE), remembered = fp ^ kMask;
    spu_workload_set_inventory(remembered, 0, 0, 0);
    set_capture_fingerprint_fault(kMask);
    const std::string log = capture_stderr([] { xd_run(kXdN + 1); });
    CHECK(xd_files().empty(), "a collision wrote %s", join(xd_files()).c_str());
    const auto c = capture_counters(remembered);
    CHECK(c && c->ineligible_collision == kXdN && c->ineligible == kXdN && c->captured == 0 && c->failed == 0,
          "counters: collisions %llu ineligible %llu", (unsigned long long)(c ? c->ineligible_collision : 0),
          (unsigned long long)(c ? c->ineligible : 0));
    CHECK(!capture_counters(fp).has_value(), "the real fingerprint was never remembered");
    CHECK(count_of(log, "reason=fingerprint-collision") == kXdN, "%zu collision lines:\n%s", count_of(log, "reason=fingerprint-collision"),
          log.c_str());
    CHECK(count_of(log, "captured image=") == 0, "something was recorded");
}

/* spu.xdiff_dir missing at the first queue (capture = 1000): one line, capture is off for the process, nothing else is
 * said or counted, and the jobs run. main() points the option at a directory that does not exist. */
void mode_xdiff_baddir()
{
    build_image(IMAGE, Prog::transform);
    xd_register(0);
    const std::uint64_t fp = fp_at(IMAGE);
    const std::string log = capture_stderr([] { xd_run(kXdN + 1); }); /* xd_run checks every output and the counts */
    CHECK(count_of(log, "[jq2-xdiff]") == 1, "%zu [jq2-xdiff] lines:\n%s", count_of(log, "[jq2-xdiff]"), log.c_str());
    CHECK(count_of(log, "capture-off reason=bad-dir") == 1 && log.find("does not exist") != std::string::npos,
          "the line names the directory problem:\n%s", log.c_str());
    CHECK(!capture_counters(fp).has_value(), "capture is off, yet the image was counted");
    CHECK(xd_files().empty(), "the parent holds %s", join(xd_files()).c_str());
}

/* A directory that was fine at the first queue and cannot be written when a fixture is: one named diagnostic per
 * attempt (the attempts are the N), the jobs and the run go on, nothing is left behind. */
void mode_xdiff_writefail()
{
    constexpr std::uint32_t kJobs = kXdN + 1;
    build_image(IMAGE, Prog::transform);
    xd_register(0);
    const std::uint64_t fp = fp_at(IMAGE);
    const std::string log = capture_stderr([] {
        xd_run(kJobs, 1, [] {
            std::error_code ec;
            fs::remove_all(g_xd_dir, ec); /* gone after the queue's check, before the first fixture */
        });
    });
    CHECK(count_of(log, "capture-off") == 0, "capture was switched off:\n%s", log.c_str());
    CHECK(count_of(log, "capture-failed") == kXdN, "%zu capture-failed lines:\n%s", count_of(log, "capture-failed"), log.c_str());
    CHECK(count_of(log, "capture-failed image=" + xd_fp_hex(fp) + " queue=0 seq=") == kXdN, "the lines name the image and the queue");
    CHECK(count_of(log, "reason=capture-limit detail=") == kXdN && count_of(log, "cannot create") == kXdN,
          "the lines carry reason and detail:\n%s", log.c_str());
    CHECK(count_of(log, "captured image=") == 0, "something was recorded");
    const auto c = capture_counters(fp);
    CHECK(c && c->failed == kXdN && c->captured == 0 && c->ineligible == 0, "counters %llu/%llu/%llu",
          (unsigned long long)(c ? c->captured : 0), (unsigned long long)(c ? c->failed : 0),
          (unsigned long long)(c ? c->ineligible : 0));
}

/* The payload bound: a recording that would pass it is a counted capture-limit failure, the job runs on. The job moves
 * 16 bytes in and 16 different bytes out; a bound of 24 admits the first payload and not the second. */
void mode_xdiff_payload()
{
    constexpr std::uint32_t kJobs = kXdN + 1;
    set_capture_payload_limit(24);
    build_image(IMAGE, Prog::transform);
    xd_register(0);
    const std::uint64_t fp = fp_at(IMAGE);
    const std::string log = capture_stderr([] { xd_run(kJobs); });
    CHECK(xd_files().empty(), "a recording over the bound wrote %s", join(xd_files()).c_str());
    CHECK(count_of(log, "capture-failed image=" + xd_fp_hex(fp)) == kXdN && count_of(log, "reason=capture-limit") == kXdN,
          "%u failures named, by capture-limit:\n%s", kXdN, log.c_str());
    const auto c = capture_counters(fp);
    CHECK(c && c->failed == kXdN && c->captured == 0, "counters %llu/%llu", (unsigned long long)(c ? c->captured : 0),
          (unsigned long long)(c ? c->failed : 0));
}

/* The queue number in a fixture's name is the creation number, not the saturating trace index: the 257th queue of the
 * process is q256 in the name and in the fixture. */
void mode_xdiff_queue256()
{
    constexpr std::uint32_t QUEUE2 = 0x80000, RING2 = 0x90000, kQueue = 256;
    build_image(IMAGE, Prog::transform);
    xd_register(0);
    const std::uint64_t fp = fp_at(IMAGE);
    put32(ATTR + 4, 8);
    put32(ATTR + 8, 256);
    capture_stderr([] {
        for (unsigned i = 0; i < kQueue; ++i) {
            CHECK(jq2_queue_create(1, 0x360001, SPURS, QUEUE2, ATTR, 0, 0, 0, RING2, 16, 1, 0) == 0, "queue %u", i);
            std::uint32_t exit_value = 0;
            CHECK(jq2_queue_shutdown(QUEUE2) == 0 && jq2_queue_join(QUEUE2, &exit_value) == 0, "join %u", i);
        }
    });
    const std::vector<JobSpec> specs = xd_run(kXdN + 1); /* the 257th queue */
    CHECK(xd_files() == xd_names(fp, 1, kXdN, kQueue), "fixtures: %s", join(xd_files()).c_str());
    for (std::uint32_t seq = 1; seq <= kXdN; ++seq) {
        Fx fx;
        if (!xd_read(xd_name(fp, kQueue, seq), fx)) continue;
        xd_check_job(fx.f, specs[seq - 1], seq, SPU_TIER_INTERP, IMAGE, kQueue);
    }
}

/* spu.xdiff_capture = 0: no tap, no line, no counter, no file. */
void mode_xdiff_off()
{
    build_image(IMAGE, Prog::transform);
    xd_register(0);
    const std::uint64_t fp = fp_at(IMAGE);
    const std::string log = capture_stderr([] { xd_run(2); });
    CHECK(log.find("[jq2-xdiff]") == std::string::npos, "capture off said something: %s", log.c_str());
    CHECK(xd_files().empty(), "capture off wrote %s", join(xd_files()).c_str());
    CHECK(!capture_counters(fp).has_value(), "capture off counted the image");
}

/* spu.xdiff_capture = 2 with no directory: nothing recorded, and the first queue says so, once. */
void mode_xdiff_nodir()
{
    build_image(IMAGE, Prog::transform);
    xd_register(0);
    const std::uint64_t fp = fp_at(IMAGE);
    const std::string log = capture_stderr([] { xd_run(3); });
    CHECK(count_of(log, "capture-off reason=no-dir") == 1, "%zu no-dir lines:\n%s", count_of(log, "capture-off reason=no-dir"),
          log.c_str());
    CHECK(xd_files().empty(), "no directory, yet %s", join(xd_files()).c_str());
    CHECK(!capture_counters(fp).has_value(), "no directory, yet the image was counted");
}

/* ---- spu.job_body: a body chosen from the staged bytes (docs/SPU_XDIFF.md sections 4 and 16, commit 6) ---- */

/* Two registered bodies that do the transform program's work (lifted_job) and say that they ran: which body, which
 * job (the descriptor's serial) and the image id the loader's context carried into the body. A job that ran
 * interpreted leaves no mark, so the mark is what tells the two apart; the guest-visible result is the same. */
struct BodyRun {
    int body;
    std::uint32_t serial;
    std::int32_t image_id;
};
struct BodyLog {
    std::mutex mu;
    std::vector<BodyRun> runs;
};
BodyLog g_body_log;

void marked_body(spu_context* c, int body)
{
    const std::uint32_t serial = ls_word(c, c->gpr[4]._u32[0] + 0x3C);
    {
        std::lock_guard<std::mutex> lk(g_body_log.mu);
        g_body_log.runs.push_back({body, serial, c->image_id});
    }
    lifted_job(c, false);
}
void marked_body_a(spu_context* c) { marked_body(c, 0); }
void marked_body_b(spu_context* c) { marked_body(c, 1); }
constexpr std::int32_t kImageIdA = 7, kImageIdB = 8;

/* Registered and promoted: the pair a title's generated registration makes for an image its corpus proved. */
void sel_register(std::uint64_t fp, spu_lifted_entry_fn fn, int image_id, const char* name)
{
    spu_workload_register_img(fp, fn, image_id, name);
    spu_workload_promote_img(fp);
}

std::vector<BodyRun> body_runs()
{
    std::lock_guard<std::mutex> lk(g_body_log.mu);
    return g_body_log.runs;
}

/* The serials of the jobs the body `body` ran, sorted, each with the image id it ran under checked. */
std::vector<std::uint32_t> serials_of(int body, std::int32_t image_id)
{
    std::vector<std::uint32_t> serials;
    for (const BodyRun& r : body_runs()) {
        if (r.body != body) continue;
        CHECK(r.image_id == image_id, "job %u ran body %d under image id %d, wanted %d", r.serial, body, r.image_id, image_id);
        serials.push_back(r.serial);
    }
    std::sort(serials.begin(), serials.end());
    return serials;
}

std::string serials_text(const std::vector<std::uint32_t>& v)
{
    std::string s;
    for (const std::uint32_t x : v) s += (s.empty() ? "" : " ") + std::to_string(x);
    return "[" + s + "]";
}

/* One identification per job, never two: `jobs` content hashes, and a byte-serial fingerprint only for the
 * contents the workers had not seen. `before` is identify_counts() taken before the jobs. */
void check_identified(const IdentifyCounts& before, std::uint64_t jobs, std::uint64_t fingerprints, const char* what)
{
    const IdentifyCounts now = identify_counts();
    CHECK(now.content_hashes - before.content_hashes == jobs, "%s: %llu content hashes for %llu jobs", what,
          (unsigned long long)(now.content_hashes - before.content_hashes), (unsigned long long)jobs);
    CHECK(now.fingerprints - before.fingerprints == fingerprints, "%s: %llu fingerprints, wanted %llu", what,
          (unsigned long long)(now.fingerprints - before.fingerprints), (unsigned long long)fingerprints);
}

/* The image id the context carried when each job's body ended (ExecutorHooks::on_end): the registration's id for a
 * chosen body, -1 for a job that ran interpreted. `want(j)` is the id job j must have had. */
void check_end_ids(std::uint32_t jobs, const std::function<std::int32_t(std::uint32_t)>& want, const char* what)
{
    std::lock_guard<std::mutex> lk(g_end_images.mu);
    for (std::uint32_t j = 0; j < jobs; ++j) {
        const auto it = g_end_images.id.find(j);
        CHECK(it != g_end_images.id.end(), "%s: job %u has no end record", what, j);
        if (it != g_end_images.id.end())
            CHECK(it->second == want(j), "%s: job %u ended with image id %d, wanted %d", what, j, it->second, want(j));
    }
}

/* A registered image runs its registered body under spu.job_body=lifted, with the registration's image id, and
 * the guest sees what the interpreter leaves: the whole arena equals that of the same jobs in a child that runs
 * with spu.job_body=interp, where the registered body never runs and nothing is identified. */
void mode_select_lifted()
{
    constexpr std::uint32_t kJobs = 6;
    build_image(IMAGE, Prog::transform);
    sel_register(fp_at(IMAGE), &marked_body_a, kImageIdA, "sel-a");

    /* The reference, forked before any thread exists and before the configuration is read. Four words in a page both
     * share: the arena digest, the registered body's runs, the content hashes and the fingerprints taken. */
    auto* ref = static_cast<std::uint64_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    CHECK(ref != MAP_FAILED, "mmap");
    if (ref == MAP_FAILED) return;
    std::string child_log;
    const int rc = run_child(
        [ref] {
            setenv("PS3RECOMP_SPU_JOB_BODY", "interp", 1);
            xd_run(kJobs);
            end_rec_clear(); /* on_end's note of how the body ended (pc, status): the host's, not the guest's */
            ref[0] = xd_arena_digest();
            ref[1] = body_runs().size();
            ref[2] = identify_counts().content_hashes;
            ref[3] = identify_counts().fingerprints;
            if (g_fail != 0) _exit(1);
        },
        child_log);
    CHECK(rc == 0, "the interpreted reference run failed (status %d): %s", rc, child_log.c_str());
    CHECK(ref[1] == 0, "spu.job_body=interp ran the registered body %llu times", (unsigned long long)ref[1]);
    CHECK(ref[2] == 0 && ref[3] == 0, "spu.job_body=interp identified %llu jobs, %llu fingerprints, with no trace and no capture",
          (unsigned long long)ref[2], (unsigned long long)ref[3]);
    std::memset(vm_base, 0, kMem); /* the arena as the child found it */

    const IdentifyCounts before = identify_counts();
    xd_run(kJobs);
    end_rec_clear();
    CHECK(xd_arena_digest() == ref[0], "guest memory after the lifted run differs from the interpreted run");
    std::vector<std::uint32_t> all(kJobs);
    for (std::uint32_t j = 0; j < kJobs; ++j) all[j] = j;
    const std::vector<std::uint32_t> ran = serials_of(0, kImageIdA);
    CHECK(ran == all, "the registered body ran jobs %s, wanted %s", serials_text(ran).c_str(), serials_text(all).c_str());
    check_identified(before, kJobs, 1, "lifted");
    check_end_ids(kJobs, [](std::uint32_t) { return kImageIdA; }, "lifted");
}

/* The image of a job is the bytes staged, not the address: other bytes at the same address and size run
 * interpreted, restored bytes run the registered body again, and the same bytes at another address run it too. */
void mode_select_other_bytes()
{
    constexpr std::uint32_t kJobs = 6, kImage2 = IMAGE + 0x1000;
    build_variant(IMAGE, 0);
    sel_register(fp_at(IMAGE), &marked_body_a, kImageIdA, "sel-a");
    /* The bytes job j finds at IMAGE: the registered ones (0), two others, the registered ones again, a third. Job
     * 5 is staged at another address with the registered bytes. */
    constexpr std::uint8_t variant[kJobs] = {0, 1, 1, 0, 2, 0};
    const IdentifyCounts before = identify_counts();
    xd_run(
        kJobs, 1, {}, [](std::uint32_t j) { return j == 5 ? kImage2 : std::uint32_t(IMAGE); },
        [&](std::uint32_t j) {
            vm_base[IMAGE + kTweak] = variant[j];
            if (j == 5) build_variant(kImage2, 0);
        });
    const std::vector<std::uint32_t> want = {0, 3, 5};
    const std::vector<std::uint32_t> ran = serials_of(0, kImageIdA);
    CHECK(ran == want, "the registered body ran jobs %s, wanted %s", serials_text(ran).c_str(), serials_text(want).c_str());
    /* Contents seen: (IMAGE, 0), (IMAGE, 1), (IMAGE, 2) and (kImage2, 0); job 2 and 3 are memory hits. */
    check_identified(before, kJobs, 4, "other bytes");
    check_end_ids(kJobs, [](std::uint32_t j) { return (j == 0 || j == 3 || j == 5) ? kImageIdA : -1; }, "other bytes");
}

/* spu.job_body=interp (the default): a registered body never runs, and a job that nothing else asks about is
 * not identified at all. */
void mode_select_interp()
{
    build_image(IMAGE, Prog::transform);
    sel_register(fp_at(IMAGE), &marked_body_a, kImageIdA, "sel-a");
    const IdentifyCounts before = identify_counts();
    xd_run(4);
    CHECK(body_runs().empty(), "the registered body ran %zu times under interp", body_runs().size());
    check_identified(before, 0, 0, "interp");
    check_end_ids(4, [](std::uint32_t) { return -1; }, "interp");
}

/* Per-image promotion: two images are registered (both bodies exist for capture and the offline runner) and only
 * the first is promoted. Under lifted only the promoted image runs its body, the other runs interpreted; under
 * interp neither runs. Promoting a fingerprint nothing is registered under is a named line and changes nothing. */
void promoted_scenario(bool lifted)
{
    constexpr std::uint32_t kJobs = 8, kImageB = IMAGE + 0x1000;
    build_image(IMAGE, Prog::transform);
    build_variant(kImageB, 1);
    sel_register(fp_at(IMAGE), &marked_body_a, kImageIdA, "sel-a");
    spu_workload_register_img(fp_at(kImageB), &marked_body_b, kImageIdB, "sel-b"); /* registered, not promoted */
    CHECK(spu_workload_is_promoted(fp_at(IMAGE)) == 1 && spu_workload_is_promoted(fp_at(kImageB)) == 0,
          "promotion flags %d %d", spu_workload_is_promoted(fp_at(IMAGE)), spu_workload_is_promoted(fp_at(kImageB)));
    const std::uint64_t bogus = 0x0123456789ABCDEFull;
    const std::string log = capture_stderr([&] { spu_workload_promote_img(bogus); });
    CHECK(log.find("promote-unregistered image=0x0123456789ABCDEF") != std::string::npos, "no named line:\n%s", log.c_str());
    CHECK(spu_workload_is_promoted(bogus) == 0, "an unregistered fingerprint is promoted");

    const IdentifyCounts before = identify_counts();
    xd_run(kJobs, 1, {}, [](std::uint32_t j) { return (j % 2) != 0 ? kImageB : std::uint32_t(IMAGE); });
    std::vector<std::uint32_t> even;
    for (std::uint32_t j = 0; j < kJobs; j += 2) even.push_back(j);
    if (lifted) {
        const std::vector<std::uint32_t> ran_a = serials_of(0, kImageIdA);
        CHECK(ran_a == even, "the promoted body ran jobs %s, wanted %s", serials_text(ran_a).c_str(), serials_text(even).c_str());
        CHECK(serials_of(1, kImageIdB).empty(), "the unpromoted body ran");
        check_identified(before, kJobs, 2, "promoted");
        check_end_ids(kJobs, [](std::uint32_t j) { return (j % 2) == 0 ? kImageIdA : -1; }, "promoted"); /* B: the interpreted fallback */
    } else {
        CHECK(body_runs().empty(), "%zu registered bodies ran under interp", body_runs().size());
        check_identified(before, 0, 0, "interp");
        check_end_ids(kJobs, [](std::uint32_t) { return -1; }, "promoted interp");
    }
}
void mode_select_promoted() { promoted_scenario(true); }
void mode_select_promoted_interp() { promoted_scenario(false); }

/* A forced body (the test seam of select_body, a title's own choice) wins over spu.job_body=lifted and is not
 * checked against the bytes: the registered body does not run, and nothing is identified. */
void mode_select_forced()
{
    g_lifted = kLiftedNormal;
    build_image(IMAGE, Prog::transform);
    sel_register(fp_at(IMAGE), &marked_body_a, kImageIdA, "sel-a");
    const IdentifyCounts before = identify_counts();
    xd_run(4);
    CHECK(body_runs().empty(), "the registered body ran %zu times with a forced body", body_runs().size());
    check_identified(before, 0, 0, "forced");
    check_end_ids(4, [](std::uint32_t) { return -1; }, "forced");
}

/* The loader's observed path chooses the body too, and the observer still sees every job. */
void mode_select_observer()
{
    constexpr std::uint32_t kJobs = 3;
    build_image(IMAGE, Prog::transform);
    sel_register(fp_at(IMAGE), &marked_body_a, kImageIdA, "sel-a");
    const jq2_job_observer obs{nullptr, &obs_start, &obs_end};
    const IdentifyCounts before = identify_counts();
    xd_run(kJobs, 1, [&obs] { CHECK(jq2_set_job_observer(QUEUE, &obs) == 0, "install observer"); });
    const std::vector<std::uint32_t> want = {0, 1, 2};
    const std::vector<std::uint32_t> ran = serials_of(0, kImageIdA);
    CHECK(ran == want, "the registered body ran jobs %s, wanted %s", serials_text(ran).c_str(), serials_text(want).c_str());
    {
        std::lock_guard<std::mutex> lk(g_obs.mu);
        CHECK(g_obs.starts == int(kJobs) && g_obs.ends == int(kJobs), "observer start/end %d/%d", g_obs.starts, g_obs.ends);
    }
    check_identified(before, kJobs, 1, "observed");
}

/* Two workers, two images, alternating jobs, a barrier at admission so that both workers hold a job at once: each
 * job runs the body of its own image, under its own image id. */
void mode_select_concurrent()
{
    constexpr std::uint32_t kJobs = 24, kWorkers = 2, kImageB = IMAGE + 0x1000;
    build_image(IMAGE, Prog::transform);
    build_variant(kImageB, 1);
    sel_register(fp_at(IMAGE), &marked_body_a, kImageIdA, "sel-a");
    sel_register(fp_at(kImageB), &marked_body_b, kImageIdB, "sel-b");
    CHECK(fp_at(IMAGE) != fp_at(kImageB), "the two images have one fingerprint");

    struct Gate {
        std::mutex mu;
        std::condition_variable cv;
        unsigned arrived = 0;
    } gate;
    const IdentifyCounts before = identify_counts();
    xd_run(
        kJobs, kWorkers,
        [&gate] {
            std::lock_guard<std::mutex> lk(g_probe.mu);
            g_probe.on_admit = [&gate](const JobStart&) {
                std::unique_lock<std::mutex> lk(gate.mu);
                if (gate.arrived >= kWorkers) return;
                ++gate.arrived;
                gate.cv.notify_all();
                gate.cv.wait(lk, [&gate] { return gate.arrived >= kWorkers; });
            };
        },
        [](std::uint32_t j) { return (j % 2) != 0 ? kImageB : std::uint32_t(IMAGE); });
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    CHECK(gate.arrived == kWorkers, "the barrier saw %u workers", gate.arrived);
    std::vector<std::uint32_t> even, odd;
    for (std::uint32_t j = 0; j < kJobs; ++j) (j % 2 != 0 ? odd : even).push_back(j);
    const std::vector<std::uint32_t> ran_a = serials_of(0, kImageIdA), ran_b = serials_of(1, kImageIdB);
    CHECK(ran_a == even, "body A ran jobs %s, wanted %s", serials_text(ran_a).c_str(), serials_text(even).c_str());
    CHECK(ran_b == odd, "body B ran jobs %s, wanted %s", serials_text(ran_b).c_str(), serials_text(odd).c_str());
    const IdentifyCounts now = identify_counts();
    CHECK(now.content_hashes - before.content_hashes == kJobs, "%llu content hashes for %u jobs",
          (unsigned long long)(now.content_hashes - before.content_hashes), kJobs);
    /* Each worker fingerprints each of the two contents at most once. */
    CHECK(now.fingerprints - before.fingerprints >= 2 && now.fingerprints - before.fingerprints <= 2 * kWorkers,
          "%llu fingerprints", (unsigned long long)(now.fingerprints - before.fingerprints));
    check_end_ids(kJobs, [](std::uint32_t j) { return (j % 2) != 0 ? kImageIdB : kImageIdA; }, "concurrent");
}

/* The choice is a worker's own (t_chosen is thread-local): both workers hold a staged job with its body chosen
 * before either job's entry runs, the gate being the observer's job_start, which the start hook calls after the
 * choice and before the entry. Two images alternate; a choice shared between the workers would run one body for both. */
struct StartGate {
    std::mutex mu;
    std::condition_variable cv;
    unsigned arrived = 0;
};
StartGate g_start_gate;
void gate_job_start(void*, spu_context*, const jq2_job_info*)
{
    std::unique_lock<std::mutex> lk(g_start_gate.mu);
    if (g_start_gate.arrived >= 2) return;
    ++g_start_gate.arrived;
    g_start_gate.cv.notify_all();
    g_start_gate.cv.wait(lk, [] { return g_start_gate.arrived >= 2; });
}
void mode_select_concurrent_hook()
{
    constexpr std::uint32_t kJobs = 12, kWorkers = 2, kImageB = IMAGE + 0x1000;
    build_image(IMAGE, Prog::transform);
    build_variant(kImageB, 1);
    sel_register(fp_at(IMAGE), &marked_body_a, kImageIdA, "sel-a");
    sel_register(fp_at(kImageB), &marked_body_b, kImageIdB, "sel-b");
    const jq2_job_observer obs{nullptr, &gate_job_start, nullptr};
    xd_run(
        kJobs, kWorkers, [&obs] { CHECK(jq2_set_job_observer(QUEUE, &obs) == 0, "install observer"); },
        [](std::uint32_t j) { return (j % 2) != 0 ? kImageB : std::uint32_t(IMAGE); });
    CHECK(g_start_gate.arrived == kWorkers, "the gate saw %u workers", g_start_gate.arrived);
    std::vector<std::uint32_t> even, odd;
    for (std::uint32_t j = 0; j < kJobs; ++j) (j % 2 != 0 ? odd : even).push_back(j);
    const std::vector<std::uint32_t> ran_a = serials_of(0, kImageIdA), ran_b = serials_of(1, kImageIdB);
    CHECK(ran_a == even, "body A ran jobs %s, wanted %s", serials_text(ran_a).c_str(), serials_text(even).c_str());
    CHECK(ran_b == odd, "body B ran jobs %s, wanted %s", serials_text(ran_b).c_str(), serials_text(odd).c_str());
    check_end_ids(kJobs, [](std::uint32_t j) { return (j % 2) != 0 ? kImageIdB : kImageIdA; }, "concurrent hook");
}

/* A remembered fingerprint that is not the staged bytes' (a content-hash collision, section 4), under lifted + capture:
 * the capture finds it, and the job still runs the right body, because the executor takes the byte-serial fingerprint
 * of the staged bytes and chooses again before the entry runs. B's bytes are staged while the worker remembers A's
 * fingerprint for them. The job is dropped from the capture and counted a collision; the worker's memory is repaired
 * (one more fingerprint), so the next jobs on B take no fingerprint, run B's body and are recorded under B's. */
void mode_xdiff_select_collision()
{
    constexpr std::uint32_t kJobs = kXdN, kImageB = IMAGE + 0x1000;
    build_image(IMAGE, Prog::transform);
    build_variant(kImageB, 1);
    const std::uint64_t fpa = fp_at(IMAGE), fpb = fp_at(kImageB);
    spu_workload_set_inventory(fpa, 0, 0, 0);
    spu_workload_set_inventory(fpb, 0, 0, 0);
    sel_register(fpa, &marked_body_a, kImageIdA, "sel-a");
    sel_register(fpb, &marked_body_b, kImageIdB, "sel-b");
    set_identify_fingerprint_override(fpa);
    const IdentifyCounts before = identify_counts();
    const std::string log = capture_stderr([&] {
        const std::vector<JobSpec> specs = xd_run(kJobs, 1, {}, [](std::uint32_t) { return kImageB; });
        CHECK(xd_files() == xd_names(fpb, 2, kJobs - 1), "fixtures: %s", join(xd_files()).c_str());
        for (std::uint32_t seq = 2; seq <= kJobs; ++seq) {
            Fx fx;
            if (!xd_read(xd_name(fpb, 0, seq), fx)) continue;
            xd_check_job(fx.f, specs[seq - 1], seq, SPU_TIER_LIFTED, kImageB);
        }
    });
    CHECK(count_of(log, "reason=fingerprint-collision") == 1, "%zu collision lines:\n%s", count_of(log, "reason=fingerprint-collision"),
          log.c_str());
    std::vector<std::uint32_t> all(kJobs);
    for (std::uint32_t j = 0; j < kJobs; ++j) all[j] = j;
    CHECK(serials_of(0, kImageIdA).empty(), "A's body ran a job staged with B's bytes");
    const std::vector<std::uint32_t> ran_b = serials_of(1, kImageIdB);
    CHECK(ran_b == all, "B's body ran jobs %s, wanted %s", serials_text(ran_b).c_str(), serials_text(all).c_str());
    check_end_ids(kJobs, [](std::uint32_t) { return kImageIdB; }, "collision");
    /* The wrong fingerprint and its repair for job 0; jobs 1 and 2 hit the repaired memory. */
    check_identified(before, kJobs, 2, "collision");
    const auto a = capture_counters(fpa);
    CHECK(a && a->ineligible_collision == 1, "%llu collisions counted on A", (unsigned long long)(a ? a->ineligible_collision : 0));
    const auto b = capture_counters(fpb);
    CHECK(b && b->captured == kJobs - 1 && b->failed == 0, "B: captured %llu", (unsigned long long)(b ? b->captured : 0));
}

/* spu.job_body=lifted with capture: the registered body is the authority T1 of every fixture, named with the
 * registration's lift identity, the interpreter replays each transcript equal, and the one identification of a job
 * serves the choice and the capture both. */
void mode_xdiff_select_lifted()
{
    constexpr std::uint32_t kCodeEnd = 0x8000, kJobs = kXdN + 1;
    constexpr std::uint64_t kOptions = 0x1122334455667788ull;
    build_image(IMAGE, Prog::transform);
    xd_register(0, kCodeEnd, kOptions);
    sel_register(fp_at(IMAGE), &marked_body_a, kImageIdA, "sel-a");
    const std::uint64_t fp = fp_at(IMAGE);
    const IdentifyCounts before = identify_counts();
    const std::uint64_t misses_before = capture_unattributed().fingerprint_cache_misses;
    const std::vector<JobSpec> specs = xd_run(kJobs);
    std::vector<std::uint32_t> all(kJobs);
    for (std::uint32_t j = 0; j < kJobs; ++j) all[j] = j;
    const std::vector<std::uint32_t> ran = serials_of(0, kImageIdA);
    CHECK(ran == all, "the registered body ran jobs %s, wanted %s", serials_text(ran).c_str(), serials_text(all).c_str());
    CHECK(xd_files() == xd_names(fp, 1, kXdN), "fixtures: %s", join(xd_files()).c_str());
    const auto c = capture_counters(fp);
    CHECK(c && c->captured == kXdN && c->failed == 0 && c->ineligible == 0 && c->ineligible_unknown_body == 0,
          "counters: captured %llu ineligible %llu", (unsigned long long)(c ? c->captured : 0),
          (unsigned long long)(c ? c->ineligible : 0));
    for (std::uint32_t seq = 1; seq <= kXdN; ++seq) {
        Fx fx;
        if (!xd_read(xd_name(fp, 0, seq), fx)) continue;
        xd_check_job(fx.f, specs[seq - 1], seq, SPU_TIER_LIFTED);
        const spu_xdiff_body_id& b = fx.f.identity.authority;
        CHECK(b.lifter_abi == SPU_RUNTIME_LIFTER_ABI && b.code_end == kCodeEnd && b.lift_options == kOptions,
              "lift identity abi %u code_end 0x%X options 0x%llX", b.lifter_abi, b.code_end, (unsigned long long)b.lift_options);
        CHECK(fx.f.end.end_class == SPU_XDIFF_END_RETURN, "end class %u", fx.f.end.end_class);
        /* No replay on the interpreter: the registered body here is hand-written, and ends with other registers than
         * the interpreter does. The transcript is what xd_check_job checked. */
    }
    check_identified(before, kJobs, 1, "lifted + capture");
    CHECK(capture_unattributed().fingerprint_cache_misses - misses_before == 1, "the capture counted %llu fingerprints",
          (unsigned long long)(capture_unattributed().fingerprint_cache_misses - misses_before));
}

/* The tier names the body that ran; a registered lift that reaches code its lift does not hold is still `lifted`, and
 * lift_miss beside it carries how many arrivals the resolver served in the interpreter. Kind 32 has both fields
 * (ps3_ftrace.h), so both records come from one run: odd jobs run the body with the unlifted stub, even ones the fully
 * lifted body. */
void mode_xdiff_select_miss()
{
    constexpr std::uint32_t kJobs = 4, kImageB = IMAGE + 0x1000;
    build_image(IMAGE, Prog::transform);
    build_variant(kImageB, 1); /* other bytes, so the two images are two registrations and two fingerprints */
    sel_register(fp_at(IMAGE), &lifted_miss_entry, kImageIdA, "miss-stub");
    sel_register(fp_at(kImageB), &lifted_normal_entry, kImageIdB, "miss-none");
    CHECK(PS3_FT_ON(), "tracing is on");
    const std::vector<JobSpec> specs =
        xd_run(kJobs, 1, {}, [](std::uint32_t j) { return (j % 2) == 0 ? std::uint32_t(IMAGE) : kImageB; });
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);

    const std::vector<ps3_ft_rec> bodies = ft_kind(PS3_FT_JQ2_BODY);
    CHECK(bodies.size() == kJobs, "%zu kind-32 records for %u jobs", bodies.size(), kJobs);
    std::map<std::uint32_t, ps3_ft_rec> by_seq;
    for (const ps3_ft_rec& r : bodies) by_seq[r.a] = r;
    for (std::uint32_t seq = 1; seq <= kJobs; ++seq) {
        const auto it = by_seq.find(seq);
        CHECK(it != by_seq.end(), "no kind 32 for seq %u", seq);
        if (it == by_seq.end()) continue;
        const ps3_ft_rec& r = it->second;
        const bool stub = (seq % 2) == 1;
        const std::uint32_t tier = r.b & 0xFFu, miss = (r.b >> 16) & 0xFFu;
        CHECK(tier == std::uint32_t{SPU_TIER_LIFTED}, "seq %u: tier %u, wanted lifted", seq, tier);
        CHECK(stub ? miss >= 1 : miss == 0, "seq %u: lift_miss %u, wanted %s", seq, miss, stub ? "at least 1" : "0");
        CHECK(((r.b >> 8) & 0xFFu) == std::uint32_t{kNotSampled}, "seq %u: disposition %u", seq, (r.b >> 8) & 0xFFu);
    }
}

/* All three consumers of a job's identity at once (spu.job_body=lifted, capture, kind-29 tracing): still one content
 * hash a job and one fingerprint for the one content. The IMAGE span keeps measuring that identification; its mean and
 * the JOB span's are printed for the report (nanoseconds). */
void mode_xdiff_select_trace()
{
    constexpr std::uint32_t kJobs = kXdN + 3;
    build_image(IMAGE, Prog::transform);
    xd_register(0);
    sel_register(fp_at(IMAGE), &marked_body_a, kImageIdA, "sel-a");
    CHECK(PS3_FT_ON(), "tracing is on");
    const IdentifyCounts before = identify_counts();
    xd_run(kJobs);
    const std::vector<std::uint32_t> idx = check_image_spans(0, kJobs);
    for (std::uint32_t j = 0; j < kJobs; ++j) CHECK(idx[j] == idx[0], "job %u has image index %u, job 0 has %u", j, idx[j], idx[0]);
    const ImageTable table = ft_images();
    const ImageEntry* e = entry_of(table, idx[0]);
    CHECK(table.size() == 1 && e != nullptr && e->fp == fp_at(IMAGE), "the image table");
    check_identified(before, kJobs, 1, "lifted + capture + trace");
    CHECK(serials_of(0, kImageIdA).size() == kJobs, "the registered body ran %zu of %u jobs", serials_of(0, kImageIdA).size(), kJobs);

    unsigned long long image_sum = 0, job_sum = 0;
    const SeqMap job = by_seq(PS3_FT_JQ2_JOB, 0), image = by_seq(PS3_FT_JQ2_IMAGE, 0);
    for (const auto& kv : image)
        for (const ps3_ft_rec& r : kv.second) image_sum += r.t1 - r.t0;
    for (const auto& kv : job)
        for (const ps3_ft_rec& r : kv.second) job_sum += r.t1 - r.t0;
    std::printf("kind 29: %u jobs, mean IMAGE span %llu ns, mean JOB span %llu ns\n", kJobs, image_sum / kJobs, job_sum / kJobs);
}

/* ---- in-run xdiff: sampled shadows on an audit thread (docs/SPU_XDIFF.md section 16, commit 10b) ------------------ */

/* The bodies. The interpreter wrapped as a registered body is the cheapest T1 that equals T0 in every effect, register
 * and local-store byte; each of the others differs from it in one thing. Registered as a body they are T1 authorities
 * (spu.job_body = lifted); registered as a twin they are T1 shadows of an interpreted job. */
void au_interp_body(spu_context* c)
{
    const std::int32_t saved = c->image_id;
    c->image_id = -1; /* pure interpretation: no rejoin of another image's lifted functions */
    spu_interp_run(c, c->pc);
    c->image_id = saved;
}

/* The transform program with `extra` added to the first word of its sum: the PUT's payload differs from the
 * interpreter's, which a world sees. */
void au_transform(spu_context* c, std::uint32_t extra)
{
    const std::uint32_t d = c->gpr[4]._u32[0];
    const std::uint32_t out = ls_word(c, d + 0x30), in = ls_word(c, d + 0x34);
    const std::uint32_t k[4] = {ls_word(c, d + 0x38) + extra, ls_word(c, d + 0x3C), out, in};
    lifted_dma(c, kLsIn, in, MFC_GET_CMD);
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint32_t v = be32(ls_word(c, kLsIn + 4 * i) + k[i]);
        std::memcpy(c->ls + kLsOut + 4 * i, &v, 4);
    }
    lifted_dma(c, kLsOut, out, MFC_PUT_CMD);
    SPU_RET(c);
}
void au_bad_body(spu_context* c) { au_transform(c, 1); }

/* The interpreter, then one store into local store the loader destroys: every effect equals T0's and only the end
 * state differs, which under the JobQueue2 profile is a latent difference (section 8.4). */
constexpr std::uint32_t kAuLatentLs = 0x30000;
void au_store_body(spu_context* c)
{
    au_interp_body(c);
    c->ls[kAuLatentLs] ^= 0x5A;
}

/* A body that raises a named stop: a subject is code under test, and a stop it reaches belongs to its sample. */
void au_stop_body(spu_context*)
{
    ps3_stop(PS3_STOP_JQ2_STATE, "a test body raised a stop inside a shadow", "none: this is a test", nullptr, 0, nullptr);
}

/* The interpreter and a count of branches no lifted code covered, past the one byte kind 32 gives it. */
void au_miss_body(spu_context* c)
{
    au_interp_body(c);
    c->lift_miss = 300;
}

/* An image's registration the way a title's generated one makes it: its inventory (no flags: nothing refuses it), its
 * body, its budgeted twin (a lifted shadow needs one), and promotion for the images job_body=lifted may run. */
void au_register(std::uint32_t ea, spu_lifted_entry_fn body, spu_lifted_entry_fn twin, std::int32_t id = 40, bool promote = false,
                 std::uint32_t flags = 0)
{
    const std::uint64_t fp = fp_at(ea);
    spu_workload_set_inventory(fp, flags, 0, 0);
    if (body != nullptr) {
        spu_workload_register_img(fp, body, id, "au-body");
        if (promote) spu_workload_promote_img(fp);
    }
    if (twin != nullptr) spu_workload_register_twin(fp, twin, id + 100);
}

/* What the audit hooks leave for a test, so that the audit thread and the workers are ordered by hooks and never by
 * sleeping. `hold` parks the audit thread at on_start; `started` counts the samples it took, `waiting` the workers that
 * found the queue full under `all`, `done` the verdicts. */
struct AuditProbe {
    std::mutex mu;
    std::condition_variable cv;
    bool hold = false;
    unsigned started = 0, waiting = 0;
    struct Done {
        std::uint64_t fp;
        std::uint32_t queue, seq, verdict, reason;
    };
    std::vector<Done> done;
    std::function<void(const AuditEvent&, std::uint32_t, std::uint32_t)> extra; /* run after a verdict, without the lock */
};
AuditProbe g_ap;

void ap_start(void*, const AuditEvent&)
{
    std::unique_lock<std::mutex> lk(g_ap.mu);
    ++g_ap.started;
    g_ap.cv.notify_all();
    g_ap.cv.wait(lk, [] { return !g_ap.hold; });
}
void ap_wait(void*, const AuditEvent&)
{
    std::lock_guard<std::mutex> lk(g_ap.mu);
    ++g_ap.waiting;
    g_ap.cv.notify_all();
}
void ap_done(void*, const AuditEvent& ev, std::uint32_t verdict, std::uint32_t reason)
{
    std::function<void(const AuditEvent&, std::uint32_t, std::uint32_t)> extra;
    {
        std::lock_guard<std::mutex> lk(g_ap.mu);
        g_ap.done.push_back({ev.fingerprint, ev.queue, ev.seq, verdict, reason});
        extra = g_ap.extra;
        g_ap.cv.notify_all();
    }
    if (extra) extra(ev, verdict, reason);
}
template <class P>
void ap_wait_for(P pred, const char* what)
{
    std::unique_lock<std::mutex> lk(g_ap.mu);
    CHECK(g_ap.cv.wait_for(lk, std::chrono::seconds(60), pred), "timed out waiting for %s", what);
}
void ap_hold(bool on)
{
    {
        std::lock_guard<std::mutex> lk(g_ap.mu);
        g_ap.hold = on;
    }
    g_ap.cv.notify_all();
}
void ap_wait_done(std::size_t n) { ap_wait_for([n] { return g_ap.done.size() >= n; }, "the verdicts"); }

/* Hooks in place, the audit's seams installed. */
void au_begin()
{
    install_hooks();
    {
        std::lock_guard<std::mutex> lk(g_probe.mu);
        g_probe.on_admit = nullptr;
    }
    AuditHooks h;
    h.on_wait = &ap_wait;
    h.on_start = &ap_start;
    h.on_done = &ap_done;
    set_audit_hooks(h);
}

/* One job on the image at `ea`, pushed alone and waited out: the guest has its result when this returns. */
JobSpec au_job(Rig& rig, std::uint32_t j, std::uint32_t ea = IMAGE)
{
    const JobSpec s = spec(j, ea);
    rig.push_wait(s.desc, 64, 0);
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync after job %u", j);
    return s;
}

/* The transform's output with `extra` added to the first word of the sum. */
bool au_out_is(const JobSpec& s, std::uint32_t extra)
{
    const std::uint32_t k[4] = {s.add + extra, s.serial, s.out, s.in};
    for (std::uint32_t i = 0; i < 4; ++i)
        if (get32(s.out + 4 * i) != get32(s.in + 4 * i) + k[i]) return false;
    return true;
}

std::vector<std::string> au_lines(const std::string& log, const std::string& needle)
{
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at < log.size()) {
        std::size_t end = log.find('\n', at);
        if (end == std::string::npos) end = log.size();
        const std::string line = log.substr(at, end - at);
        if (line.find(needle) != std::string::npos) lines.push_back(line);
        at = end + 1;
    }
    return lines;
}

/* The audit's counters of `fp`, or zeros when it has none (checked by the caller). */
AuditCounters au_counters(std::uint64_t fp)
{
    const auto c = audit_counters(fp);
    CHECK(c.has_value(), "the audit counted nothing for image 0x%016llX", (unsigned long long)fp);
    return c.value_or(AuditCounters{});
}

constexpr std::uint32_t kAuRec = 0xE00100; /* what a forked child tells its parent, in the shared arena */

/* A seeded divergence: the interpreter is the authority and the image's registered body (and twin) adds one to the
 * first word of the sum. The difference is reported once with its class, its fixture is written and replays `differ`
 * offline, and the guest sees exactly what a run without xdiff leaves. */
void mode_audit_divergence()
{
    auto* digest = static_cast<std::uint64_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    CHECK(digest != MAP_FAILED, "mmap");
    if (digest == MAP_FAILED) return;
    std::string child_log;
    const int rc = run_child(
        [digest] {
            setenv("PS3RECOMP_SPU_XDIFF", "off", 1); /* the same jobs with the audit off, before anything read the configuration */
            au_begin();
            build_image(IMAGE, Prog::transform);
            Rig rig({128, 1, false});
            au_job(rig, 0);
            rig.teardown();
            *digest = xd_arena_digest();
            if (g_fail != 0) _exit(1);
        },
        child_log);
    CHECK(rc == 0, "the reference run without xdiff failed (status %d): %s", rc, child_log.c_str());
    CHECK(child_log.find("[jq2-xdiff]") == std::string::npos, "xdiff off said something: %s", child_log.c_str());
    std::memset(vm_base, 0, kMem);

    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_bad_body, &au_bad_body);
    const std::uint64_t fp = fp_at(IMAGE);
    JobSpec s{};
    const std::string log = capture_stderr([&] {
        au_begin();
        build_image(IMAGE, Prog::transform);
        Rig rig({128, 1, false});
        s = au_job(rig, 0);
        ap_wait_done(1);
        rig.teardown();
    });
    CHECK(job_ok(s), "the guest sees the interpreter's output");
    CHECK(xd_arena_digest() == *digest, "guest memory with the audit on differs from the run without it");

    const auto lines = au_lines(log, "[jq2-xdiff] differ ");
    CHECK(lines.size() == 1, "%zu differ lines, wanted one:\n%s", lines.size(), log.c_str());
    const std::string path = g_xd_dir + "/" + xd_name(fp, 0, 1);
    if (lines.size() == 1) {
        const std::string& l = lines[0];
        CHECK(l.find("image=" + xd_fp_hex(fp) + " queue=0 seq=1 reason=effect-payload class=visible effect=") != std::string::npos,
              "the line: %s", l.c_str());
        CHECK(l.find(" where=0x") != std::string::npos, "the line names where: %s", l.c_str());
        CHECK(l.find(" authority=interp subject=lifted fixture=" + path) != std::string::npos, "the line: %s", l.c_str());
    }
    const AuditCounters c = au_counters(fp);
    CHECK(c.jobs == 1 && c.sampled == 1 && c.queued == 1 && c.dropped == 0 && c.shadowed == 1, "counts jobs %llu sampled %llu queued %llu",
          (unsigned long long)c.jobs, (unsigned long long)c.sampled, (unsigned long long)c.queued);
    CHECK(c.differ == 1 && c.differ_visible == 1 && c.differ_latent == 0 && c.equal == 0 && c.voided == 0, "differ %llu visible %llu",
          (unsigned long long)c.differ, (unsigned long long)c.differ_visible);
    CHECK(c.reason[SPU_XDIFF_R_EFFECT_PAYLOAD] == 1 && c.fixtures == 1 && c.fixtures_over == 0, "payload %llu fixtures %llu",
          (unsigned long long)c.reason[SPU_XDIFF_R_EFFECT_PAYLOAD], (unsigned long long)c.fixtures);

    Fx fx;
    CHECK(xd_files() == std::vector<std::string>{xd_name(fp, 0, 1)}, "fixtures: %s", join(xd_files()).c_str());
    if (xd_read(xd_name(fp, 0, 1), fx)) {
        CHECK(fx.f.has_verdict != 0 && fx.f.verdict.verdict == SPU_XDIFF_DIFFER && fx.f.verdict.reason == SPU_XDIFF_R_EFFECT_PAYLOAD,
              "the fixture's VRDT: verdict %u reason %u", fx.f.verdict.verdict, fx.f.verdict.reason);
        CHECK(fx.f.verdict.subject_body.tier == SPU_TIER_LIFTED && fx.f.identity.authority.tier == SPU_TIER_INTERP, "the tiers");
        xd_check_job(fx.f, s, 1, SPU_TIER_INTERP);
    }
    /* Offline: the runner's own entry, on the registered body, and on the interpreter. */
    std::string p = path;
    char a0[] = "spu_job_replay", a1[] = "--tier", a2[] = "registered", a3[] = "interp";
    char* registered[] = {a0, a1, a2, p.data()};
    char* interp[] = {a0, a1, a3, p.data()};
    std::fflush(stdout);
    CHECK(spu_job_replay_main(4, registered) == int(SPU_XDIFF_DIFFER), "spu_job_replay on the registered body is not `differ`");
    CHECK(spu_job_replay_main(4, interp) == int(SPU_XDIFF_EQUAL), "spu_job_replay on the interpreter is not `equal`");
}

/* A latent difference is reported with its class, counted and written, and stops nothing under
 * on_divergence=stop (on_latent stays report): the body differs from the interpreter in one store of destroyed state. */
void mode_audit_latent()
{
    constexpr std::uint32_t kJobs = 2;
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_store_body, &au_store_body);
    const std::uint64_t fp = fp_at(IMAGE);
    std::vector<JobSpec> specs;
    const std::string log = capture_stderr([&] {
        au_begin();
        build_image(IMAGE, Prog::transform);
        Rig rig({128, 1, false});
        for (std::uint32_t j = 0; j < kJobs; ++j) specs.push_back(au_job(rig, j));
        ap_wait_done(kJobs);
        rig.teardown();
    });
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    const auto lines = au_lines(log, "[jq2-xdiff] differ ");
    CHECK(lines.size() == kJobs, "%zu differ lines:\n%s", lines.size(), log.c_str());
    for (const std::string& l : lines)
        CHECK(l.find("reason=end-ls class=latent effect=none where=0x30000") != std::string::npos, "the line: %s", l.c_str());
    const AuditCounters c = au_counters(fp);
    CHECK(c.differ == kJobs && c.differ_latent == kJobs && c.differ_visible == 0 && c.reason[SPU_XDIFF_R_END_LS] == kJobs &&
              c.fixtures == kJobs,
          "differ %llu latent %llu end-ls %llu fixtures %llu", (unsigned long long)c.differ, (unsigned long long)c.differ_latent,
          (unsigned long long)c.reason[SPU_XDIFF_R_END_LS], (unsigned long long)c.fixtures);
    Fx fx;
    if (xd_read(xd_name(fp, 0, 1), fx))
        CHECK(fx.f.has_verdict != 0 && spu_xdiff_differ_class_of(fx.f.verdict.reason, fx.f.unit.profile) == SPU_XDIFF_CLASS_LATENT,
              "the fixture's verdict is a latent end-ls difference");
}

/* A stop of the run: the audit thread ends it with the new kind after the job was credited, with the fields of
 * docs/STOPS.md, the fixture already written. `latent`: the body differs only in destroyed state and on_latent says stop
 * (on_divergence stays report); otherwise a visible difference under on_divergence=stop. */
void audit_stop_scenario(bool latent)
{
    std::memset(vm_base + kAuRec, 0, 16);
    build_image(IMAGE, Prog::transform);
    const std::uint64_t fp = fp_at(IMAGE);
    const std::function<void()> child = [latent] {
        arm_child_watchdog(120);
        build_image(IMAGE, Prog::transform);
        au_register(IMAGE, latent ? &au_store_body : &au_bad_body, latent ? &au_store_body : &au_bad_body);
        au_begin();
        {
            std::lock_guard<std::mutex> lk(g_ap.mu);
            g_ap.extra = [](const AuditEvent&, std::uint32_t, std::uint32_t) {
                /* On the audit thread, after the verdict and before the stop: what the queue had credited by then. */
                jq2_stats st{};
                jq2_queue_stats(QUEUE, &st);
                put32(kAuRec, std::uint32_t(st.credited));
                put32(kAuRec + 4, std::uint32_t(st.executed));
                put32(kAuRec + 8, 1);
            };
        }
        Rig rig({128, 1, false});
        const JobSpec s = spec(0);
        rig.push(s.desc, 64, 0);
        jq2_port_sync(PORT, 0);
        for (;;) pause(); /* the audit thread ends the process */
    };
    std::string err;
    const int rc = run_child(child, err);
    CHECK(rc == 70, "exit status %d, wanted 70 (a named stop)", rc);
    const std::string path = g_xd_dir + "/" + xd_name(fp, 0, 1);
    const std::string want = std::string("PS3STOP v=1 kind=spu-xdiff-divergence image=") + xd_fp_hex(fp) +
                             " reason=" + (latent ? "end-ls" : "effect-payload") + " effect=" + (latent ? "none" : "") ;
    CHECK(err.find(want) != std::string::npos, "stderr carries the stop line:\n%s", err.c_str());
    CHECK(err.find(" authority=interp subject=lifted queue=0 seq=1 fixture=" + path) != std::string::npos,
          "the stop names tiers, queue, seq and fixture:\n%s", err.c_str());
    CHECK(err.find("the stop did not happen") == std::string::npos, "the child ran past the stop");
    CHECK(get32(kAuRec + 8) == 1 && get32(kAuRec) == 1 && get32(kAuRec + 4) == 1, "at the verdict the queue had executed %u and credited %u jobs",
          get32(kAuRec + 4), get32(kAuRec));
    CHECK(au_out_is(JobSpec{DESC, IN, OUT, 1, 0}, 0), "the guest has the authority's output");
    Fx fx;
    CHECK(xd_files() == std::vector<std::string>{xd_name(fp, 0, 1)}, "fixtures: %s", join(xd_files()).c_str());
    if (xd_read(xd_name(fp, 0, 1), fx)) CHECK(fx.f.has_verdict != 0, "the fixture carries its verdict");
}
void mode_audit_stop() { audit_stop_scenario(false); }
void mode_audit_stop_latent() { audit_stop_scenario(true); }

/* `sample` never waits: with the audit thread held at its first sample, eight more queue and the tenth is dropped and
 * counted while its job completes; released, every queued sample is shadowed and equal. */
void mode_audit_full()
{
    constexpr std::uint32_t kQueue = 8;
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_interp_body, &au_interp_body);
    const std::uint64_t fp = fp_at(IMAGE);
    au_begin();
    ap_hold(true);
    Rig rig({128, 1, false});
    std::vector<JobSpec> specs;
    specs.push_back(au_job(rig, 0));
    ap_wait_for([] { return g_ap.started >= 1; }, "the audit thread to take the first sample");
    for (std::uint32_t j = 1; j <= kQueue; ++j) specs.push_back(au_job(rig, j));
    CHECK(audit_queued() == kQueue, "%zu samples queued, wanted %u", audit_queued(), kQueue);
    specs.push_back(au_job(rig, kQueue + 1)); /* the queue is full: this job completes, its sample is dropped */
    AuditCounters c = au_counters(fp);
    CHECK(c.sampled == kQueue + 2 && c.queued == kQueue + 1 && c.dropped == 1 && c.shadowed == 0,
          "sampled %llu queued %llu dropped %llu shadowed %llu", (unsigned long long)c.sampled, (unsigned long long)c.queued,
          (unsigned long long)c.dropped, (unsigned long long)c.shadowed);
    ap_hold(false);
    ap_wait_done(kQueue + 1);
    rig.teardown();
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    c = au_counters(fp);
    CHECK(c.shadowed == kQueue + 1 && c.equal == kQueue + 1 && c.differ == 0 && c.dropped == 1, "shadowed %llu equal %llu dropped %llu",
          (unsigned long long)c.shadowed, (unsigned long long)c.equal, (unsigned long long)c.dropped);
    std::set<std::uint32_t> seqs;
    for (const auto& d : g_ap.done) seqs.insert(d.seq);
    CHECK(seqs.size() == kQueue + 1 && seqs.count(kQueue + 2) == 0, "the dropped job (seq %u) was shadowed", kQueue + 2);
}

/* `all` waits for room: the audit thread held at its first sample, eight more fill the queue, and the worker that has
 * the tenth parks in its hand-over (on_wait) after crediting it, so the eleventh job cannot start until the audit
 * thread is released; then every one of the eleven is shadowed. Ordered by hooks: the audit thread alone can make
 * room, and it is held. */
void mode_audit_all()
{
    constexpr std::uint32_t kQueue = 8;
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_interp_body, &au_interp_body);
    const std::uint64_t fp = fp_at(IMAGE);
    au_begin();
    ap_hold(true);
    Rig rig({128, 1, false});
    std::vector<JobSpec> specs;
    specs.push_back(au_job(rig, 0));
    ap_wait_for([] { return g_ap.started >= 1; }, "the audit thread to take the first sample");
    for (std::uint32_t j = 1; j <= kQueue; ++j) specs.push_back(au_job(rig, j));
    CHECK(audit_queued() == kQueue, "%zu samples queued, wanted %u", audit_queued(), kQueue);
    specs.push_back(au_job(rig, kQueue + 1)); /* credited; its worker now waits for room */
    ap_wait_for([] { return g_ap.waiting >= 1; }, "the worker to wait for queue room");
    specs.push_back(spec(kQueue + 2));
    rig.push_wait(specs.back().desc, 64, 0); /* the only worker is parked: this job is admitted when the audit thread makes room */
    AuditCounters c = au_counters(fp);
    CHECK(c.dropped == 0 && c.queued == kQueue + 1, "dropped %llu queued %llu while the worker waits", (unsigned long long)c.dropped,
          (unsigned long long)c.queued);
    ap_hold(false);
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    ap_wait_done(kQueue + 3);
    rig.teardown();
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    c = au_counters(fp);
    CHECK(c.sampled == kQueue + 3 && c.queued == kQueue + 3 && c.dropped == 0 && c.shadowed == kQueue + 3 && c.equal == kQueue + 3,
          "sampled %llu queued %llu dropped %llu shadowed %llu equal %llu", (unsigned long long)c.sampled, (unsigned long long)c.queued,
          (unsigned long long)c.dropped, (unsigned long long)c.shadowed, (unsigned long long)c.equal);
}

/* `all` under three workers: every job is shadowed, none is dropped, every verdict is equal. For the sanitizers. */
void mode_audit_concurrent()
{
    constexpr std::uint32_t kJobs = 36;
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_interp_body, &au_interp_body);
    const std::uint64_t fp = fp_at(IMAGE);
    au_begin();
    Rig rig({128, 3, false});
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < kJobs; ++j) {
        specs.push_back(spec(j));
        rig.push_wait(specs.back().desc, 64, 0);
    }
    CHECK(jq2_port_sync(PORT, 0) == 0, "sync");
    ap_wait_done(kJobs);
    rig.teardown();
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    const AuditCounters c = au_counters(fp);
    CHECK(c.jobs == kJobs && c.queued == kJobs && c.dropped == 0 && c.shadowed == kJobs && c.equal == kJobs,
          "jobs %llu queued %llu dropped %llu shadowed %llu equal %llu", (unsigned long long)c.jobs, (unsigned long long)c.queued,
          (unsigned long long)c.dropped, (unsigned long long)c.shadowed, (unsigned long long)c.equal);
}

/* A recording over the bound is a void / capture-limit, counted and named once, never compared and never a stop (the
 * options say stop for both classes). Two ways over: a payload that does not fit the recorder's share of the bound
 * (the recording stops), and a transcript that fits the recorder and not the queue (the finished recording is refused). */
void mode_audit_oversize()
{
    constexpr std::uint32_t kJobs = 3;
    constexpr std::uint32_t kFixed = 2 * SPU_XDIFF_LS_BYTES + 2 * SPU_XDIFF_REG_BYTES;
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_interp_body, &au_interp_body);
    const std::uint64_t fp = fp_at(IMAGE);
    std::vector<JobSpec> specs;
    std::string log = capture_stderr([&] {
        au_begin();
        Rig rig({128, 1, false});
        set_audit_sample_limit(kFixed); /* no payload fits */
        for (std::uint32_t j = 0; j < kJobs; ++j) specs.push_back(au_job(rig, j));
        AuditCounters c = au_counters(fp);
        CHECK(c.voided == kJobs && c.reason[SPU_XDIFF_R_CAPTURE_LIMIT] == kJobs && c.not_eligible == kJobs && c.queued == 0 && c.shadowed == 0,
              "early: voided %llu capture-limit %llu not eligible %llu queued %llu", (unsigned long long)c.voided,
              (unsigned long long)c.reason[SPU_XDIFF_R_CAPTURE_LIMIT], (unsigned long long)c.not_eligible, (unsigned long long)c.queued);
        set_audit_sample_limit(kFixed + 200); /* the payloads fit, the ten transcript records (400 bytes) do not */
        for (std::uint32_t j = kJobs; j < 2 * kJobs; ++j) specs.push_back(au_job(rig, j));
        c = au_counters(fp);
        CHECK(c.voided == 2 * kJobs && c.reason[SPU_XDIFF_R_CAPTURE_LIMIT] == 2 * kJobs && c.not_eligible == 2 * kJobs && c.queued == 0,
              "late: voided %llu capture-limit %llu queued %llu", (unsigned long long)c.voided,
              (unsigned long long)c.reason[SPU_XDIFF_R_CAPTURE_LIMIT], (unsigned long long)c.queued);
        set_audit_sample_limit(SPU_XDIFF_MAX_LIVE_SAMPLE_BYTES);
        specs.push_back(au_job(rig, 2 * kJobs)); /* a sample inside the bound still goes through */
        ap_wait_done(1);
        rig.teardown();
    });
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    const AuditCounters c = au_counters(fp);
    CHECK(c.shadowed == 1 && c.equal == 1 && c.sampled == 2 * kJobs + 1, "shadowed %llu equal %llu sampled %llu", (unsigned long long)c.shadowed,
          (unsigned long long)c.equal, (unsigned long long)c.sampled);
    const auto lines = au_lines(log, "[jq2-xdiff] void ");
    CHECK(lines.size() == 1 && lines[0].find("image=" + xd_fp_hex(fp) + " ") != std::string::npos &&
              lines[0].find("reason=capture-limit") != std::string::npos,
          "%zu void lines:\n%s", lines.size(), log.c_str());
    CHECK(au_lines(log, "PS3STOP").empty(), "a void stopped the run");
    /* Effect-heavy against the bound: each store grew to at most the live limit (kFixed + 200 left 200 bytes: five
     * records, a 200-byte payload store), never to the doubling's 64 records or 4 KiB, and was freed with the failed
     * recording. */
    CHECK(capture_failed_peak_capacity() <= 2 * 200, "a failed recording had grown to %llu bytes, past its bound of 400",
          (unsigned long long)capture_failed_peak_capacity());
}

/* Shutdown drops what is queued and counts it, lets the shadow in flight finish and never waits for it; a sample handed
 * over afterwards is dropped and counted. */
void mode_audit_shutdown()
{
    constexpr std::uint32_t kQueue = 4;
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_interp_body, &au_interp_body);
    const std::uint64_t fp = fp_at(IMAGE);
    au_begin();
    ap_hold(true);
    Rig rig({128, 1, false});
    std::vector<JobSpec> specs;
    specs.push_back(au_job(rig, 0));
    ap_wait_for([] { return g_ap.started >= 1; }, "the audit thread to take the first sample");
    for (std::uint32_t j = 1; j <= kQueue; ++j) specs.push_back(au_job(rig, j));
    CHECK(audit_queued() == kQueue, "%zu samples queued", audit_queued());
    audit_shutdown(); /* returns although a shadow is held in flight */
    CHECK(audit_queued() == 0, "shutdown left %zu samples queued", audit_queued());
    AuditCounters c = au_counters(fp);
    CHECK(c.queued == kQueue + 1 && c.shutdown_dropped == kQueue && c.shadowed == 0, "queued %llu shutdown-dropped %llu shadowed %llu",
          (unsigned long long)c.queued, (unsigned long long)c.shutdown_dropped, (unsigned long long)c.shadowed);
    specs.push_back(au_job(rig, kQueue + 1)); /* handed over after shutdown */
    c = au_counters(fp);
    CHECK(c.dropped == 1 && c.queued == kQueue + 1, "dropped %llu queued %llu after shutdown", (unsigned long long)c.dropped,
          (unsigned long long)c.queued);
    ap_hold(false);
    ap_wait_done(1);
    rig.teardown();
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    c = au_counters(fp);
    CHECK(c.shadowed == 1 && c.equal == 1 && c.shutdown_dropped == kQueue, "the shadow in flight finished: shadowed %llu", (unsigned long long)c.shadowed);
    audit_shutdown(); /* twice is harmless */
    CHECK(g_ap.done.size() == 1, "%zu shadows ran after shutdown", g_ap.done.size());
}

/* A job the audit cannot shadow is counted and named once: an image with a body and no budgeted twin (an unbounded lifted
 * shadow could hang the audit thread), one with an inventory and no body, one with no inventory, one with event branches.
 * None is queued, shadowed or recorded for the audit, and all run. */
void mode_audit_ineligible()
{
    constexpr std::uint32_t kJobs = 3;
    const std::uint32_t ea[4] = {IMAGE, IMAGE + 0x1000, IMAGE + 0x2000, IMAGE + 0x3000};
    build_image(ea[0], Prog::transform);
    for (std::uint8_t v = 1; v < 4; ++v) build_variant(ea[v], v);
    au_register(ea[0], &au_interp_body, nullptr, 40);              /* a body, no twin */
    au_register(ea[1], nullptr, nullptr);                          /* an inventory, no body */
    /* ea[2]: no inventory at all */
    au_register(ea[3], &au_interp_body, &au_interp_body, 60, false, SPU_WORKLOAD_INV_EVENT_BRANCHES);
    std::uint64_t fp[4];
    for (int i = 0; i < 4; ++i) fp[i] = fp_at(ea[i]);
    std::vector<JobSpec> specs;
    const std::string log = capture_stderr([&] {
        au_begin();
        Rig rig({128, 1, false});
        for (std::uint32_t j = 0; j < 4 * kJobs; ++j) specs.push_back(au_job(rig, j, ea[j % 4]));
        rig.teardown();
    });
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    const AuditCounters a = au_counters(fp[0]), b = au_counters(fp[1]), d = au_counters(fp[3]);
    CHECK(a.not_eligible == kJobs && a.no_twin == kJobs && a.no_body == 0 && a.sampled == 0 && a.queued == 0 && a.shadowed == 0,
          "no twin: not eligible %llu no-twin %llu sampled %llu", (unsigned long long)a.not_eligible, (unsigned long long)a.no_twin,
          (unsigned long long)a.sampled);
    CHECK(b.not_eligible == kJobs && b.no_body == kJobs && b.no_twin == 0 && b.sampled == 0 && b.queued == 0, "no body: not eligible %llu no-body %llu",
          (unsigned long long)b.not_eligible, (unsigned long long)b.no_body);
    CHECK(!audit_counters(fp[2]).has_value() && audit_unattributed().not_eligible == kJobs, "no inventory: %llu counted together",
          (unsigned long long)audit_unattributed().not_eligible);
    CHECK(d.not_eligible == kJobs && d.no_body == 0 && d.no_twin == 0 && d.queued == 0 && d.shadowed == 0, "event branches: not eligible %llu",
          (unsigned long long)d.not_eligible);
    CHECK(audit_queued() == 0, "a job nobody can shadow was queued");
    CHECK(au_lines(log, "audit-ineligible").size() == 2, "audit-ineligible lines:\n%s", log.c_str());
    CHECK(au_lines(log, "audit-ineligible image=" + xd_fp_hex(fp[0]) + " reason=no-twin").size() == 1, "the no-twin line:\n%s", log.c_str());
    CHECK(au_lines(log, "audit-ineligible image=" + xd_fp_hex(fp[1]) + " reason=no-body").size() == 1, "the no-body line:\n%s", log.c_str());
    CHECK(au_lines(log, "[jq2-xdiff] differ").empty(), "something was compared");
}

/* A named stop raised by the subject inside a shadow is void / host-fault for that sample, not a stop of the process: the
 * run goes on, the audit thread is reusable afterwards (the next shadow of another image is equal), and the line the
 * subject's stop leaves is the shadow's. The options say stop for both classes: a void never stops. */
void mode_audit_host_fault()
{
    constexpr std::uint32_t kStopping = 3, kFine = 2;
    const std::uint32_t ea[2] = {IMAGE, IMAGE + 0x1000};
    build_image(ea[0], Prog::transform);
    build_variant(ea[1], 1);
    au_register(ea[0], &au_interp_body, &au_stop_body, 40);
    au_register(ea[1], &au_interp_body, &au_interp_body, 50);
    const std::uint64_t fpa = fp_at(ea[0]), fpb = fp_at(ea[1]);
    CHECK(std::strcmp(spu_diag_tag(), "") == 0, "the tag outside a shadow is empty");
    spu_diag_shadow_set(1);
    CHECK(std::strcmp(spu_diag_tag(), "[shadow] ") == 0, "the tag inside a shadow");
    spu_diag_shadow_set(0);
    std::vector<JobSpec> specs;
    const std::string log = capture_stderr([&] {
        au_begin();
        Rig rig({128, 1, false});
        for (std::uint32_t j = 0; j < kStopping; ++j) specs.push_back(au_job(rig, j, ea[0]));
        for (std::uint32_t j = kStopping; j < kStopping + kFine; ++j) specs.push_back(au_job(rig, j, ea[1]));
        ap_wait_done(kStopping + kFine);
        rig.teardown();
    });
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    const AuditCounters a = au_counters(fpa), b = au_counters(fpb);
    CHECK(a.shadowed == kStopping && a.voided == kStopping && a.reason[SPU_XDIFF_R_HOST_FAULT] == kStopping && a.differ == 0,
          "stopping image: shadowed %llu void %llu host-fault %llu", (unsigned long long)a.shadowed, (unsigned long long)a.voided,
          (unsigned long long)a.reason[SPU_XDIFF_R_HOST_FAULT]);
    CHECK(b.shadowed == kFine && b.equal == kFine, "the audit thread after the diverted stops: shadowed %llu equal %llu",
          (unsigned long long)b.shadowed, (unsigned long long)b.equal);
    CHECK(au_lines(log, "PS3STOP").empty(), "the process stopped:\n%s", log.c_str());
    CHECK(au_lines(log, "[shadow] [spu-xdiff] the subject raised a named stop (kind=jq2-state").size() == kStopping, "the shadow's lines:\n%s",
          log.c_str());
    CHECK(au_lines(log, "[jq2-xdiff] void image=" + xd_fp_hex(fpa) + " ").size() == 1, "the void is named once per image and reason:\n%s",
          log.c_str());
}

/* The most a difference writes: 16 fixtures per image, the rest counted, every difference named. */
void mode_audit_fixtures_cap()
{
    constexpr std::uint32_t kJobs = 20, kCap = 16;
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_bad_body, &au_bad_body);
    const std::uint64_t fp = fp_at(IMAGE);
    std::vector<JobSpec> specs;
    const std::string log = capture_stderr([&] {
        au_begin();
        Rig rig({128, 1, false});
        for (std::uint32_t j = 0; j < kJobs; ++j) specs.push_back(au_job(rig, j));
        ap_wait_done(kJobs);
        rig.teardown();
    });
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    const AuditCounters c = au_counters(fp);
    CHECK(c.differ == kJobs && c.fixtures == kCap && c.fixtures_over == kJobs - kCap, "differ %llu fixtures %llu over %llu",
          (unsigned long long)c.differ, (unsigned long long)c.fixtures, (unsigned long long)c.fixtures_over);
    std::vector<std::string> want_files = xd_names(fp, 1, kCap);
    std::sort(want_files.begin(), want_files.end()); /* xd_files() is sorted as strings: s10 before s2 */
    CHECK(xd_files() == want_files, "fixtures: %s", join(xd_files()).c_str());
    CHECK(au_lines(log, "[jq2-xdiff] differ ").size() == kJobs, "differ lines:\n%s", log.c_str());
    CHECK(au_lines(log, "fixture=none").size() == kJobs - kCap, "the lines past the bound name no fixture");
}

/* A directory that cannot be used is one line and no fixtures; the difference is still named and counted. */
void mode_audit_baddir()
{
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_bad_body, &au_bad_body);
    const std::uint64_t fp = fp_at(IMAGE);
    JobSpec s{};
    const std::string log = capture_stderr([&] {
        au_begin();
        Rig rig({128, 1, false});
        s = au_job(rig, 0);
        ap_wait_done(1);
        rig.teardown();
    });
    CHECK(job_ok(s), "output");
    CHECK(au_lines(log, "[jq2-xdiff] fixtures-off reason=bad-dir").size() == 1, "one line says the directory is unusable:\n%s", log.c_str());
    const auto lines = au_lines(log, "[jq2-xdiff] differ ");
    CHECK(lines.size() == 1 && lines[0].find(" fixture=none") != std::string::npos, "the difference without a fixture:\n%s", log.c_str());
    const AuditCounters c = au_counters(fp);
    CHECK(c.differ == 1 && c.fixtures == 0 && c.fixtures_over == 0, "differ %llu fixtures %llu", (unsigned long long)c.differ, (unsigned long long)c.fixtures);
}

/* The roles are symmetric: under spu.job_body=lifted the registered body is the authority and the interpreter the
 * shadow, with no twin needed. A body that differs puts ITS output into the guest and is reported with the tiers the
 * other way round; one that equals the interpreter is `equal`. */
void mode_audit_lifted()
{
    const std::uint32_t ea[2] = {IMAGE, IMAGE + 0x1000};
    build_image(ea[0], Prog::transform);
    build_variant(ea[1], 1);
    au_register(ea[0], &au_bad_body, nullptr, 40, true);
    au_register(ea[1], &au_interp_body, nullptr, 50, true);
    const std::uint64_t fpa = fp_at(ea[0]), fpb = fp_at(ea[1]);
    std::vector<JobSpec> specs;
    const std::string log = capture_stderr([&] {
        au_begin();
        Rig rig({128, 1, false});
        for (std::uint32_t j = 0; j < 4; ++j) specs.push_back(au_job(rig, j, ea[j / 2]));
        ap_wait_done(4);
        rig.teardown();
    });
    for (std::uint32_t j = 0; j < 4; ++j) CHECK(au_out_is(specs[j], j < 2 ? 1u : 0u), "job %u: the guest has the authority's output", j);
    const AuditCounters a = au_counters(fpa), b = au_counters(fpb);
    CHECK(a.shadowed == 2 && a.differ == 2 && a.differ_visible == 2 && a.reason[SPU_XDIFF_R_EFFECT_PAYLOAD] == 2 && a.no_twin == 0,
          "bad body: shadowed %llu differ %llu", (unsigned long long)a.shadowed, (unsigned long long)a.differ);
    CHECK(b.shadowed == 2 && b.equal == 2 && b.not_eligible == 0, "equal body: shadowed %llu equal %llu", (unsigned long long)b.shadowed,
          (unsigned long long)b.equal);
    const auto lines = au_lines(log, "[jq2-xdiff] differ ");
    CHECK(lines.size() == 2 && lines[0].find(" authority=lifted subject=interp fixture=") != std::string::npos, "the lines:\n%s", log.c_str());
    Fx fx;
    if (xd_read(xd_name(fpa, 0, 1), fx)) {
        const spu_xdiff_body_id& body = fx.f.identity.authority;
        CHECK(body.tier == SPU_TIER_LIFTED && body.lifter_abi == SPU_RUNTIME_LIFTER_ABI && fx.f.verdict.subject_body.tier == SPU_TIER_INTERP,
              "tiers: authority %u abi %u subject %u", body.tier, body.lifter_abi, fx.f.verdict.subject_body.tier);
    }
}

/* spu.xdiff_period = 3 counts per image from its first job: of seven jobs of each of two interleaved images the first,
 * fourth and seventh are shadowed. */
void mode_audit_period()
{
    constexpr std::uint32_t kPerImage = 7;
    const std::uint32_t ea[2] = {IMAGE, IMAGE + 0x1000};
    build_image(ea[0], Prog::transform);
    build_variant(ea[1], 1);
    au_register(ea[0], &au_interp_body, &au_interp_body, 40);
    au_register(ea[1], &au_interp_body, &au_interp_body, 50);
    const std::uint64_t fp[2] = {fp_at(ea[0]), fp_at(ea[1])};
    au_begin();
    Rig rig({128, 1, false});
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < 2 * kPerImage; ++j) specs.push_back(au_job(rig, j, ea[j % 2]));
    ap_wait_done(6);
    rig.teardown();
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    std::set<std::pair<std::uint64_t, std::uint32_t>> got, want;
    for (const auto& d : g_ap.done) got.insert({d.fp, d.seq});
    /* One worker, the jobs in order: job j has seq j + 1 and is the (j / 2)-th job of its image. */
    for (std::uint32_t j = 0; j < 2 * kPerImage; ++j)
        if ((j / 2) % 3 == 0) want.insert({fp[j % 2], j + 1});
    CHECK(got == want, "%zu samples shadowed, wanted the %zu of jobs 1, 4 and 7 of each image", got.size(), want.size());
    for (const std::uint64_t f : fp) {
        const AuditCounters c = au_counters(f);
        CHECK(c.jobs == kPerImage && c.sampled == 3 && c.queued == 3 && c.dropped == 0 && c.shadowed == 3 && c.equal == 3,
              "image 0x%016llX: jobs %llu sampled %llu shadowed %llu", (unsigned long long)f, (unsigned long long)c.jobs,
              (unsigned long long)c.sampled, (unsigned long long)c.shadowed);
    }
}

/* The trace of a job under the audit (ps3_ftrace.h; docs/SPU_XDIFF.md section 14): kind 32 once per job with a JOB span,
 * zero length when it was not sampled, with the authority's tier and what became of the sample; kind 33 once per shadow
 * run with its verdict, reason and tier. Period 2: jobs 1 and 3 are queued, 2 and 4 not sampled. */
void mode_audit_trace()
{
    constexpr std::uint32_t kJobs = 4;
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_interp_body, &au_interp_body);
    CHECK(PS3_FT_ON(), "tracing is on"); /* before any worker: a hook sees "off" while another thread initialises */
    au_begin();
    Rig rig({128, 1, false});
    for (std::uint32_t j = 0; j < kJobs; ++j) au_job(rig, j);
    ap_wait_done(2);
    rig.teardown();
    const std::vector<ps3_ft_rec> bodies = ft_kind(PS3_FT_JQ2_BODY);
    CHECK(bodies.size() == kJobs, "%zu kind-32 records for %u jobs", bodies.size(), kJobs);
    std::map<std::uint32_t, ps3_ft_rec> by_seq32;
    for (const ps3_ft_rec& r : bodies) by_seq32[r.a] = r;
    for (std::uint32_t seq = 1; seq <= kJobs; ++seq) {
        const auto it = by_seq32.find(seq);
        CHECK(it != by_seq32.end(), "no kind 32 for seq %u", seq);
        if (it == by_seq32.end()) continue;
        const ps3_ft_rec& r = it->second;
        const std::uint32_t disposition = seq % 2 == 1 ? kQueued : kNotSampled;
        CHECK(r.who == 0 && r.b == (std::uint32_t{SPU_TIER_INTERP} | disposition << 8), "seq %u: who 0x%X b 0x%X, wanted disposition %u", seq, r.who, r.b,
              disposition);
        CHECK(disposition != kNotSampled || r.t0 == r.t1, "seq %u: a job that was not sampled has a span of %llu ns", seq, (unsigned long long)(r.t1 - r.t0));
        CHECK(r.t1 >= r.t0, "seq %u: the span runs backwards", seq);
    }
    const std::vector<ps3_ft_rec> shadows = ft_kind(PS3_FT_JQ2_XDIFF);
    std::set<std::uint32_t> seqs;
    for (const ps3_ft_rec& r : shadows) {
        seqs.insert(r.a);
        CHECK(r.who == 0 && r.b == (std::uint32_t{SPU_XDIFF_EQUAL} | std::uint32_t{SPU_XDIFF_R_NONE} << 8 | std::uint32_t{SPU_TIER_LIFTED} << 16),
              "seq %u: kind 33 who 0x%X b 0x%X", r.a, r.who, r.b);
        CHECK(r.t1 >= r.t0, "the subject's span runs backwards");
    }
    CHECK(shadows.size() == 2 && seqs == std::set<std::uint32_t>({1, 3}), "%zu kind-33 records", shadows.size());
}

/* spu.job_body=lifted with the audit off still emits kind 32 for each job (the tier that ran, never sampled, the
 * authority's lift misses saturated at 255) and no kind 33. The tier stays lifted: a registered body that reports a
 * miss is still T1, and the count beside it is what says the resolver served some of the job in the interpreter. */
void mode_audit_trace_lifted()
{
    constexpr std::uint32_t kJobs = 3;
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_miss_body, nullptr, 40, true);
    CHECK(PS3_FT_ON(), "tracing is on");
    au_begin();
    Rig rig({128, 1, false});
    for (std::uint32_t j = 0; j < kJobs; ++j) au_job(rig, j);
    rig.teardown();
    const std::vector<ps3_ft_rec> bodies = ft_kind(PS3_FT_JQ2_BODY);
    CHECK(bodies.size() == kJobs, "%zu kind-32 records for %u jobs", bodies.size(), kJobs);
    for (const ps3_ft_rec& r : bodies) {
        CHECK(r.b == (std::uint32_t{SPU_TIER_LIFTED} | std::uint32_t{kNotSampled} << 8 | 255u << 16), "seq %u: b 0x%X", r.a, r.b);
        CHECK(r.t0 == r.t1, "a job that was not sampled has a span");
    }
    CHECK(ft_kind(PS3_FT_JQ2_XDIFF).empty(), "a shadow ran with the audit off");
}

/* The default (interpreter, audit off) emits neither kind, and the jobs still have their JOB spans. */
void mode_audit_trace_none()
{
    constexpr std::uint32_t kJobs = 3;
    build_image(IMAGE, Prog::transform);
    CHECK(PS3_FT_ON(), "tracing is on");
    install_hooks();
    Rig rig({128, 1, false});
    for (std::uint32_t j = 0; j < kJobs; ++j) au_job(rig, j);
    rig.teardown();
    CHECK(ft_kind(PS3_FT_JQ2_JOB).size() == kJobs, "the jobs have no JOB spans");
    CHECK(ft_kind(PS3_FT_JQ2_BODY).empty() && ft_kind(PS3_FT_JQ2_XDIFF).empty(), "a record of the body or a shadow with both options at default");
    CHECK(!record_active(), "a job pays for recording with capture and the audit both off");
}

/* One recording serves the capture and the audit: with both on, a job is identified once (one content hash, one
 * fingerprint for the one content), the capture writes its fixture, and the audit shadows every job. */
void mode_audit_shared()
{
    constexpr std::uint32_t kJobs = 3;
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_interp_body, &au_interp_body);
    const std::uint64_t fp = fp_at(IMAGE);
    const IdentifyCounts before = identify_counts();
    au_begin();
    Rig rig({128, 1, false});
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < kJobs; ++j) specs.push_back(au_job(rig, j));
    ap_wait_done(kJobs);
    rig.teardown();
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    check_identified(before, kJobs, 1, "capture + audit");
    CHECK(xd_files() == xd_names(fp, 1, 1), "fixtures: %s", join(xd_files()).c_str());
    const auto cap = capture_counters(fp);
    CHECK(cap && cap->captured == 1 && cap->failed == 0, "captured %llu", (unsigned long long)(cap ? cap->captured : 0));
    const AuditCounters c = au_counters(fp);
    CHECK(c.queued == kJobs && c.shadowed == kJobs && c.equal == kJobs && c.dropped == 0, "queued %llu shadowed %llu equal %llu",
          (unsigned long long)c.queued, (unsigned long long)c.shadowed, (unsigned long long)c.equal);
    Fx fx;
    if (xd_read(xd_name(fp, 0, 1), fx)) {
        CHECK(fx.f.has_verdict == 0, "an equal shadow wrote a verdict into the captured fixture");
        xd_check_job(fx.f, specs[0], 1, SPU_TIER_INTERP);
    }
}

/* A knob that changes what the interpreter computes refuses the audit for the process, once, and the jobs run: a shadow
 * under it would compare another program. */
void mode_audit_refused()
{
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_interp_body, &au_interp_body);
    const std::uint64_t fp = fp_at(IMAGE);
    std::vector<JobSpec> specs;
    const std::string log = capture_stderr([&] {
        au_begin();
        Rig rig({128, 1, false});
        for (std::uint32_t j = 0; j < 2; ++j) specs.push_back(au_job(rig, j));
        rig.teardown();
    });
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    CHECK(au_lines(log, "[jq2-xdiff] audit-off reason=refused-knob detail=SPU_TAILRET").size() == 1, "the refusal:\n%s", log.c_str());
    CHECK(!audit_active() && !audit_counters(fp).has_value() && audit_queued() == 0 && g_ap.done.empty(), "the audit ran");
}

/* The audit's ordinal of a job counts every job of its image, recorded or not: with the odd jobs running an unknown body
 * (a forced body that is neither the interpreter nor the registered one), period 2 still shadows jobs 1, 3, 5 and 7, as
 * it would had every job been eligible. */
JobBody au_alternate_select(void*, const JobStart& js)
{
    return serial_of(js) % 2 != 0 ? JobBody{&lifted_normal_entry, -1} : JobBody{};
}
void mode_audit_ordinal()
{
    constexpr std::uint32_t kJobs = 8;
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_interp_body, &au_interp_body);
    const std::uint64_t fp = fp_at(IMAGE);
    au_begin();
    ExecutorHooks h;
    h.on_admit = &hook_admit;
    h.on_done = &hook_done;
    h.on_end = &hook_end;
    h.select_body = &au_alternate_select;
    set_executor_hooks(h);
    Rig rig({128, 1, false});
    std::vector<JobSpec> specs;
    for (std::uint32_t j = 0; j < kJobs; ++j) specs.push_back(au_job(rig, j));
    ap_wait_done(kJobs / 2);
    rig.teardown();
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    std::set<std::uint32_t> seqs;
    for (const auto& d : g_ap.done) seqs.insert(d.seq);
    CHECK(seqs == std::set<std::uint32_t>({1, 3, 5, 7}), "%zu shadowed jobs, wanted seqs 1 3 5 7", seqs.size());
    const AuditCounters c = au_counters(fp);
    const auto cap = capture_counters(fp);
    CHECK(c.jobs == kJobs && c.sampled == kJobs / 2 && c.shadowed == kJobs / 2 && c.equal == kJobs / 2, "jobs %llu sampled %llu shadowed %llu",
          (unsigned long long)c.jobs, (unsigned long long)c.sampled, (unsigned long long)c.shadowed);
    CHECK(cap && cap->ineligible_unknown_body == kJobs / 2, "%llu unknown-body jobs", (unsigned long long)(cap ? cap->ineligible_unknown_body : 0));
}

/* A host that cannot allocate the shadow context turns the audit off at configure, with one line: no producer records, no
 * thread runs, and the jobs are untouched. */
void mode_audit_alloc_fail()
{
    build_image(IMAGE, Prog::transform);
    au_register(IMAGE, &au_interp_body, &au_interp_body);
    const std::uint64_t fp = fp_at(IMAGE);
    set_audit_shadow_alloc_fail(true);
    std::vector<JobSpec> specs;
    const std::string log = capture_stderr([&] {
        au_begin();
        Rig rig({128, 1, false});
        for (std::uint32_t j = 0; j < 2; ++j) specs.push_back(au_job(rig, j));
        rig.teardown();
    });
    for (const JobSpec& s : specs) CHECK(job_ok(s), "job %u output", s.serial);
    CHECK(au_lines(log, "[jq2-xdiff] audit-off reason=no-memory").size() == 1, "the line:\n%s", log.c_str());
    CHECK(!audit_active() && !record_active() && !audit_counters(fp).has_value() && audit_queued() == 0 && g_ap.done.empty() && g_ap.started == 0,
          "the audit ran");
}

/* The options of an audit mode, in the environment before anything reads the configuration. */
void au_prologue(const std::string& mode)
{
    std::string tmpl = (fs::temp_directory_path() / "jq2_audit.XXXXXX").string();
    if (mkdtemp(tmpl.data()) == nullptr) {
        std::perror("mkdtemp");
        std::exit(2);
    }
    g_xd_dir = tmpl;
    const char* xdiff = "sample";
    std::string period = "1", on_divergence = "report", on_latent = "report", job_body = "interp", capture = "0", dir = g_xd_dir;
    /* `sample` drops a sample while the audit thread lags and eight are queued, which a test with more samples than that
     * must not depend on: it runs under `all`, which waits for room. */
    if (mode == "audit-all" || mode == "audit-concurrent" || mode == "audit-fixtures-cap") xdiff = "all";
    else if (mode == "audit-shared") {
        xdiff = "all";
        capture = "1";
    } else if (mode == "audit-period") period = "3";
    else if (mode == "audit-ordinal") period = "2";
    else if (mode == "audit-trace") period = "2";
    else if (mode == "audit-latent" || mode == "audit-stop") on_divergence = "stop";
    else if (mode == "audit-stop-latent") on_latent = "stop";
    else if (mode == "audit-oversize" || mode == "audit-host-fault") on_divergence = on_latent = "stop"; /* a void never stops */
    else if (mode == "audit-lifted") job_body = "lifted";
    else if (mode == "audit-baddir") dir += "/missing";
    else if (mode == "audit-trace-lifted") {
        xdiff = "off";
        job_body = "lifted";
    } else if (mode == "audit-trace-none") xdiff = "off";
    else if (mode == "audit-refused") setenv("SPU_TAILRET", "0", 1);
    setenv("PS3RECOMP_SPU_XDIFF", xdiff, 1);
    setenv("PS3RECOMP_SPU_XDIFF_PERIOD", period.c_str(), 1);
    setenv("PS3RECOMP_SPU_XDIFF_ON_DIVERGENCE", on_divergence.c_str(), 1);
    setenv("PS3RECOMP_SPU_XDIFF_ON_LATENT", on_latent.c_str(), 1);
    setenv("PS3RECOMP_SPU_JOB_BODY", job_body.c_str(), 1);
    setenv("PS3RECOMP_SPU_XDIFF_CAPTURE", capture.c_str(), 1);
    setenv("PS3RECOMP_SPU_XDIFF_DIR", dir.c_str(), 1);
    if (mode.rfind("audit-trace", 0) == 0) {
        setenv("PS3_FTRACE", "0:100000:/dev/null", 1);
        setenv("PS3_FTRACE_CAP", "65536", 1);
    }
}

/* The options of an xdiff mode, in the environment before anything reads the configuration. */
void xd_prologue(const std::string& mode)
{
    std::string tmpl = (fs::temp_directory_path() / "jq2_xdiff.XXXXXX").string();
    if (mkdtemp(tmpl.data()) == nullptr) {
        std::perror("mkdtemp");
        std::exit(2);
    }
    g_xd_dir = tmpl;
    std::uint32_t n = kXdN;
    std::string dir = g_xd_dir;
    if (mode == "xdiff-off") n = 0;
    else if (mode == "xdiff-nodir") {
        n = 2;
        dir.clear();
    }
    else if (mode == "xdiff-baddir") {
        n = 1000;
        dir += "/missing";
    }
    else if (mode == "xdiff-observer") n = 2;
    else if (mode == "xdiff-concurrent") n = 5;
    else if (mode == "xdiff-stop") n = 1;
    else if (mode == "xdiff-select-miss") n = 0; /* the tier record is the subject: no capture, no fixture */
    setenv("PS3RECOMP_SPU_XDIFF_CAPTURE", std::to_string(n).c_str(), 1);
    if (dir.empty()) unsetenv("PS3RECOMP_SPU_XDIFF_DIR");
    else setenv("PS3RECOMP_SPU_XDIFF_DIR", dir.c_str(), 1);
    /* Read at the first job, like the capture options. */
    if (mode.rfind("xdiff-select-", 0) == 0) setenv("PS3RECOMP_SPU_JOB_BODY", "lifted", 1);
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
    /* The recorder reads its window once, at the first hook: the trace modes say so before any hook runs. */
    if (mode == "xdiff-select-trace" || mode == "xdiff-select-miss") {
        setenv("PS3_FTRACE", "0:100000:/dev/null", 1);
        setenv("PS3_FTRACE_CAP", "65536", 1);
        /* The tier record needs neither a capture nor an audit: the job must run the production authority. */
        if (mode == "xdiff-select-miss") setenv("PS3RECOMP_SPU_XDIFF", "off", 1);
    }
    /* spu.job_body is read at the first job: the select modes set it first. */
    if (mode.rfind("select-", 0) == 0) {
        const bool interp = mode == "select-interp" || mode == "select-promoted-interp";
        setenv("PS3RECOMP_SPU_JOB_BODY", interp ? "interp" : "lifted", 1);
    }
    if (mode.rfind("trace-", 0) == 0) {
        if (mode == "trace-off" || mode == "trace-image-off") {
            unsetenv("PS3_FTRACE");
        } else {
            const std::string where = mode == "trace-image-overflow" ? overflow_trace_path() : "/dev/null";
            setenv("PS3_FTRACE", ("0:100000:" + where).c_str(), 1);
            setenv("PS3_FTRACE_CAP", "65536", 1);
        }
    }
    /* The capture options are read when the first job starts and never again: the xdiff modes set them first. */
    if (mode.rfind("xdiff-", 0) == 0) xd_prologue(mode);
    /* The audit options likewise: read at the first queue. */
    if (mode.rfind("audit-", 0) == 0) au_prologue(mode);
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
    else if (mode == "end-return") mode_end_return();
    else if (mode == "end-return-lifted") mode_end_return_lifted();
    else if (mode == "end-stop") mode_end_stop();
    else if (mode == "end-halt") mode_end_halt();
    else if (mode == "end-jm-call") mode_end_jm_call();
    else if (mode == "end-classify") mode_end_classify();
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
    else if (mode == "stop-push-flag1") push_flag1_scenario(false);
    else if (mode == "stop-copy-push-flag1") push_flag1_scenario(true);
    else if (mode == "push-block") mode_push_block();
    else if (mode == "push-nonblock") mode_push_nonblock();
    else if (mode == "pushsync-block") mode_pushsync_block();
    else if (mode == "push-block-copy") mode_push_block_copy();
    else if (mode == "push-flags") mode_push_flags();
    else if (mode == "stop-push-block-shutdown") mode_stop_push_block_shutdown();
    else if (mode == "trace-spans") mode_trace_spans();
    else if (mode == "trace-sync") mode_trace_sync();
    else if (mode == "trace-queues") mode_trace_queues();
    else if (mode == "trace-concurrent") mode_trace_concurrent();
    else if (mode == "trace-held") mode_trace_held();
    else if (mode == "trace-off") mode_trace_off();
    else if (mode == "trace-image-two") mode_trace_image_two();
    else if (mode == "trace-image-same") mode_trace_image_same();
    else if (mode == "trace-image-changed") mode_trace_image_changed();
    else if (mode == "trace-image-moved") mode_trace_image_moved();
    else if (mode == "trace-image-overflow") mode_trace_image_overflow();
    else if (mode == "trace-image-concurrent") mode_trace_image_concurrent();
    else if (mode == "trace-image-off") mode_trace_image_off();
    else if (mode == "trace-image-observer") mode_trace_image_observer();
    else if (mode == "xdiff-capture") mode_xdiff_capture();
    else if (mode == "xdiff-lifted") mode_xdiff_lifted();
    else if (mode == "xdiff-observer") mode_xdiff_observer();
    else if (mode == "xdiff-concurrent") mode_xdiff_concurrent();
    else if (mode == "xdiff-stop") mode_xdiff_stop();
    else if (mode == "xdiff-event") mode_xdiff_event();
    else if (mode == "xdiff-unresolved") mode_xdiff_unresolved();
    else if (mode == "xdiff-both") mode_xdiff_both();
    else if (mode == "xdiff-unknown") mode_xdiff_unknown();
    else if (mode == "xdiff-baddir") mode_xdiff_baddir();
    else if (mode == "xdiff-body-none") mode_xdiff_body_none();
    else if (mode == "xdiff-body-other") mode_xdiff_body_other();
    else if (mode == "xdiff-varying") mode_xdiff_varying();
    else if (mode == "xdiff-bounded") mode_xdiff_bounded();
    else if (mode == "xdiff-writefail") mode_xdiff_writefail();
    else if (mode == "xdiff-payload") mode_xdiff_payload();
    else if (mode == "xdiff-queue256") mode_xdiff_queue256();
    else if (mode == "xdiff-nonrepeating") mode_xdiff_nonrepeating();
    else if (mode == "xdiff-starvation") mode_xdiff_starvation();
    else if (mode == "xdiff-failure-lines") mode_xdiff_failure_lines();
    else if (mode == "xdiff-collision") mode_xdiff_collision();
    else if (mode == "xdiff-off") mode_xdiff_off();
    else if (mode == "xdiff-nodir") mode_xdiff_nodir();
    else if (mode == "select-lifted") mode_select_lifted();
    else if (mode == "select-other-bytes") mode_select_other_bytes();
    else if (mode == "select-interp") mode_select_interp();
    else if (mode == "select-promoted") mode_select_promoted();
    else if (mode == "select-promoted-interp") mode_select_promoted_interp();
    else if (mode == "select-forced") mode_select_forced();
    else if (mode == "select-observer") mode_select_observer();
    else if (mode == "select-concurrent") mode_select_concurrent();
    else if (mode == "select-concurrent-hook") mode_select_concurrent_hook();
    else if (mode == "xdiff-select-collision") mode_xdiff_select_collision();
    else if (mode == "xdiff-select-lifted") mode_xdiff_select_lifted();
    else if (mode == "xdiff-select-miss") mode_xdiff_select_miss();
    else if (mode == "xdiff-select-trace") mode_xdiff_select_trace();
    else if (mode == "audit-divergence") mode_audit_divergence();
    else if (mode == "audit-latent") mode_audit_latent();
    else if (mode == "audit-stop") mode_audit_stop();
    else if (mode == "audit-stop-latent") mode_audit_stop_latent();
    else if (mode == "audit-full") mode_audit_full();
    else if (mode == "audit-all") mode_audit_all();
    else if (mode == "audit-concurrent") mode_audit_concurrent();
    else if (mode == "audit-oversize") mode_audit_oversize();
    else if (mode == "audit-shutdown") mode_audit_shutdown();
    else if (mode == "audit-ineligible") mode_audit_ineligible();
    else if (mode == "audit-host-fault") mode_audit_host_fault();
    else if (mode == "audit-fixtures-cap") mode_audit_fixtures_cap();
    else if (mode == "audit-baddir") mode_audit_baddir();
    else if (mode == "audit-lifted") mode_audit_lifted();
    else if (mode == "audit-period") mode_audit_period();
    else if (mode == "audit-trace") mode_audit_trace();
    else if (mode == "audit-trace-lifted") mode_audit_trace_lifted();
    else if (mode == "audit-trace-none") mode_audit_trace_none();
    else if (mode == "audit-ordinal") mode_audit_ordinal();
    else if (mode == "audit-alloc-fail") mode_audit_alloc_fail();
    else if (mode == "audit-shared") mode_audit_shared();
    else if (mode == "audit-refused") mode_audit_refused();
    else {
        std::printf("unknown mode %s\n", mode.c_str());
        return 2;
    }
    std::error_code ignored;
    if (!g_xd_dir.empty()) fs::remove_all(g_xd_dir, ignored);
    if (g_fail != 0) {
        std::printf("FAIL: %d check(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS %s\n", mode.c_str());
    return 0;
}

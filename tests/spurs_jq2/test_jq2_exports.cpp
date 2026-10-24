/*
 * T0: the cellSpursJq HLE surface through ps3_hle_call.
 *
 * Every call goes through the generated NID table (tools/gen_hle_nids.py
 * cellSpursJq) and the real runtime/ppu/ppu_hle.cpp, so this exercises what a
 * title sees: the 32-bit guest EA arguments, the parameter save area for the
 * 10- and 12-argument creates, the s32 return in r3, and the guest memory the
 * exports write.
 *
 * Covered here (the executor's own behaviour is test_jq2_executor's):
 *   - all 17 names hash to the firmware NID and the generated table registers
 *     exactly 17 handlers;
 *   - the attribute layout the firmware writes, field by field, including
 *     +0x13 = 0xFF after Initialize and the bytes Initialize does not touch;
 *   - every guard of the 17 exports: A11 null, A10 alignment, A02 invalid,
 *     A0F state, A0A busy, A01 again;
 *   - the 10- and 12-argument creates read args 9..12 from the caller's
 *     parameter save area, and the queue/ring/pool layout they leave;
 *   - Port2Create's 128-byte port image, slot allocation in queue+0x580,
 *     Destroy's +0x21 state guard (A0F, before BUSY) and BUSY while B != 0,
 *     and slot release;
 *   - PushSync with mask 0 returns A02 and leaves the port bytes untouched;
 *   - Port2Sync mode 4 AGAIN, mode 0 success, the claim BUSY path.
 *
 * Synthetic guest memory only. Exit status 0 = pass.
 *
 * This surface replaces the upstream completion stub. The upstream ABI cannot
 * pass these checks by construction: Port2Create was declared (jq, port) and
 * wrote *port = *jq (the title passes (port, queue)); PushSync was a job push
 * returning an id (the title passes (port, tagMask, flags)); the attribute was
 * a 16-byte host struct with maxJobs/maxGrab/priorities (the firmware block is
 * the offsets written below). Those bodies are gone; an import whose handler
 * is not implemented now reaches the unresolved-NID stop instead of CELL_OK.
 */

#include "ppu_context.h"
#include "ps3emu/nid.h"
#include "jq2/jq2.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* The loader surface the runtime's HLE modules name (the same set
 * runtime/host/host_stubs.c gives a game-less host, plus vm_read64, which the
 * typed HLE adapter calls). */
extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size = 0x10000000;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
uint32_t g_barrier_sync_watch = 0;
PPU_THREAD_LOCAL ppu_context* g_active_ctx = nullptr;
uint32_t g_spu_image_src_ea = 0, g_spu_image_ls_start = 0, g_spu_image_span = 0;
const char* ppu_vfs_root = ".";

void vm_write8(uint64_t a, uint8_t v) { vm_base[(uint32_t)a] = v; }
void vm_write16(uint64_t a, uint16_t v) { v = __builtin_bswap16(v); std::memcpy(vm_base + (uint32_t)a, &v, 2); }
void vm_write32(uint64_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(vm_base + (uint32_t)a, &v, 4); }
uint32_t vm_read32(uint64_t a) { uint32_t v; std::memcpy(&v, vm_base + (uint32_t)a, 4); return __builtin_bswap32(v); }
uint64_t vm_read64(uint64_t a) { uint64_t v; std::memcpy(&v, vm_base + (uint32_t)a, 8); return __builtin_bswap64(v); }

void ps3_ww_report_inline(uint32_t, uint64_t, int) {}
void ppu_ctx_register(void*) {}
void lbp_breadcrumb_dump(const char*) {}
void ppu_log_host_chain(const char*) {}
void ppu_guest_callstack(const char*) {}
void ppu_guard_page(uint32_t) {}
void ppu_dump_guest_stack(ppu_context*, const char*) {}
void ppu_dump_bctrl_ring(uint32_t, const char*) {}
void ppu_guest_caller(char* out, size_t n) { if (out && n) out[0] = '\0'; }
uint32_t ppu_active_lr(void) { return 0; }
uint32_t ps3_spu_image_source_ea(uint32_t ea) { return ea; }
/* The executor reaches cellFiber, which names the loader's dispatch-state
 * hooks; no PPU dispatcher is linked here, which is the documented NULL case. */
int* ps3_ppu_dispatch_depth_slot(void) { return nullptr; }
int* ps3_ppu_vcall_sp_slot(void) { return nullptr; }
uint32_t** ps3_ppu_vcall_stack_slot(void) { return nullptr; }

void ps3_hle_call(uint32_t nid, ppu_context* ctx);
void ppu_hle_init(void);
uint32_t ps3_hle_count(void);

uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t) { std::abort(); }
uint32_t ppu_prof_resolve_host(void*) { return 0; }
}
/* ppu_hle.cpp writes the guest TOC through a C++-linkage vm_write64. */
void vm_write64(uint64_t a, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(vm_base + (uint32_t)a, &v, 8); }

static int g_failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        const auto _a = (actual);                                              \
        const auto _e = (expected);                                            \
        if (_a != _e) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s = 0x%llX, expected 0x%llX\n", \
                         __FILE__, __LINE__, #actual,                          \
                         (unsigned long long)_a, (unsigned long long)_e);      \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

/* ---- the firmware error values, by name --------------------------------- */

static constexpr uint32_t A01 = 0x80410A01u; /* AGAIN */
static constexpr uint32_t A02 = 0x80410A02u; /* INVAL */
static constexpr uint32_t A0A = 0x80410A0Au; /* BUSY */
static constexpr uint32_t A0F = 0x80410A0Fu; /* STAT */
static constexpr uint32_t A10 = 0x80410A10u; /* ALIGN */
static constexpr uint32_t A11 = 0x80410A11u; /* NULL_POINTER */

/* ---- the 17 imports of the title ---------------------------------------- */

struct Export {
    const char* name;
    uint32_t nid;
};

static constexpr Export kExports[] = {
    {"cellSpursJobQueueAttributeInitialize", 0x43DDAB4Fu},
    {"cellSpursJobQueueAttributeSetMaxSizeJobDescriptor", 0x1686957Eu},
    {"cellSpursJobQueueAttributeSetGrabParameters", 0xE70F874Eu},
    {"cellSpursJobQueueAttributeSetDoBusyWaiting", 0x0582338Au},
    {"cellSpursJobQueueAttributeSetSubmitWithEntryLock", 0x0F03F712u},
    {"cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck", 0x742CEC0Du},
    {"cellSpursJobQueueAttributeSetIsHaltOnError", 0xFF03CC79u},
    {"_cellSpursCreateJobQueue", 0xF244E799u},
    {"_cellSpursCreateJobQueueWithJobDescriptorPool", 0x983BE7FCu},
    {"cellSpursShutdownJobQueue", 0x8C97A96Cu},
    {"cellSpursJoinJobQueue", 0x8CE4CFFAu},
    {"cellSpursJobQueuePort2Create", 0xF11FE0F1u},
    {"cellSpursJobQueuePort2Destroy", 0xC485D207u},
    {"_cellSpursJobQueuePort2PushJobBody", 0xCF89F218u},
    {"_cellSpursJobQueuePort2CopyPushJobBody", 0xC39173A4u},
    {"cellSpursJobQueuePort2PushSync", 0xFC526B72u},
    {"cellSpursJobQueuePort2Sync", 0xFACB3CEDu},
};

/* ---- guest memory and calls --------------------------------------------- */

static constexpr uint32_t kArena = 0x800000;
static constexpr uint32_t kStackBytes = 0x1000;
static uint32_t g_brk = 0x10000;

static uint32_t alloc(uint32_t bytes, uint32_t align)
{
    uint32_t ea = (g_brk + align - 1) & ~(align - 1);
    g_brk = ea + bytes + 16;
    CHECK(g_brk < kArena);
    return ea;
}

static uint32_t rd32(uint32_t ea) { return vm_read32(ea); }
static void wr32(uint32_t ea, uint32_t v) { vm_write32(ea, v); }
static uint8_t rd8(uint32_t ea) { return vm_base[ea]; }
static void wr8(uint32_t ea, uint8_t v) { vm_base[ea] = v; }
static void fill(uint32_t ea, uint32_t bytes, uint8_t v) { std::memset(vm_base + ea, v, bytes); }

/* One guest thread. ps3_hle_call spills r2 at r1+0x28; the save-area
 * arguments live at r1 + 48 + 8*i, exactly where the adapter reads them. */
class Guest {
public:
    Guest()
    {
        stack_ = alloc(kStackBytes, 256);
        std::memset(&ctx_, 0, sizeof ctx_);
        ctx_.thread_id = 0x1000001;
        ctx_.lr = 0x10100;
        ctx_.gpr[1] = stack_;
    }

    uint32_t call(uint32_t nid, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0,
                  uint64_t a6 = 0, uint64_t a7 = 0, uint64_t a8 = 0, uint64_t a9 = 0,
                  uint64_t a10 = 0, uint64_t a11 = 0, uint64_t a12 = 0, uint64_t a13 = 0,
                  uint64_t a14 = 0)
    {
        const uint64_t args[12] = {a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14};
        std::memset(vm_base + stack_, 0, kStackBytes);
        ctx_.gpr[3] = args[0]; ctx_.gpr[4] = args[1]; ctx_.gpr[5] = args[2];
        ctx_.gpr[6] = args[3]; ctx_.gpr[7] = args[4]; ctx_.gpr[8] = args[5];
        ctx_.gpr[9] = args[6]; ctx_.gpr[10] = args[7];
        for (int i = 8; i < 12; ++i) vm_write64(stack_ + 48 + 8 * i, args[i]);
        ps3_hle_call(nid, &ctx_);
        /* An s32 result comes back sign-extended (docs/HLE_ABI.md). */
        CHECK_EQ(ctx_.gpr[3], (uint64_t)(int64_t)(int32_t)ctx_.gpr[3]);
        return (uint32_t)ctx_.gpr[3];
    }

private:
    ppu_context ctx_{};
    uint32_t stack_ = 0;
};

/* ---- NID table ---------------------------------------------------------- */

static void nid_and_registration_tests()
{
    for (const Export& e : kExports)
        CHECK_EQ(ps3_compute_nid(e.name), e.nid);

    ppu_hle_init();
    CHECK_EQ(ps3_hle_count(), 17u);
}

/* ---- attribute block ---------------------------------------------------- */

static constexpr uint32_t kAttrBytes = 0x20;

static void attribute_tests(Guest& g, uint32_t attr)
{
    /* Initialize on a poisoned block: it writes exactly +0x04, +0x08, +0x10,
     * and the four flag bytes +0x0C..+0x0F -- not +0x00 or +0x14.. . */
    fill(attr, kAttrBytes, 0xA5);
    CHECK_EQ(g.call(0x43DDAB4Fu, 0), A11);
    CHECK_EQ(g.call(0x43DDAB4Fu, attr), 0u);
    CHECK_EQ(rd32(attr + 0x04), 4u);
    CHECK_EQ(rd32(attr + 0x08), 256u);
    CHECK_EQ(rd32(attr + 0x10), 255u);
    CHECK_EQ(rd8(attr + 0x13), 0xFFu);
    for (uint32_t off = 0x0C; off <= 0x0F; ++off) CHECK_EQ(rd8(attr + off), 0u);
    CHECK_EQ(rd32(attr + 0x00), 0xA5A5A5A5u);
    CHECK_EQ(rd32(attr + 0x14), 0xA5A5A5A5u);
    CHECK_EQ(rd32(attr + 0x18), 0xA5A5A5A5u);
    CHECK_EQ(rd32(attr + 0x1C), 0xA5A5A5A5u);

    /* SetMaxSizeJobDescriptor (0x02000128): 256..1023, 128-aligned. */
    CHECK_EQ(g.call(0x1686957Eu, 0, 896), A11);
    CHECK_EQ(g.call(0x1686957Eu, attr, 255), A02);
    CHECK_EQ(g.call(0x1686957Eu, attr, 1024), A02);
    CHECK_EQ(g.call(0x1686957Eu, attr, 300), A02);
    CHECK_EQ(g.call(0x1686957Eu, attr, 1023), A02);
    CHECK_EQ(g.call(0x1686957Eu, attr, 896), 0u);
    CHECK_EQ(rd32(attr + 0x08), 896u);
    CHECK_EQ(g.call(0x1686957Eu, attr, 256), 0u);
    CHECK_EQ(rd32(attr + 0x08), 256u);

    /* SetGrabParameters (0x02000178): arg2 <= 255, 1 <= arg3 <= 16;
     * +0x04 := arg3, +0x10 := arg2. */
    CHECK_EQ(g.call(0xE70F874Eu, 0, 255, 4), A11);
    CHECK_EQ(g.call(0xE70F874Eu, attr, 256, 4), A02);
    CHECK_EQ(g.call(0xE70F874Eu, attr, 255, 0), A02);
    CHECK_EQ(g.call(0xE70F874Eu, attr, 255, 17), A02);
    CHECK_EQ(g.call(0xE70F874Eu, attr, 255, 4), 0u);
    CHECK_EQ(rd32(attr + 0x04), 4u);
    CHECK_EQ(rd32(attr + 0x10), 255u);
    CHECK_EQ(rd8(attr + 0x13), 0xFFu);
    CHECK_EQ(g.call(0xE70F874Eu, attr, 0, 1), 0u);
    CHECK_EQ(rd32(attr + 0x04), 1u);
    CHECK_EQ(rd32(attr + 0x10), 0u);
    CHECK_EQ(rd8(attr + 0x13), 0u);
    CHECK_EQ(g.call(0xE70F874Eu, attr, 255, 4), 0u); /* Odin's values */

    /* One byte each, no value validation (firmware stores the low byte). */
    struct Flag {
        uint32_t nid;
        uint32_t off;
    };
    const Flag flags[] = {
        {0x0F03F712u, 0x0C}, {0x0582338Au, 0x0D}, {0xFF03CC79u, 0x0E}, {0x742CEC0Du, 0x0F},
    };
    for (const Flag& f : flags) {
        CHECK_EQ(g.call(f.nid, 0, 1), A11);
        CHECK_EQ(g.call(f.nid, attr, 1), 0u);
        CHECK_EQ(rd8(attr + f.off), 1u);
        CHECK_EQ(g.call(f.nid, attr, 0x1234), 0u); /* low byte 0x34 */
        CHECK_EQ(rd8(attr + f.off), 0x34u);
        CHECK_EQ(g.call(f.nid, attr, 0), 0u);
        CHECK_EQ(rd8(attr + f.off), 0u);
    }
}

/* ---- create/teardown ---------------------------------------------------- */

static void create_validation_tests(Guest& g, uint32_t spurs, uint32_t queue, uint32_t ring,
                                    uint32_t attr)
{
    const uint32_t name = alloc(32, 1);
    const uint32_t extra = alloc(32, 1);
    fill(name, 32, 0);
    fill(extra, 32, 0);

    /* Null arguments, in the firmware's order (JQ-FW-PPU, jq2fw-018). */
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, 0, queue, attr, name, ring, 4, 1, extra), A11);
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, 0, attr, name, ring, 4, 1, extra), A11);
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, 0, name, ring, 4, 1, extra), A11);
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, 0, 4, 1, extra), A11);

    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs + 1, queue, attr, name, ring, 4, 1, extra), A10);
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue + 64, attr, name, ring, 4, 1, extra), A10);
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, ring + 64, 4, 1, extra), A10);

    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, ring, 0x8001, 1, extra), A02);
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, ring, 4, 9, extra), A02);
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, ring, 0, 1, extra), A02);
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, ring, 4, 0, extra), A02);

    /* Attribute range (0x0200057C-0x020005B8). */
    wr32(attr + 0x04, 0);
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, ring, 4, 1, extra), A02);
    wr32(attr + 0x04, 17);
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, ring, 4, 1, extra), A02);
    wr32(attr + 0x04, 4);
    wr32(attr + 0x08, 255);
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, ring, 4, 1, extra), A02);
    wr32(attr + 0x08, 1024);
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, ring, 4, 1, extra), A02);
    wr32(attr + 0x08, 384 + 64); /* 448, not 128-aligned */
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, ring, 4, 1, extra), A02);
    wr32(attr + 0x08, 896);

    /* A dead queue is not touched: none of the calls above registered it or
     * reached the create body's queue memset. */
    CHECK_EQ(rd32(queue + 0x10), 0xAAAAAAAAu);
}

static void create_10_args(Guest& g, uint32_t spurs, uint32_t queue, uint32_t ring, uint32_t attr)
{
    const uint32_t name = alloc(32, 1);
    const uint32_t extra = alloc(32, 1);
    fill(name, 32, 0);

    /* Worker count and extra are arguments 9 and 10: the adapter must read
     * them from the caller's parameter save area. */
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, ring, 4, 1, extra), 0u);

    CHECK_EQ(rd32(queue + 0xFFC), 2u);            /* revision */
    CHECK_EQ(rd32(queue + 0xFF8), 0x330000u);     /* SDK version */
    CHECK_EQ(rd32(queue + 0xF10), spurs);         /* SPURS EA */
    CHECK_EQ(rd32(queue + 0xF14), 896u);          /* max descriptor */
    CHECK_EQ(rd32(queue + 0x10), ring);           /* command ring */
    CHECK_EQ(rd32(queue + 0x14) >> 16, 4u);       /* depth (u16 BE at +0x14) */
    CHECK_EQ(rd32(queue + 0x04), 0x60u);          /* initial ring flags */
    CHECK_EQ(rd32(queue + 0x70), 0u);             /* no ports */
    CHECK(rd32(queue + 0x80) != 0u);              /* LV2 event flag id */
    CHECK(rd32(queue + 0x84) != 0u);
    CHECK_EQ(rd32(queue + 0x100), 0u);            /* queue block zeroed */
    CHECK_EQ(rd32(queue + 0xF04), 0u);
    CHECK_EQ(rd32(queue + 0x580), 0u);            /* port bitmap */

    /* The command-buffer room the firmware zeroes: 2048 + 128 for depth 4. */
    CHECK_EQ(rd8(ring + 0), 0u);
    CHECK_EQ(rd8(ring + 2175), 0u);
    CHECK_EQ(rd8(ring + 2176), 0xAAu); /* beyond the firmware's memset */

    /* A second create on a live queue is STAT and must not zero it. */
    CHECK_EQ(g.call(0xF244E799u, 2, 0x330000, spurs, queue, attr, name, ring, 4, 1, extra), A0F);
    CHECK_EQ(rd32(queue + 0x10), ring);
}

static void create_12_args(Guest& g, uint32_t spurs, uint32_t queue, uint32_t ring,
                           uint32_t attr, uint32_t pool, uint32_t pool_attr)
{
    const uint32_t name = alloc(32, 1);

    /* Pool and pool attribute must be both present or both absent. */
    CHECK_EQ(g.call(0x983BE7FCu, 2, 0x330000, spurs, queue, attr, pool, 0, name, ring, 8, 1, 0), A02);
    CHECK_EQ(g.call(0x983BE7FCu, 2, 0x330000, spurs, queue, attr, 0, pool_attr, name, ring, 8, 1, 0), A02);
    CHECK_EQ(g.call(0x983BE7FCu, 2, 0x330000, spurs, queue, attr, pool + 64, pool_attr, name, ring, 8, 1, 0),
             A10);
    /* No pool-attribute alignment check: the firmware has none (CreateCommon
     * 0x020004E8..0x02000530); jq2_exec_validate covers a 16-aligned one. */

    /* Success. Args 9..12 (command buffer, depth, workers, extra) are the
     * save-area parameters. */
    CHECK_EQ(g.call(0x983BE7FCu, 2, 0x330000, spurs, queue, attr, pool, pool_attr, name, ring, 8, 1, 0), 0u);
    CHECK_EQ(rd32(queue + 0x10), ring);
    CHECK_EQ(rd32(queue + 0x14) >> 16, 8u);
    CHECK_EQ(rd32(queue + 0xF14), 896u);

    /* Pool layout: class 2 (256 B) x1, carved from the pool base; the class
     * control area follows the block area (pool.hpp's guest layout). */
    CHECK_EQ(rd32(queue + 0x600 + 4 * 2), pool);
    CHECK_EQ(rd32(queue + 0x620 + 4 * 2), pool + 256);
    CHECK_EQ(rd32(queue + 0x640 + 4 * 2), pool + 256);
    CHECK_EQ(rd32(queue + 0x660), pool);
    CHECK_EQ(rd32(queue + 0x664), pool + 256);
}

/* ---- ports -------------------------------------------------------------- */

static void port_create_tests(Guest& g, uint32_t queue, uint32_t port, uint32_t port2, uint32_t port3)
{
    fill(port, 128, 0xAA);
    fill(port3, 128, 0x55);

    CHECK_EQ(g.call(0xF11FE0F1u, 0, queue), A11);
    CHECK_EQ(g.call(0xF11FE0F1u, port, 0), A11);
    CHECK_EQ(g.call(0xF11FE0F1u, port + 64, queue), A10);
    CHECK_EQ(g.call(0xF11FE0F1u, port, queue + 64), A10);
    CHECK_EQ(g.call(0xF11FE0F1u, port, queue + 0x2000), A0F); /* no live queue */

    CHECK_EQ(g.call(0xF11FE0F1u, port, queue), 0u);

    /* 128 bytes: only +0x08, +0x18, +0x1C and +0x20 are written. */
    for (uint32_t off = 0; off < 128; ++off) {
        const uint8_t want = (off == 0x20) ? 1 : 0;
        if (off >= 0x08 && off < 0x0C) continue;
        if (off >= 0x18 && off < 0x20) continue;
        CHECK_EQ(rd8(port + off), want);
    }
    CHECK_EQ(rd32(port + 0x08), queue);
    CHECK_EQ(rd32(port + 0x18), queue);
    CHECK_EQ(rd32(port + 0x1C), 0u); /* first slot */
    CHECK_EQ(rd32(queue + 0x70), 1u);
    CHECK_EQ(rd32(queue + 0x580), 1u);

    /* A second slot, then its release. */
    CHECK_EQ(g.call(0xF11FE0F1u, port2, queue), 0u);
    CHECK_EQ(rd32(port2 + 0x1C), 1u);
    CHECK_EQ(rd32(queue + 0x580), 3u);
    CHECK_EQ(g.call(0xC485D207u, port2), 0u);
    CHECK_EQ(rd32(queue + 0x580), 1u);
    CHECK_EQ(rd32(queue + 0x70), 1u);

    /* Slot exhaustion is AGAIN (0x020046F0-0x02004814). */
    wr32(queue + 0x70, 0x400);
    CHECK_EQ(g.call(0xF11FE0F1u, port3, queue), A01);
    CHECK_EQ(rd8(port3 + 0x20), 0u);
    wr32(queue + 0x70, 1);

    /* Destroy guards. */
    CHECK_EQ(g.call(0xC485D207u, 0), A11);
    CHECK_EQ(g.call(0xC485D207u, port + 64), A10);
    CHECK_EQ(g.call(0xC485D207u, port3), A0F); /* never initialised */

    /* BUSY while submissions B are outstanding; the port stays attached. */
    wr32(port + 0x10, 1);
    CHECK_EQ(g.call(0xC485D207u, port), A0A);
    CHECK_EQ(rd8(port + 0x20), 1u);
    CHECK_EQ(rd32(port + 0x08), queue);
    CHECK_EQ(rd32(queue + 0x70), 1u);
    wr32(port + 0x10, 0);

    CHECK_EQ(g.call(0xC485D207u, port), 0u);
    CHECK_EQ(rd8(port + 0x20), 0u);
    CHECK_EQ(rd32(port + 0x08), 0u);
    CHECK_EQ(rd32(port + 0x18), 0u);
    CHECK_EQ(rd32(queue + 0x70), 0u);
    CHECK_EQ(rd32(queue + 0x580), 0u);

    /* Put the port back for the push/sync cases (slot 0 again). */
    CHECK_EQ(g.call(0xF11FE0F1u, port, queue), 0u);
    CHECK_EQ(rd32(port + 0x1C), 0u);
}

static void port_destroy_state_tests(Guest& g, uint32_t queue, uint32_t port)
{
    (void)queue;
    /* Firmware order (0x02004874-0x020048B8): +0x20 init -> +0x21 state ->
     * +0x18 queue link -> +0x10 B. A non-zero +0x21 is STAT even while B is
     * outstanding, so the state check precedes BUSY. */
    wr8(port + 0x21, 1);
    wr32(port + 0x10, 1);
    CHECK_EQ(g.call(0xC485D207u, port), A0F);
    CHECK_EQ(rd8(port + 0x20), 1u);
    CHECK_EQ(rd32(port + 0x08), queue);
    CHECK_EQ(rd32(port + 0x10), 1u);

    wr8(port + 0x21, 0);
    wr32(port + 0x10, 2);
    const uint32_t before = rd32(port + 0x08);
    CHECK_EQ(g.call(0xC485D207u, port), A0A);
    CHECK_EQ(rd32(port + 0x08), before);
    CHECK_EQ(rd8(port + 0x20), 1u);
    wr32(port + 0x10, 0);
}

static void push_and_sync_tests(Guest& g, uint32_t queue, uint32_t port, uint32_t other_port)
{
    const uint32_t desc = alloc(256, 16);
    fill(desc, 256, 0);

    /* PushJobBody guards (0x02004E48). */
    CHECK_EQ(g.call(0xCF89F218u, 0, desc, 64, 0, 0), A11);
    CHECK_EQ(g.call(0xCF89F218u, port + 64, desc, 64, 0, 0), A10);
    CHECK_EQ(g.call(0xCF89F218u, other_port, desc, 64, 0, 0), A0F); /* uninitialised */
    CHECK_EQ(g.call(0xCF89F218u, port, 0, 64, 0, 0), A11);
    CHECK_EQ(g.call(0xCF89F218u, port, desc + 1, 64, 0, 0), A10);
    CHECK_EQ(g.call(0xCF89F218u, port, desc, 32, 0, 0), A02);
    CHECK_EQ(g.call(0xCF89F218u, port, desc, 1024, 0, 0), A02);
    CHECK_EQ(g.call(0xCF89F218u, port, desc, 64, 16, 0), A02);

    /* CopyPush guards (0x02004E0C). A queue without a pool has no class and
     * returns NULL_POINTER for the unconfigured class (jq2c-015). */
    CHECK_EQ(g.call(0xC39173A4u, 0, desc, 64, 64, 0, 1), A11);
    CHECK_EQ(g.call(0xC39173A4u, port + 64, desc, 64, 64, 0, 1), A10);
    CHECK_EQ(g.call(0xC39173A4u, port, desc, 128, 64, 0, 1), A02);
    CHECK_EQ(g.call(0xC39173A4u, port, desc + 1, 64, 64, 0, 1), A10);
    CHECK_EQ(g.call(0xC39173A4u, port, desc, 64, 64, 16, 1), A02);
    CHECK_EQ(g.call(0xC39173A4u, port, desc, 64, 64, 0, 1), A11); /* no pool */

    /* PushSync guards (0x020042D8). Mask 0 is INVAL and leaves the port
     * bytes exactly as they were. */
    uint8_t before[128];
    std::memcpy(before, vm_base + port, sizeof before);
    CHECK_EQ(g.call(0xFC526B72u, 0, 1, 0), A11);
    CHECK_EQ(g.call(0xFC526B72u, port + 64, 1, 0), A10);
    CHECK_EQ(g.call(0xFC526B72u, other_port, 1, 0), A0F);
    CHECK_EQ(g.call(0xFC526B72u, port, 0, 0), A02);
    CHECK_EQ(g.call(0xFC526B72u, port, 0x10000, 0), A02);
    CHECK_EQ(g.call(0xFC526B72u, port, 1, 2), A02);
    CHECK_EQ(std::memcmp(before, vm_base + port, sizeof before), 0);

    /* A valid SYNC with no jobs in flight; the workers consume it. */
    CHECK_EQ(g.call(0xFC526B72u, port, 1, 0), 0u);
    CHECK_EQ(std::memcmp(before, vm_base + port, sizeof before), 0);

    /* Port2Sync guards (0x020045D8). */
    CHECK_EQ(g.call(0xFACB3CEDu, 0, 4), A11);
    CHECK_EQ(g.call(0xFACB3CEDu, port + 64, 4), A10);
    CHECK_EQ(g.call(0xFACB3CEDu, other_port, 4), A0F);
    CHECK_EQ(g.call(0xFACB3CEDu, port, 8), A02);

    /* Mode 4 with A < B is AGAIN, and the claim is released. */
    wr32(port + 0x00, 0);
    wr32(port + 0x10, 1);
    wr32(port + 0x14, 0);
    CHECK_EQ(g.call(0xFACB3CEDu, port, 4), A01);
    CHECK_EQ(rd32(port + 0x10), 1u);
    CHECK_EQ((rd32(port + 0x14) & 0xFF000000u), 0u);
    CHECK_EQ((rd32(port + 0x00) & 0x1FFFFFFFu), 0u);

    /* Mode 4 with A == B completes: A := A - B, B := 0. */
    wr32(port + 0x00, 1);
    wr32(port + 0x10, 1);
    CHECK_EQ(g.call(0xFACB3CEDu, port, 4), 0u);
    CHECK_EQ(rd32(port + 0x00), 0u);
    CHECK_EQ(rd32(port + 0x10), 0u);

    /* Mode 0 also completes when A >= B without waiting. */
    wr32(port + 0x00, 2);
    wr32(port + 0x10, 2);
    CHECK_EQ(g.call(0xFACB3CEDu, port, 0), 0u);
    CHECK_EQ(rd32(port + 0x00), 0u);
    CHECK_EQ(rd32(port + 0x10), 0u);

    /* A held claim is BUSY, and it leaves A, B and the claim alone. */
    wr32(port + 0x00, 0);
    wr32(port + 0x10, 0);
    wr32(port + 0x14, 0x01000000u);
    CHECK_EQ(g.call(0xFACB3CEDu, port, 4), A0A);
    CHECK_EQ((rd32(port + 0x14) & 0xFF000000u), 0x01000000u);
    wr32(port + 0x14, 0);

    (void)queue;
}

/* ---- teardown ----------------------------------------------------------- */

static void teardown_tests(Guest& g, uint32_t queue, uint32_t port, uint32_t out)
{
    /* Shutdown refuses a live port with BUSY. */
    CHECK_EQ(g.call(0x8C97A96Cu, 0), A11);
    CHECK_EQ(g.call(0x8C97A96Cu, queue + 64), A10);
    CHECK_EQ(g.call(0x8C97A96Cu, queue), A0A);
    CHECK_EQ(g.call(0xC485D207u, port), 0u);
    CHECK_EQ(g.call(0x8C97A96Cu, queue), 0u);

    /* Join writes queue+0xF04 as a big-endian guest u32. */
    wr32(queue + 0xF04, 0x13572468u);
    fill(out, 16, 0xEE);
    CHECK_EQ(g.call(0x8CE4CFFAu, 0, out), A11);
    CHECK_EQ(g.call(0x8CE4CFFAu, queue, 0), A11);
    CHECK_EQ(g.call(0x8CE4CFFAu, queue + 64, out), A10);
    CHECK_EQ(g.call(0x8CE4CFFAu, queue, out), 0u);
    CHECK_EQ(rd32(out), 0x13572468u);
    CHECK_EQ(rd32(queue + 0x6C) & 0x20u, 0x20u);
    CHECK_EQ(g.call(0x8C97A96Cu, queue), A0F); /* no longer live */
    CHECK_EQ(g.call(0x8CE4CFFAu, queue, out), A0F);
}

/* ---- main --------------------------------------------------------------- */

int main()
{
    vm_base = static_cast<uint8_t*>(std::calloc(kArena, 1));
    if (!vm_base) return 2;

    Guest g;

    const uint32_t attr = alloc(kAttrBytes, 16);
    const uint32_t spurs = alloc(0x4000, 128);
    const uint32_t queue = alloc(0x1000, 128);
    const uint32_t ring = alloc(0x4000, 128);
    const uint32_t port = alloc(128, 128);
    const uint32_t port2 = alloc(128, 128);
    const uint32_t port3 = alloc(128, 128);
    const uint32_t out = alloc(16, 16);

    nid_and_registration_tests();

    fill(ring, 0x4000, 0xAA);
    fill(queue, 0x1000, 0xAA);
    attribute_tests(g, attr);
    create_validation_tests(g, spurs, queue, ring, attr);
    create_10_args(g, spurs, queue, ring, attr);
    port_create_tests(g, queue, port, port2, port3);
    port_destroy_state_tests(g, queue, port);
    push_and_sync_tests(g, queue, port, port2);
    teardown_tests(g, queue, port, out);

    /* The pool variant, on its own queue. */
    const uint32_t attr2 = alloc(kAttrBytes, 16);
    const uint32_t spurs2 = alloc(0x4000, 128);
    const uint32_t queue2 = alloc(0x1000, 128);
    const uint32_t ring2 = alloc(0x4000, 128);
    const uint32_t pool = alloc(0x4000, 128);
    const uint32_t pool_attr = alloc(0x100, 128);
    const uint32_t port4 = alloc(128, 128);
    const uint32_t out2 = alloc(16, 16);

    fill(attr2, kAttrBytes, 0);
    fill(pool_attr, 0x100, 0);
    fill(ring2, 0x4000, 0xAA);
    fill(queue2, 0x1000, 0xAA);
    CHECK_EQ(g.call(0x43DDAB4Fu, attr2), 0u);
    CHECK_EQ(g.call(0x1686957Eu, attr2, 896), 0u);
    CHECK_EQ(g.call(0xE70F874Eu, attr2, 255, 4), 0u);
    wr32(pool_attr + 4 * 2, 1); /* one 256-byte block */

    create_12_args(g, spurs2, queue2, ring2, attr2, pool, pool_attr);

    CHECK_EQ(g.call(0xF11FE0F1u, port4, queue2), 0u);
    const uint32_t desc = alloc(256, 16);
    fill(desc, 256, 0);
    /* With a pool, a configured class is still not pushed here: a successful
     * push would run a synthetic job, which test_jq2_executor covers. */
    CHECK_EQ(g.call(0xC39173A4u, port4, desc, 128, 64, 0, 1), A02);
    CHECK_EQ(g.call(0xC39173A4u, port4, desc, 64, 100, 0, 1), A02);
    CHECK_EQ(g.call(0xC39173A4u, port4, desc, 64, 512, 0, 1), A11); /* class 4 unconfigured */
    CHECK_EQ(g.call(0xC39173A4u, port4, desc + 1, 64, 64, 0, 1), A10);

    CHECK_EQ(g.call(0x8C97A96Cu, queue2), A0A); /* port still attached */
    CHECK_EQ(g.call(0xC485D207u, port4), 0u);
    CHECK_EQ(g.call(0x8C97A96Cu, queue2), 0u);
    wr32(queue2 + 0xF04, 0x0BADF00Du);
    CHECK_EQ(g.call(0x8CE4CFFAu, queue2, out2), 0u);
    CHECK_EQ(rd32(out2), 0x0BADF00Du);

    std::free(vm_base);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS: cellSpursJq HLE surface (17 exports, attribute layout, guest guards, save-area args)\n");
    return 0;
}

/*
 * ps3recomp - HLE typed-call adapter contract (PPC64 ELFv1 -> host C)
 *
 * Calls handlers through the real ps3_hle_register + ps3_hle_adapt and
 * ps3_hle_call, and checks what the guest sees:
 *
 *   declared s32 return      sign-extended into r3
 *   declared u32/bool return zero-extended into r3
 *   declared u64 return      unchanged
 *   declared void return     r3 untouched
 *   declared double return   f1, r3 untouched
 *   positional args          r3 low word for s32 even with garbage above it,
 *                            a double from f1, the next u32 from r5 (not r4)
 *   9th/10th parameters      from the caller's parameter save area
 *   guest-EA return opt-in   low 32 bits zero-extended (cellSheapAllocate)
 *   host-pointer return      stops with hle-unsupported-return (subprocess)
 *   variadic                 fixed fmt by position, r4..r10 reach va_start
 *
 * Built and run by tools/test_hle_abi.py, which compiles the real
 * runtime/ppu/ppu_hle.cpp and libs/misc/cellSheap.c with this fixture.
 */
#include "ppu_context.h"
#include "ppu_hle_adapt.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx);

/* ---- host memory the runtime's vm_* accessors read (big-endian) ---------- */
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void* g_ps3_guest_caller = nullptr;
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0;

static uint8_t g_mem[0x10000];

extern "C" {
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t) { std::abort(); }
void ppu_prof_stamp(void*, unsigned) {}
void ppu_gcm_pump() {}
uint32_t ppu_prof_resolve_host(void*) { return 0; }
void ps3_msf(const char*, ...) {}
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) {}
void spu_lockline_lock() {}
void spu_lockline_unlock() {}
void ppu_resv_break_store(uint64_t) {}
void ps3_ww_report_inline(uint32_t, uint64_t, int) {}
void ppu_dump_guest_stack(ppu_context*, const char*) {}
void ppu_dump_bctrl_ring(uint32_t, const char*) {}
void ppu_guest_callstack(const char*) {}

uint32_t vm_read32(uint64_t a)
{
    uint32_t v = 0;
    std::memcpy(&v, g_mem + (uint32_t)a, 4);
    return __builtin_bswap32(v);
}
void vm_write32(uint64_t a, uint32_t v)
{
    v = __builtin_bswap32(v);
    std::memcpy(g_mem + (uint32_t)a, &v, 4);
}
uint64_t vm_read64(uint64_t a)
{
    uint64_t v = 0;
    std::memcpy(&v, g_mem + (uint32_t)a, 8);
    return __builtin_bswap64(v);
}
} /* extern "C" */

/* ppu_hle.cpp declares vm_write64 with C++ linkage (the TOC save). */
void vm_write64(uint64_t a, uint64_t v)
{
    v = __builtin_bswap64(v);
    std::memcpy(g_mem + (uint32_t)a, &v, 8);
}

static void put64(uint32_t ea, uint64_t v) { vm_write64(ea, v); }
static void put32(uint32_t ea, uint32_t v) { vm_write32(ea, v); }
static void putstr(uint32_t ea, const char* s)
{
    while (*s) g_mem[ea++] = (uint8_t)*s++;
    g_mem[ea] = 0;
}

/* ---- handlers of each return/parameter class ---------------------------- */
static int g_void_calls;
static s32  mock_s32(s32 a) { (void)a; return (s32)0x8001000F; }
static u32  mock_u32(u32 a) { (void)a; return 0x80000000u; }
static u64  mock_u64(u64 a) { (void)a; return 0x0123456789ABCDEFull; }
static bool mock_bool(bool a) { (void)a; return true; }
static void mock_void(void) { g_void_calls++; }
static double mock_double(void) { return 2.5; }
static s32 mock_mixed(s32 a, double d, u32 b) { return a + (s32)d + (s32)b; }
static s64 mock_nine(s64 a, s64 b, s64 c, s64 d, s64 e,
                     s64 f, s64 g, s64 h, s64 i, s64 j)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
    return i + j * 10;
}
static void* mock_guest_ea(void) { return (void*)(uintptr_t)0xFFFFFFFFABCDEF01ull; }
static void* mock_host_ptr(void) { return (void*)(uintptr_t)0x0000000100001234ull; }

static char g_vfmt[128];
static s32 mock_variadic(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int d = va_arg(ap, int);
    /* A-pointer vararg is a guest EA in its low 32 bits, like every HLE pointer. */
    const char* s = (const char*)(vm_base + (uintptr_t)va_arg(ap, const char*));
    const unsigned long long x = va_arg(ap, unsigned long long);
#if defined(__aarch64__) && defined(__APPLE__)
    const double f = va_arg(ap, double);
    const int n = std::snprintf(g_vfmt, sizeof g_vfmt, "%d|%s|%llx|%.1f",
                                d, s, x, f);
#else
    const int n = std::snprintf(g_vfmt, sizeof g_vfmt, "%d|%s|%llx", d, s, x);
#endif
    va_end(ap);
    return n;
}

/* ---- registration (the real typed API) ---------------------------------- */
enum : uint32_t {
    NID_S32 = 0x10000001u, NID_U32 = 0x10000002u, NID_U64 = 0x10000003u,
    NID_BOOL = 0x10000004u, NID_VOID = 0x10000005u, NID_FP = 0x10000006u,
    NID_MIXED = 0x10000007u, NID_NINE = 0x10000008u, NID_EA = 0x10000009u,
    NID_HOSTPTR = 0x1000000Au, NID_VAR = 0x1000000Bu,
    NID_SHEAP_INIT = 0x1000000Cu, NID_SHEAP_ALLOC = 0x1000000Du,
};

static void register_handlers()
{
    ps3_hle_register(NID_S32, "mock_s32", &ps3_hle_adapt<&mock_s32>);
    ps3_hle_register(NID_U32, "mock_u32", &ps3_hle_adapt<&mock_u32>);
    ps3_hle_register(NID_U64, "mock_u64", &ps3_hle_adapt<&mock_u64>);
    ps3_hle_register(NID_BOOL, "mock_bool", &ps3_hle_adapt<&mock_bool>);
    ps3_hle_register(NID_VOID, "mock_void", &ps3_hle_adapt<&mock_void>);
    ps3_hle_register(NID_FP, "mock_double", &ps3_hle_adapt<&mock_double>);
    ps3_hle_register(NID_MIXED, "mock_mixed", &ps3_hle_adapt<&mock_mixed>);
    ps3_hle_register(NID_NINE, "mock_nine", &ps3_hle_adapt<&mock_nine>);
    ps3_hle_register(NID_EA, "mock_guest_ea", &ps3_hle_adapt_guest_ea<&mock_guest_ea>);
    ps3_hle_register(NID_HOSTPTR, "mock_host_ptr", &ps3_hle_adapt_host_ptr_stop<&mock_host_ptr>);
    ps3_hle_register(NID_VAR, "mock_variadic", &ps3_hle_adapt_variadic<&mock_variadic>);
}

static int fails;
#define CHECK(c, m) do { \
    if (c) std::printf("[PASS] %s\n", m); \
    else { std::printf("[FAIL] %s\n", m); fails++; } } while (0)

static ppu_context make_ctx()
{
    ppu_context ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.gpr[1] = 0x8000;      /* inside the fixture's guest memory */
    ctx.gpr[2] = 0x12345678;
    ctx.thread_id = 1;
    ctx.lr = 0x10100;
    return ctx;
}

static void call(uint32_t nid, ppu_context* ctx) { ps3_hle_call(nid, ctx); }

static void abi_tests()
{
    ppu_context ctx;

    ctx = make_ctx();
    call(NID_S32, &ctx);
    std::printf("s32 -> r3 = 0x%016llX\n", (unsigned long long)ctx.gpr[3]);
    CHECK(ctx.gpr[3] == 0xFFFFFFFF8001000Full, "s32 -0x7FFEFFF1 sign-extends into r3");

    ctx = make_ctx();
    call(NID_U32, &ctx);
    CHECK(ctx.gpr[3] == 0x0000000080000000ull, "u32 with bit 31 set zero-extends into r3");

    ctx = make_ctx();
    call(NID_U64, &ctx);
    CHECK(ctx.gpr[3] == 0x0123456789ABCDEFull, "u64 return is unchanged in r3");

    ctx = make_ctx();
    call(NID_BOOL, &ctx);
    CHECK(ctx.gpr[3] == 1, "bool true returns 1 in r3");

    ctx = make_ctx();
    ctx.gpr[3] = 0xAAAA5555DEADBEEFull;
    const int before = g_void_calls;
    call(NID_VOID, &ctx);
    CHECK(g_void_calls == before + 1 && ctx.gpr[3] == 0xAAAA5555DEADBEEFull,
          "void handler runs and leaves r3 untouched");

    ctx = make_ctx();
    ctx.gpr[3] = 0x1111222233334444ull;
    call(NID_FP, &ctx);
    CHECK(ctx.fpr[1] == 2.5 && ctx.gpr[3] == 0x1111222233334444ull,
          "double result lands in f1 and leaves r3 untouched");

    /* (s32 a, double d, u32 b): a from r3's LOW WORD (garbage above), d from
     * f1, b from r5 -- the double consumes its positional slot r4. */
    ctx = make_ctx();
    ctx.gpr[3] = 0xDEADBEEF00000005ull;
    ctx.fpr[1] = 10.0;
    ctx.gpr[4] = 0x9999999999999999ull;   /* must not be read as b */
    ctx.gpr[5] = 7;
    call(NID_MIXED, &ctx);
    CHECK(ctx.gpr[3] == 22, "mixed args: s32 low word, double from f1, u32 from r5");

    /* Nine/ten parameters: 9 is at r1+48+8*8, 10 at r1+48+8*9 (ELFv1 save area). */
    ctx = make_ctx();
    for (int r = 3; r <= 10; r++) ctx.gpr[r] = 0xFFFFFFFFFFFFFFFFull;
    put64(0x8000 + 48 + 8 * 8, 11);
    put64(0x8000 + 48 + 8 * 9, 22);
    call(NID_NINE, &ctx);
    CHECK(ctx.gpr[3] == 231, "parameters 9 and 10 come from the caller's save area");

    /* Guest-EA pointer return: low 32 zero-extended (bit 31 set stays positive). */
    ctx = make_ctx();
    call(NID_EA, &ctx);
    CHECK(ctx.gpr[3] == 0x00000000ABCDEF01ull, "guest-EA pointer return zero-extends the low 32 bits");

    /* Variadic: fixed fmt by position, r4..r10 forwarded to va_start. */
    putstr(0x3000, "%d|%s|%llx");
    putstr(0x3100, "hello");
    ctx = make_ctx();
    ctx.gpr[3] = 0x3000;
    ctx.gpr[4] = (uint64_t)(uint32_t)-7;         /* int -7 in the low word */
    ctx.gpr[5] = 0x3100;                         /* guest char* */
    ctx.gpr[6] = 0xDEADBEEFCAFEBABEull;
#if defined(__aarch64__) && defined(__APPLE__)
    uint64_t fbits;
    {
        const double d = 2.5;
        std::memcpy(&fbits, &d, 8);
    }
    ctx.gpr[7] = fbits;                          /* double in the GPR image */
#endif
    call(NID_VAR, &ctx);
#if defined(__aarch64__) && defined(__APPLE__)
    CHECK(std::strcmp(g_vfmt, "-7|hello|deadbeefcafebabe|2.5") == 0,
          "variadic int/pointer/u64/double reach va_start");
#else
    CHECK(std::strcmp(g_vfmt, "-7|hello|deadbeefcafebabe") == 0,
          "variadic int/pointer/u64 reach va_start (double is host-specific)");
#endif
}

/* Item 3: cellSheapAllocate must return the guest EA, checked through the
 * generated-style guest-EA adapter and ps3_hle_call. */
extern "C" s32 cellSheapInitialize(const void* attr, void* handle);
extern "C" void* cellSheapAllocate(u32 handle, u32 size);

static void sheap_test()
{
    ps3_hle_register(NID_SHEAP_INIT, "cellSheapInitialize", &ps3_hle_adapt<&cellSheapInitialize>);
    ps3_hle_register(NID_SHEAP_ALLOC, "cellSheapAllocate", &ps3_hle_adapt_guest_ea<&cellSheapAllocate>);

    /* CellSheapAttr { void* heapBase; u32 heapSize; u32 align; } as the guest
     * lays it out: a 4-byte EA to us, so read/written by guest offset. */
    put32(0x2000 + 0, 0x4000);    /* heapBase */
    put32(0x2000 + 4, 0x400);     /* heapSize */
    put32(0x2000 + 8, 16);        /* align */

    ppu_context ctx = make_ctx();
    ctx.gpr[3] = 0x2000;
    ctx.gpr[4] = 0x2100;
    call(NID_SHEAP_INIT, &ctx);
    CHECK(ctx.gpr[3] == 0, "cellSheapInitialize returns CELL_OK through the adapter");
    CHECK(vm_read32(0x2100) == 0, "cellSheapInitialize writes handle 0 to the guest");

    ctx = make_ctx();
    ctx.gpr[3] = 0;
    ctx.gpr[4] = 0x20;
    call(NID_SHEAP_ALLOC, &ctx);
    std::printf("cellSheapAllocate -> r3 = 0x%08X\n", (unsigned)ctx.gpr[3]);
    CHECK(ctx.gpr[3] == 0x4000, "cellSheapAllocate returns the guest EA of the block");

    ctx = make_ctx();
    ctx.gpr[3] = 0;
    ctx.gpr[4] = 0x20;
    call(NID_SHEAP_ALLOC, &ctx);
    CHECK(ctx.gpr[3] == 0x4020, "the next block is heap_ea + the aligned size");
}

int main(int argc, char** argv)
{
    vm_base = g_mem;
    ppu_vm_size = sizeof g_mem;
    std::memset(g_mem, 0, sizeof g_mem);
    register_handlers();

    if (argc == 2 && std::strcmp(argv[1], "hostptr") == 0) {
        /* A host-pointer return has no guest value: the call must stop. The
         * runner checks the exit code (70) and the machine line. */
        ppu_context ctx = make_ctx();
        call(NID_HOSTPTR, &ctx);
        std::printf("RETURNED r3=0x%llX\n", (unsigned long long)ctx.gpr[3]);
        return 0;
    }

    abi_tests();
    sheap_test();

    std::printf("\nResults: %s (%d failure(s))\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}

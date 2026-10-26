/* The synthetic HLE implementations compared against the synthetic lifted
 * module. Registered through the normal context-handler mechanism under
 * synthetic NIDs; no library is involved.
 *
 * Each *_wrong handler mutates exactly one comparator dimension relative to the
 * firmware twin in module.c; cases_synth.cpp declares the resulting verdict. */
#include "dh_engine.h"
#include "ppu/ppu_context.h"
#include "ps3emu/stop.h"

#include <cstdint>

extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx);

namespace {
constexpr uint32_t N_HELPER      = 0x91000100;
constexpr uint32_t N_RET_OK      = 0x91000001;
constexpr uint32_t N_WRITE_OK    = 0x91000002;
constexpr uint32_t N_IMPORT_OK   = 0x91000003;
constexpr uint32_t N_FAULT_OK    = 0x91000004;
constexpr uint32_t N_HANG        = 0x91000005;
constexpr uint32_t N_ASM_ADD     = 0x91000006;
constexpr uint32_t N_CPP_MUL     = 0x91000007;
constexpr uint32_t N_RET_WRONG   = 0x91000011;
constexpr uint32_t N_WRITE_MISS  = 0x91000012;
constexpr uint32_t N_PAD_EXTRA   = 0x91000013;
constexpr uint32_t N_IMPORT_SWAP = 0x91000014;
constexpr uint32_t N_FAULT_SPAN  = 0x91000015;
constexpr uint32_t N_FAULT_RET   = 0x91000016;
constexpr uint32_t N_FAULT_ZERO  = 0x91000017;
constexpr uint32_t N_MASK_ADJ    = 0x91000018;
constexpr uint32_t N_OVR_OK      = 0x91000021;
constexpr uint64_t FAULT_EA      = 0x90000000ull;

/* The imported helper has no effect on the return value or on memory: only the
 * import-call record can tell a correct call from a swapped one. */
void hle_helper(ppu_context* c) { c->gpr[3] = 0; }

void hle_ret_ok(ppu_context* c)    { c->gpr[3] = (uint32_t)(c->gpr[3] * 3u + 1u); }
void hle_ret_wrong(ppu_context* c) { c->gpr[3] = (uint32_t)(c->gpr[3] * 3u + 2u); }
void hle_write_ok(ppu_context* c)
{
    dh::wr32((uint32_t)c->gpr[3], 0xDEADBEEFu);
    c->gpr[3] = 0;
}
void hle_write_missing(ppu_context* c) { c->gpr[3] = 0; }
void hle_pad_extra(ppu_context* c)
{
    const uint32_t dst = (uint32_t)c->gpr[3], src = (uint32_t)c->gpr[4];
    for (int i = 0; i < 8; ++i) dh::mem[dst + i] = dh::mem[src + i];
    dh::fill(dst + 8, 24, 0);   /* the strncpy-style padding the firmware does not write */
    c->gpr[3] = 0;
}
void hle_import_ok(ppu_context* c)
{
    ps3_hle_call(N_HELPER, c);
    c->gpr[3] = (uint32_t)c->gpr[3] + 1u;
}
void hle_import_swap(ppu_context* c)
{
    const uint64_t a = c->gpr[3], b = c->gpr[4];
    c->gpr[3] = b; c->gpr[4] = a;
    ps3_hle_call(N_HELPER, c);
    c->gpr[3] = (uint32_t)c->gpr[3] + 1u;
}
void hle_fault_ok(ppu_context* c)
{
    (void)c;
    ps3_stop_guest_range("synth_fault_ok", "p", FAULT_EA, 4, dh::ARENA_SIZE);
}
void hle_fault_span(ppu_context* c)
{
    (void)c;
    ps3_stop_guest_range("synth_fault_span", "p", 0x70000000ull, 4, dh::ARENA_SIZE);
}
void hle_fault_zero(ppu_context* c)
{
    (void)c;
    ps3_stop_guest_range("synth_fault_zero", "p", FAULT_EA, 0, dh::ARENA_SIZE);
}
void hle_fault_return(ppu_context* c) { c->gpr[3] = 0x80010001u; }
void hle_hang(ppu_context* c)
{
    (void)c;
    volatile uint32_t spin = 0;
    for (;;) spin = spin * 3u + 1u;
}
void hle_asm_add(ppu_context* c) { c->gpr[3] = (uint32_t)c->gpr[3] + (uint32_t)c->gpr[4]; }
void hle_cpp_mul(ppu_context* c) { c->gpr[3] = (uint32_t)(c->gpr[3] * c->gpr[4] + 7u); }
void hle_ovr_ok(ppu_context* c)  { c->gpr[3] = (uint32_t)(c->gpr[3] * 3u + 1u); }
}  // namespace

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

extern "C" void ppu_hle_register_all(void)
{
    ps3_hle_register_ctx(N_HELPER,      "synth_helper",       hle_helper);
    ps3_hle_register_ctx(N_RET_OK,      "synth_ret_ok",       hle_ret_ok);
    ps3_hle_register_ctx(N_WRITE_OK,    "synth_write_ok",     hle_write_ok);
    ps3_hle_register_ctx(N_IMPORT_OK,   "synth_import_ok",    hle_import_ok);
    ps3_hle_register_ctx(N_FAULT_OK,    "synth_fault_ok",     hle_fault_ok);
    ps3_hle_register_ctx(N_HANG,        "synth_hang",         hle_hang);
    ps3_hle_register_ctx(N_ASM_ADD,     "synth_asm_add",      hle_asm_add);
    ps3_hle_register_ctx(N_CPP_MUL,     "synth_cpp_mul",      hle_cpp_mul);
    ps3_hle_register_ctx(N_RET_WRONG,   "synth_ret_wrong",    hle_ret_wrong);
    ps3_hle_register_ctx(N_WRITE_MISS,  "synth_write_missing", hle_write_missing);
    ps3_hle_register_ctx(N_PAD_EXTRA,   "synth_pad_extra",    hle_pad_extra);
    ps3_hle_register_ctx(N_IMPORT_SWAP, "synth_import_swap",  hle_import_swap);
    ps3_hle_register_ctx(N_FAULT_SPAN,  "synth_fault_span",   hle_fault_span);
    ps3_hle_register_ctx(N_FAULT_RET,   "synth_fault_return", hle_fault_return);
    ps3_hle_register_ctx(N_FAULT_ZERO,  "synth_fault_zero",   hle_fault_zero);
    ps3_hle_register_ctx(N_MASK_ADJ,    "synth_mask_adjacent", hle_pad_extra);
    ps3_hle_register_ctx(N_OVR_OK,      "synth_override",     hle_ovr_ok);
    /* N_RET's no-HLE twin 0x91000008 is deliberately not registered. */
}

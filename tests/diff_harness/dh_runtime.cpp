/* The few runtime symbols the lifted C and the HLE core expect from a host.
 * Single-threaded and inert by design: this is a call-level harness, not a
 * title. host_stubs.c (compiled alongside) supplies the vm_write*, vm_read32
 * and diagnostic hooks. */
#include "ppu/ppu_context.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <unistd.h>

extern "C" {
uint8_t* vm_base = nullptr;

int      vm_inline_ok = 1;                       /* lifted loads take the inline path */
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = nullptr;

uint8_t  vm_read8(uint64_t a)  { return vm_base[(uint32_t)a]; }
uint16_t vm_read16(uint64_t a) { uint16_t v; memcpy(&v, vm_base + (uint32_t)a, 2); return __builtin_bswap16(v); }
uint64_t vm_read64(uint64_t a) { uint64_t v; memcpy(&v, vm_base + (uint32_t)a, 8); return __builtin_bswap64(v); }

/* Reservations: one host thread, so a plain reserve/compare is exact. */
uint32_t ppu_lwarx32(ppu_context* c, uint64_t a)
{ c->reserve_addr = a; c->reserve_valid = 4; uint32_t v; memcpy(&v, vm_base + (uint32_t)a, 4); return __builtin_bswap32(v); }
uint64_t ppu_ldarx64(ppu_context* c, uint64_t a)
{ c->reserve_addr = a; c->reserve_valid = 8; return vm_read64(a); }
int ppu_stwcx32(ppu_context* c, uint64_t a, uint32_t v)
{ if (c->reserve_valid != 4 || c->reserve_addr != a) { c->reserve_valid = 0; return 0; }
  v = __builtin_bswap32(v); memcpy(vm_base + (uint32_t)a, &v, 4); c->reserve_valid = 0; return 1; }
int ppu_stdcx64(ppu_context* c, uint64_t a, uint64_t v)
{ if (c->reserve_valid != 8 || c->reserve_addr != a) { c->reserve_valid = 0; return 0; }
  v = __builtin_bswap64(v); memcpy(vm_base + (uint32_t)a, &v, 8); c->reserve_valid = 0; return 1; }

/* The lifted code reached a syscall the harness does not model. */
void dh_syscall(ppu_context* c);          /* dh_engine.cpp: model or named stop */
void lv2_syscall(ppu_context* c) { dh_syscall(c); }
void ppu_unlifted_stub(uint64_t addr, ppu_context*)
{ fprintf(stderr, "kind=dh-unlifted-function addr=0x%llX\n", (unsigned long long)addr); _exit(72); }
void ppu_trap_unknown_instruction(ppu_context*, uint64_t pc, uint32_t word, const char* mn)
{ fprintf(stderr, "kind=dh-unknown-instruction pc=0x%llX word=0x%08X %s\n", (unsigned long long)pc, word, mn); _exit(73); }

/* ppu_hle.cpp references; none is reached by an HLE function called directly. */
uint32_t ppu_prof_resolve_host(void*) { return 0; }
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{ fprintf(stderr, "kind=dh-guest-call\n"); _exit(74); }
}

/* ppu_hle.cpp declares vm_write64 with C++ linkage (host_stubs.c defines the C one). */
void vm_write64(uint64_t ea, uint64_t v) { v = __builtin_bswap64(v); memcpy(vm_base + (uint32_t)ea, &v, 8); }

/* The lift's store hook (tools/ppu_lifter.py PPU_STORE_HOOK) is linked out of
 * ppu_loader.cpp, which a host fixture does not build. The window itself is
 * defined by the runtime library (runtime/memory/label_window.c); no RSX label
 * window exists here, so the window stays empty and the hook is never reached. */
extern "C" {
/* cellGcmSys.c defines this too; weak keeps the link valid whichever member
 * the archive pulls (the fixture never injects code through it). */
__attribute__((weak)) uint32_t ppu_hle_inject_base = 0;
void ppu_store_hook_span(uint64_t, uint32_t) {}
}

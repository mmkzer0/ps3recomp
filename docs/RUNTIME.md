# ps3recomp Runtime Reference

Complete reference for the ps3recomp core runtime layer — the foundation that makes recompiled PS3 code run natively on any host platform.

---

## Table of Contents

1. [Overview](#overview)
2. [Virtual Memory Manager](#virtual-memory-manager)
3. [PPU Execution Context](#ppu-execution-context)
4. [SPU Execution Context](#spu-execution-context)
5. [Type System](#type-system)
6. [Endianness Layer](#endianness-layer)
7. [Syscall Dispatch](#syscall-dispatch)
8. [DMA Engine](#dma-engine)

---

## Overview

The runtime is the bridge between recompiled PS3 PowerPC code and the host operating system. It provides:

- A **4 GB virtual address space** mapped into host memory, mirroring the PS3's 32-bit effective addressing
- **PPU execution contexts** that model the full PowerPC register file (32 GPR + 32 FPR + 32 VMX/AltiVec VR)
- **SPU execution contexts** with 128 x 128-bit registers and 256 KB local store each
- **LV2 syscall dispatch** translating PS3 kernel calls into host OS equivalents
- **DMA engine** for SPU local store ↔ main memory transfers

All runtime components are header-only with `static inline` functions, meaning zero link-time overhead — the compiler inlines everything directly into recompiled code.

**Key files:**

| File | Purpose |
|------|---------|
| `runtime/memory/vm.h` | Virtual memory manager |
| `runtime/ppu/ppu_context.h` | PPU register file and helpers |
| `runtime/ppu/ppu_memory.h` | Memory access macros |
| `runtime/ppu/ppu_ops.h` | Instruction operation helpers |
| `runtime/spu/spu_context.h` | SPU register file and local store |
| `runtime/spu/spu_dma.h` | MFC DMA transfer engine |
| `runtime/syscalls/lv2_syscall_table.h` | Syscall numbers and dispatch |
| `runtime/syscalls/sys_*.c` / `sys_*.h` | Individual syscall implementations |

---

## Virtual Memory Manager

**File:** `runtime/memory/vm.h`

### Design

The PS3 uses 32-bit effective addresses despite having a 64-bit PPU. Games never exceed the 4 GB address space. ps3recomp exploits this by reserving a contiguous 4 GB region on the host and translating addresses with simple pointer arithmetic:

```c
host_ptr = vm_base + guest_addr
```

On Windows, `VirtualAlloc` with `MEM_RESERVE` reserves the 4 GB range without consuming physical memory. On POSIX, `mmap` with `MAP_NORESERVE | MAP_ANONYMOUS` does the same. Pages are committed on demand as regions are needed.

### Memory Map

| Guest Address Range | Size | Region | Committed At |
|---|---|---|---|
| `0x00010000 – 0x0FFFFFFF` | 256 MB | User ELF text + data (main memory) | `vm_init()` |
| `0x10000000 – 0x1FFFFFFF` | 256 MB | RSX mapped memory / PRX module space | On demand via `vm_commit()` |
| `0x30000000 – 0x3001FFFF` | 8 × 256 KB | SPU local store windows (raw SPU) | `vm_spu_ls_map()` |
| `0xD0000000 – 0xDDFFFFFF` | 224 MB | PPU thread stacks (`vm_stack_alloc`) | `vm_init()` |
| `0xDE000000 – 0xDFFFFFFF` | 32 MB | Host callback stack window (`VM_CB_HOST_STACK_WINDOW`, owned by `ppu_cb_entry_sp`) | `vm_init()` |

### Key Constants

```c
#define VM_TOTAL_SIZE       0x100000000ull  // 4 GB total address space
#define VM_MAIN_MEM_BASE    0x00010000u     // Main memory starts here
#define VM_MAIN_MEM_SIZE    0x10000000u     // 256 MB main memory
#define VM_RSX_BASE         0x10000000u     // RSX region start
#define VM_RSX_SIZE         0x10000000u     // 256 MB RSX
#define VM_SPU_BASE         0x30000000u     // SPU LS windows start
#define VM_SPU_WINDOW_SIZE  0x00040000u     // 256 KB per SPU
#define VM_STACK_BASE       0xD0000000u     // Stack region start
#define VM_STACK_REGION     0x10000000u     // 256 MB for stacks
#define VM_PPU_STACK_SIZE   0x00100000u     // 1 MB default per thread
#define VM_CB_HOST_STACK_SLOTS  32u         // host callback stack slots
#define VM_CB_HOST_STACK_WINDOW (VM_CB_HOST_STACK_SLOTS * VM_PPU_STACK_SIZE)  // 32 MB, top of the stack region
#define VM_CB_HOST_STACK_BASE   (VM_STACK_BASE + VM_STACK_REGION - VM_CB_HOST_STACK_WINDOW)  // 0xDE000000
#define VM_PAGE_SIZE        0x00001000u     // 4 KB pages
```

### API

#### `vm_init()`

Reserves the 4 GB address space and commits the main memory (256 MB) and stack (256 MB) regions. Zeros main memory. Returns `CELL_OK` on success, `CELL_ENOMEM` on failure.

**Platform behavior:**
- **Windows**: `VirtualAlloc(NULL, 4GB, MEM_RESERVE, PAGE_NOACCESS)` then `VirtualAlloc(region, size, MEM_COMMIT, PAGE_READWRITE)` for each active region
- **POSIX**: `mmap(NULL, 4GB, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0)` then `mprotect(region, size, PROT_READ|PROT_WRITE)`

#### `vm_shutdown()`

Releases the entire 4 GB mapping. All guest pointers become invalid.

#### `vm_commit(addr, size)`

Commits pages within the reserved range, making them readable and writable. Used to bring RSX memory, SPU windows, or dynamic allocations online. Size is rounded up to `VM_PAGE_SIZE` (4 KB).

#### `vm_protect(addr, size, read, write, exec)`

Changes page protection for a committed region. Used for guard pages, read-only data segments, and executable code regions.

#### `vm_to_host(addr)` / `vm_to_guest(host_ptr)`

Convert between PS3 guest addresses (32-bit) and host pointers. These are the fundamental translation primitives.

#### `vm_is_valid_addr(addr)`

Checks if a guest address falls within any committed memory region (main memory, stack, SPU LS, or RSX).

### Stack Allocator

The `vm_stack_alloc` structure provides a bump allocator for PPU thread stacks within the `0xD0000000` region:

```c
vm_stack_alloc sa;
vm_stack_alloc_init(&sa);

// Allocate a 1 MB stack with guard page
uint32_t stack_base = vm_stack_allocate(&sa, VM_PPU_STACK_SIZE);
// stack_base points past the guard page
// Usable stack top = stack_base + VM_PPU_STACK_SIZE
```

Each allocation includes a **guard page** (4 KB, `PAGE_NOACCESS`) at the bottom of the stack to detect overflow. The allocator returns the address *above* the guard page.

The allocator ends at `VM_CB_HOST_STACK_BASE` (`0xDE000000`), not at the end of the stack region: the top `VM_CB_HOST_STACK_WINDOW` (32 MB, 32 slots of `VM_PPU_STACK_SIZE`) is reserved for the stacks of host threads that raise guest callbacks while running no guest code (`ppu_cb_entry_sp` in `ppu_loader.cpp` owns it). A thread-stack request that would reach the window fails (`vm_stack_allocate` returns 0, `sys_ppu_thread_create` returns `CELL_ENOMEM`) instead of overlapping a live callback slot.

### SPU Local Store Mapping

For raw SPU access (PPU reading/writing SPU local store via memory-mapped IO), each SPU's 256 KB local store is mapped into the guest address space:

```c
vm_spu_ls_map(2);  // Commit SPU 2's LS window

// SPU 2's LS is now accessible at 0x30000000 + 2 * 0x40000 = 0x30080000
uint8_t* ls = vm_spu_ls_host_ptr(2);
```

---

## PPU Execution Context

**File:** `runtime/ppu/ppu_context.h`

### Register File

The `ppu_context` struct models the complete architectural state of a Cell PPU hardware thread:

```c
typedef struct ppu_context {
    uint64_t gpr[32];       // General-purpose registers r0–r31
    double   fpr[32];       // Floating-point registers f0–f31
    u128     vr[32];        // VMX/AltiVec vector registers vr0–vr31 (128-bit, 16-byte aligned)
    uint32_t cr;            // Condition register (8 × 4-bit fields)
    uint64_t lr;            // Link register (return address from bl/blr)
    uint64_t ctr;           // Count register (loop counter for bdnz)
    uint32_t xer;           // Fixed-point exception register (SO/OV/CA)
    uint32_t fpscr;         // Floating-point status and control register
    uint32_t vscr;          // Vector status and control register
    uint64_t cia;           // Current instruction address (program counter)
    uint64_t thread_id;     // Thread identification
    uint64_t reserve_addr;  // lwarx/ldarx reservation address
    uint64_t reserve_value; // the raw big-endian bits that were loaded
    uint64_t reserve_epoch; // the 128-byte line's epoch when they were
    int      reserve_valid; // width of the load (0 = no reservation, 4 or 8)
} ppu_context;
```

### Register Roles (PPC64 ABI)

| Register | ABI Name | Purpose |
|----------|----------|---------|
| `r0` | — | Volatile, used by prologue/epilogue |
| `r1` | SP | Stack pointer (16-byte aligned, grows down) |
| `r2` | TOC | Table of Contents pointer (per-module global data) |
| `r3` | ARG0/RET | First argument / return value |
| `r4–r10` | ARG1–ARG7 | Arguments 2–8 |
| `r11` | — | Syscall number (for `sc` instruction) |
| `r12` | — | Indirect call target / scratch |
| `r13` | SDA | Small Data Area base pointer |
| `r14–r31` | — | Non-volatile (callee-saved) |
| `f0` | — | Volatile FP scratch |
| `f1–f13` | — | FP arguments / return |
| `f14–f31` | — | Non-volatile FP |
| `vr0–vr19` | — | Volatile VMX |
| `vr20–vr31` | — | Non-volatile VMX |

Convenience macros provide named access:

```c
PPU_SP(ctx)     // ctx->gpr[1]  — stack pointer
PPU_TOC(ctx)    // ctx->gpr[2]  — table of contents
PPU_ARG0(ctx)   // ctx->gpr[3]  — first argument
PPU_ARG1(ctx)   // ctx->gpr[4]  — second argument
// ...
PPU_ARG7(ctx)   // ctx->gpr[10] — eighth argument
PPU_RET(ctx)    // ctx->gpr[3]  — return value
```

### Condition Register

The 32-bit CR is divided into eight 4-bit fields (CR0–CR7). Each field contains:

| Bit | Name | Meaning |
|-----|------|---------|
| 3 (MSB) | LT | Result is negative / less than |
| 2 | GT | Result is positive / greater than |
| 1 | EQ | Result is zero / equal |
| 0 (LSB) | SO | Summary Overflow (copied from XER[SO]) |

**Helper functions:**

```c
ppu_cr_get(ctx, field)              // Read 4-bit CR field (0–7)
ppu_cr_set(ctx, field, value)       // Write 4-bit CR field
ppu_cr_set_s64(ctx, field, a, b)    // Set CR field from signed comparison
ppu_cr_set_u64(ctx, field, a, b)    // Set CR field from unsigned comparison
ppu_cr_set_s32(ctx, field, a, b)    // Set CR field from 32-bit signed comparison
ppu_cr_set_u32(ctx, field, a, b)    // Set CR field from 32-bit unsigned comparison
```

### XER (Fixed-Point Exception Register)

| Bit | Name | Meaning |
|-----|------|---------|
| 31 | SO | Summary Overflow — sticky, set when OV is set |
| 30 | OV | Overflow — set by arithmetic with "o" suffix (addo, subfo) |
| 29 | CA | Carry — set by add carrying, subtract from carrying |
| 0–6 | — | Byte count for lswx/stswx string instructions |

```c
ppu_xer_get_so(ctx)         // Read SO flag
ppu_xer_get_ov(ctx)         // Read OV flag
ppu_xer_get_ca(ctx)         // Read CA flag
ppu_xer_set_ca(ctx, ca)     // Set/clear CA
ppu_xer_set_ov(ctx, ov)     // Set/clear OV (also sets SO if ov=1)
ppu_xer_get_byte_count(ctx) // Read string instruction byte count
```

### VSCR (Vector Status and Control Register)

| Bit | Name | Meaning |
|-----|------|---------|
| 16 | NJ | Non-Java mode (flush denorms to zero) |
| 0 | SAT | Saturation (sticky, set by saturating vector ops) |

### Load-Linked / Store-Conditional

The `reserve_*` fields hold a `lwarx`/`ldarx` reservation: the address, the bits loaded and the epoch of the 128-byte line they were loaded from. `stwcx.`/`stdcx.` succeed only if no store has reached the line since and the bits are unchanged. The rule is shared with the SPU's `GETLLAR`/`PUTLLC`; see [Reservations](#reservations).

### Stack Setup

```c
ppu_set_stack(ctx, stack_base, stack_size);
```

Places the stack pointer (r1) at the top of the given region, aligned down to 16 bytes, with 48 bytes reserved for the minimum PPC64 stack frame (back-chain + LR save area).

---

## SPU Execution Context

**File:** `runtime/spu/spu_context.h`

### Architecture

Each SPU is a self-contained processor with:
- **128 general-purpose 128-bit registers** (not 32 like PPU — SPU has a much wider register file)
- **256 KB local store** — all code and data must fit here; no cache, no virtual memory
- **Channels** — communication ports for DMA, mailboxes, and signals

### Register File

```c
typedef struct spu_context {
    u128     gpr[128];          // 128 × 128-bit registers (16-byte aligned)
    uint8_t  ls[256 * 1024];    // 256 KB local store (16-byte aligned)
    uint32_t pc;                // Program counter (0x00000–0x3FFFF)
    uint32_t status;            // Running/stopped/waiting state
    uint32_t spu_id;            // SPU index (0–7)
    uint32_t spu_group_id;      // Thread group ID
    uint32_t decrementer;       // Free-running down counter
    uint32_t srr0;              // Save/Restore Register 0 (exception return)
    uint32_t event_status;      // Event flags
    uint32_t event_mask;        // Event mask
    spu_channel ch_out_mbox;    // SPU → PPU outbound mailbox
    spu_channel ch_in_mbox;     // PPU → SPU inbound mailbox
    spu_channel ch_out_intr_mbox; // SPU → PPU interrupt mailbox
    spu_channel ch_sig_notify[2]; // Signal notification 1 & 2
    uint32_t mfc_lsa;           // MFC staging: local store address
    uint32_t mfc_eah;           // MFC staging: EA high word
    uint32_t mfc_eal;           // MFC staging: EA low word
    uint32_t mfc_size;          // MFC staging: transfer size
    uint32_t mfc_tag;           // MFC staging: tag group ID
    uint32_t mfc_tag_mask;      // MFC tag mask for wait/poll
    uint32_t mfc_tag_status;    // MFC tag completion status
} spu_context;
```

### SPU Status Flags

```c
#define SPU_STATUS_STOPPED          0x0   // Stopped
#define SPU_STATUS_RUNNING          0x1   // Currently executing
#define SPU_STATUS_STOPPED_BY_STOP  0x2   // Hit a `stop` instruction
#define SPU_STATUS_STOPPED_BY_HALT  0x4   // Hit a `halt` instruction
#define SPU_STATUS_WAITING_CHANNEL  0x8   // Blocked on channel read
#define SPU_STATUS_SINGLE_STEP      0x10  // Single-step mode
```

### Local Store Access

All local store access uses a mask (`SPU_LS_MASK = 0x3FFFF`) to wrap addresses within the 256 KB range:

```c
spu_ls_ptr(ctx, lsa)            // Get host pointer to LS address
spu_ls_read32(ctx, lsa)         // Read 32-bit big-endian value
spu_ls_write32(ctx, lsa, val)   // Write 32-bit big-endian value
spu_ls_read128(ctx, lsa)        // Read 128-bit value (16-byte aligned)
spu_ls_write128(ctx, lsa, val)  // Write 128-bit value (16-byte aligned)
```

### Preferred Slot

SPU instructions operate on the "preferred slot" of a 128-bit register — the leftmost element in big-endian layout. For 32-bit word operations, this is element 0 of the `_u32` array:

```c
spu_preferred_u32(&reg)         // Extract preferred word (uint32_t)
spu_preferred_s32(&reg)         // Extract preferred word (int32_t)
spu_preferred_f32(&reg)         // Extract preferred word (float)
spu_preferred_u64(&reg)         // Extract preferred doubleword
spu_make_preferred_u32(val)     // Create register with value in preferred slot
```

### Channel System

Channels are the SPU's communication mechanism. Each channel has a value and a count (0 = empty, 1 = has data):

```c
spu_channel_write(&ch, val)     // Write value, set count to 1
spu_channel_read(&ch)           // Read value, set count to 0
spu_channel_has_data(&ch)       // Check if channel has data
```

**Channel IDs:**

| ID | Name | Direction | Purpose |
|---|---|---|---|
| 0 | `SPU_RdEventStat` | Read | Event status |
| 1 | `SPU_WrEventMask` | Write | Event mask |
| 3 | `SPU_RdSigNotify1` | Read | Signal notification register 1 |
| 4 | `SPU_RdSigNotify2` | Read | Signal notification register 2 |
| 7 | `SPU_WrDec` | Write | Write decrementer |
| 8 | `SPU_RdDec` | Read | Read decrementer |
| 28 | `SPU_WrOutMbox` | Write | Write outbound mailbox (SPU → PPU) |
| 29 | `SPU_RdInMbox` | Read | Read inbound mailbox (PPU → SPU) |
| 30 | `SPU_WrOutIntrMbox` | Write | Write outbound interrupt mailbox |

### Execution Observer

**File:** `runtime/spu/spu_observe.h`

A per-context hook set for tools that drive an SPU context themselves (research
harnesses, oracles, differential tests). It is not a diagnostic switch: nothing
is installed on a normal run, there are no environment knobs or globals, and
every hook site costs one predictable null check when the pointer is clear.

```c
typedef struct spu_observer {
    void* user;
    int  (*on_insn)(void* user, spu_context* ctx, uint32_t pc);
    void (*on_mfc)(void* user, spu_context* ctx, const spu_obs_mfc* cmd);
    void (*on_atomic)(void* user, spu_context* ctx, uint32_t cmd,
                      uint64_t ea, uint32_t atomic_status);
    void (*on_channel)(void* user, spu_context* ctx, int is_write,
                       uint32_t ch, uint32_t value);
} spu_observer;

void spu_observer_set(spu_context* ctx, const spu_observer* obs);  /* NULL = off */
```

The callbacks report, in order: each instruction the **interpreter** is about
to execute (`on_insn`; lifted code is not observed at instruction level), each
MFC command at enqueue including atomics (`on_mfc`), the status an atomic
command leaves in `MFC_RdAtomicStat` (`on_atomic`: `4` GETLLAR complete, `0`
PUTLLC success, `1` PUTLLC failure, `2` PUTLLUC complete), and every `rdch` /
`wrch` once its value is known (`on_channel`). Any callback left NULL is
skipped.

`on_insn` may **yield**: returning nonzero makes the outermost
`spu_interp_run()` / `spu_interp_run_until()` return `SPU_INTERP_YIELDED`
(`0xFFFFFFFF`, never a real stop code) with `ctx->pc` at the instruction that
was not executed and no other state consumed. `spu_interp_run(ctx, ctx->pc)`
resumes exactly there, which is how a harness round-robins several contexts on
one host thread. A yield requested from a *nested* interpreter run -- one
entered by lifted code through `spu_dispatch()` or the SMC microstep -- cannot
unwind that host stack without losing guest state, so it is reported on stderr
and aborts instead of pretending to stop. Observe contexts from the outermost
entry.

```c
static int on_insn(void* user, spu_context* ctx, uint32_t pc) {
    (void)ctx;
    return ++*(int*)user > 1000;   /* run 1000 instructions per slice */
}
static spu_observer obs = { .user = &counter, .on_insn = on_insn };
spu_observer_set(&ctx, &obs);
uint32_t stop = spu_interp_run(&ctx, ctx->pc);
```

Tests: `runtime/spu/tests/test_spu_observe.c` (`ctest -R spu_observe`).

### SPURS job start

**File:** `runtime/spu/spurs_job.c` (`spu_run_spurs_job`)

A SPURS job is a raw image at LS 0 that enters through Sony's `JOBCRT` startup
code with `r3` = `CellSpursJobContext2*` and `r4` = the job descriptor, both in
local store. The loader lays the job out the way `_cellSpursCheckJob` sizes it
(binary + bss, ioBuffer, oBuffer, scratch + stack, cache buffers, each
`align1024`) and parks the context at the top of local store:

| LS | What |
|---|---|
| `0x3FC00..0x3FFFF` | job descriptor copy (`r4`); `0x3FF00` is also the link the job returns to |
| `0x3FBC0` | `JobContext2` (`r3`); `+0x14` = `sizeJobDescriptor:4` in the top nibble, the jm interface pointer in the low 18 bits |
| `0x3FBF0` | the jm interface object (two LS pointers; all zero for Ver13) |
| `0x3FAC0..0x3FBBF` | job runtime area (Ver14 only; zero for Ver13) |

**CRT gate.** The 12 bytes at LS `0x30` must read `JOBCRT Ver13` or `JOBCRT Ver14`.
The policy module accepts any `JOBCRT Ver` suffix, but only these two CRTs'
interface and kernel view are modelled, so any other marker (Ver12, Ver15, no
marker) is the named stop `spu-job-crt-version` rather than a run on a context
the CRT was not built for.

**Memory.** For an ELF job the high word of `eaBinary` is the bss in quadwords
(the CRT zeroes it itself); the loader keeps the full 64 bits, so the job's
memory is `sizeBinary + 16 * hi32`, every buffer and the stack (`r1`) start
above it, and the bss reads as zero. Every region the job owns -- image + bss,
ioBuffer, oBuffer, the scratch + stack block (its top is where `r1` starts) and
each cache buffer -- must end at or below `0x3FAC0`; sizes are summed in 64 bits
so none wraps. Anything else would lie on the runtime area or the context (a
stack top past local store wraps onto them) and is the named stop
`spu-job-memory`, whose fields name each size, the failing `region`, its end
`p` and the `limit` (`docs/STOPS.md`). The firmware knowledge base holds no
total-size limit from `_cellSpursCheckJob`, so `0x3FAC0` is this loader's own
layout bound, not a firmware figure.

**Input DMA list.** The descriptor's `sizeDmaList` bytes at `+0x30` are 8-byte
`{size word, EAL}` elements; jm2 stages them into the ioBuffer in 16-byte slots
before the job enters. The loader validates the whole list before writing any
of it: every size is 1/2/4/8 or a 16-byte multiple up to 16 KB (the MFC rules
`spu_dma.h` applies per transfer), every EAL is naturally aligned and inside
guest memory, the input and cache lists themselves stay inside guest memory
(summed in 64 bits), and the staged bytes fit `sizeInOrInOut`. A zero-size
element, a null EA, or an `eaBinary`/`sizeBinary` of zero is a stop too: an
element is never skipped and no partial ioBuffer is handed to the job. The
named stop is `spu-job-dma-list`, whose fields name the element (`docs/STOPS.md`).

**Kernel view.** Both CRTs read the SPURS kernel context with absolute loads;
with the job at LS 0 those addresses are the job's own bytes, so the loader
overlays what the kernel would show: `0x170..0x17F` zero (byte `0x175` = tracing
off), `0x1C0..0x1CF` zero with the SPU number at `0x1C8` and the DMA tag (20) at
`0x1CC`, `0x210..0x21F` zero.

**Ver14 jm interface.** The CRT's preflight reads `ctx+0x14 & 0x0FFFFFFF` and,
when it is zero, returns `0x80410A11` and stops before `main`. The job manager
supplies two entries there; the loader does the same from the job runtime area:

- word 0 (the job manager's scheduler switch) is never called by the Odin Sphere
  images. It points at a `stop 0x3B0E` trap: a call ends the run in the named
  stop `spu-job-jm-entry`.
- word 1 (the coroutine context swap) is called with the tag status in `r3`
  when a DMA tag the job waits on is not complete. There is no other context to
  swap to, so it is an eight-instruction SPU routine that polls
  `MFC_RdTagStat` (mask -1, immediate update) until every tag group is complete
  and returns with `bi $r0`. It preserves every register but `r3`, which holds
  the status (`0xFFFFFFFF`); the real swap preserves `r0-r2` and `r80-r127`.
  The tag mask channel is left at -1.

The runtime area is machine code, not part of any lifted image. A job running
in the interpreter executes it directly; lifted code reaches it through
`spu_indirect_branch`, which interprets the area (`spu_context.job_rt_ls`) until
the routine returns to its caller. A tag that never completes is a wait that
never ends, as on hardware.

Tests: `runtime/spu/tests/test_spurs_job_crt_main.c`, driven by
`tools/test_spurs_job_crt.py` (`ctest -R spurs_job_crt`): synthetic Ver13 and
Ver14 jobs with byte-checked local store, the Ver13 local store pinned by hash,
a CRT-like program that reaches `main`, the word-1 wait on interpreted and
lifted-style jobs, a DMA list that exactly fills the ioBuffer, and each stop,
including every input-DMA-list violation and a list that leaves guest memory.

---

## Type System

**File:** `include/ps3emu/ps3types.h`

### Fixed-Width Integers

```c
// Unsigned
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

// Signed
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

// Boolean
typedef uint8_t   b8;       // PS3 boolean (1 byte)
```

### 128-bit Vector Type

The `u128` union supports accessing a 128-bit value at any lane width:

```c
typedef union u128_t {
    uint64_t _u64[2];   // 2 × 64-bit unsigned
    uint32_t _u32[4];   // 4 × 32-bit unsigned
    uint16_t _u16[8];   // 8 × 16-bit unsigned
    uint8_t  _u8[16];   // 16 × 8-bit unsigned
    int64_t  _s64[2];   // 2 × 64-bit signed
    int32_t  _s32[4];   // 4 × 32-bit signed
    int16_t  _s16[8];   // 8 × 16-bit signed
    int8_t   _s8[16];   // 16 × 8-bit signed
    float    _f32[4];   // 4 × 32-bit float
    double   _f64[2];   // 2 × 64-bit double
} u128;
```

This is used for both PPU VMX/AltiVec registers and SPU general-purpose registers.

### PS3 Address Type

```c
typedef uint32_t ps3_addr_t;    // Always 32-bit, even on 64-bit host
```

### System Handle Types

```c
typedef s32  CellError;                 // Return code (0 = OK, negative = error)
typedef u64  sys_ppu_thread_t;          // PPU thread ID
typedef u32  sys_spu_thread_t;          // SPU thread ID
typedef u32  sys_spu_thread_group_t;    // SPU thread group ID
typedef u32  sys_mutex_t;               // Mutex handle
typedef u32  sys_cond_t;                // Condition variable handle
typedef u32  sys_rwlock_t;              // Read-write lock handle
typedef u32  sys_event_queue_t;         // Event queue handle
typedef u32  sys_semaphore_t;           // Semaphore handle
typedef u32  sys_lwmutex_t;             // Lightweight mutex handle
typedef u32  sys_event_flag_t;          // Event flag handle
typedef u32  sys_memory_container_t;    // Memory container handle
```

### Big-Endian Wrapper (`be_t<T>`)

**(C++ only)**

`be_t<T>` stores a value in PS3 big-endian byte order and automatically converts to/from host order on read/write. It works with any arithmetic or enum type.

```cpp
be_t<u32> x = 0x12345678;  // Stores bytes as 12 34 56 78 in memory
u32 y = x;                  // Reads back as 0x12345678 (byte-swapped on LE host)
x += 1;                     // Increments correctly (reads, adds, writes back)
```

**Key design principles:**
- Storage is always big-endian (matching PS3 memory layout)
- Conversion happens at the read/write boundary, not during computation
- `raw()` access returns the un-converted bytes (for serialization/DMA)
- Supports full arithmetic: `+=`, `-=`, `*=`, `/=`, `&=`, `|=`, `^=`, `++`, `--`

**Common aliases:**

```cpp
be_u16, be_u32, be_u64      // Unsigned big-endian
be_s16, be_s32, be_s64      // Signed big-endian
be_f32, be_f64              // Floating-point big-endian
```

### Virtual Memory Pointers (`vm::ptr<T>`)

**(C++ only)**

`vm::ptr<T>` is a typed pointer into the PS3 guest address space. It holds a 32-bit guest address and translates through `vm::g_base` on dereference:

```cpp
vm::ptr<u32> p(0x10000);    // Points to guest address 0x10000
*p = 42;                     // Writes to vm::g_base + 0x10000
u32 val = p[3];             // Reads from vm::g_base + 0x1000C
p += 1;                      // Advances by sizeof(u32) = 4 bytes in guest space
```

**Methods:**
- `addr()` — returns the raw 32-bit guest address
- `get_ptr()` — returns the host pointer (`vm::g_base + addr`)
- `operator*` / `operator->` — dereference through host translation
- `operator[]` — array-style indexing
- Pointer arithmetic advances by `sizeof(T)` in guest space
- Explicit `operator bool` — true if non-null
- Comparison operators: `==`, `!=`, `<`

**Specialization for `void`:** `vm::ptr<void>` holds an untyped address and provides `get_ptr()` returning `void*`.

**Alias:** `vm::bptr<T>` = `vm::ptr<be_t<T>>` — a pointer to a big-endian value (the most common case when mapping PS3 structures).

---

## Endianness Layer

**File:** `include/ps3emu/endian.h`

### Platform Intrinsics

The byte-swap primitives use compiler builtins for maximum performance:

| Platform | 16-bit | 32-bit | 64-bit |
|----------|--------|--------|--------|
| **MSVC** | `_byteswap_ushort` | `_byteswap_ulong` | `_byteswap_uint64` |
| **GCC/Clang** | `__builtin_bswap16` | `__builtin_bswap32` | `__builtin_bswap64` |
| **Portable** | Shift/mask fallback | Shift/mask fallback | Shift/mask fallback |

These compile to single instructions on x86 (`bswap`), ARM (`rev`), etc.

### 128-bit Swap

For VMX/AltiVec 128-bit values, a swap reverses both 64-bit halves and exchanges them:

```c
ps3_u128 ps3_bswap128(ps3_u128 v);
// Equivalent to: { bswap64(v.lo), bswap64(v.hi) }
```

### C++ Template API

```cpp
namespace ps3::endian {
    template <typename T> T byte_swap(T v);     // Generic byte swap
    template <typename T> T be_to_host(T v);    // Big-endian → host order
    template <typename T> T host_to_be(T v);    // Host order → big-endian
}
```

`be_to_host` and `host_to_be` are the same operation (swap is its own inverse) but the two names make intent clear:
- Use `be_to_host` when **reading** from PS3 memory
- Use `host_to_be` when **writing** to PS3 memory

Specializations exist for: `uint8_t`/`int8_t` (no-op), `uint16_t`/`int16_t`, `uint32_t`/`int32_t`, `uint64_t`/`int64_t`, `float`, `double`.

Float/double swap uses `memcpy` to avoid strict-aliasing violations:

```cpp
template <> float byte_swap<float>(float v) {
    uint32_t tmp;
    memcpy(&tmp, &v, sizeof(tmp));
    tmp = ps3_bswap32(tmp);
    float result;
    memcpy(&result, &tmp, sizeof(result));
    return result;
}
```

---

## Syscall Dispatch

**File:** `runtime/syscalls/lv2_syscall_table.h`

### How PS3 Syscalls Work

When PS3 code executes the `sc` (System Call) instruction:
1. The syscall number is in `r11`
2. Up to 8 arguments are in `r3–r10`
3. The kernel handler executes and places the return value in `r3`

In recompiled code, the `sc` instruction becomes a call to `lv2_syscall(ctx)`, which dispatches through a function pointer table.

### Dispatch Table

```c
typedef int64_t (*lv2_syscall_fn)(ppu_context* ctx);

typedef struct lv2_syscall_table {
    lv2_syscall_fn handlers[1024];  // Up to 1024 syscall slots
} lv2_syscall_table;

extern lv2_syscall_table g_lv2_syscalls;  // Global instance
```

### Argument Access

Syscall handlers extract arguments from the PPU context:

```c
#define LV2_ARG_U64(ctx, n)  ((ctx)->gpr[3 + (n)])     // 64-bit unsigned
#define LV2_ARG_U32(ctx, n)  ((uint32_t)(ctx)->gpr[3 + (n)])  // 32-bit unsigned
#define LV2_ARG_S32(ctx, n)  ((int32_t)(ctx)->gpr[3 + (n)])   // 32-bit signed
#define LV2_ARG_PTR(ctx, n)  ((uint32_t)(ctx)->gpr[3 + (n)])  // Guest address
```

### Syscall Number Ranges

| Range | Category | Examples |
|---|---|---|
| 1–14 | Process management | `getpid`, `exit`, `wait` |
| 41–56 | PPU thread management | `create`, `exit`, `join`, `detach`, `yield` |
| 70–76 | Timers | `create`, `start`, `stop`, `usleep` |
| 90–94, 114 | Semaphores | `create`, `destroy`, `wait`, `post` |
| 100–110 | Mutexes and condvars | `create`, `lock`, `unlock`, `wait`, `signal` |
| 120–127 | Read-write locks | `rlock`, `wlock`, `runlock`, `wunlock` |
| 128–146 | Event queues/ports/flags | `create`, `receive`, `send`, `wait`, `set` |
| 150–160 | Lightweight mutexes/condvars | `lwmutex`, `lwcond` |
| 170–194, 229–251 | SPU management | `group_create`, `start`, `join`, `write_ls` |
| 330–358 | Memory management | `allocate`, `free`, `container`, `mmapper` |
| 400–402 | TTY I/O | `read`, `write` |
| 480–494 | PRX module management | `load`, `start`, `stop`, `unload` |
| 801–841 | Filesystem | `open`, `read`, `write`, `close`, `stat`, `mkdir` |

### Registration

```c
// Register a single syscall
LV2_REGISTER_SYSCALL(SYS_PPU_THREAD_CREATE, sys_ppu_thread_create);

// Or use the bulk registration function
lv2_register_all_syscalls(&g_lv2_syscalls);
```

Unimplemented syscalls return `CELL_ENOSYS` (0x80010003).

---

## DMA Engine

**File:** `runtime/spu/spu_dma.h`

### Overview

The Memory Flow Controller (MFC) handles all data transfer between an SPU's local store and main memory. In real hardware, DMA is asynchronous — the SPU issues a command and continues executing while the MFC handles the transfer in the background. In ps3recomp, **DMA is executed synchronously** because all transfers complete before the next SPU instruction in recompiled code.

### MFC Command Opcodes

| Opcode | Name | Direction | Notes |
|--------|------|-----------|-------|
| `0x20` | `MFC_PUT_CMD` | LS → Main memory | Basic put |
| `0x21` | `MFC_PUTB_CMD` | LS → Main memory | Put with barrier |
| `0x22` | `MFC_PUTF_CMD` | LS → Main memory | Put with fence |
| `0x40` | `MFC_GET_CMD` | Main memory → LS | Basic get |
| `0x41` | `MFC_GETB_CMD` | Main memory → LS | Get with barrier |
| `0x42` | `MFC_GETF_CMD` | Main memory → LS | Get with fence |
| `0x24` | `MFC_PUTL_CMD` | LS → Main memory | Put list (scatter) |
| `0x44` | `MFC_GETL_CMD` | Main memory → LS | Get list (gather) |
| `0xA0` | `MFC_SNDSIG_CMD` | — | Send signal |
| `0xC0` | `MFC_BARRIER_CMD` | — | Ordering barrier |
| `0xC8` | `MFC_EIEIO_CMD` | — | Enforce in-order execution |
| `0xCC` | `MFC_SYNC_CMD` | — | Full synchronization |

**Opcode bit encoding:**
- Bit 6 (`0x40`): GET family
- Bit 5 (`0x20`): PUT family
- Bit 2 (`0x04`): List variant
- Bit 1 (`0x02`): Fence variant
- Bit 0 (`0x01`): Barrier variant

### Transfer Flow

1. SPU writes to MFC staging channels (via `wrch` instruction):
   - `MFC_LSA` — local store address
   - `MFC_EAH` — effective address high 32 bits
   - `MFC_EAL` — effective address low 32 bits
   - `MFC_Size` — transfer size (max 16 KB)
   - `MFC_TagID` — tag group (0–31) for synchronization
2. SPU writes command opcode to `MFC_Cmd` — this triggers the transfer
3. For synchronization, SPU writes tag mask to `MFC_WrTagMask`, then writes update type to `MFC_WrTagUpdate`, then reads `MFC_RdTagStat`

### DMA List Commands

List commands (PUTL/GETL) perform scatter/gather transfers. The list resides in local store and consists of 8-byte elements:

```c
typedef struct mfc_list_element {
    uint32_t size_and_flags;  // bits 0-14: size, bit 15: stall-and-notify
    uint32_t eal;             // effective address low 32 bits
} mfc_list_element;
```

### Tag Synchronization

Each DMA command is assigned to one of 32 tag groups. The SPU can wait for specific groups to complete:

| Update Type | Value | Behavior |
|---|---|---|
| `MFC_TAG_UPDATE_IMMEDIATE` | 0 | Return current status immediately |
| `MFC_TAG_UPDATE_ANY` | 1 | Wait until any tagged transfer completes |
| `MFC_TAG_UPDATE_ALL` | 2 | Wait until all tagged transfers complete |

In synchronous mode, all transfers complete immediately, so tag waits always succeed.

### Memory Ordering

An SPU transfer is a load or store of guest memory by another processor, and arm64 reorders independent loads and stores. `mfc_do_transfer` therefore brackets the copy: an **acquire fence before a GET** reads guest memory, and a **release fence after a PUT** wrote it. They pair with the barriers the lifter emits on the PPU side (`lwsync` lowers to an acq_rel fence, `sync` to seq_cst, `eieio` to a release fence; `PPU_FENCE` in `tools/ppu_lifter.py`): a PPU that stores data, executes `lwsync`, then stores a flag is read correctly by an SPU that GETs the flag and then the data, and an SPU that PUTs data and then a flag is read correctly by a PPU that loads the flag, executes `lwsync`, then loads the data. A PUT that copies under the lock-line lock is additionally released by `spu_lockline_unlock`.

`tools/test_spu_dma_litmus.py` (`runtime/spu/tests/test_dma_litmus_main.c`) runs both directions through the real `mfc_submit`: short in tier `t0` (`spu_dma_litmus`), 10^8 publications per direction in the `stress` tier (`spu_dma_litmus_stress`). With either fence removed, the matching direction fails within a few seconds on arm64.

### Reservations

**Files:** `runtime/memory/reservation.{h,c}` (the model), `runtime/spu/spu_coherency.{c,h}` (SPU side: the lock-line lock, the table of reserving SPU contexts, `SPU_EVENT_LR`), `runtime/spu/spu_channels.c` (`GETLLAR`/`PUTLLC`/`PUTLLUC`), `runtime/ppu/ppu_loader.cpp` (`ppu_lwarx32`/`ppu_stwcx32`/`ppu_ldarx64`/`ppu_stdcx64`, the entry points the lifter calls).

One rule covers PPU `lwarx`/`ldarx` + `stwcx.`/`stdcx.` and SPU `GETLLAR` + `PUTLLC`/`PUTLLUC`/`PUTQLLUC`. Every 128-byte line of guest memory has an **epoch**, a counter that advances each time a store lands on the line through a path that reports it. A reservation is the pair (epoch, value) read together; the conditional store succeeds only if the epoch is unchanged **and** the reserved bytes are unchanged, and the check and the store are one critical section on the line. The epoch is what an equal-value store cannot fake: an SPU `PUTLLC` that rewrites the bytes it read, or a `stwcx.` that stores the value it loaded, still advances it, so the other side's reservation is dead and its `PUTLLC` reports failure in `MFC_RdAtomicStat` (and raises `SPU_EVENT_LR`) or its `stwcx.` sets CR0 EQ clear.

- **Granule.** The 128-byte line, for both sides. That is an implementation granularity, not a claim about the architectural PPU reservation granule (OQ-E in the adoption matrix is open). A conditional store anywhere in a line kills every reservation on it, including another PPU thread's on a different word (PPU-to-PPU used to be per word); a different line is never affected, there is no hashing.
- **What advances the epoch.** A successful `stwcx.`/`stdcx.`, `PUTLLC`, `PUTLLUC`/`PUTQLLUC`, and everything that calls `spu_coh_notify_write[_except]`: SPU DMA `PUT` and `SDCRZ` into a line some SPU has reserved, PPU plain stores into such a line (`VM_WRITE_COH`), and the host helpers that edit a taskset line under the lock-line lock. An SPU's own DMA `PUT` moves its own reservation onto the new epoch (hardware does not cost an SPE its reservation for its own MFC write); its value compare still sees the data it changed.
- **Lock order.** `spu_lockline_lock` (global) before the line's epoch lock, never the reverse. A `stwcx.` on a line no SPU has reserved takes only the line lock; on a reserved line it takes both, in that order, and the reserving SPUs are told afterwards.
- **Known limits** (stated in `reservation.h`; the fix is the AM-10 fast-path design, M2+): a plain PPU store into a line **no SPU has ever reserved** is caught only by the value compare, so a same-value plain store does not kill a PPU or SPU reservation; an SPU DMA `PUT` into a line only a PPU has reserved is the same; and the very first `GETLLAR` of a line can race a `stwcx.` that has not yet seen the line marked, so that one store does not raise `SPU_EVENT_LR` (the `PUTLLC` still fails). `tests/reservation` asserts the first two as limits, so closing them fails the test and forces this paragraph to change.

`tests/reservation/test_reservation.c` (CTest `reservation_*`) runs both directions through the real SPU channel path and the real PPU atomics: `contract` (deterministic), `jq2` (an SPU `PUTLLC` counting at `+0x00` while PPU `lwarx`/`stwcx.` and `ldarx`/`stdcx.` loops count at `+0x10`/`+0x18` of the same line), `aba_ppu` and `aba_spu` (same-value stores, handshaked on atomics, no sleeps). Short runs are tiers `t0` and `tsan`; `*_stress` is 10^8 iterations in tier `stress`.

---

## lwmutex and lwcond

**Files:** `runtime/syscalls/sys_lwsync.cpp`, `sys_lwsync.h`; registered by `ppu_sysprx_register`.

`sys_lwmutex_*` and `sys_lwcond_*` are sysPrxForUser imports, reached through `ps3_hle_call`, which dispatches the context-handler registry (`ps3_hle_register_ctx`) before the generated NID table. There used to be two implementations of the same eleven NIDs:

| | context handlers in `ppu_sysprx.cpp` | host-pointer C functions in `libs/system/sysPrxForUser.c` |
|---|---|---|
| Live for a title | **yes**: registered by `ppu_sysprx_register`, found first | no: shadowed by the registry; only reachable through the generated table when a registered NID has no context handler (never, for these eleven) |
| Thread identity | `ppu_context::thread_id` | none: a host mutex, so owner = the host thread |
| Guest layout | writes `sys_lwmutex_t` and `sys_lwcond_t` words (`vm_read32`/`vm_write32`) | a host-pointer view, `sleep_queue` an index into a 256-slot pool |
| `lwcond` | signal, signal_all, signal_to and destroy were no-ops; wait released the mutex, slept 1 ms and re-took it | `pthread_cond_t`; `CELL_OK` for an unsignalled wake |
| `lock` timeout | honoured (`ETIMEDOUT` after the wait) | ignored |
| Tests | `tools/test_lwcond_layout.py` (extracted source text, stubbed semaphores) | `libs/system/tests/test_static_lwmutex.c` (host pointers) |

Decision: the context-handler form is the backend, because it is the one a title runs, it is the only one with a guest thread id (lwmutex ownership is by guest thread, and a host-pointer call has no ctx), and its guest layout (`sys_lwcond_t` = `+0x00` lwmutex EA, `+0x04` queue id; `sys_lwmutex_t` owner/recursive-count words) is the one the guest also reads. The semantics it kept: lock timeout returns `CELL_ETIMEDOUT` rather than being ignored, the recursive attribute, the 8-byte `sys_lwcond_t` layout, `EBUSY` for a contended trylock and a held destroy. The C implementation and its header declarations (`sys_lwmutex_*`, `sys_lwcond_*`, `sys_lwmutex_reset_all`, `*_t_hle`) are gone, together with their two tests; the contract they covered is in `tests/lwsync`.

The backend is rewritten in C++20 in `sys_lwsync.cpp` as host objects behind the ids the guest stores in `sleep_queue` / `lwcond_queue` (slot + generation, so a stale id is `CELL_ESRCH`), with the guest words mirrored for any guest code that reads them. Each object has a host mutex and a FIFO queue of waiter nodes; a blocked thread sleeps on its own node, so a wake goes to exactly the thread dequeued, and only a post ends the sleep, so a host-level spurious wakeup is never reported as a signal. Unlock hands the lock to the head waiter. A waiter is queued before the lock that queued it is released, so a signal issued after a waiter is enqueued cannot be lost. Acquire/release comes from the host mutexes; the guest-word mirror is relaxed.

Return codes, with the firmware behaviour they follow (RPCS3's `sys_lwmutex` / `sys_lwcond` / `sysPrxForUser` as semantic oracle, read and not copied):

| Call | Result |
|---|---|
| `lwmutex_create` | `EINVAL` for an attribute other than recursive/not-recursive or a protocol other than FIFO/priority/retry; `EFAULT` for a null or unaligned address |
| `lwmutex_lock` / `trylock` | free: owner = caller. Owner re-locks: counted if recursive, `EDEADLK` otherwise. Held by another: `lock` waits (`ETIMEDOUT` after `timeout` µs, 0 = forever), `trylock` returns `EBUSY`. Destroyed: `EINVAL`; never created: `ESRCH` |
| `lwmutex_unlock` | `EPERM` unless the caller owns it; a counted re-lock is released first |
| `lwmutex_destroy` | `EBUSY` while owned, queued on, or waited on through an lwcond; `EINVAL` if destroyed already |
| `lwcond_wait` | `EPERM` unless the caller owns the lwcond's lwmutex; releases it and queues as one step; on return, whatever ended the wait, the caller owns it again at its saved recursion count; timeout is `ETIMEDOUT`, never `OK` |
| `lwcond_signal` / `signal_all` | wake one / all waiters in queue order; no waiter is `OK`; a signaller that owns the lwmutex moves the waiters onto its queue instead (as the firmware's mode 1 does) |
| `lwcond_signal_to` | wakes the named thread id; `EPERM` if it is not waiting |
| `lwcond_destroy` | `EBUSY` with waiters |

Deliberate differences: queues are FIFO whatever the protocol attribute; object ids are not lv2 ids.

Tests: `tests/lwsync` (`test_lwsync`) drives `ps3_hle_call` with the real NIDs. `lwsync_contract` covers the table above (recursion, `EBUSY`, `EPERM`, destroy with a waiter, timeouts with the mutex re-owned, signal one of two waiters, signal_all, signal_to, FIFO order). `lwsync_mp` (message passing through an lwmutex-protected slot) and `lwsync_lostwake` (a signal racing a waiter entering the wait) are litmus tests: short in `t0` and `tsan`, 10^8 iterations in `lwsync_*_stress` (label `stress`), all with 64-bit monotonic time and a 30 s no-progress watchdog.

### Host-initiated callback thread ids

A callback raised by a host thread with no guest context (`ppu_guest_call` / `ppu_guest_call_ct` with `g_active_ctx == nullptr`: the vblank, audio and timer threads) runs as a guest thread of its own. Its id is allocated once per host thread from a counter in `0x7F000000..0x7FFEFFFF` (16,711,680 ids), kept for the life of the thread and never reused; the first id the counter refuses is `0x7FFF0000`, which is cellFiber's `FIBER_FALLBACK_THREAD_ID` (`libs/spurs/cellFiber.c`), so a host-callback thread and a parentless fiber cannot share an owner id. Each file pins its copy of the boundary with a compile-time assert (`PPU_HOST_TID_END` is pinned to `0x7FFF0000` in `runtime/ppu/ppu_loader.cpp`, and `cellFiber.c` pins `FIBER_FALLBACK_THREAD_ID` to the same value). Real guest thread ids are the thread-table index + 1 (at most `PPU_THREAD_MAX`, 64) and `sys_ppu_thread_get_id`'s "no id" sentinel is `0x7FFFFFFE`, so those ranges cannot meet either. Two such threads are therefore two owners to an lwmutex (`tests/test_loader_gaps.cpp hostid`); a callback raised under a guest context still inherits that thread's id.

---

## Error Codes

**File:** `include/ps3emu/error_codes.h`

### Error Code Format

PS3 error codes are 32-bit values. `CELL_OK` (0) indicates success. All error codes have the high bit set (0x80000000).

**Format:** `0x8XXYYZZ` where:
- `XX` identifies the subsystem
- `YYZZ` is the specific error within that subsystem

### Generic System Errors (0x8001xxxx)

| Code | Name | Meaning |
|------|------|---------|
| `0x00000000` | `CELL_OK` | Success |
| `0x80010001` | `CELL_EAGAIN` | Resource temporarily unavailable |
| `0x80010002` | `CELL_EINVAL` | Invalid argument |
| `0x80010003` | `CELL_ENOSYS` | Function not implemented |
| `0x80010004` | `CELL_ENOMEM` | Not enough memory |
| `0x80010005` | `CELL_ESRCH` | No such entity |
| `0x80010006` | `CELL_ENOENT` | No such file or directory |
| `0x80010008` | `CELL_EDEADLK` | Deadlock avoided |
| `0x80010009` | `CELL_EPERM` | Not permitted |
| `0x8001000A` | `CELL_EBUSY` | Device or resource busy |
| `0x8001000B` | `CELL_ETIMEDOUT` | Connection timed out |
| `0x8001000C` | `CELL_EABORT` | Operation aborted |
| `0x80010013` | `CELL_ECANCELED` | Operation canceled |
| `0x80010014` | `CELL_EEXIST` | Already exists |

### Module Error Bases

Each HLE module defines errors as `(base | local_code)`:

| Base | Module |
|------|--------|
| `0x80012000` | cellSysmodule |
| `0x80010700` | cellFs / sys_fs |
| `0x80310700` | cellAudio |
| `0x8002B100` | cellVideoOut |
| `0x80210700` | cellGcmSys |
| `0x80130100` | cellNet |
| `0x80121100` | cellPad |
| `0x8002B000` | cellSysutil |
| `0x80540000` | cellFont |
| `0x80410700` | cellSpurs |
| `0x80710100` | cellHttp |
| `0x80720100` | cellSsl |

### Helper Macros

```c
CELL_IS_ERROR(code)             // True if high bit is set
CELL_RETURN_IF_ERROR(expr)      // Early return if expression returns an error
CELL_MODULE_ERROR(base, code)   // Build module-specific error code
cell_error_name(code)           // Pretty-print error code for logging (C++ only)
```

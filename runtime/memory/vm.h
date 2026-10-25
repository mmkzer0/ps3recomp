/*
 * ps3recomp - Virtual memory manager
 *
 * Manages the host-side mapping of the PS3's 32-bit address space.
 * On Windows uses VirtualAlloc; on POSIX uses mmap.
 *
 * PS3 memory map (simplified):
 *   0x00010000 - 0x0FFFFFFF : User-space (main memory, 256 MB)
 *   0x10000000 - 0x1FFFFFFF : RSX mapped memory
 *   0x30000000 - 0x3FFFFFFF : SPU local store windows (raw SPU mapping)
 *   0xD0000000 - 0xDDFFFFFF : Stack area (PPU thread stacks, vm_stack_alloc)
 *   0xDE000000 - 0xDFFFFFFF : Host callback stack window (VM_CB_HOST_STACK_WINDOW)
 */

#ifndef VM_H
#define VM_H

#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"
#include "../../include/ps3emu/stop.h"   /* PS3_NODISCARD, ps3_stop_commit_failed */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <signal.h>
  #include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Address space constants
 * -----------------------------------------------------------------------*/
#define VM_TOTAL_SIZE       0x100000000ull   /* 4 GB virtual address space */
#define VM_MAIN_MEM_BASE    0x00010000u
#define VM_MAIN_MEM_SIZE    0x10000000u      /* 256 MB main memory */
#define VM_RSX_BASE         0x10000000u
#define VM_RSX_SIZE         0x10000000u      /* 256 MB RSX */
#define VM_SPU_BASE         0x30000000u
#define VM_SPU_WINDOW_SIZE  0x00040000u      /* 256 KB per SPU LS */
#define VM_STACK_BASE       0xD0000000u
#define VM_STACK_REGION     0x10000000u      /* 256 MB stack region */

#define VM_PPU_STACK_SIZE   0x00100000u      /* 1 MB default PPU stack */

/* The top of the stack region belongs to ppu_cb_entry_sp (ppu_loader.cpp): one
 * VM_PPU_STACK_SIZE slot per host thread that raises a guest callback while
 * running no guest code. vm_stack_alloc_init ends the thread-stack allocator
 * below this window, so sys_ppu_thread_create gets CELL_ENOMEM rather than a
 * stack overlapping a live callback slot. */
#define VM_CB_HOST_STACK_SLOTS   32u
#define VM_CB_HOST_STACK_WINDOW  (VM_CB_HOST_STACK_SLOTS * VM_PPU_STACK_SIZE)
#define VM_CB_HOST_STACK_BASE    (VM_STACK_BASE + VM_STACK_REGION - VM_CB_HOST_STACK_WINDOW)

/* Loader-owned main/worker TLS images. User allocations must skip this window. */
#define VM_PPU_TLS_BASE     0x10F00000u
#define VM_PPU_TLS_SIZE     0x00100000u

/* Dedicated guest windows owned by runtime allocators, not LV2 mmapper. */
#define VM_HLE_HEAP_BASE    0x50000000u
#define VM_HLE_HEAP_SIZE    0x08000000u
#define VM_AUDIO_BASE       0x58000000u
#define VM_AUDIO_PORT_SIZE  0x00200000u
#define VM_AUDIO_SIZE       0x01000000u
#define VM_SYS_VM_BASE      0x60000000u
#define VM_SYS_VM_SIZE      0x10000000u

/* Home for guest-visible structures the RUNTIME injects -- the GCM label window,
 * the control register (put/get/ref) and the IO<->EA offset tables. These must
 * NOT sit inside main memory, because that is where the title's own allocator
 * hands out blocks: flOw's heap runs 0x00C00000..0x0D000000, which swallowed the
 * old 0x03000000 home. The game memset straight over the control register, our
 * FIFO drain then read ASCII text as `put` (0x3D3D3D0F -- "===" from a log
 * banner), walked `get` into unmapped IO and resynchronised away whole frames of
 * commands. Nothing was drawn and nothing said why.
 *
 * 0x20000000 is unclaimed by the map above: past RSX (ends 0x20000000) and well
 * below the raw-SPU windows (0x30000000). The title only ever learns these
 * addresses through cellGcmGetLabelAddress / cellGcmGetControlRegister, so the
 * value is free to move as long as every user goes through this base. */
/* No fixed value works for every title. 0x03000000 was the original home until
 * flOw's heap grew over it; 0x20000000 replaced it and Gran Turismo 5 Prologue
 * maps its own 174 MB heap there (cellGcmMapMainMemory(0x20000000, 0xAE00000)),
 * so the GCM window and its 16 KB of offset tables landed on the game's own
 * allocations and quietly destroyed them. The same failure, one address along.
 *
 * So it is a variable, not a constant: a port that knows its title's memory map
 * assigns ppu_hle_inject_base before it runs any guest code, and everything
 * derived from VM_HLE_INJECT_BASE follows. The default is the historical value.
 * The window includes RSX reports, codec scratch and the 24-bit semaphore area;
 * reserve 16 MB. The title only ever learns these addresses
 * through cellGcmGetLabelAddress / cellGcmGetControlRegister /
 * cellGcmGetOffsetTable, so it is free to move. */
#ifdef __cplusplus
extern "C" {
#endif
extern uint32_t ppu_hle_inject_base;
#ifdef __cplusplus
}
#endif
#define VM_HLE_INJECT_BASE  ppu_hle_inject_base
#define VM_HLE_INJECT_SIZE  0x01000000u

#define VM_PAGE_SIZE        0x00001000u      /* 4 KB: the GUEST page size */

/* The HOST page size, which is what mprotect/VirtualProtect actually work in.
 * Windows and x86-64 Linux use 4 KB, so it used to be safe to assume the two
 * were the same number; Apple Silicon uses 16 KB, where an mprotect on a
 * 4 KB-aligned address fails with EINVAL and a 4 KB guard "page" is really a
 * 16 KB one that eats the bottom of the stack beside it. Everything that
 * changes protection goes through this, so guest-visible layout stays 4 KB
 * granular while the host operations are aligned to what the host needs. */
static inline uint32_t vm_host_page_size(void)
{
#ifdef _WIN32
    return VM_PAGE_SIZE;
#else
    static uint32_t s_page = 0;
    if (!s_page) {
        long v = sysconf(_SC_PAGESIZE);
        s_page = (v > 0) ? (uint32_t)v : VM_PAGE_SIZE;
    }
    return s_page;
#endif
}

/* Align a value up to `align` (must be power of 2) */
#define VM_ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))

/* ---------------------------------------------------------------------------
 * Global base pointer
 *
 * All PS3 guest address translation goes through this:
 *   host_ptr = vm_base + guest_addr
 * -----------------------------------------------------------------------*/
extern uint8_t* vm_base;

#ifndef _WIN32
/* mprotect over the host pages covering [addr, addr+size). */
static inline int vm__mprotect_pages(uint32_t addr, uint32_t size, int prot)
{
    uint64_t pg = vm_host_page_size();
    uint64_t a0 = (uint64_t)addr & ~(pg - 1u);
    uint64_t a1 = ((uint64_t)addr + size + pg - 1u) & ~(pg - 1u);
    return mprotect(vm_base + a0, (size_t)(a1 - a0), prot);
}
#endif

/* Guest address-space size; set non-zero once the host has mapped guest memory
 * (ppu_loader.cpp). Under native-VA mapping vm_base is deliberately 0 (guest
 * addr == host addr), so "is the VM ready?" must test this, not vm_base. */
extern uint32_t ppu_vm_size;
#define VM_READY (vm_base != 0 || ppu_vm_size != 0)

/* ---------------------------------------------------------------------------
 * Initialization / Shutdown
 * -----------------------------------------------------------------------*/

static inline void vm_shutdown(void);

/*
 * Reserve the host address range and commit the main memory region.
 * Returns CELL_OK on success, CELL_ENOMEM on failure.
 *
 * The base is 4 GB aligned, and that is a guarantee rather than a hope.
 * Translation runs one way as `host = vm_base + guest`, but a great deal of
 * the HLE runs the other way: a bridge handed a host pointer recovers the
 * guest address by truncating the pointer to 32 bits. GUEST_EA in
 * libs/guest_struct.h is that, and the GUEST_PTR macros accept either kind of
 * value on the same reasoning. The identity holds only while the low 32 bits
 * of vm_base are zero.
 *
 * It used to hold by luck, because both kernels tend to place a 4 GB
 * reservation on a 4 GB boundary. Nothing promises that, and the failure when
 * it stops is not a crash: the truncated pointer is a plausible-looking guest
 * address that moves with every run, so a write lands somewhere in the guest
 * arena and the guest's own variable keeps whatever was in it. The cost of
 * removing the question is one over-reservation that is trimmed back.
 */
static inline int32_t vm_init(void)
{
    if (vm_base != NULL) return CELL_OK; /* already initialized */

#ifdef _WIN32
    /*
     * Reserve a contiguous 4 GB region.  We only commit pages as needed.
     * MEM_RESERVE just reserves address space without backing pages.
     *
     * Windows cannot release part of a reservation, so the equivalent of the
     * POSIX trim below is to over-reserve, note where the aligned base falls,
     * release the lot and re-reserve exactly there. Another allocation could
     * take that address in between, hence the retries; on a 64-bit address
     * space losing a race for an 8 GB hole repeatedly is not a real risk, but
     * failing outright beats carrying on unaligned.
     */
    for (int attempt = 0; attempt < 8 && !vm_base; attempt++) {
        uint8_t* probe = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)(VM_TOTAL_SIZE * 2),
                                                MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) break;
        uint8_t* aligned = (uint8_t*)(((uintptr_t)probe + (uintptr_t)VM_TOTAL_SIZE - 1u)
                                      & ~((uintptr_t)VM_TOTAL_SIZE - 1u));
        VirtualFree(probe, 0, MEM_RELEASE);
        vm_base = (uint8_t*)VirtualAlloc(aligned, (SIZE_T)VM_TOTAL_SIZE,
                                         MEM_RESERVE, PAGE_NOACCESS);
    }
    if (!vm_base) return CELL_ENOMEM;

    /* Commit main memory region (256 MB, read/write) */
    if (!VirtualAlloc(vm_base + VM_MAIN_MEM_BASE, VM_MAIN_MEM_SIZE,
                      MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(vm_base, 0, MEM_RELEASE);
        vm_base = NULL;
        return CELL_ENOMEM;
    }

    /* Commit stack region */
    if (!VirtualAlloc(vm_base + VM_STACK_BASE, VM_STACK_REGION,
                      MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(vm_base, 0, MEM_RELEASE);
        vm_base = NULL;
        return CELL_ENOMEM;
    }

#else /* POSIX */
    /* Over-map and trim: ask for twice the space, keep the 4 GB aligned window
     * inside it, and give the head and tail back. mmap can hand back part of a
     * mapping, so this needs no retry and cannot race. */
    {
        size_t want = (size_t)VM_TOTAL_SIZE;
        uint8_t* raw = (uint8_t*)mmap(NULL, want * 2, PROT_NONE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                      -1, 0);
        if (raw == MAP_FAILED) {
            vm_base = NULL;
            return CELL_ENOMEM;
        }
        uint8_t* aligned = (uint8_t*)(((uintptr_t)raw + (uintptr_t)want - 1u)
                                      & ~((uintptr_t)want - 1u));
        if (aligned > raw)
            munmap(raw, (size_t)(aligned - raw));
        if (raw + want * 2 > aligned + want)
            munmap(aligned + want, (size_t)((raw + want * 2) - (aligned + want)));
        vm_base = aligned;
    }

    /* Make main memory readable/writable */
    if (mprotect(vm_base + VM_MAIN_MEM_BASE, VM_MAIN_MEM_SIZE,
                 PROT_READ | PROT_WRITE) != 0) {
        munmap(vm_base, (size_t)VM_TOTAL_SIZE);
        vm_base = NULL;
        return CELL_ENOMEM;
    }

    /* Make stack region readable/writable */
    if (mprotect(vm_base + VM_STACK_BASE, VM_STACK_REGION,
                 PROT_READ | PROT_WRITE) != 0) {
        munmap(vm_base, (size_t)VM_TOTAL_SIZE);
        vm_base = NULL;
        return CELL_ENOMEM;
    }
#endif

    /* The invariant every host-pointer-to-guest-address truncation rests on.
     * The reservations above hold it by construction; this is here so that a
     * change to either of them announces itself now, instead of as a stray
     * four-byte write to a moving address somewhere else entirely. */
    assert(((uintptr_t)vm_base & ((uintptr_t)VM_TOTAL_SIZE - 1u)) == 0);
    if (((uintptr_t)vm_base & ((uintptr_t)VM_TOTAL_SIZE - 1u)) != 0) {
        vm_shutdown();
        return CELL_ENOMEM;
    }

    /* Zero main memory */
    memset(vm_base + VM_MAIN_MEM_BASE, 0, VM_MAIN_MEM_SIZE);

    return CELL_OK;
}

static inline void vm_shutdown(void)
{
    if (!vm_base) return;

#ifdef _WIN32
    VirtualFree(vm_base, 0, MEM_RELEASE);
#else
    munmap(vm_base, (size_t)VM_TOTAL_SIZE);
#endif

    vm_base = NULL;
}

/* ---------------------------------------------------------------------------
 * Memory region commit / protect
 * -----------------------------------------------------------------------*/

/* The arena the guest may touch: [0, limit). A zero ppu_vm_size means the host
 * maps (or commits on fault) the whole 32-bit space, NOT a zero-length arena,
 * so it must never be used as the limit itself. */
static inline uint64_t vm_guest_limit(void)
{
    return ppu_vm_size ? (uint64_t)ppu_vm_size : VM_TOTAL_SIZE;
}

/* Is the guest span [ea, ea+size) wholly inside guest memory? This is the ONE
 * copy of the rule: vm_commit/vm_protect use it, and every handler that forms a
 * host pointer from a guest span asks it instead of comparing ppu_vm_size
 * itself. VM_READY is part of the rule: before vm_init nothing is mapped, and
 * under native-VA mapping vm_base is 0 (guest EA == host address), so
 * "vm_base != NULL" is not the readiness test. Written as a subtraction so that
 * a size near UINT64_MAX cannot wrap ea+size to a small number and pass. */
static inline int vm_range_ok(uint32_t ea, uint64_t size)
{
    if (!VM_READY) return 0;
    const uint64_t limit = vm_guest_limit();
    return (uint64_t)ea <= limit && size <= limit - (uint64_t)ea;
}

/* The result must be checked: a failed commit leaves the pages PROT_NONE, and a
 * host access to them faults instead of stopping by name (vm_commit_or_stop). */
PS3_NODISCARD static inline int32_t vm_commit(uint32_t addr, uint32_t size)
{
    /* In the arena and backed? A range that wraps or crosses the limit is
     * refused; vm_range_ok carries the readiness test too. */
    if (!vm_range_ok(addr, size)) return CELL_EFAULT;
    if (size == 0) return CELL_OK;
    size = VM_ALIGN_UP(size, VM_PAGE_SIZE);

#ifdef _WIN32
    if (!VirtualAlloc(vm_base + addr, size, MEM_COMMIT, PAGE_READWRITE))
        return CELL_ENOMEM;
#else
    if (vm__mprotect_pages(addr, size, PROT_READ | PROT_WRITE) != 0)
        return CELL_ENOMEM;
#endif

    return CELL_OK;
}

/* Commit a guest span that a host access (kernel I/O, memcpy) is about to touch,
 * or stop. The caller has already proved the span in range (vm_range_ok, else
 * ps3_stop_guest_range), so a failure here is a host fault (mprotect or
 * VirtualAlloc), for which the firmware has no code: PS3_STOP_HLE_COMMIT_FAILED.
 * vm_commit takes a u32 size, and only the single arena-wide [0, 4 GiB) span does
 * not fit, so the span is committed in page-aligned chunks rather than truncated. */
static inline void vm_commit_or_stop(const char* func, const char* arg, uint32_t ea, uint64_t size)
{
    /* The stop names the caller's whole span, not the chunk that failed. */
    uint32_t at = ea;
    for (uint64_t left = size; left;) {
        const uint32_t chunk = left > 0xFFFFF000ull ? 0xFFFFF000u : (uint32_t)left;
        const int32_t rc = vm_commit(at, chunk);
        if (rc != CELL_OK) ps3_stop_commit_failed(func, arg, ea, size, rc);
        at += chunk;
        left -= chunk;
    }
}

static inline int32_t vm_protect(uint32_t addr, uint32_t size, int read, int write, int exec)
{
    if (!vm_range_ok(addr, size)) return CELL_EFAULT;
    if (size == 0) return CELL_OK;
    size = VM_ALIGN_UP(size, VM_PAGE_SIZE);

#ifdef _WIN32
    DWORD prot = PAGE_NOACCESS;
    if (read && write && exec) prot = PAGE_EXECUTE_READWRITE;
    else if (read && write)    prot = PAGE_READWRITE;
    else if (read && exec)     prot = PAGE_EXECUTE_READ;
    else if (read)             prot = PAGE_READONLY;
    else if (exec)             prot = PAGE_EXECUTE;

    DWORD old;
    if (!VirtualProtect(vm_base + addr, size, prot, &old))
        return CELL_EFAULT;
#else
    int prot = PROT_NONE;
    if (read)  prot |= PROT_READ;
    if (write) prot |= PROT_WRITE;
    if (exec)  prot |= PROT_EXEC;

    if (vm__mprotect_pages(addr, size, prot) != 0)
        return CELL_EFAULT;
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Address translation helpers
 * -----------------------------------------------------------------------*/

static inline void* vm_to_host(uint32_t addr)
{
    return (void*)(vm_base + addr);
}

static inline uint32_t vm_to_guest(const void* host_ptr)
{
    return (uint32_t)((const uint8_t*)host_ptr - vm_base);
}

/* ---------------------------------------------------------------------------
 * Stack allocation for PPU threads
 *
 * Simple bump allocator within the stack region.  Each thread gets
 * a fixed-size stack with a guard page at the bottom.
 * -----------------------------------------------------------------------*/
#define VM_STACK_FREE_MAX 256u

typedef struct vm_stack_entry {
    uint32_t base;
    uint32_t size;  /* host-page aligned size */
} vm_stack_entry;

typedef struct vm_stack_alloc {
    uint32_t next_addr;     /* next stack base to allocate */
    uint32_t region_end;    /* end of the thread-stack area: VM_CB_HOST_STACK_BASE, not the region end */
    uint32_t free_count;
    vm_stack_entry free_entries[VM_STACK_FREE_MAX];
} vm_stack_alloc;

static inline void vm_stack_alloc_init(vm_stack_alloc* sa)
{
    sa->next_addr   = VM_STACK_BASE;
    sa->region_end  = VM_CB_HOST_STACK_BASE;
    sa->free_count  = 0;
    memset(sa->free_entries, 0, sizeof(sa->free_entries));
}

/*
 * Allocate a stack: *base_out = the BASE address of the usable stack (lowest
 * address, above its guard page); the stack top is base + size.
 * CELL_ENOMEM: the stack region is spent. CELL_EFAULT: the guard page could not
 * be set (vm_protect refused the range or the host failed it); nothing is
 * allocated, because a stack without its guard would overflow silently.
 */
static inline int32_t vm_stack_allocate(vm_stack_alloc* sa, uint32_t stack_size, uint32_t* base_out)
{
    /* Host page granularity throughout: the guard must be exactly one host
     * page, and the stack above it must start on a host page boundary, or a
     * 16 KB host would either refuse the mprotect or protect part of the
     * stack itself. On a 4 KB host this is the layout it always was. */
    const uint32_t pg = vm_host_page_size();
    /* 64 bits: a guest stack_size within a page of UINT32_MAX used to make
     * VM_ALIGN_UP (and stack_size + pg) wrap to a small number, so the region
     * check below passed and the caller got a stack with no room in it. A
     * request that does not fit the region is CELL_ENOMEM. */
    const uint64_t aligned = ((uint64_t)stack_size + pg - 1u) & ~((uint64_t)pg - 1u);

    /* Search host-side free list for exact-size match (LIFO: search backwards).
     * Exact-size matching avoids internal fragmentation and block splitting
     * (which would leave sub-blocks without guard pages). The guard page at
     * base - pg was set once when initially carved from bump space and stays
     * protected across reuses. */
    for (uint32_t i = sa->free_count; i > 0; i--) {
        uint32_t idx = i - 1;
        if (sa->free_entries[idx].size == (uint32_t)aligned) {
            uint32_t base = sa->free_entries[idx].base;
            sa->free_entries[idx] = sa->free_entries[sa->free_count - 1];
            sa->free_count--;
            /* Zero the usable stack range before handing out (not the guard page).
             * Before recycling, new bump-allocated stacks came from the zero-filled
             * arena (anonymous mmap / VirtualAlloc), so guest code reading uninitialized
             * stack frames saw 0. Zeroing on reuse preserves that invariant. */
            memset(vm_to_host(base), 0, (size_t)aligned);
            *base_out = base;
            return CELL_OK;
        }
    }

    /* Add a guard page */
    const uint64_t total = aligned + pg;

    if ((uint64_t)sa->next_addr + total > sa->region_end)
        return CELL_ENOMEM;

    uint32_t base = sa->next_addr;
    /* Guard page at the bottom (no access) */
    int32_t rc = vm_protect(base, pg, 0, 0, 0);
    if (rc != CELL_OK)
        return CELL_EFAULT;

    sa->next_addr += (uint32_t)total;
    *base_out = base + pg; /* skip the guard page */
    return CELL_OK;
}

/*
 * Free a stack previously allocated with vm_stack_allocate.
 * base is the usable base (*base_out from vm_stack_allocate, above the guard page).
 * The guard page at base - pg remains PROT_NONE and is not touched.
 * Freed stacks are stored host-side in sa->free_entries without storing metadata
 * in guest memory.
 */
static inline void vm_stack_free(vm_stack_alloc* sa, uint32_t base, uint32_t size)
{
    if (!sa || base == 0 || size == 0)
        return;

    const uint32_t pg = vm_host_page_size();
    const uint64_t aligned = ((uint64_t)size + pg - 1u) & ~((uint64_t)pg - 1u);
    if (aligned == 0 || (uint64_t)base + aligned > sa->region_end)
        return;
    if (base < pg || base + aligned > sa->next_addr)
        return;
    if ((base & (pg - 1u)) != 0)
        return;

    /* Prevent duplicate frees */
    for (uint32_t i = 0; i < sa->free_count; i++) {
        if (sa->free_entries[i].base == base)
            return;
    }

    if (sa->free_count < VM_STACK_FREE_MAX) {
        sa->free_entries[sa->free_count].base = base;
        sa->free_entries[sa->free_count].size = (uint32_t)aligned;
        sa->free_count++;
    } else {
        /* Free list is full: log the leak explicitly (never silent drop).
         * 256 entries exceeds the maximum number of 1 MB stacks (223) that the 224 MB
         * region can hold, and is 4x PPU_THREAD_MAX (64). */
        fprintf(stderr, "[VM] vm_stack_free: free list full (%u entries), leaking stack base=0x%08X size=0x%08X\n",
                (unsigned)sa->free_count, base, (unsigned)aligned);
    }
}

/* ---------------------------------------------------------------------------
 * SPU local store allocation
 *
 * Each SPU gets a 256 KB window in the VM_SPU_BASE range, used for
 * raw SPU memory-mapped access from the PPU.
 * -----------------------------------------------------------------------*/

static inline int32_t vm_spu_ls_map(uint32_t spu_index)
{
    if (spu_index >= 8) return CELL_EINVAL; /* max 8 SPUs */

    uint32_t addr = VM_SPU_BASE + spu_index * VM_SPU_WINDOW_SIZE;
    return vm_commit(addr, VM_SPU_WINDOW_SIZE);
}

static inline uint32_t vm_spu_ls_addr(uint32_t spu_index)
{
    return VM_SPU_BASE + spu_index * VM_SPU_WINDOW_SIZE;
}

static inline uint8_t* vm_spu_ls_host_ptr(uint32_t spu_index)
{
    return vm_base + VM_SPU_BASE + spu_index * VM_SPU_WINDOW_SIZE;
}

static inline int vm_is_valid_addr(uint32_t addr)
{
    /* Check if address falls within a committed region */
    if (addr >= VM_MAIN_MEM_BASE && addr < VM_MAIN_MEM_BASE + VM_MAIN_MEM_SIZE)
        return 1;
    if (addr >= VM_STACK_BASE && addr < VM_STACK_BASE + VM_STACK_REGION)
        return 1;
    if (addr >= VM_SPU_BASE && addr < VM_SPU_BASE + 8 * VM_SPU_WINDOW_SIZE)
        return 1;
    if (addr >= VM_RSX_BASE && addr < VM_RSX_BASE + VM_RSX_SIZE)
        return 1;
    return 0;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VM_H */

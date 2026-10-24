/*
 * ps3recomp - sysPrxForUser CRT (boot-critical HLE)
 *
 * The first firmware functions a PS3 program calls at startup come from
 * sysPrxForUser (the libc/CRT bridge). Some need the full ppu_context (e.g.
 * sys_initialize_tls sets the thread pointer r13), so they register as
 * context-aware handlers (ps3_hle_register_ctx) rather than through the generic
 * integer-ABI table.
 *
 * NIDs are computed from the names (ps3_compute_nid), so this stays correct
 * without hand-written NID literals.
 */
#define PPU_NO_INLINE_LOADS   /* declares its own out-of-line vm_read* below */
#include "ppu_recomp.h"     /* ppu_context */
#include "ps3emu/nid.h"     /* ps3_compute_nid */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* win32_compat.h is <windows.h> on Windows and the POSIX shims elsewhere --
 * Sleep, DWORD, QPC. */
#include "../platform/win32_compat.h"
#include "../memory/vm.h"   /* VM_HLE_INJECT_BASE -- not platform-specific */
#include "../syscalls/sys_lwsync.h"   /* ps3_lwsync_register */
#include "ps3emu/log_first.h"

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" uint32_t vm_read32(uint64_t a);
extern "C" uint64_t vm_read64(uint64_t a);
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);

/* Simple bump allocator for TLS areas, in a free vm region below the stack. */
static uint32_t s_tls_next = 0x0E000000u;

/* sys_initialize_tls(u64 main_thread_id, u32 tls_seg_addr, u32 tls_seg_size,
 *                     u32 tls_mem_size) -- set up the main thread's TLS block
 * and point r13 (the PPC64 thread pointer) at it. TLS variables are accessed
 * at r13 - 0x7000 (the static TLS block bias). */
static void sys_initialize_tls(ppu_context* ctx)
{
    uint32_t seg_addr = (uint32_t)ctx->gpr[4];
    uint32_t seg_size = (uint32_t)ctx->gpr[5];
    uint32_t mem_size = (uint32_t)ctx->gpr[6];

    uint32_t block = s_tls_next;
    uint32_t total = ((mem_size + 0x7000u + 0x1000u) + 0xFFFu) & ~0xFFFu;
    s_tls_next += total;

    if (seg_addr && seg_size) memcpy(vm_base + block, vm_base + seg_addr, seg_size);
    if (mem_size > seg_size)  memset(vm_base + block + seg_size, 0, mem_size - seg_size);

    ctx->gpr[13] = block + 0x7000u;   /* thread pointer; TLS data at r13-0x7000 */
    ctx->gpr[3]  = 0;                  /* CELL_OK */
    fprintf(stderr, "[crt] sys_initialize_tls: block 0x%08X, r13=0x%08X (seg 0x%X+%u, mem %u)\n",
            block, (uint32_t)ctx->gpr[13], seg_addr, seg_size, mem_size);
}

/* sys_time_get_system_time() -> microseconds since boot, REAL time.
 * This was a fake counter advancing 1 ms PER CALL ("so callers see time
 * progress") -- so any guest clock built on it ran at call-rate, not
 * wall-time. LBP's Bink movie clock (sysGetSystemTime import 0x8461E528
 * lands here) paced the intro at ~1/10th speed: video decoded at <1 fps,
 * the audio preload threshold took forever to fill (movie stayed silent),
 * and every frontend animation timed off it crawled. */
static void sys_time_get_system_time(ppu_context* ctx)
{
#ifdef _WIN32
    static LARGE_INTEGER s_freq, s_base;
    if (!s_freq.QuadPart) { QueryPerformanceFrequency(&s_freq); QueryPerformanceCounter(&s_base); }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    ctx->gpr[3] = (uint64_t)((now.QuadPart - s_base.QuadPart) * 1000000ull / (uint64_t)s_freq.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    ctx->gpr[3] = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

/* sys_process_is_stack(u32 addr) -> 1 if addr is in the stack region. We model
 * a single stack just below the TLS region; good enough for boot checks. */
static void sys_process_is_stack(ppu_context* ctx)
{
    uint32_t a = (uint32_t)ctx->gpr[3];
    ctx->gpr[3] = (a >= 0x0E000000u && a < 0x10000000u) ? 1 : 0;
}

/* sys_ppu_thread_get_id(vm::ptr<u64> id) -> *id = calling thread's real id.
 * The old fixed "1" broke every am-I-the-designated-thread check in
 * multithreaded titles (LBP's job system routes work by thread identity, so
 * its queues were never serviced and network-init parked forever). */
static void sys_ppu_thread_get_id(ppu_context* ctx)
{
    uint32_t p = (uint32_t)ctx->gpr[3];
    /* Every registered thread has a nonzero id (main = 1 via
     * ppu_thread_register_main); 0 = unregistered scratch ctx, report a
     * never-a-real-thread sentinel (outside the host-callback id range of
     * ppu_loader.cpp's ppu_host_thread_id). */
    if (p) vm_write64(p, ctx->thread_id ? (uint64_t)ctx->thread_id : 0x7FFFFFFEull);
    ctx->gpr[3] = 0;
}

/* sys_mmapper_allocate_memory(u32 size, u64 flags, vm::ptr<u32> mem_id) ->
 * hand back a unique opaque id; the backing is the flat VM, so the later
 * search_and_map just needs a non-zero id to track. */
/* id -> size so sys_mmapper_search_and_map (lv2 337) can lay blocks out
 * without overlap. Ids are dense from 0x1000. */
static uint32_t s_mm_sizes[256];
static uint32_t s_mmapper_next_id = 0x1000;
extern "C" uint32_t ps3_mmapper_block_size(uint32_t mem_id)
{
    uint32_t i = mem_id - 0x1000u;
    return (i < 256) ? s_mm_sizes[i] : 0;
}

static uint32_t mmapper_new_id(uint32_t size)
{
    uint32_t id = s_mmapper_next_id++;
    if (id - 0x1000u < 256) s_mm_sizes[id - 0x1000u] = size;
    return id;
}

static void sys_mmapper_allocate_memory(ppu_context* ctx)
{
    uint32_t size       = (uint32_t)ctx->gpr[3];
    uint32_t mem_id_ptr = (uint32_t)ctx->gpr[5];
    uint32_t id         = mmapper_new_id(size);
    if (getenv("PS3_MEMTRACE"))
        fprintf(stderr, "[mmapper] allocate_memory(size=0x%X flags=0x%llX id_ptr=0x%X) -> id 0x%X\n",
                size, (unsigned long long)ctx->gpr[4], mem_id_ptr, id);
    if (mem_id_ptr) vm_write32(mem_id_ptr, id);
    ctx->gpr[3] = 0;
}
/* sys_mmapper_allocate_memory_from_container(u32 size, u32 container, u64 flags,
 * vm::ptr<u32> mem_id) -> id in *r6. flОw's CRT uses this for its heap/mutex pool;
 * it was previously UNregistered (CRT saw failure -> "not enough memory"). */
static void sys_mmapper_allocate_memory_from_container(ppu_context* ctx)
{
    uint32_t size = (uint32_t)ctx->gpr[3];
    uint32_t mem_id_ptr = (uint32_t)ctx->gpr[6];
    uint32_t id = mmapper_new_id(size);
    if (getenv("PS3_MEMTRACE"))
        fprintf(stderr, "[mmapper] alloc_from_container(size=0x%X cid=0x%X flags=0x%llX id_ptr=0x%X) -> id 0x%X\n",
                size, (uint32_t)ctx->gpr[4], (unsigned long long)ctx->gpr[5], mem_id_ptr, id);
    if (mem_id_ptr) vm_write32(mem_id_ptr, id);
    ctx->gpr[3] = 0;
}

/* A handful of CRT helpers the early boot tends to hit; accept and continue. */
static void crt_ok(ppu_context* ctx) { ctx->gpr[3] = 0; }

/* Real preemptive thread create/exit live in the lv2 syscall layer
 * (syscalls/sys_ppu_thread.c) and spawn a host thread that runs the guest
 * entry through the recompiled code. The CRT also reaches them as
 * sysPrxForUser import NIDs (gen_hle_nids can't see them — they're not defined
 * in the sysPrxForUser lib), so bridge the NIDs to the same implementation.
 * Without this the CRT's thread/static-init runs through an uninitialised
 * object table and calls heap addresses as function pointers. */
extern "C" int64_t sys_ppu_thread_create(ppu_context* ctx);
extern "C" int64_t sys_ppu_thread_exit(ppu_context* ctx);
/* The ctx-aware dispatch (ppu_hle.cpp) does NOT propagate a handler return value
 * into gpr[3] -- each ctx handler must set gpr[3] itself. sys_ppu_thread_create
 * signals success by *returning* CELL_OK(0) (it never writes gpr[3]), so we must
 * store that return into gpr[3]. Otherwise gpr[3] is left as the incoming out-ptr
 * (&tid, nonzero) and the guest wrapper reads it as "create failed" -- e.g. LBP's
 * sub_52613C does `v6 = (ret==0); return v6 ? tid : 0`, so a nonzero ret makes it
 * hand back 0 and the caller's init (sub_C1484 / KdConvert) bails. */
static void hle_ppu_thread_create(ppu_context* ctx) { ctx->gpr[3] = sys_ppu_thread_create(ctx); }
static void hle_ppu_thread_exit(ppu_context* ctx)   { ctx->gpr[3] = sys_ppu_thread_exit(ctx); }

/* _cellGcmInitBody (NID 0x15BAE46B) -- the GCM init every PS3 game calls via the
 * cellGcmInit() SDK macro. cellGcmSys.c provides the layout-correct core
 * (cellGcmSetupContext) but needs the owning vm to allocate the guest
 * CellGcmContextData and write the game's context-out pointer; supply those as
 * callbacks. Without this the game's GCM context stays null -> null deref. */
typedef unsigned int (*CellGcmGuestAlloc)(unsigned int, unsigned int);
typedef void (*CellGcmGuestWrite32)(unsigned int, unsigned int);
extern "C" unsigned int cellGcmSetupContext(unsigned int ctx_out_addr,
    unsigned int cmdSize, unsigned int ioSize, unsigned int ioAddress,
    CellGcmGuestAlloc galloc, CellGcmGuestWrite32 gwrite32);

static unsigned int gcm_guest_alloc(unsigned int size, unsigned int align)
{
    /* Bump from a small scratch region below the main stack (0x0FF00000) and
     * above the TLS image -- a few control structs, never freed. */
    static unsigned int bump = 0x0F800000u;
    if (align < 16) align = 16;
    bump = (bump + align - 1) & ~(align - 1);
    unsigned int a = bump;
    bump += (size + 15u) & ~15u;
    return a;
}
static void gcm_guest_write32(unsigned int addr, unsigned int val) { vm_write32(addr, val); }

/* FIFO command-buffer-full callback. cellGcmSetupContext points the guest
 * context's callback OPD at GCM_FIFO_CALLBACK_SENTINEL_EA; the title's inline
 * gcmReserve calls context->callback(context, count) on ring wrap, which the
 * indirect dispatcher routes here. r3 = guest context EA.
 *
 * Derived from VM_HLE_INJECT_BASE, the same base libs/video/cellGcmSys.c
 * builds the OPD from. This was a second hardcoded copy of the address, and
 * moving the injected block out of guest-allocatable memory updated only one
 * of them: the OPD then pointed at an EA with no registered function, the
 * garbage-vcall guard no-opped the callback, the FIFO never recycled and the
 * title stopped flipping. One definition, one place. */
#define GCM_FIFO_CALLBACK_SENTINEL_EA (VM_HLE_INJECT_BASE + 0x2F00u)
extern "C" void cellGcm_fifo_recycle(unsigned int ctx_ea);
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
static void hle_gcm_callback(ppu_context* ctx)
{
    cellGcm_fifo_recycle((unsigned int)ctx->gpr[3]);   /* r3 = context EA */
    ctx->gpr[3] = 0;                                   /* CELL_OK */
}

static void hle_cellGcmInitBody(ppu_context* ctx)
{
    uint32_t ctx_out = (uint32_t)ctx->gpr[3];
    uint32_t cmdSize = (uint32_t)ctx->gpr[4];
    uint32_t ioSize  = (uint32_t)ctx->gpr[5];
    uint32_t ioAddr  = (uint32_t)ctx->gpr[6];
    fprintf(stderr, "[HLE] _cellGcmInitBody(ctx_out=0x%08X, cmdSize=0x%X, ioSize=0x%X, ioAddr=0x%X)\n",
            ctx_out, cmdSize, ioSize, ioAddr);
    /* 0 = the firmware's argument validation failed (CELL_GCM_ERROR_FAILURE, 0x802100FF) */
    ctx->gpr[3] = cellGcmSetupContext(ctx_out, cmdSize, ioSize, ioAddr, gcm_guest_alloc, gcm_guest_write32)
                      ? 0u : 0x802100FFu;
}

/* --- sys_net offline model ---------------------------------------------
 * LBP's net-services tick (sub_11A864) drains its UDP socket with
 * non-blocking recvfrom until it returns -1 (empty socket = EWOULDBLOCK on
 * real firmware). These NIDs were unresolved, and the unresolved default of
 * r3=0 reads as "received a 0-byte packet": the drain loop spins forever
 * while holding the net-manager lwmutex, wedging the whole boot at the
 * Network init node. Model the offline truth instead: no data, no sockets --
 * every receive/poll would-block. errno lives in a guest scratch cell since
 * _sys_net_errno_loc returns a POINTER the game dereferences. */
#define SYS_NET_EWOULDBLOCK_V 35
extern "C" void ps3_net_host_register(unsigned int (*guest_alloc)(unsigned int, unsigned int));
extern "C" void np_score_register_ctx(void);   /* libs/network/sceNpScore.c */
extern "C" void cellSaveData_register_ctx_handlers(void); /* libs/system/cellSaveData.c */
extern "C" void avconf_register_ctx(void);     /* libs/misc/cellAvconfExt.c */
static uint32_t g_net_errno_ea = 0;
static void hle_net_errno_loc(ppu_context* ctx)
{
    if (!g_net_errno_ea) g_net_errno_ea = gcm_guest_alloc(4, 4);
    vm_write32(g_net_errno_ea, SYS_NET_EWOULDBLOCK_V);
    ctx->gpr[3] = g_net_errno_ea;
}
static void hle_net_wouldblock(ppu_context* ctx)
{
    if (g_net_errno_ea) vm_write32(g_net_errno_ea, SYS_NET_EWOULDBLOCK_V);
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)-1;
}
static void hle_net_zero(ppu_context* ctx) { ctx->gpr[3] = 0; }
/* select/poll: nothing is ever ready offline -- but a real select BLOCKS for
 * the caller's timeout before saying so. Returning instantly turned LBP's
 * 30Hz net pump (sub_3A5548: select(1, r/w/e sets, {0s, 33333us})) into a
 * 100%-CPU busy-spin that also dominated the guest-PC profiler, masquerading
 * as a boot hang. Honor the timeout and clear the fd sets (1024-bit each). */
static void hle_net_select(ppu_context* ctx)
{
    uint32_t rd = (uint32_t)ctx->gpr[4], wr = (uint32_t)ctx->gpr[5];
    uint32_t ex = (uint32_t)ctx->gpr[6], tv = (uint32_t)ctx->gpr[7];
    uint64_t us = 10000;              /* NULL timeout = block forever: tick at 10ms instead */
    if (tv) us = vm_read64(tv) * 1000000ull + vm_read64(tv + 8);   /* {s64 sec, s64 usec} BE */
    if (us > 100000) us = 100000;     /* cap so shutdown stays responsive */
    if (us) Sleep((DWORD)((us + 999) / 1000));
    const uint32_t sets[3] = { rd, wr, ex };
    for (int s = 0; s < 3; s++)
        if (sets[s]) for (uint32_t i = 0; i < 128; i += 4) vm_write32(sets[s] + i, 0);
    ctx->gpr[3] = 0;                  /* 0 fds ready */
}
static void hle_net_poll(ppu_context* ctx)
{
    uint32_t fds  = (uint32_t)ctx->gpr[3];
    uint32_t nfds = (uint32_t)ctx->gpr[4];
    int32_t  ms   = (int32_t)(uint32_t)ctx->gpr[5];
    if (ms < 0 || ms > 100) ms = (ms < 0) ? 10 : 100;   /* -1 = infinite: tick at 10ms */
    if (ms) Sleep((DWORD)ms);
    for (uint32_t i = 0; i < nfds && i < 64; i++)       /* pollfd = {s32 fd, s16 ev, s16 rev} */
        if (fds) vm_write32(fds + i * 8 + 4, vm_read32(fds + i * 8 + 4) & 0xFFFF0000u);
    ctx->gpr[3] = 0;                  /* 0 fds ready */
}
/* Distinct small fds: the unresolved default handed EVERY socket() call fd 0,
 * making all sockets alias one id in the game's tables. */
static void hle_net_socket(ppu_context* ctx) { static uint32_t s_fd = 3; ctx->gpr[3] = s_fd++; }
/* sendto: report the full length as sent (packets vanish into the void, matching
 * the RPCS3-offline oracle where broadcasts go out and nothing answers). */
static void hle_net_sendto(ppu_context* ctx) { ctx->gpr[3] = (uint32_t)ctx->gpr[5]; }

/* _sys_spu_image_import (sysPrxForUser NID 0xEBE5F72F) -- the user-space wrapper
 * libsre calls during cellSpursInitialize to load the SPURS SPU kernel into the
 * sys_spu_image struct. Previously UNRESOLVED -> returned 0 -> libsre proceeded
 * with an EMPTY image (entry=0) -> the 5 SPURS SPU threads ran nothing -> PPU
 * busy-waits on an SPU completion that never comes -> 0 GCM draws
 * (prx/spu_kernel/README.txt step 1). Parses the SPU ELF at r4 into the image
 * struct at r3.
 *
 * The ELF-parse (segment/entry layout below) is lifted directly from sagemono's
 * sys_spu_image_import SYSCALL handler in PR #57 (fix/spu-image-import); this is
 * the sysPrxForUser LIBRARY counterpart libsre actually calls. Credit: sagemono
 * (PR #57) for the import implementation + SPU-image syscall-number fix.
 * Self-verifying: no-op unless r4 points to an ELF (so a wrong NID guess is safe).
 * sys_spu_image { u32 type; u32 entry; sys_spu_segment* segs; int nsegs; }
 * sys_spu_segment{ int type; u32 ls_start; int size; u64 src_pa }  (0x18, src@0x10) */
/* Last SPU image parsed below: where its loadable bytes live, which local
 * store address they start at, and how far they span. Read by the SPU DMA
 * path (SPU_DSP_IMAGE_EA in spu_dma.h) to recover a section load whose
 * source address the title lost between import and use. */
extern "C" uint32_t g_spu_image_src_ea = 0, g_spu_image_ls_start = 0,
                    g_spu_image_span = 0;

/* img_ea -> the EA of the SPU ELF it was parsed from. Small and fixed: a title
 * has a handful of images, and a repeat import of the same descriptor replaces
 * its entry. */
#define SPU_IMG_SRC_MAX 32
static struct { uint32_t img, src; } s_spu_img_src[SPU_IMG_SRC_MAX];

extern "C" void ps3_spu_image_record(uint32_t img_ea, uint32_t src_ea)
{
    if (!img_ea || !src_ea) return;
    for (int i = 0; i < SPU_IMG_SRC_MAX; i++)
        if (s_spu_img_src[i].img == img_ea || s_spu_img_src[i].img == 0) {
            s_spu_img_src[i].img = img_ea; s_spu_img_src[i].src = src_ea; return;
        }
}

extern "C" uint32_t ps3_spu_image_source_ea(uint32_t img_ea)
{
    for (int i = 0; i < SPU_IMG_SRC_MAX; i++)
        if (s_spu_img_src[i].img == img_ea) return s_spu_img_src[i].src;
    return 0;
}

extern "C" void spu_raw_note_image(uint32_t src_ea, uint32_t entry);  /* runtime/spu/spu_raw.c */

static void hle_sys_spu_image_import(ppu_context* ctx)
{
    uint32_t img_ea = (uint32_t)ctx->gpr[3];
    uint32_t src_ea = (uint32_t)ctx->gpr[4];
    fprintf(stderr, "[HLE] _sys_spu_image_import(img=0x%08X src=0x%08X r5=0x%08X r6=0x%08X)\n",
            img_ea, src_ea, (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);
    if (!img_ea || !src_ea || !vm_base) { ctx->gpr[3] = 0; return; }
    const uint8_t* e = vm_base + src_ea;
    if (!(e[0]==0x7F && e[1]=='E' && e[2]=='L' && e[3]=='F')) {
        fprintf(stderr, "[HLE] _sys_spu_image_import: src not an ELF -> no-op\n");
        fflush(stderr); ctx->gpr[3] = 0; return;
    }
    uint16_t machine = (uint16_t)((e[0x12] << 8) | e[0x13]);   /* 23 = SPU */
    uint32_t entry   = vm_read32(src_ea + 0x18);
    uint32_t phoff   = vm_read32(src_ea + 0x1C);
    uint16_t phentsz = (uint16_t)((e[0x2A] << 8) | e[0x2B]); if (!phentsz) phentsz = 0x20;
    uint16_t phnum   = (uint16_t)((e[0x2C] << 8) | e[0x2D]);
    static uint32_t s_seg_bump = 0x0D000000u;
    uint32_t segs_ea = s_seg_bump; int nsegs = 0;
    for (uint16_t i = 0; i < phnum && nsegs < 32; i++) {
        uint32_t ph = phoff + (uint32_t)i * phentsz;
        if (vm_read32(src_ea + ph + 0x00) != 1) continue;      /* PT_LOAD */
        uint32_t p_off = vm_read32(src_ea + ph + 0x04);
        uint32_t p_va  = vm_read32(src_ea + ph + 0x08);
        uint32_t p_fsz = vm_read32(src_ea + ph + 0x10);
        uint32_t p_msz = vm_read32(src_ea + ph + 0x14);
        uint32_t seg = segs_ea + (uint32_t)nsegs * 0x18;       /* COPY */
        vm_write32(seg + 0x00, 1); vm_write32(seg + 0x04, p_va);
        vm_write32(seg + 0x08, p_fsz);
        /* src is the u64 at +0x10; its low word, which every 32-bit reader
         * takes, is +0x14 -- but LBP's FMOD mixer reads the u32 at +0x10 as
         * the DMA source (it DMA'd its DSP overlay from EA 0 and jumped into
         * zeroed LS), so write both, as the lv2 syscall path already does. */
        vm_write32(seg + 0x10, src_ea + p_off);
        vm_write32(seg + 0x14, src_ea + p_off); nsegs++;
        if (p_msz > p_fsz && nsegs < 32) {                     /* BSS tail -> FILL 0 */
            seg = segs_ea + (uint32_t)nsegs * 0x18;
            vm_write32(seg + 0x00, 2); vm_write32(seg + 0x04, p_va + p_fsz);
            vm_write32(seg + 0x08, p_msz - p_fsz);
            vm_write32(seg + 0x10, 0); vm_write32(seg + 0x14, 0); nsegs++;
        }
    }
    s_seg_bump += (uint32_t)nsegs * 0x18;
    if (s_seg_bump >= 0x0E000000u) s_seg_bump = 0x0D000000u;
    /* Remember where this image's loadable bytes live, for the SPU-side
     * recovery in spu_dma.h (SPU_DSP_IMAGE_EA). The segments are contiguous in
     * the source image, so segment 0's address plus the span from its ls_start
     * to the end of the last segment describes the whole thing. */
    if (nsegs > 0) {
        uint32_t last = segs_ea + (uint32_t)(nsegs - 1) * 0x18;
        g_spu_image_src_ea   = vm_read32(segs_ea + 0x14);
        g_spu_image_ls_start = vm_read32(segs_ea + 0x04);
        g_spu_image_span     = vm_read32(last + 0x04) - g_spu_image_ls_start;
    }
    /* Remember which ELF this descriptor was parsed from. sys_spu_thread_group_
     * start only gets the DESCRIPTOR, but the workload registry is keyed by a
     * fingerprint of the ELF's own bytes -- without this the raw SPU path has no
     * way to ask whether the image it is about to run was lifted. */
    ps3_spu_image_record(img_ea, src_ea);
    /* ...and ask that question now, while the ELF is still identified. A raw SPU
     * is started by an MMIO store to its run-control register, with no syscall in
     * between, so this import is the last point at which the image can be matched
     * to a lifted entry (runtime/spu/spu_raw.c). No-op for a SPU-thread image. */
    spu_raw_note_image(src_ea, entry);
    vm_write32(img_ea + 0x00, 0);                              /* type = USER */
    vm_write32(img_ea + 0x04, entry);
    vm_write32(img_ea + 0x08, nsegs ? segs_ea : 0);
    vm_write32(img_ea + 0x0C, (uint32_t)nsegs);
    fprintf(stderr, "[HLE] _sys_spu_image_import -> entry=0x%05X nsegs=%d machine=%u (SPU=23)\n",
            entry, nsegs, machine);
    /* SPU_IMAGE_DIAG=1: r6 is a caller-supplied guest buffer that differs on
     * every call, which this handler ignores -- it points the segments at the
     * source image instead. Show the segments written and what the caller's
     * buffer holds, to establish whether the caller expects it to be filled. */
    if (getenv("SPU_IMAGE_DIAG")) {
        for (int i = 0; i < nsegs; i++) {
            uint32_t s = segs_ea + (uint32_t)i * 0x18;
            fprintf(stderr, "        seg[%d] type=%u ls=0x%05X size=%u src=0x%08X%08X\n",
                    i, vm_read32(s + 0x00), vm_read32(s + 0x04), vm_read32(s + 0x08),
                    vm_read32(s + 0x10), vm_read32(s + 0x14));
        }
        uint32_t r6 = (uint32_t)ctx->gpr[6];
        if (r6 && r6 < 0xD0000000u) {
            /* Scan a window around the caller's buffer for the DSP descriptor
             * the SPU later reads: its first word is 0x0052E1E8 and it carries
             * the image's {entry, size, ls_start, size} at +0x150. Finding it
             * at a fixed offset says r6 IS that descriptor. */
            for (int32_t o = -0x400; o <= 0x400; o += 4) {
                uint32_t a = (uint32_t)((int32_t)r6 + o);
                if (vm_read32(a) == 0x0052E1E8u)
                    fprintf(stderr, "        descriptor marker 0x0052E1E8 at r6%+d (0x%08X)\n", o, a);
                if (vm_read32(a) == 0x00000248u && vm_read32(a + 4) == 0x00002CB0u)
                    fprintf(stderr, "        image quad {248,2CB0,80,2CB0} at r6%+d (0x%08X) "
                            "=> descriptor+0x150, descriptor=0x%08X, its +0x140=%08X %08X %08X %08X\n",
                            o, a, a - 0x150,
                            vm_read32(a - 0x10), vm_read32(a - 0xC),
                            vm_read32(a - 8), vm_read32(a - 4));
            }
        }
    }
    fflush(stderr);
    ctx->gpr[3] = 0;
}

/* ---- sys_spinlock_* (sysPrxForUser) -------------------------------------
 *
 * A sys_spinlock_t is one 32-bit word in guest memory and nothing else -- the
 * real firmware spins on it with lwarx/stwcx. The guest only ever touches that
 * word through these four calls, so a host test-and-set on the same address is
 * exactly equivalent and stays correct across the title's own threads.
 *
 * Stored big-endian, so a guest that peeks at the word still reads 1.
 *
 * ABI: initialize / lock / unlock return void; trylock returns CELL_OK or
 * EBUSY. Found via Virtua Fighter 5, which locks with trylock in a retry loop
 * and was the first title to import the family -- the three NIDs were unnamed
 * in the database until compute_nid() was brute-forced over the sysPrxForUser
 * export list.
 */
#define SPINLOCK_BE1  0x01000000u          /* be32(1) */

static inline volatile LONG* spin_word(uint32_t ea)
{
    /* LONG stays 32-bit on LP64 hosts, unlike native long. */
    return (volatile LONG*)(vm_base + ea);
}

/* Test-and-set, not compare-and-swap: writing "locked" over an already-locked
 * word is idempotent, so a plain atomic exchange is enough -- and _Interlocked-
 * Exchange is the one RMW the POSIX shim in win32_compat.h already provides. */
static inline int spin_try(uint32_t ea)
{
    return _InterlockedExchange(spin_word(ea), (LONG)SPINLOCK_BE1) == 0;
}

static void sys_spinlock_initialize(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3];
    if (ea) _InterlockedExchange(spin_word(ea), 0);
}

/* Who currently holds each locked word, so a spin that never ends can name the
 * holder instead of just burning a core in silence.
 *
 * A spinlock has no timeout and no failure return, so a lock that is never
 * released is invisible: the thread sits in Sleep(0) at 100% of a core, a
 * sampling profiler attributes every sample to ntdll, and NOTHING says which
 * address or which holder. Guitar Hero III deadlocks exactly this way during
 * its heap bring-up and presented as "all threads idle in ntdll" while pinning
 * a core -- two facts that only make sense together once the lock is named.
 *
 * Small fixed table, linear scan: contention here is rare by construction (a
 * guest spinlock section is a few instructions), and a miss just means the
 * report says "unknown" rather than being wrong. */
#define SPIN_OWNER_MAX 64
static struct { uint32_t ea; unsigned long tid; } s_spin_owner[SPIN_OWNER_MAX];

static void spin_note_acquire(uint32_t ea)
{
    for (int i = 0; i < SPIN_OWNER_MAX; i++)
        if (s_spin_owner[i].ea == 0 || s_spin_owner[i].ea == ea) {
            s_spin_owner[i].ea = ea; s_spin_owner[i].tid = GetCurrentThreadId(); return;
        }
}
static void spin_note_release(uint32_t ea)
{
    for (int i = 0; i < SPIN_OWNER_MAX; i++)
        if (s_spin_owner[i].ea == ea) { s_spin_owner[i].ea = 0; s_spin_owner[i].tid = 0; return; }
}
static unsigned long spin_owner_of(uint32_t ea)
{
    for (int i = 0; i < SPIN_OWNER_MAX; i++)
        if (s_spin_owner[i].ea == ea) return s_spin_owner[i].tid;
    return 0;
}

static void sys_spinlock_lock(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3];
    if (!ea) return;
    /* ponytail: spin briefly, then hand the core back. On real hardware the
     * holder runs to completion in a few instructions; here it is a host thread
     * the OS can deschedule mid-section, so busy-waiting a whole quantum is the
     * wrong trade. Swap in a futex if a title ever shows real contention here. */
    for (unsigned n = 0; !spin_try(ea); n++) {
        if (n < 64) YieldProcessor();
        else        Sleep(0);
        /* Report once per stuck lock, well past any legitimate section. */
        if (n == 200000) {
            unsigned long owner = spin_owner_of(ea);
            unsigned long me    = GetCurrentThreadId();
            static int reported = 0;
            if (PS3_LOG_FIRST(reported, 8))
                fprintf(stderr,
                        "[spinlock] STUCK on 0x%08X after 200k spins: guest lr=0x%08X, "
                        "held by tid %lu, this is tid %lu%s\n",
                        ea, (uint32_t)ctx->lr, owner, me,
                        (owner && owner == me)
                          ? "  <== SELF-DEADLOCK: this thread already holds it"
                          : (owner ? "" : "  (holder unknown -- released without us seeing it?)"));
        }
    }
    spin_note_acquire(ea);
}

static void sys_spinlock_trylock(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3];
    int got = (ea && spin_try(ea));
    if (got) spin_note_acquire(ea);
    ctx->gpr[3] = got
                ? 0                                            /* CELL_OK */
                : (uint64_t)(int64_t)(int32_t)0x8001000Au;     /* EBUSY   */
}

static void sys_spinlock_unlock(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3];
    if (!ea) return;
    spin_note_release(ea);
    _InterlockedExchange(spin_word(ea), 0);
}

extern "C" void ppu_sysprx_register(void)
{
    ps3_hle_register_ctx(0x15BAE46Bu, "_cellGcmInitBody", hle_cellGcmInitBody);
    ps3_hle_register_ctx(0xEBE5F72Fu, "_sys_spu_image_import", hle_sys_spu_image_import);

    /* PS3_NET_ONLINE: real host sockets (libs/network/sysNet.c). Registered
     * first because the first registration of a NID wins, so these shadow the
     * offline model below; without the variable nothing changes. */
    if (getenv("PS3_NET_ONLINE")) ps3_net_host_register(gcm_guest_alloc);
    /* sceNpScore ranking calls take up to 15 arguments: the ones past r10
     * come off the stack, so they need the full context. */
    np_score_register_ctx();
    /* cellSaveDataUserAutoLoad/UserAutoSave take nine arguments; the ninth
     * (userdata) is on the guest stack, so they need the full context too.
     * Registered here so no project has to remember the call. */
    cellSaveData_register_ctx_handlers();
    /* cellVideoOutSetGamma's float argument is in f1. */
    avconf_register_ctx();
    /* sys_net offline model (NIDs from PSL1GHT libnet exports). Covers every
     * sys_net NID LBP imports so none fall to the unresolved-NID default. */
    ps3_hle_register_ctx(0x6005CDE1u, "_sys_net_errno_loc",     hle_net_errno_loc);
    ps3_hle_register_ctx(0x1F953B9Fu, "sys_net_bnet_recvfrom",  hle_net_wouldblock);
    ps3_hle_register_ctx(0xFBA04F37u, "sys_net_bnet_recv",      hle_net_wouldblock);
    ps3_hle_register_ctx(0xC9D09C34u, "sys_net_bnet_recvmsg",   hle_net_wouldblock);
    ps3_hle_register_ctx(0x051EE3EEu, "sys_net_bnet_poll",      hle_net_poll);
    ps3_hle_register_ctx(0x3F09E20Au, "sys_net_bnet_select",    hle_net_select);
    ps3_hle_register_ctx(0x139A9E9Bu, "netInitializeNetworkEx", hle_net_zero);   /* lib init ok */
    ps3_hle_register_ctx(0x9C056962u, "netSocket",              hle_net_socket);
    ps3_hle_register_ctx(0xB0A59804u, "netBind",                hle_net_zero);
    ps3_hle_register_ctx(0x88F03575u, "netSetSockOpt",          hle_net_zero);
    ps3_hle_register_ctx(0x9647570Bu, "netSendTo",              hle_net_sendto);
    ps3_hle_register_ctx(0x6DB6E8CDu, "netClose",               hle_net_zero);
    ps3_hle_register_ctx(0x71F4C717u, "netGetHostByName",       hle_net_zero);   /* NULL: DNS down */
    ps3_hle_register_ctx(0xB68D5625u, "netFinalizeNetwork",     hle_net_zero);
    ps3_hle_register_ctx(0xFDB8F926u, "netFreethreadContext",   hle_net_zero);
    /* Route the GCM command-buffer-full callback (invoked indirectly via the
     * context OPD) into cellGcm_fifo_recycle so the FIFO ring recycles on wrap. */
    ppu_register_function(GCM_FIFO_CALLBACK_SENTINEL_EA, hle_gcm_callback);
    ps3_hle_register_ctx(ps3_compute_nid("sys_initialize_tls"),       "sys_initialize_tls",       sys_initialize_tls);
    ps3_hle_register_ctx(ps3_compute_nid("sys_time_get_system_time"), "sys_time_get_system_time", sys_time_get_system_time);
    ps3_hle_register_ctx(ps3_compute_nid("sys_process_is_stack"),     "sys_process_is_stack",     sys_process_is_stack);
    /* Atexit registration: nothing to do at boot, just succeed. */
    ps3_hle_register_ctx(ps3_compute_nid("_sys_process_atexitspawn"), "_sys_process_atexitspawn", crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("_sys_process_at_Exitspawn"),"_sys_process_at_Exitspawn",crt_ok);

    /* Lightweight mutex and condition variable (sys_lwsync.cpp). */
    ps3_lwsync_register();

    /* Spinlocks. One guest word each, no kernel object; see the block above. */
    ps3_hle_register_ctx(ps3_compute_nid("sys_spinlock_initialize"), "sys_spinlock_initialize", sys_spinlock_initialize);
    ps3_hle_register_ctx(ps3_compute_nid("sys_spinlock_lock"),       "sys_spinlock_lock",       sys_spinlock_lock);
    ps3_hle_register_ctx(ps3_compute_nid("sys_spinlock_trylock"),    "sys_spinlock_trylock",    sys_spinlock_trylock);
    ps3_hle_register_ctx(ps3_compute_nid("sys_spinlock_unlock"),     "sys_spinlock_unlock",     sys_spinlock_unlock);

    /* Thread id + memory manager (high-frequency boot imports). The flat VM
     * means map/unmap/free are no-ops: the memory already exists everywhere. */
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_get_id"),      "sys_ppu_thread_get_id",      sys_ppu_thread_get_id);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_create"),      "sys_ppu_thread_create",      hle_ppu_thread_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_exit"),        "sys_ppu_thread_exit",        hle_ppu_thread_exit);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_allocate_memory"), "sys_mmapper_allocate_memory", sys_mmapper_allocate_memory);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_allocate_memory_from_container"), "sys_mmapper_allocate_memory_from_container", sys_mmapper_allocate_memory_from_container);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_map_memory"),     "sys_mmapper_map_memory",     crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_unmap_memory"),   "sys_mmapper_unmap_memory",   crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_free_memory"),    "sys_mmapper_free_memory",    crt_ok);
}

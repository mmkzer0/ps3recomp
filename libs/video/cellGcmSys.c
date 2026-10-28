/*
 * ps3recomp - cellGcmSys HLE module implementation
 *
 * Manages RSX initialization, display buffer registration, flip control,
 * command buffer control, tile/zcull configuration, IO memory mapping,
 * report/label areas, and address-to-offset translation.
 *
 * Actual rendering is handled elsewhere -- this module just tracks state.
 */

#include "rsx_live_draw.h"
#include "rsx_draw_engine.h"
#include "cellGcmSys.h"
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write32 (translate + byte-swap, OOB-safe) */
#include "../../runtime/memory/vm.h"    /* VM_HLE_INJECT_BASE */
#include "../../runtime/memory/label_window.h"  /* the store window, shared with the SPU MFC path */
#include "../../runtime/spu/spu_coherency.h" /* the blit row copy's reservation bookkeeping */
#include "rsx_commands.h"                    /* rsx_state, rsx_process_command_buffer */
#include "../../include/ps3emu/guest_call.h" /* g_ps3_guest_caller for direct handler dispatch */
#include "../../include/ps3emu/ps3_ftrace.h"  /* PS3_FTRACE timeline */

/* Guest EA of the GCM context (begin/end/current/callback) the title writes its
 * command stream into; recorded by cellGcmSetupContext, drained by the RSX. */
static u32 s_gcm_context_ea = 0;
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "ps3emu/log_first.h"
#include "ps3emu/stop.h"
#include "ps3emu/config.h"        /* gcm.wait_timeout_ms */
#include "gcm_default_state.h"
#include "rsx_capture.h"
#include "rsx_method_class.h"
#include <stdatomic.h>

/* The RSX capture (rsx_capture.cpp), when one is open: NULL otherwise, so a run
 * with capture off costs one pointer test per hook. It is registered before the
 * walker's first pass and cleared only after its last, never in between. */
static const rsx_capture_sink* s_cap;
void cellGcm_set_capture_sink(const rsx_capture_sink* sink) { s_cap = sink; }
/* Set while cellGcmInit rebuilds the base IO mapping: the replay does that
 * itself when it replays gcm-init, so it is not a separate event. */
static int s_cap_quiet;

/* The feed rsx_replay drives through cellGcm_rsx_replay_step: while one recorded step runs, the walker stops
 * where that step left it, and a GET_REPORT takes the timestamp the capture recorded, because the report's
 * timer is the host clock and no replay can reproduce it. */
static struct { int on; u32 end; const u64* ts; u32 n_ts, used_ts; } s_replay;

/* ---------------------------------------------------------------------------
 * Object bindings and method verdicts (rsx_method_class.h)
 * -----------------------------------------------------------------------*/

/* SET_OBJECT per subchannel: the raw handle and the class it resolves to. cellGcmInit clears them: that
 * is the clean reset a capture starts from, and the FIFO itself resets no binding. */
static u32 s_subch_handle[8];
static u8  s_subch_class[8];

static void gcm_bind_object(u32 subch, u32 handle)
{
    s_subch_handle[subch & 7u] = handle;
    s_subch_class[subch & 7u]  = (u8)rsx_class_from_handle(handle);
}

/* The class that interprets a method word. Channel control (NV406E, methods 1..0xFC) is the puller's: it
 * decodes on every subchannel, bound or not. Everything else means what the object bound to the slot
 * says, and nothing when none is. */
static rsx_class_id gcm_method_class(u32 subch, u32 m)
{
    if (m && m < 0x100u) return RSX_CLASS_NV406E;
    return (rsx_class_id)s_subch_class[subch & 7u];
}

/* The driver's own words, decoded by their full address (LV1's, not an engine's; RPCS3 does the same):
 * GCM_SET_USER_COMMAND 0xEB00/0xEB04, the flip queue 0xE944 and the flip head 0xE920/0xE924. Slot 7 is the
 * driver's, and these are taken before any class route: whatever object a slot carries, they mean the same. */
static int gcm_driver_word(u32 subch, u32 m)
{
    const u32 mfull = (subch << 13) | m;
    return mfull == 0xEB00u || mfull == 0xEB04u || mfull == 0xE944u || mfull == 0xE920u || mfull == 0xE924u;
}

/* Whether the 3D register-file mirrors (the draw engine, the live-draw engine) take this word. Both keep
 * one NV4097 register file, so a word from another class would be stored as an NV4097 register: NV308A's
 * 0x018C/0x0194/0x0198 are SET_CONTEXT_DMA_COLOR1/COLOR0/ZETA there, and a 2D write to them silently moved
 * the colour and depth surfaces to RSX local memory. They take methods of a slot bound to NV4097, and the
 * driver's words. */
static int gcm_feeds_3d_mirror(u32 subch, u32 m)
{
    if (gcm_driver_word(subch, m)) return 1;
    return m >= 0x100u && s_subch_class[subch & 7u] == RSX_CLASS_NV4097;
}

/* The address the mirrors get: a driver word keeps its full address (the engine reads 0xE9xx by it), any
 * other word is an NV4097 method and goes by its own number, whatever slot NV4097 is bound to; with the
 * subchannel left in, NV4097 on slot 7 would reach the engine's 0x1808 begin/end as 0xF808. */
static u32 gcm_mirror_method(u32 subch, u32 m)
{
    return gcm_driver_word(subch, m) ? ((subch << 13) | m) : m;
}

/* The NV0039 (M2MF) object's registers. One object, whatever subchannel it is bound to. */
static struct {
    u32 dma_in, dma_out, off_in, off_out, pitch_in, pitch_out, line_len, line_count, format;
} s_m2mf;
static _Atomic unsigned long long s_m2mf_copies;

/* Every method the walker consumes ends in one verdict (rsx_method_class.h). Unknown ones are counted
 * per (class, method); the first GCM_UNKNOWN_LOG_MAX distinct pairs are logged; a capture marks the
 * frame invalid; and the live walker STOPS (`rsx-unknown-method`, M3-SCOPE Amendment 1 finding 5,
 * switched on by slice S1). Missing states (a real state this renderer ignores, with an S2 row) are
 * counted and logged too, but they do not stop: they are known, documented gaps, not unexplained ones.
 * The counters are read from another thread (a title printing them at exit), so they are relaxed
 * atomics; the first-seen subchannel and data word are diagnostics and need no order. */
#define GCM_UNKNOWN_LOG_MAX 16u
#define GCM_MISSING_LOG_MAX 16u
static _Atomic unsigned long long s_mc_handled, s_mc_inert, s_mc_unknown, s_mc_missing, s_mc_unbound;
static struct {
    _Atomic unsigned long long count;
    u32 data;
    u8  subch;
} s_mc_tab[RSX_CLASS_COUNT][0x800];
static struct {
    _Atomic unsigned long long count;
    u32 data;
    u8  subch;
} s_miss_tab[RSX_CLASS_COUNT][0x800];
static unsigned s_mc_logged, s_miss_logged;

/* `handled` says a consumer took the method. Otherwise the (class, method) pair is looked up in the
 * table (rsx_method_class.h): INERT when the write carries the value the row pins, MISSING when the
 * row is an S2 gap or the write differs from the pinned value, and UNKNOWN when no row covers the pair
 * at all -- the only case that stops. `fifo` is the IO offset of the packet header, `word` its data
 * word: the two positions that let an operator find the command again in a capture or a log. */
static void gcm_method_verdict(u32 subch, u32 m, u32 data, int handled, u32 fifo, u32 word)
{
    const rsx_class_id cls = gcm_method_class(subch, m);
    if (handled) { atomic_fetch_add_explicit(&s_mc_handled, 1, memory_order_relaxed); return; }
    const rsx_method_rule* rule = NULL;
    const rsx_method_verdict v = rsx_method_check(cls, m, data, &rule);
    const u32 idx = (m >> 2) & 0x7FFu;
    if (v == RSX_METHOD_HANDLED) {
        /* The row names a consumer that is not on this configuration's path (the register-file engine
         * while it is disabled). The pair is classified, not unexplained, so it is not unknown. */
        atomic_fetch_add_explicit(&s_mc_handled, 1, memory_order_relaxed);
        return;
    }
    if (v == RSX_METHOD_INERT) {
        atomic_fetch_add_explicit(&s_mc_inert, 1, memory_order_relaxed);
        return;
    }
    if (v == RSX_METHOD_MISSING) {
        atomic_fetch_add_explicit(&s_mc_missing, 1, memory_order_relaxed);
        if (atomic_fetch_add_explicit(&s_miss_tab[cls][idx].count, 1, memory_order_relaxed) == 0) {
            s_miss_tab[cls][idx].data = data;
            s_miss_tab[cls][idx].subch = (u8)subch;
            if (s_miss_logged < GCM_MISSING_LOG_MAX) {
                s_miss_logged++;
                fprintf(stderr, "[rsx-method] missing %s method 0x%04X = 0x%08X (%s; subchannel %u, fifo 0x%X)\n",
                        rsx_class_name(cls), m, data, rule ? rule->name : "no row", subch, fifo);
            }
        }
        return;
    }
    /* Unknown: no consumer, no allow-list row that covers this value. */
    atomic_fetch_add_explicit(&s_mc_unknown, 1, memory_order_relaxed);
    if (atomic_fetch_add_explicit(&s_mc_tab[cls][idx].count, 1, memory_order_relaxed) == 0) {
        s_mc_tab[cls][idx].data = data;
        s_mc_tab[cls][idx].subch = (u8)subch;
        if (s_mc_logged < GCM_UNKNOWN_LOG_MAX) {
            s_mc_logged++;
            fprintf(stderr, "[rsx-method] unknown %s method 0x%04X (subchannel %u, data 0x%08X)\n",
                    rsx_class_name(cls), m, subch, data);
        }
    }
    if (s_cap) s_cap->note(RSX_CAP_NOTE_UNKNOWN_METHOD, subch, m, data);
    ps3_stop_rsx_unknown_method(rsx_class_name(cls), m, subch, fifo, word, data);
}

/* A method on a slot with no object bound reaches no engine: PFIFO has no class to decode it against
 * (envytools fifo/nv_object.xml: the subchannel's object table is what gives a method its meaning), so
 * the console drops the write. Dropping it here is the faithful model; counting it keeps the census
 * honest about writes that never become state. An UNREGISTERED handle is not dropped: this runtime does
 * not know that class's methods, so the method stays unknown and stops. */
static void gcm_method_unbound(u32 subch, u32 m, u32 data)
{
    atomic_fetch_add_explicit(&s_mc_unbound, 1, memory_order_relaxed);
    { static int n = 0;
      if (PS3_LOG_FIRST(n, 8))
          fprintf(stderr, "[rsx-method] dropped unbound method 0x%04X = 0x%08X (subchannel %u)\n",
                  m, data, subch); }
}

void gcm_method_stats(GcmMethodStats* out)
{
    if (!out) return;
    out->handled = atomic_load_explicit(&s_mc_handled, memory_order_relaxed);
    out->inert   = atomic_load_explicit(&s_mc_inert, memory_order_relaxed);
    out->missing = atomic_load_explicit(&s_mc_missing, memory_order_relaxed);
    out->unknown = atomic_load_explicit(&s_mc_unknown, memory_order_relaxed);
    out->unbound = atomic_load_explicit(&s_mc_unbound, memory_order_relaxed);
    out->m2mf_copies = atomic_load_explicit(&s_m2mf_copies, memory_order_relaxed);
}

u32 gcm_unknown_methods(GcmUnknownMethod* out, u32 max)
{
    u32 n = 0;
    for (u32 c = 0; c < RSX_CLASS_COUNT; c++)
        for (u32 i = 0; i < 0x800u; i++) {
            const unsigned long long count = atomic_load_explicit(&s_mc_tab[c][i].count, memory_order_relaxed);
            if (!count) continue;
            if (out && n < max) {
                out[n].cls = c; out[n].method = i << 2; out[n].count = count;
                out[n].subch = s_mc_tab[c][i].subch; out[n].data = s_mc_tab[c][i].data;
            }
            n++;
        }
    return n;
}

u32 gcm_missing_methods(GcmUnknownMethod* out, u32 max)
{
    u32 n = 0;
    for (u32 c = 0; c < RSX_CLASS_COUNT; c++)
        for (u32 i = 0; i < 0x800u; i++) {
            const unsigned long long count = atomic_load_explicit(&s_miss_tab[c][i].count, memory_order_relaxed);
            if (!count) continue;
            if (out && n < max) {
                out[n].cls = c; out[n].method = i << 2; out[n].count = count;
                out[n].subch = s_miss_tab[c][i].subch; out[n].data = s_miss_tab[c][i].data;
            }
            n++;
        }
    return n;
}

void gcm_method_stats_reset(void)
{
    atomic_store_explicit(&s_mc_handled, 0, memory_order_relaxed);
    atomic_store_explicit(&s_mc_inert, 0, memory_order_relaxed);
    atomic_store_explicit(&s_mc_missing, 0, memory_order_relaxed);
    atomic_store_explicit(&s_mc_unknown, 0, memory_order_relaxed);
    atomic_store_explicit(&s_mc_unbound, 0, memory_order_relaxed);
    atomic_store_explicit(&s_m2mf_copies, 0, memory_order_relaxed);
    for (u32 c = 0; c < RSX_CLASS_COUNT; c++)
        for (u32 i = 0; i < 0x800u; i++) {
            atomic_store_explicit(&s_mc_tab[c][i].count, 0, memory_order_relaxed);
            atomic_store_explicit(&s_miss_tab[c][i].count, 0, memory_order_relaxed);
        }
}

const char* gcm_method_class_name(u32 cls) { return rsx_class_name((rsx_class_id)cls); }

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* ---------------------------------------------------------------------------
 * Timestamp helpers (same pattern as sys_timer)
 * -----------------------------------------------------------------------*/

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

static u64 get_timestamp_ns(void)
{
    LARGE_INTEGER now;
    ensure_qpc_init();
    QueryPerformanceCounter(&now);
    /* Convert to nanoseconds: (count * 1e9) / freq */
    return (u64)((double)now.QuadPart * 1000000000.0 / (double)s_qpc_freq.QuadPart);
}
#else
static u64 get_timestamp_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_gcm_initialized = 0;
static u32  s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
static u32  s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
/* Flip requests (each puts the status back to WAITING) and the request a
 * pending flip completion was marked for: a completion delivered late must not
 * mark DONE a flip requested after it (see ppu_gcm_pump). */
static volatile u32 s_flip_req_seq, s_flip_tick_seq;
/* The status's read-modify-write transitions (request, tick, completion) run
 * on different threads; without a lock a completion could read "no newer
 * request" just before a request stored WAITING and then overwrite it. */
#ifdef _WIN32
static volatile LONG s_flip_lock;
#define FLIP_LOCK()   do { while (InterlockedCompareExchange(&s_flip_lock, 1, 0)) YieldProcessor(); } while (0)
#define FLIP_UNLOCK() InterlockedExchange(&s_flip_lock, 0)
#else
static volatile int s_flip_lock;
#define FLIP_LOCK()   do { while (__atomic_exchange_n(&s_flip_lock, 1, __ATOMIC_ACQUIRE)) {} } while (0)
#define FLIP_UNLOCK() __atomic_store_n(&s_flip_lock, 0, __ATOMIC_RELEASE)
#endif

/* Count of guest flip requests (SetFlipCommand / SetPrepareFlip). The present
 * loop uses this as the frame boundary: presenting only when a flip arrived
 * keeps partially-drained frames off screen (the ticker otherwise presents on
 * a fixed 16ms clock, and a drain that catches the guest mid-frame -- e.g.
 * while it's blocked in the FIFO-wrap recycle callback -- would show a
 * clear+few-draws frame as a visible flicker). */
static volatile u32 s_flip_request_count = 0;

u32 cellGcm_flip_request_count(void)
{
    return s_flip_request_count;
}
static u32  s_debug_level = CELL_GCM_DEBUG_LEVEL0;

/* Display buffers */
static CellGcmDisplayInfo s_display_buffers[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static int s_display_buffer_set[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static u32 s_current_display_buffer_id = 0;
/* Set by the flip (guest thread, at a get==put frame boundary); consumed by
 * the ticker, which presents the accumulated batch BEFORE draining further --
 * presenting on a raw flip-count change raced the drain and showed empty or
 * mixed batches (wave: black flashes, layout flicker). */
static volatile int s_flip_pending = 0;

/* The display buffer the last GCM_DRIVER_QUEUE (0xE944) method named; the next GCM_FLIP_HEAD
 * (0xE924, bit 31 set) presents it. */
static u32 s_drv_flip_queue = 0;
/* A flip _cellGcmSetFlipCommand queued in the FIFO: | bufferId, bit 8 = ours. */
#define GCM_FLIP_MARKER 0xFEAD0100u
/* Flips queued into the command stream by gcm_flip_into_fifo, and how many of
 * those markers the drain has reached. The RSX completes a flip when it
 * executes it; until the drain reaches the marker, the flip has not happened,
 * however many vblanks go by. */
static volatile u32 s_fifo_flips_queued = 0, s_fifo_flips_reached = 0;
static u32 s_fifo_flips_completed = 0;   /* reached count at the last completion */
/* A flip requested outside the command stream (cellGcmSetFlipCommand, or a
 * 0xFEAD word the drain met), not completed yet. */
static volatile int s_flip_direct_pending = 0;

/* May this beat complete a flip? Yes if the drain has executed a flip marker
 * since the last completion, or if a flip that never went through the stream
 * is waiting. A flip that is queued, or written but not yet flushed, does not
 * hold back the completion of one already executed: the RSX reports the flip
 * it did, not the ones still ahead of it. With no flip outstanding there is
 * nothing to complete: Twisted Metal resets the flip status, takes about a
 * second to build its next frame here, then queues the flip, and a beat in
 * between that reported a flip made it take the new one for done. It then
 * rebuilt the command segment the walker was still waiting in. */
static int gcm_flip_executed_take(void)
{
    const u32 r = s_fifo_flips_reached;
    if (r != s_fifo_flips_completed) { s_fifo_flips_completed = r; return 1; }
    if (s_flip_direct_pending) { s_flip_direct_pending = 0; return 1; }
    return 0;
}
/* ponytail: fixed headroom, far above one frame's commands (Simpsons ~10 KB);
 * derive it from the observed per-frame volume if a title outgrows it. */
#define GCM_FLIP_WRAP_BYTES 0x40000u

int cellGcm_take_flip_pending(void)
{
    int v = s_flip_pending;
    s_flip_pending = 0;
    return v;
}

/* The flip now names this buffer. The shared draw engine presents on the
 * host's call and only learns a buffer from 0xE944 by itself, so it would
 * keep presenting display buffer 0 for a title that flips through 0xFEAD. */
static void gcm_set_current_display_buffer(u32 bufferId)
{
    s_current_display_buffer_id = bufferId;
    if (rsx_draw_engine_enabled())
        rsx_draw_engine_note_flip(bufferId);
}


/* The RSX puts its 3D state back to the reset values at a flip, and a title
 * may count on that: one sets ZSTENCIL_CLEAR_VALUE to 0 for a single clear
 * and never writes it again, so its later depth clears are to the far plane
 * only because every flip restored 0xFFFFFF00 (RPCS3's flip_command resets
 * the whole register file, rsx_state::reset). Here it reaches every consumer
 * of the method stream, at the flip's place in it.
 * TODO: the rest of the reset image; this is the one register a title has
 * been seen to depend on. */
static void gcm_fifo_flip_resets_state(rsx_state* state)
{
    const u32 m = NV4097_SET_ZSTENCIL_CLEAR_VALUE, v = RSX_DSP_ZSTENCIL_CLEAR_RESET;
    rsx_process_method(state, m, v);
    if (rsx_live_draw_enabled())   rsx_live_draw_method(m, v);
    if (rsx_draw_engine_enabled()) rsx_draw_engine_method(m, v);
}

/* Configuration */
static CellGcmConfig s_config;
/* Offset pages (1MB) known to be LOCAL-memory-derived (set by
 * cellGcmAddressToOffset when the guest converts a localAddress-range EA). */
static u8 s_local_offset_page[1024];

/* Command buffer control */
static CellGcmControl s_control;

/* Offset table storage
 *
 * set_offset_table/clear_offset_table keep these two host arrays in step with
 * the IO mappings, and that part was always fine. What was not: the tables
 * cellGcmGetOffsetTable hands out are indexed BY THE GUEST --
 * ioAddress[ea >> 20] and eaAddress[io >> 20] -- and it handed over 64-bit host
 * pointers through a struct whose guest form is two 4-byte fields, so the title
 * got two truncated addresses pointing at nothing it could read.
 *
 * The guest-visible copy lives in the reserved GCM window alongside the label
 * and control blocks, published on each call. 4096 pages covers the 4 GiB
 * address space at 1 MiB granularity, 8 KiB a side, and this is not a hot path.
 * 0xFFFF means "not mapped", as it does in the host tables. */
/* Default home for the HLE-visible GCM window; a port overrides it before
 * running guest code when its title claims this address range. */
uint32_t ppu_hle_inject_base = 0x20000000u;

static u16 s_io_address_table[65536];
static u16 s_ea_address_table[65536];

#define GCM_OFFSET_TABLE_PAGES   4096u                 /* 4 GiB / 1 MiB       */
#define GCM_OFFSET_TABLE_IO_ADDR (VM_HLE_INJECT_BASE + 0x3000u)           /* ea >> 20  -> io>>20 */
#define GCM_OFFSET_TABLE_EA_ADDR (VM_HLE_INJECT_BASE + 0x5000u)           /* io >> 20  -> ea>>20 */

/* Local memory bump allocator */
static u32 s_local_mem_allocated = 0;  /* next free offset in local memory */

/* IO memory mapping table */
typedef struct IoMapping {
    u32 ea;         /* effective address in main memory */
    u32 io;         /* IO offset (RSX-visible) */
    u32 size;       /* size of mapping */
    int active;     /* 1 if this slot is in use */
} IoMapping;

static IoMapping s_io_mappings[CELL_GCM_MAX_IO_MAPPINGS];
static int s_io_mapping_count = 0;
/* One past the highest slot ever handed out since the table was last cleared:
 * every active slot is below it. Slots are taken first-free, so it stays at
 * the peak number of simultaneous mappings. Lets gcm_ea2io_range(), which runs
 * on every SPU PUT, skip the never-used tail of the table. It only grows
 * between resets, so a reader racing a map or unmap elsewhere in the table
 * still reaches every mapping that stays active. */
static int s_io_mapping_hw = 0;

/* Callback handlers — these are GUEST OPD addresses passed by the
 * recompiled game, not host function pointers. Stored as uint32_t and
 * invoked through g_ps3_guest_caller (see ps3emu/guest_call.h). */

/* Guest address of the RsxDriverInfo the lv2 side handed out (sys_rsx_context_allocate), or 0 while the
 * runner has not published one. The handler mask lives in it at +0x12C0 (gcm_handler_word); with no
 * driver info the setters keep only their host slots. The runner sets this before cellGcmInit, as the
 * firmware has the driver info in hand before its init body runs (dossier §2: the bootstrap's start
 * value goes into the word after the context exists). It must be 4-aligned and inside guest memory. */
uint32_t g_gcm_driver_info_ea = 0;

static u32 s_flip_handler_opd     = 0;

/* GCM_FLIPCB_ONTICK=1: fire the guest flip handler only when the flip
 * completes (cellGcmTickFlip), not also when it is requested. */
static int gcm_flipcb_on_tick_only(void)
{
    static int v = -1;
    if (v < 0) v = getenv("GCM_FLIPCB_ONTICK") ? 1 : 0;
    return v;
}

static u32 s_vblank_handler_opd   = 0;
static u32 s_user_handler_opd     = 0;
static u32 s_second_v_handler_opd = 0;
/* YDKJ_HANDLERFIX: the handler OPD's code word (*(opd)) gets clobbered to 0 by a
 * mislifted store, so g_ps3_guest_caller reads code=0 -> "not registered" and the
 * game's per-flip/vblank callback never fires -> render/advance loop stalls. We
 * capture the code at Set*Handler time (OPD still valid) and restore the OPD word
 * before each invocation if it's been clobbered. */
extern u32 vm_read32(unsigned int);
static u32 s_flip_handler_code    = 0;
static u32 s_vblank_handler_code  = 0;
static void ydkj_restore_handler_opd(u32 opd, u32 code) {
    static int fix = -1; if (fix < 0) fix = getenv("YDKJ_HANDLERFIX") ? 1 : 0;
    if (!fix || !opd || !code) return;
    extern u8* vm_base; if (!vm_base) return;
    if (vm_read32(opd) == 0) {
        u8* p = vm_base + opd; p[0]=(u8)(code>>24); p[1]=(u8)(code>>16); p[2]=(u8)(code>>8); p[3]=(u8)code;
        static int _n=0; if(PS3_LOG_FIRST(_n, 6)) fprintf(stderr,"[HANDLERFIX] restored clobbered OPD 0x%08X code=0x%08X\n",opd,code);
    }
}

/* The handler mask word, driver_info + 0x12C0, as FW 4.93 libgcm_sys keeps it (dossier
 * GCM-CALLBACK-OPD-CONTRACT.md §1, §2, §6 T6).
 *
 * Each Set*Handler does a read-modify-write of ITS OWN bit: set for a handler, cleared for NULL
 * (rlwinm sequences at the setters, vblank 0x020080B4, flip 0x02007F98, second vblank 0x02007FD0, queue
 * 0x02007DD0, user 0x02007D58). Every other bit is left alone, so a word the HLE did not build alone
 * (the bootstrap's start value, a bit the driver side owns) survives. The old rebuild from three slots
 * destroyed those and used the wrong bits (flip 0x4 is the firmware's GRAPHICS bit).
 *
 * MODE. The firmware picks the slot, and so the bit, by a config word `mode` (setter semantics, §1):
 * 0 takes the first of a pair (vblank 0x1, flip 0x8, second vblank 0x400, queue 0x20), 1-2 the second
 * (vblank 0x2, flip 0x10, second vblank 0x800, queue 0x40); the user handler has one slot and bit 0x80
 * either way. The firmware leaves open which slot a setter fills (the config word is not traced; the
 * dispatcher, §2, treats both slots alike and passes r3 = 0 for the first, 1 for the second). Mode 1-2 is
 * modelled, on this evidence: (1) RPCS3 at the pin raises only the head-1 bits, 0x2 for vblank and 0x10
 * for flip (dossier §3, `FLIP_BASE << 1`: "we only ever use head 1"); (2) ppu_gcm_pump delivers the
 * completed flip with r3 = 1, the dispatcher's 0x10 / state+0x1C path; (3) the vblank bit stays the 0x2 the
 * HLE always published. What would overturn it: a firmware trace of the config mode word (`*(*(TOC-0x7FF0)
 * + 8)`, helper 0x02001090) reading 0 on a console that delivers these events, which would put the title's
 * handlers in the first slots (0x1, 0x8, 0x400, 0x20). No setter sets graphics 0x4; it is only the start
 * value. */
#define GCM_DI_HANDLERS       0x12C0u
#define GCM_HBIT_VBLANK       0x00000002u
#define GCM_HBIT_FLIP         0x00000010u
#define GCM_HBIT_SECOND_V     0x00000800u
#define GCM_HBIT_QUEUE        0x00000040u
#define GCM_HBIT_USER         0x00000080u
/* The bootstrap (0x02008194), once the interrupt thread exists, stores 4 into the word (`li r0,4; stw`,
 * 0x02008240/0x02008244): a plain store, whatever the word held. */
#define GCM_HANDLER_START     0x00000004u

/* The word is guest memory, so big endian. The firmware's lwz/stw give no ordering and no atomicity beyond
 * the word itself; the host RMW is a CAS loop on that one 32-bit cell with the mask byte-swapped, relaxed,
 * so two registrations racing keep both bits and nothing more is claimed to the guest. The runner owns
 * g_gcm_driver_info_ea: 4-aligned and inside guest memory. */
static _Atomic u32* gcm_handler_word(void)
{
    if (!g_gcm_driver_info_ea) return NULL;
    return (_Atomic u32*)(vm_base + g_gcm_driver_info_ea + GCM_DI_HANDLERS);
}

static void gcm_handler_bit(u32 bit, int on)
{
    _Atomic u32* w = gcm_handler_word();
    if (!w) return;
    const u32 be = ps3_bswap32(bit);
    u32 old = atomic_load_explicit(w, memory_order_relaxed), cur;
    do cur = on ? (old | be) : (old & ~be);
    while (!atomic_compare_exchange_weak_explicit(w, &old, cur,
                                                  memory_order_relaxed, memory_order_relaxed));
    PS3_WW_CHECK(g_gcm_driver_info_ea + GCM_DI_HANDLERS, ps3_bswap32(cur), 4);
}

static void gcm_handler_word_start(void)
{
    _Atomic u32* w = gcm_handler_word();
    if (!w) return;
    atomic_store_explicit(w, ps3_bswap32(GCM_HANDLER_START), memory_order_relaxed);
    PS3_WW_CHECK(g_gcm_driver_info_ea + GCM_DI_HANDLERS, GCM_HANDLER_START, 4);
}

/* Legacy host-typed slots kept around for any caller still treating
 * these as host pointers. New code should use the _opd slots. */
static CellGcmFlipHandler    s_flip_handler    = NULL;
static CellGcmVBlankHandler  s_vblank_handler  = NULL;
static CellGcmUserHandler    s_user_handler    = NULL;
static CellGcmSecondVHandler s_second_v_handler = NULL;

/* Flip timing */
static u64 s_last_flip_time = 0;

/* VBlank counter (incremented on each flip as approximation) */
static u32 s_vblank_count = 0;

/* IO map reservation tracking */
static u32 s_io_map_reserved = 0;  /* bytes reserved for IO mapping */

/* Default FIFO mode */
static s32 s_default_fifo_mode = 0;

/* Graphics / queue handlers */
static CellGcmGraphicsHandler s_graphics_handler = NULL;
static CellGcmQueueHandler    s_queue_handler    = NULL;

/* Second V / VBlank frequency */
static u32 s_second_v_frequency = 0;
static u32 s_vblank_frequency   = 0;

/* User command */
#include <stdatomic.h>
#ifdef _WIN32
static volatile LONG s_user_command = 0;
#define GCM_USER_STORE(cmd) InterlockedExchange(&s_user_command, (LONG)(cmd))
#else
static atomic_uint s_user_command = 0;
#define GCM_USER_STORE(cmd) atomic_store(&s_user_command, (cmd))
#endif

/* The ordered user-command queue (AM-27). GCM_SET_USER_COMMAND (0xEB00) raises an interrupt
 * per method; the driver's cause register would coalesce two raised before the handler runs, but
 * a title's handler counts them (the semaphore its flip path blocks on is posted once per
 * command), so a coalesced command parks the title. Every command is therefore kept, in the
 * order the RSX retired it. Bounded multi-producer / single-consumer ring (Vyukov): a cell's
 * `turn` is 2*lap while it is free for that lap's producer and 2*lap+1 once the command is
 * written, so the zero-initialised array is a valid empty ring and no lock is taken. A full
 * ring drops the command and counts it; `gcm_user_ring_drops` makes the loss observable. */
#define GCM_USER_RING_SIZE 1024u
typedef struct GcmUserCell { atomic_uint turn; u32 cmd; } GcmUserCell;
static GcmUserCell s_user_ring[GCM_USER_RING_SIZE];
static atomic_uint s_user_ring_tail;     /* next position to produce into */
static atomic_uint s_user_ring_head;     /* next position to consume */
static atomic_uint s_user_ring_drops;

static void gcm_user_ring_reset(void)
{
    for (u32 i = 0; i < GCM_USER_RING_SIZE; i++) {
        atomic_store(&s_user_ring[i].turn, 0u);
        s_user_ring[i].cmd = 0;
    }
    atomic_store(&s_user_ring_tail, 0u);
    atomic_store(&s_user_ring_head, 0u);
    atomic_store(&s_user_ring_drops, 0u);
}

static int gcm_user_ring_push(u32 cmd)
{
    u32 pos = atomic_load_explicit(&s_user_ring_tail, memory_order_relaxed);
    for (;;) {
        GcmUserCell* c = &s_user_ring[pos % GCM_USER_RING_SIZE];
        const u32 want = (pos / GCM_USER_RING_SIZE) * 2u;
        const int diff = (int)(atomic_load_explicit(&c->turn, memory_order_acquire) - want);
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&s_user_ring_tail, &pos, pos + 1u,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                c->cmd = cmd;
                atomic_store_explicit(&c->turn, want + 1u, memory_order_release);
                return 1;
            }
        } else if (diff < 0) {
            atomic_fetch_add(&s_user_ring_drops, 1u);     /* the consumer is a whole lap behind */
            return 0;
        } else {
            pos = atomic_load_explicit(&s_user_ring_tail, memory_order_relaxed);
        }
    }
}

/* A command is queued and not yet popped (a producer may still be mid-publish: the pump then finds the cell
 * not ready and the producer's own wake follows). */
static int gcm_user_ring_nonempty(void)
{
    return atomic_load_explicit(&s_user_ring_tail, memory_order_acquire) !=
           atomic_load_explicit(&s_user_ring_head, memory_order_acquire);
}

/* Single consumer (the pump, which serialises itself). 1 and *cmd set, or 0 when empty. */
static int gcm_user_ring_pop(u32* cmd)
{
    const u32 pos = atomic_load_explicit(&s_user_ring_head, memory_order_relaxed);
    GcmUserCell* c = &s_user_ring[pos % GCM_USER_RING_SIZE];
    const u32 want = (pos / GCM_USER_RING_SIZE) * 2u + 1u;
    if (atomic_load_explicit(&c->turn, memory_order_acquire) != want) return 0;
    *cmd = c->cmd;
    atomic_store_explicit(&c->turn, want + 1u, memory_order_release);
    atomic_store_explicit(&s_user_ring_head, pos + 1u, memory_order_relaxed);
    return 1;
}

/* User commands the ring has had to drop because it was full (never in a healthy run). */
u32 gcm_user_ring_drops(void)
{
    return atomic_load(&s_user_ring_drops);
}

/* Driver events, in the order the RSX retires the methods (the walker is the only producer, so
 * `seq` is a total order). C3 compares event sequences against the recorded run: one flip head
 * per frame, every user command, queue-flip before its flip head. Counted always; an observer
 * (one per process) sees each event as it retires. */
static _Atomic(GcmDriverObserver) s_driver_observer;
static atomic_ullong s_driver_event_seq;
static atomic_ullong s_driver_event_count[GCM_DRV_EVENT_KINDS];

static void gcm_driver_event(u32 kind, u32 arg)
{
    GcmDriverEvent ev;
    ev.kind = kind;
    ev.arg  = arg;
    ev.seq  = atomic_fetch_add(&s_driver_event_seq, 1ull) + 1ull;
    atomic_fetch_add(&s_driver_event_count[kind], 1ull);
    GcmDriverObserver fn = atomic_load(&s_driver_observer);
    if (fn) fn(&ev);
}

void gcm_set_driver_observer(GcmDriverObserver fn)
{
    atomic_store(&s_driver_observer, fn);
}

/* Events of `kind` retired since cellGcmInit; 0 for an unknown kind. */
unsigned long long gcm_driver_event_count(u32 kind)
{
    return kind < GCM_DRV_EVENT_KINDS ? atomic_load(&s_driver_event_count[kind]) : 0ull;
}

/* A flip the driver has been told of (0xE944) but whose queue interrupt the PPU handler has not
 * yet answered: the NV406E acquire on label 0x30 that follows it waits for exactly that answer. */
static int s_drv_queue_pending;
static unsigned long long s_acq_t0;   /* tentative: defined with the label constants below */
/* A flip head (0xE924) has retired and the driver has not yet completed the flip: the next vblank
 * writes 0 to the flip-done label (0x10), which libgcm_sys's flip stream released as 0xFFFFFFFF just
 * before the head and which cellRescSetWaitFlip's ACQUIRE waits to see 0 [INFERENCE: the label is
 * the driver's, and the recording shows it 0 between flips]. */
static volatile int s_drv_flip_label_pending;
/* The last walker pass ended where the frame behind a flip head is complete although get != put: on an
 * NV406E ACQUIRE after the head (cellRescSetWaitFlip's), or right at the head. Everything ahead of
 * that point has executed, so the flip is ready to present. */
static volatile int s_acquire_blocked;

/* New session: no event retired, no flip awaiting its handshake, no acquire standing. */
static void gcm_driver_events_reset(void)
{
    atomic_store(&s_driver_event_seq, 0ull);
    for (u32 k = 0; k < GCM_DRV_EVENT_KINDS; k++) atomic_store(&s_driver_event_count[k], 0ull);
    s_drv_queue_pending = 0;
    s_drv_flip_label_pending = 0;
    s_acquire_blocked = 0;
    s_acq_t0 = 0;
}

/* Tile configuration (up to 15 tiles, 8 commonly used) */
static CellGcmTileInfo s_tiles[CELL_GCM_MAX_TILE_COUNT];

/* Zcull configuration (8 zcull regions) */
static CellGcmZcullInfo s_zcull[CELL_GCM_MAX_ZCULL_COUNT];

/* Report data area (256 slots, each 16 bytes) */
static CellGcmReportData s_report_data[CELL_GCM_MAX_REPORT_COUNT];

/* NV4097_SET_CONTEXT_DMA_REPORT (0x1A8): where NV4097_GET_REPORT writes.
 * Local (0xFEED0000, the default): the s_report_data slots above, read back
 * through cellGcmGetReport* / cellGcmGetReportDataAddress. Main memory
 * (0xFEED0001, or CELL_GCM_CONTEXT_DMA_REPORT_LOCATION_MAIN 0xBAD68000): the
 * report's 24-bit offset is an IO offset, and the 16-byte report goes to
 * guest memory at gcm_io2ea(offset), where the title reads it itself.
 * Killzone 3 sets 0xFEED0001 and keeps its reports in its own frame data. */
static u32 s_report_dma = 0xFEED0000u;
static u64 s_report_last_ts;

/* GET_REPORT timestamp in nanoseconds, strictly rising: two reports in one
 * walker pass must not share a time stamp or go backwards. */
static u64 gcm_report_timestamp(void)
{
    if (s_replay.on) {
        const u64 t = s_replay.used_ts < s_replay.n_ts ? s_replay.ts[s_replay.used_ts] : 0u;
        s_replay.used_ts++;
        return t;
    }
    u64 t = get_timestamp_ns();
    if (t <= s_report_last_ts) t = s_report_last_ts + 1u;
    s_report_last_ts = t;
    return t;
}

/* Label area (256 labels, each u32). Labels live in GUEST memory so the
 * recompiled game can poll them via vm_read32; cellGcmGetLabelAddress returns a
 * guest address into this window (16-byte spaced as on hardware). A host array
 * pointer would be read as a guest offset and land out of bounds. */
#define GCM_LABEL_GUEST_BASE  (VM_HLE_INJECT_BASE + 0x0000u)
#define GCM_LABEL_STRIDE      0x10u
/* Offsets of the labels libgcm_sys's own flip stream uses (the first reserved indexes of the
 * window): the driver's flip-ready handshake word and the flip-done marker. */
#define GCM_LABEL_FLIP_READY  0x30u
#define GCM_LABEL_FLIP_DONE   0x10u
/* The NV406E acquire the walker is stalled on: when it began, so a wait nobody ends stops. */
static u32 s_acq_off, s_acq_val;
static unsigned long long s_acq_t0;
static u32 s_labels[CELL_GCM_MAX_LABEL_COUNT];

/* GCM control register (put/get/ref). This MUST live in guest memory: the title
 * takes the pointer from cellGcmGetControlRegister, writes `put` when it kicks
 * the FIFO, and spins reading `get`/`ref` to know the RSX has caught up. A host
 * pointer would be read through vm_base as a guest address and never update, so
 * every FIFO-space / cellGcmFinish wait would hang. Placed just past the 256
 * label slots (0x03000000..0x03001000). Fields are big-endian (vm_write32). */
#define GCM_CONTROL_GUEST_ADDR (VM_HLE_INJECT_BASE + 0x2000u)

/* Synthetic guest code EA for the FIFO command-buffer-full callback. The title's
 * inline gcmReserve calls context->callback(context, count) when `current` nears
 * `end`; we point the context's callback OPD at this EA and register it in the
 * PPU function table (ppu_sysprx.cpp: hle_gcm_callback) so the indirect call
 * routes back into cellGcm_fifo_recycle(). Lives in the injected control page,
 * never real guest code. MUST match GCM_FIFO_CALLBACK_SENTINEL_EA there. */
#define GCM_FIFO_CALLBACK_SENTINEL_EA (VM_HLE_INJECT_BASE + 0x2F00u)

/* EA the ticker's RSX drain has consumed up to. Exposed so the wrap callback can
 * wait for the RSX to catch up before recycling the ring (else the frame's
 * just-written commands are lost). Updated in cellGcm_rsx_process_fifo. */
volatile u32 g_gcm_fifo_drained_ea = 0;

/* The guest variable cellGcmInit's ctx_out pointed at -- the title's own
 * gCellGcmCurrentContext. A title that switches to a command buffer of its own
 * does it by repointing this, and until something reads it back the runtime has
 * no way to notice. GCM_CTXDBG=1 reports it. */
static u32 s_gcm_ctx_out_ea = 0;

/* Set by cellGcm_syscall_bringup: this run reaches RSX through the lv2 sys_rsx_*
 * syscalls (libs/video/sys_rsx.c) rather than through the HLE cellGcm* imports. */
static int s_gcm_syscall_mode = 0;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

/* Populate the offset table entries for an IO mapping */
static void populate_offset_table(u32 ea, u32 io, u32 size)
{
    /*
     * The offset table uses 1MB pages (20-bit shift).
     * ioAddress[ea >> 20] = io >> 20   (EA -> IO offset)
     * eaAddress[io >> 20] = ea >> 20   (IO offset -> EA)
     */
    /* Round UP: a mapping is described by whole 1MB table entries, so a size
     * that is not a multiple of 1MB still needs its final partial page mapped.
     * Truncating dropped that page -- and dropped the mapping ENTIRELY for any
     * size below 1MB (pages == 0) -- after which cellGcmResolveOffset finds no
     * IO entry and falls back to VRAM, where the guest never wrote anything. */
    u32 pages = (size + 0xFFFFFu) >> 20;  /* number of 1MB pages, rounded up */
    u32 ea_page = ea >> 20;
    u32 io_page = io >> 20;

    for (u32 i = 0; i < pages && (ea_page + i) < 65536 && (io_page + i) < 65536; i++) {
        s_io_address_table[ea_page + i] = (u16)(io_page + i);
        s_ea_address_table[io_page + i] = (u16)(ea_page + i);
    }
    if (s_cap && !s_cap_quiet) s_cap->io_map(1, ea, io, size);
}

/* Clear offset table entries for an IO mapping */
static void clear_offset_table(u32 ea, u32 io, u32 size)
{
    u32 pages = size >> 20;
    u32 ea_page = ea >> 20;
    u32 io_page = io >> 20;

    for (u32 i = 0; i < pages && (ea_page + i) < 65536 && (io_page + i) < 65536; i++) {
        s_io_address_table[ea_page + i] = 0xFFFF;
        s_ea_address_table[io_page + i] = 0xFFFF;
    }
    if (s_cap) s_cap->io_map(0, ea, io, size);
}

/* Find an IO mapping by EA */
static IoMapping* find_mapping_by_ea(u32 ea)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active && s_io_mappings[i].ea == ea)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Find an IO mapping by IO offset */
static IoMapping* find_mapping_by_io(u32 io)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active && s_io_mappings[i].io == io)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Find a free IO mapping slot */
static IoMapping* find_free_mapping(void)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (!s_io_mappings[i].active) {
            /* Raised before the caller activates the slot; a caller that then
             * fails leaves it one slot high, which only costs a visit. */
            if (i >= __atomic_load_n(&s_io_mapping_hw, __ATOMIC_RELAXED))
                __atomic_store_n(&s_io_mapping_hw, i + 1, __ATOMIC_RELAXED);
            return &s_io_mappings[i];
        }
    }
    return NULL;
}

/* The 31 tiled pitches cellGcmGetTiledPitchSize can return: the library's own
 * table (libgcm_sys 0x0200B398, index 0 is a 0 sentinel that is skipped), which
 * stops at 0xE000. The generic 0x10000 that used to end this list is not in it:
 * a size above 0xE000 has no tiled pitch and the firmware answers 0. */
static const u32 s_valid_pitches[] = {
    0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800,
    0x0A00, 0x0C00, 0x0D00, 0x0E00, 0x1000, 0x1400, 0x1800,
    0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800,
    0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000,
    0xC000, 0xD000, 0xE000
};
static const int s_valid_pitch_count = sizeof(s_valid_pitches) / sizeof(s_valid_pitches[0]);

/* ---------------------------------------------------------------------------
 * API implementations -- Initialization
 * -----------------------------------------------------------------------*/

/* NID: 0xB477F06A */
s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress)
{
    printf("[cellGcmSys] Init(cmdSize=0x%X, ioSize=0x%X, ioAddr=0x%08X)\n",
           cmdSize, ioSize, ioAddress);

    if (s_gcm_initialized) {
        printf("[cellGcmSys] WARNING: already initialized\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_control, 0, sizeof(s_control));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    __atomic_store_n(&s_io_mapping_hw, 0, __ATOMIC_RELAXED);
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_zcull, 0, sizeof(s_zcull));
    memset(s_report_data, 0, sizeof(s_report_data));
    s_report_dma = 0xFEED0000u;
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    memset(s_subch_handle, 0, sizeof(s_subch_handle));
    memset(s_subch_class, 0, sizeof(s_subch_class));
    memset(&s_m2mf, 0, sizeof(s_m2mf));

    /*
     * Populate a plausible configuration.
     * In a real implementation these would reflect actual allocated memory.
     */
    s_config.localAddress    = 0xC0000000;  /* typical RSX local mem base */
    s_config.localSize       = 256 * 1024 * 1024;  /* 256 MB */
    s_config.ioAddress       = ioAddress;
    s_config.ioSize          = ioSize;
    s_config.memoryFrequency = 650000000;   /* 650 MHz */
    s_config.coreFrequency   = 500000000;   /* 500 MHz */

    s_local_mem_allocated = 0;
    s_io_mapping_count = 0;
    /* Unmapped = 0xFFFF. The tables are static (zero-initialized), so without
     * this every unmapped EA page silently aliased io page 0 (and vice versa),
     * producing plausible-but-wrong offsets instead of a translation failure. */
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    s_current_display_buffer_id = 0;
    s_flip_handler = NULL;
    s_vblank_handler = NULL;
    s_user_handler = NULL;
    s_second_v_handler = NULL;
    s_last_flip_time = get_timestamp_ns();
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_flip_mode = CELL_GCM_DISPLAY_VSYNC;
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_io_map_reserved = 0;
    s_default_fifo_mode = 0;
    s_graphics_handler = NULL;
    s_queue_handler = NULL;
    s_second_v_frequency = 0;
    s_vblank_frequency = 0;
    GCM_USER_STORE(0);
    gcm_user_ring_reset();
    gcm_driver_events_reset();

    /* Set up the initial IO mapping for the command buffer region */
    if (ioAddress != 0 && ioSize > 0) {
        s_cap_quiet = 1;
        populate_offset_table(ioAddress, 0, ioSize);
        s_cap_quiet = 0;

        s_io_mappings[0].ea     = ioAddress;
        s_io_mappings[0].io     = 0;
        s_io_mappings[0].size   = ioSize;
        __atomic_store_n(&s_io_mapping_hw, 1, __ATOMIC_RELAXED);
        s_io_mappings[0].active = 1;
        s_io_mapping_count = 1;
    }

    /* Zero the guest-visible control register (put/get/ref). */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, 0);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, 0);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 8, 0);

    /* Watch SPU DMA for pushbuffers. A title that builds its command stream on
     * an SPU never tells libgcm where that stream is, so the write is the only
     * evidence of it -- see gcm_note_spu_put. Costs one NULL check per SPU DMA
     * for titles that do not. */
    { extern void (*g_spu_put_hook)(u32 ea, u32 size);
      void gcm_note_spu_put(u32 ea, u32 size);       /* defined with the walker */
      void gcm_forget_spu_blocks(void);
      gcm_forget_spu_blocks();
      g_spu_put_hook = gcm_note_spu_put; }

    /* The bootstrap stores the start value into the handler word once the interrupt thread exists, i.e.
     * once init has succeeded (dossier §2: 0x02008194 refuses a second creation, and the already-
     * initialised return above likewise writes nothing). cellGcmInit is the HLE's init body, and
     * cellGcmSetupContext (the _cellGcmInitBody core) goes through it. The HLE has no interrupt thread
     * (#73); the word is what a guest reader sees. */
    gcm_handler_word_start();
    s_gcm_initialized = 1;
    return CELL_OK;
}

/* The default command buffer's segment record, as libgcm_sys builds it (FW 4.93, 0x020036D4,
 * called from _cellGcmInitBody 0x020009A8): the IO window starts with the default-state
 * subroutine; the command buffer proper begins 0x1000 bytes in and is cut into 32 KiB
 * segments, the context spanning one at a time. When it fills, the buffer-full callback
 * (0x02003AAC) flushes, JUMPs from the old `current` to the next segment and waits for the
 * RSX to have left that segment. The last segment wraps to the second 0x400 words of the
 * window (`wrap_start`). All sizes in words. */
#define GCM_SEGMENT_WORDS     0x2000u    /* [0x30] */
#define GCM_FIRST_SEG_OFFSET  0x1000u    /* bytes; the 6th argument of 0x020036D4 */
#define GCM_WRAP_START_WORDS  0x400u     /* [0x28] */
typedef struct GcmIb {
    int valid;
    u32 ctx_ea;        /* the CellGcmContextData InitBody returned */
    u32 io_ea;         /* ioAddress */
    u32 cmd_words;     /* [0x24]: cmdSize in words, rounded down to whole segments */
    u32 seg_words;     /* [0x30] */
    u32 nseg;          /* [0x34] */
    u32 last_end;      /* [0x18]: EA of the last word of the last segment */
    u32 first_begin_off, last_end_off;   /* [0x1C]/[0x20]: RSX offsets bounding the ring */
} GcmIb;
static GcmIb s_ib;
static u32 gcm_ea2io(u32 ea);   /* defined with the wrap callback below */

/* Point the default context at its first segment and reset the FIFO (0x020036D4 + sc 674). */
static void gcm_ib_reset_context(u32 ctx)
{
    const u32 begin = s_ib.io_ea + GCM_FIRST_SEG_OFFSET;
    vm_write32(ctx + 0x0, begin);
    vm_write32(ctx + 0x4, s_ib.io_ea + s_ib.seg_words * 4u - 4u);
    vm_write32(ctx + 0x8, begin);
    cellGcm_syscall_set_fifo(s_ib.first_begin_off, s_ib.first_begin_off);
}

/* Reusable _cellGcmInitBody core (NID 0x15BAE46B). See cellGcmSys.h for why the
 * guest vm is injected as callbacks. The CellGcmContextData layout here is the
 * one proven in the shipping Simpsons port (begin@0/end@4/current@8/callback@C);
 * getting it wrong makes the game read `current` as the callback OPD and stall
 * in cellGcmFlush.
 *
 * The IO window's layout is the firmware's (see GcmIb): default-state subroutine at io 0,
 * context at io + 0x1000 over the first 32 KiB segment. Returns 0, with nothing initialised,
 * when the firmware would fail with CELL_GCM_ERROR_FAILURE: cmdSize not a word multiple
 * or <= 0xFFFF, cmdSize + 0x1000 > ioSize, ioSize or ioAddress not 1 MiB aligned. */
u32 cellGcmSetupContext(u32 ctx_out_addr, u32 cmdSize, u32 ioSize, u32 ioAddress,
                        CellGcmGuestAlloc galloc, CellGcmGuestWrite32 gwrite32)
{
    /* PSL1GHT's gcmInitBody passes ioAddress = 0, expecting gcm to allocate the
     * IO region itself (the official SDK has the app allocate and pass it).
     * Taking the 0 literally put the FIFO command buffer at guest address 0 --
     * the guest then wrote its command stream over low memory and every offset
     * resolution failed. Allocate a guest region of ioSize instead. */
    if (ioAddress == 0 && galloc)
        ioAddress = galloc(ioSize ? ioSize : 0x100000, 0x100000);

    if (cmdSize <= 0xFFFFu || (cmdSize & 3u) != 0 || (u64)cmdSize + 0x1000u > ioSize ||
        (ioSize & 0xFFFFFu) != 0 || (ioAddress & 0xFFFFFu) != 0)
        return 0;

    u32 cdata = galloc ? galloc(16, 16) : 0;
    if (!cdata || !gwrite32)
        return 0;

    if (s_cap) s_cap->gcm_init(cmdSize, ioSize, ioAddress);
    cellGcmInit(cmdSize, ioSize, ioAddress);

    /* io 0: the default-state subroutine, then (as InitBody does at 0x02000AD8) it ends in RET,
     * which gcm_emit_default_state writes. */
    gcm_emit_default_state(ioAddress);

    /* The subroutine's first words are the channel's SET_OBJECTs, and the driver runs it during init:
     * the console's subchannels are bound before any title traffic. The RESC conversion stream proves it —
     * cellRescSetConvertAndFlip's stream (resc_convert.c) writes viewport and depth state without a bind,
     * and the firmware's first flip only CALLs the subroutine afterwards (docs/rsxfifo.txt "SetViewport"),
     * so those writes can only work on a channel that is already bound. Model that here, once per init,
     * instead of leaving the table empty until the first observed SET_OBJECT: the walker would otherwise
     * drop the first frame's streams as unbound (and the depth range and viewport with them). */
    gcm_bind_object(0, RSX_OBJ_NV4097);
    gcm_bind_object(1, RSX_OBJ_NV0039);
    gcm_bind_object(3, RSX_OBJ_NV3062);
    gcm_bind_object(4, RSX_OBJ_NV309E);
    gcm_bind_object(5, RSX_OBJ_NV308A);
    gcm_bind_object(6, RSX_OBJ_NV3089);
    gcm_bind_object(7, RSX_OBJ_DRIVER);

    s_ib.valid     = 1;
    s_ib.ctx_ea    = cdata;
    s_ib.io_ea     = ioAddress;
    s_ib.seg_words = GCM_SEGMENT_WORDS;
    s_ib.cmd_words = (cmdSize >> 2) & ~(GCM_SEGMENT_WORDS - 1u);
    s_ib.nseg      = s_ib.cmd_words / GCM_SEGMENT_WORDS;
    s_ib.last_end  = ioAddress + s_ib.cmd_words * 4u - 4u;
    s_ib.first_begin_off = gcm_ea2io(ioAddress + GCM_FIRST_SEG_OFFSET);
    s_ib.last_end_off    = gcm_ea2io(s_ib.last_end);

    /* Command-buffer-full callback: a guest OPD {func, toc} routed to
     * hle_gcm_callback -> cellGcm_fifo_recycle (the firmware's 0x02003AAC). */
    u32 opd = galloc(8, 8);
    gwrite32(cdata + 0xC, opd);
    if (opd) {
        gwrite32(opd + 0x0, GCM_FIFO_CALLBACK_SENTINEL_EA);  /* func EA */
        gwrite32(opd + 0x4, 0);                              /* toc (unused) */
    }
    gcm_ib_reset_context(cdata);
    if (ctx_out_addr)
        gwrite32(ctx_out_addr, cdata);          /* *context = &ctxdata */
    s_gcm_ctx_out_ea = ctx_out_addr;   /* the title's gCellGcmCurrentContext */
    s_gcm_context_ea = cdata;                   /* RSX drains commands from here */
    return cdata;
}

/* A store through a caller's NULL pointer. libgcm_sys writes its out-parameters without testing them, so on the
 * console a NULL one is a store into the null page, which faults; here vm_write32 would land silently at guest
 * address 0 and the title would run on. guest-null-access is the stop the loader raises for the same store made by
 * guest code (runtime/ppu/ppu_loader.cpp, ppu_null_page_stop). */
#define GCM_NULL_STORE_WHAT "an HLE handler would store through a null guest pointer"
#define GCM_NULL_STORE_REMEDY \
    "The firmware stores to this out-parameter without testing it and faults on a null one. " \
    "Find the call in the guest that passed NULL; no return value would be faithful."
#define GCM_NULL_STORE_FIELDS(fn) \
    ((const ps3_stop_field[]){ {"access", "write"}, {"width", "4"}, {"ea", "0x00000000"}, {"fn", (fn)} })

s32 cellGcmGetConfiguration(CellGcmConfig* config)
{
    /* `config` is a GUEST address; write the 6 u32 fields big-endian via
     * vm_write32 (a raw *config = s_config faults / is host-endian). The game
     * reads localAddress/localSize from here to lay out VRAM framebuffers. */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    /* libgcm_sys 0x0200476C (cellGcmGetConfiguration, NID 0xE315A0B2, opd 0x0200BCBC) never tests its argument: the
     * six words go to r31 = config at 0x0200479C..0x020047C8 and the function holds no error constant, so a NULL
     * config is a store into the null page (a fault), not CELL_GCM_ERROR_INVALID_VALUE. (While no GCM context
     * exists the function skips every store, 0x02004780..0x0200478C, and returns 0; this stops for NULL either way.) */
    if (!cfg)
        ps3_stop(PS3_STOP_GUEST_NULL_ACCESS, GCM_NULL_STORE_WHAT, GCM_NULL_STORE_REMEDY,
                 GCM_NULL_STORE_FIELDS(__func__), 4, NULL);

    vm_write32(cfg +  0, s_config.localAddress);
    vm_write32(cfg +  4, s_config.ioAddress);
    vm_write32(cfg +  8, s_config.localSize);
    vm_write32(cfg + 12, s_config.ioSize);
    vm_write32(cfg + 16, s_config.memoryFrequency);
    vm_write32(cfg + 20, s_config.coreFrequency);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Command buffer control
 * -----------------------------------------------------------------------*/

/* NID: 0xA547ADDE */
CellGcmControl* cellGcmGetControlRegister(void)
{
    /* Return the GUEST address of the control register (not &s_control, a host
     * pointer). The recompiled title reads/writes it through vm_base, so it must
     * be a guest EA for put/get/ref to actually flow. */
    return (CellGcmControl*)(uintptr_t)GCM_CONTROL_GUEST_ADDR;
}

/* ---------------------------------------------------------------------------
 * Display / flip
 * -----------------------------------------------------------------------*/

/* NID: 0xC8F3BD09 */
u32 cellGcmGetCurrentField(void)
{
    /* Returns 0 or 1 for interlaced; always 0 for progressive. */
    return 0;
}

/* NID: 0x4AE8D215 */
void cellGcmSetFlipMode(u32 mode)
{
    printf("[cellGcmSys] SetFlipMode(mode=%u)\n", mode);
    s_flip_mode = mode;
}

/* NID: 0xB2E761D4 */
void cellGcmResetFlipStatus(void)
{
    { static int n = 0;
      static int fd = -1; if (fd < 0) fd = getenv("FLIP_DBG") ? 1 : 0;
      if (fd && PS3_LOG_FIRST(n, 12))
          fprintf(stderr, "[FLIP] ResetFlipStatus%c", 10); }
    FLIP_LOCK();
    s_flip_status = CELL_GCM_FLIP_STATUS_WAITING;
    s_flip_req_seq++;
    FLIP_UNLOCK();
}

/* NID: 0x72A577CE */
u32 cellGcmGetFlipStatus(void)
{
    /* Avoid host-calling the guest OPDs directly (they're not valid host
     * function pointers). Use cellGcmTickVBlank() / TickFlip() instead,
     * which dispatch via g_ps3_guest_caller — see below. */
    /* Report the CURRENT status; cellGcmTickFlip (the ticker's 60Hz vblank
     * beat) completes pending flips. The old self-completing version made
     * every wait-for-flip loop exit on its first poll, so titles ran
     * completely unpaced (wave: 95 fps with a fixed-dt simulation). */
    { static int n = 0;
      static int fd = -1; if (fd < 0) fd = getenv("FLIP_DBG") ? 1 : 0;
      if (fd && PS3_LOG_FIRST(n, 24))
          fprintf(stderr, "[FLIP] GetFlipStatus -> %u (0=DONE 1=WAITING)%c",
                  s_flip_status, 10); }
    return s_flip_status;
}

/* Host-side ticks. The game's host driver (e.g. flow main.cpp) calls
 * these periodically so registered guest handlers fire — that's what
 * drives the title-screen state machine for many games. */
#include "ps3emu/guest_call.h"

/* ---------------------------------------------------------------------------
 * Serialized vblank/flip/user handler delivery.
 *
 * Tick producers publish pending work. The runner pumps it at HLE boundaries
 * or on a dedicated callback thread. The pump serializes guest handlers with
 * one another; it does not stop unrelated guest execution on other threads.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <windows.h>
static volatile LONG s_gcm_pending = 0;    /* bit0 = vblank, bit1 = flip */
/* Wakes one guest thread blocked in a semaphore wait, which then pumps (see
 * ppu_gcm_wait_pump). Auto-reset; NULL until the first such wait. */
static HANDLE s_gcm_pump_ev = NULL;
static void gcm_pump_wake(void) { if (s_gcm_pump_ev) SetEvent(s_gcm_pump_ev); }
#define GCM_PENDING_SET(bits)  (InterlockedOr(&s_gcm_pending, (bits)), gcm_pump_wake())
#define GCM_PENDING_TAKE()     InterlockedExchange(&s_gcm_pending, 0)
static volatile LONG s_gcm_vblank_pending = 0;   /* vblanks raised since the last pump */
#define GCM_VBLANK_POST()      InterlockedIncrement(&s_gcm_vblank_pending)
#define GCM_VBLANK_TAKE()      InterlockedExchange(&s_gcm_vblank_pending, 0)
static volatile LONG s_gcm_pumping = 0;
#define GCM_PUMP_TRY_ENTER() (InterlockedCompareExchange(&s_gcm_pumping, 1, 0) == 0)
#define GCM_PUMP_LEAVE()     InterlockedExchange(&s_gcm_pumping, 0)
#else
#include <stdatomic.h>
static atomic_int s_gcm_pending = 0;
#define GCM_PENDING_SET(bits)  atomic_fetch_or(&s_gcm_pending, (bits))
#define GCM_PENDING_TAKE()     atomic_exchange(&s_gcm_pending, 0)
static atomic_int s_gcm_vblank_pending = 0;   /* vblanks raised since the last pump */
#define GCM_VBLANK_POST()      atomic_fetch_add(&s_gcm_vblank_pending, 1)
#define GCM_VBLANK_TAKE()      atomic_exchange(&s_gcm_vblank_pending, 0)
static atomic_flag s_gcm_pumping = ATOMIC_FLAG_INIT;
#define GCM_PUMP_TRY_ENTER() (!atomic_flag_test_and_set_explicit(&s_gcm_pumping, memory_order_acquire))
#define GCM_PUMP_LEAVE()     atomic_flag_clear_explicit(&s_gcm_pumping, memory_order_release)
#endif

/* Called by the vblank ticker thread. NO guest code -- advance the vblank count
 * and mark a vblank+flip tick pending for the main thread to deliver. */
static u32 s_fifo_getoff;   /* tentative: defined below */

void cellGcm_request_tick(void)
{
    s_vblank_count++;
    GCM_VBLANK_POST();
    /* GCM_FLIP_NEEDS_FIFO=1: only mark a flip pending once the walker has
     * actually consumed up to `put`.
     *
     * A title's fifo-finish can use flip status as a fast path -- Twisted
     * Metal's reads it first and, if the flip is done, skips waiting for `get`
     * entirely. Completing a flip on the 60 Hz beat regardless of FIFO
     * progress therefore tells it the RSX has caught up when it has not, and
     * it recycles a FIFO block the walker is still inside, overwriting the
     * JUMP out of it. Off by default: a title whose FIFO never drains would
     * stop flipping entirely. */
    { static int need = -1;
      if (need < 0) { const char* e = getenv("GCM_FLIP_NEEDS_FIFO"); need = e ? atoi(e) : 0; }
      if (need) {
          u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
          const int flip = s_fifo_getoff == put;
          if (flip) { FLIP_LOCK(); s_flip_tick_seq = s_flip_req_seq; FLIP_UNLOCK(); }
          GCM_PENDING_SET(flip ? 3 : 1);
          return;
      } }
    /* A flip queued in the command stream completes when the drain reaches
     * its marker, as on the RSX; until then this beat is a vblank only.
     * Completing it on the beat told Twisted Metal the RSX had finished frames
     * it had not reached, and the title rebuilt command segments the walker
     * was still waiting to execute. Flips that never went through the stream
     * are unaffected. The completion is stamped with the flip requested last
     * (see ppu_gcm_pump). */
    if (gcm_flip_executed_take()) {
        FLIP_LOCK(); s_flip_tick_seq = s_flip_req_seq; FLIP_UNLOCK();
        GCM_PENDING_SET(3);
    } else {
        GCM_PENDING_SET(1);
    }
}

/* Deliver pending handlers on the calling host thread. Serialize the entire
 * claim-and-deliver sequence across threads: an HLE boundary and a host ticker
 * may both pump, and newer causes must not overtake already claimed callbacks.
 * A nonblocking guard also prevents nested callbacks from reentering. Leave
 * pending notifications untouched when another pump is active. */
#ifdef _WIN32
/* For sys_semaphore_wait. A guest thread blocked there also waits on this
 * event and, when it fires, runs *pump (ppu_gcm_pump) to deliver what is
 * pending, as a timer sleep or an HLE call does. Without it a flip handler
 * waits for some guest thread to reach one of those: about 1 ms a frame in
 * God of War HD, whose main thread sits in a semaphore wait until its flip
 * handler posts that semaphore. */
void ppu_gcm_pump(void);
void* ppu_gcm_wait_pump(void (**pump)(void))
{
    if (!s_gcm_pump_ev) {
        HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (e && InterlockedCompareExchangePointer((PVOID volatile*)&s_gcm_pump_ev, e, NULL) != NULL)
            CloseHandle(e);
        if (s_gcm_pending || gcm_user_ring_nonempty()) gcm_pump_wake();
    }
    *pump = ppu_gcm_pump;
    return (void*)s_gcm_pump_ev;
}
#endif

void ppu_gcm_pump(void)
{
    if (!GCM_PUMP_TRY_ENTER()) return;
    long p = (long)GCM_PENDING_TAKE();
    /* AM-27: the user commands the RSX retired, delivered below in that order */
    /* GCM_PUMP_DBG=1: how often each guest handler is actually delivered. */
    { static int d = -1; static unsigned nv, nf, calls;
      if (d < 0) d = getenv("GCM_PUMP_DBG") ? 1 : 0;
      if (d) { calls++; if (p & 1) nv++; if (p & 2) nf++;
               if ((calls % 20000) == 0)
                   fprintf(stderr, "[gcm-pump] calls=%u vblank=%u flip=%u vcount=%u%c",
                           calls, nv, nf, s_vblank_count, 10); } }
    if (p & 1) {
        /* One call per vblank the driver raised since the last pump, not one for however many are pending: the
         * console's interrupt posts each vblank to the handler (RPCS3's vblank thread queues one handler call per
         * vblank it fires, late ones back to back). With one vblank pending this is the old single call. Each call
         * gets its own vblank number, the last one the current count. */
        const long n = (long)GCM_VBLANK_TAKE();
        if (s_vblank_handler_opd && g_ps3_guest_caller) {
            for (long i = 0; i < n; i++) {
                ydkj_restore_handler_opd(s_vblank_handler_opd, s_vblank_handler_code);
                g_ps3_guest_caller(s_vblank_handler_opd, (uint64_t)(s_vblank_count - (u32)(n - 1 - i)),
                                   0, 0, 0, 0, 0, 0, 0);
            }
        }
    }
    if (p & 2) {
        /* Delivery waits for a guest thread to reach an HLE call or a timer
         * sleep, so a newer flip may have been requested (status WAITING
         * again) since this completion was marked. Marking DONE then would
         * erase that request: the ticker only completes WAITING flips, so its
         * handler would never run. inFAMOUS 2 writes its next FIFO put from
         * the flip handler, and hung with get == put on exactly that. Leave
         * the newer flip WAITING for the next tick. */
        FLIP_LOCK();
        if (s_flip_tick_seq == s_flip_req_seq)
            s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
        FLIP_UNLOCK();
        if (s_flip_handler_opd && g_ps3_guest_caller) {
            ydkj_restore_handler_opd(s_flip_handler_opd, s_flip_handler_code);
            g_ps3_guest_caller(s_flip_handler_opd, 1, 0, 0, 0, 0, 0, 0, 0);
        }
    }
    for (u32 cmd; gcm_user_ring_pop(&cmd);) {
        if (s_user_handler_opd && g_ps3_guest_caller)
            g_ps3_guest_caller(s_user_handler_opd, (uint64_t)cmd, 0, 0, 0, 0, 0, 0, 0);
    }
    GCM_PUMP_LEAVE();
}

/* Back-compat: the old direct entry points now just mark a tick pending (so any
 * caller other than the ticker still works) -- delivery stays on the main thread. */
/* A vblank is when the driver completes a flip the RSX has executed: the flip-done label goes back to
 * 0, which releases an ACQUIRE waiting on it (cellRescSetWaitFlip). The two halves are separate entry points
 * for a host whose clock must not wait behind the walker: cellGcm_vblank_signal is the vertical blank itself
 * (the counter, the handler's call) and may be raised from a thread of its own at the instant it is due;
 * cellGcm_vblank_flip_label is the driver's answer and is called where the walker runs, after it has presented the
 * frame behind the flip head (cellGcm_take_flip_pending_synced) -- the label's release is what lets the walker go past
 * the head's ACQUIRE, so the present has to come first. cellGcmTickVBlank is the two in that order. */
void cellGcm_vblank_signal(void)
{
    s_vblank_count++;
    GCM_VBLANK_POST();
    GCM_PENDING_SET(1);
}
void cellGcm_vblank_flip_label(void)
{
    if (s_drv_flip_label_pending) {
        s_drv_flip_label_pending = 0;
        atomic_thread_fence(memory_order_release);
        vm_write32(GCM_LABEL_GUEST_BASE + GCM_LABEL_FLIP_DONE, 0);
    }
}
void cellGcmTickVBlank(void)
{
    cellGcm_vblank_signal();
    cellGcm_vblank_flip_label();
}
/* Only deliver a flip completion when a flip is actually outstanding.
 *
 * Ticking unconditionally at 60 Hz fires the guest's flip handler even when
 * the guest never asked for a flip. A title whose handler touches the flip
 * status then livelocks: Tokyo Jungle's main loop is
 *
 *     do { usleep(100); } while (cellGcmGetFlipStatus() != DONE);
 *     cellGcmResetFlipStatus();
 *
 * and its handler runs code that puts the status back to WAITING, so the
 * poll never saw DONE and the game never rendered another frame. On hardware
 * the callback IS the flip interrupt -- no flip, no callback. */
void cellGcmTickFlip(void)
{
    /* Same rule as cellGcm_request_tick: a flip still ahead of the drain has
     * not happened yet. */
    FLIP_LOCK();
    const int waiting = s_flip_status == CELL_GCM_FLIP_STATUS_WAITING && gcm_flip_executed_take();
    if (waiting) s_flip_tick_seq = s_flip_req_seq;
    FLIP_UNLOCK();
    if (waiting) GCM_PENDING_SET(2);
}

/* Drain the game's GCM FIFO into the RSX backend. Called from the present thread
 * (boot_main vblank_ticker). Not yet active: s_control.put stays 0 because
 * cellGcmGetControlRegister returns a HOST pointer, so the title's put writes
 * don't reach this struct (the control register must live in guest memory for
 * the recompiled code to update it via vm_write). Once that's fixed (+ the title
 * runs past its early self-exit to actually draw), parse get..put here with
 * rsx_process_command_buffer so the game's clears/draws render. */
/* Translate an RSX IO offset to a guest EA via the offset table (1MB pages,
 * populated by cellGcmInit / cellGcmMapMainMemory / MapEaIoAddress). */
static u32 gcm_io2ea(u32 io)
{
    u32 page = io >> 20;
    if (page >= 65536) return 0;
    u16 ea_page = s_ea_address_table[page];
    if (ea_page == 0xFFFF) return 0;
    return ((u32)ea_page << 20) | (io & 0xFFFFFu);
}

/* For the RSX method layer: semaphores aimed at main memory carry IO offsets. */
u32 gcm_io_offset_to_ea(u32 io) { return gcm_io2ea(io); }

/* RSX get pointer (IO offset) + one-deep CALL return slot. The FIFO-wrap
 * recycle path teleports these when it resets a ring without a JUMP command. */
/* Milliseconds on a monotonic clock, for the runtime's own timeouts and rate
 * logs. Off Windows this returned 0, so every interval measured with it was
 * 0 there: rate logs never printed and timeouts never expired. */
unsigned long long ps3_ms_now(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

static u32 s_sema_offset = 0;   /* NV406E semaphore offset (label window) */
static int s_sema_blocked_pass = 0;   /* this drain stopped on a semaphore acquire */
static int s_park_pass = 0;           /* this drain stopped on a self-JUMP park */
/* The park the walker is waiting on (0 = none), the `put` it saw there, and
 * since when it has stood with that put: the jump-park grace in the walker. */
static u32 park_off, park_put;
static unsigned long long park_since;
/* Backlog past which the drain stops honouring one-flip-per-tick and
 * catches up instead. Sized well above a frame's command list so a
 * title that keeps up never sees this path. */
#define GCM_FIFO_CATCHUP_BYTES 0x10000u
/* A self-JUMP park the guest has not patched while `put` stands elsewhere. A title that appends after the
 * park, moves `put` there and patches the park last leaves the park standing for a while: God of War HD,
 * loading its menu, has taken more than 50 ms between the two stores, and following `put` dropped the
 * commands in between, a fence with them, so the boot stopped. The RSX itself just waits.
 *
 * Reference mode (gcm.park_resync off, the default): the walker waits at the park for as long as the guest
 * takes, and a park that stands the whole gcm.wait_timeout_ms is the named stop `gcm-wait-timeout`
 * (what=jump-park); GET never moves to `put` because a host deadline expired.
 * Compatibility mode (gcm.park_resync on): a park that has stood gcm.park_grace_ms with the same `put`
 * and is still unpatched moves GET to `put`, dropping the commands between, and logs the intervention
 * with the mode and the effective grace. */
/* How close `current` must get to `end` before the runtime will recycle a ring
 * on the title's behalf. One gcmReserve is 8 bytes; a page of slack keeps a
 * title that is merely near the end from being touched. */
#define GCM_RECYCLE_SLACK 0x1000u
/* How far past the ring's start the walker must have read before the runtime
 * will recycle on the title's behalf. The guest resumes writing at `begin`, so
 * this is the amount of already-consumed head room it gets. */
#define GCM_RECYCLE_MARGIN 0x20000u
static u32 gcm_ea2io(u32 ea);   /* defined with the wrap callback below */
static u32 s_fifo_getoff  = 0;
/* Backward JUMPs the walker has taken. The ring recycle waits for this to move:
 * it is the walker following the JUMP-to-begin the recycle wrote, i.e. having
 * consumed the whole tail. g_gcm_fifo_drained_ea == begin cannot say that on its
 * own, because it is also begin before the walker has moved at all. */
static volatile u32 s_fifo_back_jumps = 0;
/* Set while the title's buffer-full callback (cellGcm_fifo_recycle) is waiting
 * for the walker. Its context still reads current == end then, which is exactly
 * what the drain's own overrun recycle looks for; firing that under the
 * callback teleports get to begin and drops the tail the callback is waiting on. */
static volatile int s_fifo_recycle_waiting = 0;

/* Bytes the walker is behind the writer. Linear while put >= get. Once the
 * title's ring wraps, put sits BEHIND get and the backlog is the rest of the
 * ring plus everything from its start to put -- nearly the whole ring, not
 * nothing. Only computed when both pointers lie inside the title's own ring;
 * any other layout (segments linked by JUMPs) reports 0, as before. */
static u32 gcm_fifo_backlog(u32 put, u32 get)
{
    if (put >= get) return put - get;
    if (!s_gcm_context_ea) return 0;
    u32 b = gcm_ea2io(vm_read32(s_gcm_context_ea + 0x0));
    u32 e = gcm_ea2io(vm_read32(s_gcm_context_ea + 0x4));
    if (b == 0xFFFFFFFFu || e == 0xFFFFFFFFu || e <= b) return 0;
    if (get < b || get > e || put < b || put > e) return 0;
    return (e - get) + (put - b);
}
static u32 s_last_ring_off = 0;   /* last offset the walker actually consumed */
static u32 s_fifo_calloff = 0;

/* ---------------------------------------------------------------------------
 * Spans the RSX consumes, and the capture of them (rsx_capture.h)
 * -----------------------------------------------------------------------*/

/* The guest address space a consumed span must stay inside: what the host
 * backed, or all 4 GiB when it reserved them. */
static u64 gcm_vm_limit(void)
{
    extern uint32_t ppu_vm_size;
    return ppu_vm_size ? (u64)ppu_vm_size : 0x100000000ull;
}

/* [ea, ea+size) stays inside guest memory. Summed in 64 bits, so a span that
 * would wrap past 4 GiB is refused rather than folded back to the start. */
static int gcm_span_ok(u64 ea, u64 size)
{
    return size && ea + size <= gcm_vm_limit();
}

/* cellGcmSetWaitFlip (NID 0x983FB9AA, libgcm_sys 0x02001180) and cellGcmSetWaitFlipUnsafe (NID 0xDF6476BD,
 * 0x02001228; OPD 0x0200B93C): the firmware never waits on the CPU. It appends two method pairs to the
 * caller's command context and advances `current` by 16:
 *   {0x00040064, 0x10}   NV406E SEMAPHORE_OFFSET: label 0x10, the flip-done label the flip command releases
 *                        as 0xFFFFFFFF (cellGcm_emit_flip_command) in the semaphore context its `CALL 0`
 *                        default state leaves selected (0x66616661, the label window)
 *   {0x00040068, 0}      NV406E SEMAPHORE_ACQUIRE: the RSX holds here until the label is 0
 * SetWaitFlip reserves the 16 bytes first (`current + 16 > end` calls callback(ctx, 4), then writes
 * without looking again; cellGcm_ctx_reserve stops when the callback left no room). The Unsafe export
 * is the same words at `current` with no space check.
 *
 * The wait is the walker's NV406E acquire. What sets label 0x10 back to 0 is the driver half, outside
 * the firmware image: here the next vblank after a retired flip head (cellGcm_vblank_flip_label), which
 * is RPCS3's flip completion on its driver (!isHLE) path (pinned d00d8023: rpcs3/Emu/RSX/RSXThread.cpp:
 * 3725-3735 handle_emu_flip runs flip(), then emits sys_rsx_context_attribute(0xFEC);
 * rpcs3/Emu/Cell/lv2/sys_rsx.cpp:1035-1046 stores
 * u128{} at `label_addr + 0x10`). A SetWaitFlip with no flip outstanding passes at once (label 0).
 * The old HLE slept up to 64 ms per call on the PPU, which the firmware does not. */
static void gcm_wait_flip_group(const char* func, void* context, int reserve)
{
    const u32 ctx = (u32)(uintptr_t)context;
    /* The context is read (+4 end, +8 current, +0xC callback) and written (+8) as one 16-byte object. */
    if (!gcm_span_ok(ctx, sizeof(CellGcmContextData)))
        ps3_stop_guest_range(func, "ctx", ctx, sizeof(CellGcmContextData), gcm_vm_limit());
    if (reserve)
        cellGcm_ctx_reserve(ctx, 4);
    const u32 cur = vm_read32(ctx + 8);
    if (!gcm_span_ok(cur, 16))
        ps3_stop_guest_range(func, "current", cur, 16, gcm_vm_limit());
    vm_write32(cur + 0, 0x00040064u);
    vm_write32(cur + 4, GCM_LABEL_FLIP_DONE);
    vm_write32(cur + 8, 0x00040068u);
    vm_write32(cur + 12, 0u);
    vm_write32(ctx + 8, cur + 16u);
}

void cellGcmSetWaitFlip(void* context) { gcm_wait_flip_group("cellGcmSetWaitFlip", context, 1); }

void cellGcmSetWaitFlipUnsafe(void* context) { gcm_wait_flip_group("cellGcmSetWaitFlipUnsafe", context, 0); }

/* Open the capture event for one walker step. */
static void gcm_cap_step(u32 kind, u32 get, u32 put, const u32* words, u32 nwords,
                         u32 subch, u32 incrementing, u32 count, u32 target)
{
    rsx_cap_step st;
    st.kind = kind; st.get = get; st.put = put; st.call_ret = s_fifo_calloff;
    st.subch = subch; st.incrementing = incrementing; st.count = count; st.target = target;
    st.words = words; st.nwords = nwords; st.io2ea = gcm_io2ea;
    s_cap->step_begin(&st);
}

/* A capture failure, through the sink. */
static void gcm_cap_fail(const char* reason, const char* detail)
{
    s_cap->fail(reason, detail);
    abort();
}

/* A big-endian word the RSX reads from guest memory at `ea` (RSX offset
 * `off`), through the capture when one is open so that what the walker acts
 * on is the copy the capture recorded. */
static u32 gcm_cap_read32(u32 off, u32 ea, u32 kind)
{
    if (!s_cap) return vm_read32(ea);
    extern u8* vm_base;
    if (!gcm_span_ok(ea, 4)) {
        char d[64];
        snprintf(d, sizeof d, "a 4-byte RSX read at ea 0x%08X leaves guest memory", ea);
        gcm_cap_fail("span", d);
    }
    const u8* p = s_cap->read(RSX_CAP_LOC_MAIN, off, ea, vm_base + ea, 4, kind);
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* ---------------------------------------------------------------------------
 * 2D transfer engines (FIFO subchannels != 0).
 *
 * libgcm binds: sub 1/6 = NV0039 (m2mf buffer copy), sub 2 = NV3062 (context
 * surface 2D), sub 3 = NV309E (swizzled surface), sub 4 = NV308A (image from
 * CPU / inline transfer), sub 5 = NV3089 (scaled image).
 *
 * cellGcmSetFragmentProgramParameter patches a fragment program's inline
 * constants by emitting NV3062 (destination surface) + NV308A (COLOR data
 * words) through the FIFO -- it does NOT CPU-write the ucode copy. Without
 * this engine the patched constants stay zero in local memory (demosaic: all
 * its texture-size constants -> UVs scaled by 0 -> uniform output).
 * -----------------------------------------------------------------------*/
extern u32 cellGcmResolveLocated(int local, u32 offset);

static struct {
    u32 dst_dma;      /* NV3062 0x0188 SET_CONTEXT_DMA_IMAGE_DESTIN */
    u32 color_fmt;    /* NV3062 0x0300 SET_COLOR_FORMAT             */
    u32 pitch;        /* NV3062 0x0304 SET_PITCH (dst in bits 16-31) */
    u32 dst_offset;   /* NV3062 0x030C SET_OFFSET_DESTIN            */
    u32 point;        /* NV308A 0x0304 SET_POINT ((y<<16)|x)        */
    u32 size_out;     /* NV308A 0x0308 SET_SIZE_OUT                 */
} s_gcm2d;

static struct {
    u32 src_dma, dst_surf, fmt, op, clip_pt, clip_sz, out_pt, out_sz;
    u32 ds_dx, dt_dy, in_sz, in_fmt, in_off, in_uv;
} s_nv3089;

/* NV309E "swizzled surface" -- the destination object for a scaled-image blit
 * whose target is a swizzled (Morton-ordered) texture rather than a linear
 * surface. Rubber Ducky binds it to the same subchannel this file previously
 * assumed was always NV308A (image-from-CPU), so its SET_OFFSET was being read
 * as an NV308A "point" and the blit destination was never picked up at all.
 * The two are told apart by method 0x0300: NV308A has no such method. */
static struct {
    u32 fmt;        /* 0x0300: (log2h << 24) | (log2w << 16) | format */
    u32 offset;     /* 0x0304: destination offset                     */
    int active;
    int subch;      /* which subchannel NV309E is bound to. Per-subchannel:
                     * this title has NV309E on 4 and NV308A on 5, and a global
                     * flag made 5's NV308A POINT (also 0x0304) be swallowed as
                     * an NV309E offset, breaking every inline transfer. */
} s_nv309e;

/* Execute one NV3089 blit into the currently-bound NV3062 destination surface.
 * Point-sampled: ds/dx and dt/dy are 20.12 fixed point and are 0x100000 (1.0)
 * for the straight uploads this exists for; a scaled blit still lands, just
 * without filtering. */
/* Morton/Z-order offset for an NV309E swizzled surface: interleave the low
 * min(l2w,l2h) bits of x and y, then append the remaining bits of the larger
 * dimension above them. */
static u32 nv309e_swz(u32 x, u32 y, u32 l2w, u32 l2h)
{
    u32 n = l2w < l2h ? l2w : l2h, off = 0;
    for (u32 i = 0; i < n; i++)
        off |= (((x >> i) & 1u) << (2 * i)) | (((y >> i) & 1u) << (2 * i + 1));
    if (l2w > l2h)      off |= (x >> n) << (2 * n);
    else if (l2h > l2w) off |= (y >> n) << (2 * n);
    return off;
}

/* IMAGE_IN_POINT is 12.4 fixed point per axis; DS_DX / DT_DY step the source in 20 fractional bits (1.0 = 0x100000).
 * The origin in the steps' scale is therefore origin << 16: an origin of one texel (0x10) is 0x100000. */
static inline u64 nv3089_origin(u32 p) { return (u64)p << 16; }

/* The draw engine renders on the host GPU, so the scene a title copies out
 * with NV3089 is not in guest memory, and what it copies in never reaches the
 * engine's surfaces. Around the guest-memory copy below: write the source
 * rectangle back from any engine surface first, and push the written
 * rectangle into any engine surface after. 32-bit pixels only. */
static void nv3089_engine_read(u32 in_pitch, u32 out_w, u32 out_h)
{
    if (!rsx_draw_engine_enabled() || !out_w || !out_h) return;
    const u32 u0 = s_nv3089.in_uv & 0xFFFF, v0 = s_nv3089.in_uv >> 16;   /* 12.4 */
    const u32 sx0 = (u32)(nv3089_origin(u0) >> 20), sy0 = (u32)(nv3089_origin(v0) >> 20);
    const u32 sx1 = (u32)((nv3089_origin(u0) + (u64)(out_w - 1) * s_nv3089.ds_dx) >> 20);
    const u32 sy1 = (u32)((nv3089_origin(v0) + (u64)(out_h - 1) * s_nv3089.dt_dy) >> 20);
    if (sx1 < sx0 || sy1 < sy0) return;
    const u32 loc = (s_nv3089.src_dma != 0xFEED0001u) ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN;
    rsx_draw_engine_guest_read(loc, s_nv3089.in_off + sy0 * in_pitch + sx0 * 4u, in_pitch,
                               sx1 - sx0 + 1u, sy1 - sy0 + 1u);
}

static void nv3089_engine_wrote(u32 dst_pitch, u32 x0, u32 y0, u32 x1, u32 y1)
{
    if (!rsx_draw_engine_enabled() || x1 <= x0 || y1 <= y0) return;
    const u32 loc = (s_gcm2d.dst_dma != 0xFEED0001u) ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN;
    rsx_draw_engine_guest_wrote(loc, s_gcm2d.dst_offset + y0 * dst_pitch + x0 * 4u, dst_pitch,
                                x1 - x0, y1 - y0);
}

/* A guest-memory row copy with the same reservation bookkeeping a coherent
 * PPU store does (ppu_loader.cpp VM_WRITE_COH), once per row instead of per
 * pixel: an SPU holding a reservation on a 128-byte line the row overwrites
 * loses it and gets its lost-reservation event. The two byte swaps a
 * vm_read32/vm_write32 pair does cancel, so the bytes move verbatim. */
static int gcm_span_reserved(u32 d, u32 n)
{
    const u32 l0 = d & ~127u, l1 = (d + n - 1) & ~127u;
    for (u32 a = l0; a <= l1; a += 128)
        if (spu_coh_is_reserved(a)) return 1;
    return 0;
}

static void gcm_span_written(u32 d, u32 n)
{
    const u32 l0 = d & ~127u, l1 = (d + n - 1) & ~127u;
    for (u32 a = l0; a <= l1; a += 128)
        if (spu_coh_is_reserved(a)) spu_coh_notify_write(a);
}

/* Store `n` host bytes at guest `d` with that bookkeeping. `s` may overlap the guest span (memmove). */
static void gcm_row_store(u32 d, const u8* s, u32 n)
{
    extern u8* vm_base;
    if (!gcm_span_reserved(d, n)) { memmove(vm_base + d, s, n); return; }
    spu_lockline_lock();
    memmove(vm_base + d, s, n);
    gcm_span_written(d, n);
    spu_lockline_unlock();
}

static void gcm_row_copy(u32 d, u32 s, u32 n)
{
    extern u8* vm_base;
    gcm_row_store(d, vm_base + s, n);
}

/* ---------------------------------------------------------------------------
 * NV0039 (M2MF buffer copy)
 *
 * BUFFER_NOTIFY (0x328) runs the copy the other registers describe: LINE_COUNT rows of LINE_LENGTH
 * elements. An element is one byte, read every `input increment` bytes and written every `output
 * increment` bytes (FORMAT bits 0-7 and 8-15: 1, 2 or 4 on this chip), and rows are PITCH apart, a signed
 * distance. Register layout and the 1/2/4 increments: envytools rnndb graph/nv_m2mf.xml. Row order and
 * overlap follow RPCS3 Emu/RSX/NV47/HW/nv0039.cpp (e53ac18f, read as a reference, nothing taken):
 *   - rows are copied in ascending order;
 *   - an overlapping plain copy behaves as if the whole source were read first (RPCS3: memmove for a
 *     block, a packed temporary for rows);
 *   - an overlapping copy with an increment above 1 is NOT snapshotted: the elements move one at a time
 *     and a later element can read what an earlier one wrote.
 * RPCS3 also accepts increments 3, and a zero-sized copy is a no-op there; this one stops on a bad
 * increment (even for a zero-sized copy) and does nothing for an empty one.
 * -----------------------------------------------------------------------*/
#define GCM_M2MF_FN "cellGcm_rsx_process_fifo"

static u32 gcm_m2mf_inc(u32 inc, const char* which)
{
    if (inc == 1u || inc == 2u || inc == 4u) return inc;
    char why[128];
    snprintf(why, sizeof why, "NV0039 FORMAT %s increment is %u; only 1, 2 and 4 are modelled", which, inc);
    ps3_stop_gcm_unmodelled(GCM_M2MF_FN, "m2mf-increment", why);
}

static u32 gcm_m2mf_location(u32 dma, const char* which)
{
    if (dma == 0xFEED0000u) return RSX_CAP_LOC_LOCAL;
    if (dma == 0xFEED0001u) return RSX_CAP_LOC_MAIN;
    char why[128];
    snprintf(why, sizeof why, "NV0039 %s DMA object 0x%08X is neither local (0xFEED0000) nor main (0xFEED0001)",
             which, dma);
    ps3_stop_gcm_unmodelled(GCM_M2MF_FN, "m2mf-dma", why);
}

/* The guest EA of RSX offset `lo` for the whole extent [lo, hi), which must lie in the 32-bit offset
 * space and map to one contiguous run of guest memory. Every bound is summed in 64 bits, before the first
 * byte moves, so nothing wraps. */
static u64 gcm_m2mf_extent(const char* which, u32 loc, int64_t lo, int64_t hi)
{
    const u64 size = (u64)(hi - lo);
    if (lo < 0 || hi > (int64_t)0x100000000ll)
        ps3_stop_guest_range(GCM_M2MF_FN, which, (u64)lo, size, 0x100000000ull);
    u64 ea;
    if (loc == RSX_CAP_LOC_LOCAL) {
        ea = cellGcmResolveLocated(1, (u32)lo);
        if (ea != (u64)s_config.localAddress + (u64)lo)    /* the helper's 32-bit sum wrapped */
            ps3_stop_guest_range(GCM_M2MF_FN, which, (u64)s_config.localAddress + (u64)lo, size, gcm_vm_limit());
    } else {
        ea = cellGcmResolveLocated(0, (u32)lo);
        if (!ea) ps3_stop_guest_range(GCM_M2MF_FN, which, 0, size, gcm_vm_limit());
        /* IO pages map independently: a span that crosses one is contiguous in guest memory only if the
         * next page maps to the next 1 MiB. */
        const u64 first_page_ea = ea - ((u64)lo & 0xFFFFFull);
        for (u64 p = ((u64)lo >> 20) + 1; p <= ((u64)hi - 1) >> 20; p++)
            if (cellGcmResolveLocated(0, (u32)(p << 20)) != first_page_ea + ((p - ((u64)lo >> 20)) << 20))
                ps3_stop_guest_range(GCM_M2MF_FN, which, ea, size, gcm_vm_limit());
    }
    if (!gcm_span_ok(ea, size)) ps3_stop_guest_range(GCM_M2MF_FN, which, ea, size, gcm_vm_limit());
    return ea;
}

static void gcm_nv0039_copy(void)
{
    extern u8* vm_base;
    const u32 in_inc  = gcm_m2mf_inc(s_m2mf.format & 0xFFu, "input");
    const u32 out_inc = gcm_m2mf_inc((s_m2mf.format >> 8) & 0xFFu, "output");
    const u64 cols = s_m2mf.line_len, rows = s_m2mf.line_count;
    if (!cols || !rows) return;
    const u32 in_loc  = gcm_m2mf_location(s_m2mf.dma_in, "input");
    const u32 out_loc = gcm_m2mf_location(s_m2mf.dma_out, "output");

    /* The bytes each side touches: the first row's offset plus the rows' signed spread, plus one row's
     * reach. Whole spans are resolved and bounded before any byte moves. */
    const int64_t pin = (int32_t)s_m2mf.pitch_in, pout = (int32_t)s_m2mf.pitch_out;
    const int64_t sback = (int64_t)(rows - 1) * pin, dback = (int64_t)(rows - 1) * pout;
    const int64_t smin = sback < 0 ? sback : 0, dmin = dback < 0 ? dback : 0;
    const int64_t srow = (int64_t)((cols - 1) * in_inc + 1), drow = (int64_t)((cols - 1) * out_inc + 1);
    const int64_t slo = (int64_t)s_m2mf.off_in + smin, shi = (int64_t)s_m2mf.off_in + (sback > 0 ? sback : 0) + srow;
    const int64_t dlo = (int64_t)s_m2mf.off_out + dmin, dhi = (int64_t)s_m2mf.off_out + (dback > 0 ? dback : 0) + drow;
    const u64 sea = gcm_m2mf_extent("m2mf-source", in_loc, slo, shi);
    const u64 dea = gcm_m2mf_extent("m2mf-destination", out_loc, dlo, dhi);
    const u64 slen = (u64)(shi - slo), dlen = (u64)(dhi - dlo);
    if (slen > 0xFFFFFFFFull) ps3_stop_guest_range(GCM_M2MF_FN, "m2mf-source", sea, slen, 0xFFFFFFFFull);
    if (dlen > 0xFFFFFFFFull) ps3_stop_guest_range(GCM_M2MF_FN, "m2mf-destination", dea, dlen, 0xFFFFFFFFull);

    /* The source is consumed through the capture when one is open, so what moves is what was recorded. */
    const u8* sv = vm_base + sea;
    if (s_cap)
        sv = s_cap->read(in_loc, (u32)slo, (u32)sea, vm_base + sea, (u32)slen, RSX_CAP_READ_DMA_SOURCE);

    const int overlap = sea < dea + dlen && dea < sea + slen;
    if (in_inc != 1u || out_inc != 1u) {
        const u8* s = overlap ? vm_base + sea : sv;    /* overlapping: see the header comment */
        for (int64_t r = 0; r < (int64_t)rows; r++) {
            const u8* sr = s + (r * pin - smin);
            const u32 d = (u32)(dea + (u64)(r * pout - dmin));
            const int reserved = gcm_span_reserved(d, (u32)drow);
            if (reserved) spu_lockline_lock();
            for (u64 c = 0; c < cols; c++) vm_base[d + c * out_inc] = sr[c * in_inc];
            if (reserved) { gcm_span_written(d, (u32)drow); spu_lockline_unlock(); }
        }
    } else {
        const int block = pin == (int64_t)cols && pout == (int64_t)cols;
        u8* tmp = NULL;
        if (overlap) {
            tmp = (u8*)malloc((size_t)(cols * rows));
            if (!tmp) {
                char why[96];
                snprintf(why, sizeof why, "no host memory for the %llu-byte overlap buffer", cols * rows);
                ps3_stop_gcm_unmodelled(GCM_M2MF_FN, "m2mf-overlap", why);
            }
            for (int64_t r = 0; r < (int64_t)rows; r++)
                memcpy(tmp + (u64)r * cols, sv + (r * pin - smin), (size_t)cols);
            sv = tmp;
        }
        if (block) {
            gcm_row_store((u32)dea, sv, (u32)(cols * rows));
        } else {
            for (int64_t r = 0; r < (int64_t)rows; r++)
                gcm_row_store((u32)(dea + (u64)(r * pout - dmin)),
                              overlap ? tmp + (u64)r * cols : sv + (r * pin - smin), (u32)cols);
        }
        free(tmp);
    }
    if (s_cap) s_cap->write(out_loc, (u32)dlo, (u32)dea, vm_base + dea, (u32)dlen, RSX_CAP_WRITE_DMA);
    atomic_fetch_add_explicit(&s_m2mf_copies, 1, memory_order_relaxed);
    { static int n = 0;
      if (PS3_LOG_FIRST(n, 4))
          fprintf(stderr, "[NV0039] %llux%llu inc %u/%u: %s 0x%08X pitch %d -> %s 0x%08X pitch %d%s\n", cols, rows,
                  in_inc, out_inc, in_loc ? "main" : "local", s_m2mf.off_in, (int)pin,
                  out_loc ? "main" : "local", s_m2mf.off_out, (int)pout, overlap ? " (overlap)" : ""); }
}

/* One NV0039 method. 1 when it was consumed. DMA_NOTIFY (0x180) is allow-listed inert in
 * rsx_method_class.h; the rest of the object's methods have no register here. */
static int gcm_nv0039_method(u32 method, u32 data)
{
    switch (method) {
    case 0x0000: return 1;                          /* SET_OBJECT: bound by the walker */
    case 0x0184: s_m2mf.dma_in = data; return 1;     /* DMA_BUFFER_IN */
    case 0x0188: s_m2mf.dma_out = data; return 1;    /* DMA_BUFFER_OUT */
    case 0x030C: s_m2mf.off_in = data; return 1;     /* OFFSET_IN */
    case 0x0310: s_m2mf.off_out = data; return 1;    /* OFFSET_OUT */
    case 0x0314: s_m2mf.pitch_in = data; return 1;   /* PITCH_IN */
    case 0x0318: s_m2mf.pitch_out = data; return 1;  /* PITCH_OUT */
    case 0x031C: s_m2mf.line_len = data; return 1;   /* LINE_LENGTH_IN */
    case 0x0320: s_m2mf.line_count = data; return 1; /* LINE_COUNT */
    case 0x0324: s_m2mf.format = data; return 1;     /* FORMAT */
    case 0x0328: gcm_nv0039_copy(); return 1;        /* BUFFER_NOTIFY: the trigger */
    }
    return 0;
}

static void nv3089_blit(void)
{
    u32 out_w = s_nv3089.out_sz & 0xFFFF, out_h = s_nv3089.out_sz >> 16;
    u32 out_x = s_nv3089.out_pt & 0xFFFF, out_y = s_nv3089.out_pt >> 16;
    u32 in_pitch = s_nv3089.in_fmt & 0xFFFF;
    u32 dst_pitch = s_gcm2d.pitch >> 16;
    if (!out_w || !out_h || !in_pitch || !dst_pitch) return;
    /* NV3089_GATE=<file>: log every blit's geometry while <file> exists, so a
     * capture covers the scene of interest instead of the first N since boot. */
    { static const char* gate = (const char*)-1; static unsigned poll; static int open_gate;
      if (gate == (const char*)-1) gate = getenv("NV3089_GATE");
      if (gate) {
          if ((poll++ & 63u) == 0) { FILE* g = fopen(gate, "rb"); open_gate = g != NULL; if (g) fclose(g); }
          if (open_gate) {
              int _sl = (s_nv3089.src_dma != 0xFEED0001u), _dl = (s_gcm2d.dst_dma != 0xFEED0001u);
              printf("[NV3089G] fmt=0x%X out=%ux%u at %u,%u in=%ux%u uv=0x%08X ds=0x%X dt=0x%X src=0x%08X/%u dst=0x%08X/%u swz=%d%c",
                     s_nv3089.fmt & 0xFF, out_w, out_h, out_x, out_y,
                     s_nv3089.in_sz & 0xFFFF, s_nv3089.in_sz >> 16, s_nv3089.in_uv,
                     s_nv3089.ds_dx, s_nv3089.dt_dy,
                     cellGcmResolveLocated(_sl, s_nv3089.in_off), in_pitch,
                     cellGcmResolveLocated(_dl, s_gcm2d.dst_offset), dst_pitch,
                     s_nv309e.active, 10);
          }
      } }
    /* A copy out of a surface the live renderer drew exists only on the GPU;
     * mirror 1:1 blits there too (the guest-memory copy below still runs). */
    if (s_nv3089.ds_dx == 0x100000u && s_nv3089.dt_dy == 0x100000u &&
        rsx_live_draw_enabled()) {
        const u32 u0 = (s_nv3089.in_uv & 0xFFFF) >> 4, v0 = (s_nv3089.in_uv >> 16) >> 4;
        const int sl = (s_nv3089.src_dma != 0xFEED0001u);
        const int dl = (s_gcm2d.dst_dma  != 0xFEED0001u);
        rsx_live_draw_blit(sl ? 0u : 1u, s_nv3089.in_off + v0 * in_pitch + u0 * 4u, in_pitch,
                           dl ? 0u : 1u, s_gcm2d.dst_offset + (s_nv3089.out_pt >> 16) * dst_pitch
                                         + (s_nv3089.out_pt & 0xFFFF) * 4u, dst_pitch,
                           out_w, out_h);
    } else if (rsx_live_draw_enabled()) {
        /* Scaled: a resolve. Copy it on the GPU into the display buffer; fall
         * back to presenting the source when that cannot be done. */
        const u32 sl = s_nv3089.src_dma != 0xFEED0001u ? 0u : 1u;
        const u32 dl = s_gcm2d.dst_dma  != 0xFEED0001u ? 0u : 1u;
        const u32 u0 = (s_nv3089.in_uv & 0xFFFF) >> 4, v0 = (s_nv3089.in_uv >> 16) >> 4;
        const int rok = rsx_live_draw_resolve_blit(sl, s_nv3089.in_off + v0 * in_pitch + u0 * 4u, in_pitch,
                                        s_nv3089.ds_dx, s_nv3089.dt_dy, dl, s_gcm2d.dst_offset,
                                        dst_pitch, out_x, out_y, out_w, out_h);
        if (!rok && s_nv3089.in_uv <= 0x00100010u && !s_nv3089.out_pt)
            rsx_live_draw_note_resolve(sl, s_nv3089.in_off, dl, s_gcm2d.dst_offset);
    }
    /* Only the 32-bit colour formats are handled; anything else would need a
     * per-format converter and is better skipped loudly than written wrong. */
    u32 f = s_nv3089.fmt & 0xFF;
    /* NOTE: format 0x3 appears here as a downsample chain (1024x512 -> 1024x256
     * -> 512x128 -> 256x64 -> 128x32, all into one destination) that builds a
     * mip/blur pyramid, and skipping it leaves that texture empty. Treating it
     * as 4-byte A8R8G8B8 like 7/8/0xD is WRONG -- it floods the scene blue
     * (flat-blue coverage 17k -> 537k pixels), so its pixel layout differs.
     * Decode it properly before enabling; do not just add it to this list. */
    /* NV309E swizzled destination. Colour format 0x3 is A8R8G8B8 in the
     * scaled-image enum; this title uses it to build a mip/blur pyramid into a
     * swizzled texture (1024x512 -> 1024x256 -> ... all through NV309E).
     * Writing it LINEARLY into the NV3062 destination is what flooded the scene
     * blue -- wrong destination and wrong layout. */
    { u32 l2w = (s_nv309e.fmt >> 16) & 0xFF, l2h = (s_nv309e.fmt >> 24) & 0xFF;
      static int en = -1;
      /* On by default: this is the mip/blur pyramid. Without it textures have
       * no mip levels, which shows up as aliased blocky tiles and a noise patch
       * where a minified surface should be, and the shell that samples the
       * pyramid draws untextured. NV309E_BLIT=0 disables. */
      if (en < 0) { const char* e = getenv("NV309E_BLIT"); en = e ? atoi(e) : 1; }
      if (en && f == 3 && s_nv309e.active && s_nv309e.offset &&
          (1u << l2w) == out_w && (1u << l2h) == out_h) {
        int sl = (s_nv3089.src_dma != 0xFEED0001u);
        u32 src2 = cellGcmResolveLocated(sl, s_nv3089.in_off);
        u32 dst2 = cellGcmResolveLocated(1, s_nv309e.offset);
        u32 u0b = s_nv3089.in_uv & 0xFFFF, v0b = s_nv3089.in_uv >> 16;
        nv3089_engine_read(in_pitch, out_w, out_h);
        /* Copy words verbatim through raw pointers rather than vm_read32/
         * vm_write32: this moves ~500k pixels per blit and several blits run per
         * frame, and the two byte swaps would cancel anyway. */
        { extern u8* vm_base;
          const u8* sbase = vm_base + src2;
          u8* dbase = vm_base + dst2;
          for (u32 y = 0; y < out_h; y++) {
              u64 sv = nv3089_origin(v0b) + (u64)y * s_nv3089.dt_dy;
              const u8* srow = sbase + (u32)(sv >> 20) * in_pitch;
              for (u32 x = 0; x < out_w; x++) {
                  u64 su = nv3089_origin(u0b) + (u64)x * s_nv3089.ds_dx;
                  memcpy(dbase + (u64)nv309e_swz(x, y, l2w, l2h) * 4,
                         srow + (u32)(su >> 20) * 4, 4);
              }
          } }
        { static int _n = 0; static int cap = -1;
          if (cap < 0) { const char* e = getenv("NV3089_DBG"); cap = e ? atoi(e) : 0; }
          if (cap && _n < cap) { _n++;
            printf("[NV3089] swizzled %ux%u fmt=0x%X src=0x%08X -> NV309E dst=0x%08X%c",
                   out_w, out_h, f, src2, dst2, 10); } }
        return;
      } }
    /* 0x3 is A8R8G8B8 in the scaled-image format enum. This title uses it for
     * two different things: the swizzled mip/blur pyramid handled above, and
     * (with a LINEAR NV3062 destination) copies of the rendered scene surface
     * into the textures its water and effects sample -- e.g.
     * src=0xCE340000 (the scene) -> dst=0xC3746200, which is the single most
     * sampled blended texture in the frame. Skipping those left every one of
     * those textures empty. Same 4-byte pixel layout as the formats already
     * handled. */
    if (f != 3 /* A8R8G8B8 (scale-format enum) */ &&
        f != 7 /* A8R8G8B8 */ && f != 8 /* X8R8G8B8 */ && f != 0xD /* A8B8G8R8 */) {
        /* Log the GEOMETRY of a skipped blit, not just the format: a full-screen
         * copy from the scene surface to a registered display buffer is the
         * composite this title needs, and is worth telling apart from some
         * incidental small transfer. */
        { int _sl = (s_nv3089.src_dma != 0xFEED0001u);
          int _dl = (s_gcm2d.dst_dma  != 0xFEED0001u);
          printf("[NV3089] SKIPPED fmt=0x%X  %ux%u  src=0x%08X(pitch %u,%s) -> dst=0x%08X(pitch %u,%s) at %u,%u%c",
                 f, out_w, out_h,
                 cellGcmResolveLocated(_sl, s_nv3089.in_off), in_pitch, _sl?"LOCAL":"MAIN",
                 cellGcmResolveLocated(_dl, s_gcm2d.dst_offset), dst_pitch, _dl?"LOCAL":"MAIN",
                 out_x, out_y, 10); }
        static int _w = 0;
        if (PS3_LOG_FIRST(_w, 4)) printf("[NV3089] unsupported colour format 0x%X, blit skipped\n", f);
        return;
    }
    int src_local = (s_nv3089.src_dma != 0xFEED0001u);
    int dst_local = (s_gcm2d.dst_dma  != 0xFEED0001u);
    u32 src = cellGcmResolveLocated(src_local, s_nv3089.in_off);
    u32 dst = cellGcmResolveLocated(dst_local, s_gcm2d.dst_offset);
    u32 u0 = s_nv3089.in_uv & 0xFFFF, v0 = s_nv3089.in_uv >> 16;   /* 12.4 start */
    /* Clip to SET_CLIP_POINT/SIZE (destination coordinates). A title tiling a
     * resolve in 512x512 blocks relies on it to stop at the surface edge:
     * Tornado Outbreak's bottom band is 512 rows tall with 208 left in the
     * display buffer, and writing all 512 ran on into the VRAM after it --
     * where its full-screen quad's vertices were staged, so the quad drew with
     * all-zero positions. A zero clip size means unclipped. */
    u32 cx0 = s_nv3089.clip_pt & 0xFFFF, cy0 = s_nv3089.clip_pt >> 16;
    u32 cx1 = (s_nv3089.clip_sz & 0xFFFF) ? cx0 + (s_nv3089.clip_sz & 0xFFFF) : 0xFFFFFFFFu;
    u32 cy1 = (s_nv3089.clip_sz >> 16)    ? cy0 + (s_nv3089.clip_sz >> 16)    : 0xFFFFFFFFu;
    /* Bytes per pixel from the SDK enums (CELL_GCM_TRANSFER_SCALE_FORMAT_*:
     * 7 = R5G6B5, 8 = Y8 -- not 32-bit as they were once labelled). When the
     * destination surface format agrees it is a plain copy of that width; a
     * real format conversion keeps the old 4-byte copy rather than guess. */
    u32 sbpp = (f == 7) ? 2u : (f == 8) ? 1u : 4u;
    u32 dbpp = (s_gcm2d.color_fmt == 4u /* R5G6B5 */) ? 2u
             : (s_gcm2d.color_fmt == 1u /* Y8 */)     ? 1u : 4u;
    u32 bpp = (sbpp == dbpp) ? sbpp : 4u;
    if (bpp == 4) nv3089_engine_read(in_pitch, out_w, out_h);
    /* NV3089_ROWCOPY (default 1): an unscaled blit copies a row at a time.
     * The per-pixel loop pays a vm_read32 and a vm_write32 per pixel -- the
     * bounds and null-store checks, the store hooks, a reservation lookup and
     * two byte swaps that cancel -- and a title that copies 1280x720 surfaces
     * to main memory every frame (inFAMOUS 2 does, for its SPU lighting)
     * spent a quarter of the FIFO thread there. NV3089_ROWCOPY=0 keeps the
     * per-pixel copy, which is what the PPU store watches see. */
    static int rowcopy = -1;
    if (rowcopy < 0) { const char* e = getenv("NV3089_ROWCOPY"); rowcopy = e ? atoi(e) : 1; }
    if (rowcopy && s_nv3089.ds_dx == 0x100000u && s_nv3089.dt_dy == 0x100000u) {
        /* the source start in whole texels: IMAGE_IN_POINT is 12.4 */
        const u32 sx0 = (u32)(nv3089_origin(u0) >> 20), sy0 = (u32)(nv3089_origin(v0) >> 20);
        u32 xa = 0, xb = out_w;                       /* destination x range inside the clip */
        if (out_x < cx0) xa = cx0 - out_x;
        if ((u64)out_x + xb > cx1) xb = cx1 > out_x ? cx1 - out_x : 0;
        if (xa < xb) {
            const u32 n = (xb - xa) * bpp;
            for (u32 y = 0; y < out_h; y++) {
                const u32 dy = out_y + y;
                if (dy < cy0 || dy >= cy1) continue;
                const u64 sa = (u64)src + (u64)(sy0 + y) * in_pitch + (u64)(sx0 + xa) * bpp;
                const u64 da = (u64)dst + (u64)dy * dst_pitch + (u64)(out_x + xa) * bpp;
                if (sa + n > 0x100000000ull || da + n > 0x100000000ull) continue;
                gcm_row_copy((u32)da, (u32)sa, n);
            }
        }
    } else
    for (u32 y = 0; y < out_h; y++) {
        u32 dy = out_y + y;
        if (dy < cy0 || dy >= cy1) continue;
        u64 sv = nv3089_origin(v0) + (u64)y * s_nv3089.dt_dy;
        u32 sy = (u32)(sv >> 20);
        for (u32 x = 0; x < out_w; x++) {
            u32 dx = out_x + x;
            if (dx < cx0 || dx >= cx1) continue;
            u64 su = nv3089_origin(u0) + (u64)x * s_nv3089.ds_dx;
            u32 sx = (u32)(su >> 20);
            u32 s = src + sy * in_pitch + sx * bpp;
            u32 d = dst + dy * dst_pitch + dx * bpp;
            if (bpp == 4)      vm_write32(d, vm_read32(s));
            else if (bpp == 2) vm_write16(d, vm_read16(s));
            else               vm_write8(d, vm_read8(s));
        }
    }
    if (bpp == 4) {
        const u32 x0 = out_x > cx0 ? out_x : cx0, y0 = out_y > cy0 ? out_y : cy0;
        const u32 x1 = (out_x + out_w < cx1) ? out_x + out_w : cx1;
        const u32 y1 = (out_y + out_h < cy1) ? out_y + out_h : cy1;
        nv3089_engine_wrote(dst_pitch, x0, y0, x1, y1);
    }
    { static int _n = 0; static int cap = -1;
      if (cap < 0) { const char* e = getenv("NV3089_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && _n < cap) { _n++;
        printf("[NV3089] %ux%u fmt=0x%X src=0x%08X(pitch %u) -> dst=0x%08X(pitch %u) at %u,%u\n",
               out_w, out_h, f, src, in_pitch, dst, dst_pitch, out_x, out_y); } }
}

static int gcm_2d_method(u32 subch, rsx_class_id cls, u32 method, u32 data)
{
    /* GCM2D_TRACE=1: histogram of (subchannel, method) actually seen. The subchannel a title binds each 2D
     * object to is libgcm-version-specific; the histogram says which slot carried what. */
    { static int tr = -1;
      if (tr < 0) { const char* e = getenv("GCM2D_TRACE"); tr = e ? atoi(e) : 0; }
      if (tr) {
          static u8 seen[8][1024]; static u64 n = 0;
          u32 mi = (method >> 2) & 1023;
          if (subch < 8 && !seen[subch][mi]) {
              seen[subch][mi] = 1;
              printf("[GCM2D-TRACE] subch=%u method=0x%04X (first, data=0x%08X)%c",
                     subch, method, data, 10);
          }
          if (++n == 400000ull) {
              printf("[GCM2D-TRACE] --- subchannels used: ");
              for (u32 sc = 0; sc < 8; sc++) {
                  u32 c = 0; for (u32 m = 0; m < 1024; m++) c += seen[sc][m];
                  if (c) printf("%u(%u methods) ", sc, c);
              }
              printf("---%c", 10);
          }
      } }
    /* Routed by the class bound on the slot, whatever subchannel number the title picked: demosaic's SDK
     * emits dest-surface on sub 3 and image-from-CPU on sub 5 (cellGcmSetInlineTransfer: 0x4630C / 0xCA304 /
     * 0xA400 headers), other versions use the classic 2 / 4. Only the four 2D classes reach here. Returns 1
     * when a case below consumed the method; the caller turns that into a verdict. */
    if (cls == RSX_CLASS_NV3062) {          /* context surface 2D */
        switch (method) {
        case 0x0184: case 0x0188: s_gcm2d.dst_dma = data; return 1;
        case 0x0300: s_gcm2d.color_fmt  = data; return 1;
        case 0x0304: s_gcm2d.pitch      = data; return 1;
        case 0x030C: s_gcm2d.dst_offset = data; return 1;
        }
        return 0;
    }
    if (cls == RSX_CLASS_NV309E) {          /* swizzled surface */
        switch (method) {
        case 0x0300:
            s_nv309e.fmt = data; s_nv309e.active = 1;
            s_nv309e.subch = (int)subch;
            { static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
                printf("[NV309E] format=0x%08X (log2 %ux%u fmt=0x%X)%c", data,
                       1u << ((data >> 16) & 0xFF), 1u << ((data >> 24) & 0xFF),
                       data & 0xFFFF, 10); }
            return 1;
        case 0x0304: s_nv309e.offset = data; return 1;
        }
        return 0;
    }
    /* NV308A image-from-CPU. Its own 0x0300 is SET_COLOR_FORMAT, not the swizzled surface's SET_FORMAT:
     * the class says which one a word is (gcm_default_state.c writes it on this slot, ahead of the
     * NV309E bind). */
    if (cls == RSX_CLASS_NV308A) {
        switch (method) {
        case 0x0304: s_gcm2d.point    = data; return 1;
        case 0x0308: s_gcm2d.size_out = data; return 1;
        /* SIZE_IN: ignored since the first NV308A path, with no evidence that nothing depends on it, so it
         * is not allow-listed: it counts as an unknown method until S1 decides. */
        case 0x030C: return 0;
        }
        if (method >= 0x0400 && method <= 0x07FC) {
            /* COLOR data word: write into the destination surface. Inline
             * transfers use format Y32 (4 bytes/px, one row per point.y).
             * DMA 0xFEED0000 = local memory, 0xFEED0001 = main memory. */
            u32 idx = (method - 0x0400) >> 2;
            u32 px  = (s_gcm2d.point & 0xFFFF) + idx;
            u32 py  = s_gcm2d.point >> 16;
            u32 dst_pitch = s_gcm2d.pitch >> 16;
            int local = (s_gcm2d.dst_dma != 0xFEED0001u);
            u32 base = cellGcmResolveLocated(local, s_gcm2d.dst_offset);
            { static int _it = 0;
              if (PS3_LOG_FIRST(_it, 6))
                  printf("[GCM2D] inline write dst=0x%08X (off=0x%X dma=0x%X pt=%u,%u pitch=%u) = 0x%08X\n",
                         base + py * dst_pitch + px * 4, s_gcm2d.dst_offset,
                         s_gcm2d.dst_dma, px, py, dst_pitch, data); }
            vm_write32(base + py * dst_pitch + px * 4, data);
            return 1;
        }
        return 0;
    }
    /* NV3089 "scaled image from memory" -- the blit PSGL uses to move a texture
     * it has assembled elsewhere into the VRAM the sampler reads. Without it a
     * title's textures stay whatever the destination was before (zero), so
     * geometry renders flat no matter how correct the sampler is.
     *
     * Registers: 0x0184 source DMA, 0x0198 destination surface (the NV3062
     * state above), 0x0300 colour format, 0x0304 operation, 0x0308/0x030C clip
     * point/size, 0x0310/0x0314 out point/size, 0x0318/0x031C ds/dx dt/dy in
     * 20.12 fixed point, 0x0400 in size, 0x0404 (origin<<16)|pitch, 0x0408 in
     * offset, 0x040C in u/v start -- which is also the trigger. */
    if (cls == RSX_CLASS_NV3089) {
        switch (method) {
        case 0x0184: s_nv3089.src_dma   = data; return 1;
        case 0x0198: s_nv3089.dst_surf  = data; return 1;
        case 0x0300: s_nv3089.fmt       = data; return 1;
        case 0x0304: s_nv3089.op        = data; return 1;
        case 0x0308: s_nv3089.clip_pt   = data; return 1;
        case 0x030C: s_nv3089.clip_sz   = data; return 1;
        case 0x0310: s_nv3089.out_pt    = data; return 1;
        case 0x0314: s_nv3089.out_sz    = data; return 1;
        case 0x0318: s_nv3089.ds_dx     = data; return 1;
        case 0x031C: s_nv3089.dt_dy     = data; return 1;
        case 0x0400: s_nv3089.in_sz     = data; return 1;
        case 0x0404: s_nv3089.in_fmt    = data; return 1;
        case 0x0408: s_nv3089.in_off    = data; return 1;
        case 0x040C: s_nv3089.in_uv     = data; nv3089_blit(); return 1;
        }
        return 0;
    }
    return 0;
}

/* Present gate for the ticker: a flip is ready once the drain has consumed
 * everything up to put. The guest blocks in its own WaitFlip right after
 * flipping, so the FIFO holds EXACTLY the completed frame -- no guest-side
 * blocking needed (the old spin serialized two vsync-class waits per frame
 * and halved the frame rate). Call AFTER draining. */
int cellGcm_take_flip_pending_synced(void)
{
    if (!s_flip_pending) return 0;
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    u32 get = vm_read32(GCM_CONTROL_GUEST_ADDR + 4);
    if (get != put && !s_acquire_blocked) return 0;   /* frame not fully drained yet */
    s_flip_pending = 0;
    return 1;
}

/* ---- fence (SET_REFERENCE) observability ---------------------------------
 * cellGcmFinish waits for ctrl->ref with EQUALITY. On hardware every fence
 * value persists in the register for the RSX-time until the next fence
 * (~a frame), so an equality poll cannot miss it. Our drain consumes a whole
 * backlog in microseconds; publishing only the final value made intermediate
 * fences invisible -- LBP's boot deadlocked with a thread spinning on a fence
 * the counter had already passed (waiting 0x13 while ref climbed 0x51->0x1F4).
 * Queue every fence as it is drained and publish ONE per tick: the GPU work
 * is never throttled (draw/recycle proceed at full speed), only the ref
 * register advances at most one fence per tick, so every value is observable
 * for >= 16 ms, like hardware. (Throttling the DRAIN at fence boundaries
 * instead starved wrap recycles and wedged the backend -- don't.) */
#define GCM_REF_QLEN 4096
static u32 s_ref_q[GCM_REF_QLEN];
static volatile u32 s_ref_qhead = 0, s_ref_qtail = 0;   /* single producer+consumer: the ticker */

/* A JUMP/CALL whose target has no IO mapping must not be taken. Taking one
 * teleports the walker into unmapped space, where it can never reach `put`
 * again: the FIFO silently stops being processed, no fences are published, and
 * a title spinning on ctrl->ref waits forever with no indication why. Tokyo
 * Jungle did exactly this -- get left the 1 MB ring for IO 0x00F00000, which it
 * never mapped, while put sat at 0x604. Stop at the bad word and say so. */
static void gcm_fifo_bad_branch(const char* kind, u32 target, u32 word)
{
    static int n = 0;
    if (PS3_LOG_FIRST(n, 8))
        fprintf(stderr, "[cellGcmSys] FIFO %s to unmapped IO 0x%08X (word 0x%08X) "
                "-- not taken, drain stops here\n", kind, target, word);
}

/* A bad branch nearly always means the walker mis-counted a method batch a few
 * dwords back and is now reading vertex data -- a float 1.0f is 0x3F800000,
 * which decodes as JUMP 0x1F800000. Dump the run-up: the command that consumed
 * the wrong count is in here. */
/* Last words the walker actually consumed, so a bad branch can show the decode
 * chain that led there rather than raw memory around it. */
#define GCM_TRACE_N 64
static u32 s_tr_off[GCM_TRACE_N], s_tr_w[GCM_TRACE_N];
static u32 s_cf_off[8], s_cf_w[8], s_cf_i;   /* last JUMP/CALL words, for GCM_CIRCLE_DUMP */
static u32 s_tr_i = 0;

/* A branch we cannot take leaves the walker pointing at whatever it
 * mis-decoded, and it never recovers -- get freezes for the rest of the
 * run and nothing renders. Resynchronise to `put` instead, the same trade
 * the unmapped-get path already makes: the commands between here and put
 * are lost, but the next frame is written from a clean boundary and the
 * FIFO lives. */
static void gcm_ref_push_at(u32 v, u32 getoff);

/* ---- SPU-built pushbuffers ------------------------------------------------
 * Some titles do not build the RSX command stream on the PPU at all. The
 * Orange Box is one: its libgcm context points at a 64 KB buffer in the
 * binary's own .bss that is never mapped into IO space, because that buffer is
 * where the PPU records draw commands for an SPU to consume. CB.SPU reads it
 * and DMAs the real pushbuffer into mapped main memory, then `put` is set past
 * the end of what it wrote.
 *
 * So libgcm is never told where the stream is, and the walker -- which starts
 * at IO 0 and has no JUMP to follow, because the SPU side did the binding --
 * scans tens of megabytes of unrelated memory, NOPs through the zeros (a zero
 * word is a valid NOP) and dies on the first vertex float it meets.
 *
 * The DMA is the only evidence there is, so record it. Each PUT that lands in
 * a mapped IO range is a candidate pushbuffer; the one that ends just short of
 * `put` is the block the RSX is actually being pointed at. */
#define GCM_SPU_BLK_N 16
static struct { u32 io_start, io_end; } s_spu_blk[GCM_SPU_BLK_N];
static int s_spu_blk_w = 0;

/* EA -> IO through the mapping list (range lookup; the by-ea helper is exact). */
static u32 gcm_ea2io_range(u32 ea, u32* room)
{
    /* Called for every SPU PUT. The table has CELL_GCM_MAX_IO_MAPPINGS slots
     * and a title uses a handful, all below the high-water mark; the first
     * match in slot order is the one a full scan returns. */
    const int n = __atomic_load_n(&s_io_mapping_hw, __ATOMIC_RELAXED);
    for (int i = 0; i < n; i++) {
        IoMapping* m = &s_io_mappings[i];
        if (!m->active || !m->size) continue;
        if (ea >= m->ea && ea < m->ea + m->size) {
            if (room) *room = m->ea + m->size - ea;
            return m->io + (ea - m->ea);
        }
    }
    return 0xFFFFFFFFu;
}

void gcm_forget_spu_blocks(void)
{
    memset(s_spu_blk, 0, sizeof s_spu_blk);
    s_spu_blk_w = 0;
}

void gcm_note_spu_put(u32 ea, u32 size)
{
    if (!size || size > 0x40000u) return;
    u32 room = 0;
    u32 io = gcm_ea2io_range(ea, &room);
    if (io == 0xFFFFFFFFu || size > room) return;   /* not in RSX-visible memory */

    /* Coalesce with the previous block when an SPU emits one buffer in several
     * transfers -- otherwise only the last fragment is remembered and the
     * walker still starts in the middle of the stream. */
    int prev = (s_spu_blk_w + GCM_SPU_BLK_N - 1) % GCM_SPU_BLK_N;
    if (s_spu_blk[prev].io_end == io) { s_spu_blk[prev].io_end = io + size; return; }

    s_spu_blk[s_spu_blk_w].io_start = io;
    s_spu_blk[s_spu_blk_w].io_end   = io + size;
    s_spu_blk_w = (s_spu_blk_w + 1) % GCM_SPU_BLK_N;
}

/* The recorded block the RSX is being pointed at, or 0xFFFFFFFF.
 *
 * `put` sits at or just past the end of the block the title wants executed --
 * the gap is whatever libgcm appended after the SPU's data, a fence and its
 * argument in this title's case. Require the block to END within a small window
 * before `put` and to START before it, so an older double-buffered block (the
 * SPU alternates between two) is not chosen over the current one. */
static u32 gcm_spu_block_for_put(u32 put)
{
    u32 best = 0xFFFFFFFFu, best_end = 0;
    for (int i = 0; i < GCM_SPU_BLK_N; i++) {
        u32 s = s_spu_blk[i].io_start, e = s_spu_blk[i].io_end;
        if (!e || s >= put || e > put) continue;
        if (put - e > 0x40u) continue;              /* too far back to be current */
        if (e >= best_end) { best_end = e; best = s; }
    }
    return best;
}

/* A resync jumps `get` straight to `put`, which is what keeps the FIFO alive
 * when the walker cannot make progress -- but everything in between is
 * dropped, and if a SET_REFERENCE is in there the cellGcmFinish waiting on it
 * never wakes. Tokyo Jungle lost exactly one that way: two fences in the ring
 * (arg 0 at io 0x09A0, arg 1 at io 0x19E8), the walker parked before the
 * second, and the resync to put=0x19F0 stepped straight over it -- ref stayed
 * 0 while the title spun for ref==1 forever.
 *
 * Dropping DRAWS on a resync is survivable: a frame is lost. Dropping a FENCE
 * is not, it deadlocks the title. Sweep the skipped range first. */
static void gcm_fifo_resync_why(const char* why, u32* getoff, u32 put)
{
    if (s_cap) s_cap->note(RSX_CAP_NOTE_RESYNC, *getoff, put, 0);
    static int n = 0;
    if (PS3_LOG_FIRST(n, 8))
        fprintf(stderr, "[cellGcmSys] FIFO resync (%s) 0x%08X -> put 0x%08X\n",
                why, *getoff, put);
    /* If an SPU DMA'd a pushbuffer that ends just short of `put`, that is the
     * stream the RSX is being pointed at -- land at its START and walk it,
     * instead of landing on `put` and skipping it. Resyncing to `put` keeps the
     * FIFO alive but drops every command in between, and SET_BEGIN_END is one
     * of them: the draws still arrive, with no primitive type, and are dropped
     * by the backend as prim=0. */
    u32 land = gcm_spu_block_for_put(put);
    if (land == 0xFFFFFFFFu) land = put;
    else { static int b = 0; if (PS3_LOG_FIRST(b, 8))
        fprintf(stderr, "[cellGcmSys] resync landing on SPU pushbuffer "
                "0x%08X (put 0x%08X) instead of skipping to put\n", land, put); }

    /* Sweep only the range still being skipped; the walker will consume the
     * fences inside the block itself, and pushing them here too would publish
     * every one twice. */
    u32 from = (*getoff <= land) ? *getoff : s_last_ring_off;
    if (from < land) {
        unsigned rescued = 0;
        for (u32 io = from; io + 8 <= land; io += 4) {
            u32 ea = gcm_io2ea(io); if (!ea) continue;
            u32 w = vm_read32(ea);
            /* A flip marker skipped here will never be reached: count it, or
             * every flip after it waits for the drain forever. */
            if ((w & 0xFFFFFF00u) == GCM_FLIP_MARKER &&
                s_fifo_flips_reached != s_fifo_flips_queued) { s_fifo_flips_reached++; continue; }
            if ((w >> 29) != 0 || (w & 0x1FFCu) != 0x0050u) continue;
            u32 dea = gcm_io2ea(io + 4); if (!dea) continue;
            gcm_ref_push_at(vm_read32(dea), io);
            rescued++;
        }
        /* Fences are not the only sync writes a title waits on. Walk the
         * skipped range as a command stream (each header and its data; NOPs,
         * flip markers and self-JUMP parks stepped over, forward JUMPs inside the range
         * followed; stopping at anything else that is not a method header) and
         * replay the label releases in it as well: NV406E SEMAPHORE_RELEASE
         * (0x6C, at the offset 0x64 set) and the NV4097 back-end and
         * texture-read releases (0x1D70/0x1D74 at the 0x1D6C offset, in the
         * 0x01A4 context), the latter through rsx_process_method so they land
         * exactly where the walker would have written them. inFAMOUS 2 waits
         * on a 0x1D74 release from an SPU-filled command block; a resync that
         * skipped it left the title waiting for the rest of the run. Draws in
         * the range are still lost. */
        static rsx_state s_resync_state;   /* the sync methods do not touch it */
        unsigned labels = 0;
        for (u32 io = from; io + 4 <= land; ) {
            const u32 ea = gcm_io2ea(io); if (!ea) break;
            const u32 w = vm_read32(ea), type = w >> 29;
            if (w == 0 || (w & 0xFFFFFF00u) == GCM_FLIP_MARKER) { io += 4; continue; }   /* NOP, flip (counted above) */
            if ((w & 0xE0000003u) == 0x20000000u || (w & 3u) == 1u) {   /* JUMP */
                const u32 tgt = ((w & 3u) == 1u) ? (w & ~3u) : (w & 0x1FFFFFFCu);
                if (tgt == io) { io += 4; continue; }   /* a park */
                if (tgt > io && tgt < land) { io = tgt; continue; }
                break;
            }
            if (type != 0 && type != 2) break;          /* CALL, RETURN, or not a header */
            const u32 count = (w >> 18) & 0x7FFu, method = w & 0x1FFCu, subch = (w >> 13) & 7u;
            if (!count || io + 4 + count * 4 > land) break;
            for (u32 i = 0; i < count; i++) {
                const u32 dea = gcm_io2ea(io + 4 + i * 4); if (!dea) break;
                const u32 m = (type == 0) ? method + i * 4 : method, v = vm_read32(dea);
                if (subch != 0) continue;
                if (m == 0x64u) s_sema_offset = v;
                else if (m == 0x6Cu) {
                    atomic_thread_fence(memory_order_release);
                    vm_write32(GCM_LABEL_GUEST_BASE + (s_sema_offset & 0xFFFFu), v);
                    labels++;
                } else if (m == 0x01A4u || m == 0x1D6Cu || m == 0x1D70u || m == 0x1D74u) {
                    rsx_process_method(&s_resync_state, m, v);
                    labels += (m == 0x1D70u || m == 0x1D74u);
                }
            }
            io += 4 + count * 4;
        }
        if (rescued || labels) { static int m = 0; if (PS3_LOG_FIRST(m, 8))
            fprintf(stderr, "[cellGcmSys] resync rescued %u fence(s) and %u label"
                    " write(s) from 0x%08X..0x%08X\n", rescued, labels, from, land); }
    }
    *getoff = land;
}

static void gcm_fifo_dump_around(u32 getoff)
{
    /* Only ever called on a recovery (bad branch, stuck get), and capped, so
     * it no longer hides behind GCM_RECDBG: the words the walker stopped on
     * are the one thing a run that wedged once in three can still tell us,
     * and GCM_RECDBG is far too loud to leave on for a 15-minute run. */
    static int d = 0;
    if (!PS3_LOG_FIRST(d, 3)) return;
    for (u32 t = 0; t < GCM_TRACE_N; t++) {
        u32 k = (s_tr_i + t) % GCM_TRACE_N;
        if (!s_tr_w[k]) continue;
        u32 w = s_tr_w[k];
        fprintf(stderr, "[FIFOSTEP] io=%08X w=%08X  type=%u method=0x%04X count=%u\n",
                s_tr_off[k], w, w >> 29, w & 0x1FFCu, (w >> 18) & 0x7FFu);
    }
    for (int k = -20; k <= 2; k++) {
        u32 io = getoff + (u32)(k * 4);
        u32 ea = gcm_io2ea(io);
        if (!ea) continue;
        fprintf(stderr, "[FIFODUMP] %+3d io=%08X w=%08X%s\n",
                k, io, vm_read32(ea), k ? "" : "   <-- bad word");
    }
}


static void gcm_ref_push_at(u32 v, u32 getoff)
{
    { static int _d = -1; if (_d < 0) _d = getenv("GCM_REFLOG") ? 1 : 0;
      if (_d) fprintf(stderr, "[refq] drained fence 0x%X at getoff=%08X\n", v, getoff); }
    u32 t = s_ref_qtail;
    if (t - s_ref_qhead >= GCM_REF_QLEN) {   /* overflow: drop oldest (keeps liveness) */
        s_ref_qhead++;
        static int _o = 0;
        if (PS3_LOG_FIRST(_o, 4)) fprintf(stderr, "[cellGcmSys] fence queue overflow -- oldest dropped\n");
    }
    s_ref_q[t % GCM_REF_QLEN] = v;
    s_ref_qtail = t + 1;
}

/* Absolute microseconds from the QPC hardware counter -- the SAME origin in every
 * translation unit, so two probes in different files can be compared directly.
 * That is the whole point: every wrong conclusion about this stall came from
 * comparing orderings instead of times. */
unsigned long long ps3_qpc_us(void)
{
    static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (unsigned long long)(n.QuadPart / (f.QuadPart / 1000000));
}

static void gcm_ref_publish_one(void)
{
    u32 h = s_ref_qhead;
    if (h == s_ref_qtail) return;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 8, s_ref_q[h % GCM_REF_QLEN]);
    /* stderr, deliberately: [DRAIN] uses printf and stdout is block-buffered
     * when redirected, so its interleaving with an stderr probe is not a
     * timeline. Same stream = same ordering. */
    { static int _rd = -1; if (_rd < 0) _rd = getenv("GCM_REFPUB") ? 1 : 0;
      if (_rd) { static unsigned long _n = 0; if (++_n <= 24)
        fprintf(stderr, "[refpub] t=%lluus #%lu wrote 0x%08X -> readback 0x%08X\n", ps3_qpc_us(), _n,
                s_ref_q[h % GCM_REF_QLEN], vm_read32(GCM_CONTROL_GUEST_ADDR + 8)); } }
    s_ref_qhead = h + 1;
}

/* Read-driven fence publication. cellGcmFinish / FIFO-space waits spin-read the
 * ref register (GCM_CONTROL+8). The 60 Hz present-thread ticker publishes only
 * one queued fence per frame, so when present overruns during heavy boot load
 * the tick rate -- and thus fence publication -- collapses and an equality-
 * waiter stalls seconds per fence (the [finspin] boot crawl; the game is not
 * deadlocked, just starved). Let the spinning reader drive publication too:
 * this is called from vm_read32 on each ref read, advancing one queued fence,
 * globally paced to at most one per millisecond so every value stays visible
 * >= 1 ms to any equality-waiter (no skip-past). Decoupled from present, so a
 * wait makes progress even when the ticker is starved. Idempotent + additive:
 * it only ever publishes fences the ticker would eventually publish anyway. */
/* Serializes the FIFO walker between the 60 Hz present thread and the
 * poll-path below. TryAcquire on the poll path avoids re-entrancy (the walker
 * itself performs guest reads) and never blocks a spinning game thread. */
static SRWLOCK s_gcm_fifo_lock = SRWLOCK_INIT;
static void gcm_rsx_process_fifo_unlocked(void);

/* ---- guest stores to a label are ordered after the walker ------------------------------------------------
 *
 * A title that syncs the PPU and the RSX through a label queues a NV406E ACQUIRE for the label's next value
 * (one per frame: value N, then N+1, ...), kicks the command segment, and overwrites the label with the next
 * value as soon as it builds the next frame. The ACQUIRE compares for equality (the PFIFO puller, and RPCS3's
 * nv406e::semaphore_acquire `while (sema != arg)`), so the RSX has to read the kicked ACQUIRE before the
 * title's next store to that label, or it waits for N for ever. Nothing in the guest orders the two: no
 * second label, no get/put handshake, no flip status sits between the kick and the store. The console orders
 * them by speed -- the RSX fetches the kick within microseconds, the PPU needs a frame's worth of code to
 * reach the store [INFERENCE: no console trace; the guest's protocol relies on it]. The walker here is a host
 * thread that polls, and one that a loaded host leaves off the CPU for a few milliseconds loses: on a host
 * running other builds (load average 6-13 on 12 cores) a title's next label store came 5.27 ms after its kick
 * and the walker read the kick 13 us after that store; a 15-minute unloaded soak ended with the same
 * signature at 785 s [INFERENCE: the same cause, one scheduling hiccup in ~47,000 frames].
 *
 * So a store by guest code into the label window waits, before it lands, for one complete walker pass
 * that began after the store was reached (the pass reads `put` afresh): the RSX has by then read everything
 * the guest had published, and evaluated every ACQUIRE among it against the label's old value. This is the
 * console's ordering with an RSX that is never late; an ACQUIRE the RSX is held in front of (the flip wait,
 * say) is still missed by a store, as on the console, and still ends in gcm-wait-timeout. Nothing to wait
 * for -- `get` already at `put` -- costs two reads. The walker threads (any thread that ran a pass) never
 * wait: they cannot wait for themselves, and guest code a pass calls (a flip handler) may store a label.
 * `gcm.wait_timeout_ms` bounds the wait; a walker that never runs a pass is a host fault and a named stop.
 *
 * runtime/ppu/ppu_loader.cpp (compiled per game, not part of the runtime library) calls the hook from every
 * form of guest store (vm_write8..64, the store-conditionals, and the lifter's PPU_STORE_HOOK; see docs/RUNTIME.md,
 * "Guest Store Hook"). The window, its hook and the pass counters live in the runtime library
 * (runtime/memory/label_window.{h,c}) so that the SPU MFC path reaches the same state: an SPU PUT/PUTL into
 * the window is a store this runtime cannot order safely (it may be the value an ACQUIRE is waiting for),
 * and it stops by name instead (docs/STOPS.md, spu-label-window). The first pass registers the label window.
 * The one HLE path that stores a label on a guest thread, cellGcmSetFlipCommandWithWaitLabel, calls it itself.
 */

static void gcm_guest_label_store(u32 ea, u32 value)
{
    extern unsigned long long ps3_ms_now(void);
    if (ps3_label_window_on_walker()) return;
    if (vm_read32(GCM_CONTROL_GUEST_ADDR + 0) == vm_read32(GCM_CONTROL_GUEST_ADDR + 4)) return;
    /* The next pass to begin: it takes the lock after this load, so it reads a `put` this thread has published. */
    const unsigned long long target = ps3_label_window_next_pass();
    const unsigned long long t0 = ps3_ms_now();
    /* Yield first (the walker is normally a pass away), then sleep; bounded like the segment-switch wait below. */
    for (unsigned spin = 0; !ps3_label_window_pass_done(target); spin++) {
        const unsigned long long waited = ps3_ms_now() - t0;
        if (waited >= ps3_cfg_gcm_wait_timeout_ms())
            ps3_stop_gcm_wait_timeout("cellGcm_label_store", "rsx-walker-pass", ea - GCM_LABEL_GUEST_BASE, value,
                                      vm_read32(ea), (u32)waited);
        Sleep(spin < 64u ? 0u : 1u);
    }
}

static void gcm_register_label_store_hook(void)
{
    ppu_set_store_hook(GCM_LABEL_GUEST_BASE, CELL_GCM_MAX_LABEL_COUNT * GCM_LABEL_STRIDE, gcm_guest_label_store);
}

/* Kick event: a game thread whose fence wait finds the queue DRY signals the
 * present thread to run the FIFO walker NOW instead of on its next 16 ms
 * tick. A movie frame issues ~46 one-ahead cellGcmFinish waits; paying a
 * 16 ms tick per fence was ~0.75 s/frame (the 2 FPS intro). The walker (and
 * the D3D12 backend it feeds) stays on the present thread -- running it from
 * the polling game thread killed the process silently. */
static HANDLE s_gcm_kick_ev = NULL;
void* cellGcm_fifo_kick_event(void)
{
    if (!s_gcm_kick_ev) s_gcm_kick_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    return (void*)s_gcm_kick_ev;
}

/* The present thread's idle wait. It used to be a flat Sleep(4), which meant
 * the kick above was created, signalled -- and waited on by nobody, so a
 * starved guest still paid the full tick. Waiting here instead costs nothing
 * when no one kicks (the timeout is the old sleep) and drains immediately when
 * someone does. */
void cellGcm_fifo_kick_wait(unsigned ms)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_FIFO_KICK"); on = (e && *e == '0') ? 0 : 1; }
    HANDLE ev = on ? (HANDLE)cellGcm_fifo_kick_event() : NULL;
    if (ev) WaitForSingleObject(ev, ms);
    else    Sleep(ms);
}


/* Serializes fence publication between the present ticker and every guest
 * thread poll-reading the ref register (REFPOLL default-on made this path
 * multi-threaded; the SPSC queue head raced and publication order broke --
 * boot froze with all threads parked on fences that were never published in
 * order). TryAcquire: a contended poll just reads the current ref. */
static SRWLOCK s_ref_pub_lock = SRWLOCK_INIT;

void cellGcm_ref_on_poll(void)
{
    /* Pace: leave each published value observable for >= ~200us of spins
     * (equality-waiters at any poll rate see every value; still ~5000/s so
     * loading's fence storms clear in seconds, not minutes). */
    static LARGE_INTEGER s_freq;
    static volatile LONGLONG s_last_pub;
    if (!TryAcquireSRWLockExclusive(&s_ref_pub_lock))
        return;
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONGLONG min_gap = s_freq.QuadPart / 5000;          /* ~200us */
    int dry = 0;
    if (now.QuadPart - s_last_pub >= min_gap) {
        gcm_ref_publish_one();
        s_last_pub = now.QuadPart;
        dry = (s_ref_qhead == s_ref_qtail);
    }
    ReleaseSRWLockExclusive(&s_ref_pub_lock);
    if (dry && s_gcm_kick_ev)
        SetEvent(s_gcm_kick_ev);       /* dry: ask the present thread to walk */
}

static uint64_t s_ft_sema_t0;          /* PS3_FTRACE: when the walker began waiting on the current acquire */
/* GCM_METHOD_STATS=<secs>: every <secs>, what the walker has done with the methods it consumed so far --
 * the verdict totals, the NV0039 copies run, and every (class, method) it counted as unknown. A diagnostic
 * only: the counters themselves are exported through gcm_method_stats(). Walker thread. */
static void gcm_method_stats_report(void)
{
    static int secs = -1;
    static unsigned long long t0;
    if (secs < 0) { const char* e = getenv("GCM_METHOD_STATS"); secs = e ? atoi(e) : 0; }
    if (secs <= 0) return;
    extern unsigned long long ps3_ms_now(void);
    const unsigned long long now = ps3_ms_now();
    if (!t0) t0 = now;
    if (now - t0 < (unsigned long long)secs * 1000u) return;
    t0 = now;
    GcmMethodStats st;
    gcm_method_stats(&st);
    fprintf(stderr, "[rsx-method-stats] handled=%llu inert=%llu missing=%llu unknown=%llu unbound=%llu "
                    "m2mf_copies=%llu\n",
            st.handled, st.inert, st.missing, st.unknown, st.unbound, st.m2mf_copies);
    for (u32 c = 0; c < RSX_CLASS_COUNT; c++)
        for (u32 i = 0; i < 0x800u; i++) {
            const unsigned long long n = atomic_load_explicit(&s_mc_tab[c][i].count, memory_order_relaxed);
            if (n) fprintf(stderr, "[rsx-method-stats]   unknown %s 0x%04X x%llu (first: subchannel %u, data 0x%08X)\n",
                           rsx_class_name((rsx_class_id)c), i << 2, n, s_mc_tab[c][i].subch, s_mc_tab[c][i].data);
        }
    for (u32 c = 0; c < RSX_CLASS_COUNT; c++)
        for (u32 i = 0; i < 0x800u; i++) {
            const unsigned long long n = atomic_load_explicit(&s_miss_tab[c][i].count, memory_order_relaxed);
            if (n) fprintf(stderr, "[rsx-method-stats]   missing %s 0x%04X x%llu (first: subchannel %u, data 0x%08X)\n",
                           rsx_class_name((rsx_class_id)c), i << 2, n, s_miss_tab[c][i].subch, s_miss_tab[c][i].data);
        }
}


/* RSX_WALKER_PROF=<file>: an in-process sampling profile of the FIFO walker
 * thread, for hosts where an external sampler cannot attach. Once
 * RSX_WALKER_PROF_TRIGGER=<file> exists (immediately when unset), a sampler
 * thread signals the walker every millisecond for RSX_WALKER_PROF_SECONDS
 * (default 10); the handler records the interrupted pc and the frame-pointer
 * chain. The file gets one sample per line, innermost first: addresses in
 * the main image relative to it, for symbolizing offline; addresses in other
 * images as @symbol. The profiled thread is the first one to drain the FIFO.
 * Diagnostic only, off unless RSX_WALKER_PROF is set; macOS arm64. While it
 * samples, the walker takes a SIGPROF every millisecond (SA_RESTART). */
#if defined(__APPLE__) && defined(__aarch64__)
#include <signal.h>
#include <pthread.h>
#include <sys/ucontext.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
enum { WPROF_MAX = 120000, WPROF_DEPTH = 32 };
static uintptr_t (*s_wprof_buf)[WPROF_DEPTH];
static unsigned char* s_wprof_len;
static atomic_int s_wprof_n;
static pthread_t s_wprof_target;
static atomic_int s_wprof_state;     /* 0 unset, 1 armed, 2 done */
static uintptr_t s_wprof_lo, s_wprof_hi;
static void wprof_handler(int sig, siginfo_t* si, void* ctx)
{
    (void)sig; (void)si;
    const int i = atomic_load_explicit(&s_wprof_n, memory_order_relaxed);
    if (i >= WPROF_MAX || !s_wprof_buf) return;
    const ucontext_t* uc = (const ucontext_t*)ctx;
    uintptr_t* o = s_wprof_buf[i];
    unsigned d = 0;
    o[d++] = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    uintptr_t fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
    while (d < WPROF_DEPTH && fp >= s_wprof_lo && fp + 16 <= s_wprof_hi && !(fp & 7u)) {
        const uintptr_t next = ((const uintptr_t*)fp)[0], ret = ((const uintptr_t*)fp)[1];
        if (!ret) break;
        o[d++] = ret;
        if (next <= fp) break;
        fp = next;
    }
    s_wprof_len[i] = (unsigned char)d;
    /* Release: the writer reads this sample's frames after it sees the count. */
    atomic_store_explicit(&s_wprof_n, i + 1, memory_order_release);
}
static void* wprof_thread(void* arg)
{
    (void)arg;
    const char* out = getenv("RSX_WALKER_PROF");
    const char* trig = getenv("RSX_WALKER_PROF_TRIGGER");
    const char* secs_e = getenv("RSX_WALKER_PROF_SECONDS");
    const int secs = secs_e && atoi(secs_e) > 0 ? atoi(secs_e) : 10;
    struct timespec poll = {0, 100000000}, tick = {0, 1000000};
    while (trig && *trig && access(trig, F_OK) != 0) nanosleep(&poll, NULL);
    void* top = pthread_get_stackaddr_np(s_wprof_target);
    const size_t size = pthread_get_stacksize_np(s_wprof_target);
    s_wprof_hi = (uintptr_t)top; s_wprof_lo = (uintptr_t)top - size;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = wprof_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    fprintf(stderr, "[walker prof] sampling for %d s\n", secs);
    for (int k = 0; k < secs * 1000 && atomic_load(&s_wprof_n) < WPROF_MAX; k++) {
        pthread_kill(s_wprof_target, SIGPROF);
        nanosleep(&tick, NULL);
    }
    nanosleep(&poll, NULL);
    const int n = atomic_load_explicit(&s_wprof_n, memory_order_acquire);
    FILE* f = fopen(out, "w");
    if (f) {
        const intptr_t slide = _dyld_get_image_vmaddr_slide(0);
        fprintf(f, "# walker samples %d slide 0x%llx image %s\n", n, (unsigned long long)slide,
                _dyld_get_image_name(0));
        /* Addresses outside the main image (system frameworks) are named
         * here with dladdr, since only this process knows where they are. */
        const struct mach_header* mh = _dyld_get_image_header(0);
        Dl_info self_info; uintptr_t self_lo = 0;
        if (dladdr(mh, &self_info)) self_lo = (uintptr_t)self_info.dli_fbase;
        for (int i = 0; i < n; i++) {
            for (unsigned d = 0; d < s_wprof_len[i]; d++) {
                const uintptr_t pc = s_wprof_buf[i][d];
                Dl_info info;
                if (dladdr((const void*)pc, &info) && (uintptr_t)info.dli_fbase != self_lo && info.dli_sname)
                    fprintf(f, "%s@%s", d ? " " : "", info.dli_sname);
                else
                    fprintf(f, "%s%llx", d ? " " : "", (unsigned long long)(pc - (uintptr_t)slide));
            }
            fputc('\n', f);
        }
        fclose(f);
    }
    fprintf(stderr, "[walker prof] %d samples -> %s\n", n, out ? out : "(none)");
    atomic_store(&s_wprof_state, 2);
    return NULL;
}
static void wprof_note_walker(void)
{
    if (atomic_load_explicit(&s_wprof_state, memory_order_relaxed) != 0) return;
    /* One caller arms it; that thread is the one sampled. */
    int unset = 0;
    if (!atomic_compare_exchange_strong(&s_wprof_state, &unset, 1)) return;
    const char* out = getenv("RSX_WALKER_PROF");
    if (!out || !*out) { atomic_store(&s_wprof_state, 2); return; }
    s_wprof_buf = calloc(WPROF_MAX, sizeof *s_wprof_buf);
    s_wprof_len = calloc(WPROF_MAX, 1);
    if (!s_wprof_buf || !s_wprof_len) { atomic_store(&s_wprof_state, 2); return; }
    s_wprof_target = pthread_self();
    pthread_t t;
    if (pthread_create(&t, NULL, wprof_thread, NULL) != 0) {
        fprintf(stderr, "[walker prof] sampler thread not started\n");
        atomic_store(&s_wprof_state, 2);
        return;
    }
    pthread_detach(t);
}
#else
static void wprof_note_walker(void) {}
#endif

/* Host thread that last walked the FIFO, for PS3_SAMPLE_DRAIN. */
volatile unsigned long g_gcm_drain_tid;

void cellGcm_rsx_process_fifo(void)
{
    wprof_note_walker();
    g_gcm_drain_tid = GetCurrentThreadId();
    const uint64_t ft_t0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    AcquireSRWLockExclusive(&s_gcm_fifo_lock);
    ps3_label_window_pass_begin();
    gcm_rsx_process_fifo_unlocked();
    ps3_label_window_pass_end();
    ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
    gcm_method_stats_report();
    if (ft_t0) {
        const uint64_t t1 = ps3_ft_now();
        if (t1 - ft_t0 >= ps3_ft_min_ns())
            ps3_ft_span(PS3_FT_DRAIN, 0, s_ft_sema_t0 ? 1u : 0u, 0, ft_t0, t1);
    }
}

/* Run exactly one recorded step (rsx_replay). The walker is not seeked: it must already stand where the
 * recording says the step began, so a replay that has drifted is reported, not papered over. */
int cellGcm_rsx_replay_step(const GcmReplayStep* s, u32 got[2])
{
    got[0] = s_fifo_getoff; got[1] = s_fifo_calloff;
    if (s_fifo_getoff != s->get || s_fifo_calloff != s->call_ret) return GCM_REPLAY_NOT_AT_STEP;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, s->put);
    s_replay.on = 1; s_replay.end = s->end;
    s_replay.ts = s->report_ts; s_replay.n_ts = s->n_report_ts; s_replay.used_ts = 0;
    cellGcm_rsx_process_fifo();
    s_replay.on = 0;
    got[0] = s_fifo_getoff; got[1] = s_fifo_calloff;
    if (s_fifo_getoff != s->end) return GCM_REPLAY_LANDED_ELSEWHERE;
    if (s_replay.used_ts != s->n_report_ts) { got[0] = s_replay.used_ts; return GCM_REPLAY_REPORT_COUNT; }
    return 0;
}

/* The methods rsx_process_method acts on beyond its own rsx_state: the notify
 * and semaphore DMA contexts, the notify write, the back-end label offset and
 * label writes, and SET_REFERENCE (g_rsx_last_reference). Everything else only
 * updates state for a vtable backend. */
static int gcm_legacy_method_has_effect(u32 m)
{
    switch (m) {
    case 0x0050u: case 0x0104u: case 0x0180u: case 0x01A4u:
    case 0x1D6Cu: case 0x1D70u: case 0x1D74u:
        return 1;
    default:
        return 0;
    }
}

/* GCM_GET_EQ_PUT=1: see where get is published at the end of a pass. */
static int gcm_get_eq_put(void)
{
    static int eq = -1;
    if (eq < 0) { const char* e = getenv("GCM_GET_EQ_PUT"); eq = e ? atoi(e) : 0; }
    return eq;
}

/* Any per-word FIFO switch set, or a mirrored draw engine on. Read once per
 * drain: the walker hands every data word through these checks, a million a
 * second in God of War HD gameplay, and it used to test each switch on its
 * own for every word. */
static int gcm_word_diag(void)
{
    static int on = -1;
    if (on < 0)
        on = (getenv("GCM_SUBCH7") || getenv("GCM_OBJDBG") || getenv("GCM_SUBCH_GATE") || getenv("GCM_WORD_STATS") ||
              getenv("GCM_VAL_WATCH") || getenv("GCM_METHOD_HIST") || getenv("RSX_LIVE_FEED_DBG") ||
              rsx_live_draw_enabled() || rsx_draw_engine_enabled()) ? 1 : 0;
    return on;
}

/* One data word of a slot bound to NV4097, or a channel-control (NV406E) word,
 * plus the methods whose answer the walker writes itself. Returns the verdict
 * the walker records (gcm_method_verdict): nonzero when a consumer took the
 * method. */
static int gcm_3d_method(rsx_state* st, u32 m, u32 v, int legacy_state_live)
{
    extern u32 g_rsx_last_reference;
    /* #466: rsx_state feeds only the legacy vtable backend. With the draw
     * engine up and no vtable registered, the state it builds is never read,
     * so only the methods with effects outside it are worth handing over.
     * The gate lives here rather than at the call sites because #554 funnelled
     * every 3D method through this function -- there are two callers, and a
     * gate at one of them only would be a silent half-measure.
     *
     * The verdict (gcm_method_verdict) says what consumed the word: the state
     * machine when it ran (0 = it took the method), or the register-file
     * dispatch's class table (rsx_dsp_method_class), which names every register
     * the draw engine reads. SET_OBJECT is the binding table's. A method the
     * gate kept from the state machine and the class table does not claim has no
     * live consumer, so it counts as unhandled -- the state machine's answer for
     * it would be about state nothing reads. That is why every register the
     * engine reads needs its class-table row: the state machine used to answer
     * for it, unseen. */
    int handled = 0;
    if (legacy_state_live || gcm_legacy_method_has_effect(m))
        handled = rsx_process_method(st, m, v) == 0;
    if (!handled && m == 0) handled = 1;
    /* The register-file dispatch's class table is a property of the method number
     * (rsx_dispatch.h), not of this run's configuration: a method it classifies is
     * consumed on the register-file path even while that path is off, so it is
     * classified, not unknown (#47). The walker's own consumers below (GET_REPORT's
     * destination 0x1A8, GET_REPORT 0x1800) are handled here too. */
    if (!handled && (m == 0x1A8u || m == 0x1800u || rsx_dsp_method_class(m) != RSX_DSP_CLASS_TODO)) handled = 1;
    /* NV406E_SET_REFERENCE: queue the fence value for PACED
     * publication (gcm_ref_publish below) instead of letting a
     * later fence in the same batch overwrite it. */
    if (m == 0x50) gcm_ref_push_at(g_rsx_last_reference, s_fifo_getoff);
    /* NV4097_GET_REPORT: (type << 24) | offset into the report
     * area. Sample counts are not measured, so a ZPASS query
     * reports "visible" -- the conservative answer; reading 0
     * tells the title every queried object is occluded. */
    if (m == 0x1A8) s_report_dma = v;
    if (m == 0x1800) {
        const u32 off = v & 0xFFFFFFu, idx = off / 16u;
        const u32 val = (v >> 24) == 1u ? 0xFFFFu : 0u;
        const u64 ts = gcm_report_timestamp();
        const int main_mem = s_report_dma == 0xFEED0001u || s_report_dma == 0xBAD68000u;
        { static int rn = 0; if (PS3_LOG_FIRST(rn, 6)) printf("[GET_REPORT] type=%u off=0x%X dma=0x%08X%c", v >> 24, off, s_report_dma, 10); }
        if (main_mem) {
            /* CellGcmReportData, big-endian: u64 timer, u32 value, u32 padding. */
            const u32 rea = gcm_io2ea(off);
            if (rea) {
                vm_write32(rea, (u32)(ts >> 32));
                vm_write32(rea + 4u, (u32)ts);
                vm_write32(rea + 8u, val);
                vm_write32(rea + 12u, 0u);
                if (s_cap) s_cap->write(RSX_CAP_LOC_MAIN, off, rea, vm_base + rea, 16, RSX_CAP_WRITE_REPORT);
            } else { static int un = 0; if (PS3_LOG_FIRST(un, 4)) printf("[GET_REPORT] main-memory offset 0x%X is not IO-mapped%c", off, 10); }
        } else if (idx < CELL_GCM_MAX_REPORT_COUNT) {
            s_report_data[idx].timestamp = ts;
            s_report_data[idx].value = val;
        }
    }
    return handled;
}


/* The diagnostic switches below are tested on every drain pass, and GCM_RECDBG
 * on every semaphore method. getenv walks the whole environment and showed in
 * RSX-thread profiles, so each is read once and cached, like the other knobs. */
static void gcm_rsx_process_fifo_unlocked(void)
{
    s_acquire_blocked = 0;
    { static unsigned _n = 0; static unsigned long long _t0 = 0;
      extern unsigned long long ps3_ms_now(void);
      _n++;
      static int rate = -1;
      if (rate < 0) rate = getenv("GCM_RATE") ? 1 : 0;
      if (rate) {
          unsigned long long now = ps3_ms_now();
          if (_t0 == 0) _t0 = now;
          if (now - _t0 >= 2000) {
              fprintf(stderr, "[RATE] drains/sec=%.0f\n", _n * 1000.0 / (now - _t0));
              _n = 0; _t0 = now;
          }
      } }
    static rsx_state s_state;
    static int  s_inited = 0;
    extern u32 g_rsx_last_reference;

    /* A statically-linked libgcm (PS3 firmware modules -- ps1_netemu) never calls
     * cellGcmSetupContext, so there is no CellGcmContextData EA to gate on: it gets
     * its ring through sys_rsx_context_allocate instead. The walk below only needs
     * `put` and the IO table, both of which the syscall layer fills; the one part
     * that does read the context (the ring-recycle heuristic) is separately guarded
     * by s_gcm_ctx_out_ea, which stays 0 on that path. */
    if (!s_gcm_context_ea && !s_gcm_syscall_mode) return;
    if (!s_inited) { rsx_state_init(&s_state); s_inited = 1; gcm_register_label_store_hook(); }
    const int wdiag = gcm_word_diag();
    /* rsx_state feeds only the legacy vtable backend. With the draw engine
     * up and no vtable registered, the state it builds is never read, and the
     * only methods worth handing it are the ones with effects outside it
     * (gcm_legacy_method_has_effect). RSX_TRACE still sees every method. */
    static int s_rsx_trace = -1;
    if (s_rsx_trace < 0) s_rsx_trace = getenv("RSX_TRACE") ? 1 : 0;
    const int legacy_state_live = rsx_get_backend() != NULL || !rsx_draw_engine_enabled() ||
                                  s_rsx_trace;


    /* Walk the FIFO exactly like the RSX: chase the guest-written `put` (an IO
     * offset in the control register), decoding method headers and following
     * JUMP/CALL/RET control words. The old linear drain copied context->current
     * onward and treated a JUMP as end-of-buffer -- fine for titles that write
     * one flat ring (cellmark), but PSL1GHT/Tiny3D immediately JUMPs into its
     * own command ring, so everything after the jump (including the reference
     * writes its waits spin on) silently never executed. */
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    /* The words behind `put` were stored by another thread (the title, or the
     * recycle below) before it stored `put`. On x86 the hardware keeps loads
     * in order; on arm64 it does not, so without an acquire here the ring
     * word can be fetched before `put` was and come back stale -- the walker
     * then runs off the end of the previous frame's bytes until it meets a
     * word that is not a command, and parks there forever. */
    atomic_thread_fence(memory_order_acquire);

    /* GCM_CTXDBG=1: which context the title is actually driving. Ours is written
     * once at init; if the title repoints gCellGcmCurrentContext at a buffer of
     * its own, every field below moves with it and the FIFO the runtime walks
     * stops being the one the title fills. Reported on change, not per tick. */
    static int s_ctxdbg = -1;
    if (s_ctxdbg < 0) s_ctxdbg = getenv("GCM_CTXDBG") ? 1 : 0;
    if (s_ctxdbg && s_gcm_ctx_out_ea) {
        static u32 last_ptr = 0xFFFFFFFFu, last_cur = 0xFFFFFFFFu;
        u32 ptr = vm_read32(s_gcm_ctx_out_ea);
        u32 cur = ptr ? vm_read32(ptr + 0x8) : 0;
        if (ptr != last_ptr || cur != last_cur) {
            last_ptr = ptr; last_cur = cur;
            fprintf(stderr, "[CTX] gCellGcmCurrentContext@0x%08X -> 0x%08X  "
                    "begin=%08X end=%08X current=%08X callback=%08X  (ours=0x%08X)\n",
                    s_gcm_ctx_out_ea, ptr,
                    ptr ? vm_read32(ptr + 0x0) : 0, ptr ? vm_read32(ptr + 0x4) : 0,
                    cur, ptr ? vm_read32(ptr + 0xC) : 0, s_gcm_context_ea);
            fflush(stderr);
        }
    }

    u32 start_off = s_fifo_getoff;
    /* GCM_DRAINDBG: log one drain pass in GCM_DRAINDBG_EVERY (default 60). */
    static int s_drain_every = -1;
    if (s_drain_every < 0) { const char* e = getenv("GCM_DRAINDBG_EVERY");
                             s_drain_every = (e && atoi(e) > 0) ? atoi(e) : 60; }
    static int s_draindbg = -1;
    if (s_draindbg < 0) s_draindbg = getenv("GCM_DRAINDBG") ? 1 : 0;
    if (s_draindbg) {
        static int n = 0;
        if (n++ % s_drain_every == 0)
            printf("[DRAIN] getoff=%08X put=%08X ref=%08X ctx.current=%08X\n",
                   s_fifo_getoff, put, vm_read32(GCM_CONTROL_GUEST_ADDR + 8),
                   vm_read32(s_gcm_context_ea + 0x8));
    }

    /* GCM_SCANREF=1 (one-shot): list every NV406E_SET_REFERENCE actually
     * present in 0..put. "The title waits for a fence we never published" has
     * two very different causes -- we missed the command, or it was never
     * submitted -- and only a direct scan of the ring separates them. */
    { static int scanned = 0, scanref = -1;
      if (scanref < 0) scanref = getenv("GCM_SCANREF") ? 1 : 0;
      if (!scanned && scanref && put >= 0x19F0) { scanned = 1;
        unsigned found = 0;
        for (u32 io = 0; io + 8 <= put; io += 4) {
            u32 ea = gcm_io2ea(io); if (!ea) continue;
            u32 w = vm_read32(ea);
            if ((w & 0x1FFCu) == 0x0050u && (w >> 29) == 0) {
                u32 dea = gcm_io2ea(io + 4);
                fprintf(stderr, "[SCANREF] io=%08X w=%08X count=%u arg=%08X\n",
                        io, w, (w >> 18) & 0x7FFu, dea ? vm_read32(dea) : 0xDEADu);
                found++;
            }
        }
        fprintf(stderr, "[SCANREF] %u SET_REFERENCE in 0..%08X\n", found, put);
      } }

    /* GCM_FIFO_SNAP=N: dump N snapshots of the raw words at both ends of the
     * ring -- what `get` is about to decode, and what the title just wrote at
     * `put`. When get and put drift apart, the two windows say immediately
     * whether get is grinding through NOPs/garbage or the title is writing
     * somewhere get can never reach. The [FIFOSTEP] trace only ever fired on a
     * bad branch, which is exactly the case where the drift is silent. */
    { static int snap = -1;
      if (snap < 0) { const char* e = getenv("GCM_FIFO_SNAP"); snap = e ? atoi(e) : 0; }
      if (snap > 0 && put) {
          snap--;
          fprintf(stderr, "[SNAP] get=%08X put=%08X\n", s_fifo_getoff, put);
          for (int k = 0; k < 16; k++) {
              u32 gio = s_fifo_getoff + (u32)(k * 4), pio = put + (u32)(k * 4);
              u32 gea = gcm_io2ea(gio), pea = gcm_io2ea(pio);
              fprintf(stderr, "[SNAP]  get+%-3d io=%08X w=%08X   put+%-3d io=%08X w=%08X\n",
                      k * 4, gio, gea ? vm_read32(gea) : 0xDEADDEADu,
                      k * 4, pio, pea ? vm_read32(pea) : 0xDEADDEADu);
          }
          fflush(stderr);
      } }

    int budget = 0x100000;                    /* words per tick cap */
    /* GCM_DRAINDBG also reports WHY each pass stopped. A drain that ends far
     * short of `put` every tick is the signature of a FIFO that can never
     * catch up, and the reason is the whole diagnosis. */
    const char* why = "caught-up";
    /* A replayed step reports the PUT its live pass saw (the capture records it, the flip backlog test reads
     * it) but stops where the step left the walker, so one recorded step is one iteration. */
    const u32 stop = s_replay.on ? s_replay.end : put;
    while (s_fifo_getoff != stop && budget-- > 0) {
        u32 ea = gcm_io2ea(s_fifo_getoff);
        if (!ea) {
            /* get is sitting on an IO offset with no mapping -- the title
             * branched into a region and then unmapped it, or the offset was
             * never valid. Breaking silently (what this did) leaves get stuck
             * there FOREVER: it can never reach put, the FIFO stops draining,
             * no fences publish, and anything waiting on ctrl->ref hangs with
             * nothing logged. Resynchronise to put instead. Commands between
             * here and put are lost, which is bad, but a permanently dead FIFO
             * is worse -- and now it says so. */
            static u32 last_bad; static int n;
            if (s_fifo_getoff != last_bad || n < 4) {
                last_bad = s_fifo_getoff;
                if (PS3_LOG_FIRST(n, 16))
                    fprintf(stderr, "[cellGcmSys] FIFO get=0x%08X has no IO "
                            "mapping -- resyncing to put=0x%08X\n",
                            s_fifo_getoff, put);
            }
            s_fifo_getoff = put;
            /* Whatever the walker had not reached is gone, and so are the flips
             * queued in it: none of them will ever be executed, so do not let a
             * flip wait on them. */
            s_fifo_flips_reached = s_fifo_flips_queued;
            why = "no-io-mapping";
            break;
        }
        u32 w = vm_read32(ea);
        /* Only remember offsets BELOW put -- those are positions in the
         * ring the title is writing. A park in another region is where the
         * walker gave up, not how far it got through the ring. */
        if (s_fifo_getoff < put) s_last_ring_off = s_fifo_getoff + 4;
        s_tr_off[s_tr_i % GCM_TRACE_N] = s_fifo_getoff;
        s_tr_w[s_tr_i % GCM_TRACE_N] = w;
        s_tr_i++;
        if (w && ((w & 0xE0000003u) == 0x20000000u || (w & 3u) == 1u || (w & 3u) == 2u)) {
            s_cf_off[s_cf_i % 8] = s_fifo_getoff; s_cf_w[s_cf_i % 8] = w; s_cf_i++; }
        /* GCM_WORD_STATS=<secs>: NOP words vs headers vs jumps/calls/returns
         * walked -- a frame that is mostly NOPs is reserved space someone
         * (an SPU job) was supposed to fill. */
        { static int ws = -1; static unsigned long long nop, hdr, jmp, cal, ret, t0;
          if (ws < 0) { const char* e = getenv("GCM_WORD_STATS"); ws = e ? atoi(e) : 0; }
          if (ws > 0) {
              if (!w && s_fifo_getoff < put) { static int mm = 0;
                  const u32 alias = s_fifo_getoff + 0x11000000u;
                  if (mm < 6 && ea != alias && vm_read32(alias)) { mm++;
                      fprintf(stderr, "[io2ea-mismatch] io=%08X walker_ea=%08X (w=0) alias=%08X w=%08X\n",
                              s_fifo_getoff, ea, alias, vm_read32(alias)); } }
              if (!w) nop++;
              else if ((w & 0xE0000003u) == 0x20000000u || (w & 3u) == 1u) jmp++;
              else if ((w & 3u) == 2u) cal++;
              else if (w == 0x00020000u) ret++;
              else hdr++;
              extern unsigned long long ps3_ms_now(void);
              const unsigned long long now = ps3_ms_now();
              if (!t0) t0 = now;
              if (now - t0 >= (unsigned long long)ws * 1000u) {
                  fprintf(stderr, "[words] nop=%llu hdr=%llu jump=%llu call=%llu ret=%llu get=0x%08X put=0x%08X jumps:",
                          nop, hdr, jmp, cal, ret, s_fifo_getoff, put);
                  for (u32 t = 4; t >= 1; t--) { u32 q = (s_cf_i - t) % 8;
                      fprintf(stderr, " %08X@%08X", s_cf_w[q], s_cf_off[q]); }
                  fprintf(stderr, "\n");
                  nop = hdr = jmp = cal = ret = 0; t0 = now;
              } } }
        /* A run of NOP words. God of War HD builds command blocks on SPUs into
         * fixed-size reservations and leaves the rest of each one zero: in
         * gameplay about 4.6 million NOPs a second against 1.2 million command
         * headers, and each NOP went through the whole decode below. The RSX
         * does nothing for them, so the run is passed over at once, up to
         * `put` or the end of this 1 MB IO page (the next word may map
         * elsewhere). Each NOP still spends a word of this tick's budget: the
         * skip is a faster decode, not a bigger tick, and VF5 depends on the
         * walker yielding at the cap (it lost 7 milestones when a whole run
         * cost one word). */
        /* A capture records every word the walker consumes, in executed order (ps3diff.rsx/1): with a
         * sink attached the run is walked word by word, whatever the draw engine's state. */
        if (!w && !wdiag && !s_cap) {
            const u32 page_end = (s_fifo_getoff | 0xFFFFFu) + 1u;
            const u32 lim = (put > s_fifo_getoff && put < page_end) ? put : page_end;
            u32 off = s_fifo_getoff + 4u, nea = ea + 4u;
            while (off < lim && budget > 0 && !vm_read32(nea)) { off += 4u; nea += 4u; budget--; }
            if (s_fifo_getoff < put) s_last_ring_off = off;
            s_fifo_getoff = off;
            continue;
        }
        u32 type = w >> 29;
        /* The walker is reading something other than the park it waited on:
         * it has left it, by the patched word (a JUMP elsewhere, or commands
         * written over the park) or by a resync. Forget the park here. Kept,
         * its start time survived until the walker next stopped at the same
         * address with the same put -- a title that reuses its command blocks
         * does that -- and that park then inherited a grace that had already
         * run out, so the jump-park resync fired at once and dropped the
         * commands the title was about to link in. */
        if (park_off && (s_fifo_getoff != park_off ||
                         (w & 0xE0000003u) != 0x20000000u || (w & 0x1FFFFFFCu) != park_off))
            park_off = 0;

        if ((w & 0xFFFF0000u) == 0xFEAD0000u) {
            /* lv1 driver flip: statically-linked libgcm cellGcmSetFlip writes
             * this word into the FIFO instead of calling any import. Fire the
             * flip (status/count/handler) and STOP this drain so the ticker's
             * flip-gated present shows exactly the completed frame -- draining
             * on would mix the next frame's head into the batch (wave: every
             * frame split across 4 presents, layout flashing/zooming). */
            extern s32 cellGcmSetFlipCommand(u32 bufferId);
            if (s_cap) gcm_cap_step(RSX_CAP_STEP_WORD, s_fifo_getoff, put, &w, 1, 0, 0, 0, 0);
            s_fifo_getoff += 4;
            gcm_fifo_flip_resets_state(&s_state);
            /* Count the flip WORDS the title actually writes into the FIFO,
             * separately from the presents that result. When a title stops
             * appearing on screen the first question is which side stopped:
             * this counter still climbing while presents do not is a runtime
             * problem, both stopping together is the guest's. */
            { static unsigned long long flips = 0; static int dbg = -1;
              if (dbg < 0) dbg = getenv("GCM_FLIPCOUNT") ? 1 : 0;
              ++flips;
              if (dbg && (flips <= 8ull || (flips % 200ull) == 0))
                  fprintf(stderr, "[flipword] %llu FIFO flip words decoded%c",
                          flips, 10); }
            if (PS3_FT_ON()) { const uint64_t t = ps3_ft_now();
                               ps3_ft_span(PS3_FT_FLIP_FIFO, 0, w & 7u, 0, t, t); }
            if ((w & 0xFFFFFF00u) == GCM_FLIP_MARKER) {
                /* Queued by _cellGcmSetFlipCommand, which already did the
                 * guest-visible half of the request. */
                gcm_set_current_display_buffer(w & 7u);
                s_flip_pending = 1;
                s_flip_request_count++;
                /* Not past `queued`: a resync may already have written off
                 * this flip. */
                if (s_fifo_flips_reached != s_fifo_flips_queued) s_fifo_flips_reached++;
            } else {
                cellGcmSetFlipCommand(w & 0xFFu);
            }
            /* ...unless the FIFO is badly backlogged. One flip per drain is
             * right while `get` is keeping up with `put`; when it is megabytes
             * behind it is a deadlock, because the title's ring can only be
             * reclaimed as `get` advances. VF5 submits ~5x faster than one
             * flip per drain consumes, so its 4 MB ring fills, its own
             * buffer-full callback can never free space, and AMGL prints
             * "Command Buffer Overflow" forever. Past the threshold keep
             * draining -- the intermediate presents are frames we are too far
             * behind to show anyway. No-op for a title that is keeping up. */
            /* A wrapped ring has put behind get, and that is the most
             * backlogged it can be, not the least: God of War HD fills its
             * 3 MB ring at boot, wraps, and a walker stopping at every flip
             * then crawled ~1 KB a tick while the title wrote over the
             * commands ahead of it. */
            if (s_cap) s_cap->step_end();
            if (gcm_fifo_backlog(put, s_fifo_getoff) < GCM_FIFO_CATCHUP_BYTES)
                { why = "flip"; break; }
            continue;
        }

        if (type == 1) {                       /* JUMP: 0x20000000 | offset */
            { static int _rd = -1; if (_rd < 0) _rd = getenv("GCM_RECDBG") ? 1 : 0;
              if (_rd) fprintf(stderr, "[JMP] %08X -> %08X (put=%08X)\n",
                               s_fifo_getoff, w & 0x1FFFFFFCu, put); }
            { u32 tgt = w & 0x1FFFFFFCu;
              if (!gcm_io2ea(tgt)) { gcm_fifo_bad_branch("JUMP", tgt, w); gcm_fifo_dump_around(s_fifo_getoff); gcm_fifo_resync_why("unmapped-JUMP", &s_fifo_getoff, put); break; }
              /* A JUMP to its own address is the "park the RSX here" idiom:
               * the title leaves it at the write head so the GPU stops if it
               * catches up, and overwrites it when the next segment is
               * appended. Taking it re-reads the same word forever -- 38.8 M
               * times in one Twisted Metal run, which is the drain burning the
               * CPU the guest needs. Stop this pass instead; the next one
               * re-reads the word, so a patched jump is still followed. */
              if (tgt == s_fifo_getoff) {
                  /* Parked. If the title has meanwhile moved `put` somewhere
                   * else, it has switched to its other segment and left this
                   * park standing -- it only patches a park when it reuses that
                   * block. Waiting for a patch that will not come freezes the
                   * FIFO for the rest of the run, so follow the write head. */
                  /* ...but not at once. A title that links its segments by
                   * patching the park moves `put` first and patches second:
                   * inFAMOUS 2 ends each of its main-memory command blocks in
                   * a park, writes the next commands at the ring start, sets
                   * put, and only then rewrites the park as a JUMP to them. A
                   * drain that lands between the two saw put elsewhere, went
                   * straight to it, and dropped everything from the ring
                   * start to put -- the frame's fence with it, so the title
                   * waited on that label for the rest of the run. Give the
                   * patch time to arrive, and resync only for a park that
                   * has stood with the same put for longer than any title
                   * takes between the two stores. */
                  if (put != s_fifo_getoff) {
                      extern unsigned long long ps3_ms_now(void);
                      const unsigned long long now = ps3_ms_now();
                      if (park_off != s_fifo_getoff || park_put != put) {
                          park_off = s_fifo_getoff; park_put = put; park_since = now;
                      } else if (ps3_cfg_gcm_park_resync()) {
                          /* Compatibility mode: the grace expiring follows `put`, dropping the commands
                           * in between. Every intervention is logged, not just the first few. */
                          if (now - park_since >= ps3_cfg_gcm_park_grace_ms() &&
                              /* Read the park again before giving up on it.
                               * An SPU job that fills a command-buffer hole
                               * (Edge geometry reserves them as self-JUMPs)
                               * can land between this pass's read and the
                               * resync, which would then jump over the
                               * commands it just wrote. */
                              vm_read32(gcm_io2ea(s_fifo_getoff)) == w) {
                              fprintf(stderr, "[cellGcmSys] jump-park intervention: mode=compatibility grace=%u ms "
                                      "park=0x%08X stood=%llu ms put=0x%08X: get follows put, the commands between "
                                      "are dropped (gcm.park_resync)\n",
                                      ps3_cfg_gcm_park_grace_ms(), s_fifo_getoff, now - park_since, put);
                              park_off = 0;
                              gcm_fifo_dump_around(s_fifo_getoff);
                              gcm_fifo_resync_why("jump-park", &s_fifo_getoff, put);
                          }
                      } else if (now - park_since >= ps3_cfg_gcm_wait_timeout_ms()) {
                          /* Reference mode: the RSX waits at a park for the patch, however late
                           * (gcm.park_grace_ms only matters in Compatibility mode). A park that stands
                           * the whole wait timeout with put elsewhere is a producer that is missing,
                           * and a named stop. */
                          ps3_stop_gcm_wait_timeout("cellGcm_rsx_process_fifo", "jump-park", s_fifo_getoff, put,
                                                    w & 0x1FFFFFFCu, (u32)(now - park_since));
                      }
                  }
                  why = "parked";
                  s_park_pass = 1;
                  break;
              }
              if (s_cap) gcm_cap_step(RSX_CAP_STEP_JUMP, s_fifo_getoff, put, &w, 1, 0, 0, 0, tgt);
              if (tgt < s_fifo_getoff) s_fifo_back_jumps++;
              s_fifo_getoff = tgt;
              /* get is the RSX's fetch pointer: a title polling it sees the
               * GPU leave a segment as soon as the JUMP out of it is taken.
               * The end of the pass, where get is otherwise published, can
               * come tens of milliseconds later. GCM_GET_EQ_PUT keeps its
               * own value. */
              if (!gcm_get_eq_put()) vm_write32(GCM_CONTROL_GUEST_ADDR + 4, tgt); }
            if (s_cap) s_cap->step_end();
            continue;
        }
        if ((w & 3) == 2) {                    /* CALL: offset | 2 */
            { u32 tgt = w & 0x1FFFFFFCu;
              if (!gcm_io2ea(tgt)) { gcm_fifo_bad_branch("CALL", tgt, w); gcm_fifo_resync_why("unmapped-CALL", &s_fifo_getoff, put); break; }
              if (s_cap) gcm_cap_step(RSX_CAP_STEP_CALL, s_fifo_getoff, put, &w, 1, 0, 0, 0, tgt);
              s_fifo_calloff = s_fifo_getoff + 4;
              s_fifo_getoff  = tgt; }
            if (s_cap) s_cap->step_end();
            continue;
        }
        if (w == 0x00020000u) {                /* RET */
            if (s_cap) gcm_cap_step(RSX_CAP_STEP_RET, s_fifo_getoff, put, &w, 1, 0, 0, 0, s_fifo_calloff);
            s_fifo_getoff = s_fifo_calloff;
            s_fifo_calloff = 0;
            if (s_cap) s_cap->step_end();
            continue;
        }
        if (type == 0 || type == 2) {          /* method (incrementing / NI) */
            u32 count  = (w >> 18) & 0x7FF;
            u32 method = w & 0x1FFCu;
            u32 subch  = (w >> 13) & 7;
            /* TCONST_FIFO=1: how the title actually streams vertex constants.
             * NV4097_SET_TRANSFORM_CONSTANT is a 32-register window; if a block
             * arrives NON-INCREMENTING then every dword carries the same method
             * and any handler that derives the constant slot from the method
             * offset writes them all on top of each other. */
            { static int tf = -1;
              if (tf < 0) { const char* e = getenv("TCONST_FIFO"); tf = e ? atoi(e) : 0; }
              if (tf && subch == 0) {
                  static u32 hist[2048]; static u32 tot[2048]; static int dumped = 0;
                  u32 mi = (method >> 2) & 2047;
                  hist[mi]++; tot[mi] += count;
                  static u64 seen = 0;
                  if (++seen == 200000ull && !dumped) { dumped = 1;
                      fprintf(stderr, "[TCFIFO] method histogram (blocks, dwords):%c", 10);
                      for (u32 k = 0; k < 2048; k++) if (hist[k])
                          fprintf(stderr, "[TCFIFO]   0x%04X  %8u blocks %10u dwords%c",
                                  k << 2, hist[k], tot[k], 10);
                  }
              } }
            /* NV406E semaphore: OFFSET(0x64) / ACQUIRE(0x68) / RELEASE(0x6C).
             *
             * These were falling through to rsx_process_method as "unknown method" --
             * i.e. no-ops. A title that syncs with cellGcmSetFlipCommandWithWaitLabel and
             * cellGcmGetLabelAddress (Twisted Metal does both) then waits on a label the
             * RSX is supposed to write and never sees it move, so it never appends its
             * next FIFO segment and the GPU sits on a park forever.
             *
             * RELEASE writes the value; ACQUIRE stops this drain pass without consuming
             * the method, so the next pass re-reads it -- which is what the hardware does
             * while it waits. Semaphore offsets index the same label window
             * cellGcmGetLabelAddress hands out. */
            u32 cw[1 + 0x7FF];
            if (s_cap) {
                /* The ring words behind PUT were stored before PUT and are not
                 * written again until the walker has passed them (the same
                 * contract the acquire fence above relies on), so reading the
                 * packet here for the capture and again below sees one value. */
                cw[0] = w;
                for (u32 i = 0; i < count; i++) {
                    const u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);
                    if (!dea)
                        gcm_cap_fail("fifo-unmapped", "a method packet's data word has no IO mapping");
                    cw[1 + i] = vm_read32(dea);
                }
                gcm_cap_step(RSX_CAP_STEP_METHOD, s_fifo_getoff, put, cw, 1 + count, subch, type == 0, count, 0);
            }
            if (subch == 0 && (method == 0x64u || method == 0x68u || method == 0x6Cu)) {
                int sem_blocked = 0;
                for (u32 i = 0; i < count; i++) {
                    u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);
                    if (!dea) break;
                    u32 m = (type == 0) ? method + i * 4 : method;
                    u32 v = vm_read32(dea);
                    u32 la = GCM_LABEL_GUEST_BASE + (s_sema_offset & 0xFFFFu);
                    const u32 lv = (m == 0x68u) ? gcm_cap_read32(s_sema_offset, la, RSX_CAP_READ_LABEL) : 0;
                    { static int sn = 0, recdbg = -1; if (recdbg < 0) recdbg = getenv("GCM_RECDBG") ? 1 : 0;
                      if (recdbg && PS3_LOG_FIRST(sn, 12))
                        fprintf(stderr, "[SEMA] m=0x%02X v=0x%08X off=0x%X\n", m, v, s_sema_offset); }
                    /* PS3_FTRACE: an acquire this walker sat on is satisfied now. */
                    if (m == 0x68u && s_ft_sema_t0 && lv == v) {
                        ps3_ft_span(PS3_FT_SEMA, 0, s_sema_offset, v, s_ft_sema_t0, ps3_ft_now());
                        s_ft_sema_t0 = 0;
                    }
                    if (m == 0x64u)      s_sema_offset = v;
                    else if (m == 0x6Cu) {
                        /* The RSX has fetched past a RELEASE before its
                         * semaphore write lands, so a guest that sees the
                         * label must also see `get` past the command. `get`
                         * was published only at a JUMP and at the end of the
                         * pass: Ratchet & Clank: Tools of Destruction waits
                         * for a label, then reads `get` once and spins until
                         * its own `put` equals it, which a snapshot from
                         * before the RELEASE never does. GCM_GET_EQ_PUT keeps
                         * publishing put at the end of the pass instead. */
                        if (!gcm_get_eq_put())
                            vm_write32(GCM_CONTROL_GUEST_ADDR + 4, s_fifo_getoff + 4 + count * 4);
                        atomic_thread_fence(memory_order_release);
                        vm_write32(la, v);
                        if (s_cap) s_cap->write(RSX_CAP_LOC_MAIN, s_sema_offset, la, vm_base + la, 4, RSX_CAP_WRITE_LABEL);
                        { static int rn = 0; if (s_sema_offset != 0x400u && PS3_LOG_FIRST(rn, 40))   /* TEMP */
                            fprintf(stderr, "[SEMA-REL] get=0x%08X off=0x%X val=0x%08X\n", s_fifo_getoff, s_sema_offset, v); }
                    }
                    else if (m == 0x68u && lv != v) {
                        /* ACQUIRE stalls the RSX until the label holds the value; it consumes the
                         * method only then. The one agent the runtime models is the driver's
                         * answer to a queued flip: libgcm_sys's flip stream (E944, then RELEASE
                         * label 0x30 = 0, ACQUIRE label 0x30 == 1) waits for the PPU's queue
                         * interrupt handler to store 1 there [INFERENCE: the handler is the
                         * driver's, and is not delivered here]. Every other label is written by a
                         * guest agent (an SPU job, a PPU thread); if it never comes the hardware
                         * waits for ever, so the run stops with a named stop instead of
                         * stalling silently (gcm.wait_timeout_ms). */
                        if (s_sema_offset == GCM_LABEL_FLIP_READY && v == 1u && s_drv_queue_pending) {
                            s_drv_queue_pending = 0;
                            s_acq_t0 = 0;
                            atomic_thread_fence(memory_order_release);
                            vm_write32(la, v);
                            if (s_cap) s_cap->write(RSX_CAP_LOC_MAIN, s_sema_offset, la, vm_base + la, 4, RSX_CAP_WRITE_LABEL);
                        } else {
                            extern unsigned long long ps3_ms_now(void);
                            const unsigned long long now = ps3_ms_now();
                            if (!s_acq_t0 || s_acq_off != s_sema_offset || s_acq_val != v) {
                                s_acq_off = s_sema_offset; s_acq_val = v; s_acq_t0 = now;
                            }
                            const unsigned long long waited = now - s_acq_t0;
                            if (waited >= ps3_cfg_gcm_wait_timeout_ms())
                                ps3_stop_gcm_wait_timeout("cellGcm_rsx_process_fifo", "semaphore-acquire",
                                                          s_sema_offset, v, lv, (u32)waited);
                            if (PS3_FT_ON() && !s_ft_sema_t0) s_ft_sema_t0 = ps3_ft_now();
                            sem_blocked = 1; break;
                        }
                    }
                    else if (m == 0x68u) s_acq_t0 = 0;                /* satisfied */
                }
                if (sem_blocked) { if (s_cap) s_cap->step_cancel(); s_sema_blocked_pass = 1; s_acquire_blocked = 1; break; }
                s_fifo_getoff += 4 + count * 4;
                if (s_cap) s_cap->step_end();
                continue;
            }
            int drv_flip = 0;   /* a GCM_FLIP_HEAD (0xE924) ran in this method block */
            /* A run of 3D methods inside one 1 MB IO page, with no per-word
             * diagnostic on: its words are contiguous in guest memory, so the
             * offset is translated once and the per-word checks below, none of
             * which can match, are skipped. Most of a frame's words come this
             * way (God of War HD: about five million vertex-program microcode and
             * constant words a second).
             *
             * Not "none of which can match" for everything the per-word loop does, so the run is
             * taken only where that holds: a slot bound to NV4097, and methods above channel
             * control. SET_OBJECT (which binds the slot's class), the puller's own words and every
             * other class go the per-word way. Every word the run consumes still ends in a method
             * verdict, so the census (gcm_method_stats) counts the same words either way. The
             * capture step is closed here as it is at the end of the loop.
             * Never with a capture sink attached: the capture contract wants every consumed word a
             * step, in executed order, so the per-word path runs then. */
            if (!wdiag && !s_cap && count && method >= 0x100u && subch != 7 &&
                s_subch_class[subch & 7u] == RSX_CLASS_NV4097 &&
                ((s_fifo_getoff + 4) >> 20) == ((s_fifo_getoff + count * 4) >> 20)) {
                const u32 dea = gcm_io2ea(s_fifo_getoff + 4);
                if (dea) {
                    if (type == 0 && rsx_process_vp_run(&s_state, method, vm_base + dea, count)) {
                        /* Microcode and constant words: the state machine takes every one. */
                        atomic_fetch_add_explicit(&s_mc_handled, count, memory_order_relaxed);
                    } else {
                        u32 n_handled = 0;
                        for (u32 i = 0; i < count; i++) {
                            const u32 m1 = type == 0 ? method + i * 4 : method;
                            const u32 v1 = vm_read32(dea + i * 4);
                            if (gcm_3d_method(&s_state, m1, v1, legacy_state_live)) n_handled++;
                            else gcm_method_verdict(subch, m1, v1, 0, s_fifo_getoff, s_fifo_getoff + 4 + i * 4);
                        }
                        if (n_handled) atomic_fetch_add_explicit(&s_mc_handled, n_handled, memory_order_relaxed);
                    }
                    s_fifo_getoff += 4 + count * 4;
                    if (s_cap) s_cap->step_end();
                    continue;
                }
            }
            for (u32 i = 0; i < count; i++) {
                u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);
                if (!dea) break;
                u32 m = (type == 0) ? method + i * 4 : method;
                const u32 mdata = vm_read32(dea);
                /* SET_OBJECT binds before anything else reads the word: what follows on this subchannel
                 * is that object's. */
                if (m == 0) gcm_bind_object(subch, mdata);
                /* NV0039 is routed by its binding, not by a subchannel number. Its methods never reach
                 * the 3D register file or the live-draw mirror: NV0039's 0x0304-0x0328 are NV4097's
                 * alpha, blend and colour-mask state there. The puller's words (0 < m < 0x100) are
                 * NV406E on every subchannel, whatever is bound, and go the channel-control way below. */
                if (s_subch_class[subch] == RSX_CLASS_NV0039 && (m == 0 || m >= 0x100u) && !gcm_driver_word(subch, m)) {
                    gcm_method_verdict(subch, m, mdata, gcm_nv0039_method(m, mdata),
                                       s_fifo_getoff, s_fifo_getoff + 4 + i * 4);
                    continue;
                }

                /* GCM_SET_USER_COMMAND is method 0xEB00, which this decode
                 * splits into subchannel 7 / method 0x0B00 -- so it was landing
                 * in the 2D engine path and being dropped as unrecognised. It
                 * is not a 2D method at all: it tells the RSX to raise a
                 * USER_CMD interrupt, which is what ps1_netemu's flip path
                 * blocks on. */
                if (wdiag) { static int sd = -1; static unsigned long s7 = 0;
                  if (sd < 0) sd = getenv("GCM_SUBCH7") ? 1 : 0;
                  if (sd && subch == 7 && (PS3_LOG_FIRST(s7, 16)))
                      fprintf(stderr, "[subch7] method=0x%04X data=0x%08X\n",
                              m, vm_read32(dea)); }
                if (subch == 7 && m == 0x0B00u) {
                    extern void rsx_raise_user_cmd(u32 arg);
                    rsx_raise_user_cmd(vm_read32(dea));
                    /* A title that registered its handler with the HLE
                     * cellGcmSetUserHandler has no lv2 interrupt thread
                     * reading that event; its handler runs from the pump.
                     * The `continue` skips the 0xEB00 case below, so queue
                     * it for the pump here. */
                    gcm_driver_event(GCM_DRV_EVENT_USER_COMMAND, vm_read32(dea));
                    if (s_user_handler_opd) cellGcmQueueUserCommand(vm_read32(dea));
                    gcm_method_verdict(subch, m, mdata, 1, s_fifo_getoff, s_fifo_getoff + 4 + i * 4);
                    continue;
                }
                /* GCM_OBJDBG=1: log every SET_OBJECT (method 0). The handle a
                 * title binds to a subchannel is what says which class that
                 * slot decodes, so the log shows what the binding table holds. */
                if (wdiag) { static int od = -1;
                  if (od < 0) { const char* e = getenv("GCM_OBJDBG"); od = e ? 1 : 0; }
                  if (od && m == 0) {
                      static unsigned seen[8]; static int have[8];
                      const unsigned h = vm_read32(dea);
                      if (subch < 8 && (!have[subch] || seen[subch] != h)) {
                          have[subch] = 1; seen[subch] = h;
                          printf("[GCM-OBJ] subch=%u SET_OBJECT handle=0x%08X%c",
                                 subch, h, 10); fflush(stdout);
                      } } }
                /* The subchannel is a binding slot, not an engine selector: a title may bind NV4097 to
                 * something other than subchannel 0 (Twisted Metal uses subchannel 1, and Yakuza Dead
                 * Souls' SPU-built command lists bind it elsewhere), and libgcm's own default state binds
                 * subchannel 1 to NV0039. A word is therefore interpreted in the class SET_OBJECT bound on
                 * its slot (gcm_method_class), never by the slot's number. */
                /* GCM_SUBCH_GATE=<file>: while <file> exists, log every method on a
                 * non-zero subchannel -- what each binding slot actually carries. */
                if (wdiag) { static const char* sg = (const char*)-1; static unsigned poll; static int open_g;
                  if (sg == (const char*)-1) sg = getenv("GCM_SUBCH_GATE");
                  const u32 cm3 = m & 0x1FFCu;
                  /* GCM_VAL_WATCH=<hex>: every FIFO word carrying that value, any method. */
                  { static u32 vw = 1;
                    if (vw == 1) { const char* e = getenv("GCM_VAL_WATCH"); vw = e ? (u32)strtoul(e, 0, 16) : 0; }
                    if (vw && vm_read32(dea) == vw) {
                        static int vn; if (PS3_LOG_FIRST(vn, 64))
                            printf("[valwatch] subch=%u method=0x%04X = 0x%08X get=0x%X%c", subch, m, vw, s_fifo_getoff, 10);
                    } }
                  /* ...and surface offsets (colour 0x210/0x218/0x288/0x28C, zeta 0x214) and texture 1 offset */
                  if (sg && !subch && (cm3 == 0x210 || cm3 == 0x214 || cm3 == 0x218 || cm3 == 0x288 || cm3 == 0x28C ||
                                       cm3 == 0x1A20)) {
                      if ((poll++ & 255u) == 0) { FILE* g = fopen(sg, "rb"); open_g = g != NULL; if (g) fclose(g); }
                      if (open_g) printf("[surfreg] 0x%04X = 0x%08X%c", cm3, vm_read32(dea), 10);
                  }
                  if (sg && subch) {
                      if ((poll++ & 255u) == 0) { FILE* g = fopen(sg, "rb"); open_g = g != NULL; if (g) fclose(g); }
                      if (open_g) printf("[subch] %u 0x%04X = 0x%08X%c", subch, m, vm_read32(dea), 10);
                  } }
                /* Mirror the NV4097 method stream into the live NV4097->D3D12
                 * engine (caner / canersaka). Inert unless RSX_LIVE_DRAW is set.
                 * It wants the raw method with its subchannel bits -- it
                 * canonicalises with & 0x1FFC itself, which is why only words
                 * of a slot bound to NV4097 (and the driver's) may go. */
                if (wdiag) { static int live = -1;
                  if (live < 0) live = rsx_live_draw_enabled();
                  if (live && gcm_feeds_3d_mirror(subch, m)) rsx_live_draw_method(gcm_mirror_method(subch, m), vm_read32(dea)); }
                /* GCM_METHOD_HIST=<secs>: every <secs>, the most frequent
                 * (subchannel, method) pairs dispatched since the last report --
                 * what a frame is actually made of when nothing draws. */
                if (wdiag) { static int hs = -1; static u32 cnt[8][0x800]; static unsigned long long t0;
                  if (hs < 0) { const char* e = getenv("GCM_METHOD_HIST"); hs = e ? atoi(e) : 0; }
                  if (hs > 0) {
                      cnt[subch & 7][(m & 0x1FFCu) >> 2]++;
                      extern unsigned long long ps3_ms_now(void);
                      const unsigned long long now = ps3_ms_now();
                      if (!t0) t0 = now;
                      if (now - t0 >= (unsigned long long)hs * 1000u) {
                          t0 = now;
                          fprintf(stderr, "[mhist]");
                          for (int top = 0; top < 16; top++) {
                              u32 bs = 0, bm = 0, bc = 0;
                              for (u32 sc = 0; sc < 8; sc++) for (u32 mi = 0; mi < 0x800; mi++)
                                  if (cnt[sc][mi] > bc) { bc = cnt[sc][mi]; bs = sc; bm = mi; }
                              if (!bc) break;
                              fprintf(stderr, " %u:%04X=%u", bs, bm << 2, bc);
                              cnt[bs][bm] = 0;
                          }
                          fprintf(stderr, "\n");
                          memset(cnt, 0, sizeof cnt);
                      } } }

                /* And into the platform-neutral register-file draw engine,
                 * which is the macOS path under PS3RECOMP_RSX_ENGINE=dispatch.
                 * It wants the raw method with its subchannel bits for the
                 * same reason the live engine does, and masks them itself. */
                if (wdiag) { static int eng = -1;
                  if (eng < 0) eng = rsx_draw_engine_enabled();
                  if (eng && gcm_feeds_3d_mirror(subch, m)) rsx_draw_engine_method(gcm_mirror_method(subch, m), vm_read32(dea)); }

                /* RSX_LIVE_FEED_DBG=1: what the FIFO actually carries. Counted
                 * for any run, live engine or not, so the method stream and the
                 * backend's draw callbacks can be compared in one measurement. */
                if (wdiag) { static int fdbg = -1;
                  if (fdbg < 0) fdbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
                  if (fdbg) {
                      static long long n_be = 0, n_va = 0, n_ia = 0, tick = 0;
                      const u32 cm = m & 0x1FFCu;
                      if      (cm == 0x1808u) n_be++;
                      else if (cm == 0x1814u) n_va++;
                      else if (cm == 0x1824u) n_ia++;
                      if (((++tick) % 200000) == 0)
                          printf("[feed] begin_end=%lld draw_arrays=%lld draw_index=%lld\n",
                                 n_be, n_va, n_ia), fflush(stdout);
                  } }

                /* No object bound: no class decodes the method, so the console drops the write
                 * (envytools fifo/nv_object.xml: the subchannel's object is what gives a method its
                 * meaning). The driver words below are the LV1's, not an engine's, so subchannel 7 is
                 * left alone; a bind or an LV1 word is not dropped. Fail-closed: a word that is not one of
                 * the driver's (gcm_driver_word) on an unbound slot 7 is not exempt here, it ends as unknown. */
                if (m >= 0x100u && subch != 7 && s_subch_class[subch] == RSX_CLASS_UNBOUND) {
                    gcm_method_unbound(subch, m, mdata);
                    continue;
                }
                /* Driver methods (0xE9xx, 0xEBxx) occupy bits 2-15 of the
                 * FIFO word; the standard 11-bit extraction (w & 0x1FFC)
                 * truncates them.  Reconstruct the full address the same way
                 * the draw-engine call above does. */
                { const u32 mfull = (subch << 13) | m;
                const rsx_class_id cls = gcm_method_class(subch, m);
                int handled = 1;   /* the driver words below are consumed in place */
                if (mfull == 0xEB00u || mfull == 0xEB04u) {
                    gcm_driver_event(GCM_DRV_EVENT_USER_COMMAND, vm_read32(dea));
                    cellGcmQueueUserCommand(vm_read32(dea));
                } else if (mfull == 0xE944u) {
                    /* GCM_DRIVER_QUEUE index 1 (base 0xE940): the flip the next 0xE924 shows.
                     * _cellGcmSetFlipCommand (libgcm_sys 0x02008948) writes the display buffer id
                     * here, ahead of its label handshake and `CALL 0`. */
                    s_drv_flip_queue = vm_read32(dea) & 0xFFu;
                    s_drv_queue_pending = 1;
                    gcm_driver_event(GCM_DRV_EVENT_QUEUE_FLIP, s_drv_flip_queue);
                } else if (mfull == 0xE924u && (vm_read32(dea) & 0x80000000u)) {
                    /* GCM_FLIP_HEAD index 1 (base 0xE920), argument 0x8000010F as libgcm_sys writes it
                     * (bit 31 set): flip to the buffer 0xE944 queued. The same present half as the
                     * 0xFEAD marker; the guest-visible half ran when the command was emitted
                     * (cellGcm_emit_flip_command). */
                    if (s_drv_flip_queue < CELL_GCM_MAX_DISPLAY_BUFFER_NUM &&
                        s_display_buffer_set[s_drv_flip_queue]) {
                        gcm_set_current_display_buffer(s_drv_flip_queue);
                        s_flip_pending = 1;
                        s_flip_request_count++;
                        if (s_fifo_flips_reached != s_fifo_flips_queued) s_fifo_flips_reached++;
                    }
                    gcm_driver_event(GCM_DRV_EVENT_FLIP_HEAD, s_drv_flip_queue);
                    s_drv_flip_label_pending = 1;
                    drv_flip = 1;
                } else if (mfull == 0xE920u || mfull == 0xE924u) {
                    cellGcmSetFlipCommand(vm_read32(dea) & 7u);
                } else if (cls == RSX_CLASS_NV4097 || cls == RSX_CLASS_NV406E) {
                    handled = gcm_3d_method(&s_state, m, vm_read32(dea), legacy_state_live);
                } else if (cls == RSX_CLASS_NV3062 || cls == RSX_CLASS_NV308A ||
                           cls == RSX_CLASS_NV309E || cls == RSX_CLASS_NV3089) {
                    handled = gcm_2d_method(subch, cls, m, vm_read32(dea)) || m == 0;
                } else {
                    /* The driver object and a handle this table does not know decode no method but the
                     * bind itself; any other word has no consumer and ends as unknown. */
                    handled = m == 0;
                }
                gcm_method_verdict(subch, m, mdata, handled, s_fifo_getoff, s_fifo_getoff + 4 + i * 4);
                }
            }
            s_fifo_getoff += 4 + count * 4;
            if (s_cap) s_cap->step_end();
            /* A driver flip head stops the pass like the 0xFEAD marker does, so the ticker presents
             * the completed frame before the next one's head is drained. */
            if (drv_flip && gcm_fifo_backlog(put, s_fifo_getoff) < GCM_FIFO_CATCHUP_BYTES)
                { why = "flip"; s_acquire_blocked = 1; break; }
            continue;
        }
        { static int _uw = 0; if (PS3_LOG_FIRST(_uw, 16) && getenv("RTT_DUMP"))
            fprintf(stderr, "[FIFOUW] unknown word 0x%08X at getoff 0x%X\n", w, s_fifo_getoff); }
        if (s_cap) { gcm_cap_step(RSX_CAP_STEP_WORD, s_fifo_getoff, put, &w, 1, 0, 0, 0, 0); s_cap->step_end(); }
        s_fifo_getoff += 4;                    /* unknown word: skip */
    }

    /* Walker watchdog. Everything above recovers from a *recognised* stall --
     * a branch we cannot take, a park the title abandoned. A walker that stops
     * for any other reason simply freezes `get` for the rest of the run and
     * nothing renders again, and which of those happens is a race, so the same
     * boot renders one time in five. If `get` has not moved across several
     * passes while `put` is somewhere else, there is work the walker will
     * never reach: resynchronise to the write head. Same trade as the
     * unmapped-get path -- commands are lost, the FIFO lives. */
    /* The stuck-detector above only fires when `get` stops MOVING. A walker
     * going in circles moves plenty -- it just never arrives. Twisted Metal
     * and flOw both write one ring and their JUMPs lead to `put`; VF5 does
     * not. AMGL leaves libgcm's default ring at IO 0 parked on its own JUMP
     * loop and drives the RSX from a second buffer it mapped at IO 0xA00000,
     * so the walker circulates the default ring forever, `get` never reaches
     * `put`, the title's ring is never reclaimed, and AMGL prints "Command
     * Buffer Overflow" until it runs off the end of its own mapping.
     *
     * A drain that burns its ENTIRE word budget without arriving is that
     * signature: no legitimate frame leaves a million words pending, and a
     * genuine burst clears within a tick or two. Circling does not, so
     * require several passes in a row before following the write head.
     *
     * ponytail: consecutive-budget-burn is a heuristic, not a proof. The real
     * fix is knowing which buffer the RSX is bound to -- that needs the
     * SetQueueHandler / context-switch path modelled, not a walker rule. */
    { static int burned = 0, passes = -1;
      if (passes < 0) { const char* e = getenv("GCM_FIFO_CIRCLE_PASSES");
                        passes = e ? atoi(e) : 4; }
      if (passes && budget <= 0 && s_fifo_getoff != put) {
          if (++burned >= passes) {
              static int n = 0;
              if (PS3_LOG_FIRST(n, 8))
                  fprintf(stderr, "[cellGcmSys] FIFO walker circling (get=0x%08X, "
                          "put=0x%08X, full budget burned x%d) -- following the "
                          "write head\n", s_fifo_getoff, put, burned);
              /* What the walker circles in: the words at get, and where the
               * buffer it is in was jumped/called from (GCM_CIRCLE_DUMP=1). */
              if (n <= 3 && getenv("GCM_CIRCLE_DUMP"))
                  for (u32 t = 0; t < 8; t++) {
                      u32 q = (s_cf_i + t) % 8;
                      if (s_cf_w[q]) fprintf(stderr, "[circle-jump] at io=%08X w=%08X\n", s_cf_off[q], s_cf_w[q]);
                  }
              if (n <= 3 && getenv("GCM_CIRCLE_DUMP"))
                  for (int k = -8; k <= 24; k++) {
                      u32 io = s_fifo_getoff + (u32)(k * 4), ea = gcm_io2ea(io);
                      if (ea) fprintf(stderr, "[circle] %+3d io=%08X ea=%08X w=%08X\n", k, io, ea, vm_read32(ea));
                  }
              gcm_fifo_resync_why("circling", &s_fifo_getoff, put);
              burned = 0;
          }
      } else burned = 0; }

    /* A drain stopped on a semaphore acquire holds `get`
     * on purpose: that is the wait, not a stuck walker, and the stop
     * after gcm.wait_timeout_ms is what ends it. So is a self-JUMP park,
     * which gcm.wait_timeout_ms stops (Reference mode) or gcm.park_grace_ms
     * ends (Compatibility mode, gcm.park_resync). This counter counts drain passes, not
     * time: both stop every pass at the same `get`, so it fired a few
     * milliseconds in and resynced to put. For a park that is before the
     * title has patched it into a JUMP to the commands it just wrote at the
     * ring start, and the resync dropped them: inFAMOUS 2 stopped presenting
     * for good that way in about one New Marais run in five ("stuck-get" at a
     * self-jump with no "jump-park" before it). */
    { static u32 last = 0xFFFFFFFFu; static int stuck = 0;
      if (s_sema_blocked_pass || s_park_pass) {
          stuck = 0; last = s_fifo_getoff; s_sema_blocked_pass = 0; s_park_pass = 0; }
      else if (s_fifo_getoff == last && s_fifo_getoff != put) {
          if (++stuck >= 8) { gcm_fifo_dump_around(s_fifo_getoff);
                             gcm_fifo_resync_why("stuck-get", &s_fifo_getoff, put); stuck = 0; }
      } else { stuck = 0; last = s_fifo_getoff; } }

    /* Publish progress: get chases put; ref advances at most ONE queued fence
     * per tick (gcm_ref_push/gcm_ref_publish_one above) so every SET_REFERENCE
     * value stays observable to equality-waiters. The wrap recycle path polls
     * the drained EA. */
    if (s_draindbg) {
        static int n2 = 0;
        if (n2++ % s_drain_every == 0)
            fprintf(stderr, "[DRAINEND] %s: %08X -> %08X (%u bytes), put=%08X\n",
                    why, start_off, s_fifo_getoff, s_fifo_getoff - start_off, put);
    }
    /* Recycle the TITLE'S OWN command buffer when its callback will not.
     *
     * gCellGcmCurrentContext points at the title's context, not the one
     * cellGcmInit handed back: Virtua Fighter 5's is a 512 KB ring at EA
     * 0x4AE00000, and the buffer-full callback AMGL installs (0x006A8030) only
     * prints "[AMGL]:[ERROR] Command Buffer Overflow!" and returns 0 -- which
     * libgcm's gcmReserve reads as "retry", so the title writes straight past
     * `end` into unmapped memory and never renders again.
     *
     * Hardware never reaches that state because the ring is recycled once the
     * RSX has consumed it. Do the same here, on the title's context, and only
     * when it is provably safe: the walker has drained everything the title
     * submitted (get == put) and `current` is within one reserve of `end`.
     * A title whose own callback recycles never reaches that state, so this is
     * inert for every port that works today.
     *
     * ponytail: append the JUMP at `current` the way the SDK's own callback
     * does, rather than tracking a second copy of the ring's geometry. */
    if (s_gcm_ctx_out_ea && !(s_ib.valid && vm_read32(s_gcm_ctx_out_ea) == s_ib.ctx_ea)) {
        u32 ctx = vm_read32(s_gcm_ctx_out_ea);
        u32 begin = ctx ? vm_read32(ctx + 0x0) : 0;
        u32 end   = ctx ? vm_read32(ctx + 0x4) : 0;
        u32 cur   = ctx ? vm_read32(ctx + 0x8) : 0;
        /* `get == put` -- a fully drained FIFO -- was too strict. It holds
         * while a title is only clearing, and stops holding the moment its SPU
         * work starts producing real command volume: the walker then always has
         * a backlog, the recycle never fires, and the ring overflows again.
         * That is exactly where Virtua Fighter 5 stalls once its "SPU Delegate"
         * group starts feeding the renderer.
         *
         * Hardware does not need a drained FIFO either -- only that the bytes
         * about to be overwritten have already been read. So require the walker
         * to be a margin PAST `begin` instead: the head of the ring is consumed,
         * which is the region the guest is about to write. */
        u32 io_begin_chk = begin ? gcm_ea2io(begin) : 0xFFFFFFFFu;
        int head_consumed =
            (io_begin_chk != 0xFFFFFFFFu) &&
            (s_fifo_getoff == put ||
             (s_fifo_getoff > io_begin_chk &&
              s_fifo_getoff - io_begin_chk >= GCM_RECYCLE_MARGIN));
        /* Already overrun. gcmReserve writes past `end` when its callback
         * declines to recycle (AMGL's only prints), and once the guest is out
         * there the walker follows into memory that is not the ring: `get`
         * stops advancing, head_consumed can never become true again, and the
         * title is wedged for good -- which is exactly where Virtua Fighter 5
         * stops, at the same flip every run however long it is left.
         *
         * There is nothing left to protect at that point. Recycling loses
         * whatever was written past the end; not recycling loses the rest of
         * the run. */
        /* A pass that stopped at a flip has a backlog on purpose (the next
         * frame), and teleporting `get` to `begin` would drop it -- fence
         * writes included, so the title then waits on a reference that never
         * comes (The Simpsons Arcade Game froze at its 4th recycle). The next
         * pass drains to `put` and recycles then. */
        if (!strcmp(why, "flip") && s_fifo_getoff != put) head_consumed = 0;
        /* Only once the title has been SEEN to overrun. Near `end` is also
         * where a title whose callback does recycle sits just before calling
         * it: Tornado Outbreak's steps its context through 64 KB segments of a
         * larger ring, and recycling under it from this thread rewound put/get
         * behind its callback's bookkeeping -- which then waited forever for a
         * GET that could not arrive. A callback that declines (AMGL's) always
         * overruns first, so the first overrun is recycled by the path above
         * and near-end recycling is enabled from then on. */
        static int s_title_overran = 0;
        if (s_fifo_recycle_waiting) head_consumed = 0;   /* its callback is recycling */
        else if (begin && end > begin && cur >= end) { head_consumed = 1; s_title_overran = 1; }
        if (begin && end > begin && cur >= begin && cur + GCM_RECYCLE_SLACK >= end
                && head_consumed && s_title_overran) {
            u32 io_begin = gcm_ea2io(begin);
            if (io_begin != 0xFFFFFFFFu) {
                u32 jmp_at = (cur + 4 <= end) ? cur : end - 4;
                vm_write32(jmp_at, 0x20000000u | io_begin);   /* JUMP -> begin */
                vm_write32(ctx + 0x8, begin);                 /* current = begin */
                vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_begin);  /* put */
                s_fifo_getoff = io_begin;                          /* get */
                put = io_begin;
                /* Count it as the walker taking the JUMP to begin. The title's
                 * own callback can be recycling at the same moment: it writes
                 * its JUMP and `put`, and only then marks itself waiting, so a
                 * pass that read the mark clear can still land here. It then
                 * waits for a backward JUMP that never comes, because `get` is
                 * already at begin == put. God of War HD stopped for 2-4 s at
                 * random points in gameplay this way. */
                s_fifo_back_jumps++;
                static int n = 0;
                if (PS3_LOG_FIRST(n, 8))
                    fprintf(stderr, "[cellGcmSys] recycled the title's own ring "
                            "(ctx=0x%08X begin=0x%08X end=0x%08X) -- its callback "
                            "does not%c", ctx, begin, end, 10);
            }
        }
    }

    g_gcm_fifo_drained_ea = gcm_io2ea(s_fifo_getoff);
    /* GCM_GET_EQ_PUT=1: publish `get` as having reached `put` rather than where
     * the walker actually is. A probe, not a fix -- it removes the back-pressure
     * a title uses to avoid overwriting commands the GPU has not read. It exists
     * to answer one question for a title that reports its ring full while the
     * walker says it is drained: is the title reading `get`, or something else?
     * If the overflow survives this, `get` is not what it consults. */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, gcm_get_eq_put() ? put : s_fifo_getoff);

    AcquireSRWLockExclusive(&s_ref_pub_lock);                           /* ref */
    gcm_ref_publish_one();
    ReleaseSRWLockExclusive(&s_ref_pub_lock);
}

/* FIFO command-buffer-full callback body. The title's inline gcmReserve calls
 * context->callback(context, count) (routed here via the synthetic OPD) when the
 * ring's `current` nears `end`. Real libgcm flushes, waits for the RSX `get` to
 * pass, then recycles `current` to `begin`. We mirror that: wait (bounded) for
 * the ticker drain to consume everything up to `current`, then reset current to
 * begin. Runs on the guest thread; the ticker owns all rendering, so we only
 * pointer-shuffle here (the drain's own wrap-detect handles s_get). Without this
 * the ring never recycles and the whole pipeline wedges once the FIFO wraps
 * (~200 frames at the 256KB default), which looked like a GPU/driver hang. */
static u32 gcm_ea2io(u32 ea)
{
    u32 page = ea >> 20;
    if (page >= 65536) return 0xFFFFFFFFu;
    u16 io_page = s_io_address_table[page];
    if (io_page == 0xFFFF) return 0xFFFFFFFFu;
    return ((u32)io_page << 20) | (ea & 0xFFFFFu);
}

/* The default command buffer's buffer-full callback (libgcm_sys 0x02003AAC; layout in GcmIb).
 * Flush, JUMP from the old `current` to the next 32 KiB segment, move the context onto it, then
 * wait while the RSX is still inside that segment (it would be overwritten under it) or outside
 * the ring altogether (running the default-state subroutine at io 0). The last segment wraps to
 * the start of the command area. The firmware sleeps in a loop for this; so does this, bounded by
 * gcm.wait_timeout_ms (the hardware waits for ever). Runs on the guest thread; the walker is
 * the RSX and moves `get`. */
static void gcm_segment_switch(u32 ctx)
{
    extern unsigned long long ps3_ms_now(void);
    const u32 cur = vm_read32(ctx + 0x8), end = vm_read32(ctx + 0x4);
    const u32 cur_off = gcm_ea2io(cur);
    if (cur_off != 0xFFFFFFFFu) {
        atomic_thread_fence(memory_order_release);
        vm_write32(GCM_CONTROL_GUEST_ADDR + 0, cur_off);                 /* flush: put = current */
    }
    u32 start, segend;
    if (end == s_ib.last_end) {
        start  = s_ib.io_ea + GCM_WRAP_START_WORDS * 4u;
        segend = s_ib.io_ea + s_ib.seg_words * 4u - 4u;
    } else {
        start  = end + 4u;
        segend = start + s_ib.seg_words * 4u - 4u;
    }
    const u32 start_off = gcm_ea2io(start), end_off = gcm_ea2io(segend);
    vm_write32(cur, 0x20000000u | start_off);                            /* JUMP to the segment */
    vm_write32(ctx + 0x4, segend);
    vm_write32(ctx + 0x0, start);
    atomic_thread_fence(memory_order_release);
    vm_write32(ctx + 0x8, start);

    const unsigned long long t0 = ps3_ms_now();
    s_fifo_recycle_waiting = 1;
    for (;;) {
        const u32 get = vm_read32(GCM_CONTROL_GUEST_ADDR + 4);
        const int in_segment = get >= start_off && get <= end_off;
        const int outside    = get < s_ib.first_begin_off || get > s_ib.last_end_off;
        if (!in_segment && !outside) break;
        const unsigned long long waited = ps3_ms_now() - t0;
        if (waited >= ps3_cfg_gcm_wait_timeout_ms()) {
            s_fifo_recycle_waiting = 0;
            ps3_stop_gcm_wait_timeout("gcm-segment-callback", "segment-busy", start_off, end_off,
                                      get, (u32)waited);
        }
        Sleep(1);
    }
    s_fifo_recycle_waiting = 0;
}

void cellGcm_fifo_recycle(u32 ctx_ea)
{
    if (!ctx_ea) return;
    if (s_ib.valid && ctx_ea == s_ib.ctx_ea) { gcm_segment_switch(ctx_ea); return; }
    u32 begin   = vm_read32(ctx_ea + 0x0);
    u32 current = vm_read32(ctx_ea + 0x8);
    if (current <= begin) return;                       /* nothing to recycle */

    if (getenv("CELLMARK_BLINKDBG"))
        printf("[RECYCLE] ring wrap: current=0x%08X -> begin=0x%08X\n", current, begin);

    static int s_recdbg = -1;
    if (s_recdbg < 0) s_recdbg = getenv("GCM_RECDBG") ? 1 : 0;
    if (s_recdbg)
        fprintf(stderr, "[REC>] tid=%u begin=%08X cur=%08X put=%08X get=%08X drained=%08X\n",
                (unsigned)GetCurrentThreadId(), begin, current,
                vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 4),
                g_gcm_fifo_drained_ea);

    /* Do what the SDK's default command-buffer-full callback does: append a
     * JUMP-to-begin at the write head and move `put` to begin. The FIFO walker
     * consumes the tail, follows the jump, and idles at begin; then it's safe
     * for the guest to write from begin (that region was consumed long ago). */
    u32 io_begin = gcm_ea2io(begin);
    const u32 back_jumps0 = s_fifo_back_jumps;
    if (io_begin != 0xFFFFFFFFu) {
        vm_write32(current, 0x20000000u | io_begin);            /* JUMP begin  */
        /* The jump has to be in guest memory before `put` moves behind the
         * walker's `get`. Program order is not enough on arm64: the walker
         * may observe the new `put`, find `get` != `put`, and read the old
         * word at `current` before the jump lands there. */
        atomic_thread_fence(memory_order_release);
        vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_begin);        /* put = begin */
    }

    /* Wait (bounded ~2s) for the walker to consume the tail + take the jump so
     * no commands are lost; a stalled ticker degrades to dropped commands.
     * "Took the jump" is a backward JUMP counted after the one written above.
     * Comparing the drained EA with begin let the wait through at once when
     * the walker had not moved yet (drained starts at begin), and the title
     * then overwrote megabytes of commands the RSX had never read. */
    /* The bound is on the walker making no progress, not on the total wait: a
     * title that fills a 3 MB ring with hundreds of tiny frames at boot (God of
     * War HD) leaves a backlog the draw engine needs tens of seconds to execute,
     * and giving up after 2 s handed the title megabytes of commands to write
     * over. A walker that has stopped moving still ends the wait after 2 s, and
     * 60 s caps the whole thing. */
    int spins = 0, idle = 0;
    u32 last_get = s_fifo_getoff;
    s_fifo_recycle_waiting = 1;
    while (io_begin != 0xFFFFFFFFu && s_fifo_back_jumps == back_jumps0 &&
           idle < 2000 && spins < 60000) {
        Sleep(1); spins++;
        if (s_fifo_getoff != last_get) { last_get = s_fifo_getoff; idle = 0; }
        else idle++;
        if (s_recdbg && spins % 1000 == 0)
            fprintf(stderr, "[REC~] %d ms: walker get=%08X put=%08X back_jumps=%u\n",
                    spins, s_fifo_getoff, vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                    s_fifo_back_jumps);
    }
    if (s_fifo_back_jumps == back_jumps0 && io_begin != 0xFFFFFFFFu) {
        static int warned = 0;
        if (PS3_LOG_FIRST(warned, 4))
            printf("[cellGcmSys] fifo recycle: drain stalled (drained=0x%08X begin=0x%08X, %d ms)\n",
                   g_gcm_fifo_drained_ea, begin, spins);
    }

    vm_write32(ctx_ea + 0x8, begin);                    /* recycle ring to base */
    s_fifo_recycle_waiting = 0;                          /* current no longer reads end */

    if (s_recdbg)
        fprintf(stderr, "[REC<] tid=%u cur-now=%08X put=%08X get=%08X drained=%08X spins=%d\n",
                (unsigned)GetCurrentThreadId(), vm_read32(ctx_ea + 0x8),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 4),
                g_gcm_fifo_drained_ea, spins);
}

/* A host that brings its backend up on another thread -- a frame clock that
 * opens the window while the title boots -- can register the draw engine after
 * the title has already called cellGcmSetDisplayBuffer. The engine then knows
 * no scanout, and every flip presents whatever surface happens to be bound:
 * Twisted Metal on Linux showed an offscreen target instead of its composited
 * display buffer. rsx_draw_engine_init calls this to catch up. */
void cellGcm_replay_display_buffers_to_engine(void)
{
    for (u32 i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i])
            rsx_draw_engine_set_display_buffer(i, 0, s_display_buffers[i].offset,
                                               s_display_buffers[i].pitch,
                                               s_display_buffers[i].width,
                                               s_display_buffers[i].height);
}

/* NID: 0xA53D12AE */
s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height)
{
    printf("[cellGcmSys] SetDisplayBuffer(id=%u, offset=0x%X, pitch=%u, %ux%u)\n",
           bufferId, offset, pitch, width, height);

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    s_display_buffers[bufferId].offset = offset;
    s_display_buffers[bufferId].pitch  = pitch;
    s_display_buffers[bufferId].width  = width;
    s_display_buffers[bufferId].height = height;
    s_display_buffer_set[bufferId] = 1;
    if (s_cap) s_cap->display_buffer(bufferId, offset, pitch, width, height);

    /* Tell the live engine too, or it has no registered scanout to present and
     * falls back to whatever surface happens to be current. Display buffers are
     * always in RSX local memory (location 0). */
    if (rsx_live_draw_enabled())
        rsx_live_draw_set_display_buffer(bufferId, 0, offset, pitch, width, height);
    if (rsx_draw_engine_enabled())
        rsx_draw_engine_set_display_buffer(bufferId, 0, offset, pitch, width, height);

    return CELL_OK;
}

/* Backend query (not a guest API): is this raw RSX offset a registered
 * display buffer? Used to split display draws (-> backbuffer) from
 * offscreen render-to-texture passes. */
int cellGcmOffsetIsDisplay(u32 offset)
{
    for (int i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i] && s_display_buffers[i].offset == offset)
            return 1;
    return 0;
}

/* How many display buffers the title has actually registered. cellResc needs
 * it to know how far to rotate its flip target; nothing else about the set is
 * anyone else's business, so this stays a count rather than exposing the array. */
u32 cellGcm_display_buffer_count(void)
{
    u32 n = 0;
    for (u32 i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i]) n++;
    return n;
}

static s32 gcm_flip_request(u32 bufferId, int in_fifo)
{
    /* GCM_FLIPCOUNT=1: every flip, with a timestamp. FLIP_DBG caps at 20 lines,
     * which answers "did it ever flip?" and not "is it still flipping, and how
     * fast?" -- the question that matters when a title stops appearing on
     * screen. Virtua Fighter 5 does not use the in-FIFO 0xFEADxxxx flip word at
     * all (that counter stays at zero for the whole run); it flips through
     * cellGcmSetFlip, so this is where its frame rate actually is. */
    { static int dbg = -1;
      if (dbg < 0) dbg = getenv("GCM_FLIPCOUNT") ? 1 : 0;
      if (dbg) {
          extern unsigned long long ps3_ms_now(void);
          static unsigned long long n = 0, t0 = 0;
          unsigned long long now = ps3_ms_now();
          if (!t0) t0 = now;
          ++n;
          if (n <= 400ull || (n % 10ull) == 0)
              fprintf(stderr, "[flip] %llu at %llu ms%c", n, now - t0, 10);
      } }

    /* GCM_FLIP_BT=<n>: dump the flipping thread's guest stack for the flips
     * around n. When a title renders correctly for a while and then stops
     * submitting -- no error, no unresolved import, no overflow -- the only
     * thing separating the last good frame from the first missing one is what
     * the render path was doing on each. This is the window onto that. */
    { static long long at = -2;
      if (at == -2) { const char* e = getenv("GCM_FLIP_BT"); at = e ? atoll(e) : -1; }
      if (at >= 0) {
          static unsigned long long m = 0;
          ++m;
          if ((long long)m >= at - 5 && (long long)m <= at + 5) {
              extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
              extern void ppu_dump_guest_stack(ppu_context*, const char*);
              if (g_active_ctx) {
                  char tag[48];
                  snprintf(tag, sizeof tag, "flip#%llu", m);
                  ppu_dump_guest_stack(g_active_ctx, tag);
                  { extern void ppu_dump_bctrl_ring(uint32_t, const char*);
                    ppu_dump_bctrl_ring((uint32_t)g_active_ctx->thread_id, tag); }
              }
          }
      } }

    { static int _n=0, _fd=-1; if (_fd < 0) _fd = getenv("FLIP_DBG") ? 1 : 0;
      if (_fd && PS3_LOG_FIRST(_n, 20))
        fprintf(stderr, "[FLIP] SetFlipCommand(buf=%u) set=%d\n",
                bufferId, bufferId < CELL_GCM_MAX_DISPLAY_BUFFER_NUM ? s_display_buffer_set[bufferId] : -1); }
    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (!s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;


    /* in_fifo: the flip sits in the command stream as a GCM_FLIP_MARKER, and
     * the drain does this half when it reaches it (see the 0xFEAD case). */
    if (!in_fifo) {
        gcm_set_current_display_buffer(bufferId);
        s_flip_pending = 1;   /* ticker: present BEFORE the next drain */
        s_flip_request_count++;
        s_flip_direct_pending = 1;
    }
    /* Flip requested but not yet shown: the status stays WAITING until the present thread's
     * cellGcmTickFlip marks it done (vsync). cellGcmSetWaitFlip does not look at it: its wait is the
     * RSX's, on label 0x10. */
    FLIP_LOCK();
    s_flip_status = CELL_GCM_FLIP_STATUS_WAITING;
    s_flip_req_seq++;
    FLIP_UNLOCK();
    s_last_flip_time = get_timestamp_ns();
    if (PS3_FT_ON()) { const uint64_t t = ps3_ft_now();
                       ps3_ft_span(PS3_FT_FLIP_REQ, 0, bufferId, (uint32_t)in_fifo, t, t); }

    /* Invoke via OPD resolution, not a raw call into guest code.
     *
     * On hardware this callback fires when the flip COMPLETES, at vsync --
     * which is what cellGcmTickFlip does. Calling it again here, at request
     * time, is a compatibility wart: some titles need the early edge, but a
     * title that issues its first flip while still initialising then runs its
     * handler against half-built state. Tokyo Jungle faults that way, in
     * func_001F2D3C, dereferencing a singleton it has not constructed yet.
     * GCM_FLIPCB_ONTICK=1 leaves the callback to the tick alone. */
    if (s_flip_handler_opd && g_ps3_guest_caller && !gcm_flipcb_on_tick_only())
        g_ps3_guest_caller(s_flip_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);  /* head 0 = primary display */

    return CELL_OK;
}

/* NID: 0x5C770579 */
s32 cellGcmSetFlipCommand(u32 bufferId) { return gcm_flip_request(bufferId, 0); }

/* ---- C2: the libgcm command-context primitives the library emitters share ------------------
 *
 * The firmware writes its commands through `CellGcmContextData` {begin, end, current, callback}
 * (guest memory, big-endian). Every group is preceded by `current + 4*words > end`; when that
 * holds it calls `callback(ctx, words)` and carries on only if the callback returns 0.
 * ps3_invoke_guest cannot return the guest's r3, so success is judged the way the firmware's
 * next write would be: re-read the context and require the room. */

int cellGcm_ctx_reserve(u32 ctx, u32 words)
{
    u32 cur = vm_read32(ctx + 8), end = vm_read32(ctx + 4);
    if (cur + words * 4u <= end)
        return 1;
    const u32 opd = vm_read32(ctx + 0xC);
    /* The OPD is a guest pointer the firmware loads two words from (code, TOC). */
    if (opd && !gcm_span_ok(opd, 8))
        ps3_stop_guest_range("cellGcm_ctx_reserve", "callback", opd, 8, gcm_vm_limit());
    if (opd && vm_read32(opd) == GCM_FIFO_CALLBACK_SENTINEL_EA)
        cellGcm_fifo_recycle(ctx);                       /* the runtime's own ring */
    else if (opd)
        ps3_invoke_guest(opd, ctx, words, 0, 0, 0, 0, 0, 0);
    cur = vm_read32(ctx + 8);
    end = vm_read32(ctx + 4);
    if (cur + words * 4u <= end)
        return 1;
    char why[96];
    snprintf(why, sizeof why, "callback 0x%08X left no room: current 0x%08X end 0x%08X need %u words",
             opd, cur, end, words);
    ps3_stop_gcm_unmodelled("cellGcm_ctx_reserve", "buffer-full-callback", why);
}

/* One command group: reserve `n` words, write them at `current`, advance `current`. */
void cellGcm_ctx_emit(u32 ctx, const u32* w, u32 n)
{
    cellGcm_ctx_reserve(ctx, n);
    const u32 cur = vm_read32(ctx + 8);
    for (u32 i = 0; i < n; i++)
        vm_write32(cur + i * 4u, w[i]);
    vm_write32(ctx + 8, cur + n * 4u);
}

/* `_cellGcmSetFlipCommand` as libgcm_sys writes it (body 0x02008948, entry 0x02008AE0), behind
 * libresc's helper 0x02008780: a buffer id above 7 is CELL_GCM_ERROR_FAILURE (0x802100FF),
 * else 19 words are reserved and written:
 *   GCM_DRIVER_QUEUE 0xE944 = bufferId                     (subchannel 7)
 *   NV406E 0x0060 = 0x56616661 (semaphore DMA), 0x0064 = 0x30, 0x006C = 0 (release label 0x30),
 *   0x0064 = 0x30, 0x0068 = 1 (acquire label 0x30 == 1: the flip-ready handshake)
 *   CALL 0                                                  (driver default-state subroutine)
 *   0x0064 = 0x10, 0x006C = 0xFFFFFFFF                      (release label 0x10)
 *   GCM_FLIP_HEAD 0xE924 = 0x8000010F                       (subchannel 7)
 * The guest-visible half of the flip request (status, flip handler) runs now, as it does for the
 * 0xFEAD marker path; the walker presents when it reaches 0xE924. */
s32 cellGcm_emit_flip_command(u32 ctx, u32 bufferId)
{
    if (bufferId > 7)
        return CELL_GCM_ERROR_FAILURE;
    const u32 w[19] = {
        0x0004E944u, bufferId,
        0x00040060u, 0x56616661u,
        0x00040064u, 0x30u,
        0x0004006Cu, 0u,
        0x00040064u, 0x30u,
        0x00040068u, 1u,
        0x00000002u,
        0x00040064u, 0x10u,
        0x0004006Cu, 0xFFFFFFFFu,
        0x0004E924u, 0x8000010Fu,
    };
    cellGcm_ctx_emit(ctx, w, 19);
    if (s_display_buffer_set[bufferId]) {
        s_fifo_flips_queued++;
        gcm_flip_request(bufferId, 1);
    }
    return CELL_OK;
}

/* Host-side cellGcmAddressToOffset: the result in `*offset_out` (host memory). The guest API
 * writes through a guest pointer, so the offset goes through a scratch word in the HLE page. */
s32 cellGcm_address_to_offset(u32 address, u32* offset_out)
{
    const u32 scratch = VM_HLE_INJECT_BASE + 0x2F40u;
    const s32 rc = cellGcmAddressToOffset(address, (u32*)(uintptr_t)scratch);
    *offset_out = vm_read32(scratch);
    return rc;
}

/* The real _cellGcmSetFlipCommand appends the flip to the command buffer, so
 * the RSX flips once it has executed everything queued before it. Firing it
 * at call time instead presented whatever the drain had reached by then --
 * and the title has not even flushed its frame's tail yet: The Simpsons Arcade
 * Game calls it with ctx->current ~2.4 KB past put, so frames showed without
 * their last draws (a dialog, a dimming overlay) for one flip. Write a marker
 * at ctx->current instead; the drain stops at it and fires the flip there,
 * exactly as it does for the 0xFEAD words a statically-linked libgcm emits.
 * Returns 0 when there is no room or ctx is not a mapped command buffer. */
static int gcm_flip_into_fifo(u32 ctx, u32 bufferId)
{
    if (!ctx || bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM ||
        !s_display_buffer_set[bufferId]) return 0;
    const u32 end = vm_read32(ctx + 4), cur = vm_read32(ctx + 8);
    /* Never near the end: those bytes are the title's reserve before its
     * ring wraps, and the recycle above keys on them. */
    if (cur + 8 + GCM_RECYCLE_SLACK > end || gcm_ea2io(cur) == 0xFFFFFFFFu) return 0;
    vm_write32(cur, GCM_FLIP_MARKER | bufferId);
    s_fifo_flips_queued++;
    /* Wrap here, on the guest thread, once the ring is nearly used -- what the
     * SDK's buffer-full callback does. Otherwise the drain thread recycles it
     * (see "recycled the title's own ring"), rewriting ctx->current and put
     * while the guest may be between advancing current and flushing: put
     * then lands on begin, the guest's closing fence is never executed, and
     * it spins on `ref` for good. Safe when the walker has left the head of
     * the ring well behind, which is the region the guest writes next.
     *
     * Only for a ring the runtime owns, i.e. one whose buffer-full callback is
     * still the runtime's (cellGcmSetupContext installs it). A title with its
     * own callback manages its own buffers, and its context need not be a ring
     * at all: Killzone 3 flips on a 0x30000-byte draw context whose `end`
     * moves down as it allocates from the top, so it always looks nearly full.
     * Rewinding it to `begin` in mid-frame cut the frame short, dropping the
     * fence release in its tail, and the title then waited for it forever. */
    const u32 begin = vm_read32(ctx), io_begin = gcm_ea2io(begin);
    const u32 cb_opd = vm_read32(ctx + 0xC);
    const int runtime_ring = cb_opd && vm_read32(cb_opd) == GCM_FIFO_CALLBACK_SENTINEL_EA &&
                             !(s_ib.valid && ctx == s_ib.ctx_ea);   /* segmented: its callback switches */
    if (runtime_ring && end - (cur + 4) < GCM_FLIP_WRAP_BYTES && io_begin != 0xFFFFFFFFu &&
        s_fifo_getoff >= io_begin + GCM_RECYCLE_MARGIN) {
        vm_write32(cur + 4, 0x20000000u | io_begin);   /* JUMP -> begin */
        vm_write32(ctx + 8, begin);
        return 1;
    }
    vm_write32(ctx + 8, cur + 4);
    return 1;
}

/* cellGcmSetFlip(context, buffer_id) — immediate flip request. PSL1GHT's
 * gcmSetFlip import (NID 0xDC09357E) lands here; without it vkcube's flip was
 * a no-op, GetFlipStatus never cleared, and init timed out into exit(-1). */
s32 cellGcmSetFlip(void* context, u32 bufferId)
{
    (void)context;   /* command context — flip is immediate in HLE */
    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0x2B827690 */
s32 cellGcmSetFlipCommandWithWaitLabel(u32 bufferId, u32 labelIndex, u32 labelValue)
{
    if (labelIndex >= CELL_GCM_MAX_LABEL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* Wait until the label reaches the expected value (instant in HLE). Labels
     * live in guest memory (see cellGcmGetLabelAddress) so the game can poll. */
    {   /* A guest thread's store into the label window, made by the HLE: ordered like the lifted ones. */
        const u32 label_ea = GCM_LABEL_GUEST_BASE + labelIndex * GCM_LABEL_STRIDE;
        gcm_guest_label_store(label_ea, labelValue);
        vm_write32(label_ea, labelValue);
    }

    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0x0B4B62D5 */
s32 cellGcmSetPrepareFlip(void* ctx, u32 bufferId)
{
    (void)ctx;  /* command buffer context -- not used in HLE */

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (!s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetPrepareFlip(bufferId=%u)\n", bufferId);

    gcm_set_current_display_buffer(bufferId);
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_flip_request_count++;
    s_last_flip_time = get_timestamp_ns();

    /* Invoke the guest flip handler via OPD resolution -- s_flip_handler holds
     * the raw guest OPD (e.g. 0x530D70); calling it as a host function pointer
     * jumps into guest code and crashes. See cellGcmSetFlipCommand for why
     * GCM_FLIPCB_ONTICK exists. */
    if (s_flip_handler_opd && g_ps3_guest_caller && !gcm_flipcb_on_tick_only())
        g_ps3_guest_caller(s_flip_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);

    return CELL_OK;
}

/* NID: 0x371674CF */
CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index)
{
    u32 id = index % CELL_GCM_MAX_DISPLAY_BUFFER_NUM;
    return &s_display_buffers[id];
}

/* NID: 0x93806525 */
u32 cellGcmGetCurrentDisplayBufferId(void)
{
    return s_current_display_buffer_id;
}

/* NID: 0xA41EF7E8 */
void cellGcmSetFlipHandler(CellGcmFlipHandler handler)
{
    /* `handler` is a guest OPD address — store as such and remember
     * the legacy host-typed value too for any callers still using
     * the old API style. Real dispatch goes through
     * cellGcmDispatchVBlank()/DispatchFlip() via g_ps3_guest_caller. */
    printf("[cellGcmSys] SetFlipHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_flip_handler_opd = (u32)(size_t)handler;
    s_flip_handler = handler;
    { u32 c = s_flip_handler_opd ? vm_read32(s_flip_handler_opd) : 0; if (c) s_flip_handler_code = c; }
    gcm_handler_bit(GCM_HBIT_FLIP, s_flip_handler_opd != 0);
}

/* NID: 0xA91B0402 */
void cellGcmSetVBlankHandler(CellGcmVBlankHandler handler)
{
    printf("[cellGcmSys] SetVBlankHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_vblank_handler_opd = (u32)(size_t)handler;
    s_vblank_handler = handler;
    { u32 c = s_vblank_handler_opd ? vm_read32(s_vblank_handler_opd) : 0; if (c) s_vblank_handler_code = c; }
    gcm_handler_bit(GCM_HBIT_VBLANK, s_vblank_handler_opd != 0);
}

/* NID: 0xDC494430 */
void cellGcmSetSecondVHandler(CellGcmSecondVHandler handler)
{
    printf("[cellGcmSys] SetSecondVHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_second_v_handler_opd = (u32)(size_t)handler;
    s_second_v_handler = handler;
    gcm_handler_bit(GCM_HBIT_SECOND_V, s_second_v_handler_opd != 0);
}

/* NID: 0x06EDEA9E */
void cellGcmSetUserHandler(CellGcmUserHandler handler)
{
    printf("[cellGcmSys] SetUserHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_user_handler_opd = (u32)(size_t)handler;
    s_user_handler = handler;
    gcm_handler_bit(GCM_HBIT_USER, s_user_handler_opd != 0);
}

/* NID: 0x63387071 */
u64 cellGcmGetLastFlipTime(void)
{
    return s_last_flip_time;
}

/* ---------------------------------------------------------------------------
 * Address translation / IO mapping
 * -----------------------------------------------------------------------*/

/* Mirror the host tables into the guest window the title will index. */
static void gcm_publish_offset_tables(void)
{
    for (u32 i = 0; i < GCM_OFFSET_TABLE_PAGES; i++) {
        vm_write16(GCM_OFFSET_TABLE_IO_ADDR + i * 2, s_io_address_table[i]);
        vm_write16(GCM_OFFSET_TABLE_EA_ADDR + i * 2, s_ea_address_table[i]);
    }
}

/* NID: 0x2922AED0 */
s32 cellGcmGetOffsetTable(CellGcmOffsetTable* table)
{
    u32 ea = (u32)(uintptr_t)table;
    if (!ea)
        return CELL_GCM_ERROR_INVALID_VALUE;

    gcm_publish_offset_tables();

    /* The guest struct is two 4-byte EAs, not two host pointers. */
    vm_write32(ea + 0, GCM_OFFSET_TABLE_IO_ADDR);
    vm_write32(ea + 4, GCM_OFFSET_TABLE_EA_ADDR);
    return CELL_OK;
}

static u32 gcm_io_alloc(u32 size);   /* defined below */

/* NID: 0x21AC3697 */
s32 cellGcmAddressToOffset(u32 address, u32* offset)
{
    /* `offset` is a GUEST address; write big-endian via vm_write32. */
    uint32_t off_ea = (uint32_t)(uintptr_t)offset;
    u32 out;   /* the offset to hand back: every success path sets it and ends at `store` */
    /* A2O_DBG=<N>: the EA->offset conversions a title asks for. A texture bound
     * at an offset nothing ever writes usually means the offset came from an EA
     * in a space we classified wrongly, and that is only visible here. */
    { static int cap = -1, k = 0;
      if (cap < 0) { const char* e = getenv("A2O_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && k < cap) { k++;
        u32 nz = 0;
        for (u32 i = 0; i < 0x400u; i += 13) if (vm_base && vm_base[address + i]) nz++;
        printf("[A2O] ea=0x%08X  (first 1KB nonzero=%u/79)\n", address, nz); } }

    /* Local memory: offset = address - localBase. Record the offset page as
     * LOCAL-derived so cellGcmResolveOffset can disambiguate it from an IO
     * (main-memory) offset with the same page number -- the two offset spaces
     * overlap, and a title with a large IO region (gcm/cube: 16MB) otherwise
     * gets its VRAM vertex/texture/FP reads resolved into the empty command
     * buffer. */
    if (address >= s_config.localAddress &&
        address < s_config.localAddress + s_config.localSize) {
        u32 loff = address - s_config.localAddress;
        if ((loff >> 20) < sizeof(s_local_offset_page)) s_local_offset_page[loff >> 20] = 1;
        out = loff;
        goto store;
    }

    /* IO-mapped main memory: consult offset table */
    u32 page = address >> 20;
    if (page < 65536 && s_io_address_table[page] != 0xFFFF) {
        u32 io_page = s_io_address_table[page];
        out = (io_page << 20) | (address & 0xFFFFF);
        goto store;
    }

    /* Legacy fallback for initial IO region */
    if (s_config.ioAddress != 0 && address >= s_config.ioAddress &&
        address < s_config.ioAddress + s_config.ioSize) {
        out = address - s_config.ioAddress;
        goto store;
    }

    /* Auto-map unmapped main memory. On real hardware PSL1GHT pre-maps its
     * whole RSX heap in gcmInitBody, so its libraries never call MapMainMemory
     * before handing an EA to the RSX (Tiny3D's command ring lives in an
     * sys_mmapper region). Mirror that by mapping the 1MB page on first use.
     *
     * The bound used to be a flat `< 0x40000000`, which is not a description of
     * anything -- RSX local memory is already handled at the top of this
     * function against its real base, and the raw-SPU window at 0x30000000 sits
     * *below* the old bound, so it was not keeping that out either. What it did
     * do was exclude titles whose RSX-visible heap lives high: Virtua Fighter 5
     * maps 0x4A900000..0x4B400000 and then hands us 0x4B400040, which fell past
     * 0x40000000, skipped the auto-map, and failed -- 976 times a boot, leaving
     * the title with no offsets to draw with (20 draws in five minutes).
     *
     * So the test is now what it always meant: anything below RSX local memory
     * is main memory. Local is checked first and returns above, so reaching
     * here with an address under localAddress means an unmapped main-memory
     * page, which is exactly the case this fallback exists for.
     *
     * The firmware has no auto-map: libgcm_sys 0x02003F90 (cellGcmAddressToOffset, NID 0x21AC3697,
     * opd 0x0200BC64) looks the page up in the EA->IO table (0x02003FC4) and answers an unmapped one with
     * 0x802100FF, CELL_GCM_ERROR_FAILURE (0x02003FDC -> 0x02003FBC; the code is preloaded in r8 at
     * 0x02003F94/0x02003F9C). The auto-map is this runtime's extension for the homebrew SDKs above. */
    /* NULL is not a buffer. Auto-mapping it costs an IO page and, worse, hands
     * the title a real-looking offset for guest address 0 -- so an EA the title
     * computed as NULL (a failed allocation, an out-param it never filled)
     * silently becomes a valid RSX offset instead of an error it could notice.
     * The Orange Box's chooser asks for exactly this, and the page it burned was
     * the one the following cellGcmMapMainMemory would otherwise have taken.
     *
     * The firmware's answer for page 0 is its answer for any unmapped page, FAILURE with *offset left
     * alone (libgcm_sys 0x02003F90, 0x02003FDC -> 0x02003FBC; it has no address test of its own; a mapped page 0
     * was handled above). The code here was INVALID_VALUE and the out-param was cleared; neither is the firmware's. */
    if (address < 0x100000u) {
        printf("[cellGcmSys] AddressToOffset REFUSED: ea 0x%08X is not a mappable "
               "address (page 0)\n", address);
        return CELL_GCM_ERROR_FAILURE;
    }

    u32 auto_map_limit = s_config.localAddress ? s_config.localAddress : 0x40000000u;
    if (address < auto_map_limit && s_gcm_initialized) {
        u32 ea_page = address & ~0xFFFFFu;
        u32 io      = gcm_io_alloc(0x100000u);
        /* No IO page left for the auto-map. The firmware has no auto-map to run out of; its answer for an EA
         * it has no IO page for is FAILURE (libgcm_sys 0x02003FDC -> 0x02003FBC, r8 = 0x802100FF from
         * 0x02003F94), and this is that answer, with *offset untouched as there. */
        if (io == 0xFFFFFFFFu) {
            printf("[cellGcmSys] AddressToOffset: no free IO page to auto-map ea 0x%08X\n", address);
            return CELL_GCM_ERROR_FAILURE;
        }
        populate_offset_table(ea_page, io, 0x100000u);
        /* Record it in the mapping list too, not just the offset table.
         *
         * These are two different structures answering two different questions,
         * and only the offset table was being filled in. The FIFO walker
         * resolves io -> ea through the MAPPING LIST (find_io_mapping_by_io),
         * so an auto-mapped range was invisible to it: a JUMP into one reported
         * "no IO mapping", the walker resynced straight to put, and every fence
         * in the skipped range was lost -- which strands a title spinning on
         * ctrl->ref with no way to see why. Both paths have to agree about what
         * is mapped. */
        IoMapping* m = find_free_mapping();
        if (m) {
            m->ea = ea_page; m->io = io; m->size = 0x100000u; m->active = 1;
            s_io_mapping_count++;
        } else {
            printf("[cellGcmSys] WARNING: auto-map has no free IO mapping slot; "
                   "the FIFO will not be able to walk io 0x%08X\n", io);
        }
        printf("[cellGcmSys] AddressToOffset: auto-mapped ea 0x%08X -> io 0x%08X\n",
               ea_page, io);
        out = io | (address & 0xFFFFFu);
        goto store;
    }

    printf("[cellGcmSys] WARNING: AddressToOffset failed for 0x%08X\n", address);
    { static int _d = -1; if (_d < 0) _d = getenv("A2O_FAILDBG") ? 1 : 0;
      if (_d) { static int _n = 0; if (PS3_LOG_FIRST(_n, 12))
          fprintf(stderr, "[A2O] FAILED ea=0x%08X (not in local or IO space)%c",
                  address, 10); } }
    return CELL_GCM_ERROR_FAILURE;

store:
    /* libgcm_sys 0x02003F90 stores the offset (0x02003FB8, 0x02003FE4) only on success and never tests the
     * pointer: a NULL `offset` is a store into the null page (a fault), not CELL_GCM_ERROR_INVALID_VALUE, and
     * an EA it cannot convert returns FAILURE before any store (so NULL with such an EA is FAILURE, not a fault). */
    if (!off_ea)
        ps3_stop(PS3_STOP_GUEST_NULL_ACCESS, GCM_NULL_STORE_WHAT, GCM_NULL_STORE_REMEDY,
                 GCM_NULL_STORE_FIELDS(__func__), 4, NULL);
    vm_write32(off_ea, out);
    return CELL_OK;
}

/* IO-offset space allocator for main-memory mappings. It never hands out a
 * page that is mapped, the Init command-buffer region included: handing out io
 * 0 (the old s_local_mem_allocated bump, which begins at 0) remapped the FIFO's
 * own io page -- demosaic's cellGcmMapMainMemory(sys heap) got offset 0,
 * io2ea(0) then resolved into its heap (all zeros) and the RSX walker consumed
 * every frame's commands as no-ops (no draws, label fence never written, guest
 * waited forever). */
/* First fit over the IO -> EA table: the lowest run of 1MB IO pages that no
 * mapping holds. A bump pointer kept across cellGcmInit (Init wipes the tables
 * and claims io 0..ioSize itself) handed a later map IO pages inside the Init
 * window: a title that maps 2MB before Init left the pointer at 0x300000, and
 * its next map took io 0x300000 onwards, on top of the window. Command buffers
 * in that part of its IO heap then resolved into the other mapping, the FIFO
 * walker read zeros and resynced, and a frame's commands were lost. Searching
 * the table cannot hand out a page that is in use.
 * Returns 0xFFFFFFFF when no run is free. */
#define GCM_IO_PAGES 256u   /* the RSX's 256MB IO window (cellGcmGetMaxIoMapSize) */
static u32 gcm_io_alloc(u32 size)
{
    const u32 need = (size + 0xFFFFFu) >> 20;
    u32 run = 0;
    for (u32 p = 1; p < GCM_IO_PAGES && need; p++) {   /* io 0 is the FIFO's own page */
        run = (s_ea_address_table[p] == 0xFFFF) ? run + 1 : 0;
        if (run == need) return (p + 1 - need) << 20;
    }
    return 0xFFFFFFFFu;
}

/* NID: 0xA114EC67 */
s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset)
{
    /* Log the REQUEST before validating it. Every rejection below used to
     * happen before the only printf in this function, so a title whose
     * mapping we refused looked -- in the log -- like a title that never asked.
     * Virtua Fighter 5 then fails cellGcmAddressToOffset ~976 times a boot for
     * addresses immediately past the region it did get, draws 20 times instead
     * of thousands, and never flips; the refusal that would explain it was
     * invisible. Same lesson as the savedata early returns. */
    printf("[cellGcmSys] MapMainMemory(ea=0x%08X, size=0x%X)\n", ea, size);

    /* libgcm_sys 0x0200488C (cellGcmMapMainMemory, NID 0xA114EC67, opd 0x0200BCCC) tests the EA first: its low
     * 20 bits (0x0200489C, 0x020048A4, 0x020048D4) and a misaligned one returns 0x802100FF =
     * CELL_GCM_ERROR_FAILURE (0x020048D8..0x020048DC). RPCS3 cellGcmMapMainMemory() (cellGcmSys.cpp) answers
     * FAILURE too. This was INVALID_ALIGNMENT, a code the firmware never returns here. */
    if ((ea & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapMainMemory REFUSED: ea 0x%08X is not 1MB aligned\n", ea);
        return CELL_GCM_ERROR_FAILURE;
    }
    /* The size's low 20 bits are the second test (libgcm_sys 0x02004918, 0x0200491C, 0x02004920), the same
     * FAILURE 0x802100FF (0x020048D8). A size that is a multiple of 1MB goes on, 0 included. */
    if ((size & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapMainMemory REFUSED: size 0x%X is not a multiple of 1MB\n", size);
        return CELL_GCM_ERROR_FAILURE;
    }
    /* The firmware does not test for size 0 (libgcm_sys 0x0200488C: nothing between 0x02004920 and the search at
     * 0x02004954): an empty window is "found" at IO page 0 and the call is handed to lv2 with length 0
     * (sc 672 at 0x020049F8, r6 = size), whose answer the firmware returns unchanged (0x02004AA8..0x02004AC4).
     * That syscall is not in the firmware set, so the result is unknown; RPCS3 answers FAILURE for it, which
     * the firmware would only do for an argument it tested itself. Named stop, not a guess. */
    if (size == 0)
        ps3_stop_gcm_unmodelled(__func__, "zero-size",
                                "libgcm_sys passes a size of 0 to lv2 (sc 672); the kernel's answer is not known");

    /* The firmware has no "initialised" test either: libgcm_sys 0x0200488C searches the IO->EA table at
     * TOC(-0x7FC8)+0x1800 against the IO window size at +0x1C08 (0x0200494C), and both are 0 until cellGcmInit
     * fills them (zero in the lifted image). Before Init the bound ioPages - run (0x02004950) underflows, the scan
     * at 0x02004954 walks past the table, and the lv2 context id 0x02000204 returns (0) is not a context. That is
     * undefined behaviour, not an error code; NO_IO_PAGE_TABLE here was never the firmware's answer (RPCS3's
     * cellGcmMapMainMemory() has no such test either). Named stop. */
    if (!s_gcm_initialized)
        ps3_stop_hle_uninitialized(__func__, "cellGcmSys",
                                   "libgcm_sys searches IO page tables that cellGcmInit builds; before it they are zero");

    u32 io_offset = gcm_io_alloc(size);
    /* libgcm_sys 0x0200488C searches the IO->EA table first-fit for a run of size>>20 free pages
     * (0x02004954..0x02004988) and, when none is left, unlocks (0x020087C8) and returns 0x80210001 =
     * CELL_GCM_ERROR_NO_IO_PAGE_TABLE (0x0200499C..0x020049A0). The same code, for the same condition. */
    if (io_offset == 0xFFFFFFFFu) {
        printf("[cellGcmSys] MapMainMemory REFUSED: no free run of 0x%X bytes of IO space\n", size);
        return CELL_GCM_ERROR_NO_IO_PAGE_TABLE;
    }

    IoMapping* mapping = find_free_mapping();
    /* The firmware keeps a map only in the two u16 page tables (libgcm_sys 0x02004A14..0x02004A74) and has no
     * list to run out of: once a run is free the map succeeds. The slot is this runtime's bookkeeping for the
     * FIFO walker (find_io_mapping_by_io), so a full list is a limit of ours; the FAILURE it returned was not the
     * firmware's (its only FAILURE here is a misaligned argument, 0x020048D8). A code would be a guess. Named stop. */
    if (!mapping)
        ps3_stop_gcm_unmodelled(__func__, "io-mapping-slots",
                                "the runtime's IO mapping list is full where the firmware's page tables are not");

    /* libgcm_sys stores the new offset only at the end (stw r23, 0(r26) at 0x02004A7C, r26 = offset) with no test
     * of the pointer, so a NULL `offset` faults there, after the tables are updated; it is not
     * CELL_GCM_ERROR_INVALID_VALUE. */
    if (!offset)
        ps3_stop(PS3_STOP_GUEST_NULL_ACCESS, GCM_NULL_STORE_WHAT, GCM_NULL_STORE_REMEDY,
                 GCM_NULL_STORE_FIELDS(__func__), 4, NULL);

    printf("[cellGcmSys] MapMainMemory -> io 0x%08X\n", io_offset);

    mapping->ea     = ea;
    mapping->io     = io_offset;
    mapping->size   = size;
    mapping->active = 1;
    s_io_mapping_count++;

    populate_offset_table(ea, io_offset, size);

    vm_write32((uint32_t)(uintptr_t)offset, io_offset);   /* guest out-param */
    return CELL_OK;
}

/* NID: 0x63441CB4 */
s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size)
{
    /* Logged before validation, and every failure path named -- same reason as
     * cellGcmMapMainMemory above. Two of these returned silently AFTER the log
     * line as well, so even a call that got that far could vanish. */
    printf("[cellGcmSys] MapEaIoAddress(ea=0x%08X, io=0x%08X, size=0x%X)\n", ea, io, size);

    if ((ea & 0xFFFFF) != 0 || (io & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: ea 0x%08X / io 0x%08X not 1MB aligned\n", ea, io);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if (size == 0 || (size & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: size 0x%X is not a non-zero multiple of 1MB\n", size);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if (find_mapping_by_ea(ea) != NULL) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: ea 0x%08X is already mapped\n", ea);
        return CELL_GCM_ERROR_ADDRESS_OVERWRAP;
    }

    IoMapping* mapping = find_free_mapping();
    if (!mapping) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: no free IO mapping slots\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    mapping->ea     = ea;
    mapping->io     = io;
    mapping->size   = size;
    mapping->active = 1;
    s_io_mapping_count++;

    populate_offset_table(ea, io, size);

    return CELL_OK;
}

/* NID: 0x626E8518 */
s32 cellGcmMapEaIoAddressWithFlags(u32 ea, u32 io, u32 size, u32 flags)
{
    /* RPCS3 cellGcmSys.cpp accepts exactly STRICT_ORDERING (not an arbitrary
     * bit mask). The software FIFO already reads shared guest memory in FIFO
     * order after its acquire fence; there is no relaxed DMA mapping path to
     * select here. Preserve that path rather than pretending to enable a new
     * ordering mode by storing an unused flag. */
    if (flags != CELL_GCM_IOMAP_FLAG_STRICT_ORDERING)
        return CELL_GCM_ERROR_INVALID_VALUE;
    if ((ea | io | size) & 0xFFFFFu || !size)
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    const u64 ea_end = (u64)ea + size;
    const u64 io_end = (u64)io + size;
    /* The HLE models the standard 256 MiB RSX IO aperture. Widen sums before
     * checking bounds so wrapping inputs cannot publish aliases at page 0. */
    if (ea_end > 0x100000000ull || io_end > 0x10000000ull)
        return CELL_GCM_ERROR_INVALID_VALUE;
    /* The legacy mapper checks only an identical EA base. Preflight complete
     * intervals on both sides so a rejected flagged map changes neither the
     * mapping list nor either direction of the translation table. */
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        const IoMapping* m = &s_io_mappings[i];
        if (m->active &&
            (((u64)ea < (u64)m->ea + m->size && ea_end > m->ea) ||
             ((u64)io < (u64)m->io + m->size && io_end > m->io)))
            return CELL_GCM_ERROR_ADDRESS_OVERWRAP;
    }
    return cellGcmMapEaIoAddress(ea, io, size);
}

/* NID: 0xEFD00F54 (disambiguation by context) */
s32 cellGcmUnmapEaIoAddress(u32 ea)
{
    IoMapping* mapping = find_mapping_by_ea(ea);
    if (!mapping)
        return CELL_GCM_ERROR_FAILURE;

    printf("[cellGcmSys] UnmapEaIoAddress(ea=0x%08X)\n", ea);

    clear_offset_table(mapping->ea, mapping->io, mapping->size);
    mapping->active = 0;
    s_io_mapping_count--;

    return CELL_OK;
}

/* NID: 0xDB23E867 */
s32 cellGcmUnmapIoAddress(u32 io)
{
    IoMapping* mapping = find_mapping_by_io(io);
    if (!mapping)
        return CELL_GCM_ERROR_FAILURE;

    printf("[cellGcmSys] UnmapIoAddress(io=0x%08X)\n", io);

    clear_offset_table(mapping->ea, mapping->io, mapping->size);
    mapping->active = 0;
    s_io_mapping_count--;

    return CELL_OK;
}

/* NID: 0x2A6FBA9C */
s32 cellGcmIoOffsetToAddress(u32 ioOffset, u32* ea)
{
    uint32_t ea_ea = (uint32_t)(uintptr_t)ea;   /* guest out-param */
    if (!ea_ea)
        return CELL_GCM_ERROR_INVALID_VALUE;

    u32 page = ioOffset >> 20;
    if (page < 65536 && s_ea_address_table[page] != 0xFFFF) {
        u32 ea_page = s_ea_address_table[page];
        vm_write32(ea_ea, (ea_page << 20) | (ioOffset & 0xFFFFF));
        return CELL_OK;
    }

    printf("[cellGcmSys] WARNING: IoOffsetToAddress failed for 0x%08X\n", ioOffset);
    vm_write32(ea_ea, 0);
    return CELL_GCM_ERROR_FAILURE;
}

/* Resolve an RSX FIFO offset (from a vertex-array / texture / surface method)
 * to a guest effective address the backend can read via vm_base + addr.
 *
 * An RSX offset refers to either IO-mapped *main* memory or *local* video
 * memory, distinguished on hardware by a per-object location bit that the
 * command stream no longer carries by the time the backend sees it. Resolve by
 * table: if the offset's 1MB page is IO-mapped, it's main memory; otherwise it
 * is local video memory (localAddress + offset). IO mappings are sparse (the
 * command/IO window is a few MB) while local objects sit high in the local
 * heap, so this disambiguates every real case — only offsets inside the small
 * IO window are treated as main. */
u32 cellGcmResolveOffset(u32 offset)
{
    u32 page = offset >> 20;
    /* A page the guest derived from a LOCAL EA resolves to VRAM even when the
     * IO table also covers that page number (overlapping offset spaces). */
    if (page < sizeof(s_local_offset_page) && s_local_offset_page[page])
        return s_config.localAddress + offset;
    if (page < 65536) {
        u16 ea_page = s_ea_address_table[page];
        if (ea_page != 0 && ea_page != 0xFFFF)
            return ((u32)ea_page << 20) | (offset & 0xFFFFF);
    }
    return s_config.localAddress + offset;
}

/* IO-table-first resolve: skip the local-page shortcut and ask the IO offset
 * table, falling back to VRAM only if the page is unmapped. cellGcmResolveOffset
 * prefers local for any page the guest ever derived from a local EA, which is
 * right for a title that really does keep the data in VRAM but wrong for one
 * that builds its textures in main memory on pages whose numbers happen to
 * collide. Returns 0 when the IO table has no mapping, so a caller can tell
 * "not IO-mapped" from "IO-mapped at 0". */
u32 cellGcmResolveIO(u32 offset)
{
    u32 page = offset >> 20;
    if (page < 65536) {
        u16 ea_page = s_ea_address_table[page];
        if (ea_page != 0 && ea_page != 0xFFFF)
            return ((u32)ea_page << 20) | (offset & 0xFFFFF);
    }
    return 0;
}

/* Location-aware resolve. RSX offsets are TWO overlapping number spaces --
 * LOCAL (VRAM, ea = localAddress + offset) and MAIN (IO-mapped, via the
 * offset table). cellGcmResolveOffset guesses table-first, which breaks when
 * a title's IO region is large enough to cover the same page numbers as its
 * local allocations (gcm/cube: ioSize 16MB, FP ucode at local 0xB90000 --
 * table hit returned the empty command-buffer EA instead of VRAM). Callers
 * that KNOW the location (texture format bits, SET_SHADER_PROGRAM bits) must
 * use this. `local` != 0 -> local memory.
 *
 * A MAIN offset asks the IO table first. cellGcmResolveOffset would send it
 * to VRAM whenever the guest once derived a local offset in the same 1 MB
 * page number, and a title with both a large IO map and a full VRAM has
 * many such pages: Twisted Metal's menu fragment programs at IO 0x08B96040
 * and 0x0F75E800 and a G-buffer texture at IO 0x02A1F380 were read out of
 * VRAM -- texture bytes that decode as a NOP program, so the menu's UI drew
 * transparent black. An offset the IO table does not map keeps the old
 * guess. */
u32 cellGcmResolveLocated(int local, u32 offset)
{
    if (local)
        return s_config.localAddress + offset;
    const u32 ea = cellGcmResolveIO(offset);
    return ea ? ea : cellGcmResolveOffset(offset);
}

/* ---------------------------------------------------------------------------
 * Label / report / timestamp
 * -----------------------------------------------------------------------*/

/* NID: 0xF80196C1 */
u32* cellGcmGetLabelAddress(u8 index)
{
    /* Every u8 index names a label: the RSX label area is 256 entries
     * (CELL_GCM_MAX_LABEL_COUNT) and the firmware's parameter is u8, so the
     * type already bounds the index. The old `index >= 256` test could never
     * be true. */
    { static int n = 0; if (n++ < 32)   /* TEMP */
        fprintf(stderr, "[cellGcmSys] GetLabelAddress(0x%X) -> 0x%08X\n", index,
                GCM_LABEL_GUEST_BASE + (u32)index * GCM_LABEL_STRIDE); }
    return (u32*)(uintptr_t)(GCM_LABEL_GUEST_BASE + (u32)index * GCM_LABEL_STRIDE);
}

/* NID: 0x9A0159AF */
CellGcmReportData* cellGcmGetReportDataAddress(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT) {
        printf("[cellGcmSys] WARNING: GetReportDataAddress index %u out of range\n", index);
        return NULL;
    }
    return &s_report_data[index];
}

/* NID: 0x99D397AC. The value NV4097_GET_REPORT stored at this index (the
 * walker fills it). Unregistered, every call returned a faked 0. */
u32 cellGcmGetReport(u32 type, u32 index)
{
    { static unsigned n = 0;   /* unsigned: the periodic line keeps counting, and wrapping is defined */
      if (n++ < 8 || (n % 2000) == 0) printf("[cellGcmSys] GetReport(type=%u, index=%u) -> %u%c", type, index,
                          index < CELL_GCM_MAX_REPORT_COUNT ? s_report_data[index].value : 0u, 10); }
    if (index >= CELL_GCM_MAX_REPORT_COUNT) return 0;
    return s_report_data[index].value;
}

/* NID: 0x5A41C10F */
u64 cellGcmGetTimeStamp(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    /*
     * On real hardware this reads the RSX timestamp for a given report index.
     * We return the host timestamp in nanoseconds as a reasonable approximation.
     */
    s_report_data[index].timestamp = get_timestamp_ns();
    return s_report_data[index].timestamp;
}

/* ---------------------------------------------------------------------------
 * Tile / Zcull configuration
 * -----------------------------------------------------------------------*/

/* NID: 0xD0B1D189 */
s32 cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size,
                   u32 pitch, u8 comp, u16 base, u8 bank)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetTile(index=%u, loc=%u, offset=0x%X, size=0x%X, pitch=%u)\n",
           index, location, offset, size, pitch);

    s_tiles[index].offset = offset;
    s_tiles[index].size   = size;
    s_tiles[index].pitch  = pitch;
    s_tiles[index].format = comp;
    s_tiles[index].base   = base;
    s_tiles[index].bank   = bank;
    s_tiles[index].bound  = 0;

    /* Build tile register value (simplified) */
    s_tiles[index].tile  = (location << 0) | (pitch << 8);
    s_tiles[index].limit = offset + size - 1;

    return CELL_OK;
}

/* NID: 0xD34A420D */
s32 cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height,
                    u32 cullStart, u32 zFormat, u32 aaFormat,
                    u32 zcullDir, u32 zcullFormat,
                    u32 sFunc, u32 sRef, u32 sMask)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetZcull(index=%u, offset=0x%X, %ux%u)\n",
           index, offset, width, height);

    s_zcull[index].offset      = offset;
    s_zcull[index].width       = width;
    s_zcull[index].height      = height;
    s_zcull[index].cullStart   = cullStart;
    s_zcull[index].zFormat     = zFormat;
    s_zcull[index].aaFormat    = aaFormat;
    s_zcull[index].zcullDir    = zcullDir;
    s_zcull[index].zcullFormat = zcullFormat;
    s_zcull[index].sFunc       = sFunc;
    s_zcull[index].sRef        = sRef;
    s_zcull[index].sMask       = sMask;
    s_zcull[index].bound       = 0;

    return CELL_OK;
}

/* NID: 0x4524CCCD */
s32 cellGcmBindTile(u8 index)
{
    /* libgcm_sys 0x02002AB8 (cellGcmBindTile, NID 0x4524CCCD, opd 0x0200BB64) takes the index as the low byte
     * of r3 (0x02002ACC) and compares it first, `cmplwi 0xE; bgt` (0x02002AD4, 0x02002AE4): 15 and up return
     * 0x80210003 = CELL_GCM_ERROR_INVALID_VALUE (preloaded in r3 at 0x02002AC8/0x02002AE0) before anything
     * else. The same limit and the same code. */
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] BindTile(index=%u)\n", index);
    s_tiles[index].bound = 1;
    return CELL_OK;
}

/* NID: 0x9DC04436 */
s32 cellGcmBindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] BindZcull(index=%u)\n", index);
    s_zcull[index].bound = 1;
    return CELL_OK;
}

/* NID: 0xD9B7653E */
s32 cellGcmUnbindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] UnbindTile(index=%u)\n", index);
    s_tiles[index].bound = 0;
    return CELL_OK;
}

/* NID: 0xA75640E8 */
s32 cellGcmUnbindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] UnbindZcull(index=%u)\n", index);
    s_zcull[index].bound = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Misc
 * -----------------------------------------------------------------------*/

/* NID: 0x055BD74D
 * SDK prototype (cell/gcm.h): uint32_t cellGcmGetTiledPitchSize(const uint32_t size)
 * -- returns the pitch BY VALUE in r3 (no out-pointer!). The old signature here
 * took (size, u32* pitch-out) and returned a status code, so every caller got 0
 * / an error code as its "pitch" -- gcm/cube's `bne` on the result then skipped
 * its entire config+framebuffer-alloc block (GetConfiguration never ran, local
 * memory base stayed 0, AddressToOffset(0) auto-mapped a bogus IO alias, and the
 * FIFO teleported into it). Returns 0 only for size > max, like hardware. */
u32 cellGcmGetTiledPitchSize(u32 size)
{
    u32 r = 0;
    /* Smallest valid tiled pitch >= size (RSX supports only specific pitches). */
    if (size) {
        for (int i = 0; i < s_valid_pitch_count; i++) {
            if (s_valid_pitches[i] >= size) { r = s_valid_pitches[i]; break; }
        }
    }
    { static int _n=0; if (PS3_LOG_FIRST(_n, 4)) fprintf(stderr, "[cellGcmSys] GetTiledPitchSize(0x%X) -> 0x%X\n", size, r); }
    return r;
}

/* NID: 0x51C9D62B */
void cellGcmSetDebugOutputLevel(u32 level)
{
    printf("[cellGcmSys] SetDebugOutputLevel(level=%u)\n", level);
    s_debug_level = level;
}

/* ---------------------------------------------------------------------------
 * Additional functions needed by Tokyo Jungle
 * -----------------------------------------------------------------------*/

/* Notify data area — similar to report data, 16 bytes per slot */

/* Notify data lives in the IO page 0xF1 (IO 0x0F100000 + index * 0x40), the
 * page NV4097_NOTIFY writes {timestamp, 0} into. This returned a HOST pointer,
 * so the guest wrote its "pending" flags through a truncated address and the
 * RSX had nothing to clear: Twisted Metal's SpuPostFX thread waits for that
 * clear, so post-processing never ran and the frame label it releases never
 * moved. NULL when the title has not mapped the page, as libgcm does. */
void* cellGcmGetNotifyDataAddress(u32 index)
{
    u32 ea = gcm_io2ea(0x0F100000u);
    if (index >= 256 || !ea) return NULL;
    return (void*)(uintptr_t)(ea + index * 0x40u);
}

/* Timestamp location — returns CELL_GCM_LOCATION_LOCAL or MAIN */
/* The Location suffix names an ARGUMENT, not an out-parameter: it is the
 * caller saying which memory the report lives in, CELL_GCM_LOCATION_LOCAL or
 * _MAIN, exactly as cellGcmGetReportDataLocation and
 * cellGcmGetReportDataAddressLocation take it three hundred lines below. This
 * one alone was declared u32* and treated the value as a guest address to
 * write the location INTO, which inverts the parameter and loses the return.
 *
 * A title passing CELL_GCM_LOCATION_MAIN therefore had a 1 taken for an
 * effective address, and vm_write32 stored four bytes at guest address 1.
 * Yakuza: Dead Souls does that during display setup and dies there.
 *
 * It returns the timestamp, like cellGcmGetTimeStamp, whose body this is with
 * a location the report table does not distinguish. */
u64 cellGcmGetTimeStampLocation(u32 index, u32 location)
{
    (void)location;

    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    s_report_data[index].timestamp = get_timestamp_ns();
    return s_report_data[index].timestamp;
}

/* SetTileInfo — alternative to SetTile with same parameters */
s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size,
                       u32 pitch, u8 comp, u16 base, u8 bank)
{
    /* Delegates to existing SetTile */
    return cellGcmSetTile(index, location, offset, size, pitch, comp, base, bank);
}

/* Default FIFO size — configures command buffer size before init */
static u32 s_default_fifo_size = 0x40000; /* 256KB default */

s32 cellGcmSetDefaultFifoSize(u32 size)
{
    printf("[cellGcmSys] SetDefaultFifoSize(0x%X)\n", size);
    s_default_fifo_size = size;
    return CELL_OK;
}

/* Internal flip commands — called directly by some games */
/* libgcm's internal exports pass the command context FIRST -- taking
 * bufferId as arg1 made the range check eat the context pointer, so the
 * flip counter never advanced (wave: presents free-ran 4x per frame,
 * layout flashing/zooming). */
/* NID: 0x21397818. gcmSetFlipCommand (PSL1GHT libgcm_sys exports.h) -- it
 * was misassigned to cellGcmGetLabelAddress (real NID 0xF80196C1), so every
 * import-based flip (LBP: cellGcmSetFlip -> stub 0x7482FC(ctx, id)) landed
 * in the label getter and no flip was EVER issued: the intro-movie pump ran,
 * drew, "flipped" into the wrong function, and the screen stayed black. */
s32 _cellGcmSetFlipCommand(void* ctx, u32 bufferId)
{
    if (gcm_flip_into_fifo((u32)(uintptr_t)ctx, bufferId))
        return gcm_flip_request(bufferId, 1);
    return cellGcmSetFlipCommand(bufferId);
}

/* libgcm writes this into the context: NV406E SEMAPHORE_OFFSET and
 * SEMAPHORE_ACQUIRE for the label, then the flip, so the RSX flips once the
 * label holds the value. The import used to write the label itself, from the
 * PPU, and flip at call time: whoever was meant to write the label was
 * overruled, and the flip did not wait for anything. Twisted Metal flips this
 * way on its main context. Queue it the same way as _cellGcmSetFlipCommand,
 * and fall back to the immediate form when the context is not a mapped command
 * buffer or has no room. */
/* NID: 0xD8F88E1A */
s32 _cellGcmSetFlipCommandWithWaitLabel(void* ctx, u32 bufferId,
                                        u32 labelIndex, u32 labelValue)
{
    const u32 c = (u32)(uintptr_t)ctx;
    if (c && labelIndex < CELL_GCM_MAX_LABEL_COUNT &&
        bufferId < CELL_GCM_MAX_DISPLAY_BUFFER_NUM && s_display_buffer_set[bufferId]) {
        const u32 end = vm_read32(c + 4), cur = vm_read32(c + 8);
        if (cur + 16 + 8 + GCM_RECYCLE_SLACK <= end && gcm_ea2io(cur) != 0xFFFFFFFFu) {
            vm_write32(cur + 0,  0x00040064u);   /* SEMAPHORE_OFFSET  */
            vm_write32(cur + 4,  labelIndex * GCM_LABEL_STRIDE);
            vm_write32(cur + 8,  0x00040068u);   /* SEMAPHORE_ACQUIRE */
            vm_write32(cur + 12, labelValue);
            vm_write32(c + 8, cur + 16);
            if (gcm_flip_into_fifo(c, bufferId))
                return gcm_flip_request(bufferId, 1);
            vm_write32(c + 8, cur);              /* no room for the flip */
        }
    }
    return cellGcmSetFlipCommandWithWaitLabel(bufferId, labelIndex, labelValue);
}

/* ---------------------------------------------------------------------------
 * Additional functions (RPCS3 parity)
 * -----------------------------------------------------------------------*/

/* FIFO command buffer callback — called when put pointer wraps.
 * In recomp we don't have a real GPU consuming FIFO, so this is a no-op. */
s32 cellGcmCallback(void* context, u32 count)
{
    (void)context;
    (void)count;
    return CELL_OK;
}

/* Map RSX local memory — returns the base address and size of local VRAM */
s32 cellGcmMapLocalMemory(u32* address, u32* size)
{
    if (!address || !size)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* Guest out-params, like every other pointer parameter in this file. */
    vm_write32((uint32_t)(uintptr_t)address, s_config.localAddress);
    vm_write32((uint32_t)(uintptr_t)size,    s_config.localSize);

    printf("[cellGcmSys] MapLocalMemory(address=0x%08X, size=0x%X)\n",
           s_config.localAddress, s_config.localSize);
    return CELL_OK;
}

/* Shutdown RSX — clear all state */
void cellGcmTerminate(void)
{
    printf("[cellGcmSys] Terminate\n");

    s_gcm_initialized = 0;
    s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_io_map_reserved = 0;
    s_default_fifo_mode = 0;
    GCM_USER_STORE(0);
    gcm_user_ring_reset();
    gcm_driver_events_reset();
    memset(&s_ib, 0, sizeof s_ib);

    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_control, 0, sizeof(s_control));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    __atomic_store_n(&s_io_mapping_hw, 0, __ATOMIC_RELAXED);
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_zcull, 0, sizeof(s_zcull));
    memset(s_report_data, 0, sizeof(s_report_data));
    s_report_dma = 0xFEED0000u;
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));

    s_flip_handler     = NULL;
    s_vblank_handler   = NULL;
    s_user_handler     = NULL;
    s_second_v_handler = NULL;
    s_graphics_handler = NULL;
    s_queue_handler    = NULL;
    s_local_mem_allocated = 0;
    s_io_mapping_count = 0;
    /* Unmapped = 0xFFFF. The tables are static (zero-initialized), so without
     * this every unmapped EA page silently aliased io page 0 (and vice versa),
     * producing plausible-but-wrong offsets instead of a translation failure. */
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    s_current_display_buffer_id = 0;
    s_last_flip_time = 0;
}

/* Return remaining IO map space (256MB total IO space minus already mapped) */
u32 cellGcmGetMaxIoMapSize(void)
{
    /* RSX has a 256MB IO address window */
    u32 total_io = 256 * 1024 * 1024;
    u32 used = 0;

    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active)
            used += s_io_mappings[i].size;
    }

    return (used < total_io) ? (total_io - used) : 0;
}

/* Reserve IO map space (pre-allocation for future mappings) */
s32 cellGcmReserveIoMapSize(u32 size)
{
    u32 max_size = cellGcmGetMaxIoMapSize();
    if (size > max_size - s_io_map_reserved)
        return CELL_GCM_ERROR_FAILURE;

    s_io_map_reserved += size;
    return CELL_OK;
}

/* Unreserve IO map space */
s32 cellGcmUnreserveIoMapSize(u32 size)
{
    if (size > s_io_map_reserved)
        return CELL_GCM_ERROR_FAILURE;

    s_io_map_reserved -= size;
    return CELL_OK;
}

/* Return incrementing VBlank counter */
u32 cellGcmGetVBlankCount(void)
{
    /* Approximate: increment each time queried (games use this for timing) */
    return s_vblank_count++;
}

/* Return tile info for a given index */
CellGcmTileInfo* cellGcmGetTileInfo(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT) {
        printf("[cellGcmSys] WARNING: GetTileInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_tiles[index];
}

/* Return zcull info for a given index */
CellGcmZcullInfo* cellGcmGetZcullInfo(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT) {
        printf("[cellGcmSys] WARNING: GetZcullInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_zcull[index];
}

/* Return display buffer info by index */
CellGcmDisplayInfo* cellGcmGetDisplayInfo(u32 index)
{
    if (index >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM) {
        printf("[cellGcmSys] WARNING: GetDisplayInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_display_buffers[index];
}

/* Set default FIFO mode (before init) */
void cellGcmInitDefaultFifoMode(s32 mode)
{
    printf("[cellGcmSys] InitDefaultFifoMode(mode=%d)\n", mode);
    s_default_fifo_mode = mode;
}

/* Set command buffer to defaults (reset put/get pointers) */
void cellGcmSetDefaultCommandBuffer(void)
{
    printf("[cellGcmSys] SetDefaultCommandBuffer\n");
    s_control.put = 0;
    s_control.get = 0;
    s_control.ref = 0;

    /* _cellGcmSetDefaultCommandBuffer (libgcm_sys 0x020038D0) re-runs the segment-record
     * builder (0x020036D4) with the sizes cellGcmInit saved: the default context goes back to
     * its first segment and put and get to its start. It does not touch the title's pointer
     * to the context. */
    if (s_ib.valid) gcm_ib_reset_context(s_ib.ctx_ea);
    /* On hardware this re-points gCellGcmCurrentContext at the default context
     * cellGcmInit built. Zeroing a host-side struct is not that: a title that
     * calls it to go back to the big default buffer stays on whatever smaller
     * segment it had switched to.
     *
     * GCM_DEFAULT_CTX_REPOINT=1 does the real thing -- write our context's EA
     * into the guest variable cellGcmInit's ctx_out pointed at, and rewind that
     * context. Off by default because a title managing its own segment chain
     * may hold pointers into the old one; this exists to be measured, not
     * assumed.
     *
     * Measured once now, on Virtua Fighter 5, which is the case this was
     * written for. VF5's AMGL keeps its own command buffers (it repoints
     * gCellGcmCurrentContext at a 512 KB segment of its own inside the 4 MB it
     * maps) and calls this entry to recover when one fills. Getting a host-side
     * struct zeroed instead leaves it on the exhausted segment, so it allocates
     * another, fills that, and asks again:
     *
     *     [AMGL]:[ERROR] Command Buffer Overflow!   x91, 636 calls to here
     *
     * With the repoint: **0 overflows**, and the command packets reaching the
     * draw engine go from 12,756 to 15,097 over the same five minutes. Still
     * one title's evidence, so still opt-in -- but it is no longer unmeasured,
     * and a port whose title manages its own segments should try it. */
    if (getenv("GCM_DEFAULT_CTX_REPOINT") && s_gcm_ctx_out_ea && s_gcm_context_ea) {
        vm_write32(s_gcm_context_ea + 0x8, vm_read32(s_gcm_context_ea + 0x0));
        vm_write32(s_gcm_ctx_out_ea, s_gcm_context_ea);
        printf("[cellGcmSys] SetDefaultCommandBuffer: repointed "
               "gCellGcmCurrentContext@0x%08X -> 0x%08X\n",
               s_gcm_ctx_out_ea, s_gcm_context_ea);
    }
}

/* Debug dump — no-op in recomp */
void cellGcmDumpGraphicsError(void)
{
    printf("[cellGcmSys] DumpGraphicsError (no-op)\n");
}

/* The default command buffer's size in words: the part of cellGcmInit's command size that is a
 * whole number of segments (libgcm_sys 0x02003464 returns the context record's +0x24). 0 before
 * cellGcmInit. */
u32 cellGcmGetDefaultCommandWordSize(void)
{
    return s_ib.valid ? s_ib.cmd_words : 0;
}

/* The default segment's size in words: 0x2000 words, 32 KiB (libgcm_sys 0x02003470, +0x30). */
u32 cellGcmGetDefaultSegmentWordSize(void)
{
    return s_ib.valid ? s_ib.seg_words : 0;
}

/* Immediate flip — perform flip right now */
s32 cellGcmSetFlipImmediate(u32 bufferId)
{
    printf("[cellGcmSys] SetFlipImmediate(bufferId=%u)\n", bufferId);
    return cellGcmSetFlipCommand(bufferId);
}

/* Set flip status directly */
void cellGcmSetFlipStatus(u32 status)
{
    s_flip_status = status;
}

/* Store graphics handler callback */
void cellGcmSetGraphicsHandler(CellGcmGraphicsHandler handler)
{
    printf("[cellGcmSys] SetGraphicsHandler(%p)\n", (void*)(size_t)handler);
    s_graphics_handler = handler;
}

/* Store queue handler callback */
void cellGcmSetQueueHandler(CellGcmQueueHandler handler)
{
    printf("[cellGcmSys] SetQueueHandler(%p)\n", (void*)(size_t)handler);
    s_queue_handler = handler;
    gcm_handler_bit(GCM_HBIT_QUEUE, handler != NULL);
}

/* Set second V frequency */
void cellGcmSetSecondVFrequency(u32 freq)
{
    printf("[cellGcmSys] SetSecondVFrequency(%u)\n", freq);
    s_second_v_frequency = freq;
}

/* Set VBlank frequency */
void cellGcmSetVBlankFrequency(u32 freq)
{
    printf("[cellGcmSys] SetVBlankFrequency(%u)\n", freq);
    s_vblank_frequency = freq;
}

/* Store user command value */
void cellGcmSetUserCommand(u32 cmd)
{
    GCM_USER_STORE(cmd);
}

/* Called when a FIFO consumer retires a user-interrupt method. The command goes onto the ordered
 * ring (AM-27): every command is delivered, in the order the RSX retired it, never coalesced;
 * guest code runs later in the pump. A full ring drops the command and counts it. */
void cellGcmQueueUserCommand(u32 cmd)
{
    GCM_USER_STORE(cmd);
    gcm_user_ring_push(cmd);
#ifdef _WIN32
    gcm_pump_wake();
#endif
}

/* Invalidate a tile region (unbind + clear) */
s32 cellGcmSetInvalidateTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetInvalidateTile(index=%u)\n", index);

    s_tiles[index].bound = 0;
    memset(&s_tiles[index], 0, sizeof(CellGcmTileInfo));
    return CELL_OK;
}

/* EA IO address remap — stub, sorting not needed in recomp */
void cellGcmSortRemapEaIoAddress(void)
{
    /* No-op: IO address remapping not required for recomp */
}

/* Report data with location parameter */
CellGcmReportData* cellGcmGetReportDataAddressLocation(u32 index, u32 location)
{
    (void)location;  /* We only have one report area */

    if (index >= CELL_GCM_MAX_REPORT_COUNT) {
        printf("[cellGcmSys] WARNING: GetReportDataAddressLocation index %u out of range\n", index);
        return NULL;
    }
    return &s_report_data[index];
}

/* Get report value at index with location */
u32 cellGcmGetReportDataLocation(u32 index, u32 location)
{
    (void)location;

    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    return s_report_data[index].value;
}

/* ---------------------------------------------------------------------------
 * VRAM upload registry
 *
 * A title whose texture uploads land at an address that does not match the
 * offset it later programs into SET_TEXTURE_OFFSET leaves the sampler reading
 * untouched memory. Recording what was uploaded lets the texture path find the
 * real bytes by size instead of trusting the offset. Diagnostic bridge: it is
 * a lookup, not a fix for the offset bookkeeping itself.
 * -----------------------------------------------------------------------*/
#define RSX_UPLOAD_LOG 256
static struct { u32 dst, size; int claimed; } s_uploads[RSX_UPLOAD_LOG];
static u32 s_upload_n;

void rsx_note_vram_upload(u32 dst_ea, u32 size)
{
    if (!size) return;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].dst     = dst_ea;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].size    = size;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].claimed = 0;
    s_upload_n++;
}

/* OLDEST unclaimed upload of exactly `want` bytes, claimed on return. Matching
 * by size alone hands every same-sized surface the same image; consuming them in
 * order pairs the Nth bind of a size with the Nth upload of that size, which is
 * the order a title loads and then draws its materials in. Returns 0 if none. */
u32 rsx_find_vram_upload(u32 want)
{
    u32 n = s_upload_n < RSX_UPLOAD_LOG ? s_upload_n : RSX_UPLOAD_LOG;
    u32 first = s_upload_n > RSX_UPLOAD_LOG ? s_upload_n - RSX_UPLOAD_LOG : 0;
    for (u32 i = 0; i < n; i++) {
        u32 idx = (first + i) % RSX_UPLOAD_LOG;
        if (s_uploads[idx].size == want && !s_uploads[idx].claimed) {
            s_uploads[idx].claimed = 1;
            return s_uploads[idx].dst;
        }
    }
    return 0;
}

/* Let the same pairing repeat next frame. */
void rsx_reset_upload_claims(void)
{
    for (u32 i = 0; i < RSX_UPLOAD_LOG; i++) s_uploads[i].claimed = 0;
}

/* ---------------------------------------------------------------------------
 * sys_rsx_* bridge (libs/video/sys_rsx.c)
 *
 * A PS3 firmware module links libgcm statically and drives RSX through the lv2
 * syscalls instead of importing cellGcmSys, so none of the HLE entry points above
 * ever run. Everything the FIFO walker needs is still here -- it just has to be
 * reachable from the syscall layer. These four accessors are that surface; the
 * walker, the method decoder and rsx_live_draw are untouched.
 * -----------------------------------------------------------------------*/

/* Bring the GCM state up the way cellGcmInit would, minus the IO window (the
 * driver maps that itself with sys_rsx_context_iomap). Returns the guest base of
 * RSX local memory, which is what sys_rsx_memory_allocate hands back. */
u32 cellGcm_syscall_bringup(u32 local_size)
{
    if (!s_gcm_initialized)
        cellGcmInit(0x10000, 0, 0);
    if (local_size)
        s_config.localSize = local_size;
    s_gcm_syscall_mode = 1;
    return s_config.localAddress;
}

/* Guest EA of the put/get/ref triple. The driver gets this as
 * sys_rsx_context_allocate's lpar_dma_control + 0x40 (verified in the image:
 * `lwz r3,0x18(r9)` / `addi r3,r3,64`), so the syscall returns this minus 0x40
 * and the driver's own flushes land exactly where the walker reads. */
u32 cellGcm_control_guest_addr(void) { return GCM_CONTROL_GUEST_ADDR; }

/* sys_rsx_context_iomap / iounmap: the same 1MB-page offset table
 * cellGcmMapEaIoAddress fills, which is what gcm_io2ea() and every offset
 * resolution read. */
void cellGcm_syscall_iomap(u32 ea, u32 io, u32 size)
{
    populate_offset_table(ea, io, size);
    /* A free slot, not s_io_mappings[count]: an unmap leaves a hole, and the
     * count then indexes a live slot. */
    IoMapping* m = find_free_mapping();
    if (m) {
        m->ea = ea; m->io = io; m->size = size; m->active = 1;
        s_io_mapping_count++;
    }
    if (!s_config.ioSize) { s_config.ioAddress = ea; s_config.ioSize = size; }
}

void cellGcm_syscall_iounmap(u32 io, u32 size)
{
    u32 page = io >> 20;
    u32 ea = (page < 65536 && s_ea_address_table[page] != 0xFFFF)
             ? ((u32)s_ea_address_table[page] << 20) : 0;
    if (ea) clear_offset_table(ea, io, size);
}

/* sys_rsx_context_attribute(0x001): the driver's explicit FIFO reset. Move the
 * walker with it -- leaving s_fifo_getoff where it was makes the next drain walk
 * from a stale offset through whatever the driver has since overwritten. */
void cellGcm_syscall_set_fifo(u32 put, u32 get)
{
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, put);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, get);
    s_fifo_getoff  = get;
    s_fifo_calloff = 0;
}

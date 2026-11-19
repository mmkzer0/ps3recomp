/*
 * ps3recomp - the window of guest memory a guest store has to be ordered after
 *
 * A device model (the RSX walker) is handed commands through guest memory and
 * runs on a host thread, so a guest store to a word the device is about to
 * consume can land before the device looks. The model registers one window and
 * a hook; every guest store that overlaps it runs the hook before its bytes
 * land. This state lives in the runtime library rather than in the per-title
 * loader so that both the PPU store path (runtime/ppu/ppu_loader.cpp) and the
 * SPU MFC path (runtime/spu/spu_dma.h) reach the same window.
 *
 * The hook and the walker-pass counters are the RSX label ordering of #17: a
 * store into a label waits for one complete walker pass that began after the
 * store was reached (libs/video/cellGcmSys.c, "guest stores to a label are
 * ordered after the walker"). See docs/RUNTIME.md, "Guest Store Hook".
 */
#ifndef PS3EMU_LABEL_WINDOW_H
#define PS3EMU_LABEL_WINDOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* lo | len << 32; len 0 = no window. Exported by name because the lifted
 * code's PPU_STORE_HOOK (tools/ppu_lifter.py, HEADER_PREAMBLE) reads it inline
 * before a raw store, and the PPU store path does the same here. */
extern uint64_t ppu_store_window;

typedef void (*ps3_label_window_fn)(uint32_t ea, uint32_t value);

/* Register the one window (fn NULL, or len 0, registers none). The hook runs on
 * the storing thread with the old bytes still in memory, with the first window
 * address the store covers. Replaces the previous registration atomically. */
void ppu_set_store_hook(uint32_t lo, uint32_t len, ps3_label_window_fn fn);

/* The inline maybe-overlap test: the last byte of the store is at or above the
 * window's start and its first at or below its end. A store that wraps the
 * 32-bit space may take the out-of-line test needlessly, never the other way.
 * n must be > 0. */
static inline int ps3_label_window_maybe(uint32_t ea, uint32_t n)
{
    const uint64_t w = __atomic_load_n(&ppu_store_window, __ATOMIC_ACQUIRE);
    return (uint64_t)(uint32_t)(ea + n - 1u - (uint32_t)w) < (w >> 32) + n - 1u;
}

/* Decide whether [ea, ea + n) overlaps the window in 64-bit arithmetic and run
 * the hook for the first window address the store covers. */
void ps3_label_window_store(uint32_t ea, uint32_t n, uint32_t value);

/* The exact overlap decision, for a device that writes guest memory itself (an
 * SPU MFC PUT/PUTL, or an atomic PUTLLC/PUTLLUC/PUTQLLUC line). n is the
 * transfer size in bytes. */
int ps3_label_window_overlaps(uint64_t ea, uint32_t n);

/* The model's own control registers, a sub-span of the window: a store there needs the
 * model's hook (a doorbell), never the ordering. A store into a label has to be ordered
 * after the model was handed it (the store hook above); a store into the model's control
 * registers only tells the model there is work, and its value is its own protocol's. This
 * is what lets an SPU MFC PUT wholly inside the control span land: the runtime cannot
 * order an SPU store (waiting for the model would deadlock the producer), so a PUT that
 * reaches a label still stops, while one that only kicks the model is made and the model
 * is told it landed (`landed`, after the bytes are in memory). `landed` may be NULL, in
 * which case the store is still allowed. The span must be inside the window; len 0
 * registers none. Replaces the previous registration atomically. */
typedef void (*ps3_label_window_landed_fn)(uint32_t ea);
void ppu_set_control_span(uint32_t lo, uint32_t len, ps3_label_window_landed_fn landed);

/* Is [ea, ea + n) wholly inside the control span? The caller has already established that
 * the span overlaps the window; this is the exact test, in 64-bit arithmetic, and it is
 * false when no control span is registered. n must be > 0. */
int ps3_label_window_in_control(uint64_t ea, uint32_t n);

/* The bytes of a store into the control span are in memory: run the model's landed hook,
 * once. Called by the runtime's own store paths (an SPU PUT), after the copy. */
void ps3_label_window_control_landed(uint32_t ea);

/* Process-wide count of SPU PUT/PUTL and atomic put transfers whose span overlapped the
 * window. Bumped before the named stop, so it is observable from the first
 * such transfer on (the stop's `count` field, or this accessor). */
unsigned long long ps3_label_window_spu_puts(void);
unsigned long long ps3_label_window_note_spu_put(void);

/* Walker-pass ordering. A pass is one complete walker run that read everything
 * the guest had published. A non-walker store that overlaps the window waits
 * for a pass that began after it was reached (`next_pass`), which makes the
 * walker read and evaluate every ACQUIRE queued before the store against the
 * label's old value. The counters are process-wide; "this thread ran a pass"
 * is per thread, because a walker thread cannot wait for itself and may run
 * guest code (a flip handler) that stores a label. */
void ps3_label_window_pass_begin(void);   /* call under the walker's lock, before it reads `put` */
void ps3_label_window_pass_end(void);     /* call under the walker's lock, after the pass */
int ps3_label_window_on_walker(void);
unsigned long long ps3_label_window_next_pass(void);        /* a pass beginning after this call gets this id */
int ps3_label_window_pass_done(unsigned long long target);  /* that pass has finished */

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_LABEL_WINDOW_H */

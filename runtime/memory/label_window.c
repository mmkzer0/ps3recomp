/*
 * ps3recomp - label window state (runtime/memory/label_window.h)
 *
 * One window and one hook, process-wide: a device model registers it and every
 * guest store that overlaps it is ordered after what the model was handed. The
 * walker-pass counters live here too, because the hook's wait (and the walker's
 * own store exemption) are part of the same contract.
 */
#include "label_window.h"

#include <stdatomic.h>
#include <stddef.h>

#if defined(_MSC_VER)
#  define LW_TLS __declspec(thread)
#else
#  define LW_TLS __thread
#endif

uint64_t ppu_store_window;                  /* zero-initialised: no window */

static _Atomic(ps3_label_window_fn) s_hook;
static _Atomic unsigned long long s_pass_begin, s_pass_end;
static _Atomic unsigned long long s_spu_puts;
static LW_TLS int t_walker;                 /* this thread has run a walker pass */

void ppu_set_store_hook(uint32_t lo, uint32_t len, ps3_label_window_fn fn)
{
    /* Publish the hook before the window: a thread that sees the window must
     * not run a stale or NULL callback for it. Clearing publishes the zero
     * window first, so no store can see no window and then call a hook. */
    if (!fn) {
        __atomic_store_n(&ppu_store_window, 0, __ATOMIC_RELEASE);
        atomic_store_explicit(&s_hook, NULL, memory_order_release);
        return;
    }
    atomic_store_explicit(&s_hook, fn, memory_order_release);
    __atomic_store_n(&ppu_store_window, ((uint64_t)len << 32) | lo, __ATOMIC_RELEASE);
}

void ps3_label_window_store(uint32_t ea, uint32_t n, uint32_t value)
{
    const uint64_t w = __atomic_load_n(&ppu_store_window, __ATOMIC_ACQUIRE);
    const uint64_t lo = (uint32_t)w, len = w >> 32;
    if (!len || (uint64_t)ea + n <= lo || (uint64_t)ea >= lo + len) return;
    const ps3_label_window_fn fn = atomic_load_explicit(&s_hook, memory_order_acquire);
    if (fn) fn(ea > lo ? ea : (uint32_t)lo, value);
}

int ps3_label_window_overlaps(uint64_t ea, uint32_t n)
{
    if (!n) return 0;
    const uint64_t w = __atomic_load_n(&ppu_store_window, __ATOMIC_ACQUIRE);
    const uint64_t lo = (uint32_t)w, len = w >> 32;
    return len && ea < lo + len && ea + n > lo;
}

unsigned long long ps3_label_window_spu_puts(void)
{
    return atomic_load_explicit(&s_spu_puts, memory_order_relaxed);
}

unsigned long long ps3_label_window_note_spu_put(void)
{
    return atomic_fetch_add_explicit(&s_spu_puts, 1ull, memory_order_relaxed) + 1ull;
}

/* The pass counters keep the seq_cst order they had in cellGcmSys.c before the move. The label store's wait
 * (gcm_guest_label_store) loads s_pass_begin after its thread published `put`, and the walker reads `put`
 * after bumping s_pass_begin: a store-buffering shape. Weakening these orders is a change of its own, with
 * its own litmus test, not part of moving the state. */
void ps3_label_window_pass_begin(void)
{
    t_walker = 1;
    atomic_fetch_add_explicit(&s_pass_begin, 1ull, memory_order_seq_cst);
}

void ps3_label_window_pass_end(void)
{
    atomic_fetch_add_explicit(&s_pass_end, 1ull, memory_order_seq_cst);
}

int ps3_label_window_on_walker(void)
{
    return t_walker;
}

unsigned long long ps3_label_window_next_pass(void)
{
    return atomic_load_explicit(&s_pass_begin, memory_order_seq_cst) + 1ull;
}

int ps3_label_window_pass_done(unsigned long long target)
{
    return atomic_load_explicit(&s_pass_end, memory_order_seq_cst) >= target;
}

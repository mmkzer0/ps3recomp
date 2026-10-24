/*
 * ps3recomp - the HLE call observer
 *
 * A port that compares its run with a recording (the event-level gates of docs/PS3DIFF.md) needs
 * to see every firmware import the guest makes, with the arguments it made it with and what came
 * back. The dispatch point every import goes through, ps3_hle_call, reports to one observer:
 *
 *   on_call    before the handler runs; the guest's argument registers (r3..r10) and its stack
 *              are as the guest left them
 *   on_return  after the handler returned; r3 is the return value
 *
 * Both run on the calling guest thread, concurrently with other threads' calls: an observer that
 * keeps state serialises it itself. An import that ends in a named stop, or that never returns
 * (sys_ppu_thread_exit), reaches on_call only. Neither callback may block or call back into
 * ps3_hle_call.
 *
 * Off unless installed: the cost on the plain path is one atomic load per import. One observer
 * per process; installing replaces, NULL removes. Install it before the guest runs.
 */
#ifndef PS3EMU_HLE_OBSERVER_H
#define PS3EMU_HLE_OBSERVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ppu_context;

typedef struct ps3_hle_observer {
    void* user;
    void (*on_call)(void* user, uint32_t nid, const struct ppu_context* ctx);
    void (*on_return)(void* user, uint32_t nid, const struct ppu_context* ctx);
} ps3_hle_observer;

/* The observer must outlive its installation (the pointer is kept, not copied). */
void ps3_hle_set_observer(const ps3_hle_observer* observer);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_HLE_OBSERVER_H */

/*
 * ps3recomp - replay sync events: where a guest run says "this happened"
 *
 * A pad replay (libs/input/pad_trace.h) keys its input on the guest's poll ordinal. How many polls a guest has
 * made when a loaded scene appears depends on the host: a step loop that catches up wall time makes more polls per
 * flip, a slow backend delays a load. The replay therefore lets a trace carry sync records, each naming an event
 * of the guest's own course and the poll ordinal the recording had made when it happened; the replay re-bases its
 * input clock on the event (docs/MODULES_REFERENCE.md, "cellPad: record and replay").
 *
 * This header is the seam between the code that sees an event and the replay that wants it. The reporting side
 * (the draw engine's pipeline creation) calls ps3_replay_sync_event and knows nothing of pads; the pad replay
 * installs the sink when its trace holds sync records. Without a sink the call is one atomic load.
 *
 * The sink variable is weak (header-only, like g_ps3_ft), so a fixture that links the reporting side without the
 * input library still links.
 */
#ifndef PS3EMU_REPLAY_SYNC_H
#define PS3EMU_REPLAY_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event kinds. The numbers are internal; a trace names a kind by its string (PS3_REPLAY_SYNC_NAME_*). */
enum ps3_replay_sync_kind {
    PS3_REPLAY_SYNC_NONE     = 0,
    PS3_REPLAY_SYNC_PIPELINE = 1,   /* the draw engine built a pipeline; key = the engine's 64-bit pipeline key */
};

#define PS3_REPLAY_SYNC_NAME_PIPELINE "pipeline"

typedef void (*ps3_replay_sync_fn)(uint32_t kind, uint64_t key);

#if defined(_MSC_VER) || defined(__MINGW32__)
#  define PS3_REPLAY_SYNC_SHARED __declspec(selectany)
#else
#  define PS3_REPLAY_SYNC_SHARED __attribute__((weak))
#endif

PS3_REPLAY_SYNC_SHARED ps3_replay_sync_fn g_ps3_replay_sync_sink = 0;

/* Install the sink (NULL removes it). Any thread. */
static inline void ps3_replay_sync_set_sink(ps3_replay_sync_fn fn)
{
    __atomic_store_n(&g_ps3_replay_sync_sink, fn, __ATOMIC_RELEASE);
}

/* Report one event. Any thread; the sink runs on the caller's thread and must not block on the caller. */
static inline void ps3_replay_sync_event(uint32_t kind, uint64_t key)
{
    const ps3_replay_sync_fn fn = __atomic_load_n(&g_ps3_replay_sync_sink, __ATOMIC_ACQUIRE);
    if (fn) fn(kind, key);
}

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_REPLAY_SYNC_H */

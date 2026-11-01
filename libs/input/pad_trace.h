/*
 * ps3recomp - pad trace: record what the guest was handed, replay it later
 *
 * A performance or regression run is only comparable if the same input reaches
 * the guest at the same point of the run. A human cannot repeat a battle, so one
 * run is recorded (pad.record) and later runs replay it (pad.replay). Config keys
 * and the file format are in docs/MODULES_REFERENCE.md ("cellPad: record and
 * replay"); the user-visible behaviour is in docs/CONFIG.md.
 *
 * What is recorded is the guest-visible result of each cellPadGetData, per port,
 * after the host backend and every injector (PAD_SCRIPT, PAD_FILE, PAD_STICK,
 * PAD_AUTOPRESS, keyboard) have been applied: the words that go to guest memory.
 * Each record is keyed by (flip, poll). `flip` is the guest's flip-request count
 * at the call; `poll` is this port's GetData ordinal since that count last
 * changed. The flip count is the one clock that does not depend on how fast the
 * host runs the title. Only a change of state is written (plus the first poll of
 * a port), so a trace is a list of presses and releases that can be read and
 * diffed.
 *
 * Replay is state-based: a poll is answered with the state of the latest record
 * of its port at or before the poll's key. The host pad and the injectors play
 * no part in what the guest sees.
 *
 * The module is self-contained: it depends on stop.h and the C library. The
 * flip clock comes in through PadTraceOptions, and cellPad.c does the config
 * reading and the hooks.
 *
 * Failure policy: a trace that cannot be written, or a replay file that is not
 * a well-formed version-1 trace, ends the run with the named stop `pad-trace`
 * (role, line, reason); nothing is guessed or skipped.
 *
 * Threading: any guest thread may poll. One mutex per PadTrace guards every
 * field; the flip count is read inside it, so keys in the file never go
 * backwards.
 */

#ifndef PS3EMU_PAD_TRACE_H
#define PS3EMU_PAD_TRACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAD_TRACE_FORMAT      "ps3recomp-pad-trace"
#define PAD_TRACE_VERSION     1u
#define PAD_TRACE_MAX_PORTS   7u    /* CELL_PAD_MAX_PORT_NUM */
#define PAD_TRACE_MAX_WORDS   64u   /* CELL_PAD_MAX_CODES */

typedef enum PadTraceRole {
    PAD_TRACE_ROLE_RECORD = 0,
    PAD_TRACE_ROLE_REPLAY = 1,
} PadTraceRole;

/* One port's CellPadData as the guest reads it, in host order: `len` is the
 * packet length in bytes of a connected port's report (0 for a port with no
 * device), `words` the button[] array. Whether a given poll gets a fresh packet
 * (the guest sees len > 0) follows the host's report clock, which differs from
 * run to run, so it is not part of the state: replay decides it (see
 * pad_trace_poll). */
typedef struct PadTraceState {
    int32_t  len;
    uint16_t words[PAD_TRACE_MAX_WORDS];
} PadTraceState;

typedef struct PadTraceOptions {
    const char* record_path;      /* NULL or "" = not recording */
    const char* replay_path;      /* NULL or "" = not replaying */
    uint32_t  (*flip)(void);      /* the guest's flip-request count; required */
} PadTraceOptions;

/* Why a trace could not be opened or read. `reason` is a stable id: header,
 * version, syntax, unknown-key, duplicate-key, value, record-kind,
 * keys-backwards, port-range, after-end, truncated, exists, io. `line` is the
 * 1-based line of the replay file, 0 when no line applies. */
typedef struct PadTraceError {
    PadTraceRole role;
    uint32_t     line;
    char         reason[24];
    char         detail[192];
} PadTraceError;

typedef struct PadTrace PadTrace;

/* Open what `o` asks for. Returns 0 with *out NULL when neither path is set.
 * On failure returns -1 and fills *err; nothing is left open. */
int pad_trace_create(const PadTraceOptions* o, PadTrace** out, PadTraceError* err);

/* pad_trace_create, then the named stop on failure. NULL when neither path is
 * set. */
PadTrace* pad_trace_open(const PadTraceOptions* o);

/* The process's trace: opened by the first call, the same pointer afterwards
 * (a later call's options are ignored). Registers the exit hooks that close it
 * (atexit and at_quick_exit: a guest exit goes through quick_exit). NULL when
 * neither path is set. Stops on failure. */
PadTrace* pad_trace_global(const PadTraceOptions* o);

int pad_trace_replaying(const PadTrace* t);

/* One cellPadGetData of `port` (< PAD_TRACE_MAX_PORTS), at the current flip.
 *
 * Recording: `*st` is the state the guest is about to receive; it is recorded
 * if it differs from the port's last record.
 * Replaying: `*st` is replaced by the recorded state, or by `*neutral` when the
 * trace has none for this key (before the port's first record, or after the end
 * of the trace). The result is what a recording would see, so recording while
 * replaying writes the replayed state.
 *
 * `*fresh` (replay only, else set to 1) tells whether the guest gets a new
 * packet: the first poll of the port after a flip, or any poll whose state
 * differs from the previous one served. Later polls of an unchanged state are
 * len 0, as on the hardware, which is what ends a drain loop. */
void pad_trace_poll(PadTrace* t, uint32_t port, PadTraceState* st,
                    const PadTraceState* neutral, int* fresh);

/* One cellPadGetInfo/GetInfo2/PeriphGetInfo: which ports are connected, 1 or 0
 * per port. Recording: `on` is what the guest is told, written when it changes.
 * Replaying: `on` holds the default for a trace with no connection record yet,
 * and is replaced by the latest record at or before the current flip. */
void pad_trace_conn(PadTrace* t, uint8_t on[PAD_TRACE_MAX_PORTS]);

/* Push the written records to the file. */
void pad_trace_flush(PadTrace* t);

/* Finish: write the final poll counts and the end record, close the file, print
 * the replay summary. Idempotent. The object stays valid (replay keeps serving). */
void pad_trace_close(PadTrace* t);
void pad_trace_destroy(PadTrace* t);   /* close, then free */

/* Replay bookkeeping. `first_div_flip` is the first flip whose poll count for
 * some port differs from the recording, -1 when none has. */
typedef struct PadTraceStats {
    uint64_t polls;            /* replayed polls */
    uint64_t exact;            /* polls whose (flip, poll) has a record of its own */
    uint64_t held;             /* polls answered from an earlier record or neutral */
    uint64_t records;          /* state records in the file */
    uint64_t records_reached;  /* of those, the ones a poll hit exactly */
    int64_t  first_div_flip;
    uint32_t end_flip;         /* last flip the recording covers */
    int      ended;            /* a poll has gone past it */
} PadTraceStats;

void pad_trace_stats(PadTrace* t, PadTraceStats* out);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_PAD_TRACE_H */

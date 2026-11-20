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
 * PAD_AUTOPRESS, keyboard) have been applied: `len` and the words the guest
 * reads. The key is the port's 0-based GetData ordinal, so poll n of a replay
 * is handed exactly what the recording's poll n was handed.
 *
 * Nothing else can be the key. The flip clock moves between runs: a title's
 * step loop runs one to three steps per tick to catch up wall time, so how many
 * polls happen inside one guest flip depends on the host, and a (flip, poll)
 * key (version 2) turned a one-step press recorded at (F,0)->(F+1,0) into a
 * two-step press whenever a replay took a catch-up tick where the recording did
 * not. Version 1 keyed on the RSX walker's flip count, one step further from
 * the guest. Both are refused: neither key can be re-derived, so a run has to
 * be recorded again.
 *
 * The ordinal key makes the recorded input stream exact in polls; it cannot
 * make the guest deterministic in time. A title whose logic is gated on wall
 * time, or that waits on an asynchronous load, still drifts by a few steps, and
 * the flip at each record is kept as an annotation so the replay summary can
 * report it: `flip_drift` is the min and max of (served flip - recorded flip)
 * over the records a poll landed on, i.e. the guest-time drift the key absorbs.
 *
 * Only a change is written (the first poll of a port, then whenever len or the
 * words differ). `len` is part of the state, so a draining title writes about
 * two records per fresh packet -- the packet (len > 0) and the poll that read
 * len 0 -- and its exact len sequence replays. The connection the
 * cellPadGetInfo/GetInfo2/PeriphGetInfo calls report is keyed the same way, on
 * the info-call ordinal.
 *
 * Replay is state-based: poll n of a port is answered with the latest record of
 * that port whose seq is <= n. The host pad and the injectors play no part in
 * what the guest sees. The end record holds each port's poll count; a poll
 * whose ordinal reaches its own port's count is past the recording and the pad
 * is neutral. A killed run has no end record, keeps serving the last record of
 * each port, and says so once at replay start.
 *
 * The module is self-contained: it depends on stop.h and the C library. The
 * flip clock comes in through PadTraceOptions, and cellPad.c does the config
 * reading and the hooks. The flip count is annotation only: it is read for the
 * record and for the drift report.
 *
 * Failure policy: a trace that cannot be written, or a replay file that is not
 * a well-formed version-3 trace, ends the run with the named stop `pad-trace`
 * (role, line, reason); nothing is guessed or skipped.
 *
 * Threading: any guest thread may poll. One mutex per PadTrace guards every
 * field, so the per-port ordinals are handed out in one order and the keys in
 * the file never repeat or go backwards.
 */

#ifndef PS3EMU_PAD_TRACE_H
#define PS3EMU_PAD_TRACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAD_TRACE_FORMAT      "ps3recomp-pad-trace"
#define PAD_TRACE_VERSION     3u
/* What the `seq` key counts. Version 2 named a clock in this field; version 3
 * keys on a count, so the header says which count, and a file recorded under
 * another key cannot be mistaken for this one. */
#define PAD_TRACE_CLOCK      "guest-polls"
#define PAD_TRACE_MAX_PORTS   7u    /* CELL_PAD_MAX_PORT_NUM */
#define PAD_TRACE_MAX_WORDS   64u   /* CELL_PAD_MAX_CODES */

typedef enum PadTraceRole {
    PAD_TRACE_ROLE_RECORD = 0,
    PAD_TRACE_ROLE_REPLAY = 1,
} PadTraceRole;

/* One port's CellPadData as the guest reads it, in host order: `len` is the
 * guest-visible len of that poll (0 when the host had no new report, which is
 * what ends a drain loop) and `words` the button[] array. This is the whole
 * state: the replay hands it back exactly, so a poll sequence replays its own
 * len sequence. */
typedef struct PadTraceState {
    int32_t  len;
    uint16_t words[PAD_TRACE_MAX_WORDS];
} PadTraceState;

typedef struct PadTraceOptions {
    const char* record_path;      /* NULL or "" = not recording */
    const char* replay_path;      /* NULL or "" = not replaying */
    uint32_t  (*flip)(void);      /* cellGcm_guest_flip_count; for the annotation and the drift */
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

/* One cellPadGetData of `port` (< PAD_TRACE_MAX_PORTS), at the port's next
 * ordinal.
 *
 * Recording: `*st` is what the guest is about to receive and is written if it
 * differs from the port's last record (len or words).
 * Replaying: `*st` is replaced by the record of `port` with the largest seq <=
 * this call's ordinal, or by `*neutral` when there is none (before the port's
 * first record, or past the end record's poll count). The result is what a
 * recording would have served, so recording while replaying writes the
 * replayed state. */
void pad_trace_poll(PadTrace* t, uint32_t port, PadTraceState* st, const PadTraceState* neutral);

/* One cellPadGetInfo/GetInfo2/PeriphGetInfo, at the info call's ordinal: which
 * ports are connected, 1 or 0 per port. Recording: `on` is what the guest is
 * told, written when it changes. Replaying: `on` is replaced by the latest
 * record at or before this call's ordinal (the default when there is none). */
void pad_trace_conn(PadTrace* t, uint8_t on[PAD_TRACE_MAX_PORTS]);

/* Push the written records to the file. */
void pad_trace_flush(PadTrace* t);

/* Finish: write the end record and close the file, print the replay summary.
 * Idempotent. The object stays valid (replay keeps serving). */
void pad_trace_close(PadTrace* t);
void pad_trace_destroy(PadTrace* t);   /* close, then free */

/* Replay bookkeeping. `reached_exactly` counts the records whose seq a poll
 * landed on; `flip_drift` is the smallest and largest (served flip - recorded
 * flip) over those records, the guest-time drift the ordinal key absorbs and
 * cannot remove. `end_seq` is the end record's per-port poll counts; a killed
 * run has no end record (`have_end` 0) and every entry stays 0. */
typedef struct PadTraceStats {
    uint64_t polls;            /* replayed polls */
    uint64_t exact;            /* polls whose ordinal has a record of its own */
    uint64_t held;             /* polls answered from an earlier record or neutral */
    uint64_t records;          /* state records in the file */
    uint64_t records_reached;  /* of those, the ones a poll landed on */
    int      have_drift;       /* a record was reached, so the drift is known */
    int64_t  drift_min;
    int64_t  drift_max;
    int      have_end;         /* the file has an end record */
    uint64_t end_seq[PAD_TRACE_MAX_PORTS];   /* each port's poll count */
    int      ended;            /* a poll has gone past its port's count */
} PadTraceStats;

void pad_trace_stats(PadTrace* t, PadTraceStats* out);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_PAD_TRACE_H */

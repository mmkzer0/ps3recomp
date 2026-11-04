/* ring110.h - shared engine of the #110 probe (Layer 1 and Layer 2).
 *
 * One port, one polling thread, RAM logging, CSV after the run (PROBE.md §3, §4).
 * Layer 1 plugs in sentinel blocks and refills; Layer 2 plugs in a marker
 * trial scheduler through the hooks below.
 */
#ifndef R110_H
#define R110_H

#include <stdio.h>
#include <ppu-types.h>
#include <sys/event_queue.h>
#include <audio/audio.h>

#include "hwprobe.h"

/* PROBE.md §0: exported by libaudio.sprx (exports.h) but not declared in audio.h. */
s32 audioGetPortBlockTag(u32 port, u64 block, u64 *tag);
s32 audioGetPortTimestamp(u32 port, u64 tag, u64 *stamp);

#define R110_MAX_BLOCKS 32
#define R110_MAX_CH     8

/* EV record kinds */
#define EVK_IDX     0 /* index changed to idx (hi = upper 32 bits of the u64, expected 0) */
#define EVK_SENT    1 /* watched word of block blk changed: a=word, b=word+1, c=last word of block */
#define EVK_REFILL  2 /* guest rewrote sentinel block blk (Layer 1) */
#define EVK_GAP     3 /* polling gap: a = gap in timebase ticks (low 32 bit), blk = 0 */
#define EVK_GWRITE  4 /* guest marker write finished (Layer 2): blk, a = code id, b = 0 ref / 1 test */
#define EVK_GZERO   5 /* guest zeroed block blk after a trial (Layer 2) */

typedef struct {
	u64 tb;
	u32 idx;   /* index value at the time of the record */
	u8  kind;
	u8  blk;
	u8  hi;    /* upper half of the u64 index was nonzero */
	u8  pad;
	u32 a, b, c;
	u32 zmask; /* bit b set while the watched word of block b is zero */
} r110_ev_t;   /* 32 bytes */

typedef struct {
	u32 tick;
	u32 idx;
	u64 tb0, tb1, tb2, tb3;
	s64 st_a, st_b;
	u64 tag[3];
	u64 ts[3];
	s32 rc_tag[3];
	s32 rc_ts[3];
} r110_q4_t;

typedef struct {
	u64 tb;
	u64 source, d1, d2, d3;
	u32 idx_at_wake;
	s32 rc;
} r110_qe_t;

/* Timestamp request for the tag the current block had m ticks ago (see scan_ts_window) */
typedef struct {
	u32 tick;  /* ordinal of the Q4 row's index change */
	u32 m;
	u64 tb;
	u64 tag;
	u64 ts;
	s32 rc;
} r110_qt_t;

typedef struct {
	u64 tb_a;
	s64 st;
	u64 tb_b;
} r110_sync_t;

/* Runtime options. Layer defaults are filled by the layer, then ring110.cfg overrides. */
typedef struct {
	int  blocks;      /* 8 | 16 | 32 */
	int  channels;    /* 2 | 8 */
	int  ticks;       /* measured index changes after the discard window */
	int  discard;     /* index changes dropped at the start (analysis side) */
	int  q4_every;    /* Q4 calls on every N-th index change, 0 = never */
	int  ev_thread;   /* audio notify event queue + consumer thread */
	int  refill;      /* Layer 1: refill consumed sentinel blocks */
	int  readmode;    /* 0 plain ld, 1 dcbf+sync before each index read */
	int  prio;        /* polling thread priority, -1 = try 0,1,2,10,100,500 */
	int  max_seconds; /* hard stop */
	u32  seed;
	char tag[24];
	char outdir[64];

	/* Layer 2 only */
	char preset[16];
	int  jset[8];
	int  njset;
	double omin_ms, omax_ms, ostep_ms;
	int  reps;
	int  ref_only;
	int  quiet_laps;
	int  c2;          /* control C2: markers before audioPortStart, no trials */
	int  c2_ticks;

	/* hwprobe screen and console declaration (README.md "Result header") */
	int  screen;      /* 1 = on-screen status, 0 = leave video untouched */
	int  pad;         /* 1 = wait for START before the run and before exit, 0 = run unattended */
	int  hold_s;      /* pad = 0: keep the final screen this many seconds (START ends it early) */
	char fw_version[24];
	char fw_type[24];
	char console_model[24];
} r110_cfg_t;

typedef struct r110_ctx r110_ctx_t;

typedef struct {
	const char *layer; /* "L1" | "L2" */
	/* before the polling thread starts (fill blocks) */
	void (*on_prepare)(r110_ctx_t *c);
	/* polling thread, index changed (after the IDX event was logged) */
	void (*on_index)(r110_ctx_t *c, u64 tb, u32 r);
	/* polling thread, c->deadline reached (c->deadline is cleared before the call) */
	void (*on_deadline)(r110_ctx_t *c, u64 tb);
	/* extra CSV rows (TRIAL...) */
	void (*on_dump)(r110_ctx_t *c, FILE *f);
	/* extra META rows */
	void (*on_meta)(r110_ctx_t *c, FILE *f);
} r110_hooks_t;

struct r110_ctx {
	r110_cfg_t cfg;
	const r110_hooks_t *hk;

	/* port */
	u32 port;
	audioPortConfig pc;
	volatile u64   *ri;
	volatile u32   *base;     /* audio data as raw words */
	u32 bw;                   /* words per block = 256 * channels */
	u32 n;                    /* blocks */
	u32 nch;
	u32 watch;                /* word offset inside a block that is polled */
	int sentinel_mode;        /* Layer 1: refill logic active */
	u32 sent[R110_MAX_BLOCKS];
	u32 last[R110_MAX_BLOCKS];
	u32 dirty;                /* blocks whose last word differs from sent[] (sentinel mode) */
	u32 dc;                   /* Layer 1: DC word that fills the rest of a block */

	/* timebase */
	u64 tbfreq;
	u64 gap_ticks;            /* log polling gaps above this (20 us) */
	u64 period_ticks;         /* 256/48000 s in timebase ticks */

	/* polling state, read by hooks */
	u32 cur_idx;
	u32 zmask;                /* bit b set while the watched word of block b is zero */
	u8  cur_hi;
	int run;                  /* run id, valid when CSV rows are written */
	u32 nchg;                 /* index changes seen since polling began */
	u64 last_chg_tb;
	u64 deadline;             /* 0 = none */
	volatile int done;
	volatile int stop;        /* set by main (sysutil exit) */

	/* start phase (main thread) */
	u64 idx_before_start;
	u64 start_tb0, start_tb1;
	u64 idx_after_start;
	volatile int poll_ready;
	volatile int poll_finished;
	int poll_prio_used;
	int ev_prio_used;
	u64 max_gap;
	u32 abort_reason;         /* 0 normal, 1 timeout, 2 no index change, 3 stop */

	/* logs */
	r110_ev_t *ev;
	u32 nev, ev_cap, ev_drop;
	r110_q4_t *q4;
	u32 nq4, q4_cap;
	r110_qt_t *qt;
	u32 nqt, qt_cap;
	r110_qe_t *qe;
	volatile u32 nqe;
	u32 qe_cap;
	r110_sync_t sync[128];
	u32 nsync;

	/* event queue */
	sys_event_queue_t evq;
	sys_ipc_key_t evkey;
	int evq_ok;
	volatile int ev_stop;
};

/* helpers shared with the layers (timebase and sync: hwprobe.h) */
static inline volatile u32 *r110_block(r110_ctx_t *c, u32 b)
{
	return c->base + (u64)b * c->bw;
}

void r110_log(r110_ctx_t *c, u64 tb, u8 kind, u8 blk, u32 a, u32 b, u32 cc);

/* defaults common to both layers; layer fills the rest */
void r110_cfg_defaults(r110_cfg_t *cfg);
/* read key=value overrides from /dev_usb000/ring110.cfg or /dev_hdd0/tmp/hwprobe/ring110.cfg */
int  r110_cfg_load(r110_cfg_t *cfg);

/* run one complete probe: init, port, start, poll, stop, write CSV. Returns 0 on success. */
int  r110_main_run(const r110_cfg_t *cfg, const r110_hooks_t *hk);

#endif

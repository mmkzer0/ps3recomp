/* ring110.c - shared engine of the ring110 probe. See ring110.h and ../FORMAT.md. */
#include <stdlib.h>
#include <string.h>
#include <ppu-types.h>
#include <sys/thread.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>

#include "ring110.h"

#define R110_PROBE_VERSION "1"
#define R110_VERSION       "ring110-probe " R110_PROBE_VERSION " (PSL1GHT 7e1bd963)"
#define EV_CAP       (1u << 19) /* 524288 records, 16 MiB */
#define Q4_CAP       4096
#define QT_CAP       (1u << 16) /* timestamp window scan rows, 17 per Q4 tick */
#define QT_MAX_AGE   16         /* m = 3..16 are accepted on the console of the smoke runs, see scan_ts_window */
#define QE_CAP       8192

/* ------------------------------------------------------------------ config */

void r110_cfg_defaults(r110_cfg_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->blocks = AUDIO_BLOCK_8;
	cfg->channels = AUDIO_PORT_2CH;
	cfg->ticks = 2000;
	cfg->discard = 100;
	cfg->q4_every = 4;
	cfg->ev_thread = 1;
	cfg->refill = 1;
	cfg->readmode = 0;
	cfg->prio = -1;
	cfg->max_seconds = 60;
	cfg->seed = 1;
	strcpy(cfg->outdir, "/dev_hdd0/tmp/hwprobe");
	cfg->screen = 1;
	cfg->pad = 1;
}

static void cfg_set(r110_cfg_t *cfg, const char *k, const char *v)
{
	if (!strcmp(k, "blocks")) cfg->blocks = atoi(v);
	else if (!strcmp(k, "channels")) cfg->channels = atoi(v);
	else if (!strcmp(k, "ticks")) cfg->ticks = atoi(v);
	else if (!strcmp(k, "discard")) cfg->discard = atoi(v);
	else if (!strcmp(k, "q4_every")) cfg->q4_every = atoi(v);
	else if (!strcmp(k, "ev_thread")) cfg->ev_thread = atoi(v);
	else if (!strcmp(k, "refill")) cfg->refill = atoi(v);
	else if (!strcmp(k, "readmode")) cfg->readmode = atoi(v);
	else if (!strcmp(k, "prio")) cfg->prio = atoi(v);
	else if (!strcmp(k, "max_seconds")) cfg->max_seconds = atoi(v);
	else if (!strcmp(k, "seed")) cfg->seed = (u32)strtoul(v, NULL, 0);
	else if (!strcmp(k, "tag")) { strncpy(cfg->tag, v, sizeof(cfg->tag) - 1); }
	else if (!strcmp(k, "outdir")) { strncpy(cfg->outdir, v, sizeof(cfg->outdir) - 1); }
	else if (!strcmp(k, "preset")) { strncpy(cfg->preset, v, sizeof(cfg->preset) - 1); }
	else if (!strcmp(k, "jset")) {
		char buf[64];
		char *p, *save = NULL;
		strncpy(buf, v, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = 0;
		cfg->njset = 0;
		for (p = strtok_r(buf, ",", &save); p && cfg->njset < 8; p = strtok_r(NULL, ",", &save))
			cfg->jset[cfg->njset++] = atoi(p);
	}
	else if (!strcmp(k, "omin_ms")) cfg->omin_ms = atof(v);
	else if (!strcmp(k, "omax_ms")) cfg->omax_ms = atof(v);
	else if (!strcmp(k, "ostep_ms")) cfg->ostep_ms = atof(v);
	else if (!strcmp(k, "reps")) cfg->reps = atoi(v);
	else if (!strcmp(k, "ref_only")) cfg->ref_only = atoi(v);
	else if (!strcmp(k, "quiet_laps")) cfg->quiet_laps = atoi(v);
	else if (!strcmp(k, "c2")) cfg->c2 = atoi(v);
	else if (!strcmp(k, "c2_ticks")) cfg->c2_ticks = atoi(v);
	else if (!strcmp(k, "screen")) cfg->screen = atoi(v);
	else if (!strcmp(k, "pad")) cfg->pad = atoi(v);
	else if (!strcmp(k, "hold_s")) cfg->hold_s = atoi(v);
	else if (!strcmp(k, "fw_version")) { strncpy(cfg->fw_version, v, sizeof(cfg->fw_version) - 1); }
	else if (!strcmp(k, "fw_type")) { strncpy(cfg->fw_type, v, sizeof(cfg->fw_type) - 1); }
	else if (!strcmp(k, "console_model")) { strncpy(cfg->console_model, v, sizeof(cfg->console_model) - 1); }
	else printf("ring110.cfg: unknown key '%s'\n", k);
}

int r110_cfg_load(r110_cfg_t *cfg)
{
	static const char *paths[] = { "/dev_usb000/ring110.cfg", "/dev_hdd0/tmp/hwprobe/ring110.cfg" };
	char line[128];
	FILE *f = NULL;
	unsigned i;
	for (i = 0; i < sizeof(paths) / sizeof(paths[0]) && !f; i++)
		f = fopen(paths[i], "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		char *eq, *e;
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;
		eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq++ = 0;
		for (e = eq + strlen(eq); e > eq && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); e--)
			e[-1] = 0;
		cfg_set(cfg, line, eq);
	}
	fclose(f);
	return 1;
}

/* ------------------------------------------------------------------ logging */

void r110_log(r110_ctx_t *c, u64 tb, u8 kind, u8 blk, u32 a, u32 b, u32 cc)
{
	r110_ev_t *e;
	if (c->nev >= c->ev_cap) {
		c->ev_drop++;
		return;
	}
	e = &c->ev[c->nev++];
	e->tb = tb;
	e->idx = c->cur_idx;
	e->kind = kind;
	e->blk = blk;
	e->hi = c->cur_hi;
	e->pad = 0;
	e->a = a;
	e->b = b;
	e->c = cc;
	e->zmask = c->zmask;
}

/* ------------------------------------------------------------------ helpers */

static void take_sync(r110_ctx_t *c)
{
	r110_sync_t *s;
	if (c->nsync >= sizeof(c->sync) / sizeof(c->sync[0]))
		return;
	s = &c->sync[c->nsync++];
	s->tb_a = hwp_mftb();
	s->st = sysGetSystemTime();
	s->tb_b = hwp_mftb();
}

/* cellAudioGetPortTimestamp does not accept the tag of a block that has not been played yet. libaudio
 * (firmware reading, 0x1AD4) rejects with TAG_NOT_FOUND (0x8031070f) a tag above the port's current tag,
 * one more than 14 block steps below it, and any tag while the port's tag reference is still zero. The
 * tags Q4 gets for blocks r-1, r and r+1 are the tag of the block at the index and of blocks that play
 * later, so asking for their timestamps fails by construction (first smoke run: every call, rc 0x8031070f).
 *
 * So after the three-block read, ask for the timestamp of tags that were the current block's m ticks
 * ago: tag(r) - m * step, step = tag(r+1) - tag(r) = 1 << shift (2 on the console of the smoke runs).
 * m = 0 is the tag of block r itself. The rows (QT) show which m the firmware accepts and what time
 * it reports for them: second smoke run, m = 0..2 refused, m = 3..16 accepted (the port's current tag
 * trails tag(r) by three steps; the firmware reading puts the old edge at m = 17, not reached by the
 * scan). Called after the Q4 bracket so it does not stretch tb1..tb2. */
static void scan_ts_window(r110_ctx_t *c, const r110_q4_t *q)
{
	u64 step, m;
	if (q->rc_tag[1] || q->rc_tag[2] || q->tag[2] <= q->tag[1])
		return;
	step = q->tag[2] - q->tag[1];
	for (m = 0; m <= QT_MAX_AGE && c->nqt < c->qt_cap && m * step < q->tag[1]; m++) {
		r110_qt_t *t = &c->qt[c->nqt++];
		t->tick = q->tick;
		t->m = (u32)m;
		t->tag = q->tag[1] - m * step;
		t->ts = 0;
		t->tb = hwp_mftb();
		t->rc = audioGetPortTimestamp(c->port, t->tag, &t->ts);
	}
}

static void do_q4(r110_ctx_t *c, u32 r)
{
	r110_q4_t *q;
	int k;
	if (c->nq4 >= c->q4_cap)
		return;
	q = &c->q4[c->nq4++];
	q->tick = c->nchg;
	q->idx = r;
	q->tb0 = hwp_mftb();
	q->st_a = sysGetSystemTime();
	q->tb1 = hwp_mftb();
	for (k = 0; k < 3; k++) {
		u32 b = (r % c->n + c->n + k - 1) % c->n;
		q->tag[k] = 0;
		q->ts[k] = 0;
		q->rc_tag[k] = audioGetPortBlockTag(c->port, b, &q->tag[k]);
		q->rc_ts[k] = audioGetPortTimestamp(c->port, q->tag[k], &q->ts[k]);
	}
	q->tb2 = hwp_mftb();
	q->st_b = sysGetSystemTime();
	q->tb3 = hwp_mftb();
	scan_ts_window(c, q);
}

/* ------------------------------------------------------------------ polling thread */

static void poll_thread(void *arg)
{
	r110_ctx_t *c = (r110_ctx_t *)arg;
	volatile u64 *const ri = c->ri;
	volatile u32 *const base = c->base;
	const u32 n = c->n, bw = c->bw, watch = c->watch;
	const int readmode = c->cfg.readmode;
	const u64 gap_ticks = c->gap_ticks;
	const u64 tbfreq = c->tbfreq;
	const u32 q4_every = (u32)c->cfg.q4_every;
	const u32 stop_after = c->cfg.ticks > 0 ? (u32)(c->cfg.ticks + c->cfg.discard) : 0;
	u64 prev_tb = 0, lastv, last_sync_tb, start_tb, it = 0;
	u32 b;

	c->zmask = 0;
	for (b = 0; b < n; b++) {
		c->last[b] = base[(u64)b * bw + watch];
		if (c->last[b] == 0)
			c->zmask |= 1u << b;
		if (c->sentinel_mode && c->last[b] != c->sent[b])
			c->dirty |= 1u << b;
	}
	lastv = *ri;
	c->cur_idx = (u32)lastv;
	start_tb = last_sync_tb = hwp_mftb();
	take_sync(c);
	c->poll_ready = 1;

	for (;;) {
		u64 tb = hwp_mftb();
		u64 v;
		volatile u32 *p;
		int idx_changed = 0;

		if (readmode)
			__asm__ volatile("dcbf 0,%0; sync" : : "r"(ri) : "memory");
		v = *ri;

		if (prev_tb) {
			u64 gap = tb - prev_tb;
			if (gap > c->max_gap)
				c->max_gap = gap;
			if (gap > gap_ticks)
				r110_log(c, tb, EVK_GAP, 0, (u32)gap, 0, 0);
		}
		prev_tb = tb;

		if (v != lastv) {
			lastv = v;
			c->cur_idx = (u32)v;
			c->nchg++;
			c->last_chg_tb = tb;
			if ((u32)(v >> 32))
				c->cur_hi = 1;
			r110_log(c, tb, EVK_IDX, 0, 0, 0, 0);
			c->cur_hi = 0;
			idx_changed = 1;
		}

		p = base + watch;
		for (b = 0; b < n; b++, p += bw) {
			u32 w = *p;
			if (w != c->last[b]) {
				c->last[b] = w;
				if (w == 0)
					c->zmask |= 1u << b;
				else
					c->zmask &= ~(1u << b);
				if (c->sentinel_mode) {
					if (w != c->sent[b])
						c->dirty |= 1u << b;
					else
						c->dirty &= ~(1u << b);
				}
				r110_log(c, tb, EVK_SENT, (u8)b, w, p[1], p[bw - 1 - watch]);
			}
		}

		if (c->dirty && c->cfg.refill) {
			u32 r = c->cur_idx % n;
			u32 m = c->dirty;
			for (b = 0; m; b++, m >>= 1) {
				u32 d, i;
				volatile u32 *q;
				if (!(m & 1))
					continue;
				d = (b + n - r) % n;
				if (d < 3 || d > n - 3)
					continue;
				q = r110_block(c, b);
				q[0] = c->sent[b];
				for (i = 1; i < bw; i++)
					q[i] = c->dc;
				hwp_sync();
				c->last[b] = c->sent[b];
				c->dirty &= ~(1u << b);
				c->zmask &= ~(1u << b);
				r110_log(c, hwp_mftb(), EVK_REFILL, (u8)b, c->sent[b], 0, 0);
			}
		}

		if (idx_changed) {
			if (c->hk->on_index)
				c->hk->on_index(c, tb, c->cur_idx);
			if (q4_every && (c->nchg % q4_every) == 0)
				do_q4(c, c->cur_idx);
			if (tb - last_sync_tb > 10 * tbfreq) {
				take_sync(c);
				last_sync_tb = tb;
			}
		}

		if (c->deadline && tb >= c->deadline) {
			c->deadline = 0;
			if (c->hk->on_deadline)
				c->hk->on_deadline(c, tb);
		}

		if (c->done)
			break;
		if (stop_after && c->nchg >= stop_after)
			break;
		if ((++it & 0xFF) == 0) {
			u64 ref;
			if (c->stop) {
				if (!c->abort_reason)
					c->abort_reason = 3;
				break;
			}
			if ((tb - start_tb) > (u64)c->cfg.max_seconds * tbfreq) {
				c->abort_reason = 1;
				break;
			}
			ref = c->nchg ? c->last_chg_tb : c->start_tb1;
			if (ref && (tb - ref) > 3 * tbfreq) {
				c->abort_reason = 2;
				break;
			}
		}
	}

	take_sync(c);
	c->poll_finished = 1;
	sysThreadExit(0);
}

/* ------------------------------------------------------------------ event queue thread */

static void ev_thread(void *arg)
{
	r110_ctx_t *c = (r110_ctx_t *)arg;
	sys_event_t e;
	while (!c->ev_stop) {
		s32 rc = sysEventQueueReceive(c->evq, &e, 200000);
		u64 tb = hwp_mftb();
		if (rc == 0 && c->nqe < c->qe_cap) {
			r110_qe_t *q = &c->qe[c->nqe];
			q->tb = tb;
			q->source = e.source;
			q->d1 = e.data_1;
			q->d2 = e.data_2;
			q->d3 = e.data_3;
			q->idx_at_wake = (u32)*c->ri;
			q->rc = rc;
			c->nqe = c->nqe + 1;
		}
	}
	sysThreadExit(0);
}

/* ------------------------------------------------------------------ CSV */

#define U(x) ((unsigned long long)(x))

static const char *config_name(const r110_cfg_t *cfg)
{
	return cfg->tag[0] ? cfg->tag : cfg->preset[0] ? cfg->preset : "default";
}

static int write_csv(r110_ctx_t *c, int run, const char *name, char *path, size_t cap)
{
	hwp_header_t h;
	u32 i;
	FILE *f = hwp_result_open(c->cfg.outdir, name, path, cap);
	if (!f)
		return -1;

	memset(&h, 0, sizeof(h));
	h.probe = "ring110";
	h.probe_version = R110_PROBE_VERSION;
	h.config = config_name(&c->cfg);
	h.fw_version = c->cfg.fw_version;
	h.fw_type = c->cfg.fw_type;
	h.console_model = c->cfg.console_model;
	h.tbfreq = c->tbfreq;
	hwp_write_header(f, &h);

	fprintf(f, "META,version,%s\n", R110_VERSION);
	fprintf(f, "META,build,%s %s\n", __DATE__, __TIME__);
	fprintf(f, "META,layer,%s\n", c->hk->layer);
	fprintf(f, "META,run,%d\n", run);
	fprintf(f, "META,tbfreq,%llu\n", U(c->tbfreq));
	fprintf(f, "META,blocks,%u\n", c->n);
	fprintf(f, "META,channels,%u\n", c->nch);
	fprintf(f, "META,pc_readIndex,0x%08x\n", c->pc.readIndex);
	fprintf(f, "META,pc_status,%u\n", c->pc.status);
	fprintf(f, "META,pc_channelCount,%llu\n", U(c->pc.channelCount));
	fprintf(f, "META,pc_numBlocks,%llu\n", U(c->pc.numBlocks));
	fprintf(f, "META,pc_portSize,%u\n", c->pc.portSize);
	fprintf(f, "META,pc_audioDataStart,0x%08x\n", c->pc.audioDataStart);
	fprintf(f, "META,idx_before_start,0x%016llx\n", U(c->idx_before_start));
	fprintf(f, "META,start_tb0,%llu\n", U(c->start_tb0));
	fprintf(f, "META,start_tb1,%llu\n", U(c->start_tb1));
	fprintf(f, "META,idx_after_start,0x%016llx\n", U(c->idx_after_start));
	fprintf(f, "META,poll_prio,%d\n", c->poll_prio_used);
	fprintf(f, "META,ev_prio,%d\n", c->ev_prio_used);
	fprintf(f, "META,max_gap_ticks,%llu\n", U(c->max_gap));
	fprintf(f, "META,gap_log_ticks,%llu\n", U(c->gap_ticks));
	fprintf(f, "META,abort_reason,%u\n", c->abort_reason);
	fprintf(f, "META,nchg,%u\n", c->nchg);
	fprintf(f, "META,ev_drop,%u\n", c->ev_drop);
	fprintf(f, "META,discard,%d\n", c->cfg.discard);
	fprintf(f, "META,ticks,%d\n", c->cfg.ticks);
	fprintf(f, "META,q4_every,%d\n", c->cfg.q4_every);
	fprintf(f, "META,ev_thread,%d\n", c->cfg.ev_thread);
	fprintf(f, "META,evq_ok,%d\n", c->evq_ok);
	fprintf(f, "META,refill,%d\n", c->cfg.refill);
	fprintf(f, "META,readmode,%d\n", c->cfg.readmode);
	fprintf(f, "META,seed,%u\n", c->cfg.seed);
	fprintf(f, "META,tag,%s\n", c->cfg.tag);
	fprintf(f, "META,watch_word,%u\n", c->watch);
	fprintf(f, "META,sentinel_mode,%d\n", c->sentinel_mode);
	hwp_ui_write_meta(f);
	if (c->hk->on_meta)
		c->hk->on_meta(c, f);
	for (i = 0; i < c->n; i++) {
		if (c->sentinel_mode)
			fprintf(f, "SENTWORD,%u,0x%08x\n", i, c->sent[i]);
		fprintf(f, "FINALWORD,%u,0x%08x\n", i, c->last[i]);
	}

	for (i = 0; i < c->nsync; i++)
		fprintf(f, "SYNC,%llu,%lld,%llu\n", U(c->sync[i].tb_a), (long long)c->sync[i].st, U(c->sync[i].tb_b));

	for (i = 0; i < c->nev; i++) {
		const r110_ev_t *e = &c->ev[i];
		fprintf(f, "EV,%llu,%u,%u,%u,%u,0x%08x,0x%08x,0x%08x,0x%08x\n", U(e->tb), e->kind, e->idx, e->hi,
		        e->blk, e->a, e->b, e->c, e->zmask);
	}

	for (i = 0; i < c->nq4; i++) {
		const r110_q4_t *q = &c->q4[i];
		fprintf(f,
		        "Q4,%u,%u,%llu,%lld,%llu,%llu,%lld,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d,%d,%d,%d,%d,%d\n",
		        q->tick, q->idx, U(q->tb0), (long long)q->st_a, U(q->tb1), U(q->tb2), (long long)q->st_b,
		        U(q->tb3), U(q->tag[0]), U(q->tag[1]), U(q->tag[2]), U(q->ts[0]), U(q->ts[1]), U(q->ts[2]),
		        q->rc_tag[0], q->rc_tag[1], q->rc_tag[2], q->rc_ts[0], q->rc_ts[1], q->rc_ts[2]);
	}

	for (i = 0; i < c->nqt; i++) {
		const r110_qt_t *t = &c->qt[i];
		fprintf(f, "QT,%u,%u,%llu,%llu,%llu,%d\n", t->tick, t->m, U(t->tb), U(t->tag), U(t->ts), t->rc);
	}

	for (i = 0; i < c->nqe; i++) {
		const r110_qe_t *q = &c->qe[i];
		fprintf(f, "QE,%llu,0x%llx,%llu,%llu,%llu,%u,%d\n", U(q->tb), U(q->source), U(q->d1), U(q->d2),
		        U(q->d3), q->idx_at_wake, q->rc);
	}

	if (c->hk->on_dump)
		c->hk->on_dump(c, f);

	fprintf(f, "END,%u,%u\n", c->nev, c->ev_drop);
	fclose(f);
	return 0;
}

/* ------------------------------------------------------------------ main flow */

static int create_thread(sys_ppu_thread_t *t, void (*fn)(void *), void *arg, int prio, const char *name, int *used)
{
	static const int fallback[] = { 0, 1, 2, 10, 100, 500, 1000 };
	unsigned i;
	s32 rc;
	if (prio >= 0) {
		rc = sysThreadCreate(t, fn, arg, prio, 64 * 1024, THREAD_JOINABLE, (char *)name);
		if (rc == 0) {
			*used = prio;
			return 0;
		}
		printf("sysThreadCreate(%s, prio %d) = 0x%08x\n", name, prio, (unsigned)rc);
		return rc;
	}
	for (i = 0; i < sizeof(fallback) / sizeof(fallback[0]); i++) {
		rc = sysThreadCreate(t, fn, arg, fallback[i], 64 * 1024, THREAD_JOINABLE, (char *)name);
		if (rc == 0) {
			*used = fallback[i];
			return 0;
		}
		printf("sysThreadCreate(%s, prio %d) = 0x%08x\n", name, fallback[i], (unsigned)rc);
	}
	return rc;
}

/* Puts an error on the screen and in <outdir>/ring110_last.txt (no result file exists yet, so an
 * operator without a screen can still see why); the final redraw and the wait for START happen
 * at `finish`. */
static int ui_fail(const r110_cfg_t *cfg, const char *msg, s32 rc)
{
	char line[160];
	snprintf(line, sizeof(line), "%s (0x%08x)", msg, (unsigned)rc);
	hwp_result_note_last(cfg->outdir, "ring110", "(no result file)", line);
	hwp_ui_set_phase("stopped");
	hwp_ui_set_error("%s", line);
	return 1;
}

int r110_main_run(const r110_cfg_t *cfg, const r110_hooks_t *hk)
{
	static r110_ctx_t ctx;
	r110_ctx_t *c = &ctx;
	audioPortParam pp;
	sys_ppu_thread_t t_poll = 0, t_ev = 0;
	u64 ret64;
	s32 rc;
	int run, rv = 1;
	char path[160], name[96], title[48];
	int ui_err = 0;

	memset(c, 0, sizeof(*c));
	c->cfg = *cfg;
	c->hk = hk;
	c->n = (u32)cfg->blocks;
	c->nch = (u32)cfg->channels;
	c->bw = AUDIO_BLOCK_SAMPLES * c->nch;
	c->tbfreq = hwp_tb_freq();
	c->gap_ticks = c->tbfreq / 50000; /* 20 us */
	c->period_ticks = (c->tbfreq * AUDIO_BLOCK_SAMPLES) / 48000;
	c->ev_cap = EV_CAP;
	c->q4_cap = Q4_CAP;
	c->qt_cap = QT_CAP;
	c->qe_cap = QE_CAP;
	c->ev = (r110_ev_t *)calloc(c->ev_cap, sizeof(r110_ev_t));
	c->q4 = (r110_q4_t *)calloc(c->q4_cap, sizeof(r110_q4_t));
	c->qt = (r110_qt_t *)calloc(c->qt_cap, sizeof(r110_qt_t));
	c->qe = (r110_qe_t *)calloc(c->qe_cap, sizeof(r110_qe_t));
	if (!c->ev || !c->q4 || !c->qt || !c->qe) {
		printf("log allocation failed\n");
		return 1;
	}
	if (c->n > R110_MAX_BLOCKS || c->nch > R110_MAX_CH) {
		printf("bad shape\n");
		return 1;
	}

	hwp_events_init();
	snprintf(title, sizeof(title), "ring110 %s probe", hk->layer);
	hwp_ui_init(title, cfg->screen);
	hwp_ui_set_config(config_name(cfg));
	hwp_ui_set_run(0, 1);
	hwp_ui_set_phase("ready");
	if (cfg->pad) {
		hwp_ui_set_hint("Press START to run. Leave the pad alone while it measures.");
		hwp_ui_redraw();
		if (hwp_wait_button(HWP_BTN_START, 0) == HWP_WAIT_EXIT) {
			hwp_ui_shutdown();
			return 1;
		}
		hwp_ui_set_hint("");
	}

	hwp_ui_set_phase("audio setup");
	hwp_ui_redraw();
	rc = audioInit();
	printf("audioInit: %08x\n", (unsigned)rc);
	if (rc) {
		ui_err = ui_fail(cfg, "audioInit failed", rc);
		goto finish;
	}

	memset(&pp, 0, sizeof(pp));
	pp.numChannels = c->nch;
	pp.numBlocks = c->n;
	pp.attrib = AUDIO_PORT_INITLEVEL;
	pp.level = 1.0f;
	rc = audioPortOpen(&pp, &c->port);
	printf("audioPortOpen: %08x port %u\n", (unsigned)rc, c->port);
	if (rc) {
		ui_err = ui_fail(cfg, "audioPortOpen failed", rc);
		goto quit;
	}

	memset(&c->pc, 0, sizeof(c->pc));
	rc = audioGetPortConfig(c->port, &c->pc);
	printf("audioGetPortConfig: %08x readIndex %08x status %u ch %llu blocks %llu size %u data %08x\n",
	       (unsigned)rc, c->pc.readIndex, c->pc.status, U(c->pc.channelCount), U(c->pc.numBlocks),
	       c->pc.portSize, c->pc.audioDataStart);
	if (rc || !c->pc.readIndex || !c->pc.audioDataStart) {
		ui_err = ui_fail(cfg, "audioGetPortConfig gave no read index or data address", rc);
		goto close;
	}
	c->ri = (volatile u64 *)(u64)c->pc.readIndex;
	c->base = (volatile u32 *)(u64)c->pc.audioDataStart;
	c->idx_before_start = *c->ri;

	if (cfg->ev_thread) {
		rc = audioCreateNotifyEventQueue(&c->evq, &c->evkey);
		if (rc == 0)
			rc = audioSetNotifyEventQueue(c->evkey);
		if (rc == 0) {
			sysEventQueueDrain(c->evq);
			c->evq_ok = 1;
		}
		printf("notify event queue: %08x\n", (unsigned)rc);
	}

	if (hk->on_prepare)
		hk->on_prepare(c);

	/* From here to the join of the polling thread nothing may draw (hwprobe.h, integrity rule):
	 * the frame below stays on screen for the whole measurement. */
	hwp_ui_set_phase("measuring (screen frozen)");
	hwp_ui_set_run(1, 1);
	hwp_ui_redraw();
	hwp_timed_begin();
	if (create_thread(&t_poll, poll_thread, c, cfg->prio, "r110_poll", &c->poll_prio_used)) {
		hwp_timed_end();
		printf("cannot create polling thread\n");
		c->abort_reason = 5;
		goto teardown;
	}
	while (!c->poll_ready)
		sysUsleep(1000);
	if (c->evq_ok) {
		/* one priority step below the polling thread, see SPEC-NOTES.md */
		if (create_thread(&t_ev, ev_thread, c, c->poll_prio_used + 1, "r110_ev", &c->ev_prio_used)) {
			printf("cannot create event thread, no QE rows\n");
			c->ev_prio_used = -1;
		}
	}

	/* start phase, main thread; the polling thread is already logging */
	c->start_tb0 = hwp_mftb();
	rc = audioPortStart(c->port);
	c->start_tb1 = hwp_mftb();
	c->idx_after_start = *c->ri;
	printf("audioPortStart: %08x\n", (unsigned)rc);
	if (rc) {
		c->stop = 1;
		c->abort_reason = 4;
	}

	while (!c->poll_finished) {
		hwp_poll_events();
		if (hwp_exit_requested())
			c->stop = 1;
		sysUsleep(50000);
	}
	sysThreadJoin(t_poll, &ret64);
	hwp_timed_end();
	audioPortStop(c->port);
	rv = 0;

teardown:
	c->ev_stop = 1;
	if (t_ev)
		sysThreadJoin(t_ev, &ret64);
	if (c->evq_ok) {
		audioRemoveNotifyEventQueue(c->evkey);
		sysEventQueueDestroy(c->evq, 0);
	}

	hwp_ui_set_phase("writing result");
	hwp_ui_redraw();
	run = hwp_next_run_id(cfg->outdir, "ring110");
	c->run = run;
	snprintf(name, sizeof(name), "ring110_%s_r%03d_n%uc%u%s%s.csv", hk->layer, run, c->n, c->nch,
	         cfg->tag[0] ? "_" : "", cfg->tag);
	if (write_csv(c, run, name, path, sizeof(path)) != 0) {
		rv = 1;
		ui_err = 1;
		hwp_ui_set_summary("nchg %u, abort_reason %u", c->nchg, c->abort_reason);
		hwp_ui_set_error("result file could not be written, see the console log");
	} else {
		char sum[128];
		snprintf(sum, sizeof(sum), "abort_reason=%u nchg=%u events=%u dropped=%u", c->abort_reason, c->nchg,
		         c->nev, c->ev_drop);
		printf("wrote %s (%u events, %u dropped, abort_reason %u)\n", path, c->nev, c->ev_drop, c->abort_reason);
		hwp_result_note_last(cfg->outdir, "ring110", path, sum);
		hwp_ui_set_summary("%s", sum);
		/* abort_reason 3 = the user quit, the data up to then is valid (FORMAT.md) */
		if (c->abort_reason && c->abort_reason != 3) {
			rv = 1;
			ui_err = 1;
			hwp_ui_set_error("run aborted, abort_reason %u, result: %s", c->abort_reason, path);
		} else {
			hwp_ui_set_done(path);
		}
	}
close:
	audioPortClose(c->port);
quit:
	audioQuit();
finish:
	if (!ui_err)
		hwp_ui_set_phase("finished");
	hwp_ui_set_hint(cfg->pad ? "Press START to exit" : cfg->hold_s > 0 ? "Exits by itself" : "");
	hwp_ui_redraw();
	if (cfg->pad)
		hwp_wait_button(HWP_BTN_START, 0);
	else if (cfg->hold_s > 0)
		hwp_wait_button(HWP_BTN_START, cfg->hold_s);
	hwp_ui_shutdown();
	return rv;
}

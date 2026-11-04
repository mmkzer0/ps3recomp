/* ring110 Layer 2 (PROBE.md §5): controlled-write sweep with audio markers.
 *
 * All blocks are exact silence except the markers. Per trial (state machine below, driven
 * by the polling thread of common/ring110.c):
 *   1. wait for the next index change to r (t_c), write the reference marker (code 0)
 *      into block (r + n/2) mod n;
 *   2. busy-wait to t_c + o, write the test marker (code = trial + 1) into block
 *      (r + j) mod n, log t_w;
 *   3. stay quiet for quiet_laps laps, then zero the marker blocks if the hardware did not;
 *   4. random 0..P extra delay, next trial.
 * Markers: 128 samples of +-0.5 (code pn.h), channels 0 and 1, starting at sample 32.
 * No printf, no file I/O while the polling loop runs.
 */
#include <string.h>
#include "../../common/ring110.h"
#include "../../common/pn.h"

#define MAX_TRIALS 4096
#define MARK_START 32
#define F_HALF     0x3F000000u /* +0.5f */
#define F_NHALF    0xBF000000u /* -0.5f */

enum { ST_WARM, ST_WAIT_IDX, ST_WAIT_TEST, ST_QUIET, ST_RAND };

typedef struct {
	s32 j;
	float o_ms;
	u8  kind; /* 0 test, 1 reference only */
	u8  done;
	u32 code; /* test code id, 0xFFFFFFFF if none */
	u32 idx_r, test_blk, ref_blk;
	u64 t_c, t_ref, t_w;
} trial_t;

static trial_t g_trial[MAX_TRIALS];
static u32 g_ntrial, g_cur;
static int g_state = ST_WARM;
static u32 g_rng;
static int8_t g_ref_signs[R110_PN_LEN];
static int8_t g_test_signs[R110_PN_LEN];
static u64 g_quiet_ticks;
static int g_c2;
static u32 g_ngrid_steps;

static u32 rnd(void)
{
	g_rng = r110_xorshift32(g_rng);
	return g_rng;
}

/* ---- markers -------------------------------------------------------- */

static void write_marker(r110_ctx_t *c, u32 blk, const int8_t *sg)
{
	volatile u32 *q = r110_block(c, blk) + (u64)MARK_START * c->nch;
	u32 s;
	for (s = 0; s < R110_PN_LEN; s++) {
		u32 v = sg[s] > 0 ? F_HALF : F_NHALF;
		q[0] = v;
		q[1] = v;
		q += c->nch;
	}
	hwp_sync();
	/* the watched word is sample 32 / channel 0: keep the poller from reporting our own write */
	c->last[blk] = sg[0] > 0 ? F_HALF : F_NHALF;
	c->zmask &= ~(1u << blk);
}

static void zero_block(r110_ctx_t *c, u32 blk)
{
	volatile u32 *q = r110_block(c, blk);
	u32 i;
	for (i = 0; i < c->bw; i++)
		q[i] = 0;
	hwp_sync();
	c->last[blk] = 0;
	c->zmask |= 1u << blk;
}

static void zero_marker_blocks(r110_ctx_t *c, u64 tb)
{
	u32 b;
	for (b = 0; b < c->n; b++) {
		if (c->last[b] != 0) {
			zero_block(c, b);
			r110_log(c, tb, EVK_GZERO, (u8)b, 0, 0, 0);
		}
	}
}

/* ---- trial plan ------------------------------------------------------ */

static void build_plan(const r110_cfg_t *cfg)
{
	int ji, k, rep;
	u32 i;
	int steps = 1;
	g_ntrial = 0;
	if (cfg->ostep_ms > 0 && cfg->omax_ms > cfg->omin_ms)
		steps = (int)((cfg->omax_ms - cfg->omin_ms) / cfg->ostep_ms + 1e-6) + 1;
	g_ngrid_steps = (u32)steps;
	for (ji = 0; ji < cfg->njset; ji++)
		for (k = 0; k < steps; k++)
			for (rep = 0; rep < cfg->reps; rep++) {
				if (g_ntrial >= MAX_TRIALS)
					break;
				g_trial[g_ntrial].j = cfg->jset[ji];
				g_trial[g_ntrial].o_ms = (float)(cfg->omin_ms + k * cfg->ostep_ms);
				g_trial[g_ntrial].kind = 0;
				g_ntrial++;
			}
	for (i = 0; i < (u32)cfg->ref_only && g_ntrial < MAX_TRIALS; i++) {
		g_trial[g_ntrial].j = 99;
		g_trial[g_ntrial].o_ms = -1.0f;
		g_trial[g_ntrial].kind = 1;
		g_ntrial++;
	}
	/* Fisher-Yates */
	for (i = g_ntrial; i > 1; i--) {
		u32 a = i - 1, b = rnd() % i;
		trial_t t = g_trial[a];
		g_trial[a] = g_trial[b];
		g_trial[b] = t;
	}
	for (i = 0; i < g_ntrial; i++) {
		g_trial[i].code = g_trial[i].kind ? 0xFFFFFFFFu : i + 1;
		g_trial[i].done = 0;
	}
}

/* ---- hooks ----------------------------------------------------------- */

static void l2_prepare(r110_ctx_t *c)
{
	u32 b;
	c->sentinel_mode = 0;
	c->watch = (u32)MARK_START * c->nch;
	c->dc = 0;
	for (b = 0; b < c->n; b++) {
		volatile u32 *q = r110_block(c, b);
		u32 i;
		for (i = 0; i < c->bw; i++)
			q[i] = 0;
	}
	hwp_sync();
	g_rng = c->cfg.seed ? c->cfg.seed : 1;
	r110_pn_signs(0, g_ref_signs);
	g_quiet_ticks = (u64)c->cfg.quiet_laps * c->n * c->period_ticks + c->period_ticks;
	g_c2 = c->cfg.c2;
	if (g_c2) {
		/* control C2: markers in blocks 0 and 1 before audioPortStart */
		r110_pn_signs(1001, g_test_signs);
		write_marker(c, 0, g_test_signs);
		r110_pn_signs(1002, g_test_signs);
		write_marker(c, 1, g_test_signs);
		g_ntrial = 0;
	} else {
		build_plan(&c->cfg);
	}
}

static void start_trial(r110_ctx_t *c, u64 tb, u32 r)
{
	trial_t *t = &g_trial[g_cur];
	u32 rm = r % c->n;
	t->t_c = tb;
	t->idx_r = r;
	t->ref_blk = (rm + c->n / 2) % c->n;
	write_marker(c, t->ref_blk, g_ref_signs);
	t->t_ref = hwp_mftb();
	r110_log(c, t->t_ref, EVK_GWRITE, (u8)t->ref_blk, 0, 0, 0);
	if (t->kind == 0) {
		t->test_blk = (u32)((s32)rm + t->j + (s32)c->n) % c->n;
		r110_pn_signs(t->code, g_test_signs);
		c->deadline = tb + (u64)((double)t->o_ms * 1e-3 * (double)c->tbfreq);
		g_state = ST_WAIT_TEST;
	} else {
		t->test_blk = 0xFFFFFFFFu;
		c->deadline = t->t_ref + g_quiet_ticks;
		g_state = ST_QUIET;
	}
}

static void l2_index(r110_ctx_t *c, u64 tb, u32 r)
{
	if (g_c2) {
		return;
	}
	if (g_state == ST_WARM && (int)c->nchg > c->cfg.discard) {
		if (g_ntrial == 0) {
			c->done = 1;
			return;
		}
		g_state = ST_WAIT_IDX;
	}
	if (g_state == ST_WAIT_IDX)
		start_trial(c, tb, r);
}

static void l2_deadline(r110_ctx_t *c, u64 tb)
{
	trial_t *t = &g_trial[g_cur];
	switch (g_state) {
	case ST_WAIT_TEST:
		write_marker(c, t->test_blk, g_test_signs);
		t->t_w = hwp_mftb();
		r110_log(c, t->t_w, EVK_GWRITE, (u8)t->test_blk, t->code, 1, 0);
		c->deadline = t->t_w + g_quiet_ticks;
		g_state = ST_QUIET;
		break;
	case ST_QUIET:
		zero_marker_blocks(c, tb);
		t->done = 1;
		g_cur++;
		if (g_cur >= g_ntrial) {
			c->done = 1;
			return;
		}
		c->deadline = tb + (u64)(rnd() % (u32)c->period_ticks) + 1;
		g_state = ST_RAND;
		break;
	case ST_RAND:
		g_state = ST_WAIT_IDX;
		break;
	default:
		break;
	}
}

static void l2_meta(r110_ctx_t *c, FILE *f)
{
	fprintf(f, "META,l2_ntrials,%u\n", g_ntrial);
	fprintf(f, "META,l2_grid_steps,%u\n", g_ngrid_steps);
	fprintf(f, "META,l2_njset,%d\n", c->cfg.njset);
	{
		int i;
		fprintf(f, "META,l2_jset,");
		for (i = 0; i < c->cfg.njset; i++)
			fprintf(f, "%s%d", i ? ";" : "", c->cfg.jset[i]);
		fprintf(f, "\n");
	}
	fprintf(f, "META,l2_omin_ms,%.4f\n", c->cfg.omin_ms);
	fprintf(f, "META,l2_omax_ms,%.4f\n", c->cfg.omax_ms);
	fprintf(f, "META,l2_ostep_ms,%.4f\n", c->cfg.ostep_ms);
	fprintf(f, "META,l2_reps,%d\n", c->cfg.reps);
	fprintf(f, "META,l2_ref_only,%d\n", c->cfg.ref_only);
	fprintf(f, "META,l2_quiet_laps,%d\n", c->cfg.quiet_laps);
	fprintf(f, "META,l2_preset,%s\n", c->cfg.preset);
	fprintf(f, "META,l2_c2,%d\n", c->cfg.c2);
	fprintf(f, "META,l2_c2_codes,1001;1002\n");
	fprintf(f, "META,l2_marker_start_sample,%d\n", MARK_START);
	fprintf(f, "META,l2_marker_len,%d\n", R110_PN_LEN);
	fprintf(f, "META,l2_marker_channels,0;1\n");
	fprintf(f, "META,l2_ref_code,0\n");
	fprintf(f, "META,l2_trials_done,%u\n", g_cur);
}

static void l2_dump(r110_ctx_t *c, FILE *f)
{
	u32 i;
	double tbf = (double)c->tbfreq;
	for (i = 0; i < g_ntrial; i++) {
		const trial_t *t = &g_trial[i];
		double oa = t->kind || !t->t_w ? -1.0 : (double)(t->t_w - t->t_c) * 1000.0 / tbf;
		fprintf(f, "TRIAL,%d,%u,%d,%.4f,%.4f,%llu,%llu,%u,%d,%d,%d,%llu,%u,%u\n", c->run, i, t->j,
		        (double)t->o_ms, oa, (unsigned long long)t->t_c, (unsigned long long)t->t_w, t->idx_r,
		        (int)t->code, (int)t->test_blk, (int)t->ref_blk, (unsigned long long)t->t_ref, t->kind,
		        t->done);
	}
}

static const r110_hooks_t HOOKS = {
	.layer = "L2",
	.on_prepare = l2_prepare,
	.on_index = l2_index,
	.on_deadline = l2_deadline,
	.on_dump = l2_dump,
	.on_meta = l2_meta,
};

/* ---- config / presets ------------------------------------------------- */

static void set_grid(r110_cfg_t *cfg, int blocks, int channels, const int *j, int nj, double omin, double omax,
                     double ostep, int reps, int ref_only)
{
	int i;
	cfg->blocks = blocks;
	cfg->channels = channels;
	cfg->njset = nj;
	for (i = 0; i < nj; i++)
		cfg->jset[i] = j[i];
	cfg->omin_ms = omin;
	cfg->omax_ms = omax;
	cfg->ostep_ms = ostep;
	cfg->reps = reps;
	cfg->ref_only = ref_only;
}

static void apply_preset(r110_cfg_t *cfg)
{
	static const int j4[] = { -1, 0, 1, 2 };
	static const int j2[] = { -1, 0 };
	static const int j0[] = { 0 };
	static const int j01[] = { 0, 1 };
	const char *p = cfg->preset;
	if (!strcmp(p, "main"))
		set_grid(cfg, 8, 2, j4, 4, 0.0, 12.0, 0.5, 10, 200);
	else if (!strcmp(p, "shape16"))
		set_grid(cfg, 16, 2, j2, 2, 0.0, 6.0, 0.5, 10, 50);
	else if (!strcmp(p, "shape8ch"))
		set_grid(cfg, 8, 8, j2, 2, 0.0, 6.0, 0.5, 10, 50);
	else if (!strcmp(p, "fine"))
		set_grid(cfg, 8, 2, j01, 2, 0.0, 0.0, 0.1, 10, 50); /* user sets omin_ms / omax_ms */
	else if (!strcmp(p, "c1")) /* o = 3P = 16 ms, j = 0, always a hit */
		set_grid(cfg, 8, 2, j0, 1, 16.0, 16.0, 1.0, 30, 10);
	else if (!strcmp(p, "c3")) /* late write o = 20 ms, j = 0 */
		set_grid(cfg, 8, 2, j0, 1, 20.0, 20.0, 1.0, 20, 10);
	else if (!strcmp(p, "c2")) {
		cfg->c2 = 1;
		cfg->ticks = cfg->c2_ticks;
	} else if (p[0])
		printf("unknown preset '%s'\n", p);
}

int main(int argc, char *argv[])
{
	r110_cfg_t cfg;
	(void)argc;
	(void)argv;
	r110_cfg_defaults(&cfg);
	cfg.q4_every = 0;
	cfg.ticks = 0;
	cfg.refill = 0;
	cfg.quiet_laps = 4;
	cfg.c2_ticks = 300;
	cfg.max_seconds = 900;
	strcpy(cfg.preset, "main");
	r110_cfg_load(&cfg); /* pass 1: pick the preset */
	apply_preset(&cfg);
	r110_cfg_load(&cfg); /* pass 2: explicit keys override the preset */
	if (cfg.c2)
		cfg.ticks = cfg.c2_ticks;
	return r110_main_run(&cfg, &HOOKS);
}

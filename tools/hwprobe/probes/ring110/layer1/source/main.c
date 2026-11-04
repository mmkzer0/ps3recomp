/* ring110 Layer 1 (PROBE.md §4): guest-only observations, no audio capture needed.
 *
 * Every block carries a sentinel (sample 0 / channel 0 = 0.001f * (b + 1), rest of the
 * block = DC 0.001f). The polling thread (common/ring110.c) logs every change of the
 * read index and of each sentinel word, refills consumed blocks far from the latch
 * (3 <= (b - r) mod n <= n - 3), and runs Q4 (tag/timestamp) and Q6 (notify event).
 */
#include <string.h>
#include "../../common/ring110.h"

static u32 f2u(float f)
{
	u32 u;
	memcpy(&u, &f, sizeof(u));
	return u;
}

static void l1_prepare(r110_ctx_t *c)
{
	u32 b, i;
	c->sentinel_mode = 1;
	c->watch = 0;
	c->dc = f2u(0.001f);
	for (b = 0; b < c->n; b++) {
		volatile u32 *q = r110_block(c, b);
		c->sent[b] = f2u(0.001f * (float)(b + 1));
		q[0] = c->sent[b];
		for (i = 1; i < c->bw; i++)
			q[i] = c->dc;
	}
	hwp_sync();
}

static const r110_hooks_t HOOKS = {
	.layer = "L1",
	.on_prepare = l1_prepare,
};

int main(int argc, char *argv[])
{
	r110_cfg_t cfg;
	(void)argc;
	(void)argv;
	r110_cfg_defaults(&cfg);
	r110_cfg_load(&cfg);
	return r110_main_run(&cfg, &HOOKS);
}

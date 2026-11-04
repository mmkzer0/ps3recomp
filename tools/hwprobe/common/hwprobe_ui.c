/* hwprobe_ui.c - on-screen status, pad and sysutil events. See hwprobe.h for the
 * measurement integrity rule that governs when this file may draw.
 *
 * The status text is drawn by the PPU straight into the display buffers (XRGB, linear) with
 * the PSL1GHT debug font glyphs (hwprobe_font.h), then flipped. The PSL1GHT debug font
 * renderer sample draws the same glyphs with an RSX vertex/fragment program, but those
 * programs need the Cg compiler (cgcomp loads NVIDIA's libCg), which does not exist for the
 * linux/arm64 build container, and a CPU blit has no RSX state to get wrong. The display
 * setup (rsxInit, video configure, two display buffers, flip protocol) follows PSL1GHT
 * samples/graphics/rsxtest/source/rsxutil.cpp.
 *
 * Copyright (c) 2011 PSL1GHT Development Team (parts adapted from the PSL1GHT samples,
 * MIT licence; the full notice is in hwprobe_font.h)
 */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <io/pad.h>
#include <rsx/rsx.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>

#include "hwprobe.h"
#include "hwprobe_font.h"

#define HOST_BUFFER_SIZE (32 * 1024 * 1024) /* IO-mapped host memory for rsxInit; blitting/cairo samples use it */
#define CMD_BUFFER_SIZE  0x100000
#define HOST_ALIGN       (1024 * 1024)
#define LABEL_INDEX      255
#define NUM_FB           2
#define FLIP_TIMEOUT_US  500000

#define RGB(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))
#define COL_BG    RGB(0x10, 0x18, 0x20)
#define COL_TITLE RGB(0xff, 0xd0, 0x40)
#define COL_TEXT  RGB(0xe8, 0xe8, 0xe8)
#define COL_DIM   RGB(0x90, 0x90, 0x90)
#define COL_GOOD  RGB(0x40, 0xe0, 0x60)
#define COL_BAD   RGB(0xff, 0x50, 0x50)

enum { ST_NONE, ST_DONE, ST_ERROR };

static struct {
	int  enabled;
	char title[64];
	char config[64];
	char phase[96];
	char summary[200];
	char hint[96];
	char status[320];
	int  status_kind;
	int  run_k, run_n;

	gcmContextData *ctx;
	void *host;
	u32 width, height, pitch, scale;
	u32 *fb[NUM_FB];
	u32 fb_off[NUM_FB];
	int back;
	u32 label;

	volatile int timed;       /* a timed section is open */
	volatile int draw_active; /* hwp_ui_redraw() is between its checks and its end */
	hwp_ui_stats_t stats;
} ui;

/* ------------------------------------------------------------------ events and pad */

static volatile int g_exit_req;
static int g_pad_ok;

static void sysutil_cb(u64 status, u64 param, void *usrdata)
{
	(void)param;
	(void)usrdata;
	if (status == SYSUTIL_EXIT_GAME)
		g_exit_req = 1;
}

void hwp_events_init(void)
{
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
	g_pad_ok = ioPadInit(7) == 0;
	if (!g_pad_ok)
		printf("ioPadInit failed: no pad input\n");
}

void hwp_poll_events(void)
{
	sysUtilCheckCallback();
}

int hwp_exit_requested(void)
{
	return g_exit_req;
}

u32 hwp_pad_buttons(void)
{
	padInfo info;
	padData d;
	u32 mask = 0;
	int i;
	if (!g_pad_ok || ioPadGetInfo(&info) != 0)
		return 0;
	for (i = 0; i < MAX_PORT_NUM; i++) {
		if (!info.status[i] || ioPadGetData(i, &d) != 0)
			continue;
		if (d.BTN_START)
			mask |= HWP_BTN_START;
		if (d.BTN_SELECT)
			mask |= HWP_BTN_SELECT;
		if (d.BTN_CROSS)
			mask |= HWP_BTN_CROSS;
		if (d.BTN_CIRCLE)
			mask |= HWP_BTN_CIRCLE;
	}
	return mask;
}

int hwp_wait_button(u32 mask, int timeout_s)
{
	u64 deadline = timeout_s > 0 ? hwp_mftb() + (u64)timeout_s * hwp_tb_freq() : 0;
	/* a button still held from the previous prompt must not answer this one */
	while (hwp_pad_buttons() & mask) {
		hwp_poll_events();
		if (g_exit_req)
			return HWP_WAIT_EXIT;
		usleep(20000);
	}
	for (;;) {
		hwp_poll_events();
		if (g_exit_req)
			return HWP_WAIT_EXIT;
		if (hwp_pad_buttons() & mask)
			return HWP_WAIT_BUTTON;
		if (deadline && hwp_mftb() >= deadline)
			return HWP_WAIT_TIMEOUT;
		usleep(20000);
	}
}

/* ------------------------------------------------------------------ state setters */

static void set_str(char *dst, size_t cap, const char *src)
{
	strncpy(dst, src ? src : "", cap - 1);
	dst[cap - 1] = 0;
}

void hwp_ui_set_config(const char *name) { set_str(ui.config, sizeof(ui.config), name); }
void hwp_ui_set_phase(const char *phase) { set_str(ui.phase, sizeof(ui.phase), phase); }
void hwp_ui_set_hint(const char *text) { set_str(ui.hint, sizeof(ui.hint), text); }

void hwp_ui_set_run(int k, int n)
{
	ui.run_k = k;
	ui.run_n = n;
}

void hwp_ui_set_summary(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(ui.summary, sizeof(ui.summary), fmt, ap);
	va_end(ap);
}

void hwp_ui_set_done(const char *result_path)
{
	snprintf(ui.status, sizeof(ui.status), "DONE  result: %s", result_path ? result_path : "(none)");
	ui.status_kind = ST_DONE;
}

void hwp_ui_set_error(const char *fmt, ...)
{
	va_list ap;
	char msg[280];
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	snprintf(ui.status, sizeof(ui.status), "ERROR  %s", msg);
	ui.status_kind = ST_ERROR;
}

void hwp_ui_stats(hwp_ui_stats_t *s)
{
	*s = ui.stats;
	s->enabled = ui.enabled;
}

void hwp_ui_write_meta(FILE *f)
{
	fprintf(f, "META,ui_enabled,%d\n", ui.enabled);
	fprintf(f, "META,ui_draws,%u\n", ui.stats.draws);
	fprintf(f, "META,ui_deferred,%u\n", ui.stats.deferred);
	fprintf(f, "META,ui_overlaps,%u\n", ui.stats.overlaps);
	fprintf(f, "META,ui_timed_sections,%u\n", ui.stats.timed_sections);
	fprintf(f, "META,ui_max_draw_us,%llu\n", (unsigned long long)ui.stats.max_draw_us);
}

/* ------------------------------------------------------------------ timed sections */

void hwp_timed_begin(void)
{
	ui.timed = 1;
	hwp_sync();
	ui.stats.timed_sections++;
	/* Dekker pairing with hwp_ui_redraw(): it sets draw_active, syncs, then reads timed;
	 * we set timed, sync, then read draw_active. One of the two sees the other. */
	if (ui.draw_active)
		ui.stats.overlaps++;
}

void hwp_timed_end(void)
{
	hwp_sync();
	ui.timed = 0;
}

/* ------------------------------------------------------------------ video */

static void wait_label(void)
{
	rsxSetWriteBackendLabel(ui.ctx, LABEL_INDEX, ui.label);
	rsxFlushBuffer(ui.ctx);
	while (*(volatile u32 *)gcmGetLabelAddress(LABEL_INDEX) != ui.label)
		usleep(30);
	ui.label++;
}

static void wait_rsx_idle(void)
{
	rsxSetWriteBackendLabel(ui.ctx, LABEL_INDEX, ui.label);
	rsxSetWaitLabel(ui.ctx, LABEL_INDEX, ui.label);
	ui.label++;
	wait_label();
}

static int configure_video(void)
{
	static const u32 ids[] = { VIDEO_RESOLUTION_720, VIDEO_RESOLUTION_1080, VIDEO_RESOLUTION_576,
	                           VIDEO_RESOLUTION_480 };
	videoResolution res;
	videoConfiguration cfg;
	unsigned i;
	for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		if (videoGetResolutionAvailability(VIDEO_PRIMARY, ids[i], VIDEO_ASPECT_AUTO, 0) != 1)
			continue;
		if (videoGetResolution(ids[i], &res) != 0)
			continue;
		memset(&cfg, 0, sizeof(cfg));
		cfg.resolution = (u8)ids[i];
		cfg.format = VIDEO_BUFFER_FORMAT_XRGB;
		cfg.aspect = VIDEO_ASPECT_AUTO;
		cfg.pitch = (u32)res.width * 4;
		if (videoConfigure(VIDEO_PRIMARY, &cfg, NULL, 0) != 0)
			continue;
		ui.width = res.width;
		ui.height = res.height;
		ui.pitch = cfg.pitch;
		return 0;
	}
	return -1;
}

static void fill(u32 *fb, u32 color)
{
	u32 n = ui.width * ui.height, i;
	for (i = 0; i < n; i++)
		fb[i] = color;
}

int hwp_ui_init(const char *title, int enable)
{
	int i;
	memset(&ui.stats, 0, sizeof(ui.stats));
	set_str(ui.title, sizeof(ui.title), title);
	ui.run_k = 0;
	ui.run_n = 0;
	ui.status_kind = ST_NONE;
	ui.enabled = 0;
	if (!enable)
		return -1;

	ui.host = memalign(HOST_ALIGN, HOST_BUFFER_SIZE);
	if (!ui.host) {
		printf("hwprobe ui: host buffer allocation failed, screen off\n");
		return -1;
	}
	if (rsxInit(&ui.ctx, CMD_BUFFER_SIZE, HOST_BUFFER_SIZE, ui.host) != 0 || !ui.ctx) {
		printf("hwprobe ui: rsxInit failed, screen off\n");
		free(ui.host);
		ui.host = NULL;
		return -1;
	}
	if (configure_video() != 0) {
		printf("hwprobe ui: no usable video resolution, screen off\n");
		return -1;
	}
	wait_rsx_idle();
	gcmSetFlipMode(GCM_FLIP_VSYNC);
	for (i = 0; i < NUM_FB; i++) {
		ui.fb[i] = (u32 *)rsxMemalign(64, ui.height * ui.pitch);
		if (!ui.fb[i] || rsxAddressToOffset(ui.fb[i], &ui.fb_off[i]) != 0 ||
		    gcmSetDisplayBuffer(i, ui.fb_off[i], ui.pitch, ui.width, ui.height) != 0) {
			printf("hwprobe ui: display buffer %d setup failed, screen off\n", i);
			return -1;
		}
		fill(ui.fb[i], COL_BG);
	}
	ui.scale = ui.height >= 1000 ? 3 : ui.height >= 700 ? 2 : 1;
	ui.back = 0;
	ui.enabled = 1;
	return 0;
}

/* ------------------------------------------------------------------ drawing */

static void put_glyph(u32 *fb, u32 x, u32 y, unsigned char ch, u32 color)
{
	const unsigned char *g;
	u32 r, c, sy, sx;
	if (ch < HWP_GLYPH_FIRST || ch > HWP_GLYPH_LAST)
		ch = '?';
	g = hwp_font + (ch - HWP_GLYPH_FIRST) * HWP_GLYPH_H;
	for (r = 0; r < HWP_GLYPH_H; r++) {
		for (c = 0; c < HWP_GLYPH_W; c++) {
			if (!(g[r] & (0x80 >> c)))
				continue;
			for (sy = 0; sy < ui.scale; sy++) {
				u32 py = y + r * ui.scale + sy;
				if (py >= ui.height)
					continue;
				for (sx = 0; sx < ui.scale; sx++) {
					u32 px = x + c * ui.scale + sx;
					if (px < ui.width)
						fb[py * (ui.pitch / 4) + px] = color;
				}
			}
		}
	}
}

/* Draws `text` from line `*line`; advances *line. Words (split at spaces) are never broken: a word
 * that does not fit the rest of the line starts the next one, and a word longer than a whole line is
 * cut from the left and drawn as "..." + its tail, so the file name at the end of a path stays whole. */
static void put_text(u32 *fb, u32 *line, const char *text, u32 color)
{
	u32 cell_w = (HWP_GLYPH_W + 1) * ui.scale;
	u32 cell_h = (HWP_GLYPH_H + 2) * ui.scale;
	u32 margin_x = ui.width / 20, margin_y = ui.height / 14;
	u32 cols = (ui.width - 2 * margin_x) / cell_w;
	u32 col = 0;
	const char *p = text;
	if (!*p) {
		(*line)++;
		return;
	}
	while (*p) {
		const char *w;
		u32 n, i, skip = 0;
		if (*p == ' ') {
			if (col > 0 && col < cols) /* spaces at the start of a line are dropped */
				col++;
			p++;
			continue;
		}
		for (w = p; *w && *w != ' '; w++)
			;
		n = (u32)(w - p);
		if (n > cols) {
			skip = n - (cols - 3);
			n = cols;
		}
		if (col > 0 && col + n > cols) {
			col = 0;
			(*line)++;
		}
		if (skip) {
			for (i = 0; i < 3; i++)
				put_glyph(fb, margin_x + (col + i) * cell_w, margin_y + *line * cell_h, '.', color);
			col += 3;
			p += skip;
			n -= 3;
		}
		for (i = 0; i < n; i++)
			put_glyph(fb, margin_x + (col + i) * cell_w, margin_y + *line * cell_h, (unsigned char)p[i], color);
		col += n;
		p = w;
	}
	(*line)++;
}

static void render(u32 *fb)
{
	char buf[200];
	u32 line = 0;
	fill(fb, COL_BG);
	put_text(fb, &line, ui.title, COL_TITLE);
	line++;
	snprintf(buf, sizeof(buf), "config: %s", ui.config[0] ? ui.config : "-");
	put_text(fb, &line, buf, COL_TEXT);
	snprintf(buf, sizeof(buf), "phase:  %s", ui.phase[0] ? ui.phase : "-");
	put_text(fb, &line, buf, COL_TEXT);
	snprintf(buf, sizeof(buf), "run:    %d/%d", ui.run_k, ui.run_n);
	put_text(fb, &line, buf, COL_TEXT);
	line++;
	put_text(fb, &line, "last:", COL_DIM);
	put_text(fb, &line, ui.summary[0] ? ui.summary : "-", COL_TEXT);
	line++;
	if (ui.status_kind != ST_NONE)
		put_text(fb, &line, ui.status, ui.status_kind == ST_DONE ? COL_GOOD : COL_BAD);
	if (ui.hint[0]) {
		line++;
		put_text(fb, &line, ui.hint, COL_DIM);
	}
}

static void wait_flip_done(void)
{
	u32 waited = 0;
	while (gcmGetFlipStatus() != 0 && waited < FLIP_TIMEOUT_US) {
		usleep(200);
		waited += 200;
	}
	gcmResetFlipStatus();
}

int hwp_ui_redraw(void)
{
	u64 t0;
	if (!ui.enabled)
		return 0;
	if (ui.timed) {
		ui.stats.deferred++;
		return 0;
	}
	ui.draw_active = 1;
	hwp_sync();
	if (ui.timed) { /* a timed section opened between the first check and here */
		ui.draw_active = 0;
		ui.stats.deferred++;
		return 0;
	}
	t0 = hwp_mftb();

	render(ui.fb[ui.back]);
	gcmResetFlipStatus();
	gcmSetFlip(ui.ctx, ui.back);
	rsxFlushBuffer(ui.ctx);
	gcmSetWaitFlip(ui.ctx);
	wait_flip_done();
	ui.back ^= 1;

	{
		u64 us = (hwp_mftb() - t0) * 1000000ull / hwp_tb_freq();
		if (us > ui.stats.max_draw_us)
			ui.stats.max_draw_us = us;
	}
	ui.stats.draws++;
	hwp_sync();
	ui.draw_active = 0;
	return 1;
}

void hwp_ui_shutdown(void)
{
	int i;
	if (!ui.enabled)
		return;
	gcmSetWaitFlip(ui.ctx);
	rsxFinish(ui.ctx, 1);
	for (i = 0; i < NUM_FB; i++)
		rsxFree(ui.fb[i]);
	free(ui.host);
	ui.host = NULL;
	ui.enabled = 0;
}

/* lv2conf Layer 1: runs every case of cases/registry.c on the console, in registry order, and
 * writes one result file (FORMAT.md) after the last case.
 *
 * While the cases run nothing touches the file system: rows are formatted into RAM (conf_ppu.c)
 * and the current case is a RAM marker plus the on-screen status line (redrawn only between
 * cases; every case is bracketed as a timed section, so ui_overlaps must come out 0). A watchdog
 * thread at a low priority ends a run that stays in one case longer than `case_limit_s`: it writes
 * what was recorded, names the case in META,aborted_at and leaves the process, so a case that
 * blocks for good costs one run, not the operator's attention.
 *
 * Config: /dev_usb000/lv2conf.cfg, else /dev_hdd0/tmp/hwprobe/lv2conf.cfg (key=value lines, # comments).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/process.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include "hwprobe.h"
#include "../../conf.h"
#include "conf_ppu.h"

#define PROBE_NAME "lv2conf"
#define PROBE_VERSION "1"
#define DEFAULT_DIR "/dev_hdd0/tmp/hwprobe"

typedef struct {
	int screen;
	int pad;
	int hold_s;
	int case_limit_s;
	int progress_file;
	char tag[48];
	char outdir[96];
	char fw_version[32];
	char fw_type[32];
	char console_model[32];
	char groups[256]; /* comma list of groups to run; empty = all */
	char skip[256];   /* comma list of case ids or groups not to run */
} cfg_t;

static void cfg_defaults(cfg_t *c)
{
	memset(c, 0, sizeof(*c));
	c->screen = 1;
	c->pad = 1;
	c->case_limit_s = 30;
	c->progress_file = 1;
	strcpy(c->outdir, DEFAULT_DIR);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
	strncpy(dst, src, cap - 1);
	dst[cap - 1] = 0;
}

static void cfg_set(cfg_t *c, const char *k, const char *v)
{
	if (!strcmp(k, "screen")) c->screen = atoi(v);
	else if (!strcmp(k, "pad")) c->pad = atoi(v);
	else if (!strcmp(k, "hold_s")) c->hold_s = atoi(v);
	else if (!strcmp(k, "case_limit_s")) c->case_limit_s = atoi(v);
	else if (!strcmp(k, "progress_file")) c->progress_file = atoi(v);
	else if (!strcmp(k, "tag")) copy_str(c->tag, sizeof(c->tag), v);
	else if (!strcmp(k, "outdir")) copy_str(c->outdir, sizeof(c->outdir), v);
	else if (!strcmp(k, "fw_version")) copy_str(c->fw_version, sizeof(c->fw_version), v);
	else if (!strcmp(k, "fw_type")) copy_str(c->fw_type, sizeof(c->fw_type), v);
	else if (!strcmp(k, "console_model")) copy_str(c->console_model, sizeof(c->console_model), v);
	else if (!strcmp(k, "groups")) copy_str(c->groups, sizeof(c->groups), v);
	else if (!strcmp(k, "skip")) copy_str(c->skip, sizeof(c->skip), v);
	else printf("lv2conf.cfg: unknown key '%s'\n", k);
}

static int cfg_load(cfg_t *c)
{
	static const char *paths[] = { "/dev_usb000/lv2conf.cfg", DEFAULT_DIR "/lv2conf.cfg" };
	char line[320];
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
		cfg_set(c, line, eq);
	}
	fclose(f);
	return 1;
}

/* does the comma separated `list` contain `word` as a whole entry */
static int list_has(const char *list, const char *word)
{
	size_t n = strlen(word);
	const char *p = list;
	while (*p) {
		const char *e = strchr(p, ',');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len == n && !strncmp(p, word, n))
			return 1;
		if (!e)
			break;
		p = e + 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ run state */

static cfg_t g_cfg;
static volatile unsigned g_idx;        /* index of the case that runs (RAM progress marker) */
static volatile unsigned g_started;    /* cases started */
static volatile unsigned g_completed;  /* cases that returned */
static volatile unsigned g_skipped;
static volatile u64 g_case_start_tb;
static volatile int g_in_case;
static volatile int g_finished;
static volatile int g_writing;
static char g_path[160];
static int g_write_failed;

static const char *config_name(void)
{
	return g_cfg.tag[0] ? g_cfg.tag : "default";
}

static int write_results(int aborted)
{
	hwp_header_t h;
	FILE *f;
	const char *rows;
	size_t len;
	int truncated;
	char name[96];
	int run = hwp_next_run_id(g_cfg.outdir, PROBE_NAME);

	snprintf(name, sizeof(name), "%s_r%03d%s%s.csv", PROBE_NAME, run, g_cfg.tag[0] ? "_" : "", g_cfg.tag);
	f = hwp_result_open(g_cfg.outdir, name, g_path, sizeof(g_path));
	if (!f)
		return -1;

	memset(&h, 0, sizeof(h));
	h.probe = PROBE_NAME;
	h.probe_version = PROBE_VERSION;
	h.config = config_name();
	h.fw_version = g_cfg.fw_version;
	h.fw_type = g_cfg.fw_type;
	h.console_model = g_cfg.console_model;
	h.tbfreq = hwp_tb_freq();
	hwp_write_header(f, &h);

	rows = conf_ppu_rows(&len, &truncated);
	fprintf(f, "META,backend,ppu\n");
	fprintf(f, "META,build,%s %s\n", __DATE__, __TIME__);
	fprintf(f, "META,cases_total,%u\n", conf_case_count);
	fprintf(f, "META,cases_run,%u\n", g_completed);
	fprintf(f, "META,cases_skipped,%u\n", g_skipped);
	fprintf(f, "META,run,%d\n", run);
	fprintf(f, "META,arena_peak,%u\n", conf_ppu_arena_peak());
	fprintf(f, "META,arena_overflows,%u\n", conf_ppu_arena_overflows());
	fprintf(f, "META,rows_truncated,%d\n", truncated);
	fprintf(f, "META,exit_requested,%d\n", hwp_exit_requested());
	fprintf(f, "META,case_limit_s,%d\n", g_cfg.case_limit_s);
	fprintf(f, "META,cfg_groups,%s\n", g_cfg.groups);
	fprintf(f, "META,cfg_skip,%s\n", g_cfg.skip);
	if (aborted) {
		unsigned i = g_idx;
		fprintf(f, "META,aborted_at,%s\n", i < conf_case_count ? conf_cases[i].id : "none");
	}
	hwp_ui_write_meta(f);
	if (len && fwrite(rows, 1, len, f) != len)
		g_write_failed = 1;
	if (fclose(f) != 0)
		g_write_failed = 1;
	return 0;
}

/* Ends the run when one case stays too long. Low priority, wakes twice a second. */
static void watchdog(void *arg)
{
	(void)arg;
	for (;;) {
		sysUsleep(500000);
		if (g_finished)
			break;
		if (g_in_case && g_cfg.case_limit_s > 0 &&
		    (hwp_mftb() - g_case_start_tb) > (u64)g_cfg.case_limit_s * hwp_tb_freq()) {
			if (!__sync_lock_test_and_set(&g_writing, 1)) {
				unsigned i = g_idx;
				conf_ppu_skip(i < conf_case_count ? conf_cases[i].id : "none", "hung_watchdog");
				if (write_results(1) == 0)
					printf("lv2conf: watchdog wrote %s\n", g_path);
				hwp_result_note_last(g_cfg.outdir, PROBE_NAME, g_path, "aborted by watchdog");
			}
			sysProcessExit(2);
		}
	}
	sysThreadExit(0);
}

int main(int argc, char *argv[])
{
	char title[64];
	char sum[160];
	unsigned i;
	int rv = 0;
	sys_ppu_thread_t wd = 0;
	(void)argc;
	(void)argv;

	cfg_defaults(&g_cfg);
	cfg_load(&g_cfg);

	hwp_events_init();
	snprintf(title, sizeof(title), "lv2conf probe v%s", PROBE_VERSION);
	hwp_ui_init(title, g_cfg.screen);
	hwp_ui_set_config(config_name());
	hwp_ui_set_run(0, 1);
	hwp_ui_set_phase("ready");
	if (g_cfg.pad) {
		hwp_ui_set_hint("Press START to run the cases.");
		hwp_ui_redraw();
		if (hwp_wait_button(HWP_BTN_START, 0) == HWP_WAIT_EXIT) {
			hwp_ui_shutdown();
			return 0;
		}
		hwp_ui_set_hint("");
	}

	if (sysThreadCreate(&wd, watchdog, NULL, 3000, 0x8000, 0, "lv2conf_wd") != 0)
		printf("lv2conf: no watchdog thread\n");

	hwp_ui_set_phase("running");
	hwp_ui_set_run(1, 1);
	for (i = 0; i < conf_case_count; i++) {
		const struct conf_case *c = &conf_cases[i];
		g_idx = i;
		if (g_cfg.groups[0] && !list_has(g_cfg.groups, c->group)) {
			g_skipped++;
			conf_ppu_skip(c->id, "not_selected");
			continue;
		}
		if (g_cfg.skip[0] && (list_has(g_cfg.skip, c->id) || list_has(g_cfg.skip, c->group))) {
			g_skipped++;
			conf_ppu_skip(c->id, "cfg_skip");
			continue;
		}
		snprintf(sum, sizeof(sum), "%u/%u %s", i + 1, conf_case_count, c->id);
		hwp_ui_set_summary("%s", sum);
		hwp_ui_redraw();
		if (g_cfg.progress_file)
			hwp_result_note_last(g_cfg.outdir, PROBE_NAME, "(running)", sum);
		conf_ppu_begin(c->id);
		g_started++;
		g_case_start_tb = hwp_mftb();
		g_in_case = 1;
		hwp_timed_begin();
		c->fn();
		hwp_timed_end();
		g_in_case = 0;
		g_completed++;
		hwp_poll_events();
		if (hwp_exit_requested())
			break;
	}
	g_idx = conf_case_count;
	g_finished = 1;

	hwp_ui_set_phase("writing result");
	hwp_ui_redraw();
	if (__sync_lock_test_and_set(&g_writing, 1)) {
		/* the watchdog is already writing and leaves the process itself */
		for (;;)
			sysUsleep(1000000);
	}
	if (write_results(0) != 0 || g_write_failed) {
		rv = 1;
		hwp_ui_set_error("result file could not be written, see the console log");
		hwp_result_note_last(g_cfg.outdir, PROBE_NAME, "(no result file)", "write failed");
	} else {
		snprintf(sum, sizeof(sum), "cases_run=%u/%u skipped=%u", g_completed, conf_case_count, g_skipped);
		printf("wrote %s (%s)\n", g_path, sum);
		hwp_result_note_last(g_cfg.outdir, PROBE_NAME, g_path, sum);
		hwp_ui_set_summary("%s", sum);
		hwp_ui_set_done(g_path);
	}
	if (!rv)
		hwp_ui_set_phase("finished");
	hwp_ui_set_hint(g_cfg.pad ? "Press START to exit" : g_cfg.hold_s > 0 ? "Exits by itself" : "");
	hwp_ui_redraw();
	if (g_cfg.pad)
		hwp_wait_button(HWP_BTN_START, 0);
	else if (g_cfg.hold_s > 0)
		hwp_wait_button(HWP_BTN_START, g_cfg.hold_s);
	hwp_ui_shutdown();
	return rv;
}

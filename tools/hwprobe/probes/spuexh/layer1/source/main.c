/* spuexh Layer 1: the exhaustive SPU instruction sweep on a console (see ../../RUN.md, ../../FORMAT.md).
 *
 * Runs, in this order and each only if configured: special probe A (the DP compares as raw words), special probe B
 * (decoder validity), the sweep. The result file is open for the whole run and grows while it runs (header first, hash rows
 * as the SPUs deliver them), so a power loss or a hang leaves everything computed so far. Every SPU launch is a thread
 * group with an exception event queue; a fault or hang costs one job or one case and is recorded, not fatal.
 *
 * Config: /dev_usb000/spuexh.cfg, else /dev_hdd0/tmp/hwprobe/spuexh.cfg (key=value lines, # comments).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/systime.h>

#include "hwprobe.h"
#include "spuexh_core.h"
#include "backend_ppu.h"

#define PROBE_NAME "spuexh"
#define PROBE_VERSION "1"
#define DEFAULT_DIR "/dev_hdd0/tmp/hwprobe"

typedef struct {
	int screen, pad, hold_s, progress_file;
	char tag[48], outdir[96], fw_version[32], fw_type[32], console_model[32];
} cfg_t;

static spuexh_runcfg_t g_rc;
static cfg_t g_cfg;
static char g_cfg_error[200];

static void copy_str(char *dst, size_t cap, const char *src)
{
	snprintf(dst, cap, "%s", src);
}

static void cfg_set(const char *k, const char *v)
{
	char err[200];
	int r;
	if (!strcmp(k, "screen")) g_cfg.screen = atoi(v);
	else if (!strcmp(k, "pad")) g_cfg.pad = atoi(v);
	else if (!strcmp(k, "hold_s")) g_cfg.hold_s = atoi(v);
	else if (!strcmp(k, "progress_file")) g_cfg.progress_file = atoi(v);
	else if (!strcmp(k, "tag")) copy_str(g_cfg.tag, sizeof(g_cfg.tag), v);
	else if (!strcmp(k, "outdir")) copy_str(g_cfg.outdir, sizeof(g_cfg.outdir), v);
	else if (!strcmp(k, "fw_version")) copy_str(g_cfg.fw_version, sizeof(g_cfg.fw_version), v);
	else if (!strcmp(k, "fw_type")) copy_str(g_cfg.fw_type, sizeof(g_cfg.fw_type), v);
	else if (!strcmp(k, "console_model")) copy_str(g_cfg.console_model, sizeof(g_cfg.console_model), v);
	else {
		r = spuexh_runcfg_set(&g_rc, k, v, err, sizeof(err));
		if (r < 0) copy_str(g_cfg_error, sizeof(g_cfg_error), err);
		else if (r == 0) printf("spuexh.cfg: unknown key '%s'\n", k);
	}
}

static int cfg_load(void)
{
	static const char *paths[] = { "/dev_usb000/spuexh.cfg", DEFAULT_DIR "/spuexh.cfg" };
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
		cfg_set(line, eq);
	}
	fclose(f);
	return 1;
}

static const char *config_name(void)
{
	return g_cfg.tag[0] ? g_cfg.tag : g_rc.preset;
}

static int ui_cb(void *u, const char *summary)
{
	(void)u;
	hwp_poll_events();
	hwp_ui_set_summary("%s", summary);
	hwp_ui_redraw();
	if (g_cfg.progress_file)
		hwp_result_note_last(g_cfg.outdir, PROBE_NAME, "(running)", summary);
	return hwp_exit_requested();
}

int main(int argc, char *argv[])
{
	char title[64], path[160], name[96], sum[200], err[200];
	hwp_header_t h;
	FILE *f;
	int run, rv = 0, acc[5], i;
	spuexh_sweep_res_t sr;
	spuexh_sink_t sink;
	spuexh_plan_t plan;
	(void)argc;
	(void)argv;

	spuexh_runcfg_defaults(&g_rc, "smoke");
	memset(&g_cfg, 0, sizeof(g_cfg));
	g_cfg.screen = 1;
	g_cfg.pad = 1;
	g_cfg.progress_file = 1;
	strcpy(g_cfg.outdir, DEFAULT_DIR);
	cfg_load();
	/* Write containment: the probe writes only below /dev_hdd0/tmp/hwprobe. Anything else (including a path
	 * with a ".." component) fails the config check, and the probe exits without touching the file system. */
	if (!spuexh_outdir_ok(g_cfg.outdir))
		copy_str(g_cfg_error, sizeof(g_cfg_error), "outdir must be /dev_hdd0/tmp/hwprobe or below it, without a .. component");
	if (!g_cfg_error[0] && spuexh_runcfg_finish(&g_rc, err, sizeof(err)) < 0)
		copy_str(g_cfg_error, sizeof(g_cfg_error), err);

	hwp_events_init();
	snprintf(title, sizeof(title), "spuexh probe v%s", PROBE_VERSION);
	hwp_ui_init(title, g_cfg.screen);
	hwp_ui_set_config(config_name());
	hwp_ui_set_run(0, 1);
	hwp_ui_set_phase("ready");
	if (g_cfg_error[0]) {
		hwp_ui_set_error("config: %s", g_cfg_error);
		hwp_ui_redraw();
		hwp_wait_button(HWP_BTN_START, 20);
		hwp_ui_shutdown();
		return 1;
	}
	spuexh_plan(&g_rc.wire, &plan);
	snprintf(sum, sizeof(sum), "%s: %llu jobs, %llu hashes, %d lanes entries", g_rc.preset, (unsigned long long)plan.jobs, (unsigned long long)plan.hashes, g_rc.nlanes);
	hwp_ui_set_summary("%s", sum);
	if (g_cfg.pad) {
		hwp_ui_set_hint("Press START to run.");
		hwp_ui_redraw();
		if (hwp_wait_button(HWP_BTN_START, 0) == HWP_WAIT_EXIT) {
			hwp_ui_shutdown();
			return 0;
		}
		hwp_ui_set_hint("");
	}

	run = hwp_next_run_id(g_cfg.outdir, PROBE_NAME);
	snprintf(name, sizeof(name), "%s_r%03d%s%s.csv", PROBE_NAME, run, g_cfg.tag[0] ? "_" : "", g_cfg.tag);
	f = hwp_result_open(g_cfg.outdir, name, path, sizeof(path));
	if (!f) {
		hwp_ui_set_error("cannot open a result file");
		hwp_ui_redraw();
		hwp_wait_button(HWP_BTN_START, 20);
		hwp_ui_shutdown();
		return 1;
	}
	memset(&h, 0, sizeof(h));
	h.probe = PROBE_NAME;
	h.probe_version = PROBE_VERSION;
	h.config = config_name();
	h.fw_version = g_cfg.fw_version;
	h.fw_type = g_cfg.fw_type;
	h.console_model = g_cfg.console_model;
	h.tbfreq = hwp_tb_freq();
	hwp_write_header(f, &h);
	fprintf(f, "META,backend,ppu\n");
	fprintf(f, "META,build,%s %s\n", __DATE__, __TIME__);
	fprintf(f, "META,run,%d\n", run);
	fprintf(f, "META,preset,%s\n", g_rc.preset);
	fprintf(f, "META,plan_jobs,%llu\nMETA,plan_hashes,%llu\nMETA,plan_calls,%llu\n", (unsigned long long)plan.jobs,
	        (unsigned long long)plan.hashes, (unsigned long long)plan.calls);
	fprintf(f, "META,spus_requested,%d\n", g_rc.spus);
	fprintf(f, "META,special_a,%d\nMETA,special_b,%d\nMETA,sweep,%d\n", g_rc.special_a, g_rc.special_b, g_rc.sweep);
	fprintf(f, "META,gen_seed,%016llx\n", (unsigned long long)EXH_SEED);
	fflush(f);

	hwp_ui_set_phase("starting SPUs");
	hwp_ui_redraw();
	int fatal = 0;
	if (spuexh_ppu_init() != 0) {
		fprintf(f, "META,sweep_error,spu_init_failed\n");
		hwp_ui_set_error("SPU initialisation failed");
		rv = 1;
	} else {
		sink.out = f;
		sink.ui = ui_cb;
		sink.uu = NULL;
		hwp_ui_set_run(1, 1);
		if (g_rc.special_a) {
			hwp_ui_set_phase("special A: DP compares");
			const int ra = spuexh_run_special_a(&spuexh_ppu_backend, &g_rc, &sink, acc);
			if (ra < 0) fatal = 1;    /* -1: a launch could not be contained (FATAL row); 1: operator stop */
			g_rc.dpcmp_known = 1;
			for (i = 0; i < 5; i++)
				g_rc.dpcmp_ok[i] = acc[i];
		}
		if (g_rc.special_b && !fatal && !hwp_exit_requested()) {
			hwp_ui_set_phase("special B: decoder");
			if (spuexh_run_special_b(&spuexh_ppu_backend, &g_rc, &sink) < 0) fatal = 1;
		}
		if (g_rc.nlanes && !fatal && !hwp_exit_requested()) {
			hwp_ui_set_phase("lanes");
			if (spuexh_run_lanes(&spuexh_ppu_backend, &g_rc, &sink) < 0) fatal = 1;
		}
		if (g_rc.sweep && !fatal && !hwp_exit_requested()) {
			hwp_ui_set_phase("sweep");
			rv = spuexh_run_sweep(&spuexh_ppu_backend, &g_rc, &sink, &sr);
			if (rv != 0) rv = 1;
			fprintf(f, "META,hashes,%llu\nMETA,jobs_done,%llu\nMETA,faults,%llu\nMETA,relaunches,%llu\n", (unsigned long long)sr.hashes,
			        (unsigned long long)sr.jobs_done, (unsigned long long)sr.faults, (unsigned long long)sr.relaunches);
			fprintf(f, "META,timing_overflows,%llu\n", (unsigned long long)sr.timing_overflows);
			fprintf(f, "META,completed,%d\nMETA,aborted,%d\nMETA,spus_used,%d\nMETA,wall_s,%.1f\n", sr.completed, sr.aborted, sr.spus_used, sr.wall_s);
			snprintf(sum, sizeof(sum), "hashes=%llu faults=%llu %s", (unsigned long long)sr.hashes, (unsigned long long)sr.faults,
			         sr.completed ? "complete" : "INCOMPLETE");
		} else {
			snprintf(sum, sizeof(sum), fatal ? "FATAL: a launch could not be contained" : g_rc.nlanes ? "special probes and lanes done" : "special probes done");
		}
		if (fatal) {
			rv = 1;
			hwp_ui_set_error("a launch could not be contained: see the FATAL row in the result file");
		}
	}
	fprintf(f, "META,exit_requested,%d\n", hwp_exit_requested());
	hwp_ui_write_meta(f);
	if (fclose(f) != 0)
		rv = 1;
	hwp_result_note_last(g_cfg.outdir, PROBE_NAME, path, sum);
	hwp_ui_set_summary("%s", sum);
	hwp_ui_set_done(path);
	hwp_ui_set_phase(rv ? "finished with problems" : "finished");
	hwp_ui_set_hint(g_cfg.pad ? "Press START to exit" : g_cfg.hold_s > 0 ? "Exits by itself" : "");
	hwp_ui_redraw();
	if (g_cfg.pad)
		hwp_wait_button(HWP_BTN_START, 0);
	else if (g_cfg.hold_s > 0)
		hwp_wait_button(HWP_BTN_START, g_cfg.hold_s);
	hwp_ui_shutdown();
	return rv;
}

/* hwprobe.h - small library every hwprobe probe links (PSL1GHT, PPU, C).
 *
 *   - timebase helpers
 *   - result files: versioned header, writer with USB fallback, run ids
 *   - on-screen status (CPU-drawn text in the RSX display buffers) and pad input
 *
 * MEASUREMENT INTEGRITY RULE
 * --------------------------
 * The result file is the evidence; the screen is status only. Drawing costs PPU time
 * and a flip costs RSX/lv2 activity, so it must never run while a probe is timing
 * something:
 *
 *   1. A probe brackets every timed section with hwp_timed_begin() / hwp_timed_end().
 *   2. The screen is redrawn only between timed sections, or from a lower-priority
 *      thread that never runs during one. hwp_ui_redraw() refuses to draw while a
 *      timed section is open (the request is counted as "deferred" and the screen
 *      keeps its last frame).
 *   3. If a draw and a timed section overlap anyway (a probe thread racing the
 *      bracket), hwp_timed_begin() counts it. hwp_ui_write_meta() puts the counters
 *      into the result file (META,ui_overlaps ...), so an analysis can tell. A probe
 *      that wants to be sure checks hwp_ui_stats().overlaps == 0 before it trusts a run.
 *   4. Video output itself (vblank, flips of the last frame) cannot be paused. A probe
 *      that is sensitive to it offers a way to run with the screen off
 *      (hwp_ui_init(.., enable = 0): no video is initialised at all) and records the
 *      setting in the result file.
 *
 * Threading: the setters and hwp_ui_redraw() belong to one thread (normally the probe's
 * main thread). hwp_timed_begin/end may be called from any thread.
 */
#ifndef HWPROBE_H
#define HWPROBE_H

#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <ppu-types.h>

/* ------------------------------------------------------------------ timebase */

static inline u64 hwp_mftb(void)
{
	u64 t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

static inline void hwp_sync(void)
{
	__asm__ volatile("sync" ::: "memory");
}

/* Timebase ticks per second (sysGetTimebaseFrequency), read once. 79 800 000 on the
 * consoles seen so far; probes log it instead of assuming it. */
u64 hwp_tb_freq(void);

/* ------------------------------------------------------------------ result files */

#define HWP_FORMAT_VERSION 1

/* Every result file starts with this header (see README.md, "Result header"):
 *
 *   HWPROBE,<format version>
 *   HDR,<key>,<value>            one row per field, in the order of hwp_header_t
 *
 * A reader rejects a file whose first row is not HWPROBE,<known version>. Values never
 * contain ',' or line breaks (the writer replaces them with '_'). NULL = "unknown". */
typedef struct {
	const char *probe;          /* probe name, e.g. "ring110" */
	const char *probe_version;  /* probe's own version constant */
	const char *config;         /* run configuration name ("default" when none was chosen) */
	const char *fw_version;     /* firmware version as the operator declared it, or NULL */
	const char *fw_type;        /* "REBUG", "CEX", ... as declared, or NULL */
	const char *console_model;  /* e.g. "CECHC04" as declared, or NULL */
	u64 tbfreq;
} hwp_header_t;

/* git describe and source hash of the build come from -DHWP_GIT_DESCRIBE / -DHWP_SOURCE_SHA256
 * (tools/hwprobe/build.sh); "unknown" when the build did not provide them. PSL1GHT at the
 * pinned commit has no firmware/model query, so the three console fields are only what the
 * operator declared and console_info_source says so. */
void hwp_write_header(FILE *f, const hwp_header_t *h);

/* Next run id of `probe` in `dir` (counter in <dir>/<probe>_runid.txt), 1 when absent. */
int hwp_next_run_id(const char *dir, const char *probe);

/* Creates `dir` if missing, opens <dir>/<name> for writing with a 64 KiB buffer. When
 * that fails, falls back to /dev_usb000/<name>. `path` receives the path actually used.
 * Returns NULL when both fail. One result file at a time (static buffer). */
FILE *hwp_result_open(const char *dir, const char *name, char *path, size_t cap);

/* <dir>/<probe>_last.txt = result path + one summary line, for operators who only
 * have the console's file manager. */
void hwp_result_note_last(const char *dir, const char *probe, const char *path, const char *summary);

/* ------------------------------------------------------------------ events and pad */

#define HWP_BTN_START  1u
#define HWP_BTN_SELECT 2u
#define HWP_BTN_CROSS  4u
#define HWP_BTN_CIRCLE 8u

#define HWP_WAIT_BUTTON  0
#define HWP_WAIT_EXIT    1 /* the PS-menu "quit game" request arrived */
#define HWP_WAIT_TIMEOUT 2

/* Registers the sysutil callback (exit request) and opens the pad. Call once. */
void hwp_events_init(void);

/* Services sysutil callbacks; call regularly from the main thread (not in a timed section
 * of that thread). */
void hwp_poll_events(void);
int  hwp_exit_requested(void);

/* Buttons currently held on any connected pad (HWP_BTN_* mask). */
u32 hwp_pad_buttons(void);

/* Blocks until one of `mask` is pressed, an exit is requested, or `timeout_s` passes
 * (0 = no timeout). Never call it inside a timed section. */
int hwp_wait_button(u32 mask, int timeout_s);

/* ------------------------------------------------------------------ on-screen status */

typedef struct {
	int  enabled;        /* video initialised and drawing */
	u32  draws;          /* frames drawn and flipped */
	u32  deferred;       /* hwp_ui_redraw() calls refused because a timed section was open */
	u32  overlaps;       /* draws that overlapped a timed section (should stay 0) */
	u32  timed_sections; /* hwp_timed_begin() calls */
	u64  max_draw_us;    /* longest hwp_ui_redraw() that drew, microseconds */
} hwp_ui_stats_t;

/* Starts the screen. enable = 0 leaves video untouched (status stays off, stats say so).
 * Returns 0 when the screen is up, -1 when it is off (disabled or video/RSX init failed;
 * the probe still runs, the result file says so). */
int  hwp_ui_init(const char *title, int enable);
void hwp_ui_set_config(const char *name);
void hwp_ui_set_phase(const char *phase);
void hwp_ui_set_run(int k, int n);
void hwp_ui_set_summary(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void hwp_ui_set_hint(const char *text);
void hwp_ui_set_done(const char *result_path);
void hwp_ui_set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Draws the current state and waits for the flip. Returns 1 when drawn, 0 when skipped
 * (screen off, or a timed section is open). */
int  hwp_ui_redraw(void);
void hwp_ui_stats(hwp_ui_stats_t *s);
void hwp_ui_shutdown(void);

/* Bracket every timed section (see the rule at the top of this file). */
void hwp_timed_begin(void);
void hwp_timed_end(void);

/* META rows for the result file: ui_enabled, ui_draws, ui_deferred, ui_overlaps,
 * ui_timed_sections, ui_max_draw_us. Call when the file is written, after the last draw
 * that matters for the run. */
void hwp_ui_write_meta(FILE *f);

#endif

/* hwprobe_result.c - timebase, versioned result header, result file writer. See hwprobe.h. */
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/systime.h>

#include "hwprobe.h"

#ifndef HWP_GIT_DESCRIBE
#define HWP_GIT_DESCRIBE "unknown"
#endif
#ifndef HWP_SOURCE_SHA256
#define HWP_SOURCE_SHA256 "unknown"
#endif

u64 hwp_tb_freq(void)
{
	static u64 f;
	if (!f)
		f = sysGetTimebaseFrequency();
	return f;
}

/* A value with a comma or a line break would shift every later field of its row. */
static void hdr_row(FILE *f, const char *key, const char *val)
{
	const char *p;
	fprintf(f, "HDR,%s,", key);
	for (p = val && val[0] ? val : "unknown"; *p; p++)
		fputc(*p == ',' || *p == '\n' || *p == '\r' ? '_' : *p, f);
	fputc('\n', f);
}

void hwp_write_header(FILE *f, const hwp_header_t *h)
{
	char tb[32];
	int operator_declared = (h->fw_version && h->fw_version[0]) || (h->fw_type && h->fw_type[0]) ||
	                        (h->console_model && h->console_model[0]);
	snprintf(tb, sizeof(tb), "%llu", (unsigned long long)h->tbfreq);
	fprintf(f, "HWPROBE,%d\n", HWP_FORMAT_VERSION);
	hdr_row(f, "probe", h->probe);
	hdr_row(f, "probe_version", h->probe_version);
	hdr_row(f, "git_describe", HWP_GIT_DESCRIBE);
	hdr_row(f, "source_sha256", HWP_SOURCE_SHA256);
	hdr_row(f, "fw_version", h->fw_version);
	hdr_row(f, "fw_type", h->fw_type);
	hdr_row(f, "console_model", h->console_model);
	hdr_row(f, "console_info_source", operator_declared ? "operator" : "unavailable");
	hdr_row(f, "config", h->config && h->config[0] ? h->config : "default");
	hdr_row(f, "tbfreq", tb);
}

/* One level only: the parent (/dev_hdd0/tmp) exists on every console. An existing
 * directory makes the syscall fail, which is the expected case and ignored. */
static void ensure_dir(const char *dir)
{
	(void)sysLv2FsMkdir(dir, 0777);
}

int hwp_next_run_id(const char *dir, const char *probe)
{
	char path[160];
	FILE *f;
	int id = 0;

	ensure_dir(dir);
	snprintf(path, sizeof(path), "%s/%s_runid.txt", dir, probe);
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%d", &id) != 1)
			id = 0;
		fclose(f);
	}
	id++;
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%d\n", id);
		fclose(f);
	}
	return id;
}

FILE *hwp_result_open(const char *dir, const char *name, char *path, size_t cap)
{
	static char iobuf[1 << 16];
	FILE *f;

	ensure_dir(dir);
	snprintf(path, cap, "%s/%s", dir, name);
	f = fopen(path, "w");
	if (!f) {
		printf("cannot open %s\n", path);
		snprintf(path, cap, "/dev_usb000/%s", name);
		f = fopen(path, "w");
		if (!f) {
			printf("cannot open %s\n", path);
			return NULL;
		}
		printf("writing %s instead\n", path);
	}
	setvbuf(f, iobuf, _IOFBF, sizeof(iobuf));
	return f;
}

void hwp_result_note_last(const char *dir, const char *probe, const char *path, const char *summary)
{
	char p[160];
	FILE *f;
	ensure_dir(dir);
	snprintf(p, sizeof(p), "%s/%s_last.txt", dir, probe);
	f = fopen(p, "w");
	if (!f)
		return;
	fprintf(f, "%s\n%s\n", path, summary);
	fclose(f);
}

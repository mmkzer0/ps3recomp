/* conf_ppu.c - the PPU backend of conf.h: real lv2 syscalls on a console.
 *
 *  - conf_sc is an inline `sc` with r3..r10 as PSL1GHT's lv2syscall8 does (same register
 *    assignment and clobbers); the kernel's r3 comes back sign-extended from 32 bits.
 *  - scratch is a static buffer below 4 GiB, so a pointer is a 32-bit guest address as the
 *    kernel expects. The PPU is big endian, the accessors are plain volatile loads and stores.
 *  - rows are formatted into RAM while the cases run (no file I/O around the syscalls under test)
 *    and written by main.c after the last case.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ppu-types.h>

#include "hwprobe.h"

#include "../../conf.h"
#include "conf_ppu.h"

#define ARENA_SIZE (512u * 1024u)
#define SINK_SIZE (64u * 1024u)
#define ROWS_SIZE (768u * 1024u)

static uint8_t g_arena[ARENA_SIZE] __attribute__((aligned(4096)));
static uint8_t g_sink[SINK_SIZE] __attribute__((aligned(4096)));
static unsigned g_arena_used;
static unsigned g_arena_peak;
static unsigned g_arena_overflows;
static volatile int g_arena_lock;

static char g_rows[ROWS_SIZE];
static size_t g_rows_len;
static int g_rows_truncated;
static const char *g_case = "none";

/* ------------------------------------------------------------------ syscall */

int64_t conf_sc(uint32_t num, unsigned nargs, const uint64_t *args)
{
	register u64 p1 __asm__("3") = nargs > 0 ? args[0] : 0;
	register u64 p2 __asm__("4") = nargs > 1 ? args[1] : 0;
	register u64 p3 __asm__("5") = nargs > 2 ? args[2] : 0;
	register u64 p4 __asm__("6") = nargs > 3 ? args[3] : 0;
	register u64 p5 __asm__("7") = nargs > 4 ? args[4] : 0;
	register u64 p6 __asm__("8") = nargs > 5 ? args[5] : 0;
	register u64 p7 __asm__("9") = nargs > 6 ? args[6] : 0;
	register u64 p8 __asm__("10") = nargs > 7 ? args[7] : 0;
	register u64 scn __asm__("11") = num;
	__asm__ __volatile__("sc"
	                     : "=r"(p1), "=r"(p2), "=r"(p3), "=r"(p4), "=r"(p5), "=r"(p6), "=r"(p7), "=r"(p8), "=r"(scn)
	                     : "r"(p1), "r"(p2), "r"(p3), "r"(p4), "r"(p5), "r"(p6), "r"(p7), "r"(p8), "r"(scn)
	                     : "r0", "r12", "lr", "ctr", "xer", "cr0", "cr1", "cr5", "cr6", "cr7", "memory");
	return (int64_t)(int32_t)(uint32_t)p1;
}

/* ------------------------------------------------------------------ scratch */

static void arena_lock(void)
{
	while (__sync_lock_test_and_set(&g_arena_lock, 1))
		;
}

static void arena_unlock(void)
{
	__sync_lock_release(&g_arena_lock);
}

conf_addr conf_alloc(uint32_t size, uint32_t align)
{
	unsigned at;
	conf_addr a;
	if (align == 0)
		align = 1;
	arena_lock();
	at = (g_arena_used + align - 1) / align * align;
	if (at + size > ARENA_SIZE || size > SINK_SIZE) {
		/* a case must still be able to finish; the overflow is counted and reported as META */
		g_arena_overflows++;
		memset(g_sink, 0, size > SINK_SIZE ? SINK_SIZE : size);
		a = (conf_addr)(uintptr_t)g_sink;
	} else {
		g_arena_used = at + size;
		if (g_arena_used > g_arena_peak)
			g_arena_peak = g_arena_used;
		memset(g_arena + at, 0, size);
		a = (conf_addr)(uintptr_t)(g_arena + at);
	}
	arena_unlock();
	return a;
}

uint8_t conf_r8(conf_addr a)
{
	return *(volatile uint8_t *)(uintptr_t)a;
}

uint32_t conf_r32(conf_addr a)
{
	return *(volatile uint32_t *)(uintptr_t)a;
}

uint64_t conf_r64(conf_addr a)
{
	return *(volatile uint64_t *)(uintptr_t)a;
}

void conf_w8(conf_addr a, uint8_t v)
{
	*(volatile uint8_t *)(uintptr_t)a = v;
}

void conf_w32(conf_addr a, uint32_t v)
{
	*(volatile uint32_t *)(uintptr_t)a = v;
}

void conf_w64(conf_addr a, uint64_t v)
{
	*(volatile uint64_t *)(uintptr_t)a = v;
}

/* ------------------------------------------------------------------ time */

uint64_t conf_tb(void)
{
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

uint64_t conf_tbfreq(void)
{
	return hwp_tb_freq();
}

/* ------------------------------------------------------------------ rows */

/* append one formatted row; a row that does not fit marks the buffer truncated instead of being cut */
static void row_put(int n, const char *line)
{
	if (n < 0 || (size_t)n >= 256 || g_rows_len + (size_t)n >= sizeof(g_rows)) {
		g_rows_truncated = 1;
		return;
	}
	memcpy(g_rows + g_rows_len, line, (size_t)n);
	g_rows_len += (size_t)n;
}

void conf_rec(const char *key, int64_t value)
{
	char line[256];
	row_put(snprintf(line, sizeof(line), "CASE,%s,%s,%lld\n", g_case, key, (long long)value), line);
}

void conf_rec_rc(const char *key, int64_t rc)
{
	char line[256];
	row_put(snprintf(line, sizeof(line), "CASE,%s,%s,0x%08x\n", g_case, key, (unsigned)(uint32_t)rc), line);
}

void conf_rec_time(const char *key, uint64_t ticks)
{
	char line[256];
	row_put(snprintf(line, sizeof(line), "TIME,%s,%s,%llu\n", g_case, key, (unsigned long long)ticks), line);
}

void conf_ppu_begin(const char *id)
{
	arena_lock();
	g_arena_used = 0;
	arena_unlock();
	g_case = id;
}

void conf_ppu_skip(const char *id, const char *reason)
{
	char line[256];
	row_put(snprintf(line, sizeof(line), "SKIP,%s,%s\n", id, reason), line);
}

const char *conf_ppu_rows(size_t *len, int *truncated)
{
	*len = g_rows_len;
	*truncated = g_rows_truncated;
	return g_rows;
}

unsigned conf_ppu_arena_peak(void)
{
	return g_arena_peak;
}

unsigned conf_ppu_arena_overflows(void)
{
	return g_arena_overflows;
}

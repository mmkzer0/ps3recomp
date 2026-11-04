/* conf.h - the lv2conf case API (portable C99, includes only <stdint.h>).
 *
 * One set of cases (cases/<group>.c + cases/registry.c) runs on two backends: the PPU backend
 * (layer1/source/conf_ppu.c: real lv2 syscalls on a console) and the host backend (the
 * toolkit's HLE syscall table). Both write the same result rows (FORMAT.md), a compare tool
 * diffs them.
 *
 * Rules for case code (the reason for each is in RUN.md / FORMAT.md):
 *  - include only this header; no libc, no PSL1GHT, no toolkit header;
 *  - syscalls by number through conf_sc(), the number's firmware name in a comment;
 *  - attribute structs are written byte by byte with conf_w*() at explicit offsets (a 64-bit
 *    host and the PPU must not share a struct layout);
 *  - never record raw kernel ids, addresses or pointers, only relations (id != 0, ids differ,
 *    a returned value equals the written one);
 *  - every call that can block has a finite timeout, or the case is CONF_HW_ONLY;
 *  - pointer arguments are valid scratch, or NULL only where RPCS3 checks NULL before use;
 *  - nothing outside /dev_hdd0/tmp/hwprobe/, no dev_flash, no process exit, no shutdown, no network;
 *  - a case destroys what it created, so cases are independent and order-free.
 */
#ifndef LV2CONF_CONF_H
#define LV2CONF_CONF_H

#include <stdint.h>

typedef uint32_t conf_addr; /* 32-bit guest effective address */

/* Raw lv2 syscall by NUMBER: args -> r3..r10 (nargs <= 8), returns r3 sign-extended from 32 bits. */
int64_t conf_sc(uint32_t num, unsigned nargs, const uint64_t *args);

/* Zeroed guest scratch; all of it is released when the next case starts. */
conf_addr conf_alloc(uint32_t size, uint32_t align);

/* Big-endian guest semantics on both backends. */
uint8_t  conf_r8 (conf_addr a);
uint32_t conf_r32(conf_addr a);
uint64_t conf_r64(conf_addr a);
void     conf_w8 (conf_addr a, uint8_t v);
void     conf_w32(conf_addr a, uint32_t v);
void     conf_w64(conf_addr a, uint64_t v);

/* Deterministic observation of the current case. */
void conf_rec(const char *key, int64_t value);
/* A return code; written as 0x%08x of the low 32 bits. */
void conf_rec_rc(const char *key, int64_t rc);

/* Timebase. */
uint64_t conf_tb(void);
uint64_t conf_tbfreq(void);
/* Timing observation; never compared for equality. */
void conf_rec_time(const char *key, uint64_t ticks);

enum { CONF_HW_ONLY = 1u, CONF_TIMING = 2u, CONF_THREADS = 4u };

struct conf_case {
	const char *id;    /* "<group>.<name>", lower-case, [a-z0-9_.] */
	const char *group;
	unsigned flags;    /* CONF_* */
	void (*fn)(void);
};

extern const struct conf_case conf_cases[];
extern const unsigned conf_case_count;

#endif

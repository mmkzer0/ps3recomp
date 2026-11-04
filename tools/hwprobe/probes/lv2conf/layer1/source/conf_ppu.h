/* conf_ppu.h - what main.c needs from the PPU backend beyond conf.h. */
#ifndef LV2CONF_CONF_PPU_H
#define LV2CONF_CONF_PPU_H

#include <stddef.h>

/* Starts a case: releases all scratch (zeroed again on allocation) and names the case that
 * conf_rec*() rows are attributed to. Call it before running the case's function. */
void conf_ppu_begin(const char *id);

/* A SKIP row for a case that is not run. */
void conf_ppu_skip(const char *id, const char *reason);

/* The rows recorded so far, in order, newline terminated. `*truncated` is set when the row buffer
 * overflowed and later rows were dropped. The buffer stays valid for the process lifetime. */
const char *conf_ppu_rows(size_t *len, int *truncated);

/* High-water mark of the scratch arena over all cases, bytes, and the number of allocations that
 * did not fit (they were served from a sink so the case could finish). */
unsigned conf_ppu_arena_peak(void);
unsigned conf_ppu_arena_overflows(void);

#endif

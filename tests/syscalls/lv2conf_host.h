/*
 * lv2conf host backend: what test_lv2conf.c needs from lv2conf_host.c besides the conf.h API.
 *
 * The runner forks one child per case. The parent maps guest memory once; each child registers the
 * syscall table itself, so one case's kernel state never reaches the next.
 */
#ifndef LV2CONF_HOST_H
#define LV2CONF_HOST_H

#include <stdint.h>
#include <stdio.h>

/* The PS3 timebase frequency. conf_tb() reports host time in ticks of this rate, so a TIME row from the
 * host reads in the same unit as one from the console. */
#define LV2CONF_HOST_TBFREQ 79800000ull

/* What the parent can read about a child after it ended (shared memory). */
struct lv2conf_host_status {
    volatile uint32_t finished;      /* the case function returned */
    volatile uint32_t sc_num;        /* syscall slot in flight, UINT32_MAX when none */
    volatile uintptr_t sc_handler;   /* the table entry that slot dispatched to */
};

/* Parent, once, before the first fork: map guest memory (vm_init) and the shared status. 0 on success. */
int lv2conf_host_init(void);

/* The shared status block lv2conf_host_init mapped. */
struct lv2conf_host_status *lv2conf_host_status(void);

/* Child, first thing: this process runs case `id` and writes its CASE and TIME rows to `rows`. Resets the
 * scratch arena and registers the toolkit's lv2 syscall table. */
void lv2conf_host_begin_case(const char *id, FILE *rows);

#endif

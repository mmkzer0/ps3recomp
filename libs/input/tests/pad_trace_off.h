/* The pad trace as a fixture that includes cellPad.c without testing it sees it:
 * neither pad.record nor pad.replay is set, so the hooks in cellPad.c meet no
 * trace. Include after cellPad.c. A fixture that exercises the trace links
 * libs/input/pad_trace.c and supplies its own ps3_config_get instead. */
#ifndef PAD_TRACE_OFF_H
#define PAD_TRACE_OFF_H

#include <stdlib.h>
#include "../pad_trace.h"
#include "ps3emu/config.h"

static ps3_config pad_off_config;   /* zeroed: both pad options empty */
const ps3_config* ps3_config_get(void) { return &pad_off_config; }

PadTrace* pad_trace_global(const PadTraceOptions* o) { (void)o; return NULL; }
int pad_trace_replaying(const PadTrace* t) { (void)t; return 0; }
void pad_trace_flush(PadTrace* t) { (void)t; }
/* cellPad.c reaches these only through a non-NULL trace. */
void pad_trace_poll(PadTrace* t, uint32_t port, PadTraceState* st, const PadTraceState* neutral, int* fresh)
{ (void)t; (void)port; (void)st; (void)neutral; (void)fresh; abort(); }
void pad_trace_conn(PadTrace* t, uint8_t on[PAD_TRACE_MAX_PORTS]) { (void)t; (void)on; abort(); }

#endif

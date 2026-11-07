/* backend_ppu.c - the SPU thread backend of the spuexh probe on a console (PSL1GHT sys_spu_* calls).
 *
 * One call of run() = one SPU thread group: create, connect an event queue for group exceptions (mandatory: without
 * it a group exception can take the whole process down), initialise one thread per params block, start, then wait.
 * The wait ends when every thread has reported a final state in its status block, an exception event arrives, the
 * supervisor's progress callback asks to stop (hang) or the call's wall limit passes (limit_ms > 0 is required).
 * Join is called only after every thread reports exited or a successful terminate. lv2 provides no join timeout. A failed
 * terminate, join, group destroy or event-queue destroy returns -3 with the syscall and rc recorded in FATAL; an unknown
 * or live group is left for process exit to reclaim rather than followed by another wait. Other lv2 results and exception
 * event words are returned as text so the result file records them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <sys/spu.h>
#include <sys/event_queue.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include "hwprobe.h"
#include "spuexh_core.h"
#include "backend_ppu.h"

#include "spuexh_spu_bin.h"

static sysSpuImage g_image;
static int g_image_ok;

double spuexh_now(void)
{
	return (double)hwp_mftb() / (double)hwp_tb_freq();
}

int spuexh_ppu_init(void)
{
	s32 r;
	r = sysSpuInitialize(6, 0);               /* may already be done by the system; the error is not interesting */
	(void)r;
	r = sysSpuImageImport(&g_image, spuexh_spu_bin, 0);
	if (r != 0) {
		printf("spuexh: sysSpuImageImport failed: 0x%08x\n", (unsigned)r);
		return -1;
	}
	g_image_ok = 1;
	return 0;
}

static void *ppu_alloc(spuexh_backend_t *be, size_t size, uint64_t *ea)
{
	void *p;
	(void)be;
	size = (size + 127) & ~(size_t)127;
	p = memalign(128, size);
	if (!p)
		return NULL;
	memset(p, 0, size);
	*ea = (uint64_t)(uintptr_t)p;
	return p;
}

static void note(char *desc, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void note(char *desc, size_t cap, const char *fmt, ...)
{
	size_t n = strlen(desc);
	va_list ap;
	if (n + 2 >= cap)
		return;
	va_start(ap, fmt);
	vsnprintf(desc + n, cap - n, fmt, ap);
	va_end(ap);
}
static int destroy_event_queue(sys_event_queue_t q, spuexh_thread_res_t *res)
{
	s32 r = sysEventQueueDestroy(q, SYS_EVENT_QUEUE_FORCE_DESTROY);
	if (r != 0) {
		res[0].fatal = 1;
		snprintf(res[0].desc, sizeof(res[0].desc), "sysEventQueueDestroy rc=0x%08x", (unsigned)r);
		return -1;
	}
	return 0;
}

static int destroy_group_and_queue(sys_spu_group_t gid, sys_event_queue_t q, spuexh_thread_res_t *res)
{
	s32 group_rc = sysSpuThreadGroupDestroy(gid);
	s32 queue_rc = sysEventQueueDestroy(q, SYS_EVENT_QUEUE_FORCE_DESTROY);
	if (group_rc == 0 && queue_rc == 0)
		return 0;
	res[0].fatal = 1;
	res[0].desc[0] = 0;
	if (group_rc != 0)
		snprintf(res[0].desc, sizeof(res[0].desc), "sysSpuThreadGroupDestroy rc=0x%08x", (unsigned)group_rc);
	if (queue_rc != 0) {
		size_t n = strlen(res[0].desc);
		snprintf(res[0].desc + n, sizeof(res[0].desc) - n, "%ssysEventQueueDestroy rc=0x%08x",
		         group_rc != 0 ? "; " : "", (unsigned)queue_rc);
	}
	return -1;
}


static int ppu_run(spuexh_backend_t *be, int n, const uint64_t *param_ea, const volatile spuexh_status_t *const *st,
                   spuexh_progress_fn progress, void *pu, int limit_ms, spuexh_thread_res_t *res)
{
	sys_spu_group_t gid = 0;
	sys_spu_thread_t tid[6];
	sys_event_queue_t q = 0;
	sys_event_queue_attr_t qa;
	sysSpuThreadGroupAttribute ga = { 7 + 1, "spuexhg", 0, { 0 } };
	sysSpuThreadAttribute ta = { "spuexht", 8, SPU_THREAD_ATTR_NONE };
	sysSpuThreadArgument arg;
	sys_event_t ev;
	double t0, last_ev = 0;
	int i, nev = 0, all_done = 0, fault = 0, stopped = 0, timed = 0, created = 0, fatal = 0;
	u32 cause = 0, status = 0;
	s32 r;
	char evtext[192] = "";
	(void)be;

	/* No launch without a wall limit: limit_ms <= 0 would be an unbounded launch (RUN.md policy). */
	if (!g_image_ok || n < 1 || n > 6 || limit_ms <= 0)
		return -1;
	memset(&qa, 0, sizeof(qa));
	qa.attr_protocol = SYS_EVENT_QUEUE_PRIO;
	qa.type = SYS_EVENT_QUEUE_PPU;
	memcpy(qa.name, "spuexh", 7);
	r = sysEventQueueCreate(&q, &qa, SYS_EVENT_QUEUE_KEY_LOCAL, 32);
	if (r != 0) {
		snprintf(res[0].desc, sizeof(res[0].desc), "sysEventQueueCreate rc=0x%08x", (unsigned)r);
		res[0].fatal = 1;
		return -3;                    /* not a matter of the thread count: do not retry this launch */
	}
	r = sysSpuThreadGroupCreate(&gid, (u32)n, 100, &ga);
	if (r != 0) {
		snprintf(res[0].desc, sizeof(res[0].desc), "thread group create (%d threads) failed 0x%08x", n, (unsigned)r);
		if (destroy_event_queue(q, res) < 0)
			return -3;
		return -2;
	}
	/* The exception queue is mandatory: without it a group exception can take the whole process down
	 * (RUN.md). A failed connect is a launch failure - destroy the unstarted group, run nothing. */
	r = sysSpuThreadGroupConnectEvent(gid, q, SPU_THREAD_GROUP_EVENT_EXCEPTION);
	if (r != 0) {
		snprintf(res[0].desc, sizeof(res[0].desc), "sysSpuThreadGroupConnectEvent rc=0x%08x", (unsigned)r);
		res[0].fatal = 1;
		if (destroy_group_and_queue(gid, q, res) < 0)
			return -3;
		return -3;
	}
	for (i = 0; i < n; i++) {
		memset(&arg, 0, sizeof(arg));
		arg.arg0 = param_ea[i];
		r = sysSpuThreadInitialize(&tid[i], gid, (u32)i, &g_image, &ta, &arg);
		if (r != 0) {
			snprintf(res[0].desc, sizeof(res[0].desc), "thread %d initialize failed 0x%08x", i, (unsigned)r);
			if (destroy_group_and_queue(gid, q, res) < 0)
				return -3;
			return -2;
		}
		created++;
	}
	r = sysSpuThreadGroupStart(gid);
	if (r != 0) {
		snprintf(res[0].desc, sizeof(res[0].desc), "thread group start failed 0x%08x", (unsigned)r);
		if (destroy_group_and_queue(gid, q, res) < 0)
			return -3;
		return -2;
	}

	t0 = spuexh_now();
	for (;;) {
		r = sysEventQueueReceive(q, &ev, 200000 /* us */);
		__sync_synchronize();
		if (r == 0) {
			fault = 1;
			last_ev = spuexh_now();
			if (nev < 2)
				note(evtext, sizeof(evtext), "event[src=%llx d1=%llx d2=%llx d3=%llx] ", (unsigned long long)ev.source,
				     (unsigned long long)ev.data_1, (unsigned long long)ev.data_2, (unsigned long long)ev.data_3);
			nev++;
		}
		all_done = 1;
		for (i = 0; i < n; i++) {
			uint32_t s = W32(st[i]->state);
			if (s != 2 && s != 3)
				all_done = 0;
		}
		if (all_done || fault)
			break;
		if (progress && progress(pu)) {
			stopped = 1;
			break;
		}
		if (limit_ms > 0 && (spuexh_now() - t0) * 1000.0 > limit_ms) {
			timed = 1;
			break;
		}
	}
	(void)last_ev;
	/* Join policy (RUN.md): neither a group-exception event nor the final DMA status proves that lv2 has ended the
	 * threads, and join waits for group completion with no timeout (sys/spu.h:875-889 joins after termination or
	 * completion). So before any join the probe asks lv2 per thread: sysSpuThreadGetExitStatus is polled until every
	 * thread reports exited, bounded by the launch's remaining deadline with a floor of 2 s (an event or a deadline
	 * that arrives earlier never shortens that floor). An exception event, or a thread that has not exited when the
	 * deadline passes, means the group is not known to end by itself: it is terminated. Terminate 0 -> the group is
	 * ending, join is bounded (recorded as a terminate-join). Terminate error -> the group's state is unknown: FATAL,
	 * no join, the probe stops and process exit lets lv2 reclaim it.
	 *
	 * What GetExitStatus returns for a running or exception-stopped thread is UNVERIFIED in this tree: PSL1GHT's
	 * sys/spu.h documents only "zero if no error occured, nonzero otherwise", and no lv2/RPCS3 source is available
	 * here. The probe therefore records every nonzero code it sees in META,exit_status_pending rows (first occurrence
	 * per code), so the first hardware run settles the semantics. */
	const double poll_until = t0 + ((double)limit_ms > 2000.0 ? (double)limit_ms : 2000.0) / 1000.0;
	s32 pend[6] = { 0, 0, 0, 0, 0, 0 };
	int exited_all = 0, terminated = 0;
	for (;;) {
		exited_all = 1;
		for (i = 0; i < n; i++) {
			s32 es = 0;
			const s32 e = sysSpuThreadGetExitStatus(tid[i], &es);
			if (e != 0) {
				exited_all = 0;
				if (!pend[i])
					pend[i] = e;
			}
		}
		if (exited_all || spuexh_now() >= poll_until)
			break;
		sysUsleep(20000);
	}
	if (fault || !exited_all) {
		r = sysSpuThreadGroupTerminate(gid, 0x7E57);
		note(evtext, sizeof(evtext), "sysSpuThreadGroupTerminate rc=0x%08x ", (unsigned)r);
		if (r != 0) {
			fatal = 1;
			note(evtext, sizeof(evtext), "FATAL: no join after a failed terminate ");
		} else {
			terminated = 1;
			note(evtext, sizeof(evtext), "terminate-join ");
		}
	}
	if (fatal) {
		for (i = 0; i < n; i++) {
			snprintf(res[i].desc, sizeof(res[i].desc), "%sFATAL: sysSpuThreadGroupTerminate rc=0x%08x %s",
			         fault ? "FAULT " : "", (unsigned)r, evtext);
			res[i].fatal = 1;
			res[i].timed_out = 1;
			res[i].exit_status_pending = pend[i];
		}
		/* The live group and its queue are left for process exit: touching them could block. */
		return -3;
	}
	r = sysSpuThreadGroupJoin(gid, &cause, &status);
	if (r != 0) {
		for (i = 0; i < n; i++) {
			snprintf(res[i].desc, sizeof(res[i].desc), "sysSpuThreadGroupJoin rc=0x%08x", (unsigned)r);
			res[i].fatal = 1;
		}
		return -3; /* a failed join does not prove the group ended, so do not destroy live resources */
	}
	for (i = 0; i < n; i++) {
		s32 es = -1;
		sysSpuThreadGetExitStatus(tid[i], &es);
		snprintf(res[i].desc, sizeof(res[i].desc), "join 0x%08x cause=%u status=0x%x exit=0x%x %s%s%s%s", (unsigned)r,
		         (unsigned)cause, (unsigned)status, (unsigned)es, fault ? "FAULT " : "", timed || stopped ? "LIMIT " : "",
		         terminated ? "terminate-join " : "", evtext);
		res[i].exited = all_done && W32(st[i]->state) == 2;
		res[i].timed_out = timed || stopped;
		res[i].terminate_join = terminated;
		res[i].exit_status_pending = pend[i];
	}
	(void)created;
	if (destroy_group_and_queue(gid, q, res) < 0) {
		for (i = 0; i < n; i++) {
			res[i].fatal = 1;
			if (i > 0)
				snprintf(res[i].desc, sizeof(res[i].desc), "%s", res[0].desc);
		}
		return -3;
	}
	return 0;
}

spuexh_backend_t spuexh_ppu_backend = { "ppu", NULL, ppu_alloc, ppu_run };

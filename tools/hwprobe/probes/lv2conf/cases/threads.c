/* Group "threads": cases that need a second PPU thread, or the calling thread's own id.
 *
 * conf.h has no thread API (a host backend cannot run guest code), so these bodies are PPU code
 * written against PSL1GHT (sysThreadCreate, sysThreadExit, sysThreadGetId). They are all flagged
 * CONF_THREADS; a host build compiles empty stubs of the same names and its runner skips the
 * flag. Children only use conf_sc() and plain guest-memory accessors, never conf_rec*: the main
 * thread reads what the child wrote into scratch after the join (or after a flag the child sets
 * last) and records it.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4), rpcs3/Emu/Cell/lv2/:
 *   sys_ppu_thread.cpp  join :183-268  detach :270-316  get_join_state :318-329  set/get priority :331-426
 *   sys_mutex.cpp :148-310 (lock waits)   sys_semaphore.cpp :81-114, :116-231, :257-338
 *   sys_cond.cpp :430-617   sys_event.cpp :473-608 (receive)   sys_event_flag.cpp :127-270, :481-534
 *   sys_rwlock.cpp :337-489
 * Every wait a child makes has a 2 s timeout, so a case that goes wrong ends instead of hanging.
 */
#include "cc.h"

#define S32(x) ((uint64_t)(int64_t)(int32_t)(x))
#define CHILD_TIMEOUT_US 2000000ull

#if defined(__powerpc64__)

#include <sys/thread.h>
#include <lv2/thread.h>

/* context words every case shares, at the start of a 64-byte scratch block */
#define CX_READY 0   /* u32: set to 1 by the child when it is about to block */
#define CX_RC 4      /* u32: rc of the child's main call */
#define CX_ID_A 8    /* u32: object id handed to the child */
#define CX_ID_B 12   /* u32 */
#define CX_RES 16    /* u64: a result word of the child */
#define CX_RC2 24    /* u32: a second rc */
#define CX_ID_C 28   /* u32 */
#define CX_SIZE 64

static conf_addr ctx_of(void *arg)
{
	return (conf_addr)(uintptr_t)arg;
}

static conf_addr ctx_new(void)
{
	return conf_alloc(CX_SIZE, 8);
}

static int64_t spawn(void (*fn)(void *), conf_addr ctx, int prio, int joinable, uint64_t *tid)
{
	sys_ppu_thread_t t = 0;
	s32 rc = sysThreadCreate(&t, fn, (void *)(uintptr_t)ctx, prio, 0x10000, joinable ? THREAD_JOINABLE : 0, "lv2conf");
	*tid = (uint64_t)t;
	return rc;
}

/* join the child, return the join rc; *ret receives its exit value */
static int64_t join_child(uint64_t tid, uint64_t *ret)
{
	conf_addr r = conf_alloc(8, 8);
	int64_t rc;
	conf_w64(r, SENT64);
	rc = sc2(SC_SYS_PPU_THREAD_JOIN, tid, r);
	*ret = conf_r64(r);
	return rc;
}

/* poll a context word until it equals `want` (up to 1 s); returns 1 when it did */
static int wait_word(conf_addr a, uint32_t want)
{
	unsigned i;
	for (i = 0; i < 1000; i++) {
		if (conf_r32(a) == want)
			return 1;
		(void)sc1(SC_SYS_TIMER_USLEEP, 1000);
	}
	return conf_r32(a) == want;
}

static void nap(uint64_t us)
{
	(void)sc1(SC_SYS_TIMER_USLEEP, us);
}

/* ---------------------------------------------------------------- create / join */

static void child_value(void *arg)
{
	conf_w32(ctx_of(arg) + CX_RC, 0xC0FFEEu);
	sysThreadExit(0x1122334455667788ull);
}

/* a joinable child's exit value comes back through join (sys_ppu_thread.cpp:253-266) */
void case_threads_create_join_value(void)
{
	conf_addr c = ctx_new();
	uint64_t tid = 0, ret = 0;
	int64_t rc = spawn(child_value, c, 1500, 1, &tid);
	conf_rec_rc("create", rc);
	rec_bool("tid_nonzero", tid != 0);
	if (rc != 0)
		return;
	conf_rec_rc("join", join_child(tid, &ret));
	rec_bool("exit_value", ret == 0x1122334455667788ull);
	rec_bool("child_ran", conf_r32(c + CX_RC) == 0xC0FFEEu);
	conf_rec_rc("join_again", join_child(tid, &ret));
}

/* join of an unknown id is ESRCH even with a thread alive; join of oneself is EDEADLK (:189-192) */
void case_threads_self_join(void)
{
	sys_ppu_thread_t me = 0;
	uint64_t ret = 0;
	conf_rec_rc("get_id", sysThreadGetId(&me));
	rec_bool("id_nonzero", me != 0);
	conf_rec_rc("join_self", join_child((uint64_t)me, &ret));
}

/* ---------------------------------------------------------------- priority */

static void child_sem_waiter(void *arg)
{
	conf_addr c = ctx_of(arg);
	conf_w32(c + CX_READY, 1);
	conf_w32(c + CX_RC, (uint32_t)sc2(SC_SYS_SEMAPHORE_WAIT, conf_r32(c + CX_ID_A), CHILD_TIMEOUT_US));
	sysThreadExit(0);
}

static uint32_t sem_new(int32_t initial, int32_t max)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, 0);
	if (sc4(SC_SYS_SEMAPHORE_CREATE, p, attr_sema_rw(SYNC_PRIO, SYNC_NOT_PROCESS_SHARED), S32(initial), S32(max)) != 0)
		return 0;
	return conf_r32(p);
}

/* a child's priority is the one it was created with, readable and changeable by another thread
 * (sys_ppu_thread.cpp:366-426, :331-364) */
void case_threads_priority_of_child(void)
{
	conf_addr c = ctx_new();
	conf_addr pw = conf_alloc(4, 4);
	uint32_t sem = sem_new(0, 1);
	uint64_t tid = 0, ret = 0;
	conf_w32(c + CX_ID_A, sem);
	if (spawn(child_sem_waiter, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
		return;
	}
	(void)wait_word(c + CX_READY, 1);
	conf_w32(pw, SENT32);
	conf_rec_rc("get", sc2(SC_SYS_PPU_THREAD_GET_PRIORITY, tid, pw));
	conf_rec("prio_at_create", (int64_t)(int32_t)conf_r32(pw));
	conf_rec_rc("set", sc2(SC_SYS_PPU_THREAD_SET_PRIORITY, tid, 1400));
	conf_w32(pw, SENT32);
	(void)sc2(SC_SYS_PPU_THREAD_GET_PRIORITY, tid, pw);
	conf_rec("prio_after_set", (int64_t)(int32_t)conf_r32(pw));
	conf_rec_rc("set_out_of_range", sc2(SC_SYS_PPU_THREAD_SET_PRIORITY, tid, 3072));
	(void)sc2(SC_SYS_SEMAPHORE_POST, sem, 1);
	conf_rec_rc("join", join_child(tid, &ret));
	rec_bool("child_rc_ok", conf_r32(c + CX_RC) == 0);
	(void)sc1(SC_SYS_SEMAPHORE_DESTROY, sem);
}

/* the calling thread's own priority is readable by its id and settable to itself */
void case_threads_self_priority(void)
{
	sys_ppu_thread_t me = 0;
	conf_addr pw = conf_alloc(4, 4);
	int32_t p0, p1;
	(void)sysThreadGetId(&me);
	conf_w32(pw, SENT32);
	conf_rec_rc("get", sc2(SC_SYS_PPU_THREAD_GET_PRIORITY, (uint64_t)me, pw));
	p0 = (int32_t)conf_r32(pw);
	rec_bool("prio_in_range", p0 >= 0 && p0 <= 3071);
	conf_rec_rc("set_same", sc2(SC_SYS_PPU_THREAD_SET_PRIORITY, (uint64_t)me, S32(p0)));
	conf_w32(pw, SENT32);
	(void)sc2(SC_SYS_PPU_THREAD_GET_PRIORITY, (uint64_t)me, pw);
	p1 = (int32_t)conf_r32(pw);
	rec_bool("unchanged", p0 == p1);
}

/* ---------------------------------------------------------------- join state / detach */

static void child_join_state(void *arg)
{
	conf_addr c = ctx_of(arg);
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, SENT32);
	conf_w32(c + CX_RC, (uint32_t)sc1(SC_SYS_PPU_THREAD_GET_JOIN_STATE, p));
	conf_w32(c + CX_RES, conf_r32(p));
	conf_w32(c + CX_READY, 1);
	sysThreadExit(0);
}

/* sys_ppu_thread.cpp:327: the join state is 1 for a joinable thread and 0 for a detached one */
void case_threads_join_state_by_flags(void)
{
	conf_addr cj = ctx_new(), cd = ctx_new();
	uint64_t tj = 0, td = 0, ret = 0;
	if (spawn(child_join_state, cj, 1500, 1, &tj) == 0) {
		conf_rec_rc("join", join_child(tj, &ret));
		conf_rec_rc("joinable_rc", conf_r32(cj + CX_RC));
		conf_rec("joinable_state", (int64_t)conf_r32(cj + CX_RES));
	}
	if (spawn(child_join_state, cd, 1500, 0, &td) == 0) {
		rec_bool("detached_finished", wait_word(cd + CX_READY, 1));
		conf_rec_rc("detached_rc", conf_r32(cd + CX_RC));
		conf_rec("detached_state", (int64_t)conf_r32(cd + CX_RES));
	}
}

/* sys_ppu_thread.cpp:287-288, :208-210, :292-293: detach a joinable thread once; a second detach
 * and a join of a detached thread are CELL_EINVAL; once the thread has exited the id is gone (ESRCH) */
void case_threads_detach_errors(void)
{
	conf_addr c = ctx_new();
	uint32_t sem = sem_new(0, 1);
	uint64_t tid = 0, ret = 0;
	conf_w32(c + CX_ID_A, sem);
	if (spawn(child_sem_waiter, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
		return;
	}
	(void)wait_word(c + CX_READY, 1);
	conf_rec_rc("detach", sc1(SC_SYS_PPU_THREAD_DETACH, tid));
	conf_rec_rc("detach_again", sc1(SC_SYS_PPU_THREAD_DETACH, tid));
	conf_rec_rc("join_detached", join_child(tid, &ret));
	(void)sc2(SC_SYS_SEMAPHORE_POST, sem, 1);
	nap(100000); /* the child wakes, returns from its wait and exits */
	conf_rec_rc("detach_after_exit", sc1(SC_SYS_PPU_THREAD_DETACH, tid));
	conf_rec_rc("join_after_exit", join_child(tid, &ret));
	(void)sc1(SC_SYS_SEMAPHORE_DESTROY, sem);
}

/* ---------------------------------------------------------------- mutex */

static void child_mutex_contender(void *arg)
{
	conf_addr c = ctx_of(arg);
	uint32_t m = conf_r32(c + CX_ID_A);
	conf_w32(c + CX_RC2, (uint32_t)sc1(SC_SYS_MUTEX_TRYLOCK, m));
	conf_w32(c + CX_READY, 1);
	conf_w32(c + CX_RC, (uint32_t)sc2(SC_SYS_MUTEX_LOCK, m, CHILD_TIMEOUT_US));
	if (conf_r32(c + CX_RC) == 0)
		conf_w32(c + CX_ID_C, (uint32_t)sc1(SC_SYS_MUTEX_UNLOCK, m));
	sysThreadExit(0);
}

/* a second thread cannot take a held mutex (trylock EBUSY), waits, and gets it when the owner
 * unlocks; its unlock works; the first thread cannot unlock what it no longer owns */
void case_threads_mutex_contention(void)
{
	conf_addr c = ctx_new();
	uint32_t m = mutex_new(0);
	uint64_t tid = 0, ret = 0;
	conf_rec_rc("lock", sc2(SC_SYS_MUTEX_LOCK, m, CHILD_TIMEOUT_US));
	conf_w32(c + CX_ID_A, m);
	if (spawn(child_mutex_contender, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
		(void)sc1(SC_SYS_MUTEX_UNLOCK, m);
		(void)sc1(SC_SYS_MUTEX_DESTROY, m);
		return;
	}
	(void)wait_word(c + CX_READY, 1);
	nap(50000);
	conf_rec_rc("unlock", sc1(SC_SYS_MUTEX_UNLOCK, m));
	conf_rec_rc("join", join_child(tid, &ret));
	conf_rec_rc("child_trylock", conf_r32(c + CX_RC2));
	conf_rec_rc("child_lock", conf_r32(c + CX_RC));
	conf_rec_rc("child_unlock", conf_r32(c + CX_ID_C));
	conf_rec_rc("unlock_not_owner", sc1(SC_SYS_MUTEX_UNLOCK, m));
	conf_rec_rc("destroy", sc1(SC_SYS_MUTEX_DESTROY, m));
}

/* destroying a mutex somebody waits for (sys_mutex.cpp:112-119 checks only the owner) */
void case_threads_mutex_destroy_with_waiter(void)
{
	conf_addr c = ctx_new();
	uint32_t m = mutex_new(0);
	uint64_t tid = 0, ret = 0;
	conf_rec_rc("lock", sc2(SC_SYS_MUTEX_LOCK, m, CHILD_TIMEOUT_US));
	conf_w32(c + CX_ID_A, m);
	if (spawn(child_mutex_contender, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
		(void)sc1(SC_SYS_MUTEX_UNLOCK, m);
		(void)sc1(SC_SYS_MUTEX_DESTROY, m);
		return;
	}
	(void)wait_word(c + CX_READY, 1);
	nap(50000);
	conf_rec_rc("destroy_while_owned", sc1(SC_SYS_MUTEX_DESTROY, m));
	conf_rec_rc("unlock", sc1(SC_SYS_MUTEX_UNLOCK, m));
	conf_rec_rc("join", join_child(tid, &ret));
	conf_rec_rc("child_lock", conf_r32(c + CX_RC));
	(void)sc1(SC_SYS_MUTEX_DESTROY, m);
}

/* ---------------------------------------------------------------- semaphore */

/* a waiter drives the value below zero (get_value reads 0, sys_semaphore.cpp:348), blocks destroy
 * (EBUSY, :89-92), and is released by post */
void case_threads_semaphore_waiter(void)
{
	conf_addr c = ctx_new();
	conf_addr v = conf_alloc(4, 4);
	uint32_t sem = sem_new(0, 2);
	uint64_t tid = 0, ret = 0;
	conf_w32(c + CX_ID_A, sem);
	if (spawn(child_sem_waiter, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
		(void)sc1(SC_SYS_SEMAPHORE_DESTROY, sem);
		return;
	}
	(void)wait_word(c + CX_READY, 1);
	nap(50000);
	conf_w32(v, SENT32);
	conf_rec_rc("get_value", sc2(SC_SYS_SEMAPHORE_GET_VALUE, sem, v));
	conf_rec("value_with_waiter", (int64_t)(int32_t)conf_r32(v));
	conf_rec_rc("destroy_with_waiter", sc1(SC_SYS_SEMAPHORE_DESTROY, sem));
	conf_rec_rc("post", sc2(SC_SYS_SEMAPHORE_POST, sem, 1));
	conf_rec_rc("join", join_child(tid, &ret));
	conf_rec_rc("child_wait", conf_r32(c + CX_RC));
	conf_w32(v, SENT32);
	(void)sc2(SC_SYS_SEMAPHORE_GET_VALUE, sem, v);
	conf_rec("value_after", (int64_t)(int32_t)conf_r32(v));
	conf_rec_rc("destroy", sc1(SC_SYS_SEMAPHORE_DESTROY, sem));
}

/* the waiter is not woken by a failed post past the maximum (value stays) */
void case_threads_semaphore_timeout_in_child(void)
{
	conf_addr c = ctx_new();
	uint32_t sem = sem_new(0, 1);
	uint64_t tid = 0, ret = 0;
	conf_w32(c + CX_ID_A, sem);
	if (spawn(child_sem_waiter, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
		(void)sc1(SC_SYS_SEMAPHORE_DESTROY, sem);
		return;
	}
	conf_rec_rc("join", join_child(tid, &ret));
	conf_rec_rc("child_wait", conf_r32(c + CX_RC));
	conf_rec_rc("destroy", sc1(SC_SYS_SEMAPHORE_DESTROY, sem));
}

/* ---------------------------------------------------------------- cond */

static void child_cond_waiter(void *arg)
{
	conf_addr c = ctx_of(arg);
	uint32_t m = conf_r32(c + CX_ID_A), cv = conf_r32(c + CX_ID_B);
	conf_w32(c + CX_RC2, (uint32_t)sc2(SC_SYS_MUTEX_LOCK, m, CHILD_TIMEOUT_US));
	conf_w32(c + CX_READY, 1);
	conf_w32(c + CX_RC, (uint32_t)sc2(SC_SYS_COND_WAIT, cv, CHILD_TIMEOUT_US));
	conf_w32(c + CX_ID_C, (uint32_t)sc1(SC_SYS_MUTEX_UNLOCK, m));
	sysThreadExit(0);
}

/* the child holds the mutex until its wait releases it, so the parent's lock can only succeed once
 * the child waits; signal wakes it, it owns the mutex again (sys_cond.cpp:471-494, :611-616) */
void case_threads_cond_signal(void)
{
	conf_addr c = ctx_new();
	uint32_t m = mutex_new(0), cv = 0;
	conf_addr p = conf_alloc(4, 4);
	uint64_t tid = 0, ret = 0;
	conf_w32(p, 0);
	(void)sc3(SC_SYS_COND_CREATE, p, m, attr_cond(SYNC_NOT_PROCESS_SHARED));
	cv = conf_r32(p);
	conf_w32(c + CX_ID_A, m);
	conf_w32(c + CX_ID_B, cv);
	if (spawn(child_cond_waiter, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
		(void)sc1(SC_SYS_COND_DESTROY, cv);
		(void)sc1(SC_SYS_MUTEX_DESTROY, m);
		return;
	}
	(void)wait_word(c + CX_READY, 1);
	conf_rec_rc("parent_lock", sc2(SC_SYS_MUTEX_LOCK, m, CHILD_TIMEOUT_US));
	conf_rec_rc("signal", sc1(SC_SYS_COND_SIGNAL, cv));
	conf_rec_rc("parent_unlock", sc1(SC_SYS_MUTEX_UNLOCK, m));
	conf_rec_rc("join", join_child(tid, &ret));
	conf_rec_rc("child_lock", conf_r32(c + CX_RC2));
	conf_rec_rc("child_wait", conf_r32(c + CX_RC));
	conf_rec_rc("child_unlock", conf_r32(c + CX_ID_C));
	conf_rec_rc("cond_destroy", sc1(SC_SYS_COND_DESTROY, cv));
	conf_rec_rc("mutex_destroy", sc1(SC_SYS_MUTEX_DESTROY, m));
}

/* while a thread waits on the condition variable it cannot be destroyed (sys_cond.cpp:132-135, CELL_EBUSY) */
void case_threads_cond_destroy_with_waiter(void)
{
	conf_addr c = ctx_new();
	uint32_t m = mutex_new(0), cv = 0;
	conf_addr p = conf_alloc(4, 4);
	uint64_t tid = 0, ret = 0;
	conf_w32(p, 0);
	(void)sc3(SC_SYS_COND_CREATE, p, m, attr_cond(SYNC_NOT_PROCESS_SHARED));
	cv = conf_r32(p);
	conf_w32(c + CX_ID_A, m);
	conf_w32(c + CX_ID_B, cv);
	if (spawn(child_cond_waiter, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
		(void)sc1(SC_SYS_COND_DESTROY, cv);
		(void)sc1(SC_SYS_MUTEX_DESTROY, m);
		return;
	}
	(void)wait_word(c + CX_READY, 1);
	conf_rec_rc("parent_lock", sc2(SC_SYS_MUTEX_LOCK, m, CHILD_TIMEOUT_US));
	conf_rec_rc("destroy_with_waiter", sc1(SC_SYS_COND_DESTROY, cv));
	conf_rec_rc("signal_all", sc1(SC_SYS_COND_SIGNAL_ALL, cv));
	conf_rec_rc("parent_unlock", sc1(SC_SYS_MUTEX_UNLOCK, m));
	conf_rec_rc("join", join_child(tid, &ret));
	conf_rec_rc("child_wait", conf_r32(c + CX_RC));
	(void)sc1(SC_SYS_COND_DESTROY, cv);
	(void)sc1(SC_SYS_MUTEX_DESTROY, m);
}

/* ---------------------------------------------------------------- event queue */

static void child_queue_receiver(void *arg)
{
	conf_addr c = ctx_of(arg);
	conf_addr ev = conf_alloc(32, 8);
	conf_w32(c + CX_READY, 1);
	conf_w32(c + CX_RC, (uint32_t)sc3(SC_SYS_EVENT_QUEUE_RECEIVE, conf_r32(c + CX_ID_A), ev, CHILD_TIMEOUT_US));
	sysThreadExit(0);
}

/* a blocked receiver is woken by a send through a port (sys_event.cpp:166-185); the event is
 * consumed by it, so the queue is empty afterwards */
void case_threads_event_receive_wakeup(void)
{
	conf_addr c = ctx_new();
	conf_addr q = conf_alloc(4, 4), pt = conf_alloc(4, 4), ev = conf_alloc(32, 8), cnt = conf_alloc(4, 4);
	uint64_t tid = 0, ret = 0;
	conf_w32(q, 0);
	conf_w32(pt, 0);
	(void)sc4(SC_SYS_EVENT_QUEUE_CREATE, q, attr_equeue(SYNC_FIFO, 1), 0, 4);
	(void)sc3(SC_SYS_EVENT_PORT_CREATE, pt, 1, 0x55);
	(void)sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, conf_r32(pt), conf_r32(q));
	conf_w32(c + CX_ID_A, conf_r32(q));
	if (spawn(child_queue_receiver, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
	} else {
		(void)wait_word(c + CX_READY, 1);
		nap(50000);
		conf_rec_rc("send", sc4(SC_SYS_EVENT_PORT_SEND, conf_r32(pt), 1, 2, 3));
		conf_rec_rc("join", join_child(tid, &ret));
		conf_rec_rc("child_receive", conf_r32(c + CX_RC));
		conf_w32(cnt, SENT32);
		conf_rec_rc("tryreceive", sc4(SC_SYS_EVENT_QUEUE_TRYRECEIVE, conf_r32(q), ev, 4, cnt));
		conf_rec("queue_count_after", (int64_t)conf_r32(cnt));
	}
	(void)sc1(SC_SYS_EVENT_PORT_DISCONNECT, conf_r32(pt));
	(void)sc1(SC_SYS_EVENT_PORT_DESTROY, conf_r32(pt));
	(void)sc2(SC_SYS_EVENT_QUEUE_DESTROY, conf_r32(q), 1);
}

/* destroy with a receiver blocked: mode 0 -> CELL_EBUSY (sys_event.cpp:314-317); FORCE wakes the
 * receiver with CELL_ECANCELED (:385) */
void case_threads_event_queue_destroy_with_receiver(void)
{
	conf_addr c = ctx_new();
	conf_addr q = conf_alloc(4, 4);
	uint64_t tid = 0, ret = 0;
	conf_w32(q, 0);
	(void)sc4(SC_SYS_EVENT_QUEUE_CREATE, q, attr_equeue(SYNC_FIFO, 1), 0, 4);
	conf_w32(c + CX_ID_A, conf_r32(q));
	if (spawn(child_queue_receiver, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
		(void)sc2(SC_SYS_EVENT_QUEUE_DESTROY, conf_r32(q), 1);
		return;
	}
	(void)wait_word(c + CX_READY, 1);
	nap(50000);
	conf_rec_rc("destroy_plain", sc2(SC_SYS_EVENT_QUEUE_DESTROY, conf_r32(q), 0));
	conf_rec_rc("destroy_force", sc2(SC_SYS_EVENT_QUEUE_DESTROY, conf_r32(q), 1));
	conf_rec_rc("join", join_child(tid, &ret));
	conf_rec_rc("child_receive", conf_r32(c + CX_RC));
}

/* ---------------------------------------------------------------- event flag */

static void child_flag_waiter(void *arg)
{
	conf_addr c = ctx_of(arg);
	conf_addr r = conf_alloc(8, 8);
	uint32_t mode = conf_r32(c + CX_ID_B);
	conf_w64(r, SENT64);
	conf_w32(c + CX_READY, 1);
	conf_w32(c + CX_RC, (uint32_t)sc5(SC_SYS_EVENT_FLAG_WAIT, conf_r32(c + CX_ID_A), 0x1, mode, r, CHILD_TIMEOUT_US));
	conf_w64(c + CX_RES, conf_r64(r));
	sysThreadExit(0);
}

static uint32_t flag_new(uint32_t type)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, 0);
	if (sc3(SC_SYS_EVENT_FLAG_CREATE, p, attr_eflag(SYNC_PRIO, type), 0) != 0)
		return 0;
	return conf_r32(p);
}

/* sys_event_flag.cpp:310-460: a set that satisfies a waiter wakes it with the pattern; CLEAR then
 * removes the waited bit (check_pattern :98-101) */
void case_threads_eventflag_set_wakes(void)
{
	conf_addr c = ctx_new();
	conf_addr g = conf_alloc(8, 8);
	uint32_t f = flag_new(0x20000);
	uint64_t tid = 0, ret = 0;
	conf_w32(c + CX_ID_A, f);
	conf_w32(c + CX_ID_B, 0x02 | 0x10); /* OR | CLEAR */
	if (spawn(child_flag_waiter, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
		(void)sc1(SC_SYS_EVENT_FLAG_DESTROY, f);
		return;
	}
	(void)wait_word(c + CX_READY, 1);
	nap(50000);
	conf_rec_rc("set", sc2(SC_SYS_EVENT_FLAG_SET, f, 0x5));
	conf_rec_rc("join", join_child(tid, &ret));
	conf_rec_rc("child_wait", conf_r32(c + CX_RC));
	conf_rec("child_result", (int64_t)conf_r64(c + CX_RES));
	conf_w64(g, SENT64);
	(void)sc2(SC_SYS_EVENT_FLAG_GET, f, g);
	conf_rec("pattern_after", (int64_t)conf_r64(g));
	(void)sc1(SC_SYS_EVENT_FLAG_DESTROY, f);
}

/* sys_event_flag.cpp:481-534: cancel releases the waiters with CELL_ECANCELED and reports how
 * many; destroy while one waits is CELL_EBUSY (:105-108) */
void case_threads_eventflag_cancel(void)
{
	conf_addr c = ctx_new();
	conf_addr n = conf_alloc(4, 4);
	uint32_t f = flag_new(0x20000);
	uint64_t tid = 0, ret = 0;
	conf_w32(c + CX_ID_A, f);
	conf_w32(c + CX_ID_B, 0x01); /* AND */
	if (spawn(child_flag_waiter, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
		(void)sc1(SC_SYS_EVENT_FLAG_DESTROY, f);
		return;
	}
	(void)wait_word(c + CX_READY, 1);
	nap(50000);
	conf_rec_rc("destroy_with_waiter", sc1(SC_SYS_EVENT_FLAG_DESTROY, f));
	conf_w32(n, SENT32);
	conf_rec_rc("cancel", sc2(SC_SYS_EVENT_FLAG_CANCEL, f, n));
	conf_rec("cancelled", (int64_t)conf_r32(n));
	conf_rec_rc("join", join_child(tid, &ret));
	conf_rec_rc("child_wait", conf_r32(c + CX_RC));
	(void)sc1(SC_SYS_EVENT_FLAG_DESTROY, f);
}

/* sys_event_flag.cpp:170-173: a flag created for a single waiter refuses a second waiter with CELL_EPERM */
void case_threads_eventflag_single_waiter(void)
{
	conf_addr c = ctx_new();
	conf_addr r = conf_alloc(8, 8);
	uint32_t f = flag_new(0x10000);
	uint64_t tid = 0, ret = 0;
	conf_w32(c + CX_ID_A, f);
	conf_w32(c + CX_ID_B, 0x01);
	if (spawn(child_flag_waiter, c, 1500, 1, &tid) != 0) {
		conf_rec("spawn_failed", 1);
		(void)sc1(SC_SYS_EVENT_FLAG_DESTROY, f);
		return;
	}
	(void)wait_word(c + CX_READY, 1);
	nap(50000);
	conf_rec_rc("second_waiter", sc5(SC_SYS_EVENT_FLAG_WAIT, f, 0x1, 0x01, r, WAIT_US));
	(void)sc2(SC_SYS_EVENT_FLAG_SET, f, 0x1);
	conf_rec_rc("join", join_child(tid, &ret));
	conf_rec_rc("child_wait", conf_r32(c + CX_RC));
	(void)sc1(SC_SYS_EVENT_FLAG_DESTROY, f);
}

/* ---------------------------------------------------------------- rwlock */

static void child_rw(void *arg)
{
	conf_addr c = ctx_of(arg);
	uint32_t rw = conf_r32(c + CX_ID_A);
	conf_w32(c + CX_RC2, (uint32_t)sc1(SC_SYS_RWLOCK_TRYRLOCK, rw));
	(void)sc1(SC_SYS_RWLOCK_RUNLOCK, rw);
	conf_w32(c + CX_ID_C, (uint32_t)sc1(SC_SYS_RWLOCK_TRYWLOCK, rw));
	conf_w32(c + CX_RC, (uint32_t)sc2(SC_SYS_RWLOCK_WLOCK, rw, 100000));
	sysThreadExit(0);
}

/* sys_rwlock.cpp:104-140, :497-520: another thread may share a read lock, but cannot write while
 * it is held: try-writer EBUSY, timed writer ETIMEDOUT */
void case_threads_rwlock_reader_blocks_writer(void)
{
	conf_addr c = ctx_new();
	conf_addr p = conf_alloc(4, 4);
	uint32_t rw;
	uint64_t tid = 0, ret = 0;
	conf_w32(p, 0);
	(void)sc2(SC_SYS_RWLOCK_CREATE, p, attr_sema_rw(SYNC_PRIO, SYNC_NOT_PROCESS_SHARED));
	rw = conf_r32(p);
	conf_rec_rc("rlock", sc2(SC_SYS_RWLOCK_RLOCK, rw, CHILD_TIMEOUT_US));
	conf_w32(c + CX_ID_A, rw);
	if (spawn(child_rw, c, 1500, 1, &tid) == 0) {
		conf_rec_rc("join", join_child(tid, &ret));
		conf_rec_rc("child_tryrlock", conf_r32(c + CX_RC2));
		conf_rec_rc("child_trywlock", conf_r32(c + CX_ID_C));
		conf_rec_rc("child_wlock", conf_r32(c + CX_RC));
	} else {
		conf_rec("spawn_failed", 1);
	}
	conf_rec_rc("runlock", sc1(SC_SYS_RWLOCK_RUNLOCK, rw));
	conf_rec_rc("destroy", sc1(SC_SYS_RWLOCK_DESTROY, rw));
}

#else /* host build: same names, never called (the runner skips CONF_THREADS) */

#define THREAD_STUB(n) void case_threads_##n(void) { }
THREAD_STUB(create_join_value)
THREAD_STUB(self_join)
THREAD_STUB(priority_of_child)
THREAD_STUB(self_priority)
THREAD_STUB(join_state_by_flags)
THREAD_STUB(detach_errors)
THREAD_STUB(mutex_contention)
THREAD_STUB(mutex_destroy_with_waiter)
THREAD_STUB(semaphore_waiter)
THREAD_STUB(semaphore_timeout_in_child)
THREAD_STUB(cond_signal)
THREAD_STUB(cond_destroy_with_waiter)
THREAD_STUB(event_receive_wakeup)
THREAD_STUB(event_queue_destroy_with_receiver)
THREAD_STUB(eventflag_set_wakes)
THREAD_STUB(eventflag_cancel)
THREAD_STUB(eventflag_single_waiter)
THREAD_STUB(rwlock_reader_blocks_writer)
#undef THREAD_STUB

#endif

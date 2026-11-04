/* Group "cond": sys_cond_*.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4), rpcs3/Emu/Cell/lv2/:
 *   sys_cond.cpp  create :83-120, destroy :122-158, signal :160-230, signal_all :232-312,
 *                 signal_to :314-428, wait :430-617
 *   sys_mutex.cpp destroy :121-124 (a mutex with a condition variable -> CELL_EPERM)
 *   sys_sync.h    lv2_obj::create :311-412
 * Waits are single threaded: the mutex is held, the timeout is finite, so wait ends ETIMEDOUT.
 */
#include "cc.h"

#define BOGUS_ID 0x7FFFFF01u

static int64_t cond_create(uint32_t mutex_id, conf_addr at, uint32_t *id)
{
	conf_addr p = conf_alloc(4, 4);
	int64_t rc;
	conf_w32(p, SENT32);
	rc = sc3(SC_SYS_COND_CREATE, p, mutex_id, at);
	*id = (rc == 0) ? conf_r32(p) : 0;
	return rc;
}

static void cleanup(uint32_t cond, uint32_t mutex)
{
	if (cond)
		(void)sc1(SC_SYS_COND_DESTROY, cond);
	if (mutex)
		(void)sc1(SC_SYS_MUTEX_DESTROY, mutex);
}

void case_cond_create_destroy(void)
{
	uint32_t m = mutex_new(0), c;
	conf_rec_rc("create_rc", cond_create(m, attr_cond(SYNC_NOT_PROCESS_SHARED), &c));
	rec_bool("id_nonzero", c != 0);
	conf_rec_rc("destroy_rc", sc1(SC_SYS_COND_DESTROY, c));
	cleanup(0, m);
}

void case_cond_object_count(void)
{
	uint32_t m = mutex_new(0), c;
	int64_t n0 = obj_count(OBJ_COND);
	(void)cond_create(m, attr_cond(SYNC_NOT_PROCESS_SHARED), &c);
	int64_t n1 = obj_count(OBJ_COND);
	(void)sc1(SC_SYS_COND_DESTROY, c);
	int64_t n2 = obj_count(OBJ_COND);
	rec_bool("count_ok", n0 >= 0);
	conf_rec("delta_create", n1 - n0);
	conf_rec("delta_destroy", n2 - n1);
	cleanup(0, m);
}

/* sys_cond.cpp:89-94: an unknown mutex id -> CELL_ESRCH */
void case_cond_create_bad_mutex(void)
{
	uint32_t c;
	conf_rec_rc("rc", cond_create(BOGUS_ID, attr_cond(SYNC_NOT_PROCESS_SHARED), &c));
	rec_bool("id_zero", c == 0);
	cleanup(c, 0);
}

/* sys_sync.h:314-341 via sys_cond.cpp:105: pshared must be one of the two defined values */
void case_cond_create_pshared_values(void)
{
	static const uint32_t ps[] = { 0, SYNC_NOT_PROCESS_SHARED, 0x300 };
	char key[16] = { 'p', 's', '_', '0', 0 };
	uint32_t m = mutex_new(0);
	unsigned i;
	for (i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
		uint32_t c;
		key[3] = (char)('0' + i);
		conf_rec_rc(key, cond_create(m, attr_cond(ps[i]), &c));
		cleanup(c, 0);
	}
	cleanup(0, m);
}

/* sys_mutex.cpp:121-124: a mutex that still has a condition variable cannot be destroyed (EPERM) */
void case_cond_mutex_destroy_with_cond(void)
{
	uint32_t m = mutex_new(0), c;
	(void)cond_create(m, attr_cond(SYNC_NOT_PROCESS_SHARED), &c);
	conf_rec_rc("mutex_destroy_attached", sc1(SC_SYS_MUTEX_DESTROY, m));
	conf_rec_rc("cond_destroy", sc1(SC_SYS_COND_DESTROY, c));
	conf_rec_rc("mutex_destroy_detached", sc1(SC_SYS_MUTEX_DESTROY, m));
}

/* sys_cond.cpp:143-145: destroying an id twice -> CELL_ESRCH the second time */
void case_cond_destroy_twice(void)
{
	uint32_t m = mutex_new(0), c;
	(void)cond_create(m, attr_cond(SYNC_NOT_PROCESS_SHARED), &c);
	conf_rec_rc("first", sc1(SC_SYS_COND_DESTROY, c));
	conf_rec_rc("second", sc1(SC_SYS_COND_DESTROY, c));
	cleanup(0, m);
}

/* sys_cond.cpp:143-145 and :223-226, :305-308: an unknown id is CELL_ESRCH */
void case_cond_invalid_id(void)
{
	conf_rec_rc("destroy_bogus", sc1(SC_SYS_COND_DESTROY, BOGUS_ID));
	conf_rec_rc("signal_bogus", sc1(SC_SYS_COND_SIGNAL, BOGUS_ID));
	conf_rec_rc("signal_all_bogus", sc1(SC_SYS_COND_SIGNAL_ALL, BOGUS_ID));
	conf_rec_rc("wait_bogus", sc2(SC_SYS_COND_WAIT, BOGUS_ID, WAIT_US));
	conf_rec_rc("signal_to_bogus", sc2(SC_SYS_COND_SIGNAL_TO, BOGUS_ID, BOGUS_ID));
}

/* sys_cond.cpp:228, :310: signalling a condition variable nobody waits on succeeds */
void case_cond_signal_no_waiter(void)
{
	uint32_t m = mutex_new(0), c;
	(void)cond_create(m, attr_cond(SYNC_NOT_PROCESS_SHARED), &c);
	conf_rec_rc("signal", sc1(SC_SYS_COND_SIGNAL, c));
	conf_rec_rc("signal_all", sc1(SC_SYS_COND_SIGNAL_ALL, c));
	cleanup(c, m);
}

/* sys_cond.cpp:407-409: signal_to with a thread id that does not exist -> CELL_ESRCH */
void case_cond_signal_to_bogus_thread(void)
{
	uint32_t m = mutex_new(0), c;
	(void)cond_create(m, attr_cond(SYNC_NOT_PROCESS_SHARED), &c);
	conf_rec_rc("rc", sc2(SC_SYS_COND_SIGNAL_TO, c, BOGUS_ID));
	cleanup(c, m);
}

/* sys_cond.cpp:443-446, :506-508: waiting without owning the mutex -> CELL_EPERM */
void case_cond_wait_not_owner(void)
{
	uint32_t m = mutex_new(0), c;
	(void)cond_create(m, attr_cond(SYNC_NOT_PROCESS_SHARED), &c);
	conf_rec_rc("wait", sc2(SC_SYS_COND_WAIT, c, WAIT_US));
	cleanup(c, m);
}

/* sys_cond.cpp:565-598, :611: a timed wait nobody signals ends CELL_ETIMEDOUT with the mutex owned again */
void case_cond_wait_timeout(void)
{
	uint32_t m = mutex_new(0), c;
	uint64_t t0, t1;
	(void)cond_create(m, attr_cond(SYNC_NOT_PROCESS_SHARED), &c);
	conf_rec_rc("lock", sc2(SC_SYS_MUTEX_LOCK, m, WAIT_US));
	t0 = conf_tb();
	conf_rec_rc("wait", sc2(SC_SYS_COND_WAIT, c, WAIT_US));
	t1 = conf_tb();
	conf_rec_time("wait_ticks", t1 - t0);
	conf_rec_rc("unlock_after_wait", sc1(SC_SYS_MUTEX_UNLOCK, m));
	conf_rec_rc("unlock_extra", sc1(SC_SYS_MUTEX_UNLOCK, m));
	cleanup(c, m);
}

/* same, on a recursive mutex held twice: the count is restored after the wait (sys_cond.cpp:475, :614) */
void case_cond_wait_timeout_recursive(void)
{
	uint32_t m = mutex_new(1), c;
	(void)cond_create(m, attr_cond(SYNC_NOT_PROCESS_SHARED), &c);
	conf_rec_rc("lock1", sc2(SC_SYS_MUTEX_LOCK, m, WAIT_US));
	conf_rec_rc("lock2", sc2(SC_SYS_MUTEX_LOCK, m, WAIT_US));
	conf_rec_rc("wait", sc2(SC_SYS_COND_WAIT, c, WAIT_US));
	conf_rec_rc("unlock1", sc1(SC_SYS_MUTEX_UNLOCK, m));
	conf_rec_rc("unlock2", sc1(SC_SYS_MUTEX_UNLOCK, m));
	conf_rec_rc("unlock3", sc1(SC_SYS_MUTEX_UNLOCK, m));
	cleanup(c, m);
}

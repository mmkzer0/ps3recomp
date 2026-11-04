/* Group "semaphore": sys_semaphore_*.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4),
 * rpcs3/Emu/Cell/lv2/sys_semaphore.cpp: create :33-79, destroy :81-114, wait :116-231,
 * trywait :233-255, post :257-338, get_value :340-365. Every wait is timed; none of these cases
 * needs a second thread.
 */
#include "cc.h"

#define BOGUS_ID 0x7FFFFF01u
/* s32 arguments cross the syscall boundary sign-extended, as a compiled caller passes them */
#define S32(x) ((uint64_t)(int64_t)(int32_t)(x))

static int64_t sem_create(int32_t initial, int32_t max, uint32_t protocol, uint32_t *id)
{
	conf_addr p = conf_alloc(4, 4);
	int64_t rc;
	conf_w32(p, SENT32);
	rc = sc4(SC_SYS_SEMAPHORE_CREATE, p, attr_sema_rw(protocol, SYNC_NOT_PROCESS_SHARED), S32(initial), S32(max));
	*id = (rc == 0) ? conf_r32(p) : 0;
	return rc;
}

static void sem_destroy_quiet(uint32_t id)
{
	if (id)
		(void)sc1(SC_SYS_SEMAPHORE_DESTROY, id);
}

/* semaphore value as sys_semaphore_get_value reports it, or -1 when the call failed */
static int64_t sem_value(uint32_t id)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, SENT32);
	if (sc2(SC_SYS_SEMAPHORE_GET_VALUE, id, p) != 0)
		return -1;
	return (int32_t)conf_r32(p);
}

void case_semaphore_create_destroy(void)
{
	uint32_t id;
	conf_rec_rc("create_rc", sem_create(0, 1, SYNC_PRIO, &id));
	rec_bool("id_nonzero", id != 0);
	conf_rec_rc("destroy_rc", sc1(SC_SYS_SEMAPHORE_DESTROY, id));
}

void case_semaphore_object_count(void)
{
	uint32_t id;
	int64_t n0 = obj_count(OBJ_SEMAPHORE);
	(void)sem_create(0, 1, SYNC_PRIO, &id);
	int64_t n1 = obj_count(OBJ_SEMAPHORE);
	sem_destroy_quiet(id);
	int64_t n2 = obj_count(OBJ_SEMAPHORE);
	rec_bool("count_ok", n0 >= 0);
	conf_rec("delta_create", n1 - n0);
	conf_rec("delta_destroy", n2 - n1);
}

/* sys_semaphore.cpp:44-48: max <= 0, initial < 0 or initial > max -> CELL_EINVAL */
void case_semaphore_create_params(void)
{
	static const int32_t ini[] = { 0, 1, 2, 0, -1, 0, 3, 1 };
	static const int32_t mx[] = { 1, 1, 1, 0, 1, -1, 3, 0x7FFFFFFF };
	char key[16] = { 'p', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(ini) / sizeof(ini[0]); i++) {
		uint32_t id;
		key[2] = (char)('0' + i);
		conf_rec_rc(key, sem_create(ini[i], mx[i], SYNC_PRIO, &id));
		sem_destroy_quiet(id);
	}
}

/* sys_semaphore.cpp:52-58: protocol FIFO or PRIORITY, anything else EINVAL */
void case_semaphore_create_protocols(void)
{
	static const uint32_t protos[] = { 0, SYNC_FIFO, SYNC_PRIO, SYNC_PRIO_INHERIT, SYNC_RETRY };
	char key[16] = { 'p', 'r', 'o', 't', 'o', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(protos) / sizeof(protos[0]); i++) {
		uint32_t id;
		key[6] = (char)('0' + i);
		conf_rec_rc(key, sem_create(0, 1, protos[i], &id));
		sem_destroy_quiet(id);
	}
}

/* sys_sync.h:314-341: pshared must be PROCESS_SHARED or NOT_PROCESS_SHARED */
void case_semaphore_create_pshared_values(void)
{
	static const uint32_t ps[] = { 0, SYNC_NOT_PROCESS_SHARED, 0x300 };
	char key[16] = { 'p', 's', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
		conf_addr p = conf_alloc(4, 4);
		conf_w32(p, SENT32);
		key[3] = (char)('0' + i);
		conf_rec_rc(key, sc4(SC_SYS_SEMAPHORE_CREATE, p, attr_sema_rw(SYNC_PRIO, ps[i]), 0, 1));
		if (conf_r32(p) != SENT32)
			sem_destroy_quiet(conf_r32(p));
	}
}

/* sys_semaphore.cpp:39-42: sem_id == NULL -> CELL_EFAULT */
void case_semaphore_create_null_id(void)
{
	conf_rec_rc("rc", sc4(SC_SYS_SEMAPHORE_CREATE, 0, attr_sema_rw(SYNC_PRIO, SYNC_NOT_PROCESS_SHARED), 0, 1));
}

/* sys_semaphore.cpp:39-42: attr == NULL -> CELL_EFAULT */
void case_semaphore_create_null_attr(void)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, SENT32);
	conf_rec_rc("rc", sc4(SC_SYS_SEMAPHORE_CREATE, p, 0, 0, 1));
	rec_bool("id_untouched", conf_r32(p) == SENT32);
}

/* sys_semaphore.cpp:249-252: trywait on a zero semaphore -> CELL_EBUSY */
void case_semaphore_trywait_zero(void)
{
	uint32_t id;
	(void)sem_create(0, 1, SYNC_PRIO, &id);
	conf_rec_rc("trywait", sc1(SC_SYS_SEMAPHORE_TRYWAIT, id));
	conf_rec("value", sem_value(id));
	sem_destroy_quiet(id);
}

/* trywait decrements until the value is zero (sys_semaphore.cpp:241) */
void case_semaphore_trywait_counts_down(void)
{
	uint32_t id;
	(void)sem_create(2, 5, SYNC_PRIO, &id);
	conf_rec_rc("t1", sc1(SC_SYS_SEMAPHORE_TRYWAIT, id));
	conf_rec("v1", sem_value(id));
	conf_rec_rc("t2", sc1(SC_SYS_SEMAPHORE_TRYWAIT, id));
	conf_rec_rc("t3", sc1(SC_SYS_SEMAPHORE_TRYWAIT, id));
	conf_rec("v3", sem_value(id));
	sem_destroy_quiet(id);
}

/* sys_semaphore.cpp:124-131, :193-221: wait on an available semaphore returns at once; on a zero
 * one the finite timeout ends with CELL_ETIMEDOUT and the value is unchanged */
void case_semaphore_wait(void)
{
	uint32_t id;
	uint64_t t0, t1;
	(void)sem_create(1, 1, SYNC_PRIO, &id);
	conf_rec_rc("wait_available", sc2(SC_SYS_SEMAPHORE_WAIT, id, WAIT_US));
	conf_rec("value_after", sem_value(id));
	t0 = conf_tb();
	conf_rec_rc("wait_empty", sc2(SC_SYS_SEMAPHORE_WAIT, id, WAIT_US));
	t1 = conf_tb();
	conf_rec_time("wait_ticks", t1 - t0);
	conf_rec("value_after_timeout", sem_value(id));
	conf_rec_rc("destroy_after_timeout", sc1(SC_SYS_SEMAPHORE_DESTROY, id));
}

/* post raises the value (sys_semaphore.cpp:263-292); get_value reports it (:346-363) */
void case_semaphore_post_get_value(void)
{
	uint32_t id;
	(void)sem_create(0, 3, SYNC_PRIO, &id);
	conf_rec_rc("post2", sc2(SC_SYS_SEMAPHORE_POST, id, 2));
	conf_rec("value_2", sem_value(id));
	conf_rec_rc("post1", sc2(SC_SYS_SEMAPHORE_POST, id, 1));
	conf_rec("value_3", sem_value(id));
	sem_destroy_quiet(id);
}

/* sys_semaphore.cpp:267, :318-321: post past max leaves the value alone and returns CELL_EBUSY */
void case_semaphore_post_past_max(void)
{
	uint32_t id;
	(void)sem_create(1, 2, SYNC_PRIO, &id);
	conf_rec_rc("post2", sc2(SC_SYS_SEMAPHORE_POST, id, 2));
	conf_rec("value_after_fail", sem_value(id));
	conf_rec_rc("post1", sc2(SC_SYS_SEMAPHORE_POST, id, 1));
	conf_rec_rc("post1_full", sc2(SC_SYS_SEMAPHORE_POST, id, 1));
	conf_rec("value_final", sem_value(id));
	sem_destroy_quiet(id);
}

/* sys_semaphore.cpp:283-286: count <= 0 -> CELL_EINVAL (after the id lookup) */
void case_semaphore_post_bad_count(void)
{
	uint32_t id;
	(void)sem_create(0, 3, SYNC_PRIO, &id);
	conf_rec_rc("post0", sc2(SC_SYS_SEMAPHORE_POST, id, 0));
	conf_rec_rc("post_neg", sc2(SC_SYS_SEMAPHORE_POST, id, S32(-1)));
	conf_rec_rc("post0_bogus_id", sc2(SC_SYS_SEMAPHORE_POST, BOGUS_ID, 0));
	conf_rec("value", sem_value(id));
	sem_destroy_quiet(id);
}

/* sys_semaphore.cpp:356-359: count == NULL -> CELL_EFAULT */
void case_semaphore_get_value_null(void)
{
	uint32_t id;
	(void)sem_create(1, 2, SYNC_PRIO, &id);
	conf_rec_rc("rc", sc2(SC_SYS_SEMAPHORE_GET_VALUE, id, 0));
	sem_destroy_quiet(id);
}

/* sys_semaphore.cpp:351-354, :244-247, :278-281, :98-101, :148-151: unknown id -> CELL_ESRCH */
void case_semaphore_invalid_id(void)
{
	conf_addr p = conf_alloc(4, 4);
	conf_rec_rc("destroy", sc1(SC_SYS_SEMAPHORE_DESTROY, BOGUS_ID));
	conf_rec_rc("wait", sc2(SC_SYS_SEMAPHORE_WAIT, BOGUS_ID, WAIT_US));
	conf_rec_rc("trywait", sc1(SC_SYS_SEMAPHORE_TRYWAIT, BOGUS_ID));
	conf_rec_rc("post", sc2(SC_SYS_SEMAPHORE_POST, BOGUS_ID, 1));
	conf_rec_rc("get_value", sc2(SC_SYS_SEMAPHORE_GET_VALUE, BOGUS_ID, p));
}

void case_semaphore_destroy_twice(void)
{
	uint32_t id;
	(void)sem_create(0, 1, SYNC_PRIO, &id);
	conf_rec_rc("first", sc1(SC_SYS_SEMAPHORE_DESTROY, id));
	conf_rec_rc("second", sc1(SC_SYS_SEMAPHORE_DESTROY, id));
}

/* Group "mutex": sys_mutex_*.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4), rpcs3/Emu/Cell/lv2/:
 *   sys_mutex.cpp   create :38-104, destroy :106-146, lock :148-310, trylock :312-339, unlock :341-387
 *   sys_mutex.h     try_lock :65-99 (recursion, EDEADLK), try_unlock :132-159 (EPERM when not owner)
 *   sys_sync.h      lv2_obj::create :311-412 (pshared / flags validation)
 * Every lock call carries a finite timeout; none of these cases can block on a second thread.
 */
#include "cc.h"

#define BOGUS_ID 0x7FFFFF01u

/* sys_mutex_create with an attribute, returns the rc and the id (0 when failed) */
static int64_t create_with(conf_addr at, uint32_t *id)
{
	conf_addr p = conf_alloc(4, 4);
	int64_t rc;
	conf_w32(p, SENT32);
	rc = sc2(SC_SYS_MUTEX_CREATE, p, at);
	*id = (rc == 0) ? conf_r32(p) : 0;
	return rc;
}

static void destroy_quiet(uint32_t id)
{
	if (id)
		(void)sc1(SC_SYS_MUTEX_DESTROY, id);
}

void case_mutex_create_destroy(void)
{
	uint32_t id;
	int64_t rc = create_with(attr_mutex(SYNC_PRIO, SYNC_NOT_RECURSIVE, SYNC_NOT_PROCESS_SHARED, SYNC_NOT_ADAPTIVE), &id);
	conf_rec_rc("create_rc", rc);
	rec_bool("id_nonzero", id != 0);
	conf_rec_rc("destroy_rc", sc1(SC_SYS_MUTEX_DESTROY, id));
}

void case_mutex_create_two_distinct(void)
{
	uint32_t a, b;
	conf_rec_rc("rc_a", create_with(attr_mutex(SYNC_FIFO, SYNC_RECURSIVE, SYNC_NOT_PROCESS_SHARED, SYNC_NOT_ADAPTIVE), &a));
	conf_rec_rc("rc_b", create_with(attr_mutex(SYNC_FIFO, SYNC_RECURSIVE, SYNC_NOT_PROCESS_SHARED, SYNC_NOT_ADAPTIVE), &b));
	rec_bool("ids_differ", a != b);
	destroy_quiet(a);
	destroy_quiet(b);
}

/* the live-object count (sys_process_get_number_of_object) moves by one with create and destroy */
void case_mutex_object_count(void)
{
	int64_t n0 = obj_count(OBJ_MUTEX);
	uint32_t id = mutex_new(0);
	int64_t n1 = obj_count(OBJ_MUTEX);
	destroy_quiet(id);
	int64_t n2 = obj_count(OBJ_MUTEX);
	rec_bool("count_ok", n0 >= 0);
	conf_rec("delta_create", n1 - n0);
	conf_rec("delta_destroy", n2 - n1);
}

/* sys_mutex.cpp:58-70: protocol FIFO, PRIORITY, PRIORITY_INHERIT pass; anything else EINVAL */
void case_mutex_create_protocols(void)
{
	static const uint32_t protos[] = { 0, SYNC_FIFO, SYNC_PRIO, SYNC_PRIO_INHERIT, SYNC_RETRY, 5, 0x100 };
	char key[16] = { 'p', 'r', 'o', 't', 'o', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(protos) / sizeof(protos[0]); i++) {
		uint32_t id;
		key[6] = (char)('0' + i);
		conf_rec_rc(key, create_with(attr_mutex(protos[i], SYNC_NOT_RECURSIVE, SYNC_NOT_PROCESS_SHARED, SYNC_NOT_ADAPTIVE), &id));
		destroy_quiet(id);
	}
}

/* sys_mutex.cpp:72-81: recursive values 0x10 and 0x20 pass; anything else EINVAL */
void case_mutex_create_recursive_values(void)
{
	static const uint32_t rec[] = { 0, SYNC_RECURSIVE, SYNC_NOT_RECURSIVE, 0x30, 1 };
	char key[16] = { 'r', 'e', 'c', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(rec) / sizeof(rec[0]); i++) {
		uint32_t id;
		key[4] = (char)('0' + i);
		conf_rec_rc(key, create_with(attr_mutex(SYNC_PRIO, rec[i], SYNC_NOT_PROCESS_SHARED, SYNC_NOT_ADAPTIVE), &id));
		destroy_quiet(id);
	}
}

/* sys_sync.h:314-341: pshared must be SYS_SYNC_PROCESS_SHARED or SYS_SYNC_NOT_PROCESS_SHARED */
void case_mutex_create_pshared_values(void)
{
	static const uint32_t ps[] = { 0, SYNC_NOT_PROCESS_SHARED, 0x300, 1 };
	char key[16] = { 'p', 's', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
		uint32_t id;
		key[3] = (char)('0' + i);
		conf_rec_rc(key, create_with(attr_mutex(SYNC_PRIO, SYNC_NOT_RECURSIVE, ps[i], SYNC_NOT_ADAPTIVE), &id));
		destroy_quiet(id);
	}
}

/* sys_mutex.cpp:83-86: an adaptive value other than NOT_ADAPTIVE only logs a todo */
void case_mutex_create_adaptive_values(void)
{
	static const uint32_t ad[] = { 0, SYNC_ADAPTIVE, SYNC_NOT_ADAPTIVE };
	char key[16] = { 'a', 'd', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(ad) / sizeof(ad[0]); i++) {
		uint32_t id;
		key[3] = (char)('0' + i);
		conf_rec_rc(key, create_with(attr_mutex(SYNC_PRIO, SYNC_NOT_RECURSIVE, SYNC_NOT_PROCESS_SHARED, ad[i]), &id));
		destroy_quiet(id);
	}
}

/* sys_mutex.cpp:44-47: mutex_id == NULL -> CELL_EFAULT */
void case_mutex_create_null_id(void)
{
	conf_rec_rc("rc", sc2(SC_SYS_MUTEX_CREATE, 0,
	                      attr_mutex(SYNC_PRIO, SYNC_NOT_RECURSIVE, SYNC_NOT_PROCESS_SHARED, SYNC_NOT_ADAPTIVE)));
}

/* sys_mutex.cpp:44-47: attr == NULL -> CELL_EFAULT; the id word is left alone */
void case_mutex_create_null_attr(void)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, SENT32);
	conf_rec_rc("rc", sc2(SC_SYS_MUTEX_CREATE, p, 0));
	rec_bool("id_untouched", conf_r32(p) == SENT32);
}

void case_mutex_lock_unlock(void)
{
	uint32_t id = mutex_new(0);
	conf_rec_rc("lock", sc2(SC_SYS_MUTEX_LOCK, id, WAIT_US));
	conf_rec_rc("unlock", sc1(SC_SYS_MUTEX_UNLOCK, id));
	conf_rec_rc("lock_again", sc2(SC_SYS_MUTEX_LOCK, id, WAIT_US));
	conf_rec_rc("unlock_again", sc1(SC_SYS_MUTEX_UNLOCK, id));
	destroy_quiet(id);
}

/* sys_mutex.h:81-95 + sys_mutex.cpp:202: owner relocking a non-recursive mutex -> CELL_EDEADLK */
void case_mutex_lock_relock_nonrecursive(void)
{
	uint32_t id = mutex_new(0);
	conf_rec_rc("lock", sc2(SC_SYS_MUTEX_LOCK, id, WAIT_US));
	conf_rec_rc("relock", sc2(SC_SYS_MUTEX_LOCK, id, WAIT_US));
	conf_rec_rc("unlock", sc1(SC_SYS_MUTEX_UNLOCK, id));
	conf_rec_rc("unlock_extra", sc1(SC_SYS_MUTEX_UNLOCK, id));
	destroy_quiet(id);
}

/* sys_mutex.h:95, sys_mutex.cpp:335: trylock by the owner of a non-recursive mutex -> CELL_EDEADLK */
void case_mutex_trylock_relock_nonrecursive(void)
{
	uint32_t id = mutex_new(0);
	conf_rec_rc("trylock", sc1(SC_SYS_MUTEX_TRYLOCK, id));
	conf_rec_rc("trylock_again", sc1(SC_SYS_MUTEX_TRYLOCK, id));
	conf_rec_rc("unlock", sc1(SC_SYS_MUTEX_UNLOCK, id));
	destroy_quiet(id);
}

/* sys_mutex.h:84-92 and :141-145: a recursive mutex counts locks; the extra unlock is EPERM */
void case_mutex_recursive_count(void)
{
	uint32_t id = mutex_new(1);
	conf_rec_rc("lock1", sc2(SC_SYS_MUTEX_LOCK, id, WAIT_US));
	conf_rec_rc("lock2", sc2(SC_SYS_MUTEX_LOCK, id, WAIT_US));
	conf_rec_rc("trylock3", sc1(SC_SYS_MUTEX_TRYLOCK, id));
	conf_rec_rc("unlock1", sc1(SC_SYS_MUTEX_UNLOCK, id));
	conf_rec_rc("unlock2", sc1(SC_SYS_MUTEX_UNLOCK, id));
	conf_rec_rc("destroy_while_held", sc1(SC_SYS_MUTEX_DESTROY, id)); /* one lock still counted: EBUSY (:118) */
	conf_rec_rc("unlock3", sc1(SC_SYS_MUTEX_UNLOCK, id));
	conf_rec_rc("unlock4", sc1(SC_SYS_MUTEX_UNLOCK, id));
	destroy_quiet(id);
}

/* sys_mutex.h:136-139: unlock by a thread that does not own the mutex -> CELL_EPERM */
void case_mutex_unlock_not_owner(void)
{
	uint32_t id = mutex_new(0);
	conf_rec_rc("unlock_unlocked", sc1(SC_SYS_MUTEX_UNLOCK, id));
	destroy_quiet(id);
}

/* sys_mutex.cpp:118-119: destroying a locked mutex -> CELL_EBUSY; after the unlock it works */
void case_mutex_destroy_locked(void)
{
	uint32_t id = mutex_new(0);
	conf_rec_rc("lock", sc2(SC_SYS_MUTEX_LOCK, id, WAIT_US));
	conf_rec_rc("destroy_locked", sc1(SC_SYS_MUTEX_DESTROY, id));
	conf_rec_rc("unlock", sc1(SC_SYS_MUTEX_UNLOCK, id));
	conf_rec_rc("destroy", sc1(SC_SYS_MUTEX_DESTROY, id));
}

/* sys_mutex.cpp:130-133: destroying an id twice -> CELL_ESRCH the second time */
void case_mutex_destroy_twice(void)
{
	uint32_t id = mutex_new(0);
	conf_rec_rc("first", sc1(SC_SYS_MUTEX_DESTROY, id));
	conf_rec_rc("second", sc1(SC_SYS_MUTEX_DESTROY, id));
}

/* sys_mutex.cpp:130-133, :195-198, :323-326, :376-379: an unknown id is CELL_ESRCH everywhere */
void case_mutex_invalid_id(void)
{
	conf_rec_rc("lock_bogus", sc2(SC_SYS_MUTEX_LOCK, BOGUS_ID, WAIT_US));
	conf_rec_rc("trylock_bogus", sc1(SC_SYS_MUTEX_TRYLOCK, BOGUS_ID));
	conf_rec_rc("unlock_bogus", sc1(SC_SYS_MUTEX_UNLOCK, BOGUS_ID));
	conf_rec_rc("destroy_bogus", sc1(SC_SYS_MUTEX_DESTROY, BOGUS_ID));
	conf_rec_rc("lock_zero", sc2(SC_SYS_MUTEX_LOCK, 0, WAIT_US));
	conf_rec_rc("destroy_zero", sc1(SC_SYS_MUTEX_DESTROY, 0));
}

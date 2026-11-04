/* Group "rwlock": sys_rwlock_*.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4),
 * rpcs3/Emu/Cell/lv2/sys_rwlock.cpp: create :32-66, destroy :68-96, rlock :98-224,
 * tryrlock :226-259, runlock :261-335, wlock :337-489, trywlock :491-521, wunlock :523-593.
 * Number 126 is sys_rwlock_trywlock; 148 is _sys_rwlock_trywlock, an alias of it in RPCS3
 * (sys_rwlock.h:61, lv2.cpp:262 and :284). The single thread holds the locks itself, so a second
 * lock call that would wait runs into its finite timeout.
 */
#include "cc.h"

#define BOGUS_ID 0x7FFFFF01u

static int64_t rw_create(uint32_t protocol, uint32_t pshared, uint32_t *id)
{
	conf_addr p = conf_alloc(4, 4);
	int64_t rc;
	conf_w32(p, SENT32);
	rc = sc2(SC_SYS_RWLOCK_CREATE, p, attr_sema_rw(protocol, pshared));
	*id = (rc == 0) ? conf_r32(p) : 0;
	return rc;
}

static uint32_t rw_new(void)
{
	uint32_t id;
	(void)rw_create(SYNC_PRIO, SYNC_NOT_PROCESS_SHARED, &id);
	return id;
}

static void rw_destroy_quiet(uint32_t id)
{
	if (id)
		(void)sc1(SC_SYS_RWLOCK_DESTROY, id);
}

void case_rwlock_create_destroy(void)
{
	uint32_t id;
	conf_rec_rc("create_rc", rw_create(SYNC_PRIO, SYNC_NOT_PROCESS_SHARED, &id));
	rec_bool("id_nonzero", id != 0);
	conf_rec_rc("destroy_rc", sc1(SC_SYS_RWLOCK_DESTROY, id));
}

void case_rwlock_object_count(void)
{
	int64_t n0 = obj_count(OBJ_RWLOCK);
	uint32_t id = rw_new();
	int64_t n1 = obj_count(OBJ_RWLOCK);
	rw_destroy_quiet(id);
	int64_t n2 = obj_count(OBJ_RWLOCK);
	rec_bool("count_ok", n0 >= 0);
	conf_rec("delta_create", n1 - n0);
	conf_rec("delta_destroy", n2 - n1);
}

/* sys_rwlock.cpp:45-51: protocol FIFO or PRIORITY, anything else EINVAL */
void case_rwlock_create_protocols(void)
{
	static const uint32_t protos[] = { 0, SYNC_FIFO, SYNC_PRIO, SYNC_PRIO_INHERIT, SYNC_RETRY };
	char key[16] = { 'p', 'r', 'o', 't', 'o', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(protos) / sizeof(protos[0]); i++) {
		uint32_t id;
		key[6] = (char)('0' + i);
		conf_rec_rc(key, rw_create(protos[i], SYNC_NOT_PROCESS_SHARED, &id));
		rw_destroy_quiet(id);
	}
}

/* sys_rwlock.cpp:38-41: rw_lock_id == NULL or attr == NULL -> CELL_EFAULT */
void case_rwlock_create_null_args(void)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, SENT32);
	conf_rec_rc("null_id", sc2(SC_SYS_RWLOCK_CREATE, 0, attr_sema_rw(SYNC_PRIO, SYNC_NOT_PROCESS_SHARED)));
	conf_rec_rc("null_attr", sc2(SC_SYS_RWLOCK_CREATE, p, 0));
	rec_bool("id_untouched", conf_r32(p) == SENT32);
}

/* sys_rwlock.cpp:226-258, :261-292: any number of readers; the fourth unlock has nothing to release
 * (sys_rwlock.cpp:306-309, CELL_EPERM) */
void case_rwlock_readers_stack(void)
{
	uint32_t id = rw_new();
	conf_rec_rc("tryr1", sc1(SC_SYS_RWLOCK_TRYRLOCK, id));
	conf_rec_rc("tryr2", sc1(SC_SYS_RWLOCK_TRYRLOCK, id));
	conf_rec_rc("r3", sc2(SC_SYS_RWLOCK_RLOCK, id, WAIT_US));
	conf_rec_rc("unlock1", sc1(SC_SYS_RWLOCK_RUNLOCK, id));
	conf_rec_rc("unlock2", sc1(SC_SYS_RWLOCK_RUNLOCK, id));
	conf_rec_rc("destroy_while_read", sc1(SC_SYS_RWLOCK_DESTROY, id));
	conf_rec_rc("unlock3", sc1(SC_SYS_RWLOCK_RUNLOCK, id));
	conf_rec_rc("unlock4", sc1(SC_SYS_RWLOCK_RUNLOCK, id));
	rw_destroy_quiet(id);
}

/* sys_rwlock.cpp:497-520 and :523-546: a writer excludes a try-reader and a try-writer;
 * the owner trying again is CELL_EDEADLK (:512-515) */
void case_rwlock_trywlock_exclusive(void)
{
	uint32_t id = rw_new();
	conf_rec_rc("trywlock", sc1(SC_SYS_RWLOCK_TRYWLOCK, id));
	conf_rec_rc("trywlock_again", sc1(SC_SYS_RWLOCK_TRYWLOCK, id));
	conf_rec_rc("tryrlock", sc1(SC_SYS_RWLOCK_TRYRLOCK, id));
	conf_rec_rc("destroy_while_write", sc1(SC_SYS_RWLOCK_DESTROY, id));
	conf_rec_rc("wunlock", sc1(SC_SYS_RWLOCK_WUNLOCK, id));
	conf_rec_rc("wunlock_again", sc1(SC_SYS_RWLOCK_WUNLOCK, id));
	rw_destroy_quiet(id);
}

/* number 148 (_sys_rwlock_trywlock) must behave as number 126 */
void case_rwlock_trywlock_slot_148(void)
{
	uint32_t id = rw_new();
	conf_rec_rc("trywlock_148", sc1(SC__SYS_RWLOCK_TRYWLOCK, id));
	conf_rec_rc("trywlock_126", sc1(SC_SYS_RWLOCK_TRYWLOCK, id));
	conf_rec_rc("tryrlock", sc1(SC_SYS_RWLOCK_TRYRLOCK, id));
	conf_rec_rc("wunlock", sc1(SC_SYS_RWLOCK_WUNLOCK, id));
	conf_rec_rc("trywlock_148_free", sc1(SC__SYS_RWLOCK_TRYWLOCK, id));
	conf_rec_rc("wunlock_second", sc1(SC_SYS_RWLOCK_WUNLOCK, id));
	rw_destroy_quiet(id);
}

/* sys_rwlock.cpp:394-397: wlock by the writer itself -> CELL_EDEADLK, without waiting */
void case_rwlock_wlock_relock(void)
{
	uint32_t id = rw_new();
	conf_rec_rc("wlock", sc2(SC_SYS_RWLOCK_WLOCK, id, WAIT_US));
	conf_rec_rc("wlock_again", sc2(SC_SYS_RWLOCK_WLOCK, id, WAIT_US));
	conf_rec_rc("wunlock", sc1(SC_SYS_RWLOCK_WUNLOCK, id));
	rw_destroy_quiet(id);
}

/* sys_rwlock.cpp:104-140, :187-215: a reader that cannot enter while the writer is this thread
 * waits out the timeout */
void case_rwlock_rlock_under_writer(void)
{
	uint32_t id = rw_new();
	conf_rec_rc("wlock", sc2(SC_SYS_RWLOCK_WLOCK, id, WAIT_US));
	conf_rec_rc("rlock", sc2(SC_SYS_RWLOCK_RLOCK, id, WAIT_US));
	conf_rec_rc("wunlock", sc1(SC_SYS_RWLOCK_WUNLOCK, id));
	conf_rec_rc("destroy", sc1(SC_SYS_RWLOCK_DESTROY, id));
}

/* sys_rwlock.cpp:363-381, :434-479: a writer behind this thread's own read lock waits out the timeout
 * and leaves the lock usable (:473-476) */
void case_rwlock_wlock_under_reader(void)
{
	uint32_t id = rw_new();
	conf_rec_rc("rlock", sc2(SC_SYS_RWLOCK_RLOCK, id, WAIT_US));
	conf_rec_rc("wlock", sc2(SC_SYS_RWLOCK_WLOCK, id, WAIT_US));
	conf_rec_rc("runlock", sc1(SC_SYS_RWLOCK_RUNLOCK, id));
	conf_rec_rc("trywlock_after", sc1(SC_SYS_RWLOCK_TRYWLOCK, id));
	conf_rec_rc("wunlock", sc1(SC_SYS_RWLOCK_WUNLOCK, id));
	rw_destroy_quiet(id);
}

/* sys_rwlock.cpp:497-518, :226-258: a reader blocks try-writers */
void case_rwlock_trywlock_under_reader(void)
{
	uint32_t id = rw_new();
	conf_rec_rc("tryrlock", sc1(SC_SYS_RWLOCK_TRYRLOCK, id));
	conf_rec_rc("trywlock", sc1(SC_SYS_RWLOCK_TRYWLOCK, id));
	conf_rec_rc("wunlock", sc1(SC_SYS_RWLOCK_WUNLOCK, id));
	conf_rec_rc("runlock", sc1(SC_SYS_RWLOCK_RUNLOCK, id));
	rw_destroy_quiet(id);
}

/* sys_rwlock.cpp:306-309: runlock without a read lock -> CELL_EPERM (unlocked, and write locked) */
void case_rwlock_runlock_not_held(void)
{
	uint32_t id = rw_new();
	conf_rec_rc("runlock_unlocked", sc1(SC_SYS_RWLOCK_RUNLOCK, id));
	conf_rec_rc("wlock", sc2(SC_SYS_RWLOCK_WLOCK, id, WAIT_US));
	conf_rec_rc("runlock_write_locked", sc1(SC_SYS_RWLOCK_RUNLOCK, id));
	conf_rec_rc("wunlock", sc1(SC_SYS_RWLOCK_WUNLOCK, id));
	rw_destroy_quiet(id);
}

/* sys_rwlock.cpp:542-545: wunlock by a thread that is not the writer -> CELL_EPERM */
void case_rwlock_wunlock_not_held(void)
{
	uint32_t id = rw_new();
	conf_rec_rc("wunlock_unlocked", sc1(SC_SYS_RWLOCK_WUNLOCK, id));
	conf_rec_rc("rlock", sc2(SC_SYS_RWLOCK_RLOCK, id, WAIT_US));
	conf_rec_rc("wunlock_read_locked", sc1(SC_SYS_RWLOCK_WUNLOCK, id));
	conf_rec_rc("runlock", sc1(SC_SYS_RWLOCK_RUNLOCK, id));
	rw_destroy_quiet(id);
}

/* sys_rwlock.cpp:76-79: destroy while locked -> CELL_EBUSY, after the unlock it works */
void case_rwlock_destroy_locked(void)
{
	uint32_t id = rw_new();
	conf_rec_rc("wlock", sc2(SC_SYS_RWLOCK_WLOCK, id, WAIT_US));
	conf_rec_rc("destroy_locked", sc1(SC_SYS_RWLOCK_DESTROY, id));
	conf_rec_rc("wunlock", sc1(SC_SYS_RWLOCK_WUNLOCK, id));
	conf_rec_rc("destroy", sc1(SC_SYS_RWLOCK_DESTROY, id));
	conf_rec_rc("destroy_twice", sc1(SC_SYS_RWLOCK_DESTROY, id));
}

/* sys_rwlock.cpp:85-88, :142-145, :248-251, :282-285, :384-387, :505-508, :537-540: unknown id -> ESRCH */
void case_rwlock_invalid_id(void)
{
	conf_rec_rc("destroy", sc1(SC_SYS_RWLOCK_DESTROY, BOGUS_ID));
	conf_rec_rc("rlock", sc2(SC_SYS_RWLOCK_RLOCK, BOGUS_ID, WAIT_US));
	conf_rec_rc("tryrlock", sc1(SC_SYS_RWLOCK_TRYRLOCK, BOGUS_ID));
	conf_rec_rc("runlock", sc1(SC_SYS_RWLOCK_RUNLOCK, BOGUS_ID));
	conf_rec_rc("wlock", sc2(SC_SYS_RWLOCK_WLOCK, BOGUS_ID, WAIT_US));
	conf_rec_rc("trywlock", sc1(SC_SYS_RWLOCK_TRYWLOCK, BOGUS_ID));
	conf_rec_rc("wunlock", sc1(SC_SYS_RWLOCK_WUNLOCK, BOGUS_ID));
	conf_rec_rc("trywlock_148", sc1(SC__SYS_RWLOCK_TRYWLOCK, BOGUS_ID));
}

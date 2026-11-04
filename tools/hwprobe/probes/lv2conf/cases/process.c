/* Group "process": process identity and the kernel object listing.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4), rpcs3/Emu/Cell/lv2/sys_process.cpp:
 *   getpid :62-72 (RPCS3 answers a constant 1, marked TODO)   getppid :74-78
 *   get_number_of_object :99-132   get_id :153-208   get_sdk_version :308-331
 * The pid itself is never recorded, only relations to it.
 */
#include "cc.h"

#define BOGUS_OBJECT 0xFFFFu

void case_process_getpid_stable(void)
{
	int64_t a = sc0(SC_SYS_PROCESS_GETPID);
	int64_t b = sc0(SC_SYS_PROCESS_GETPID);
	rec_bool("nonzero", (uint32_t)a != 0);
	rec_bool("positive", a > 0);
	rec_bool("stable", a == b);
}

/* the parent is another process */
void case_process_getppid(void)
{
	int64_t pid = sc0(SC_SYS_PROCESS_GETPID);
	int64_t ppid = sc0(SC_SYS_PROCESS_GETPPID);
	rec_bool("differs_from_pid", ppid != pid);
}

/* sys_process.cpp:308-331: the SDK version of the calling process is reported through a word */
void case_process_get_sdk_version(void)
{
	conf_addr p = conf_alloc(4, 4);
	int64_t pid = sc0(SC_SYS_PROCESS_GETPID);
	conf_w32(p, SENT32);
	conf_rec_rc("rc", sc2(SC_SYS_PROCESS_GET_SDK_VERSION, (uint64_t)(uint32_t)pid, p));
	rec_bool("written", conf_r32(p) != SENT32);
	rec_bool("nonzero", conf_r32(p) != SENT32 && conf_r32(p) != 0);
}

/* sys_process.cpp:125-128: an unknown object type -> CELL_EINVAL; a known one reports a count */
void case_process_object_count(void)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, SENT32);
	conf_rec_rc("unknown_type", sc2(SC_SYS_PROCESS_GET_NUMBER_OF_OBJECT, BOGUS_OBJECT, p));
	rec_bool("unknown_untouched", conf_r32(p) == SENT32);
	conf_rec_rc("mutex_type", sc2(SC_SYS_PROCESS_GET_NUMBER_OF_OBJECT, OBJ_MUTEX, p));
	rec_bool("mutex_written", conf_r32(p) != SENT32);
}

/* sys_process.cpp:184-194: get_id lists the ids of one object type; ours is among them, and the
 * listed count never exceeds what get_number_of_object reports for the same moment */
void case_process_get_id_lists_mutex(void)
{
	uint32_t id = mutex_new(0);
	conf_addr buf = conf_alloc(4 * 256, 4);
	conf_addr sz = conf_alloc(4, 4);
	uint32_t n, i;
	int found = 0;
	int64_t count = obj_count(OBJ_MUTEX);
	conf_w32(sz, SENT32);
	conf_rec_rc("rc", sc4(SC_SYS_PROCESS_GET_ID, OBJ_MUTEX, buf, 256, sz));
	n = conf_r32(sz);
	for (i = 0; n != SENT32 && i < n && i < 256; i++)
		if (conf_r32(buf + 4 * i) == id)
			found = 1;
	rec_bool("set_size_written", n != SENT32);
	rec_bool("set_size_matches_count", n != SENT32 && (int64_t)n == count);
	rec_bool("contains_ours", found);
	(void)sc1(SC_SYS_MUTEX_DESTROY, id);
}

/* sys_process.cpp:178-181: an unknown object type -> CELL_EINVAL */
void case_process_get_id_unknown_type(void)
{
	conf_addr buf = conf_alloc(16, 4);
	conf_addr sz = conf_alloc(4, 4);
	conf_w32(sz, SENT32);
	conf_rec_rc("rc", sc4(SC_SYS_PROCESS_GET_ID, BOGUS_OBJECT, buf, 4, sz));
	rec_bool("size_untouched", conf_r32(sz) == SENT32);
}

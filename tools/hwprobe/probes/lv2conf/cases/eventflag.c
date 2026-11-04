/* Group "eventflag": sys_event_flag_*.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4), rpcs3/Emu/Cell/lv2/:
 *   sys_event_flag.cpp  create :48-95, destroy :97-125, wait :127-270, trywait :272-308,
 *                       set :310-460, clear :462-479, cancel :481-534, get :536-562
 *   sys_event_flag.h    constants :7-17, attribute struct :19-32, check_mode :61-79, check_pattern :82-108
 * Waits are timed; a waiter that nobody sets runs into its timeout.
 */
#include "cc.h"

#define BOGUS_ID 0x7FFFFF01u
#define WAITER_SINGLE 0x10000u
#define WAITER_MULTIPLE 0x20000u
#define WAIT_AND 0x01u
#define WAIT_OR 0x02u
#define WAIT_CLEAR 0x10u
#define WAIT_CLEAR_ALL 0x20u

static int64_t ef_create(uint32_t protocol, uint32_t type, uint64_t init, uint32_t *id)
{
	conf_addr p = conf_alloc(4, 4);
	int64_t rc;
	conf_w32(p, SENT32);
	rc = sc3(SC_SYS_EVENT_FLAG_CREATE, p, attr_eflag(protocol, type), init);
	*id = (rc == 0) ? conf_r32(p) : 0;
	return rc;
}

static uint32_t ef_new(uint64_t init)
{
	uint32_t id;
	(void)ef_create(SYNC_PRIO, WAITER_MULTIPLE, init, &id);
	return id;
}

static void ef_destroy_quiet(uint32_t id)
{
	if (id)
		(void)sc1(SC_SYS_EVENT_FLAG_DESTROY, id);
}

/* current pattern through sys_event_flag_get (:536-562); SENT64 when the call failed */
static uint64_t ef_pattern(uint32_t id)
{
	conf_addr p = conf_alloc(8, 8);
	conf_w64(p, SENT64);
	if (sc2(SC_SYS_EVENT_FLAG_GET, id, p) != 0)
		return SENT64;
	return conf_r64(p);
}

void case_eventflag_create_destroy(void)
{
	uint32_t id;
	conf_rec_rc("create_rc", ef_create(SYNC_PRIO, WAITER_MULTIPLE, 0x0F, &id));
	rec_bool("id_nonzero", id != 0);
	rec_bool("init_pattern", ef_pattern(id) == 0x0F);
	conf_rec_rc("destroy_rc", sc1(SC_SYS_EVENT_FLAG_DESTROY, id));
}

void case_eventflag_object_count(void)
{
	int64_t n0 = obj_count(OBJ_EVENT_FLAG);
	uint32_t id = ef_new(0);
	int64_t n1 = obj_count(OBJ_EVENT_FLAG);
	ef_destroy_quiet(id);
	int64_t n2 = obj_count(OBJ_EVENT_FLAG);
	rec_bool("count_ok", n0 >= 0);
	conf_rec("delta_create", n1 - n0);
	conf_rec("delta_destroy", n2 - n1);
}

/* sys_event_flag.cpp:63-67: protocol FIFO or PRIORITY, anything else EINVAL */
void case_eventflag_create_protocols(void)
{
	static const uint32_t protos[] = { 0, SYNC_FIFO, SYNC_PRIO, SYNC_PRIO_INHERIT, SYNC_RETRY };
	char key[16] = { 'p', 'r', 'o', 't', 'o', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(protos) / sizeof(protos[0]); i++) {
		uint32_t id;
		key[6] = (char)('0' + i);
		conf_rec_rc(key, ef_create(protos[i], WAITER_MULTIPLE, 0, &id));
		ef_destroy_quiet(id);
	}
}

/* sys_event_flag.cpp:69-75: type SINGLE (0x10000) or MULTIPLE (0x20000), anything else EINVAL */
void case_eventflag_create_types(void)
{
	static const uint32_t types[] = { 0, WAITER_SINGLE, WAITER_MULTIPLE, 1, 0x30000 };
	char key[16] = { 't', 'y', 'p', 'e', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		uint32_t id;
		key[5] = (char)('0' + i);
		conf_rec_rc(key, ef_create(SYNC_PRIO, types[i], 0, &id));
		ef_destroy_quiet(id);
	}
}

/* sys_event_flag.cpp:54-57: id == NULL or attr == NULL -> CELL_EFAULT */
void case_eventflag_create_null_args(void)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, SENT32);
	conf_rec_rc("null_id", sc3(SC_SYS_EVENT_FLAG_CREATE, 0, attr_eflag(SYNC_PRIO, WAITER_MULTIPLE), 0));
	conf_rec_rc("null_attr", sc3(SC_SYS_EVENT_FLAG_CREATE, p, 0, 0));
	rec_bool("id_untouched", conf_r32(p) == SENT32);
}

/* set ORs the bits in (:310-460), clear ANDs the pattern with its argument (:462-479), get reads (:536-562) */
void case_eventflag_set_clear_get(void)
{
	uint32_t id = ef_new(0x0F);
	conf_rec_rc("set", sc2(SC_SYS_EVENT_FLAG_SET, id, 0xF0));
	rec_bool("after_set_is_ff", ef_pattern(id) == 0xFF);
	conf_rec_rc("clear", sc2(SC_SYS_EVENT_FLAG_CLEAR, id, 0x3C));
	rec_bool("after_clear_is_3c", ef_pattern(id) == 0x3C);
	conf_rec_rc("set_zero", sc2(SC_SYS_EVENT_FLAG_SET, id, 0));
	rec_bool("after_set_zero_is_3c", ef_pattern(id) == 0x3C);
	conf_rec_rc("clear_all_bits", sc2(SC_SYS_EVENT_FLAG_CLEAR, id, 0));
	rec_bool("after_clear_zero_is_0", ef_pattern(id) == 0);
	ef_destroy_quiet(id);
}

/* high bits of the 64-bit pattern survive */
void case_eventflag_pattern_64bit(void)
{
	uint32_t id = ef_new(0);
	conf_rec_rc("set", sc2(SC_SYS_EVENT_FLAG_SET, id, 0x8000000000000001ull));
	rec_bool("pattern", ef_pattern(id) == 0x8000000000000001ull);
	ef_destroy_quiet(id);
}

/* sys_event_flag.h:91-95 + sys_event_flag.cpp:301-306: AND needs every bit, else CELL_EBUSY; the result
 * word is stored on every path (:23-36) */
void case_eventflag_trywait_and(void)
{
	uint32_t id = ef_new(0x0F);
	conf_addr r = conf_alloc(8, 8);
	conf_w64(r, SENT64);
	conf_rec_rc("subset", sc4(SC_SYS_EVENT_FLAG_TRYWAIT, id, 0x03, WAIT_AND, r));
	conf_rec("result_subset", (int64_t)conf_r64(r));
	conf_w64(r, SENT64);
	conf_rec_rc("missing_bit", sc4(SC_SYS_EVENT_FLAG_TRYWAIT, id, 0x13, WAIT_AND, r));
	conf_rec("result_missing", (int64_t)conf_r64(r));
	rec_bool("pattern_unchanged", ef_pattern(id) == 0x0F);
	ef_destroy_quiet(id);
}

/* sys_event_flag.h:91-95: OR needs any bit */
void case_eventflag_trywait_or(void)
{
	uint32_t id = ef_new(0x0F);
	conf_addr r = conf_alloc(8, 8);
	conf_w64(r, SENT64);
	conf_rec_rc("none", sc4(SC_SYS_EVENT_FLAG_TRYWAIT, id, 0x30, WAIT_OR, r));
	conf_rec("result_none", (int64_t)conf_r64(r));
	conf_w64(r, SENT64);
	conf_rec_rc("one", sc4(SC_SYS_EVENT_FLAG_TRYWAIT, id, 0x31, WAIT_OR, r));
	conf_rec("result_one", (int64_t)conf_r64(r));
	ef_destroy_quiet(id);
}

/* sys_event_flag.h:98-101: CLEAR removes the waited bits only; the result is the pattern before */
void case_eventflag_trywait_clear(void)
{
	uint32_t id = ef_new(0xFF);
	conf_addr r = conf_alloc(8, 8);
	conf_rec_rc("rc", sc4(SC_SYS_EVENT_FLAG_TRYWAIT, id, 0x0F, WAIT_AND | WAIT_CLEAR, r));
	conf_rec("result", (int64_t)conf_r64(r));
	conf_rec("pattern_after", (int64_t)ef_pattern(id));
	ef_destroy_quiet(id);
}

/* sys_event_flag.h:102-105: CLEAR_ALL empties the whole pattern */
void case_eventflag_trywait_clear_all(void)
{
	uint32_t id = ef_new(0xFF);
	conf_addr r = conf_alloc(8, 8);
	conf_rec_rc("rc", sc4(SC_SYS_EVENT_FLAG_TRYWAIT, id, 0x0F, WAIT_OR | WAIT_CLEAR_ALL, r));
	conf_rec("result", (int64_t)conf_r64(r));
	conf_rec("pattern_after", (int64_t)ef_pattern(id));
	ef_destroy_quiet(id);
}

/* a failed trywait does not clear anything, even with CLEAR set */
void case_eventflag_trywait_clear_on_failure(void)
{
	uint32_t id = ef_new(0x0F);
	conf_addr r = conf_alloc(8, 8);
	conf_rec_rc("rc", sc4(SC_SYS_EVENT_FLAG_TRYWAIT, id, 0xF0, WAIT_AND | WAIT_CLEAR, r));
	conf_rec("pattern_after", (int64_t)ef_pattern(id));
	ef_destroy_quiet(id);
}

/* sys_event_flag.h:61-79 via :280-284 and :141-145: the low nibble must be AND (1) or OR (2), the high
 * part 0, CLEAR (0x10) or CLEAR_ALL (0x20). The list mixes valid (0x11, 0x12, 0x21, 0x22) and invalid
 * words; the pattern is refilled after each call because a valid CLEAR mode consumes bits. */
void case_eventflag_bad_modes(void)
{
	static const uint32_t modes[] = { 0, 3, 0x04, 0x30, 0x40, 0x11, 0x12, 0x21, 0x22, 0x31 };
	uint32_t id = ef_new(0xFF);
	conf_addr r = conf_alloc(8, 8);
	char key[16] = { 'm', 'o', 'd', 'e', '_', '0', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		key[5] = (char)('0' + i / 10);
		key[6] = (char)('0' + i % 10);
		conf_w64(r, SENT64);
		conf_rec_rc(key, sc4(SC_SYS_EVENT_FLAG_TRYWAIT, id, 0x01, modes[i], r));
		(void)sc2(SC_SYS_EVENT_FLAG_SET, id, 0xFF);
	}
	conf_w64(r, SENT64);
	conf_rec_rc("wait_bad_mode", sc5(SC_SYS_EVENT_FLAG_WAIT, id, 0x01, 0, r, WAIT_US));
	conf_rec("wait_bad_mode_result", (int64_t)conf_r64(r));
	ef_destroy_quiet(id);
}

/* sys_event_flag.cpp:149-157, :194-195: an already satisfied wait returns at once with the pattern */
void case_eventflag_wait_ready(void)
{
	uint32_t id = ef_new(0x0F);
	conf_addr r = conf_alloc(8, 8);
	conf_rec_rc("rc", sc5(SC_SYS_EVENT_FLAG_WAIT, id, 0x03, WAIT_AND, r, WAIT_US));
	conf_rec("result", (int64_t)conf_r64(r));
	ef_destroy_quiet(id);
}

/* sys_event_flag.cpp:231-259: an unsatisfied wait ends CELL_ETIMEDOUT with the current pattern in the result */
void case_eventflag_wait_timeout(void)
{
	uint32_t id = ef_new(0x05);
	conf_addr r = conf_alloc(8, 8);
	uint64_t t0, t1;
	conf_w64(r, SENT64);
	t0 = conf_tb();
	conf_rec_rc("rc", sc5(SC_SYS_EVENT_FLAG_WAIT, id, 0x0A, WAIT_OR, r, WAIT_US));
	t1 = conf_tb();
	conf_rec("result", (int64_t)conf_r64(r));
	conf_rec_time("wait_ticks", t1 - t0);
	conf_rec_rc("destroy_after_timeout", sc1(SC_SYS_EVENT_FLAG_DESTROY, id));
}

/* sys_event_flag.cpp:487, :532, :524-527: cancel with no waiters reports 0 and succeeds; a NULL count is allowed */
void case_eventflag_cancel_no_waiters(void)
{
	uint32_t id = ef_new(0);
	conf_addr n = conf_alloc(4, 4);
	conf_w32(n, SENT32);
	conf_rec_rc("rc", sc2(SC_SYS_EVENT_FLAG_CANCEL, id, n));
	conf_rec("num", (int64_t)conf_r32(n));
	conf_rec_rc("rc_null_num", sc2(SC_SYS_EVENT_FLAG_CANCEL, id, 0));
	ef_destroy_quiet(id);
}

/* sys_event_flag.cpp:555-556: flags == NULL -> CELL_EFAULT */
void case_eventflag_get_null(void)
{
	uint32_t id = ef_new(1);
	conf_rec_rc("rc", sc2(SC_SYS_EVENT_FLAG_GET, id, 0));
	ef_destroy_quiet(id);
}

/* sys_event_flag.cpp:298-299 and the other lookups: unknown id -> CELL_ESRCH; get zeroes its
 * output first (:549) */
void case_eventflag_invalid_id(void)
{
	conf_addr r = conf_alloc(8, 8);
	conf_rec_rc("destroy", sc1(SC_SYS_EVENT_FLAG_DESTROY, BOGUS_ID));
	conf_rec_rc("wait", sc5(SC_SYS_EVENT_FLAG_WAIT, BOGUS_ID, 1, WAIT_OR, r, WAIT_US));
	conf_rec_rc("trywait", sc4(SC_SYS_EVENT_FLAG_TRYWAIT, BOGUS_ID, 1, WAIT_OR, r));
	conf_rec_rc("set", sc2(SC_SYS_EVENT_FLAG_SET, BOGUS_ID, 1));
	conf_rec_rc("clear", sc2(SC_SYS_EVENT_FLAG_CLEAR, BOGUS_ID, 1));
	conf_rec_rc("cancel", sc2(SC_SYS_EVENT_FLAG_CANCEL, BOGUS_ID, 0));
	conf_w64(r, SENT64);
	conf_rec_rc("get", sc2(SC_SYS_EVENT_FLAG_GET, BOGUS_ID, r));
	conf_rec("get_output", (int64_t)conf_r64(r));
}

void case_eventflag_destroy_twice(void)
{
	uint32_t id = ef_new(0);
	conf_rec_rc("first", sc1(SC_SYS_EVENT_FLAG_DESTROY, id));
	conf_rec_rc("second", sc1(SC_SYS_EVENT_FLAG_DESTROY, id));
}

/* Group "timer": sys_timer_* and the sleep calls.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4), rpcs3/Emu/Cell/lv2/sys_timer.cpp:
 *   create :171-196  destroy :198-234  get_information :236-261  start :263-340  stop :342-361
 *   connect_event_queue :363-404  disconnect_event_queue :406-439  sleep :441-448  usleep :450-483
 * sys_timer.h: sys_timer_information_t (next_expire u64, period u64, state u32, pad u32) :14-20.
 * No case waits for a timer to fire: the firing time is in the system-time domain, which conf.h
 * does not expose. Sleep durations are timing rows (never compared for equality).
 */
#include "cc.h"

#define BOGUS_ID 0x7FFFFF01u

static uint32_t timer_new(void)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, 0);
	if (sc1(SC_SYS_TIMER_CREATE, p) != 0)
		return 0;
	return conf_r32(p);
}

static void timer_destroy_quiet(uint32_t id)
{
	if (id) {
		(void)sc1(SC_SYS_TIMER_DISCONNECT_EVENT_QUEUE, id);
		(void)sc1(SC_SYS_TIMER_DESTROY, id);
	}
}

static uint32_t queue_new(void)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, 0);
	if (sc4(SC_SYS_EVENT_QUEUE_CREATE, p, attr_equeue(SYNC_FIFO, 1), 0, 4) != 0)
		return 0;
	return conf_r32(p);
}

static void queue_destroy_quiet(uint32_t id)
{
	if (id)
		(void)sc2(SC_SYS_EVENT_QUEUE_DESTROY, id, 1);
}

void case_timer_create_destroy(void)
{
	int64_t n0 = obj_count(OBJ_TIMER);
	uint32_t id = timer_new();
	int64_t n1 = obj_count(OBJ_TIMER);
	rec_bool("id_nonzero", id != 0);
	conf_rec_rc("destroy_rc", sc1(SC_SYS_TIMER_DESTROY, id));
	conf_rec("delta_create", n1 - n0);
	conf_rec("delta_destroy", obj_count(OBJ_TIMER) - n1);
}

/* sys_timer.cpp:215-218: an unknown id, and an id destroyed before -> CELL_ESRCH */
void case_timer_destroy_invalid(void)
{
	uint32_t id = timer_new();
	conf_rec_rc("bogus", sc1(SC_SYS_TIMER_DESTROY, BOGUS_ID));
	conf_rec_rc("first", sc1(SC_SYS_TIMER_DESTROY, id));
	conf_rec_rc("second", sc1(SC_SYS_TIMER_DESTROY, id));
}

/* sys_timer.cpp:282-285: starting a timer with no event queue -> CELL_ENOTCONN */
void case_timer_start_unconnected(void)
{
	uint32_t id = timer_new();
	conf_rec_rc("start", sc3(SC__SYS_TIMER_START, id, 0, 1000000));
	conf_rec_rc("stop_rc", sc1(SC_SYS_TIMER_STOP, id));
	timer_destroy_quiet(id);
}

/* sys_timer.cpp:271-275: a period below 100 us is invalid, before the id is even looked up;
 * an unknown id is CELL_ESRCH (:322-325) */
void case_timer_start_checks(void)
{
	uint32_t id = timer_new();
	conf_rec_rc("short_period_bogus_id", sc3(SC__SYS_TIMER_START, BOGUS_ID, 0, 99));
	conf_rec_rc("short_period", sc3(SC__SYS_TIMER_START, id, 0, 99));
	conf_rec_rc("bogus_id", sc3(SC__SYS_TIMER_START, BOGUS_ID, 0, 1000000));
	conf_rec_rc("stop_bogus", sc1(SC_SYS_TIMER_STOP, BOGUS_ID));
	timer_destroy_quiet(id);
}

/* sys_timer.cpp:375-383, :419-422, :206-209: connect once, a second connect is EISCONN, a connected
 * timer cannot be destroyed (EISCONN), disconnecting twice is ENOTCONN */
void case_timer_connect_disconnect(void)
{
	uint32_t t = timer_new(), q = queue_new();
	conf_rec_rc("disconnect_first", sc1(SC_SYS_TIMER_DISCONNECT_EVENT_QUEUE, t));
	conf_rec_rc("connect", sc5(SC_SYS_TIMER_CONNECT_EVENT_QUEUE, t, q, 0x1234, 1, 2));
	conf_rec_rc("connect_again", sc5(SC_SYS_TIMER_CONNECT_EVENT_QUEUE, t, q, 0x1234, 1, 2));
	conf_rec_rc("destroy_connected", sc1(SC_SYS_TIMER_DESTROY, t));
	conf_rec_rc("disconnect", sc1(SC_SYS_TIMER_DISCONNECT_EVENT_QUEUE, t));
	conf_rec_rc("disconnect_again", sc1(SC_SYS_TIMER_DISCONNECT_EVENT_QUEUE, t));
	conf_rec_rc("destroy", sc1(SC_SYS_TIMER_DESTROY, t));
	queue_destroy_quiet(q);
}

/* sys_timer.cpp:373-376, :393-396: unknown queue or timer -> CELL_ESRCH */
void case_timer_connect_invalid(void)
{
	uint32_t t = timer_new();
	conf_rec_rc("bogus_queue", sc5(SC_SYS_TIMER_CONNECT_EVENT_QUEUE, t, BOGUS_ID, 0, 0, 0));
	conf_rec_rc("bogus_timer", sc5(SC_SYS_TIMER_CONNECT_EVENT_QUEUE, BOGUS_ID, 0, 0, 0, 0));
	conf_rec_rc("disconnect_bogus_timer", sc1(SC_SYS_TIMER_DISCONNECT_EVENT_QUEUE, BOGUS_ID));
	timer_destroy_quiet(t);
}

/* sys_timer.cpp:293-297: a one-shot whose base time is not in the future -> CELL_ETIMEDOUT */
void case_timer_start_oneshot_in_past(void)
{
	uint32_t t = timer_new(), q = queue_new();
	(void)sc5(SC_SYS_TIMER_CONNECT_EVENT_QUEUE, t, q, 0, 0, 0);
	conf_rec_rc("start", sc3(SC__SYS_TIMER_START, t, 1, 0));
	timer_destroy_quiet(t);
	queue_destroy_quiet(q);
}

/* sys_timer.cpp:45-58 (sys_timer.h): a stopped timer reports state 0, next_expire 0, period 0; a
 * running periodic timer reports state 1 and its period; stop returns it to the first state.
 * Started with a one second period and stopped at once, so it never fires. */
void case_timer_get_information(void)
{
	uint32_t t = timer_new(), q = queue_new();
	conf_addr info = conf_alloc(24, 8);
	(void)sc5(SC_SYS_TIMER_CONNECT_EVENT_QUEUE, t, q, 0, 0, 0);
	conf_w32(info + 16, SENT32);
	conf_rec_rc("info_stopped", sc2(SC_SYS_TIMER_GET_INFORMATION, t, info));
	conf_rec("state_stopped", (int64_t)conf_r32(info + 16));
	conf_rec("period_stopped", (int64_t)conf_r64(info + 8));
	conf_rec_rc("start", sc3(SC__SYS_TIMER_START, t, 0, 1000000));
	conf_rec_rc("info_running", sc2(SC_SYS_TIMER_GET_INFORMATION, t, info));
	conf_rec("state_running", (int64_t)conf_r32(info + 16));
	conf_rec("period_running", (int64_t)conf_r64(info + 8));
	rec_bool("next_expire_nonzero", conf_r64(info) != 0);
	conf_rec_rc("start_again", sc3(SC__SYS_TIMER_START, t, 0, 1000000));
	conf_rec_rc("stop_rc", sc1(SC_SYS_TIMER_STOP, t));
	conf_rec_rc("info_after_stop", sc2(SC_SYS_TIMER_GET_INFORMATION, t, info));
	conf_rec("state_after_stop", (int64_t)conf_r32(info + 16));
	timer_destroy_quiet(t);
	queue_destroy_quiet(q);
}

void case_timer_get_information_invalid(void)
{
	conf_addr info = conf_alloc(24, 8);
	conf_rec_rc("rc", sc2(SC_SYS_TIMER_GET_INFORMATION, BOGUS_ID, info));
}

/* sys_timer.cpp:450-483: usleep returns CELL_OK; the elapsed time is only a timing row */
void case_timer_usleep_returns(void)
{
	uint64_t t0 = conf_tb(), t1;
	int64_t rc = sc1(SC_SYS_TIMER_USLEEP, 10000);
	t1 = conf_tb();
	conf_rec_rc("rc", rc);
	conf_rec_time("elapsed_ticks", t1 - t0);
}

/* the sleep lasts at least as long as asked: the verdict goes in as a timing row, because a loaded
 * host may legitimately differ */
void case_timer_usleep_duration(void)
{
	static const uint64_t req_us[] = { 1000, 5000, 20000, 50000 };
	char key[24] = { 'e', 'l', 'a', 'p', 's', 'e', 'd', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(req_us) / sizeof(req_us[0]); i++) {
		uint64_t t0 = conf_tb(), t1;
		(void)sc1(SC_SYS_TIMER_USLEEP, req_us[i]);
		t1 = conf_tb();
		key[8] = (char)('0' + i);
		conf_rec_time(key, t1 - t0);
	}
	conf_rec_time("tbfreq", conf_tbfreq());
}

/* sys_timer.cpp:477-480: usleep(0) is a plain yield and returns CELL_OK */
void case_timer_usleep_zero(void)
{
	conf_rec_rc("rc", sc1(SC_SYS_TIMER_USLEEP, 0));
}

/* sys_timer_sleep (:441-448) is usleep(seconds * 1e6); zero seconds returns at once */
void case_timer_sleep_zero(void)
{
	conf_rec_rc("rc", sc1(SC_SYS_TIMER_SLEEP, 0));
}

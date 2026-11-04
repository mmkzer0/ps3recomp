/* Group "event": sys_event_queue_* and sys_event_port_*.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4), rpcs3/Emu/Cell/lv2/sys_event.cpp:
 *   lv2_event_queue::send :124-229   queue_create :231-273   queue_destroy :275-426
 *   queue_tryreceive :428-471        queue_receive :473-608  queue_drain :610-629
 *   port_create :631-651  port_destroy :653-680  port_connect_local :682-711
 *   port_disconnect :750-778  port_send :780-848
 * sys_event.h: attribute struct :58-68, sys_event_t (source, data1, data2, data3) :70-76.
 * The payload of a blocking receive comes back in registers r4..r7, which conf_sc does not return;
 * payloads are therefore checked through tryreceive, which writes them to memory.
 */
#include "cc.h"

#define BOGUS_ID 0x7FFFFF01u
#define S32(x) ((uint64_t)(int64_t)(int32_t)(x))
#define PPU_QUEUE 1u
#define SPU_QUEUE 2u
#define PORT_LOCAL 1u
#define PORT_IPC 3u

static int64_t eq_create(uint32_t protocol, uint32_t type, int32_t size, uint32_t *id)
{
	conf_addr p = conf_alloc(4, 4);
	int64_t rc;
	conf_w32(p, SENT32);
	rc = sc4(SC_SYS_EVENT_QUEUE_CREATE, p, attr_equeue(protocol, type), 0, S32(size));
	*id = (rc == 0) ? conf_r32(p) : 0;
	return rc;
}

static uint32_t eq_new(int32_t size)
{
	uint32_t id;
	(void)eq_create(SYNC_FIFO, PPU_QUEUE, size, &id);
	return id;
}

static void eq_destroy_quiet(uint32_t id)
{
	if (id)
		(void)sc2(SC_SYS_EVENT_QUEUE_DESTROY, id, 1); /* force: also ends a leftover waiter */
}

static int64_t port_create(uint32_t type, uint64_t name, uint32_t *id)
{
	conf_addr p = conf_alloc(4, 4);
	int64_t rc;
	conf_w32(p, SENT32);
	rc = sc3(SC_SYS_EVENT_PORT_CREATE, p, type, name);
	*id = (rc == 0) ? conf_r32(p) : 0;
	return rc;
}

static uint32_t port_new(uint64_t name)
{
	uint32_t id;
	(void)port_create(PORT_LOCAL, name, &id);
	return id;
}

static void port_destroy_quiet(uint32_t id)
{
	if (id) {
		(void)sc1(SC_SYS_EVENT_PORT_DISCONNECT, id);
		(void)sc1(SC_SYS_EVENT_PORT_DESTROY, id);
	}
}

/* tryreceive up to `size` events into ev (32 bytes each); returns the count word, -1 on failure */
static int64_t try_receive(uint32_t q, conf_addr ev, int32_t size, int64_t *rc_out)
{
	conf_addr cnt = conf_alloc(4, 4);
	int64_t rc;
	conf_w32(cnt, SENT32);
	rc = sc4(SC_SYS_EVENT_QUEUE_TRYRECEIVE, q, ev, S32(size), cnt);
	if (rc_out)
		*rc_out = rc;
	if (rc != 0)
		return -1;
	return (int64_t)conf_r32(cnt);
}

void case_event_queue_create_destroy(void)
{
	uint32_t id;
	conf_rec_rc("create_rc", eq_create(SYNC_FIFO, PPU_QUEUE, 4, &id));
	rec_bool("id_nonzero", id != 0);
	conf_rec_rc("destroy_rc", sc2(SC_SYS_EVENT_QUEUE_DESTROY, id, 0));
}

void case_event_queue_object_count(void)
{
	int64_t n0 = obj_count(OBJ_EVENT_QUEUE);
	uint32_t id = eq_new(4);
	int64_t n1 = obj_count(OBJ_EVENT_QUEUE);
	eq_destroy_quiet(id);
	int64_t n2 = obj_count(OBJ_EVENT_QUEUE);
	rec_bool("count_ok", n0 >= 0);
	conf_rec("delta_create", n1 - n0);
	conf_rec("delta_destroy", n2 - n1);
}

/* sys_event.cpp:237-240: size <= 0 or size > 127 -> CELL_EINVAL */
void case_event_queue_create_sizes(void)
{
	static const int32_t sz[] = { 0, 1, 127, 128, -1, 255 };
	char key[16] = { 's', 'z', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(sz) / sizeof(sz[0]); i++) {
		uint32_t id;
		key[3] = (char)('0' + i);
		conf_rec_rc(key, eq_create(SYNC_FIFO, PPU_QUEUE, sz[i], &id));
		eq_destroy_quiet(id);
	}
}

/* sys_event.cpp:242-248: protocol FIFO or PRIORITY, anything else EINVAL */
void case_event_queue_create_protocols(void)
{
	static const uint32_t protos[] = { 0, SYNC_FIFO, SYNC_PRIO, SYNC_PRIO_INHERIT, SYNC_RETRY };
	char key[16] = { 'p', 'r', 'o', 't', 'o', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(protos) / sizeof(protos[0]); i++) {
		uint32_t id;
		key[6] = (char)('0' + i);
		conf_rec_rc(key, eq_create(protos[i], PPU_QUEUE, 4, &id));
		eq_destroy_quiet(id);
	}
}

/* sys_event.cpp:250-256: type PPU_QUEUE (1) or SPU_QUEUE (2), anything else EINVAL */
void case_event_queue_create_types(void)
{
	static const uint32_t types[] = { 0, PPU_QUEUE, SPU_QUEUE, 3 };
	char key[16] = { 't', 'y', 'p', 'e', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		uint32_t id;
		key[5] = (char)('0' + i);
		conf_rec_rc(key, eq_create(SYNC_FIFO, types[i], 4, &id));
		eq_destroy_quiet(id);
	}
}

/* sys_event.cpp:281-284: destroy mode 0 or FORCE (1), anything else EINVAL */
void case_event_queue_destroy_modes(void)
{
	static const int32_t modes[] = { 0, 1, 2, -1 };
	char key[16] = { 'm', 'o', 'd', 'e', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		uint32_t id = eq_new(4);
		key[5] = (char)('0' + i);
		conf_rec_rc(key, sc2(SC_SYS_EVENT_QUEUE_DESTROY, id, S32(modes[i])));
		eq_destroy_quiet(id);
	}
}

/* sys_event.cpp:344-347: an unknown id, and an id destroyed before -> CELL_ESRCH */
void case_event_queue_destroy_invalid(void)
{
	uint32_t id = eq_new(4);
	conf_rec_rc("bogus", sc2(SC_SYS_EVENT_QUEUE_DESTROY, BOGUS_ID, 0));
	conf_rec_rc("first", sc2(SC_SYS_EVENT_QUEUE_DESTROY, id, 0));
	conf_rec_rc("second", sc2(SC_SYS_EVENT_QUEUE_DESTROY, id, 0));
}

/* sys_event.cpp:455-470: an empty queue yields count 0 and CELL_OK; the array is not touched */
void case_event_queue_tryreceive_empty(void)
{
	uint32_t id = eq_new(4);
	conf_addr ev = conf_alloc(32 * 4, 8);
	int64_t rc;
	int64_t n;
	conf_w64(ev, SENT64);
	n = try_receive(id, ev, 4, &rc);
	conf_rec_rc("rc", rc);
	conf_rec("count", n);
	rec_bool("array_untouched", conf_r64(ev) == SENT64);
	eq_destroy_quiet(id);
}

/* sys_event.cpp:436-444: unknown id -> ESRCH; an SPU queue cannot be polled -> EINVAL */
void case_event_queue_tryreceive_errors(void)
{
	uint32_t spu;
	conf_addr ev = conf_alloc(32 * 4, 8);
	int64_t rc;
	(void)try_receive(BOGUS_ID, ev, 4, &rc);
	conf_rec_rc("bogus", rc);
	(void)eq_create(SYNC_FIFO, SPU_QUEUE, 4, &spu);
	(void)try_receive(spu, ev, 4, &rc);
	conf_rec_rc("spu_queue", rc);
	eq_destroy_quiet(spu);
}

/* sys_event.cpp:563-590: a timed receive on an empty queue -> CELL_ETIMEDOUT */
void case_event_queue_receive_timeout(void)
{
	uint32_t id = eq_new(4);
	conf_addr ev = conf_alloc(32, 8);
	uint64_t t0 = conf_tb(), t1;
	int64_t rc = sc3(SC_SYS_EVENT_QUEUE_RECEIVE, id, ev, WAIT_US);
	t1 = conf_tb();
	conf_rec_rc("rc", rc);
	conf_rec_time("wait_ticks", t1 - t0);
	eq_destroy_quiet(id);
}

/* sys_event.cpp:512-515, :483-486: receive on an unknown id -> ESRCH, on an SPU queue -> EINVAL */
void case_event_queue_receive_errors(void)
{
	uint32_t spu;
	conf_addr ev = conf_alloc(32, 8);
	conf_rec_rc("bogus", sc3(SC_SYS_EVENT_QUEUE_RECEIVE, BOGUS_ID, ev, WAIT_US));
	(void)eq_create(SYNC_FIFO, SPU_QUEUE, 4, &spu);
	conf_rec_rc("spu_queue", sc3(SC_SYS_EVENT_QUEUE_RECEIVE, spu, ev, WAIT_US));
	eq_destroy_quiet(spu);
}

/* sys_event.cpp:610-629: drain empties the queue; an unknown id -> ESRCH */
void case_event_queue_drain(void)
{
	uint32_t q = eq_new(4), p = port_new(0);
	conf_addr ev = conf_alloc(32 * 4, 8);
	conf_rec_rc("connect", sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q));
	conf_rec_rc("send1", sc4(SC_SYS_EVENT_PORT_SEND, p, 1, 2, 3));
	conf_rec_rc("send2", sc4(SC_SYS_EVENT_PORT_SEND, p, 4, 5, 6));
	conf_rec_rc("drain", sc1(SC_SYS_EVENT_QUEUE_DRAIN, q));
	conf_rec("count_after", try_receive(q, ev, 4, 0));
	conf_rec_rc("drain_bogus", sc1(SC_SYS_EVENT_QUEUE_DRAIN, BOGUS_ID));
	port_destroy_quiet(p);
	eq_destroy_quiet(q);
}

/* sys_event.cpp:274-426: destroying a queue that still holds events is allowed (mode 0, no waiters) */
void case_event_queue_destroy_with_events(void)
{
	uint32_t q = eq_new(4), p = port_new(0);
	conf_rec_rc("connect", sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q));
	conf_rec_rc("send", sc4(SC_SYS_EVENT_PORT_SEND, p, 1, 2, 3));
	conf_rec_rc("destroy", sc2(SC_SYS_EVENT_QUEUE_DESTROY, q, 0));
	port_destroy_quiet(p);
}

void case_event_port_create_destroy(void)
{
	int64_t n0 = obj_count(OBJ_EVENT_PORT);
	uint32_t id;
	int64_t rc = port_create(PORT_LOCAL, 0, &id);
	int64_t n1 = obj_count(OBJ_EVENT_PORT);
	conf_rec_rc("create_rc", rc);
	rec_bool("id_nonzero", id != 0);
	conf_rec_rc("destroy_rc", sc1(SC_SYS_EVENT_PORT_DESTROY, id));
	conf_rec("delta_create", n1 - n0);
	conf_rec("delta_destroy", obj_count(OBJ_EVENT_PORT) - n1);
}

/* sys_event.cpp:637-641: port type LOCAL (1) or IPC (3), anything else EINVAL */
void case_event_port_create_types(void)
{
	static const uint32_t types[] = { 0, PORT_LOCAL, 2, PORT_IPC, 4 };
	char key[16] = { 't', 'y', 'p', 'e', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		uint32_t id;
		key[5] = (char)('0' + i);
		conf_rec_rc(key, port_create(types[i], 0, &id));
		port_destroy_quiet(id);
	}
}

/* sys_event.cpp:669-672: an unknown id, and an id destroyed before -> CELL_ESRCH */
void case_event_port_destroy_invalid(void)
{
	uint32_t id = port_new(0);
	conf_rec_rc("bogus", sc1(SC_SYS_EVENT_PORT_DESTROY, BOGUS_ID));
	conf_rec_rc("first", sc1(SC_SYS_EVENT_PORT_DESTROY, id));
	conf_rec_rc("second", sc1(SC_SYS_EVENT_PORT_DESTROY, id));
}

/* sys_event.cpp:703-709, :765-768: connect twice is EISCONN, disconnect when not connected ENOTCONN */
void case_event_port_connect_disconnect(void)
{
	uint32_t q = eq_new(4), p = port_new(0);
	conf_rec_rc("disconnect_first", sc1(SC_SYS_EVENT_PORT_DISCONNECT, p));
	conf_rec_rc("connect", sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q));
	conf_rec_rc("connect_again", sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q));
	conf_rec_rc("disconnect", sc1(SC_SYS_EVENT_PORT_DISCONNECT, p));
	conf_rec_rc("disconnect_again", sc1(SC_SYS_EVENT_PORT_DISCONNECT, p));
	conf_rec_rc("reconnect", sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q));
	port_destroy_quiet(p);
	eq_destroy_quiet(q);
}

/* sys_event.cpp:693-696, :760-763: unknown port or queue -> CELL_ESRCH */
void case_event_port_connect_invalid(void)
{
	uint32_t q = eq_new(4), p = port_new(0);
	conf_rec_rc("bogus_port", sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, BOGUS_ID, q));
	conf_rec_rc("bogus_queue", sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, BOGUS_ID));
	conf_rec_rc("disconnect_bogus", sc1(SC_SYS_EVENT_PORT_DISCONNECT, BOGUS_ID));
	port_destroy_quiet(p);
	eq_destroy_quiet(q);
}

/* sys_event.cpp:698-701: connect_local on an IPC port -> CELL_EINVAL */
void case_event_port_connect_wrong_type(void)
{
	uint32_t q = eq_new(4), p;
	(void)port_create(PORT_IPC, 0, &p);
	conf_rec_rc("rc", sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q));
	port_destroy_quiet(p);
	eq_destroy_quiet(q);
}

/* sys_event.cpp:661-664: destroying a connected port -> CELL_EISCONN */
void case_event_port_destroy_connected(void)
{
	uint32_t q = eq_new(4), p = port_new(0);
	(void)sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q);
	conf_rec_rc("destroy_connected", sc1(SC_SYS_EVENT_PORT_DESTROY, p));
	conf_rec_rc("disconnect", sc1(SC_SYS_EVENT_PORT_DISCONNECT, p));
	conf_rec_rc("destroy", sc1(SC_SYS_EVENT_PORT_DESTROY, p));
	eq_destroy_quiet(q);
}

/* sys_event.cpp:810: send through a port that is not connected -> CELL_ENOTCONN; unknown port -> ESRCH (:814) */
void case_event_port_send_errors(void)
{
	uint32_t p = port_new(0);
	conf_rec_rc("unconnected", sc4(SC_SYS_EVENT_PORT_SEND, p, 1, 2, 3));
	conf_rec_rc("bogus", sc4(SC_SYS_EVENT_PORT_SEND, BOGUS_ID, 1, 2, 3));
	port_destroy_quiet(p);
}

/* sys_event.cpp:803-807, :460: a named port delivers its name as the event source; data1..3 arrive intact */
void case_event_port_send_payload(void)
{
	uint32_t q = eq_new(4), p = port_new(0x0123456789ABCDEFull);
	conf_addr ev = conf_alloc(32 * 4, 8);
	(void)sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q);
	conf_rec_rc("send", sc4(SC_SYS_EVENT_PORT_SEND, p, 0x1111111122222222ull, 0x3333333344444444ull, 0x5555555566666666ull));
	conf_rec("count", try_receive(q, ev, 4, 0));
	rec_bool("source_is_name", conf_r64(ev) == 0x0123456789ABCDEFull);
	rec_bool("data1", conf_r64(ev + 8) == 0x1111111122222222ull);
	rec_bool("data2", conf_r64(ev + 16) == 0x3333333344444444ull);
	rec_bool("data3", conf_r64(ev + 24) == 0x5555555566666666ull);
	port_destroy_quiet(p);
	eq_destroy_quiet(q);
}

/* sys_event.cpp:805: an unnamed port's source is (pid << 32) | port id. Recorded as relations only. */
void case_event_port_send_default_source(void)
{
	uint32_t q = eq_new(4), p = port_new(0);
	conf_addr ev = conf_alloc(32 * 4, 8);
	uint64_t src;
	int64_t pid;
	(void)sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q);
	(void)sc4(SC_SYS_EVENT_PORT_SEND, p, 7, 8, 9);
	(void)try_receive(q, ev, 4, 0);
	src = conf_r64(ev);
	pid = sc0(SC_SYS_PROCESS_GETPID);
	rec_bool("source_nonzero", src != 0);
	rec_bool("low_is_port_id", (uint32_t)src == p);
	rec_bool("high_is_pid", (src >> 32) == (uint64_t)(uint32_t)pid);
	port_destroy_quiet(p);
	eq_destroy_quiet(q);
}

/* sys_event.cpp:154-164 and :839-841: a full queue refuses the next event with CELL_EBUSY and keeps
 * the first one */
void case_event_port_send_full_queue(void)
{
	uint32_t q = eq_new(1), p = port_new(0xAA);
	conf_addr ev = conf_alloc(32 * 4, 8);
	(void)sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q);
	conf_rec_rc("send1", sc4(SC_SYS_EVENT_PORT_SEND, p, 1, 0, 0));
	conf_rec_rc("send2", sc4(SC_SYS_EVENT_PORT_SEND, p, 2, 0, 0));
	conf_rec("count", try_receive(q, ev, 4, 0));
	conf_rec("data1_kept", (int64_t)conf_r64(ev + 8));
	conf_rec_rc("send3_after_receive", sc4(SC_SYS_EVENT_PORT_SEND, p, 3, 0, 0));
	port_destroy_quiet(p);
	eq_destroy_quiet(q);
}

/* sys_event.cpp:455-462: events leave in send order; `size` caps how many one call returns */
void case_event_port_send_order(void)
{
	uint32_t q = eq_new(4), p = port_new(0x77);
	conf_addr ev = conf_alloc(32 * 4, 8);
	int64_t n1, n2;
	(void)sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q);
	(void)sc4(SC_SYS_EVENT_PORT_SEND, p, 10, 0, 0);
	(void)sc4(SC_SYS_EVENT_PORT_SEND, p, 20, 0, 0);
	(void)sc4(SC_SYS_EVENT_PORT_SEND, p, 30, 0, 0);
	n1 = try_receive(q, ev, 2, 0);
	conf_rec("first_call_count", n1);
	conf_rec("e0", (int64_t)conf_r64(ev + 8));
	conf_rec("e1", (int64_t)conf_r64(ev + 32 + 8));
	n2 = try_receive(q, ev, 4, 0);
	conf_rec("second_call_count", n2);
	conf_rec("e2", (int64_t)conf_r64(ev + 8));
	port_destroy_quiet(p);
	eq_destroy_quiet(q);
}

/* sys_event.cpp:149-152, :661, :765: after the queue is destroyed, a port that was connected to it
 * cannot send (ENOTCONN), reports not-connected on disconnect, and can be destroyed */
void case_event_port_outlives_queue(void)
{
	uint32_t q = eq_new(4), p = port_new(0);
	(void)sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q);
	conf_rec_rc("queue_destroy", sc2(SC_SYS_EVENT_QUEUE_DESTROY, q, 0));
	conf_rec_rc("send", sc4(SC_SYS_EVENT_PORT_SEND, p, 1, 2, 3));
	conf_rec_rc("disconnect", sc1(SC_SYS_EVENT_PORT_DISCONNECT, p));
	conf_rec_rc("port_destroy", sc1(SC_SYS_EVENT_PORT_DESTROY, p));
}

/* sys_event.cpp:500-509 and :563-590: a blocking receive takes a queued event at once (CELL_OK), the
 * next one on the now empty queue ends CELL_ETIMEDOUT. The payload travels in registers and is not
 * observable here; the port_send_payload case covers it through tryreceive. */
void case_event_queue_send_then_receive(void)
{
	uint32_t q = eq_new(4), p = port_new(0x99);
	conf_addr ev = conf_alloc(32, 8);
	(void)sc2(SC_SYS_EVENT_PORT_CONNECT_LOCAL, p, q);
	conf_rec_rc("send", sc4(SC_SYS_EVENT_PORT_SEND, p, 1, 2, 3));
	conf_rec_rc("receive_queued", sc3(SC_SYS_EVENT_QUEUE_RECEIVE, q, ev, WAIT_US));
	conf_rec_rc("receive_empty", sc3(SC_SYS_EVENT_QUEUE_RECEIVE, q, ev, WAIT_US));
	port_destroy_quiet(p);
	eq_destroy_quiet(q);
}

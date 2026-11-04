/* Group "container": memory containers, syscalls 341/342 (create, destroy), 343 (get_size),
 * 350 (allocate_from_container) and the older numbers 324/325 that carry the same two calls.
 * Slots 341/342 are the ones issue #96 asks about: each case that creates with one number and
 * destroys with another records whether the pairs are interchangeable.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4):
 *   lv2.cpp:404-405 (324/325) and :421-423 (341/342/343) bind the same handlers
 *   sys_memory.cpp  container_create :427-461  container_destroy :463-496  container_get_size :498-516
 *                   allocate_from_container :229-297
 * Sizes are 1 MiB multiples: the kernel rounds a container down to 1 MiB (:434).
 */
#include "cc.h"

#define PAGE_64K 0x200ull
#define PAGE_1M 0x400ull
#define SZ_1M 0x100000ull
#define BOGUS_ID 0x7FFFFF01u

static int64_t ct_create_with(uint32_t sc, uint64_t size, uint32_t *id)
{
	conf_addr p = conf_alloc(4, 4);
	int64_t rc;
	conf_w32(p, SENT32);
	rc = sc2(sc, p, size);
	*id = (rc == 0) ? conf_r32(p) : 0;
	return rc;
}

static int64_t ct_create(uint64_t size, uint32_t *id)
{
	return ct_create_with(SC_SYS_MEMORY_CONTAINER_CREATE, size, id);
}

static void ct_destroy_quiet(uint32_t id)
{
	if (id)
		(void)sc1(SC_SYS_MEMORY_CONTAINER_DESTROY, id);
}

/* container size and available amount; returns the rc of the call (0 = success) */
static int64_t ct_size(uint32_t id, uint32_t *total, uint32_t *avail)
{
	conf_addr p = conf_alloc(8, 4);
	int64_t rc;
	conf_w32(p, SENT32);
	conf_w32(p + 4, SENT32);
	rc = sc2(SC_SYS_MEMORY_CONTAINER_GET_SIZE, p, id);
	if (rc != 0)
		return rc;
	*total = conf_r32(p);
	*avail = conf_r32(p + 4);
	return 0;
}

static int user_mem(uint32_t *total, uint32_t *avail)
{
	conf_addr p = conf_alloc(8, 4);
	conf_w32(p, SENT32);
	conf_w32(p + 4, SENT32);
	if (sc1(SC_SYS_MEMORY_GET_USER_MEMORY_SIZE, p) != 0)
		return -1;
	*total = conf_r32(p);
	*avail = conf_r32(p + 4);
	return 0;
}

void case_container_create_destroy(void)
{
	uint32_t id, total = 0, avail = 0;
	conf_rec_rc("create_rc", ct_create(2 * SZ_1M, &id));
	rec_bool("id_nonzero", id != 0);
	conf_rec_rc("get_size_rc", ct_size(id, &total, &avail));
	conf_rec("total", (int64_t)total);
	conf_rec("avail", (int64_t)avail);
	conf_rec_rc("destroy_rc", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY, id));
}

/* the older numbers 324/325 */
void case_container_create_destroy_old_slots(void)
{
	uint32_t id, total = 0, avail = 0;
	conf_rec_rc("create_rc", ct_create_with(SC_SYS_MEMORY_CONTAINER_CREATE_OLD, 2 * SZ_1M, &id));
	rec_bool("id_nonzero", id != 0);
	conf_rec_rc("get_size_rc", ct_size(id, &total, &avail));
	conf_rec("total", (int64_t)total);
	conf_rec_rc("destroy_rc", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY_OLD, id));
}

/* 341 created, 325 destroyed, and 324 created, 342 destroyed: do the two number pairs share containers */
void case_container_slot_cross(void)
{
	uint32_t a, b;
	conf_rec_rc("create_341", ct_create_with(SC_SYS_MEMORY_CONTAINER_CREATE, SZ_1M, &a));
	conf_rec_rc("destroy_325", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY_OLD, a));
	conf_rec_rc("create_324", ct_create_with(SC_SYS_MEMORY_CONTAINER_CREATE_OLD, SZ_1M, &b));
	conf_rec_rc("destroy_342", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY, b));
	ct_destroy_quiet(a);
	ct_destroy_quiet(b);
}

/* distinct containers have distinct ids */
void case_container_create_several(void)
{
	uint32_t id[4];
	unsigned i, distinct = 1;
	for (i = 0; i < 4; i++)
		conf_rec_rc(i == 0 ? "create_0" : i == 1 ? "create_1" : i == 2 ? "create_2" : "create_3", ct_create(SZ_1M, &id[i]));
	for (i = 0; i < 4; i++) {
		unsigned j;
		for (j = i + 1; j < 4; j++)
			if (id[i] == id[j])
				distinct = 0;
	}
	rec_bool("ids_distinct", distinct);
	rec_bool("ids_nonzero", id[0] && id[1] && id[2] && id[3]);
	for (i = 0; i < 4; i++)
		ct_destroy_quiet(id[i]);
}

/* sys_memory.cpp:434-439: sizes round down to 1 MiB; a size below 1 MiB, including 0 -> CELL_ENOMEM */
void case_container_create_sizes(void)
{
	static const uint64_t sz[] = { 0, 1, 0xFFFFF, 0x100000, 0x180000, 0x200001 };
	char key[16] = { 's', 'z', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(sz) / sizeof(sz[0]); i++) {
		uint32_t id, total = 0, avail = 0;
		key[3] = (char)('0' + i);
		conf_rec_rc(key, ct_create(sz[i], &id));
		if (id != 0 && ct_size(id, &total, &avail) == 0) {
			key[2] = 't';
			conf_rec(key, (int64_t)total);
			key[2] = '_';
		}
		ct_destroy_quiet(id);
	}
}

/* sys_memory.cpp:446-449: more than the free user memory -> CELL_ENOMEM */
void case_container_create_too_big(void)
{
	uint32_t id;
	conf_rec_rc("1g", ct_create(0x40000000ull, &id));
	ct_destroy_quiet(id);
	conf_rec_rc("above_32bit", ct_create(0x100000000ull, &id));
	ct_destroy_quiet(id);
}

/* sys_memory.cpp:387-393 and :446: a container takes its size out of the user memory total and out of
 * what is available; destroying it returns both */
void case_container_changes_user_memory(void)
{
	uint32_t t0, a0, t1, a1, t2, a2, id;
	(void)user_mem(&t0, &a0);
	(void)ct_create(4 * SZ_1M, &id);
	(void)user_mem(&t1, &a1);
	conf_rec_rc("destroy", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY, id));
	(void)user_mem(&t2, &a2);
	conf_rec("total_drop", (int64_t)t0 - (int64_t)t1);
	conf_rec("avail_drop", (int64_t)a0 - (int64_t)a1);
	rec_bool("total_restored", t2 == t0);
	rec_bool("avail_restored", a2 == a0);
}

/* sys_memory.cpp:481-485 and :504-507: an unknown id, and an id destroyed before -> CELL_ESRCH */
void case_container_invalid_id(void)
{
	uint32_t id, total = 0, avail = 0;
	(void)ct_create(SZ_1M, &id);
	conf_rec_rc("destroy_bogus", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY, BOGUS_ID));
	conf_rec_rc("get_size_bogus", ct_size(BOGUS_ID, &total, &avail));
	conf_rec_rc("first_destroy", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY, id));
	conf_rec_rc("second_destroy", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY, id));
	conf_rec_rc("zero_destroy", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY, 0));
}

/* sys_memory.cpp:256-275, :498-516: allocate_from_container draws from the container and the
 * container cannot be destroyed while a block is out (:474-477, CELL_EBUSY) */
void case_container_allocate_from(void)
{
	uint32_t id, total = 0, avail = 0, addr = 0;
	conf_addr p = conf_alloc(4, 4);
	(void)ct_create(4 * SZ_1M, &id);
	conf_w32(p, SENT32);
	conf_rec_rc("alloc", sc4(SC_SYS_MEMORY_ALLOCATE_FROM_CONTAINER, SZ_1M, id, PAGE_1M, p));
	addr = conf_r32(p);
	rec_bool("addr_aligned_1m", addr != 0 && addr != SENT32 && (addr % SZ_1M) == 0);
	(void)ct_size(id, &total, &avail);
	conf_rec("avail_after_alloc", (int64_t)avail);
	conf_rec_rc("destroy_with_block", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY, id));
	conf_rec_rc("free", sc1(SC_SYS_MEMORY_FREE, addr));
	(void)ct_size(id, &total, &avail);
	conf_rec("avail_after_free", (int64_t)avail);
	conf_rec_rc("destroy", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY, id));
}

/* a block from a container is 64K-pageable too, and exhausts the container (:259-262, CELL_ENOMEM) */
void case_container_allocate_exhaust(void)
{
	uint32_t id;
	conf_addr p = conf_alloc(4, 4);
	uint32_t a1, a2;
	(void)ct_create(2 * SZ_1M, &id);
	conf_w32(p, SENT32);
	conf_rec_rc("alloc_1m", sc4(SC_SYS_MEMORY_ALLOCATE_FROM_CONTAINER, SZ_1M, id, PAGE_1M, p));
	a1 = conf_r32(p);
	conf_w32(p, SENT32);
	conf_rec_rc("alloc_1m_64k", sc4(SC_SYS_MEMORY_ALLOCATE_FROM_CONTAINER, SZ_1M, id, PAGE_64K, p));
	a2 = conf_r32(p);
	conf_w32(p, SENT32);
	conf_rec_rc("alloc_over", sc4(SC_SYS_MEMORY_ALLOCATE_FROM_CONTAINER, SZ_1M, id, PAGE_1M, p));
	rec_bool("over_untouched", conf_r32(p) == SENT32);
	if (a1 != SENT32)
		(void)sc1(SC_SYS_MEMORY_FREE, a1);
	if (a2 != SENT32)
		(void)sc1(SC_SYS_MEMORY_FREE, a2);
	conf_rec_rc("destroy", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY, id));
}

/* sys_memory.cpp:235-254, :267-270: argument checks run in the order size, flags, alignment, container */
void case_container_allocate_from_errors(void)
{
	uint32_t id;
	conf_addr p = conf_alloc(4, 4);
	(void)ct_create(2 * SZ_1M, &id);
	conf_rec_rc("bogus_cid", sc4(SC_SYS_MEMORY_ALLOCATE_FROM_CONTAINER, SZ_1M, BOGUS_ID, PAGE_1M, p));
	conf_rec_rc("bogus_cid_bad_flags", sc4(SC_SYS_MEMORY_ALLOCATE_FROM_CONTAINER, SZ_1M, BOGUS_ID, 0x100, p));
	conf_rec_rc("bogus_cid_bad_size", sc4(SC_SYS_MEMORY_ALLOCATE_FROM_CONTAINER, 0x1000, BOGUS_ID, PAGE_1M, p));
	conf_rec_rc("size_zero", sc4(SC_SYS_MEMORY_ALLOCATE_FROM_CONTAINER, 0, id, PAGE_1M, p));
	conf_rec_rc("bad_flags", sc4(SC_SYS_MEMORY_ALLOCATE_FROM_CONTAINER, SZ_1M, id, 0x100, p));
	conf_rec_rc("unaligned", sc4(SC_SYS_MEMORY_ALLOCATE_FROM_CONTAINER, SZ_1M + 1, id, PAGE_1M, p));
	conf_rec_rc("too_big", sc4(SC_SYS_MEMORY_ALLOCATE_FROM_CONTAINER, 4 * SZ_1M, id, PAGE_1M, p));
	ct_destroy_quiet(id);
}

/* the container slots must answer as containers, not as the mmapper calls that sit next to them
 * (330-337): arguments that name nothing give container errors (sys_memory.cpp:436-439 size 0 ->
 * CELL_ENOMEM, :481-485 and :504-507 unknown id -> CELL_ESRCH) on both number pairs */
void case_container_neighbour_slots(void)
{
	conf_addr p = conf_alloc(8, 4);
	conf_rec_rc("destroy_342_bogus", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY, BOGUS_ID));
	conf_rec_rc("destroy_325_bogus", sc1(SC_SYS_MEMORY_CONTAINER_DESTROY_OLD, BOGUS_ID));
	conf_rec_rc("get_size_343_bogus", sc2(SC_SYS_MEMORY_CONTAINER_GET_SIZE, p, BOGUS_ID));
	conf_rec_rc("create_341_zero", sc2(SC_SYS_MEMORY_CONTAINER_CREATE, p, 0));
	conf_rec_rc("create_324_zero", sc2(SC_SYS_MEMORY_CONTAINER_CREATE_OLD, p, 0));
}

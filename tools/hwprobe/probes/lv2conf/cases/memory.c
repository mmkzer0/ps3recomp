/* Group "memory": sys_memory_allocate / free / get_page_attribute / get_user_memory_size.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4), rpcs3/Emu/Cell/lv2/sys_memory.cpp:
 *   allocate :171-227  free :299-315  get_page_attribute :317-374  get_user_memory_size :376-416
 * sys_memory.h: page-size flags :34-37 (4K 0x100, 64K 0x200, 1M 0x400), sys_memory_info_t :54-58,
 * sys_page_attr_t :61-67. No case records an address, only its alignment and the amounts.
 */
#include "cc.h"

#define PAGE_64K 0x200ull
#define PAGE_1M 0x400ull
#define SZ_64K 0x10000ull
#define SZ_1M 0x100000ull

/* allocate `size` with `flags`; returns rc, *addr = the address (0 when failed) */
static int64_t mem_alloc(uint64_t size, uint64_t flags, uint32_t *addr)
{
	conf_addr p = conf_alloc(4, 4);
	int64_t rc;
	conf_w32(p, SENT32);
	rc = sc3(SC_SYS_MEMORY_ALLOCATE, size, flags, p);
	*addr = (rc == 0) ? conf_r32(p) : 0;
	return rc;
}

static void mem_free_quiet(uint32_t addr)
{
	if (addr)
		(void)sc1(SC_SYS_MEMORY_FREE, addr);
}

/* user memory totals; returns 0 on success */
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

void case_memory_allocate_1m(void)
{
	uint32_t a;
	conf_rec_rc("alloc", mem_alloc(SZ_1M, PAGE_1M, &a));
	rec_bool("addr_nonzero", a != 0);
	rec_bool("aligned_1m", (a % SZ_1M) == 0);
	conf_rec_rc("free", sc1(SC_SYS_MEMORY_FREE, a));
}

void case_memory_allocate_64k(void)
{
	uint32_t a;
	conf_rec_rc("alloc", mem_alloc(SZ_64K, PAGE_64K, &a));
	rec_bool("addr_nonzero", a != 0);
	rec_bool("aligned_64k", (a % SZ_64K) == 0);
	conf_rec_rc("free", sc1(SC_SYS_MEMORY_FREE, a));
}

/* sys_memory.cpp:183-186: flags == 0 is the 1 MiB page size */
void case_memory_allocate_default_flags(void)
{
	uint32_t a;
	conf_rec_rc("alloc", mem_alloc(SZ_1M, 0, &a));
	rec_bool("aligned_1m", a != 0 && (a % SZ_1M) == 0);
	conf_rec_rc("free", sc1(SC_SYS_MEMORY_FREE, a));
}

/* sys_memory.cpp:183-191: only 1M, 64K and 0 are accepted (4K is not); the out word is not written on failure */
void case_memory_allocate_bad_flags(void)
{
	static const uint64_t flags[] = { 0x100, 0x300, 0x800, 1, 0x600, 0x1000 };
	char key[16] = { 'f', 'l', 'a', 'g', 's', '_', '0', 0 };
	unsigned i;
	for (i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
		uint32_t a;
		key[6] = (char)('0' + i);
		conf_rec_rc(key, mem_alloc(SZ_1M, flags[i], &a));
		mem_free_quiet(a);
	}
}

/* sys_memory.cpp:177-180: size 0 -> CELL_EALIGN */
void case_memory_allocate_size_zero(void)
{
	uint32_t a;
	conf_rec_rc("rc", mem_alloc(0, PAGE_1M, &a));
	mem_free_quiet(a);
}

/* sys_memory.cpp:193-196: the size must be a multiple of the page size -> CELL_EALIGN */
void case_memory_allocate_size_alignment(void)
{
	uint32_t a;
	conf_rec_rc("1m_with_64k", mem_alloc(SZ_64K, PAGE_1M, &a));
	mem_free_quiet(a);
	conf_rec_rc("1m_plus_1", mem_alloc(SZ_1M + 1, PAGE_1M, &a));
	mem_free_quiet(a);
	conf_rec_rc("64k_half", mem_alloc(SZ_64K / 2, PAGE_64K, &a));
	mem_free_quiet(a);
	conf_rec_rc("64k_and_a_half", mem_alloc(SZ_64K + SZ_64K / 2, PAGE_64K, &a));
	mem_free_quiet(a);
	conf_rec_rc("odd_0x1234_64k", mem_alloc(0x1234, PAGE_64K, &a));
	mem_free_quiet(a);
	conf_rec_rc("64k_x17", mem_alloc(SZ_64K * 17, PAGE_64K, &a));
	rec_bool("64k_x17_aligned", a != 0 && (a % SZ_64K) == 0);
	mem_free_quiet(a);
	conf_rec_rc("default_flags_64k", mem_alloc(SZ_64K, 0, &a));
	mem_free_quiet(a);
}

/* sys_memory.cpp:202-205: more than the user memory holds -> CELL_ENOMEM */
void case_memory_allocate_too_big(void)
{
	uint32_t a;
	conf_rec_rc("1g", mem_alloc(0x40000000ull, PAGE_1M, &a));
	mem_free_quiet(a);
	conf_rec_rc("above_32bit", mem_alloc(0x100000000ull, PAGE_1M, &a));
	mem_free_quiet(a);
}

/* sys_memory.cpp:211-222: alloc_addr == NULL; RPCS3 releases the block again and answers CELL_EFAULT.
 * `avail_restored` records whether the console took the memory back too. */
void case_memory_allocate_null_addr(void)
{
	uint32_t t0, a0, t1, a1;
	(void)user_mem(&t0, &a0);
	conf_rec_rc("rc", sc3(SC_SYS_MEMORY_ALLOCATE, SZ_1M, PAGE_1M, 0));
	(void)user_mem(&t1, &a1);
	rec_bool("avail_restored", a1 == a0);
	rec_bool("total_same", t1 == t0);
}

/* sys_memory.cpp:305-310: a block is freed once; the second free and an unaligned address -> CELL_EINVAL */
void case_memory_free_errors(void)
{
	uint32_t a;
	(void)mem_alloc(SZ_1M, PAGE_1M, &a);
	conf_rec_rc("unaligned", sc1(SC_SYS_MEMORY_FREE, a + 0x1000));
	conf_rec_rc("first", sc1(SC_SYS_MEMORY_FREE, a));
	conf_rec_rc("second", sc1(SC_SYS_MEMORY_FREE, a));
	conf_rec_rc("zero", sc1(SC_SYS_MEMORY_FREE, 0));
	conf_rec_rc("unmapped_aligned", sc1(SC_SYS_MEMORY_FREE, 0xDEAD0000u));
}

/* sys_memory.cpp:376-416 + sys_memory.h:54-58: totals are reported and available never exceeds total */
void case_memory_user_memory_size(void)
{
	uint32_t total, avail;
	int bad = user_mem(&total, &avail);
	rec_bool("call_ok", bad == 0);
	rec_bool("total_nonzero", bad == 0 && total != 0);
	rec_bool("avail_nonzero", bad == 0 && avail != 0);
	rec_bool("avail_le_total", bad == 0 && avail <= total);
}

/* sys_memory.cpp:202-203, :313: an allocation lowers the available amount by its size, free gives it back */
void case_memory_allocate_changes_available(void)
{
	uint32_t t0, a0, t1, a1, t2, a2, addr;
	(void)user_mem(&t0, &a0);
	(void)mem_alloc(SZ_1M * 2, PAGE_1M, &addr);
	(void)user_mem(&t1, &a1);
	conf_rec_rc("free", sc1(SC_SYS_MEMORY_FREE, addr));
	(void)user_mem(&t2, &a2);
	conf_rec("avail_drop", (int64_t)a0 - (int64_t)a1);
	rec_bool("total_unchanged", t0 == t1 && t1 == t2);
	rec_bool("avail_restored", a2 == a0);
}

/* 64K pages count by their size too */
void case_memory_allocate_64k_changes_available(void)
{
	uint32_t t0, a0, t1, a1, addr;
	(void)user_mem(&t0, &a0);
	(void)mem_alloc(SZ_64K * 3, PAGE_64K, &addr);
	(void)user_mem(&t1, &a1);
	mem_free_quiet(addr);
	conf_rec("avail_drop", (int64_t)a0 - (int64_t)a1);
}

/* sys_memory.cpp:343-373: page attribute of a 1 MiB block: read/write, any access right, the page size */
void case_memory_page_attribute(void)
{
	uint32_t a;
	conf_addr at = conf_alloc(24, 8);
	(void)mem_alloc(SZ_1M, PAGE_1M, &a);
	conf_w64(at, SENT64);
	conf_w64(at + 8, SENT64);
	conf_w32(at + 16, SENT32);
	conf_w32(at + 20, SENT32);
	conf_rec_rc("rc", sc2(SC_SYS_MEMORY_GET_PAGE_ATTRIBUTE, a, at));
	conf_rec("attribute", (int64_t)conf_r64(at));
	conf_rec("access_right", (int64_t)conf_r64(at + 8));
	conf_rec("page_size", (int64_t)conf_r32(at + 16));
	conf_rec("pad", (int64_t)conf_r32(at + 20));
	mem_free_quiet(a);
}

/* same for a 64K block */
void case_memory_page_attribute_64k(void)
{
	uint32_t a;
	conf_addr at = conf_alloc(24, 8);
	(void)mem_alloc(SZ_64K, PAGE_64K, &a);
	conf_rec_rc("rc", sc2(SC_SYS_MEMORY_GET_PAGE_ATTRIBUTE, a, at));
	conf_rec("page_size", (int64_t)conf_r32(at + 16));
	mem_free_quiet(a);
}

/* sys_memory.cpp:345-348: an address nothing is mapped at -> CELL_EINVAL */
void case_memory_page_attribute_unmapped(void)
{
	conf_addr at = conf_alloc(24, 8);
	conf_rec_rc("addr_0x1000", sc2(SC_SYS_MEMORY_GET_PAGE_ATTRIBUTE, 0x1000, at));
	conf_rec_rc("addr_0", sc2(SC_SYS_MEMORY_GET_PAGE_ATTRIBUTE, 0, at));
}

/* Group "ppu_thread": the PPU thread calls that need no second thread.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4), rpcs3/Emu/Cell/lv2/sys_ppu_thread.cpp:
 *   yield :165-181  join :183-268  detach :270-316  get_join_state :318-329
 *   set_priority :331-364  get_priority :366-426  get_stack_information :428-436
 * sys_ppu_thread.h:21-25 sys_ppu_thread_stack_t (addr, size). A calling thread's own id is not
 * reachable through a syscall, so the self-referencing cases live in threads.c (CONF_THREADS).
 */
#include "cc.h"

#define BOGUS_ID 0x7FFFFF01u
#define S32(x) ((uint64_t)(int64_t)(int32_t)(x))

/* sys_ppu_thread.cpp:171-180: 0 after a context switch, 1 (CELL_CANCEL) when there was none; either is a success */
void case_ppu_thread_yield(void)
{
	int64_t rc = sc0(SC_SYS_PPU_THREAD_YIELD);
	rec_bool("rc_0_or_1", rc == 0 || rc == 1);
}

/* sys_ppu_thread.cpp:415-418: unknown thread -> CELL_ESRCH (the output is not written) */
void case_ppu_thread_get_priority_unknown(void)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, SENT32);
	conf_rec_rc("rc", sc2(SC_SYS_PPU_THREAD_GET_PRIORITY, BOGUS_ID, p));
	rec_bool("untouched", conf_r32(p) == SENT32);
}

/* sys_ppu_thread.cpp:337-340 comes before the thread lookup (:358-361): a priority above 3071 is
 * CELL_EINVAL whoever the thread is; an in-range priority on an unknown thread is CELL_ESRCH */
void case_ppu_thread_set_priority_checks(void)
{
	conf_rec_rc("range_high", sc2(SC_SYS_PPU_THREAD_SET_PRIORITY, BOGUS_ID, S32(3072)));
	conf_rec_rc("range_very_high", sc2(SC_SYS_PPU_THREAD_SET_PRIORITY, BOGUS_ID, S32(0x7FFFFFFF)));
	conf_rec_rc("range_low", sc2(SC_SYS_PPU_THREAD_SET_PRIORITY, BOGUS_ID, S32(-0x7FFFFFFF)));
	conf_rec_rc("in_range_unknown", sc2(SC_SYS_PPU_THREAD_SET_PRIORITY, BOGUS_ID, 1000));
	conf_rec_rc("zero_unknown", sc2(SC_SYS_PPU_THREAD_SET_PRIORITY, BOGUS_ID, 0));
	conf_rec_rc("top_unknown", sc2(SC_SYS_PPU_THREAD_SET_PRIORITY, BOGUS_ID, 3071));
}

/* sys_ppu_thread.cpp:322-328: the calling thread's join state is written; NULL -> CELL_EFAULT */
void case_ppu_thread_get_join_state(void)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, SENT32);
	conf_rec_rc("rc", sc1(SC_SYS_PPU_THREAD_GET_JOIN_STATE, p));
	rec_bool("written", conf_r32(p) != SENT32);
	rec_bool("is_0_or_1", conf_r32(p) <= 1);
	conf_rec_rc("null", sc1(SC_SYS_PPU_THREAD_GET_JOIN_STATE, 0));
}

/* sys_ppu_thread.cpp:428-436: the stack of the calling thread */
void case_ppu_thread_get_stack_information(void)
{
	conf_addr p = conf_alloc(8, 4);
	uint32_t addr, size;
	conf_w32(p, SENT32);
	conf_w32(p + 4, SENT32);
	conf_rec_rc("rc", sc1(SC_SYS_PPU_THREAD_GET_STACK_INFORMATION, p));
	addr = conf_r32(p);
	size = conf_r32(p + 4);
	rec_bool("addr_written", addr != SENT32);
	rec_bool("addr_nonzero", addr != 0 && addr != SENT32);
	rec_bool("size_nonzero", size != 0 && size != SENT32);
	rec_bool("size_4k_multiple", size != SENT32 && (size & 0xFFF) == 0);
	rec_bool("range_fits_32bit", addr != SENT32 && size != SENT32 && (uint64_t)addr + size <= 0x100000000ull);
}

/* sys_ppu_thread.cpp:224-227 and :276: an unknown thread -> CELL_ESRCH from join and detach */
void case_ppu_thread_join_detach_unknown(void)
{
	conf_addr r = conf_alloc(8, 8);
	conf_rec_rc("join", sc2(SC_SYS_PPU_THREAD_JOIN, BOGUS_ID, r));
	conf_rec_rc("detach", sc1(SC_SYS_PPU_THREAD_DETACH, BOGUS_ID));
}

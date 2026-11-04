/* Group "time": the lv2 time syscalls and the timebase.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4), rpcs3/Emu/Cell/lv2/sys_time.cpp:
 *   - sys_time_get_timezone (144) :272  writes both out values, returns CELL_OK
 *   - sys_time_get_current_time (145) :343  sec NULL -> CELL_EFAULT (:347); sec is written before
 *     nsec is checked, nsec NULL -> CELL_EFAULT (:384)
 *   - sys_time_get_timebase_frequency (147) :440  returns the frequency itself (RPCS3 uses 80 MHz,
 *     g_timebase_freq at :143; the consoles seen so far report 79 800 000)
 * Whether the real kernel agrees is what the hardware rows are for.
 */
#include "cc.h"

#define SYS_TIME_GET_TIMEZONE 144
#define SYS_TIME_GET_CURRENT_TIME 145
#define SYS_TIME_GET_TIMEBASE_FREQUENCY 147

/* the frequency, and that it is what conf_tbfreq() (the backend's own source) reports */
void case_time_timebase_freq(void)
{
	int64_t f = sc0(SYS_TIME_GET_TIMEBASE_FREQUENCY);
	conf_rec("freq", f);
	rec_bool("matches_backend", (uint64_t)f == conf_tbfreq());
	rec_bool("nonzero", f != 0);
}

/* two reads of the timebase never go backwards, and time passes between a syscall and a read */
void case_time_tb_monotonic(void)
{
	uint64_t t0 = conf_tb();
	uint64_t t1 = conf_tb();
	(void)sc0(SYS_TIME_GET_TIMEBASE_FREQUENCY);
	uint64_t t2 = conf_tb();
	rec_bool("nondecreasing_01", t1 >= t0);
	rec_bool("nondecreasing_12", t2 >= t1);
	conf_rec_time("syscall_ticks", t2 - t1);
}

/* sys_time_get_current_time: both outputs written, nanoseconds below one second, a plausible epoch */
void case_time_current_time(void)
{
	conf_addr a = conf_alloc(16, 8);
	int64_t rc;
	uint64_t sec, nsec;
	conf_w64(a, SENT64);
	conf_w64(a + 8, SENT64);
	rc = sc2(SYS_TIME_GET_CURRENT_TIME, a, a + 8);
	sec = conf_r64(a);
	nsec = conf_r64(a + 8);
	conf_rec_rc("rc", rc);
	rec_bool("sec_written", sec != SENT64);
	rec_bool("nsec_written", nsec != SENT64);
	rec_bool("sec_after_2001", sec > 1000000000ull && sec < 4102444800ull);
	rec_bool("nsec_below_1e9", nsec < 1000000000ull);
}

/* the wall clock read twice in a row does not go backwards */
void case_time_current_time_monotonic(void)
{
	conf_addr a = conf_alloc(32, 8);
	int64_t rc1 = sc2(SYS_TIME_GET_CURRENT_TIME, a, a + 8);
	int64_t rc2 = sc2(SYS_TIME_GET_CURRENT_TIME, a + 16, a + 24);
	uint64_t s1 = conf_r64(a), n1 = conf_r64(a + 8);
	uint64_t s2 = conf_r64(a + 16), n2 = conf_r64(a + 24);
	conf_rec_rc("rc1", rc1);
	conf_rec_rc("rc2", rc2);
	rec_bool("nondecreasing", s2 > s1 || (s2 == s1 && n2 >= n1));
}

/* sec == NULL (sys_time.cpp:347 checks it first, CELL_EFAULT) */
void case_time_current_time_null_sec(void)
{
	conf_addr a = conf_alloc(8, 8);
	conf_w64(a, SENT64);
	conf_rec_rc("rc", sc2(SYS_TIME_GET_CURRENT_TIME, 0, a));
	rec_bool("nsec_untouched", conf_r64(a) == SENT64);
}

/* nsec == NULL (sys_time.cpp:384: sec is written, then CELL_EFAULT) */
void case_time_current_time_null_nsec(void)
{
	conf_addr a = conf_alloc(8, 8);
	conf_w64(a, SENT64);
	conf_rec_rc("rc", sc2(SYS_TIME_GET_CURRENT_TIME, a, 0));
	rec_bool("sec_written", conf_r64(a) != SENT64);
}

/* sys_time_get_timezone (sys_time.cpp:272): CELL_OK, both words written. The values are the
 * console's settings, so only plausibility is recorded: minutes east of UTC within +-14 h and a
 * summer-time offset of 0 or 60. */
void case_time_timezone(void)
{
	conf_addr a = conf_alloc(8, 4);
	int64_t rc;
	int32_t tz, dst;
	conf_w32(a, SENT32);
	conf_w32(a + 4, SENT32);
	rc = sc2(SYS_TIME_GET_TIMEZONE, a, a + 4);
	tz = (int32_t)conf_r32(a);
	dst = (int32_t)conf_r32(a + 4);
	conf_rec_rc("rc", rc);
	rec_bool("tz_written", (uint32_t)tz != SENT32);
	rec_bool("dst_written", (uint32_t)dst != SENT32);
	rec_bool("tz_plausible", tz >= -14 * 60 && tz <= 14 * 60);
	rec_bool("dst_plausible", dst == 0 || dst == 60);
}

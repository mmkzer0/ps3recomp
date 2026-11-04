/* Group "slot96": the slots issue #96 names, called with arguments that name nothing (unknown group
 * or thread id, descriptor -1, a zeroed image), so the call can only fail. A handler that answers
 * CELL_OK here is a stub standing in for work it does not do; a handler bound to the wrong family
 * gives that family's error.
 *
 * Oracle at the pin (RPCS3 d00d80231af3f9b0286f38fd72f7513583f1a9d4), rpcs3/Emu/Cell/lv2/:
 *   lv2.cpp      slot table: 158 _sys_spu_image_close :294, 169 sys_spu_initialize :305,
 *                174-176 sys_spu_thread_group_suspend/resume/yield :310-312,
 *                192-194 sys_spu_thread_disconnect_event/bind_queue/unbind_queue :328-330,
 *                820 sys_fs_fsync :803, 832 sys_fs_ftruncate :815
 *   sys_spu.cpp  initialize :483-509, _sys_spu_image_close :609-629, group suspend/resume/yield
 *                :1355/:1443/:1536, disconnect_event/bind_queue/unbind_queue :2211/:2244/:2296
 * No SPU is started and no file is opened or changed.
 */
#include "cc.h"

#define BOGUS_ID 0x7FFFFF01u
#define FD_NONE ((uint64_t)(int64_t)-1)

/* sys_spu.cpp:615-618: an image whose type is not SYS_SPU_IMAGE_TYPE_KERNEL is refused (CELL_EINVAL).
 * The image is a zeroed scratch struct: RPCS3 reads through the pointer without a NULL check. */
void case_slot96_spu_image_close_zeroed(void)
{
	conf_rec_rc("rc", sc1(158, conf_alloc(32, 8)));
}

/* sys_spu.cpp:491-508: a repeated initialisation with the same limits succeeds while no SPU group exists */
void case_slot96_spu_initialize_twice(void)
{
	conf_rec_rc("first", sc2(169, 6, 0));
	conf_rec_rc("second", sc2(169, 6, 0));
}

/* sys_spu.cpp:1355, :1443, :1536: an unknown thread group -> CELL_ESRCH */
void case_slot96_spu_group_unknown(void)
{
	conf_rec_rc("suspend_174", sc1(174, BOGUS_ID));
	conf_rec_rc("resume_175", sc1(175, BOGUS_ID));
	conf_rec_rc("yield_176", sc1(176, BOGUS_ID));
}

/* sys_spu.cpp:2211, :2244, :2296: an unknown SPU thread -> CELL_ESRCH */
void case_slot96_spu_thread_unknown(void)
{
	conf_rec_rc("disconnect_event_192", sc3(192, BOGUS_ID, 0, 0));
	conf_rec_rc("bind_queue_193", sc3(193, BOGUS_ID, 0, 0));
	conf_rec_rc("unbind_queue_194", sc2(194, BOGUS_ID, 0));
}

/* lv2.cpp slot 820 is sys_fs_fsync: descriptor -1 is not open -> an error, never CELL_OK */
void case_slot96_fs_fsync_bad_fd(void)
{
	conf_rec_rc("rc", sc1(820, FD_NONE));
}

/* lv2.cpp slot 832 is sys_fs_ftruncate: descriptor -1 is not open -> an error, never CELL_OK */
void case_slot96_fs_ftruncate_bad_fd(void)
{
	conf_rec_rc("rc", sc2(832, FD_NONE, 0));
}

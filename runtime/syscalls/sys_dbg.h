/*
 * ps3recomp - lv2 debug-interface syscalls (the 0x3DC slot)
 *
 * Kept apart from lv2_register.c so the loader fixture (tests/test_loader_gaps.cpp)
 * can link the one handler through the real dispatcher.
 */

#ifndef SYS_DBG_H
#define SYS_DBG_H

#include "lv2_syscall_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Syscall 988 (0x3DC). Takes no guest pointer: it neither reads nor writes
 * guest memory and never stops. See sys_dbg.c for what the answer rests on. */
int64_t sys_dbg_988(ppu_context* ctx);

void sys_dbg_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_DBG_H */

/*
 * ps3recomp - sys_crashdump HLE
 */

#ifndef SYS_CRASHDUMP_H
#define SYS_CRASHDUMP_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#define SYS_CRASH_DUMP_MAX_LABEL_SIZE 16
#define SYS_CRASH_DUMP_MAX_LOG_AREA   127

typedef struct sys_crash_dump_log_area_info_t {
    char label[SYS_CRASH_DUMP_MAX_LABEL_SIZE];
    u32 addr;
    u32 size;
} sys_crash_dump_log_area_info_t;

#ifdef __cplusplus
extern "C" {
#endif

s32 sys_crash_dump_set_user_log_area(u8 index, const sys_crash_dump_log_area_info_t* new_entry);
s32 sys_crash_dump_get_user_log_area(u8 index, sys_crash_dump_log_area_info_t* entry);

#ifdef __cplusplus
}
#endif

#endif /* SYS_CRASHDUMP_H */

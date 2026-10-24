/*
 * ps3recomp - sys_crashdump HLE implementation
 *
 * Implements sys_crash_dump_set_user_log_area / sys_crash_dump_get_user_log_area.
 * Applications register memory buffers containing user log data to be captured
 * in system crash dumps.
 *
 * No firmware PRX body is available in the repository. The signature and error
 * contract (CELL_EINVAL on index > 127 or NULL pointer, CELL_OK on success)
 * follow oracle RPCS3 sys_crashdump.cpp at commit e53ac18f. Out-of-bounds guest
 * spans fail closed with ps3_stop_guest_range (hle-guest-range).
 */

#include "sys_crashdump.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "ps3emu/stop.h"
#include "ps3emu/log_first.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&s_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&s_lock)
#define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;

typedef struct {
    int in_use;
    char label[SYS_CRASH_DUMP_MAX_LABEL_SIZE];
    u32 addr;
    u32 size;
} CrashDumpLogArea;

static CrashDumpLogArea s_log_areas[SYS_CRASH_DUMP_MAX_LOG_AREA + 1];

static inline uint64_t guest_limit(void)
{
    return ppu_vm_size ? (uint64_t)ppu_vm_size : 0x100000000ull;
}

static inline int guest_range_ok(uint64_t ea, uint64_t size)
{
    const uint64_t limit = guest_limit();
    return ea <= limit && size <= limit - ea;
}

s32 sys_crash_dump_set_user_log_area(u8 index, const sys_crash_dump_log_area_info_t* new_entry)
{
    if (index > SYS_CRASH_DUMP_MAX_LOG_AREA || !new_entry) {
        return (s32)CELL_EINVAL;
    }
    const u32 ea = (u32)(uintptr_t)new_entry;
    if (!guest_range_ok(ea, sizeof(sys_crash_dump_log_area_info_t))) {
        ps3_stop_guest_range(__func__, "new_entry", ea, sizeof(sys_crash_dump_log_area_info_t), guest_limit());
    }

    LOCK();
    CrashDumpLogArea* area = &s_log_areas[index];
    memcpy(area->label, vm_base + ea, SYS_CRASH_DUMP_MAX_LABEL_SIZE);
    area->label[SYS_CRASH_DUMP_MAX_LABEL_SIZE - 1] = '\0';
    area->addr = vm_read32(ea + 16);
    area->size = vm_read32(ea + 20);
    area->in_use = 1;
    UNLOCK();

    static int _n = 0;
    if (PS3_LOG_FIRST(_n, 8)) {
        printf("[sys_crashdump] set_user_log_area(index=%u, label='%.16s', addr=0x%08X, size=%u)\n",
               index, area->label, area->addr, area->size);
    }
    return CELL_OK;
}

s32 sys_crash_dump_get_user_log_area(u8 index, sys_crash_dump_log_area_info_t* entry)
{
    if (index > SYS_CRASH_DUMP_MAX_LOG_AREA || !entry) {
        return (s32)CELL_EINVAL;
    }
    const u32 ea = (u32)(uintptr_t)entry;
    if (!guest_range_ok(ea, sizeof(sys_crash_dump_log_area_info_t))) {
        ps3_stop_guest_range(__func__, "entry", ea, sizeof(sys_crash_dump_log_area_info_t), guest_limit());
    }

    LOCK();
    const CrashDumpLogArea* area = &s_log_areas[index];
    if (area->in_use) {
        memcpy(vm_base + ea, area->label, SYS_CRASH_DUMP_MAX_LABEL_SIZE);
        vm_write32(ea + 16, area->addr);
        vm_write32(ea + 20, area->size);
    } else {
        memset(vm_base + ea, 0, sizeof(sys_crash_dump_log_area_info_t));
    }
    UNLOCK();

    return CELL_OK;
}

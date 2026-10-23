/*
 * ps3recomp - sysPrxForUser _sys_spu_printf_* family
 *
 * The user-space half of the SDK's SPU printf server. A title's
 * spu_printf_initialize() (libsputhread) hands four callbacks to
 * _sys_spu_printf_initialize(); the SPU thread/group creation paths then call
 * the attach/detach functions below so the server learns which SPU groups and
 * threads exist. Every one of those is a thin dispatch to the matching
 * callback the title registered.
 *
 * Exports (NIDs computed from the names, tools/nid_database.py):
 *   0x45FE2FCE  _sys_spu_printf_initialize
 *   0xDD3B27AC  _sys_spu_printf_finalize
 *   0xDD0C1E09  _sys_spu_printf_attach_group
 *   0x5FDFB2FE  _sys_spu_printf_detach_group
 *   0x1AE10B92  _sys_spu_printf_attach_thread
 *   0xB3BBCF2A  _sys_spu_printf_detach_thread
 *
 * Semantics, from RPCS3 rpcs3/Emu/Cell/Modules/sys_spu_.cpp (RPCS3/rpcs3,
 * last change to that file cd87a646219024b136f7cc4d0ceeb1a19d5c43ad, 2025-02-11;
 * lines 11-14 globals, 412-483 the six functions, 495-500 registration;
 * callback type sysPrxForUser.h:11  `using spu_printf_cb_t = vm::ptr<s32(u32 arg)>`):
 *
 *   s32 _sys_spu_printf_initialize(agcb, dgcb, atcb, dtcb)
 *       Stores the four callback OPDs (attach-group, detach-group,
 *       attach-thread, detach-thread) in process-global state. No argument is
 *       validated; a NULL callback is stored as NULL. Always CELL_OK. A second
 *       call replaces the first.
 *   s32 _sys_spu_printf_finalize(void)
 *       Zeroes all four callbacks. Always CELL_OK.
 *   s32 _sys_spu_printf_attach_group(u32 group)    -> agcb(group)
 *   s32 _sys_spu_printf_detach_group(u32 group)    -> dgcb(group)
 *   s32 _sys_spu_printf_attach_thread(u32 thread)  -> atcb(thread)
 *   s32 _sys_spu_printf_detach_thread(u32 thread)  -> dtcb(thread)
 *       The matching callback is a guest function s32 cb(u32 id). If it is NULL
 *       (never initialised, or finalised) the result is CELL_ESTAT and nothing
 *       is called; otherwise the callback runs on the caller's thread and its
 *       return value is the function's return value, unchanged. These keep the
 *       firmware's s32 signature: the HLE adapter (ppu_hle_adapt.hpp) calls
 *       them through it and sign-extends a 32-bit signed result into r3, so
 *       CELL_ESTAT reaches the guest as 0xFFFFFFFF8001000F.
 *
 * Evidence anchor: the retail firmware set examined locally does not contain
 * a liblv2 PRX exporting these symbols, so RPCS3 (rpcs3/Emu/Cell/Modules/
 * sys_spu_.cpp, commit cd87a646219024b136f7cc4d0ceeb1a19d5c43ad) is the
 * reference for the return codes; whether the retail initialize rejects a
 * second call is not established. This implementation follows RPCS3 (replace).
 *
 * Thread safety: RPCS3 keeps bare globals. Guest threads are host threads here,
 * so the four OPDs live behind one lock and every attach/detach takes a
 * consistent snapshot of its callback under that lock. The guest callback runs
 * with the lock RELEASED: it is arbitrary guest code that may itself reach
 * attach/detach/finalize (SPU thread creation inside the server), and holding a
 * lock across it would self-deadlock. A finalize that races an in-flight
 * attach can therefore still see that one callback complete after finalize
 * returns, which is the same window the real unsynchronised store has.
 *
 * The callbacks are guest OPDs, not host function pointers: they run through
 * ppu_guest_call (runtime/ppu/ppu_loader.cpp), which also returns the guest r3.
 */

#include "sysPrxForUser.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

extern u64 ppu_guest_call(u32 opd_addr,
                          u64 a0, u64 a1, u64 a2, u64 a3,
                          u64 a4, u64 a5, u64 a6, u64 a7);

enum {
    SPU_PRINTF_ATTACH_GROUP,
    SPU_PRINTF_DETACH_GROUP,
    SPU_PRINTF_ATTACH_THREAD,
    SPU_PRINTF_DETACH_THREAD,
    SPU_PRINTF_CB_COUNT
};

static u32 s_spu_printf_cb[SPU_PRINTF_CB_COUNT];   /* guest OPD addresses */

#ifdef _WIN32
static SRWLOCK s_spu_printf_lock = SRWLOCK_INIT;
static void spu_printf_lock(void)   { AcquireSRWLockExclusive(&s_spu_printf_lock); }
static void spu_printf_unlock(void) { ReleaseSRWLockExclusive(&s_spu_printf_lock); }
#else
static pthread_mutex_t s_spu_printf_lock = PTHREAD_MUTEX_INITIALIZER;
static void spu_printf_lock(void)   { pthread_mutex_lock(&s_spu_printf_lock); }
static void spu_printf_unlock(void) { pthread_mutex_unlock(&s_spu_printf_lock); }
#endif

s32 _sys_spu_printf_initialize(u32 agcb, u32 dgcb, u32 atcb, u32 dtcb)
{
    spu_printf_lock();
    s_spu_printf_cb[SPU_PRINTF_ATTACH_GROUP]  = agcb;
    s_spu_printf_cb[SPU_PRINTF_DETACH_GROUP]  = dgcb;
    s_spu_printf_cb[SPU_PRINTF_ATTACH_THREAD] = atcb;
    s_spu_printf_cb[SPU_PRINTF_DETACH_THREAD] = dtcb;
    spu_printf_unlock();
    return CELL_OK;
}

s32 _sys_spu_printf_finalize(void)
{
    spu_printf_lock();
    for (int i = 0; i < SPU_PRINTF_CB_COUNT; i++)
        s_spu_printf_cb[i] = 0;
    spu_printf_unlock();
    return CELL_OK;
}

static s32 spu_printf_dispatch(int which, u32 id)
{
    spu_printf_lock();
    const u32 opd = s_spu_printf_cb[which];
    spu_printf_unlock();
    if (!opd)
        return (s32)CELL_ESTAT;
    /* The callback's own r3 above the low word is not part of an s32 result. */
    return (s32)(u32)ppu_guest_call(opd, id, 0, 0, 0, 0, 0, 0, 0);
}

s32 _sys_spu_printf_attach_group(u32 group)
{
    return spu_printf_dispatch(SPU_PRINTF_ATTACH_GROUP, group);
}

s32 _sys_spu_printf_detach_group(u32 group)
{
    return spu_printf_dispatch(SPU_PRINTF_DETACH_GROUP, group);
}

s32 _sys_spu_printf_attach_thread(u32 thread)
{
    return spu_printf_dispatch(SPU_PRINTF_ATTACH_THREAD, thread);
}

s32 _sys_spu_printf_detach_thread(u32 thread)
{
    return spu_printf_dispatch(SPU_PRINTF_DETACH_THREAD, thread);
}

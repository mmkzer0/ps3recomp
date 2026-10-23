/*
 * ps3recomp - lightweight mutex and condition variable (sysPrxForUser)
 *
 * sys_lwmutex_* and sys_lwcond_* are the guest-visible half of two lv2 kernel
 * objects. They are context handlers, not generated-table handlers: they need
 * the calling guest thread's id (lwmutex ownership is by thread id) and all
 * their pointer arguments are guest effective addresses. See sys_lwsync.cpp.
 */

#ifndef PS3RECOMP_SYS_LWSYNC_H
#define PS3RECOMP_SYS_LWSYNC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the eleven handlers with ps3_hle_register_ctx. Called by
 * ppu_sysprx_register. */
void ps3_lwsync_register(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SYS_LWSYNC_H */

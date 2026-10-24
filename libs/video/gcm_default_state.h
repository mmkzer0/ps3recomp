/*
 * ps3recomp - the driver default-state subroutine at IO offset 0 (see gcm_default_state.c)
 */

#ifndef PS3RECOMP_GCM_DEFAULT_STATE_H
#define PS3RECOMP_GCM_DEFAULT_STATE_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The subroutine's size in bytes, RET included: 0x98C bytes of methods, then the RET word. */
#define GCM_DEFAULT_STATE_BYTES 0x990u

/* Write the subroutine at guest EA `ea` (big-endian). Returns its size in bytes. */
u32 gcm_emit_default_state(u32 ea);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_GCM_DEFAULT_STATE_H */

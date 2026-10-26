/*
 * ps3recomp - the object class a FIFO subchannel is bound to, and what a method on it may mean
 *
 * A subchannel is a binding slot, not an engine: SET_OBJECT (method 0) names the object that the
 * slot's methods drive, and the object's class decides what a method number is. Every method the
 * walker (cellGcmSys.c) or the register-file dispatch (rsx_dispatch.c) sees is exactly one of
 *
 *   handled   a consumer reads it: it changes what an executed command does,
 *   inert     an allow-list entry below, each with the evidence that nothing observable depends on it,
 *   unknown   neither. Counted per (class, method); the frame it belongs to is invalid in a capture.
 *
 * Header-only on purpose: rsx_commands.c and rsx_dispatch.c are compiled alone by CI fixtures.
 */
#ifndef PS3RECOMP_RSX_METHOD_CLASS_H
#define PS3RECOMP_RSX_METHOD_CLASS_H

#include <stdint.h>

/* Object handles libgcm_sys 4.93 binds in its default-state subroutine (gcm_default_state.c: the
 * SET_OBJECT words on subchannels 0, 1, 3, 4, 5, 6 and 7). NV4097, NV0039 and NV3062 are the values
 * RPCS3 resets its method registers to (Emu/RSX/rsx_methods.cpp, NV4097_SET_OBJECT / NV0039_SET_OBJECT /
 * NV3062_SET_OBJECT). [INFERENCE] NV308A and NV309E are named from the methods the default state and a
 * title send to them: 0x184-0x19C and 0x2FC are NV308A's contexts and operation, a bare 0x184 is
 * NV309E's image DMA. */
#define RSX_OBJ_NV4097  0x31337000u
#define RSX_OBJ_NV0039  0x31337303u
#define RSX_OBJ_NV3062  0x313371C3u
#define RSX_OBJ_NV308A  0x31337808u
#define RSX_OBJ_NV309E  0x31337A73u
#define RSX_OBJ_NV3089  0x3137AF00u
#define RSX_OBJ_DRIVER  0xCAFEBABEu

typedef enum {
    RSX_CLASS_UNBOUND = 0,    /* nothing has bound this subchannel since the context was set up */
    RSX_CLASS_UNREGISTERED,   /* bound to a handle this table does not know */
    RSX_CLASS_NV406E,         /* channel control: methods below 0x100 on any subchannel */
    RSX_CLASS_NV4097,
    RSX_CLASS_NV0039,
    RSX_CLASS_NV3062,
    RSX_CLASS_NV308A,
    RSX_CLASS_NV309E,
    RSX_CLASS_NV3089,
    RSX_CLASS_DRIVER,         /* libgcm's own object: flip, queue and user-command words */
    RSX_CLASS_COUNT
} rsx_class_id;

static inline const char* rsx_class_name(rsx_class_id c)
{
    static const char* const names[RSX_CLASS_COUNT] = {
        "unbound", "unregistered", "NV406E", "NV4097", "NV0039", "NV3062", "NV308A", "NV309E", "NV3089",
        "driver",
    };
    return (unsigned)c < RSX_CLASS_COUNT ? names[c] : "invalid";
}

/* The class a SET_OBJECT handle names. 0 is the reset value, not a binding. */
static inline rsx_class_id rsx_class_from_handle(uint32_t handle)
{
    switch (handle) {
    case 0:              return RSX_CLASS_UNBOUND;
    case RSX_OBJ_NV4097: return RSX_CLASS_NV4097;
    case RSX_OBJ_NV0039: return RSX_CLASS_NV0039;
    case RSX_OBJ_NV3062: return RSX_CLASS_NV3062;
    case RSX_OBJ_NV308A: return RSX_CLASS_NV308A;
    case RSX_OBJ_NV309E: return RSX_CLASS_NV309E;
    case RSX_OBJ_NV3089: return RSX_CLASS_NV3089;
    case RSX_OBJ_DRIVER: return RSX_CLASS_DRIVER;
    default:             return RSX_CLASS_UNREGISTERED;
    }
}

typedef enum { RSX_METHOD_HANDLED = 0, RSX_METHOD_INERT, RSX_METHOD_UNKNOWN } rsx_method_verdict;

/* The allow-list. An entry needs evidence that no consumer would behave differently if the method
 * were dropped; "nobody handles it" is not evidence. M3-SCOPE Amendment 1 (finding 5) leaves the
 * remaining unclassified methods to slice S1, so this list stays short on purpose. */
static inline int rsx_method_inert(rsx_class_id c, uint32_t method)
{
    switch (c) {
    case RSX_CLASS_NV4097:
        /* 0x0100 NO_OPERATION: RPCS3 resets the register and binds no handler to it
         * (rsx_methods.cpp, NV4097_NO_OPERATION); rsx_dispatch.c already carries it as an explicit no-op. */
        return method == 0x0100u;
    case RSX_CLASS_NV0039:
        /* 0x0180 DMA_NOTIFY (envytools rnndb graph/nv_m2mf.xml): the notifier the hardware would write on
         * BUF_NOTIFY. RPCS3's nv0039::buffer_notify never reads it (its `notify` argument only reaches a
         * trace line) and moves the same bytes. */
        return method == 0x0180u;
    default:
        return 0;
    }
}

#endif

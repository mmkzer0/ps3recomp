/*
 * ps3recomp - sceNpSignaling HLE implementation
 *
 * Contexts, handlers and the connections to Matching2 room members. A
 * connection comes up when sceNpMatching2.c reports the member up
 * (np_signaling_room_peer) and is mutual once the title has also activated it;
 * events go to each context's handler and extended handler with the SDK values.
 * Connections to peers outside a room stay PENDING. Nothing is sent from here:
 * the title's own P2P sockets carry the traffic.
 *
 * Threads: the handlers run on whichever guest thread calls them, and
 * np_signaling_room_peer on Matching2's own thread (its deferred "established"),
 * so s_ctx, s_conn and s_initialized are shared state. s_sig_lock guards both
 * tables; the guest-memory spans a handler touches are proved BEFORE the lock is
 * taken (a stop never leaves it held), and guest writes use a copy of the
 * connection taken under it.
 */

#include "sceNpSignaling.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "../../runtime/platform/win32_compat.h"   /* SRWLOCK */
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../../runtime/memory/vm.h"        /* vm_range_ok / vm_guest_limit: the guest arena rule */
#include "ps3emu/stop.h"                    /* ps3_stop_guest_range */
#include "../system/cellSysutil.h"          /* cellSysutilQueueGuestCallbackArgs */

/* The title's pointers arrive as raw guest EAs and the accessors below add vm_base without a bound, so every span
 * a handler reads or writes is proved inside guest memory first. A span that leaves it is no firmware error code
 * (the console would have faulted in its own copy): a named stop, before the first access. */
static void sig_span(const char* func, const char* arg, u32 ea, u32 size)
{
    if (!vm_range_ok(ea, size)) ps3_stop_guest_range(func, arg, ea, size, vm_guest_limit());
}

/* Internal state */

/* Written by Init/Term under s_sig_lock, read lock-free by every handler's ready check: acquire/release so a handler
 * that sees the flag also sees the context table Init cleared. */
static atomic_int s_initialized;

/* Guards s_ctx and s_conn. The handlers below take it after their span checks; conn_event, conn_find and
 * conn_check_mutual run with it held. The only call out while it is held is cellSysutilQueueGuestCallbackArgs, whose
 * lock is a leaf (it queues; guest callbacks run later, outside it, in cellSysutilCheckCallback), so the order
 * s_sig_lock -> sysutil queue lock has no reverse path. */
static SRWLOCK s_sig_lock = SRWLOCK_INIT;
static void sig_lock(void)   { AcquireSRWLockExclusive(&s_sig_lock); }
static void sig_unlock(void) { ReleaseSRWLockExclusive(&s_sig_lock); }

typedef struct {
    int in_use;
    SceNpSignalingHandler handler;
    void* handlerArg;
    SceNpSignalingHandler extHandler;   /* sceNpSignalingAddExtendedHandler (guest descriptor) */
    void* extHandlerArg;
} SignalingCtx;

static SignalingCtx s_ctx[SCE_NP_SIGNALING_CTX_MAX];

/* Context ids are slot + 1: titles treat id 0 as "no context". J-Stars Victory VS+ activates a room member's
 * connection only when its context id is nonzero; with the first context numbered 0 it never activated one.
 * Caller holds s_sig_lock and uses the pointer only inside it. */
static SignalingCtx* ctx_get(SceNpSignalingCtxId id)
{
    return (id >= 1 && id <= SCE_NP_SIGNALING_CTX_MAX && s_ctx[id - 1].in_use) ? &s_ctx[id - 1] : NULL;
}

/* Connections, one per peer (by online id), shared by all contexts; conn id = index + 1. Only room members made by
 * Matching2 come up for now: their address is the one psnr gives for the member's P2P socket. */
typedef struct {
    int used;
    char online_id[17];
    u8 ip[4];
    u16 port;
    s32 status;   /* SCE_NP_SIGNALING_CONN_STATUS_* */
    int peer_up;     /* Matching2 reported the member's P2P socket up (np_signaling_room_peer) */
    int activated;   /* the title called ActivateConnection for this peer */
    int mutual;      /* EXT_MUTUAL_ACTIVATED sent */
} SignalingConn;

static SignalingConn s_conn[SCE_NP_SIGNALING_CONN_MAX];

/* Caller holds s_sig_lock and uses the pointer only inside it. */
static SignalingConn* conn_get(SceNpSignalingConnId id)
{
    return (id >= 1 && id <= SCE_NP_SIGNALING_CONN_MAX && s_conn[id - 1].used) ? &s_conn[id - 1] : NULL;
}

/* A copy of a connection taken under the lock; false when there is none. The handlers that write to guest memory
 * work from the copy, so the guest write happens outside the lock. */
static int conn_snapshot(SceNpSignalingConnId id, SignalingConn* out)
{
    sig_lock();
    const SignalingConn* c = conn_get(id);
    if (c) *out = *c;
    sig_unlock();
    return c != NULL;
}

static u32 conn_addr(const SignalingConn* c)   /* in_addr, network order as a guest u32 */
{
    return ((u32)c->ip[0] << 24) | ((u32)c->ip[1] << 16) | ((u32)c->ip[2] << 8) | c->ip[3];
}

/* Every context's handler and extended handler: (ctxId, connId, event, errorCode, arg). The extended handler also
 * receives the normal events. Delivered on the title's next cellSysutilCheckCallback. Holds s_sig_lock: events of
 * two threads reach the queue in the order their state changes happened. */
static void conn_event(SceNpSignalingConnId id, s32 event, s32 err)
{
    const int ext_only = event >= SCE_NP_SIGNALING_EVENT_EXT_PEER_ACTIVATED;
    for (u32 i = 0; i < SCE_NP_SIGNALING_CTX_MAX; i++) {
        if (!s_ctx[i].in_use) continue;
        const u32 h[2] = { (u32)(uintptr_t)s_ctx[i].handler, (u32)(uintptr_t)s_ctx[i].extHandler };
        const u32 a[2] = { (u32)(uintptr_t)s_ctx[i].handlerArg, (u32)(uintptr_t)s_ctx[i].extHandlerArg };
        for (int k = ext_only; k < 2; k++) {
            if (!h[k]) continue;
            const u64 args[8] = { i + 1, id, (u64)(u32)event, (u64)(u32)err, a[k], 0, 0, 0 };
            cellSysutilQueueGuestCallbackArgs(h[k], args);
        }
    }
}

/* The connection to a peer by online id; create = make one (status INACTIVE) if there is none. Index or -1.
 * Holds s_sig_lock. */
static int conn_find(const char* online_id, int create)
{
    int freei = -1;
    for (int i = 0; i < SCE_NP_SIGNALING_CONN_MAX; i++) {
        if (s_conn[i].used && strcmp(s_conn[i].online_id, online_id) == 0) return i;
        if (!s_conn[i].used && freei < 0) freei = i;
    }
    if (!create || freei < 0) return -1;
    memset(&s_conn[freei], 0, sizeof(s_conn[freei]));
    s_conn[freei].used = 1;
    strncpy(s_conn[freei].online_id, online_id, 16);
    return freei;
}

/* Both ends active: the peer's side is up (room member) and this side activated it. The extended handler hears that
 * the peer activated the connection and that it is mutual; J-Stars Victory VS+ marks a room member connected only
 * on EXT_MUTUAL_ACTIVATED (on ESTABLISHED it only logs). Holds s_sig_lock. */
static void conn_check_mutual(int idx)
{
    SignalingConn* c = &s_conn[idx];
    if (c->mutual || !c->activated || c->status != SCE_NP_SIGNALING_CONN_STATUS_ACTIVE) return;
    c->mutual = 1;
    conn_event((SceNpSignalingConnId)(idx + 1), SCE_NP_SIGNALING_EVENT_EXT_PEER_ACTIVATED, 0);
    conn_event((SceNpSignalingConnId)(idx + 1), SCE_NP_SIGNALING_EVENT_EXT_MUTUAL_ACTIVATED, 0);
}

void np_signaling_room_peer(const char* online_id, const u8 ip[4], u16 port, int up)
{
    sig_lock();
    const int idx = conn_find(online_id, up);
    if (idx < 0) { sig_unlock(); return; }
    SignalingConn* c = &s_conn[idx];
    const s32 want = up ? SCE_NP_SIGNALING_CONN_STATUS_ACTIVE : SCE_NP_SIGNALING_CONN_STATUS_INACTIVE;
    c->peer_up = up != 0;
    if (up) { memcpy(c->ip, ip, 4); c->port = port; }
    if (c->status == want) { sig_unlock(); return; }
    c->status = want;
    printf("[sceNpSignaling] conn %d (%s, %u.%u.%u.%u:%u) %s\n", idx + 1, c->online_id, c->ip[0], c->ip[1], c->ip[2],
           c->ip[3], c->port, up ? "established" : "dead");
    conn_event((SceNpSignalingConnId)(idx + 1), up ? SCE_NP_SIGNALING_EVENT_ESTABLISHED : SCE_NP_SIGNALING_EVENT_DEAD, 0);
    if (up) conn_check_mutual(idx);
    else c->activated = c->mutual = 0;
    sig_unlock();
}

/* sceNpSignalingInit is not a firmware export (no NID): on the console signaling is usable once sceNpInit has run,
 * so a title calls sceNpSignalingCreateCtx straight after sceNpInit. Accept either, as sceNpBasic does. */
int np_is_initialized(void);
static int signaling_ready(void)
{
    return atomic_load_explicit(&s_initialized, memory_order_acquire) || np_is_initialized();
}

/* Lifecycle */

s32 sceNpSignalingInit(void)
{
    printf("[sceNpSignaling] Init()\n");
    sig_lock();
    if (atomic_load_explicit(&s_initialized, memory_order_relaxed)) {
        sig_unlock();
        return (s32)SCE_NP_SIGNALING_ERROR_ALREADY_INITIALIZED;
    }
    memset(s_ctx, 0, sizeof(s_ctx));
    atomic_store_explicit(&s_initialized, 1, memory_order_release);
    sig_unlock();
    return CELL_OK;
}

s32 sceNpSignalingTerm(void)
{
    printf("[sceNpSignaling] Term()\n");
    atomic_store_explicit(&s_initialized, 0, memory_order_release);
    return CELL_OK;
}

/* Context management */

s32 sceNpSignalingCreateCtx(const void* npId, SceNpSignalingHandler handler,
                              void* arg, SceNpSignalingCtxId* ctxId)
{
    (void)npId;
    printf("[sceNpSignaling] CreateCtx()\n");

    if (!signaling_ready()) return (s32)SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED;
    if (!ctxId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    sig_span("sceNpSignalingCreateCtx", "ctxId", (u32)(uintptr_t)ctxId, 4);

    s32 rc = (s32)SCE_NP_SIGNALING_ERROR_OUT_OF_MEMORY;
    u32 id = 0;
    sig_lock();
    for (int i = 0; i < SCE_NP_SIGNALING_CTX_MAX; i++) {
        if (!s_ctx[i].in_use) {
            memset(&s_ctx[i], 0, sizeof(SignalingCtx));
            s_ctx[i].in_use = 1;
            s_ctx[i].handler = handler;
            s_ctx[i].handlerArg = arg;
            id = (u32)i + 1;
            rc = CELL_OK;
            break;
        }
    }
    sig_unlock();
    if (rc == CELL_OK) vm_write32((u32)(uintptr_t)ctxId, id);
    return rc;
}

s32 sceNpSignalingDestroyCtx(SceNpSignalingCtxId ctxId)
{
    printf("[sceNpSignaling] DestroyCtx(%u)\n", ctxId);
    sig_lock();
    SignalingCtx* c = ctx_get(ctxId);
    if (c) c->in_use = 0;
    sig_unlock();
    return c ? CELL_OK : (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
}

/* The extended handler receives the extended events (peer activated, ...) besides the ones the CreateCtx handler
 * gets. A title may create the context with a null handler and register this one instead. Stored
 * here; room members' connections (np_signaling_room_peer) raise its events. */
s32 sceNpSignalingAddExtendedHandler(SceNpSignalingCtxId ctxId, SceNpSignalingHandler handler, void* arg)
{
    printf("[sceNpSignaling] AddExtendedHandler(ctx=%u, handler=0x%08X)\n", ctxId, (u32)(uintptr_t)handler);
    if (!signaling_ready()) return (s32)SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED;
    sig_lock();
    SignalingCtx* c = ctx_get(ctxId);
    if (c) { c->extHandler = handler; c->extHandlerArg = arg; }
    sig_unlock();
    return c ? CELL_OK : (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
}

/* Connection operations — offline stubs */

s32 sceNpSignalingActivateConnection(SceNpSignalingCtxId ctxId,
                                       const void* peerNpId,
                                       SceNpSignalingConnId* connId)
{
    sig_lock();
    const int ctx_ok = ctx_get(ctxId) != NULL;
    sig_unlock();
    if (!ctx_ok) return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    if (!peerNpId || !connId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    sig_span("sceNpSignalingActivateConnection", "peerNpId", (u32)(uintptr_t)peerNpId, 16);
    sig_span("sceNpSignalingActivateConnection", "connId", (u32)(uintptr_t)connId, 4);
    char id[17] = {0};
    for (u32 o = 0; o < 16; o++) id[o] = (char)vm_read8((u32)(uintptr_t)peerNpId + o);
    sig_lock();
    const int idx = conn_find(id, 1);
    if (idx < 0) { sig_unlock(); return (s32)SCE_NP_SIGNALING_ERROR_OUT_OF_MEMORY; }
    SignalingConn* c = &s_conn[idx];
    /* A connection the title deactivated (or one never up) restarts from what Matching2 last said of the peer: ACTIVE
     * when its socket is up, PENDING until it is. */
    if (c->status == SCE_NP_SIGNALING_CONN_STATUS_INACTIVE)
        c->status = c->peer_up ? SCE_NP_SIGNALING_CONN_STATUS_ACTIVE : SCE_NP_SIGNALING_CONN_STATUS_PENDING;
    c->activated = 1;
    printf("[sceNpSignaling] ActivateConnection(ctx=%u, %s) -> conn %d (%s)\n", ctxId, id, idx + 1,
           c->status == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE ? "active" : "pending");
    if (c->status == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE) {   /* already up: say so, as a new activation would */
        conn_event((SceNpSignalingConnId)(idx + 1), SCE_NP_SIGNALING_EVENT_ESTABLISHED, 0);
        conn_check_mutual(idx);
    }
    sig_unlock();
    vm_write32((u32)(uintptr_t)connId, (u32)(idx + 1));
    return CELL_OK;
}

/* The title takes its side of a connection back. Evidence: no firmware module for sceNp is in the decrypted set, so
 * this rests on RPCS3 at the pinned oracle (Modules/sceNp.cpp sceNpSignalingDeactivateConnection, NP/signaling_handler.cpp
 * stop_sig_nl with forceful = true): NOT_INITIALIZED before sceNpInit, and nothing else is checked (neither the context
 * nor the connection id: an id with no connection is a no-op CELL_OK). A known connection goes INACTIVE and loses its
 * activation WITHOUT any callback ("forceful: we don't go through any transition and don't call any CB"). The peer's
 * side is Matching2's: peer_up is kept, so an ActivateConnection later brings a member that is still in the room back
 * to ACTIVE. That last step is inference; RPCS3 restarts a handshake there. */
s32 sceNpSignalingDeactivateConnection(SceNpSignalingCtxId ctxId,
                                         SceNpSignalingConnId connId)
{
    (void)ctxId;
    if (!signaling_ready()) return (s32)SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED;
    sig_lock();
    SignalingConn* c = conn_get(connId);
    if (c) {
        c->status = SCE_NP_SIGNALING_CONN_STATUS_INACTIVE;
        c->activated = c->mutual = 0;
    }
    sig_unlock();
    return CELL_OK;
}

s32 sceNpSignalingGetConnectionStatus(SceNpSignalingCtxId ctxId,
                                        SceNpSignalingConnId connId,
                                        s32* connStatus,
                                        u32* peerAddr, u16* peerPort)
{
    (void)ctxId;
    if (connStatus) sig_span("sceNpSignalingGetConnectionStatus", "connStatus", (u32)(uintptr_t)connStatus, 4);
    if (peerAddr) sig_span("sceNpSignalingGetConnectionStatus", "peerAddr", (u32)(uintptr_t)peerAddr, 4);
    if (peerPort) sig_span("sceNpSignalingGetConnectionStatus", "peerPort", (u32)(uintptr_t)peerPort, 2);
    SignalingConn snap;
    const int found = conn_snapshot(connId, &snap);
    const SignalingConn* c = found ? &snap : NULL;
    if (connStatus) vm_write32((u32)(uintptr_t)connStatus, (u32)(c ? c->status : SCE_NP_SIGNALING_CONN_STATUS_INACTIVE));
    if (peerAddr) vm_write32((u32)(uintptr_t)peerAddr, c ? conn_addr(c) : 0u);
    if (peerPort) vm_write16((u32)(uintptr_t)peerPort, (u16)(c ? c->port : 0));
    return c ? CELL_OK : (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
}

s32 sceNpSignalingGetConnectionInfo(SceNpSignalingCtxId ctxId,
                                      SceNpSignalingConnId connId,
                                      s32 code, void* info)
{
    (void)ctxId;
    if (!info) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    SignalingConn snap;
    if (!conn_snapshot(connId, &snap)) return (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
    const SignalingConn* c = &snap;
    const u32 ea = (u32)(uintptr_t)info;
    /* The member of the info union the code names; an unknown code writes nothing. */
    u32 span = 0;
    switch (code) {
    case SCE_NP_SIGNALING_CONN_INFO_RTT:
    case SCE_NP_SIGNALING_CONN_INFO_BANDWIDTH:
    case SCE_NP_SIGNALING_CONN_INFO_PACKET_LOSS: span = 4; break;
    case SCE_NP_SIGNALING_CONN_INFO_PEER_NPID: span = 36; break;
    case SCE_NP_SIGNALING_CONN_INFO_PEER_ADDRESS:
    case SCE_NP_SIGNALING_CONN_INFO_MAPPED_ADDRESS: span = 6; break;
    default: return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    }
    sig_span("sceNpSignalingGetConnectionInfo", "info", ea, span);
    switch (code) {
    case SCE_NP_SIGNALING_CONN_INFO_RTT:         vm_write32(ea, 1000); break;        /* us; loopback/LAN */
    case SCE_NP_SIGNALING_CONN_INFO_BANDWIDTH:   vm_write32(ea, 10000000); break;    /* bits/s */
    case SCE_NP_SIGNALING_CONN_INFO_PACKET_LOSS: vm_write32(ea, 0); break;
    case SCE_NP_SIGNALING_CONN_INFO_PEER_NPID:   /* SceNpId: SceNpOnlineId { char data[16]; term; dummy[3] }, opt[8], reserved[8] */
        for (u32 o = 0; o < 36; o++) vm_write8(ea + o, 0);
        for (u32 o = 0; o < 16 && c->online_id[o]; o++) vm_write8(ea + o, (u8)c->online_id[o]);
        break;
    case SCE_NP_SIGNALING_CONN_INFO_PEER_ADDRESS:
    case SCE_NP_SIGNALING_CONN_INFO_MAPPED_ADDRESS:
        vm_write32(ea, conn_addr(c));
        vm_write16(ea + 4, c->port);
        break;
    default: return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    }
    return CELL_OK;
}

s32 sceNpSignalingGetConnectionFromNpId(SceNpSignalingCtxId ctxId, const void* npId,
                                          SceNpSignalingConnId* connId)
{
    (void)ctxId;
    if (!npId || !connId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    sig_span("sceNpSignalingGetConnectionFromNpId", "npId", (u32)(uintptr_t)npId, 16);
    sig_span("sceNpSignalingGetConnectionFromNpId", "connId", (u32)(uintptr_t)connId, 4);
    char id[17] = {0};
    for (u32 o = 0; o < 16; o++) id[o] = (char)vm_read8((u32)(uintptr_t)npId + o);
    int found = -1;
    sig_lock();
    for (int i = 0; i < SCE_NP_SIGNALING_CONN_MAX; i++)
        if (s_conn[i].used && strcmp(s_conn[i].online_id, id) == 0) { found = i; break; }
    sig_unlock();
    if (found < 0) return (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
    vm_write32((u32)(uintptr_t)connId, (u32)(found + 1));
    return CELL_OK;
}

s32 sceNpSignalingGetLocalNetInfo(u32* localAddr, u16* localPort)
{
    if (localAddr) sig_span("sceNpSignalingGetLocalNetInfo", "localAddr", (u32)(uintptr_t)localAddr, 4);
    if (localPort) sig_span("sceNpSignalingGetLocalNetInfo", "localPort", (u32)(uintptr_t)localPort, 2);
    if (localAddr) vm_write32((u32)(uintptr_t)localAddr, (u32)0x7F000001); /* 127.0.0.1 */
    if (localPort) vm_write16((u32)(uintptr_t)localPort, (u16)0);
    return CELL_OK;
}

s32 sceNpSignalingGetPeerNetInfo(SceNpSignalingCtxId ctxId,
                                   SceNpSignalingConnId connId,
                                   u32* peerAddr, u16* peerPort)
{
    (void)ctxId;
    if (peerAddr) sig_span("sceNpSignalingGetPeerNetInfo", "peerAddr", (u32)(uintptr_t)peerAddr, 4);
    if (peerPort) sig_span("sceNpSignalingGetPeerNetInfo", "peerPort", (u32)(uintptr_t)peerPort, 2);
    SignalingConn snap;
    const int found = conn_snapshot(connId, &snap);
    const SignalingConn* c = found ? &snap : NULL;
    if (peerAddr) vm_write32((u32)(uintptr_t)peerAddr, c ? conn_addr(c) : 0u);
    if (peerPort) vm_write16((u32)(uintptr_t)peerPort, (u16)(c ? c->port : 0));
    return c ? CELL_OK : (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
}

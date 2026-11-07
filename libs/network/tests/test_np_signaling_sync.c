/*
 * test_np_signaling_sync - sceNpSignaling: DeactivateConnection, and the shared tables under threads (#130, #131).
 *
 *   cc -std=gnu17 -I include -o test_np_signaling_sync libs/network/tests/test_np_signaling_sync.c -pthread
 *
 * Compiles sceNpSignaling.c into this file with sysutil stubbed, as test_np_signaling_rooms.c does.
 *
 *   deactivate  sceNpSignalingDeactivateConnection is NOT_INITIALIZED before sceNpInit; a known connection goes INACTIVE
 *               and loses its activation with no event queued; one connection only; an unknown id is CELL_OK and changes
 *               nothing; a later ActivateConnection restarts from what Matching2 last said of the peer (ACTIVE and
 *               mutual again while the member is up, PENDING once it left). Evidence: RPCS3 at the pinned oracle, see
 *               the comment on the handler.
 *   threads     Matching2's thread flips room members up and down (np_signaling_room_peer) while the title's thread
 *               creates contexts and activates, deactivates and queries connections: s_ctx, s_conn and the ready flag
 *               are shared state (#131). Run under ThreadSanitizer (ctest --preset tsan -R np_signaling_sync): an
 *               unlocked table is a data race there on its first overlap, whatever the interleaving; the final state
 *               check catches a torn entry without it.
 */
#include "../sceNpSignaling.c"

#include <assert.h>
#include <pthread.h>
#include "../../../tests/guest_range_catch.h"

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_linelock_lock(uint32_t a) { (void)a; }
void spu_linelock_release(uint32_t a) { (void)a; }

static int g_np_init;   /* what sceNpInit has done; set before any thread starts */
int np_is_initialized(void) { return g_np_init; }

/* What would be queued for the title's handlers; two threads queue, so the stub has its own lock. */
#define QMAX 64
static pthread_mutex_t s_q_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { u32 opd; u64 a[8]; } s_q[QMAX];
static int s_nq;
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8])
{
    pthread_mutex_lock(&s_q_mu);
    if (s_nq < QMAX) {
        s_q[s_nq].opd = opd;
        memcpy(s_q[s_nq].a, args, sizeof(s_q[s_nq].a));
    }
    s_nq++;
    pthread_mutex_unlock(&s_q_mu);
    return CELL_OK;
}

static u32 be32_at(u32 ea)
{
    const uint8_t* p = vm_base + ea;
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
static u32 be16_at(u32 ea) { return (u32)vm_base[ea] << 8 | vm_base[ea + 1]; }

#define EA(x) ((void*)(uintptr_t)(x))

/* The queued call k: handler opd, (ctx, conn, event, error, arg). */
static void expect(int k, u32 opd, u32 ctx, u32 conn, u32 event, u32 arg)
{
    assert(k < s_nq && k < QMAX);
    assert(s_q[k].opd == opd);
    assert(s_q[k].a[0] == ctx && s_q[k].a[1] == conn && s_q[k].a[2] == event && s_q[k].a[3] == 0 && s_q[k].a[4] == arg);
}

static uint8_t mem[0x10000];
#define H 0x8000u
#define HARG 0x8100u
#define X 0x8200u
#define XARG 0x8300u
#define CTX_EA 0x100u
#define CONN_EA 0x104u
#define ST_EA 0x108u
#define ADDR_EA 0x10Cu
#define PORT_EA 0x110u
#define NPID_EA 0x300u

static void set_npid(const char* name)
{
    memset(mem + NPID_EA, 0, 36);
    memcpy(mem + NPID_EA, name, strlen(name));
}

static u32 status_of(u32 ctx, u32 conn)
{
    assert(sceNpSignalingGetConnectionStatus(ctx, conn, (s32*)EA(ST_EA), NULL, NULL) == CELL_OK);
    return be32_at(ST_EA);
}

static u32 activate(u32 ctx, const char* name)
{
    set_npid(name);
    assert(sceNpSignalingActivateConnection(ctx, EA(NPID_EA), (SceNpSignalingConnId*)EA(CONN_EA)) == CELL_OK);
    return be32_at(CONN_EA);
}

static int mode_deactivate(void)
{
    /* Before sceNpInit (and before sceNpSignalingInit) there is nothing to deactivate. */
    g_np_init = 0;
    assert(sceNpSignalingDeactivateConnection(1, 1) == (s32)SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    g_np_init = 1;

    assert(sceNpSignalingCreateCtx(NULL, (SceNpSignalingHandler)EA(H), EA(HARG), (SceNpSignalingCtxId*)EA(CTX_EA)) == CELL_OK);
    const u32 ctx = be32_at(CTX_EA);
    assert(sceNpSignalingAddExtendedHandler(ctx, (SceNpSignalingHandler)EA(X), EA(XARG)) == CELL_OK);

    /* A mutual connection: activated, then the member is up. */
    const u8 ip2[4] = { 192, 168, 1, 20 }, ip3[4] = { 192, 168, 1, 30 };
    const u32 c2 = activate(ctx, "peer-2");
    np_signaling_room_peer("peer-2", ip2, 3659, 1);
    np_signaling_room_peer("peer-3", ip3, 3660, 1);
    set_npid("peer-3");
    const u32 c3 = activate(ctx, "peer-3");
    assert(c2 != c3 && status_of(ctx, c2) == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE &&
           status_of(ctx, c3) == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE);
    const int quiet = s_nq;   /* peer-2: ESTABLISHED (2 handlers) and mutual (2); peer-3: ESTABLISHED, then both again */
    assert(quiet == 10);

    /* Deactivate peer-2: INACTIVE, no callback ("forceful": no transition, no event); peer-3 is untouched. */
    assert(sceNpSignalingDeactivateConnection(ctx, c2) == CELL_OK);
    assert(status_of(ctx, c2) == SCE_NP_SIGNALING_CONN_STATUS_INACTIVE);
    assert(status_of(ctx, c3) == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE);
    assert(s_nq == quiet);

    /* A connection id with no connection is accepted and changes nothing. */
    assert(sceNpSignalingDeactivateConnection(ctx, 15) == CELL_OK);
    assert(sceNpSignalingDeactivateConnection(ctx, 0) == CELL_OK);
    assert(status_of(ctx, c3) == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE && s_nq == quiet);

    /* Activate again while the member is still up: the same connection, ACTIVE at once, and mutual again (the
     * deactivation took the activation away): ESTABLISHED to both handlers, then the two extended events. */
    assert(activate(ctx, "peer-2") == c2);
    assert(status_of(ctx, c2) == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE);
    assert(s_nq == quiet + 4);
    expect(quiet + 0, H, ctx, c2, SCE_NP_SIGNALING_EVENT_ESTABLISHED, HARG);
    expect(quiet + 1, X, ctx, c2, SCE_NP_SIGNALING_EVENT_ESTABLISHED, XARG);
    expect(quiet + 2, X, ctx, c2, SCE_NP_SIGNALING_EVENT_EXT_PEER_ACTIVATED, XARG);
    expect(quiet + 3, X, ctx, c2, SCE_NP_SIGNALING_EVENT_EXT_MUTUAL_ACTIVATED, XARG);

    /* The member leaves: DEAD. Deactivated or not, a connection to a peer that is gone restarts PENDING. */
    np_signaling_room_peer("peer-2", ip2, 3659, 0);
    assert(s_nq == quiet + 6);
    expect(quiet + 4, H, ctx, c2, SCE_NP_SIGNALING_EVENT_DEAD, HARG);
    expect(quiet + 5, X, ctx, c2, SCE_NP_SIGNALING_EVENT_DEAD, XARG);
    assert(activate(ctx, "peer-2") == c2);
    assert(status_of(ctx, c2) == SCE_NP_SIGNALING_CONN_STATUS_PENDING && s_nq == quiet + 6);
    assert(sceNpSignalingDeactivateConnection(ctx, c2) == CELL_OK);   /* a pending one: back to INACTIVE */
    assert(status_of(ctx, c2) == SCE_NP_SIGNALING_CONN_STATUS_INACTIVE && s_nq == quiet + 6);
    assert(activate(ctx, "peer-2") == c2 && status_of(ctx, c2) == SCE_NP_SIGNALING_CONN_STATUS_PENDING);
    /* ... and the member comes back up while it is pending: ESTABLISHED and mutual, as for any pending activation. */
    np_signaling_room_peer("peer-2", ip2, 3659, 1);
    assert(status_of(ctx, c2) == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE && s_nq == quiet + 6 + 4);

    assert(sceNpSignalingDestroyCtx(ctx) == CELL_OK);
    printf("test_np_signaling_sync deactivate: ok (%d events)\n", s_nq);
    return 0;
}

/* ---- threads -------------------------------------------------------------------------------------------------- */

#define PEERS 6
#define ITER 300
static atomic_int g_go;
static const char* const k_peer[PEERS] = { "peer-a", "peer-b", "peer-c", "peer-d", "peer-e", "peer-f" };

static void* matching2_thread(void* arg)
{
    (void)arg;
    while (!atomic_load_explicit(&g_go, memory_order_acquire)) { }
    for (int it = 0; it < ITER; it++)
        for (int p = 0; p < PEERS; p++) {
            const u8 ip[4] = { 10, 0, (u8)p, 7 };
            np_signaling_room_peer(k_peer[p], ip, (u16)(3659 + p), (it + p) & 1);
        }
    for (int p = 0; p < PEERS; p++) {   /* the last word is "up" for everyone, at an address that names the peer */
        const u8 ip[4] = { 10, 0, (u8)p, 7 };
        np_signaling_room_peer(k_peer[p], ip, (u16)(3659 + p), 1);
    }
    return NULL;
}

static int mode_threads(void)
{
    g_np_init = 1;
    assert(sceNpSignalingCreateCtx(NULL, (SceNpSignalingHandler)EA(H), EA(HARG), (SceNpSignalingCtxId*)EA(CTX_EA)) == CELL_OK);
    const u32 ctx = be32_at(CTX_EA);
    assert(sceNpSignalingAddExtendedHandler(ctx, (SceNpSignalingHandler)EA(X), EA(XARG)) == CELL_OK);

    pthread_t th;
    assert(pthread_create(&th, NULL, matching2_thread, NULL) == 0);
    atomic_store_explicit(&g_go, 1, memory_order_release);
    for (int it = 0; it < ITER; it++)
        for (int p = 0; p < PEERS; p++) {
            const u32 conn = activate(ctx, k_peer[p]);
            u32 tmp_ctx_ea = 0x120;
            assert(sceNpSignalingCreateCtx(NULL, (SceNpSignalingHandler)EA(H), EA(HARG), (SceNpSignalingCtxId*)EA(tmp_ctx_ea)) == CELL_OK);
            assert(sceNpSignalingAddExtendedHandler(be32_at(tmp_ctx_ea), (SceNpSignalingHandler)EA(X), EA(XARG)) == CELL_OK);
            assert(sceNpSignalingGetConnectionStatus(ctx, conn, (s32*)EA(ST_EA), (u32*)EA(ADDR_EA), (u16*)EA(PORT_EA)) == CELL_OK);
            assert(sceNpSignalingGetConnectionInfo(ctx, conn, SCE_NP_SIGNALING_CONN_INFO_PEER_ADDRESS, EA(0x200)) == CELL_OK);
            set_npid(k_peer[p]);
            assert(sceNpSignalingGetConnectionFromNpId(ctx, EA(NPID_EA), (SceNpSignalingConnId*)EA(CONN_EA)) == CELL_OK);
            assert(be32_at(CONN_EA) == conn);
            if ((it + p) % 3 == 0) assert(sceNpSignalingDeactivateConnection(ctx, conn) == CELL_OK);
            assert(sceNpSignalingDestroyCtx(be32_at(tmp_ctx_ea)) == CELL_OK);
        }
    assert(pthread_join(th, NULL) == 0);

    /* Matching2's last word was "up" for every peer with its own address: after one more activation each connection
     * is ACTIVE at that address, whatever the title deactivated and in whatever order. */
    for (int p = 0; p < PEERS; p++) {
        const u32 conn = activate(ctx, k_peer[p]);
        assert(sceNpSignalingGetConnectionStatus(ctx, conn, (s32*)EA(ST_EA), (u32*)EA(ADDR_EA), (u16*)EA(PORT_EA)) == CELL_OK);
        assert(be32_at(ST_EA) == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE);
        assert(be32_at(ADDR_EA) == (0x0A000007u | ((u32)p << 8)) && be16_at(PORT_EA) == 3659 + p);
    }
    assert(sceNpSignalingDestroyCtx(ctx) == CELL_OK);
    printf("test_np_signaling_sync threads: ok (%d events)\n", s_nq);
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);   /* the handlers print a line a call */
    vm_base = mem;
    ppu_vm_size = (u32)sizeof mem;
    const char* mode = argc > 1 ? argv[1] : "deactivate";
    int rc = 2;
    if (strcmp(mode, "deactivate") == 0) rc = mode_deactivate();
    else if (strcmp(mode, "threads") == 0) rc = mode_threads();
    else printf("unknown mode %s\n", mode);
    fflush(stdout);
    return rc;
}

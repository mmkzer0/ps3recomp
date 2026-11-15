/*
 * ps3recomp - SPU effect port
 *
 * The one boundary between an SPU context and everything outside it. Design
 * and evidence: docs/SPU_XDIFF.md ("The effect port").
 *
 * SPU code reaches the world only through channel instructions: guest memory
 * through MFC commands, other threads through mailboxes, signals and events,
 * time through the decrementer. Interpreted and lifted code issue all of them
 * as spu_wrch / spu_rdch / spu_rchcnt (spu_channels.c), so a port installed on
 * a context sits in those three functions and serves every tier that runs SPU
 * code. A body that is not SPU code (a native reimplementation) calls the same
 * three functions to reach the world, and is mediated the same way.
 *
 * A port has two halves, and either may be absent:
 *
 *   serve  REPLACES the runtime's channel bodies. The context then touches no
 *          guest memory, reservation, mailbox, lock or clock: each value it
 *          reads comes from the serve table and each value it writes ends
 *          there.
 *   tap    is TOLD each effect and cannot change it: its callbacks return
 *          nothing and take a const context. The body that runs is the one
 *          the context would have run without the tap.
 *
 *     backend     serve          tap         the context's world
 *     real        -              -           the runtime (no port installed)
 *     record      -              recorder    the runtime, written down
 *     replay      transcript     -           one recorded run
 *     synthetic   seeded model   recorder    a model; its run becomes a fixture
 *
 * The split is the enforcement of two rules that would otherwise be remembered:
 * a recorder cannot alter the authoritative run (it has no way to return a
 * value), and a replaying context cannot produce an effect (nothing below the
 * seam runs for it, neither the MFC engine's stops nor its hooks).
 *
 * What is not an effect: the five MFC parameter latches (spu_port_is_latch).
 * They only store a value in the context until an MFC_Cmd write reads it, so
 * they stay in spu_wrch for every backend and no port sees them. Their values
 * reach a port as the request of the command that consumes them, and no
 * channel reads them back. MFC_WrTagMask is not one of them: the mask decides
 * what MFC_RdTagStat returns and MFC_RdTagMask reads it, so a replay that did
 * not compare it would answer a wait on the wrong tags with the right value.
 *
 * Where the seam is (docs/SPU_XDIFF.md section 5.5; not in this header):
 *   spu_wrch    after the latch switch:  tap->wrch, then serve->wrch or the
 *               runtime body; tap->xfer zero or more times from inside it.
 *   spu_rdch    serve->rdch or the runtime body (which may block), then
 *               tap->rdch with the value the SPU receives.
 *   spu_rchcnt  as spu_rdch.
 * With no port each of the three pays one predictable branch on the context's
 * port pointer and nothing else.
 *
 * Threading: a context has one execution owner (docs/SPU_CONTEXT_REGISTRIES.md).
 * Every callback runs on that thread, inside the channel instruction, and
 * never concurrently for one context. `ctx->pc` is the channel instruction's
 * address under the interpreter and the last transfer target under lifted
 * code, which stores pc at transfers only.
 */
#ifndef SPU_EFFECT_PORT_H
#define SPU_EFFECT_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

/* The effect vocabulary of this header: which channels are latches, what a
 * transfer report carries and the order of tap callbacks. A fixture records
 * the value it was written under (spu_xdiff.h); bump it when a transcript
 * recorded before the change would be read differently after it. */
#define SPU_EFFECT_PORT_ABI 1u

/* ---- effects ------------------------------------------------------------- */

/* Nonzero for a channel whose write is context state, not an effect: MFC_LSA
 * (16), MFC_EAH (17), MFC_EAL (18), MFC_Size (19) and MFC_TagID (20). The
 * numbers are the CBEA channel assignments spu_context.h names; this header
 * is included where that one is not. */
static inline int spu_port_is_latch(uint32_t channel)
{
    return channel >= 16u && channel <= 20u;
}

/* Which way a transfer moved bytes. */
typedef enum spu_port_xfer_dir {
    SPU_XFER_TO_LS   = 1, /* GET family, GETLLAR: `size` bytes arrived at `lsa` */
    SPU_XFER_FROM_LS = 2, /* PUT family, PUTLLC, PUTLLUC, PUTQLLUC: the bytes at `lsa` were offered */
    SPU_XFER_ZERO_EA = 3  /* SDCRZ: `size` bytes at `ea` zeroed; no local-store operand */
} spu_port_xfer_dir;

/* One completed movement between local store and the world, reported from
 * inside the channel write that caused it: one per plain command, one per list
 * element that moved bytes (also when a stalled list resumes inside
 * MFC_WrListStallAck), one per atomic command.
 *
 * The bytes are not passed: they are in the context's local store at `lsa`
 * when the report is made (after a TO_LS transfer landed, while a FROM_LS
 * source is still intact), which is the only place a recorder may take them
 * from. A FROM_LS report describes what the SPU offered; whether a PUTLLC
 * stored it is `status`. */
typedef struct spu_port_xfer {
    uint64_t ea;     /* effective address of the first byte; a list element's own EA */
    uint32_t lsa;    /* local-store address of the first byte, masked; 0 for ZERO_EA */
    uint32_t size;   /* bytes moved; 128 for an atomic line */
    uint32_t cmd;    /* the MFC opcode that moved them, list bit cleared (0x40 GET, 0x20 PUT, 0xD0 GETLLAR, ...) */
    uint32_t tag;    /* tag group, 0..31 */
    uint32_t dir;    /* spu_port_xfer_dir */
    uint32_t status; /* atomic commands: what MFC_RdAtomicStat now returns (4 GETLLAR, 0 PUTLLC stored,
                      * 1 PUTLLC lost its reservation, 2 PUTLLUC/PUTQLLUC); 0 otherwise */
} spu_port_xfer;

/* ---- serve --------------------------------------------------------------- */

/* The world of a context that must not reach the real one. All three members
 * are required: a missing one could only fall through to the runtime, which is
 * the leak this table exists to prevent (spu_port_set aborts on it).
 *
 * A serve callback never blocks, takes no runtime lock and raises no named
 * stop for what the guest did. When it cannot answer (its transcript is
 * exhausted, or the request is not the one it holds) it keeps the reason in
 * its own state and ends the run with spu_halt(ctx); it does not invent a
 * value. A context with a serve table therefore runs under spu_run_with_halt,
 * as every job body already does. */
typedef struct spu_effect_serve {
    /* A channel write that is not a latch. For MFC_Cmd (21) the request is
     * `value` and the context's latches (mfc_lsa, mfc_eah, mfc_eal, mfc_size,
     * mfc_tag); bytes move between the serve table and ctx->ls here. */
    void (*wrch)(void* user, struct spu_context* ctx, uint32_t channel, uint32_t value);
    /* A channel read: the value the SPU receives in the preferred slot. */
    uint32_t (*rdch)(void* user, struct spu_context* ctx, uint32_t channel);
    /* A channel count. */
    uint32_t (*rchcnt)(void* user, struct spu_context* ctx, uint32_t channel);
} spu_effect_serve;

/* ---- tap ----------------------------------------------------------------- */

/* Told every effect of a context, in program order. A NULL member is skipped.
 *
 * Order within one channel write: `wrch` first, with the value about to take
 * effect, then one `xfer` per movement it causes; nothing closes the group, the
 * next callback does. Reads report after the fact, because only then is the
 * value known. So the callbacks of a run, in the order they arrive, are the
 * run's transcript. */
typedef struct spu_effect_tap {
    void (*wrch)(void* user, const struct spu_context* ctx, uint32_t channel, uint32_t value);
    void (*rdch)(void* user, const struct spu_context* ctx, uint32_t channel, uint32_t value);
    void (*rchcnt)(void* user, const struct spu_context* ctx, uint32_t channel, uint32_t count);
    void (*xfer)(void* user, const struct spu_context* ctx, const spu_port_xfer* x);
} spu_effect_tap;

/* ---- port ---------------------------------------------------------------- */

typedef struct spu_effect_port {
    const spu_effect_serve* serve; /* NULL: the runtime's own channel bodies */
    void*                   serve_user;
    const spu_effect_tap*   tap;   /* NULL: nothing is told */
    void*                   tap_user;
} spu_effect_port;

/* Install or clear (NULL) the port of a context, between runs only. `port` and
 * what it points at must outlive every run of the context. Like the observer
 * (spu_observe.h) a port is per context and is neither copied nor inherited;
 * spu_context_init leaves none. A serve table with a NULL member is a
 * programming error and aborts, since continuing would run the context
 * against the real world. */
void spu_port_set(struct spu_context* ctx, const spu_effect_port* port);

/* The installed port, or NULL. */
const spu_effect_port* spu_port_get(const struct spu_context* ctx);

/* For a serve table that moves bytes itself: report one transfer to the
 * context's tap, exactly as the runtime's MFC engine does for its own. A
 * no-op without a tap. */
void spu_port_report_xfer(struct spu_context* ctx, const spu_port_xfer* x);

/* End the run of a served context that cannot be answered: the serve callback
 * keeps `reason` in its own state, then calls this instead of returning a value
 * it does not hold. It leaves through spu_halt, which longjmps to the
 * spu_run_with_halt landing pad. spu_halt returns when no pad is armed, and a
 * context that went on from there would run against a world nobody recorded,
 * so the process aborts instead, after a diagnostic naming the context and the
 * reason. Never returns. */
#if defined(_MSC_VER)
#  define SPU_PORT_NORETURN __declspec(noreturn)
#else
#  define SPU_PORT_NORETURN __attribute__((noreturn))
#endif
SPU_PORT_NORETURN void spu_port_serve_halt(struct spu_context* ctx, const char* reason);

/* The hint for the one branch the real path pays per channel operation. */
#if defined(__GNUC__) || defined(__clang__)
#  define SPU_PORT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define SPU_PORT_UNLIKELY(x) (x)
#endif

/* The real path's channel bodies are inlined into the public entry points, so a
 * context with no port pays the port test and nothing else (no extra call).
 * Each body has that one call site (spu_channels.c, spu_wrch_real). */
#if defined(_MSC_VER)
#  define SPU_PORT_ALWAYS_INLINE static __forceinline
#else
#  define SPU_PORT_ALWAYS_INLINE static inline __attribute__((always_inline))
#endif

/* Kept out of the channel entry points: the port halves, which the real path
 * never runs, and the MFC write body (spu_channels.c, spu_wrch_mfc). */
#if defined(_MSC_VER)
#  define SPU_PORT_NOINLINE static __declspec(noinline)
#else
#  define SPU_PORT_NOINLINE static __attribute__((noinline))
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_EFFECT_PORT_H */

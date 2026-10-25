/*
 * ps3recomp - named stop
 *
 * The one way the runtime gives up. A gap the runtime cannot honour (a firmware
 * import with no handler, a syscall nobody implements, code the lifter did not
 * lift, a rejected configuration, a wrong executable) must not be papered over
 * with a made-up return value: the guest would carry on from state the real
 * console never produced, and the divergence would surface thousands of calls
 * later, nowhere near its cause. A stop ends the process at the cause and says
 * what the cause was.
 *
 * One call prints, on stderr, a banner a human cannot miss followed by ONE
 * machine-readable line, then flushes and exits with PS3_STOP_EXIT_CODE. It
 * never returns and never runs atexit handlers: the state it is reporting is
 * not trustworthy enough for them.
 *
 * The machine line is the stable interface (tools, CI and tests match on it;
 * the banner is prose and may change):
 *
 *   PS3STOP v=1 kind=<kind> <key>=<value> ... [pc=0x.. lr=0x.. tid=N]
 *           [last_hle_nid=0x.. last_hle=<name>] [syscall=N]
 *
 * It is a single line (wrapped above only for the comment). A value that is not
 * made solely of [A-Za-z0-9_.:/+@,-] is double-quoted with \" \\ \n \r \t and
 * \xNN escapes, so the line splits on spaces the way a shell would.
 *
 * Kinds are stable kebab-case ids. A new gap gets a new kind here rather than
 * a free-text variant of an old one, because the id is what a title's tooling
 * counts and allow-lists by.
 *
 * Threading: any thread may stop at any time. Stops are serialised, so
 * concurrent stops produce exactly one banner and one machine line (the
 * first), never interleaved text; every other thread parks until the process
 * exits. A stop raised from inside a stop (a frontier provider that itself
 * stops) exits at once with the bare machine line.
 */

#ifndef PS3EMU_STOP_H
#define PS3EMU_STOP_H

#include <stddef.h>
#include <stdint.h>

/* Marks a result that reports an error and must not be discarded: dropping one
 * is a silent loss (an event never delivered, a thread never started and then
 * joined) and the runtime's rule is to fail closed or stop, never carry on.
 * This header is the fail-closed one every syscall and HLE header already
 * reaches, so the marker lives beside the stop it prevents. C compilers that
 * are not MSVC use warn_unused_result; C++ uses the standard attribute. */
#if defined(_MSC_VER) && !defined(__clang__)
#  include <sal.h>
#  define PS3_NODISCARD _Check_return_
#elif defined(__cplusplus)
#  define PS3_NODISCARD [[nodiscard]]
#else
#  define PS3_NODISCARD __attribute__((warn_unused_result))
#endif

#ifdef __cplusplus
#  define PS3_NORETURN [[noreturn]]
extern "C" {
#else
#  define PS3_NORETURN _Noreturn
#endif

/* Process exit status of every stop. 70 is EX_SOFTWARE (sysexits.h): distinct
 * from success, from the 1/2 a failed test or a usage error returns, from the
 * shell's 126/127 and from the 128+signal range, so a wrapper can tell "the
 * runtime refused to continue" from every other way a run ends. */
#define PS3_STOP_EXIT_CODE 70

/* kind: enumerator suffix, stable id. */
#define PS3_STOP_KINDS(X)                                                      \
    /* A1a: raised by the runtime's own startup gates. */                      \
    X(IDENTITY_MISMATCH,        "identity-mismatch")                           \
    X(CONFIG_INVALID,           "config-invalid")                              \
    /* Declared for the guest-execution gaps; raised by their own slice. */    \
    X(UNRESOLVED_NID,           "unresolved-nid")                              \
    X(UNIMPLEMENTED_SYSCALL,    "unimplemented-syscall")                       \
    X(UNLIFTED_FUNCTION,        "unlifted-function")                           \
    X(UNRESOLVED_INDIRECT_CALL, "unresolved-indirect-call")                    \
    X(UNKNOWN_INSTRUCTION,      "unknown-instruction")                         \
    X(DISPATCH_DEPTH_EXCEEDED,  "dispatch-depth-exceeded")                     \
    X(SPU_DMA_REPEAT,           "spu-dma-repeat")                              \
    X(UNRESOLVED_GUEST_CALLBACK, "unresolved-guest-callback")                  \
    /* A6: an HLE handler returns a host pointer; stop, never truncate it. */ \
    X(HLE_UNSUPPORTED_RETURN,   "hle-unsupported-return")                      \
    X(SPU_JOB_CRT_VERSION,      "spu-job-crt-version")                         \
    X(SPU_JOB_MEMORY,           "spu-job-memory")                              \
    X(SPU_JOB_JM_ENTRY,         "spu-job-jm-entry")                            \
    X(SPU_JOB_DMA_LIST,         "spu-job-dma-list")                            \
    X(JQ2_POOL_RELEASE,         "jq2-pool-release")                            \
    /* B4: the JobQueue2 HLE executor. */                                      \
    X(JQ2_JOB_END,              "jq2-job-end")                                 \
    X(JQ2_RING_ENTRY,           "jq2-ring-entry")                              \
    X(JQ2_WAKE,                 "jq2-wake")                                    \
    X(JQ2_STATE,                "jq2-state")                                   \
    X(JQ2_JM_CALL,              "jq2-jm-call")                                 \
    X(HLE_GUEST_RANGE,          "hle-guest-range")                             \
    /* C1: an HLE call before its module's init, firmware behaviour undefined. */ \
    X(HLE_UNINITIALIZED,        "hle-uninitialized")                           \
    X(SYNC2_SEMAPHORE_UNSUPPORTED, "sync2-semaphore-unsupported")            \
    X(FIBER_UNSUPPORTED,        "fiber-unsupported")                           \
    /* C2: a GCM/RESC path the firmware has and the runtime does not model. */ \
    X(GCM_UNMODELLED,           "gcm-unmodelled")                              \
    /* C2: an RSX wait (a semaphore acquire, the command-context callback) that no one ended. */ \
    X(GCM_WAIT_TIMEOUT,         "gcm-wait-timeout")                            \
    /* C3: a guest load or store into the unmapped null page (ppu.null_page). */ \
    X(GUEST_NULL_ACCESS,        "guest-null-access")                           \
    X(STACK_GUARD_FAULT,        "stack-guard-fault")                           \
    /* An lv2-internal producer sent to a queue id that never existed: our  */ \
    /* bookkeeping is wrong (full or destroyed queues drop, as lv2 does). */  \
    X(EVENT_DELIVERY_LOST,      "event-delivery-lost")                         \
    /* A JobQueue2 parameter combination the firmware accepts whose console behaviour is unknown. */ \
    X(JQ2_UNMODELLED,           "jq2-unmodelled")                              \
    /* An MFC transfer the hardware rejects (a command/alignment error). */   \
    X(SPU_MFC_ERROR,            "spu-mfc-error")                               \
    /* An in-range guest span the host could not make accessible (vm_commit). */ \
    X(HLE_COMMIT_FAILED,        "hle-commit-failed")

/* A fixed underlying type in C++ makes a kind outside the list (a caller's bug,
 * which the stop still reports as "invalid-kind") a well-defined value rather
 * than undefined behaviour; C enums are int-sized already, so the ABI matches. */
#ifdef __cplusplus
typedef enum ps3_stop_kind : int {
#else
typedef enum ps3_stop_kind {
#endif
#define PS3_STOP_KIND_ENUM(name, id) PS3_STOP_##name,
    PS3_STOP_KINDS(PS3_STOP_KIND_ENUM)
#undef PS3_STOP_KIND_ENUM
    PS3_STOP_KIND_COUNT
} ps3_stop_kind;

/* The kebab-case id ("identity-mismatch"), never NULL. */
const char* ps3_stop_kind_id(ps3_stop_kind kind);

/* One subject fact. `key` is [a-z][a-z0-9_]* (anything else is rewritten to
 * '_'); a key that collides with a reserved one of the machine line (v, kind,
 * pc, lr, tid, last_hle_nid, last_hle, syscall) is emitted as f_<key>. `value`
 * is already formatted by the caller: hex ids as "0x%08X", names verbatim. */
typedef struct ps3_stop_field {
    const char* key;
    const char* value;
} ps3_stop_field;

/* Where the guest was, when the gap is a guest-execution one. pc is the guest
 * address of the offending instruction or call target as the caller knows it
 * (a ppu_context's `cia` is only refreshed at block boundaries); lr and
 * thread_id come from the ppu_context. */
typedef struct ps3_stop_site {
    uint64_t pc;
    uint64_t lr;
    uint64_t thread_id;
} ps3_stop_site;

/* End the process. `what`: one sentence saying what was found. `remedy`: what
 * the operator can do about it; mandatory, a stop that cannot say is a bug.
 * `fields`/`nfields`: the subject, in order. `site`: NULL when the stop has no
 * guest context (startup gates).
 *
 * Example, for an import with no handler:
 *
 *     char nid[16]; snprintf(nid, sizeof nid, "0x%08X", n);
 *     ps3_stop_field f[] = { {"nid", nid}, {"name", name} };
 *     ps3_stop_site s = { pc, ctx->lr, ctx->thread_id };
 *     ps3_stop(PS3_STOP_UNRESOLVED_NID, "import has no HLE handler",
 *              "implement it, or allow-list it in [[hle.stub]]",
 *              f, 2, &s);
 */
PS3_NORETURN void ps3_stop(ps3_stop_kind kind, const char* what,
                           const char* remedy,
                           const ps3_stop_field* fields, size_t nfields,
                           const ps3_stop_site* site);

/* Generic helper to raise PS3_STOP_HLE_GUEST_RANGE.
 * Fields: func, arg, ea, size, limit. */
PS3_NORETURN void ps3_stop_guest_range(const char* func, const char* arg,
                                       uint64_t ea, uint64_t size, uint64_t limit);

/* Generic helper to raise PS3_STOP_HLE_COMMIT_FAILED: a guest span that is
 * inside guest memory (vm_range_ok) but that the host could not make readable
 * and writable for a host access (vm_commit: mprotect/VirtualAlloc failed), so
 * a handler that hands it to the kernel cannot proceed. A host fault, never an
 * error code the firmware does not return. Fields: func, arg, ea, size, rc. */
PS3_NORETURN void ps3_stop_commit_failed(const char* func, const char* arg,
                                         uint64_t ea, uint64_t size, int32_t rc);

/* Generic helper to raise PS3_STOP_HLE_UNINITIALIZED: an HLE call made before
 * its module's init, where the firmware's behaviour is undefined (it reads or
 * writes state its init has not built), so no return value is faithful.
 * Fields: func, module, reason. */
PS3_NORETURN void ps3_stop_hle_uninitialized(const char* func, const char* module,
                                             const char* reason);

/* Generic helper to raise PS3_STOP_GCM_UNMODELLED: the firmware has a command-stream
 * path (or a command-context state) the runtime does not reproduce, so any stream
 * it emitted would be a guess. Fields: func, feature, reason. */
PS3_NORETURN void ps3_stop_gcm_unmodelled(const char* func, const char* feature,
                                          const char* reason);

/* Generic helper to raise PS3_STOP_GCM_WAIT_TIMEOUT: the RSX (or the guest at a command-context
 * segment switch) waited for something no agent produced within gcm.wait_timeout_ms. The real
 * hardware waits forever; a bounded wait that expires means a producer is missing. Fields: func,
 * what, offset, value, current, waited_ms. */
PS3_NORETURN void ps3_stop_gcm_wait_timeout(const char* func, const char* what, uint32_t offset,
                                            uint32_t value, uint32_t current, uint32_t waited_ms);

/* ---- frontier ---------------------------------------------------------- */

/* The last thing the guest was seen doing, so a stop says how far the run got.
 * The runtime already tracks this where it dispatches (the last HLE import,
 * the lv2 syscall a thread is inside); a stop does not trace anything itself,
 * it asks whoever owns that state. */
typedef struct ps3_stop_frontier {
    int         has_hle;       /* last_hle_nid / last_hle are valid */
    uint32_t    hle_nid;
    const char* hle_name;      /* may be NULL or "" when unnamed */
    int         has_syscall;   /* syscall_num is valid */
    uint32_t    syscall_num;
} ps3_stop_frontier;

/* `thread_id` is the stopping thread's guest id when `site` was given, else 0.
 * A provider fills the parts it owns and leaves the rest alone, must not
 * block, allocate or stop, and runs on the stopping thread. */
typedef void (*ps3_stop_frontier_fn)(uint64_t thread_id, ps3_stop_frontier* out);

/* Register a provider (idempotent per function, at most 8). Safe to call from
 * a static initialiser: the registry needs no prior setup. */
void ps3_stop_register_frontier(ps3_stop_frontier_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_STOP_H */

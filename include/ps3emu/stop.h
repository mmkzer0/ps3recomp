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
    X(LIFTER_ABI,               "lifter-abi")                                  \
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
    /* An SPU PUT/PUTL, or an atomic PUTLLC/PUTLLUC/PUTQLLUC line, into the store window a device model registered (the RSX label window). */ \
    X(SPU_LABEL_WINDOW,         "spu-label-window")                            \
    /* An in-range guest span the host could not make accessible (vm_commit). */ \
    X(HLE_COMMIT_FAILED,        "hle-commit-failed")                           \
    /* A SPURS task the taskset policy module cannot load: the kernel halts the SPU. */ \
    X(SPURS_TASK_HALT,          "spurs-task-halt")                             \
    /* A SPURS task or job whose SPU program the runtime cannot run (no lifted entry). */ \
    X(SPURS_DISPATCH_MISS,      "spurs-dispatch-miss")                         \
    /* H1b: an RSX capture or replay that cannot be trusted (a blob, a span, an output). */ \
    X(RSX_CAPTURE,              "rsx-capture")                                 \
    /* A host path an HLE handler builds does not fit its buffer; a cut path names another directory. */ \
    X(HOST_PATH_LIMIT,          "host-path-limit")                                                         \
    /* S1: a FIFO method no class consumes and no allow-list entry cites. */                               \
    X(RSX_UNKNOWN_METHOD,       "rsx-unknown-method")

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

/* Generic helper to raise PS3_STOP_SPU_LABEL_WINDOW: an SPU MFC PUT/PUTL, or an atomic PUTLLC/PUTLLUC/PUTQLLUC
 * line (#51), whose span overlaps the guest
 * store window a device model registered (libs/video/cellGcmSys.c registers the RSX label window). The
 * runtime cannot know whether the PUT produces the value an RSX ACQUIRE is waiting for (it would have to
 * land before the walker reads the ACQUIRE) or overwrites a label the walker has not read yet (it would
 * have to land after a pass), and waiting for a pass can deadlock the first case, so the PUT is counted
 * and stopped instead of guessed. `site` is the SPU's local-store pc and its id as the guest thread;
 * fields: func, cmd, ea, size, tag, count (the per-process count including this PUT). */
PS3_NORETURN void ps3_stop_spu_label_window(const char* func, uint32_t cmd, uint64_t ea,
                                            uint32_t size, uint32_t tag, uint64_t count,
                                            const ps3_stop_site* site);

/* Generic helper to raise PS3_STOP_LIFTER_ABI: the lifted code was generated by a ppu_lifter.py whose
 * ABI this runtime does not implement (the loader is linked against the generated header, so the value
 * comes from the lift itself). Starting anyway would silently skip the hooks the runtime assumes (the
 * RSX label store hook, for one) -- wrong frames, not a missing feature. Fields: expected, got. */
PS3_NORETURN void ps3_stop_lifter_abi(uint32_t expected, uint32_t got);

/* Generic helper to raise PS3_STOP_SPURS_TASK_HALT: a SPURS task the SPU-side taskset policy module
 * cannot load (spursTasksetLoadElf fails: a null or misaligned ELF address, not an SPU ELF, a load
 * segment outside the task area), where the real kernel halts the SPU (RPCS3 cellSpursSpu.cpp
 * spursTasksetLoadElf -> spursHalt). The HLE has no SPU that halts and no firmware error to return
 * (cellSpursCreateTask validates only null and 16-byte alignment), so it stops rather than report a
 * started task that never runs. `site` is the calling guest thread's position when the caller is on
 * one, else NULL. Fields: api, taskset, task, elf, reason. */
PS3_NORETURN void ps3_stop_spurs_task_halt(const char* api, uint32_t taskset_ea, uint32_t task,
                                           uint32_t elf_ea, const char* reason,
                                           const ps3_stop_site* site);

/* Generic helper to raise PS3_STOP_SPURS_DISPATCH_MISS: a SPURS task or job whose image is a loadable
 * SPU program that the runtime cannot dispatch (no lifted entry is registered for its fingerprint, or the
 * host could not start the worker). The console runs the program; reporting the task started or the job
 * done would be success for work that never ran. `task` >= 0 names a task of `owner_ea`'s taskset
 * (fields api, taskset, task, image, size, fingerprint); `task` < 0 a job at `owner_ea` (fields api, job,
 * image, size, fingerprint). `image_ea` is the guest EA of the image and `size` its byte count the
 * fingerprint covers. `site` as for ps3_stop_spurs_task_halt. */
PS3_NORETURN void ps3_stop_spurs_dispatch_miss(const char* api, uint32_t owner_ea, int32_t task,
                                               uint32_t image_ea, uint32_t size, uint64_t fingerprint,
                                               const ps3_stop_site* site);

/* Generic helper to raise PS3_STOP_RSX_CAPTURE: the RSX capture or replay (ps3diff.rsx/1, libs/video/
 * rsx_capture.h) met something that would make its result untrustworthy, so it ends here instead of writing
 * or replaying a capture that only looks complete: a blob missing or altered, a capture that did not start
 * at a clean GCM reset, a consumed span two reads disagree on, an unsupported readback format, an output it
 * cannot write. Fields: func, reason (a stable id: midstream, blob-missing, blob-altered, read-conflict,
 * readback-format, output-exists, io, ...), detail (free text naming the file, address or event). */
PS3_NORETURN void ps3_stop_rsx_capture(const char* func, const char* reason, const char* detail);

/* Generic helper to raise PS3_STOP_RSX_UNKNOWN_METHOD: a FIFO method whose (class, method) pair no
 * consumer reads and no allow-list row classifies (rsx_method_class.h). The frame it belongs to cannot
 * be trusted, so the walker ends the process instead of consuming a command nobody explained: a method
 * silently dropped is state the console applied and this runtime did not. `cls` is the bound class's
 * name (rsx_class_name), `fifo` the IO offset of the packet header the method arrived in and `word` the
 * data word's offset. Fields: class, method, subchannel, fifo, word, data. */
PS3_NORETURN void ps3_stop_rsx_unknown_method(const char* cls, uint32_t method, uint32_t subchannel,
                                              uint32_t fifo, uint32_t word, uint32_t data);

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

/*
 * ps3recomp - cellSpurs HLE
 *
 * SPURS (SPU Runtime System): task management, workloads, event flags.
 * Maps SPU concepts onto host threads for HLE execution.
 */

#ifndef PS3RECOMP_CELL_SPURS_H
#define PS3RECOMP_CELL_SPURS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 *
 * The bases and every value are the firmware's (libsre 4.93) and RPCS3's
 * (Emu/Cell/Modules/cellSpurs.h at e53ac18f, CellSpurs*Error).
 *
 * Firmware evidence, libsre image 0x02000000 (sha256 3511ee39...):
 *   - cellSpursCreateTask (code 0x02012520) returns 0x80410911 for a null
 *     taskset (lis 0x8041 / ori 0x911 at 0x0201253C-0x02012548) and 0x80410910
 *     for a misaligned one (ori 0x910 at 0x02012594);
 *   - cellSpursTaskExitCodeInitialize (0x02013638) returns 0x8041090F on its
 *     state check (ori 0x90F at 0x02013760);
 *   - 0x804108xx policy returns are in libsre (0x801/0x802/0x810/0x811 at
 *     0x020085E8..0x020099D0) and the 0x80410A17 job code in libspurs_jq.
 * A code with no firmware or RPCS3 backing is not defined here.
 * -----------------------------------------------------------------------*/

/* The policy-module, task and job ranges above the core range. */
#define CELL_SPURS_ERROR_BASE_POLICY_MODULE     0x80410800
#define CELL_SPURS_ERROR_BASE_TASK              0x80410900
#define CELL_SPURS_ERROR_BASE_JOB               0x80410A00

/* Core return codes (RPCS3 CellSpursCoreError; firmware literals 0x0701,
 * 0x0702, 0x0705, 0x0709, 0x070A, 0x070F, 0x0710, 0x0711). */
#define CELL_SPURS_CORE_ERROR_AGAIN             (CELL_ERROR_BASE_SPURS | 0x01)
#define CELL_SPURS_CORE_ERROR_INVAL             (CELL_ERROR_BASE_SPURS | 0x02)
#define CELL_SPURS_CORE_ERROR_NOMEM             (CELL_ERROR_BASE_SPURS | 0x04)
#define CELL_SPURS_CORE_ERROR_SRCH              (CELL_ERROR_BASE_SPURS | 0x05)
#define CELL_SPURS_CORE_ERROR_BUSY              (CELL_ERROR_BASE_SPURS | 0x0A)
#define CELL_SPURS_CORE_ERROR_STAT              (CELL_ERROR_BASE_SPURS | 0x0F)
#define CELL_SPURS_CORE_ERROR_ALIGN             (CELL_ERROR_BASE_SPURS | 0x10)
#define CELL_SPURS_CORE_ERROR_NULL_POINTER      (CELL_ERROR_BASE_SPURS | 0x11)
#define CELL_SPURS_CORE_ERROR_PERM              (CELL_ERROR_BASE_SPURS | 0x09)

/* Policy-module return codes (RPCS3 CellSpursPolicyModuleError). */
#define CELL_SPURS_POLICY_MODULE_ERROR_AGAIN        (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x01)
#define CELL_SPURS_POLICY_MODULE_ERROR_INVAL        (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x02)
#define CELL_SPURS_POLICY_MODULE_ERROR_NOSYS        (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x03)
#define CELL_SPURS_POLICY_MODULE_ERROR_NOMEM        (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x04)
#define CELL_SPURS_POLICY_MODULE_ERROR_SRCH         (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x05)
#define CELL_SPURS_POLICY_MODULE_ERROR_NOENT        (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x06)
#define CELL_SPURS_POLICY_MODULE_ERROR_NOEXEC       (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x07)
#define CELL_SPURS_POLICY_MODULE_ERROR_DEADLK       (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x08)
#define CELL_SPURS_POLICY_MODULE_ERROR_PERM         (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x09)
#define CELL_SPURS_POLICY_MODULE_ERROR_BUSY         (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x0A)
#define CELL_SPURS_POLICY_MODULE_ERROR_ABORT        (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x0C)
#define CELL_SPURS_POLICY_MODULE_ERROR_FAULT        (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x0D)
#define CELL_SPURS_POLICY_MODULE_ERROR_CHILD        (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x0E)
#define CELL_SPURS_POLICY_MODULE_ERROR_STAT         (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x0F)
#define CELL_SPURS_POLICY_MODULE_ERROR_ALIGN        (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x10)
#define CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER (CELL_SPURS_ERROR_BASE_POLICY_MODULE | 0x11)

/* Task return codes (RPCS3 CellSpursTaskError). The task range is 0x804109xx:
 * the CreateTask NULL_POINTER/ALIGN returns above and the STAT return of
 * TaskExitCodeInitialize are the firmware's own literals. */
#define CELL_SPURS_TASK_ERROR_AGAIN             (CELL_SPURS_ERROR_BASE_TASK | 0x01)
#define CELL_SPURS_TASK_ERROR_INVAL             (CELL_SPURS_ERROR_BASE_TASK | 0x02)
#define CELL_SPURS_TASK_ERROR_NOSYS             (CELL_SPURS_ERROR_BASE_TASK | 0x03)
#define CELL_SPURS_TASK_ERROR_NOMEM             (CELL_SPURS_ERROR_BASE_TASK | 0x04)
#define CELL_SPURS_TASK_ERROR_SRCH              (CELL_SPURS_ERROR_BASE_TASK | 0x05)
#define CELL_SPURS_TASK_ERROR_NOEXEC            (CELL_SPURS_ERROR_BASE_TASK | 0x07)
#define CELL_SPURS_TASK_ERROR_PERM              (CELL_SPURS_ERROR_BASE_TASK | 0x09)
#define CELL_SPURS_TASK_ERROR_BUSY              (CELL_SPURS_ERROR_BASE_TASK | 0x0A)
#define CELL_SPURS_TASK_ERROR_FAULT             (CELL_SPURS_ERROR_BASE_TASK | 0x0D)
#define CELL_SPURS_TASK_ERROR_STAT              (CELL_SPURS_ERROR_BASE_TASK | 0x0F)
#define CELL_SPURS_TASK_ERROR_ALIGN             (CELL_SPURS_ERROR_BASE_TASK | 0x10)
#define CELL_SPURS_TASK_ERROR_NULL_POINTER      (CELL_SPURS_ERROR_BASE_TASK | 0x11)
#define CELL_SPURS_TASK_ERROR_FATAL             (CELL_SPURS_ERROR_BASE_TASK | 0x14)
#define CELL_SPURS_TASK_ERROR_SHUTDOWN          (CELL_SPURS_ERROR_BASE_TASK | 0x20)

/* Job return codes (RPCS3 CellSpursJobError). */
#define CELL_SPURS_JOB_ERROR_AGAIN               (CELL_SPURS_ERROR_BASE_JOB | 0x01)
#define CELL_SPURS_JOB_ERROR_INVAL               (CELL_SPURS_ERROR_BASE_JOB | 0x02)
#define CELL_SPURS_JOB_ERROR_NOSYS               (CELL_SPURS_ERROR_BASE_JOB | 0x03)
#define CELL_SPURS_JOB_ERROR_NOMEM               (CELL_SPURS_ERROR_BASE_JOB | 0x04)
#define CELL_SPURS_JOB_ERROR_SRCH                (CELL_SPURS_ERROR_BASE_JOB | 0x05)
#define CELL_SPURS_JOB_ERROR_NOENT               (CELL_SPURS_ERROR_BASE_JOB | 0x06)
#define CELL_SPURS_JOB_ERROR_NOEXEC              (CELL_SPURS_ERROR_BASE_JOB | 0x07)
#define CELL_SPURS_JOB_ERROR_DEADLK              (CELL_SPURS_ERROR_BASE_JOB | 0x08)
#define CELL_SPURS_JOB_ERROR_PERM                (CELL_SPURS_ERROR_BASE_JOB | 0x09)
#define CELL_SPURS_JOB_ERROR_BUSY                (CELL_SPURS_ERROR_BASE_JOB | 0x0A)
#define CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR      (CELL_SPURS_ERROR_BASE_JOB | 0x0B)
#define CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR_SIZE (CELL_SPURS_ERROR_BASE_JOB | 0x0C)
#define CELL_SPURS_JOB_ERROR_FAULT               (CELL_SPURS_ERROR_BASE_JOB | 0x0D)
#define CELL_SPURS_JOB_ERROR_CHILD               (CELL_SPURS_ERROR_BASE_JOB | 0x0E)
#define CELL_SPURS_JOB_ERROR_STAT                (CELL_SPURS_ERROR_BASE_JOB | 0x0F)
#define CELL_SPURS_JOB_ERROR_ALIGN               (CELL_SPURS_ERROR_BASE_JOB | 0x10)
#define CELL_SPURS_JOB_ERROR_NULL_POINTER        (CELL_SPURS_ERROR_BASE_JOB | 0x11)
#define CELL_SPURS_JOB_ERROR_MEMORY_CORRUPTED    (CELL_SPURS_ERROR_BASE_JOB | 0x12)
#define CELL_SPURS_JOB_ERROR_MEMORY_SIZE         (CELL_SPURS_ERROR_BASE_JOB | 0x17)
#define CELL_SPURS_JOB_ERROR_UNKNOWN_COMMAND     (CELL_SPURS_ERROR_BASE_JOB | 0x18)
#define CELL_SPURS_JOB_ERROR_JOBLIST_ALIGNMENT   (CELL_SPURS_ERROR_BASE_JOB | 0x19)
#define CELL_SPURS_JOB_ERROR_JOB_ALIGNMENT       (CELL_SPURS_ERROR_BASE_JOB | 0x1A)
#define CELL_SPURS_JOB_ERROR_CALL_OVERFLOW       (CELL_SPURS_ERROR_BASE_JOB | 0x1B)
#define CELL_SPURS_JOB_ERROR_ABORT               (CELL_SPURS_ERROR_BASE_JOB | 0x1C)
#define CELL_SPURS_JOB_ERROR_DMALIST_ELEMENT     (CELL_SPURS_ERROR_BASE_JOB | 0x1D)
#define CELL_SPURS_JOB_ERROR_NUM_CACHE           (CELL_SPURS_ERROR_BASE_JOB | 0x1E)
#define CELL_SPURS_JOB_ERROR_INVALID_BINARY      (CELL_SPURS_ERROR_BASE_JOB | 0x1F)

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define CELL_SPURS_MAX_SPU                  8
#define CELL_SPURS_MAX_WORKLOAD             16
#define CELL_SPURS_MAX_TASK                 128
#define CELL_SPURS_MAX_TASKSET              16
#define CELL_SPURS_MAX_PRIORITY             16

#define CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS    16

/* Event flag modes */
#define CELL_SPURS_EVENT_FLAG_CLEAR_AUTO        0
#define CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL      1

/* Event flag wait modes */
#define CELL_SPURS_EVENT_FLAG_OR                0
#define CELL_SPURS_EVENT_FLAG_AND               1

/* Event flag directions */
#define CELL_SPURS_EVENT_FLAG_SPU2SPU           0
#define CELL_SPURS_EVENT_FLAG_SPU2PPU           1
#define CELL_SPURS_EVENT_FLAG_PPU2SPU           2
#define CELL_SPURS_EVENT_FLAG_ANY2ANY           3

/* Workload flags */
#define CELL_SPURS_WORKLOAD_ATTR_RESET_ON_DETACH   0x0001

/* ---------------------------------------------------------------------------
 * Structures
 *
 * Note: On real PS3 hardware these must be 128-byte aligned for DMA.
 * We relax alignment for host HLE but keep the size fields.
 * -----------------------------------------------------------------------*/

/* Forward declarations for opaque internal data */
typedef struct CellSpurs CellSpurs;
typedef struct CellSpursAttribute CellSpursAttribute;
typedef struct CellSpursTaskset CellSpursTaskset;
typedef struct CellSpursTasksetAttribute CellSpursTasksetAttribute;
typedef struct CellSpursTask CellSpursTask;
typedef struct CellSpursTaskAttribute CellSpursTaskAttribute;
typedef struct CellSpursTaskExitCode CellSpursTaskExitCode;
typedef struct CellSpursEventFlag CellSpursEventFlag;
typedef struct CellSpursWorkloadAttribute CellSpursWorkloadAttribute;

typedef u32 CellSpursWorkloadId;
typedef u32 CellSpursTaskId;

/* SPURS attribute -- used to configure a SPURS instance before creation */
struct CellSpursAttribute {
    u32  nSpus;
    s32  spuPriority[CELL_SPURS_MAX_SPU];
    u8   prefix[16];
    u32  prefixSize;
    u32  flags;
    u32  container;
    u8   _padding[128];
};

/* Main SPURS instance */
struct CellSpurs {
    u32  initialized;
    u32  nSpus;
    u32  flags;
    u8   prefix[16];
    /* Internal worker state (host threads) */
    void* _internal;
    u8   _padding[256];
};

/* Taskset -- container for SPU tasks */
struct CellSpursTaskset {
    u32  initialized;
    u32  taskCount;
    u32  shutdownRequested;
    CellSpurs* spurs;
    u8   _padding[128];
};

struct CellSpursTasksetAttribute {
    u32  revision;
    u32  flags;
    u64  args;
    u8   priority[CELL_SPURS_MAX_SPU];
    u32  tasksetSize; /* host-layout attribute; retained through creation */
    u8   _padding[60];
};

/* Task -- represents a unit of SPU work */
struct CellSpursTask {
    u32  id;
    u32  active;
    u32  completed;
    void* entryPoint;
    u64  argA;
    u64  argB;
    u8   _padding[64];
};

struct CellSpursTaskAttribute {
    u32  revision;
    u32  sizeContext;
    u64  eaContext;
    u64  eaElf;        /* SPU task ELF guest EA (set by _cellSpursTaskAttributeInitialize) */
    /* Guest EAs of the 16-byte CellSpursTaskLsPattern / CellSpursTaskArgument
     * blocks passed to _cellSpursTaskAttributeInitialize. A wait-capable task
     * NEEDS both carried into its TaskInfo: the SPU task library validates
     * argument != 0 and lsPattern-covers-stack before any blocking wait
     * (else 0x8041090F) -- dropping them kept LBP's binkspu movie-IO task
     * spinning forever. */
    u32  lsPattern_ea;
    u32  argument_ea;
    u8   _padding[48];
};

/* Guest-owned exit container: unlike the legacy host records above, both its
 * 128-byte size and its guest address alignment are part of the API. */
struct CellSpursTaskExitCode {
#if defined(_MSC_VER)
    __declspec(align(128)) u8 reserved[128];
#elif defined(__cplusplus)
    alignas(128) u8 reserved[128];
#else
    _Alignas(128) u8 reserved[128];
#endif
};

/* Event flag -- SPURS-level event synchronization */
struct CellSpursEventFlag {
    u32  initialized;
    u16  bits;
    u16  clearMode;
    u16  direction;
    u8   _padding[64];
};

/* Workload attribute */
struct CellSpursWorkloadAttribute {
    u32  revision;
    u32  sdkVersion;
    u64  pm;
    u32  sizePm;
    u64  data;
    u8   priority[CELL_SPURS_MAX_SPU];
    u32  minContention;
    u32  maxContention;
    u8   _padding[128];
};

/* ---------------------------------------------------------------------------
 * SPURS core functions
 * -----------------------------------------------------------------------*/

s32 cellSpursInitialize(CellSpurs* spurs, s32 nSpus, s32 spuPriority,
                        s32 ppuPriority, u8 exitIfNoWork);
s32 cellSpursInitializeWithAttribute(CellSpurs* spurs,
                                     const CellSpursAttribute* attr);
s32 cellSpursFinalize(CellSpurs* spurs);

s32 cellSpursAttributeInitialize(CellSpursAttribute* attr, s32 nSpus,
                                 s32 spuPriority, s32 ppuPriority,
                                 u8 exitIfNoWork);
s32 cellSpursAttributeSetNamePrefix(CellSpursAttribute* attr,
                                    const char* prefix, u32 size);
s32 cellSpursAttributeSetSpuThreadGroupType(CellSpursAttribute* attr,
                                            s32 type);
s32 cellSpursAttributeEnableSpuPrintfIfAvailable(CellSpursAttribute* attr);

s32 cellSpursGetNumSpuThread(const CellSpurs* spurs, u32* nThreads);
s32 cellSpursSetMaxContention(CellSpurs* spurs, CellSpursWorkloadId wid,
                              u32 maxContention);
s32 cellSpursSetPriorities(CellSpurs* spurs, CellSpursWorkloadId wid,
                           const u8* priorities);

s32 cellSpursAttachLv2EventQueue(CellSpurs* spurs, u32 queue, u8* port,
                                 s32 isDynamic);
s32 cellSpursDetachLv2EventQueue(CellSpurs* spurs, u8 port);

/* ---------------------------------------------------------------------------
 * Taskset functions
 * -----------------------------------------------------------------------*/

s32 cellSpursCreateTaskset(CellSpurs* spurs, CellSpursTaskset* taskset,
                           u64 args, const u8* priority, u32 maxContention);
s32 cellSpursCreateTasksetWithAttribute(CellSpurs* spurs,
                                        CellSpursTaskset* taskset,
                                        const CellSpursTasksetAttribute* attr);
s32 cellSpursDestroyTaskset(CellSpursTaskset* taskset);
s32 cellSpursShutdownTaskset(CellSpursTaskset* taskset);
s32 cellSpursJoinTaskset(CellSpursTaskset* taskset);

s32 cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute* attr);
s32 cellSpursTasksetAttributeSetName(CellSpursTasksetAttribute* attr,
                                      const char* name);

/* ---------------------------------------------------------------------------
 * Task functions
 * -----------------------------------------------------------------------*/

/* 7-arg SDK ABI: ... , CellSpursTaskLsPattern* lsPattern, CellSpursTaskArgument* argument.
 * Both are guest EAs (passed as u32 by the generic HLE adapter). */
s32 cellSpursCreateTask(CellSpursTaskset* taskset, CellSpursTaskId* taskId,
                        void* elf, void* context, u32 sizeContext,
                        u32 lsPattern_ea, u32 argument_ea);
s32 cellSpursJoinTask(CellSpursTaskset* taskset, CellSpursTaskId taskId,
                      s32* exitCode);
s32 cellSpursSendSignal(CellSpursTaskset* taskset, CellSpursTaskId taskId);

s32 cellSpursTaskAttributeInitialize(CellSpursTaskAttribute* attr);
s32 cellSpursTaskExitCodeInitialize(CellSpursTaskExitCode* exitCode);

/* ---------------------------------------------------------------------------
 * Workload functions
 * -----------------------------------------------------------------------*/

s32 cellSpursAddWorkload(CellSpurs* spurs, CellSpursWorkloadId* wid,
                         const void* pm, u32 sizePm, u64 data,
                         const u8* priority, u32 minContention,
                         u32 maxContention);
s32 cellSpursAddWorkloadWithAttribute(CellSpurs* spurs,
                                       CellSpursWorkloadId* wid,
                                       const CellSpursWorkloadAttribute* attr);
s32 cellSpursRemoveWorkload(CellSpurs* spurs, CellSpursWorkloadId wid);

s32 cellSpursWorkloadAttributeInitialize(CellSpursWorkloadAttribute* attr,
                                         u32 revision, u32 sdkVersion,
                                         const void* pm, u32 sizePm,
                                         u64 data, const u8* priority,
                                         u32 minContention,
                                         u32 maxContention);

s32 cellSpursReadyCountStore(CellSpurs* spurs, CellSpursWorkloadId wid,
                             u32 value);
s32 cellSpursReadyCountSwap(CellSpurs* spurs, CellSpursWorkloadId wid,
                            u32* old, u32 value);
s32 cellSpursReadyCountCompareAndSwap(CellSpurs* spurs,
                                       CellSpursWorkloadId wid,
                                       u32* old, u32 compare, u32 value);
s32 cellSpursWakeUp(CellSpurs* spurs);

/* ---------------------------------------------------------------------------
 * Event flag functions
 * -----------------------------------------------------------------------*/

s32 cellSpursEventFlagInitialize(CellSpursTaskset* taskset,
                                 CellSpursEventFlag* eventFlag,
                                 u32 clearMode, u32 direction);
s32 cellSpursEventFlagAttachLv2EventQueue(CellSpursEventFlag* eventFlag);

/* Task LS patterns / context save area sizing, and the queue surface Saints
 * Row 2's taskset build path calls. ABI for the first two is read off that
 * call site (see the block comment in cellSpurs.c); pointer args are guest
 * EAs, so they are taken as u64 and translated inside. */
s32 cellSpursTaskGetReadOnlyAreaPattern(u64 pattern_ea, u64 elf_ea);
s32 cellSpursTaskGetContextSaveAreaSize(u64 size_out_ea, u64 pattern_ea);
s32 cellSpursBarrierInitialize(u64 taskset_ea, u64 barrier_ea, u32 total);
s32 cellSpursGetTasksetInfo(u64 taskset_ea, u64 info_ea);
s32 cellSpursTasksetAttributeSetTasksetSize(CellSpursTasksetAttribute* attr, u32 size);
s32 cellSpursQueueAttachLv2EventQueue(u64 queue_ea);
s32 cellSpursQueueSize(u64 queue_ea, u64 size_ea);
s32 _cellSpursLFQueueInitialize(u64 owner_ea, u64 queue_ea, u64 buffer_ea,
                                u32 size, u32 depth, u32 direction);
s32 cellSpursLFQueueAttachLv2EventQueue(u64 queue_ea);

s32 cellSpursEventFlagSet(CellSpursEventFlag* eventFlag, u16 bits);
s32 cellSpursEventFlagWait(CellSpursEventFlag* eventFlag, u16* bits,
                           u32 mode);
s32 cellSpursEventFlagTryWait(CellSpursEventFlag* eventFlag, u16* bits,
                              u32 mode);
s32 cellSpursEventFlagClear(CellSpursEventFlag* eventFlag, u16 bits);
s32 cellSpursEventFlagGetDirection(CellSpursEventFlag* eventFlag,
                                   u32* direction);

s32 cellSpursEventFlagDetachLv2EventQueue(CellSpursEventFlag* eventFlag);

/* ---------------------------------------------------------------------------
 * SPURS attribute initialisers, job chains, queues and job guards.
 *
 * The SDK's "_"-prefixed initialisers and the JobQueue2 surface. Their
 * definitions are in cellSpurs.c; several take decoded guest effective
 * addresses (u64 so the caller's full register is available) and are
 * registered as context-aware or plain HLE handlers accordingly.
 * -----------------------------------------------------------------------*/

s32 _cellSpursAttributeInitialize(CellSpursAttribute* attr, u32 revision,
                                  u32 sdkVersion, s32 nSpus, s32 spuPriority,
                                  s32 ppuPriority, u8 exitIfNoWork);
s32 _cellSpursEventFlagInitialize(void* spurs, void* taskset,
                                  CellSpursEventFlag* eventFlag,
                                  u32 clearMode, u32 direction);
s32 _cellSpursJobChainAttributeInitialize(u32 jmRevision, u32 sdkRevision,
                                          u64 attr_ea, u64 entry_ea,
                                          u32 sizeJobDescriptor, u32 maxGrabbedJob,
                                          u64 prio_ea, u32 maxContention);
s32 _cellSpursQueueInitialize(u64 spurs_ea, u64 taskset_ea, u64 queue_ea,
                              u64 buffer_ea, u32 size, u32 depth, u32 direction);
s32 _cellSpursSendSignal(void* taskset, u32 taskId);
s32 _cellSpursTaskAttributeInitialize(CellSpursTaskAttribute* attr, u32 revision,
                                      u32 sdkVersion, u64 eaElf, u64 eaContext,
                                      u32 sizeContext, const void* lsPattern,
                                      const void* argument);
s32 _cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute* attr,
                                         u32 revision, u32 sdkVersion,
                                         u64 argTaskset, u64 priority,
                                         u32 maxContention);
s32 _cellSpursWorkloadAttributeInitialize(u64 attr_ea, u32 revision,
                                          u32 sdkVersion, u64 pm_ea, u32 size,
                                          u64 data, u64 prio_ea, u32 minContention);

s32 cellSpursCreateJobChain(u64 spurs_ea, u64 jc_ea, u64 entry_ea,
                            u32 sizeJobDescriptor, u32 maxGrabbedJob,
                            u64 prio_ea, u32 maxContention,
                            u32 autoRequestSpuCount);
s32 cellSpursCreateJobChainWithAttribute(u64 spurs_ea, u64 jc_ea, u64 attr_ea);
s32 cellSpursCreateTaskWithAttribute(CellSpursTaskset* taskset,
                                     CellSpursTaskId* taskId,
                                     CellSpursTaskAttribute* attr);
s32 cellSpursGetInfo(u64 spurs_ea, u64 info_ea);
s32 cellSpursGetWorkloadInfo(u64 spurs_ea, u32 wid, u64 info_ea);
s32 cellSpursJobChainAttributeSetName(u64 attr_ea, u64 name_ea);
s32 cellSpursJobChainGetError(u64 jc_ea, u64 cause_out_ea);
s32 cellSpursJobGuardInitialize(u64 jc_ea, u64 guard_ea, u32 notify_count,
                                u32 request_spu_count, u32 auto_reset);
s32 cellSpursJobGuardNotify(u64 guard_ea);
s32 cellSpursJobGuardReset(u64 guard_ea);
s32 cellSpursJoinJobChain(u64 jc_ea);
s32 cellSpursKickJobChain(u64 jc_ea, u32 numReadyCount);
s32 cellSpursQueueClear(u64 queue_ea);
s32 cellSpursQueuePushBody(u64 queue_ea, u64 data_ea, u32 isBlocking);
s32 cellSpursRequestIdleSpu(u64 spurs_ea);
s32 cellSpursRunJobChain(u64 jc_ea);
s32 cellSpursSetExceptionEventHandler(u64 spurs_ea, u64 handler_ea, u64 arg_ea);
s32 cellSpursShutdownJobChain(u64 jc_ea);
s32 cellSpursShutdownWorkload(u64 spurs_ea, u32 wid);
s32 cellSpursWaitForWorkloadShutdown(u64 spurs_ea, u32 wid);
s32 cellSpursWorkloadAttributeSetName(u64 attr_ea, u64 nameClass_ea,
                                      u64 nameInstance_ea);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SPURS_H */

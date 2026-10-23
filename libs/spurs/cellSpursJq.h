/*
 * ps3recomp - cellSpursJq HLE: the SPURS JobQueue2 PPU export surface
 *
 * The 17 `cellSpursJq` exports a title imports, with the firmware's argument
 * order and error codes (FW 4.93 libspurs_jq, export map in
 * work/m05/jq2/JQ-FW-PPU.md). Every handler is a thin adapter: it decodes the
 * guest arguments and calls the typed C contract in libs/spurs/jq2/jq2.h,
 * which owns the ring, the scheduler, the descriptor pool and the SPU workers.
 * None of the JobQueue logic lives here.
 *
 *   _cellSpursCreateJobQueue (0xF244E799), 10 args   jq2_queue_create (no pool)
 *   _cellSpursCreateJobQueueWithJobDescriptorPool
 *                            (0x983BE7FC), 12 args   jq2_queue_create (pool)
 *   cellSpursShutdownJobQueue (0x8C97A96C)            jq2_queue_shutdown
 *   cellSpursJoinJobQueue (0x8CE4CFFA)                jq2_queue_join
 *   cellSpursJobQueuePort2Create (0xF11FE0F1)         jq2_port_create
 *   cellSpursJobQueuePort2Destroy (0xC485D207)        jq2_port_destroy
 *   _cellSpursJobQueuePort2PushJobBody (0xCF89F218)   jq2_port_push_job
 *   _cellSpursJobQueuePort2CopyPushJobBody
 *                            (0xC39173A4)             jq2_port_copy_push
 *   cellSpursJobQueuePort2PushSync (0xFC526B72)       jq2_port_push_sync
 *   cellSpursJobQueuePort2Sync (0xFACB3CED)           jq2_port_sync
 *
 * The seven attribute exports edit the caller's attribute block only:
 *
 *   +0x04  u32  grab/class parameter, 1..16 (Initialize writes 4)
 *   +0x08  u32  maximum job descriptor size, 256..1023 and 128-aligned
 *               (Initialize writes 256; the observed title sets 896)
 *   +0x0C  u8   submitWithEntryLock
 *   +0x0D  u8   doBusyWaiting
 *   +0x0E  u8   isHaltOnError
 *   +0x0F  u8   isJobTypeMemoryCheck
 *   +0x10  u32  grab parameters, 0..255 (Initialize writes 255; the create
 *               path reads byte +0x13)
 *   +0x00 and +0x14.. are not written by these exports; the firmware leaves
 *   them to the caller.
 *
 * All pointers are 32-bit guest effective addresses (the toolkit's GUEST_PTR
 * convention). All functions return the firmware's s32: 0 or one of the
 * JQ2_ERR_* values in jq2/jq2.h. An unimplemented path is a named stop, never
 * CELL_OK.
 *
 * Firmware evidence: export OPDs and code ranges in JQ-FW-PPU.md; the
 * attribute exports are the code at image EAs 0x02000010-0x020001C8; the
 * create bodies validate attr+0x04/+0x08 (0x0200057C-0x020005B8, jq2fw-019)
 * and copy +0x0C..+0x0F/+0x13 into the queue's flags (0x02000748-0x02000790,
 * jq2fw-020).
 */

#ifndef PS3RECOMP_CELL_SPURS_JQ_H
#define PS3RECOMP_CELL_SPURS_JQ_H

#include "ps3emu/ps3types.h"
#include "jq2/jq2.h"   /* JQ2_ERR_*, the one firmware error table */

#ifdef __cplusplus
extern "C" {
#endif

/* The caller-owned attribute block, firmware layout. The names follow the
 * setters that write each field; the create path treats +0x04 as the
 * descriptor-class count and +0x08 as the maximum descriptor size
 * (JQ-FW-PPU, jq2fw-019). */
typedef struct CellSpursJobQueueAttribute {
    u32 reserved00;             /* +0x00, never touched by the exports */
    u32 maxGrab;                /* +0x04, 1..16 */
    u32 maxSizeJobDescriptor;   /* +0x08, 256..1023, multiple of 128 */
    u8  submitWithEntryLock;    /* +0x0C */
    u8  doBusyWaiting;          /* +0x0D */
    u8  isHaltOnError;          /* +0x0E */
    u8  isJobTypeMemoryCheck;   /* +0x0F */
    u32 grabParameters;         /* +0x10, 0..255; byte +0x13 read by create */
    /* The firmware writes no field above +0x13. */
} CellSpursJobQueueAttribute;

/* ---- attribute block (NIDs 0x43DDAB4F, 0x1686957E, 0xE70F874E,
 *      0x0582338A, 0x0F03F712, 0x742CEC0D, 0xFF03CC79) -------------------- */

s32 cellSpursJobQueueAttributeInitialize(CellSpursJobQueueAttribute* attr);
s32 cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(CellSpursJobQueueAttribute* attr, u32 size);
s32 cellSpursJobQueueAttributeSetGrabParameters(CellSpursJobQueueAttribute* attr,
                                                u32 grabParameters, u32 maxGrab);
s32 cellSpursJobQueueAttributeSetSubmitWithEntryLock(CellSpursJobQueueAttribute* attr, s32 submitWithEntryLock);
s32 cellSpursJobQueueAttributeSetDoBusyWaiting(CellSpursJobQueueAttribute* attr, s32 doBusyWaiting);
s32 cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck(CellSpursJobQueueAttribute* attr, s32 isCheck);
s32 cellSpursJobQueueAttributeSetIsHaltOnError(CellSpursJobQueueAttribute* attr, s32 isHaltOnError);

/* ---- queue lifecycle (NIDs 0xF244E799, 0x983BE7FC, 0x8C97A96C, 0x8CE4CFFA) */

s32 _cellSpursCreateJobQueue(u32 revision, u32 sdk_version, void* spurs,
                             void* queue, const CellSpursJobQueueAttribute* attr,
                             const char* name, void* command_buffer,
                             u32 command_depth, u32 worker_count, void* extra);
s32 _cellSpursCreateJobQueueWithJobDescriptorPool(u32 revision, u32 sdk_version, void* spurs,
                                                  void* queue, const CellSpursJobQueueAttribute* attr,
                                                  void* pool, const void* pool_attr,
                                                  const char* name, void* command_buffer,
                                                  u32 command_depth, u32 worker_count, void* extra);
s32 cellSpursShutdownJobQueue(void* queue);
s32 cellSpursJoinJobQueue(void* queue, s32* exit_code);

/* ---- port (NIDs 0xF11FE0F1, 0xC485D207) --------------------------------- */

s32 cellSpursJobQueuePort2Create(void* port, void* queue);
s32 cellSpursJobQueuePort2Destroy(void* port);

/* ---- pushes and sync (NIDs 0xCF89F218, 0xC39173A4, 0xFC526B72, 0xFACB3CED) */

s32 _cellSpursJobQueuePort2PushJobBody(void* port, const void* descriptor,
                                       u32 descriptor_size, u32 tag, u32 flags);
s32 _cellSpursJobQueuePort2CopyPushJobBody(void* port, const void* source,
                                           u32 descriptor_size, u32 copy_size,
                                           u32 tag, u32 flags);
s32 cellSpursJobQueuePort2PushSync(void* port, u32 tag_mask, u32 flags);
s32 cellSpursJobQueuePort2Sync(void* port, u32 mode);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SPURS_JQ_H */

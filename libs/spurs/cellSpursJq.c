/*
 * ps3recomp - cellSpursJq HLE implementation
 *
 * The PPU-side SPURS JobQueue2 surface. Each function is an argument adapter
 * over libs/spurs/jq2/jq2.h; the guards, guest-memory updates, ring writes,
 * descriptor pool and SPU workers live in the executor. Firmware signatures,
 * argument order and return codes are from FW 4.93 libspurs_jq
 * (work/m05/jq2/JQ-FW-PPU.md, KB jq2fw-*).
 *
 * No function here returns CELL_OK for a path it did not take: an argument the
 * firmware rejects returns the firmware's code, and anything the model cannot
 * honour stops by name inside the executor.
 */

#include "cellSpursJq.h"

#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: big-endian guest stores */

/* A pointer parameter carries a 32-bit guest EA (docs/HLE_ABI.md). Never
 * dereference these as host pointers. */
#define GUEST_EA(p) ((u32)(uintptr_t)(p))

/* ---- attribute block ---------------------------------------------------
 *
 * Firmware code (relocated image base 0x02000000):
 *   AttributeInitialize                  0x02000010-0x02000058
 *   SetMaxSizeJobDescriptor              0x02000128-0x02000178
 *   SetGrabParameters                    0x02000178-0x020001C8
 *   SetSubmitWithEntryLock               0x02000098-0x020000BC
 *   SetDoBusyWaiting                     0x020000BC-0x020000E0
 *   SetIsHaltOnError                     0x020000E0-0x02000104
 *   SetIsJobTypeMemoryCheck              0x02000104-0x02000128
 * Each checks only attr != NULL (A11); none checks alignment.
 */

s32 cellSpursJobQueueAttributeInitialize(CellSpursJobQueueAttribute* attr)
{
    if (!attr) return JQ2_ERR_NULL_POINTER;

    /* 0x02000024-0x0200004C: +0x10 := 255 (its top byte +0x13 becomes 0xFF),
     * +0x04 := 4, +0x08 := 256, +0x0C..+0x0F := 0. +0x00 is left alone. */
    const u32 ea = GUEST_EA(attr);
    vm_write32(ea + 0x10, 255);
    vm_write32(ea + 0x04, 4);
    vm_write32(ea + 0x08, 256);
    vm_write8(ea + 0x0F, 0);
    vm_write8(ea + 0x0C, 0);
    vm_write8(ea + 0x0D, 0);
    vm_write8(ea + 0x0E, 0);
    return JQ2_OK;
}

s32 cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(CellSpursJobQueueAttribute* attr, u32 size)
{
    if (!attr) return JQ2_ERR_NULL_POINTER;
    /* 0x02000128-0x02000174: (size - 256) <= 767 and size & 127 == 0. */
    if (size < 256 || size > 1023 || (size & 127u) != 0) return JQ2_ERR_INVAL;
    vm_write32(GUEST_EA(attr) + 0x08, size);
    return JQ2_OK;
}

s32 cellSpursJobQueueAttributeSetGrabParameters(CellSpursJobQueueAttribute* attr,
                                                u32 grabParameters, u32 maxGrab)
{
    if (!attr) return JQ2_ERR_NULL_POINTER;
    /* 0x02000178-0x020001C4: arg2 <= 255, 1 <= arg3 <= 16, then
     * +0x04 := arg3 and +0x10 := arg2. */
    if (grabParameters > 255 || maxGrab < 1 || maxGrab > 16) return JQ2_ERR_INVAL;
    const u32 ea = GUEST_EA(attr);
    vm_write32(ea + 0x04, maxGrab);
    vm_write32(ea + 0x10, grabParameters);
    return JQ2_OK;
}

s32 cellSpursJobQueueAttributeSetSubmitWithEntryLock(CellSpursJobQueueAttribute* attr, s32 submitWithEntryLock)
{
    if (!attr) return JQ2_ERR_NULL_POINTER;
    vm_write8(GUEST_EA(attr) + 0x0C, (u8)submitWithEntryLock);   /* 0x020000B0 */
    return JQ2_OK;
}

s32 cellSpursJobQueueAttributeSetDoBusyWaiting(CellSpursJobQueueAttribute* attr, s32 doBusyWaiting)
{
    if (!attr) return JQ2_ERR_NULL_POINTER;
    vm_write8(GUEST_EA(attr) + 0x0D, (u8)doBusyWaiting);        /* 0x020000D4 */
    return JQ2_OK;
}

s32 cellSpursJobQueueAttributeSetIsHaltOnError(CellSpursJobQueueAttribute* attr, s32 isHaltOnError)
{
    if (!attr) return JQ2_ERR_NULL_POINTER;
    vm_write8(GUEST_EA(attr) + 0x0E, (u8)isHaltOnError);        /* 0x020000F8 */
    return JQ2_OK;
}

s32 cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck(CellSpursJobQueueAttribute* attr, s32 isCheck)
{
    if (!attr) return JQ2_ERR_NULL_POINTER;
    vm_write8(GUEST_EA(attr) + 0x0F, (u8)isCheck);              /* 0x0200011C */
    return JQ2_OK;
}

/* ---- queue lifecycle ---------------------------------------------------
 *
 * _cellSpursCreateJobQueue (0x02000CA8) forwards to the common 12-argument
 * body (0x020003CC) with pool = pool_attr = NULL; the pool export enters the
 * same body directly (jq2fw-015/016/018). Argument order is the one the title
 * builds at EBOOT 0x72092C and 0x7C6148.
 */

s32 _cellSpursCreateJobQueue(u32 revision, u32 sdk_version, void* spurs,
                             void* queue, const CellSpursJobQueueAttribute* attr,
                             const char* name, void* command_buffer,
                             u32 command_depth, u32 worker_count, void* extra)
{
    return jq2_queue_create(revision, sdk_version, GUEST_EA(spurs), GUEST_EA(queue), GUEST_EA(attr),
                            0, 0, GUEST_EA(name), GUEST_EA(command_buffer), command_depth, worker_count,
                            GUEST_EA(extra));
}

s32 _cellSpursCreateJobQueueWithJobDescriptorPool(u32 revision, u32 sdk_version, void* spurs,
                                                  void* queue, const CellSpursJobQueueAttribute* attr,
                                                  void* pool, const void* pool_attr,
                                                  const char* name, void* command_buffer,
                                                  u32 command_depth, u32 worker_count, void* extra)
{
    return jq2_queue_create(revision, sdk_version, GUEST_EA(spurs), GUEST_EA(queue), GUEST_EA(attr),
                            GUEST_EA(pool), GUEST_EA(pool_attr), GUEST_EA(name), GUEST_EA(command_buffer),
                            command_depth, worker_count, GUEST_EA(extra));
}

s32 cellSpursShutdownJobQueue(void* queue)
{
    return jq2_queue_shutdown(GUEST_EA(queue));
}

s32 cellSpursJoinJobQueue(void* queue, s32* exit_code)
{
    /* jq2_queue_join takes the one host pointer in this surface: the word the
     * firmware writes through its out argument. The guest store happens here,
     * after Join has stopped every worker (0x02000E04-0x02000E18, jq2fw-044). */
    u32 value = 0;
    const s32 rc = jq2_queue_join(GUEST_EA(queue), exit_code ? &value : NULL);
    if (rc == JQ2_OK) vm_write32(GUEST_EA(exit_code), value);
    return rc;
}

/* ---- port -------------------------------------------------------------- */

s32 cellSpursJobQueuePort2Create(void* port, void* queue)
{
    return jq2_port_create(GUEST_EA(port), GUEST_EA(queue));
}

s32 cellSpursJobQueuePort2Destroy(void* port)
{
    return jq2_port_destroy(GUEST_EA(port));
}

/* ---- pushes and sync --------------------------------------------------- */

s32 _cellSpursJobQueuePort2PushJobBody(void* port, const void* descriptor,
                                       u32 descriptor_size, u32 tag, u32 flags)
{
    return jq2_port_push_job(GUEST_EA(port), GUEST_EA(descriptor), descriptor_size, tag, flags);
}

s32 _cellSpursJobQueuePort2CopyPushJobBody(void* port, const void* source,
                                           u32 descriptor_size, u32 copy_size,
                                           u32 tag, u32 flags)
{
    return jq2_port_copy_push(GUEST_EA(port), GUEST_EA(source), descriptor_size, copy_size, tag, flags);
}

s32 cellSpursJobQueuePort2PushSync(void* port, u32 tag_mask, u32 flags)
{
    return jq2_port_push_sync(GUEST_EA(port), tag_mask, flags);
}

s32 cellSpursJobQueuePort2Sync(void* port, u32 mode)
{
    return jq2_port_sync(GUEST_EA(port), mode);
}

/*
 * ps3recomp - the spu_context census (docs/SPU_XDIFF.md section 9)
 *
 * A shadow context is built from a fixture, never copied from a live one, so
 * every member of spu_context a job's entry state sets must be carried by the
 * fixture. This table states that per member and tools/test_spu_context_census.py
 * compares it with the compiler's record layout: a member added to the struct
 * fails the test until it has a class here.
 *
 * The entry state is spu_context_init, then image_id, job_rt_ls, job_rt_ls2,
 * the local store, the registers and pc (spurs_job.c:939-984). Everything else
 * is zero then, which is why a context built by spu_context_init and given the
 * fixture's registers, store, pc and job_rt_ls{,_2} matches the authority.
 * image_id is the one exception: it is set at entry and has no fixture field,
 * so section 9 has the replay derive it (the registered id, or -1 for T0).
 */
#include "spu_xdiff_census.h"

#include "../spu_context.h"

#include <stddef.h>

/* name, class; the offset is the compiler's, never a number. */
#define M(name, klass) { #name, (uint32_t)offsetof(spu_context, name), SPU_XDIFF_MEMBER_##klass }

static const spu_xdiff_member s_members[] = {
    M(gpr,                    CARRIED), /* REG0: registers 0..127, 16 bytes each */
    M(ls_store,               CARRIED), /* LS_0: the entry store, stored as extents over zero */
    M(ls,                     HOST),    /* the store pointer; init points it at ls_store */
    M(pc,                     CARRIED), /* UNIT.entry_pc; a branch elsewhere is found by the transcript */
    M(status,                 ZERO),    /* init leaves SPU_STATUS_STOPPED (0) */
    M(stop_code,              ZERO),    /* only a stop sets it, and a stop is in the transcript's end */
    M(spu_id,                 HOST),    /* host thread identity from init's argument; no job reads it */
    M(spu_group_id,           HOST),    /* host thread identity; no job reads it */
    M(image_id,               HOST),    /* set at entry (spurs_job.c:942) but not carried: section 9 derives it from IDEN.authority */
    M(park_on_empty_inmbox,   ZERO),    /* a raw-SPU driver knob; jobs run with 0 */
    M(decrementer,            ZERO),    /* channel state; its reload and every RdDec value are in the transcript */
    M(dec_base_ns,            ZERO),    /* host-time stamp of the reload above */
    M(srr0,                   ZERO),    /* written only by an interrupt, which a served context never takes */
    M(event_status,           ZERO),    /* raised by writes the transcript holds, in that order */
    M(event_mask,             ZERO),    /* a WrEventMask write is an effect, so every change is recorded */
    M(ch_out_mbox,            ZERO),    /* mailbox state follows from the transcript's writes and reads */
    M(ch_in_mbox,             ZERO),
    M(rcv_evt,                ZERO),    /* sys_spu_thread_receive_event reply, set by a host call */
    M(rcv_evt_n,              ZERO),
    M(rcv_evt_i,              ZERO),
    M(ch_out_intr_mbox,       ZERO),
    M(ch_sig_notify,          ZERO),
    M(mfc_lsa,                ZERO),    /* the five MFC latches: zero at entry; their values reach the transcript in the command that reads them */
    M(mfc_eah,                ZERO),
    M(mfc_eal,                ZERO),
    M(mfc_size,               ZERO),
    M(mfc_tag,                ZERO),
    M(mfc_tag_mask,           ZERO),    /* a WrTagMask write is an effect; its value is in the transcript */
    M(mfc_tag_status,         ZERO),    /* follows from the transfers the transcript holds */
    M(resv_ea,                ZERO),    /* reservation state follows from the atomics in the transcript */
    M(resv_valid,             ZERO),
    M(atomic_stat,            ZERO),    /* the last atomic's status; the atomics are in the transcript */
    M(dbg_getllar,            HOST),    /* SPU_PUTLLC_WHY diagnostic counter; no job reads it */
    M(resv_line,              ZERO),    /* GETLLAR's snapshot; the same line arrives as an XFER payload */
    M(resv_epoch,             ZERO),
    M(policy_mode,            ZERO),    /* only a policy-module run sets it; a job enters with 0 */
    M(resident_ovl,           ZERO),    /* dispatch bookkeeping, zero at entry */
    M(resident_code,          ZERO),    /* dispatch bookkeeping, zero at entry */
    M(reloc_delta,            ZERO),    /* dispatch bookkeeping, zero at entry */
    M(dcache_id,              ZERO),    /* dispatch cache, zero at entry (spu_context.h:346-352) */
    M(dcache_residency,       ZERO),
    M(resident_task,          ZERO),    /* dispatch bookkeeping, zero at entry */
    M(host_depth,             ZERO),    /* lifted call-depth counter, zero at entry */
    M(steps,                  ZERO),    /* trampoline step counter, zero at entry */
    M(module_img_a00,         ZERO),    /* drain model image id, zero at entry */
    M(lockstep_quantum_ctr,   ZERO),    /* lockstep is off unless the host arms it; zero at entry */
    M(lockstep_release_tb,    ZERO),
    M(dec_start_tb,           ZERO),
    M(ch_wait_cv,             HOST),    /* host wait handle; a served context never blocks */
    M(ch_wait_lock,           HOST),
    M(int_enable,             ZERO),    /* 0 at entry; the runtime sets it from bie/bid, and a built context starts at 0 */
    M(irq_frame,              HOST),    /* host frame pointer of an interrupt, never taken by a served context */
    M(list_stall_mask,        ZERO),    /* no list is parked at entry; stalls are in the transcript */
    M(list_stall_stat,        ZERO),
    M(list_stall_elem_lsa,    ZERO),
    M(list_stall_remaining,   ZERO),
    M(list_stall_dest_lsa,    ZERO),
    M(list_stall_ea_base,     ZERO),
    M(list_stall_cmd,         ZERO),
    M(irq_saved,              ZERO),    /* interrupt save slot, zero at entry */
    M(irq_resume_pc,          ZERO),
    M(irq_gpr,                ZERO),
    M(drain_ret_pc,           ZERO),    /* interpreter return point, zero at entry */
    M(observer,               HOST),    /* the shadow driver installs its own; init leaves NULL */
    M(port,                   HOST),    /* the replay installs its serve table; init leaves NULL */
    M(lift_miss,              ZERO),    /* the closed resolver counts branches; a built context starts at 0 */
    M(getllar_spin_ea,        HOST),    /* the GETLLAR spin detector (spu_mfc_atomic): a diagnostic, never reached by a served context */
    M(getllar_spin_n,         HOST),
    M(job_rt_ls,              CARRIED), /* UNIT.job_rt_ls (spurs_job.c:943) */
    M(job_rt_ls2,             CARRIED), /* UNIT.job_rt_ls2 (spurs_job.c:944) */
    M(stop_request,           HOST),    /* written by another host thread; init clears it */
    M(coh_listed,             HOST),    /* coherency registration, owned by the host; a built context is not listed */
};

#undef M

const spu_xdiff_member* spu_xdiff_context_members(size_t* count)
{
    if (count) *count = sizeof s_members / sizeof s_members[0];
    return s_members;
}

/* FNV-1a-32 over one integer, low byte first: the census is a number a fixture
 * stores and another host compares, so it must not depend on the host's byte
 * order (every toolkit host is little-endian, but the value is written down). */
static uint32_t census_u32(uint32_t h, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

uint32_t spu_xdiff_context_census(void)
{
    size_t n = 0;
    const spu_xdiff_member* m = spu_xdiff_context_members(&n);
    uint32_t h = 2166136261u; /* FNV-1a offset basis */
    h = census_u32(h, (uint32_t)sizeof(spu_context));
    for (size_t i = 0; i < n; i++) {
        h = census_u32(h, m[i].offset);
        h = census_u32(h, m[i].klass);
    }
    return h;
}

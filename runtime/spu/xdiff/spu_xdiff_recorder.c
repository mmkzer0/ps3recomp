/*
 * ps3recomp - recording one job into an xdiff fixture (spu_xdiff_recorder.h)
 *
 * A tap on the effect port: every callback appends one transcript record, in
 * arrival order, and takes payload bytes from the context's own local store
 * (docs/SPU_XDIFF.md sections 6 and 7.5). Nothing here reads guest memory or
 * walks a list: what is recorded is what the runtime did.
 *
 * The first thing that cannot be recorded stops recording (nothing after it is
 * appended) and is returned by spu_xdiff_recorder_end; the run itself goes on,
 * because a tap cannot change it and an audit must never cost the guest its job.
 */
#include "spu_xdiff_recorder.h"
#include "spu_xdiff_census.h"
#include "../spu_context.h"
#include "../spu_workload.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* spu_xdiff.h repeats SPU_LS_SIZE as SPU_XDIFF_LS_BYTES (it is included where spu_context.h is not);
 * this file includes both. Registers are the file's 128 x 16 bytes. */
static_assert(SPU_XDIFF_LS_BYTES == SPU_LS_SIZE, "SPU_XDIFF_LS_BYTES is the local-store size");
static_assert(sizeof(((spu_context*)0)->gpr) == SPU_XDIFF_REG_BYTES, "the fixture holds all 128 registers");
static_assert(sizeof(((spu_context*)0)->gpr[0]._u32) == 16, "a register is four words");

#if defined(__aarch64__) || defined(_M_ARM64)
#  define XD_HOST_ARCH SPU_XDIFF_HOST_ARM64
#elif defined(__x86_64__) || defined(_M_X64)
#  define XD_HOST_ARCH SPU_XDIFF_HOST_X86_64
#else
#  error "spu_xdiff_recorder: no fixture host_arch value for this target"
#endif

/* The helper paths this build compiled, from the conditions spu_helpers.h and spu_context.h select
 * them by (NEON: spu_ls_read128/write128 always, the helpers unless a scalar switch is set; SSSE3:
 * SPU_LS_VEC; SSE4.1: SPU_X86 and the pshufb shufb; AVX2: the float and estimate paths). FMA is not
 * a helper selector: no spu path is chosen by it, so the bit records only that the compiler was
 * allowed to emit fused multiply-adds for the libm fma() the helpers call. */
static uint32_t host_isa(void)
{
    uint32_t isa = 0;
#if defined(__ARM_NEON)
    isa |= SPU_XDIFF_ISA_NEON;
#endif
#if defined(SPU_LS_VEC) && SPU_LS_VEC
    isa |= SPU_XDIFF_ISA_SSSE3;
#endif
#if !defined(__ARM_NEON) && defined(__SSE4_1__) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
    isa |= SPU_XDIFF_ISA_SSE41;
#endif
#if !defined(__ARM_NEON) && defined(__AVX2__) && !defined(SPU_SCALAR_HELPERS)
    isa |= SPU_XDIFF_ISA_AVX2;
#endif
#if defined(__FMA__)
    isa |= SPU_XDIFF_ISA_FMA;
#endif
#if defined(SPU_SCALAR_HELPERS)
    isa |= SPU_XDIFF_ISA_SCALAR;
#endif
    return isa;
}

/* ---- failure ---------------------------------------------------------------- */

/* The first failure is the verdict; later ones would only describe the consequences of it. */
static void rec_fail(spu_xdiff_recorder* r, uint32_t reason, const char* fmt, ...)
{
    va_list ap;
    if (r->failed != (uint32_t)SPU_XDIFF_R_NONE) return;
    r->failed = reason;
    r->error.reason = reason;
    va_start(ap, fmt);
    vsnprintf(r->error.detail, sizeof r->error.detail, fmt, ap);
    va_end(ap);
}

/* ---- records ---------------------------------------------------------------- */

static spu_xdiff_effect rec_new(uint32_t kind, const spu_context* ctx)
{
    spu_xdiff_effect e;
    memset(&e, 0, sizeof e);   /* every unused field is 0 (7.5), reserved included */
    e.kind = (uint8_t)kind;
    e.pc = (uint32_t)ctx->pc;
    e.blob = SPU_XDIFF_NO_BLOB;
    return e;
}

static void rec_append(spu_xdiff_recorder* r, const spu_xdiff_effect* e)
{
    if (spu_xdiff_fixture_add_effect(r->fixture, e) != 0)
        rec_fail(r, SPU_XDIFF_R_CAPTURE_LIMIT, "transcript record %u (kind %u): out of memory or SPU_XDIFF_MAX_EFFECTS",
                 r->fixture->effect_count, (unsigned)e->kind);
}

/* Stores the `n` bytes of local store at `lsa` as one payload. The span is checked whole, in 64 bits,
 * before the first byte is read: the runtime stops on a transfer past the end of local store
 * (spu_dma.h, "ls-overrun") and never wraps one, so a span that leaves the store is not something a
 * real run reports; a serve table could, and this is where it is named. A list's elements do wrap in
 * the runtime (mfc_run_list masks each element address); the format records one contiguous span, so
 * a list that wraps is refused here instead of being recorded as something it is not. */
static int rec_payload(spu_xdiff_recorder* r, const spu_context* ctx, uint64_t lsa, uint64_t n, const char* what,
                       uint32_t* blob)
{
    uint32_t off;
    *blob = SPU_XDIFF_NO_BLOB;
    if (n == 0) return 0;
    if (lsa + n > (uint64_t)SPU_LS_SIZE) {
        rec_fail(r, SPU_XDIFF_R_UNSUPPORTED_EFFECT, "record %u: %s spans 0x%llX+%llu, past local store (or wraps it)",
                 r->fixture->effect_count, what, (unsigned long long)lsa, (unsigned long long)n);
        return -1;
    }
    off = spu_xdiff_fixture_add_blob(r->fixture, ctx->ls + (uint32_t)lsa, (uint32_t)n, r->max_blob_bytes);
    if (off == SPU_XDIFF_NO_BLOB) {   /* n > 0: nothing was stored, so memory or the payload bound ran out */
        rec_fail(r, SPU_XDIFF_R_CAPTURE_LIMIT, "record %u: the %s payload of %llu bytes does not fit (blob %u of %u bytes)",
                 r->fixture->effect_count, what, (unsigned long long)n, r->fixture->blob_bytes,
                 r->max_blob_bytes ? r->max_blob_bytes : (unsigned)SPU_XDIFF_MAX_BLOB_BYTES);
        return -1;
    }
    *blob = off;
    return 0;
}

/* spu_xdiff.h owns the rule, so the recorder, the decoder and the replay cannot disagree on it. */
static int is_list_command(uint32_t cmd)
{
    return spu_xdiff_cmd_is_list(cmd);
}

/* ---- the tap ---------------------------------------------------------------- */

static int tap_ok(spu_xdiff_recorder* r, const spu_context* ctx)
{
    if (r->failed != (uint32_t)SPU_XDIFF_R_NONE) return 0;
    if (ctx != r->ctx) {
        rec_fail(r, SPU_XDIFF_R_UNSUPPORTED_EFFECT, "a tap callback arrived from context %p; the recorder serves %p",
                 (const void*)ctx, (const void*)r->ctx);
        return 0;
    }
    return 1;
}

static void tap_wrch(void* user, const spu_context* ctx, uint32_t channel, uint32_t value)
{
    spu_xdiff_recorder* r = (spu_xdiff_recorder*)user;
    spu_xdiff_effect e;
    if (!tap_ok(r, ctx)) return;
    if (channel > 0xFFu) {
        rec_fail(r, SPU_XDIFF_R_UNSUPPORTED_EFFECT, "write of channel %u: the record holds the channel in a byte", channel);
        return;
    }
    e = rec_new(SPU_FX_WRCH, ctx);
    e.channel = (uint8_t)channel;
    e.value = value;

    if (channel == (uint32_t)MFC_Cmd) {
        /* The request is the command word and the latches it consumes, verbatim: lsa and size are latch
         * values here, not a local-store range (a GETLLAR after a stale large MFC_Size is an ordinary
         * record). Only a list command's payload reads local store, and that span is checked. The tap
         * hears the write before it acts, so the latches are exactly what the command will read. */
        e.kind = SPU_FX_MFC_CMD;
        e.tag = (uint8_t)(ctx->mfc_tag & 0x1Fu);
        e.ea = ((uint64_t)ctx->mfc_eah << 32) | ctx->mfc_eal;
        e.lsa = ctx->mfc_lsa;
        e.size = ctx->mfc_size;
        if (is_list_command(value) &&
            rec_payload(r, ctx, ctx->mfc_eal & SPU_LS_MASK, e.size, "list", &e.blob) != 0)   /* EAL is the list's LS address */
            return;
    } else if (channel == (uint32_t)MFC_WrListStallAck) {
        /* A parked list is resumed from local store as it is NOW: the guest's stall handler may have
         * rewritten the elements (7.5), so they are read before the write acts. The runtime keys the
         * parked list by the low five bits of the acknowledged value (mfc_list_stall_ack). */
        const uint32_t t = value & 0x1Fu;
        if (ctx->list_stall_mask & (1u << t)) {
            const uint32_t elem = ctx->list_stall_elem_lsa[t] & SPU_LS_MASK;
            const uint64_t bytes = (uint64_t)ctx->list_stall_remaining[t] * 8u;
            if (bytes > (uint64_t)SPU_LS_SIZE) {
                rec_fail(r, SPU_XDIFF_R_UNSUPPORTED_EFFECT, "record %u: tag %u parks %u list elements, more than local store holds",
                         r->fixture->effect_count, t, ctx->list_stall_remaining[t]);
                return;
            }
            if (bytes != 0) {   /* a list parked on its last element has nothing left: lsa 0, size 0, no payload (7.5) */
                e.lsa = elem;
                e.size = (uint32_t)bytes;
                if (rec_payload(r, ctx, elem, bytes, "parked list", &e.blob) != 0) return;
            }
        }
    }
    rec_append(r, &e);
}

static void tap_read(spu_xdiff_recorder* r, const spu_context* ctx, uint32_t kind, uint32_t channel, uint32_t value)
{
    spu_xdiff_effect e;
    if (!tap_ok(r, ctx)) return;
    if (channel > 0xFFu) {
        rec_fail(r, SPU_XDIFF_R_UNSUPPORTED_EFFECT, "read of channel %u: the record holds the channel in a byte", channel);
        return;
    }
    e = rec_new(kind, ctx);
    e.channel = (uint8_t)channel;
    e.value = value;
    rec_append(r, &e);
}

static void tap_rdch(void* user, const spu_context* ctx, uint32_t channel, uint32_t value)
{
    tap_read((spu_xdiff_recorder*)user, ctx, SPU_FX_RDCH, channel, value);
}

static void tap_rchcnt(void* user, const spu_context* ctx, uint32_t channel, uint32_t count)
{
    tap_read((spu_xdiff_recorder*)user, ctx, SPU_FX_RCHCNT, channel, count);
}

static void tap_xfer(void* user, const spu_context* ctx, const spu_port_xfer* x)
{
    spu_xdiff_recorder* r = (spu_xdiff_recorder*)user;
    spu_xdiff_effect e;
    if (!tap_ok(r, ctx)) return;
    if (x->tag > 31u) {
        rec_fail(r, SPU_XDIFF_R_UNSUPPORTED_EFFECT, "record %u: transfer in tag group %u", r->fixture->effect_count, x->tag);
        return;
    }
    e = rec_new(SPU_FX_XFER, ctx);
    e.tag = (uint8_t)x->tag;
    e.dir = (uint8_t)x->dir;
    e.value = x->cmd;   /* the opcode, list bit already clear (spu_port_xfer) */
    e.status = x->status;
    e.ea = x->ea;
    e.size = x->size;
    switch (x->dir) {
    case SPU_XFER_TO_LS:     /* the bytes have landed at lsa */
    case SPU_XFER_FROM_LS:   /* the source is still intact at lsa */
        e.lsa = x->lsa;
        if (rec_payload(r, ctx, x->lsa, x->size, x->dir == SPU_XFER_TO_LS ? "transfer to local store" : "transfer from local store",
                        &e.blob) != 0)
            return;
        break;
    case SPU_XFER_ZERO_EA:   /* SDCRZ: no local-store operand, so no lsa and no payload */
        break;
    default:
        rec_fail(r, SPU_XDIFF_R_UNSUPPORTED_EFFECT, "record %u: transfer direction %u has no record", r->fixture->effect_count, x->dir);
        return;
    }
    rec_append(r, &e);
}

/* ---- the registers as the file holds them ------------------------------------ */

/* Byte k of register r is the byte a `stqd` of r stores at offset k: word i of the host register is
 * stored big-endian at byte 4i, which is the conversion spu_ls_write128 makes (spu_context.h). */
static void regs_to_guest(uint8_t out[SPU_XDIFF_REG_BYTES], const spu_context* ctx)
{
    for (uint32_t reg = 0; reg < 128u; reg++)
        for (uint32_t i = 0; i < 4u; i++) {
            const uint32_t w = ctx->gpr[reg]._u32[i];
            uint8_t* p = out + reg * 16u + i * 4u;
            p[0] = (uint8_t)(w >> 24);
            p[1] = (uint8_t)(w >> 16);
            p[2] = (uint8_t)(w >> 8);
            p[3] = (uint8_t)w;
        }
}

/* ---- begin / end ------------------------------------------------------------- */

static int refuse(spu_xdiff_recorder* r, uint32_t reason, const char* fmt, ...)
{
    va_list ap;
    r->failed = reason;
    r->error.reason = reason;
    va_start(ap, fmt);
    vsnprintf(r->error.detail, sizeof r->error.detail, fmt, ap);
    va_end(ap);
    return -1;
}

int spu_xdiff_recorder_begin(spu_xdiff_recorder* r, spu_xdiff_fixture* f, spu_context* ctx, const spu_xdiff_unit* unit,
                             const spu_xdiff_body_id* authority, const spu_effect_serve* serve, void* serve_user,
                             uint32_t max_blob_bytes)
{
    if (!r) return -1;
    memset(r, 0, sizeof *r);
    /* r->failed stays set when begin refuses, so an end() on a recorder that never started cannot answer NONE. */
    if (!f || !ctx || !unit || !authority)
        return refuse(r, SPU_XDIFF_R_FIXTURE_FORMAT, "begin: a required argument is NULL");
    if (!f->ls0 || !f->ls1)
        return refuse(r, SPU_XDIFF_R_FIXTURE_FORMAT, "begin: the fixture is not initialised (spu_xdiff_fixture_init)");
    if (ctx->port != NULL)
        return refuse(r, SPU_XDIFF_R_UNSUPPORTED_EFFECT,
                      "begin: context %p already has an effect port; a recorder is the only tap it can carry", (void*)ctx);
    if (unit->kind != (uint32_t)SPU_XDIFF_UNIT_JOB ||
        (unit->profile != (uint32_t)SPU_XDIFF_PROFILE_JOBCHAIN && unit->profile != (uint32_t)SPU_XDIFF_PROFILE_JQ2))
        return refuse(r, SPU_XDIFF_R_FIXTURE_FORMAT, "begin: unit kind %u profile %u is not one a fixture describes", unit->kind,
                      unit->profile);
    /* The entry state the fixture claims is the entry state the context is in: a recorder started late
     * would write a fixture whose LS_0 is not where the transcript begins. */
    if (ctx->pc != unit->entry_pc)
        return refuse(r, SPU_XDIFF_R_FIXTURE_FORMAT, "begin: context pc 0x%X is not the unit's entry_pc 0x%X (not at its entry state)",
                      (unsigned)ctx->pc, unit->entry_pc);
    if ((uint64_t)unit->image_ls + unit->image_bytes > (uint64_t)SPU_LS_SIZE)
        return refuse(r, SPU_XDIFF_R_FIXTURE_FORMAT, "begin: image [0x%X, +%u) is outside local store", unit->image_ls,
                      unit->image_bytes);

    spu_xdiff_fixture_clear(f);
    r->fixture = f;
    r->ctx = ctx;
    r->max_blob_bytes = max_blob_bytes;

    f->identity.image_fingerprint = spu_workload_fingerprint(ctx->ls + unit->image_ls, unit->image_bytes);
    f->identity.authority = *authority;
    f->identity.effect_port_abi = SPU_EFFECT_PORT_ABI;
    f->identity.context_census = spu_xdiff_context_census();
    f->identity.host_arch = (uint32_t)XD_HOST_ARCH;
    f->identity.host_isa = host_isa();
    /* toolkit_commit stays zero (unknown: the build carries no commit define), toolkit_dirty 0. */
    f->identity.float_rz = (uint8_t)(spu_float_rz_mode() ? 1 : 0);
    f->unit = *unit;
    regs_to_guest(f->reg0, ctx);
    memcpy(f->ls0, ctx->ls, SPU_LS_SIZE);

    r->tap.wrch = tap_wrch;
    r->tap.rdch = tap_rdch;
    r->tap.rchcnt = tap_rchcnt;
    r->tap.xfer = tap_xfer;
    r->port.serve = serve;
    r->port.serve_user = serve_user;
    r->port.tap = &r->tap;
    r->port.tap_user = r;
    spu_port_set(ctx, &r->port);   /* aborts on a serve table with a missing member: a programming error */
    return 0;
}

uint32_t spu_xdiff_recorder_end(spu_xdiff_recorder* r, spu_context* ctx, uint32_t end_class, uint32_t stop_code)
{
    spu_xdiff_fixture* f;
    if (!r) return (uint32_t)SPU_XDIFF_R_FIXTURE_FORMAT;
    if (!r->ctx) {   /* begin refused, or end() already ran: nothing is installed and nothing is complete */
        if (r->failed == (uint32_t)SPU_XDIFF_R_NONE)
            rec_fail(r, SPU_XDIFF_R_FIXTURE_FORMAT, "end: the recorder is not recording");
        return r->failed;
    }
    if (ctx != r->ctx) {
        /* Leave the recorder's own context alone: its port is still ours to remove, and the caller named another. */
        rec_fail(r, SPU_XDIFF_R_FIXTURE_FORMAT, "end: context %p is not the recorded context %p", (void*)ctx, (void*)r->ctx);
        return r->failed;
    }
    spu_port_set(ctx, NULL);
    f = r->fixture;
    r->ctx = NULL;

    if (end_class < (uint32_t)SPU_XDIFF_END_RETURN || end_class > (uint32_t)SPU_XDIFF_END_OTHER)
        rec_fail(r, SPU_XDIFF_R_FIXTURE_FORMAT, "end: end class %u is not one of the five of the END_ section", end_class);

    regs_to_guest(f->reg1, ctx);
    memcpy(f->ls1, ctx->ls, SPU_LS_SIZE);
    /* A fixture that did not record completely must not encode: class 0 is outside the five END_ classes,
     * which the encoder and the decoder both refuse. */
    f->end.end_class = r->failed == (uint32_t)SPU_XDIFF_R_NONE ? end_class : 0;
    f->end.stop_code = stop_code;
    f->end.pc = (uint32_t)ctx->pc & SPU_LS_MASK;
    f->end.status = (uint32_t)ctx->status;
    return r->failed;
}

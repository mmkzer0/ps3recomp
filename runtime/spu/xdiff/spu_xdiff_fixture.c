/*
 * ps3recomp - the xdiff fixture: in-memory form, payload store, encoder and
 * validating decoder (docs/SPU_XDIFF.md section 7; layouts in spu_xdiff.h)
 *
 * A fixture file is untrusted input: it is written by one run, copied around
 * and read by another, and may be truncated or corrupt. So the decoder checks
 * everything section 7.8 lists over the whole span of every offset before it
 * trusts one, and the encoder runs the same checks over the bytes it built
 * before it returns them: a fixture this module writes is one it reads.
 *
 * Two things keep a fixture small without changing the format: local stores
 * are stored as sorted extents that a reader lays over a zero store (LS_0) or
 * over the entry store (LS_1), and payloads are deduplicated inside one
 * fixture by a table keyed by length and FNV-1a-64 and confirmed by comparing
 * the bytes (section 7.2).
 */
#include "spu_xdiff_fixture.h"

#include "../spu_context.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#define XD_GETPID _getpid
#define XD_OPEN_EXCL(p) _open((p), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE)
#define XD_FDOPEN(fd) _fdopen((fd), "wb")
#define XD_CLOSE _close
#else
#include <fcntl.h>
#include <unistd.h>
#define XD_GETPID getpid
#define XD_OPEN_EXCL(p) open((p), O_WRONLY | O_CREAT | O_EXCL, 0666)
#define XD_FDOPEN(fd) fdopen((fd), "wb")
#define XD_CLOSE close
#endif

/* The fixture's fixed-width records are the context's, and a drift is a format
 * change, not a build detail. */
static_assert(SPU_XDIFF_LS_BYTES == SPU_LS_SIZE,
              "SPU_XDIFF_LS_BYTES must be the context's local store");
static_assert(SPU_XDIFF_REG_BYTES == sizeof(((spu_context*)0)->gpr),
              "SPU_XDIFF_REG_BYTES must be the context's register file");

/* ---- the file's sections ------------------------------------------------ */

enum {
    SEC_IDEN = 0, SEC_UNIT, SEC_REG0, SEC_LS0, SEC_EFCT, SEC_BLOB,
    SEC_REG1, SEC_LS1, SEC_END, SEC_FMASK, SEC_VRDT, SEC_KINDS
};

typedef struct section_kind {
    uint32_t    tag;
    const char* name;
} section_kind;

static const section_kind s_kind[SEC_KINDS] = {
    { SPU_XDIFF_SEC_IDENTITY, "IDEN" },
    { SPU_XDIFF_SEC_UNIT,     "UNIT" },
    { SPU_XDIFF_SEC_REG0,     "REG0" },
    { SPU_XDIFF_SEC_LS0,      "LS_0" },
    { SPU_XDIFF_SEC_EFFECTS,  "EFCT" },
    { SPU_XDIFF_SEC_BLOB,     "BLOB" },
    { SPU_XDIFF_SEC_REG1,     "REG1" },
    { SPU_XDIFF_SEC_LS1,      "LS_1" },
    { SPU_XDIFF_SEC_END,      "END_" },
    { SPU_XDIFF_SEC_FMASK,    "FMAS" },
    { SPU_XDIFF_SEC_VERDICT,  "VRDT" },
};

static int section_kind_of(uint32_t tag)
{
    for (int k = 0; k < SEC_KINDS; k++)
        if (s_kind[k].tag == tag) return k;
    return -1;
}

static void fourcc_text(uint32_t tag, char out[5])
{
    for (int i = 0; i < 4; i++) {
        const uint8_t c = (uint8_t)(tag >> (8 * i));
        out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    out[4] = 0;
}

/* ---- errors ------------------------------------------------------------- */

static int fail(spu_xdiff_error* err, uint32_t reason, const char* fmt, ...)
{
    if (err) {
        va_list ap;
        err->reason = reason;
        va_start(ap, fmt);
        vsnprintf(err->detail, sizeof err->detail, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* ---- FNV-1a-64 ---------------------------------------------------------- */

/* The algorithm of spu_workload_fingerprint (spu_workload.c:56-65), used for
 * both the file checksum and the image identity so a fixture's fingerprint is
 * the one the recorder computed. Incremental because the checksum field is
 * read as zero (spu_xdiff.h, spu_xdiff_file_header.checksum). */
#define FNV64_BASIS 1469598103934665603ULL
#define FNV64_PRIME 1099511628211ULL

static uint64_t fnv1a64(uint64_t h, const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= FNV64_PRIME;
    }
    return h;
}

/* ---- field access on bytes ---------------------------------------------- */

/* The file is little-endian and so is every host that compiles this (the
 * #error in spu_xdiff.h), so a field is its bytes. memcpy because a section
 * need not be aligned for the caller's buffer. */
static uint32_t rd_u32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static uint64_t rd_u64(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static uint64_t align8(uint64_t v)
{
    return (v + 7u) & ~(uint64_t)7u;
}

/* ---- effect records ----------------------------------------------------- */

static const char* effect_kind_name(uint32_t kind)
{
    switch (kind) {
    case SPU_FX_WRCH:    return "WRCH";
    case SPU_FX_RDCH:    return "RDCH";
    case SPU_FX_RCHCNT:  return "RCHCNT";
    case SPU_FX_MFC_CMD: return "MFC_CMD";
    case SPU_FX_XFER:    return "XFER";
    default:             return "unknown";
    }
}

/* A payload is `size` bytes at offset `blob` (spu_xdiff.h); a record with no
 * payload carries SPU_XDIFF_NO_BLOB and size 0, which is what the payload
 * store answers for a zero-byte payload too. */
static int check_payload(const spu_xdiff_effect* e, uint32_t i, uint64_t blob_bytes,
                         spu_xdiff_error* err)
{
    if (e->blob == SPU_XDIFF_NO_BLOB)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "EFCT: record %u: %u payload bytes with no payload (blob is none)", i, e->size);
    if (e->size == 0)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "EFCT: record %u: blob 0x%x with size 0", i, (unsigned)e->blob);
    if ((uint64_t)e->blob + e->size > blob_bytes)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "EFCT: record %u: blob 0x%x + %u bytes is outside BLOB's %llu bytes",
                    i, (unsigned)e->blob, e->size, (unsigned long long)blob_bytes);
    return 0;
}

/* What the record before was, which decides whether a transfer may follow: a
 * transfer belongs to the write that caused it, so it comes straight after
 * that MFC_CMD, after another transfer of the same command, or after the
 * MFC_WrListStallAck that resumes a parked list (sections 6 and 7.8). */
typedef enum effect_after {
    AFTER_NOTHING = 0,
    AFTER_WRCH,
    AFTER_STALL_ACK,
    AFTER_MFC_CMD,
    AFTER_XFER,
    AFTER_READ
} effect_after;

static const char* after_name(effect_after after)
{
    switch (after) {
    case AFTER_WRCH:      return "a WRCH";
    case AFTER_STALL_ACK: return "an MFC_WrListStallAck";
    case AFTER_MFC_CMD:   return "an MFC_CMD";
    case AFTER_XFER:      return "an XFER";
    case AFTER_READ:      return "a channel read";
    default:              return "nothing";
    }
}

/* One record against section 7.5's columns: a field the kind's column leaves
 * empty is 0 (`blob` is none), and every span a record names is inside the
 * section it names it in. `after` answers section 7.8's "an XFER has no
 * command before it". */
static int check_effect(const spu_xdiff_effect* e, uint32_t i, uint64_t blob_bytes,
                        effect_after after, spu_xdiff_error* err)
{
    const char* kn = effect_kind_name(e->kind);

    if (e->kind < SPU_FX_WRCH || e->kind > SPU_FX_XFER)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "EFCT: record %u: kind %u is not an effect kind", i, (unsigned)e->kind);
    if (e->reserved)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "EFCT: record %u: reserved is %u, unused", i, (unsigned)e->reserved);

#define UNUSED(field)                                                                     \
    do {                                                                                  \
        if (e->field)                                                                     \
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,                                  \
                        "EFCT: record %u: %s is %llu, unused for %s", i, #field,          \
                        (unsigned long long)e->field, kn);                                \
    } while (0)

    switch (e->kind) {
    case SPU_FX_WRCH:
        /* A WRCH of MFC_WrListStallAck with a list parked on its tag carries the
         * elements left, at the next element's address (section 6); no other
         * channel write has a local-store span or a payload. */
        UNUSED(tag);
        UNUSED(dir);
        UNUSED(status);
        UNUSED(ea);
        if (e->size == 0) {
            if (e->lsa)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "EFCT: record %u: lsa 0x%x with size 0", i, (unsigned)e->lsa);
            if (e->blob != SPU_XDIFF_NO_BLOB)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "EFCT: record %u: blob 0x%x with size 0", i, (unsigned)e->blob);
        } else {
            if (e->channel != (uint8_t)MFC_WrListStallAck)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "EFCT: record %u: a WRCH of channel %u carries %u payload bytes; "
                            "only MFC_WrListStallAck (26) does",
                            i, (unsigned)e->channel, e->size);
            if (check_payload(e, i, blob_bytes, err)) return -1;
            if ((uint64_t)e->lsa + e->size > SPU_XDIFF_LS_BYTES)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "EFCT: record %u: lsa 0x%x + %u bytes is outside local store",
                            i, (unsigned)e->lsa, e->size);
        }
        break;

    case SPU_FX_RDCH:
    case SPU_FX_RCHCNT:
        UNUSED(tag);
        UNUSED(dir);
        UNUSED(status);
        UNUSED(ea);
        if (e->lsa || e->size)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "EFCT: record %u: lsa/size are %u/%u, unused for %s",
                        i, (unsigned)e->lsa, e->size, kn);
        if (e->blob != SPU_XDIFF_NO_BLOB)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "EFCT: record %u: blob 0x%x, unused for %s", i, (unsigned)e->blob, kn);
        break;

    case SPU_FX_MFC_CMD:
        /* The latches are recorded verbatim: an atomic or a barrier ignores
         * MFC_Size, so a stale latch is legal and is not a span (ruling on
         * section 7.8). The request a list command adds is its staged list,
         * whose byte length is MFC_Size (spu_dma.h, mfc_is_list); an atomic or
         * a barrier keeps bit 2 and is not a list (spu_xdiff_cmd_is_list). */
        if (e->channel != (uint8_t)MFC_Cmd)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "EFCT: record %u: channel is %u, MFC_Cmd is %u", i, (unsigned)e->channel,
                        (unsigned)MFC_Cmd);
        if (e->tag > 31)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "EFCT: record %u: tag %u is not a tag group (0..31)", i, (unsigned)e->tag);
        UNUSED(dir);
        UNUSED(status);
        if (spu_xdiff_cmd_is_list(e->value)) {
            if (e->size == 0) {
                if (e->blob != SPU_XDIFF_NO_BLOB)
                    return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                                "EFCT: record %u: blob 0x%x with size 0", i, (unsigned)e->blob);
            } else {
                if (e->blob == SPU_XDIFF_NO_BLOB)
                    return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                                "EFCT: record %u: list command of %u bytes with no list payload",
                                i, e->size);
                if (check_payload(e, i, blob_bytes, err)) return -1;
            }
        } else if (e->blob != SPU_XDIFF_NO_BLOB) {
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "EFCT: record %u: blob 0x%x, unused for a plain command",
                        i, (unsigned)e->blob);
        }
        break;

    case SPU_FX_XFER:
        if (e->channel != 0)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "EFCT: record %u: channel is %u, unused for XFER", i, (unsigned)e->channel);
        if (spu_xdiff_cmd_is_list(e->value))
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "EFCT: record %u: opcode 0x%x is a list command, and a transfer reports "
                        "the element's opcode with its list bit clear",
                        i, (unsigned)e->value);
        if (e->tag > 31)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "EFCT: record %u: tag %u is not a tag group (0..31)", i, (unsigned)e->tag);
        if (e->dir < (uint8_t)SPU_XFER_TO_LS || e->dir > (uint8_t)SPU_XFER_ZERO_EA)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "EFCT: record %u: dir %u is not 1 (to LS), 2 (from LS) or 3 (zero EA)",
                        i, (unsigned)e->dir);
        if (after != AFTER_MFC_CMD && after != AFTER_XFER && after != AFTER_STALL_ACK)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "EFCT: record %u: an XFER cannot follow %s; it follows an MFC_CMD, "
                        "another XFER of that command, or an MFC_WrListStallAck",
                        i, after_name(after));
        if (e->dir == (uint8_t)SPU_XFER_ZERO_EA) {
            /* SDCRZ: no local-store operand, so no lsa and no payload. */
            if (e->lsa)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "EFCT: record %u: lsa 0x%x, unused for a zero-EA transfer",
                            i, (unsigned)e->lsa);
            if (e->blob != SPU_XDIFF_NO_BLOB)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "EFCT: record %u: blob 0x%x, unused for a zero-EA transfer",
                            i, (unsigned)e->blob);
            break;
        }
        if (e->size == 0) {
            if (e->blob != SPU_XDIFF_NO_BLOB)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "EFCT: record %u: blob 0x%x with size 0", i, (unsigned)e->blob);
        } else {
            if (e->blob == SPU_XDIFF_NO_BLOB)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "EFCT: record %u: transfer of %u bytes with no payload", i, e->size);
            if (check_payload(e, i, blob_bytes, err)) return -1;
        }
        if ((uint64_t)e->lsa + e->size > SPU_XDIFF_LS_BYTES)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "EFCT: record %u: lsa 0x%x + %u bytes is outside local store",
                        i, (unsigned)e->lsa, e->size);
        break;

    default:
        break; /* unreachable: the range check above rejects every other kind */
    }
#undef UNUSED
    return 0;
}

static int check_effects(const uint8_t* p, uint64_t bytes, uint64_t blob_bytes,
                         spu_xdiff_error* err)
{
    const uint32_t count = (uint32_t)(bytes / sizeof(spu_xdiff_effect));
    effect_after after = AFTER_NOTHING;
    for (uint32_t i = 0; i < count; i++) {
        spu_xdiff_effect e;
        memcpy(&e, p + (size_t)i * sizeof e, sizeof e);
        if (check_effect(&e, i, blob_bytes, after, err)) return -1;
        switch (e.kind) {
        case SPU_FX_WRCH:
            after = e.channel == (uint8_t)MFC_WrListStallAck ? AFTER_STALL_ACK : AFTER_WRCH;
            break;
        case SPU_FX_MFC_CMD: after = AFTER_MFC_CMD; break;
        case SPU_FX_XFER:    after = AFTER_XFER; break;
        default:             after = AFTER_READ; break;
        }
    }
    return 0;
}

/* ---- local-store extents ------------------------------------------------ */

/* An extent is an 8-byte header and its bytes padded to 8 (spu_xdiff.h), so it
 * costs 8 bytes plus padding. Walking the section is the only check of its
 * contents, and it is used twice: once with a NULL store to validate, once to
 * lay the extents over a store. */
static int walk_extents(const char* sec, const uint8_t* p, uint64_t bytes, uint8_t* store,
                        spu_xdiff_error* err)
{
    uint64_t at = 0;
    uint32_t index = 0;
    uint32_t prev_end = 0;
    int have_prev = 0;

    while (at < bytes) {
        if (bytes - at < sizeof(spu_xdiff_ls_extent))
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "%s: extent %u: header runs past the section's %llu bytes",
                        sec, index, (unsigned long long)bytes);
        const uint32_t lsa = rd_u32(p + at);
        const uint32_t len = rd_u32(p + at + 4);
        if (len == 0)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "%s: extent %u: bytes is 0", sec, index);
        if ((uint64_t)lsa + len > SPU_XDIFF_LS_BYTES)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "%s: extent %u: lsa 0x%x + %u bytes is outside local store",
                        sec, index, (unsigned)lsa, len);
        if (have_prev && lsa < prev_end)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "%s: extent %u: lsa 0x%x is before the previous extent's end 0x%x "
                        "(extents are sorted and do not overlap)",
                        sec, index, (unsigned)lsa, (unsigned)prev_end);
        const uint64_t body = at + sizeof(spu_xdiff_ls_extent);
        if (bytes - body < len)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "%s: extent %u: its %u bytes run past the section", sec, index, len);
        const uint64_t padded = align8(len);
        if (bytes - body < padded)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "%s: extent %u: its padding runs past the section", sec, index);
        for (uint64_t k = len; k < padded; k++)
            if (p[body + k])
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "%s: extent %u: padding is not zero", sec, index);
        if (store) memcpy(store + lsa, p + body, len);
        prev_end = lsa + len;
        have_prev = 1;
        at = body + padded;
        index++;
    }
    return 0;
}

/* ---- fixed-record structure --------------------------------------------- */

/* UNIT describes one supported entry state; kind selects which fields apply. */
static int check_unit(const uint8_t* p, spu_xdiff_error* err)
{
    spu_xdiff_unit u;
    memcpy(&u, p, sizeof u);
    if (u.kind == (uint32_t)SPU_XDIFF_UNIT_JOB) {
        if (u.profile != (uint32_t)SPU_XDIFF_PROFILE_JOBCHAIN &&
            u.profile != (uint32_t)SPU_XDIFF_PROFILE_JQ2)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "UNIT: profile is %u, not 1 (jobchain loader) or 2 (JobQueue2 loader)",
                        (unsigned)u.profile);
    } else if (u.kind == (uint32_t)SPU_XDIFF_UNIT_FUNCTION) {
        if (u.profile != 0)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "UNIT: function profile is %u, not 0", (unsigned)u.profile);
        if (u.job_rt_ls || u.job_rt_ls2 || u.image_ea || u.bss_bytes ||
            u.desc_ls || u.desc_bytes || u.io_ls || u.io_bytes || u.out_ls || u.out_bytes ||
            u.scratch_ls || u.scratch_bytes || u.stack_bytes || u.ctx_ls || u.origin_queue ||
            u.origin_worker || u.origin_seq || u.origin_descriptor_ea || u.reserved)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "UNIT: function unit has a nonzero job-only field");
        if (u.r1 > SPU_XDIFF_LS_BYTES)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "UNIT: function entry r1 0x%x is outside local store", (unsigned)u.r1);
        if ((u.return_ls & 3u) || u.return_ls > SPU_XDIFF_LS_BYTES - 4u)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "UNIT: function return PC 0x%x is not a word address in local store",
                        (unsigned)u.return_ls);
    } else {
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "UNIT: kind is %u, not 1 (job) or 2 (function)", (unsigned)u.kind);
    }
    if ((u.entry_pc & 3u) != 0)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "UNIT: entry_pc 0x%x is not 4-byte aligned", (unsigned)u.entry_pc);
    if (u.entry_pc > SPU_XDIFF_LS_BYTES - 4u)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "UNIT: entry_pc 0x%x is outside local store", (unsigned)u.entry_pc);
    if (u.image_bytes == 0)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "UNIT: image_bytes is 0, so the image fingerprint covers no bytes");
    return 0;
}

static int check_function_mask(const uint8_t* p, const spu_xdiff_unit* unit,
                               spu_xdiff_error* err)
{
    spu_xdiff_function_mask mask;
    memcpy(&mask, p, sizeof mask);
    if (mask.reserved)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "FMAS: reserved is %u, not 0", (unsigned)mask.reserved);
    if ((mask.live_out[0] & (1ULL << 1)) == 0)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "FMAS: live_out omits $r1, which the liveness plan always keeps live");
    if (mask.dead_stack_bytes > unit->r1 || mask.dead_stack_bytes > SPU_XDIFF_LS_BYTES)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "FMAS: dead stack extent %u is larger than entry r1 0x%x",
                    mask.dead_stack_bytes, unit->r1);
    return 0;
}

/* END_'s class is what a comparison holds the subject to (section 7.6), so a
 * class outside the vocabulary is a recording that never classified its end. */
static int check_end(const uint8_t* p, spu_xdiff_error* err)
{
    spu_xdiff_end e;
    memcpy(&e, p, sizeof e);
    if (e.end_class < (uint32_t)SPU_XDIFF_END_RETURN ||
        e.end_class > (uint32_t)SPU_XDIFF_END_OTHER)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "END_: end_class is %u, not 1..5 (return, stop, halt, jm-call, other)",
                    (unsigned)e.end_class);
    return 0;
}

/* ---- the container ------------------------------------------------------ */

typedef struct fixture_part {
    int      present;
    uint64_t offset;
    uint64_t bytes;
} fixture_part;

typedef struct fixture_layout {
    fixture_part part[SEC_KINDS];
    uint64_t     table_end;
} fixture_layout;

/* Section 7.8, in the order a reader can answer it without trusting a count or
 * an offset: the header, the checksum (which covers the header), the table's
 * tags and declared sizes, then every span, then the sections' contents. */
static int parse_file(const uint8_t* bytes, size_t n, uint32_t reader_version,
                      fixture_layout* lay, spu_xdiff_error* err)
{
    const size_t hdr_bytes = sizeof(spu_xdiff_file_header);
    const size_t sec_bytes = sizeof(spu_xdiff_section);

    memset(lay, 0, sizeof *lay);

    /* A zstd frame is a fixture wrapped for a corpus; this reader links no
     * decompressor, so it names the wrapper instead of reading its bytes as a
     * header (section 7.2; the corpus driver decodes it). */
    if (n >= 4 && bytes[0] == 0x28 && bytes[1] == 0xB5 && bytes[2] == 0x2F && bytes[3] == 0xFD)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "header: the file starts with the zstd frame magic 28 B5 2F FD; this reader "
                    "links no decompressor (docs/SPU_XDIFF.md 7.2), the corpus driver decodes it");

    if (n < hdr_bytes)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "header: %zu bytes, a fixture header is %zu bytes", n, hdr_bytes);

    spu_xdiff_file_header h;
    memcpy(&h, bytes, sizeof h);
    if (memcmp(h.magic, SPU_XDIFF_MAGIC, sizeof h.magic) != 0)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "header: magic is not \"SPUJOB\\r\\n\"");
    if ((h.version != SPU_XDIFF_VERSION_V1 && h.version != SPU_XDIFF_VERSION) ||
        h.version > reader_version)
        return fail(err, SPU_XDIFF_R_UNSUPPORTED_VERSION,
                    "header: version %u is not supported by reader version %u",
                    (unsigned)h.version, (unsigned)reader_version);
    if (h.header_bytes != hdr_bytes)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "header: header_bytes is %u, the section table starts at %zu",
                    (unsigned)h.header_bytes, hdr_bytes);
    if (h.section_bytes != sec_bytes)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "header: section_bytes is %u, a section is %zu bytes",
                    (unsigned)h.section_bytes, sec_bytes);
    if (h.section_count > SPU_XDIFF_MAX_SECTIONS)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "header: section_count is %u, the bound is %u",
                    (unsigned)h.section_count, (unsigned)SPU_XDIFF_MAX_SECTIONS);
    if (h.file_bytes != (uint64_t)n)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "header: file_bytes is %llu, the file is %zu bytes",
                    (unsigned long long)h.file_bytes, n);

    /* The checksum covers the header, the table and the sections with its own
     * field read as zero, so any single flipped byte fails here unless a
     * field check above already named the field. */
    {
        const size_t at = offsetof(spu_xdiff_file_header, checksum);
        static const uint8_t zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        uint64_t sum = fnv1a64(FNV64_BASIS, bytes, at);
        sum = fnv1a64(sum, zero, sizeof zero);
        sum = fnv1a64(sum, bytes + at + sizeof zero, n - at - sizeof zero);
        if (sum != h.checksum)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "header: checksum is 0x%016llx, the bytes hash to 0x%016llx",
                        (unsigned long long)h.checksum, (unsigned long long)sum);
    }

    const uint64_t table_end =
        (uint64_t)h.header_bytes + (uint64_t)h.section_count * (uint64_t)h.section_bytes;
    if (table_end > n)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "section table: %u sections of %u bytes from %u do not fit in %zu bytes",
                    (unsigned)h.section_count, (unsigned)h.section_bytes,
                    (unsigned)h.header_bytes, n);
    lay->table_end = table_end;

    for (uint32_t i = 0; i < h.section_count; i++) {
        const uint8_t* p = bytes + h.header_bytes + (size_t)i * h.section_bytes;
        const uint32_t tag = rd_u32(p);
        const uint32_t flags = rd_u32(p + 4);
        const uint64_t offset = rd_u64(p + 8);
        const uint64_t len = rd_u64(p + 16);
        const int k = section_kind_of(tag);
        if (k < 0) {
            char cc[5];
            fourcc_text(tag, cc);
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "section table: entry %u has unknown tag '%s' (0x%08x)",
                        (unsigned)i, cc, (unsigned)tag);
        }
        if (flags)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "section table: %s flags is %u, a section's flags are 0",
                        s_kind[k].name, (unsigned)flags);
        if (lay->part[k].present)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "section table: %s appears twice", s_kind[k].name);
        lay->part[k].present = 1;
        lay->part[k].offset = offset;
        lay->part[k].bytes = len;
    }

    for (int k = 0; k < SEC_KINDS; k++)
        if (k != SEC_VRDT && k != SEC_FMASK && !lay->part[k].present)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "%s: required section is missing", s_kind[k].name);

    /* Declared sizes before spans: these need no offset, and a count is
     * bounded here so nothing downstream sizes a buffer from it. */
    {
        static const struct { int k; size_t want; } fixed[] = {
            { SEC_IDEN, sizeof(spu_xdiff_identity) },
            { SEC_UNIT, sizeof(spu_xdiff_unit) },
            { SEC_REG0, SPU_XDIFF_REG_BYTES },
            { SEC_REG1, SPU_XDIFF_REG_BYTES },
            { SEC_END,  sizeof(spu_xdiff_end) },
            { SEC_FMASK, sizeof(spu_xdiff_function_mask) },
            { SEC_VRDT, sizeof(spu_xdiff_verdict_record) },
        };
        for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
            const fixture_part* p = &lay->part[fixed[i].k];
            if (p->present && p->bytes != fixed[i].want)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "%s: %llu bytes, the record is %zu bytes",
                            s_kind[fixed[i].k].name, (unsigned long long)p->bytes, fixed[i].want);
        }
    }
    if (lay->part[SEC_EFCT].bytes % sizeof(spu_xdiff_effect) != 0)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "EFCT: %llu bytes is not a whole number of %zu-byte records",
                    (unsigned long long)lay->part[SEC_EFCT].bytes, sizeof(spu_xdiff_effect));
    {
        const uint64_t records = lay->part[SEC_EFCT].bytes / sizeof(spu_xdiff_effect);
        if (records > SPU_XDIFF_MAX_EFFECTS)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "EFCT: %llu records, the bound is %u",
                        (unsigned long long)records, (unsigned)SPU_XDIFF_MAX_EFFECTS);
    }
    if (lay->part[SEC_BLOB].bytes > SPU_XDIFF_MAX_BLOB_BYTES)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "BLOB: %llu bytes, the bound is %u",
                    (unsigned long long)lay->part[SEC_BLOB].bytes,
                    (unsigned)SPU_XDIFF_MAX_BLOB_BYTES);

    for (int k = 0; k < SEC_KINDS; k++) {
        const fixture_part* p = &lay->part[k];
        if (!p->present) continue;
        if (p->offset % 8 != 0)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "%s: offset %llu is not 8-byte aligned",
                        s_kind[k].name, (unsigned long long)p->offset);
        if (p->offset < lay->table_end)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "%s: offset %llu starts inside the section table (which ends at %llu)",
                        s_kind[k].name, (unsigned long long)p->offset,
                        (unsigned long long)lay->table_end);
        if (p->offset > n || p->bytes > (uint64_t)n - p->offset)
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "%s: offset %llu + %llu bytes is outside the file's %zu bytes",
                        s_kind[k].name, (unsigned long long)p->offset,
                        (unsigned long long)p->bytes, n);
    }

    for (int a = 0; a < SEC_KINDS; a++) {
        const fixture_part* pa = &lay->part[a];
        if (!pa->present || pa->bytes == 0) continue;
        for (int b = a + 1; b < SEC_KINDS; b++) {
            const fixture_part* pb = &lay->part[b];
            if (!pb->present || pb->bytes == 0) continue;
            if (pa->offset < pb->offset + pb->bytes && pb->offset < pa->offset + pa->bytes)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "%s: [%llu, %llu) overlaps %s [%llu, %llu)",
                            s_kind[a].name, (unsigned long long)pa->offset,
                            (unsigned long long)(pa->offset + pa->bytes),
                            s_kind[b].name, (unsigned long long)pb->offset,
                            (unsigned long long)(pb->offset + pb->bytes));
        }
    }

    {
        const fixture_part* ls0 = &lay->part[SEC_LS0];
        const fixture_part* ls1 = &lay->part[SEC_LS1];
        const uint8_t* unit_bytes = bytes + lay->part[SEC_UNIT].offset;
        spu_xdiff_unit unit;
        spu_xdiff_end end;
        if (check_unit(unit_bytes, err)) return -1;
        memcpy(&unit, unit_bytes, sizeof unit);
        if (h.version == SPU_XDIFF_VERSION_V1 &&
            (unit.kind == (uint32_t)SPU_XDIFF_UNIT_FUNCTION || lay->part[SEC_FMASK].present))
            return fail(err, SPU_XDIFF_R_UNSUPPORTED_VERSION,
                        "header: version 1 does not define function units or FMAS");
        if (unit.kind == (uint32_t)SPU_XDIFF_UNIT_FUNCTION) {
            if (!lay->part[SEC_FMASK].present)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "FMAS: required for UNIT kind function");
            if (lay->part[SEC_EFCT].bytes || lay->part[SEC_BLOB].bytes)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "EFCT/BLOB: function units cannot carry job effects");
            if (check_function_mask(bytes + lay->part[SEC_FMASK].offset, &unit, err)) return -1;
            memcpy(&end, bytes + lay->part[SEC_END].offset, sizeof end);
            if (end.end_class != (uint32_t)SPU_XDIFF_END_RETURN)
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "END_: function unit end_class is %u, not return",
                            (unsigned)end.end_class);
        } else if (lay->part[SEC_FMASK].present) {
            return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                        "FMAS: present for a non-function unit");
        }
        if (check_end(bytes + lay->part[SEC_END].offset, err)) return -1;
        if (walk_extents("LS_0", bytes + ls0->offset, ls0->bytes, NULL, err)) return -1;
        if (walk_extents("LS_1", bytes + ls1->offset, ls1->bytes, NULL, err)) return -1;
    }

    /* Padding is zero (7.1): every byte outside the sections is padding, and a
     * non-zero one is not a container this format describes. Walked in offset
     * order, which the overlap check above makes total. */
    {
        uint64_t at = lay->table_end;
        for (int done = 0; done < SEC_KINDS; done++) {
            int next = -1;
            for (int k = 0; k < SEC_KINDS; k++) {
                const fixture_part* p = &lay->part[k];
                if (!p->present || p->bytes == 0 || p->offset < at) continue;
                if (next < 0 || p->offset < lay->part[next].offset) next = k;
            }
            if (next < 0) break;
            for (uint64_t i = at; i < lay->part[next].offset; i++)
                if (bytes[i])
                    return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                                "section table: the padding before %s at 0x%llx is not zero",
                                s_kind[next].name, (unsigned long long)i);
            at = lay->part[next].offset + lay->part[next].bytes;
        }
        for (uint64_t i = at; i < n; i++)
            if (bytes[i])
                return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                            "header: the padding after the last section at 0x%llx is not zero",
                            (unsigned long long)i);
    }
    {
        const fixture_part* efct = &lay->part[SEC_EFCT];
        const fixture_part* blob = &lay->part[SEC_BLOB];
        if (check_effects(bytes + efct->offset, efct->bytes, blob->bytes, err)) return -1;
    }
    return 0;
}

/* The identity is the fingerprint of the staged image bytes, which are
 * [unit.image_ls, +unit.image_bytes) of the entry store (spu_xdiff.h, IDEN).
 * The renderer's recorder hashed the same range with the same function. */
static int check_identity(const uint8_t* ls, const spu_xdiff_unit* unit,
                          const spu_xdiff_identity* id, spu_xdiff_error* err)
{
    if ((uint64_t)unit->image_ls + unit->image_bytes > SPU_XDIFF_LS_BYTES)
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "UNIT: image at LS 0x%x + %u bytes is outside local store",
                    (unsigned)unit->image_ls, unit->image_bytes);
    const uint64_t got = fnv1a64(FNV64_BASIS, ls + unit->image_ls, unit->image_bytes);
    if (got != id->image_fingerprint)
        return fail(err, SPU_XDIFF_R_FIXTURE_IDENTITY,
                    "IDEN: image fingerprint is 0x%016llx, the %u bytes at LS 0x%x hash to "
                    "0x%016llx",
                    (unsigned long long)id->image_fingerprint, unit->image_bytes,
                    (unsigned)unit->image_ls, (unsigned long long)got);
    return 0;
}

/* ---- payload store (section 7.2) ---------------------------------------- */

/* A payload is stored once per fixture: the table is keyed by length and
 * content hash and confirmed by comparing the bytes, so a job that reads one
 * line eight times spends one copy. Nothing is deduplicated across fixtures. */
struct blob_node {
    struct blob_node* next;
    uint64_t          hash;
    uint32_t          offset;
    uint32_t          length;
};

struct spu_xdiff_blob_index {
    struct blob_node** bucket;
    uint32_t           bucket_count;
    uint32_t           count;
    /* The public fixture has no field for "the last add_blob stored nothing"
     * and its header is a contract, so the flag lives here. */
    int                failed;
};

static void blob_index_free(struct spu_xdiff_blob_index* ix)
{
    if (!ix) return;
    for (uint32_t i = 0; i < ix->bucket_count; i++) {
        struct blob_node* n = ix->bucket[i];
        while (n) {
            struct blob_node* next = n->next;
            free(n);
            n = next;
        }
    }
    free(ix->bucket);
    free(ix);
}

static struct spu_xdiff_blob_index* blob_index_get(spu_xdiff_fixture* f)
{
    if (f->blob_index) return f->blob_index;
    struct spu_xdiff_blob_index* ix = calloc(1, sizeof *ix);
    if (ix) f->blob_index = ix;
    return ix;
}

static uint32_t blob_index_find(const spu_xdiff_fixture* f, const struct spu_xdiff_blob_index* ix,
                                uint64_t hash, const void* p, uint32_t size)
{
    if (!ix->bucket_count) return SPU_XDIFF_NO_BLOB;
    const uint32_t slot = (uint32_t)(hash % ix->bucket_count);
    for (const struct blob_node* n = ix->bucket[slot]; n; n = n->next)
        if (n->hash == hash && n->length == size && memcmp(f->blob + n->offset, p, size) == 0)
            return n->offset;
    return SPU_XDIFF_NO_BLOB;
}

/* Inserts, doubling the bucket array at a load factor of one. */
static int blob_index_insert(struct spu_xdiff_blob_index* ix, struct blob_node* node)
{
    if (ix->count + 1 > ix->bucket_count) {
        uint32_t want = ix->bucket_count ? ix->bucket_count * 2 : 64;
        struct blob_node** bucket = calloc(want, sizeof *bucket);
        if (!bucket) return -1;
        for (uint32_t i = 0; i < ix->bucket_count; i++) {
            struct blob_node* n = ix->bucket[i];
            while (n) {
                struct blob_node* next = n->next;
                const uint32_t slot = (uint32_t)(n->hash % want);
                n->next = bucket[slot];
                bucket[slot] = n;
                n = next;
            }
        }
        free(ix->bucket);
        ix->bucket = bucket;
        ix->bucket_count = want;
    }
    const uint32_t slot = (uint32_t)(node->hash % ix->bucket_count);
    node->next = ix->bucket[slot];
    ix->bucket[slot] = node;
    ix->count++;
    return 0;
}

/* ---- lifecycle ---------------------------------------------------------- */

int spu_xdiff_fixture_init(spu_xdiff_fixture* f)
{
    if (!f) return -1;
    memset(f, 0, sizeof *f);
    f->ls0 = calloc(1, SPU_XDIFF_LS_BYTES);
    f->ls1 = calloc(1, SPU_XDIFF_LS_BYTES);
    if (!f->ls0 || !f->ls1) {
        free(f->ls0);
        free(f->ls1);
        memset(f, 0, sizeof *f);
        return -1;
    }
    return 0;
}

void spu_xdiff_fixture_free(spu_xdiff_fixture* f)
{
    if (!f) return;
    free(f->ls0);
    free(f->ls1);
    free(f->effects);
    free(f->blob);
    blob_index_free(f->blob_index);
    memset(f, 0, sizeof *f);
}

void spu_xdiff_fixture_clear(spu_xdiff_fixture* f)
{
    if (!f) return;
    /* Keep the big allocations: a fixture is refilled job after job. The
     * payload table is only valid for the payloads it was built over, so it
     * goes (the header says it is NULL until the first payload). */
    uint8_t* ls0 = f->ls0;
    uint8_t* ls1 = f->ls1;
    spu_xdiff_effect* effects = f->effects;
    const uint32_t effect_cap = f->effect_cap;
    uint8_t* blob = f->blob;
    const uint32_t blob_cap = f->blob_cap;
    blob_index_free(f->blob_index);
    memset(f, 0, sizeof *f);
    f->ls0 = ls0;
    f->ls1 = ls1;
    f->effects = effects;
    f->effect_cap = effect_cap;
    f->blob = blob;
    f->blob_cap = blob_cap;
    if (ls0) memset(ls0, 0, SPU_XDIFF_LS_BYTES);
    if (ls1) memset(ls1, 0, SPU_XDIFF_LS_BYTES);
}

/* ---- payloads ----------------------------------------------------------- */

uint32_t spu_xdiff_fixture_add_blob(spu_xdiff_fixture* f, const void* bytes, uint32_t size,
                                    uint32_t max_blob_bytes)
{
    if (!f || !bytes) return SPU_XDIFF_NO_BLOB;
    if (size == 0) return SPU_XDIFF_NO_BLOB;

    struct spu_xdiff_blob_index* ix = blob_index_get(f);
    if (!ix) return SPU_XDIFF_NO_BLOB;
    ix->failed = 0;

    const uint64_t hash = fnv1a64(FNV64_BASIS, bytes, size);
    const uint32_t seen = blob_index_find(f, ix, hash, bytes, size);
    if (seen != SPU_XDIFF_NO_BLOB) return seen;

    uint64_t limit = max_blob_bytes ? max_blob_bytes : SPU_XDIFF_MAX_BLOB_BYTES;
    if (limit > SPU_XDIFF_MAX_BLOB_BYTES) limit = SPU_XDIFF_MAX_BLOB_BYTES;
    if ((uint64_t)f->blob_bytes + size > limit ||
        (f->live_limit && (uint64_t)f->effect_count * sizeof(spu_xdiff_effect) + f->blob_bytes + size > f->live_limit)) {
        ix->failed = 1;
        return SPU_XDIFF_NO_BLOB;
    }

    const uint32_t need = f->blob_bytes + size;
    if (!f->blob || need > f->blob_cap) {
        uint64_t want = f->blob_cap ? (uint64_t)f->blob_cap * 2u : 4096u;
        if (want < need) want = need;
        if (want > SPU_XDIFF_MAX_BLOB_BYTES) want = SPU_XDIFF_MAX_BLOB_BYTES;
        /* The allocation is bounded by the live limit too: doubling must not retain twice what may be used. */
        if (f->live_limit && want > f->live_limit) want = f->live_limit;
        uint8_t* grown = want >= need ? (uint8_t*)realloc(f->blob, (size_t)want) : NULL;
        if (!grown) {
            ix->failed = 1;
            return SPU_XDIFF_NO_BLOB;
        }
        f->blob = grown;
        f->blob_cap = (uint32_t)want;
    }

    struct blob_node* node = (struct blob_node*)malloc(sizeof *node);
    if (!node) {
        ix->failed = 1;
        return SPU_XDIFF_NO_BLOB;
    }
    node->hash = hash;
    node->length = size;
    node->offset = f->blob_bytes;
    memcpy(f->blob + f->blob_bytes, bytes, size);
    f->blob_bytes = need;
    if (blob_index_insert(ix, node)) {
        /* The bytes are in the store but the table could not hold them; undo
         * the append so the store's top is always what the table covers. */
        f->blob_bytes -= size;
        free(node);
        ix->failed = 1;
        return SPU_XDIFF_NO_BLOB;
    }
    return node->offset;
}

int spu_xdiff_fixture_blob_failed(const spu_xdiff_fixture* f)
{
    return f && f->blob_index && f->blob_index->failed ? 1 : 0;
}

int spu_xdiff_fixture_add_effect(spu_xdiff_fixture* f, const spu_xdiff_effect* e)
{
    if (!f || !e) return -1;
    if (f->effect_count >= SPU_XDIFF_MAX_EFFECTS) return -1;
    if (f->live_limit &&
        ((uint64_t)f->effect_count + 1) * sizeof *e + f->blob_bytes > f->live_limit) return -1;
    if (f->effect_count != 0 && !f->effects) return -1;
    if (f->effect_count == f->effect_cap) {
        uint32_t want = f->effect_cap ? f->effect_cap * 2u : 64u;
        if (want > SPU_XDIFF_MAX_EFFECTS || want < f->effect_cap) want = SPU_XDIFF_MAX_EFFECTS;
        if (f->live_limit && want > f->live_limit / sizeof *e) want = f->live_limit / (uint32_t)sizeof *e;
        if (want <= f->effect_cap) return -1;
        spu_xdiff_effect* grown =
            (spu_xdiff_effect*)realloc(f->effects, (size_t)want * sizeof *grown);
        if (!grown) return -1;
        f->effects = grown;
        f->effect_cap = want;
    }
    f->effects[f->effect_count++] = *e;
    return 0;
}

/* ---- encode ------------------------------------------------------------- */

typedef struct extent {
    uint32_t lsa;
    uint32_t bytes;
} extent;

typedef struct extent_vec {
    extent*  v;
    uint32_t n;
    uint32_t cap;
} extent_vec;

static void extent_vec_free(extent_vec* v)
{
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static int extent_vec_push(extent_vec* v, uint32_t lsa, uint32_t bytes)
{
    if (v->n == v->cap) {
        uint32_t want = v->cap ? v->cap * 2u : 16u;
        extent* grown = (extent*)realloc(v->v, (size_t)want * sizeof *grown);
        if (!grown) return -1;
        v->v = grown;
        v->cap = want;
    }
    v->v[v->n].lsa = lsa;
    v->v[v->n].bytes = bytes;
    v->n++;
    return 0;
}

/* An extent costs an 8-byte header plus padding, so a run of equal bytes
 * shorter than 16 is cheaper inside its extent than split around it. LS_0
 * skips zero runs from 64 bytes on (a cache line; below that the header costs
 * more than the zeros it saves). The format fixes no split rule: any exact
 * cover decodes to the same store (spu_xdiff.h, LS_0/LS_1). */
#define LS0_ZERO_SPLIT  64u
#define LS1_EQUAL_SPLIT 16u

static int ls_byte_equal(const uint8_t* bytes, const uint8_t* base, uint32_t i)
{
    return base ? bytes[i] == base[i] : bytes[i] == 0;
}

static int ls_extents(const uint8_t* bytes, const uint8_t* base, uint32_t split, extent_vec* out)
{
    const uint32_t n = SPU_XDIFF_LS_BYTES;
    uint32_t i = 0;
    while (i < n) {
        while (i < n && ls_byte_equal(bytes, base, i)) i++;
        if (i == n) break;
        const uint32_t start = i;
        while (i < n) {
            if (!ls_byte_equal(bytes, base, i)) {
                i++;
                continue;
            }
            uint32_t run_end = i;
            while (run_end < n && ls_byte_equal(bytes, base, run_end)) run_end++;
            if (run_end - i >= split || run_end == n) break;
            i = run_end;
        }
        if (extent_vec_push(out, start, i - start)) return -1;
    }
    return 0;
}

static uint64_t extent_section_bytes(const extent_vec* v)
{
    uint64_t n = 0;
    for (uint32_t i = 0; i < v->n; i++)
        n += sizeof(spu_xdiff_ls_extent) + align8(v->v[i].bytes);
    return n;
}

static void write_extents(uint8_t* p, const extent_vec* v, const uint8_t* store)
{
    uint64_t at = 0;
    for (uint32_t i = 0; i < v->n; i++) {
        spu_xdiff_ls_extent h;
        h.lsa = v->v[i].lsa;
        h.bytes = v->v[i].bytes;
        memcpy(p + at, &h, sizeof h);
        memcpy(p + at + sizeof h, store + h.lsa, h.bytes);
        at += sizeof h + align8(h.bytes);
        /* the padding is already zero: the file buffer was calloc'd */
    }
}

/* Re-labels a check's detail under the one reason spu_xdiff_fixture.h gives
 * for encode; the detail is copied first because it is where fail() writes. */
static int encode_refuse(spu_xdiff_error* err)
{
    char detail[sizeof err->detail];
    memcpy(detail, err->detail, sizeof detail);
    return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "%s", detail);
}

int spu_xdiff_fixture_encode(const spu_xdiff_fixture* f, uint8_t** out, size_t* out_bytes,
                             spu_xdiff_error* err)
{
    spu_xdiff_error local;
    if (!err) err = &local;
    if (!f || !out || !out_bytes)
        return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "encode: no fixture or no output slot");
    *out = NULL;
    *out_bytes = 0;
    if (!f->ls0 || !f->ls1)
        return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT,
                    "encode: the fixture has no local store (init was not called)");
    if ((f->effect_count != 0 && !f->effects) || (f->blob_bytes != 0 && !f->blob))
        return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "encode: the fixture's records are missing");
    if (f->effect_count > SPU_XDIFF_MAX_EFFECTS)
        return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "EFCT: %u records, the bound is %u",
                    f->effect_count, (unsigned)SPU_XDIFF_MAX_EFFECTS);
    if (f->blob_bytes > SPU_XDIFF_MAX_BLOB_BYTES)
        return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "BLOB: %u bytes, the bound is %u",
                    f->blob_bytes, (unsigned)SPU_XDIFF_MAX_BLOB_BYTES);

    /* The identity check of decode, on the store the LS_0 extents are built
     * from: any exact cover decodes to this store, so this is the file's. */
    if (check_identity(f->ls0, &f->unit, &f->identity, err)) return encode_refuse(err);

    extent_vec ls0 = { 0, 0, 0 }, ls1 = { 0, 0, 0 };
    if (ls_extents(f->ls0, NULL, LS0_ZERO_SPLIT, &ls0) ||
        ls_extents(f->ls1, f->ls0, LS1_EQUAL_SPLIT, &ls1)) {
        extent_vec_free(&ls0);
        extent_vec_free(&ls1);
        return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "encode: out of memory for the extents");
    }

    int present[SEC_KINDS] = { 0 };
    uint64_t size[SEC_KINDS] = { 0 };
    present[SEC_IDEN] = 1;
    size[SEC_IDEN] = sizeof(spu_xdiff_identity);
    present[SEC_UNIT] = 1;
    size[SEC_UNIT] = sizeof(spu_xdiff_unit);
    present[SEC_REG0] = 1;
    size[SEC_REG0] = SPU_XDIFF_REG_BYTES;
    present[SEC_LS0] = 1;
    size[SEC_LS0] = extent_section_bytes(&ls0);
    present[SEC_EFCT] = 1;
    size[SEC_EFCT] = (uint64_t)f->effect_count * sizeof(spu_xdiff_effect);
    present[SEC_BLOB] = 1;
    size[SEC_BLOB] = f->blob_bytes;
    present[SEC_REG1] = 1;
    size[SEC_REG1] = SPU_XDIFF_REG_BYTES;
    present[SEC_LS1] = 1;
    size[SEC_LS1] = extent_section_bytes(&ls1);
    present[SEC_END] = 1;
    size[SEC_END] = sizeof(spu_xdiff_end);
    present[SEC_FMASK] = f->unit.kind == (uint32_t)SPU_XDIFF_UNIT_FUNCTION;
    size[SEC_FMASK] = present[SEC_FMASK] ? sizeof(spu_xdiff_function_mask) : 0;
    present[SEC_VRDT] = f->has_verdict ? 1 : 0;
    size[SEC_VRDT] = f->has_verdict ? sizeof(spu_xdiff_verdict_record) : 0;

    uint32_t count = 0;
    uint64_t off[SEC_KINDS] = { 0 };
    for (int k = 0; k < SEC_KINDS; k++)
        if (present[k]) count++;
    const uint64_t table_end =
        sizeof(spu_xdiff_file_header) + (uint64_t)count * sizeof(spu_xdiff_section);
    uint64_t cursor = align8(table_end);
    int last = SEC_IDEN;
    for (int k = 0; k < SEC_KINDS; k++) {
        if (!present[k]) continue;
        off[k] = cursor;
        cursor = align8(cursor + size[k]);
        last = k;
    }
    const uint64_t file_bytes = off[last] + size[last];
    if (file_bytes > (uint64_t)SIZE_MAX) {
        extent_vec_free(&ls0);
        extent_vec_free(&ls1);
        return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT,
                    "encode: %llu bytes do not fit in memory here",
                    (unsigned long long)file_bytes);
    }

    uint8_t* buf = (uint8_t*)calloc(1, (size_t)file_bytes);
    if (!buf) {
        extent_vec_free(&ls0);
        extent_vec_free(&ls1);
        return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "encode: out of memory for %llu bytes",
                    (unsigned long long)file_bytes);
    }

    spu_xdiff_file_header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, SPU_XDIFF_MAGIC, sizeof h.magic);
    h.version = SPU_XDIFF_VERSION;
    h.header_bytes = (uint32_t)sizeof h;
    h.section_count = count;
    h.section_bytes = (uint32_t)sizeof(spu_xdiff_section);
    h.file_bytes = file_bytes;
    memcpy(buf, &h, sizeof h);

    {
        uint32_t entry = 0;
        for (int k = 0; k < SEC_KINDS; k++) {
            if (!present[k]) continue;
            spu_xdiff_section s;
            memset(&s, 0, sizeof s);
            s.tag = s_kind[k].tag;
            s.offset = off[k];
            s.bytes = size[k];
            memcpy(buf + sizeof h + (size_t)entry * sizeof s, &s, sizeof s);
            entry++;
        }
    }

    memcpy(buf + off[SEC_IDEN], &f->identity, sizeof f->identity);
    memcpy(buf + off[SEC_UNIT], &f->unit, sizeof f->unit);
    memcpy(buf + off[SEC_REG0], f->reg0, SPU_XDIFF_REG_BYTES);
    write_extents(buf + off[SEC_LS0], &ls0, f->ls0);
    if (f->effect_count)
        memcpy(buf + off[SEC_EFCT], f->effects,
               (size_t)f->effect_count * sizeof(spu_xdiff_effect));
    if (f->blob_bytes) memcpy(buf + off[SEC_BLOB], f->blob, f->blob_bytes);
    memcpy(buf + off[SEC_REG1], f->reg1, SPU_XDIFF_REG_BYTES);
    write_extents(buf + off[SEC_LS1], &ls1, f->ls1);
    memcpy(buf + off[SEC_END], &f->end, sizeof f->end);
    if (present[SEC_FMASK])
        memcpy(buf + off[SEC_FMASK], &f->function_mask, sizeof f->function_mask);
    if (f->has_verdict) memcpy(buf + off[SEC_VRDT], &f->verdict, sizeof f->verdict);

    {
        /* The checksum field is still zero, which is how a reader hashes it. */
        const uint64_t sum = fnv1a64(FNV64_BASIS, buf, (size_t)file_bytes);
        memcpy(buf + offsetof(spu_xdiff_file_header, checksum), &sum, sizeof sum);
    }
    extent_vec_free(&ls0);
    extent_vec_free(&ls1);

    /* The encoder never returns a file the decoder rejects: the same checks,
     * over the bytes as written. A refusal is reported as CAPTURE_LIMIT, the
     * one reason spu_xdiff_fixture.h gives for encode, with the decoder's own
     * detail naming the section, record or offset. */
    fixture_layout lay;
    if (parse_file(buf, (size_t)file_bytes, SPU_XDIFF_VERSION, &lay, err)) {
        free(buf);
        return encode_refuse(err);
    }
    *out = buf;
    *out_bytes = (size_t)file_bytes;
    return 0;
}

/* ---- decode ------------------------------------------------------------- */

static int decode_fill(const uint8_t* bytes, const fixture_layout* lay, spu_xdiff_fixture* f,
                       spu_xdiff_error* err)
{
    if (spu_xdiff_fixture_init(f))
        return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT,
                    "decode: out of memory for two %u-byte local stores",
                    (unsigned)SPU_XDIFF_LS_BYTES);

    memcpy(&f->identity, bytes + lay->part[SEC_IDEN].offset, sizeof f->identity);
    memcpy(&f->unit, bytes + lay->part[SEC_UNIT].offset, sizeof f->unit);
    if (lay->part[SEC_FMASK].present)
        memcpy(&f->function_mask, bytes + lay->part[SEC_FMASK].offset, sizeof f->function_mask);
    memcpy(f->reg0, bytes + lay->part[SEC_REG0].offset, SPU_XDIFF_REG_BYTES);
    memcpy(f->reg1, bytes + lay->part[SEC_REG1].offset, SPU_XDIFF_REG_BYTES);
    memcpy(&f->end, bytes + lay->part[SEC_END].offset, sizeof f->end);
    if (lay->part[SEC_VRDT].present) {
        f->has_verdict = 1;
        memcpy(&f->verdict, bytes + lay->part[SEC_VRDT].offset, sizeof f->verdict);
    }

    {
        const uint32_t count =
            (uint32_t)(lay->part[SEC_EFCT].bytes / sizeof(spu_xdiff_effect));
        if (count != 0) {
            f->effects = (spu_xdiff_effect*)malloc((size_t)count * sizeof *f->effects);
            if (!f->effects) {
                spu_xdiff_fixture_free(f);
                return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT,
                            "decode: out of memory for %u effects", count);
            }
            memcpy(f->effects, bytes + lay->part[SEC_EFCT].offset,
                   (size_t)count * sizeof *f->effects);
            f->effect_cap = count;
            f->effect_count = count;
        }
    }
    {
        const uint32_t blob = (uint32_t)lay->part[SEC_BLOB].bytes;
        if (blob != 0) {
            f->blob = (uint8_t*)malloc(blob);
            if (!f->blob) {
                spu_xdiff_fixture_free(f);
                return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT,
                            "decode: out of memory for %u payload bytes", blob);
            }
            memcpy(f->blob, bytes + lay->part[SEC_BLOB].offset, blob);
            f->blob_cap = blob;
            f->blob_bytes = blob;
        }
    }

    /* LS_0 is laid over the zero store init made; LS_1 over the result. */
    if (walk_extents("LS_0", bytes + lay->part[SEC_LS0].offset, lay->part[SEC_LS0].bytes,
                     f->ls0, err)) {
        spu_xdiff_fixture_free(f);
        return -1;
    }
    memcpy(f->ls1, f->ls0, SPU_XDIFF_LS_BYTES);
    if (walk_extents("LS_1", bytes + lay->part[SEC_LS1].offset, lay->part[SEC_LS1].bytes,
                     f->ls1, err)) {
        spu_xdiff_fixture_free(f);
        return -1;
    }

    if (check_identity(f->ls0, &f->unit, &f->identity, err)) {
        spu_xdiff_fixture_free(f);
        return -1;
    }
    return 0;
}

int spu_xdiff_fixture_decode_max_version(const uint8_t* bytes, size_t n, uint32_t max_version,
                                         spu_xdiff_fixture* f, spu_xdiff_error* err)
{
    spu_xdiff_error local;
    if (!err) err = &local;
    if (!f) return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT, "decode: no fixture to fill");
    memset(f, 0, sizeof *f);
    if (max_version < SPU_XDIFF_VERSION_V1 || max_version > SPU_XDIFF_VERSION)
        return fail(err, SPU_XDIFF_R_UNSUPPORTED_VERSION,
                    "reader version %u is not supported by this build", (unsigned)max_version);
    if (!bytes) return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT, "header: no bytes");

    fixture_layout lay;
    if (parse_file(bytes, n, max_version, &lay, err)) return -1;
    return decode_fill(bytes, &lay, f, err);
}

int spu_xdiff_fixture_decode(const uint8_t* bytes, size_t n, spu_xdiff_fixture* f,
                             spu_xdiff_error* err)
{
    return spu_xdiff_fixture_decode_max_version(bytes, n, SPU_XDIFF_VERSION, f, err);
}

/* ---- files -------------------------------------------------------------- */

/* The largest file the format can describe: the payload bound, every effect,
 * and room for the fixed records, the two extent sections and the table. A
 * longer file is malformed before it is read. */
#define FIXTURE_MAX_FILE_BYTES                                                     \
    ((uint64_t)SPU_XDIFF_MAX_BLOB_BYTES +                                          \
     (uint64_t)SPU_XDIFF_MAX_EFFECTS * sizeof(spu_xdiff_effect) + 8u * 1024u * 1024u)

static int read_file(const char* path, uint8_t** out, size_t* out_bytes, spu_xdiff_error* err)
{
    FILE* fp = fopen(path, "rb");
    if (!fp)
        return fail(err, SPU_XDIFF_R_NO_FIXTURE, "cannot open %s: %s", path, strerror(errno));
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT, "%s: cannot seek: %s", path, strerror(errno));
    }
    const long end = ftell(fp);
    if (end < 0) {
        fclose(fp);
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT, "%s: cannot size: %s", path, strerror(errno));
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT, "%s: cannot rewind: %s", path,
                    strerror(errno));
    }
    const uint64_t size = (uint64_t)end;
    if (size > FIXTURE_MAX_FILE_BYTES) {
        fclose(fp);
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT,
                    "%s: %llu bytes, larger than a fixture may be (%llu)", path,
                    (unsigned long long)size, (unsigned long long)FIXTURE_MAX_FILE_BYTES);
    }
    uint8_t* buf = (uint8_t*)malloc(size ? (size_t)size : 1);
    if (!buf) {
        fclose(fp);
        return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "%s: out of memory for %llu bytes", path,
                    (unsigned long long)size);
    }
    const size_t got = size ? fread(buf, 1, (size_t)size, fp) : 0;
    fclose(fp);
    if (got != (size_t)size) {
        free(buf);
        return fail(err, SPU_XDIFF_R_FIXTURE_FORMAT, "%s: read %zu of %llu bytes", path, got,
                    (unsigned long long)size);
    }
    *out = buf;
    *out_bytes = (size_t)size;
    return 0;
}

int spu_xdiff_fixture_read(const char* path, spu_xdiff_fixture* f, spu_xdiff_error* err)
{
    spu_xdiff_error local;
    if (!err) err = &local;
    if (!f) return fail(err, SPU_XDIFF_R_NO_FIXTURE, "read: no fixture to fill");
    memset(f, 0, sizeof *f);
    if (!path) return fail(err, SPU_XDIFF_R_NO_FIXTURE, "read: no path");

    uint8_t* bytes = NULL;
    size_t n = 0;
    if (read_file(path, &bytes, &n, err)) return -1;
    const int rc = spu_xdiff_fixture_decode(bytes, n, f, err);
    free(bytes);
    return rc;
}

int spu_xdiff_fixture_write(const spu_xdiff_fixture* f, const char* path, spu_xdiff_error* err)
{
    spu_xdiff_error local;
    if (!err) err = &local;
    if (!path) return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "write: no path");

    /* The temp name is the process's and this attempt's own, and it is created exclusively: two processes
     * that share a directory (two runs of one title, a restart over a leftover) never write into one file,
     * so a reader of the final name sees one writer's complete bytes or none. A crash can leave a stray
     * `.partial`; nothing reads those. The final name stays `path`: when it already exists the rename below
     * replaces it (the same fingerprint, queue and seq of an earlier run). */
    static _Atomic unsigned s_attempt;
    const unsigned long long pid = (unsigned long long)XD_GETPID();
    const unsigned attempt = atomic_fetch_add_explicit(&s_attempt, 1u, memory_order_relaxed); /* uniqueness only */
    const size_t plen = strlen(path);
    const size_t pcap = plen + 1 + 20 + 1 + 10 + sizeof ".partial";
    char* partial = (char*)malloc(pcap);
    if (!partial)
        return fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "write: out of memory for the path");
    snprintf(partial, pcap, "%s.%llu.%u.partial", path, pid, attempt);

    uint8_t* bytes = NULL;
    size_t n = 0;
    if (spu_xdiff_fixture_encode(f, &bytes, &n, err)) {
        free(partial);
        return -1;
    }

    /* No fsync: the rename is what keeps a reader from seeing half a file. */
    FILE* fp = NULL;
    int open_errno = 0;
    {
        const int fd = XD_OPEN_EXCL(partial);
        if (fd < 0) {
            open_errno = errno;
        } else if ((fp = XD_FDOPEN(fd)) == NULL) {
            open_errno = errno;
            XD_CLOSE(fd);
            remove(partial);
        }
    }
    if (!fp) {
        free(bytes);
        const int rc = fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "%s: cannot create: %s", partial,
                            strerror(open_errno));
        free(partial);
        return rc;
    }
    int wrote = n == 0 || fwrite(bytes, 1, n, fp) == n;
    int e = 0;
    if (!wrote) e = errno;
    if (fclose(fp) != 0 && wrote) {
        wrote = 0;
        e = errno;
    }
    free(bytes);
    if (!wrote) {
        remove(partial);
        const int rc = fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "%s: write failed: %s", partial,
                            strerror(e));
        free(partial);
        return rc;
    }
#ifdef _WIN32
    /* C's rename does not replace an existing file there. */
    remove(path);
#endif
    if (rename(partial, path) != 0) {
        e = errno;
        remove(partial);
        const int rc = fail(err, SPU_XDIFF_R_CAPTURE_LIMIT, "%s -> %s: rename failed: %s", partial,
                            path, strerror(e));
        free(partial);
        return rc;
    }
    free(partial);
    return 0;
}

/* ---- name --------------------------------------------------------------- */

int spu_xdiff_fixture_name(char* buf, size_t n, uint64_t image_fingerprint, uint32_t queue,
                           uint32_t seq)
{
    return snprintf(buf, n, "%016llx-q%u-s%u.spujob",
                    (unsigned long long)image_fingerprint, (unsigned)queue, (unsigned)seq);
}

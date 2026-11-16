/*
 * ps3recomp - SPU job differential execution (xdiff): the shared contract
 *
 * One SPU job image can run in several tiers (interpreted, lifted, partly
 * native, native). xdiff runs one tier as the authority, records what it did
 * at the effect port (spu_effect_port.h), and replays that record into another
 * tier, which can then neither see the live world nor change it. This header
 * holds what both sides, the offline runner and the report tools agree on:
 *
 *   - the tiers and the two configuration choices (which tier is the
 *     authority, whether the other one shadows it);
 *   - the verdict vocabulary and its stable reason ids;
 *   - the layout of a version-2 fixture (and version-1 job fixtures): a job's
 *     entry/transcript/end state or one function call's entry/exit state and masks.
 *
 * Design, matching rules and the evidence each choice rests on:
 * docs/SPU_XDIFF.md. Nothing here records, replays or compares; the only
 * function bodies are pure lookups over the tables below.
 *
 * A fixture recorded from a title holds that title's code and data. It never
 * enters this repository; toolkit tests build theirs from synthetic images.
 */
#ifndef SPU_XDIFF_H
#define SPU_XDIFF_H

#include "spu_effect_port.h"

#include <assert.h>
#include <stdint.h>

/* The structures below are the bytes of a fixture file, which is
 * little-endian. Both host families the toolkit runs on are; a big-endian
 * host needs a converting reader, not these structures. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "spu_xdiff.h describes a little-endian file layout"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- tiers and modes ----------------------------------------------------- */

/* The implementation tiers of one job image. Values are stored in fixtures
 * and trace records. */
typedef enum spu_job_tier {
    SPU_TIER_INTERP  = 0, /* T0: the SPU interpreter; needs no registration */
    SPU_TIER_LIFTED  = 1, /* T1: the whole image, lifted by tools/spu_lifter.py */
    SPU_TIER_PARTIAL = 2, /* T2: T1 with native overrides of single functions */
    SPU_TIER_NATIVE  = 3  /* T3: a native reimplementation of the whole job */
} spu_job_tier;
#define SPU_TIER_MASK  0x0Fu
/* Or'ed into a stored tier: the body changes behaviour on purpose (a mod). It
 * is never compared as equivalent to anything (spu_xdiff_match_for). */
#define SPU_TIER_TWEAK 0x80u

/* `spu.job_body`: which tier is the authority, the one whose result the guest
 * sees. LIFTED means the registered body of the image, whatever its tier; an
 * image with none runs interpreted. */
typedef enum spu_job_body {
    SPU_JOB_BODY_INTERP = 0,
    SPU_JOB_BODY_LIFTED = 1
} spu_job_body;

/* `spu.xdiff`: whether the other tier of an image runs as a shadow. */
typedef enum spu_xdiff_mode {
    SPU_XDIFF_OFF    = 0, /* no recording, no shadow */
    SPU_XDIFF_SAMPLE = 1, /* every N-th job of an image; a sample that cannot be queued is dropped and counted */
    SPU_XDIFF_ALL    = 2  /* every job; the worker waits for queue room, so none is dropped */
} spu_xdiff_mode;

/* `spu.xdiff_on_divergence`: what a `differ` verdict does to the run. The
 * authority's result is what the guest sees in both cases. */
typedef enum spu_xdiff_on_divergence {
    SPU_XDIFF_REPORT = 0, /* diagnostic, counter, fixture; the run goes on */
    SPU_XDIFF_STOP   = 1  /* the same, then a named stop */
} spu_xdiff_on_divergence;

/* How a replaying body is held to a transcript. */
typedef enum spu_xdiff_match {
    /* Same program: the same effects, in the same order, with the same
     * arguments; then every register and all of local store, bit for bit. */
    SPU_XDIFF_MATCH_PROGRAM  = 1,
    /* Contract: the same writes and signals per synchronisation epoch, reads
     * served by address; the end classification, not the private end state. */
    SPU_XDIFF_MATCH_CONTRACT = 2
} spu_xdiff_match;

/* The rule for a pair of stored tiers, or 0 when the pair is not comparable.
 * T0/T1 execute the same instructions. T2's pilot keeps that strict rule;
 * only T3 uses the relaxed contract, and a tweak has no comparison rule. */
static inline uint32_t spu_xdiff_match_for(uint32_t authority_tier, uint32_t subject_tier)
{
    const uint32_t a = authority_tier & SPU_TIER_MASK, s = subject_tier & SPU_TIER_MASK;
    if ((authority_tier | subject_tier) & SPU_TIER_TWEAK) return 0;
    if (a > (uint32_t)SPU_TIER_NATIVE || s > (uint32_t)SPU_TIER_NATIVE) return 0;
    return (a <= (uint32_t)SPU_TIER_PARTIAL && s <= (uint32_t)SPU_TIER_PARTIAL)
               ? (uint32_t)SPU_XDIFF_MATCH_PROGRAM
               : (uint32_t)SPU_XDIFF_MATCH_CONTRACT;
}

/* ---- verdicts ------------------------------------------------------------ */

/* The outcome of one comparison. The value is the exit status of the offline
 * runner, and the vocabulary is the shader differential's (0 equal, 2 input
 * malformed or not comparable, 3 not found, 4 differ, 5 void), so one wrapper
 * script reads both; 1 stays a usage error and is not a verdict.
 *
 * `void` means the comparison could not be decided. It is never folded into
 * `equal`: a void run is evidence of nothing. */
#define SPU_XDIFF_VERDICTS(X)        \
    X(EQUAL,     0, "equal")         \
    X(MALFORMED, 2, "malformed")     \
    X(NOT_FOUND, 3, "not-found")     \
    X(DIFFER,    4, "differ")        \
    X(VOID,      5, "void")

typedef enum spu_xdiff_verdict {
#define SPU_XDIFF_VERDICT_ENUM(name, value, id) SPU_XDIFF_##name = value,
    SPU_XDIFF_VERDICTS(SPU_XDIFF_VERDICT_ENUM)
#undef SPU_XDIFF_VERDICT_ENUM
    SPU_XDIFF_VERDICT_INVALID = 255
} spu_xdiff_verdict;

/* Why. Ids are stable kebab-case, like stop kinds (include/ps3emu/stop.h):
 * reports count by them, so a new situation gets a new row, never a reused
 * one. Each reason belongs to exactly one verdict.
 *
 * The differ reasons are in the order a same-program replay meets them: an
 * effect of another kind or channel, the same effect with other arguments, a
 * write of other bytes, an effect past the transcript's end, a transcript
 * left unfinished, then the end state including its return destination. */
#define SPU_XDIFF_REASONS(X)                                                                        \
    X(NONE,               0, "none",               EQUAL)                                           \
    X(EFFECT_KIND,       16, "effect-kind",        DIFFER)                                          \
    X(EFFECT_REQUEST,    17, "effect-request",     DIFFER)                                          \
    X(EFFECT_PAYLOAD,    18, "effect-payload",     DIFFER)                                          \
    X(EFFECT_EXTRA,      19, "effect-extra",       DIFFER)                                          \
    X(EFFECT_MISSING,    20, "effect-missing",     DIFFER)                                          \
    X(END_CLASS,         21, "end-class",          DIFFER)                                          \
    X(END_REGS,          22, "end-regs",           DIFFER)                                          \
    X(END_LS,            23, "end-ls",             DIFFER)                                          \
    X(END_PC,            27, "end-pc",             DIFFER)                                          \
    /* Contract matching only: a write or signal ahead of its epoch; an epoch whose bytes differ. */ \
    X(EPOCH_ORDER,       24, "epoch-order",        DIFFER)                                          \
    X(EPOCH_WRITES,      25, "epoch-writes",       DIFFER)                                          \
    /* The subject spent its budget; the authority had none to spend. */                            \
    X(BUDGET,            32, "budget",             VOID)                                            \
    X(DEPTH_GUARD,       37, "depth-guard",        VOID)                                            \
    /* The run used something no port mediates (an interrupt, bisled). */                           \
    X(UNSUPPORTED_EFFECT, 33, "unsupported-effect", VOID)                                           \
    /* Contract matching only: a read the transcript does not hold. Never served from live memory. */ \
    X(UNCOVERED_READ,    34, "uncovered-read",     VOID)                                            \
    /* The subject branched to code its tier has no body for. */                                    \
    X(UNLIFTED_BRANCH,   35, "unlifted-branch",    VOID)                                            \
    /* An out-of-process subject died. */                                                           \
    X(HOST_FAULT,        36, "host-fault",         VOID)                                            \
    /* The authority exceeded the bounded live capture buffer; the comparison was not attempted. */ \
    X(CAPTURE_LIMIT,    38, "capture-limit",     VOID)                                            \
    X(FIXTURE_FORMAT,    48, "fixture-format",     MALFORMED)                                       \
    /* The file is well-formed but this reader does not implement its version. */                    \
    X(UNSUPPORTED_VERSION, 51, "unsupported-version", MALFORMED)                                     \
    /* A well-formed fixture of another image, or of an effect vocabulary this build cannot replay. */ \
    X(FIXTURE_IDENTITY,  49, "fixture-identity",   MALFORMED)                                       \
    /* The pair of tiers has no matching rule (a tweak). */                                         \
    X(NOT_COMPARABLE,    50, "not-comparable",     MALFORMED)                                       \
    X(NO_FIXTURE,        64, "no-fixture",         NOT_FOUND)                                       \
    X(NO_BODY,           65, "no-body",            NOT_FOUND)                                       \
    /* The offline runner's poison run (--poison): state a liveness claim called dead changed the run. */ \
    X(POISON,            26, "poison",             DIFFER)

typedef enum spu_xdiff_reason {
#define SPU_XDIFF_REASON_ENUM(name, value, id, verdict) SPU_XDIFF_R_##name = value,
    SPU_XDIFF_REASONS(SPU_XDIFF_REASON_ENUM)
#undef SPU_XDIFF_REASON_ENUM
    SPU_XDIFF_R_INVALID = 255
} spu_xdiff_reason;

/* The id of a verdict, or "invalid-verdict". Never NULL. */
static inline const char* spu_xdiff_verdict_id(uint32_t verdict)
{
    switch (verdict) {
#define SPU_XDIFF_VERDICT_ID(name, value, id) case value: return id;
    SPU_XDIFF_VERDICTS(SPU_XDIFF_VERDICT_ID)
#undef SPU_XDIFF_VERDICT_ID
    default: return "invalid-verdict";
    }
}

/* The id of a reason, or "invalid-reason". Never NULL. */
static inline const char* spu_xdiff_reason_id(uint32_t reason)
{
    switch (reason) {
#define SPU_XDIFF_REASON_ID(name, value, id, verdict) case value: return id;
    SPU_XDIFF_REASONS(SPU_XDIFF_REASON_ID)
#undef SPU_XDIFF_REASON_ID
    default: return "invalid-reason";
    }
}

/* The verdict a reason belongs to, or SPU_XDIFF_VERDICT_INVALID. */
static inline uint32_t spu_xdiff_reason_verdict(uint32_t reason)
{
    switch (reason) {
#define SPU_XDIFF_REASON_VERDICT(name, value, id, verdict) case value: return (uint32_t)SPU_XDIFF_##verdict;
    SPU_XDIFF_REASONS(SPU_XDIFF_REASON_VERDICT)
#undef SPU_XDIFF_REASON_VERDICT
    default: return (uint32_t)SPU_XDIFF_VERDICT_INVALID;
    }
}

/* How much a `differ` matters, which decides what it may do to a run
 * (docs/SPU_XDIFF.md section 8.4). VISIBLE: the world saw something else (an
 * effect, the end class, an epoch) or the loader keeps the state that
 * differs. LATENT: only state the loader destroys differs, registers or local
 * store after a JobQueue2 job returned. Same program, a latent difference is
 * still a defect of the lift, one this input does not expose; it reports, and
 * blocks promotion like any `differ`. */
typedef enum spu_xdiff_differ_class {
    SPU_XDIFF_CLASS_NONE    = 0, /* not a differ */
    SPU_XDIFF_CLASS_VISIBLE = 1,
    SPU_XDIFF_CLASS_LATENT  = 2
} spu_xdiff_differ_class;

/* `profile` is the fixture's spu_xdiff_profile (UNIT). */
static inline uint32_t spu_xdiff_differ_class_of(uint32_t reason, uint32_t profile)
{
    if (spu_xdiff_reason_verdict(reason) != (uint32_t)SPU_XDIFF_DIFFER) return SPU_XDIFF_CLASS_NONE;
    if ((reason == (uint32_t)SPU_XDIFF_R_END_REGS || reason == (uint32_t)SPU_XDIFF_R_END_LS) &&
        profile == 2u /* SPU_XDIFF_PROFILE_JQ2: the loader keeps no local store */)
        return SPU_XDIFF_CLASS_LATENT;
    return SPU_XDIFF_CLASS_VISIBLE;
}

/* "visible", "latent", or "none". Never NULL. */
static inline const char* spu_xdiff_differ_class_id(uint32_t klass)
{
    return klass == SPU_XDIFF_CLASS_VISIBLE ? "visible" : klass == SPU_XDIFF_CLASS_LATENT ? "latent" : "none";
}

/* 1 when an MFC command word is a list command, whose MFC_CMD record carries
 * the staged list as payload; and 0 for every other opcode, whose XFER may
 * keep bit 2. The runtime decides in this order, on the whole command word
 * (spu_channels.c, spu_mfc_atomic; spu_dma.h, mfc_submit): the four atomics
 * first, PUTLLC 0xB4 among them although it has bit 2 set, then the three
 * barriers, then bit 2. The recorder, the decoder and the replay all ask
 * here, and compare as the runtime does, so a word with class bits set is a
 * list exactly when the runtime runs it as one. */
static inline int spu_xdiff_cmd_is_list(uint32_t cmd)
{
    switch (cmd) {
    case 0xD0u: case 0xB4u: case 0xB0u: case 0xB8u: /* GETLLAR, PUTLLC, PUTLLUC, PUTQLLUC */
    case 0xC0u: case 0xC8u: case 0xCCu:             /* barrier, eieio, sync */
        return 0;
    default:
        return (cmd & 0x04u) != 0;
    }
}

/* ---- transcript ---------------------------------------------------------- */

/* One record per tap callback (spu_effect_port.h), in the order they arrived.
 * An XFER belongs to the nearest WRCH or MFC_CMD before it. */
typedef enum spu_xdiff_effect_kind {
    SPU_FX_WRCH    = 1, /* a channel write other than a latch or MFC_Cmd */
    SPU_FX_RDCH    = 2, /* a channel read and the value it returned */
    SPU_FX_RCHCNT  = 3, /* a channel count and the count it returned */
    SPU_FX_MFC_CMD = 4, /* an MFC_Cmd write with the latches it consumed */
    SPU_FX_XFER    = 5  /* one movement of bytes that command (or a list resume) caused */
} spu_xdiff_effect_kind;

#define SPU_XDIFF_NO_BLOB 0xFFFFFFFFu

/* A transcript record. Unused fields are 0 (`blob` is SPU_XDIFF_NO_BLOB), so
 * two records of the same effect are the same 40 bytes and a same-program
 * replay compares requests with one memcmp of the fields it owns.
 *
 *   kind     channel  value         status  ea        lsa   size        blob
 *   WRCH     ch       written       -       -         -     -           -
 *   RDCH     ch       returned      -       -         -     -           -
 *   RCHCNT   ch       count         -       -         -     -           -
 *   MFC_CMD  21       command word  -       EAH:EAL   LSA   MFC_Size    list commands: the list as staged
 *   XFER     -        opcode        atomic  first EA  LSA   bytes       the bytes moved; none for ZERO_EA
 *
 * `tag` is the tag group of MFC_CMD and XFER. `dir` is the XFER's
 * spu_port_xfer_dir. `pc` is ctx->pc at the effect: exact for the
 * interpreter, the last transfer target for lifted code, so it locates and is
 * never compared. For a list command `ea`'s low word is the list's local-store
 * address and `size` its length; the list bytes are part of the request,
 * because two different lists that happen to move the same bytes are two
 * different programs. */
typedef struct spu_xdiff_effect {
    uint8_t  kind;    /* spu_xdiff_effect_kind */
    uint8_t  channel;
    uint8_t  tag;
    uint8_t  dir;
    uint32_t pc;
    uint32_t value;
    uint32_t status;
    uint64_t ea;
    uint32_t lsa;
    uint32_t size;
    uint32_t blob;    /* byte offset in the BLOB section of `size` payload bytes */
    uint32_t reserved;
} spu_xdiff_effect;
static_assert(sizeof(spu_xdiff_effect) == 40, "spu_xdiff_effect is a file record");

/* ---- fixture: ps3recomp.spujob/2 ------------------------------------------
 *
 * The reader accepts version-1 job fixtures and version-2 job or function
 * fixtures. Writers emit version 2. */
#define SPU_XDIFF_SCHEMA_V1 "ps3recomp.spujob/1"
#define SPU_XDIFF_SCHEMA    "ps3recomp.spujob/2"
#define SPU_XDIFF_VERSION_V1 1u
#define SPU_XDIFF_VERSION  2u
#define SPU_XDIFF_MAGIC   "SPUJOB\r\n"

/* Bounds a reader enforces before it trusts a count or an offset; a fixture
 * is an input file like any other and may be truncated, corrupt or hostile. */
#define SPU_XDIFF_MAX_SECTIONS   32u
#define SPU_XDIFF_MAX_EFFECTS    (1u << 22)
#define SPU_XDIFF_MAX_BLOB_BYTES (1u << 30)
/* Live audit has a smaller, bounded buffer than an offline fixture reader. */
#define SPU_XDIFF_MAX_LIVE_SAMPLE_BYTES (4u * 1024u * 1024u)
#define SPU_XDIFF_LIVE_QUEUE_SLOTS      8u
#define SPU_XDIFF_LIVE_QUEUE_BYTES      (SPU_XDIFF_MAX_LIVE_SAMPLE_BYTES * SPU_XDIFF_LIVE_QUEUE_SLOTS)
#define SPU_XDIFF_LS_BYTES       (256u * 1024u) /* SPU_LS_SIZE */
#define SPU_XDIFF_REG_BYTES      (128u * 16u)   /* the register file */

/* Offset 0. Sections follow the table in any order, each at an 8-byte
 * boundary, none overlapping; padding is zero. */
typedef struct spu_xdiff_file_header {
    uint8_t  magic[8];      /* SPU_XDIFF_MAGIC */
    uint32_t version;       /* SPU_XDIFF_VERSION */
    uint32_t header_bytes;  /* sizeof(spu_xdiff_file_header); the section table starts here */
    uint32_t section_count; /* <= SPU_XDIFF_MAX_SECTIONS */
    uint32_t section_bytes; /* sizeof(spu_xdiff_section) */
    uint64_t file_bytes;    /* the whole file: a short file is a truncated one */
    uint64_t checksum;      /* FNV-1a-64 (spu_workload_fingerprint) over bytes [0, file_bytes), this field
                             * read as zero: a flipped header byte fails it like any other */
} spu_xdiff_file_header;
static_assert(sizeof(spu_xdiff_file_header) == 40, "spu_xdiff_file_header is a file record");

typedef struct spu_xdiff_section {
    uint32_t tag;    /* SPU_XDIFF_SEC_* */
    uint32_t flags;  /* 0 */
    uint64_t offset; /* from the start of the file */
    uint64_t bytes;
} spu_xdiff_section;
static_assert(sizeof(spu_xdiff_section) == 24, "spu_xdiff_section is a file record");

#define SPU_XDIFF_FOURCC(a, b, c, d) \
    ((uint32_t)(uint8_t)(a) | (uint32_t)(uint8_t)(b) << 8 | (uint32_t)(uint8_t)(c) << 16 | (uint32_t)(uint8_t)(d) << 24)

/* Required once for a job unit; FMAS is required only for a function unit. */
#define SPU_XDIFF_SEC_IDENTITY SPU_XDIFF_FOURCC('I', 'D', 'E', 'N') /* spu_xdiff_identity */
#define SPU_XDIFF_SEC_UNIT     SPU_XDIFF_FOURCC('U', 'N', 'I', 'T') /* spu_xdiff_unit */
#define SPU_XDIFF_SEC_REG0     SPU_XDIFF_FOURCC('R', 'E', 'G', '0') /* entry registers, SPU_XDIFF_REG_BYTES */
#define SPU_XDIFF_SEC_LS0      SPU_XDIFF_FOURCC('L', 'S', '_', '0') /* entry local store: extents over zero */
#define SPU_XDIFF_SEC_EFFECTS  SPU_XDIFF_FOURCC('E', 'F', 'C', 'T') /* spu_xdiff_effect[]; may be empty */
#define SPU_XDIFF_SEC_BLOB     SPU_XDIFF_FOURCC('B', 'L', 'O', 'B') /* payload bytes; may be empty */
#define SPU_XDIFF_SEC_REG1     SPU_XDIFF_FOURCC('R', 'E', 'G', '1') /* the authority's end registers */
#define SPU_XDIFF_SEC_LS1      SPU_XDIFF_FOURCC('L', 'S', '_', '1') /* its end local store: extents over LS0 */
#define SPU_XDIFF_SEC_END      SPU_XDIFF_FOURCC('E', 'N', 'D', '_') /* spu_xdiff_end */
#define SPU_XDIFF_SEC_FMASK    SPU_XDIFF_FOURCC('F', 'M', 'S', 'K') /* function live-out and dead-stack masks */
/* Optional, at most once: the verdict that made the runtime write this
 * fixture. A reader that replays ignores it. */
#define SPU_XDIFF_SEC_VERDICT  SPU_XDIFF_FOURCC('V', 'R', 'D', 'T') /* spu_xdiff_verdict_record */

/* Which body ran one side of a comparison. */
typedef struct spu_xdiff_body_id {
    uint8_t  tier;         /* spu_job_tier, with SPU_TIER_TWEAK when it applies */
    uint8_t  reserved[3];
    uint32_t lifter_abi;   /* the SPU lifter ABI the body was generated for; 0 for T0 and T3 */
    uint32_t code_end;     /* the lifter's --code-end for the image, an LS address; 0: none */
    uint32_t reserved2;
    uint64_t lift_options; /* FNV-1a-64 of the lifter's canonical option string; 0 for T0 and T3 */
    uint64_t native_rev;   /* T2, T3: the revision its registrant declares for the native code; else 0 */
} spu_xdiff_body_id;
static_assert(sizeof(spu_xdiff_body_id) == 32, "spu_xdiff_body_id is a file record");

typedef enum spu_xdiff_host_arch {
    SPU_XDIFF_HOST_ARM64  = 1,
    SPU_XDIFF_HOST_X86_64 = 2
} spu_xdiff_host_arch;

/* The SPU helper paths the recording build compiled (spu_helpers.h,
 * spu_context.h). A fixture replayed on a host with other bits and found to
 * differ shows a helper whose result depends on the host. */
#define SPU_XDIFF_ISA_NEON   (1u << 0)
#define SPU_XDIFF_ISA_SSSE3  (1u << 1)
#define SPU_XDIFF_ISA_SSE41  (1u << 2)
#define SPU_XDIFF_ISA_AVX2   (1u << 3)
#define SPU_XDIFF_ISA_FMA    (1u << 4)
#define SPU_XDIFF_ISA_SCALAR (1u << 31) /* SPU_SCALAR_HELPERS: the portable forms only */

/* Section IDEN: what was run, by what, where.
 *
 * The image is not named here by address. An LS address or a guest EA is not
 * identity: one address holds different jobs over time, and one image is
 * staged from several EAs. The identity is the fingerprint of the bytes as
 * they were staged, which are bytes [unit.image_ls, +unit.image_bytes) of the
 * entry local store; a reader recomputes it and rejects a mismatch. */
typedef struct spu_xdiff_identity {
    uint64_t image_fingerprint;  /* spu_workload_fingerprint (FNV-1a-64) over the staged image */
    spu_xdiff_body_id authority; /* the body whose run this is */
    uint32_t effect_port_abi;    /* SPU_EFFECT_PORT_ABI of the recording runtime */
    uint32_t context_census;     /* that runtime's spu_context census (docs/SPU_XDIFF.md, "Shadow contexts") */
    uint32_t host_arch;          /* spu_xdiff_host_arch */
    uint32_t host_isa;           /* SPU_XDIFF_ISA_* */
    uint8_t  toolkit_commit[20]; /* the recording toolkit's git commit, raw; all zero: unknown */
    uint8_t  toolkit_dirty;      /* 1: built from a modified tree */
    uint8_t  float_rz;           /* 1: the run used the native round-toward-zero float mode (SPU_FLOAT_RZ) */
    uint8_t  reserved[2];
} spu_xdiff_identity;
static_assert(sizeof(spu_xdiff_identity) == 80, "spu_xdiff_identity is a file record");

typedef enum spu_xdiff_unit_kind {
    SPU_XDIFF_UNIT_JOB = 1,      /* one job, from the loader's entry state to the end of its run */
    SPU_XDIFF_UNIT_FUNCTION = 2  /* one function call, with its liveness masks */
} spu_xdiff_unit_kind;

/* Which loader staged the unit, and so which end classification applies. */
typedef enum spu_xdiff_profile {
    SPU_XDIFF_PROFILE_JOBCHAIN = 1, /* spu_run_spurs_job*: image at LS 0 */
    SPU_XDIFF_PROFILE_JQ2      = 2  /* spu_run_jq2_job: the JobQueue2 policy module's layout */
} spu_xdiff_profile;

/* Section UNIT: how the entry state was laid out. Everything a job context
 * holds at entry beyond registers and local store is here; the rest of
 * spu_context is zero then (spu_context_init), which is what lets a replaying
 * context be built from a fixture instead of copied from a live one.
 *
 * `layout_*` is spu_job_layout (spu_workload.h) field for field, as the
 * loader's start hook received it. `origin_*` joins the fixture with a trace
 * of the run it came from and is zero for a synthetic fixture. */
typedef struct spu_xdiff_unit {
    uint32_t kind;       /* spu_xdiff_unit_kind */
    uint32_t profile;    /* spu_xdiff_profile */
    uint32_t entry_pc;   /* ctx->pc at entry */
    uint32_t return_ls;  /* job: loader-planted link; function: authority's return PC */
    uint32_t job_rt_ls;  /* ctx->job_rt_ls */
    uint32_t job_rt_ls2; /* ctx->job_rt_ls2 */
    uint32_t image_ea, image_ls, image_bytes, bss_bytes;
    uint32_t desc_ls, desc_bytes;
    uint32_t io_ls, io_bytes;
    uint32_t out_ls, out_bytes;
    uint32_t scratch_ls, scratch_bytes, stack_bytes;
    uint32_t r1;
    uint32_t ctx_ls;
    uint32_t origin_queue;  /* queue index */
    uint32_t origin_worker;
    uint32_t origin_seq;    /* the job's per-queue sequence number (ps3_ftrace.h, JQ2 kinds) */
    uint32_t origin_descriptor_ea;
    uint32_t reserved;
} spu_xdiff_unit;
static_assert(sizeof(spu_xdiff_unit) == 104, "spu_xdiff_unit is a file record");

/* Section FMAS, required for UNIT kind FUNCTION: live-out registers and the
 * dead frame extent below UNIT.r1. Bits use register numbers 0..127. */
typedef struct spu_xdiff_function_mask {
    uint64_t live_out[2];       /* bit r is 1 when $rN is live at any function exit */
    uint32_t dead_stack_bytes;  /* [UNIT.r1 - dead_stack_bytes, UNIT.r1) is dead */
    uint32_t reserved;          /* 0 */
} spu_xdiff_function_mask;
static_assert(sizeof(spu_xdiff_function_mask) == 24, "spu_xdiff_function_mask is a file record");

/* Sections REG0, REG1: registers 0..127, 16 bytes each, in guest byte order:
 * byte k of register r is the byte a `stqd` of r stores at offset k. That is
 * the conversion spu_ls_write128 makes, so the file does not depend on how a
 * host lays out a u128.
 *
 * Sections LS_0, LS_1: a sequence of extents, sorted by `lsa`, not
 * overlapping, each inside local store; an extent's bytes follow its header
 * and are padded with zeros to an 8-byte boundary. LS_0 is laid over a
 * zero-filled store and gives the entry state; LS_1 is laid over the entry
 * state and gives the authority's end state. Any exact cover is valid: how
 * finely a writer splits is not part of the format. */
typedef struct spu_xdiff_ls_extent {
    uint32_t lsa;
    uint32_t bytes; /* > 0; lsa + bytes <= SPU_XDIFF_LS_BYTES */
} spu_xdiff_ls_extent;
static_assert(sizeof(spu_xdiff_ls_extent) == 8, "spu_xdiff_ls_extent is a file record");

/* How a run ended, in terms that do not depend on the tier. An interpreted
 * job and a lifted one arrive at the same normal return with different pc and
 * status (libs/spurs/jq2/executor.cpp, require_normal_return), so the raw
 * fields below locate and the class is what is compared. */
#define SPU_XDIFF_END_CLASSES(X)                                                     \
    X(RETURN,  1, "return")  /* returned through the link the loader planted */      \
    X(STOP,    2, "stop")    /* stop-and-signal; `stop_code` is compared too */      \
    X(HALT,    3, "halt")    /* a halt instruction's condition held */               \
    X(JM_CALL, 4, "jm-call") /* called the job manager's scheduler-switch entry */   \
    X(OTHER,   5, "other")   /* the run ended anywhere else */

typedef enum spu_xdiff_end_class {
#define SPU_XDIFF_END_ENUM(name, value, id) SPU_XDIFF_END_##name = value,
    SPU_XDIFF_END_CLASSES(SPU_XDIFF_END_ENUM)
#undef SPU_XDIFF_END_ENUM
    SPU_XDIFF_END_INVALID = 255
} spu_xdiff_end_class;

/* The id of an end class, or "invalid-end". Never NULL. */
static inline const char* spu_xdiff_end_id(uint32_t end_class)
{
    switch (end_class) {
#define SPU_XDIFF_END_ID(name, value, id) case value: return id;
    SPU_XDIFF_END_CLASSES(SPU_XDIFF_END_ID)
#undef SPU_XDIFF_END_ID
    default: return "invalid-end";
    }
}

/* Section END_. */
typedef struct spu_xdiff_end {
    uint32_t end_class; /* spu_xdiff_end_class */
    uint32_t stop_code; /* ctx->stop_code */
    uint32_t pc;        /* ctx->pc & SPU_LS_MASK, as the tier left it */
    uint32_t status;    /* ctx->status, as the tier left it */
} spu_xdiff_end;
static_assert(sizeof(spu_xdiff_end) == 16, "spu_xdiff_end is a file record");

#define SPU_XDIFF_NO_EFFECT 0xFFFFFFFFu
/* `where` of a difference with no byte position (a scalar field of a request,
 * the end class); reports print it as null. A real position is a payload
 * byte, a register index or an LS address, all far below it. */
#define SPU_XDIFF_NO_WHERE  0xFFFFFFFFu

/* Section VRDT, and the result of one comparison in memory.
 *
 * `effect_index` is the transcript record the subject failed at
 * (EFFECT_EXTRA: the record count; an end-state reason: SPU_XDIFF_NO_EFFECT)
 * and `subject` what it issued there (kind 0: nothing). `where` narrows it:
 * EFFECT_PAYLOAD and EFFECT_REQUEST of a list, the first differing byte of
 * the payload; END_REGS, the register; END_LS, the local-store address.
 * `want` and `got` are the authority's and the subject's 16 bytes from
 * there. */
typedef struct spu_xdiff_verdict_record {
    uint32_t verdict;      /* spu_xdiff_verdict */
    uint32_t reason;       /* spu_xdiff_reason */
    uint32_t match;        /* spu_xdiff_match */
    uint32_t effect_index;
    spu_xdiff_body_id subject_body;
    spu_xdiff_effect subject;
    uint32_t where;
    uint32_t subject_pc;   /* the subject's ctx->pc when the verdict was reached */
    uint8_t  want[16];
    uint8_t  got[16];
    uint64_t budget;       /* budget in its counter unit: instructions (T0) or backward transfers (T1); 0: none */
    uint64_t budget_spent;
} spu_xdiff_verdict_record;
static_assert(sizeof(spu_xdiff_verdict_record) == 144, "spu_xdiff_verdict_record is a file record");

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_XDIFF_H */

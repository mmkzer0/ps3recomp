/*
 * ps3recomp - an xdiff fixture in memory, and its file form
 *
 * A fixture is a job's entry state, effects and end state, or one function
 * call's entry/exit state and liveness masks (docs/SPU_XDIFF.md section 7;
 * layouts in spu_xdiff.h). This module owns the in-memory form, the payload
 * store, the encoder and the validating decoder. It runs no SPU code: the
 * recorder fills a fixture (spu_xdiff_recorder.h), the replay reads one
 * (spu_xdiff_replay.h).
 *
 * A fixture file is an input like any other and may be truncated, corrupt or
 * hostile. The decoder checks everything section 7.8 lists before it trusts a
 * count or an offset, and answers with a reason, never with a partial fixture.
 */
#ifndef SPU_XDIFF_FIXTURE_H
#define SPU_XDIFF_FIXTURE_H

#include "../spu_xdiff.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_xdiff_blob_index; /* the encoder-side payload table; private */

/* One fixture. Local stores are whole (SPU_XDIFF_LS_BYTES each) in memory;
 * only the file form stores them as extents. Registers are in the file's
 * guest byte order (spu_xdiff.h, REG0/REG1). */
typedef struct spu_xdiff_fixture {
    spu_xdiff_identity identity;
    spu_xdiff_unit     unit;
    spu_xdiff_function_mask function_mask; /* FMAS, for function units */
    uint8_t            reg0[SPU_XDIFF_REG_BYTES]; /* entry registers */
    uint8_t*           ls0;                       /* entry local store, owned */
    spu_xdiff_effect*  effects;                   /* the transcript, owned */
    uint32_t           effect_count;
    uint32_t           effect_cap;
    uint8_t*           blob;                      /* payload bytes the effects refer to, owned */
    uint32_t           blob_bytes;
    uint32_t           blob_cap;
    uint32_t           live_limit;                /* 0: none. Else the most effects * 40 + blob_bytes may reach while recording; also bounds both allocations. Set after the recorder begins (clear zeroes it) */
    uint8_t            reg1[SPU_XDIFF_REG_BYTES]; /* the authority's end registers */
    uint8_t*           ls1;                       /* its end local store, owned */
    spu_xdiff_end      end;
    int                has_verdict;               /* 1: `verdict` is the VRDT section */
    spu_xdiff_verdict_record verdict;
    spu_xdiff_code     code;                      /* CODE, v3 function units; count 0: absent */
    uint8_t*           staged;                    /* STAG: the staged image bytes (unit.image_bytes of them) when code.count != 0, owned */
    uint32_t           staged_bytes;
    struct spu_xdiff_blob_index* blob_index;      /* deduplication of payloads; NULL until the first one */
} spu_xdiff_fixture;

/* Why a call failed: a reason of spu_xdiff.h (FIXTURE_FORMAT or
 * FIXTURE_IDENTITY for the decoder, CAPTURE_LIMIT when a bound was hit while
 * filling, UNIT_CODE_MODIFIED from set_code) and one line for a human, naming
 * the section, record or offset. */
typedef struct spu_xdiff_error {
    uint32_t reason;
    char     detail[200];
} spu_xdiff_error;

/* Zeroes `f` and allocates both local stores (zero-filled). Returns 0, or -1
 * when memory is short (f is then empty and safe to free). */
int  spu_xdiff_fixture_init(spu_xdiff_fixture* f);
/* Frees what `f` owns and zeroes it. Safe on a zeroed or failed fixture. */
void spu_xdiff_fixture_free(spu_xdiff_fixture* f);
/* Empties the transcript and payloads and zeroes everything else, keeping the
 * allocations, so one fixture can be refilled job after job. */
void spu_xdiff_fixture_clear(spu_xdiff_fixture* f);

/* Appends `size` payload bytes and returns their offset in BLOB. A payload
 * already stored (same length and bytes, found by hash and confirmed by
 * comparing) returns the earlier offset (section 7.2). Returns
 * SPU_XDIFF_NO_BLOB when `size` is 0, when memory is short, or when the store
 * would exceed `max_blob_bytes` (0: SPU_XDIFF_MAX_BLOB_BYTES); the caller
 * tells the cases apart with spu_xdiff_fixture_blob_failed. */
uint32_t spu_xdiff_fixture_add_blob(spu_xdiff_fixture* f, const void* bytes, uint32_t size,
                                    uint32_t max_blob_bytes);
/* 1 when the last add_blob with a nonzero size stored nothing. */
int spu_xdiff_fixture_blob_failed(const spu_xdiff_fixture* f);

/* Appends one record (copied). Returns 0, or -1 when memory is short or the
 * transcript would exceed SPU_XDIFF_MAX_EFFECTS. */
int spu_xdiff_fixture_add_effect(spu_xdiff_fixture* f, const spu_xdiff_effect* e);

/* The file form. `*out` is malloc'd and owned by the caller; the checksum is
 * set. Fails (CAPTURE_LIMIT) only on memory or a record the decoder would
 * refuse, which the error names: the encoder never writes a file the decoder
 * rejects. */
int spu_xdiff_fixture_encode(const spu_xdiff_fixture* f, uint8_t** out, size_t* out_bytes,
                             spu_xdiff_error* err);
/* Encodes and writes `path` through `<path>.<pid>.<n>.partial` (created exclusively, unique per
 * process and attempt) and a rename, so a reader never sees half a fixture. */
int spu_xdiff_fixture_write(const spu_xdiff_fixture* f, const char* path, spu_xdiff_error* err);

/* Attaches section CODE (and the STAG bytes it is checked against) to a FUNCTION
 * fixture whose unit and ls0 are filled. `staged_ls` is the parent job's entry
 * local store; its image span hashes to identity.image_fingerprint. Validates
 * the ranges (1..SPU_XDIFF_CODE_RANGES_MAX, 4-aligned, nonzero, sorted,
 * non-overlapping, inside [unit.image_ls, +unit.image_bytes)), then requires
 * ls0 == staged_ls on every range: the first differing byte refuses with
 * UNIT_CODE_MODIFIED naming its LS address. A malformed range list or a non-function
 * unit is FIXTURE_FORMAT, memory CAPTURE_LIMIT. On success `f->code` and `f->staged`
 * (a copy of staged_ls over the image span) are filled; on failure `f` is unchanged.
 * Writing the fixture then emits version 3. */
int spu_xdiff_fixture_set_code(spu_xdiff_fixture* f, const spu_xdiff_code_range* ranges, uint32_t count,
                               const uint8_t* staged_ls, spu_xdiff_error* err);

/* The identity rule of the decoder, for a fixture held in memory: FIXTURE_FORMAT for a malformed
 * unit span, CODE or STAG, FIXTURE_IDENTITY when the bytes do not name `identity.image_fingerprint`.
 * With code.count == 0 it hashes the entry store over the image span (job fixtures, version-1 and
 * version-2 function fixtures); with CODE it requires STAG to hash to the fingerprint and ls0 to
 * equal STAG on every range, and never hashes the span. Returns 0 when the fixture is consistent. */
int spu_xdiff_fixture_check_identity(const spu_xdiff_fixture* f, spu_xdiff_error* err);

/* Validates `bytes` against section 7.8 and fills `f` (which the call
 * initialises). On failure `f` is empty and `err->reason` is FIXTURE_FORMAT,
 * UNSUPPORTED_VERSION, or FIXTURE_IDENTITY when a well-formed fixture's image
 * bytes do not hash to its `image_fingerprint` (a version-3 function unit with
 * CODE: its STAG bytes do not, or LS_0 differs from STAG on a CODE range). A
 * zstd frame is refused by name: the runtime links no decompressor; the
 * corpus driver decodes. */
int spu_xdiff_fixture_decode(const uint8_t* bytes, size_t n, spu_xdiff_fixture* f, spu_xdiff_error* err);

/* Compatibility probe: decode only files no newer than `max_version`. A
 * version-1 reader uses 1 and reports a version-2 file as unsupported before
 * interpreting its sections; a version-2 reader does the same for a version-3
 * file. The normal decoder accepts versions 1, 2 and 3. */
int spu_xdiff_fixture_decode_max_version(const uint8_t* bytes, size_t n, uint32_t max_version,
                                         spu_xdiff_fixture* f, spu_xdiff_error* err);
/* Reads and decodes a file. A file that cannot be opened is NO_FIXTURE. */
int spu_xdiff_fixture_read(const char* path, spu_xdiff_fixture* f, spu_xdiff_error* err);

/* The file name of section 7.2, `<fingerprint as 16 hex digits>-q<queue>-s<seq>.spujob`.
 * Returns the length snprintf would write; truncation is the caller's error. */
int spu_xdiff_fixture_name(char* buf, size_t n, uint64_t image_fingerprint, uint32_t queue, uint32_t seq);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_XDIFF_FIXTURE_H */

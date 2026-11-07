/*
 * ps3recomp - which presents a frame dump writes, and what it calls the file
 *
 * The Vulkan backend's PS3RECOMP_VK_DUMP_LIST / _RANGE selection and the
 * `<base>_s<present_seq>_f<flip_id>.ppm` file name, kept apart from the
 * backend so the contract is testable without a GPU. The Metal backend names
 * its files the same way (PS3RECOMP_METAL_FRAME_DUMP) from its own copy; a
 * diff of presented frames across the two backends compares by that name.
 *
 * Pure: no environment, no files, no process exit. The caller reads the two
 * variables and turns a failure into the stop that names the variable.
 */
#ifndef PS3RECOMP_RSX_FRAME_SELECT_H
#define PS3RECOMP_RSX_FRAME_SELECT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Present numbers one list may name. More is an error, never a silent cut:
 * a frame the user asked for would come out missing with nothing to say why. */
#define RSX_FRAME_SELECT_LIST_MAX 32u

typedef struct rsx_frame_select {
    int      have_range;
    uint32_t range_lo, range_hi;                 /* inclusive */
    int      have_list;
    uint32_t list[RSX_FRAME_SELECT_LIST_MAX];
    unsigned n_list;
} rsx_frame_select;

typedef enum rsx_frame_select_status {
    RSX_FRAME_SELECT_OK = 0,
    RSX_FRAME_SELECT_BAD_RANGE,     /* not `<first>-<last>` (or `:`) with first <= last, both in 0..2^32-1 */
    RSX_FRAME_SELECT_BAD_LIST,      /* an empty entry, a non-number, a sign, trailing text, or a value over 2^32-1 */
    RSX_FRAME_SELECT_LIST_TOO_LONG  /* more than RSX_FRAME_SELECT_LIST_MAX entries; *count says how many */
} rsx_frame_select_status;

/* Parse the two variables' values (NULL or "" is unset). Entries are decimal
 * or 0x-prefixed hex, separated by commas, spaces allowed around them. A list
 * wins over a range when both are set, as in the Metal backend. On a failure
 * *out names nothing (no range, no list) and, for LIST_TOO_LONG only, *count
 * is the number of entries found. */
rsx_frame_select_status rsx_frame_select_parse(rsx_frame_select* out, const char* range, const char* list,
                                               unsigned* count);

/* True when present `seq` is one the selection names. A selection with neither
 * a list nor a range names nothing: the caller's default decides. */
int rsx_frame_select_wanted(const rsx_frame_select* s, uint32_t seq);

/* "<base>_s00042_f00041.ppm". A trailing ".ppm" on the base is replaced, and
 * `no_flip` (RSX_FRAME_NO_FLIP) names the flip "none". Returns the length the
 * full name needs, snprintf-style: >= n means it did not fit and `out` holds a
 * truncated string. */
size_t rsx_frame_dump_name(char* out, size_t n, const char* base, uint32_t present_seq, uint32_t flip_id,
                           uint32_t no_flip);

#ifdef __cplusplus
}
#endif

#endif

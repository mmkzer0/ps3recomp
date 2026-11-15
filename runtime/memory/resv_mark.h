/*
 * ps3recomp - the reserved-by-an-SPU mark, readable inline
 *
 * One bit per 128-byte line of the 32-bit guest address space, set by GETLLAR
 * (resv_mark_spu, reservation.c) and never cleared, and the armed flag in front
 * of it that stays zero until the first mark. The PPU store paths ask it before
 * every guest store, so the read is inline here rather than a call into
 * reservation.c; resv_marked_spu (reservation.h) is the same read out of line,
 * for the callers that do not sit on a store path.
 *
 * Plain objects accessed only through the __atomic builtins, because this
 * header is included from C++ and from clang-cl C, where _Atomic is not
 * portable, and without ppu_context.h, which the loader (a translation unit
 * that declares its own ppu_context through ppu_recomp.h) cannot include. The
 * orders are the model's: the set is an atomic OR followed by a store of the
 * flag, the read two relaxed loads. Nothing outside reservation.c writes them.
 */
#ifndef PS3_RESV_MARK_H
#define PS3_RESV_MARK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RESV_LINE_SHIFT 7
#define RESV_MARK_BYTES (1u << (32 - RESV_LINE_SHIFT - 3))

extern unsigned char resv_spu_marks[RESV_MARK_BYTES];
extern int           resv_spu_armed;

/* Is the line marked? One load of a flag that stays zero until the first
 * GETLLAR, so a title that runs no SPU code pays one predictable branch. */
static inline int resv_marked_spu_inl(uint32_t ea)
{
    if (!__atomic_load_n(&resv_spu_armed, __ATOMIC_RELAXED)) return 0;
    const uint32_t line = ea >> RESV_LINE_SHIFT;
    return (__atomic_load_n(&resv_spu_marks[line >> 3], __ATOMIC_RELAXED) >> (line & 7)) & 1u;
}

#ifdef __cplusplus
}
#endif

#endif /* PS3_RESV_MARK_H */

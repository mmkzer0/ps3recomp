/*
 * ps3recomp - the spu_context census
 *
 * A shadow context is built from a fixture, never copied from a live one
 * (docs/SPU_XDIFF.md section 9). That is only sound while every member of
 * spu_context a job's entry state sets is carried by the fixture. The census
 * gives every member one class; tools/test_spu_context_census.py compares this
 * table with the compiler's record layout, so a new member fails the test
 * until someone classifies it.
 */
#ifndef SPU_XDIFF_CENSUS_H
#define SPU_XDIFF_CENSUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum spu_xdiff_member_class {
    SPU_XDIFF_MEMBER_CARRIED = 1, /* set at job entry and carried by a fixture (REG0, LS_0, UNIT) */
    SPU_XDIFF_MEMBER_ZERO    = 2, /* zero at job entry (spu_context_init), so a built context matches */
    SPU_XDIFF_MEMBER_HOST    = 3  /* host-only state no job reads: wait handles, pointers, counters */
} spu_xdiff_member_class;

typedef struct spu_xdiff_member {
    const char* name;   /* the member's name in spu_context, as the compiler prints it */
    uint32_t    offset; /* offsetof(spu_context, name) */
    uint32_t    klass;  /* spu_xdiff_member_class */
} spu_xdiff_member;

/* Every member of spu_context, in declaration order. */
const spu_xdiff_member* spu_xdiff_context_members(size_t* count);

/* FNV-1a-32 over sizeof(spu_context) and, per member in order, its offset and
 * class. A fixture stores it (IDEN.context_census); a replay under another
 * census is reported, and still runs while the port ABI and the carried
 * members are unchanged. */
uint32_t spu_xdiff_context_census(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_XDIFF_CENSUS_H */

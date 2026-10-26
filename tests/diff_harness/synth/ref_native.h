/* The host-native reference implementation of synlib (syn_spec.h): the "HLE"
 * side of the synthetic differential-harness test. Declared with the C types the
 * HLE adapter (runtime/ppu/ppu_hle_adapt.hpp) marshals the guest registers by. */
#ifndef SYN_REF_NATIVE_H
#define SYN_REF_NATIVE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t  synObjInit(uint32_t obj, uint32_t name, uint32_t flags, uint32_t capacity);
int32_t  synObjCommand(uint32_t obj, uint32_t cmd, uint32_t arg);
int32_t  synObjPush(uint32_t obj, uint32_t n);
int32_t  synBufChecksum(uint32_t buf, uint32_t words, uint32_t out);
uint64_t synBits(uint64_t a, uint64_t b, uint32_t c);
int32_t  synFpu(uint32_t in, uint32_t out);

/* The import: a host helper the module calls (the harness forwards the call here). */
void     synHostMemset(uint32_t dst, int32_t value, uint32_t n);

#ifdef __cplusplus
}
#endif
#endif

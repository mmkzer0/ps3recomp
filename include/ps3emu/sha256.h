/*
 * ps3recomp - SHA-256 (FIPS 180-4)
 *
 * Identity checks only: which executable a runtime was lifted from. Not for
 * anything that needs a constant-time or hardware-accelerated hash.
 */

#ifndef PS3EMU_SHA256_H
#define PS3EMU_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3_SHA256_DIGEST_SIZE 32
#define PS3_SHA256_HEX_SIZE    65   /* 64 lowercase hex digits + NUL */

typedef struct ps3_sha256_ctx {
    uint32_t state[8];
    uint64_t bytes;          /* total message length so far */
    uint8_t  block[64];
    size_t   block_len;
} ps3_sha256_ctx;

void ps3_sha256_init(ps3_sha256_ctx* ctx);
void ps3_sha256_update(ps3_sha256_ctx* ctx, const void* data, size_t len);
void ps3_sha256_final(ps3_sha256_ctx* ctx, uint8_t out[PS3_SHA256_DIGEST_SIZE]);

/* One shot: lowercase hex digest of data[0..len), NUL-terminated. */
void ps3_sha256_hex(const void* data, size_t len, char out[PS3_SHA256_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_SHA256_H */

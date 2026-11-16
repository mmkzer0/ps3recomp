/* Portable RSX DXT1/2/3/4/5 block decoder. */
#ifndef PS3RECOMP_RSX_BC_DECODE_H
#define PS3RECOMP_RSX_BC_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RSX_BC_FORMAT_BC1 = 1, /* DXT1 */
    RSX_BC_FORMAT_BC2 = 2, /* DXT2/DXT3; no premultiplied-alpha transform */
    RSX_BC_FORMAT_BC3 = 3  /* DXT4/DXT5; no premultiplied-alpha transform */
} rsx_bc_format;

/* Decode row-major 4x4 BC blocks into RGBA8 rows. The compressed source is
 * tightly packed by block row, as rsx_texture_decode() produces for 2D faces
 * and as the draw engine's VTC slice path produces for 3D slices. Partial
 * right and bottom blocks are clipped to width and height. Destination rows
 * may be padded; blocks and destination must not overlap.
 * RGBA channel order matches the native BC sample before its view swizzle.
 *
 * Returns 1 on success and 0 for invalid arguments, insufficient source bytes,
 * or unrepresentable row arithmetic. Validation completes before the first
 * destination write. */
int rsx_bc_decode(const uint8_t *blocks, size_t blocks_size,
                  uint32_t width, uint32_t height, rsx_bc_format format,
                  uint8_t *rgba, size_t rgba_pitch);

#ifdef __cplusplus
}
#endif
#endif

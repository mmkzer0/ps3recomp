#include "rsx_bc_decode.h"

#include <limits.h>

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le48(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 6u; ++i)
        value |= (uint64_t)p[i] << (8u * i);
    return value;
}

static uint8_t expand5(unsigned value)
{
    return (uint8_t)((value << 3) | (value >> 2));
}

static uint8_t expand6(unsigned value)
{
    return (uint8_t)((value << 2) | (value >> 4));
}

/* The Metal oracle pins Q8 coefficients; ideal rational thirds differ by a byte. */
static uint8_t lerp_q8(uint8_t c0, uint8_t c1, unsigned weight0)
{
    return (uint8_t)(((unsigned)c0 * weight0 +
                      (unsigned)c1 * (256u - weight0) + 128u) >> 8);
}

static void color_palette(const uint8_t *block, unsigned color_offset,
                          int bc1, uint8_t colors[4][4], int *three_color)
{
    const uint16_t endpoint0 = read_le16(block + color_offset);
    const uint16_t endpoint1 = read_le16(block + color_offset + 2u);
    const uint8_t c0[3] = {
        expand5((endpoint0 >> 11) & 31u),
        expand6((endpoint0 >> 5) & 63u),
        expand5(endpoint0 & 31u)
    };
    const uint8_t c1[3] = {
        expand5((endpoint1 >> 11) & 31u),
        expand6((endpoint1 >> 5) & 63u),
        expand5(endpoint1 & 31u)
    };
    *three_color = bc1 && endpoint0 <= endpoint1;
    for (unsigned lane = 0; lane < 3u; ++lane) {
        colors[0][lane] = c0[lane];
        colors[1][lane] = c1[lane];
        if (*three_color) {
            colors[2][lane] = (uint8_t)(((unsigned)c0[lane] + c1[lane] + 1u) / 2u);
            colors[3][lane] = 0;
        } else {
            colors[2][lane] = lerp_q8(c0[lane], c1[lane], 172u);
            colors[3][lane] = lerp_q8(c0[lane], c1[lane], 84u);
        }
    }
    colors[0][3] = 255u;
    colors[1][3] = 255u;
    colors[2][3] = 255u;
    colors[3][3] = *three_color ? 0u : 255u;
}

static void alpha_palette_bc3(const uint8_t *block, uint8_t alpha[8])
{
    /* These Q8 coefficients come from the exhaustive Metal alpha readback. */
    static const uint8_t six_value_weights0[6] = { 220u, 184u, 148u, 108u, 72u, 36u };
    static const uint8_t five_value_weights0[4] = { 204u, 152u, 104u, 52u };
    const unsigned a0 = block[0];
    const unsigned a1 = block[1];
    alpha[0] = (uint8_t)a0;
    alpha[1] = (uint8_t)a1;
    if (a0 > a1) {
        for (unsigned i = 0; i < 6u; ++i)
            alpha[i + 2u] = lerp_q8((uint8_t)a0, (uint8_t)a1,
                                    six_value_weights0[i]);
    } else {
        for (unsigned i = 0; i < 4u; ++i)
            alpha[i + 2u] = lerp_q8((uint8_t)a0, (uint8_t)a1,
                                    five_value_weights0[i]);
        alpha[6] = 0u;
        alpha[7] = 255u;
    }
}

static int checked_mul_size(size_t a, size_t b, size_t *out)
{
    if (b != 0u && a > SIZE_MAX / b) return 0;
    *out = a * b;
    return 1;
}

int rsx_bc_decode(const uint8_t *blocks, size_t blocks_size,
                  uint32_t width, uint32_t height, rsx_bc_format format,
                  uint8_t *rgba, size_t rgba_pitch)
{
    size_t block_bytes;
    switch (format) {
    case RSX_BC_FORMAT_BC1: block_bytes = 8u; break;
    case RSX_BC_FORMAT_BC2:
    case RSX_BC_FORMAT_BC3: block_bytes = 16u; break;
    default: return 0;
    }
    if (!blocks || !rgba || width == 0u || height == 0u) return 0;

    const uint32_t blocks_w = width / 4u + (width % 4u != 0u);
    const uint32_t blocks_h = height / 4u + (height % 4u != 0u);
    size_t src_row_bytes, src_bytes, dst_row_bytes;
    if (!checked_mul_size((size_t)blocks_w, block_bytes, &src_row_bytes) ||
        !checked_mul_size(src_row_bytes, (size_t)blocks_h, &src_bytes) ||
        !checked_mul_size((size_t)width, 4u, &dst_row_bytes) ||
        src_bytes > blocks_size || rgba_pitch < dst_row_bytes ||
        (height > 1u && rgba_pitch > (SIZE_MAX - dst_row_bytes) / (height - 1u)))
        return 0;

    for (uint32_t by = 0; by < blocks_h; ++by) {
        for (uint32_t bx = 0; bx < blocks_w; ++bx) {
            const uint8_t *block = blocks + (size_t)by * src_row_bytes +
                                   (size_t)bx * block_bytes;
            const unsigned color_offset = format == RSX_BC_FORMAT_BC1 ? 0u : 8u;
            uint8_t colors[4][4];
            int three_color;
            color_palette(block, color_offset, format == RSX_BC_FORMAT_BC1,
                          colors, &three_color);
            const uint32_t color_indices = read_le32(block + color_offset + 4u);
            uint64_t alpha_indices = 0;
            uint8_t alpha[8];
            if (format == RSX_BC_FORMAT_BC3) {
                alpha_indices = read_le48(block + 2u);
                alpha_palette_bc3(block, alpha);
            }

            for (unsigned p = 0; p < 16u; ++p) {
                const uint32_t x = bx * 4u + (p & 3u);
                const uint32_t y = by * 4u + (p >> 2);
                if (x >= width || y >= height) continue;
                const unsigned color_index = (color_indices >> (2u * p)) & 3u;
                uint8_t *pixel = rgba + (size_t)y * rgba_pitch + (size_t)x * 4u;
                pixel[0] = colors[color_index][0];
                pixel[1] = colors[color_index][1];
                pixel[2] = colors[color_index][2];
                if (format == RSX_BC_FORMAT_BC1) {
                    pixel[3] = colors[color_index][3];
                } else if (format == RSX_BC_FORMAT_BC2) {
                    const unsigned alpha4 =
                        (block[p >> 1] >> ((p & 1u) * 4u)) & 15u;
                    pixel[3] = (uint8_t)(alpha4 * 17u);
                } else {
                    const unsigned alpha_index =
                        (unsigned)(alpha_indices >> (3u * p)) & 7u;
                    pixel[3] = alpha[alpha_index];
                }
            }
        }
    }
    return 1;
}

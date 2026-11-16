#include "rsx_bc_test_corpus.h"

#include <stdlib.h>
#include <string.h>

static uint16_t pack565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_color(uint8_t *block, unsigned offset, uint16_t c0, uint16_t c1)
{
    put16(block + offset, c0);
    put16(block + offset + 2u, c1);
    uint32_t indices = 0;
    for (unsigned p = 0; p < 16u; ++p)
        indices |= (uint32_t)(p & 3u) << (2u * p);
    for (unsigned byte = 0; byte < 4u; ++byte)
        block[offset + 4u + byte] = (uint8_t)(indices >> (8u * byte));
}

static int image_init(rsx_bc_test_image *image, uint32_t block_count,
                      uint32_t blocks_w, uint32_t block_bytes)
{
    memset(image, 0, sizeof(*image));
    if (blocks_w == 0u || block_bytes == 0u) return 0;
    image->blocks_w = blocks_w;
    image->blocks_h = block_count / blocks_w + (block_count % blocks_w != 0u);
    image->block_count = block_count;
    image->block_bytes = block_bytes;
    image->width = blocks_w * 4u;
    image->height = image->blocks_h * 4u;
    image->row_bytes = (size_t)blocks_w * block_bytes;
    image->size = image->row_bytes * image->blocks_h;
    image->bytes = calloc(1, image->size);
    return image->bytes != NULL;
}

static uint8_t *image_block(rsx_bc_test_image *image, uint32_t index)
{
    const uint32_t by = index / image->blocks_w;
    const uint32_t bx = index % image->blocks_w;
    return image->bytes + (size_t)by * image->row_bytes +
           (size_t)bx * image->block_bytes;
}

void rsx_bc_test_image_free(rsx_bc_test_image *image)
{
    if (!image) return;
    free(image->bytes);
    memset(image, 0, sizeof(*image));
}

int rsx_bc_test_make_color_image(rsx_bc_test_image *image)
{
    /* Blue and green cover every ordered endpoint pair in both BC1 modes.
     * Red determines the packed endpoint ordering, so only valid pairs occur. */
    const uint32_t block_count = 32u * 32u + 32u +
                                 64u * 64u * 2u + 32u * 32u * 2u;
    if (!image_init(image, block_count, 256u, 8u)) return 0;
    uint32_t block_index = 0;
    for (unsigned channel = 0; channel < 3u; ++channel) {
        const unsigned limit = channel == 1u ? 64u : 32u;
        for (unsigned mode = 0; mode < 2u; ++mode) {
            for (unsigned e0 = 0; e0 < limit; ++e0) {
                for (unsigned e1 = 0; e1 < limit; ++e1) {
                    if (channel == 0u && ((mode == 0u && e0 < e1) ||
                                          (mode == 1u && e0 > e1)))
                        continue;
                    unsigned r0, g0, b0, r1, g1, b1;
                    if (channel == 0u) {
                        r0 = e0; r1 = e1;
                        g0 = mode == 0u ? 63u : 0u;
                        g1 = mode == 0u ? 0u : 63u;
                        b0 = 7u; b1 = 23u;
                    } else if (channel == 1u) {
                        r0 = mode == 0u ? 31u : 0u;
                        r1 = mode == 0u ? 0u : 31u;
                        g0 = e0; g1 = e1;
                        b0 = 7u; b1 = 23u;
                    } else {
                        r0 = mode == 0u ? 31u : 0u;
                        r1 = mode == 0u ? 0u : 31u;
                        g0 = 11u; g1 = 43u;
                        b0 = e0; b1 = e1;
                    }
                    put_color(image_block(image, block_index++), 0u,
                              pack565(r0, g0, b0), pack565(r1, g1, b1));
                }
            }
        }
    }
    if (block_index != block_count) {
        rsx_bc_test_image_free(image);
        return 0;
    }
    return 1;
}

int rsx_bc_test_make_bc2_alpha_image(rsx_bc_test_image *image)
{
    if (!image_init(image, 4u, 4u, 16u)) return 0;
    for (unsigned color_index = 0; color_index < 4u; ++color_index) {
        uint8_t *block = image_block(image, color_index);
        uint64_t alpha = 0;
        for (unsigned p = 0; p < 16u; ++p)
            alpha |= (uint64_t)p << (4u * p);
        for (unsigned byte = 0; byte < 8u; ++byte)
            block[byte] = (uint8_t)(alpha >> (8u * byte));
        put_color(block, 8u, pack565(31u, 63u, color_index * 7u),
                  pack565(0u, 0u, 31u - color_index * 7u));
        uint32_t indices = 0;
        for (unsigned p = 0; p < 16u; ++p)
            indices |= color_index << (2u * p);
        for (unsigned byte = 0; byte < 4u; ++byte)
            block[12u + byte] = (uint8_t)(indices >> (8u * byte));
    }
    return 1;
}

int rsx_bc_test_make_bc3_alpha_image(rsx_bc_test_image *image)
{
    if (!image_init(image, 256u * 256u, 256u, 16u)) return 0;
    uint32_t block_index = 0;
    for (unsigned a0 = 0; a0 < 256u; ++a0) {
        for (unsigned a1 = 0; a1 < 256u; ++a1) {
            uint8_t *block = image_block(image, block_index++);
            block[0] = (uint8_t)a0;
            block[1] = (uint8_t)a1;
            uint64_t alpha_indices = 0;
            for (unsigned p = 0; p < 16u; ++p)
                alpha_indices |= (uint64_t)(p & 7u) << (3u * p);
            for (unsigned byte = 0; byte < 6u; ++byte)
                block[2u + byte] = (uint8_t)(alpha_indices >> (8u * byte));
            const uint16_t c0 = pack565(a0 & 31u, a1 & 63u, a0 & 31u);
            const uint16_t c1 = pack565(a1 & 31u, a0 & 63u, a1 & 31u);
            put_color(block, 8u, c0, c1);
        }
    }
    return block_index == image->block_count;
}

void rsx_bc_test_make_partial_block(uint8_t block[16], rsx_bc_format format,
                                    unsigned mip_level)
{
    memset(block, 0, 16u);
    const uint16_t c0 = pack565(25u - mip_level * 3u,
                                49u - mip_level * 5u,
                                17u + mip_level * 3u);
    const uint16_t c1 = pack565(4u + mip_level * 2u,
                                8u + mip_level * 7u,
                                2u + mip_level);
    if (format == RSX_BC_FORMAT_BC1) {
        put_color(block, 0u, c0, c1);
    } else if (format == RSX_BC_FORMAT_BC2) {
        uint64_t alpha = 0;
        for (unsigned p = 0; p < 16u; ++p)
            alpha |= (uint64_t)((p + mip_level * 3u) & 15u) << (4u * p);
        for (unsigned byte = 0; byte < 8u; ++byte)
            block[byte] = (uint8_t)(alpha >> (8u * byte));
        put_color(block, 8u, c0, c1);
    } else {
        block[0] = (uint8_t)(223u - mip_level * 17u);
        block[1] = (uint8_t)(33u + mip_level * 19u);
        uint64_t alpha_indices = 0;
        for (unsigned p = 0; p < 16u; ++p)
            alpha_indices |= (uint64_t)((p + mip_level) & 7u) << (3u * p);
        for (unsigned byte = 0; byte < 6u; ++byte)
            block[2u + byte] = (uint8_t)(alpha_indices >> (8u * byte));
        put_color(block, 8u, c0, c1);
    }
}

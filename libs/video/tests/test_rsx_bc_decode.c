/* Portable BC decoder tests: synthetic exhaustive palettes and seeded blocks. */
#include "rsx_bc_decode.h"
#include "rsx_bc_test_corpus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
static unsigned s_failures;

#define CHECK(condition, label) do { \
    ++s_checks; \
    if (!(condition)) { \
        ++s_failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (label)); \
    } \
} while (0)

static uint16_t load16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t load32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t load48(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 6u; ++i)
        value |= (uint64_t)p[i] << (8u * i);
    return value;
}

static unsigned endpoint_component(uint16_t endpoint, unsigned lane, unsigned *bits)
{
    if (lane == 0u) {
        *bits = 5u;
        return (endpoint >> 11) & 31u;
    }
    if (lane == 1u) {
        *bits = 6u;
        return (endpoint >> 5) & 63u;
    }
    *bits = 5u;
    return endpoint & 31u;
}

static uint8_t endpoint_byte(unsigned value, unsigned bits)
{
    if (bits == 5u) return (uint8_t)((value << 3) | (value >> 2));
    return (uint8_t)((value << 2) | (value >> 4));
}

static uint8_t interpolate_color(uint8_t c0, uint8_t c1, unsigned index)
{
    const unsigned weight0 = index == 2u ? 172u : 84u;
    return (uint8_t)(((unsigned)c0 * weight0 +
                      (unsigned)c1 * (256u - weight0) + 128u) >> 8);
}

static uint8_t interpolate_alpha(uint8_t a0, uint8_t a1, unsigned index)
{
    static const uint8_t six_value_weights0[6] = { 220u, 184u, 148u, 108u, 72u, 36u };
    static const uint8_t five_value_weights0[4] = { 204u, 152u, 104u, 52u };
    if (a0 > a1) {
        const unsigned weight0 = six_value_weights0[index - 2u];
        return (uint8_t)(((unsigned)a0 * weight0 +
                          (unsigned)a1 * (256u - weight0) + 128u) >> 8);
    }
    if (index <= 5u) {
        const unsigned weight0 = five_value_weights0[index - 2u];
        return (uint8_t)(((unsigned)a0 * weight0 +
                          (unsigned)a1 * (256u - weight0) + 128u) >> 8);
    }
    return index == 6u ? 0u : 255u;
}

static void reference_texel(const uint8_t *block, rsx_bc_format format,
                            unsigned p, uint8_t rgba[4])
{
    const unsigned color_offset = format == RSX_BC_FORMAT_BC1 ? 0u : 8u;
    const uint16_t c0 = load16(block + color_offset);
    const uint16_t c1 = load16(block + color_offset + 2u);
    const uint32_t color_indices = load32(block + color_offset + 4u);
    const unsigned color_index = (color_indices >> (2u * p)) & 3u;
    const int three_color = format == RSX_BC_FORMAT_BC1 && c0 <= c1;

    for (unsigned lane = 0; lane < 3u; ++lane) {
        unsigned bits;
        const unsigned v0 = endpoint_component(c0, lane, &bits);
        const unsigned v1 = endpoint_component(c1, lane, &bits);
        const uint8_t e0 = endpoint_byte(v0, bits);
        const uint8_t e1 = endpoint_byte(v1, bits);
        if (color_index < 2u) {
            rgba[lane] = color_index == 0u ? e0 : e1;
        } else if (three_color && color_index == 3u) {
            rgba[lane] = 0u;
        } else if (three_color) {
            rgba[lane] = (uint8_t)(((unsigned)e0 + e1 + 1u) / 2u);
        } else {
            rgba[lane] = interpolate_color(e0, e1, color_index);
        }
    }

    if (format == RSX_BC_FORMAT_BC1) {
        rgba[3] = three_color && color_index == 3u ? 0u : 255u;
    } else if (format == RSX_BC_FORMAT_BC2) {
        const unsigned alpha4 = (block[p >> 1] >> ((p & 1u) * 4u)) & 15u;
        rgba[3] = (uint8_t)(alpha4 * 17u);
    } else {
        const uint64_t alpha_indices = load48(block + 2u);
        const unsigned alpha_index = (unsigned)(alpha_indices >> (3u * p)) & 7u;
        if (alpha_index < 2u)
            rgba[3] = block[alpha_index];
        else
            rgba[3] = interpolate_alpha(block[0], block[1], alpha_index);
    }
}

static const uint8_t *image_block(const rsx_bc_test_image *image,
                                  uint32_t x, uint32_t y)
{
    return image->bytes + (size_t)(y / 4u) * image->row_bytes +
           (size_t)(x / 4u) * image->block_bytes;
}

static int check_image(rsx_bc_format format, const rsx_bc_test_image *image,
                       const char *name)
{
    const size_t row_bytes = (size_t)image->width * 4u;
    const size_t dst_pitch = row_bytes + 13u;
    const size_t output_size = dst_pitch * image->height;
    uint8_t *output = malloc(output_size + 16u);
    if (!output) {
        fprintf(stderr, "FAIL %s output allocation\n", name);
        ++s_failures;
        return 0;
    }
    memset(output, 0xA5, output_size + 16u);
    CHECK(rsx_bc_decode(image->bytes, image->size, image->width, image->height,
                        format, output, dst_pitch) == 1, name);

    for (uint32_t y = 0; y < image->height; ++y) {
        const uint8_t *destination = output + (size_t)y * dst_pitch;
        for (uint32_t x = 0; x < image->width; ++x) {
            uint8_t expected[4];
            reference_texel(image_block(image, x, y), format,
                           (y & 3u) * 4u + (x & 3u), expected);
            for (unsigned lane = 0; lane < 4u; ++lane) {
                ++s_checks;
                if (destination[(size_t)x * 4u + lane] != expected[lane]) {
                    if (s_failures < 8u)
                        fprintf(stderr, "FAIL %s (%u,%u) lane%u got=%u want=%u\n",
                                name, x, y, lane,
                                destination[(size_t)x * 4u + lane], expected[lane]);
                    ++s_failures;
                }
            }
        }
        for (size_t i = row_bytes; i < dst_pitch; ++i) {
            ++s_checks;
            if (destination[i] != 0xA5u) {
                ++s_failures;
                break;
            }
        }
    }
    for (size_t i = output_size; i < output_size + 16u; ++i) {
        ++s_checks;
        if (output[i] != 0xA5u) {
            ++s_failures;
            break;
        }
    }
    free(output);
    return 1;
}

static uint32_t random_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static int make_random_image(rsx_bc_test_image *image, rsx_bc_format format,
                             uint32_t width, uint32_t height, uint32_t *rng)
{
    memset(image, 0, sizeof(*image));
    image->width = width;
    image->height = height;
    image->blocks_w = width / 4u + (width % 4u != 0u);
    image->blocks_h = height / 4u + (height % 4u != 0u);
    image->block_bytes = format == RSX_BC_FORMAT_BC1 ? 8u : 16u;
    image->block_count = image->blocks_w * image->blocks_h;
    image->row_bytes = (size_t)image->blocks_w * image->block_bytes;
    image->size = image->row_bytes * image->blocks_h;
    image->bytes = malloc(image->size);
    if (!image->bytes) return 0;
    for (size_t i = 0; i < image->size; ++i)
        image->bytes[i] = (uint8_t)(random_next(rng) >> 24);
    return 1;
}

static void test_invalid_arguments(void)
{
    uint8_t blocks[16] = { 0 };
    uint8_t output[128];
    memset(output, 0x6D, sizeof(output));
    CHECK(!rsx_bc_decode(NULL, sizeof(blocks), 4, 4, RSX_BC_FORMAT_BC1,
                         output, 16), "reject null source");
    CHECK(!rsx_bc_decode(blocks, sizeof(blocks), 4, 4, RSX_BC_FORMAT_BC1,
                         NULL, 16), "reject null destination");
    CHECK(!rsx_bc_decode(blocks, sizeof(blocks), 0, 4, RSX_BC_FORMAT_BC1,
                         output, 16), "reject zero width");
    CHECK(!rsx_bc_decode(blocks, sizeof(blocks), 4, 0, RSX_BC_FORMAT_BC1,
                         output, 16), "reject zero height");
    CHECK(!rsx_bc_decode(blocks, 7, 4, 4, RSX_BC_FORMAT_BC1,
                         output, 16), "reject truncated BC1 block");
    CHECK(!rsx_bc_decode(blocks, 15, 4, 4, RSX_BC_FORMAT_BC3,
                         output, 16), "reject truncated BC3 block");
    CHECK(!rsx_bc_decode(blocks, sizeof(blocks), 4, 4, RSX_BC_FORMAT_BC1,
                         output, 15), "reject short destination pitch");
    CHECK(!rsx_bc_decode(blocks, sizeof(blocks), 4, 4, (rsx_bc_format)99,
                         output, 16), "reject unknown format");
    for (size_t i = 0; i < sizeof(output); ++i) {
        ++s_checks;
        if (output[i] != 0x6Du) {
            ++s_failures;
            break;
        }
    }
}

int main(void)
{
    rsx_bc_test_image color = { 0 }, bc2 = { 0 }, bc3 = { 0 };
    CHECK(rsx_bc_test_make_color_image(&color), "create exhaustive BC1 color inputs");
    CHECK(rsx_bc_test_make_bc2_alpha_image(&bc2), "create exhaustive BC2 alpha inputs");
    CHECK(rsx_bc_test_make_bc3_alpha_image(&bc3), "create exhaustive BC3 alpha inputs");
    if (color.bytes && bc2.bytes && bc3.bytes) {
        check_image(RSX_BC_FORMAT_BC1, &color, "BC1 exhaustive colors");
        check_image(RSX_BC_FORMAT_BC2, &bc2, "BC2 explicit alpha");
        check_image(RSX_BC_FORMAT_BC3, &bc3, "BC3 exhaustive alpha");
    }

    for (unsigned f = 1u; f <= 3u; ++f) {
        const rsx_bc_format format = (rsx_bc_format)f;
        const unsigned block_bytes = format == RSX_BC_FORMAT_BC1 ? 8u : 16u;
        const uint32_t dims[3] = { 4u, 2u, 1u };
        for (unsigned level = 0; level < 3u; ++level) {
            uint8_t block[16];
            rsx_bc_test_make_partial_block(block, format, level);
            rsx_bc_test_image mip = {
                .bytes = block,
                .blocks_w = 1u,
                .blocks_h = 1u,
                .block_count = 1u,
                .block_bytes = block_bytes,
                .width = dims[level],
                .height = dims[level],
                .row_bytes = block_bytes,
                .size = block_bytes
            };
            check_image(format, &mip, "partial 4x4/2x2/1x1 mip");
        }
    }

    uint32_t rng = 0x6D2B79F5u;
    for (unsigned f = 1u; f <= 3u; ++f) {
        rsx_bc_test_image random = { 0 };
        CHECK(make_random_image(&random, (rsx_bc_format)f, 127u, 129u, &rng),
              "create seeded random BC block corpus");
        if (random.bytes) {
            check_image((rsx_bc_format)f, &random, "seeded random BC blocks");
            rsx_bc_test_image_free(&random);
        }
    }

    test_invalid_arguments();
    printf("rsx_bc_decode: checks=%u failures=%u; exhaustive BC1 blocks=%u, "
           "BC3 endpoint pairs=65536 x 8 indices, random seed=0x6D2B79F5\n",
           s_checks, s_failures, color.block_count);
    rsx_bc_test_image_free(&color);
    rsx_bc_test_image_free(&bc2);
    rsx_bc_test_image_free(&bc3);
    return s_failures ? 1 : 0;
}

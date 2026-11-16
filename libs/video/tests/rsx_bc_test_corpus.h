#ifndef PS3RECOMP_RSX_BC_TEST_CORPUS_H
#define PS3RECOMP_RSX_BC_TEST_CORPUS_H

#include "rsx_bc_decode.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *bytes;
    uint32_t blocks_w;
    uint32_t blocks_h;
    uint32_t block_count;
    uint32_t block_bytes;
    uint32_t width;
    uint32_t height;
    size_t row_bytes;
    size_t size;
} rsx_bc_test_image;

int rsx_bc_test_make_color_image(rsx_bc_test_image *image);
int rsx_bc_test_make_bc2_alpha_image(rsx_bc_test_image *image);
int rsx_bc_test_make_bc3_alpha_image(rsx_bc_test_image *image);
void rsx_bc_test_make_partial_block(uint8_t block[16], rsx_bc_format format,
                                    unsigned mip_level);
void rsx_bc_test_image_free(rsx_bc_test_image *image);

#endif

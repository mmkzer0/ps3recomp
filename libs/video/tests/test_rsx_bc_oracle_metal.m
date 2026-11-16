/* Synthetic Metal oracle for Apple GPU BC decoding; no title data. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "rsx_bc_decode.h"
#include "rsx_bc_test_corpus.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>


static unsigned s_failures;
static uint64_t s_checked_components;
static uint64_t s_off_grid_components;
static float s_max_grid_error;
static uint64_t s_formula_components;
static uint64_t s_formula_mismatches;
static uint64_t s_decoder_components;
static uint64_t s_decoder_mismatches;

#define CHECK(condition, label) do { \
    if (!(condition)) { \
        ++s_failures; \
        fprintf(stderr, "FAIL %s\n", (label)); \
    } \
} while (0)




static uint16_t load_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t load_le48(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 6u; ++i)
        value |= (uint64_t)p[i] << (8u * i);
    return value;
}

static uint8_t expand_color_endpoint(uint16_t endpoint, unsigned lane)
{
    if (lane == 0u) {
        const unsigned value = (endpoint >> 11) & 31u;
        return (uint8_t)((value << 3) | (value >> 2));
    }
    if (lane == 1u) {
        const unsigned value = (endpoint >> 5) & 63u;
        return (uint8_t)((value << 2) | (value >> 4));
    }
    const unsigned value = endpoint & 31u;
    return (uint8_t)((value << 3) | (value >> 2));
}

static uint8_t measured_color_lerp(uint8_t c0, uint8_t c1, unsigned index)
{
    const unsigned weight0 = index == 2u ? 172u : 84u;
    return (uint8_t)(((unsigned)c0 * weight0 +
                      (unsigned)c1 * (256u - weight0) + 128u) >> 8);
}

static uint8_t measured_alpha_lerp(unsigned a0, unsigned a1, unsigned index)
{
    static const uint8_t six_weights0[6] = { 220u, 184u, 148u, 108u, 72u, 36u };
    static const uint8_t five_weights0[4] = { 204u, 152u, 104u, 52u };
    if (a0 > a1) {
        const unsigned weight0 = six_weights0[index - 2u];
        return (uint8_t)((a0 * weight0 + a1 * (256u - weight0) + 128u) >> 8);
    }
    if (index <= 5u) {
        const unsigned weight0 = five_weights0[index - 2u];
        return (uint8_t)((a0 * weight0 + a1 * (256u - weight0) + 128u) >> 8);
    }
    return index == 6u ? 0u : 255u;
}

static void measured_texel(const uint8_t *block, rsx_bc_format format,
                           unsigned p, uint8_t rgba[4])
{
    const unsigned color_offset = format == RSX_BC_FORMAT_BC1 ? 0u : 8u;
    const uint16_t c0 = load_le16(block + color_offset);
    const uint16_t c1 = load_le16(block + color_offset + 2u);
    const uint32_t indices = load_le32(block + color_offset + 4u);
    const unsigned color_index = (indices >> (2u * p)) & 3u;
    const int three_color = format == RSX_BC_FORMAT_BC1 && c0 <= c1;
    for (unsigned lane = 0; lane < 3u; ++lane) {
        const uint8_t e0 = expand_color_endpoint(c0, lane);
        const uint8_t e1 = expand_color_endpoint(c1, lane);
        if (color_index < 2u)
            rgba[lane] = color_index == 0u ? e0 : e1;
        else if (three_color && color_index == 3u)
            rgba[lane] = 0u;
        else if (three_color)
            rgba[lane] = (uint8_t)(((unsigned)e0 + e1 + 1u) / 2u);
        else
            rgba[lane] = measured_color_lerp(e0, e1, color_index);
    }

    if (format == RSX_BC_FORMAT_BC1) {
        rgba[3] = three_color && color_index == 3u ? 0u : 255u;
    } else if (format == RSX_BC_FORMAT_BC2) {
        const unsigned nibble = (block[p >> 1] >> ((p & 1u) * 4u)) & 15u;
        rgba[3] = (uint8_t)(nibble * 17u);
    } else {
        const uint64_t alpha_indices = load_le48(block + 2u);
        const unsigned alpha_index = (unsigned)(alpha_indices >> (3u * p)) & 7u;
        rgba[3] = alpha_index < 2u ? block[alpha_index] :
                  measured_alpha_lerp(block[0], block[1], alpha_index);
    }
}

static void compare_formula_image(rsx_bc_format format,
                                  const rsx_bc_test_image *image,
                                  const float *hardware_pixels)
{
    if (s_off_grid_components != 0u) return;
    const size_t row_pitch = (size_t)image->width * 4u;
    uint8_t *decoded = malloc(row_pitch * image->height);
    CHECK(decoded != NULL, "CPU decoder output allocation");
    if (!decoded) return;
    const int decoded_ok = rsx_bc_decode(image->bytes, image->size,
                                         image->width, image->height, format,
                                         decoded, row_pitch);
    CHECK(decoded_ok, "CPU decode of the oracle's compressed rows");
    if (!decoded_ok) {
        free(decoded);
        return;
    }

    for (uint32_t y = 0; y < image->height; ++y) {
        for (uint32_t x = 0; x < image->width; ++x) {
            const uint8_t *block = image->bytes +
                (size_t)(y / 4u) * image->row_bytes +
                (size_t)(x / 4u) * image->block_bytes;
            const unsigned p = (y & 3u) * 4u + (x & 3u);
            uint8_t expected[4];
            measured_texel(block, format, p, expected);
            const size_t pixel_offset = ((size_t)y * image->width + x) * 4u;
            const uint8_t *cpu_pixel = decoded + (size_t)y * row_pitch + (size_t)x * 4u;
            for (unsigned lane = 0; lane < 4u; ++lane) {
                const unsigned observed =
                    (unsigned)roundf(hardware_pixels[pixel_offset + lane] * 255.0f);
                ++s_formula_components;
                s_formula_mismatches += observed != expected[lane];
                ++s_decoder_components;
                s_decoder_mismatches += observed != cpu_pixel[lane];
            }
        }
    }
    free(decoded);
}

static void analyze_grid(const char *name, const float *pixels,
                         uint32_t width, uint32_t height)
{
    uint64_t local_off_grid = 0;
    float local_max_error = 0.0f;
    const size_t count = (size_t)width * height * 4u;
    for (size_t i = 0; i < count; ++i) {
        const float value = pixels[i];
        const float scaled = value * 255.0f;
        const float nearest = roundf(scaled);
        const float error = fabsf(scaled - nearest);
        const int byte_exact = value == nearest / 255.0f;
        if (!isfinite(value) || value < 0.0f || value > 1.0f ||
            error > 0.00001f || !byte_exact)
            ++local_off_grid;
        if (error > local_max_error) local_max_error = error;
    }
    s_checked_components += count;
    s_off_grid_components += local_off_grid;
    if (local_max_error > s_max_grid_error) s_max_grid_error = local_max_error;
    printf("Metal readback %s: %ux%u, components=%zu, off-1/255=%llu, max_scaled_error=%.8g\n",
           name, width, height, count, (unsigned long long)local_off_grid,
           (double)local_max_error);
}

static const char *kReadShader =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void read_bc(texture2d<float, access::read> image [[texture(0)]],\n"
    "                    device float4 *out [[buffer(0)]],\n"
    "                    constant uint2 &extent [[buffer(1)]],\n"
    "                    constant uint &level [[buffer(2)]],\n"
    "                    uint2 p [[thread_position_in_grid]]) {\n"
    "  if (p.x >= extent.x || p.y >= extent.y) return;\n"
    "  out[p.y * extent.x + p.x] = image.read(p, level);\n"
    "}\n";

static id<MTLComputePipelineState> make_read_pipeline(id<MTLDevice> device)
{
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:kReadShader];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        fprintf(stderr, "Metal shader compile failed: %s\n",
                error.localizedDescription.UTF8String ?: "unknown error");
        return nil;
    }
    id<MTLFunction> function = [library newFunctionWithName:@"read_bc"];
    if (!function) {
        fputs("Metal shader has no read_bc function\n", stderr);
        return nil;
    }
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline)
        fprintf(stderr, "Metal pipeline creation failed: %s\n",
                error.localizedDescription.UTF8String ?: "unknown error");
    return pipeline;
}

static id<MTLBuffer> read_texture(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                  id<MTLComputePipelineState> pipeline,
                                  id<MTLTexture> texture, uint32_t width,
                                  uint32_t height, uint32_t level)
{
    const size_t byte_count = (size_t)width * height * sizeof(float) * 4u;
    id<MTLBuffer> output = [device newBufferWithLength:byte_count
                                               options:MTLResourceStorageModeShared];
    if (!output) return nil;
    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (!command_buffer || !encoder) return nil;
    const uint32_t extent[2] = { width, height };
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder setBytes:&extent length:sizeof(extent) atIndex:1];
    [encoder setBytes:&level length:sizeof(level) atIndex:2];
    const NSUInteger group_width = pipeline.threadExecutionWidth;
    const NSUInteger max_threads = pipeline.maxTotalThreadsPerThreadgroup;
    const NSUInteger group_height = MAX(1u, MIN((NSUInteger)8u, max_threads / group_width));
    const MTLSize group = MTLSizeMake(group_width, group_height, 1);
    const MTLSize groups = MTLSizeMake((width + group_width - 1u) / group_width,
                                       (height + group_height - 1u) / group_height, 1);
    [encoder dispatchThreadgroups:groups threadsPerThreadgroup:group];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted || command_buffer.error) {
        fprintf(stderr, "Metal read command failed: %s\n",
                command_buffer.error.localizedDescription.UTF8String ?: "unknown error");
        return nil;
    }
    return output;
}

static MTLPixelFormat pixel_format(unsigned format)
{
    switch (format) {
    case 1: return MTLPixelFormatBC1_RGBA;
    case 2: return MTLPixelFormatBC2_RGBA;
    default: return MTLPixelFormatBC3_RGBA;
    }
}

static int run_image(id<MTLDevice> device, id<MTLCommandQueue> queue,
                     id<MTLComputePipelineState> pipeline,
                     MTLPixelFormat pixel, rsx_bc_format format,
                     const rsx_bc_test_image *image, const char *name)
{
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel
                                                          width:image->width
                                                         height:image->height
                                                      mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    CHECK(texture != nil, "compressed Metal texture allocation");
    if (!texture) return 0;
    [texture replaceRegion:MTLRegionMake2D(0, 0, image->width, image->height)
               mipmapLevel:0 withBytes:image->bytes bytesPerRow:image->row_bytes];
    id<MTLBuffer> output = read_texture(device, queue, pipeline, texture,
                                        image->width, image->height, 0);
    CHECK(output != nil, "Metal texture.read compute output");
    if (!output) return 0;
    analyze_grid(name, output.contents, image->width, image->height);
    compare_formula_image(format, image, output.contents);
    return 1;
}


static void run_partial_mips(id<MTLDevice> device, id<MTLCommandQueue> queue,
                             id<MTLComputePipelineState> pipeline,
                             rsx_bc_format format)
{
    const unsigned block_bytes = format == RSX_BC_FORMAT_BC1 ? 8u : 16u;
    const uint32_t dims[3] = { 4u, 2u, 1u };
    const MTLPixelFormat pixel = pixel_format((unsigned)format);
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel
                                                          width:4 height:4 mipmapped:YES];
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    CHECK(texture != nil, "partial mip compressed texture allocation");
    if (!texture) return;
    for (unsigned level = 0; level < 3u; ++level) {
        uint8_t block[16];
        rsx_bc_test_make_partial_block(block, format, level);
        [texture replaceRegion:MTLRegionMake2D(0, 0, dims[level], dims[level])
                   mipmapLevel:level withBytes:block bytesPerRow:block_bytes];
        id<MTLBuffer> output = read_texture(device, queue, pipeline, texture,
                                            dims[level], dims[level], level);
        CHECK(output != nil, "partial mip texture.read output");
        if (output) {
            rsx_bc_test_image mip = {
                .bytes = block, .blocks_w = 1u, .blocks_h = 1u, .block_count = 1u,
                .block_bytes = block_bytes, .width = dims[level], .height = dims[level],
                .row_bytes = block_bytes, .size = block_bytes
            };
            analyze_grid("partial-block mip", output.contents,
                         dims[level], dims[level]);
            compare_formula_image(format, &mip, output.contents);
        }
    }
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            puts("SKIP: no Metal device");
            return 77;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        CHECK(queue != nil, "Metal command queue");
        id<MTLComputePipelineState> pipeline = make_read_pipeline(device);
        CHECK(pipeline != nil, "Metal texture.read pipeline");
        if (!queue || !pipeline) return 1;

        rsx_bc_test_image color = { 0 }, bc2 = { 0 }, bc3 = { 0 };
        CHECK(rsx_bc_test_make_color_image(&color), "BC1 exhaustive color corpus");
        CHECK(rsx_bc_test_make_bc2_alpha_image(&bc2), "BC2 explicit alpha corpus");
        CHECK(rsx_bc_test_make_bc3_alpha_image(&bc3), "BC3 exhaustive alpha corpus");
        if (!color.bytes || !bc2.bytes || !bc3.bytes) {
            rsx_bc_test_image_free(&color);
            rsx_bc_test_image_free(&bc2);
            rsx_bc_test_image_free(&bc3);
            return 1;
        }
        printf("BC1 color inputs: 5-bit blue 1024 ordered pairs x 2 modes x 4 indices; "
               "6-bit green 4096 ordered pairs x 2 modes x 4 indices; "
               "red 1056 valid ordered pairs\n");
        puts("BC2 alpha inputs: all 16 nibbles x all 4 color indices");
        puts("BC3 alpha inputs: all 65536 ordered endpoint pairs x all 8 indices");
        if (!run_image(device, queue, pipeline, MTLPixelFormatBC1_RGBA,
                       RSX_BC_FORMAT_BC1, &color, "BC1 color pairs") ||
            !run_image(device, queue, pipeline, MTLPixelFormatBC2_RGBA,
                       RSX_BC_FORMAT_BC2, &bc2, "BC2 explicit alpha") ||
            !run_image(device, queue, pipeline, MTLPixelFormatBC3_RGBA,
                       RSX_BC_FORMAT_BC3, &bc3, "BC3 alpha pairs")) {
            rsx_bc_test_image_free(&color);
            rsx_bc_test_image_free(&bc2);
            rsx_bc_test_image_free(&bc3);
            return 1;
        }
        for (unsigned format = RSX_BC_FORMAT_BC1;
             format <= RSX_BC_FORMAT_BC3; ++format)
            run_partial_mips(device, queue, pipeline, (rsx_bc_format)format);

        if (s_off_grid_components == 0u) {
            puts("Measured formula: 5/6-bit bit replication; BC1 3-color average "
                 "(sum+1)/2; 4-color Q8 weights 172/84 and 84/172 with +128, >>8.");
            puts("BC3 alpha Q8 weights: six-value 220,184,148,108,72,36; "
                 "five-value 204,152,104,52; +128, >>8; mode-5 tail 0,255.");
            CHECK(s_formula_mismatches == 0u, "hardware equals measured formula");
            CHECK(s_decoder_mismatches == 0u, "CPU decoder equals Metal readback");
        } else {
            puts("Metal output is not exactly RGBA8; stop before selecting RGBA8 storage.");
        }
        printf("ORACLE_RESULT exact_8bit=%s checked_components=%llu "
               "formula_components=%llu formula_mismatches=%llu "
               "cpu_components=%llu cpu_hardware_mismatches=%llu failures=%u\n",
               s_off_grid_components == 0u ? "yes" : "no",
               (unsigned long long)s_checked_components,
               (unsigned long long)s_formula_components,
               (unsigned long long)s_formula_mismatches,
               (unsigned long long)s_decoder_components,
               (unsigned long long)s_decoder_mismatches, s_failures);
        rsx_bc_test_image_free(&color);
        rsx_bc_test_image_free(&bc2);
        rsx_bc_test_image_free(&bc3);
        return s_failures ? 1 : 0;
    }
}

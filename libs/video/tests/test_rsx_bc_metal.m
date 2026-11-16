/* The GPU tier of the BC emulation (libs/video/rsx_metal_bc.m) against the CPU decoder, byte for byte.
 *
 * Synthetic blocks only: the exhaustive colour and alpha sets of the oracle, a seeded random corpus, every width
 * and height from 1 to 9 (all partial-block shapes of a mip chain's tail) and the 4x4/2x2/1x1 mips, for BC1, BC2
 * and BC3. Each decode goes into a texture at a chosen slice, level and depth and is read back with the texture's
 * own bytes; the rest of the texture must stay untouched. Several decodes are in flight at once, so the slot pool
 * is exercised too. Skips (77) without a Metal device. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "rsx_bc_decode.h"
#include "rsx_bc_test_corpus.h"
#include "rsx_metal_bc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_failures;
static uint64_t s_texels;
static uint64_t s_decodes;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        if (s_failures++ < 20) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                                 fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
    } \
} while (0)

static const char* format_name(rsx_bc_format f) { return f == RSX_BC_FORMAT_BC1 ? "BC1" : f == RSX_BC_FORMAT_BC2 ? "BC2" : "BC3"; }

static id<MTLTexture> make_texture(id<MTLDevice> dev, MTLTextureType type, uint32_t w, uint32_t h,
                                   uint32_t depth, uint32_t mips)
{
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType = type;
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    td.width = w; td.height = h; td.depth = depth;
    td.mipmapLevelCount = mips;
    td.arrayLength = 1;
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    return [dev newTextureWithDescriptor:td];
}

/* A texture's contents are not promised to start at zero: give every subresource a pattern, so "untouched"
 * means a decode left it alone. */
enum { FILL = 0x5A };
static void fill_texture(id<MTLTexture> t)
{
    const NSUInteger faces = [t textureType] == MTLTextureTypeCube ? 6 : 1;
    for (NSUInteger level = 0; level < [t mipmapLevelCount]; ++level) {
        const NSUInteger w = MAX(1u, [t width] >> level), h = MAX(1u, [t height] >> level);
        const NSUInteger d = [t textureType] == MTLTextureType3D ? MAX(1u, [t depth] >> level) : 1;
        uint8_t* bytes = malloc(w * h * d * 4u);
        memset(bytes, FILL, w * h * d * 4u);
        for (NSUInteger slice = 0; slice < faces; ++slice) {
            if ([t textureType] == MTLTextureType3D)
                [t replaceRegion:MTLRegionMake3D(0, 0, 0, w, h, d) mipmapLevel:level slice:0 withBytes:bytes
                     bytesPerRow:w * 4u bytesPerImage:w * h * 4u];
            else
                [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level slice:slice withBytes:bytes
                     bytesPerRow:w * 4u bytesPerImage:0];
        }
        free(bytes);
    }
}

/* What the CPU decoder says a level is. */
static uint8_t* expected_rgba(const rsx_bc_test_image* im, rsx_bc_format f)
{
    uint8_t* out = malloc((size_t)im->width * im->height * 4u);
    if (!out || !rsx_bc_decode(im->bytes, im->size, im->width, im->height, f, out, (size_t)im->width * 4u)) {
        free(out);
        return NULL;
    }
    return out;
}

/* The level's bytes as the GPU left them: a blit into a buffer on the same queue, committed after the decode
 * with nothing waited for in between, so a queue that did not order the two would read the texture early. */
static uint8_t* read_back(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> t, NSUInteger slice,
                          NSUInteger level, NSUInteger z, uint32_t w, uint32_t h)
{
    id<MTLBuffer> buf = [dev newBufferWithLength:(NSUInteger)w * h * 4u options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, z)
               sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
        destinationBytesPerRow:(NSUInteger)w * 4u destinationBytesPerImage:(NSUInteger)w * h * 4u];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    uint8_t* out = malloc((size_t)w * h * 4u);
    memcpy(out, [buf contents], (size_t)w * h * 4u);
    return out;
}

static void compare(const char* what, rsx_bc_format f, const uint8_t* got, const uint8_t* want, uint32_t w, uint32_t h)
{
    size_t bad = 0;
    for (size_t i = 0; i < (size_t)w * h * 4u; ++i) {
        if (got[i] != want[i] && bad++ < 3)
            fprintf(stderr, "  %s %s %ux%u byte %zu (texel %zu lane %zu): gpu %u cpu %u\n", what, format_name(f), w, h,
                    i, i / 4u, i % 4u, got[i], want[i]);
    }
    CHECK(bad == 0, "%s %s %ux%u: %zu bytes differ", what, format_name(f), w, h, bad);
    s_texels += (uint64_t)w * h;
}

/* One image decoded into mip 0 of a 2D texture, and (a second decode, in flight with the first) into a
 * cube face / 3D slice at another level. */
static void check_image(id<MTLDevice> dev, id<MTLCommandQueue> q, RsxMetalBc* bc, const rsx_bc_test_image* im,
                        rsx_bc_format f, const char* what)
{
    uint8_t* want = expected_rgba(im, f);
    CHECK(want != NULL, "%s: the CPU decoder refused the image", what);
    if (!want) return;
    id<MTLTexture> t = make_texture(dev, MTLTextureType2D, im->width, im->height, 1, 1);
    CHECK([bc decodeBlocks:im->bytes size:im->size format:f width:im->width height:im->height into:t slice:0
                     level:0 z:0 queue:q completion:nil], "%s: decode refused", what);
    s_decodes++;
    uint8_t* got = read_back(dev, q, t, 0, 0, 0, im->width, im->height);
    compare(what, f, got, want, im->width, im->height);
    free(got);
    free(want);
}

static void check_shapes(id<MTLDevice> dev, id<MTLCommandQueue> q, RsxMetalBc* bc, rsx_bc_format f, uint32_t* rng)
{
    /* Every width and height 1..9: whole blocks, and every partial right and bottom edge. Eight decodes are
     * queued before any is read, into the levels of one 2D texture (mip level = index), then read in order. */
    for (uint32_t h = 1; h <= 9; ++h) {
        for (uint32_t w = 1; w <= 9; ++w) {
            rsx_bc_test_image im;
            CHECK(rsx_bc_test_make_random_image(&im, f, w, h, rng), "random image %ux%u", w, h);
            check_image(dev, q, bc, &im, f, "shape");
            rsx_bc_test_image_free(&im);
        }
    }
}

static void check_levels(id<MTLDevice> dev, id<MTLCommandQueue> q, RsxMetalBc* bc, rsx_bc_format f, uint32_t* rng)
{
    /* A mip chain 37x21 -> 1x1 with the partial tail (4x4, 2x2, 1x1) from the corpus's own mip blocks, all in
     * flight at once; then a cube face, and a 3D slice, each in a texture whose other subresources stay zero. */
    const uint32_t base_w = 37, base_h = 21;
    uint32_t mips = 1;
    while ((base_w >> mips) || (base_h >> mips)) ++mips;
    id<MTLTexture> chain = make_texture(dev, MTLTextureType2D, base_w, base_h, 1, mips);
    id<MTLTexture> cube = make_texture(dev, MTLTextureTypeCube, 16, 16, 1, 2);
    id<MTLTexture> volume = make_texture(dev, MTLTextureType3D, 8, 8, 3, 2);
    fill_texture(chain); fill_texture(cube); fill_texture(volume);
    rsx_bc_test_image levels[16];
    uint8_t* want[16];
    memset(want, 0, sizeof want);
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t w = base_w >> m ? base_w >> m : 1u, h = base_h >> m ? base_h >> m : 1u;
        CHECK(rsx_bc_test_make_random_image(&levels[m], f, w, h, rng), "level image");
        want[m] = expected_rgba(&levels[m], f);
        CHECK([bc decodeBlocks:levels[m].bytes size:levels[m].size format:f width:w height:h into:chain slice:0
                         level:m z:0 queue:q completion:nil], "level %u decode refused", m);
        s_decodes++;
    }
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t w = levels[m].width, h = levels[m].height;
        uint8_t* got = read_back(dev, q, chain, 0, m, 0, w, h);
        compare("mip chain", f, got, want[m], w, h);
        free(got);
        free(want[m]);
        rsx_bc_test_image_free(&levels[m]);
    }

    rsx_bc_test_image face_img, vol_img;
    CHECK(rsx_bc_test_make_random_image(&face_img, f, 8, 8, rng), "face image");
    CHECK(rsx_bc_test_make_random_image(&vol_img, f, 8, 8, rng), "volume image");
    uint8_t* face_want = expected_rgba(&face_img, f);
    uint8_t* vol_want = expected_rgba(&vol_img, f);
    CHECK([bc decodeBlocks:face_img.bytes size:face_img.size format:f width:8 height:8 into:cube slice:4 level:1 z:0
                     queue:q completion:nil], "cube face decode refused");
    CHECK([bc decodeBlocks:vol_img.bytes size:vol_img.size format:f width:8 height:8 into:volume slice:0 level:0 z:1
                     queue:q completion:nil], "volume slice decode refused");
    s_decodes += 2;
    uint8_t* face = read_back(dev, q, cube, 4, 1, 0, 8, 8);
    compare("cube face 4 level 1", f, face, face_want, 8, 8);
    uint8_t* slice = read_back(dev, q, volume, 0, 0, 1, 8, 8);
    compare("volume slice z=1", f, slice, vol_want, 8, 8);
    free(face); free(slice);
    /* Everything else in those textures still holds the fill. */
    uint8_t pattern[16 * 16 * 4];
    memset(pattern, FILL, sizeof pattern);
    uint8_t* other_face = read_back(dev, q, cube, 3, 1, 0, 8, 8);
    CHECK(memcmp(other_face, pattern, 8 * 8 * 4) == 0, "%s: cube face 3 was written", format_name(f));
    uint8_t* other_level = read_back(dev, q, cube, 4, 0, 0, 16, 16);
    CHECK(memcmp(other_level, pattern, sizeof pattern) == 0, "%s: cube face 4 level 0 was written", format_name(f));
    uint8_t* other_z = read_back(dev, q, volume, 0, 0, 2, 8, 8);
    CHECK(memcmp(other_z, pattern, 8 * 8 * 4) == 0, "%s: volume slice 2 was written", format_name(f));
    free(other_face); free(other_level); free(other_z);
    free(face_want); free(vol_want);
    rsx_bc_test_image_free(&face_img);
    rsx_bc_test_image_free(&vol_img);
}

static void check_mip_blocks(id<MTLDevice> dev, id<MTLCommandQueue> q, RsxMetalBc* bc, rsx_bc_format f)
{
    const uint32_t dims[3] = { 4u, 2u, 1u };
    const unsigned block_bytes = f == RSX_BC_FORMAT_BC1 ? 8u : 16u;
    for (unsigned level = 0; level < 3u; ++level) {
        uint8_t block[16];
        rsx_bc_test_make_partial_block(block, f, level);
        rsx_bc_test_image mip = { .bytes = block, .blocks_w = 1u, .blocks_h = 1u, .block_count = 1u,
                                  .block_bytes = block_bytes, .width = dims[level], .height = dims[level],
                                  .row_bytes = block_bytes, .size = block_bytes };
        check_image(dev, q, bc, &mip, f, "4x4/2x2/1x1 mip block");
    }
}

static void check_invalid(id<MTLDevice> dev, id<MTLCommandQueue> q, RsxMetalBc* bc)
{
    uint8_t blocks[16] = { 0xFF, 0xFF, 0xFF, 0xFF, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0 };
    id<MTLTexture> ok = make_texture(dev, MTLTextureType2D, 4, 4, 1, 1);
    fill_texture(ok);
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:4 height:4 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> wrong_format = [dev newTextureWithDescriptor:td];
    CHECK(![bc decodeBlocks:NULL size:8 format:RSX_BC_FORMAT_BC1 width:4 height:4 into:ok slice:0 level:0 z:0 queue:q
                 completion:nil], "reject null blocks");
    CHECK(![bc decodeBlocks:blocks size:7 format:RSX_BC_FORMAT_BC1 width:4 height:4 into:ok slice:0 level:0 z:0
                      queue:q completion:nil], "reject a truncated BC1 block");
    CHECK(![bc decodeBlocks:blocks size:15 format:RSX_BC_FORMAT_BC3 width:4 height:4 into:ok slice:0 level:0 z:0
                      queue:q completion:nil], "reject a truncated BC3 block");
    CHECK(![bc decodeBlocks:blocks size:8 format:RSX_BC_FORMAT_BC1 width:0 height:4 into:ok slice:0 level:0 z:0
                      queue:q completion:nil], "reject zero width");
    CHECK(![bc decodeBlocks:blocks size:8 format:(rsx_bc_format)9 width:4 height:4 into:ok slice:0 level:0 z:0
                      queue:q completion:nil], "reject an unknown format");
    CHECK(![bc decodeBlocks:blocks size:8 format:RSX_BC_FORMAT_BC1 width:4 height:4 into:wrong_format slice:0 level:0
                          z:0 queue:q completion:nil], "reject a texture that is not RGBA8Unorm");
    uint8_t* untouched = read_back(dev, q, ok, 0, 0, 0, 4, 4);
    uint8_t pattern[64];
    memset(pattern, FILL, sizeof pattern);
    CHECK(memcmp(untouched, pattern, sizeof pattern) == 0, "a refused decode left the texture untouched");
    free(untouched);
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { puts("no Metal device: skipped"); return 77; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError* err = nil;
        RsxMetalBc* bc = [[RsxMetalBc alloc] initWithDevice:dev host:NULL error:&err];
        if (!bc) { fprintf(stderr, "FAIL: the GPU decoder did not build: %s\n", [[err localizedDescription] UTF8String]); return 1; }

        rsx_bc_test_image color = { 0 }, bc2 = { 0 }, bc3 = { 0 };
        CHECK(rsx_bc_test_make_color_image(&color), "exhaustive BC1 colours");
        CHECK(rsx_bc_test_make_bc2_alpha_image(&bc2), "exhaustive BC2 alpha");
        CHECK(rsx_bc_test_make_bc3_alpha_image(&bc3), "exhaustive BC3 alpha");
        if (color.bytes) check_image(dev, q, bc, &color, RSX_BC_FORMAT_BC1, "exhaustive BC1 colours");
        if (bc2.bytes) check_image(dev, q, bc, &bc2, RSX_BC_FORMAT_BC2, "exhaustive BC2 alpha");
        if (bc3.bytes) check_image(dev, q, bc, &bc3, RSX_BC_FORMAT_BC3, "exhaustive BC3 alpha");

        uint32_t rng = 0x6D2B79F5u;
        for (unsigned f = 1u; f <= 3u; ++f) {
            const rsx_bc_format format = (rsx_bc_format)f;
            rsx_bc_test_image random;
            CHECK(rsx_bc_test_make_random_image(&random, format, 127u, 129u, &rng), "random corpus");
            if (random.bytes) check_image(dev, q, bc, &random, format, "seeded random blocks 127x129");
            rsx_bc_test_image_free(&random);
            CHECK(rsx_bc_test_make_random_image(&random, format, 1024u, 512u, &rng), "random corpus");
            if (random.bytes) check_image(dev, q, bc, &random, format, "seeded random blocks 1024x512");
            rsx_bc_test_image_free(&random);
            check_shapes(dev, q, bc, format, &rng);
            check_levels(dev, q, bc, format, &rng);
            check_mip_blocks(dev, q, bc, format);
        }
        check_invalid(dev, q, bc);
        CHECK([bc failureCount] == 0, "a decode command buffer failed");

        rsx_bc_test_image_free(&color);
        rsx_bc_test_image_free(&bc2);
        rsx_bc_test_image_free(&bc3);
        printf("rsx_bc_metal: decodes=%llu texels compared=%llu failures=%u\n",
               (unsigned long long)s_decodes, (unsigned long long)s_texels, s_failures);
    }
    return s_failures ? 1 : 0;
}

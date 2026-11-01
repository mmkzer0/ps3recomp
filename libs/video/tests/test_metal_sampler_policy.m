/* Pure policy test: no Metal device, shader compilation, or guest data. */
#include "rsx_metal_sampler_policy.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const MTLPixelFormat alpha[] = {
        MTLPixelFormatRGBA8Unorm, MTLPixelFormatBGRA8Unorm,
        MTLPixelFormatBC1_RGBA, MTLPixelFormatBC2_RGBA, MTLPixelFormatBC3_RGBA,
        MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA32Float
    };
    const MTLPixelFormat opaque[] = {
        MTLPixelFormatR8Unorm, MTLPixelFormatRG8Unorm,
        MTLPixelFormatR16Unorm, MTLPixelFormatRG16Unorm,
        MTLPixelFormatRG16Float, MTLPixelFormatR32Float, MTLPixelFormatInvalid
    };
    for (unsigned support = 0; support <= 1; support++) {
        for (unsigned i = 0; i < sizeof alpha / sizeof alpha[0]; i++)
            assert(rsx_metal_border_policy(alpha[i], 0, support) == RSX_METAL_BORDER_ZERO);
        for (unsigned i = 0; i < sizeof opaque / sizeof opaque[0]; i++)
            assert(rsx_metal_border_policy(opaque[i], 0, support) ==
                   (support ? RSX_METAL_BORDER_NATIVE : RSX_METAL_BORDER_UNSUPPORTED));
        for (unsigned border = 1; border <= 2; border++) {
            for (unsigned i = 0; i < sizeof alpha / sizeof alpha[0]; i++)
                assert(rsx_metal_border_policy(alpha[i], border, support) ==
                       (support ? RSX_METAL_BORDER_NATIVE : RSX_METAL_BORDER_UNSUPPORTED));
            for (unsigned i = 0; i < sizeof opaque / sizeof opaque[0]; i++)
                assert(rsx_metal_border_policy(opaque[i], border, support) ==
                       (support ? RSX_METAL_BORDER_NATIVE : RSX_METAL_BORDER_UNSUPPORTED));
        }
    }
    puts("Metal border policy: alpha, non-alpha, zero/nonzero and capability cases passed");
    return 0;
}

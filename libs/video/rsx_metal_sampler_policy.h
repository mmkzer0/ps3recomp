#ifndef PS3RECOMP_RSX_METAL_SAMPLER_POLICY_H
#define PS3RECOMP_RSX_METAL_SAMPLER_POLICY_H

#import <Metal/Metal.h>

typedef enum {
    RSX_METAL_BORDER_NATIVE,
    RSX_METAL_BORDER_ZERO,
    RSX_METAL_BORDER_UNSUPPORTED
} rsx_metal_border_mode;

/* MTLSampler.h: clamp-to-zero is transparent only for alpha-bearing formats.
 * Keep this classification shared with the policy test, independent of a GPU. */
static inline int rsx_metal_format_has_alpha(MTLPixelFormat format)
{
    switch (format) {
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBC1_RGBA:
    case MTLPixelFormatBC2_RGBA:
    case MTLPixelFormatBC3_RGBA:
    case MTLPixelFormatRGBA16Float:
    case MTLPixelFormatRGBA32Float:
        return 1;
    default:
        return 0;
    }
}

static inline rsx_metal_border_mode rsx_metal_border_policy(
    MTLPixelFormat format, unsigned border, int supports_border)
{
    if (border == 0 && rsx_metal_format_has_alpha(format))
        return RSX_METAL_BORDER_ZERO;
    return supports_border ? RSX_METAL_BORDER_NATIVE : RSX_METAL_BORDER_UNSUPPORTED;
}

#endif

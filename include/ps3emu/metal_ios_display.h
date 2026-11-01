#ifndef PS3EMU_METAL_IOS_DISPLAY_H
#define PS3EMU_METAL_IOS_DISPLAY_H

#import <Metal/Metal.h>
#import <UIKit/UIKit.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The app owns the UIKit lifecycle; the backend uses this view's drawable layer. */
UIView *ps3recomp_metal_ios_view(CGRect frame);
CAMetalLayer *ps3recomp_metal_ios_configure_layer(id<MTLDevice> device,
                                                   uint32_t width,
                                                   uint32_t height);

#ifdef __cplusplus
}
#endif

#endif

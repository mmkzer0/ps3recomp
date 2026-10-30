#import "ps3emu/metal_ios_display.h"

#import <QuartzCore/QuartzCore.h>

@interface PS3RecompMetalView : UIView
@end

@implementation PS3RecompMetalView

+ (Class)layerClass
{
    return [CAMetalLayer class];
}

@end

static PS3RecompMetalView *s_view;

UIView *ps3recomp_metal_ios_view(CGRect frame)
{
    NSCAssert([NSThread isMainThread], @"UIKit display view must be created on the main thread");
    if (!s_view) {
        s_view = [[PS3RecompMetalView alloc] initWithFrame:frame];
        s_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        s_view.contentScaleFactor = UIScreen.mainScreen.scale;
        s_view.backgroundColor = UIColor.blackColor;
        s_view.opaque = YES;
        CAMetalLayer *layer = (CAMetalLayer *)s_view.layer;
        layer.contentsGravity = kCAGravityResizeAspect;
        layer.opaque = YES;
    }
    return s_view;
}

CAMetalLayer *ps3recomp_metal_ios_configure_layer(id<MTLDevice> device,
                                                   uint32_t width,
                                                   uint32_t height)
{
    __block CAMetalLayer *result = nil;
    void (^configure)(void) = ^{
        if (!s_view || !device || !width || !height)
            return;
        CAMetalLayer *layer = (CAMetalLayer *)s_view.layer;
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.drawableSize = CGSizeMake(width, height);
        result = layer;
    };
    if ([NSThread isMainThread])
        configure();
    else
        dispatch_sync(dispatch_get_main_queue(), configure);
    return result;
}

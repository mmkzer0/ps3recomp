#import <stdio.h>
#import "ps3emu/metal_ios_display.h"
#import "rsx_metal_backend.h"

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

/* iOS denies GPU allocation and command submission to an app that is not active, and the backend turns a denied
 * allocation into a handle the guest waits on forever. Tell it when the process may and may not use the GPU: the
 * RSX thread then waits in front of its next allocation or commit instead of failing (host_gpu_gate.h). Resign
 * and background both suspend (they overlap on the way down, and suspend is idempotent); only becoming active
 * resumes. The blocks run on the posting thread, the main thread, and never wait on the GPU or the RSX thread. */
static void ps3recomp_observe_gpu_lifecycle(void)
{
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    void (^suspend)(NSNotification *) = ^(NSNotification *note) {
        (void)note;
        fprintf(stderr, "[RSX metal] app inactive: GPU work paused\n");
        rsx_metal_backend_gpu_suspend();
    };
    void (^resume)(NSNotification *) = ^(NSNotification *note) {
        (void)note;
        fprintf(stderr, "[RSX metal] app active: GPU work resumed\n");
        rsx_metal_backend_gpu_resume();
    };
    [center addObserverForName:UIApplicationWillResignActiveNotification
                        object:nil queue:nil usingBlock:suspend];
    [center addObserverForName:UIApplicationDidEnterBackgroundNotification
                        object:nil queue:nil usingBlock:suspend];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification
                        object:nil queue:nil usingBlock:resume];
    /* An app started in the background has already missed the notification. A launch that is merely not yet
     * active is left alone: it becomes active in a moment and the gate starts open. */
    if (UIApplication.sharedApplication.applicationState == UIApplicationStateBackground)
        rsx_metal_backend_gpu_suspend();
}

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
        ps3recomp_observe_gpu_lifecycle();
    }
    return s_view;
}

CAMetalLayer *ps3recomp_metal_ios_configure_layer(id<MTLDevice> device,
                                                   uint32_t width,
                                                   uint32_t height)
{
    __block CAMetalLayer *result = nil;
    void (^configure)(void) = ^{
        if (!s_view) {
            fprintf(stderr, "[RSX metal] no UIKit view: the app must call "
                            "ps3recomp_metal_ios_view on the main thread before the guest starts\n");
            return;
        }
        if (!device || !width || !height) {
            fprintf(stderr, "[RSX metal] UIKit layer needs a device and a non-zero size "
                            "(device %p, %ux%u)\n", (__bridge void *)device, width, height);
            return;
        }
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

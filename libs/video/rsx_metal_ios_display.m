#import <stdio.h>
#import "ps3emu/metal_ios_display.h"
#import "ps3emu/ps3_log.h"   /* PS3_LOG: tagged lines, stderr bytes unchanged */
#import "../../runtime/platform/host_clock.h"   /* host_clock_pause/resume: guest time stands still while inactive */
#import "rsx_metal_backend.h"
#import <time.h>

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

/* The app is "active" for the guest only while the GPU may be used. iOS keeps mach time running across a suspend and
 * across Control Center, so guest time (host_monotonic_ns: the timebase, system time, timers, usleep deadlines)
 * would jump by the paused duration while the audio mixer and the vblank source, which stand still with it, do not:
 * a title's movie, timed on system time, ran ahead of its audio after an app switch. The same edges therefore
 * stop the guest clock (host_clock.h; audio and vblank follow its paused state) and tell the backend when the
 * process may and may not use the GPU: the RSX thread then waits in front of its next allocation or commit
 * instead of failing (host_gpu_gate.h). Resign and background both suspend (they overlap on the way down, and
 * suspend is idempotent); only becoming active resumes. The blocks run on the posting thread, the main thread,
 * and never wait on the GPU or the RSX thread. */
static uint64_t s_guest_pause_began_ns;   /* __atomic: host uptime when the guest clock was last paused */

/* The clock stops before the gate closes and restarts after it opens: nothing the gate holds back or lets go of
 * ever runs against a guest clock that is in the other state. */
static void ps3recomp_pause_guest_time(const char *why)
{
    if (host_clock_paused())
        return;
    __atomic_store_n(&s_guest_pause_began_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), __ATOMIC_RELAXED);
    host_clock_pause();
    PS3_LOG(PS3_LOG_INFO, "host clock", "%s: guest time paused\n", why);
}

static void ps3recomp_resume_guest_time(void)
{
    if (!host_clock_paused())
        return;
    const uint64_t began = __atomic_load_n(&s_guest_pause_began_ns, __ATOMIC_RELAXED);
    host_clock_resume();
    PS3_LOG(PS3_LOG_INFO, "host clock", "app active: guest time resumed after %llu ms paused\n",
            (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000ull));
}

static void ps3recomp_observe_gpu_lifecycle(void)
{
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    void (^suspend)(NSNotification *) = ^(NSNotification *note) {
        (void)note;
        PS3_LOG(PS3_LOG_INFO, "RSX metal", "app inactive: GPU work paused\n");
        ps3recomp_pause_guest_time("app inactive");
        rsx_metal_backend_gpu_suspend();
    };
    void (^resume)(NSNotification *) = ^(NSNotification *note) {
        (void)note;
        PS3_LOG(PS3_LOG_INFO, "RSX metal", "app active: GPU work resumed\n");
        rsx_metal_backend_gpu_resume();
        ps3recomp_resume_guest_time();
    };
    [center addObserverForName:UIApplicationWillResignActiveNotification
                        object:nil queue:nil usingBlock:suspend];
    [center addObserverForName:UIApplicationDidEnterBackgroundNotification
                        object:nil queue:nil usingBlock:suspend];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification
                        object:nil queue:nil usingBlock:resume];
    /* An app started in the background has already missed the notification. A launch that is merely not yet
     * active is left alone: it becomes active in a moment and the gate starts open. */
    if (UIApplication.sharedApplication.applicationState == UIApplicationStateBackground) {
        ps3recomp_pause_guest_time("app started in background");
        rsx_metal_backend_gpu_suspend();
    }
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
            PS3_LOG(PS3_LOG_INFO, "RSX metal", "no UIKit view: the app must call "
                            "ps3recomp_metal_ios_view on the main thread before the guest starts\n");
            return;
        }
        if (!device || !width || !height) {
            PS3_LOG(PS3_LOG_INFO, "RSX metal", "UIKit layer needs a device and a non-zero size "
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

/*
 * ps3recomp - iOS host pad backend (GameController framework)
 *
 * cellPad's iOS backend. The framework reports connections, disconnections and
 * value changes on whatever queue it is told to; here that is ONE private
 * serial queue, so the three kinds of event cannot interleave. cellPad's poll
 * thread reads a per-port snapshot under a small lock (pad_gc_read) and maps it
 * to the guest's pad state (pad_gc_map).
 *
 * Why one queue: GameController posts connect/disconnect on the main queue and
 * value changes on GCDevice.handlerQueue. A button held while a controller is
 * taken away could otherwise have its last value report re-published after the
 * disconnect cleared the port -- the stuck-pad defect pad_host_release
 * documents. Serializing both, and dropping a report whose controller is no
 * longer bound to the port, closes that window (IOS-LEVELING-PLAN.md finding 3).
 */

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <os/lock.h>

#include <string.h>

#import "pad_gc.h"
#import "ps3emu/ps3_log.h"

/* A port's published snapshot. The controller reference cannot live here: ARC
 * forbids Objective-C object pointers in a C struct, so the module's strong
 * references (s_bound below) are a separate, queue-only array. This struct is
 * plain data, which is why pad_gc_read can copy it under the lock. */
typedef struct {
    pad_gc_report report;
    int           bound;
} pad_gc_slot;

static pad_gc_slot s_slots[PAD_GC_PORTS];
static os_unfair_lock s_lock = OS_UNFAIR_LOCK_INIT;

/* The bound controller per port, strong and owned by this module. Touched only
 * on s_queue (connect, disconnect, value reports, the start sweep). */
static GCController* s_bound[PAD_GC_PORTS];

/* Created once and kept for the process: a notification left over from a
 * stopped session then still runs on the same serial queue as the new
 * session's handlers, never beside them. The generation check drops it. */
static dispatch_queue_t s_queue;
static NSOperationQueue* s_observers_queue;
static id s_connect_observer;
static id s_disconnect_observer;

static int s_started;         /* pad/cellPad thread only */
static int s_running;         /* __atomic: handlers drop work from a stopped session */
static unsigned s_generation; /* __atomic: a stop/start pair invalidates queued notifications */

/* Read the whole extended profile. Called on s_queue only. The D-pad is read
 * as four axes, so both the bits and the pressure values come from the same
 * elements; buttonOptions and the stick clicks do not exist on every
 * controller and message nil to 0 / NO. */
static void pad_gc_read_profile(GCExtendedGamepad* gp, pad_gc_report* out)
{
    out->buttons = 0;
    if (gp.buttonA.isPressed)            out->buttons |= PAD_GC_BTN_A;
    if (gp.buttonB.isPressed)            out->buttons |= PAD_GC_BTN_B;
    if (gp.buttonX.isPressed)            out->buttons |= PAD_GC_BTN_X;
    if (gp.buttonY.isPressed)            out->buttons |= PAD_GC_BTN_Y;
    if (gp.leftShoulder.isPressed)       out->buttons |= PAD_GC_BTN_L1;
    if (gp.rightShoulder.isPressed)      out->buttons |= PAD_GC_BTN_R1;
    if (gp.leftThumbstickButton.isPressed)  out->buttons |= PAD_GC_BTN_L3;
    if (gp.rightThumbstickButton.isPressed) out->buttons |= PAD_GC_BTN_R3;
    if (gp.buttonMenu.isPressed)         out->buttons |= PAD_GC_BTN_MENU;
    if (gp.buttonOptions.isPressed)      out->buttons |= PAD_GC_BTN_OPTIONS;
    if (gp.dpad.up.isPressed)            out->buttons |= PAD_GC_BTN_UP;
    if (gp.dpad.down.isPressed)          out->buttons |= PAD_GC_BTN_DOWN;
    if (gp.dpad.left.isPressed)          out->buttons |= PAD_GC_BTN_LEFT;
    if (gp.dpad.right.isPressed)         out->buttons |= PAD_GC_BTN_RIGHT;

    out->lx = gp.leftThumbstick.xAxis.value;
    out->ly = gp.leftThumbstick.yAxis.value;
    out->rx = gp.rightThumbstick.xAxis.value;
    out->ry = gp.rightThumbstick.yAxis.value;

    out->l2 = gp.leftTrigger.value;
    out->r2 = gp.rightTrigger.value;

    out->cross    = gp.buttonA.value;
    out->circle   = gp.buttonB.value;
    out->square   = gp.buttonX.value;
    out->triangle = gp.buttonY.value;
    out->l1       = gp.leftShoulder.value;
    out->r1       = gp.rightShoulder.value;
    out->up       = gp.dpad.up.value;
    out->down     = gp.dpad.down.value;
    out->left     = gp.dpad.left.value;
    out->right    = gp.dpad.right.value;
}

/* Port holding this controller, -1 if none. Called on s_queue only. */
static int pad_gc_port_of(GCController* controller)
{
    for (int p = 0; p < PAD_GC_PORTS; p++)
        if (s_bound[p] == controller) return p;
    return -1;
}

static const char* pad_gc_vendor(GCController* controller)
{
    const char* name = controller.vendorName.UTF8String;
    return name ? name : "?";
}

static void pad_gc_connect(GCController* controller)
{
    if (!controller) return;
    if (!__atomic_load_n(&s_running, __ATOMIC_ACQUIRE)) return;   /* stopped meanwhile */
    if (pad_gc_port_of(controller) >= 0) return;                   /* already adopted */

    GCExtendedGamepad* gp = controller.extendedGamepad;
    if (!gp) {
        PS3_LOG_N(1, PS3_LOG_INFO, "cellPad",
                  "controller %s has no extended profile: ignored\n", pad_gc_vendor(controller));
        return;
    }

    int port = -1;
    for (int p = 0; p < PAD_GC_PORTS && port < 0; p++)
        if (!s_bound[p]) port = p;
    if (port < 0) {
        PS3_LOG_N(8, PS3_LOG_INFO, "cellPad",
                  "controller %s: every port is taken, ignored\n", pad_gc_vendor(controller));
        return;
    }

    /* The lowest free port is this controller's player number, so a title that
     * asks "which pad is this" sees the same layout a PS3 would report. */
    controller.playerIndex = (GCControllerPlayerIndex)(GCControllerPlayerIndex1 + port);
    s_bound[port] = controller;

    pad_gc_report report;
    pad_gc_read_profile(gp, &report);
    os_unfair_lock_lock(&s_lock);
    s_slots[port].report = report;
    s_slots[port].bound = 1;
    os_unfair_lock_unlock(&s_lock);

    /* Both notifications and this handler run on s_queue, so the identity
     * check below cannot race the disconnect that clears s_bound[port]. */
    controller.handlerQueue = s_queue;
    __weak GCController* weak_controller = controller;
    gp.valueChangedHandler = ^(GCExtendedGamepad* gamepad, GCControllerElement* element) {
        (void)element;
        GCController* c = weak_controller;
        if (!c) return;
        if (!__atomic_load_n(&s_running, __ATOMIC_ACQUIRE)) return;
        const int p = pad_gc_port_of(c);
        if (p < 0) return;   /* unbound: a report that raced the disconnect is dropped */
        pad_gc_report r;
        pad_gc_read_profile(gamepad, &r);
        os_unfair_lock_lock(&s_lock);
        s_slots[p].report = r;
        os_unfair_lock_unlock(&s_lock);
    };

    PS3_LOG_N(16, PS3_LOG_INFO, "cellPad",
              "controller %s connected on port %d\n", pad_gc_vendor(controller), port);
}

static void pad_gc_disconnect(GCController* controller)
{
    if (!controller) return;
    const int port = pad_gc_port_of(controller);
    if (port < 0) return;   /* never bound: no profile or no free port, or already stopped */

    GCExtendedGamepad* gp = controller.extendedGamepad;
    if (gp) gp.valueChangedHandler = nil;

    s_bound[port] = nil;
    os_unfair_lock_lock(&s_lock);
    memset(&s_slots[port], 0, sizeof s_slots[port]);   /* bound = 0, report zeroed */
    os_unfair_lock_unlock(&s_lock);

    PS3_LOG_N(16, PS3_LOG_INFO, "cellPad",
              "controller %s disconnected from port %d\n", pad_gc_vendor(controller), port);
}

void pad_gc_start(void)
{
    if (s_started) return;
    s_started = 1;

    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_queue_attr_t attr =
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        s_queue = dispatch_queue_create("ps3recomp.cellPad.gamecontroller", attr);
    });

    __atomic_store_n(&s_running, 1, __ATOMIC_RELEASE);
    const unsigned gen = __atomic_add_fetch(&s_generation, 1, __ATOMIC_ACQ_REL);

    /* Underlying queue plus one operation at a time: NSNotificationCenter runs
     * the observer blocks serialized on q, exactly like the value handlers. */
    s_observers_queue = [[NSOperationQueue alloc] init];
    s_observers_queue.underlyingQueue = s_queue;
    s_observers_queue.maxConcurrentOperationCount = 1;

    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    s_connect_observer =
        [center addObserverForName:GCControllerDidConnectNotification
                            object:nil
                             queue:s_observers_queue
                        usingBlock:^(NSNotification* note) {
            if (__atomic_load_n(&s_generation, __ATOMIC_ACQUIRE) != gen) return;
            pad_gc_connect(note.object);
        }];
    s_disconnect_observer =
        [center addObserverForName:GCControllerDidDisconnectNotification
                            object:nil
                             queue:s_observers_queue
                        usingBlock:^(NSNotification* note) {
            if (__atomic_load_n(&s_generation, __ATOMIC_ACQUIRE) != gen) return;
            pad_gc_disconnect(note.object);
        }];

    /* Adopt what is already attached; a controller that connects between the
     * observer registration and this sweep is adopted by whichever runs. */
    dispatch_async(s_queue, ^{
        if (__atomic_load_n(&s_generation, __ATOMIC_ACQUIRE) != gen) return;
        for (GCController* c in [GCController controllers]) pad_gc_connect(c);
    });
}

void pad_gc_stop(void)
{
    if (!s_started) return;
    s_started = 0;
    __atomic_store_n(&s_running, 0, __ATOMIC_RELEASE);

    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    if (s_connect_observer) { [center removeObserver:s_connect_observer]; s_connect_observer = nil; }
    if (s_disconnect_observer) { [center removeObserver:s_disconnect_observer]; s_disconnect_observer = nil; }

    /* Wait for a handler that is already running, then unbind on q so the
     * disconnect cannot interleave with a report. Every port reads unbound
     * for a reader afterwards. */
    dispatch_sync(s_queue, ^{
        for (int p = 0; p < PAD_GC_PORTS; p++) {
            GCController* c = s_bound[p];
            if (!c) continue;
            c.extendedGamepad.valueChangedHandler = nil;
            s_bound[p] = nil;
            os_unfair_lock_lock(&s_lock);
            memset(&s_slots[p], 0, sizeof s_slots[p]);
            os_unfair_lock_unlock(&s_lock);
        }
    });
    s_observers_queue = nil;
}

int pad_gc_read(unsigned port, pad_gc_report* out)
{
    if (!out || port >= PAD_GC_PORTS) return 0;
    int bound;
    os_unfair_lock_lock(&s_lock);
    bound = s_slots[port].bound;
    if (bound) *out = s_slots[port].report;
    os_unfair_lock_unlock(&s_lock);
    return bound;
}

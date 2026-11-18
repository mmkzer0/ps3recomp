/*
 * ps3recomp - cellAudio host output on iOS: AVAudioSession + CoreAudio RemoteIO
 *
 * The mix thread (the ring's producer) pushes one 256-frame block per guest
 * period, paced on the monotonic clock by audio_pace.h. The RemoteIO render
 * callback (the ring's consumer) pulls whatever its IO buffer asks for, on the
 * device's clock. The ring between them is the queue audio_pace.h guards the
 * drift against, so this file only has to keep the two sides honest: underrun
 * is silence, overrun is a drop, neither blocks, and both are counted.
 *
 * Session category SoloAmbient: it honours the silent switch (an iPad without
 * one has it in Control Center), mutes other apps' audio, and plays nothing in
 * the background. The IO buffer is requested at 256 frames (one guest period,
 * 5.333 ms) and what the system grants is logged, because audio_pace.h's fixed
 * thresholds absorb a granted buffer of at most 1024 frames (see below).
 *
 * Ownership across a stop (the invariant the whole file keeps): only the
 * consumer advances `tail`, so the producer may reset the ring only while the
 * unit is stopped. The notification observers therefore do not stop, reset or
 * start anything: they set a bit in s_requests and log. The mix thread acts on
 * the bits in audio_ios_service. While the unit is not running, audio_ios_submit
 * drops blocks instead of queuing them, so a restart does not play up to a ring
 * of stale audio, and the mixer, seeing no device, paces on the clock alone.
 */

#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#import <UIKit/UIKit.h>
#import <stdio.h>
#import <string.h>
#import <time.h>

#import "audio_host_ios.h"
#import "audio_pace.h"
#import "audio_ring.h"
#import "ps3emu/ps3_log.h"

#define IOS_RATE_HZ        48000
#define IOS_BLOCK_FRAMES   256u      /* one guest audio period */
#define IOS_RING_FRAMES    4096u     /* 16 blocks; a power of two */
#define IOS_MAX_SLICE      4096u     /* frames per render slice the unit must be ready for */

/* audio_pace.h's HI mark is where the mixer starts skipping periods; the ring must hold that plus one whole IO
 * buffer of the largest size the pacing still absorbs (1024 frames, IOS-LEVELING-PLAN finding 4), or the ring
 * itself would refuse blocks the pacer still calls normal. */
#define IOS_PACED_IO_FRAMES 1024u
_Static_assert(IOS_RING_FRAMES >= AUDIO_PACE_QUEUE_HI_BLOCKS * IOS_BLOCK_FRAMES + IOS_PACED_IO_FRAMES,
               "ring smaller than audio_pace HI plus one IO buffer");
_Static_assert((IOS_RING_FRAMES & (IOS_RING_FRAMES - 1)) == 0, "ring capacity must be a power of two");

/* Requests the observers leave for the mix thread (bits of s_requests). Order between bits is not kept: stop
 * and resume together restart, which is also the right thing after an interruption that ended within a period. */
enum {
    REQ_STOP    = 1u << 0,   /* interruption began: stop the unit, drop blocks */
    REQ_RESUME  = 1u << 1,   /* interruption ended: reset the ring, restart */
    REQ_REBUILD = 1u << 2,   /* media services were reset: every audio object is invalid, rebuild */
    REQ_ENSURE  = 1u << 3,   /* app became active: restart only if the output is down. It also fires at launch
                              * and after Control Center or a notification banner took the focus without an
                              * interruption; restarting a running unit there would drop the queued audio. */
};

static audio_ring s_ring;
static float s_ring_storage[IOS_RING_FRAMES * AUDIO_RING_CHANNELS];

static uint32_t s_requests;              /* __atomic: REQ_* bits, observers set, mix thread exchanges */
static uint32_t s_active;                /* __atomic: unit running and blocks are queued */
static uint32_t s_interruptions;         /* __atomic: began-notifications seen */
static uint64_t s_held_frames;           /* __atomic: frames dropped by submit while inactive */

/* Below are touched by open, close and the mix thread only, which never run at the same time. */
static int s_opened;
static AudioComponentInstance s_unit;
static NSMutableArray<id<NSObject>> *s_observers;
static int s_need_start;                 /* a start was requested and has not succeeded yet */
static uint64_t s_next_try_ns;           /* earliest time of the next start attempt */
static const char *s_start_why = "";
static uint32_t s_starts;

static uint64_t now_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_MONOTONIC);
}

static const char *err_text(NSError *err)
{
    return err.localizedDescription.UTF8String ?: "unknown";
}

/* ------------------------------------------------------------------------ */
/* Logging: aggressive on purpose while the IO size and route behaviour are being tuned. */

static void route_string(char *buf, size_t n)
{
    buf[0] = '\0';
    for (AVAudioSessionPortDescription *port in AVAudioSession.sharedInstance.currentRoute.outputs) {
        if (buf[0])
            strlcat(buf, "+", n);
        strlcat(buf, port.portType.UTF8String ?: "?", n);
    }
    if (!buf[0])
        strlcpy(buf, "none", n);
}

static void log_session(const char *why)
{
    AVAudioSession *session = AVAudioSession.sharedInstance;
    const double rate = session.sampleRate;
    const double duration = session.IOBufferDuration;
    const unsigned frames = (unsigned)(duration * rate + 0.5);
    char route[160];
    route_string(route, sizeof route);
    PS3_LOG_N(200, PS3_LOG_INFO, "cellAudio",
              "ios: %s: sampleRate %.0f Hz, IO buffer %.3f ms (%u frames), outputLatency %.1f ms, output %s\n",
              why, rate, duration * 1000.0, frames, session.outputLatency * 1000.0, route);
    if (frames > IOS_PACED_IO_FRAMES) {
        PS3_LOG_N(50, PS3_LOG_WARN, "cellAudio",
                  "ios: granted IO buffer %u frames is above %u: audio_pace's fixed thresholds (LO 2, HI 6 blocks) "
                  "absorb at most %u frames, so expect catch-ups and underruns (ftrace kinds 19 and 20) and a guest "
                  "audio clock below 187.5 blocks/s; a Bluetooth route is the usual cause\n",
                  frames, IOS_PACED_IO_FRAMES, IOS_PACED_IO_FRAMES);
    }
}

/* ------------------------------------------------------------------------ */
/* The render callback: the ring's consumer, on CoreAudio's real-time thread. Nothing here may block, allocate,
 * message Objective-C or log. */

static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                          UInt32 frames, AudioBufferList *io)
{
    (void)ref; (void)ts; (void)bus;
    if (io->mNumberBuffers < 1 || !io->mBuffers[0].mData)
        return kAudio_ParamError;

    /* The buffer's own size bounds the copy, not the slice count the unit announces. */
    uint32_t n = io->mBuffers[0].mDataByteSize / (AUDIO_RING_CHANNELS * (uint32_t)sizeof(float));
    if (n > frames)
        n = frames;
    const uint32_t got = audio_ring_read(&s_ring, (float *)io->mBuffers[0].mData, n);
    if (got == 0)
        *flags |= kAudioUnitRenderAction_OutputIsSilence;
    /* The stream format is interleaved, so there is one buffer; clear any other the host hands over. */
    for (UInt32 i = 1; i < io->mNumberBuffers; ++i)
        if (io->mBuffers[i].mData)
            memset(io->mBuffers[i].mData, 0, io->mBuffers[i].mDataByteSize);
    return noErr;
}

/* ------------------------------------------------------------------------ */
/* Unit and session. Mix thread, open and close only. */

static void dispose_unit(void)
{
    if (!s_unit)
        return;
    AudioOutputUnitStop(s_unit);
    AudioUnitUninitialize(s_unit);
    AudioComponentInstanceDispose(s_unit);
    s_unit = NULL;
}

static int create_unit(void)
{
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_RemoteIO,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    const char *what = "AudioComponentFindNext";
    OSStatus st = kAudio_ParamError;
    AudioUnit unit = NULL;

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp)
        goto fail;
    what = "AudioComponentInstanceNew";
    if ((st = AudioComponentInstanceNew(comp, &unit)) != noErr)
        goto fail;

    /* Interleaved float stereo at the guest's rate: the unit converts to the hardware rate. */
    AudioStreamBasicDescription fmt = {
        .mSampleRate = IOS_RATE_HZ,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = AUDIO_RING_CHANNELS * sizeof(float),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = AUDIO_RING_CHANNELS * sizeof(float),
        .mChannelsPerFrame = AUDIO_RING_CHANNELS,
        .mBitsPerChannel = 8 * sizeof(float),
    };
    what = "StreamFormat";
    if ((st = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt,
                                   sizeof fmt)) != noErr)
        goto fail;

    /* A granted IO buffer above the default slice (Bluetooth can grant thousands of frames) must not make the
     * unit refuse to render. */
    UInt32 max_slice = IOS_MAX_SLICE;
    what = "MaximumFramesPerSlice";
    if ((st = AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                   &max_slice, sizeof max_slice)) != noErr)
        goto fail;

    AURenderCallbackStruct cb = { .inputProc = render_cb, .inputProcRefCon = NULL };
    what = "SetRenderCallback";
    if ((st = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                                   sizeof cb)) != noErr)
        goto fail;

    what = "AudioUnitInitialize";
    if ((st = AudioUnitInitialize(unit)) != noErr)
        goto fail;

    s_unit = unit;
    return 0;

fail:
    PS3_LOG_N(50, PS3_LOG_ERROR, "cellAudio", "ios: RemoteIO setup failed in %s (OSStatus %d)\n", what, (int)st);
    if (unit)
        AudioComponentInstanceDispose(unit);
    return -1;
}

/* Category, preferences and activation, repeated on every start: after an interruption or a media services
 * reset the session has to be configured and activated again anyway. A refused preference is logged and the
 * granted value is read back afterwards; a refused category or activation is fatal for this attempt. */
static int activate_session(void)
{
    AVAudioSession *session = AVAudioSession.sharedInstance;
    NSError *err = nil;

    if (![session setCategory:AVAudioSessionCategorySoloAmbient error:&err]) {
        PS3_LOG_N(50, PS3_LOG_ERROR, "cellAudio", "ios: setCategory(SoloAmbient) failed: %s\n", err_text(err));
        return -1;
    }
    if (![session setPreferredSampleRate:IOS_RATE_HZ error:&err])
        PS3_LOG_N(20, PS3_LOG_WARN, "cellAudio", "ios: setPreferredSampleRate(%d) refused: %s\n", IOS_RATE_HZ,
                  err_text(err));
    err = nil;
    if (![session setPreferredIOBufferDuration:(double)IOS_BLOCK_FRAMES / IOS_RATE_HZ error:&err])
        PS3_LOG_N(20, PS3_LOG_WARN, "cellAudio", "ios: setPreferredIOBufferDuration(%u frames) refused: %s\n",
                  IOS_BLOCK_FRAMES, err_text(err));
    err = nil;
    if (![session setActive:YES error:&err]) {
        PS3_LOG_N(50, PS3_LOG_ERROR, "cellAudio", "ios: setActive(YES) failed: %s\n", err_text(err));
        return -1;
    }
    return 0;
}

/* Brings the unit to running with an empty ring. `active` is cleared and the unit stopped first (idempotent), so
 * the ring reset has no consumer to race and a failed attempt leaves the mixer on the clock instead of filling
 * a ring nobody drains; this holds for the first start, a restart after an interruption, and an app that became
 * active with the unit still nominally running. */
static int start_output(const char *why)
{
    __atomic_store_n(&s_active, 0, __ATOMIC_RELEASE);
    if (s_unit)
        AudioOutputUnitStop(s_unit);
    else if (create_unit() != 0)
        return -1;

    audio_ring_reset(&s_ring);
    if (activate_session() != 0)
        return -1;

    const OSStatus st = AudioOutputUnitStart(s_unit);
    if (st != noErr) {
        PS3_LOG_N(50, PS3_LOG_ERROR, "cellAudio", "ios: AudioOutputUnitStart failed (OSStatus %d)\n", (int)st);
        return -1;
    }
    s_starts++;
    __atomic_store_n(&s_active, 1, __ATOMIC_RELEASE);
    PS3_LOG_N(200, PS3_LOG_INFO, "cellAudio", "ios: output unit started (%s), start #%u\n", why, s_starts);
    log_session(why);
    return 0;
}

/* Clears `active` first, so submit stops queuing before the consumer stops. */
static void halt_output(const char *why)
{
    __atomic_store_n(&s_active, 0, __ATOMIC_RELEASE);
    if (s_unit)
        AudioOutputUnitStop(s_unit);
    PS3_LOG_N(200, PS3_LOG_INFO, "cellAudio", "ios: output unit stopped (%s)\n", why);
}

/* ------------------------------------------------------------------------ */
/* Observers: record and log, nothing else (ownership rule, top of file). */

static void request(uint32_t bits)
{
    __atomic_fetch_or(&s_requests, bits, __ATOMIC_RELEASE);
}

static void install_observers(void)
{
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    AVAudioSession *session = AVAudioSession.sharedInstance;
    s_observers = [NSMutableArray array];

    [s_observers addObject:[center addObserverForName:AVAudioSessionInterruptionNotification object:session
                                                queue:nil
                                           usingBlock:^(NSNotification *note) {
        const NSUInteger type = [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
        if (type == AVAudioSessionInterruptionTypeBegan) {
            __atomic_fetch_add(&s_interruptions, 1, __ATOMIC_RELAXED);
            PS3_LOG_N(200, PS3_LOG_INFO, "cellAudio", "ios: interruption began\n");
            request(REQ_STOP);
        } else {
            const NSUInteger opts = [note.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
            PS3_LOG_N(200, PS3_LOG_INFO, "cellAudio", "ios: interruption ended (shouldResume %d)\n",
                      (opts & AVAudioSessionInterruptionOptionShouldResume) != 0);
            request(REQ_RESUME);
        }
    }]];

    [s_observers addObject:[center addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                                               object:session queue:nil
                                           usingBlock:^(NSNotification *note) {
        (void)note;
        PS3_LOG_N(50, PS3_LOG_WARN, "cellAudio", "ios: media services were reset\n");
        request(REQ_REBUILD);
    }]];

    [s_observers addObject:[center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil
                                                queue:nil
                                           usingBlock:^(NSNotification *note) {
        (void)note;
        PS3_LOG_N(200, PS3_LOG_INFO, "cellAudio", "ios: app became active\n");
        request(REQ_ENSURE);
    }]];

    [s_observers addObject:[center addObserverForName:AVAudioSessionRouteChangeNotification object:session
                                                queue:nil
                                           usingBlock:^(NSNotification *note) {
        const NSUInteger reason = [note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
        PS3_LOG_N(200, PS3_LOG_INFO, "cellAudio", "ios: route change, reason %lu\n", (unsigned long)reason);
        log_session("route change");
    }]];
}

static void remove_observers(void)
{
    for (id<NSObject> token in s_observers)
        [NSNotificationCenter.defaultCenter removeObserver:token];
    s_observers = nil;
}

/* ------------------------------------------------------------------------ */
/* The C surface (audio_host_ios.h). */

int audio_ios_open(void)
{
    @autoreleasepool {
        if (s_opened)
            return 0;
        if (!audio_ring_init(&s_ring, s_ring_storage, IOS_RING_FRAMES)) {
            PS3_LOG(PS3_LOG_ERROR, "cellAudio", "ios: ring init refused capacity %u\n", IOS_RING_FRAMES);
            return -1;
        }
        __atomic_store_n(&s_requests, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&s_active, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&s_interruptions, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&s_held_frames, 0, __ATOMIC_RELAXED);
        s_need_start = 0;
        s_next_try_ns = 0;
        s_starts = 0;

        s_opened = 1;
        install_observers();
        if (start_output("open") != 0) {
            PS3_LOG(PS3_LOG_ERROR, "cellAudio", "ios: output could not be started; running on the clock alone\n");
            audio_ios_close();
            return -1;
        }
        return 0;
    }
}

void audio_ios_close(void)
{
    @autoreleasepool {
        if (!s_opened)
            return;
        remove_observers();
        __atomic_store_n(&s_active, 0, __ATOMIC_RELEASE);
        dispose_unit();

        NSError *err = nil;
        if (![AVAudioSession.sharedInstance setActive:NO
                                          withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                                error:&err])
            PS3_LOG_N(20, PS3_LOG_WARN, "cellAudio", "ios: setActive(NO) failed: %s\n", err_text(err));

        PS3_LOG_N(10, PS3_LOG_INFO, "cellAudio",
                  "ios: closed after %u start(s), %u interruption(s); ring short %llu events / %llu frames, "
                  "dropped %llu frames, held %llu frames\n",
                  s_starts, __atomic_load_n(&s_interruptions, __ATOMIC_RELAXED),
                  (unsigned long long)audio_ring_short_events(&s_ring),
                  (unsigned long long)audio_ring_short_frames(&s_ring),
                  (unsigned long long)audio_ring_dropped_frames(&s_ring),
                  (unsigned long long)__atomic_load_n(&s_held_frames, __ATOMIC_RELAXED));
        s_need_start = 0;
        s_opened = 0;
    }
}

void audio_ios_service(void)
{
    if (!s_opened)
        return;
    const uint32_t req = __atomic_exchange_n(&s_requests, 0, __ATOMIC_ACQUIRE);
    if (!req && !s_need_start)
        return;

    @autoreleasepool {
        if (req & (REQ_STOP | REQ_REBUILD))
            halt_output((req & REQ_REBUILD) ? "media services reset" : "interruption");
        if (req & REQ_REBUILD)
            dispose_unit();
        if (req & REQ_STOP)
            s_need_start = 0;
        /* A missing interruption-ended (iOS does not promise one) leaves the unit stopped until the app is
         * active again; that is the case this restart is for. */
        if ((req & REQ_ENSURE) && !(req & REQ_STOP) && !__atomic_load_n(&s_active, __ATOMIC_ACQUIRE) &&
            !s_need_start) {
            s_need_start = 1;
            s_next_try_ns = 0;
            s_start_why = "app became active";
        }
        if (req & (REQ_RESUME | REQ_REBUILD)) {
            s_need_start = 1;
            s_next_try_ns = 0;
            s_start_why = (req & REQ_REBUILD) ? "media services reset" : "resume";
        }

        /* A start that fails (a call still holds the session) is tried again once a second until the
         * interruption-ended or app-active request it was waiting for goes through, not on every loop turn. */
        if (s_need_start) {
            const uint64_t now = now_ns();
            if (now >= s_next_try_ns) {
                if (start_output(s_start_why) == 0)
                    s_need_start = 0;
                else
                    s_next_try_ns = now + 1000000000ull;
            }
        }
    }
}

int audio_ios_active(void)
{
    return (int)__atomic_load_n(&s_active, __ATOMIC_ACQUIRE);
}

void audio_ios_submit(const float *stereo, uint32_t frames)
{
    if (!__atomic_load_n(&s_active, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_held_frames, __atomic_load_n(&s_held_frames, __ATOMIC_RELAXED) + frames,
                         __ATOMIC_RELAXED);
        return;
    }
    audio_ring_write(&s_ring, stereo, frames);
}

uint32_t audio_ios_queued_frames(void)
{
    return __atomic_load_n(&s_active, __ATOMIC_ACQUIRE) ? audio_ring_fill(&s_ring) : 0;
}

void audio_ios_rate_note(char *buf, size_t n)
{
    snprintf(buf, n, "ring short %llu events / %llu frames, dropped %llu frames, held %llu frames, "
                     "starts %u, interruptions %u",
             (unsigned long long)audio_ring_short_events(&s_ring),
             (unsigned long long)audio_ring_short_frames(&s_ring),
             (unsigned long long)audio_ring_dropped_frames(&s_ring),
             (unsigned long long)__atomic_load_n(&s_held_frames, __ATOMIC_RELAXED),
             s_starts, __atomic_load_n(&s_interruptions, __ATOMIC_RELAXED));
}

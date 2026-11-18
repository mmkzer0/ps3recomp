/*
 * ps3recomp - cellAudio host output on iOS (CoreAudio RemoteIO), C surface for cellAudio.c
 *
 * The mixer's monotonic clock leads: it mixes one block per guest period and
 * pushes it here. The RemoteIO render callback drains a ring on the device's
 * clock, and the ring's fill is the queue audio_pace.h guards against drift
 * (audio_ios_queued_frames). See docs/MODULES_REFERENCE.md, "cellAudio on iOS".
 *
 * Threads. Every function except audio_ios_active and audio_ios_rate_note is
 * the mix thread's (the ring's producer) once it runs; open and close belong
 * to cellAudioInit and cellAudioQuit, which sequence themselves with the mix
 * thread's creation and join. The notification observers (interruption, media
 * services reset, app active, route change) run on whatever thread posts them
 * and only record a request and log; audio_ios_service, called by the mix
 * thread, performs the stop, ring reset and restart. Only the producer may
 * touch the ring while the unit is stopped, which is what makes the reset safe.
 */

#ifndef PS3EMU_AUDIO_HOST_IOS_H
#define PS3EMU_AUDIO_HOST_IOS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configures the audio session, creates and starts the RemoteIO unit, installs
 * the observers. 0 on success; -1 with every step undone and the reason logged,
 * after which audio_ios_active() is 0 and the mixer runs on the clock alone. */
int audio_ios_open(void);

/* Disposes the unit, removes the observers, deactivates the session. Idempotent. */
void audio_ios_close(void);

/* Runs the work the observers requested: stop on an interruption, ring reset
 * and restart when it ends or the app becomes active, a rebuild after a media
 * services reset. Mix thread, once per loop turn; cheap when nothing is requested. */
void audio_ios_service(void);

/* 1 while the unit runs and submitted blocks are queued; 0 otherwise (not open,
 * interrupted, being restarted). Any thread. */
int audio_ios_active(void);

/* Producer only. Queues `frames` interleaved stereo float frames; what does not
 * fit is dropped and counted, never waited for. While inactive every block is
 * dropped (and counted as held), so the ring holds no stale audio when the unit
 * restarts and the mixer paces on the clock as it does with no device. */
void audio_ios_submit(const float* stereo, uint32_t frames);

/* Frames in the ring: the queue audio_pace.h reads. 0 while inactive. */
uint32_t audio_ios_queued_frames(void);

/* One line of counters for the AUDIO_RATE diagnostic: the ring's underruns and
 * drops, frames held back while inactive, restarts and interruptions. */
void audio_ios_rate_note(char* buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_AUDIO_HOST_IOS_H */

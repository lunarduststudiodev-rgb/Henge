/* The guest's sound on the host (Mac or iOS): the default output device through an AudioUnit
   whose render callback pulls PCM straight from the virtio-snd ring. The unit
   is set to the stream's own format, and CoreAudio converts to the device.
   And the microphone: the default input into the guest's input stream. */
#include "audio_mac.h"

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>

/* The Mac's default output device, or the iOS app's own output (the app sets its audio session to playback). */
#if TARGET_OS_IPHONE
#define HV_OUTPUT_UNIT kAudioUnitSubType_RemoteIO
#else
#define HV_OUTPUT_UNIT kAudioUnitSubType_DefaultOutput
#endif
#include <stdio.h>
#include <string.h>

static AudioComponentInstance unit;
static int unit_running;
static unsigned cur_channels, cur_format, cur_rate;

static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                       UInt32 frames, AudioBufferList *io) {
    (void)flags, (void)ts, (void)bus, (void)frames;
    HvVirtioSnd *d = ref;
    for (UInt32 i = 0; i < io->mNumberBuffers; i++)
        hv_virtio_snd_pull(d, io->mBuffers[i].mData, io->mBuffers[i].mDataByteSize);
    return noErr;
}

static int start(void *opaque, unsigned channels, unsigned format, unsigned rate_hz) {
    HvVirtioSnd *d = opaque;
    if (unit && (channels != cur_channels || format != cur_format || rate_hz != cur_rate)) {
        AudioOutputUnitStop(unit);
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        unit = NULL;
        unit_running = 0;
    }
    if (!unit) {
        AudioComponentDescription desc = {kAudioUnitType_Output, HV_OUTPUT_UNIT,
                                          kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent comp = AudioComponentFindNext(NULL, &desc);
        if (!comp || AudioComponentInstanceNew(comp, &unit) != noErr) return -1;
        unsigned bytes = format == HV_VSND_FMT_S16 ? 2 : 4;
        AudioStreamBasicDescription f = {0};
        f.mSampleRate = rate_hz;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = (format == HV_VSND_FMT_FLOAT ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) |
                         kAudioFormatFlagIsPacked;
        f.mBytesPerPacket = f.mBytesPerFrame = bytes * channels;
        f.mFramesPerPacket = 1;
        f.mChannelsPerFrame = channels;
        f.mBitsPerChannel = bytes * 8;
        AURenderCallbackStruct cb = {render, d};
        if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr ||
            AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr ||
            AudioUnitInitialize(unit) != noErr) {
            fprintf(stderr, "hengevm: sound: the output device refused %u ch, format %u, %u Hz\n", channels, format, rate_hz);
            AudioComponentInstanceDispose(unit);
            unit = NULL;
            return -1;
        }
        cur_channels = channels, cur_format = format, cur_rate = rate_hz;
    }
    if (!unit_running && AudioOutputUnitStart(unit) == noErr) unit_running = 1;
    return unit_running ? 0 : -1;
}

static void stop(void *opaque) {
    (void)opaque;
    if (unit && unit_running) AudioOutputUnitStop(unit); /* waits for the render callback to finish */
    unit_running = 0;
}

/* ---- the microphone: the default input (the Mac's, or the iPad's) through an AudioQueue, on only while the guest
   records. The guest's start and stop come on its CPU thread, which must not wait for CoreAudio (or for the iPad's
   audio session to switch to recording), so the queue is made, started and stopped on a serial queue of its own. */

static dispatch_queue_t mic_q;
static void (*mic_hook)(int on);
static _Atomic int mic_want;          /* the guest records (and the machine isn't paused) */
static _Atomic unsigned mic_fmt[3];   /* channels, format, rate it wants */
static int mic_paused;                /* the guest was recording when the machine paused */
/* on mic_q only: */
static AudioQueueRef mic_queue;
static unsigned mic_cur[3];
static int mic_hooked;                /* the hook was told 1 */

#define MIC_BUFFERS 3
#define MIC_BUFFER_MS 20

static void mic_input(void *ref, AudioQueueRef q, AudioQueueBufferRef buf, const AudioTimeStamp *ts, UInt32 n,
                      const AudioStreamPacketDescription *pd) {
    (void)ts, (void)n, (void)pd;
    HvVirtioSnd *d = ref;
    if (buf->mAudioDataByteSize) hv_virtio_snd_capture(d, buf->mAudioData, buf->mAudioDataByteSize);
    if (atomic_load(&mic_want)) AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

static void mic_close(void) {
    if (mic_queue) {
        AudioQueueStop(mic_queue, true);
        AudioQueueDispose(mic_queue, true);
        mic_queue = NULL;
    }
    if (mic_hooked && mic_hook) mic_hook(0);
    mic_hooked = 0;
}

/* Bring the microphone to what's wanted now (each call looks afresh, so a quick start and stop settles right). */
static void mic_apply(void *ctx) {
    HvVirtioSnd *d = ctx;
    unsigned want[3] = {atomic_load(&mic_fmt[0]), atomic_load(&mic_fmt[1]), atomic_load(&mic_fmt[2])};
    if (!atomic_load(&mic_want)) {
        if (mic_queue || mic_hooked) fprintf(stderr, "hengevm: sound: the microphone is off\n");
        mic_close();
        return;
    }
    if (mic_queue && !memcmp(want, mic_cur, sizeof want)) return;
    if (mic_queue) {
        AudioQueueStop(mic_queue, true);
        AudioQueueDispose(mic_queue, true);
        mic_queue = NULL;
    }
    /* The iPad's audio session records only from here on (the app's hook switches it); then the queue. */
    if (!mic_hooked && mic_hook) mic_hook(1);
    mic_hooked = 1;
    unsigned channels = want[0], format = want[1], rate_hz = want[2], bytes = format == HV_VSND_FMT_S16 ? 2 : 4;
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = rate_hz;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = (format == HV_VSND_FMT_FLOAT ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) |
                     kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = bytes * channels;
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = bytes * 8;
    OSStatus err = AudioQueueNewInput(&f, mic_input, d, NULL, NULL, 0, &mic_queue);
    if (err == noErr) {
        UInt32 size = rate_hz * MIC_BUFFER_MS / 1000 * f.mBytesPerFrame;
        for (int i = 0; i < MIC_BUFFERS && err == noErr; i++) {
            AudioQueueBufferRef b;
            err = AudioQueueAllocateBuffer(mic_queue, size, &b);
            if (err == noErr) err = AudioQueueEnqueueBuffer(mic_queue, b, 0, NULL);
        }
        if (err == noErr) err = AudioQueueStart(mic_queue, NULL);
    }
    if (err != noErr) {
        /* No microphone (none there, or not allowed): the Mac hears silence, by the device's clock. */
        fprintf(stderr, "hengevm: sound: the microphone did not start (%d); the Mac hears silence\n", (int)err);
        mic_close();
        return;
    }
    memcpy(mic_cur, want, sizeof want);
    fprintf(stderr, "hengevm: sound: the microphone is on (%u ch, format %u, %u Hz)\n", channels, format, rate_hz);
}

static int cap_start(void *opaque, unsigned channels, unsigned format, unsigned rate_hz) {
    atomic_store(&mic_fmt[0], channels);
    atomic_store(&mic_fmt[1], format);
    atomic_store(&mic_fmt[2], rate_hz);
    atomic_store(&mic_want, 1);
    dispatch_async_f(mic_q, opaque, mic_apply);
    return 0;
}

static void cap_stop(void *opaque) {
    atomic_store(&mic_want, 0);
    dispatch_async_f(mic_q, opaque, mic_apply);
}

void hv_audio_set_mic_hook(void (*hook)(int on)) {
    mic_hook = hook;
}

static HvVirtioSnd *attached;
static int paused_running; /* the output was playing when the machine paused */

void hv_audio_attach(HvVirtioSnd *d) {
    attached = d;
    if (!mic_q) mic_q = dispatch_queue_create("henge.microphone", DISPATCH_QUEUE_SERIAL);
    d->backend = (HvVirtioSndBackend){start, stop, d, cap_start, cap_stop};
}

void hv_audio_pause(int pause) {
    /* The microphone (and the iPad's recording indicator) is off while the machine is paused. */
    if (pause) {
        mic_paused = atomic_exchange(&mic_want, 0);
        if (mic_paused && attached) dispatch_async_f(mic_q, attached, mic_apply);
    } else if (mic_paused) {
        mic_paused = 0;
        atomic_store(&mic_want, 1);
        if (attached) dispatch_async_f(mic_q, attached, mic_apply);
    }
    if (pause) {
        paused_running = unit && unit_running;
        if (paused_running) AudioOutputUnitStop(unit); /* waits for the render callback to finish */
        unit_running = 0;
        return;
    }
    if (!paused_running) return;
    paused_running = 0;
    if (unit && AudioOutputUnitStart(unit) == noErr) {
        unit_running = 1;
        return;
    }
    /* No output now (another app took it?): play the stream by the clock instead, so the guest's
       buffers keep coming back and nothing in macOS waits on sound for good. */
    fprintf(stderr, "hengevm: sound: the output did not start again after a pause; the Mac plays silently\n");
    if (attached) attached->backend.start = NULL;
}

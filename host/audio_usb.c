/* The USB speaker's sound on the host (Mac or iOS): an output AudioUnit of its own, 48 kHz float stereo, whose
   render callback pulls from the speaker's ring (hv_usb_audio_pull). It runs while macOS streams to the speaker
   (its alternate setting 1 or 2); the volume macOS sets arrives here (the speaker's Feature Unit). */
#include "audio_usb.h"

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>

#if TARGET_OS_IPHONE
#define HV_OUTPUT_UNIT kAudioUnitSubType_RemoteIO
#define HV_DEVICE_VOLUME 1 /* the iPad's system volume is the volume: full strength here */
#else
#define HV_OUTPUT_UNIT kAudioUnitSubType_DefaultOutput
#define HV_DEVICE_VOLUME 0 /* a gain here */
#endif

static HvUsbAudio *spk;
static AudioComponentInstance unit;
static int unit_running, paused_running;
static _Atomic float gain = 1.0f;
static void (*listener)(void *, double, int);
static void *listener_opaque;

static float gain_for(int db256, int mute) {
    if (mute) return 0.0f;
    if (HV_DEVICE_VOLUME) return 1.0f;
    if (db256 <= HV_USB_AUDIO_VOL_MIN) return 0.0f; /* the bottom of the slider is silence, as macOS's own */
    return (float)pow(10.0, db256 / 256.0 / 20.0);
}

static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                       UInt32 frames, AudioBufferList *io) {
    (void)flags, (void)ts, (void)bus;
    HvUsbAudio *a = ref;
    float g = atomic_load_explicit(&gain, memory_order_relaxed);
    for (UInt32 i = 0; i < io->mNumberBuffers; i++) {
        UInt32 n = io->mBuffers[i].mDataByteSize / (2 * sizeof(float));
        if (n > frames) n = frames;
        hv_usb_audio_pull(a, io->mBuffers[i].mData, n, g);
    }
    return noErr;
}

static int make_unit(HvUsbAudio *a) {
    AudioComponentDescription desc = {kAudioUnitType_Output, HV_OUTPUT_UNIT, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &unit) != noErr) return -1;
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = HV_USB_AUDIO_RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = 2 * sizeof(float);
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    AURenderCallbackStruct cb = {render, a};
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr ||
        AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr ||
        AudioUnitInitialize(unit) != noErr) {
        fprintf(stderr, "hengevm: speaker: the output device refused 48 kHz stereo\n");
        AudioComponentInstanceDispose(unit);
        unit = NULL;
        return -1;
    }
    return 0;
}

/* CPU thread: macOS started its stream to the speaker. */
static void start(void *opaque, unsigned bits) {
    HvUsbAudio *a = opaque;
    (void)bits; /* the ring gives float whatever came */
    int db, mute;
    hv_usb_audio_get_volume(a, &db, &mute); /* a restored Mac's volume, or what macOS set before streaming */
    atomic_store_explicit(&gain, gain_for(db, mute), memory_order_relaxed);
    if (!unit && make_unit(a) != 0) return; /* plays nowhere: the ring fills and drops, macOS doesn't wait */
    if (!unit_running && AudioOutputUnitStart(unit) == noErr) unit_running = 1;
}

static void stop(void *opaque) {
    (void)opaque;
    if (unit && unit_running) AudioOutputUnitStop(unit); /* waits for the render callback to finish */
    unit_running = 0;
}

/* CPU thread: macOS set the volume or mute. */
static void volume(void *opaque, int db256, int mute) {
    (void)opaque;
    atomic_store_explicit(&gain, gain_for(db256, mute), memory_order_relaxed);
    void (*cb)(void *, double, int) = __atomic_load_n(&listener, __ATOMIC_ACQUIRE);
    if (cb) cb(__atomic_load_n(&listener_opaque, __ATOMIC_ACQUIRE), hv_usb_audio_db_to_scalar(db256), mute);
}

void hv_audio_usb_attach(HvUsbAudio *a) {
    spk = a;
    a->backend = (HvUsbAudioBackend){start, stop, volume, a};
}

void hv_audio_usb_set_listener(void (*cb)(void *opaque, double level, int mute), void *opaque) {
    __atomic_store_n(&listener, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&listener_opaque, opaque, __ATOMIC_RELEASE);
    __atomic_store_n(&listener, cb, __ATOMIC_RELEASE);
}

void hv_audio_usb_pause(int pause) {
    if (pause) {
        paused_running = unit && unit_running;
        if (paused_running) AudioOutputUnitStop(unit);
        unit_running = 0;
        return;
    }
    if (!paused_running) return;
    paused_running = 0;
    if (unit && AudioOutputUnitStart(unit) == noErr) unit_running = 1;
    else fprintf(stderr, "hengevm: speaker: the output did not start again after a pause; the speaker plays silently\n");
}

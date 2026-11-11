#ifndef HENGEVM_VIRTIO_SND_H
#define HENGEVM_VIRTIO_SND_H

#include <pthread.h>
#include <stdatomic.h>

#include "virtio.h"

/* virtio-snd (virtio 1.2, section 5.14) with an output stream (0) and an input
   stream (1, the microphone): what Virtualization.framework's
   VZVirtioSoundDeviceConfiguration gives a macOS guest with an input and an output
   stream, which AppleVirtIOSound drives. HV_NO_MIC leaves the input out.

   The guest's PCM goes into a ring here; a host backend (CoreAudio on the Mac)
   pulls it out on its own thread with hv_virtio_snd_pull. A transmit buffer goes
   back to the guest only once its audio has been pulled, which is what paces the
   guest's audio clock. Without a backend the ring drains at the stream's rate by
   the wall clock, so the guest still runs at the right speed. */
#define HV_VSND_RING (1u << 18)   /* bytes: over a second of 48 kHz stereo 16-bit */
#define HV_VSND_PENDING 64

/* PCM formats and rates, as the spec numbers them (the ones this device offers). */
#define HV_VSND_FMT_S16 5
#define HV_VSND_FMT_S32 17
#define HV_VSND_FMT_FLOAT 19
#define HV_VSND_RATE_44100 6
#define HV_VSND_RATE_48000 7

typedef struct HvVirtioSndBackend {
    /* Called on the CPU thread. Return 0 if the host can play this. */
    int (*start)(void *opaque, unsigned channels, unsigned format, unsigned rate_hz);
    void (*stop)(void *opaque);
    void *opaque;
    /* The microphone (the input stream), on the CPU thread, which they must not block: start or stop
       the host's capture on another thread. Once started, the host hands its PCM, in exactly this
       format, to hv_virtio_snd_capture. cap_start returns 0 if the host will try (if it then sends
       nothing, the Mac hears silence). NULL: no microphone, the Mac hears silence. */
    int (*cap_start)(void *opaque, unsigned channels, unsigned format, unsigned rate_hz);
    void (*cap_stop)(void *opaque);
} HvVirtioSndBackend;

/* The input stream. The host's capture goes into a ring; the CPU thread fills the guest's
   capture buffers from it at the stream's rate by the wall clock (silence when the host has
   nothing), so the guest's input runs at the right speed with or without a microphone. */
typedef struct HvVirtioSndIn {
    unsigned channels, format, rate, frame_bytes, rate_hz;
    uint32_t buffer_bytes, period_bytes;
    int prepared, running;
    int capturing;  /* the backend's capture was started (cap_start said yes) */
    int primed;     /* the ring had a cushion of audio: take from it (else silence until it has) */
    int restart;    /* a snapshot was loaded while it ran: start the host's capture again */
    uint8_t *ring;  /* written by the host's capture thread */
    _Atomic uint64_t written, read;
    uint64_t start_ns, delivered; /* the clock: bytes due since start_ns, bytes given to the guest */
    HvVirtqElem pending[HV_VSND_PENDING]; /* capture buffers held until their audio is due */
    uint32_t phead, ptail;
    uint64_t overruns, underruns, xfers;
} HvVirtioSndIn;

typedef struct HvVirtioSnd {
    HvVirtio vio;
    HvVirtioSndBackend backend;
    /* the output stream's parameters (set by PCM_SET_PARAMS) */
    unsigned channels, format, rate, frame_bytes, rate_hz;
    uint32_t buffer_bytes, period_bytes;
    int prepared, running;
    /* ring: written by the CPU thread, read by the backend */
    uint8_t *ring;
    _Atomic uint64_t written, consumed;
    uint64_t clock_start_ns, clock_start_consumed; /* no backend: wall-clock drain */
    /* transmit buffers waiting for their audio to be played, in order */
    HvVirtqElem pending[HV_VSND_PENDING];
    uint64_t pending_end[HV_VSND_PENDING]; /* `written` after this buffer's data */
    uint32_t phead, pcopy, ptail; /* pcopy: next one whose audio is not yet in the ring */
    uint64_t underruns, xfers;
    HvVirtioSndIn in; /* the microphone */
} HvVirtioSnd;

HvVirtioSnd *hv_virtio_snd_create(HvMachine *m, HvPcie *bus);
/* CPU thread, each tick: return played buffers to the guest. */
void hv_virtio_snd_poll(HvVirtioSnd *d);
/* Backend thread: take up to `len` bytes of PCM; the rest of `buf` is silence. Returns bytes taken. */
uint32_t hv_virtio_snd_pull(HvVirtioSnd *d, void *buf, uint32_t len);
/* Host capture thread: len bytes of PCM from the microphone, in the format cap_start asked for.
   What doesn't fit (the guest isn't taking it) is dropped. */
void hv_virtio_snd_capture(HvVirtioSnd *d, const void *pcm, uint32_t len);
/* The number of streams the guest was told of: 2 with the microphone, 1 without. */
int hv_virtio_snd_streams(const HvVirtioSnd *d);
/* After a snapshot load: if the input stream was running, start the host's capture again, the clock from now. */
void hv_virtio_snd_capture_restart(HvVirtioSnd *d);

#endif

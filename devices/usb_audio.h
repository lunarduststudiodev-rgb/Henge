#ifndef HENGEVM_USB_AUDIO_H
#define HENGEVM_USB_AUDIO_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "machine.h"

/* A USB Audio Class 2.0 speaker, high speed, the kind macOS's own AppleUSBAudio drives: what makes the
   virtual Mac's sound behave like a Bluetooth speaker's.

   - Its Feature Unit has a master volume and mute the host sets (SET_CUR), so macOS does no volume of its
     own: the PCM comes at full strength and the volume is the device's (the iPad's system volume, or a gain
     on the Mac host). Every way the Mac sets its volume (the menu bar, Control Center, the volume keys)
     arrives here.
   - Its status interrupt endpoint tells macOS when the volume changed on the device's side (UAC 2.0's
     interrupt data message, CUR changed), and macOS reads it again (GET_CUR) and moves its slider.
   - A HID Consumer Control interface beside it (volume up, down, mute: what a USB headset's buttons are)
     makes macOS step its own volume and show its own volume display for the iPad's volume buttons.

   Its one audio stream, 48 kHz stereo, comes on an isochronous OUT endpoint (adaptive: it takes what the
   host sends each millisecond), 16-bit (alternate setting 1) or 24-bit in 32 (setting 2). The PCM goes into
   a ring the host's audio thread pulls from as float stereo (hv_usb_audio_pull). */

#define HV_USB_AUDIO_RATE 48000
#define HV_USB_AUDIO_CHANNELS 2
/* The volume range macOS is given, in 1/256 dB (UAC's units): -60 dB to 0 dB in steps of 1/16 dB. */
#define HV_USB_AUDIO_VOL_MIN (-60 * 256)
#define HV_USB_AUDIO_VOL_MAX 0
#define HV_USB_AUDIO_VOL_RES 16

/* Endpoints, as device context indexes (xHCI's DCI: endpoint number x 2, + 1 for IN). */
#define HV_USB_AUDIO_DCI_ISO_OUT 2 /* EP1 OUT: the PCM */
#define HV_USB_AUDIO_DCI_STATUS 5  /* EP2 IN: audio control status (interrupt) */
#define HV_USB_AUDIO_DCI_HID 7     /* EP3 IN: the volume buttons (interrupt) */

/* Consumer usages the HID interface reports. */
#define HV_USB_AUDIO_KEY_VOLUME_UP 0xe9
#define HV_USB_AUDIO_KEY_VOLUME_DOWN 0xea
#define HV_USB_AUDIO_KEY_MUTE 0xe2

#define HV_USB_AUDIO_RING (1u << 17) /* bytes of PCM waiting for the host: 340 ms at 16-bit, 170 ms at 24 */
#define HV_USB_AUDIO_QUEUE 32

typedef struct HvUsbAudioBackend {
    /* CPU thread: macOS started its stream (bits 16 or 24) or stopped it. The host's audio may run all the
       time (hv_usb_audio_pull gives silence while there is no stream); these say when it is worth running. */
    void (*start)(void *opaque, unsigned bits);
    void (*stop)(void *opaque);
    /* CPU thread: macOS set the volume (1/256 dB, HV_USB_AUDIO_VOL_MIN..MAX) or mute. */
    void (*volume)(void *opaque, int db256, int mute);
    void *opaque;
} HvUsbAudioBackend;

typedef struct HvUsbAudio {
    HvMachine *m;
    HvUsbAudioBackend backend;
    char name[64];       /* the USB product name: what macOS calls the output */
    /* CPU thread */
    int configured;
    int alt;             /* interface 1's alternate setting: 0 no stream, 1 16-bit, 2 24-bit */
    int started;         /* the backend plays the stream */
    uint8_t cfg[256];    /* the configuration descriptor */
    int cfg_len;
    /* under mu: the Feature Unit's controls, which the host sets too */
    pthread_mutex_t mu;
    int vol;             /* 1/256 dB */
    int mute;
    uint8_t status[HV_USB_AUDIO_QUEUE][6]; /* interrupt data messages waiting for EP2 */
    uint32_t shead, stail;
    uint8_t hid[HV_USB_AUDIO_QUEUE];       /* HID reports waiting for EP3 */
    uint32_t hhead, htail;
    uint8_t hid_cur;
    _Atomic int pending; /* something waits on EP2 or EP3 */
    /* The PCM: written by the CPU thread, read by the host's audio thread. Each start of the stream is a new
       epoch: the ring from start_at holds its frames (frame_bytes each). */
    uint8_t *ring;
    _Atomic uint64_t written, consumed, start_at;
    _Atomic unsigned frame_bytes; /* 4 (16-bit) or 8 (24 in 32), while the stream runs; 0 stopped */
    _Atomic unsigned epoch;
    /* host audio thread */
    unsigned seen_epoch, cur_frame_bytes;
    int primed;          /* enough is waiting to start playing */
    /* counters (each written by one thread) */
    uint64_t packets, dropped, underruns, skipped;
} HvUsbAudio;

HvUsbAudio *hv_usb_audio_create(HvMachine *m, const char *name);
void hv_usb_audio_free(HvUsbAudio *a);
/* Before macOS reads its descriptors: the name it shows (the USB product string). */
void hv_usb_audio_set_name(HvUsbAudio *a, const char *name);

/* ---- the USB side (CPU thread) ---- */

/* A control request on endpoint 0. data: what an OUT request's data stage carried (data_len bytes). Returns
   the reply's length (IN), 0 (OUT), or -1 to stall. */
int hv_usb_audio_control(HvUsbAudio *a, const uint8_t setup[8], const uint8_t *data, int data_len, uint8_t *out, int max);
/* A bus reset (or the device went): unconfigured, the stream stops. */
void hv_usb_audio_reset(HvUsbAudio *a);
/* One isochronous OUT packet's PCM. */
void hv_usb_audio_iso_out(HvUsbAudio *a, const void *data, uint32_t len);
/* The next message for the status endpoint (6 bytes) or the HID endpoint (1 byte): its length, 0 if none. */
int hv_usb_audio_next_status(HvUsbAudio *a, uint8_t out[6]);
int hv_usb_audio_next_hid(HvUsbAudio *a, uint8_t out[1]);
int hv_usb_audio_pending(HvUsbAudio *a);

/* ---- the host's side (any thread) ---- */

/* The volume changed on the device (the iPad's own volume): macOS is told, and moves its slider. */
void hv_usb_audio_host_volume(HvUsbAudio *a, int db256);
void hv_usb_audio_host_mute(HvUsbAudio *a, int mute);
/* A volume button pressed and let go (HV_USB_AUDIO_KEY_*): macOS steps its volume and shows it. */
void hv_usb_audio_host_key(HvUsbAudio *a, int usage);
/* The Feature Unit's state now. */
void hv_usb_audio_get_volume(HvUsbAudio *a, int *db256, int *mute);
/* Host audio thread: `frames` stereo frames (interleaved float, -1..1) times gain; silence where there is
   none. Returns the frames that were the Mac's. It keeps about 30 ms waiting: it starts once that much has
   come (and again after running dry), and skips ahead when more than about 100 ms waits (the host plays
   slower than the Mac sends: their clocks drift apart, or the host's audio stopped for a while). */
uint32_t hv_usb_audio_pull(HvUsbAudio *a, float *out, uint32_t frames, float gain);

/* The volume as a 0..1 slider position (linear in dB over the range; HV_USB_AUDIO_VOL_MIN is 0), and back
   (rounded to a step of the range). */
double hv_usb_audio_db_to_scalar(int db256);
int hv_usb_audio_scalar_to_db(double s);

/* Snapshots: what macOS set up. */
typedef struct HvUsbAudioState {
    int32_t configured, alt, vol, mute;
} HvUsbAudioState;
void hv_usb_audio_get_state(HvUsbAudio *a, HvUsbAudioState *s);
int hv_usb_audio_set_state(HvUsbAudio *a, const HvUsbAudioState *s); /* 0, or -1 if it makes no sense */

#endif

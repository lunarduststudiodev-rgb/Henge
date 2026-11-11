#ifndef HENGEVM_AUDIO_USB_H
#define HENGEVM_AUDIO_USB_H

#include "usb_audio.h"

/* Play the USB speaker's stream (devices/usb_audio.h) on the host's output, as a Bluetooth speaker plays a
   phone's: the PCM comes at full strength and the volume macOS sets is the device's.
   - On a Mac host the volume is a gain applied here.
   - On iOS the sound plays at full strength and the volume is the iPad's own system volume: the app listens
     (hv_audio_usb_set_listener) and sets it, and tells the speaker when the iPad's volume changes
     (hv_usb_audio_host_volume) so macOS moves its slider. Mute is silence here on both. */
void hv_audio_usb_attach(HvUsbAudio *a);
/* The machine is pausing (1) or going on (0), as hv_audio_pause. */
void hv_audio_usb_pause(int pause);
/* Called (on a CPU thread) whenever macOS sets the speaker's volume: level 0..1 as a slider, and mute. */
void hv_audio_usb_set_listener(void (*cb)(void *opaque, double level, int mute), void *opaque);

#endif

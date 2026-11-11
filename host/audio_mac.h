#ifndef HENGEVM_AUDIO_MAC_H
#define HENGEVM_AUDIO_MAC_H

#include "virtio_snd.h"

/* Play the guest's virtio-snd output on the Mac's default output device, and give it the default input (microphone). */
void hv_audio_attach(HvVirtioSnd *d);
/* The machine is pausing (1) or going on (0): the output stops and starts again where it was. Only
   while no CPU runs (the guest's own starts and stops come from them). */
void hv_audio_pause(int pause);
/* The microphone: hook(1) is called (on a queue of its own, which it may block) just before the host's input
   starts, because the guest records; hook(0) once it has stopped. The iOS app switches its audio session to
   recording and back. Set before hv_audio_attach. */
void hv_audio_set_mic_hook(void (*hook)(int on));

#endif

#ifndef HENGEVM_VIRTIO_INPUT_H
#define HENGEVM_VIRTIO_INPUT_H

#include <pthread.h>

#include "virtio.h"

/* virtio-input (device type 18), Linux input event codes, as macOS's
   AppleVirtIOInput drivers expect: it calls a device with relative X/Y a
   mouse, one with absolute X/Y a trackpad, and one with keys but no left
   button a keyboard. Events come from the host UI on any thread
   (hv_virtio_input_send) and reach the guest from the CPU thread
   (hv_virtio_input_poll, from the board's tick).
   The tablet is the absolute one: AppleVirtIOInputTrackpad makes it a HID
   pointer whose X and Y run over the ABS_INFO range across the whole screen,
   so the host puts the pointer exactly where a finger is. */
enum { HV_VINPUT_KEYBOARD, HV_VINPUT_MOUSE, HV_VINPUT_TABLET };

#define HV_EV_SYN 0
#define HV_EV_KEY 1
#define HV_EV_REL 2
#define HV_EV_ABS 3
#define HV_ABS_X 0
#define HV_ABS_Y 1
#define HV_TABLET_MAX 32767 /* tablet X and Y run 0 ..= this, edge to edge */
#define HV_REL_X 0
#define HV_REL_Y 1
#define HV_REL_HWHEEL 6
#define HV_REL_WHEEL 8
#define HV_BTN_LEFT 0x110
#define HV_BTN_RIGHT 0x111
#define HV_BTN_MIDDLE 0x112
/* AppleVirtIOInput{Mouse,Trackpad}::handleKeyEvent sets HID button 1 for
   BTN_LEFT, button 2 for BTN_MIDDLE and button 3 for BTN_RIGHT, and macOS
   takes HID button 2 as the secondary (right) click. So a right click is
   BTN_MIDDLE here and a middle click BTN_RIGHT. */
#define HV_BTN_PRIMARY HV_BTN_LEFT
#define HV_BTN_SECONDARY HV_BTN_MIDDLE
#define HV_BTN_TERTIARY HV_BTN_RIGHT

#define HV_VINPUT_RING 4096
#define HV_VINPUT_KEYS 0x300 /* key and button codes (Linux KEY_MAX + 1) */

typedef struct HvVirtioInput {
    HvVirtio vio;
    int kind;
    pthread_mutex_t mu;
    struct { uint16_t type, code; uint32_t value; } ring[HV_VINPUT_RING];
    uint32_t head, tail; /* under mu */
    uint64_t delivered, dropped;
    int asked_range; /* the guest read ABS_INFO: its trackpad driver has the tablet */
    uint8_t down[HV_VINPUT_KEYS / 8]; /* CPU thread: keys and buttons the guest was last told are down */
} HvVirtioInput;

HvVirtioInput *hv_virtio_input_create(HvMachine *m, HvPcie *bus, int kind);
/* Any thread: queue one event (send HV_EV_SYN, 0, 0 after each group). */
void hv_virtio_input_send(HvVirtioInput *d, uint16_t type, uint16_t code, int32_t value);
/* CPU thread: hand queued events to the guest's buffers. */
void hv_virtio_input_poll(HvVirtioInput *d);
/* Any thread: queue a release of every key and button the guest was told is down (after a restore:
   nobody holds them now). */
void hv_virtio_input_release_all(HvVirtioInput *d);
/* Any thread: the guest's driver has started the device and posted buffers. */
int hv_virtio_input_ready(HvVirtioInput *d);

#endif

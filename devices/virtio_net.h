#ifndef HENGEVM_VIRTIO_NET_H
#define HENGEVM_VIRTIO_NET_H

#include <stdatomic.h>

#include "virtio.h"

/* virtio-net with one queue pair. The host side of the network is a pair of
   hooks: `transmit` gets every frame the guest sends, and frames for the guest
   go in through hv_virtio_net_receive. No backend is attached yet. */
typedef struct HvVirtioNet {
    HvVirtio vio;
    void (*transmit)(void *opaque, const uint8_t *frame, uint32_t len);
    void (*rx_notify)(void *opaque);
    void *opaque;
    uint64_t tx_frames, rx_frames, rx_dropped;
    uint64_t link_up_at; /* monotonic ns: the link comes back then (0: not down) */
    /* The cable, as the host's own network has it (hv_virtio_net_set_carrier): the host's report (bit 63 up,
       the rest the monotonic ns it changed), what the guest is shown since the debounce, and the debounce. */
    _Atomic uint64_t carrier_req;
    int carrier;
    uint64_t carrier_down_ns, carrier_up_ns; /* how long a report must stand: going out, coming back */
    uint64_t carrier_changes, tx_dropped;
    /* CPU thread, as the cable goes out (0) or back in (1) for the host's network: the backend's chance to
       start afresh behind it. */
    void (*carrier_changed)(void *opaque, int up);
} HvVirtioNet;

HvVirtioNet *hv_virtio_net_create(HvMachine *m, HvPcie *bus, const uint8_t mac[6]);
/* Returns 0 if the guest had no receive buffer posted (the caller keeps the frame for later). */
int hv_virtio_net_receive(HvVirtioNet *n, const uint8_t *frame, uint32_t len);
/* The cable out for `ms`, then back in (each a config change the driver sees): after Suspend the
   network behind the device is a new one, and a Mac that saw its link drop asks it for an address again
   and gives up its old connections at once instead of waiting for them to time out. */
void hv_virtio_net_relink(HvVirtioNet *n, unsigned ms);
int hv_virtio_net_link_up(HvVirtioNet *n);
void hv_virtio_net_poll(HvVirtioNet *n); /* brings the link back when it is time (the board's tick) */

/* The host's own network: up (1) when it can reach anything, down (0) when it can't (airplane mode, Wi-Fi gone).
   Any thread. The guest sees its cable unplugged once the host has been without a network for carrier_down_ns
   (3 s), and plugged in again once it has had one back for carrier_up_ns (1 s), so a hand-over from Wi-Fi to
   cellular never shows. Unplugged, nothing goes in or out, as with a real cable. The board's tick applies it.
   at_ns: when the host's network changed (monotonic ns, 0 for now). */
void hv_virtio_net_set_carrier(HvVirtioNet *n, int up, uint64_t at_ns);
/* Monotonic ns, the clock at_ns and the debounce use. */
uint64_t hv_virtio_net_now(void);

#endif

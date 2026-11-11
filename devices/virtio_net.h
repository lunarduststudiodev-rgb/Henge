#ifndef HENGEVM_VIRTIO_NET_H
#define HENGEVM_VIRTIO_NET_H

#include "virtio.h"

/* virtio-net with one queue pair. The host side of the network is a pair of
   hooks: `transmit` gets every frame the guest sends, and frames for the guest
   go in through hv_virtio_net_receive. No backend is attached yet. */
typedef struct HvVirtioNet {
    HvVirtio vio;
    void (*transmit)(void *opaque, const uint8_t *frame, uint32_t len);
    void *opaque;
    uint64_t tx_frames, rx_frames, rx_dropped;
} HvVirtioNet;

HvVirtioNet *hv_virtio_net_create(HvMachine *m, HvPcie *bus, const uint8_t mac[6]);
/* Returns 0 if the guest had no receive buffer posted (the caller keeps the frame for later). */
int hv_virtio_net_receive(HvVirtioNet *n, const uint8_t *frame, uint32_t len);
void hv_virtio_net_set_link(HvVirtioNet *n, int up);

#endif

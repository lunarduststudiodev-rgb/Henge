#ifndef HENGEVM_NET_SLIRP_H
#define HENGEVM_NET_SLIRP_H

#include "machine.h"
#include "virtio_net.h"

/* Before hv_net_attach: the Unix socket behind the guest's 10.0.2.4:80 (share.c). */
void hv_net_set_share_socket(const char *path);
/* The host's tests, before hv_net_attach: the Mac's ports on 127.0.0.1 only, never the device's other interfaces (a
   test is never reachable from the network). Henge itself never calls it: the Mac's ports are on every interface. */
void hv_net_ports_loopback(int on);
/* The host's tests: while nonzero, making the NAT fails (when the cable goes back in, say). */
extern _Atomic int hv_net_test_fail_start;
/* User-mode NAT for the guest's virtio-net (see net_slirp.c). Returns 0 on success. */
int hv_net_attach(HvMachine *m, HvVirtioNet *n);
/* CPU thread, from the board's tick: hand frames for the guest to the device. */
void hv_net_poll(void);

/* For the perf log: speed each way, how quickly the guest acknowledges data, how long frames wait for it,
   and frames lost, since the last call. 1 when any frame went either way in that time, else 0. */
int hv_net_report(char *buf, size_t n);
/* Since the network started, any thread: frames lost on the way to the guest (its queue full), from it (the network
   thread's queue full), and as too long for the card; and TCP resets the NAT sent it (a connection it ended or
   refused). A web page or video that fails can be lined up with them. The same counts hv_net_report reports. */
void hv_net_losses(uint64_t *dropped_to_guest, uint64_t *dropped_from_guest, uint64_t *too_long, uint64_t *resets);
/* The last TCP reset either way, as the log has it ("reset to the Mac, port 49823 to 142.250.1.1:443 (opened
   12.0 s ago; the Mac closed its side 2.0 s ago): the far side reset it"), or "". Any thread. */
void hv_net_last_reset(char *buf, size_t n);
/* The link's MTU, what libslirp and the card both use. */
int hv_net_mtu(void);

extern uint64_t hv_net_tx_frames, hv_net_rx_frames;
/* Suspend lab: sees every frame, guest to host (to_guest 0, a CPU thread) and back (1, the network thread,
   or net_dns.c's for DNS answers). */
extern void (*hv_net_watch)(const uint8_t *frame, uint32_t len, int to_guest);

#endif

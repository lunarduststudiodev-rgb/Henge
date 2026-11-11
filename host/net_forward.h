#ifndef HENGEVM_NET_FORWARD_H
#define HENGEVM_NET_FORWARD_H

#include <stddef.h>
#include <stdint.h>

/* The Mac on the network as a MacBook would be: every port it listens on is the same port on every interface of
   the device (net_forward.c). No switch: the Mac's own Sharing settings and firewall decide. */

typedef struct HvFwdPort {
    uint16_t port;
    uint8_t udp; /* 0 TCP, 1 UDP */
} HvFwdPort;

#define HV_FWD_MAX 512

/* What the helper in the Mac says it listens on (share.c's "ports" line), any thread. n < 0: the helper isn't
   linked (any more): the standard sharing ports again. At most HV_FWD_MAX are taken. */
void hv_net_ports_set(const HvFwdPort *ports, int n);
/* What is forwarded while no helper is linked: the standard sharing ports (Remote Login, Kerberos, File Sharing,
   AFP, Remote Management, Screen Sharing) unless replaced here (at most 8; n < 0: those again). For the host's tests,
   which must not take this Mac's own sharing ports. */
void hv_net_ports_standard(const HvFwdPort *ports, int n);
/* Ports the Mac advertises a Bonjour service on (mdns.c): forwarded whether or not the helper is linked. */
void hv_net_ports_advertised(const HvFwdPort *ports, int n);
/* 1: no listener on the device (Henge is leaving the screen, or the Mac is paused: iOS takes a suspended app's
   listening sockets back, TN2277). 0: open them again, and try once more any port that failed. Any thread. */
void hv_net_forwards_hold(int hold);
/* The Mac stopped: every listener closes, for good. Any thread. */
void hv_net_forwards_close(void);

/* For Henge's Details: one per port wanted. err 0: open; else why it isn't (errno: EADDRINUSE, the device itself
   uses the port; EACCES or EPERM, iOS keeps it for its own). Returns how many were written; *helper is 1 when the
   list is the helper's, 0 when it is the standard set; *held 1 while held. Any thread. */
typedef struct HvFwdStatus {
    uint16_t port;
    uint8_t udp;
    int err;
} HvFwdStatus;
int hv_net_forward_status(HvFwdStatus *out, int max, int *helper, int *held);
/* Bumped each time the network thread has applied a change (what hv_net_forward_status says may differ): mdns.c
   looks again then. Any thread. */
unsigned hv_net_forward_generation(void);
/* "22 TCP open; 62078 TCP: Address already in use" and so on, for the log. Any thread. */
void hv_net_forward_line(char *buf, size_t n);

/* ---- for net_slirp.c (the network thread) ----
   open: 0, or the errno that kept the port from being opened. close: the listener goes; connections it took stay. */
typedef struct HvFwdOps {
    int (*open)(int udp, uint16_t port);
    void (*close)(int udp, uint16_t port);
    void (*wake)(void);
} HvFwdOps;
void hv_fwd_attach(const HvFwdOps *ops);
/* At the top of the network thread's loop, never between its poll and slirp_pollfds_poll: open and close what
   changed. */
void hv_fwd_apply(void);
/* The NAT was made afresh (slirp_cleanup closed every listener): everything wanted is opened again, if not held. */
void hv_fwd_lost(void);

#endif

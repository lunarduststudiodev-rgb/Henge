#ifndef HENGEVM_NET_DNS_H
#define HENGEVM_NET_DNS_H

#include <stdint.h>

/* The guest's DNS server, as the NAT's DHCP hands it out (net_slirp.c). */
#define HV_DNS_ADDR "10.0.2.3"

/* Starts the resolver (net_dns.c) on a thread of its own. Frames for the guest go to deliver, from that
   thread. HV_DNS=off leaves DNS to libslirp as before. 0, or -1. */
int hv_dns_start(void (*deliver)(const uint8_t *frame, uint32_t len));
/* CPU thread, for each frame the guest sends: 1 when it was a DNS query to HV_DNS_ADDR over UDP and is
   taken (the answer comes through deliver); 0 when the NAT should have it. */
int hv_dns_take(const uint8_t *frame, uint32_t len);
/* The port of the host's loopback where DNS over TCP to HV_DNS_ADDR goes (libslirp sends it there), or 0
   when there is none (HV_DNS=off): libslirp has it. */
uint16_t hv_dns_tcp_port(void);
/* The host's network changed: look its DNS servers up again and forget which one answered last. */
void hv_dns_network_changed(void);

#endif

/* The host's network tests must never be reachable from the network the MacBook is on (gate r2 M2): they call
   hv_net_ports_loopback(1) and hv_mdns_local_only(1) before hv_net_attach. This checks it: every socket this process
   listens on (TCP listening, and UDP bound to a port Henge forwards) is on 127.0.0.1. Returns how many aren't; the
   first is described in what. */
#ifndef HENGEVM_TEST_LOOPBACK_ONLY_H
#define HENGEVM_TEST_LOOPBACK_ONLY_H

#include <arpa/inet.h>
#include <libproc.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net_forward.h"

static int open_beyond_loopback(char *what, size_t n) {
    static HvFwdStatus st[HV_FWD_MAX];
    int nst = hv_net_forward_status(st, HV_FWD_MAX, NULL, NULL), bad = 0;
    if (what && n) what[0] = 0;
    for (int fd = 0; fd < getdtablesize(); fd++) {
        int type = 0, listening = 0;
        socklen_t l = sizeof type;
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &l) != 0) continue;
        struct sockaddr_storage ss;
        l = sizeof ss;
        if (getsockname(fd, (struct sockaddr *)&ss, &l) != 0) continue;
        uint16_t port;
        int loop;
        if (ss.ss_family == AF_INET) {
            const struct sockaddr_in *a = (const struct sockaddr_in *)&ss;
            port = ntohs(a->sin_port), loop = a->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
        } else if (ss.ss_family == AF_INET6) {
            const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)&ss;
            port = ntohs(a->sin6_port), loop = IN6_IS_ADDR_LOOPBACK(&a->sin6_addr);
        } else {
            continue;
        }
        if (!port) continue;
        if (type == SOCK_STREAM) { /* macOS has no getsockopt(SO_ACCEPTCONN) (ENOPROTOOPT): the kernel's own word */
            struct socket_fdinfo si;
            listening = proc_pidfdinfo(getpid(), fd, PROC_PIDFDSOCKETINFO, &si, sizeof si) == (int)sizeof si &&
                        (si.psi.soi_options & SO_ACCEPTCONN);
        } else if (type == SOCK_DGRAM) {
            for (int i = 0; i < nst; i++) listening |= st[i].udp && st[i].port == port && !st[i].err;
        }
        if (!listening || loop) continue;
        if (!bad++ && what && n) snprintf(what, n, "%s port %u (fd %d) on more than 127.0.0.1", type == SOCK_STREAM ? "TCP" : "UDP", port, fd);
    }
    return bad;
}

#endif

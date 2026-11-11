# libslirp in Henge

libslirp 4.9.7 (https://gitlab.freedesktop.org/slirp/libslirp, tag v4.9.7; BSD-3-Clause, see
`COPYRIGHT`), the user-mode NAT behind the guest's network card (`host/net_slirp.c`). Only `src/` is
kept, with `libslirp-version.h` as meson would generate it. The Makefile builds it for the Mac
(`build/libslirp.a`, on Homebrew's glib) and into the iOS engine library (`build/ios/libhengevm.a`,
on the iOS glib from Husk, compiled against its headers in `third_party/glib-ios`).

## Henge's patch: TCP window scaling

Stock libslirp turns RFC 1323 off (`TCP_DO_RFC1323 0` in `tcp_subr.c`), so it never uses window
scaling and keeps at most 64 KB in flight to the guest on any connection. A download then runs
at 64 KB divided by how long the guest takes to acknowledge it: 3 MB/s at 20 ms, which is about
what macOS in Henge on an iPad managed while the iPad itself downloaded several times faster.

The patch (every change is marked `Henge:`) does window scaling as RFC 7323 section 2 describes it,
still without timestamps:

- `tcp_subr.c`: every connection asks for it (`TF_REQ_SCALE`).
- `tcp_input.c`: `tcp_dooptions` reads the window scale option from a SYN; the options of a guest's
  SYN are read before the host connect completes as well, since `cont_conn` no longer has them
  (stock libslirp also lost the guest's MSS there); scaling starts when the connection is
  established and both sides offered it; the window in any segment but a SYN is scaled.
- `tcp_output.c`: our SYN offers it, and our SYN-ACK when the other side's SYN did, with a shift
  that covers our receive buffer. (Scaling the window we advertise was already there.)
- `tcp.h`: buffers of 1 MB toward the guest and 256 KB from it, from 128 KB each. Pages are only
  touched as data passes through, and are freed when the connection closes.

`tests/bench_net.c` (`make build/bench_net`) measures it: one download through the NAT to a guest
played on this Mac that acknowledges after a set delay.

## Henge's patch: data that arrived before a far-side reset reaches the guest

Stock libslirp drops a connection the moment the far side resets it (`soread` gets ECONNRESET),
throwing away whatever is still in `so_snd`. That data has already been acknowledged to the far
side, so nothing will send it again: a download that ends with the server's reset loses its tail.
With the 1 MB buffer above that can be hundreds of KB.

- `socket.h`, `socket.c`: on a read error other than ECONNREFUSED, while `so_snd` still holds data,
  `soread` keeps the error in `henge_reset_err` and leaves the connection open.
- `slirp.c`: the poll loop sends the reset (`tcp_drop`) once `so_snd` has drained to the guest, and
  asks for an immediate poll when that moment comes.

## Henge's patch: why each reset was sent

`net_slirp.c` logs every TCP reset the NAT sends the guest with its reason. `tcp_input.c`,
`tcp_timer.c` and `socket.c` set `slirp_henge_reset_why` (`libslirp.h`) wherever they send one;
`tcp_respond` and `tcp_output` copy it into the packet (`henge_why` in `mbuf.h`), and `if_encap` sets
it again just before the packet leaves, so a reset that waited for ARP keeps its reason.

## Henge's patch: DNS over TCP goes to Henge's resolver

The guest asks 10.0.2.3 over TCP when a UDP answer came back truncated. Stock libslirp connects
that to the host's first resolv.conf server, which on an iPad is often unreachable or refuses TCP.
`sotranslate_out4` (`socket.c`) sends it instead to `slirp_henge_dns_tcp_port` on loopback when
`net_slirp.c` has set it: `net_dns.c`'s TCP server, which asks the same servers as its UDP side.

## Henge's patches: the Mac's ports on the network

`host/net_forward.c` opens each port the Mac listens on at the same number on the device, with
`slirp_add_hostfwd` (design: `local/audit-2026-11-09/designs/port-forwarding-hostfwd.md`). Three
things a forward does in stock libslirp that a real Mac doesn't:

- `tcp_input.c`: a forwarded connection is accepted on the device before the guest is asked. When
  the guest refuses it (a reset: nothing listens there, or its firewall said no), stock libslirp
  closes the accepted socket normally, so the client sees a connection open and then close. The
  socket is now closed with a zero linger, which sends a reset: the client gets "connection
  refused", as from a Mac on the network.
- `socket.c`: `tcpx_listen` listened with a backlog of 1, so a client opening several connections
  at once (Finder to File Sharing, a browser to a dev server) had some refused. It is `SOMAXCONN`.
- `udp.c`, `libslirp.h`: `udp_input` finds a UDP socket by the guest's address and port alone. A
  NAT socket the guest made by sending from a port before it was forwarded would keep taking that
  port's replies-to-be, so they would leave from another device port than the forward's.
  `slirp_henge_udp_forget` drops those sockets; `net_slirp.c` calls it before adding a UDP forward.

`tests/test_net_forward.c` fails without each of the three.

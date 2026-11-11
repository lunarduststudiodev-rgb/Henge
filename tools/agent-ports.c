/* Which ports this Mac listens on from outside itself (agent-ports.h).

   macOS has no public notice for "a socket started listening", nor a public form of its socket tables: netstat
   reads net.inet.{tcp,udp}.pcblist_n, whose records (xinpgen, then per socket an xsocket_n, its buffers, its
   statistics, an xinpcb_n and for TCP an xtcpcb_n, each with its length and kind first) are xnu's PRIVATE structures
   (bsd/netinet/in_pcb.h, bsd/sys/socketvar.h). The two this needs are copied below as xnu has them, packed to 4 as
   there. The buffer is walked by each record's own length and kind, every step bounds-checked, and a record of a
   kind this reads whose length isn't the one copied here stops the whole read: Henge then keeps the standard
   sharing ports rather than act on a misread table. */
#include "agent-ports.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define XSO_SOCKET 0x001
#define XSO_INPCB 0x010
#define INP_IPV4 0x1
#define INP_IPV6 0x2
#define IN6P_IPV6_V6ONLY 0x00008000

#pragma pack(4)
struct xinpgen_copy {
    uint32_t xig_len;
    uint32_t xig_count;
    uint64_t xig_gen;
    uint64_t xig_sogen;
};

struct xsocket_n_copy {
    uint32_t xso_len;
    uint32_t xso_kind;
    uint64_t xso_so;
    short so_type;
    uint32_t so_options;
    short so_linger;
    short so_state;
    uint64_t so_pcb;
    int xso_protocol;
    int xso_family;
    short so_qlen, so_incqlen, so_qlimit, so_timeo;
    unsigned short so_error;
    int32_t so_pgid;
    uint32_t so_oobmark;
    uint32_t so_uid;
    int32_t so_last_pid;
    int32_t so_e_pid;
    uint64_t so_gencnt;
    uint32_t so_flags;
    uint32_t so_flags1;
    int32_t so_usecount;
    int32_t so_retaincnt;
    uint32_t xso_filter_flags;
};

struct in_addr_4in6_copy {
    uint32_t ia46_pad32[3];
    struct in_addr ia46_addr4;
};

struct xinpcb_n_copy {
    uint32_t xi_len;
    uint32_t xi_kind;
    uint64_t xi_inpp;
    unsigned short inp_fport;
    unsigned short inp_lport;
    uint64_t inp_ppcb;
    uint64_t inp_gencnt;
    int inp_flags;
    uint32_t inp_flow;
    unsigned char inp_vflag;
    unsigned char inp_ip_ttl;
    unsigned char inp_ip_p;
    union {
        struct in_addr_4in6_copy inp46_foreign;
        struct in6_addr inp6_foreign;
    } inp_dependfaddr;
    union {
        struct in_addr_4in6_copy inp46_local;
        struct in6_addr inp6_local;
    } inp_dependladdr;
    struct {
        unsigned char inp4_ip_tos;
    } inp_depend4;
    struct {
        uint8_t inp6_hlim;
        int inp6_cksum;
        unsigned short inp6_ifindex;
        short inp6_hops;
    } inp_depend6;
    uint32_t inp_flowhash;
    uint32_t inp_flags2;
};
#pragma pack()

#define ROUNDUP64(n) (((n) + 7u) & ~(size_t)7u)

static int reached_from_outside(const struct xsocket_n_copy *so, const struct xinpcb_n_copy *inp, int udp,
                                const uint32_t *own, int nown) {
    uint16_t port = ntohs(inp->inp_lport);
    if (!port) return 0;
    if (udp) {
        if (inp->inp_fport) return 0;          /* connected: a client's */
        /* Unconnected, on any port (49152 and up too): a server a MacBook's network can reach, as it can this one's
           (sharingd and homed answer there; a client that only sends is reached there too). */
        if (port == 5353 || port == 68) return 0; /* Bonjour (Henge relays it itself), the DHCP client */
    } else if (!(so->so_options & 0x0002)) {   /* SO_ACCEPTCONN: listening */
        return 0;
    }
    if (so->xso_family == AF_INET) {
        uint32_t a = inp->inp_dependladdr.inp46_local.ia46_addr4.s_addr;
        if (a == INADDR_ANY) return 1;
        for (int i = 0; i < nown; i++)
            if (own[i] == a) return 1;
        return 0; /* 127.0.0.1 among them: this Mac's own */
    }
    if (so->xso_family == AF_INET6) {
        /* A dual-stack socket on :: takes IPv4 too (sshd, smbd, Node's default); the device forwards IPv4 only. */
        static const struct in6_addr any6 = IN6ADDR_ANY_INIT;
        return !memcmp(&inp->inp_dependladdr.inp6_local, &any6, sizeof any6) && !(inp->inp_flags & IN6P_IPV6_V6ONLY);
    }
    return 0;
}

int agent_ports_parse(const void *buf, size_t len, int udp, const uint32_t *own, int nown, AgentPort *out, int max,
                      int *sockets) {
    const unsigned char *b = buf;
    int added = 0, walked = 0;
    if (sockets) *sockets = 0;
    if (len < sizeof(struct xinpgen_copy)) return -1;
    struct xinpgen_copy head;
    memcpy(&head, b, sizeof head);
    if (head.xig_len != sizeof head) return -1;
    struct xsocket_n_copy so;
    struct xinpcb_n_copy inp;
    unsigned have = 0; /* the kinds seen of the socket being read */
    size_t at = ROUNDUP64(head.xig_len);
    for (;;) {
        int end = at + 8 > len;
        uint32_t rlen = 0, kind = 0;
        if (!end) {
            memcpy(&rlen, b + at, 4);
            memcpy(&kind, b + at + 4, 4);
            end = rlen <= sizeof head; /* the closing xinpgen */
        }
        /* One socket's records are together; a kind seen again starts the next socket. */
        if (end || (have & kind & (XSO_SOCKET | XSO_INPCB))) {
            if ((have & (XSO_SOCKET | XSO_INPCB)) == (XSO_SOCKET | XSO_INPCB)) {
                walked++;
                if (reached_from_outside(&so, &inp, udp, own, nown) && added < max)
                    out[added++] = (AgentPort){ntohs(inp.inp_lport), (uint8_t)(udp != 0)};
            }
            have = 0;
        }
        if (end) break;
        if (rlen < 8 || rlen > len - at) return -1;
        if (kind == XSO_SOCKET) {
            if (rlen != sizeof so) return -1;
            memcpy(&so, b + at, sizeof so);
        } else if (kind == XSO_INPCB) {
            if (rlen != sizeof inp) return -1;
            memcpy(&inp, b + at, sizeof inp);
        }
        have |= kind;
        size_t step = ROUNDUP64((size_t)rlen);
        if (step > len - at) break; /* the last record, unpadded */
        at += step;
    }
    if (sockets) *sockets = walked;
    return added;
}

static int read_table(const char *name, int udp, const uint32_t *own, int nown, AgentPort *out, int max, int *sockets) {
    for (int tries = 0; tries < 4; tries++) {
        size_t len = 0;
        if (sysctlbyname(name, NULL, &len, NULL, 0) != 0) return -1;
        len += len / 4 + 4096; /* sockets come and go between the two calls */
        void *buf = malloc(len);
        if (!buf) return -1;
        if (sysctlbyname(name, buf, &len, NULL, 0) != 0) {
            free(buf);
            if (errno == ENOMEM) continue;
            return -1;
        }
        int n = agent_ports_parse(buf, len, udp, own, nown, out, max, sockets);
        free(buf);
        return n;
    }
    return -1;
}

static int cmp(const void *a, const void *b) {
    const AgentPort *x = a, *y = b;
    return x->udp != y->udp ? x->udp - y->udp : (int)x->port - (int)y->port;
}

int agent_ports_read(AgentPort *out, int max, int *sockets) {
    uint32_t own[32];
    int nown = 0;
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) == 0) {
        for (struct ifaddrs *i = ifs; i && nown < 32; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !(i->ifa_flags & IFF_LOOPBACK))
                own[nown++] = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
        freeifaddrs(ifs);
    }
    int st = 0, su = 0;
    int nt = read_table("net.inet.tcp.pcblist_n", 0, own, nown, out, max, &st);
    if (nt < 0) return -1;
    int nu = read_table("net.inet.udp.pcblist_n", 1, own, nown, out + nt, max - nt, &su);
    if (nu < 0) return -1;
    if (sockets) *sockets = st + su;
    int n = nt + nu;
    qsort(out, (size_t)n, sizeof *out, cmp);
    int u = 0;
    for (int i = 0; i < n; i++)
        if (!u || cmp(&out[u - 1], &out[i])) out[u++] = out[i];
    return u;
}

void agent_ports_line(const AgentPort *p, int n, char *buf, size_t cap) {
    size_t l = (size_t)snprintf(buf, cap, "ports");
    int last = -1;
    for (int i = 0; i < n && l + 16 < cap; i++) {
        if (p[i].udp != last) l += (size_t)snprintf(buf + l, cap - l, p[i].udp ? " udp" : " tcp"), last = p[i].udp;
        l += (size_t)snprintf(buf + l, cap - l, " %u", p[i].port);
    }
    if (l + 1 < cap) buf[l++] = '\n', buf[l] = 0;
}

int agent_cost_add(AgentCost *c, uint64_t now, uint64_t took, AgentCost *out) {
    if (!c->looks || took < c->least) c->least = took;
    if (took > c->most) c->most = took;
    c->sum += took, c->looks++;
    if (!c->since) c->since = now;
    if (took >= AGENT_COST_SLOW && !c->slow_said) { /* at once, and the hour goes on */
        c->slow_said = 1;
        *out = *c;
        return 1;
    }
    if (now - c->since < AGENT_COST_EVERY) return 0;
    *out = *c;
    *c = (AgentCost){.since = now};
    return 1;
}

static int key_why(char *why, size_t cap, const char *fmt, const char *path, const char *what) {
    snprintf(why, cap, fmt, path, what);
    return -1;
}

/* An existing file, as it is: read only if it is one root alone could have made and alone may read. */
static int key_read(const char *path, uid_t owner, uint8_t key[AGENT_KEY_SIZE], char *why, size_t cap) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return key_why(why, cap, "%s %s", path, errno == ELOOP ? "is a link to another file: refused (it is left as it is)" : strerror(errno));
    struct stat st;
    const char *bad = NULL;
    char badbuf[96];
    if (fstat(fd, &st) != 0) bad = strerror(errno);
    else if (!S_ISREG(st.st_mode)) bad = "isn't a plain file: refused (it is left as it is)";
    else if (st.st_uid != owner) {
        snprintf(badbuf, sizeof badbuf, "is owned by user %u, not %u: refused (it is left as it is)", (unsigned)st.st_uid, (unsigned)owner);
        bad = badbuf;
    } else if ((st.st_mode & 07777) != 0600) {
        snprintf(badbuf, sizeof badbuf, "has mode %04o, not 0600: refused (it is left as it is)", (unsigned)(st.st_mode & 07777));
        bad = badbuf;
    } else if (st.st_size != AGENT_KEY_SIZE) bad = "isn't 32 bytes long: refused (it is left as it is)";
    else if (read(fd, key, AGENT_KEY_SIZE) != AGENT_KEY_SIZE) bad = "can't be read whole";
    close(fd);
    if (bad) {
        memset(key, 0, AGENT_KEY_SIZE);
        return key_why(why, cap, "%s %s", path, bad);
    }
    return 0;
}

int agent_system_key(const char *path, uid_t owner, uint8_t key[AGENT_KEY_SIZE], char *why, size_t cap) {
    why[0] = 0;
    struct stat st;
    if (lstat(path, &st) == 0 || errno != ENOENT) return key_read(path, owner, key, why, cap);
    /* None: made next to it, then linked into place, which never replaces a file another put there meanwhile. */
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    unlink(tmp); /* a start cut short */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return key_why(why, cap, "%s can't be made: %s", path, strerror(errno));
    arc4random_buf(key, AGENT_KEY_SIZE);
    int ok = fchmod(fd, 0600) == 0 && write(fd, key, AGENT_KEY_SIZE) == AGENT_KEY_SIZE && fsync(fd) == 0;
    int err = errno;
    close(fd);
    if (ok && link(tmp, path) != 0) ok = 0, err = errno;
    unlink(tmp);
    memset(key, 0, AGENT_KEY_SIZE);
    if (!ok && err == EEXIST) return key_read(path, owner, key, why, cap); /* made by another just now */
    if (!ok) return key_why(why, cap, "%s can't be made: %s", path, strerror(err));
    /* Read back as any start would read it: what is used is what is kept. */
    return key_read(path, owner, key, why, cap) == 0 ? 1 : -1;
}

void agent_key_hex(const uint8_t key[AGENT_KEY_SIZE], char out[2 * AGENT_KEY_SIZE + 1]) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < AGENT_KEY_SIZE; i++) out[2 * i] = d[key[i] >> 4], out[2 * i + 1] = d[key[i] & 15];
    out[2 * AGENT_KEY_SIZE] = 0;
}

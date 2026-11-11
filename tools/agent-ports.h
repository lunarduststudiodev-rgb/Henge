#ifndef HENGE_AGENT_PORTS_H
#define HENGE_AGENT_PORTS_H

/* Which ports this Mac listens on from outside itself, from the kernel's socket tables (agent-ports.c): what the
   helper's system part tells Henge, so each is the same port on the device (host/net_forward.c). Plain C, so a
   host test can run it. */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct AgentPort {
    uint16_t port;
    uint8_t udp;
} AgentPort;

/* One table (net.inet.tcp.pcblist_n, udp 0; net.inet.udp.pcblist_n, udp 1) as the sysctl gave it. own: this Mac's
   IPv4 addresses (network order) a socket may be bound to and still be reached from outside, besides 0.0.0.0.
   Adds each port to out (up to max, in any order, possibly twice). Returns how many it added, or -1 if the buffer
   isn't laid out as this file knows (a macOS that changed it: nothing is guessed). *sockets: how many it walked. */
int agent_ports_parse(const void *buf, size_t len, int udp, const uint32_t *own, int nown, AgentPort *out, int max,
                      int *sockets);

/* Both tables, read now, sorted and without repeats. -1 if either can't be read or parsed. A process that isn't
   root sees only its own sockets: the helper's system part (a LaunchDaemon, root) sees everyone's. */
int agent_ports_read(AgentPort *out, int max, int *sockets);

/* "ports tcp 22 445 udp 88\n", or "ports\n" when there are none. */
void agent_ports_line(const AgentPort *p, int n, char *buf, size_t cap);

/* What the looks at the tables cost, for the helper's log: a note once an hour, and at once for the first look in
   that hour to take AGENT_COST_SLOW or more. Times in nanoseconds. */
#define AGENT_COST_EVERY (3600ull * 1000000000ull)
#define AGENT_COST_SLOW (20ull * 1000000ull)
typedef struct AgentCost {
    uint64_t since, least, most, sum;
    int looks, slow_said;
} AgentCost;
/* A look at now that took took. 1 when a note is due: *out has the figures so far (looks since `since`). */
int agent_cost_add(AgentCost *c, uint64_t now, uint64_t took, AgentCost *out);

/* The system part's key (share.c, "the system link"): 32 random bytes in a file only root may read, which the app
   pins per Mac on the first link after the Mac starts afresh. Only root can read it, so a link that sends it is the
   system part's, not a user's program. Made here only when there is no file at path at all, whole or not at all,
   and never in place of one: a file there that isn't one root alone owns and may read (owner, mode 0600, 32 bytes,
   a plain file and not a link to one) is refused and left as it is, and *why says what is wrong with it.
   1: made now; 0: read; -1: refused or can't be read, nothing in key. owner: who must own it (root; a test, itself). */
#define AGENT_KEY_SIZE 32
int agent_system_key(const char *path, uid_t owner, uint8_t key[AGENT_KEY_SIZE], char *why, size_t cap);
/* The key as 64 lowercase hex digits, for the link's Henge-Key line. */
void agent_key_hex(const uint8_t key[AGENT_KEY_SIZE], char out[2 * AGENT_KEY_SIZE + 1]);

#endif

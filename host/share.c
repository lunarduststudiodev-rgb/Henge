/* The folder and clipboard a virtual Mac shares with the device it runs on.

   The folder is a small WebDAV server, because macOS has a WebDAV client built in, so it mounts like any
   network drive with nothing to install for that part. The network hands each connection the guest makes to
   10.0.2.4:80 to a Unix socket in the app's own storage: no TCP port is open, so other apps can't reach this.

   /.henge/ is not part of the folder. It has the clipboard, where the helper inside macOS (tools/henge-agent.m)
   waits for the device's clipboard and sends the guest's, and what it takes to install that helper.

   Locks are accepted but not enforced: macOS only writes to a server that offers them, and there is one client. */
#define __STDC_WANT_LIB_EXT1__ 1 /* memset_s: a key wiped where the compiler can't leave it out */
#include "share.h"

#include <CommonCrypto/CommonDigest.h>
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <removefile.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define HEAD_MAX (32 * 1024)
#define SMALL_BODY_MAX (16 * 1024 * 1024) /* a clipboard, or a request's XML */
#define TEMP_PREFIX ".henge-put-"

static char root[PATH_MAX];
static char agent_path[PATH_MAX];
static char bench_video[PATH_MAX];

/* ---- the clipboard ---- */

static pthread_mutex_t clip_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clip_cond = PTHREAD_COND_INITIALIZER;
static char *device_clip; /* the device's, for the guest */
static unsigned long device_seq;
static char *guest_clip; /* the guest's, until the device takes it */
static time_t agent_seen;

void hv_share_set_clipboard(const char *text) {
    pthread_mutex_lock(&clip_mu);
    free(device_clip);
    device_clip = strdup(text);
    device_seq++;
    pthread_cond_broadcast(&clip_cond);
    pthread_mutex_unlock(&clip_mu);
}

char *hv_share_take_clipboard(void) {
    pthread_mutex_lock(&clip_mu);
    char *text = guest_clip;
    guest_clip = NULL;
    pthread_mutex_unlock(&clip_mu);
    return text;
}

long hv_share_agent_seen(void) {
    if (hv_share_touch_linked()) return 0;
    pthread_mutex_lock(&clip_mu);
    long ago = agent_seen ? (long)(time(NULL) - agent_seen) : -1;
    pthread_mutex_unlock(&clip_mu);
    return ago;
}

/* ---- connections ---- */

typedef struct Conn {
    int fd;
    char buf[64 * 1024]; /* read ahead */
    size_t at, have;
} Conn;

typedef struct Request {
    char method[16];
    char target[HEAD_MAX]; /* as sent, without the query */
    char path[PATH_MAX];   /* decoded */
    char query[128];
    char destination[PATH_MAX]; /* decoded path */
    char if_header[256];
    long long length;
    int chunked, expect_continue, close, overwrite, depth;
    long long range_from, range_to; /* -1: none */
    long chunk_left;
    int body_done;
    char key[65]; /* Henge-Key: the system part's proof that it is root (64 hex digits), "" if none or not one */
    int key_bad;  /* a Henge-Key that wasn't 64 hex digits */
} Request;

/* A growing string for XML. */
typedef struct Text {
    char *s;
    size_t len, cap;
} Text;

static void add(Text *t, const char *s, size_t n) {
    if (t->len + n + 1 > t->cap) {
        t->cap = (t->len + n + 1) * 2;
        t->s = realloc(t->s, t->cap);
    }
    memcpy(t->s + t->len, s, n);
    t->len += n;
    t->s[t->len] = 0;
}

static void adds(Text *t, const char *s) { add(t, s, strlen(s)); }

static void addf(Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void addf(Text *t, const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) add(t, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

static void add_xml(Text *t, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '&': adds(t, "&amp;"); break;
        case '<': adds(t, "&lt;"); break;
        case '>': adds(t, "&gt;"); break;
        case '"': adds(t, "&quot;"); break;
        default: add(t, s, 1);
        }
    }
}

static void add_url(Text *t, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~/", c)) add(t, s, 1);
        else addf(t, "%%%02X", c);
    }
}

static int send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = send(fd, c, n, 0);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        c += w, n -= (size_t)w;
    }
    return 0;
}

/* What a link holds unread before a send to it fails: 256 KB. The guest often stops reading for a second or more (a
   code-cache flush, a busy WindowServer, a loaded Mac), and a scroll sends a line of about 40 bytes per finger move at
   up to 120 a second, so the default 8 KB would end the link within a second's stall; 256 KB holds about 6500 such lines,
   close to a minute of scrolling. On a macOS Unix socket only the sender's SO_SNDBUF counts (measured on a socketpair:
   8190 bytes by default, 262144 with this, the reader's SO_RCVBUF changing nothing), and our end is the sender. */
enum { LINK_SEND_BUFFER = 256 * 1024 };

/* A helper's link, once its 101 has gone: Henge writes to it under link_mu, so a write must never wait. MSG_DONTWAIT
   isn't enough: on macOS and iOS a Unix socket's send waits with it all the same when the buffer is full (a socketpair
   checked: the send stayed blocked), and a helper that stopped reading held link_mu, and with it Henge, until it read
   again. Non-blocking, the send fails with EAGAIN at once and link_send_l ends the link: only a helper that has
   stopped for long, since the buffer (LINK_SEND_BUFFER) rides out the guest's ordinary stalls. Its reader waits in
   conn_read. */
static int link_nonblocking(int fd) {
    int size = LINK_SEND_BUFFER;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size) != 0) /* still a link, with less room: say so */
        fprintf(stderr, "share: the helper's link keeps the smaller send buffer: %s\n", strerror(errno));
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

static ssize_t conn_read(Conn *c, void *out, size_t n) {
    if (c->at < c->have) {
        size_t take = c->have - c->at < n ? c->have - c->at : n;
        memcpy(out, c->buf + c->at, take);
        c->at += take;
        return (ssize_t)take;
    }
    for (;;) {
        ssize_t r = recv(c->fd, out, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { /* a link (link_nonblocking): wait for more here */
            struct pollfd p = {.fd = c->fd, .events = POLLIN};
            if (poll(&p, 1, -1) < 0 && errno != EINTR) return -1;
            if (p.revents & POLLNVAL) return -1;
            continue; /* data, or the end (recv says which) */
        }
        return r;
    }
}

/* A line of a chunked body's framing. */
static int conn_line(Conn *c, char *line, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char ch;
        if (conn_read(c, &ch, 1) != 1) return -1;
        if (ch == '\n') {
            if (n && line[n - 1] == '\r') n--;
            line[n] = 0;
            return 0;
        }
        line[n++] = ch;
    }
    return -1;
}

/* The next piece of the request's body: >0 bytes, 0 at its end, <0 if the connection broke. */
static ssize_t body_read(Conn *c, Request *r, void *out, size_t n) {
    if (r->body_done) return 0;
    if (r->chunked) {
        if (r->chunk_left == 0) {
            char line[64];
            if (conn_line(c, line, sizeof line) != 0) return -1;
            if (!line[0] && conn_line(c, line, sizeof line) != 0) return -1; /* the CRLF ending the last chunk */
            r->chunk_left = strtol(line, NULL, 16);
            if (r->chunk_left < 0) return -1;
            if (r->chunk_left == 0) {
                while (conn_line(c, line, sizeof line) == 0 && line[0]) {} /* trailers */
                r->body_done = 1;
                return 0;
            }
        }
        size_t take = (size_t)r->chunk_left < n ? (size_t)r->chunk_left : n;
        ssize_t got = conn_read(c, out, take);
        if (got <= 0) return -1;
        r->chunk_left -= got;
        return got;
    }
    if (r->length <= 0) {
        r->body_done = 1;
        return 0;
    }
    size_t take = (unsigned long long)r->length < n ? (size_t)r->length : n;
    ssize_t got = conn_read(c, out, take);
    if (got <= 0) return -1;
    r->length -= got;
    return got;
}

static void body_ready(Conn *c, Request *r) {
    if (r->expect_continue) {
        r->expect_continue = 0;
        static const char go[] = "HTTP/1.1 100 Continue\r\n\r\n";
        send_all(c->fd, go, sizeof go - 1);
    }
}

/* Reads what is left of a body, so the connection can carry on. Returns -1 if it can't. */
static int body_discard(Conn *c, Request *r) {
    char junk[8192];
    body_ready(c, r);
    for (;;) {
        ssize_t got = body_read(c, r, junk, sizeof junk);
        if (got == 0) return 0;
        if (got < 0) return -1;
    }
}

static char *body_small(Conn *c, Request *r, size_t *len) {
    Text t = {0};
    char piece[16384];
    body_ready(c, r);
    for (;;) {
        ssize_t got = body_read(c, r, piece, sizeof piece);
        if (got < 0 || t.len + (size_t)got > SMALL_BODY_MAX) {
            free(t.s);
            return NULL;
        }
        if (got == 0) break;
        add(&t, piece, (size_t)got);
    }
    add(&t, "", 0);
    *len = t.len;
    return t.s;
}

static int respond(Conn *c, Request *r, int status, const char *reason, const char *headers, const char *body, size_t len) {
    char head[2048];
    int n = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nServer: Henge\r\nContent-Length: %zu\r\n%s%s\r\n", status, reason, len,
                     headers ? headers : "", r->close ? "Connection: close\r\n" : "");
    if (send_all(c->fd, head, (size_t)n) != 0) return -1;
    return (body && len && strcmp(r->method, "HEAD") != 0) ? send_all(c->fd, body, len) : 0;
}

static int respond_empty(Conn *c, Request *r, int status, const char *reason) {
    return respond(c, r, status, reason, NULL, NULL, 0);
}

/* ---- paths ---- */

static int hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decodes a URL path, and refuses any that could leave the folder. */
static int decode_path(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = in; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '%') {
            int a = hex(p[1]), b = a < 0 ? -1 : hex(p[2]);
            if (a < 0 || b < 0) return -1;
            c = a * 16 + b;
            p += 2;
            if (c == 0) return -1;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    if (out[0] != '/') return -1;
    for (const char *s = out; s; s = strchr(s + 1, '/')) {
        const char *part = s + 1, *end = strchr(part, '/');
        size_t len = end ? (size_t)(end - part) : strlen(part);
        if (len == 2 && part[0] == '.' && part[1] == '.') return -1;
    }
    return 0;
}

static int local_path(const char *path, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s%s", root, path);
    if (n < 0 || (size_t)n >= cap) return -1;
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
    return 0;
}

/* ---- the folder ---- */

static void add_entry(Text *t, const char *url_path, const char *name, const struct stat *st, int is_root) {
    int dir = S_ISDIR(st->st_mode);
    char modified[64], created[64];
    struct tm tm;
    gmtime_r(&st->st_mtimespec.tv_sec, &tm);
    strftime(modified, sizeof modified, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    gmtime_r(&st->st_birthtimespec.tv_sec, &tm);
    strftime(created, sizeof created, "%Y-%m-%dT%H:%M:%SZ", &tm);
    adds(t, "<D:response><D:href>");
    add_url(t, url_path);
    if (dir && url_path[strlen(url_path) - 1] != '/') adds(t, "/");
    adds(t, "</D:href><D:propstat><D:prop><D:displayname>");
    add_xml(t, name);
    addf(t, "</D:displayname><D:getlastmodified>%s</D:getlastmodified><D:creationdate>%s</D:creationdate>", modified, created);
    if (dir) {
        adds(t, "<D:resourcetype><D:collection/></D:resourcetype>");
    } else {
        addf(t, "<D:resourcetype/><D:getcontentlength>%lld</D:getcontentlength><D:getcontenttype>application/octet-stream</D:getcontenttype>"
                "<D:getetag>\"%llx-%llx\"</D:getetag>",
             (long long)st->st_size, (unsigned long long)st->st_size, (unsigned long long)st->st_mtimespec.tv_sec);
    }
    adds(t, "<D:supportedlock><D:lockentry><D:lockscope><D:exclusive/></D:lockscope><D:locktype><D:write/></D:locktype></D:lockentry></D:supportedlock>"
            "<D:lockdiscovery/>");
    if (is_root) {
        struct statfs fs;
        if (statfs(root, &fs) == 0) {
            addf(t, "<D:quota-available-bytes>%llu</D:quota-available-bytes><D:quota-used-bytes>%llu</D:quota-used-bytes>",
                 (unsigned long long)fs.f_bavail * fs.f_bsize, (unsigned long long)(fs.f_blocks - fs.f_bfree) * fs.f_bsize);
        }
    }
    adds(t, "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
}

static int do_propfind(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    char local[PATH_MAX];
    struct stat st;
    if (local_path(r->path, local, sizeof local) != 0 || stat(local, &st) != 0) return respond_empty(c, r, 404, "Not Found");
    Text t = {0};
    adds(&t, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">");
    const char *slash = strrchr(r->path, '/');
    int is_root = strcmp(r->path, "/") == 0;
    add_entry(&t, r->path, is_root ? "Henge" : (slash && slash[1] ? slash + 1 : r->path), &st, is_root);
    if (S_ISDIR(st.st_mode) && r->depth != 0) {
        DIR *d = opendir(local);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strncmp(e->d_name, TEMP_PREFIX, strlen(TEMP_PREFIX))) continue;
            char child[PATH_MAX], url[PATH_MAX];
            struct stat cs;
            if (snprintf(child, sizeof child, "%s/%s", local, e->d_name) >= (int)sizeof child) continue;
            if (snprintf(url, sizeof url, "%s%s%s", r->path, r->path[strlen(r->path) - 1] == '/' ? "" : "/", e->d_name) >= (int)sizeof url) continue;
            if (lstat(child, &cs) != 0 || !(S_ISREG(cs.st_mode) || S_ISDIR(cs.st_mode))) continue; /* no links out of the folder */
            add_entry(&t, url, e->d_name, &cs, 0);
        }
        if (d) closedir(d);
    }
    adds(&t, "</D:multistatus>");
    int rc = respond(c, r, 207, "Multi-Status", "Content-Type: application/xml; charset=utf-8\r\n", t.s, t.len);
    free(t.s);
    return rc;
}

static int do_proppatch(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    Text t = {0};
    adds(&t, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>");
    add_url(&t, r->path);
    adds(&t, "</D:href><D:propstat><D:prop/><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response></D:multistatus>");
    int rc = respond(c, r, 207, "Multi-Status", "Content-Type: application/xml; charset=utf-8\r\n", t.s, t.len);
    free(t.s);
    return rc;
}

static int do_get(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    char local[PATH_MAX];
    struct stat st;
    if (local_path(r->path, local, sizeof local) != 0) return respond_empty(c, r, 404, "Not Found");
    int fd = open(local, O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0) close(fd);
        return respond_empty(c, r, 404, "Not Found");
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        return respond_empty(c, r, 405, "Method Not Allowed");
    }
    long long from = 0, to = st.st_size - 1;
    int partial = 0;
    if (r->range_from >= 0 || r->range_to >= 0) {
        if (r->range_from < 0) { /* the last N bytes */
            from = st.st_size > r->range_to ? st.st_size - r->range_to : 0;
        } else {
            from = r->range_from;
            if (r->range_to >= 0 && r->range_to < to) to = r->range_to;
        }
        if (from >= st.st_size && st.st_size > 0) {
            close(fd);
            char h[80];
            snprintf(h, sizeof h, "Content-Range: bytes */%lld\r\n", (long long)st.st_size);
            return respond(c, r, 416, "Range Not Satisfiable", h, NULL, 0);
        }
        partial = 1;
    }
    long long len = st.st_size ? to - from + 1 : 0;
    char modified[64];
    struct tm tm;
    gmtime_r(&st.st_mtimespec.tv_sec, &tm);
    strftime(modified, sizeof modified, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    char head[1024];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %s\r\nServer: Henge\r\nContent-Length: %lld\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\n"
                     "Last-Modified: %s\r\nETag: \"%llx-%llx\"\r\n",
                     partial ? "206 Partial Content" : "200 OK", len, modified, (unsigned long long)st.st_size,
                     (unsigned long long)st.st_mtimespec.tv_sec);
    if (partial) n += snprintf(head + n, sizeof head - (size_t)n, "Content-Range: bytes %lld-%lld/%lld\r\n", from, to, (long long)st.st_size);
    n += snprintf(head + n, sizeof head - (size_t)n, "%s\r\n", r->close ? "Connection: close\r\n" : "");
    int rc = send_all(c->fd, head, (size_t)n);
    if (rc == 0 && strcmp(r->method, "HEAD") != 0) {
        static const size_t piece = 256 * 1024;
        char *buf = malloc(piece);
        long long at = from, left = len;
        while (rc == 0 && left > 0) {
            ssize_t got = pread(fd, buf, (size_t)left < piece ? (size_t)left : piece, at);
            if (got <= 0) rc = -1;
            else {
                rc = send_all(c->fd, buf, (size_t)got);
                at += got, left -= got;
            }
        }
        free(buf);
    }
    close(fd);
    return rc;
}

/* Into a temporary file beside it, then renamed, so a transfer cut short leaves the old file alone. */
static int do_put(Conn *c, Request *r) {
    char local[PATH_MAX], temp[PATH_MAX];
    struct stat st;
    if (local_path(r->path, local, sizeof local) != 0) {
        body_discard(c, r);
        return respond_empty(c, r, 403, "Forbidden");
    }
    int existed = stat(local, &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) {
        if (body_discard(c, r) != 0) return -1;
        return respond_empty(c, r, 405, "Method Not Allowed");
    }
    char *slash = strrchr(local, '/');
    size_t dir_len = (size_t)(slash - local);
    if (snprintf(temp, sizeof temp, "%.*s/" TEMP_PREFIX "XXXXXX", (int)dir_len, local) >= (int)sizeof temp) {
        body_discard(c, r);
        return respond_empty(c, r, 414, "URI Too Long");
    }
    int fd = mkstemp(temp);
    if (fd < 0) {
        int gone = errno == ENOENT;
        if (body_discard(c, r) != 0) return -1;
        return gone ? respond_empty(c, r, 409, "Conflict") : respond_empty(c, r, 507, "Insufficient Storage");
    }
    fchmod(fd, 0644);
    body_ready(c, r);
    static const size_t piece = 256 * 1024;
    char *buf = malloc(piece);
    int full = 0;
    for (;;) {
        ssize_t got = body_read(c, r, buf, piece);
        if (got < 0) { /* the client went away: no response to give */
            close(fd);
            unlink(temp);
            free(buf);
            return -1;
        }
        if (got == 0) break;
        if (!full) {
            for (ssize_t done = 0; done < got;) {
                ssize_t w = write(fd, buf + done, (size_t)(got - done));
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) {
                    full = 1;
                    break;
                }
                done += w;
            }
        }
    }
    free(buf);
    close(fd);
    if (full || rename(temp, local) != 0) {
        unlink(temp);
        return respond_empty(c, r, 507, "Insufficient Storage");
    }
    return existed ? respond_empty(c, r, 204, "No Content") : respond_empty(c, r, 201, "Created");
}

static int remove_path(const char *local) {
    struct stat st;
    if (lstat(local, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) return removefile(local, NULL, REMOVEFILE_RECURSIVE);
    return unlink(local);
}

static int do_delete(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    char local[PATH_MAX];
    if (strcmp(r->path, "/") == 0 || local_path(r->path, local, sizeof local) != 0) return respond_empty(c, r, 403, "Forbidden");
    if (remove_path(local) != 0) return errno == ENOENT ? respond_empty(c, r, 404, "Not Found") : respond_empty(c, r, 403, "Forbidden");
    return respond_empty(c, r, 204, "No Content");
}

static int do_mkcol(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    char local[PATH_MAX];
    if (local_path(r->path, local, sizeof local) != 0) return respond_empty(c, r, 403, "Forbidden");
    if (mkdir(local, 0755) == 0) return respond_empty(c, r, 201, "Created");
    if (errno == EEXIST) return respond_empty(c, r, 405, "Method Not Allowed");
    if (errno == ENOENT) return respond_empty(c, r, 409, "Conflict");
    return respond_empty(c, r, 403, "Forbidden");
}

static int do_move_copy(Conn *c, Request *r, int move) {
    if (body_discard(c, r) != 0) return -1;
    char from[PATH_MAX], to[PATH_MAX];
    struct stat st;
    if (!r->destination[0] || strcmp(r->path, "/") == 0 || local_path(r->path, from, sizeof from) != 0 ||
        local_path(r->destination, to, sizeof to) != 0)
        return respond_empty(c, r, 400, "Bad Request");
    if (stat(from, &st) != 0) return respond_empty(c, r, 404, "Not Found");
    int existed = stat(to, &st) == 0;
    if (existed && !r->overwrite) return respond_empty(c, r, 412, "Precondition Failed");
    if (existed && strcmp(from, to) != 0 && remove_path(to) != 0) return respond_empty(c, r, 403, "Forbidden");
    int rc = move ? rename(from, to) : copyfile(from, to, NULL, COPYFILE_ALL | COPYFILE_RECURSIVE);
    if (rc != 0) return errno == ENOENT ? respond_empty(c, r, 409, "Conflict") : respond_empty(c, r, 403, "Forbidden");
    return existed ? respond_empty(c, r, 204, "No Content") : respond_empty(c, r, 201, "Created");
}

static int do_lock(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    static unsigned long counter;
    char token[128];
    const char *open_ = strchr(r->if_header, '<'), *end = open_ ? strchr(open_, '>') : NULL;
    if (open_ && end && (size_t)(end - open_) < sizeof token) { /* a refresh keeps its token */
        snprintf(token, sizeof token, "%.*s", (int)(end - open_ - 1), open_ + 1);
    } else {
        snprintf(token, sizeof token, "opaquelocktoken:henge-%lx-%lx", (unsigned long)time(NULL), __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED));
    }
    int status = 200;
    char local[PATH_MAX];
    struct stat st;
    if (local_path(r->path, local, sizeof local) != 0) return respond_empty(c, r, 403, "Forbidden");
    if (stat(local, &st) != 0) { /* locking a name that isn't there makes an empty file */
        int fd = open(local, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) return respond_empty(c, r, 409, "Conflict");
        close(fd);
        status = 201;
    }
    Text t = {0};
    addf(&t, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
             "<D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/></D:lockscope><D:depth>0</D:depth>"
             "<D:timeout>Second-3600</D:timeout><D:locktoken><D:href>%s</D:href></D:locktoken></D:activelock></D:lockdiscovery></D:prop>",
         token);
    char headers[256];
    snprintf(headers, sizeof headers, "Content-Type: application/xml; charset=utf-8\r\nLock-Token: <%s>\r\n", token);
    int rc = respond(c, r, status, status == 201 ? "Created" : "OK", headers, t.s, t.len);
    free(t.s);
    return rc;
}

/* ---- /.henge/ ---- */

/* What "curl -s http://10.0.2.4/.henge/install | sh" runs in macOS. */
static const char install_script[] =
    "#!/bin/sh\n"
    "D=\"$HOME/Library/Application Support/Henge\"\n"
    "P=\"$HOME/Library/LaunchAgents/com.henge.agent.plist\"\n"
    "mkdir -p \"$D\" \"$HOME/Library/LaunchAgents\"\n"
    "launchctl bootout \"gui/$(id -u)/com.henge.agent\" 2>/dev/null\n"
    "curl -fsS http://" HV_SHARE_ADDR "/.henge/agent | gunzip > \"$D/henge-agent\" || { echo 'Could not reach Henge.'; exit 1; }\n"
    "chmod 755 \"$D/henge-agent\"\n"
    "cat > \"$P\" <<EOF\n"
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\"><dict>\n"
    "<key>Label</key><string>com.henge.agent</string>\n"
    "<key>ProgramArguments</key><array><string>$D/henge-agent</string></array>\n"
    "<key>RunAtLoad</key><true/><key>KeepAlive</key><true/>\n"
    "<key>LimitLoadToSessionType</key><string>Aqua</string>\n"
    "<key>ProcessType</key><string>Interactive</string>\n"
    "</dict></plist>\n"
    "EOF\n"
    "launchctl bootstrap \"gui/$(id -u)\" \"$P\"\n"
    "echo 'Henge is set up. You can close this window.'\n";

static int serve_file(Conn *c, Request *r, const char *path) {
    if (body_discard(c, r) != 0) return -1;
    int fd = path[0] ? open(path, O_RDONLY) : -1;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0) close(fd);
        return respond_empty(c, r, 404, "Not Found");
    }
    char *data = malloc((size_t)st.st_size);
    ssize_t got = data ? pread(fd, data, (size_t)st.st_size, 0) : -1;
    close(fd);
    int rc = got == st.st_size ? respond(c, r, 200, "OK", "Content-Type: application/octet-stream\r\n", data, (size_t)got)
                               : respond_empty(c, r, 500, "Internal Server Error");
    free(data);
    return rc;
}

/* GET ?since=N waits up to 25 s for a newer clipboard than N. */
static int clipboard_get(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    unsigned long since = 0;
    const char *q = strstr(r->query, "since=");
    if (q) since = strtoul(q + 6, NULL, 10);
    struct timespec until;
    struct timeval now;
    gettimeofday(&now, NULL);
    until.tv_sec = now.tv_sec + 25;
    until.tv_nsec = now.tv_usec * 1000;
    pthread_mutex_lock(&clip_mu);
    agent_seen = time(NULL);
    while (device_seq <= since && pthread_cond_timedwait(&clip_cond, &clip_mu, &until) != ETIMEDOUT) {}
    agent_seen = time(NULL);
    unsigned long seq = device_seq;
    char *text = seq > since && device_clip ? strdup(device_clip) : NULL;
    pthread_mutex_unlock(&clip_mu);
    char headers[64];
    snprintf(headers, sizeof headers, "X-Henge-Seq: %lu\r\n", seq);
    int rc = text ? respond(c, r, 200, "OK", headers, text, strlen(text)) : respond(c, r, 204, "No Content", headers, NULL, 0);
    free(text);
    return rc;
}

static int clipboard_put(Conn *c, Request *r) {
    size_t len = 0;
    char *text = body_small(c, r, &len);
    if (!text) return -1;
    pthread_mutex_lock(&clip_mu);
    free(guest_clip);
    guest_clip = text;
    agent_seen = time(NULL);
    pthread_mutex_unlock(&clip_mu);
    return respond_empty(c, r, 204, "No Content");
}

/* ---- the touch link ----

   The helper keeps one connection open, upgraded from GET /.henge/link?v=VERSION&trusted=0|1, and both ends
   send lines on it. The app sends:
     hit ID X Y          what is under a finger (X, Y: 0..1 across the screen)
     tap ID X Y          what a tap on the touchscreen lands in (version 28): answered as "tap", as a hit also is
     s b|c X Y DX DY     a finger scrolls what is at X, Y: began or changed, by DX, DY (fractions of the screen)
     s e X Y VX VY       it lifted, moving at VX, VY (screens a second): glide on from that
     stop                a finger came down: stop a glide
     menu ID             how many pop-up menus are showing (version 16)
     menuclose ID        close the pop-up menus that are showing with their own Cancel action (version 18)
     ask                 show macOS's Accessibility request again
     update              fetch /.henge/agent, replace yourself, and exit (launchd starts the new one)
     bench STEPS         run the benchmark's steps (version 36; "all" or names with commas between)
   The helper sends:
     hit ID drag|scroll [press X Y W H]  the answer, and the frame (fractions of the screen) of a button that shows
                         itself pressed while it is touched, if that is what is there (version 16)
     menu ID N           N pop-up menus are showing (or, for menuclose, N were closed)
     glide 0|1           whether a glide is quick enough now that a finger landing on it only stops it
     screen away 0|1     whether the screen is locked, its display asleep or the screen saver on: macOS does
                         nothing with touchscreen touches then (version 26)
     trust 0|1           whether macOS lets it control the Mac
     p                   now and then, so a dead link shows
     note TEXT           something for Henge's log, such as an old network folder it disconnected
     focus none|text|secure  what has the keyboard's focus now: nothing to type into, a text field or text
                         view, or a password field (macOS's secure input is on)
     context SEL BEFORE AFTER KIND | context none KIND  the text around a text field's caret: how many characters
                         are selected, the text before the caret and from it (what is selected, then more), base64
                         or "-", and the field's kind: prose, code, address or plain (version 22)
     field SERIAL ID STATE KIND [X Y W H]  (version 28, in place of focus and context) one numbered line for each
                         change: STATE text, secure, none, or unknown when the helper can't tell (nothing is known to
                         have changed then); ID the focused element's, the same while it keeps the focus; KIND as
                         above; X Y W H where the field is
     ctx ID SEL BEFORE AFTER | ctx ID none  the text around the caret of field ID, as context's
     tap ID editable|secure|no|unknown ROLE [X Y W H]  what a tap or hit ID lands in, and the field's frame
     bench begin NAME TARGET_MS | bench end NAME ok|skip|fail [WHY] [KEY=NUMBER ...] | bench note TEXT |
     bench done | bench failed WHY   the benchmark as it goes (version 36): for host/bench.c
   Sends never wait: a line that would block is dropped, and the link with it, so a stuck helper can't
   hold up the app's touch handling. */

static pthread_mutex_t link_mu = PTHREAD_MUTEX_INITIALIZER;
static int link_fd = -1;
/* Which link it is: a new number for each (never 0), 0 while there is none. Under link_mu. The benchmark's lines and
   its "failed gone" carry it, so a run hears only the link it was asked on. */
static unsigned link_id, link_ids;
static int link_trusted;
static int link_version; /* the linked helper's (HV_AGENT_SINCE_*), 0 while none is */
/* Under link_mu: a helper that does what HV_AGENT_SINCE_* `since` says is linked. */
static int link_can(int since) { return link_fd >= 0 && link_version >= since; }
static int link_focus = HV_FOCUS_UNKNOWN;
static float link_focus_frame[4]; /* where the focused field is, fractions of the screen; width 0 if not said */
static char link_before[1024], link_after[1024]; /* the helper's "context", base64 */
static int link_selected, link_context_known, link_field_kind;
static unsigned link_context_seq;
static unsigned link_next;
static struct { unsigned id; int kind; int press; float frame[4]; } link_answers[16];
/* What a tap or hit lands in (version 28), by the same ids as link_answers. */
static struct { unsigned id; int answer; float frame[4]; char role[48]; } link_taps[16];
/* The last "field" line (version 28). link_focus follows it, except while it says unknown. */
static unsigned link_field_serial, link_field_id, link_generation;
static int link_field_state = HV_FOCUS_UNKNOWN, link_field_kind_said = HV_FIELD_PROSE;
static float link_field_frame[4];
/* "look" questions (version 34) and their answers: HV_TAP_WAITING until "looked" comes, HV_FOCUS_UNKNOWN once the
   link they were asked on is gone. */
static struct { unsigned id; int answer; int sure; } link_looks[8];
static void looks_forget(void) { /* under link_mu, as the link closes or another opens */
    for (size_t i = 0; i < sizeof link_looks / sizeof *link_looks; i++)
        if (link_looks[i].answer == HV_TAP_WAITING) link_looks[i].answer = HV_FOCUS_UNKNOWN;
}
static void (*_Atomic link_listener)(void);
static int touch_links_seen; /* under link_mu: a user's helper has linked since Henge started this Mac */
static unsigned menu_next;
static struct { unsigned id; int count; } menu_answers[8];
static int link_gliding; /* the helper's "glide 1", until "glide 0" or the link closes */
/* Whether a press must wait for the helper's scroll queue (version 33, input_fence.h). Under link_mu. After any scroll
   line the helper may still be posting scroll events (the line's own, or a glide it set off) until it answers a stop
   sent after that line: its stops run on the same queue as its scroll lines. Stops are numbered from 1, compared as a
   serial count (one that wrapped past 0 is still later). */
static int link_scroll_unfenced;
static unsigned link_scroll_after; /* the last stop sent when the last scroll line went: a later one comes after it */
static unsigned link_stop_last;    /* the last stop sent */
static int link_stop_late;         /* a stop went unanswered too long: not waited on until the helper answers one */
static unsigned link_stop_answered; /* the latest stop the helper answered (0 none yet) */
static unsigned link_stop_base;     /* stops up to this one went to an earlier link: the helper linked now never had them */
static void (*_Atomic glide_listener)(unsigned id, int gone);
static int link_touchscreen = -1; /* the helper's "touchscreen on|off", until the link closes */
static int link_screen_away = -1; /* the helper's "screen away 0|1" (version 26), until the link closes */
/* Whether macOS does anything with the touchscreen's touches (hv_share_touchscreen_watch). Under link_mu. Kept as the
   helper relinks (it does as it updates); asked afresh once the Mac has restarted. */
static int ts_verdict = -1;   /* 1 macOS made events of a touch, 0 it didn't (or the helper can't tell), -1 not known */
static int ts_misses;         /* gestures in a row macOS made nothing of, since it last did */
static int ts_unsure;         /* the 0 is the helper's "can't tell" (it can't see events without Accessibility): tried
                                 again once it's allowed */
static unsigned ts_watch_next, ts_watch_id;
static struct timespec ts_watch_at; /* when the waiting question was asked; tv_sec 0: none waiting */
/* The gesture asked about: under way (its answer is kept until it ends), and whether it ended as cancelled (a
   resting palm the Pencil took over from, Henge's own three-finger tap, a pause): what macOS made of one of those
   says nothing, either way, so its answer counts for nothing. */
static int ts_live, ts_cancelled;
static int ts_held = -2;      /* the answer to the gesture under way, kept until it ends; -2 none */
static int ts_always_on;      /* the app's "Native (Always)" (hv_share_touchscreen_always) */
void hv_share_touchscreen_always(int always) { __atomic_store_n(&ts_always_on, always != 0, __ATOMIC_RELAXED); }
static int ts_boot = -1;      /* the Mac's uptime (s) as the helper said when it last linked */
static int ts_booted;         /* when the Mac started (its clock, s), as the helper said when it last linked; 0 not known */

/* Under link_mu: what the helper answered for a gesture (1 macOS made events, 0 none, -1 it can't tell). Returns
   what to log: 0 nothing. */
static const char *ts_answer(int count) {
    int was = ts_verdict;
    if (count > 0) {
        ts_verdict = 1, ts_misses = 0, ts_unsure = 0;
    } else if (count == 0) {
        /* Never seen working: the first gesture it drops is enough. Seen working: a gesture macOS rightly makes
           nothing of (a resting palm, say) mustn't turn it off, so three in a row. */
        if (++ts_misses >= (ts_verdict == 1 ? 3 : 1)) ts_verdict = 0, ts_unsure = 0;
    } else if (ts_verdict != 1) {
        /* The helper can't see events (no Accessibility permission yet): nothing says touches do anything, so the
           pointer, which surely works; tried again once it's allowed. */
        ts_verdict = 0, ts_unsure = 1;
    }
    if (ts_verdict == was) return 0;
    /* Native (Always) never goes back to the pointer, so don't say it does. (The helper's check is blind to real
       touches anyway: macOS hands them to apps without any event it can see.) */
    if (ts_verdict != 1 && __atomic_load_n(&ts_always_on, __ATOMIC_RELAXED))
        return "the helper saw no pointer events come of touches (it can't see real touches); Native (Always) keeps the touchscreen";
    return ts_verdict == 1 ? "macOS takes touches from the touchscreen"
           : ts_unsure ? "the helper can't tell whether macOS takes touches (no Accessibility permission): touch is imitated with the pointer until it can"
           : "macOS did nothing with touches on the touchscreen: touch is imitated with the pointer from now on";
}
static pthread_cond_t sync_cv = PTHREAD_COND_INITIALIZER; /* under link_mu */
static unsigned sync_next, sync_answered;
static int sync_ok;

static void link_send(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void signed_in_linked(void); /* the system link, below */

static void link_told(void) {
    void (*listener)(void) = atomic_load(&link_listener);
    if (listener) listener();
}

static void glide_told(unsigned id, int gone) {
    void (*listener)(unsigned, int) = atomic_load(&glide_listener);
    if (listener) listener(id, gone);
}

static void bench_gone(unsigned link);

/* The link `link` (link_id) closed or failed, with stops up to `gone` (link_stop_last then) sent on it: no answer is
   coming to any of them, nor to a benchmark it was running. Outside link_mu. A stop sent on a link since isn't one of
   them. */
static void link_went(unsigned gone, unsigned link) {
    link_told();
    glide_told(gone, 1);
    bench_gone(link);
}

void hv_share_set_glide_listener(void (*listener)(unsigned id, int gone)) { atomic_store(&glide_listener, listener); }

void hv_share_set_listener(void (*listener)(void)) { atomic_store(&link_listener, listener); }

/* Sends one line, under link_mu: the link's link_id if that closed it (the caller tells the listeners, link_went, once
   it lets go of link_mu), else 0. */
static unsigned link_send_l(const char *line, int n) {
    if (link_fd < 0) return 0;
    ssize_t w = send(link_fd, line, (size_t)n, 0); /* doesn't wait: the link is non-blocking (link_nonblocking) */
    if (w == n) return 0;
    unsigned went = link_id;
    /* full, or broken: a half-sent line would garble the rest, so start over */
    shutdown(link_fd, SHUT_RDWR);
    link_fd = -1, link_trusted = 0, link_version = 0, link_focus = HV_FOCUS_UNKNOWN, link_gliding = 0, link_touchscreen = -1, link_screen_away = -1;
    link_scroll_unfenced = 0, link_stop_late = 0;
    link_field_state = HV_FOCUS_UNKNOWN, link_generation++, looks_forget();
    link_id = 0;
    return went;
}

static void link_send(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0 || (size_t)n >= sizeof line) return;
    pthread_mutex_lock(&link_mu);
    unsigned closed = link_send_l(line, n);
    unsigned gone = link_stop_last;
    pthread_mutex_unlock(&link_mu);
    if (closed) link_went(gone, closed);
}

int hv_share_touch_ready(void) {
    pthread_mutex_lock(&link_mu);
    int ready = link_fd >= 0 && link_trusted;
    pthread_mutex_unlock(&link_mu);
    return ready;
}

int hv_share_agent_current(void) {
    pthread_mutex_lock(&link_mu);
    int current = link_fd >= 0 && link_version == HV_AGENT_VERSION;
    pthread_mutex_unlock(&link_mu);
    return current;
}

int hv_share_agent_version(void) {
    pthread_mutex_lock(&link_mu);
    int version = link_fd >= 0 ? link_version : 0;
    pthread_mutex_unlock(&link_mu);
    return version;
}

int hv_share_focus(void) {
    pthread_mutex_lock(&link_mu);
    int focus = link_can(HV_AGENT_SINCE_FIELD) ? link_focus : HV_FOCUS_UNKNOWN;
    pthread_mutex_unlock(&link_mu);
    return focus;
}

int hv_share_focus_frame(float frame[4]) {
    pthread_mutex_lock(&link_mu);
    int known = link_can(HV_AGENT_SINCE_FIELD) && link_focus > HV_FOCUS_NONE && link_focus_frame[2] > 0;
    if (known) memcpy(frame, link_focus_frame, sizeof link_focus_frame);
    pthread_mutex_unlock(&link_mu);
    return known;
}

unsigned hv_share_context(char *before, size_t before_cap, char *after, size_t after_cap, int *selected, int *known, int *kind) {
    pthread_mutex_lock(&link_mu);
    int live = link_can(HV_AGENT_SINCE_FIELD);
    snprintf(before, before_cap, "%s", live ? link_before : "");
    snprintf(after, after_cap, "%s", live ? link_after : "");
    *selected = live ? link_selected : 0;
    *known = live && link_context_known;
    *kind = live ? link_field_kind : HV_FIELD_PROSE;
    unsigned seq = live ? link_context_seq : 0;
    pthread_mutex_unlock(&link_mu);
    return seq;
}

int hv_share_field(HvShareField *f) {
    pthread_mutex_lock(&link_mu);
    int live = link_can(HV_AGENT_SINCE_FIELD);
    f->serial = link_field_serial;
    f->id = link_field_id;
    f->generation = link_generation;
    f->state = live ? link_field_state : HV_FOCUS_UNKNOWN;
    f->kind = live ? link_field_kind_said : HV_FIELD_PROSE;
    memcpy(f->frame, link_field_frame, sizeof f->frame);
    if (!live) memset(f->frame, 0, sizeof f->frame);
    pthread_mutex_unlock(&link_mu);
    return live;
}

static double unit(double v);

static void tap_slot(unsigned id) { /* under link_mu */
    link_taps[id % 16].id = id;
    link_taps[id % 16].answer = HV_TAP_WAITING;
    memset(link_taps[id % 16].frame, 0, sizeof link_taps[0].frame);
    link_taps[id % 16].role[0] = 0;
}

unsigned hv_share_tap(double x, double y) {
    pthread_mutex_lock(&link_mu);
    unsigned id = 0;
    if (link_can(HV_AGENT_SINCE_FIELD)) {
        if (!++link_next) link_next = 1;
        id = link_next;
        link_answers[id % 16].id = 0; /* not a hit */
        tap_slot(id);
    }
    pthread_mutex_unlock(&link_mu);
    if (id) link_send("tap %u %.5f %.5f\n", id, unit(x), unit(y));
    return id;
}

int hv_share_tap_answer(unsigned id, float frame[4], char *role, size_t role_cap) {
    pthread_mutex_lock(&link_mu);
    int known = id && link_taps[id % 16].id == id;
    int answer = known ? link_taps[id % 16].answer : HV_TAP_UNKNOWN;
    if (frame) {
        if (known) memcpy(frame, link_taps[id % 16].frame, sizeof link_taps[0].frame);
        else memset(frame, 0, 4 * sizeof *frame);
    }
    if (role && role_cap) snprintf(role, role_cap, "%s", known ? link_taps[id % 16].role : "");
    pthread_mutex_unlock(&link_mu);
    return answer;
}

int hv_share_touch_linked(void) {
    pthread_mutex_lock(&link_mu);
    int linked = link_fd >= 0;
    pthread_mutex_unlock(&link_mu);
    return linked;
}

void hv_share_touch_ask(void) { link_send("ask\n"); }
void hv_share_tools_open(void) { link_send("tools\n"); }

/* What the helper said of the drives it was asked to eject, newest over oldest. */
static struct { char serial[48]; int answer; } ejects[8];
static unsigned eject_next;

static int eject_slot(const char *serial) { /* under link_mu */
    for (int i = 0; i < 8; i++)
        if (ejects[i].serial[0] && !strcmp(ejects[i].serial, serial)) return i;
    return -1;
}

void hv_share_eject(const char *serial) {
    if (!serial || !*serial || strlen(serial) >= sizeof ejects[0].serial || strpbrk(serial, " \n\r")) return;
    pthread_mutex_lock(&link_mu);
    int i = eject_slot(serial);
    if (i < 0) i = (int)(eject_next++ % 8), snprintf(ejects[i].serial, sizeof ejects[i].serial, "%s", serial);
    ejects[i].answer = -1;
    pthread_mutex_unlock(&link_mu);
    link_send("eject %s\n", serial);
}

static struct { char serial[48]; int answer; } formats[4];
static unsigned format_next;

void hv_share_format(const char *serial, const char *name) {
    if (!serial || !*serial || strlen(serial) >= sizeof formats[0].serial || strpbrk(serial, " \n\r") || !name || !*name) return;
    char clean[64];
    size_t n = 0;
    for (const char *p = name; *p && n < sizeof clean - 1; p++) clean[n++] = *p == '\n' || *p == '\r' ? ' ' : *p;
    clean[n] = 0;
    pthread_mutex_lock(&link_mu);
    int i = -1;
    for (int k = 0; k < 4; k++)
        if (!strcmp(formats[k].serial, serial)) i = k;
    if (i < 0) i = (int)(format_next++ % 4), snprintf(formats[i].serial, sizeof formats[i].serial, "%s", serial);
    formats[i].answer = -1;
    pthread_mutex_unlock(&link_mu);
    link_send("format %s %s\n", serial, clean);
}

int hv_share_format_answer(const char *serial) {
    int answer = -1;
    pthread_mutex_lock(&link_mu);
    for (int k = 0; k < 4 && serial; k++)
        if (formats[k].serial[0] && !strcmp(formats[k].serial, serial)) answer = formats[k].answer;
    pthread_mutex_unlock(&link_mu);
    return answer;
}

/* The shutdown or restart asked of the helper, the newest only (under link_mu). */
static unsigned power_id;
static int power_answer = HV_POWER_WAITING;

static void (*_Atomic bench_listener)(unsigned link, const char *line);

void hv_share_set_bench_listener(void (*listener)(unsigned link, const char *line)) { atomic_store(&bench_listener, listener); }

/* The link `link` went (closed, failed, or a newer helper's replaced it): no more is coming on it. Outside link_mu.
   bench.c takes it only for the link its run was asked on. */
static void bench_gone(unsigned link) {
    void (*listener)(unsigned, const char *) = atomic_load(&bench_listener);
    if (listener && link) listener(link, "failed gone");
}

void hv_share_set_bench_video(const char *path) { snprintf(bench_video, sizeof bench_video, "%s", path ? path : ""); }

unsigned hv_share_bench(const char *steps) {
    if (!steps || !*steps || strlen(steps) > 200 || strpbrk(steps, " \r\n")) return 0;
    char line[256];
    int n = snprintf(line, sizeof line, "bench %s\n", steps);
    pthread_mutex_lock(&link_mu);
    unsigned link = link_can(HV_AGENT_SINCE_BENCH) ? link_id : 0, closed = link ? link_send_l(line, n) : 0;
    unsigned gone = link_stop_last;
    pthread_mutex_unlock(&link_mu);
    /* Sending it closed the link: no run. The benchmark isn't told (its caller holds bench.c's lock; it hears 0). */
    if (closed) link_went(gone, 0);
    return closed ? 0 : link;
}

unsigned hv_share_power(int restart) {
    pthread_mutex_lock(&link_mu);
    unsigned id = 0;
    if (link_fd >= 0 && link_version >= HV_AGENT_SINCE_POWER) {
        id = ++power_id;
        if (!id) id = ++power_id;
        power_answer = HV_POWER_WAITING;
    }
    pthread_mutex_unlock(&link_mu);
    if (id) link_send("power %s %u\n", restart ? "restart" : "shutdown", id);
    return id;
}

int hv_share_power_answer(unsigned id) {
    pthread_mutex_lock(&link_mu);
    int answer = id && id == power_id ? power_answer : HV_POWER_WAITING;
    pthread_mutex_unlock(&link_mu);
    return answer;
}

int hv_share_eject_answer(const char *serial) {
    pthread_mutex_lock(&link_mu);
    int i = serial ? eject_slot(serial) : -1, answer = i < 0 ? -1 : ejects[i].answer;
    pthread_mutex_unlock(&link_mu);
    return answer;
}

unsigned hv_share_look(void) {
    pthread_mutex_lock(&link_mu);
    unsigned id = 0;
    if (link_can(HV_AGENT_SINCE_LOOK)) {
        if (!++link_next) link_next = 1;
        id = link_next;
        link_looks[id % 8].id = id;
        link_looks[id % 8].answer = HV_TAP_WAITING;
        link_looks[id % 8].sure = 0;
    }
    pthread_mutex_unlock(&link_mu);
    return id;
}

void hv_share_look_send(unsigned id) {
    pthread_mutex_lock(&link_mu);
    int asked = id && link_looks[id % 8].id == id && link_looks[id % 8].answer == HV_TAP_WAITING;
    pthread_mutex_unlock(&link_mu);
    if (asked) link_send("look %u\n", id); /* a link opened since has it unknown already, and ignores its answer */
}

int hv_share_look_answer(unsigned id, int *sure) {
    pthread_mutex_lock(&link_mu);
    int known = id && link_looks[id % 8].id == id;
    int answer = known ? link_looks[id % 8].answer : HV_FOCUS_UNKNOWN;
    if (sure) *sure = known && answer == HV_FOCUS_SECURE && link_looks[id % 8].sure;
    pthread_mutex_unlock(&link_mu);
    return answer;
}

static double unit(double v) { return v > 0 ? v < 1 ? v : 1 : 0; } /* also NaN */

unsigned hv_share_touch_hit(double x, double y) {
    pthread_mutex_lock(&link_mu);
    unsigned id = 0;
    if (link_fd >= 0) {
        if (!++link_next) link_next = 1;
        id = link_next;
        link_answers[id % 16].id = id;
        link_answers[id % 16].kind = HV_TOUCH_UNKNOWN;
        link_answers[id % 16].press = 0;
        if (link_can(HV_AGENT_SINCE_FIELD)) tap_slot(id); /* such a helper answers what it lands in too */
        else link_taps[id % 16].id = 0; /* an older one doesn't: not known, rather than waited for */
    }
    pthread_mutex_unlock(&link_mu);
    if (id) link_send("hit %u %.5f %.5f\n", id, unit(x), unit(y));
    return id;
}

int hv_share_touch_answer(unsigned id) {
    pthread_mutex_lock(&link_mu);
    int kind = id && link_answers[id % 16].id == id ? link_answers[id % 16].kind : HV_TOUCH_UNKNOWN;
    pthread_mutex_unlock(&link_mu);
    return kind;
}

int hv_share_touch_press(unsigned id, float frame[4]) {
    pthread_mutex_lock(&link_mu);
    int press = id && link_answers[id % 16].id == id && link_answers[id % 16].kind != HV_TOUCH_UNKNOWN && link_answers[id % 16].press;
    if (press) memcpy(frame, link_answers[id % 16].frame, sizeof link_answers[0].frame);
    pthread_mutex_unlock(&link_mu);
    return press;
}

unsigned hv_share_menu_ask(void) {
    pthread_mutex_lock(&link_mu);
    unsigned id = 0;
    if (link_can(HV_AGENT_SINCE_MENUS)) { /* an older helper would not answer */
        if (!++menu_next) menu_next = 1;
        id = menu_next;
        menu_answers[id % 8].id = id;
        menu_answers[id % 8].count = -1;
    }
    pthread_mutex_unlock(&link_mu);
    if (id) link_send("menu %u\n", id);
    return id;
}

unsigned hv_share_menu_close(void) {
    pthread_mutex_lock(&link_mu);
    unsigned id = 0;
    if (link_can(HV_AGENT_SINCE_MENUS)) {
        if (!++menu_next) menu_next = 1;
        id = menu_next;
        menu_answers[id % 8].id = id;
        menu_answers[id % 8].count = -1;
    }
    pthread_mutex_unlock(&link_mu);
    if (id) link_send("menuclose %u\n", id);
    return id;
}

int hv_share_menu_answer(unsigned id) {
    pthread_mutex_lock(&link_mu);
    int count = id && menu_answers[id % 8].id == id ? menu_answers[id % 8].count : -1;
    pthread_mutex_unlock(&link_mu);
    return count;
}

int hv_share_touch_gliding(void) {
    pthread_mutex_lock(&link_mu);
    int gliding = link_can(HV_AGENT_SINCE_TOUCH) && link_gliding;
    pthread_mutex_unlock(&link_mu);
    return gliding;
}

void hv_share_touch_scroll(int phase, double x, double y, double dx, double dy) {
    char p = phase == HV_SCROLL_BEGAN ? 'b' : phase == HV_SCROLL_ENDED ? 'e' : 'c';
    if (!(dx == dx) || !(dy == dy)) dx = dy = 0;
    char line[128];
    int n = snprintf(line, sizeof line, "s %c %.5f %.5f %.6f %.6f\n", p, unit(x), unit(y), dx, dy);
    if (n <= 0 || (size_t)n >= sizeof line) return; /* a speed no finger makes */
    /* Noted and sent under one lock: a stop numbered after this line is also sent after it */
    pthread_mutex_lock(&link_mu);
    link_scroll_unfenced = 1, link_scroll_after = link_stop_last; /* posted, and maybe a glide, until a later stop is answered */
    unsigned closed = link_send_l(line, n);
    unsigned gone = link_stop_last;
    pthread_mutex_unlock(&link_mu);
    if (closed) link_went(gone, closed);
}

/* Numbers and sends a stop, under link_mu: stops reach the helper in the order of their numbers, and after every
   scroll line sent before them. Returns its number (0: an older helper's plain stop, or none: no link), and sets
   *closed to the link's link_id if sending it closed the link. */
static unsigned send_stop_l(unsigned *closed) {
    char line[32];
    if (!link_can(HV_AGENT_SINCE_STOPPED)) {
        *closed = link_send_l("stop\n", 5); /* an older helper knows only this, and doesn't answer */
        return 0;
    }
    if (!++link_stop_last) link_stop_last = 1; /* never 0 */
    unsigned id = link_stop_last;
    *closed = link_send_l(line, snprintf(line, sizeof line, "stop %u\n", id));
    return *closed ? 0 : id;
}

void hv_share_touch_stop(void) {
    unsigned closed = 0;
    pthread_mutex_lock(&link_mu);
    send_stop_l(&closed);
    unsigned gone = link_stop_last;
    pthread_mutex_unlock(&link_mu);
    if (closed) link_went(gone, closed);
}

unsigned hv_share_glide_fence(void) {
    unsigned closed = 0;
    unsigned id = 0;
    pthread_mutex_lock(&link_mu);
    if (link_scroll_unfenced && !link_stop_late && link_can(HV_AGENT_SINCE_STOPPED)) {
        id = link_stop_last;
        if ((int)(id - link_scroll_after) <= 0) id = send_stop_l(&closed); /* none sent since the last scroll line */
        /* else one sent early, after it, is waited for */
    }
    unsigned gone = link_stop_last;
    pthread_mutex_unlock(&link_mu);
    if (closed) link_went(gone, closed); /* the link went as it was sent: no answer comes (id is 0) */
    return id;
}

void hv_share_touch_late(unsigned id) {
    pthread_mutex_lock(&link_mu);
    /* Not if the stop went to an earlier link (the helper linked now never had it), nor if it answered this stop, or
       a later one, meanwhile (the answer and the time limit can cross) */
    if ((int)(id - link_stop_base) > 0 && (!link_stop_answered || (int)(link_stop_answered - id) < 0)) link_stop_late = 1;
    pthread_mutex_unlock(&link_mu);
}

int hv_share_guest_sync(unsigned timeout_ms) {
    pthread_mutex_lock(&link_mu);
    if (!link_can(HV_AGENT_SINCE_SYNC)) { /* an older helper would not answer */
        pthread_mutex_unlock(&link_mu);
        return -1;
    }
    if (!++sync_next) sync_next = 1;
    unsigned id = sync_next;
    pthread_mutex_unlock(&link_mu);
    link_send("sync %u\n", id);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
    pthread_mutex_lock(&link_mu);
    int r = -1;
    for (;;) {
        if (sync_answered == id) {
            r = sync_ok ? 0 : -1;
            break;
        }
        if (link_fd < 0 || pthread_cond_timedwait(&sync_cv, &link_mu, &until) != 0) break;
    }
    pthread_mutex_unlock(&link_mu);
    return r;
}

static int query_int(const char *query, const char *name) {
    size_t n = strlen(name);
    for (const char *q = query; (q = strstr(q, name)); q += n)
        if ((q == query || q[-1] == '&') && q[n] == '=') return atoi(q + n + 1);
    return 0;
}

/* A line from the helper. One too long for `cap` is cut short, the rest passed over, rather than ending the link:
 * a long note from the helper (each is up to 159 characters in the log) once dropped it, with the note. */
static int link_line(Conn *c, char *line, size_t cap) {
    size_t n = 0;
    for (;;) {
        char ch;
        if (conn_read(c, &ch, 1) != 1) return -1;
        if (ch == '\n') break;
        if (n + 1 < cap) line[n++] = ch;
    }
    if (n && line[n - 1] == '\r') n--;
    line[n] = 0;
    return 0;
}

static int field_kind(const char *kind) {
    return !strcmp(kind, "code") ? HV_FIELD_CODE : !strcmp(kind, "address") ? HV_FIELD_ADDRESS : !strcmp(kind, "plain") ? HV_FIELD_PLAIN
                                                                                                                       : HV_FIELD_PROSE;
}

static int touch_link(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    int version = query_int(r->query, "v"), trusted = query_int(r->query, "trusted");
    int up = query_int(r->query, "up"), boot = query_int(r->query, "boot");
    static const char head[] = "HTTP/1.1 101 Switching Protocols\r\nServer: Henge\r\nUpgrade: henge-link\r\nConnection: Upgrade\r\n\r\n";
    if (send_all(c->fd, head, sizeof head - 1) != 0 || link_nonblocking(c->fd) != 0) return -1;
    int current = version == HV_AGENT_VERSION;
    pthread_mutex_lock(&link_mu);
    unsigned replaced = link_fd >= 0 ? link_id : 0;
    touch_links_seen = 1; /* someone is logged in: a new system key is no longer taken this run (system_link) */
    if (link_fd >= 0) shutdown(link_fd, SHUT_RDWR); /* the newest helper is the one */
    link_fd = c->fd;
    if (!++link_ids) link_ids = 1; /* never 0 */
    unsigned mine = link_id = link_ids;
    link_version = version > 0 ? version : 0;
    link_trusted = trusted && version >= HV_AGENT_SINCE_TOUCH;
    link_focus = HV_FOCUS_UNKNOWN;
    link_context_known = 0, link_field_kind = HV_FIELD_PROSE, link_context_seq++;
    link_field_state = HV_FOCUS_UNKNOWN, link_generation++, looks_forget();
    link_gliding = 0;
    link_scroll_unfenced = 0, link_stop_late = 0; /* the old helper's glide went with it; a new one has none */
    unsigned gone = link_stop_base = link_stop_last; /* sent to the old helper: no answer is coming */
    link_touchscreen = -1;
    link_screen_away = -1;
    ts_watch_at.tv_sec = 0, ts_live = 0, ts_held = -2;
    /* The Mac restarted (it started at another time; or, from a helper that doesn't say, its uptime went back): whether
       macOS takes touches is asked again. So it is when the helper can now see events and couldn't before. */
    int booted = query_int(r->query, "booted");
    int restarted = booted && ts_booted ? (booted > ts_booted + 60 || booted < ts_booted - 60) : ts_boot >= 0 && boot + 5 < ts_boot;
    int retry = restarted || (ts_unsure && link_trusted);
    if (retry && ts_verdict >= 0) {
        if (!restarted) restarted = -1;
    } else {
        restarted = 0;
    }
    if (retry) ts_verdict = -1, ts_misses = 0, ts_unsure = 0;
    ts_boot = boot;
    if (booted) ts_booted = booted;
    pthread_mutex_unlock(&link_mu);
    glide_told(gone, 1); /* at once: a press waiting on the old helper goes (one waiting on this one, later, doesn't) */
    bench_gone(replaced); /* so does a benchmark it was running: its link's own end won't say (it isn't the link then) */
    if (restarted) fprintf(stderr, "share: %s: the touchscreen is tried again\n", restarted > 0 ? "the Mac restarted" : "the helper can see events now");
    fprintf(stderr, "share: the helper linked (version %d%s), %s\n", version,
            current ? "" : ", out of date: to be updated, and what it can do is used meanwhile",
            version < HV_AGENT_SINCE_TOUCH ? "too old for touch"
            : trusted                      ? "allowed to control the Mac"
                                           : "waiting for Accessibility permission");
    if (boot) fprintf(stderr, "share: the helper linked %.1f s after it started\n", up / 1000.0); /* its uptime is the device's, not the Mac's */
    signed_in_linked();
    link_told();
    if (!current && agent_path[0]) link_send("update\n");
    char line[2304]; /* a "context" line is the longest */
    while (link_line(c, line, sizeof line) == 0) {
        pthread_mutex_lock(&clip_mu);
        agent_seen = time(NULL);
        pthread_mutex_unlock(&clip_mu);
        unsigned id;
        char kind[16];
        int count;
        if (sscanf(line, "hit %u %15s", &id, kind) == 2) {
            float f[4] = {0, 0, 0, 0};
            int press = sscanf(line, "hit %*u %*15s press %f %f %f %f", &f[0], &f[1], &f[2], &f[3]) == 4 && f[2] > 0 && f[3] > 0 &&
                        f[0] == f[0] && f[1] == f[1];
            pthread_mutex_lock(&link_mu);
            if (link_answers[id % 16].id == id) {
                link_answers[id % 16].press = press;
                memcpy(link_answers[id % 16].frame, f, sizeof f);
                link_answers[id % 16].kind = !strcmp(kind, "drag") ? HV_TOUCH_DRAG : HV_TOUCH_SCROLL; /* last: it says the rest is in */
            }
            pthread_mutex_unlock(&link_mu);
        } else if (sscanf(line, "menu %u %d", &id, &count) == 2) {
            pthread_mutex_lock(&link_mu);
            if (menu_answers[id % 8].id == id) menu_answers[id % 8].count = count < 0 ? 0 : count;
            pthread_mutex_unlock(&link_mu);
        } else if (sscanf(line, "stopped %u", &id) == 1 && id) {
            pthread_mutex_lock(&link_mu);
            int ours = link_fd == c->fd;
            if (ours) {
                link_stop_late = 0; /* it answers again */
                if (!link_stop_answered || (int)(id - link_stop_answered) > 0) link_stop_answered = id;
                if ((int)(id - link_scroll_after) > 0) link_scroll_unfenced = 0; /* stopped after the last scroll line */
            }
            pthread_mutex_unlock(&link_mu);
            if (ours) glide_told(id, 0);
        } else if (!strncmp(line, "glide ", 6)) {
            pthread_mutex_lock(&link_mu);
            if (link_fd == c->fd) link_gliding = atoi(line + 6) != 0;
            pthread_mutex_unlock(&link_mu);
        } else if (!strncmp(line, "touchscreen ", 12)) {
            int on = !strcmp(line + 12, "on");
            pthread_mutex_lock(&link_mu);
            int was = link_touchscreen;
            if (link_fd == c->fd) link_touchscreen = on;
            pthread_mutex_unlock(&link_mu);
            if (was != on) fprintf(stderr, "share: the helper says macOS %s\n", on ? "has the touchscreen" : "has no touchscreen");
        } else if (!strncmp(line, "screen away ", 12)) {
            int away = atoi(line + 12) != 0;
            pthread_mutex_lock(&link_mu);
            int was = link_screen_away;
            if (link_fd == c->fd) link_screen_away = away;
            pthread_mutex_unlock(&link_mu);
            if (was != away)
                fprintf(stderr, "share: the helper says the Mac's screen is %s\n",
                        away ? "locked, asleep or behind the screen saver: fingers go with the pointer, which wakes it"
                             : "in use");
        } else if (sscanf(line, "touchseen %u %d", &id, &count) == 2) {
            pthread_mutex_lock(&link_mu);
            const char *say = 0;
            if (link_fd == c->fd && id == ts_watch_id && ts_watch_at.tv_sec != 0) {
                ts_watch_at.tv_sec = 0;
                if (ts_cancelled) {
                    /* says nothing */
                } else if (ts_live) {
                    ts_held = count; /* it counts once the gesture has ended as a touch */
                } else {
                    say = ts_answer(count);
                }
            }
            pthread_mutex_unlock(&link_mu);
            if (say) fprintf(stderr, "share: %s\n", say);
        } else if (!strncmp(line, "synced ", 7)) {
            unsigned sid = 0;
            int synced = 0, failed = 0;
            sscanf(line + 7, "%u %d %d", &sid, &synced, &failed);
            pthread_mutex_lock(&link_mu);
            sync_answered = sid;
            sync_ok = failed == 0;
            pthread_cond_broadcast(&sync_cv);
            pthread_mutex_unlock(&link_mu);
            fprintf(stderr, "share: macOS wrote its disk cache out (%d volume%s%s)\n", synced, synced == 1 ? "" : "s",
                    failed ? ", some could not be" : "");
        } else if (!strncmp(line, "ejected ", 8)) {
            char serial[48] = "";
            int ok = 0;
            if (sscanf(line + 8, "%47s %d", serial, &ok) == 2) {
                pthread_mutex_lock(&link_mu);
                int i = eject_slot(serial);
                if (i >= 0) ejects[i].answer = ok ? 1 : 0;
                pthread_mutex_unlock(&link_mu);
            }
        } else if (!strncmp(line, "power ", 6)) {
            unsigned pid = 0;
            char what[16] = "";
            if (sscanf(line + 6, "%u %15s", &pid, what) == 2) {
                int answer = !strcmp(what, "sent") ? HV_POWER_SENT : !strcmp(what, "asking") ? HV_POWER_ASKING
                           : !strcmp(what, "cancelled") ? HV_POWER_CANCELLED : HV_POWER_FAILED;
                pthread_mutex_lock(&link_mu);
                if (pid == power_id && power_answer != HV_POWER_CANCELLED) power_answer = answer; /* a late "sent" */
                pthread_mutex_unlock(&link_mu);
                fprintf(stderr, "share: shutdown or restart %u: %s\n", pid, line + 6);
            }
        } else if (!strncmp(line, "formatted ", 10)) {
            char serial[48] = "";
            int ok = 0;
            if (sscanf(line + 10, "%47s %d", serial, &ok) == 2) {
                pthread_mutex_lock(&link_mu);
                for (int k = 0; k < 4; k++)
                    if (formats[k].serial[0] && !strcmp(formats[k].serial, serial)) formats[k].answer = ok ? 1 : 0;
                pthread_mutex_unlock(&link_mu);
            }
        } else if (!strncmp(line, "trust ", 6)) {
            int now = atoi(line + 6) && version >= HV_AGENT_SINCE_TOUCH;
            pthread_mutex_lock(&link_mu);
            if (link_fd == c->fd) link_trusted = now;
            int retry = link_fd == c->fd && now && ts_unsure; /* it can see events now: whether macOS takes touches is asked again */
            if (retry) ts_verdict = -1, ts_misses = 0, ts_unsure = 0, ts_live = 0, ts_held = -2, ts_watch_at.tv_sec = 0;
            pthread_mutex_unlock(&link_mu);
            if (retry) fprintf(stderr, "share: the helper can see events now: the touchscreen is tried again\n");
            fprintf(stderr, "share: the helper is %s\n", now ? "allowed to control the Mac" : "not allowed to control the Mac");
        } else if (!strncmp(line, "field ", 6)) {
            unsigned serial = 0, fid = 0;
            char state[16] = "", fkind[16] = "";
            float f[4] = {0, 0, 0, 0};
            int got = sscanf(line + 6, "%u %u %15s %15s %f %f %f %f", &serial, &fid, state, fkind, &f[0], &f[1], &f[2], &f[3]);
            if (got < 4) continue;
            int now = !strcmp(state, "secure") ? HV_FOCUS_SECURE : !strcmp(state, "text") ? HV_FOCUS_TEXT
                    : !strcmp(state, "none") ? HV_FOCUS_NONE : HV_FOCUS_UNKNOWN;
            if (got != 8 || !(f[2] > 0) || !(f[3] > 0) || f[0] != f[0] || f[1] != f[1]) f[0] = f[1] = f[2] = f[3] = 0;
            pthread_mutex_lock(&link_mu);
            if (link_fd == c->fd) {
                if (fid != link_field_id) { /* another field: what was around the last one's caret is no longer */
                    link_context_known = 0, link_selected = 0, link_before[0] = link_after[0] = 0, link_context_seq++;
                }
                link_field_serial = serial;
                link_field_id = fid;
                link_field_state = now;
                link_field_kind_said = field_kind(fkind);
                memcpy(link_field_frame, f, sizeof f);
                if (now != HV_FOCUS_UNKNOWN) { /* for those that ask the old way */
                    link_focus = now;
                    memcpy(link_focus_frame, f, sizeof f);
                    if (link_field_kind != link_field_kind_said) link_field_kind = link_field_kind_said, link_context_seq++;
                }
            }
            pthread_mutex_unlock(&link_mu);
            link_told();
        } else if (!strncmp(line, "ctx ", 4)) {
            unsigned fid = 0;
            char before[1024] = "", after[1024] = "";
            int sel = 0;
            int got = sscanf(line + 4, "%u %d %1023s %1023s", &fid, &sel, before, after), known = got == 4;
            if (got < 1) continue;
            pthread_mutex_lock(&link_mu);
            if (link_fd == c->fd && fid == link_field_id) {
                link_context_known = known;
                link_field_kind = link_field_kind_said;
                link_selected = known ? sel : 0;
                snprintf(link_before, sizeof link_before, "%s", known && strcmp(before, "-") ? before : "");
                snprintf(link_after, sizeof link_after, "%s", known && strcmp(after, "-") ? after : "");
                link_context_seq++;
            }
            pthread_mutex_unlock(&link_mu);
        } else if (sscanf(line, "tap %u %15s", &id, kind) == 2) {
            char role[48] = "";
            float f[4] = {0, 0, 0, 0};
            int got = sscanf(line, "tap %*u %*15s %47s %f %f %f %f", role, &f[0], &f[1], &f[2], &f[3]);
            if (got != 5 || !(f[2] > 0) || !(f[3] > 0) || f[0] != f[0] || f[1] != f[1]) f[0] = f[1] = f[2] = f[3] = 0;
            int answer = !strcmp(kind, "editable") ? HV_TAP_EDITABLE : !strcmp(kind, "secure") ? HV_TAP_SECURE
                       : !strcmp(kind, "no") ? HV_TAP_NO : HV_TAP_UNKNOWN;
            pthread_mutex_lock(&link_mu);
            if (link_taps[id % 16].id == id) {
                memcpy(link_taps[id % 16].frame, f, sizeof f);
                for (size_t i = 0; role[i]; i++) /* the guest's text: printable only */
                    if ((unsigned char)role[i] < 0x21 || (unsigned char)role[i] > 0x7e) role[i] = '?';
                snprintf(link_taps[id % 16].role, sizeof link_taps[0].role, "%s", strcmp(role, "-") ? role : "");
                link_taps[id % 16].answer = answer; /* last: it says the rest is in */
            }
            pthread_mutex_unlock(&link_mu);
            link_told();
        } else if (sscanf(line, "looked %u %15s", &id, kind) == 2) {
            /* "secure" sure to be a password field, "held" one only from its app holding secure input */
            int answer = !strcmp(kind, "secure") || !strcmp(kind, "held") ? HV_FOCUS_SECURE : !strcmp(kind, "text") ? HV_FOCUS_TEXT
                       : !strcmp(kind, "none") ? HV_FOCUS_NONE : HV_FOCUS_UNKNOWN;
            pthread_mutex_lock(&link_mu);
            if (link_fd == c->fd && link_looks[id % 8].id == id && link_looks[id % 8].answer == HV_TAP_WAITING) {
                link_looks[id % 8].sure = !strcmp(kind, "secure");
                link_looks[id % 8].answer = answer;
            }
            pthread_mutex_unlock(&link_mu);
            link_told();
        } else if (!strncmp(line, "focus ", 6)) {
            char kind[16] = "";
            float f[4] = {0, 0, 0, 0};
            int got = sscanf(line + 6, "%15s %f %f %f %f", kind, &f[0], &f[1], &f[2], &f[3]);
            int now = !strcmp(kind, "secure") ? HV_FOCUS_SECURE : !strcmp(kind, "text") ? HV_FOCUS_TEXT : HV_FOCUS_NONE;
            if (got != 5 || !(f[2] > 0) || !(f[3] > 0)) f[0] = f[1] = f[2] = f[3] = 0;
            pthread_mutex_lock(&link_mu);
            if (link_fd == c->fd) {
                link_focus = now;
                memcpy(link_focus_frame, f, sizeof f);
            }
            pthread_mutex_unlock(&link_mu);
        } else if (!strncmp(line, "context ", 8)) {
            char before[1024] = "", after[1024] = "", kind[16] = "";
            int sel = 0;
            int got = sscanf(line + 8, "%d %1023s %1023s %15s", &sel, before, after, kind), known = got >= 3;
            if (!known) sscanf(line + 8, "none %15s", kind);
            int field = field_kind(kind);
            pthread_mutex_lock(&link_mu);
            if (link_fd == c->fd) {
                link_context_known = known;
                link_field_kind = field;
                link_selected = known ? sel : 0;
                snprintf(link_before, sizeof link_before, "%s", known && strcmp(before, "-") ? before : "");
                snprintf(link_after, sizeof link_after, "%s", known && strcmp(after, "-") ? after : "");
                link_context_seq++;
            }
            pthread_mutex_unlock(&link_mu);
        } else if (!strncmp(line, "bench ", 6)) {
            void (*listener)(unsigned, const char *) = atomic_load(&bench_listener);
            if (listener) {
                for (char *p = line + 6; *p; p++) /* the guest's text: printable only */
                    if ((unsigned char)*p < 0x20 || (unsigned char)*p >= 0x7f) *p = '?';
                listener(mine, line + 6); /* this link's: a run asked on a newer one doesn't take it */
            }
        } else if (!strncmp(line, "note ", 5)) {
            char note[160];
            size_t n = 0;
            for (const char *p = line + 5; *p && n < sizeof note - 1; p++) /* the guest's text: printable only */
                note[n++] = (unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f ? *p : '?';
            note[n] = 0;
            fprintf(stderr, "share: the helper: %s\n", note);
        }
    }
    pthread_mutex_lock(&link_mu);
    int closed = link_fd == c->fd;
    if (closed) link_fd = -1, link_trusted = 0, link_version = 0, link_focus = HV_FOCUS_UNKNOWN, link_gliding = 0, link_touchscreen = -1, link_screen_away = -1;
    if (closed) link_scroll_unfenced = 0, link_stop_late = 0;
    if (closed) link_field_state = HV_FOCUS_UNKNOWN, link_generation++, looks_forget(), link_id = 0;
    pthread_cond_broadcast(&sync_cv); /* a sync waiting on this link has no answer coming */
    gone = link_stop_last; /* every stop sent on it */
    pthread_mutex_unlock(&link_mu);
    if (closed) link_went(gone, mine);
    fprintf(stderr, "share: the helper's link closed\n");
    return -1; /* this connection is done */
}

/* ---- the system link ----

   The helper's system part (henge-agent --system: a LaunchDaemon, root, no window server) runs from the Mac's start
   until it shuts down, before and across logins, and keeps its own connection, upgraded from
   GET /.henge/system?v=VERSION. It is apart from the touch link above, so neither displaces the other. It sends:
     session starting|login|user   what the screen is at: macOS still starting, the login window, or someone's
                                   desktop; at once on linking, and as it changes
     p / note TEXT                 as on the touch link
     touchscreen ready|no          macOS 27 or later: WindowServer's external-touchscreen support is on, and (helper
                                   23 and later) the screen's ID is known to tag a touchscreen with, so a USB
                                   touchscreen (xhci.c's port 6) is taken as one; or it isn't (older macOS)
     touchscreen replug            the touchscreen was tagged with the screen's ID after WindowServer may have looked
                                   at it: to be plugged in again
     ports [tcp N ...] [udp N ...] what the Mac listens on, from the kernel's tables (version 37): the device's same
                                   ports go to it (net_forward.c), in place of the standard sharing set
   Its request carries Henge-Key: 64 hex digits, a key only root in the Mac can read (version 37). The app keeps the
   key's SHA-256 for this Mac (hv_share_system_pin) and takes "ports" only from a link with that key: anyone in the
   Mac can open this link, and a list from just anyone could close the Mac's Remote Login and File Sharing. A new key
   is pinned when this Mac has none, or on the first system link after the Mac started afresh before anyone's helper
   has linked: with Henge Tools' system part in, that is its link (a LaunchDaemon starts before the login window, so
   before any user's process), and so root's key (macOS reinstalled, or the key file lost). That is trust on first
   use: on a Mac with no pin yet, or one started afresh without the system part in, a user's process may be first and
   be pinned. The system part, linking first on the next fresh start, takes the pin back. A pin file there that can't
   be read is a pin to a key no link has: only that window takes a new one, and so is a pin that couldn't be saved
   (an empty file is left in its place). A link without the pinned key is taken while no proven one is linked, but
   none of what it says is (its session, touchscreen, notes, ports): only while this Mac has no pin at all (a helper
   from before keys) are its lines other than ports taken. It never takes the place of a proven one.
   The app sends:
     update                        as on the touch link (the system part's copy of the helper is its own)
     trace                         record what WindowServer and AppKit say about touches into "Henge touch trace.txt"
                                   in the shared folder, and note what each kind of HID client sees of the touchscreen's
                                   tag: a test, sent only with HENGE_TOUCH_TRACE=1 (henge-env.txt) (version 29) */

static int system_fd = -1;
static int system_trusted; /* the linked system part has this Mac's key */
static int system_session = HV_SESSION_UNKNOWN;
static int system_touchscreen = -1;
static int system_replug; /* "touchscreen replug", until hv_share_touchscreen_take_replug */
static struct timespec signed_in_at; /* when the system part last said someone logged in, for how long the helper took */

int hv_share_touchscreen_ready(void) {
    pthread_mutex_lock(&link_mu);
    int r = system_fd >= 0 ? system_touchscreen : -1;
    pthread_mutex_unlock(&link_mu);
    return r;
}

int hv_share_touchscreen(void) {
    pthread_mutex_lock(&link_mu);
    int r = link_can(HV_AGENT_SINCE_TOUCH) ? link_touchscreen : -1;
    pthread_mutex_unlock(&link_mu);
    return r;
}

int hv_share_screen_away(void) {
    pthread_mutex_lock(&link_mu);
    int r = link_can(HV_AGENT_SINCE_SCREEN_AWAY) ? link_screen_away : -1;
    pthread_mutex_unlock(&link_mu);
    return r;
}

void hv_share_touchscreen_watch(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&link_mu);
    /* One question at a time; one unanswered for 3 s (an older helper, a dropped line) never counts. */
    int ask = link_can(HV_AGENT_SINCE_TOUCH) && link_touchscreen == 1 && ts_verdict != 0 &&
              (ts_watch_at.tv_sec == 0 || now.tv_sec - ts_watch_at.tv_sec >= 3);
    if (ask) ts_watch_id = ++ts_watch_next, ts_watch_at = now, ts_live = 1, ts_cancelled = 0, ts_held = -2;
    unsigned id = ts_watch_id;
    pthread_mutex_unlock(&link_mu);
    if (ask) link_send("touchwatch %u\n", id);
}

void hv_share_touchscreen_gesture(int over, int cancelled) {
    pthread_mutex_lock(&link_mu);
    const char *say = 0;
    if (ts_live) {
        if (cancelled) ts_cancelled = 1;
        if (over) {
            ts_live = 0;
            if (ts_cancelled) ts_watch_at.tv_sec = 0; /* its answer, still to come, counts for nothing */
            else if (ts_held != -2) say = ts_answer(ts_held);
            ts_held = -2;
        }
    }
    pthread_mutex_unlock(&link_mu);
    if (say) fprintf(stderr, "share: %s\n", say);
}

void hv_share_touchscreen_retry(void) {
    pthread_mutex_lock(&link_mu);
    int was = ts_verdict;
    ts_verdict = -1, ts_misses = 0, ts_unsure = 0, ts_live = 0, ts_held = -2, ts_watch_at.tv_sec = 0;
    pthread_mutex_unlock(&link_mu);
    if (was == 0) fprintf(stderr, "share: the touchscreen was turned on again: it is tried again\n");
}

int hv_share_touchscreen_take_replug(void) {
    pthread_mutex_lock(&link_mu);
    int r = system_replug;
    system_replug = 0;
    pthread_mutex_unlock(&link_mu);
    return r;
}

int hv_share_touchscreen_verdict(void) {
    pthread_mutex_lock(&link_mu);
    int r = ts_verdict;
    pthread_mutex_unlock(&link_mu);
    return r;
}

int hv_share_session(void) {
    pthread_mutex_lock(&link_mu);
    int s = system_fd >= 0 ? system_session : HV_SESSION_UNKNOWN;
    pthread_mutex_unlock(&link_mu);
    return s;
}

/* The touch link is up: if someone logged in a moment ago, how long the helper took, for Henge's log. */
static void signed_in_linked(void) {
    pthread_mutex_lock(&link_mu);
    struct timespec at = signed_in_at;
    signed_in_at.tv_sec = 0;
    pthread_mutex_unlock(&link_mu);
    if (!at.tv_sec) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double s = (double)(now.tv_sec - at.tv_sec) + (now.tv_nsec - at.tv_nsec) / 1e9;
    if (s < 600) fprintf(stderr, "share: the helper linked %.1f s after someone logged in\n", s);
}

void (*_Atomic hv_share_ports)(const uint16_t *port, const uint8_t *udp, int n);

/* The Mac's key, as the app trusts it (all under link_mu): its SHA-256, kept in pin_path (written under pin_mu). */
static char pin_path[PATH_MAX];
static uint8_t pin[CC_SHA256_DIGEST_LENGTH];
static int have_pin;
static int cold_boot;          /* Henge started this Mac afresh (hv_share_system_cold_boot), not from a Suspend */
static int system_links_seen;  /* system links asked for since */
static uint8_t refused_said[4]; /* the last refused key's first bytes of hash, said once */
static int refused_said_any, door_said;
static pthread_mutex_t pin_mu = PTHREAD_MUTEX_INITIALIZER;
/* Taking ports and putting the standard set back are one step each, in order (ports_mu, then link_mu): a link
   replaced or closed never has its list applied after it. ports_taken: the list in force is a system part's. */
static pthread_mutex_t ports_mu = PTHREAD_MUTEX_INITIALIZER;
static int ports_taken;

static void hex8(const uint8_t *h, char out[9]) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) out[2 * i] = d[h[i] >> 4], out[2 * i + 1] = d[h[i] & 15];
    out[8] = 0;
}

/* Every byte looked at whatever they hold: how long it takes says nothing of where they differ. */
static int same_hash(const uint8_t *a, const uint8_t *b) {
    uint8_t d = 0;
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) d |= a[i] ^ b[i];
    return d == 0;
}

void hv_share_system_pin(const char *path) {
    uint8_t h[CC_SHA256_DIGEST_LENGTH];
    int got = 0, unknown = 0;
    if (path && path[0]) {
        int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st;
        if (fd >= 0) {
            got = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == (off_t)sizeof h && read(fd, h, sizeof h) == (ssize_t)sizeof h;
            close(fd);
        }
        unknown = !got && (fd >= 0 || errno != ENOENT);
        if (unknown)
            fprintf(stderr, "share: this Mac's pinned key (%s) can't be read: no key is taken until the system part's first "
                            "link after the Mac starts afresh\n", path);
    }
    pthread_mutex_lock(&link_mu);
    snprintf(pin_path, sizeof pin_path, "%s", path ? path : "");
    have_pin = got || unknown;
    if (got) memcpy(pin, h, sizeof pin);
    else memset(pin, 0, sizeof pin); /* unknown: pinned to all zeros, which no key's SHA-256 is */
    pthread_mutex_unlock(&link_mu);
}

void hv_share_system_cold_boot(void) {
    pthread_mutex_lock(&link_mu);
    cold_boot = 1;
    pthread_mutex_unlock(&link_mu);
}

/* The pin, written whole: next to it, then in its place. Only its owner (the app) may read it. */
static void pin_save(const uint8_t *h) {
    pthread_mutex_lock(&link_mu);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", pin_path);
    pthread_mutex_unlock(&link_mu);
    if (!path[0]) return; /* kept for this run only */
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    pthread_mutex_lock(&pin_mu);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    int ok = fd >= 0, err = 0;
    ssize_t wrote = ok ? write(fd, h, CC_SHA256_DIGEST_LENGTH) : 0;
    if (ok && wrote != CC_SHA256_DIGEST_LENGTH) ok = 0, err = wrote < 0 ? errno : EIO;
    if (ok && fsync(fd) != 0) ok = 0, err = errno;
    if (fd < 0) err = errno;
    if (fd >= 0) close(fd);
    if (ok && rename(tmp, path) != 0) ok = 0, err = errno;
    if (ok) { /* the rename itself, kept too */
        char folder[PATH_MAX];
        snprintf(folder, sizeof folder, "%s", path);
        char *slash = strrchr(folder, '/');
        if (slash) *slash = 0;
        int dfd = slash ? open(folder[0] ? folder : "/", O_RDONLY | O_CLOEXEC) : -1;
        if (dfd >= 0) fsync(dfd), close(dfd);
    }
    if (!ok) {
        /* An empty file is left in the pin's place (gate r3 S1), an older pin there emptied too: the next launch can't
           read it, so it takes no key until the Mac starts afresh (hv_share_system_pin), where with nothing there it
           would take the first key it is sent, even on a resume, and with the old pin the key that was just replaced.
           Done after the attempt is taken away, so a full disk has room for it; emptying frees the old pin's. Never
           through a link (O_NOFOLLOW) or into a FIFO (O_NONBLOCK); what is in the pin's place is then looked at. */
        unlink(tmp);
        int m = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        if (m >= 0) close(m);
        struct stat st;
        int marked = lstat(path, &st) == 0 && !(S_ISREG(st.st_mode) && st.st_size == CC_SHA256_DIGEST_LENGTH);
        fprintf(stderr, "share: this Mac's key couldn't be kept (%s): it is trusted until Henge stops; %s\n", strerror(err),
                marked ? "after that, no key is taken until the Mac starts afresh"
                       : "nothing could be left in its place either, so after that the first key sent is taken");
    }
    pthread_mutex_unlock(&pin_mu);
}

/* "ports tcp 22 445 3000 udp 88": what the Mac listens on, as the system part read it from the kernel's tables. */
static void take_ports(const char *s, void (*hook)(const uint16_t *, const uint8_t *, int)) {
    uint16_t port[HV_SHARE_MAX_PORTS];
    uint8_t udp[HV_SHARE_MAX_PORTS];
    int n = 0, is_udp = 0;
    while (*s && n < HV_SHARE_MAX_PORTS) {
        while (*s == ' ') s++;
        if (!strncmp(s, "tcp", 3)) is_udp = 0, s += 3;
        else if (!strncmp(s, "udp", 3)) is_udp = 1, s += 3;
        else if (*s >= '0' && *s <= '9') {
            long v = 0;
            for (; *s >= '0' && *s <= '9'; s++) /* the whole number, however long: 655361 is no port, not 65536 and 1 */
                if (v <= 65535) v = v * 10 + (*s - '0');
            if (v >= 1 && v <= 65535) port[n] = (uint16_t)v, udp[n] = (uint8_t)is_udp, n++;
        } else if (*s) {
            s++; /* anything else is skipped */
        }
    }
    if (hook) hook(port, udp, n);
}

enum { KEY_NONE, KEY_KNOWN, KEY_PINNED, KEY_REPINNED, KEY_REFUSED };

static int system_link(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    int version = query_int(r->query, "v");
    /* The key: only its hash is kept, and only 8 hex digits of that are ever said. */
    uint8_t hash[CC_SHA256_DIGEST_LENGTH], old[CC_SHA256_DIGEST_LENGTH];
    int keyed = r->key[0] != 0;
    if (keyed) {
        uint8_t key[32];
        for (int i = 0; i < 32; i++) {
            unsigned v;
            sscanf(r->key + 2 * i, "%2x", &v);
            key[i] = (uint8_t)v;
        }
        CC_SHA256(key, sizeof key, hash);
        memset_s(key, sizeof key, 0, sizeof key);
    }
    memset_s(r->key, sizeof r->key, 0, sizeof r->key);
    char tag[9] = "", old_tag[16] = "";
    if (keyed) hex8(hash, tag);

    pthread_mutex_lock(&ports_mu);
    pthread_mutex_lock(&link_mu);
    /* The one moment a new key is taken: the first system link since Henge started this Mac afresh, before any
       user's helper. Not after a Suspend, nor on a link after the first. */
    int window = cold_boot && !system_links_seen && !touch_links_seen;
    system_links_seen++;
    int verdict = KEY_NONE, say_refused = 0;
    if (have_pin) {
        static const uint8_t none[sizeof pin];
        memcpy(old, pin, sizeof old);
        if (memcmp(old, none, sizeof old)) hex8(old, old_tag), strcat(old_tag, "...");
        else snprintf(old_tag, sizeof old_tag, "unreadable"); /* its pin file (hv_share_system_pin) */
    }
    if (keyed) {
        if (!have_pin) verdict = KEY_PINNED;
        else if (same_hash(hash, pin)) verdict = KEY_KNOWN;
        else if (window) verdict = KEY_REPINNED;
        else {
            verdict = KEY_REFUSED;
            say_refused = !refused_said_any || memcmp(refused_said, hash, sizeof refused_said);
            memcpy(refused_said, hash, sizeof refused_said), refused_said_any = 1;
        }
        if (verdict == KEY_PINNED || verdict == KEY_REPINNED) memcpy(pin, hash, sizeof pin), have_pin = 1;
    }
    int trusted = verdict == KEY_KNOWN || verdict == KEY_PINNED || verdict == KEY_REPINNED;
    int door = !trusted && system_fd >= 0 && system_trusted; /* never in place of a proven link */
    int say_door = door && verdict != KEY_REFUSED && !door_said;
    if (say_door) door_said = 1;
    int reset = 0;
    if (!door) {
        if (system_fd >= 0) shutdown(system_fd, SHUT_RDWR); /* the newest is the one, as for the touch link */
        system_fd = c->fd;
        system_trusted = trusted;
        system_session = HV_SESSION_UNKNOWN; /* until it says */
        system_touchscreen = -1;
        /* A list taken from the link before stays only for a link that will say its own (r1 S3). */
        reset = ports_taken && !(trusted && version >= HV_AGENT_SINCE_PORTS);
        if (reset) ports_taken = 0;
    }
    pthread_mutex_unlock(&link_mu);
    void (*hook)(const uint16_t *, const uint8_t *, int) = atomic_load(&hv_share_ports);
    if (reset && hook) hook(NULL, NULL, -1); /* the standard set again */
    pthread_mutex_unlock(&ports_mu);

    if (verdict == KEY_PINNED || verdict == KEY_REPINNED) pin_save(hash);
    if (verdict == KEY_PINNED) fprintf(stderr, "share: the helper's system part proved it is root: this Mac's key (%s...) is kept\n", tag);
    if (verdict == KEY_REPINNED)
        fprintf(stderr, "share: the helper's system part has a new key (%s..., was %s) on the first link since the Mac started "
                        "afresh: macOS was reinstalled or its key made again; kept in place of the old one\n", tag, old_tag);
    if (say_refused)
        fprintf(stderr, "share: a system link with a key this Mac's isn't (%s..., this Mac's is %s): nothing it says is taken (its "
                        "ports, session, touchscreen). A new key is taken only on the first link after the Mac starts "
                        "afresh (shut it down and start it again; a restart inside macOS isn't one)\n", tag, old_tag);
    if (say_door) fprintf(stderr, "share: a system link without this Mac's key, while the one with it is linked: refused\n");
    if (door) {
        static const char no[] = "HTTP/1.1 403 Forbidden\r\nServer: Henge\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(c->fd, no, sizeof no - 1);
        return -1;
    }

    static const char head[] = "HTTP/1.1 101 Switching Protocols\r\nServer: Henge\r\nUpgrade: henge-link\r\nConnection: Upgrade\r\n\r\n";
    int ok = send_all(c->fd, head, sizeof head - 1) == 0 && link_nonblocking(c->fd) == 0;
    int current = version == HV_AGENT_VERSION;
    if (ok)
        fprintf(stderr, "share: the helper's system part linked (version %d%s%s)\n", version, current ? "" : ", out of date: to be updated",
                trusted ? ", with this Mac's key" : version < HV_AGENT_SINCE_PORTS ? "" : r->key_bad ? ", a key that isn't one" : !keyed ? ", no key" : "");
    /* Apple's own word on touches, recorded into the shared folder: only when asked for (a test, heavy on the Mac). */
    const char *trace = getenv("HENGE_TOUCH_TRACE");
    if (ok && trace && atoi(trace) && version >= HV_AGENT_SINCE_TRACE) {
        static const char ask[] = "trace\n";
        send(c->fd, ask, sizeof ask - 1, 0); /* non-blocking: a link just opened has room */
        fprintf(stderr, "share: asked the helper's system part for the touch trace (HENGE_TOUCH_TRACE)\n");
    }
    if (ok && !current && agent_path[0]) {
        static const char update[] = "update\n";
        send(c->fd, update, sizeof update - 1, 0);
    }
    static const char *const names[] = {"macOS is starting", "the login window", "someone's desktop"};
    char line[4096]; /* a "ports" line: up to HV_SHARE_MAX_PORTS numbers */
    int not_taken_said = 0;
    while (ok && link_line(c, line, sizeof line) == 0) {
        /* What a link says is taken only while it is the one in place, and, once this Mac has a key, only with it
           (gate r3 S2): its session (the Log In button), touchscreen and notes as well as its ports, which also need the
           key before the first. A line that isn't taken is said once a link, whatever it is. */
        int is_ports = !strncmp(line, "ports", 5) && (line[5] == ' ' || !line[5]) && version >= HV_AGENT_SINCE_PORTS;
        pthread_mutex_lock(&link_mu);
        int here = system_fd == c->fd, keyed_ok = system_trusted || (!have_pin && !is_ports);
        pthread_mutex_unlock(&link_mu);
        if (!here || !keyed_ok) {
            if (!not_taken_said++)
                fprintf(stderr, "share: the helper's system part's %s aren't taken: %s\n", is_ports ? "ports" : "lines",
                        here ? "its link doesn't have this Mac's key" : "its link was replaced");
            continue;
        }
        if (!strncmp(line, "session ", 8)) {
            const char *s = line + 8;
            int now = !strcmp(s, "login") ? HV_SESSION_LOGIN : !strcmp(s, "user") ? HV_SESSION_USER
                    : !strcmp(s, "starting") ? HV_SESSION_STARTING : HV_SESSION_UNKNOWN;
            pthread_mutex_lock(&link_mu);
            int was = system_fd == c->fd ? system_session : now;
            if (system_fd == c->fd) system_session = now;
            if (now == HV_SESSION_USER && was == HV_SESSION_LOGIN) {
                clock_gettime(CLOCK_MONOTONIC, &signed_in_at);
                if (link_fd >= 0) signed_in_at.tv_sec = 0; /* a helper already linked: not the one just starting */
            }
            pthread_mutex_unlock(&link_mu);
            if (now != was || was == HV_SESSION_UNKNOWN)
                fprintf(stderr, "share: the Mac is at %s\n", now >= 0 ? names[now] : "something unknown");
        } else if (is_ports) {
            /* Looked at again in the step that applies it: not a stale link's list after another took its place
               (r1 S3), for a line read whole just before. */
            pthread_mutex_lock(&ports_mu);
            pthread_mutex_lock(&link_mu);
            int take = system_fd == c->fd && system_trusted;
            if (take) ports_taken = 1;
            pthread_mutex_unlock(&link_mu);
            if (take) take_ports(line + 5, atomic_load(&hv_share_ports));
            pthread_mutex_unlock(&ports_mu);
            if (!take && !not_taken_said++)
                fprintf(stderr, "share: the helper's system part's ports aren't taken: its link was replaced\n");
        } else if (!strcmp(line, "touchscreen replug")) {
            pthread_mutex_lock(&link_mu);
            if (system_fd == c->fd) system_replug = 1;
            pthread_mutex_unlock(&link_mu);
            fprintf(stderr, "share: the helper's system part tagged the touchscreen late: it is plugged in again\n");
        } else if (!strncmp(line, "touchscreen ", 12)) {
            int ready = !strcmp(line + 12, "ready");
            if (ready && version < HV_AGENT_SINCE_TOUCHSCREEN) continue; /* untagged: macOS wouldn't take it; updated first */
            pthread_mutex_lock(&link_mu);
            if (system_fd == c->fd) system_touchscreen = ready;
            pthread_mutex_unlock(&link_mu);
            fprintf(stderr, "share: %s\n", ready ? "macOS takes a touchscreen: the helper's system part turned its support on"
                                                 : "macOS here takes no touchscreen (older than macOS 27)");
        } else if (!strncmp(line, "note ", 5)) {
            char note[160];
            size_t n = 0;
            for (const char *p = line + 5; *p && n < sizeof note - 1; p++) /* the guest's text: printable only */
                note[n++] = (unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f ? *p : '?';
            note[n] = 0;
            fprintf(stderr, "share: the helper's system part: %s\n", note);
        }
    }
    pthread_mutex_lock(&ports_mu);
    pthread_mutex_lock(&link_mu);
    int last = system_fd == c->fd;
    if (last) system_fd = -1, system_trusted = 0, system_session = HV_SESSION_UNKNOWN, system_touchscreen = -1;
    reset = last && ports_taken; /* the link the list came from, or one that took its place, has gone */
    if (reset) ports_taken = 0;
    pthread_mutex_unlock(&link_mu);
    hook = atomic_load(&hv_share_ports);
    if (reset && hook) hook(NULL, NULL, -1); /* the standard set again */
    pthread_mutex_unlock(&ports_mu);
    fprintf(stderr, "share: the helper's system part's link closed\n");
    return -1;
}

static int serve_henge(Conn *c, Request *r) {
    const char *what = r->path + strlen("/.henge/");
    if (!strcmp(what, "install") && !strcmp(r->method, "GET")) {
        if (body_discard(c, r) != 0) return -1;
        return respond(c, r, 200, "OK", "Content-Type: text/plain\r\n", install_script, sizeof install_script - 1);
    }
    if (!strcmp(what, "agent") && !strcmp(r->method, "GET")) return serve_file(c, r, agent_path);
    if (!strcmp(what, "bench-video") && !strcmp(r->method, "GET")) return serve_file(c, r, bench_video);
    if (!strcmp(what, "link") && !strcmp(r->method, "GET")) return touch_link(c, r);
    if (!strcmp(what, "system") && !strcmp(r->method, "GET")) return system_link(c, r);
    if (!strcmp(what, "clipboard") && !strcmp(r->method, "GET")) return clipboard_get(c, r);
    if (!strcmp(what, "clipboard") && !strcmp(r->method, "PUT")) return clipboard_put(c, r);
    if (body_discard(c, r) != 0) return -1;
    return respond_empty(c, r, 404, "Not Found");
}

/* ---- requests ---- */

static int header_is(const char *line, const char *name, const char **value) {
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0 || line[n] != ':') return 0;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    *value = v;
    return 1;
}

/* 1 with a request, 0 when the client is done, -1 on a malformed one. */
static int read_request(Conn *c, Request *r) {
    static __thread char head[HEAD_MAX];
    size_t n = 0;
    for (;;) {
        char ch;
        ssize_t got = conn_read(c, &ch, 1);
        if (got <= 0) return n ? -1 : 0;
        if (n + 1 >= sizeof head) return -1;
        head[n++] = ch;
        if (n >= 4 && !memcmp(head + n - 4, "\r\n\r\n", 4)) break;
        if (n == 2 && !memcmp(head, "\r\n", 2)) n = 0; /* blank lines between requests */
    }
    head[n] = 0;
    memset(r, 0, sizeof *r);
    r->range_from = r->range_to = -1;
    r->overwrite = 1;
    r->depth = 1;
    char *line = head, *eol = strstr(line, "\r\n");
    *eol = 0;
    char version[16];
    if (sscanf(line, "%15s %32767s %15s", r->method, r->target, version) != 3) return -1;
    char *query = strchr(r->target, '?');
    if (query) {
        *query++ = 0;
        snprintf(r->query, sizeof r->query, "%s", query);
    }
    /* A client may send the whole URL. */
    if (!strncasecmp(r->target, "http://", 7)) {
        char *p = strchr(r->target + 7, '/');
        memmove(r->target, p ? p : "/", strlen(p ? p : "/") + 1);
    }
    if (decode_path(r->target, r->path, sizeof r->path) != 0) return -1;
    if (!strcmp(version, "HTTP/1.0")) r->close = 1;
    for (line = eol + 2; *line; line = eol + 2) {
        eol = strstr(line, "\r\n");
        if (!eol) break;
        *eol = 0;
        const char *v;
        if (header_is(line, "Content-Length", &v)) r->length = strtoll(v, NULL, 10);
        else if (header_is(line, "Transfer-Encoding", &v)) r->chunked = strcasestr(v, "chunked") != NULL;
        else if (header_is(line, "Expect", &v)) r->expect_continue = strcasestr(v, "100-continue") != NULL;
        else if (header_is(line, "Connection", &v)) r->close = strcasestr(v, "close") != NULL || r->close;
        else if (header_is(line, "Depth", &v)) r->depth = v[0] == '0' ? 0 : 1;
        else if (header_is(line, "Overwrite", &v)) r->overwrite = v[0] != 'F' && v[0] != 'f';
        else if (header_is(line, "If", &v)) snprintf(r->if_header, sizeof r->if_header, "%s", v);
        else if (header_is(line, "Henge-Key", &v)) {
            size_t k = strspn(v, "0123456789abcdefABCDEF");
            if (k == 64 && !v[k]) memcpy(r->key, v, 65);
            else r->key_bad = 1;
        }
        else if (header_is(line, "Destination", &v)) {
            const char *p = v;
            if (!strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8)) {
                p = strchr(strstr(p, "//") + 2, '/');
                if (!p) p = "/";
            }
            if (decode_path(p, r->destination, sizeof r->destination) != 0) r->destination[0] = 0;
        } else if (header_is(line, "Range", &v) && !strncasecmp(v, "bytes=", 6)) {
            long long a = -1, b = -1;
            const char *s = v + 6;
            if (*s == '-') b = strtoll(s + 1, NULL, 10);
            else {
                char *end;
                a = strtoll(s, &end, 10);
                if (*end == '-' && end[1]) b = strtoll(end + 1, NULL, 10);
            }
            r->range_from = a, r->range_to = b;
        }
    }
    return 1;
}

static int handle(Conn *c, Request *r) {
    if (!strncmp(r->path, "/.henge/", 8)) return serve_henge(c, r);
    const char *m = r->method;
    if (!strcmp(m, "OPTIONS")) {
        if (body_discard(c, r) != 0) return -1;
        return respond(c, r, 200, "OK",
                       "DAV: 1, 2\r\nMS-Author-Via: DAV\r\nAllow: OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, PROPPATCH, MOVE, COPY, LOCK, UNLOCK\r\n", NULL, 0);
    }
    if (!strcmp(m, "PROPFIND")) return do_propfind(c, r);
    if (!strcmp(m, "PROPPATCH")) return do_proppatch(c, r);
    if (!strcmp(m, "GET") || !strcmp(m, "HEAD")) return do_get(c, r);
    if (!strcmp(m, "PUT")) return do_put(c, r);
    if (!strcmp(m, "DELETE")) return do_delete(c, r);
    if (!strcmp(m, "MKCOL")) return do_mkcol(c, r);
    if (!strcmp(m, "MOVE")) return do_move_copy(c, r, 1);
    if (!strcmp(m, "COPY")) return do_move_copy(c, r, 0);
    if (!strcmp(m, "LOCK")) return do_lock(c, r);
    if (!strcmp(m, "UNLOCK")) {
        if (body_discard(c, r) != 0) return -1;
        return respond_empty(c, r, 204, "No Content");
    }
    if (body_discard(c, r) != 0) return -1;
    return respond_empty(c, r, 405, "Method Not Allowed");
}

static void *connection(void *arg) {
    Conn *c = arg;
    Request *r = malloc(sizeof *r);
    for (;;) {
        int got = read_request(c, r);
        if (got <= 0) {
            if (got < 0) {
                memset(r, 0, sizeof *r);
                strcpy(r->method, "GET");
                r->close = 1;
                respond_empty(c, r, 400, "Bad Request");
            }
            break;
        }
        if (getenv("HV_SHARE_LOG")) fprintf(stderr, "share: %s %s\n", r->method, r->path);
        if (handle(c, r) != 0 || r->close) break;
    }
    close(c->fd);
    free(r);
    free(c);
    return NULL;
}

static void *listener(void *arg) {
    int server = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            usleep(100000);
            continue;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        Conn *c = calloc(1, sizeof *c);
        c->fd = fd;
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, connection, c) != 0) {
            close(fd);
            free(c);
        }
        pthread_attr_destroy(&a);
    }
    return NULL;
}

int hv_share_start(const char *folder, const char *socket_path, const char *agent) {
    static int started;
    if (started) return 0;
    if (!realpath(folder, root)) return -1;
    if (agent) snprintf(agent_path, sizeof agent_path, "%s", agent);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(socket_path) >= sizeof addr.sun_path) {
        fprintf(stderr, "hengevm: share: socket path too long: %s\n", socket_path);
        return -1;
    }
    strcpy(addr.sun_path, socket_path);
    unlink(socket_path);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(server, 16) != 0) {
        fprintf(stderr, "hengevm: share: %s: %s\n", socket_path, strerror(errno));
        if (server >= 0) close(server);
        return -1;
    }
    chmod(socket_path, 0600);
    pthread_t t;
    pthread_create(&t, NULL, listener, (void *)(intptr_t)server);
    pthread_detach(t);
    started = 1;
    return 0;
}

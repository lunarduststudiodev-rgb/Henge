/* The shared-folder device's file side, driven the way macOS's AppleVirtIOFS
   drives it: Linux FUSE requests in, answers out, against real folders here. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "virtio_fs.h"

static int checks, failures;
static void check(const char *what, long long got, long long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %lld, want %lld\n", what, got, want);
    }
}

/* Linux numbers, as the Mac's driver expects them. */
enum { ENOENT_L = 2, EBADF_L = 9, EEXIST_L = 17, EISDIR_L = 21, EINVAL_L = 22, EROFS_L = 30, ENOSYS_L = 38,
       ENOTEMPTY_L = 39, ELOOP_L = 40, ENODATA_L = 61 };

static HvFuse F;
static uint8_t out[1 << 20];
static uint8_t req[1 << 18];
static uint32_t rlen;
static uint64_t unique;

static void begin(void) { rlen = 40; }
static void put(const void *p, uint32_t n) {
    memcpy(req + rlen, p, n);
    rlen += n;
}
static void put_str(const char *s) { put(s, (uint32_t)strlen(s) + 1); }
static void put32(uint32_t v) { put(&v, 4); }
static void put64(uint64_t v) { put(&v, 8); }

/* Sends what was put; returns the error (positive Linux number, 0 for OK) and sets *len to the answer's body length. */
static uint32_t body_len;
static int send(uint32_t op, uint64_t node) {
    uint32_t h[10] = {0};
    h[0] = rlen;
    h[1] = op;
    uint64_t u = ++unique;
    memcpy(&h[2], &u, 8);
    memcpy(&h[4], &node, 8);
    h[6] = 501, h[7] = 20, h[8] = 1234;
    memcpy(req, h, 40);
    memset(out, 0xa5, 64);
    uint32_t n = hv_fuse_handle(&F, req, rlen, out, sizeof out);
    if (n == 0) return -1; /* no answer */
    uint32_t len;
    int32_t err;
    uint64_t back;
    memcpy(&len, out, 4);
    memcpy(&err, out + 4, 4);
    memcpy(&back, out + 8, 8);
    check("answer length matches its header", len, n);
    check("answer carries the request's unique", (long long)back, (long long)u);
    body_len = n - 16;
    return -err;
}
static uint8_t *body(void) { return out + 16; }
static uint64_t u64at(uint32_t off) { uint64_t v; memcpy(&v, body() + off, 8); return v; }
static uint32_t u32at(uint32_t off) { uint32_t v; memcpy(&v, body() + off, 4); return v; }

/* entry_out: nodeid at 0, attr at 40 (ino 0, size 8, mode 40+60). */
static uint64_t lookup(uint64_t dir, const char *name, int *err) {
    begin();
    put_str(name);
    int e = send(1, dir);
    if (err) *err = e;
    return e ? 0 : u64at(0);
}
static uint32_t entry_mode(void) { return u32at(40 + 60); }

static uint64_t create(uint64_t dir, const char *name, uint32_t flags, uint32_t mode, uint64_t *fh, int *err) {
    begin();
    put32(flags), put32(mode), put32(022), put32(0);
    put_str(name);
    int e = send(35, dir);
    if (err) *err = e;
    if (e) return 0;
    check("create answers entry and open", body_len, 128 + 16);
    *fh = u64at(128);
    return u64at(0);
}
static int open_file(uint64_t node, uint32_t flags, uint64_t *fh) {
    begin();
    put32(flags), put32(0);
    int e = send(14, node);
    if (!e) *fh = u64at(0);
    return e;
}
static int release(uint64_t node, uint64_t fh, int dir) {
    begin();
    put64(fh), put32(0), put32(0), put64(0);
    return send(dir ? 29 : 18, node);
}
static int write_at(uint64_t node, uint64_t fh, uint64_t off, const char *data, uint32_t *written) {
    begin();
    uint32_t n = (uint32_t)strlen(data);
    put64(fh), put64(off), put32(n), put32(0), put64(0), put32(0), put32(0);
    put(data, n);
    int e = send(16, node);
    if (!e) *written = u32at(0);
    return e;
}
static int read_at(uint64_t node, uint64_t fh, uint64_t off, uint32_t size) {
    begin();
    put64(fh), put64(off), put32(size), put32(0), put64(0), put32(0), put32(0);
    return send(15, node);
}
static int getattr(uint64_t node) {
    begin();
    put32(0), put32(0), put64(0);
    return send(3, node);
}
static uint32_t attr_mode(void) { return u32at(16 + 60); }
static uint64_t attr_size(void) { return u64at(16 + 8); }
static void forget(uint64_t node, uint64_t n) {
    begin();
    put64(n);
    check("FORGET gets no answer", send(2, node), -1);
}

static int host_read(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t k = read(fd, buf, cap - 1);
    close(fd);
    if (k < 0) return -1;
    buf[k] = 0;
    return (int)k;
}

/* Every name READDIR gives for node, all pages of it read size bytes at a time. */
static int list(uint64_t node, uint32_t size, char names[][64], int max) {
    begin();
    put32(0), put32(0);
    if (send(27, node)) return -1;
    uint64_t fh = u64at(0);
    int count = 0;
    uint64_t off = 0;
    for (int pages = 0; pages < 1000; pages++) {
        begin();
        put64(fh), put64(off), put32(size), put32(0), put64(0), put32(0), put32(0);
        if (send(28, node)) return -1;
        if (body_len == 0) break;
        for (uint32_t at = 0; at < body_len;) {
            uint64_t ino, next;
            uint32_t namelen;
            memcpy(&ino, body() + at, 8);
            memcpy(&next, body() + at + 8, 8);
            memcpy(&namelen, body() + at + 16, 4);
            if (count < max) {
                memcpy(names[count], body() + at + 24, namelen < 63 ? namelen : 63);
                names[count][namelen < 63 ? namelen : 63] = 0;
            }
            count++;
            off = next;
            at += (24 + namelen + 7) & ~7u;
        }
    }
    release(node, fh, 1);
    return count;
}
static int has(char names[][64], int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], name)) return 1;
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Under /tmp on purpose: it is a link to /private/tmp, as /var is on the iPad. */
    char shared[] = "/tmp/henge-fs-shared.XXXXXX", tools[] = "/tmp/henge-fs-tools.XXXXXX", outside[] = "/tmp/henge-fs-out.XXXXXX";
    if (!mkdtemp(shared) || !mkdtemp(tools) || !mkdtemp(outside)) return 1;
    char path[1024], buf[256];
    snprintf(path, sizeof path, "%s/Install Henge Tools.txt", tools);
    FILE *fp = fopen(path, "w");
    fputs("installer", fp);
    fclose(fp);
    snprintf(path, sizeof path, "%s/secret", outside);
    fp = fopen(path, "w");
    fputs("not yours", fp);
    fclose(fp);

    hv_fuse_init(&F);
    check("export Shared", hv_fuse_export(&F, "Shared", shared, 0), 0);
    check("export Henge Tools", hv_fuse_export(&F, "Henge Tools", tools, 1), 0);
    check("a file is not a folder to export", hv_fuse_export(&F, "x", path, 0), -1);
    check("a bad export name", hv_fuse_export(&F, "a/b", shared, 0), -1);

    /* INIT, as the driver sends it (7.31 with its flags). */
    begin();
    put32(7), put32(31), put32(0x20000), put32((1u << 0) | (1u << 3) | (1u << 5) | (1u << 22) | (1u << 30));
    for (int i = 0; i < 12; i++) put32(0);
    check("INIT ok", send(26, 0), 0);
    check("INIT answer 0x40", body_len, 0x40);
    check("FUSE 7", u32at(0), 7);
    check("FUSE .31", u32at(4), 31);
    check("flags offered back, not INIT_EXT", u32at(12), (1u << 0) | (1u << 3) | (1u << 5) | (1u << 22));
    check("max_write 128 KB", u32at(20), 128 * 1024);

    /* The virtual top folder. */
    check("root GETATTR", getattr(1), 0);
    check("root is a read-only folder", attr_mode(), S_IFDIR | 0555);
    check("root owner is whoever asks", u32at(16 + 68), 501);
    char names[128][64];
    int n = list(1, 4096, names, 128);
    check("root lists . .. and two exports", n, 4);
    check("root lists Shared", has(names, n, "Shared"), 1);
    check("root lists Henge Tools", has(names, n, "Henge Tools"), 1);
    int err;
    uint64_t sh = lookup(1, "Shared", &err);
    check("LOOKUP Shared", err, 0);
    check("Shared is a writable folder", entry_mode() & (S_IFMT | 0200), S_IFDIR | 0200);
    uint64_t tl = lookup(1, "Henge Tools", NULL);
    check("Henge Tools found", tl != 0, 1);
    check("Henge Tools is read-only", entry_mode() & 0222, 0);
    lookup(1, "Nope", &err);
    check("unknown top name ENOENT", err, ENOENT_L);
    lookup(1, "..", &err);
    check(".. is no name", err, ENOENT_L);
    begin();
    put32(0100 | 2), put32(0644), put32(0), put32(0);
    put_str("x");
    check("nothing can be made at the top", send(35, 1), EROFS_L);

    /* Create, write, read back, through the Mac and on the host. */
    uint64_t fh, fh2;
    uint64_t hello = create(sh, "hello.txt", 0100 | 1 /* O_CREAT|O_WRONLY */, S_IFREG | 0644, &fh, &err);
    check("CREATE ok", err, 0);
    uint32_t written = 0;
    check("WRITE ok", write_at(hello, fh, 0, "hello, world", &written), 0);
    check("WRITE wrote all", written, 12);
    check("READ through a write-only open (page cache fills)", read_at(hello, fh, 7, 100), 0);
    check("READ length", body_len, 5);
    check("READ data", memcmp(body(), "world", 5), 0);
    check("RELEASE ok", release(hello, fh, 0), 0);
    check("RELEASE twice is harmless", release(hello, fh, 0), 0);
    check("READ on a closed handle", read_at(hello, fh, 0, 10), EBADF_L);
    snprintf(path, sizeof path, "%s/hello.txt", shared);
    check("file reached the host", host_read(path, buf, sizeof buf), 12);
    check("host content", strcmp(buf, "hello, world"), 0);
    struct stat st;
    stat(path, &st);
    check("mode with umask", st.st_mode & 0777, 0644);
    check("GETATTR file", getattr(hello), 0);
    check("GETATTR size", attr_size(), 12);
    check("LOOKUP again gives the same node", lookup(sh, "hello.txt", NULL), hello);
    check("CREATE with O_EXCL on an existing file", (create(sh, "hello.txt", 0100 | 0200 | 2, 0644, &fh2, &err), err), EEXIST_L);

    /* SETATTR: truncate and times. */
    begin();
    uint8_t sa[88] = {0};
    uint32_t valid = (1u << 3) | (1u << 5); /* SIZE, MTIME */
    uint64_t size = 5, mtime = 1000000000;
    memcpy(sa, &valid, 4), memcpy(sa + 16, &size, 8), memcpy(sa + 40, &mtime, 8);
    put(sa, 88);
    check("SETATTR size and mtime", send(4, hello), 0);
    check("SETATTR answers attributes", body_len, 0x68);
    check("SETATTR new size", attr_size(), 5);
    stat(path, &st);
    check("host truncated", st.st_size, 5);
    check("host mtime", st.st_mtimespec.tv_sec, 1000000000);
    /* Truncate through a read-only handle falls back to the name. */
    check("OPEN read-only", open_file(hello, 0, &fh2), 0);
    begin();
    memset(sa, 0, sizeof sa);
    valid = (1u << 3) | (1u << 6), size = 2;
    memcpy(sa, &valid, 4), memcpy(sa + 8, &fh2, 8), memcpy(sa + 16, &size, 8);
    put(sa, 88);
    check("SETATTR size through a read handle", send(4, hello), 0);
    stat(path, &st);
    check("host truncated again", st.st_size, 2);
    release(hello, fh2, 0);
    check("OPEN with O_TRUNC", open_file(hello, 01000 | 1, &fh2), 0);
    stat(path, &st);
    check("O_TRUNC emptied it", st.st_size, 0);
    release(hello, fh2, 0);

    /* Folders, rename, remove. */
    begin();
    put32(S_IFDIR | 0777), put32(022);
    put_str("Docs");
    check("MKDIR", send(9, sh), 0);
    uint64_t docs = u64at(0);
    snprintf(path, sizeof path, "%s/Docs", shared);
    stat(path, &st);
    check("folder made with umask", st.st_mode & 0777, 0755);
    begin();
    put64(docs);
    put_str("hello.txt"), put_str("moved.txt");
    check("RENAME into a folder", send(12, sh), 0);
    check("renamed node keeps its ID", lookup(docs, "moved.txt", NULL), hello);
    lookup(sh, "hello.txt", &err);
    check("old name gone", err, ENOENT_L);
    check("renamed file still readable by node", getattr(hello), 0);
    create(sh, "other.txt", 0100 | 2, 0644, &fh, NULL);
    release(0, fh, 0);
    begin();
    put64(docs), put32(1 /* NOREPLACE */), put32(0);
    put_str("other.txt"), put_str("moved.txt");
    check("RENAME2 NOREPLACE onto a file", send(45, sh), EEXIST_L);
    begin();
    put64(docs), put32(2 /* EXCHANGE */), put32(0);
    put_str("other.txt"), put_str("moved.txt");
    check("RENAME2 EXCHANGE", send(45, sh), 0);
    check("exchanged: hello's node is now Shared/other.txt", lookup(sh, "other.txt", NULL), hello);
    begin();
    put_str("Docs");
    check("RMDIR a full folder", send(11, sh), ENOTEMPTY_L);
    begin();
    put_str("moved.txt");
    check("UNLINK", send(10, docs), 0);
    begin();
    put_str("Docs");
    check("RMDIR the emptied folder", send(11, sh), 0);
    check("removed folder's node answers ENOENT by path", getattr(docs), ENOENT_L);

    /* Many files, read a page at a time. */
    for (int i = 0; i < 300; i++) {
        char name[64];
        snprintf(name, sizeof name, "file %03d with a longer name.txt", i);
        create(sh, name, 0100 | 1, 0644, &fh, &err);
        if (err) break;
        release(0, fh, 0);
    }
    n = list(sh, 512, names, 128);
    check("READDIR pages: 300 files, other.txt, . and ..", n, 303);

    /* Read-only Henge Tools. */
    uint64_t inst = lookup(tl, "Install Henge Tools.txt", &err);
    check("tools file found", err, 0);
    check("tools file has no write bits", entry_mode() & 0222, 0);
    check("OPEN tools read-only", open_file(inst, 0, &fh), 0);
    check("READ tools", read_at(inst, fh, 0, 100), 0);
    check("READ tools data", body_len == 9 && !memcmp(body(), "installer", 9), 1);
    uint32_t w;
    check("WRITE to read-only handle", write_at(inst, fh, 0, "x", &w), EROFS_L);
    release(inst, fh, 0);
    check("OPEN tools for writing", open_file(inst, 2, &fh), EROFS_L);
    check("CREATE in tools", (create(tl, "new", 0100 | 2, 0644, &fh, &err), err), EROFS_L);
    begin();
    put_str("Install Henge Tools.txt");
    check("UNLINK in tools", send(10, tl), EROFS_L);
    begin();
    put64(sh);
    put_str("Install Henge Tools.txt"), put_str("stolen");
    check("RENAME out of tools", send(12, tl), EROFS_L);

    /* Links may not lead out. */
    snprintf(path, sizeof path, "%s/escape", shared);
    symlink(outside, path);
    uint64_t esc = lookup(sh, "escape", &err);
    check("a link is found as a link", err == 0 && (entry_mode() & S_IFMT) == S_IFLNK, 1);
    lookup(esc, "secret", &err);
    check("but not walked through", err, ELOOP_L);
    check("READLINK", (begin(), send(5, esc)), 0);
    check("READLINK target", body_len == strlen(outside) && !memcmp(body(), outside, body_len), 1);
    check("OPEN a link refuses", open_file(esc, 0, &fh), ELOOP_L);
    begin();
    put_str("made-link"), put_str("/etc/passwd");
    check("SYMLINK", send(6, sh), 0);
    check("SYMLINK made a link", (entry_mode() & S_IFMT), S_IFLNK);

    /* Bad names. */
    begin();
    put32(0100 | 2), put32(0644), put32(0), put32(0);
    put_str("a/b");
    check("CREATE a/b", send(35, sh), EINVAL_L);
    lookup(sh, "a/b", &err);
    check("LOOKUP a/b", err, ENOENT_L);
    begin();
    put32(0100 | 2), put32(0644), put32(0), put32(0);
    put(".", 1); /* no terminator */
    check("CREATE with an unterminated name", send(35, sh), EINVAL_L);
    begin();
    put32(1);
    check("READ too short", send(15, hello), EINVAL_L);

    /* Extended attributes (Finder's tags and info live here). */
    uint64_t oth = lookup(sh, "other.txt", NULL);
    begin();
    put32(5), put32(0);
    put_str("com.example.tag"), put("green", 5);
    check("SETXATTR", send(21, oth), 0);
    begin();
    put32(5), put32(1 /* XATTR_CREATE, Linux */);
    put_str("com.example.tag"), put("again", 5);
    check("SETXATTR CREATE on an existing one", send(21, oth), EEXIST_L);
    begin();
    put32(0), put32(0);
    put_str("com.example.tag");
    check("GETXATTR size", send(22, oth), 0);
    check("GETXATTR size answer", u32at(0), 5);
    begin();
    put32(64), put32(0);
    put_str("com.example.tag");
    check("GETXATTR value", send(22, oth), 0);
    check("GETXATTR data", body_len == 5 && !memcmp(body(), "green", 5), 1);
    begin();
    put32(256), put32(0);
    check("LISTXATTR", send(23, oth), 0);
    check("LISTXATTR names it", memmem(body(), body_len, "com.example.tag", 16) != NULL, 1);
    begin();
    put_str("com.example.tag");
    check("REMOVEXATTR", send(24, oth), 0);
    begin();
    put32(64), put32(0);
    put_str("com.example.tag");
    check("GETXATTR after remove", send(22, oth), ENODATA_L);

    /* The rest of what the driver may send. */
    check("STATFS", (begin(), send(17, sh)), 0);
    check("STATFS block size", u32at(40) > 0 && u64at(0) > 0, 1);
    check("OPEN lseek target", open_file(oth, 2, &fh), 0);
    write_at(oth, fh, 0, "abc", &w);
    begin();
    put64(fh), put64(0), put32(3 /* SEEK_DATA, Linux */), put32(0);
    check("LSEEK SEEK_DATA", send(46, oth), 0);
    check("data starts at 0", u64at(0), 0);
    begin();
    put64(fh), put32(0), put32(0);
    check("FSYNC", send(20, oth), 0);
    begin();
    put64(fh), put32(0), put32(0), put64(0);
    check("FLUSH", send(25, oth), 0);
    begin();
    put32(1u << 0), put32(0), put64(fh);
    check("GETATTR by handle", send(3, oth), 0);
    check("GETATTR by handle size", attr_size(), 3);
    release(oth, fh, 0);
    check("ACCESS W_OK on tools", (begin(), put32(2), put32(0), send(34, inst)), EROFS_L);
    check("ACCESS R_OK on tools", (begin(), put32(4), put32(0), send(34, inst)), 0);
    check("OPENDIR a file", (begin(), put32(0), put32(0), send(27, oth)), 20);
    check("OPEN the top folder", open_file(1, 0, &fh), EISDIR_L);
    check("unknown opcode", (begin(), send(99, sh)), ENOSYS_L);
    check("unknown node", getattr(987654), ENOENT_L);
    begin();
    put64(1);
    check("INTERRUPT gets no answer", send(36, 0), -1);

    /* FORGET lets nodes go; a new INIT starts afresh. */
    uint32_t before = F.nnodes;
    uint64_t again = lookup(sh, "other.txt", NULL);
    check("lookup counted on the same node", again, oth);
    check("no new node for a known name", F.nnodes, before);
    uint64_t made = lookup(sh, "made-link", NULL);
    forget(made, 2); /* SYMLINK's entry and this lookup */
    check("FORGET frees the node", F.nnodes, before - 1);
    check("forgotten node is unknown", getattr(made), ENOENT_L);
    begin();
    put32(1);
    put32(0);
    put64(oth), put64(100);
    check("BATCH_FORGET gets no answer", send(42, 0), -1);
    check("BATCH_FORGET freed it", getattr(oth), ENOENT_L);
    begin();
    put32(7), put32(31), put32(0x20000), put32(0);
    for (int i = 0; i < 12; i++) put32(0);
    check("INIT again", send(26, 0), 0);
    check("only the top and exports remain", F.nnodes, 3);
    check("exports still answer", lookup(1, "Shared", NULL), sh);

    hv_fuse_free(&F);
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s' '%s'", shared, tools, outside);
    system(cmd);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}

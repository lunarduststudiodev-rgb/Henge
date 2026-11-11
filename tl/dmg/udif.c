/* The UDIF container of a .dmg: a trailer ("koly") pointing at a property
   list whose "blkx" entries describe each partition as runs of sectors, each
   stored raw, zero-filled or compressed. */
#include <CoreFoundation/CoreFoundation.h>
#include <bzlib.h>
#include <compression.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "tl_dmg.h"
#include "tl_dmg_private.h"

enum {
    CHUNK_ZERO = 0x00000000,
    CHUNK_RAW = 0x00000001,
    CHUNK_IGNORE = 0x00000002,
    CHUNK_ADC = 0x80000004,
    CHUNK_ZLIB = 0x80000005,
    CHUNK_BZIP2 = 0x80000006,
    CHUNK_LZFSE = 0x80000007,
    CHUNK_LZMA = 0x80000008,
    CHUNK_COMMENT = 0x7ffffffe,
    CHUNK_END = 0xffffffff,
};

typedef struct Chunk {
    uint64_t sector, count; /* absolute, in 512-byte sectors */
    uint64_t offset, length; /* in the file */
    uint32_t type;
} Chunk;

typedef struct Partition {
    char name[128];
    uint64_t sector, count;
} Partition;

#define CACHE_SLOTS 4

struct TlDisk {
    int fd;
    uint64_t file_size, size;
    Chunk *chunks;
    size_t nchunks;
    Partition *parts;
    int nparts;
    struct {
        long chunk;
        uint8_t *data;
        size_t cap;
        uint64_t used;
    } cache[CACHE_SLOTS];
    uint64_t tick;
    uint8_t *packed;
    size_t packed_cap;
};

void tl_set_err(char *err, size_t len, const char *fmt, ...) {
    if (!err || !len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, len, fmt, ap);
    va_end(ap);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static int chunk_cmp(const void *a, const void *b) {
    const Chunk *x = a, *y = b;
    return x->sector < y->sector ? -1 : x->sector > y->sector;
}

static int add_blkx(TlDisk *d, const uint8_t *b, size_t len, const char *name) {
    if (len < 204 || memcmp(b, "mish", 4)) return -1;
    uint64_t first = be64(b + 8), count = be64(b + 16), data_off = be64(b + 24);
    uint32_t n = be32(b + 200);
    if (204 + (uint64_t)n * 40 > len) return -1;
    Partition *p = realloc(d->parts, (d->nparts + 1) * sizeof *p);
    if (!p) return -1;
    d->parts = p;
    Partition *part = &d->parts[d->nparts++];
    snprintf(part->name, sizeof part->name, "%s", name ? name : "");
    part->sector = first, part->count = count;
    Chunk *c = realloc(d->chunks, (d->nchunks + n) * sizeof *c);
    if (!c) return -1;
    d->chunks = c;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = b + 204 + 40 * i;
        uint32_t type = be32(e);
        if (type == CHUNK_COMMENT || type == CHUNK_END) continue;
        Chunk *ch = &d->chunks[d->nchunks++];
        ch->type = type;
        ch->sector = first + be64(e + 8);
        ch->count = be64(e + 16);
        ch->offset = data_off + be64(e + 24);
        ch->length = be64(e + 32);
        if ((first + be64(e + 8) + ch->count) * 512 > d->size) d->size = (first + be64(e + 8) + ch->count) * 512;
    }
    return 0;
}

static int read_plist(TlDisk *d, uint64_t off, uint64_t len, char *err, size_t errlen) {
    if (!len || off + len > d->file_size || len > (256u << 20)) { tl_set_err(err, errlen, "the image's table is damaged"); return -1; }
    uint8_t *xml = malloc(len);
    if (!xml || pread(d->fd, xml, len, (off_t)off) != (ssize_t)len) { free(xml); tl_set_err(err, errlen, "cannot read the image's table"); return -1; }
    CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, xml, (CFIndex)len, kCFAllocatorNull);
    CFPropertyListRef plist = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    int rc = -1;
    if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
        CFDictionaryRef rf = CFDictionaryGetValue(plist, CFSTR("resource-fork"));
        CFArrayRef blkx = rf && CFGetTypeID(rf) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(rf, CFSTR("blkx")) : NULL;
        if (blkx && CFGetTypeID(blkx) == CFArrayGetTypeID()) {
            rc = 0;
            for (CFIndex i = 0; i < CFArrayGetCount(blkx); i++) {
                CFDictionaryRef e = CFArrayGetValueAtIndex(blkx, i);
                if (CFGetTypeID(e) != CFDictionaryGetTypeID()) continue;
                CFDataRef bytes = CFDictionaryGetValue(e, CFSTR("Data"));
                CFStringRef name = CFDictionaryGetValue(e, CFSTR("Name"));
                if (!name || CFGetTypeID(name) != CFStringGetTypeID()) name = CFDictionaryGetValue(e, CFSTR("CFName"));
                char cname[128] = "";
                if (name && CFGetTypeID(name) == CFStringGetTypeID()) CFStringGetCString(name, cname, sizeof cname, kCFStringEncodingUTF8);
                if (!bytes || CFGetTypeID(bytes) != CFDataGetTypeID()) continue;
                if (add_blkx(d, CFDataGetBytePtr(bytes), (size_t)CFDataGetLength(bytes), cname)) { rc = -1; break; }
            }
        }
    }
    if (plist) CFRelease(plist);
    free(xml);
    if (rc) tl_set_err(err, errlen, "the image's partition table cannot be read");
    return rc;
}

TlDisk *tl_disk_open(const char *path, char *err, size_t errlen) {
    TlDisk *d = calloc(1, sizeof *d);
    d->fd = open(path, O_RDONLY);
    struct stat st;
    if (d->fd < 0 || fstat(d->fd, &st)) {
        tl_set_err(err, errlen, "cannot open %s: %s", path, strerror(errno));
        free(d);
        return NULL;
    }
    d->file_size = (uint64_t)st.st_size;
    for (int i = 0; i < CACHE_SLOTS; i++) d->cache[i].chunk = -1;
    uint8_t koly[512];
    if (d->file_size >= 512 && pread(d->fd, koly, 512, (off_t)(d->file_size - 512)) == 512 && !memcmp(koly, "koly", 4)) {
        uint64_t fork = be64(koly + 24);
        uint64_t xml_off = be64(koly + 216), xml_len = be64(koly + 224);
        if (read_plist(d, xml_off, xml_len, err, errlen)) { tl_disk_close(d); return NULL; }
        for (size_t i = 0; i < d->nchunks; i++) d->chunks[i].offset += fork;
        qsort(d->chunks, d->nchunks, sizeof *d->chunks, chunk_cmp);
        if (be64(koly + 492) * 512 > d->size) d->size = be64(koly + 492) * 512;
    } else {
        /* No trailer: a plain disk or volume image. */
        d->chunks = calloc(1, sizeof *d->chunks);
        d->chunks[0] = (Chunk){0, d->file_size / 512, 0, d->file_size, CHUNK_RAW};
        d->nchunks = 1;
        d->size = d->file_size;
        d->parts = calloc(1, sizeof *d->parts);
        d->parts[0] = (Partition){"whole image", 0, d->file_size / 512};
        d->nparts = 1;
    }
    return d;
}

void tl_disk_close(TlDisk *d) {
    if (!d) return;
    if (d->fd >= 0) close(d->fd);
    for (int i = 0; i < CACHE_SLOTS; i++) free(d->cache[i].data);
    free(d->packed);
    free(d->chunks);
    free(d->parts);
    free(d);
}

uint64_t tl_disk_size(const TlDisk *d) { return d->size; }
int tl_disk_partition_count(const TlDisk *d) { return d->nparts; }
const char *tl_disk_partition_name(const TlDisk *d, int i) { return i >= 0 && i < d->nparts ? d->parts[i].name : NULL; }
uint64_t tl_disk_partition_offset(const TlDisk *d, int i) { return i >= 0 && i < d->nparts ? d->parts[i].sector * 512 : 0; }
uint64_t tl_disk_partition_size(const TlDisk *d, int i) { return i >= 0 && i < d->nparts ? d->parts[i].count * 512 : 0; }

/* Apple Data Compression, the oldest dmg codec. */
static int adc_decode(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen) {
    size_t i = 0, o = 0;
    while (i < inlen && o < outlen) {
        uint8_t b = in[i++];
        if (b & 0x80) {
            size_t n = (b & 0x7f) + 1;
            if (i + n > inlen || o + n > outlen) return -1;
            memcpy(out + o, in + i, n);
            i += n, o += n;
        } else {
            size_t n, dist;
            if (b & 0x40) {
                if (i + 2 > inlen) return -1;
                n = (b & 0x3f) + 4, dist = ((size_t)in[i] << 8 | in[i + 1]) + 1, i += 2;
            } else {
                if (i + 1 > inlen) return -1;
                n = ((b >> 2) & 0x0f) + 3, dist = ((size_t)(b & 3) << 8 | in[i]) + 1, i += 1;
            }
            if (dist > o || o + n > outlen) return -1;
            for (size_t k = 0; k < n; k++, o++) out[o] = out[o - dist];
        }
    }
    return o == outlen ? 0 : -1;
}

int tl_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen, int raw) {
    z_stream z = {0};
    if (inflateInit2(&z, raw ? -15 : 15) != Z_OK) return -1;
    z.next_in = (Bytef *)in, z.avail_in = (uInt)inlen;
    z.next_out = out, z.avail_out = (uInt)outlen;
    int r = inflate(&z, Z_FINISH);
    size_t got = outlen - z.avail_out;
    inflateEnd(&z);
    return (r == Z_STREAM_END || r == Z_OK || r == Z_BUF_ERROR) && got == outlen ? 0 : -1;
}

static int decode_chunk(TlDisk *d, const Chunk *c, uint8_t *out, size_t outlen) {
    if (c->type == CHUNK_ZERO || c->type == CHUNK_IGNORE) {
        memset(out, 0, outlen);
        return 0;
    }
    if (c->type == CHUNK_RAW) {
        size_t n = c->length < outlen ? (size_t)c->length : outlen;
        if (pread(d->fd, out, n, (off_t)c->offset) != (ssize_t)n) return -1;
        if (n < outlen) memset(out + n, 0, outlen - n);
        return 0;
    }
    if (c->length > (512u << 20)) return -1;
    if (d->packed_cap < c->length) {
        free(d->packed);
        d->packed_cap = (size_t)c->length;
        d->packed = malloc(d->packed_cap);
        if (!d->packed) { d->packed_cap = 0; return -1; }
    }
    if (pread(d->fd, d->packed, (size_t)c->length, (off_t)c->offset) != (ssize_t)c->length) return -1;
    switch (c->type) {
    case CHUNK_ZLIB: return tl_inflate(d->packed, (size_t)c->length, out, outlen, 0);
    case CHUNK_ADC: return adc_decode(d->packed, (size_t)c->length, out, outlen);
    case CHUNK_BZIP2: {
        unsigned int got = (unsigned int)outlen;
        return BZ2_bzBuffToBuffDecompress((char *)out, &got, (char *)d->packed, (unsigned int)c->length, 0, 0) == BZ_OK &&
                       got == outlen
                   ? 0
                   : -1;
    }
    case CHUNK_LZFSE:
        return compression_decode_buffer(out, outlen, d->packed, (size_t)c->length, NULL, COMPRESSION_LZFSE) == outlen ? 0 : -1;
    case CHUNK_LZMA:
        return compression_decode_buffer(out, outlen, d->packed, (size_t)c->length, NULL, COMPRESSION_LZMA) == outlen ? 0 : -1;
    default: return -1;
    }
}

static const uint8_t *chunk_data(TlDisk *d, long index) {
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (d->cache[i].chunk == index) {
            d->cache[i].used = ++d->tick;
            return d->cache[i].data;
        }
    int slot = 0;
    for (int i = 1; i < CACHE_SLOTS; i++)
        if (d->cache[i].used < d->cache[slot].used) slot = i;
    const Chunk *c = &d->chunks[index];
    size_t need = (size_t)c->count * 512;
    if (d->cache[slot].cap < need) {
        free(d->cache[slot].data);
        d->cache[slot].data = malloc(need);
        d->cache[slot].cap = d->cache[slot].data ? need : 0;
        if (!d->cache[slot].data) return NULL;
    }
    d->cache[slot].chunk = -1;
    if (decode_chunk(d, c, d->cache[slot].data, need)) return NULL;
    d->cache[slot].chunk = index;
    d->cache[slot].used = ++d->tick;
    return d->cache[slot].data;
}

int tl_disk_read(TlDisk *d, uint64_t off, void *buf, size_t len) {
    uint8_t *out = buf;
    while (len) {
        uint64_t sector = off / 512;
        /* The chunk holding this sector: the last one starting at or before it. */
        size_t lo = 0, hi = d->nchunks;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (d->chunks[mid].sector <= sector) lo = mid + 1;
            else hi = mid;
        }
        if (!lo) return -1;
        long idx = (long)lo - 1;
        const Chunk *c = &d->chunks[idx];
        uint64_t start = c->sector * 512, end = (c->sector + c->count) * 512;
        if (off >= end) { /* a gap the image does not describe: zeros */
            uint64_t next = (size_t)lo < d->nchunks ? d->chunks[lo].sector * 512 : d->size;
            if (next <= off) return -1;
            size_t n = (size_t)(next - off < len ? next - off : len);
            memset(out, 0, n);
            out += n, off += n, len -= n;
            continue;
        }
        size_t n = (size_t)(end - off < len ? end - off : len);
        if (c->type == CHUNK_RAW) {
            uint64_t at = c->offset + (off - start);
            uint64_t avail = off - start < c->length ? c->length - (off - start) : 0;
            size_t r = (size_t)(avail < n ? avail : n);
            if (r && pread(d->fd, out, r, (off_t)at) != (ssize_t)r) return -1;
            if (r < n) memset(out + r, 0, n - r);
        } else if (c->type == CHUNK_ZERO || c->type == CHUNK_IGNORE) {
            memset(out, 0, n);
        } else {
            const uint8_t *data = chunk_data(d, idx);
            if (!data) return -1;
            memcpy(out, data + (off - start), n);
        }
        out += n, off += n, len -= n;
    }
    return 0;
}

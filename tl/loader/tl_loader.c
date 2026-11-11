/* Henge's Mach-O loader for the Mac-app translation layer. See tl_loader.h. */
#include "tl_loader.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <limits.h>
#include <mach-o/fat.h>
#include <mach-o/fixup-chains.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define TL_PAGE 0x4000ull
#define TL_MAX_SEGS 32
#define TL_MAX_DEPS 256
#define TL_MAX_RPATHS 32

typedef struct TlSeg {
    char name[17];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    int prot;
} TlSeg;

typedef struct TlDep {
    char *install_name;
    int weak, reexport;
    TlImage *tl;        /* a guest image this loader loaded */
    void *handle;       /* or a host library */
    int no_library;     /* neither: only the shim can answer */
} TlDep;

struct TlImage {
    char *path, *dir;
    int fd;
    uint8_t *map;
    size_t maplen;
    const uint8_t *slice; /* the arm64 slice of the file */
    size_t slicelen;
    const struct mach_header_64 *fmh;
    uint8_t *base;        /* where the mach header (start of __TEXT) is mapped */
    int64_t slide;        /* runtime address - vmaddr */
    uint64_t vmmin, vmsize;
    TlSeg segs[TL_MAX_SEGS];
    int nsegs;
    TlDep deps[TL_MAX_DEPS];
    int ndeps;
    char *rpaths[TL_MAX_RPATHS];
    int nrpaths;
    const uint8_t *trie;
    uint32_t trie_size;
    const struct linkedit_data_command *chained;
    const struct dyld_info_command *dyldinfo;
    uint64_t entryoff;
    int has_main;
    int initialized, visiting;
    pthread_key_t tlv_key;
    uint64_t tlv_start, tlv_size, tlv_init_size; /* vmaddrs */
    TlImage *next;
};

static TlHost *g_host;
static TlImage *g_images, *g_main;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- messages ---- */

static void tl_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_host && g_host->log) g_host->log(line);
    else fprintf(stderr, "tl: %s\n", line);
}

static void set_err(char *err, size_t len, const char *fmt, ...) {
    if (!err || !len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, len, fmt, ap);
    va_end(ap);
}

/* ---- the executable region ---- */

int tl_host_init_split(TlHost *host, size_t bytes) {
    memset(host, 0, sizeof *host);
    bytes = (bytes + TL_PAGE - 1) & ~(TL_PAGE - 1);
    void *rx = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rx == MAP_FAILED) return -1;
    vm_address_t rw = 0;
    vm_prot_t cur, max;
    if (vm_remap(mach_task_self(), &rw, bytes, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)rx, FALSE, &cur,
                 &max, VM_INHERIT_NONE) != KERN_SUCCESS ||
        mprotect(rx, bytes, PROT_READ | PROT_EXEC) != 0) {
        munmap(rx, bytes);
        return -1;
    }
    tl_host_init_region(host, rx, (void *)rw, bytes);
    return 0;
}

void tl_host_init_region(TlHost *host, void *rx, void *rw, size_t bytes) {
    host->rx = rx;
    host->rw_diff = (intptr_t)((uint8_t *)rw - (uint8_t *)rx);
    host->size = bytes;
    host->used = 0;
}

static uint8_t *region_take(size_t bytes) {
    bytes = (bytes + TL_PAGE - 1) & ~(TL_PAGE - 1);
    if (g_host->used + bytes > g_host->size) return NULL;
    uint8_t *p = g_host->rx + g_host->used;
    g_host->used += bytes;
    return p;
}

/* Code for stubs the loader writes itself (missing symbols). */
static uint8_t *g_stub_page, *g_stub_end, *g_stub_next;

static uint32_t *stub_alloc(size_t bytes) {
    if (!g_stub_page || g_stub_next + bytes > g_stub_end) {
        g_stub_page = region_take(TL_PAGE);
        if (!g_stub_page) return NULL;
        g_stub_next = g_stub_page;
        g_stub_end = g_stub_page + TL_PAGE;
    }
    uint32_t *p = (uint32_t *)g_stub_next;
    g_stub_next += bytes;
    return p;
}

static void code_write(void *rx_dst, const void *src, size_t bytes) {
    memcpy((uint8_t *)rx_dst + g_host->rw_diff, src, bytes);
}

static void code_finish(void *rx, size_t bytes) {
    sys_dcache_flush((uint8_t *)rx + g_host->rw_diff, bytes);
    sys_icache_invalidate(rx, bytes);
}

/* ---- missing symbols ---- */

static char *g_missing[4096];
static int g_nmissing;

int tl_missing_count(void) { return g_nmissing; }
const char *tl_missing_name(int i) { return i >= 0 && i < g_nmissing ? g_missing[i] : NULL; }

static void tl_missing_called(const char *name) {
    tl_log("called a missing symbol: %s", name);
    fflush(NULL);
    abort();
}

/* ldr x0, name; ldr x16, handler; br x16; nop; then the two literals. */
static uint64_t missing_stub(const char *name) {
    char *copy = strdup(name);
    if (g_nmissing < (int)(sizeof g_missing / sizeof *g_missing)) g_missing[g_nmissing++] = copy;
    uint32_t *p = stub_alloc(32);
    if (!p) return 0;
    uint32_t code[8];
    code[0] = 0x58000000u | (4u << 5) | 0;  /* ldr x0, [pc + 16] -> code[4..5] */
    code[1] = 0x58000000u | (5u << 5) | 16; /* ldr x16, [pc + 20] -> code[6..7] */
    code[2] = 0xd61f0200u;                  /* br x16 */
    code[3] = 0xd503201fu;                  /* nop */
    uint64_t name_addr = (uint64_t)copy, handler = (uint64_t)&tl_missing_called;
    memcpy(&code[4], &name_addr, 8);
    memcpy(&code[6], &handler, 8);
    code_write(p, code, sizeof code);
    code_finish(p, sizeof code);
    return (uint64_t)p;
}

/* ---- reading the file ---- */

static uint64_t read_uleb(const uint8_t **pp, const uint8_t *end) {
    uint64_t v = 0;
    int shift = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        uint8_t b = *p++;
        if (shift < 64) v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    *pp = p;
    return v;
}

static int64_t read_sleb(const uint8_t **pp, const uint8_t *end) {
    int64_t v = 0;
    int shift = 0;
    uint8_t b = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        b = *p++;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    if (shift < 64 && (b & 0x40)) v |= -((int64_t)1 << shift);
    *pp = p;
    return v;
}

static int pick_slice(TlImage *im, char *err, size_t errlen) {
    const uint8_t *m = im->map;
    uint32_t magic = *(const uint32_t *)m;
    if (magic == FAT_CIGAM || magic == FAT_MAGIC || magic == FAT_CIGAM_64 || magic == FAT_MAGIC_64) {
        int swap = magic == FAT_CIGAM || magic == FAT_CIGAM_64, wide = magic == FAT_CIGAM_64 || magic == FAT_MAGIC_64;
        const struct fat_header *fh = (const void *)m;
        uint32_t n = swap ? OSSwapInt32(fh->nfat_arch) : fh->nfat_arch;
        int saw_arm64e = 0, saw_x86 = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t cpu, sub;
            uint64_t off, size;
            if (wide) {
                const struct fat_arch_64 *a = (const void *)(m + sizeof *fh + i * sizeof(struct fat_arch_64));
                cpu = swap ? OSSwapInt32(a->cputype) : a->cputype;
                sub = swap ? OSSwapInt32(a->cpusubtype) : a->cpusubtype;
                off = swap ? OSSwapInt64(a->offset) : a->offset;
                size = swap ? OSSwapInt64(a->size) : a->size;
            } else {
                const struct fat_arch *a = (const void *)(m + sizeof *fh + i * sizeof(struct fat_arch));
                cpu = swap ? OSSwapInt32(a->cputype) : a->cputype;
                sub = swap ? OSSwapInt32(a->cpusubtype) : a->cpusubtype;
                off = swap ? OSSwapInt32(a->offset) : a->offset;
                size = swap ? OSSwapInt32(a->size) : a->size;
            }
            if (cpu == CPU_TYPE_X86_64) saw_x86 = 1;
            if (cpu != CPU_TYPE_ARM64) continue;
            /* Plain arm64 is subtype 0 or 1; 2 (and newer values, 12 on macOS 27) are arm64e with pointer authentication. */
            if ((sub & ~CPU_SUBTYPE_MASK) > CPU_SUBTYPE_ARM64_V8) { saw_arm64e = 1; continue; }
            if (off + size > im->maplen) break;
            im->slice = m + off;
            im->slicelen = size;
            return 0;
        }
        set_err(err, errlen, "%s has no arm64 code%s", im->path,
                saw_arm64e ? " (only arm64e, Apple's own)" : saw_x86 ? " (Intel only)" : "");
        return -1;
    }
    if (magic != MH_MAGIC_64) {
        set_err(err, errlen, "%s is not a 64-bit Mach-O file", im->path);
        return -1;
    }
    const struct mach_header_64 *mh = (const void *)m;
    if (mh->cputype != CPU_TYPE_ARM64 || (mh->cpusubtype & ~CPU_SUBTYPE_MASK) > CPU_SUBTYPE_ARM64_V8) {
        set_err(err, errlen, "%s is %s, not arm64", im->path,
                mh->cputype == CPU_TYPE_X86_64 ? "Intel" : mh->cputype == CPU_TYPE_ARM64 ? "arm64e" : "another CPU's code");
        return -1;
    }
    im->slice = m;
    im->slicelen = im->maplen;
    return 0;
}

#define FOR_EACH_CMD(im, lc)                                                                                  \
    for (const struct load_command *lc = (const void *)((im)->slice + sizeof(struct mach_header_64)),        \
                                   *lc##_end = (const void *)((im)->slice + sizeof(struct mach_header_64) + \
                                                              (im)->fmh->sizeofcmds);                        \
         lc < lc##_end && lc->cmdsize; lc = (const void *)((const uint8_t *)lc + lc->cmdsize))

static int parse(TlImage *im, char *err, size_t errlen) {
    if (pick_slice(im, err, errlen)) return -1;
    im->fmh = (const void *)im->slice;
    if (im->fmh->filetype != MH_EXECUTE && im->fmh->filetype != MH_DYLIB && im->fmh->filetype != MH_BUNDLE) {
        set_err(err, errlen, "%s: unsupported Mach-O type %u", im->path, im->fmh->filetype);
        return -1;
    }
    uint64_t lo = UINT64_MAX, hi = 0;
    FOR_EACH_CMD(im, lc) {
        switch (lc->cmd) {
        case LC_SEGMENT_64: {
            const struct segment_command_64 *s = (const void *)lc;
            if (!strcmp(s->segname, "__PAGEZERO")) break;
            if (im->nsegs == TL_MAX_SEGS) { set_err(err, errlen, "%s: too many segments", im->path); return -1; }
            TlSeg *g = &im->segs[im->nsegs++];
            memcpy(g->name, s->segname, 16);
            g->vmaddr = s->vmaddr, g->vmsize = s->vmsize, g->fileoff = s->fileoff, g->filesize = s->filesize;
            g->prot = s->initprot;
            if ((s->vmaddr & (TL_PAGE - 1)) && s->vmsize) {
                set_err(err, errlen, "%s: segment %s is not 16 KB aligned", im->path, g->name);
                return -1;
            }
            if (s->vmsize && s->vmaddr < lo) lo = s->vmaddr;
            if (s->vmaddr + s->vmsize > hi) hi = s->vmaddr + s->vmsize;
            break;
        }
        case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB: case LC_LOAD_UPWARD_DYLIB:
        case LC_LAZY_LOAD_DYLIB: {
            const struct dylib_command *d = (const void *)lc;
            if (im->ndeps == TL_MAX_DEPS) { set_err(err, errlen, "%s: too many libraries", im->path); return -1; }
            TlDep *dep = &im->deps[im->ndeps++];
            dep->install_name = strdup((const char *)lc + d->dylib.name.offset);
            dep->weak = lc->cmd == LC_LOAD_WEAK_DYLIB;
            dep->reexport = lc->cmd == LC_REEXPORT_DYLIB;
            break;
        }
        case LC_RPATH: {
            const struct rpath_command *r = (const void *)lc;
            if (im->nrpaths < TL_MAX_RPATHS) im->rpaths[im->nrpaths++] = strdup((const char *)lc + r->path.offset);
            break;
        }
        case LC_DYLD_EXPORTS_TRIE: {
            const struct linkedit_data_command *l = (const void *)lc;
            im->trie = im->slice + l->dataoff, im->trie_size = l->datasize;
            break;
        }
        case LC_DYLD_CHAINED_FIXUPS: im->chained = (const void *)lc; break;
        case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY:
            im->dyldinfo = (const void *)lc;
            if (!im->trie && im->dyldinfo->export_size)
                im->trie = im->slice + im->dyldinfo->export_off, im->trie_size = im->dyldinfo->export_size;
            break;
        case LC_MAIN: {
            const struct entry_point_command *e = (const void *)lc;
            im->entryoff = e->entryoff, im->has_main = 1;
            break;
        }
        case LC_ENCRYPTION_INFO_64: {
            const struct encryption_info_command_64 *e = (const void *)lc;
            if (e->cryptid) { set_err(err, errlen, "%s is encrypted (App Store FairPlay)", im->path); return -1; }
            break;
        }
        default: break;
        }
    }
    if (!im->nsegs || lo == UINT64_MAX) { set_err(err, errlen, "%s has no segments", im->path); return -1; }
    im->vmmin = lo;
    im->vmsize = (hi - lo + TL_PAGE - 1) & ~(TL_PAGE - 1);
    return 0;
}

/* ---- mapping ---- */

static int map_segments(TlImage *im, char *err, size_t errlen) {
    uint8_t *base = region_take(im->vmsize);
    if (!base) { set_err(err, errlen, "%s: the executable region is full", im->path); return -1; }
    im->base = base;
    im->slide = (int64_t)((uint64_t)base - im->vmmin);
    for (int i = 0; i < im->nsegs; i++) {
        TlSeg *g = &im->segs[i];
        if (!g->vmsize) continue;
        uint8_t *dst = (uint8_t *)(g->vmaddr + im->slide);
        uint64_t span = (g->vmsize + TL_PAGE - 1) & ~(TL_PAGE - 1);
        uint64_t copy = g->filesize < g->vmsize ? g->filesize : g->vmsize;
        if (g->fileoff + copy > im->slicelen) { set_err(err, errlen, "%s: segment %s is cut short", im->path, g->name); return -1; }
        if (g->prot & VM_PROT_EXECUTE) {
            code_write(dst, im->slice + g->fileoff, copy); /* the rest of the region is still zero: it is never reused */
            code_finish(dst, span);
        } else {
            /* Ordinary memory in place of these pages of the region, at the file's distance from the code. */
            void *p = mmap(dst, span, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
            if (p != dst) { set_err(err, errlen, "%s: cannot place segment %s (%s)", im->path, g->name, strerror(errno)); return -1; }
            memcpy(dst, im->slice + g->fileoff, copy);
        }
    }
    return 0;
}

/* ---- libraries ---- */

static TlImage *load_one(const char *path, char *err, size_t errlen);

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* @executable_path, @loader_path and @rpath against this image and the executable. */
static int expand_path(TlImage *im, const char *in, char *out, size_t len) {
    const char *exe_dir = g_main ? g_main->dir : im->dir;
    if (!strncmp(in, "@executable_path/", 17)) return snprintf(out, len, "%s/%s", exe_dir, in + 17) < (int)len;
    if (!strncmp(in, "@loader_path/", 13)) return snprintf(out, len, "%s/%s", im->dir, in + 13) < (int)len;
    if (!strncmp(in, "@rpath/", 7)) {
        TlImage *owners[2] = {im, g_main && g_main != im ? g_main : NULL};
        for (int o = 0; o < 2; o++) {
            TlImage *who = owners[o];
            if (!who) continue;
            for (int i = 0; i < who->nrpaths; i++) {
                char base[PATH_MAX];
                if (!expand_path(who, who->rpaths[i], base, sizeof base)) continue;
                snprintf(out, len, "%s/%s", base, in + 7);
                if (file_exists(out)) return 1;
            }
        }
        return 0;
    }
    return snprintf(out, len, "%s", in) < (int)len;
}

static int load_deps(TlImage *im, char *err, size_t errlen) {
    for (int i = 0; i < im->ndeps; i++) {
        TlDep *dep = &im->deps[i];
        const char *name = dep->install_name;
        if (name[0] == '@') {
            char path[PATH_MAX];
            if (expand_path(im, name, path, sizeof path) && file_exists(path)) {
                char real[PATH_MAX];
                if (!realpath(path, real)) snprintf(real, sizeof real, "%s", path);
                dep->tl = load_one(real, err, errlen);
                if (!dep->tl) return -1;
                continue;
            }
            if (dep->weak) { dep->no_library = 1; continue; }
            set_err(err, errlen, "%s needs %s, which is not in the app", im->path, name);
            return -1;
        }
        char buf[PATH_MAX];
        const char *host_path = g_host->map_install_name ? g_host->map_install_name(name, buf, sizeof buf) : name;
        if (host_path) dep->handle = dlopen(host_path, RTLD_LAZY | RTLD_GLOBAL);
        if (!dep->handle) {
            dep->no_library = 1;
            if (g_host->verbose || (!dep->weak && !g_host->shim_lookup))
                tl_log("%s: no host library for %s%s", im->path, name, host_path ? "" : " (none on this host)");
        }
    }
    return 0;
}

/* ---- symbols ---- */

typedef struct TlExport {
    int found;
    uint64_t flags;
    uint64_t value;      /* image offset, or absolute */
    uint64_t other;      /* re-export: library ordinal; resolver offset */
    const char *import;  /* re-export: the name in that library ("" = same name) */
} TlExport;

static TlExport trie_find(const uint8_t *start, uint32_t size, const char *sym) {
    TlExport r = {0};
    const uint8_t *end = start + size, *p = start;
    if (!start || !size) return r;
    for (int depth = 0; depth < 512; depth++) {
        if (p >= end) return r;
        const uint8_t *q = p;
        uint64_t term = read_uleb(&q, end);
        if (*sym == 0 && term) {
            r.flags = read_uleb(&q, end);
            if (r.flags & EXPORT_SYMBOL_FLAGS_REEXPORT) {
                r.other = read_uleb(&q, end);
                r.import = (const char *)q;
            } else {
                r.value = read_uleb(&q, end);
                if (r.flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER) r.other = read_uleb(&q, end);
            }
            r.found = 1;
            return r;
        }
        const uint8_t *kids = q + term;
        if (kids >= end) return r;
        uint8_t n = *kids++;
        const uint8_t *next = NULL;
        for (int i = 0; i < n && !next; i++) {
            const char *edge = (const char *)kids;
            size_t el = strnlen(edge, (size_t)(end - kids));
            kids += el + 1;
            uint64_t off = read_uleb(&kids, end);
            if (!strncmp(sym, edge, el)) {
                sym += el;
                next = start + off;
            }
        }
        if (!next) return r;
        p = next;
    }
    return r;
}

static uint64_t resolve_in_dep(TlImage *im, int ordinal, const char *sym, int *found);

/* An exported symbol of a guest image, following re-exports. */
static uint64_t image_export(TlImage *im, const char *sym, int *found) {
    TlExport e = trie_find(im->trie, im->trie_size, sym);
    if (!e.found) {
        /* Re-exported libraries without a trie entry (whole-library re-exports). */
        for (int i = 0; i < im->ndeps; i++) {
            if (!im->deps[i].reexport) continue;
            int f = 0;
            uint64_t v = resolve_in_dep(im, i + 1, sym, &f);
            if (f) { *found = 1; return v; }
        }
        *found = 0;
        return 0;
    }
    *found = 1;
    if (e.flags & EXPORT_SYMBOL_FLAGS_REEXPORT)
        return resolve_in_dep(im, (int)e.other, e.import && *e.import ? e.import : sym, found);
    uint64_t kind = e.flags & EXPORT_SYMBOL_FLAGS_KIND_MASK;
    if (kind == EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE) return e.value;
    if (e.flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER) {
        uint64_t (*resolver)(void) = (void *)(im->base + e.other);
        return resolver();
    }
    return (uint64_t)(im->base + e.value);
}

static uint64_t host_symbol(void *handle, const char *install_name, const char *sym, int *found) {
    *found = 0;
    if (g_host->shim_lookup && install_name) {
        void *s = g_host->shim_lookup(install_name, sym);
        if (s) { *found = 1; return (uint64_t)s; }
    }
    if (!handle || sym[0] != '_') return 0;
    void *s = dlsym(handle, sym + 1);
    if (s) *found = 1;
    return (uint64_t)s;
}

static uint64_t flat_lookup(const char *sym, int *found) {
    for (TlImage *im = g_images; im; im = im->next) {
        uint64_t v = image_export(im, sym, found);
        if (*found) return v;
    }
    return host_symbol(RTLD_DEFAULT, NULL, sym, found);
}

static uint64_t resolve_in_dep(TlImage *im, int ordinal, const char *sym, int *found) {
    *found = 0;
    if (ordinal == BIND_SPECIAL_DYLIB_SELF) return image_export(im, sym, found);
    if (ordinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE) return g_main ? image_export(g_main, sym, found) : 0;
    if (ordinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP) return flat_lookup(sym, found);
    if (ordinal == BIND_SPECIAL_DYLIB_WEAK_LOOKUP) {
        uint64_t v = host_symbol(RTLD_DEFAULT, NULL, sym, found);
        if (*found) return v;
        return flat_lookup(sym, found);
    }
    if (ordinal < 1 || ordinal > im->ndeps) return 0;
    TlDep *dep = &im->deps[ordinal - 1];
    if (dep->tl) return image_export(dep->tl, sym, found);
    return host_symbol(dep->handle, dep->install_name, sym, found);
}

static void tl_stub_binder(void) { abort(); } /* old binaries name it; every lazy pointer is bound up front here */

static int resolve_import(TlImage *im, int ordinal, const char *sym, int weak, uint64_t *out, char *err, size_t errlen) {
    if (!strcmp(sym, "dyld_stub_binder")) { *out = (uint64_t)&tl_stub_binder; return 0; }
    int found = 0;
    uint64_t v = resolve_in_dep(im, ordinal, sym, &found);
    if (found) { *out = v; return 0; }
    if (weak) { *out = 0; return 0; }
    const char *lib = ordinal >= 1 && ordinal <= im->ndeps ? im->deps[ordinal - 1].install_name : "(flat)";
    if (g_host->strict) {
        set_err(err, errlen, "%s: %s is missing from %s", im->path, sym, lib);
        return -1;
    }
    if (g_host->verbose) tl_log("%s: %s missing from %s, bound to a stub", im->path, sym, lib);
    *out = missing_stub(sym);
    return 0;
}

/* ---- fixups ---- */

static int fixup_chained(TlImage *im, char *err, size_t errlen) {
    const uint8_t *data = im->slice + im->chained->dataoff;
    const struct dyld_chained_fixups_header *h = (const void *)data;
    if (h->fixups_version != 0 || h->symbols_format != 0) {
        set_err(err, errlen, "%s: unknown chained fixups version", im->path);
        return -1;
    }
    uint64_t *targets = calloc(h->imports_count ? h->imports_count : 1, sizeof *targets);
    const char *syms = (const char *)data + h->symbols_offset;
    const uint8_t *imp = data + h->imports_offset;
    for (uint32_t i = 0; i < h->imports_count; i++) {
        int ordinal, weak;
        uint32_t name_off;
        int64_t addend = 0;
        if (h->imports_format == DYLD_CHAINED_IMPORT) {
            const struct dyld_chained_import *x = (const void *)(imp + 4 * i);
            ordinal = x->lib_ordinal > 0xF0 ? (int8_t)x->lib_ordinal : x->lib_ordinal;
            weak = x->weak_import, name_off = x->name_offset;
        } else if (h->imports_format == DYLD_CHAINED_IMPORT_ADDEND) {
            const struct dyld_chained_import_addend *x = (const void *)(imp + 8 * i);
            ordinal = x->lib_ordinal > 0xF0 ? (int8_t)x->lib_ordinal : x->lib_ordinal;
            weak = x->weak_import, name_off = x->name_offset, addend = x->addend;
        } else if (h->imports_format == DYLD_CHAINED_IMPORT_ADDEND64) {
            const struct dyld_chained_import_addend64 *x = (const void *)(imp + 16 * i);
            ordinal = x->lib_ordinal > 0xFFF0 ? (int16_t)x->lib_ordinal : x->lib_ordinal;
            weak = x->weak_import, name_off = x->name_offset, addend = (int64_t)x->addend;
        } else {
            set_err(err, errlen, "%s: unknown import format %u", im->path, h->imports_format);
            free(targets);
            return -1;
        }
        uint64_t v;
        if (resolve_import(im, ordinal, syms + name_off, weak, &v, err, errlen)) { free(targets); return -1; }
        targets[i] = v ? v + (uint64_t)addend : 0;
    }
    const struct dyld_chained_starts_in_image *starts = (const void *)(data + h->starts_offset);
    for (uint32_t s = 0; s < starts->seg_count; s++) {
        if (!starts->seg_info_offset[s]) continue;
        const struct dyld_chained_starts_in_segment *seg = (const void *)((const uint8_t *)starts + starts->seg_info_offset[s]);
        if (seg->pointer_format != DYLD_CHAINED_PTR_64 && seg->pointer_format != DYLD_CHAINED_PTR_64_OFFSET) {
            set_err(err, errlen, "%s: pointer format %u (arm64e?) is not supported", im->path, seg->pointer_format);
            free(targets);
            return -1;
        }
        for (uint32_t pg = 0; pg < seg->page_count; pg++) {
            uint16_t off = seg->page_start[pg];
            if (off == DYLD_CHAINED_PTR_START_NONE) continue;
            uint8_t *loc = im->base + seg->segment_offset + (uint64_t)pg * seg->page_size + off;
            for (;;) {
                uint64_t raw;
                memcpy(&raw, loc, 8);
                uint64_t next, val;
                if (raw >> 63) { /* bind */
                    uint32_t ordinal = raw & 0xFFFFFF;
                    uint64_t addend = (raw >> 24) & 0xFF;
                    next = (raw >> 51) & 0xFFF;
                    val = ordinal < h->imports_count ? targets[ordinal] : 0;
                    if (val) val += addend;
                } else {
                    uint64_t target = raw & 0xFFFFFFFFFull, high8 = (raw >> 36) & 0xFF;
                    next = (raw >> 51) & 0xFFF;
                    val = seg->pointer_format == DYLD_CHAINED_PTR_64 ? target + (uint64_t)im->slide
                                                                      : (uint64_t)im->base + target;
                    val |= high8 << 56;
                }
                memcpy(loc, &val, 8);
                if (!next) break;
                loc += next * 4;
            }
        }
    }
    free(targets);
    return 0;
}

static uint8_t *seg_addr(TlImage *im, int seg, uint64_t off) {
    if (seg < 0 || seg >= im->nsegs) return NULL;
    return (uint8_t *)(im->segs[seg].vmaddr + im->slide + off);
}

/* The segment index the opcodes use counts every LC_SEGMENT_64, __PAGEZERO included. */
static int opcode_seg(TlImage *im, int index) {
    int k = 0, mine = 0;
    FOR_EACH_CMD(im, lc) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *s = (const void *)lc;
        int skipped = !strcmp(s->segname, "__PAGEZERO");
        if (k == index) return skipped ? -1 : mine;
        if (!skipped) mine++;
        k++;
    }
    return -1;
}

static int do_rebases(TlImage *im, char *err, size_t errlen) {
    const uint8_t *p = im->slice + im->dyldinfo->rebase_off, *end = p + im->dyldinfo->rebase_size;
    int seg = -1, type = REBASE_TYPE_POINTER;
    uint64_t off = 0;
    while (p < end) {
        uint8_t b = *p++, op = b & REBASE_OPCODE_MASK, imm = b & REBASE_IMMEDIATE_MASK;
        uint64_t count, skip;
        switch (op) {
        case REBASE_OPCODE_DONE: return 0;
        case REBASE_OPCODE_SET_TYPE_IMM: type = imm; break;
        case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: seg = opcode_seg(im, imm); off = read_uleb(&p, end); break;
        case REBASE_OPCODE_ADD_ADDR_ULEB: off += read_uleb(&p, end); break;
        case REBASE_OPCODE_ADD_ADDR_IMM_SCALED: off += imm * 8; break;
        case REBASE_OPCODE_DO_REBASE_IMM_TIMES: count = imm, skip = 0; goto rebase;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: count = read_uleb(&p, end), skip = 0; goto rebase;
        case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: count = 1, skip = read_uleb(&p, end); goto rebase;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
            count = read_uleb(&p, end), skip = read_uleb(&p, end);
        rebase:
            for (uint64_t i = 0; i < count; i++) {
                uint8_t *loc = seg_addr(im, seg, off);
                if (!loc || type != REBASE_TYPE_POINTER) { set_err(err, errlen, "%s: bad rebase", im->path); return -1; }
                uint64_t v;
                memcpy(&v, loc, 8);
                v += (uint64_t)im->slide;
                memcpy(loc, &v, 8);
                off += 8 + skip;
            }
            break;
        default: set_err(err, errlen, "%s: unknown rebase opcode %#x", im->path, op); return -1;
        }
    }
    return 0;
}

static int do_binds(TlImage *im, const uint8_t *p, const uint8_t *end, int lazy, char *err, size_t errlen) {
    int seg = -1, ordinal = 0, weak = 0;
    const char *sym = "";
    int64_t addend = 0;
    uint64_t off = 0;
    while (p < end) {
        uint8_t b = *p++, op = b & BIND_OPCODE_MASK, imm = b & BIND_IMMEDIATE_MASK;
        uint64_t count, skip;
        switch (op) {
        case BIND_OPCODE_DONE: if (!lazy) return 0; break; /* lazy binds end each entry with DONE */
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM: ordinal = imm; break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: ordinal = (int)read_uleb(&p, end); break;
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM: ordinal = imm ? (int)(int8_t)(BIND_OPCODE_MASK | imm) : 0; break;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
            sym = (const char *)p;
            p += strlen(sym) + 1;
            weak = (imm & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0;
            break;
        case BIND_OPCODE_SET_TYPE_IMM: break;
        case BIND_OPCODE_SET_ADDEND_SLEB: addend = read_sleb(&p, end); break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: seg = opcode_seg(im, imm); off = read_uleb(&p, end); break;
        case BIND_OPCODE_ADD_ADDR_ULEB: off += read_uleb(&p, end); break;
        case BIND_OPCODE_DO_BIND: count = 1, skip = 0; goto bind;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: count = 1, skip = read_uleb(&p, end); goto bind;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: count = 1, skip = imm * 8ull; goto bind;
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
            count = read_uleb(&p, end), skip = read_uleb(&p, end);
        bind: {
            uint64_t v;
            if (resolve_import(im, ordinal, sym, weak, &v, err, errlen)) return -1;
            if (v) v += (uint64_t)addend;
            for (uint64_t i = 0; i < count; i++) {
                uint8_t *loc = seg_addr(im, seg, off);
                if (!loc) { set_err(err, errlen, "%s: bad bind for %s", im->path, sym); return -1; }
                memcpy(loc, &v, 8);
                off += 8 + skip;
            }
            break;
        }
        case BIND_OPCODE_THREADED: set_err(err, errlen, "%s: threaded binds are not supported", im->path); return -1;
        default: set_err(err, errlen, "%s: unknown bind opcode %#x", im->path, op); return -1;
        }
    }
    return 0;
}

/* Weak definitions: where the image's own definition should give way to one already loaded. */
static int do_weak_binds(TlImage *im) {
    const uint8_t *p = im->slice + im->dyldinfo->weak_bind_off, *end = p + im->dyldinfo->weak_bind_size;
    int seg = -1;
    const char *sym = "";
    int64_t addend = 0;
    uint64_t off = 0;
    int strong = 0;
    while (p < end) {
        uint8_t b = *p++, op = b & BIND_OPCODE_MASK, imm = b & BIND_IMMEDIATE_MASK;
        uint64_t count, skip;
        switch (op) {
        case BIND_OPCODE_DONE: return 0;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
            sym = (const char *)p;
            p += strlen(sym) + 1;
            strong = (imm & BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION) != 0;
            break;
        case BIND_OPCODE_SET_TYPE_IMM: break;
        case BIND_OPCODE_SET_ADDEND_SLEB: addend = read_sleb(&p, end); break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: seg = opcode_seg(im, imm); off = read_uleb(&p, end); break;
        case BIND_OPCODE_ADD_ADDR_ULEB: off += read_uleb(&p, end); break;
        case BIND_OPCODE_DO_BIND: count = 1, skip = 0; goto bind;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: count = 1, skip = read_uleb(&p, end); goto bind;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: count = 1, skip = imm * 8ull; goto bind;
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
            count = read_uleb(&p, end), skip = read_uleb(&p, end);
        bind: {
            int found = 0;
            uint64_t v = strong ? 0 : host_symbol(RTLD_DEFAULT, NULL, sym, &found);
            for (uint64_t i = 0; i < count; i++) {
                uint8_t *loc = seg_addr(im, seg, off);
                if (loc && found) { uint64_t w = v + (uint64_t)addend; memcpy(loc, &w, 8); }
                off += 8 + skip;
            }
            break;
        }
        default: return 0;
        }
    }
    return 0;
}

static int fixup(TlImage *im, char *err, size_t errlen) {
    if (im->chained) return fixup_chained(im, err, errlen);
    if (!im->dyldinfo) return 0;
    if (do_rebases(im, err, errlen)) return -1;
    const uint8_t *s = im->slice;
    const struct dyld_info_command *d = im->dyldinfo;
    if (do_binds(im, s + d->bind_off, s + d->bind_off + d->bind_size, 0, err, errlen)) return -1;
    if (do_binds(im, s + d->lazy_bind_off, s + d->lazy_bind_off + d->lazy_bind_size, 1, err, errlen)) return -1;
    if (d->weak_bind_size) do_weak_binds(im);
    return 0;
}

/* ---- sections ---- */

typedef void (^TlSectionVisitor)(const struct section_64 *sect, uint8_t *addr);

static void each_section(TlImage *im, TlSectionVisitor visit) {
    FOR_EACH_CMD(im, lc) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *s = (const void *)lc;
        const struct section_64 *sect = (const void *)(s + 1);
        for (uint32_t i = 0; i < s->nsects; i++) visit(&sect[i], (uint8_t *)(sect[i].addr + im->slide));
    }
}

static uint8_t *find_section(TlImage *im, const char *seg, const char *name, uint64_t *size) {
    __block uint8_t *found = NULL;
    __block uint64_t sz = 0;
    each_section(im, ^(const struct section_64 *sect, uint8_t *addr) {
        if (!found && !strncmp(sect->segname, seg, 16) && !strncmp(sect->sectname, name, 16)) found = addr, sz = sect->size;
    });
    if (size) *size = sz;
    return found;
}

/* ---- thread-local variables ---- */

typedef struct TlvDesc {
    void *thunk;
    uint64_t key;
    uint64_t offset;
} TlvDesc;

extern void *tl_tlv_get_addr(TlvDesc *d); /* tl_tlv.S */

static TlImage *g_tlv_images[256];
static int g_ntlv;

void *tl_tlv_instantiate(TlvDesc *d) {
    TlImage *im = NULL;
    for (int i = 0; i < g_ntlv; i++)
        if ((uint64_t)g_tlv_images[i]->tlv_key == d->key) im = g_tlv_images[i];
    if (!im) abort();
    uint8_t *mem = malloc(im->tlv_size ? im->tlv_size : 1);
    uint8_t *tmpl = (uint8_t *)(im->tlv_start + im->slide);
    memcpy(mem, tmpl, im->tlv_init_size);
    memset(mem + im->tlv_init_size, 0, im->tlv_size - im->tlv_init_size);
    pthread_setspecific(im->tlv_key, mem);
    return mem;
}

static int setup_tlv(TlImage *im, char *err, size_t errlen) {
    __block uint64_t lo = UINT64_MAX, hi = 0, init_hi = 0;
    __block TlvDesc *descs = NULL;
    __block uint64_t ndescs = 0;
    each_section(im, ^(const struct section_64 *sect, uint8_t *addr) {
        uint32_t type = sect->flags & SECTION_TYPE;
        if (type == S_THREAD_LOCAL_REGULAR || type == S_THREAD_LOCAL_ZEROFILL) {
            if (sect->addr < lo) lo = sect->addr;
            if (sect->addr + sect->size > hi) hi = sect->addr + sect->size;
            if (type == S_THREAD_LOCAL_REGULAR && sect->addr + sect->size > init_hi) init_hi = sect->addr + sect->size;
        } else if (type == S_THREAD_LOCAL_VARIABLES) {
            descs = (TlvDesc *)addr;
            ndescs = sect->size / sizeof(TlvDesc);
        }
    });
    if (!descs) return 0;
    if (lo == UINT64_MAX) lo = hi = init_hi = 0;
    im->tlv_start = lo, im->tlv_size = hi - lo, im->tlv_init_size = init_hi > lo ? init_hi - lo : 0;
    if (pthread_key_create(&im->tlv_key, free)) { set_err(err, errlen, "%s: no thread-local key", im->path); return -1; }
    if (g_ntlv == (int)(sizeof g_tlv_images / sizeof *g_tlv_images)) { set_err(err, errlen, "too many images with thread-local variables"); return -1; }
    g_tlv_images[g_ntlv++] = im;
    for (uint64_t i = 0; i < ndescs; i++) {
        descs[i].thunk = (void *)&tl_tlv_get_addr;
        descs[i].key = im->tlv_key;
    }
    return 0;
}

/* ---- unwinding ---- */

struct tl_unwind_sections {
    uintptr_t dso_base;
    uintptr_t dwarf_section;
    size_t dwarf_section_length;
    uintptr_t compact_unwind_section;
    size_t compact_unwind_section_length;
};

static int find_unwind(uintptr_t addr, struct tl_unwind_sections *info) {
    for (TlImage *im = g_images; im; im = im->next) {
        if (addr < (uintptr_t)im->base || addr >= (uintptr_t)im->base + im->vmsize) continue;
        uint64_t eh = 0, cu = 0;
        uint8_t *ehp = find_section(im, "__TEXT", "__eh_frame", &eh);
        uint8_t *cup = find_section(im, "__TEXT", "__unwind_info", &cu);
        info->dso_base = (uintptr_t)im->base;
        info->dwarf_section = (uintptr_t)ehp, info->dwarf_section_length = ehp ? eh : 0;
        info->compact_unwind_section = (uintptr_t)cup, info->compact_unwind_section_length = cup ? cu : 0;
        return 1;
    }
    return 0;
}

static void register_unwinder(void) {
    static int done;
    if (done) return;
    done = 1;
    int (*add)(int (*)(uintptr_t, struct tl_unwind_sections *)) = dlsym(RTLD_DEFAULT, "__unw_add_find_dynamic_unwind_sections");
    if (add) add(find_unwind);
    else tl_log("this host cannot unwind loaded code: exceptions through it will abort");
}

/* ---- runtime registration ---- */

static void register_swift(TlImage *im) {
    static void (*protos)(const void *, const void *), (*confs)(const void *, const void *), (*types)(const void *, const void *);
    uint64_t n;
    uint8_t *p;
    if (!protos) {
        protos = dlsym(RTLD_DEFAULT, "swift_registerProtocols");
        confs = dlsym(RTLD_DEFAULT, "swift_registerProtocolConformances");
        types = dlsym(RTLD_DEFAULT, "swift_registerTypeMetadataRecords");
    }
    int any = 0;
    if ((p = find_section(im, "__TEXT", "__swift5_protos", &n)) && n) { any = 1; if (protos) protos(p, p + n); }
    if ((p = find_section(im, "__TEXT", "__swift5_proto", &n)) && n) { any = 1; if (confs) confs(p, p + n); }
    if ((p = find_section(im, "__TEXT", "__swift5_types", &n)) && n) { any = 1; if (types) types(p, p + n); }
    if (any && !(protos && confs && types)) tl_log("%s uses Swift, but the Swift runtime is not loaded", im->path);
}

static int has_objc(TlImage *im) {
    return find_section(im, "__DATA", "__objc_imageinfo", NULL) || find_section(im, "__DATA_CONST", "__objc_imageinfo", NULL);
}

typedef struct ProgramVars {
    const void *mh;
    int *NXArgcPtr;
    const char ***NXArgvPtr;
    const char ***environPtr;
    const char **__prognamePtr;
} ProgramVars;

extern int *_NSGetArgc(void);
extern char ***_NSGetArgv(void);
extern char ***_NSGetEnviron(void);
extern char **_NSGetProgname(void);

static void run_initializers(TlImage *im) {
    if (im->initialized || im->visiting) return;
    im->visiting = 1;
    for (int i = 0; i < im->ndeps; i++)
        if (im->deps[i].tl) run_initializers(im->deps[i].tl);
    im->visiting = 0;
    im->initialized = 1;
    if (has_objc(im)) {
        void (*load)(const char *, const void *) = dlsym(RTLD_DEFAULT, "_objc_load_image");
        if (load) load(im->path, im->base);
    }
    ProgramVars vars = {im->base, _NSGetArgc(), (const char ***)_NSGetArgv(), (const char ***)_NSGetEnviron(),
                        (const char **)_NSGetProgname()};
    static const char *apple_storage[] = {NULL};
    const char **apple = apple_storage;
    each_section(im, ^(const struct section_64 *sect, uint8_t *addr) {
        uint32_t type = sect->flags & SECTION_TYPE;
        typedef void (*Init)(int, const char **, const char **, const char **, const ProgramVars *);
        if (type == S_MOD_INIT_FUNC_POINTERS) {
            Init *fns = (Init *)addr;
            for (uint64_t k = 0; k < sect->size / 8; k++)
                fns[k](*vars.NXArgcPtr, *vars.NXArgvPtr, *vars.environPtr, apple, &vars);
        } else if (type == S_INIT_FUNC_OFFSETS) {
            uint32_t *offs = (uint32_t *)addr;
            for (uint64_t k = 0; k < sect->size / 4; k++)
                ((Init)(im->base + offs[k]))(*vars.NXArgcPtr, *vars.NXArgvPtr, *vars.environPtr, apple, &vars);
        }
    });
}

/* ---- loading ---- */

static TlImage *g_pending[512];
static int g_npending;

static TlImage *load_one(const char *path, char *err, size_t errlen) {
    for (TlImage *im = g_images; im; im = im->next)
        if (!strcmp(im->path, path)) return im;
    TlImage *im = calloc(1, sizeof *im);
    im->path = strdup(path);
    im->dir = strdup(path);
    char *slash = strrchr(im->dir, '/');
    if (slash) *slash = 0;
    im->fd = open(path, O_RDONLY);
    struct stat st;
    if (im->fd < 0 || fstat(im->fd, &st)) { set_err(err, errlen, "cannot open %s: %s", path, strerror(errno)); return NULL; }
    im->maplen = (size_t)st.st_size;
    im->map = mmap(NULL, im->maplen, PROT_READ, MAP_PRIVATE, im->fd, 0);
    if (im->map == MAP_FAILED) { set_err(err, errlen, "cannot read %s", path); return NULL; }
    if (parse(im, err, errlen) || map_segments(im, err, errlen)) return NULL;
    if (!g_main && im->fmh->filetype == MH_EXECUTE) g_main = im;
    /* Listed before its libraries load, so a cycle finds it. */
    im->next = g_images;
    g_images = im;
    if (g_npending < (int)(sizeof g_pending / sizeof *g_pending)) g_pending[g_npending++] = im;
    if (g_host->verbose) tl_log("mapped %s at %p (%#llx bytes)", path, im->base, (unsigned long long)im->vmsize);
    if (load_deps(im, err, errlen)) return NULL;
    return im;
}

TlImage *tl_load(TlHost *host, const char *path, char *err, size_t errlen) {
    pthread_mutex_lock(&g_lock);
    g_host = host;
    g_npending = 0;
    char real[PATH_MAX];
    if (!realpath(path, real)) {
        set_err(err, errlen, "cannot find %s", path);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    TlImage *exe = load_one(real, err, errlen);
    if (!exe) { pthread_mutex_unlock(&g_lock); return NULL; }
    int n = g_npending;
    TlImage *batch[512];
    memcpy(batch, g_pending, n * sizeof *batch);
    for (int i = 0; i < n; i++)
        if (fixup(batch[i], err, errlen) || setup_tlv(batch[i], err, errlen)) { pthread_mutex_unlock(&g_lock); return NULL; }
    register_unwinder();
    for (int i = 0; i < n; i++) register_swift(batch[i]);
    const struct mach_header *mhs[512];
    const char *paths[512];
    int nobjc = 0;
    for (int i = n - 1; i >= 0; i--) /* libraries first, as dyld hands them over */
        if (has_objc(batch[i])) mhs[nobjc] = (const void *)batch[i]->base, paths[nobjc++] = batch[i]->path;
    if (nobjc) {
        void (*map)(unsigned, const char *const *, const struct mach_header *const *) = dlsym(RTLD_DEFAULT, "_objc_map_images");
        if (!map) { set_err(err, errlen, "this host's Objective-C runtime cannot take images (needs iOS 16 or macOS 13)"); pthread_mutex_unlock(&g_lock); return NULL; }
        map((unsigned)nobjc, paths, mhs);
    }
    pthread_mutex_unlock(&g_lock);
    run_initializers(exe);
    return exe;
}

int tl_run_main(TlImage *exe, int argc, const char **argv, const char **envp) {
    if (!exe->has_main) return -1;
    int (*entry)(int, const char **, const char **, const char **) = (void *)(exe->base + exe->entryoff);
    const char *apple[] = {NULL};
    return entry(argc, argv, envp, apple);
}

void *tl_image_base(const TlImage *im) { return im->base; }
size_t tl_image_size(const TlImage *im) { return im->vmsize; }
const char *tl_image_path(const TlImage *im) { return im->path; }
void *tl_image_symbol(TlImage *im, const char *sym) {
    int found = 0;
    uint64_t v = image_export(im, sym, &found);
    return found ? (void *)v : NULL;
}

/* ---- install names on iOS ---- */

static const char *const g_mac_only[] = {
    "AppKit", "Cocoa", "Carbon", "ApplicationServices", "OpenGL", "AGL", "AppleScriptObjC", "Quartz", "QuickLookUI",
    "ScreenSaver", "InputMethodKit", "PreferencePanes", "ServiceManagement", "OSAKit", "Automator", "DiskArbitration",
    "SecurityInterface", "ExceptionHandling", "IOBluetoothUI", "InstallerPlugins", "SyncServices", "DirectoryService",
};

const char *tl_ios_map_install_name(const char *name, char *buf, size_t len) {
    if (!strncmp(name, "/System/iOSSupport/", 19)) name += 18; /* keep the leading '/' of /System/Library/... */
    const char *fw = strstr(name, "/System/Library/Frameworks/");
    if (fw == name || !strncmp(name, "/System/Library/PrivateFrameworks/", 34)) {
        /* .../X.framework/Versions/A/X -> .../X.framework/X */
        const char *dot = strstr(name, ".framework/");
        if (!dot) return name;
        const char *start = name;
        for (const char *p = dot; p > name; p--)
            if (p[-1] == '/') { start = p; break; }
        size_t stem = (size_t)(dot - start);
        for (size_t i = 0; i < sizeof g_mac_only / sizeof *g_mac_only; i++)
            if (strlen(g_mac_only[i]) == stem && !strncmp(start, g_mac_only[i], stem)) return NULL;
        const char *leaf = strrchr(name, '/') + 1;
        if (snprintf(buf, len, "%.*s.framework/%s", (int)(dot - name), name, leaf) >= (int)len) return NULL;
        return buf;
    }
    return name; /* /usr/lib/... is where it is on iOS too */
}

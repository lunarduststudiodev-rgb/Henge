#include "codestore.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mmu.h"
#include "translate.h"

/* ---- The file ----
   A header (CsHeader, in its own CS_ALIGN bytes), then each zone's parts (its code words, its block records, its
   slots, its FP/SIMD word records), then the code pages' records. Everything at offsets the header gives, each part
   with its CRC32C. Records are this build's own (a store is only ever loaded by the build that saved it). */

#define CS_MAGIC "HVCODE01"
#define CS_VERSION 3 /* 3: CsBlock keeps the gate offset and HvBlock.words (code-writes) */
#define CS_ALIGN 16384u
#ifndef CS_CHUNK /* tests/test_code_store.c is built with a small one, to go through many batches */
#define CS_CHUNK (4u << 20) /* read or written at a time: a full cache's records are never all in memory at once */
#endif
#define CS_THREADS 4
#define CS_NONE 0xffffffffu
#define CS_MARGIN (1ull << 30) /* storage left free after a save, at least */

typedef struct {
    uint64_t off, len;
    uint32_t crc, pad;
} CsPart;

typedef struct {
    uint64_t used, score, filled; /* HvJitZone's */
    uint32_t nblocks, nslots, nundef, pad;
    CsPart code, blocks, slots, undef;
} CsZone;

typedef struct {
    uint64_t pa, size, host;
} CsRam;

typedef struct {
    char magic[8];
    uint32_t version, header_bytes;
    uint8_t key[16], build[16];
    uint64_t config;                  /* hv_translate_config */
    uint64_t capacity, zone_words;    /* the code region, in words */
    uint64_t base, slots;             /* where the region and the slot table were */
    uint32_t nzones, zone, zone_blocks, zone_slots, zone_undef, max_blocks, max_slots, max_undef;
    uint32_t jit_gen, nram, npages, page_bits;
    uint32_t page_crcs, pad;          /* CsPage.crc set (saved with HV_CODE_STORE_VERIFY=1) */
    uint64_t zone_switches;
    CsRam ram[HV_MAX_RAM];
    CsZone zones[HV_JIT_ZONES];
    CsPart pages;
    uint32_t crc; /* of everything before it */
} CsHeader;

typedef struct {
    uint64_t va, pa, code;            /* code: word offset in the region */
    uint32_t pg, gate;                /* gate: HvBlock.gate */
    uint32_t flags, ninsns, nundef, undef_at;
    uint32_t page;                    /* its code page's record; CS_NONE: not kept (stale, or holds host addresses) */
    uint32_t ram;
} CsBlock;

typedef struct {
    uint32_t site, stub, cond, pad;   /* word offsets in the region, + 1 (0: none) */
    uint64_t xa;                      /* HvMachine.slot_xa */
} CsSlot;

typedef struct {
    uint64_t pg;
    uint32_t ram, crc;                /* crc: the page's guest bytes */
} CsPage;

/* tests/test_code_store.c damages a record where only its CRC finds it, at an offset worked out from these. */
_Static_assert(sizeof(CsHeader) <= CS_ALIGN && CS_ALIGN == 16384, "the first zone's code at 16 KB");
_Static_assert(sizeof(CsBlock) == 56 && sizeof(CsSlot) == 24 && offsetof(CsSlot, pad) == 12, "the test's record layout");
_Static_assert(CS_CHUNK % 4 == 0 && CS_CHUNK >= sizeof(CsBlock), "a batch holds whole code words and records");

/* What the translator writes in an exit's stub (put_branch_exit), and before its B for an exit to another page. */
#define STUB_STR_X30 (0xf9000000u | (uint32_t)(HV_CPU_X(30) / 8) << 10 | 28u << 5 | 30u)
#define STUB_LDR_THUNK (0xf9400000u | (uint32_t)(HV_CPU_CHAIN_THUNK / 8) << 10 | 28u << 5 | 15u)
#define STUB_NOP 0xd503201fu
#define STUB_BLR_X15 0xd63f01e0u
#define X_EOR_KEY (0xca000000u | 15u << 16 | 7u << 5 | 7u) /* eor x7, x7, x15 */
#define B_OP 0x14000000u

static uint64_t page_bytes(void) { return 1ull << HV_TLB_PAGE_BITS; } /* an exit's key names a TLB page */
static uint64_t align_up(uint64_t v) { return (v + CS_ALIGN - 1) & ~(uint64_t)(CS_ALIGN - 1); }
static double ms_since(uint64_t t0) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}

static int env_off(void) {
    const char *v = getenv("HV_CODE_STORE");
    return v && v[0] == '0';
}

/* HV_CODE_STORE_VERIFY=1: the guest's code pages checked against the ones saved (a check of the store itself, for
   testing: it reads every page the code came from, at the Suspend and at the resume). */
static int env_verify(void) {
    const char *v = getenv("HV_CODE_STORE_VERIFY");
    return v && v[0] == '1';
}

/* This build of Henge: the UUID the linker gave the image the engine is in. */
static int build_id(uint8_t out[16]) {
    Dl_info di;
    memset(out, 0, 16);
    if (!dladdr((const void *)(uintptr_t)hv_code_store_save, &di) || !di.dli_fbase) return -1;
    const struct mach_header_64 *h = di.dli_fbase;
    if (h->magic != MH_MAGIC_64) return -1;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *c = (const void *)lc;
        if (c->cmd == LC_UUID) {
            memcpy(out, ((const struct uuid_command *)c)->uuid, 16);
            return 0;
        }
        lc += c->cmdsize;
    }
    return -1;
}

static char *side_path(const char *snap, const char *suffix) {
    size_t n = strlen(snap) + strlen(suffix) + 1;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s%s", snap, suffix);
    return p;
}

/* fn(ctx, i) for every i below n, on up to CS_THREADS threads (this one too). */
typedef struct {
    void (*fn)(void *, int);
    void *ctx;
    int n, next;
} Par;

static void *par_main(void *a) {
    Par *p = a;
    for (;;) {
        int i = __atomic_fetch_add(&p->next, 1, __ATOMIC_RELAXED);
        if (i >= p->n) return NULL;
        p->fn(p->ctx, i);
    }
}

static void par_run(void (*fn)(void *, int), void *ctx, int n) {
    Par p = {fn, ctx, n, 0};
    pthread_t t[CS_THREADS];
    int k = 0;
    while (k < CS_THREADS - 1 && k < n - 1 && pthread_create(&t[k], NULL, par_main, &p) == 0) k++;
    par_main(&p);
    for (int i = 0; i < k; i++) pthread_join(t[i], NULL);
}

void hv_code_key_new(HvCodeKey *k) { arc4random_buf(k->id, sizeof k->id); }

int hv_code_key_put(HvSnapWriter *w, const HvCodeKey *k) {
    HvSnapOut o = {0};
    hv_snap_out(&o, k->id, sizeof k->id);
    return hv_snap_put(w, HV_SNAP_CODE_KEY, 1, &o);
}

int hv_code_key_get(HvSnapReader *r, HvCodeKey *k) {
    HvSnapIn in;
    if (!hv_snap_has(r, HV_SNAP_CODE_KEY) || hv_snap_get(r, HV_SNAP_CODE_KEY, 1, &in) != 0) return -1; /* none kept */
    hv_snap_in(&in, k->id, sizeof k->id);
    return hv_snap_in_done(&in) == 0 ? 0 : -1;
}

void hv_code_store_remove(const char *snap_path) {
    char *p = side_path(snap_path, ".code"), *q = side_path(snap_path, ".code.partial");
    if (p) unlink(p);
    if (q) unlink(q);
    free(p);
    free(q);
}

static uint32_t header_crc(const CsHeader *h) { return hv_crc32c(0, h, offsetof(CsHeader, crc)); }

static uint64_t ram_pages(const HvRam *r) { return (r->size + (1ull << HV_RAM_PAGE_BITS) - 1) >> HV_RAM_PAGE_BITS; }

static uint32_t page_crc(const HvRam *r, uint64_t pg) {
    uint64_t at = pg << HV_RAM_PAGE_BITS, n = r->size - at < (1ull << HV_RAM_PAGE_BITS) ? r->size - at : 1ull << HV_RAM_PAGE_BITS;
    return hv_crc32c(0, r->host + at, n);
}

/* ---- Saving ---- */

typedef struct {
    HvMachine *m;
    CsHeader *h;
    int fd;
    const uint64_t *cancel;
    uint64_t ticket;
    uint32_t *page_of[HV_MAX_RAM]; /* per RAM page: its CsPage + 1, or 0 */
    CsPage *pages;
    int failed;
} SaveJob;

static int save_stop(SaveJob *j) {
    return __atomic_load_n(&j->failed, __ATOMIC_RELAXED) ||
           (j->cancel && __atomic_load_n(j->cancel, __ATOMIC_ACQUIRE) != j->ticket);
}

/* A part written in pieces, its CRC as it goes. */
typedef struct {
    CsPart *p;
    uint64_t at;
    uint32_t crc;
} PartOut;

static int out_put(SaveJob *j, PartOut *o, const void *data, size_t len) {
    const uint8_t *src = data;
    for (size_t at = 0; at < len;) {
        if (save_stop(j)) return -1;
        size_t n = len - at < CS_CHUNK ? len - at : CS_CHUNK;
        o->crc = hv_crc32c(o->crc, src + at, n);
        for (size_t done = 0; done < n;) {
            ssize_t wr = pwrite(j->fd, src + at + done, n - done, (off_t)(o->p->off + o->at + at + done));
            if (wr < 0 && errno == EINTR) continue;
            if (wr <= 0) {
                fprintf(stderr, "hengevm: code store: can't write: %s\n", wr < 0 ? strerror(errno) : "nothing written");
                __atomic_store_n(&j->failed, 1, __ATOMIC_RELAXED);
                return -1;
            }
            done += (size_t)wr;
        }
        at += n;
    }
    o->at += len;
    o->p->len = o->at;
    o->p->crc = o->crc;
    return 0;
}

static int put_part(SaveJob *j, CsPart *p, const void *data, size_t len) {
    PartOut o = {p, 0, 0};
    return out_put(j, &o, data, len);
}

/* Whether block b of zone z is kept: live, its page not written since, nothing of this run's in its code. */
static int block_kept(const HvMachine *m, const HvBlock *b, int z) {
    const HvJitZone *zn = &m->zones[z];
    if (b->zone != z || b->jit_gen != zn->born || b->nosave || b->dead || !b->code) return 0;
    if (m->zones_dying >> z & 1) return 0; /* being emptied (the reclaimer), or emptied: nothing of it is kept */
    size_t w = (size_t)(b->code - m->jit.base), c0 = (size_t)z * m->zone_words;
    if (b->code < m->jit.base || w < c0 || w >= c0 + zn->used) return 0;
    const HvRam *r = b->ram;
    if (!r || r < m->ram || r >= m->ram + m->nram || b->pg >= ram_pages(r)) return 0;
    if ((b->pa - r->pa) >> HV_RAM_PAGE_BITS != b->pg) return 0;
    if (__atomic_load_n(&r->page_gen[b->pg], __ATOMIC_ACQUIRE) != b->gen) return 0;
    if (b->nundef && b->nundef != HV_UNDEF_UNKNOWN &&
        (b->undef_at < (uint32_t)z * m->zone_undef || b->undef_at + b->nundef > (uint32_t)z * m->zone_undef + zn->nundef))
        return 0;
    return 1;
}

static void save_pages(void *ctx, int i) {
    SaveJob *j = ctx;
    uint32_t per = (j->h->npages + 63) / 64;
    for (uint32_t k = (uint32_t)i * per; k < j->h->npages && k < (uint32_t)(i + 1) * per; k++)
        j->pages[k].crc = page_crc(&j->m->ram[j->pages[k].ram], j->pages[k].pg);
}

/* A zone's parts. Its code and FP/SIMD records go straight from the machine; its block and exit records are made a
   batch at a time (CS_CHUNK), so a full cache's never need memory of their own. */
static void save_zone(void *ctx, int z) {
    SaveJob *j = ctx;
    HvMachine *m = j->m;
    CsZone *cz = &j->h->zones[z];
    if (save_stop(j)) return;
    uint64_t c0 = (uint64_t)z * m->zone_words, c1 = c0 + cz->used;
    uint8_t *buf = malloc(CS_CHUNK);
    if (!buf) goto fail;
    CsBlock *blocks = (CsBlock *)buf;
    const uint32_t per_block = CS_CHUNK / sizeof *blocks;
    const HvBlock *b0 = m->blocks + (size_t)z * m->zone_blocks;
    PartOut ob = {&cz->blocks, 0, 0};
    for (uint32_t i0 = 0; i0 < cz->nblocks; i0 += per_block) {
        uint32_t n = cz->nblocks - i0 < per_block ? cz->nblocks - i0 : per_block;
        for (uint32_t i = 0; i < n; i++) {
            const HvBlock *b = &b0[i0 + i];
            CsBlock *o = &blocks[i];
            memset(o, 0, sizeof *o);
            o->va = b->va, o->pa = b->pa, o->pg = b->pg, o->gate = b->gate, o->flags = b->flags, o->ninsns = b->ninsns | (uint32_t)b->words << 16;
            o->nundef = b->nundef, o->undef_at = b->undef_at;
            o->code = b->code ? (uint64_t)(b->code - m->jit.base) : 0;
            o->ram = b->ram && b->ram >= m->ram && b->ram < m->ram + m->nram ? (uint32_t)(b->ram - m->ram) : CS_NONE;
            o->page = block_kept(m, b, z) ? j->page_of[o->ram][b->pg] - 1 : CS_NONE;
        }
        if (out_put(j, &ob, blocks, (size_t)n * sizeof *blocks) != 0) goto fail;
    }
    CsSlot *slots = (CsSlot *)buf;
    const uint32_t per_slot = CS_CHUNK / sizeof *slots;
    uint32_t s0 = (uint32_t)z * m->zone_slots;
    PartOut os = {&cz->slots, 0, 0};
    for (uint32_t i0 = 0; i0 < cz->nslots; i0 += per_slot) {
        uint32_t n = cz->nslots - i0 < per_slot ? cz->nslots - i0 : per_slot;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t s = s0 + i0 + i;
            CsSlot *o = &slots[i];
            memset(o, 0, sizeof *o);
            o->xa = m->slot_xa[s];
            /* An exit not placed (its block's translation failed after taking it) keeps the stub of an older use of
               the zone (translate.c resets only its site and branch): nothing leads to it, so none is saved. */
            uint32_t *p[3] = {m->slot_site[s], m->slot_site[s] ? m->slot_stub[s] : NULL, m->slot_site[s] ? m->slot_cond[s] : NULL};
            uint32_t *w[3] = {&o->site, &o->stub, &o->cond};
            for (int k = 0; k < 3; k++) {
                if (!p[k]) continue;
                uint64_t off = (uint64_t)(p[k] - m->jit.base);
                if (p[k] < m->jit.base || off < c0 || off >= c1) { /* not in its zone's code: no store is kept */
                    fprintf(stderr, "hengevm: code store: zone %d's slot %u is outside it\n", z, s);
                    goto fail;
                }
                *w[k] = (uint32_t)(off + 1);
            }
            if (o->site && !o->stub) goto fail;
        }
        if (out_put(j, &os, slots, (size_t)n * sizeof *slots) != 0) goto fail;
    }
    if (put_part(j, &cz->code, m->jit.base + c0, cz->used * 4) != 0 ||
        put_part(j, &cz->undef, m->undef_map + (size_t)z * m->zone_undef, (size_t)cz->nundef * 4) != 0)
        goto fail;
    free(buf);
    return;
fail:
    free(buf);
    __atomic_store_n(&j->failed, 1, __ATOMIC_RELAXED);
}

int hv_code_store_save(HvMachine *m, const char *snap_path, const HvCodeKey *k, const uint64_t *cancel,
                       uint64_t ticket) {
    if (env_off() || !m->blocks || m->nzones < 1) return -1;
    uint64_t t0 = mach_absolute_time();
    char *path = side_path(snap_path, ".code"), *partial = side_path(snap_path, ".code.partial");
    CsHeader *h = calloc(1, sizeof *h);
    SaveJob j = {.m = m, .h = h, .fd = -1, .cancel = cancel, .ticket = ticket};
    int rc = -1, locked = 0;
    uint64_t blocks_kept = 0;
    if (!path || !partial || !h) goto out;
    if (build_id(h->build) != 0) {
        fprintf(stderr, "hengevm: code store: this build has no UUID: not saved\n");
        goto out;
    }
    /* Held to the end: the code is written straight from the cache, which nothing may change meanwhile. Its CPUs are
       paused for the Suspend anyway; were it called off, the save stops within a CS_CHUNK and lets go. */
    pthread_mutex_lock(&m->tr_mu);
    locked = 1;
    if (__atomic_load_n(&m->flush_pending, __ATOMIC_SEQ_CST)) { /* the cache was about to change anyway */
        fprintf(stderr, "hengevm: code store: the code cache was making room: not saved\n");
        goto out;
    }
    hv_jit_zones_sync(m);
    memcpy(h->magic, CS_MAGIC, sizeof h->magic);
    h->version = CS_VERSION;
    h->header_bytes = sizeof *h;
    memcpy(h->key, k->id, sizeof h->key);
    h->config = hv_translate_config(m);
    h->capacity = m->jit.capacity, h->zone_words = m->zone_words;
    h->base = (uint64_t)(uintptr_t)m->jit.base, h->slots = (uint64_t)(uintptr_t)m->slots;
    h->nzones = (uint32_t)m->nzones, h->zone = (uint32_t)m->zone;
    h->zone_blocks = m->zone_blocks, h->zone_slots = m->zone_slots, h->zone_undef = m->zone_undef;
    h->max_blocks = m->max_blocks, h->max_slots = m->max_slots, h->max_undef = m->max_undef;
    h->jit_gen = m->jit_gen, h->nram = (uint32_t)m->nram, h->page_bits = HV_RAM_PAGE_BITS;
    h->zone_switches = m->zone_switches;
    for (int i = 0; i < m->nram; i++) {
        h->ram[i] = (CsRam){m->ram[i].pa, m->ram[i].size, (uint64_t)(uintptr_t)m->ram[i].host};
        if (!(j.page_of[i] = calloc(ram_pages(&m->ram[i]), sizeof *j.page_of[i]))) goto out;
    }
    /* The pages kept blocks are in (each once), and where everything goes in the file. */
    for (int z = 0; z < m->nzones; z++) {
        const HvBlock *b0 = m->blocks + (size_t)z * m->zone_blocks;
        for (uint32_t i = 0; i < m->zones[z].nblocks; i++)
            if (block_kept(m, &b0[i], z)) {
                uint32_t *p = &j.page_of[b0[i].ram - m->ram][b0[i].pg];
                if (!*p) *p = ++h->npages;
                blocks_kept++;
            }
    }
    if (!(j.pages = calloc(h->npages ? h->npages : 1, sizeof *j.pages))) goto out;
    for (int i = 0; i < m->nram; i++)
        for (uint64_t pg = 0; pg < ram_pages(&m->ram[i]); pg++)
            if (j.page_of[i][pg]) j.pages[j.page_of[i][pg] - 1] = (CsPage){pg, (uint32_t)i, 0};
    uint64_t off = align_up(sizeof *h);
    for (int z = 0; z < m->nzones; z++) {
        const HvJitZone *zn = &m->zones[z];
        CsZone *cz = &h->zones[z];
        cz->score = zn->score, cz->filled = zn->filled;
        /* A zone the reclaimer is emptying, or has emptied for the next fill, is saved empty: its blocks may be half
           taken off (and none is kept: block_kept). Loading resets every exit to its stub, so no other zone's exit
           saved chained into it leads anywhere. */
        if (!(m->zones_dying >> z & 1)) cz->used = zn->used, cz->nblocks = zn->nblocks, cz->nslots = zn->nslots, cz->nundef = zn->nundef;
        cz->code.off = off, off = align_up(off + cz->used * 4);
        cz->blocks.off = off, off = align_up(off + (uint64_t)cz->nblocks * sizeof(CsBlock));
        cz->slots.off = off, off = align_up(off + (uint64_t)cz->nslots * sizeof(CsSlot));
        cz->undef.off = off, off = align_up(off + (uint64_t)cz->nundef * 4);
    }
    h->pages.off = off, off = align_up(off + (uint64_t)h->npages * sizeof(CsPage));
    /* Room for it, and some to spare: a store is never worth a full iPad. */
    char *dir = strdup(snap_path), *slash = dir ? strrchr(dir, '/') : NULL;
    if (slash) *slash = 0;
    struct statfs fs;
    int room = !dir || statfs(slash ? dir : ".", &fs) != 0 || (uint64_t)fs.f_bavail * fs.f_bsize > off + CS_MARGIN;
    free(dir);
    if (!room) {
        fprintf(stderr, "hengevm: code store: not enough free storage for %.0f MB: not saved\n", off / 1048576.0);
        goto out;
    }
    unlink(partial);
    j.fd = open(partial, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (j.fd < 0) {
        fprintf(stderr, "hengevm: code store: can't create %s: %s\n", partial, strerror(errno));
        goto out;
    }
    fcntl(j.fd, F_NOCACHE, 1); /* written once, read once at the resume */
    if ((h->page_crcs = (uint32_t)env_verify())) par_run(save_pages, &j, 64);
    par_run(save_zone, &j, m->nzones);
    if (save_stop(&j) || put_part(&j, &h->pages, j.pages, (size_t)h->npages * sizeof *j.pages) != 0) goto out;
    h->crc = header_crc(h);
    CsPart hp = {0, 0, 0, 0};
    if (put_part(&j, &hp, h, sizeof *h) != 0) goto out;
    if (close(j.fd) != 0) {
        j.fd = -1;
        goto out;
    }
    j.fd = -1;
    /* No flush to storage waited for: one cut short by a crash fails its checks, and is ignored. */
    if (rename(partial, path) != 0) {
        fprintf(stderr, "hengevm: code store: can't rename into place: %s\n", strerror(errno));
        goto out;
    }
    rc = 0;
    fprintf(stderr, "hengevm: code store: saved %llu blocks in %u zones, %.0f MB, in %.0f ms\n",
            (unsigned long long)blocks_kept, h->nzones, off / 1048576.0, ms_since(t0));
out:
    if (locked) pthread_mutex_unlock(&m->tr_mu);
    if (j.fd >= 0) close(j.fd);
    if (rc != 0 && partial) {
        unlink(partial);
        if (j.cancel && j.ticket != __atomic_load_n(j.cancel, __ATOMIC_ACQUIRE))
            fprintf(stderr, "hengevm: code store: cancelled, nothing kept\n");
    }
    for (int i = 0; i < HV_MAX_RAM; i++) free(j.page_of[i]);
    free(j.pages);
    free(h);
    free(path);
    free(partial);
    return rc;
}

/* ---- Loading ---- */

typedef struct {
    HvMachine *m;
    const CsHeader *h;
    int fd;
    CsPage *pages;
    uint8_t *page_ok;
    uint32_t new_gen;
    uint8_t ok[HV_JIT_ZONES];        /* the zone loaded */
    uint8_t written[HV_JIT_ZONES];   /* not loaded, but its tables were written (and cleared): to give back */
    uint32_t stale[HV_JIT_ZONES];    /* its blocks from pages that had changed (HV_CODE_STORE_VERIFY=1) */
    const char *why[HV_JIT_ZONES];
} LoadJob;

static int read_all(int fd, void *dst, size_t len, uint64_t off) {
    for (size_t done = 0; done < len;) {
        ssize_t n = pread(fd, (uint8_t *)dst + done, len - done, (off_t)(off + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

/* A small part read whole into memory and checked. */
static void *get_part(int fd, const CsPart *p, size_t expect) {
    if (p->len != expect) return NULL;
    void *buf = malloc(expect ? expect : 1);
    if (!buf) return NULL;
    if (read_all(fd, buf, expect, p->off) != 0 || hv_crc32c(0, buf, expect) != p->crc) {
        free(buf);
        return NULL;
    }
    return buf;
}

static void load_pages(void *ctx, int i) {
    LoadJob *j = ctx;
    uint32_t per = (j->h->npages + 63) / 64;
    for (uint32_t k = (uint32_t)i * per; k < j->h->npages && k < (uint32_t)(i + 1) * per; k++)
        j->page_ok[k] = page_crc(&j->m->ram[j->pages[k].ram], j->pages[k].pg) == j->pages[k].crc;
}

static void jit_put(HvMachine *m, uint32_t *at, const void *v, size_t bytes) { hv_jit_write(&m->jit, at, v, bytes); }

/* A zone, read a CS_CHUNK at a time straight into the machine's tables and code region (its own share of each:
   zones load side by side), each record checked as it comes and each part's CRC at its end. Its blocks are left
   for hv_code_store_load to make findable: a kept one with jit_gen set, any other dead (jit_gen 0). A zone that
   fails any check is cleared again, as it was. */
static void load_zone(void *ctx, int z) {
    LoadJob *j = ctx;
    HvMachine *m = j->m;
    const CsZone *cz = &j->h->zones[z];
    if (!cz->nblocks) return; /* nothing in it to load */
    uint64_t c0 = (uint64_t)z * m->zone_words, c1 = c0 + cz->used;
    uint32_t *base = m->jit.base, s0 = (uint32_t)z * m->zone_slots;
    HvBlock *b0 = m->blocks + (size_t)z * m->zone_blocks;
    uint32_t *undef_to = m->undef_map + (size_t)z * m->zone_undef;
    uint8_t *buf = malloc(CS_CHUNK);
    int wrote = 0, code_wrote = 0;
    uint32_t crc;
#define FAIL(s) do { j->why[z] = (s); goto fail; } while (0)
    if (!buf) FAIL("out of memory");
    if (cz->blocks.len != (uint64_t)cz->nblocks * sizeof(CsBlock) || cz->slots.len != (uint64_t)cz->nslots * sizeof(CsSlot) ||
        cz->undef.len != (uint64_t)cz->nundef * 4 || cz->code.len != cz->used * 4)
        FAIL("its records are damaged");
    wrote = 1;
    /* Its blocks. */
    CsBlock *rec = (CsBlock *)buf;
    const uint32_t per_block = CS_CHUNK / sizeof *rec;
    crc = 0;
    for (uint32_t i0 = 0; i0 < cz->nblocks; i0 += per_block) {
        uint32_t n = cz->nblocks - i0 < per_block ? cz->nblocks - i0 : per_block;
        if (read_all(j->fd, rec, (size_t)n * sizeof *rec, cz->blocks.off + (uint64_t)i0 * sizeof *rec) != 0)
            FAIL("its block records can't be read");
        crc = hv_crc32c(crc, rec, (size_t)n * sizeof *rec);
        for (uint32_t i = 0; i < n; i++) {
            const CsBlock *o = &rec[i];
            HvBlock *b = &b0[i0 + i];
            memset(b, 0, sizeof *b);
            b->va = o->va, b->pa = o->pa, b->flags = o->flags, b->ninsns = (uint8_t)o->ninsns, b->words = (uint16_t)(o->ninsns >> 16);
            b->nundef = o->nundef, b->undef_at = o->undef_at, b->pg = o->pg, b->gate = o->gate;
            b->code = o->code - c0 < cz->used ? base + o->code : NULL;
            b->ram = o->ram < (uint32_t)m->nram ? &m->ram[o->ram] : NULL;
            b->zone = (uint8_t)z;
            b->gen = 0xffffffffu; /* no lookup finds it until hv_code_store_load says (its page's generation) */
            b->dead = 1;          /* until then, and for good if it isn't kept */
            if (o->page == CS_NONE) continue; /* dead: jit_gen 0, never its zone's */
            if (o->page >= j->h->npages || o->code < c0 || o->code >= c1 || o->ram >= (uint32_t)m->nram ||
                j->pages[o->page].ram != o->ram || j->pages[o->page].pg != o->pg ||
                (o->nundef && o->nundef != HV_UNDEF_UNKNOWN &&
                 (o->undef_at < (uint32_t)z * m->zone_undef ||
                  o->undef_at + o->nundef > (uint32_t)z * m->zone_undef + cz->nundef)))
                FAIL("a block record is out of place");
            if (!j->page_ok[o->page]) {
                j->stale[z]++;
                continue;
            }
            b->jit_gen = j->new_gen;
        }
    }
    if (crc != cz->blocks.crc) FAIL("its block records are damaged");
    /* The code, where it was in the region. */
    code_wrote = 1;
    crc = 0;
    for (uint64_t at = 0; at < cz->code.len; at += CS_CHUNK) {
        size_t n = cz->code.len - at < CS_CHUNK ? (size_t)(cz->code.len - at) : CS_CHUNK;
        if (read_all(j->fd, buf, n, cz->code.off + at) != 0) FAIL("its code can't be read");
        crc = hv_crc32c(crc, buf, n);
        jit_put(m, base + c0 + at / 4, buf, n);
    }
    if (crc != cz->code.crc) FAIL("its code is damaged");
    /* Every exit unchained, its stub's slot and its TLB key for this run, each checked first: an exit not as the
       translator writes it drops the zone. (Each write lands in this zone's own code, at a place checked to be in
       it: a damaged record found by the CRC only at the end leaves nothing outside the zone to undo.) */
    CsSlot *srec = (CsSlot *)buf;
    const uint32_t per_slot = CS_CHUNK / sizeof *srec;
    crc = 0;
    for (uint32_t i0 = 0; i0 < cz->nslots; i0 += per_slot) {
        uint32_t n = cz->nslots - i0 < per_slot ? cz->nslots - i0 : per_slot;
        if (read_all(j->fd, srec, (size_t)n * sizeof *srec, cz->slots.off + (uint64_t)i0 * sizeof *srec) != 0)
            FAIL("its exits can't be read");
        crc = hv_crc32c(crc, srec, (size_t)n * sizeof *srec);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t s = s0 + i0 + i;
            const CsSlot *o = &srec[i];
            const uint32_t *w[3] = {&o->site, &o->stub, &o->cond};
            for (int k = 0; k < 3; k++)
                if (*w[k] && (*w[k] - 1 < c0 || *w[k] - 1 >= c1)) FAIL("an exit is out of place");
            /* The stub's longest form (STR, LDR, NOP, BLR) is in the zone; its slot pointer is checked below. */
            if (o->site && (!o->stub || o->stub - 1 + 4 > c1)) FAIL("an exit has no stub");
            m->slots[s] = 0, m->slot_xpg[s] = m->slot_xprev[s] = m->slot_xnext[s] = 0;
            m->slot_site[s] = o->site ? base + o->site - 1 : NULL;
            m->slot_stub[s] = o->stub ? base + o->stub - 1 : NULL;
            m->slot_cond[s] = o->cond ? base + o->cond - 1 : NULL;
            m->slot_xa[s] = o->xa;
            uint32_t *site = m->slot_site[s], *stub = m->slot_stub[s], *cond = m->slot_cond[s];
            if (!site) continue;
            if ((*site & 0xfc000000u) != B_OP || stub[0] != STUB_STR_X30 || stub[1] != STUB_LDR_THUNK)
                FAIL("an exit isn't as expected");
            uint32_t *data = stub[2] == STUB_BLR_X15 ? stub + 3 : stub[2] == STUB_NOP && stub[3] == STUB_BLR_X15 ? stub + 4 : NULL;
            if (!data || ((uintptr_t)data & 7) || (uint64_t)(data + 4 - base) > c1) FAIL("an exit's stub isn't as expected");
            uint64_t old_slot = (uint64_t)data[0] | (uint64_t)data[1] << 32, target = (uint64_t)data[2] | (uint64_t)data[3] << 32;
            if (old_slot != j->h->slots + (uint64_t)s * 8) FAIL("an exit's stub names another slot");
            uint64_t new_slot = (uint64_t)(uintptr_t)&m->slots[s];
            uint32_t ptr[2] = {(uint32_t)new_slot, (uint32_t)(new_slot >> 32)};
            jit_put(m, data, ptr, 8);
            int64_t back = stub - site;
            if (back < -(1 << 25) || back >= (1 << 25)) FAIL("an exit's stub is out of reach");
            uint32_t insn = B_OP | (uint32_t)(back & 0x3ffffff);
            jit_put(m, site, &insn, 4);
            if (cond) {
                insn = *cond;
                if (hv_branch_retarget(&insn, site - cond)) jit_put(m, cond, &insn, 4);
                else FAIL("an exit's conditional branch is out of reach");
            }
            if (!o->xa) continue;
            /* To another page: the RAM it was in, now here; the key before the B, made with the new address. */
            uint64_t page = target & ~(page_bytes() - 1);
            if (site - base < (ptrdiff_t)c0 + 3 || (site[-1] & 0xff00001fu) != 0xb5000007u || site[-2] != X_EOR_KEY ||
                (site[-3] & 0xff00001fu) != 0x5800000fu)
                FAIL("an exit to another page isn't as expected");
            int32_t imm = (int32_t)(site[-3] << 8) >> 13; /* imm19, in words */
            uint32_t *lit = site - 3 + imm;
            if (lit - base < (ptrdiff_t)c0 || (uint64_t)(lit + 2 - base) > c1 || ((uintptr_t)lit & 7))
                FAIL("an exit's key is out of place");
            uint64_t key = (uint64_t)lit[0] | (uint64_t)lit[1] << 32;
            if (key != hv_exec_key(page, (int64_t)o->xa)) {
                /* A 4 KB-page process's block (HV_BLOCK_SMALL, an Intel app) keys its exits by the target's 4 KB page. */
                uint64_t small = target & ~((1ull << HV_TLB_SMALL_BITS) - 1);
                if (!HV_TLB_SMALL || key != hv_exec_key(small, (int64_t)o->xa)) FAIL("an exit's key isn't as expected");
                page = small;
            }
            uint64_t host = o->xa + page;
            int r = -1;
            for (uint32_t i2 = 0; i2 < j->h->nram && r < 0; i2++)
                if (host - j->h->ram[i2].host < j->h->ram[i2].size) r = (int)i2;
            if (r < 0) FAIL("an exit to another page is to no RAM"); /* the translator only makes them into RAM */
            uint64_t xa = o->xa + ((uint64_t)(uintptr_t)m->ram[r].host - j->h->ram[r].host);
            m->slot_xa[s] = xa;
            key = hv_exec_key(page, (int64_t)xa);
            uint32_t kw[2] = {(uint32_t)key, (uint32_t)(key >> 32)};
            jit_put(m, lit, kw, 8);
        }
    }
    if (crc != cz->slots.crc) FAIL("its exits are damaged");
    /* Its FP/SIMD word records. */
    if (read_all(j->fd, undef_to, (size_t)cz->nundef * 4, cz->undef.off) != 0) FAIL("its FP/SIMD records can't be read");
    if (hv_crc32c(0, undef_to, (size_t)cz->nundef * 4) != cz->undef.crc) FAIL("its FP/SIMD records are damaged");
    hv_jit_finish(&m->jit, base + c0, cz->used);
    free(buf);
    j->ok[z] = 1;
    return;
fail:
    if (wrote) { /* the zone's share of each table as it was (empty), and nothing leads into its code */
        memset(b0, 0, (size_t)cz->nblocks * sizeof *b0);
        for (uint32_t s = s0; s < s0 + cz->nslots; s++) {
            m->slots[s] = 0, m->slot_xpg[s] = m->slot_xprev[s] = m->slot_xnext[s] = 0, m->slot_xa[s] = 0;
            m->slot_site[s] = m->slot_stub[s] = m->slot_cond[s] = NULL;
        }
        memset(undef_to, 0, (size_t)cz->nundef * 4);
        j->stale[z] = 0;
        j->written[z] = 1;
    }
    if (code_wrote) hv_jit_finish(&m->jit, base + c0, cz->used);
    free(buf);
#undef FAIL
}

long hv_code_store_load(HvMachine *m, const char *snap_path, const HvCodeKey *k) {
    if (env_off()) return -1;
    uint64_t t0 = mach_absolute_time();
    char *path = side_path(snap_path, ".code");
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    CsHeader *h = calloc(1, sizeof *h);
    LoadJob j = {.m = m, .h = h, .fd = fd};
    long loaded = -1;
    const char *no = NULL;
    uint8_t build[16];
    if (fd < 0 || !h) {
        if (fd < 0 && errno != ENOENT) fprintf(stderr, "hengevm: code store: can't open it: %s\n", strerror(errno));
        goto out;
    }
    fcntl(fd, F_NOCACHE, 1);
    if (read_all(fd, h, sizeof *h, 0) != 0 || memcmp(h->magic, CS_MAGIC, sizeof h->magic) != 0 ||
        h->version != CS_VERSION || h->header_bytes != sizeof *h || h->crc != header_crc(h))
        no = "it is damaged, or from another version of Henge";
    else if (memcmp(h->key, k->id, sizeof h->key) != 0) no = "it was saved with another Suspend";
    else if (build_id(build) != 0 || memcmp(build, h->build, sizeof build) != 0) no = "it was saved by another build of Henge";
    else if (h->config != hv_translate_config(m)) no = "the translator's settings have changed";
    else if (h->capacity != m->jit.capacity || h->zone_words != m->zone_words || h->nzones != (uint32_t)m->nzones ||
             h->zone_blocks != m->zone_blocks || h->zone_slots != m->zone_slots || h->zone_undef != m->zone_undef ||
             h->max_blocks != m->max_blocks || h->max_slots != m->max_slots || h->max_undef != m->max_undef ||
             h->zone >= h->nzones || h->nzones > HV_JIT_ZONES || h->page_bits != HV_RAM_PAGE_BITS)
        no = "the code cache is laid out differently";
    else if (h->nram != (uint32_t)m->nram) no = "the memory is laid out differently";
    else if (hv_blocks_held(m) || hv_jit_words_used(m)) no = "code was translated before it";
    for (uint32_t i = 0; !no && i < h->nram; i++)
        if (h->ram[i].pa != m->ram[i].pa || h->ram[i].size != m->ram[i].size) no = "the memory is laid out differently";
    for (uint32_t z = 0; !no && z < h->nzones; z++) {
        const CsZone *cz = &h->zones[z];
        if (cz->used > m->zone_words || cz->nblocks > m->zone_blocks || cz->nslots > m->zone_slots || cz->nundef > m->zone_undef)
            no = "it is damaged";
    }
    if (no) {
        fprintf(stderr, "hengevm: code store: not loaded: %s\n", no);
        goto out;
    }
    loaded = 0;
    if (!(j.pages = get_part(fd, &h->pages, (size_t)h->npages * sizeof(CsPage))) ||
        !(j.page_ok = calloc(h->npages ? h->npages : 1, 1))) {
        fprintf(stderr, "hengevm: code store: not loaded: its page records are damaged\n");
        goto out;
    }
    for (uint32_t i = 0; i < h->npages; i++)
        if (j.pages[i].ram >= h->nram || j.pages[i].pg >= ram_pages(&m->ram[j.pages[i].ram])) {
            fprintf(stderr, "hengevm: code store: not loaded: a page record is out of place\n");
            goto out;
        }
    /* The guest's code bytes are the ones saved with it (the snapshot's RAM, paired by the key): checked only when
       asked, since it reads every page the code came from (much of it not needed again for a while). */
    if (env_verify() && h->page_crcs) par_run(load_pages, &j, 64);
    else memset(j.page_ok, 1, h->npages);
    if (env_verify() && !h->page_crcs)
        fprintf(stderr, "hengevm: code store: saved without HV_CODE_STORE_VERIFY=1: its pages can't be checked\n");
    /* The reclaimer: nothing to empty in an empty cache (a job, or a zone dying, given up), and its tables filled
       from the store from here on */
    pthread_mutex_lock(&m->tr_mu);
    hv_jit_reclaim_cancel(m);
    m->zones_dying = 0, m->zone_ready = -1, m->room_early = m->room_early_seen = 0;
    memset(m->unhook_at, 0, sizeof m->unhook_at);
    pthread_mutex_unlock(&m->tr_mu);
    j.new_gen = m->jit_gen + 1; /* the cache is empty: no block of an older generation is anywhere */
    par_run(load_zone, &j, (int)h->nzones);
    /* The machine's side, on this thread: the hash, the code pages marked, the zones and their cursors. */
    uint32_t zones_ok = 0, dropped = 0, stale = 0;
    memset(m->zones, 0, sizeof m->zones);
    memset(m->unhook_at, 0, sizeof m->unhook_at); /* no zone half taken off: the cursors start each one over */
    for (uint32_t z = 0; z < h->nzones; z++) {
        const CsZone *cz = &h->zones[z];
        HvJitZone *zn = &m->zones[z];
        zn->born = j.new_gen;
        if (!j.ok[z]) { /* empty, as a zone never filled: the first one eviction takes */
            if (cz->nblocks) fprintf(stderr, "hengevm: code store: zone %u not loaded: %s\n", z, j.why[z] ? j.why[z] : "?");
            dropped += cz->nblocks;
            /* Its tables' memory given back, as an evicted zone's (above the high-water marks of those that loaded,
               nothing else would); its code words are written over as it fills. */
            if (j.written[z]) hv_machine_drop_zone_tables(m, (int)z, NULL);
            continue;
        }
        zones_ok++;
        stale += j.stale[z];
        zn->score = cz->score, zn->filled = cz->filled;
        zn->used = cz->used, zn->nblocks = cz->nblocks, zn->nslots = cz->nslots, zn->nundef = cz->nundef;
        zn->ndead = cz->nblocks;
        zn->dead_words = 0;
        HvBlock *b0 = m->blocks + (size_t)z * m->zone_blocks;
        for (uint32_t i = 0; i < cz->nblocks; i++) {
            HvBlock *b = &b0[i];
            if (b->jit_gen != j.new_gen) { /* not kept */
                zn->dead_words += b->words;
                continue;
            }
            /* Its entry as the translator leaves a live block's: anything else is not code this build made. */
            size_t w = b->code ? (size_t)(b->code - m->jit.base) : 0;
            if (!b->code || !b->gate || w + b->gate >= (size_t)z * m->zone_words + zn->used || *b->code != HV_BLOCK_NOP) {
                b->jit_gen = 0;
                zn->dead_words += b->words;
                continue;
            }
            /* As protect_code_page and lookup leave a page (dispatch.c): marked as code, so a write to it drops its
               blocks (no CPU has run, so no TLB maps it writable yet); the pieces its code came from noted (all
               from its start on when grouped accesses may have looked further, as in hv_translate_last_read); on
               the page's list. */
            b->gen = __atomic_load_n(&b->ram->page_gen[b->pg], __ATOMIC_ACQUIRE);
            b->dead = 0;
            zn->ndead--;
            __atomic_store_n(&b->ram->code_pages[b->pg], 1, __ATOMIC_RELEASE);
            uint64_t off = (b->pa - b->ram->pa) & ((1ull << HV_RAM_PAGE_BITS) - 1);
            b->ram->code_mask[b->pg] |= hv_code_pieces(off, hv_knob[HV_K_GROUPS] ? (1ull << HV_RAM_PAGE_BITS) - off : 4ull * (b->ninsns ? b->ninsns : 1));
            hv_page_list_insert(m, b->ram, b->pg, b);
            uint32_t hh = hv_block_hash(b->va, b->pa, b->flags);
            hv_hash_insert(m, b, hh);
            __atomic_or_fetch(&m->hash_used, HV_PART(hh, 21), __ATOMIC_RELAXED); /* as lookup's insert */
            loaded++;
        }
        if ((uint32_t)z * m->zone_blocks + cz->nblocks > m->hw_blocks) m->hw_blocks = (uint32_t)z * m->zone_blocks + cz->nblocks;
        if ((uint32_t)z * m->zone_slots + cz->nslots > m->hw_slots) m->hw_slots = (uint32_t)z * m->zone_slots + cz->nslots;
        if ((uint32_t)z * m->zone_undef + cz->nundef > m->hw_undef) m->hw_undef = (uint32_t)z * m->zone_undef + cz->nundef;
    }
    __atomic_store_n(&m->jit_gen, j.new_gen, __ATOMIC_SEQ_CST);
    m->zone_switches = h->zone_switches;
    m->room_gen = 0;
    hv_jit_zone_fill(m, (int)h->zone);
    hv_jit_reclaim_kick(m); /* the zone filled next may be well past its early mark already: one emptied ahead */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    fprintf(stderr, "hengevm: code store: loaded %ld blocks from %u of %u zones in %.0f ms", loaded, zones_ok, h->nzones,
            ms_since(t0));
    if (dropped) fprintf(stderr, "; %u blocks in zones not loaded", dropped);
    if (stale) fprintf(stderr, "; %u blocks from pages that had changed, dropped", stale);
    fprintf(stderr, "\n");
out:
    free(j.pages);
    free(j.page_ok);
    if (fd >= 0) close(fd);
    free(h);
    free(path);
    return loaded;
}

#include "virtio_balloon.h"
#include "machine.h"
#include "ramfile.h"
#include "saveguard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define INFLATEQ 0
#define DEFLATEQ 1
#define HOST_PAGE 0x4000ull
#define PFN_SHIFT 12 /* the protocol's page, whatever the guest's own is */
#define CFG_NUM_PAGES 0
#define CFG_ACTUAL 4
#define MAX_PFNS 4096

/* Host pages [off, off + len) of guest RAM are no longer the guest's: give them back to the OS. A file's
   pages go at once (a hole); anonymous memory is marked reusable, which the kernel drops before compressing
   anything under memory pressure (measured on the Mac: given-back pages were dropped, kept pages compressed).
   Memory with Quick Suspend's files (core/ramfile.h) is made zeros again, fresh, and marked dirty, so the files
   follow (as holes): reusable pages could come back as zeros or as they were, which the files wouldn't know. */
static void give_back(HvVirtioBalloon *b, uint64_t off, uint64_t len) {
    /* The hole zeroes them, which no claim sees: an auto-save's guard keeps them as they were first */
    hv_guard_claim_range(b->vio.m, b->ram_pa + off, len);
    int bad;
    HvRam *r = hv_ram_lookup(b->vio.m, b->ram_pa);
    if (r && r->file) {
        bad = hv_ramfile_discard(r->file, off + (b->ram_pa - r->pa), len) != 0;
    } else if (b->ram_fd >= 0) {
        struct fpunchhole ph = {.fp_flags = 0, .reserved = 0, .fp_offset = (off_t)off, .fp_length = (off_t)len};
        bad = fcntl(b->ram_fd, F_PUNCHHOLE, &ph) != 0;
        if (!bad) msync(b->ram + off, len, MS_INVALIDATE);
    } else {
        bad = madvise(b->ram + off, len, MADV_FREE_REUSABLE) != 0;
    }
    if (bad) {
        if (b->failed++ < 4) fprintf(stderr, "hengevm: balloon: giving back %llu KB at +%llx failed: %s\n",
                                     (unsigned long long)(len >> 10), (unsigned long long)off, strerror(errno));
    } else {
        b->given_back += len / HOST_PAGE;
    }
}

static void take_back(HvVirtioBalloon *b, uint64_t off, uint64_t len) {
    HvRam *r = hv_ram_lookup(b->vio.m, b->ram_pa);
    /* the guest has them again: count them as used (fresh memory for Quick Suspend's: counted as it is touched) */
    if (b->ram_fd < 0 && !(r && r->file)) madvise(b->ram + off, len, MADV_FREE_REUSE);
    b->taken_back += len / HOST_PAGE;
}

/* One element's page numbers, coalesced into runs of whole host pages. A driver may list the four 4 KB quarters
   of each 16 KB page, or (thinking in its own page size) only the first: both are handled, the second when no
   page number in the element has a quarter other than 0. */
static void handle(HvVirtioBalloon *b, const HvVirtqElem *e, int inflate) {
    static uint32_t pfn[MAX_PFNS];
    uint32_t bytes = hv_virtq_read(&b->vio, e, 0, pfn, sizeof pfn);
    uint32_t n = bytes / 4;
    int whole_pages = 1; /* every page number is the first quarter of a host page */
    for (uint32_t i = 0; i < n; i++)
        if (pfn[i] & 3) { whole_pages = 0; break; }
    if (!b->told_pattern && n) {
        b->told_pattern = 1;
        fprintf(stderr, "hengevm: balloon: the guest lists %s (first %u: %x %x %x %x)\n",
                whole_pages ? "one page number per 16 KB page" : "4 KB page numbers", n,
                pfn[0], n > 1 ? pfn[1] : 0, n > 2 ? pfn[2] : 0, n > 3 ? pfn[3] : 0);
    }
    if (inflate) b->inflated_pfns += n; else b->deflated_pfns += n;
    uint64_t run_off = 0, run_len = 0;
    uint64_t page = ~0ull;
    uint32_t quarters = 0;
    for (uint32_t i = 0; i <= n; i++) {
        uint64_t pa = i < n ? (uint64_t)pfn[i] << PFN_SHIFT : 0;
        uint64_t this_page = i < n ? (pa & ~(HOST_PAGE - 1)) : ~1ull;
        if (this_page != page) { /* the page before is complete, or not */
            if (page != ~0ull) {
                int whole = whole_pages || quarters == 0xf;
                if (whole && page >= b->ram_pa && page + HOST_PAGE <= b->ram_pa + b->ram_size) {
                    uint64_t off = page - b->ram_pa;
                    if (run_len && run_off + run_len == off) run_len += HOST_PAGE;
                    else {
                        if (run_len) (inflate ? give_back : take_back)(b, run_off, run_len);
                        run_off = off, run_len = HOST_PAGE;
                    }
                } else if (!whole) b->partial++;
                else b->out_of_ram++;
            }
            page = this_page, quarters = 0;
        }
        if (i < n) quarters |= 1u << ((pa >> PFN_SHIFT) & 3);
    }
    if (run_len) (inflate ? give_back : take_back)(b, run_off, run_len);
}

static void notify(HvVirtio *v, int q) {
    HvVirtioBalloon *b = v->dev;
    if (q != INFLATEQ && q != DEFLATEQ) return;
    static HvVirtqElem e;
    int any = 0, r;
    while ((r = hv_virtq_pop(v, q, &e)) > 0) {
        handle(b, &e, q == INFLATEQ);
        hv_virtq_push(v, q, &e, 0);
        any = 1;
    }
    if (any) hv_virtio_interrupt(v);
}

static void config_write(HvVirtio *v, uint32_t off, uint64_t val, unsigned size) {
    HvVirtioBalloon *b = v->dev;
    if (off < CFG_ACTUAL || off + size > CFG_ACTUAL + 4) return; /* num_pages is the host's */
    memcpy(v->config + off, &val, size);
    uint32_t actual;
    memcpy(&actual, v->config + CFG_ACTUAL, 4);
    static uint32_t last_told;
    /* every 256 MB of change, and the first */
    if (!last_told || (actual > last_told ? actual - last_told : last_told - actual) >= (256u << 20) >> PFN_SHIFT) {
        last_told = actual ? actual : 1;
        fprintf(stderr, "hengevm: balloon: the guest has given %u MB of the %u MB asked; %llu MB back to the OS so far\n",
                actual >> (20 - PFN_SHIFT), b->applied >> (20 - PFN_SHIFT),
                (unsigned long long)((b->given_back - b->taken_back) * HOST_PAGE >> 20));
    }
}

HvVirtioBalloon *hv_virtio_balloon_create(HvMachine *m, HvPcie *bus, uint8_t *ram, uint64_t ram_pa, uint64_t ram_size, int ram_fd) {
    HvVirtioBalloon *b = calloc(1, sizeof *b);
    HvVirtio *v = &b->vio;
    hv_virtio_init(v, m, 5, 0x058000, 2, 0); /* memory controller, other; VERSION_1 only */
    v->dev = b;
    v->notify = notify;
    v->config_write = config_write;
    v->config_len = 8;
    b->ram = ram, b->ram_pa = ram_pa, b->ram_size = ram_size, b->ram_fd = ram_fd;
    hv_pcie_attach(bus, &v->pci);
    return b;
}

void hv_virtio_balloon_set_target(HvVirtioBalloon *b, uint64_t bytes) {
    if (!b) return;
    uint64_t pages = (bytes > b->ram_size ? b->ram_size : bytes) >> PFN_SHIFT;
    __atomic_store_n(&b->target, (uint32_t)pages, __ATOMIC_RELEASE);
}

uint64_t hv_virtio_balloon_actual(const HvVirtioBalloon *b) {
    if (!b) return 0;
    uint32_t actual;
    memcpy(&actual, b->vio.config + CFG_ACTUAL, 4);
    return (uint64_t)actual << PFN_SHIFT;
}

void hv_virtio_balloon_poll(HvVirtioBalloon *b) {
    if (!b || !(b->vio.status & 4)) return; /* the driver is not running yet: it reads num_pages when it starts */
    uint32_t target = __atomic_load_n(&b->target, __ATOMIC_ACQUIRE);
    if (target == b->applied) return;
    b->applied = target;
    memcpy(b->vio.config + CFG_NUM_PAGES, &target, 4);
    fprintf(stderr, "hengevm: balloon: asking the guest for %u MB\n", target >> (20 - PFN_SHIFT));
    hv_virtio_config_changed(&b->vio);
}

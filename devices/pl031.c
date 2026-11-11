#include "pl031.h"

#include <stdlib.h>
#include <time.h>

/* Registers and ID values as in QEMU's hw/rtc/pl031.c. */

static const uint8_t pl031_id[8] = {0x31, 0x10, 0x14, 0x00, 0x0d, 0xf0, 0x05, 0xb1};

void (*hv_rtc_watch)(const char *clock, int write, uint64_t reg, uint64_t value);

static uint32_t now(HvPl031 *r) {
    return (uint32_t)((int64_t)time(NULL) + r->offset);
}

static void update(HvPl031 *r) {
    hv_gicv3_set_irq(r->gic, r->intid, (r->ris & r->imsc) != 0);
}

void hv_pl031_tick(HvPl031 *r) {
    if (r->mr && now(r) >= r->mr && !(r->ris & 1)) {
        r->ris |= 1;
        update(r);
    }
}

static uint64_t pl031_read(void *opaque, uint64_t off, unsigned size) {
    (void)size;
    HvPl031 *r = opaque;
    if (hv_rtc_watch && (off == 0 || off == 8)) hv_rtc_watch("pl031", 0, off, now(r));
    switch (off) {
    case 0x00: return now(r);         /* DR */
    case 0x04: return r->mr;
    case 0x08: return now(r);         /* LR reads back the current value */
    case 0x0c: return 1;              /* CR: always enabled */
    case 0x10: return r->imsc;
    case 0x14: return r->ris;
    case 0x18: return r->ris & r->imsc;
    default:
        if (off >= 0xfe0 && off < 0x1000) return pl031_id[(off - 0xfe0) >> 2];
        return 0;
    }
}

static void pl031_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)size;
    HvPl031 *r = opaque;
    if (hv_rtc_watch) hv_rtc_watch("pl031", 1, off, v);
    switch (off) {
    case 0x04: r->mr = (uint32_t)v; hv_pl031_tick(r); break;
    case 0x08: r->offset = (int64_t)(uint32_t)v - (int64_t)time(NULL); break;
    case 0x10: r->imsc = (uint32_t)v & 1; break;
    case 0x1c: r->ris &= ~((uint32_t)v & 1); break;
    default: break;
    }
    update(r);
}

HvPl031 *hv_pl031_create(HvMachine *m, HvGic *gic, uint64_t pa, int intid) {
    HvPl031 *r = calloc(1, sizeof *r);
    r->gic = gic;
    r->intid = intid;
    hv_machine_add_mmio(m, pa, 0x1000, r, pl031_read, pl031_write, "pl031");
    return r;
}

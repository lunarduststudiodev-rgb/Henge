#include "cfg.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define CFG_SIZE 0x10000
/* iBootStage2 writes up to 0x500000 (seen as a data abort at 0x4ffff8 with the
   64 KB QEMU uses), so the guest treats the whole megabyte from the config base
   as memory. The structure stays in the first 64 KB; the rest is plain RAM. */
#define CFG_WINDOW 0x100000

typedef struct VMAppleCfg {
    uint32_t version;         /* 0x000 */
    uint32_t nr_cpus;         /* 0x004 */
    uint32_t unk1;            /* 0x008 */
    uint32_t unk2;            /* 0x00c */
    uint32_t unk3;            /* 0x010 */
    uint32_t unk4;            /* 0x014 */
    uint64_t ecid;            /* 0x018 */
    uint64_t ram_size;        /* 0x020 */
    uint32_t run_installer1;  /* 0x028 */
    uint32_t unk5;            /* 0x02c */
    uint32_t unk6;            /* 0x030 */
    uint32_t run_installer2;  /* 0x034 */
    uint32_t rnd;             /* 0x038 */
    uint32_t unk7;            /* 0x03c */
    uint8_t mac_en0[8];       /* 0x040: six bytes and two of padding */
    uint8_t mac_en1[8];       /* 0x048 */
    uint8_t mac_wifi0[8];     /* 0x050 */
    uint8_t mac_bt0[8];       /* 0x058 */
    uint8_t reserved[0xa0];   /* 0x060 */
    uint32_t cpu_ids[0x80];   /* 0x100 */
    uint8_t scratch[0x200];   /* 0x300 */
    char serial[32];          /* 0x500 */
    char unk8[32];            /* 0x520 */
    char model[32];           /* 0x540 */
    uint8_t unk9[32];         /* 0x560 */
    uint32_t unk10;           /* 0x580 */
    char soc_name[32];        /* 0x584 */
} __attribute__((packed)) VMAppleCfg;

/* QEMU's comments put serial at 0x380, but its struct (whose layout the guest
   actually sees) has cpu_ids[0x80] of 4 bytes, so the real offsets follow the
   struct. These pin the layout to QEMU's definition. */
_Static_assert(offsetof(VMAppleCfg, mac_en0) == 0x40, "cfg layout");
_Static_assert(offsetof(VMAppleCfg, cpu_ids) == 0x100, "cfg layout");
_Static_assert(offsetof(VMAppleCfg, serial) == 0x500, "cfg layout");
_Static_assert(offsetof(VMAppleCfg, soc_name) == 0x584, "cfg layout");

/* The region is a device rather than RAM because of `rnd` (0x38): the boot ROM
   fills buffers with random bytes by reading it once per word, so it behaves
   as a random-number register. QEMU keeps it as a fixed property (default 0),
   which gives the ROM all-zero randomness. Everything else reads back as
   written, like the RAM QEMU uses. */
static uint64_t rng_state; /* 0: use the system's random source */

static uint64_t cfg_read(void *opaque, uint64_t off, unsigned size) {
    uint64_t v = 0;
    if (off >= 0x38 && off < 0x3c) {
        uint64_t r;
        if (rng_state) {
            rng_state ^= rng_state << 13;
            rng_state ^= rng_state >> 7;
            rng_state ^= rng_state << 17;
            r = rng_state;
        } else {
            r = (uint64_t)arc4random() << 32 | arc4random();
        }
        return size >= 4 ? (uint32_t)r : r & ((1ull << (size * 8)) - 1);
    }
    memcpy(&v, (uint8_t *)opaque + off, size);
    return v;
}

static void cfg_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    memcpy((uint8_t *)opaque + off, &v, size);
}

static char default_model[32];
static char default_soc_name[32];

static void put_str(char *dst, const char *src) {
    strncpy(dst, src, 31);
    dst[31] = 0;
}

void hv_cfg_set_device_info(const char *model, const char *soc_name) {
    put_str(default_model, model ? model : "");
    put_str(default_soc_name, soc_name ? soc_name : "");
}

uint8_t *hv_cfg_create(HvMachine *m, uint64_t pa, const HvCfgParams *p) {
    rng_state = p->seed;
    uint8_t *mem = aligned_alloc(16384, CFG_WINDOW);
    memset(mem, 0, CFG_WINDOW);
    VMAppleCfg *c = (VMAppleCfg *)mem;
    c->version = 2;
    c->nr_cpus = p->nr_cpus ? p->nr_cpus : 1;
    c->unk1 = 1;
    c->unk2 = 1;
    c->unk3 = 0x20;
    c->unk4 = 0;
    c->ecid = p->ecid;
    c->ram_size = p->ram_size;
    c->run_installer1 = p->run_installer1;
    c->unk5 = 1;
    c->unk6 = 1;
    c->run_installer2 = p->run_installer2;
    c->rnd = p->rnd;
    c->unk7 = 0;
    memcpy(c->mac_en0, p->mac_en0, 6);
    memcpy(c->mac_en1, p->mac_en1, 6);
    memcpy(c->mac_wifi0, p->mac_wifi0, 6);
    memcpy(c->mac_bt0, p->mac_bt0, 6);
    for (uint32_t i = 0; i < c->nr_cpus && i < 0x80; i++) c->cpu_ids[i] = i;
    const char *model = p->model ? p->model : (default_model[0] ? default_model : "VM0001");
    const char *soc_name = p->soc_name ? p->soc_name : (default_soc_name[0] ? default_soc_name : "Apple M1 (Virtual)");
    put_str(c->serial, p->serial ? p->serial : "1234");
    put_str(c->unk8, "D/A");
    put_str(c->model, model);
    c->unk10 = 1;
    put_str(c->soc_name, soc_name);
    /* Experiment (HV_CFG_POKE="0x8=0,0x2c=0"): override 32-bit words, to find
       what the unknown fields mean. */
    for (const char *e = getenv("HV_CFG_POKE"); e && *e;) {
        char *end;
        unsigned long off = strtoul(e, &end, 0), val = 0;
        if (*end == '=') val = strtoul(end + 1, &end, 0);
        if (off + 4 <= CFG_SIZE) memcpy(mem + off, &(uint32_t){(uint32_t)val}, 4);
        e = *end == ',' ? end + 1 : end;
        if (*end != ',') break;
    }
    hv_machine_add_mmio(m, pa, CFG_SIZE, mem, cfg_read, cfg_write, "cfg");
    hv_machine_add_ram(m, pa + CFG_SIZE, CFG_WINDOW - CFG_SIZE, mem + CFG_SIZE, 0);
    return mem;
}

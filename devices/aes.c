#include "aes.h"

#include <CommonCrypto/CommonCryptor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Command and register layout from QEMU's hw/vmapple/aes.c. */
#define CMD_SHIFT 28
#define CMD_KEY 0x1
#define CMD_IV 0x2
#define CMD_DATA 0x5
#define CMD_STORE_IV 0x6
#define CMD_FLAG 0x8

#define REG_STATUS 0x0c
#define REG_IRQ_STATUS 0x18
#define REG_IRQ_ENABLE 0x1c
#define REG_WATERMARK 0x20
#define REG_Q_STATUS 0x24
#define REG_FLAG_INFO 0x30
#define REG_FIFO 0x200

#define IRQ_INVALID_CMD (1u << 2)
#define IRQ_FLAG (1u << 5)

static const uint32_t key_lens[4] = {16, 24, 32, 64};

/* Built-in keys 1-3, as QEMU defines them: 32 bytes, the first byte is the index. */
static HvAesKey builtin(uint32_t sel) {
    HvAesKey k = {.len = 32};
    k.key[0] = (uint8_t)sel;
    return k;
}

static void update_irq(HvAes *a) {
    hv_gicv3_set_irq(a->gic, a->intid, (a->irq_status & a->irq_enable) != 0);
}

static int has_payload(HvAes *a, uint32_t words) {
    return a->fifo_idx >= words + 1;
}

static int cmd_key(HvAes *a) {
    uint32_t cmd = a->fifo[0];
    uint32_t sel = (cmd >> 24) & 7, ctx = (cmd >> 27) & 1, mode = (cmd >> 16) & 3;
    if (mode > 1) { /* only ECB (0) and CBC (1); drop the command */
        if (a->m->log) fprintf(stderr, "hengevm: aes: key command %08x asks for mode %u (not modelled)\n", cmd, mode);
        return 1;
    }
    a->cbc = mode == 1;
    a->encrypt = (cmd >> 20) & 1;
    if (sel) {
        static unsigned seen;
        if (!(seen & (1u << sel)) && a->m->log) {
            seen |= 1u << sel;
            fprintf(stderr, "hengevm: aes: key select %u (cmd %08x)%s\n", sel, cmd, sel <= 3 ? "" : " not modelled");
        }
        if (sel <= 3) a->key[ctx] = builtin(sel);
        return 1;
    }
    uint32_t len = key_lens[(cmd >> 22) & 3];
    if (len > sizeof a->key[ctx].key) return 1;
    if (!has_payload(a, len / 4)) return 0; /* wait for the key words */
    memcpy(a->key[ctx].key, &a->fifo[1], len);
    a->key[ctx].len = len;
    return 1;
}

static int cmd_iv(HvAes *a) {
    uint32_t ctx = (a->fifo[0] >> 26) & 3;
    if (!has_payload(a, 4)) return 0;
    memcpy(a->iv[ctx], &a->fifo[1], 16);
    return 1;
}

static int cmd_data(HvAes *a) {
    if (!has_payload(a, 3)) return 0;
    uint32_t cmd = a->fifo[0];
    uint32_t key_ctx = (cmd >> 27) & 1, iv_ctx = (cmd >> 25) & 3, len = cmd & 0xffffff;
    uint64_t src = a->fifo[2] | (((uint64_t)a->fifo[1] << 16) & 0xffff00000000ull);
    uint64_t dst = a->fifo[3] | (((uint64_t)a->fifo[1] << 32) & 0xffff00000000ull);
    HvAesKey *k = &a->key[key_ctx];
    if (k->len != 16 && k->len != 24 && k->len != 32) {
        if (a->m->log) fprintf(stderr, "hengevm: aes: data command %08x with a %u-byte key (not modelled)\n", cmd, k->len);
        return 1;
    }
    const uint8_t *in = hv_phys_ptr_r(a->m, src, len);
    uint8_t *out = hv_phys_ptr_w(a->m, dst, len);
    if (!in || !out) {
        if (out) hv_dma_written(a->m, dst, len); /* its lend ends */
        if (a->m->log) fprintf(stderr, "hengevm: aes: DMA outside RAM (%llx -> %llx, %u bytes)\n",
                               (unsigned long long)src, (unsigned long long)dst, len);
        return 1;
    }
    uint8_t *tmp = malloc(len ? len : 1);
    size_t moved = 0;
    CCCryptorStatus st = CCCrypt(a->encrypt ? kCCEncrypt : kCCDecrypt, kCCAlgorithmAES, a->cbc ? 0 : kCCOptionECBMode,
                                 k->key, k->len, a->cbc ? (const void *)a->iv[iv_ctx] : NULL, in, len, tmp, len,
                                 &moved);
    if (st == kCCSuccess) memcpy(out, tmp, len);
    hv_dma_written(a->m, dst, len); /* failed too: its lend ends */
    if (st != kCCSuccess && a->m->log) fprintf(stderr, "hengevm: aes: cipher failed (%d), length %u\n", (int)st, len);
    free(tmp);
    return 1;
}

static int cmd_store_iv(HvAes *a) {
    if (!has_payload(a, 1)) return 0;
    uint32_t cmd = a->fifo[0];
    uint32_t ctx = (cmd >> 26) & 3;
    uint64_t addr = a->fifo[1] | (((uint64_t)cmd << 32) & 0xff00000000ull);
    hv_phys_write(a->m, addr, a->iv[ctx], 16);
    return 1;
}

static void fifo_process(HvAes *a) {
    if (!a->fifo_idx) return;
    if (a->m->log >= 1 && getenv("HENGEVM_AES_LOG"))
        fprintf(stderr, "hengevm: aes cmd %x (word %u of command %08x)\n", a->fifo[0] >> CMD_SHIFT, a->fifo_idx, a->fifo[0]);
    int done;
    switch (a->fifo[0] >> CMD_SHIFT) {
    case CMD_KEY: done = cmd_key(a); break;
    case CMD_IV: done = cmd_iv(a); break;
    case CMD_DATA: done = cmd_data(a); break;
    case CMD_STORE_IV: done = cmd_store_iv(a); break;
    case CMD_FLAG:
        /* Commands complete as they arrive, so a flag raises its interrupt at once. */
        if ((a->fifo[0] >> 27) & 1) a->irq_status |= IRQ_FLAG;
        a->flag_info = a->fifo[0] & 0xff;
        done = 1;
        break;
    default:
        if (a->m->log) fprintf(stderr, "hengevm: aes: unknown command %08x\n", a->fifo[0]);
        a->irq_status |= IRQ_INVALID_CMD;
        done = 1;
        break;
    }
    if (done) a->fifo_idx = 0;
}

static uint64_t aes1_read(void *opaque, uint64_t off, unsigned size) {
    (void)size;
    HvAes *a = opaque;
    switch (off) {
    case REG_STATUS: return a->status;
    case REG_IRQ_STATUS: return a->irq_status;
    case REG_IRQ_ENABLE: return a->irq_enable;
    case REG_WATERMARK: return a->watermark;
    case REG_Q_STATUS: return a->q_status;
    case REG_FLAG_INFO: return a->flag_info;
    default:
        if (a->m->log >= 2) fprintf(stderr, "hengevm: aes: read of unknown offset %llx\n", (unsigned long long)off);
        return 0;
    }
}

static void aes1_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)size;
    HvAes *a = opaque;
    switch (off) {
    case REG_IRQ_STATUS: a->irq_status &= ~(uint32_t)v; break;
    case REG_IRQ_ENABLE: a->irq_enable = (uint32_t)v; break;
    case REG_FIFO:
        if (a->fifo_idx < 9) a->fifo[a->fifo_idx++] = (uint32_t)v;
        fifo_process(a);
        break;
    default:
        if (a->m->log >= 2) fprintf(stderr, "hengevm: aes: write of unknown offset %llx\n", (unsigned long long)off);
        return;
    }
    update_irq(a);
}

static uint64_t aes2_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque, (void)off, (void)size;
    return 0;
}

static void aes2_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvAes *a = opaque;
    (void)size;
    if (a->m->log) fprintf(stderr, "hengevm: aes2: write +%llx = %llx (not modelled)\n", (unsigned long long)off, (unsigned long long)v);
}

HvAes *hv_aes_create(HvMachine *m, HvGic *gic, uint64_t pa1, uint64_t pa2, int intid) {
    HvAes *a = calloc(1, sizeof *a);
    a->m = m;
    a->gic = gic;
    a->intid = intid;
    a->status = 0x3f80; /* QEMU's reset values */
    a->q_status = 2;
    hv_machine_add_mmio(m, pa1, 0x4000, a, aes1_read, aes1_write, "aes");
    hv_machine_add_mmio(m, pa2, 0x4000, a, aes2_read, aes2_write, "aes2");
    return a;
}

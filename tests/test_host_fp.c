/* Host floating point on a CPU thread (host/host_fp.h). While a CPU runs, the guest's FPCR and FPSR are live
   in the host registers, and the MMIO handlers run under them. pvg_lite's doorbells compute the display's
   modes in floating point and call the Metal side, so they run under the host's controls and leave the
   guest's FPCR and FPSR as they were. Here with guest memory and a stand-in for the Metal side: no GPU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../devices/pvg_lite.c"

#define BASE 0x40000000ull
#define PG(n) (BASE + 0x4000ull * (n))
static uint8_t ram[16 * 0x4000];
void *hv_phys_ptr(HvMachine *m, uint64_t pa, uint64_t len) {
    (void)m;
    return pa >= BASE && pa - BASE <= sizeof ram && len <= sizeof ram - (pa - BASE) ? ram + (pa - BASE) : NULL;
}
void *hv_phys_ptr_w(HvMachine *m, uint64_t pa, uint64_t len) { return hv_phys_ptr(m, pa, len); }
const void *hv_phys_ptr_r(HvMachine *m, uint64_t pa, uint64_t len) { return hv_phys_ptr(m, pa, len); }
void hv_dma_written(HvMachine *m, uint64_t pa, uint64_t len) { (void)m, (void)pa, (void)len; }
int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque, uint64_t (*read)(void *, uint64_t, unsigned),
                        void (*write)(void *, uint64_t, uint64_t, unsigned), const char *name) {
    (void)m, (void)pa, (void)size, (void)opaque, (void)read, (void)write, (void)name;
    return 0;
}
void hv_gicv3_pend(HvGic *g, int intid) { (void)g, (void)intid; }
void hv_machine_wake(HvMachine *m) { (void)m; }
void hv_dev_lock(HvMachine *m) { (void)m; }
void hv_dev_unlock(HvMachine *m) { (void)m; }

/* The Metal side. The two hooks a CPU thread calls compute in floating point, as Apple's code may (1/3 is
   inexact), and note the controls they ran under. */
#include "pvg_metal_standin.h"
#include "zalloc.h"
static volatile double third;
static uint64_t hook_fpcr[2] = {~0ull, ~0ull};
static void host_work(int k) {
    __asm__ volatile("mrs %0, fpcr" : "=r"(hook_fpcr[k]));
    volatile double one = 1.0, three = 3.0;
    third = one / three;
}

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The guest's controls: round toward zero, flush to zero, default NaN. Its flags: none raised. */
static const uint64_t GUEST_FPCR = 3ull << 22 | 1ull << 24 | 1ull << 25;
static uint64_t fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static uint64_t fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static void as_guest(void) { __asm__ volatile("msr fpcr, %0\n\tmsr fpsr, xzr" ::"r"(GUEST_FPCR) : "memory"); }
static void as_host(void) { __asm__ volatile("msr fpcr, xzr\n\tmsr fpsr, xzr" ::: "memory"); }

static uint32_t *g32(uint64_t pa) { return hv_phys_ptr(NULL, pa, 4); }

/* The display's page set through channel 1 (no Metal side: the device answers it itself). The record on
   page 2, the ring's table on page 5, the ring on page 10, the display's page 12. */
static void test_display_page(void) {
    printf("display page: modes worked out under the host's controls\n");
    memset(ram, 0, sizeof ram);
    metal_on = 0;
    HvPvgLite *p = hv_pvg_lite_create(zalloc(_Alignof(HvMachine), sizeof(HvMachine)), NULL, 0, 0, 1, 2);
    gfx_write(p, 0x101c, PG(2) >> 14, 4), gfx_write(p, 0x1030, PG(1) >> 14, 4);
    uint32_t *rec = g32(PG(2) + 0x400);
    rec[4] = (uint32_t)(PG(5) >> 14);
    *g32(PG(5)) = (uint32_t)(PG(10) >> 14);
    uint32_t c[5] = {1, 20, 1, 0, (uint32_t)(PG(12) >> 14)};
    memcpy(ram + (PG(10) - BASE), c, sizeof c);
    rec[0] = 20;
    as_guest();
    gfx_write(p, 0x1020, 1, 4);
    uint64_t cr = fpcr(), sr = fpsr();
    as_host();
    CHECK(p->display_pa == PG(12), "the display's page taken: %llx", (unsigned long long)p->display_pa);
    CHECK(cr == GUEST_FPCR, "the guest's FPCR kept: %llx", (unsigned long long)cr);
    CHECK(sr == 0, "no flag left in the guest's FPSR: %llx", (unsigned long long)sr);
}

/* The same, added with no doorbell and found by CPU 0's tick (hv_pvg_lite_poll, from the board's tick). */
static void test_poll(void) {
    printf("the tick's poll: modes worked out under the host's controls\n");
    memset(ram, 0, sizeof ram);
    metal_on = 0;
    HvPvgLite *p = hv_pvg_lite_create(zalloc(_Alignof(HvMachine), sizeof(HvMachine)), NULL, 0, 0, 1, 2);
    gfx_write(p, 0x101c, PG(2) >> 14, 4), gfx_write(p, 0x1030, PG(1) >> 14, 4);
    uint32_t *rec = g32(PG(2) + 0x400);
    rec[4] = (uint32_t)(PG(5) >> 14);
    *g32(PG(5)) = (uint32_t)(PG(10) >> 14);
    uint32_t c[5] = {1, 20, 1, 0, (uint32_t)(PG(13) >> 14)};
    memcpy(ram + (PG(10) - BASE), c, sizeof c);
    rec[0] = 20;
    as_guest();
    hv_pvg_lite_poll(p);
    uint64_t cr = fpcr(), sr = fpsr();
    as_host();
    CHECK(p->display_pa == PG(13), "the display's page taken: %llx", (unsigned long long)p->display_pa);
    CHECK(cr == GUEST_FPCR, "the guest's FPCR kept: %llx", (unsigned long long)cr);
    CHECK(sr == 0, "no flag left in the guest's FPSR: %llx", (unsigned long long)sr);
}

/* The Metal side's hooks, called from the doorbells: the pointer moved, the mapper started again. */
static void test_metal_hooks(void) {
    printf("Metal's hooks: called under the host's controls\n");
    memset(ram, 0, sizeof ram);
    metal_on = 1;
    HvPvgLite *p = hv_pvg_lite_create(zalloc(_Alignof(HvMachine), sizeof(HvMachine)), NULL, 0, 0, 1, 2);
    atomic_store(&p->metal, (HvPvgMetal *)&metal_on);
    as_guest();
    gfx_write(p, 0x1220, 1, 4);
    iosfc_write(p, 0x1020, 0, 8);
    uint64_t cr = fpcr(), sr = fpsr();
    as_host();
    CHECK(hook_fpcr[0] == 0, "cursor_moved ran under FPCR %llx", (unsigned long long)hook_fpcr[0]);
    CHECK(hook_fpcr[1] == 0, "mapper_reset ran under FPCR %llx", (unsigned long long)hook_fpcr[1]);
    CHECK(cr == GUEST_FPCR, "the guest's FPCR kept: %llx", (unsigned long long)cr);
    CHECK(sr == 0, "no flag left in the guest's FPSR: %llx", (unsigned long long)sr);
}

/* Brackets nest, and put back flags the guest had raised itself. */
static HV_HOST_FP_WORK void inexact(void) { volatile double one = 1.0, three = 3.0; third = one / three; }
static void test_nesting(void) {
    printf("brackets: nested, the guest's own flags kept\n");
    __asm__ volatile("msr fpcr, %0\n\tmsr fpsr, %1" ::"r"(GUEST_FPCR), "r"(1ull << 3) : "memory"); /* UFC raised */
    HvHostFp outer = hv_host_fp_begin();
    HvHostFp inner = hv_host_fp_begin();
    inexact();
    hv_host_fp_end(inner);
    uint64_t mid = fpsr();
    hv_host_fp_end(outer);
    uint64_t cr = fpcr(), sr = fpsr();
    as_host();
    CHECK(mid == 1u << 3, "the inner end puts back what the outer begin left: %llx", (unsigned long long)mid);
    CHECK(cr == GUEST_FPCR && sr == 1u << 3, "outside, the guest's: FPCR %llx FPSR %llx", (unsigned long long)cr, (unsigned long long)sr);
}

int main(void) {
    on_cpu_hook = host_work;
    test_display_page();
    test_poll();
    test_metal_hooks();
    test_nesting();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}

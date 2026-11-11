#include "timer.h"

#include "sysreg.h"

static uint64_t one(HvGic *g, int c, int ppi, uint64_t ctl, uint64_t cval, uint64_t now, uint64_t offset) {
    int enabled = (ctl & 1) && !(ctl & 2);
    int fired = enabled && now - offset >= cval;
    hv_gicv3_set_ppi(g, c, ppi, fired);
    /* Deadline in host counter units (the guest's counter stood still while paused); nothing to
       wait for if off or already firing. */
    return enabled && !fired ? cval + offset + hv_time_bias : 0;
}

uint64_t hv_timer_update(HvGic *g, HvCpu *cpu) {
    uint64_t now = hv_counter();
    uint64_t v = one(g, (int)cpu->index, HV_PPI_VTIMER, cpu->cntv_ctl, cpu->cntv_cval, now, cpu->cntvoff);
    uint64_t p = one(g, (int)cpu->index, HV_PPI_PTIMER, cpu->cntp_ctl, cpu->cntp_cval, now, 0);
    if (!v) return p;
    if (!p) return v;
    return v < p ? v : p;
}

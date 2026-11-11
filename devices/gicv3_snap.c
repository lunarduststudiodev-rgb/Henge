/* Suspend: the GIC's state in a snapshot (core/snapshot.h). Apart from gicv3.c so the engine
   library links without the snapshot code until something uses it. */
#include "gicv3.h"

#include <stdio.h>

#include "snapshot.h"

static int gic_ncpus(HvGic *g) { return g->m->ncpus ? g->m->ncpus : 1; }

/* ---- Suspend: the whole GIC, field by field (core/snapshot.h). ---- */

#define GIC_SPI_FIELDS(X) X(group) X(grpmod) X(enabled) X(latch) X(level) X(active) X(priority) X(edge) X(route) X(spi_on)
#define GIC_CPU_FIELDS(X)                                                                                   \
    X(group) X(grpmod) X(enabled) X(latch) X(level) X(active) X(priority) X(edge) X(waker) X(pmr) X(bpr0) \
    X(bpr1) X(icc_ctlr) X(igrpen0) X(igrpen1) X(sre) X(ap0r) X(ap1r) X(depth)

int hv_gicv3_snap_save(HvGic *g, HvSnapWriter *w) {
    HvSnapOut o = {0};
    uint32_t nirqs = HV_GIC_NUM_IRQS, n = (uint32_t)gic_ncpus(g);
    pthread_mutex_lock(&g->mu);
    HV_SNAP_OUT(&o, nirqs);
    HV_SNAP_OUT(&o, n);
    HV_SNAP_OUT(&o, g->ctlr);
#define OUT(f) HV_SNAP_OUT(&o, g->f);
    GIC_SPI_FIELDS(OUT)
#undef OUT
    for (uint32_t c = 0; c < n; c++) {
        HvGicCpu *k = &g->cpu[c];
#define OUT(f) HV_SNAP_OUT(&o, k->f);
        GIC_CPU_FIELDS(OUT)
#undef OUT
        for (int i = 0; i < 32; i++) {
            HV_SNAP_OUT(&o, k->stack[i].intid);
            HV_SNAP_OUT(&o, k->stack[i].prio);
            HV_SNAP_OUT(&o, k->stack[i].group);
        }
    }
    pthread_mutex_unlock(&g->mu);
    return hv_snap_put(w, HV_SNAP_GIC, 1, &o);
}

int hv_gicv3_snap_load(HvGic *g, HvSnapReader *r) {
    HvSnapIn in;
    if (hv_snap_get(r, HV_SNAP_GIC, 1, &in) != 0) return -1;
    uint32_t nirqs, n;
    HV_SNAP_IN(&in, nirqs);
    HV_SNAP_IN(&in, n);
    if (in.bad || nirqs != HV_GIC_NUM_IRQS || n != (uint32_t)gic_ncpus(g)) {
        fprintf(stderr, "hengevm: snapshot: the GIC was saved with %u interrupts and %u CPUs, this one has %d and %d\n", nirqs, n,
                HV_GIC_NUM_IRQS, gic_ncpus(g));
        return -1;
    }
    pthread_mutex_lock(&g->mu);
    HV_SNAP_IN(&in, g->ctlr);
#define IN(f) HV_SNAP_IN(&in, g->f);
    GIC_SPI_FIELDS(IN)
#undef IN
    int bad = 0;
    for (uint32_t c = 0; c < n; c++) {
        HvGicCpu *k = &g->cpu[c];
#define IN(f) HV_SNAP_IN(&in, k->f);
        GIC_CPU_FIELDS(IN)
#undef IN
        for (int i = 0; i < 32; i++) {
            HV_SNAP_IN(&in, k->stack[i].intid);
            HV_SNAP_IN(&in, k->stack[i].prio);
            HV_SNAP_IN(&in, k->stack[i].group);
        }
        if (k->depth < 0 || k->depth > 32) bad = 1;
    }
    if (hv_snap_in_done(&in) != 0) bad = 1;
    pthread_mutex_unlock(&g->mu);
    if (!bad) hv_gicv3_update(g); /* every CPU's lines from the loaded state */
    if (bad) fprintf(stderr, "hengevm: snapshot: the GIC section is damaged\n");
    return bad ? -1 : 0;
}

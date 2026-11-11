/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Suspending a Mac in the experimental Rosetta mode, and keeping every older save loadable.

   tests/cpus_v1.hvsnap is a CPU and clock save written by an older engine (the one before the mode
   existed): this file built with -DMAKE_FIXTURE against that engine, run with the path to write.
   It must load as mode 0 with the Rosetta state zero, whatever mode the machine was made in, and a Mac not in
   the mode must still write the very same bytes. A Mac in the mode writes version 2 with its P/A flags and
   Apple FP controls, and goes on in that mode whatever the machine was made with. */
#include "machine.h"
#include "snapshot.h"
#ifndef MAKE_FIXTURE
#include "rosetta.h"
#include "zalloc.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NCPUS 2

static HvMachine m;
static HvCpu cpus[NCPUS];
static int failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    if (got == want) return;
    printf("FAIL %s: got %llu (%#llx), want %llu (%#llx)\n", what, (unsigned long long)got, (unsigned long long)got,
           (unsigned long long)want, (unsigned long long)want);
    failures++;
}

/* What each CPU holds: the same in the fixture's writer and here. */
static void fill(HvCpu *c, unsigned i) {
    uint64_t k = 0x1000ull * (i + 1);
    for (int r = 0; r < 31; r++) c->x[r] = k + (uint64_t)r;
    for (size_t b = 0; b < sizeof c->v; b++) ((uint8_t *)c->v)[b] = (uint8_t)(b * 7 + i);
    c->sp = k + 0x100, c->pc = 0x10000 + 4 * i, c->nzcv = 0x60000000, c->fpcr = 0x03000000, c->fpsr = 0x10;
    c->tpidr_el0 = k + 1, c->tpidr_el1 = k + 2, c->sp_el0 = k + 3, c->sp_el1 = k + 4, c->daif = 0x3c0;
    c->sctlr_el1 = 0x30d00801, c->ttbr0_el1 = k << 4, c->ttbr1_el1 = k << 5, c->tcr_el1 = 0x35b5503510ull;
    c->actlr_el1 = 0, c->cntkctl_el1 = 3, c->el = 0, c->spsel = 1, c->off = (int)i;
}

static void same(const char *what, HvCpu *c, unsigned i) {
    HvCpu *want = zalloc(_Alignof(HvCpu), sizeof *want);
    fill(want, i);
    char name[128];
#define SAME(f)                                                         \
    snprintf(name, sizeof name, "%s: CPU %u " #f, what, i);             \
    check(name, (uint64_t)c->f, (uint64_t)want->f);
    SAME(x[0]) SAME(x[30]) SAME(sp) SAME(pc) SAME(nzcv) SAME(fpcr) SAME(fpsr) SAME(tpidr_el0) SAME(tpidr_el1)
    SAME(sp_el0) SAME(sp_el1) SAME(daif) SAME(sctlr_el1) SAME(ttbr0_el1) SAME(ttbr1_el1) SAME(tcr_el1)
    SAME(cntkctl_el1) SAME(el) SAME(spsel) SAME(off)
#undef SAME
    snprintf(name, sizeof name, "%s: CPU %u vector registers", what, i);
    check(name, memcmp(c->v, want->v, sizeof c->v) == 0, 1);
    free(want);
}

/* A machine of NCPUS CPUs, paused, none running (as a restore finds it). */
static void machine(int mode) {
    if (hv_machine_init(&m, 4 << 20) != 0) exit(1);
    for (unsigned i = 0; i < NCPUS; i++) {
        hv_cpu_reset_index(&cpus[i], &m, i);
        cpus[i].mpidr = i; /* the board's (vmapple sets it) */
        cpus[i].pc_state->at_gate = 1;
    }
#ifndef MAKE_FIXTURE
    if (mode && hv_machine_set_rosetta(&m, mode) != 0) exit(1);
#else
    (void)mode;
#endif
    hv_machine_restore_paused(&m, 123456789);
}

static int save(const char *path) {
    HvSnapWriter *w = hv_snap_begin(path, HV_SNAP_NO_SYNC);
    if (!w) return -1;
    if (hv_snap_put_cpus(w, &m) != 0) {
        hv_snap_abort(w);
        return -1;
    }
    return hv_snap_commit(w);
}

#ifdef MAKE_FIXTURE
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    machine(0);
    for (unsigned i = 0; i < NCPUS; i++) fill(&cpus[i], i);
    return save(argv[1]) == 0 ? 0 : 1;
}
#else

static int load(const char *path) {
    HvSnapReader *r = hv_snap_open(path, NULL, 0);
    if (!r) return -2;
    int rc = hv_snap_get_cpus(r, &m);
    hv_snap_close(r);
    return rc;
}

/* The CPU section's bytes, for comparing two saves. */
static uint8_t *section(const char *path, uint32_t version, size_t *len) {
    HvSnapReader *r = hv_snap_open(path, NULL, 0);
    HvSnapIn in;
    if (!r || hv_snap_get(r, HV_SNAP_CPUS, version, &in) != 0) {
        if (r) hv_snap_close(r);
        return NULL;
    }
    uint8_t *p = malloc(in.len);
    memcpy(p, in.p, in.len);
    *len = in.len;
    hv_snap_close(r);
    return p;
}

static int version_of(const char *path) {
    HvSnapReader *r = hv_snap_open(path, NULL, 0);
    if (!r) return -2;
    int v = hv_snap_version(r, HV_SNAP_CPUS), mode = hv_snap_cpus_rosetta(r);
    hv_snap_close(r);
    return v * 10 + mode; /* 10: version 1, mode 0; 21: version 2, mode 1 */
}

static void dirty(void) { /* what a machine made in Rosetta mode might hold before the restore */
    for (unsigned i = 0; i < NCPUS; i++) cpus[i].apflags = HV_APFLAGS_MASK, cpus[i].aspsr_el1 = 7, cpus[i].afpcr_el0 = 3;
}

int main(int argc, char **argv) {
    (void)argc;
    const char *old = "tests/cpus_v1.hvsnap";
    char dir[] = "/tmp/henge-rosetta-snap.XXXXXX", a[64], b[64];
    if (!mkdtemp(dir)) return 1;
    snprintf(a, sizeof a, "%s/a", dir);
    snprintf(b, sizeof b, "%s/b", dir);
    int afp = hv_rosetta_host_supported();
    unsetenv("HV_SOFTWARE_TSO");
    (void)argv;

    /* 1. The old save, into a machine made in Rosetta mode where the host has it: mode 0, flags zero. */
    check("old save: CPU section version 1, mode 0", (uint64_t)version_of(old), 10);
    machine(afp ? 1 : 0);
    dirty();
    check("old save: loads", (uint64_t)load(old), 0);
    check("old save: the machine is in mode 0", (uint64_t)m.rosetta, 0);
    check("old save: no software TSO", (uint64_t)m.software_tso, 0);
    for (unsigned i = 0; i < NCPUS; i++) {
        same("old save", &cpus[i], i);
        check("old save: CPU mode 0", cpus[i].rosetta_mode, 0);
        check("old save: P/A flags zero", cpus[i].apflags, 0);
        check("old save: ASPSR zero", cpus[i].aspsr_el1, 0);
        check("old save: AFPCR zero", cpus[i].afpcr_el0, 0);
    }

    /* 2. A Mac not in the mode writes what the old build wrote, byte for byte. */
    check("mode 0: saves", (uint64_t)save(a), 0);
    check("mode 0: CPU section version 1", (uint64_t)version_of(a), 10);
    size_t la = 0, lb = 0;
    uint8_t *pa = section(a, 1, &la), *pb = section(old, 1, &lb);
    check("mode 0: the same bytes as the old build's", pa && pb && la == lb && memcmp(pa, pb, la) == 0, 1);
    free(pa), free(pb);

    /* 3. A damaged mode is refused. */
    {
        HvSnapWriter *w = hv_snap_begin(b, HV_SNAP_NO_SYNC);
        HvSnapOut o = {0}, t = {0};
        uint32_t n = NCPUS, mode = 7;
        uint64_t counter = 1, freq = 24000000, us = 1;
        HV_SNAP_OUT(&o, n);
        HV_SNAP_OUT(&o, mode);
        HV_SNAP_OUT(&t, counter);
        HV_SNAP_OUT(&t, freq);
        HV_SNAP_OUT(&t, us);
        int ok = w && hv_snap_put(w, HV_SNAP_CPUS, 2, &o) == 0 && hv_snap_put(w, HV_SNAP_TIME, 1, &t) == 0 &&
                 hv_snap_commit(w) == 0;
        check("bad mode: written", ok, 1);
        check("bad mode: mode unknown", (uint64_t)version_of(b), (uint64_t)(20 - 1));
        machine(0);
        check("bad mode: refused", (uint64_t)load(b), (uint64_t)-1);
    }

    if (!afp) {
        printf("Rosetta suspend: %d failures (no FEAT_AFP here: the Rosetta-mode saves were not tried)\n", failures);
        return failures ? 1 : 0;
    }

    /* 4. Each Rosetta mode saves its state and goes on in that mode, in a machine made without it. */
    for (int mode = 1; mode <= 2; mode++) {
        machine(mode);
        for (unsigned i = 0; i < NCPUS; i++) {
            fill(&cpus[i], i);
            cpus[i].apflags = i ? HV_APFLAGS_MASK : 1ull << 26;
            cpus[i].aspsr_el1 = 5, cpus[i].afpcr_el0 = i + 1, cpus[i].actlr_el1 = 0x70;
        }
        check("Rosetta: saves", (uint64_t)save(a), 0);
        check("Rosetta: CPU section version 2 with its mode", (uint64_t)version_of(a), (uint64_t)(20 + mode));
        machine(0);
        check("Rosetta: loads", (uint64_t)load(a), 0);
        check("Rosetta: the machine is in the saved mode", (uint64_t)m.rosetta, (uint64_t)mode);
        check("Rosetta: software TSO on", (uint64_t)m.software_tso, 1);
        for (unsigned i = 0; i < NCPUS; i++) {
            same("Rosetta", &cpus[i], i);
            check("Rosetta: CPU mode", cpus[i].rosetta_mode, (uint64_t)mode);
            check("Rosetta: P/A flags", cpus[i].apflags, i ? HV_APFLAGS_MASK : 1ull << 26);
            check("Rosetta: ASPSR", cpus[i].aspsr_el1, 5);
            check("Rosetta: AFPCR", cpus[i].afpcr_el0, i + 1);
            check("Rosetta: ACTLR", cpus[i].actlr_el1, 0x70);
        }
    }

    /* 5. A Rosetta save stays one: its next save (after going on) is version 2 again. */
    check("Rosetta: saves again", (uint64_t)save(b), 0);
    check("Rosetta: still version 2", (uint64_t)version_of(b), 22);

    unlink(a), unlink(b), rmdir(dir);
    printf("Rosetta suspend: %d failures\n", failures);
    return failures ? 1 : 0;
}
#endif

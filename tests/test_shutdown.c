/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Based on PR #50 by liukaixiang817. */
#include "dispatch.h"
#include "exception.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Run real guest HVC instructions, including power-off from a secondary while
   CPU 0 is parked in CPU_OFF. A kick alone cannot release its condition wait. */
static HvMachine machine;
static HvCpu cpus[3];
static _Atomic int finished[3];
static int failures;

static void check(const char *what, int ok) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

static void *run_cpu(void *arg) {
    HvCpu *cpu = arg;
    int reason = hv_run(cpu);
    atomic_store(&finished[cpu->index], 1);
    return (void *)(intptr_t)reason;
}

static void power_request(uint32_t function, int caller) {
    uint32_t *code = aligned_alloc(16384, 16384);
    if (!code || hv_machine_init(&machine, 4 << 20)) exit(1);
    code[0] = 0xd4000002; /* hvc #0 */
    code[1] = 0xd2800f69; /* mov x9, #123: must not execute after SYSTEM_OFF/RESET */
    code[2] = 0x14000000; /* b . */
    hv_machine_add_ram(&machine, 0x10000, 16384, code, 1);
    pthread_t threads[3];
    for (int i = 0; i < 3; i++) {
        atomic_store(&finished[i], 0);
        hv_cpu_reset_index(&cpus[i], &machine, i);
        cpus[i].pc = 0x10000;
        cpus[i].x[0] = function;
        cpus[i].off = i != caller;
        if (i != caller) pthread_create(&threads[i], NULL, run_cpu, &cpus[i]);
    }
    for (int i = 0; i < 3; i++) if (i != caller) {
        for (int n = 0; n < 1000 && !__atomic_load_n(&cpus[i].parked_off, __ATOMIC_SEQ_CST); n++) usleep(1000);
        check("idle CPU reached CPU_OFF", cpus[i].parked_off);
        /* Synchronize with the condition wait, not just the parked flag. */
        pthread_mutex_lock(&cpus[i].pc_state->wake_mu);
        pthread_mutex_unlock(&cpus[i].pc_state->wake_mu);
    }
    pthread_create(&threads[caller], NULL, run_cpu, &cpus[caller]);
    for (int n = 0; n < 500; n++) {
        if (atomic_load(&finished[0]) && atomic_load(&finished[1]) && atomic_load(&finished[2])) break;
        usleep(1000);
    }
    for (int i = 0; i < 3; i++) {
        check("power request wakes every parked CPU", atomic_load(&finished[i]));
        /* A failed regression must still release its test threads. */
        hv_cpu_wake(&cpus[i]);
        void *result;
        pthread_join(threads[i], &result);
        check("CPU returns HALT", (intptr_t)result == HV_EXIT_HALT);
        check("power request preserved", cpus[i].halted == (function == 0x84000008 ? 1 : 2));
        check("no guest code after power request", cpus[i].x[9] == 0);
    }
    hv_machine_halt(&machine, HV_HALT_HOST);
    for (int i = 0; i < 3; i++)
        check("host cleanup preserves guest outcome", cpus[i].halted == (function == 0x84000008 ? 1 : 2));
    hv_machine_free(&machine);
    free(code);
}

static void *power_on(void *unused) {
    (void)unused;
    hv_exception_insn(&cpus[0], 0xd4000002, 0x10000);
    atomic_store(&finished[0], 1);
    return NULL;
}

static void stop_during_cpu_on(void) {
    if (hv_machine_init(&machine, 4 << 20)) exit(1);
    for (int i = 0; i < 2; i++) {
        hv_cpu_reset_index(&cpus[i], &machine, i);
        cpus[i].mpidr = i;
    }
    /* CPU_ON is waiting for a target that has not reached its CPU_OFF wait. */
    cpus[1].off = 1;
    cpus[0].x[0] = 0xc4000003;
    cpus[0].x[1] = 1;
    atomic_store(&finished[0], 0);
    pthread_t thread;
    pthread_create(&thread, NULL, power_on, NULL);
    usleep(20000);
    hv_machine_halt(&machine, HV_HALT_SHUTDOWN);
    for (int n = 0; n < 500 && !atomic_load(&finished[0]); n++) usleep(1000);
    check("shutdown cancels a pending CPU_ON", atomic_load(&finished[0]));
    /* Release even the old implementation so a regression reports failure. */
    __atomic_store_n(&cpus[1].parked_off, 1, __ATOMIC_SEQ_CST);
    pthread_join(thread, NULL);
    check("CPU_ON cannot clear shutdown", cpus[1].halted == HV_HALT_SHUTDOWN && cpus[1].off);
    check("CPU_ON reports DENIED", cpus[0].x[0] == (uint64_t)-3);
    /* Also cover the target that was already parked when the request arrived. */
    cpus[0].x[0] = 0xc4000003;
    cpus[0].x[1] = 1;
    hv_exception_insn(&cpus[0], 0xd4000002, 0x10000);
    check("a stopped CPU stays stopped", cpus[1].halted == HV_HALT_SHUTDOWN && cpus[1].off);
    hv_machine_free(&machine);
}

/* A halt while the machine is paused (an auto-save or Suspend in progress as the Mac is stopped): the paused
   CPUs leave the gate and end, CPU 0 parked in CPU_OFF too, and a pause waiter does not hang. */
static void halt_while_paused(void) {
    uint32_t *code = aligned_alloc(16384, 16384);
    if (!code || hv_machine_init(&machine, 4 << 20)) exit(1);
    code[0] = 0x91000529; /* add x9, x9, #1 */
    code[1] = 0x17ffffff; /* b . - 4: runs until stopped */
    hv_machine_add_ram(&machine, 0x10000, 16384, code, 1);
    pthread_t threads[3];
    for (int i = 0; i < 3; i++) {
        atomic_store(&finished[i], 0);
        hv_cpu_reset_index(&cpus[i], &machine, i);
        cpus[i].pc = 0x10000;
        cpus[i].off = i == 0; /* CPU 0 parked in CPU_OFF, as macOS leaves it at times */
        pthread_create(&threads[i], NULL, run_cpu, &cpus[i]);
    }
    usleep(20000);
    hv_machine_pause(&machine);
    check("the machine pauses", hv_machine_pause_wait(&machine, 2000) == 0);
    uint64_t x1 = cpus[1].x[9], x2 = cpus[2].x[9];
    usleep(10000);
    check("no guest code while paused", cpus[1].x[9] == x1 && cpus[2].x[9] == x2);
    hv_machine_halt(&machine, HV_HALT_HOST);
    for (int n = 0; n < 1000; n++) {
        if (atomic_load(&finished[0]) && atomic_load(&finished[1]) && atomic_load(&finished[2])) break;
        usleep(1000);
    }
    check("a pause waiter returns once every CPU has gone", hv_machine_pause_wait(&machine, 100) == 0);
    for (int i = 0; i < 3; i++) {
        check("a halt ends a paused (or CPU_OFF) CPU", atomic_load(&finished[i]));
        hv_cpu_wake(&cpus[i]);
        hv_machine_resume(&machine); /* a failed regression must still release its threads */
        void *result;
        pthread_join(threads[i], &result);
        check("paused CPU returns HALT", (intptr_t)result == HV_EXIT_HALT);
        check("halt reason kept", cpus[i].halted == HV_HALT_HOST);
    }
    check("no guest code after the halt", cpus[1].x[9] == x1 && cpus[2].x[9] == x2);
    hv_machine_free(&machine);
    free(code);
}

int main(void) {
    alarm(15);
    for (int caller = 0; caller < 2; caller++) {
        power_request(0x84000008, caller);
        power_request(0x84000009, caller);
    }
    stop_during_cpu_on();
    halt_while_paused();
    printf("Guest shutdown/reset: %d failures\n", failures);
    return failures != 0;
}

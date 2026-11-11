/* When a caller relies on the machine being paused and it isn't (hv_machine_paused_counter: a Suspend or
   snapshot's "the machine is not paused"), the log says why in one line: the first CPU still able to run guest
   code and its gate state, or that no pause was asked for. Settled, it says nothing. */
#include "machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { NCPU = 3 };

static HvMachine m;
static HvCpu *cpus[NCPU];
static int failures;

static void check(const char *what, int ok, const char *log) {
    if (ok) return;
    printf("FAIL %s; the log said: \"%s\"\n", what, log);
    failures++;
}

/* hv_machine_paused_counter's result, and what it wrote to stderr. */
static int call(char *log, size_t len) {
    FILE *f = tmpfile();
    fflush(stderr);
    int saved = dup(2);
    dup2(fileno(f), 2);
    uint64_t counter;
    int r = hv_machine_paused_counter(&m, &counter);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    rewind(f);
    size_t n = fread(log, 1, len - 1, f);
    log[n] = 0;
    fclose(f);
    char *nl = strchr(log, '\n');
    if (nl && nl[1] == 0) *nl = 0;
    return r;
}

int main(void) {
    if (hv_machine_init(&m, 8u << 20) != 0) return 1;
    for (int c = 0; c < NCPU; c++) {
        if (posix_memalign((void **)&cpus[c], _Alignof(HvCpu), sizeof(HvCpu))) abort();
        memset(cpus[c], 0, sizeof(HvCpu));
        hv_cpu_reset_index(cpus[c], &m, (uint32_t)c);
        m.cpus[c] = cpus[c];
    }
    m.ncpus = NCPU;
    char log[512];

    int r = call(log, sizeof log);
    check("not paused: refused", r == -1, log);
    check("not paused: says no pause was asked for", !strcmp(log, "hengevm: pause: not settled: no pause requested"), log);

    m.pause_req = 1;
    for (int c = 0; c < NCPU; c++) cpus[c]->pc_state->exited = 0; /* all running (a new CPU starts as exited) */
    cpus[0]->pc_state->at_gate = 1;
    cpus[1]->off = 1, cpus[1]->parked_off = 1; /* powered off and parked: counts as settled */
    cpus[2]->pc_state->active = 1;             /* still in the dispatcher */
    r = call(log, sizeof log);
    check("one CPU running: refused", r == -1, log);
    check("one CPU running: names it and its state",
          !strcmp(log, "hengevm: pause: not settled: cpu 2 at_gate 0 exited 0 off 0 parked_off 0 active 1"), log);

    cpus[1]->parked_off = 0; /* turned off but not yet parked: it can still be in the guest */
    r = call(log, sizeof log);
    check("the first unsettled CPU is named", strstr(log, "cpu 1 at_gate 0 exited 0 off 1 parked_off 0") != NULL, log);

    cpus[1]->parked_off = 1;
    cpus[2]->pc_state->exited = 1;
    r = call(log, sizeof log);
    check("settled: the counter is read", r == 0, log);
    check("settled: nothing said", log[0] == 0, log);

    printf("%s\n", failures ? "test_pause_say FAILED" : "test_pause_say ok");
    return failures != 0;
}

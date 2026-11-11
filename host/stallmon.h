/* The freeze recorder: while a Mac runs, notes each moment its screen stalls (or Henge's own main thread does) and
   what every guest CPU and the main thread were doing then, one JSON line each in <dir>/henge-freezes.jsonl and a
   sentence in the native log. tools/freeze_report.py ranks what it finds. Off with HV_FREEZES=0 (henge-env.txt). */
#ifndef HV_STALLMON_H
#define HV_STALLMON_H

#include <stdint.h>

typedef struct HvStallHooks {
    int (*running)(void);         /* 1 while a Mac runs (paused or not) */
    double (*input_idle_s)(void); /* seconds since any input to the Mac; < 0 none yet */
    double (*press_idle_s)(void); /* seconds since a key, click or finger landing; very large if none yet */
} HvStallHooks;

/* Before the machine's CPU threads start (they register as they do). Once per process; later calls do nothing. */
void hv_stall_start(const char *dir, const HvStallHooks *hooks);
/* Records written this run. */
int hv_stall_records(void);
/* Ticks the recorder has run past its check for being held up (a "starved" record is written by then): a test waits
   for these, not for a time. */
unsigned hv_stall_ticks(void);
/* Threads it samples now (the main thread once registered, and each guest CPU thread until it ends). */
int hv_stall_threads(void);
/* For the benchmark (bench.c): what has been found this run, by stall_detect.h's kind (HV_STALL_HITCH ...), the
   records the file has no room for included, and how long they lasted together (ms); */
void hv_stall_found(unsigned counts[6], uint64_t *total_ms);
/* the time the guest CPU threads have had on a host CPU (s) between them, and how many there are now (0 while the
   recorder is off: HV_FREEZES=0); */
int hv_stall_cpu_time(double *seconds);
/* and the device's heat, as OSThermalPressureLevel's 0 (nominal) .. 4 (sleeping), -1 not known. */
int hv_stall_thermal(void);

#endif

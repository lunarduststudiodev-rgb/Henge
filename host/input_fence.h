/* The order the person's input reaches macOS in. The helper's scroll glide (share.c: "s" lines, run on the helper's
   own queue inside macOS) and everything else (keys, the pointer, clicks, the touchscreen: the Mac's HID devices)
   reach macOS by two ways with no order between them, so a click right after a fling could land a glide step or two
   from where the page stopped. A press (a button or a finger going down) that may follow scroll lines the helper hasn't
   posted yet sends it a numbered stop, and it and everything after it wait, in order, until the helper says that stop
   is done ("stopped <id>": every glide step is posted by then), the link goes, or HV_FENCE_LIMIT_MS passes (a stuck
   helper mustn't hold the Mac's input; it is then not waited on until it answers again). Nothing else is ever held.

   Every event the person makes goes through here (henge-vm.c), so nothing can overtake a held click. Any thread. */
#pragma once

#include <stdint.h>

#define HV_FENCE_LIMIT_MS 120
#define HV_FENCE_CONTACTS 10

typedef struct HvFenceEvent HvFenceEvent;
struct HvFenceEvent {
    void (*deliver)(const HvFenceEvent *); /* sends it to the Mac (checks the Mac can take it) */
    int press;                             /* a button or a finger going down: waits for the helper's glide to stop */
    int a, b, n;
    double x, y, dx, dy;
    struct { int id, down, cancel; double x, y; } contacts[HV_FENCE_CONTACTS];
};

/* Sends e now, or after what is held (e is copied). */
void hv_fence_send(const HvFenceEvent *e);
/* What is held goes now, without waiting (the Mac is about to pause or stop: what the person let go of must reach it
   first, or a button would stay down in macOS). why: for the log. */
void hv_fence_flush(const char *why);
/* Events held now (tests). */
int hv_fence_held(void);
/* How the fence tells whether the Mac stood still while a press waited (henge-vm.c: hv_vm_stops): a count that is odd
   while the Mac is stopped and goes up at every stop and every start. A helper that didn't answer in time is only
   marked as not answering (hv_share_touch_late), and the press let go, if the Mac ran the whole time: a stopped Mac
   can't answer, so the press waits on for a whole limit more. NULL: the Mac is taken to run. */
void hv_fence_set_stops(unsigned (*stops)(void));
/* The time limit, HV_FENCE_LIMIT_MS unless changed (tests: a slow test machine mustn't end holds early). */
void hv_fence_set_limit_ms(unsigned ms);

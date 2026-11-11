#ifndef HENGEVM_VM_H
#define HENGEVM_VM_H

#include <stddef.h>
#include <stdint.h>

#include "disk.h"
#include "vmapple.h"

/* The virtual Mac as a host app drives it: build it from files, run it, feed
   it input, stop it. The macOS command-line tool and the iOS app both use this;
   graphics are the host's own business (Mac: hv_pvg_attach before starting). */
typedef struct HvVmConfig {
    const char *rom;          /* AVPBooter.vmapple2.bin */
    const char *disk, *aux;   /* the root disk image and the auxiliary storage */
    uint64_t aux_offset;      /* where the aux image's data starts (16 KB) */
    uint64_t ecid, seed;      /* the machine identity the disk was installed with */
    uint64_t ram_mb;
    int ncpus;
    int log;                  /* m->log */
    int no_net, no_sound;
    int installer;            /* boot the ROM in DFU mode with the USB device port (installing macOS) */
    /* JIT memory the host already has (iOS: granted by a debugger), or NULL to map it here. */
    void *jit_rx, *jit_rw;
    size_t jit_bytes;         /* 0: 768 MB */
    int power_source;         /* HV_VM_POWER_*: the power source macOS sees (not on installer boots) */
    int rosetta;              /* the experimental Rosetta mode: 0 as HV_ROSETTA says (off unless set), 1 on (the Mac's
                                 own setting). hv_vm_restore puts the Mac back in the mode it was suspended in. */
} HvVmConfig;

/* What the Mac runs on, as Apple's virtual Macs do (devices/power_source.h): none (as before), a battery
   that hv_vm_set_power keeps up to date (the menu-bar battery, Battery settings, low-battery warnings),
   or wall power (macOS shows no battery, as on a desktop Mac). */
#define HV_VM_POWER_NONE 0
#define HV_VM_POWER_BATTERY 1
#define HV_VM_POWER_WALL 2
/* The host's battery: percent 0..100, external 1 on a charger, charging 1 while it fills, full 1 when the
   charger is in and it is full. Any thread, any time (before hv_vm_create too: the battery starts so);
   the Mac hears of a change within a tick. Until it is first called the battery reads full, on a charger. */
void hv_vm_set_power(double percent, int external, int charging, int full);

/* Returns 0, or -1 with a reason in err. */
/* Before hv_vm_create: the device's shared folder (read-write, called shared_name on the Mac, which mounts it by
   itself in "My Shared Files"), and the Henge Tools disk (a raw disk image, plugged in read-only as a USB drive). Either may be
   NULL. Not for installer boots. */
void hv_vm_set_folders(const char *shared_name, const char *shared, const char *tools_disk);
int hv_vm_create(const HvVmConfig *cfg, char *err, size_t errlen);
HvMachine *hv_vm_machine(void);
HvVmapple *hv_vm_board(void);
HvCpu *hv_vm_cpu(int index);
extern void (*hv_prof_register)(void); /* debugging: each thread that will run guest code calls this */
/* Frames go to this CAMetalLayer, drawn by the GPU, instead of hv_vm_boot_display. */
void hv_vm_set_display_layer(void *layer);
uint64_t hv_vm_gpu_frames(void);
/* How long each graphics stage has been held up, ms (devices/pvg_lite.h HV_PVG_STAGE_*), zeros before the board is up:
   for the freeze recorder, from any thread. */
void hv_vm_gfx_stages(uint32_t ms[5]);
/* 1 once macOS has shown its pointer: the login window or the desktop is up. */
int hv_vm_pointer_shown(void);
/* 1 once this OS's Metal has refused GPU code macOS sent (macOS 15 on iPadOS 17): the screen may stay black. */
int hv_vm_gpu_code_refused(void);
/* Where the Mac's graphics stand after a GPU failure, and the person's "Try Again" (devices/pvg_lite.h:
   hv_pvg_metal_gpu_state, hv_pvg_metal_gpu_try_again). Any thread. */
int hv_vm_gpu_state(void);
int hv_vm_gpu_try_again(void);
int hv_vm_ncpus(void);

/* The boot framebuffer iBoot hands the kernel (XNU's boot_args video info):
   the Apple logo and progress bar while booting or installing, before a GPU
   driver takes the display; then what the GPU path has drawn on the CPU, if
   anything. 32-bit BGRA, stride bytes a row, copied into to when cap holds
   stride * height (0); 1: only d filled in, to is too small; -1: nothing yet. */
typedef struct HvBootDisplay {
    uint32_t width, height, stride;
} HvBootDisplay;
int hv_vm_boot_display(HvBootDisplay *d, uint8_t *to, size_t cap);

/* Extra device work on CPU 0 each tick (Mac graphics). Set before hv_vm_start. */
void hv_vm_set_poll(void (*poll)(void));

/* Runs CPU 0 on a new thread (unless the caller runs it: cpu0 = 0), the other
   CPUs and the 1 ms kicker. on_exit is called on CPU 0's thread when the
   machine stops (powered off, a fatal error, or hv_vm_stop), after hv_vm_finish
   (every CPU's thread ended, the disks' requests done and flushed); reason is an
   HV_EXIT_* value, and hv_vm_halted_by says who stopped it. */
void hv_vm_start(int cpu0, void (*on_exit)(int reason));
/* The memory ready to run (kept in its file: emptied for a fresh start over a suspended Mac's, and no save
   left describing it). hv_vm_start does it too, and doesn't start if it fails; called first, a failure can be
   told to the person. 0, or -1. */
int hv_vm_prepare_memory(void);
/* Call on a thread before it runs guest code (hv_vm_start's threads do): see vm.c. */
void hv_vm_prepare_cpu_thread(void);
/* That, and the scheduling class the guest's CPUs run at. */
void hv_vm_cpu_qos(void);
/* Run CPU 0 on the calling thread (what hv_vm_start does on its own thread); returns its exit reason. */
int hv_vm_run_cpu0(void);
/* After hv_vm_run_cpu0 returns, on that thread: stop the other CPUs and join their threads, let the disks'
   workers finish the requests they have, and flush. hv_vm_start's own CPU 0 thread does it. */
void hv_vm_finish(void);
/* Once stopped: 1 macOS turned the Mac off, 2 macOS restarted it (PSCI SYSTEM_OFF / SYSTEM_RESET), 3 hv_vm_stop
   or an error that stopped CPU 0, 4 macOS reported a panic (pvpanic); 0 still running (core/cpu.h HvHaltReason). */
int hv_vm_halted_by(void);
/* Presses the Mac's power button, as a finger would (100 ms). macOS does what a real Mac's button does: with someone
   logged in it asks what to do (Restart, Sleep, Shut Down); at the login window it shows the same choice. Pressed
   again while macOS asks: nothing more. Any thread; the press happens on the Mac's next tick, and waits while the Mac
   is paused. */
void hv_vm_power_button(void);
/* Stop at the next block; the CPUs' threads end. Paused or not. */
void hv_vm_stop(void);
/* Pause: every CPU stops between instructions and the guest's clock stands still, sound stops, the
   disks' outstanding requests finish and everything written so far is put on storage (F_FULLFSYNC).
   Waits up to timeout_ms for the CPUs. 0 paused with everything on storage; 1 paused, but a disk
   could not be synced (or a request did not finish); -1 the CPUs did not stop in time (it runs on,
   as before); -2 not running. Any thread but a CPU's; one caller at a time with resume and stop. */
int hv_vm_pause(unsigned timeout_ms);
/* Go on from a pause, the guest's clock from where it stopped. */
void hv_vm_resume(void);
int hv_vm_paused(void);
/* Odd while the CPUs are stopped for a pause (or an auto-save's), or stopping; one more at every stop and every start:
   whether the Mac ran the whole time between two looks. */
unsigned hv_vm_stops(void);
void hv_vm_flush_disks(void);

/* Suspend: the whole Mac (its processors, memory and every device, what only the GPU holds included) saved
   to the file at path, to go on later from exactly there, in this process or a new one (hv_vm_restore).
   It pauses first (hv_vm_pause, timeout_ms for the CPUs) and is never worse than that: whatever happens the
   Mac stays paused, with everything its disks were sent on storage. The file is all or nothing (written
   aside, then renamed into place) and on storage before this returns 0. Resuming here (hv_vm_resume)
   deletes it first, since from the first instruction it is out of date.
   0 suspended (paused, the file complete); -1 not saved, the Mac paused as by hv_vm_pause (why says what
   went wrong; a file at path from before is gone); -2 not paused at all (as hv_vm_pause's -1 and -2).
   Blocks for the CPUs, the devices and the write: not the main thread. One caller at a time, as for pause. */
int hv_vm_suspend(const char *path, unsigned timeout_ms, char *why, size_t why_len);
/* A suspend that can be called off while it runs (the person came back and the Mac goes on): ticket is
   hv_vm_suspend_ticket() from when it was asked for; any hv_vm_suspend_cancel() since stops it at its next step
   (between each MB of RAM): -3, nothing saved (a file at path from before is gone), the Mac paused as by
   hv_vm_pause. Otherwise as hv_vm_suspend. */
int hv_vm_suspend_ticketed(const char *path, unsigned timeout_ms, uint64_t ticket, char *why, size_t why_len);
uint64_t hv_vm_suspend_ticket(void);
/* Storage the memory's part of a suspend may take, at most, were it made now (paused): the whole memory, or with
   the memory kept in files much less (what changed since they were last written). */
uint64_t hv_vm_suspend_room(void);
/* Any thread, at once: every suspend asked for before this (running or yet to start) stops. */
void hv_vm_suspend_cancel(void);
/* Paused, ahead of a suspend: the screen's state read now (what only the GPU holds into memory, at most
   limit bytes), for hv_vm_suspend to write later without the GPU (on iOS it can only be used until Henge has
   finished leaving the screen). Kept until hv_vm_resume. Waits up to timeout_ms for the GPU's work.
   0 read; -1 not (why says; the suspend reads the GPU itself, as without this); -2 not paused. */
int hv_vm_capture_gpu(uint64_t limit, unsigned timeout_ms, char *why, size_t why_len);
/* Restore: after hv_vm_create (with the same files, identity, CPUs and memory as when it was saved) and the
   USB drives the suspended Mac had plugged in again (hv_vm_plug_disk, same serial numbers and files),
   before hv_vm_start, which lets it run on from where it was saved. Refused unless the snapshot is complete
   and undamaged, from this engine, and of this Mac with its disks exactly as they were: a disk written since
   would be wrecked. The snapshot is deleted before anything runs, so it can only ever be used once.
   0 restored; -1 refused, nothing changed (the snapshot stays as it was); -2 failed partway: this machine
   can't run (hv_vm_start does nothing) and the process must start again for another; the snapshot stays
   unless it was the deletion that failed. why: a sentence for the person. */
int hv_vm_restore(const char *path, char *why, size_t why_len);
/* Auto-save: the running Mac saved as hv_vm_suspend_ticketed saves it, its two disks cloned at the same instant
   (disk_to: the disk image, aux_to: the whole auxiliary file; APFS clones, so nothing is copied and the live disks
   moving on later leave them as they are), and the snapshot naming those clones (so it is still of its disks when
   the live ones have moved on). Then the Mac goes on, the snapshot kept (hv_vm_resume deletes only a suspend's).
   Its stop is a pause to the guest (the Mac is stopped for the whole save): taken off its clock, and macOS told it
   woke.
   Refused (-2, nothing done) unless the Mac runs as the person left it: not paused, not an installer, not
   suspended, its memory kept in files that can be copied (it never writes the memory whole: a minute's stop on the
   screen; it fails instead). The check and the pause are one step: a pause can't come between and be undone.
   0 kept; -1 not (why says); -3 cancelled (hv_vm_suspend_cancel); -2 not paused at all. On anything but 0 nothing
   is left at path, disk_to or aux_to. The Mac runs on after it, unless hv_vm_autosave_hold was called since
   hold_ticket (hv_vm_autosave_hold_ticket, read when it was asked for): then it stays paused, as a pause leaves it,
   for whatever held it (a pause, a Suspend, leaving the screen) to have from this very instant. Blocks for the
   pause and the write: not the main thread. One caller at a time with pause, resume, suspend. */
int hv_vm_autosave(const char *path, const char *disk_to, const char *aux_to, unsigned timeout_ms, uint64_t ticket,
                   uint64_t hold_ticket, char *why, size_t why_len);
uint64_t hv_vm_autosave_hold_ticket(void);
/* Any thread: an auto-save under way finishes, and leaves the Mac paused rather than running. */
void hv_vm_autosave_hold(void);
/* Whether an auto-save could be taken this run: the memory is kept in files, and they can be copied. */
int hv_vm_memory_copyable(void);
/* Going on from an auto-save: hv_vm_create built on its clones of the disks, then this instead of hv_vm_restore.
   After everything loaded, before anything runs, each clone is put in the Mac's own place as a file of its own
   (disk_to, aux_to; host/disk.h hv_disk_place), and the machine runs on those: the disk that was there is kept at
   disk_aside (NULL: it goes), unless nothing was written to it since the auto-save. The auto-save itself is left as
   it is, to go on from again. As hv_vm_restore otherwise: 0 restored; -1 refused, nothing changed; -2 failed
   partway (this machine can't run). */
int hv_vm_restore_autosave(const char *path, const char *disk_to, const char *disk_aside, const char *aux_to, char *why,
                           size_t why_len);
/* A machine built and not run (an auto-save's restore refused it, nothing changed): its disks reopened at these
   paths (the same sizes), so it can start afresh on the Mac's own files. 0, or -1. */
int hv_vm_use_disks(const char *disk_path, const char *aux_path);
/* Before hv_vm_create for a restore: whether the Mac saved at path had a power source. 1 yes, 0 no (saved by a
   Henge from before the battery: build it with HV_VM_POWER_NONE, so its PCI slots are the saved ones and it
   resumes; the battery is there from its next start), -1 unreadable (the restore will say why). */
int hv_vm_snapshot_has_power(const char *path);

/* USB drives the Mac sees arrive and leave while it runs, as a USB stick or external SSD (up to four).
   Any thread, once hv_vm_create has built the machine; not on installer boots.
   plug: the raw disk image at path (read-only if readonly, or if the file can't be written); name is what
   the Mac calls the device, serial its USB serial number. The port it went into (2..5), -1 the file can't
   be opened, -2 four are plugged in already, -3 no machine. unplug pulls it out at once: eject it on the
   Mac first (or it reads as ejected) so nothing it holds is lost. state: an HV_XHCI_DRIVE_* value. */
#define HV_VM_TOOLS_SERIAL "HENGE-TOOLS"
int hv_vm_plug_disk(const char *path, int readonly, const char *name, const char *serial);
void hv_vm_unplug_disk(int port);
int hv_vm_disk_state(int port);
int hv_vm_disk_readonly(int port);
/* The port (2..5) the drive with this serial number is in or on its way into, or -1: after a restore, which
   puts each drive back in the port it was saved in, whatever port plugging it in first gave. */
int hv_vm_disk_port(const char *serial);
const char *hv_vm_reason_name(int reason);

/* The host's own network, as the host sees it (NWPathMonitor): 1 when it can reach anything, 0 when it can't
   (airplane mode, Wi-Fi gone). The Mac's Ethernet follows it like a cable plugged into the host: macOS shows
   "Cable Unplugged" a few seconds after the host loses its network, and plugs back in and asks for an address a
   second after it returns (virtio_net.h). Any thread, before hv_vm_create or after; never called: always in. */
void hv_vm_host_network(int up);

/* Input from the host UI, any thread. Keys are Linux input codes (KEY_A = 30). */
void hv_vm_key(uint16_t code, int down);
void hv_vm_key_hid(uint8_t usage, int down); /* HID keyboard usage, which is what iOS reports for a hardware keyboard */
void hv_vm_modifiers_up(void); /* ⇧, ⌃, ⌥ and ⌘ up, whoever holds them */

/* The Mac's speaker: a USB audio device whose volume is the host's, as a Bluetooth speaker's is (host/audio_usb.h).
   Its name on the Mac, before hv_vm_create (NULL or "": "Henge Speaker"). */
void hv_vm_set_speaker_name(const char *name);
/* Called on a CPU thread whenever macOS sets the speaker's volume (its slider, Control Center, the volume keys):
   level 0..1 (the slider's position), and mute. iOS: set the iPad's own volume to it. */
void hv_vm_set_speaker_listener(void (*cb)(void *opaque, double level, int mute), void *opaque);
/* The host's own volume changed (the iPad's, 0..1): macOS is told and moves its slider, without a display. */
void hv_vm_speaker_host_volume(double level);
/* A volume button pressed (1 up, -1 down, 0 mute): macOS steps its volume and shows its own volume display. */
void hv_vm_speaker_host_key(int key);
/* The speaker's volume now (level 0..1, mute); -1 before the Mac is built. */
int hv_vm_speaker_volume(double *level, int *mute);
void hv_vm_mouse_move(int dx, int dy);
void hv_vm_mouse_button(int button, int down); /* 0 left, 1 right, 2 middle */
void hv_vm_mouse_wheel(int dy);
void hv_vm_mouse_hwheel(int dx);
/* The absolute pointer (touch, Pencil): x and y from 0 at the left/top edge of the
   Mac's screen to 1 at the right/bottom. A press or release lands where it says, in
   the same report. Ready once macOS's driver has taken the device (not in recoveryOS
   before it loads, nor with HV_NO_TABLET). */
int hv_vm_touch_ready(void);
void hv_vm_touch(double x, double y);
/* Where the pointer starts (0..1 of the screen), instead of the touch device's top-left corner: sent once the
   guest can take it, until macOS shows its pointer or a real touch or mouse move comes. */
void hv_vm_park_pointer(double x, double y);
void hv_vm_touch_button(double x, double y, int button, int down); /* 0 left, 1 right, 2 middle */
/* Hides the pointer on the screen while a finger is the pointer, as a touch Mac does; the relative
   mouse (hv_vm_mouse_*) shows it again as soon as it moves. Any thread. */
void hv_vm_hide_pointer(int hide);
/* Where the guest has its pointer, in display pixels. 0, or -1 before it has one. */
int hv_vm_pointer(int *x, int *y);
/* The Mac's screen in the pixels hv_vm_pointer uses: the display mode macOS runs, which a scaled
   resolution is stretched over. 0, or -1 before it shows anything. */
int hv_vm_screen_size(int *w, int *h);
/* The same screen in macOS's points: what its scrolling and its windows are measured in. */
int hv_vm_screen_points(int *w, int *h);
/* A small picture of the display, BGRA, width * height * 4 bytes. 0, or -1 before anything shows. */
int hv_vm_snapshot(uint8_t *bgra, int width, int height);

/* The touchscreen: on macOS 27 and later, with Henge Tools, a USB touchscreen macOS takes as its own (touch
   events, its gestures), plugged in on its own once the helper says macOS is ready for it. Older macOS never
   sees it; the absolute pointer above is then what touch drives.
   hv_vm_touchscreen_config: allowed or not (default allowed), and the touched screen's size in millimetres
   (what macOS is told; set it before it's plugged in). Any thread. */
void hv_vm_touchscreen_config(int enabled, int w_mm, int h_mm);
/* Native (Always): the touchscreen is used whenever it is plugged in and set up, with no check that macOS responds to
   its touches, and never swapped for the pointer by Henge itself. 0 (the default): Native (Auto). Any thread. */
void hv_vm_touchscreen_always(int always);
/* devices/xhci.h's HV_XHCI_TOUCH_*: 0 absent, 1 plugged in, 2 macOS reads it, 3 unavailable this run. */
int hv_vm_touchscreen_state(void);
/* 1 while touch should go through it: macOS reads it, and the logged-in user's helper says macOS has it. */
int hv_vm_touchscreen_usable(void);
/* As a gesture starts on the touchscreen: has the helper check that macOS does something with it. If it doesn't, the
   touchscreen stops being usable (and is pulled out) until the Mac restarts: touch is imitated with the pointer. */
void hv_vm_touchscreen_watch(void);
/* For the app to say what touch is doing. */
enum {
    HV_VM_TS_NONE = 0,    /* this Mac doesn't take one (older macOS, no Henge Tools, not set up yet) */
    HV_VM_TS_OFF = 1,     /* the app turned it off */
    HV_VM_TS_WAITING = 2, /* plugged in or about to be; macOS hasn't taken it (yet), or the helper hasn't said */
    HV_VM_TS_TRYING = 3,  /* fingers go to it; whether macOS does anything with them isn't known yet */
    HV_VM_TS_WORKS = 4,   /* fingers go to it, and macOS was seen to take them */
    HV_VM_TS_IGNORED = 5, /* macOS did nothing with its touches: it's out, and touch is imitated with the pointer */
};
int hv_vm_touchscreen_status(void);
/* One frame: every finger down (id 0..9, the same while it touches), and each lifted since the last frame
   once with down 0 (cancel 1 if it didn't end as a touch). x and y as for hv_vm_touch. 0, or -1 if the
   touchscreen can't take it (not ready). Any thread. */
typedef struct HvVmContact {
    int id, down, cancel;
    double x, y;
} HvVmContact;
int hv_vm_touchscreen_frame(int n, const HvVmContact *c);

#endif

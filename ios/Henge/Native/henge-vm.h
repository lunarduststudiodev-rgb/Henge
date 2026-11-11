/* The engine (HengeVM, ../host/vm.h) as the Swift side sees it. */
#ifndef HENGE_VM_H
#define HENGE_VM_H

#include <stddef.h>
#include <stdint.h>

/* Builds the machine from imported files, on the JIT region the app holds
   (henge_ios_jit_prewarm), and starts it. 0, or -1 with a reason in err. */
int henge_vm_boot(const char *rom, const char *disk, const char *aux, uint64_t ecid, uint64_t seed, uint64_t ram_mb,
                  int cpus, char *err, size_t errlen);
/* Installs macOS from an IPSW onto a new machine (blank disk and auxiliary
   storage): boots the ROM in DFU mode and runs the restore over its USB port,
   as Virtualization.framework's installer does. workdir is scratch space.
   0, or -1 with a reason in err; progress below. */
int henge_vm_install(const char *rom, const char *disk, const char *aux, uint64_t ecid, const char *ipsw,
                     const char *workdir, uint64_t ram_mb, int cpus, char *err, size_t errlen);
/* The restore's step (0 detect, 1 prepare, 2 system image, 3 verify, 4 firmware,
   6 FUD, 7 personalized images; 100 done, 101 failed) and how far it is (0..1). */
int henge_install_step(void);
double henge_install_progress(void);
/* Writes a blank auxiliary storage file (what VZMacAuxiliaryStorage makes). 0 or -1. */
int henge_make_aux(const char *path);

/* The iPad's battery, which the Mac shows as its own (menu bar, Battery settings, low-battery warnings):
   percent 0..100, charger 1 when plugged in, charging 1 while it fills, full 1 when plugged in and full.
   Any thread, any time, before henge_vm_boot too. Until it is first called the Mac's battery reads full,
   on a charger. iOS's Low Power Mode has no way through: macOS keeps its own. */
void henge_vm_set_power(double percent, int charger, int charging, int full);

/* The experimental Rosetta mode (Intel apps on the Mac), the Mac's own setting: on 1 or off 0 for the next
   henge_vm_boot. A suspended Mac goes on in the mode it was suspended in (henge_vm_boot_saved); a fresh start after
   that takes this. Off unless set (or HV_ROSETTA in henge-env.txt). */
void henge_vm_set_rosetta(int on);
/* 1 if this device can run the Rosetta mode (its chip has FEAT_AFP: M2, A15 and later), else 0. */
int henge_vm_rosetta_supported(void);

/* 0 not started, 1 running, 2 stopped. */
int henge_vm_state(void);
/* Why it stopped (an HV_EXIT_* name), once state is 2. */
const char *henge_vm_exit_reason(void);
/* Who stopped it, once state is 2: 1 macOS shut down, 2 macOS restarted, 3 Henge stopped it (or an error stopped
   it: then the exit reason isn't HALT), 4 macOS reported a panic; 0 while it runs (host/vm.h hv_vm_halted_by). */
int henge_vm_halted_by(void);
void henge_vm_stop(void);
/* Pause: the Mac stops between instructions with its clock standing still, sound off, and everything its
   disks were sent on the iPad's storage. 0 paused and safe; 1 paused, but the disk could not be synced;
   -1 it did not stop in time (it runs on); -2 not running. Blocks up to timeout_ms plus the disk sync:
   not on the main thread. */
int henge_vm_pause(unsigned timeout_ms);
void henge_vm_resume(void);
int henge_vm_paused(void);
/* Before a pause, while the Mac runs: the Henge helper has macOS write what it holds for its disks.
   0 done; -1 no helper of this version, or no answer in timeout_ms. Blocks: not on the main thread. */
int henge_share_sync_guest(unsigned timeout_ms);

/* Suspend: the whole Mac saved to path (memory, processors, every device, what only the GPU holds) to go on
   later from exactly there, in this run or after iOS ends Henge (host/vm.h's hv_vm_suspend). Never worse than a
   pause: whatever happens the Mac is left paused, with everything its disks were sent on storage. 0 suspended,
   the file complete and on storage; -1 not saved, paused all the same (why says what went wrong; a file at path
   from before is gone); -2 not paused (as henge_vm_pause's -1 and -2). henge_vm_resume goes on from there and
   deletes the file, out of date from the first instruction. Blocks: not on the main thread.
   ticket: henge_vm_suspend_ticket() when it was asked for. A henge_vm_suspend_cancel() since stops it (even
   before it starts, between each MB of memory once under way): -3, nothing kept, the Mac paused. */
int henge_vm_suspend(const char *path, unsigned timeout_ms, uint64_t ticket, char *why, size_t why_len);
/* Auto-save (host/vm.h hv_vm_autosave): the running Mac saved at path with its disks cloned to disk_to and aux_to,
   and it goes on (unless henge_vm_autosave_hold was called since hold_ticket: then it stays paused). Input sent
   meanwhile waits for it. 0 kept; -1 not (why); -3 cancelled; -2 not taken (not running as the person left it). */
int henge_vm_autosave(const char *path, const char *disk_to, const char *aux_to, unsigned timeout_ms, uint64_t ticket,
                      uint64_t hold_ticket, char *why, size_t why_len);
uint64_t henge_vm_autosave_hold_ticket(void);
void henge_vm_autosave_hold(void);
/* 1 if an auto-save could be taken this run (the Mac runs, its memory in files that can be copied). */
int henge_vm_autosave_possible(void);
/* 1 once the machine has stopped because macOS turned the Mac off or restarted it (not a failure, not a stop). */
int henge_vm_stopped_by_macos(void);
/* 1 if macOS had already turned the Mac off or restarted it when Henge stopped it (a stop keeps the first reason):
   known as soon as henge_vm_stop returns, before the machine has finished stopping. */
int henge_vm_halted_by_macos(void);
/* Going on from an auto-save: as henge_vm_boot_saved, built on its clones; once loaded, copies of them take the
   places of disk and aux (the disk that was there kept at disk_aside, unless nothing was written to it since), and
   the auto-save stays as it is. -1 refused, nothing changed, the machine now on the Mac's own files
   (henge_vm_start_fresh can start it); -4 refused, and those couldn't be opened: a fresh start needs Henge opened
   again; -2 failed partway; -3 not built. */
int henge_vm_boot_autosaved(const char *rom, const char *disk, const char *aux, const char *disk_clone,
                            const char *aux_clone, const char *disk_aside, uint64_t ecid, uint64_t seed, uint64_t ram_mb,
                            int cpus, const char *snapshot, char *err, size_t errlen);
/* Seconds since the person's last key, click, pointer move or touch to the Mac; -1 none yet. */
double henge_vm_input_idle_s(void);
uint64_t henge_vm_suspend_ticket(void);
/* Storage in MB the Mac's memory may take in a suspend made now (paused): the whole memory, or much less when it is
   kept in files (host/vm.h's hv_vm_suspend_room; 0 when nothing changed). UINT64_MAX when no Mac runs. */
uint64_t henge_vm_suspend_room_mb(void);
/* Any thread, returns at once: every suspend asked for before now stops at its next step. */
void henge_vm_suspend_cancel(void);
/* Paused, on leaving the screen: the screen's state read now, while iOS still lets Henge use the GPU (only until
   Henge has finished leaving), for a henge_vm_suspend after it. At most limit bytes, held in memory until the
   Mac goes on. 0 read; -1 not (why says; the suspend then tries the GPU itself); -2 not paused. Blocks up to
   timeout_ms: not on the main thread. */
int henge_vm_capture_gpu(uint64_t limit, unsigned timeout_ms, char *why, size_t why_len);
/* Before henge_vm_boot_saved: a USB drive the suspended Mac had in, to be in again as it was (the same file,
   untouched since, name, serial and read-only or not). Up to four. 0, or -1. */
int henge_vm_preplug(const char *path, int readonly, const char *name, const char *serial);
/* Goes on from a snapshot henge_vm_suspend made, after iOS ended Henge: builds the machine as henge_vm_boot does
   (henge_vm_folders, henge_vm_preplug and the display settings first, with the Henge Tools disk the very file it
   had: same inode and time), puts it back as it was and starts it.
   0 running on from the snapshot, which is deleted (it can only be used once).
   -1 refused before anything was loaded, the snapshot as it was (err: a sentence for the person, such as a disk that changed
      or another version of Henge). The machine is built: henge_vm_start_fresh starts macOS afresh, after which
      the snapshot is of no use (delete it).
   -2 nothing started and this run of Henge can't start a machine (err says why): quit Henge and open it again.
   -3 the machine couldn't be built (no JIT, a file it can't open: err says why, as henge_vm_boot's -1): the
      snapshot wasn't tried and is as it was. */
int henge_vm_boot_saved(const char *rom, const char *disk, const char *aux, uint64_t ecid, uint64_t seed, uint64_t ram_mb,
                        int cpus, const char *snapshot, char *err, size_t errlen);
/* After henge_vm_boot_saved's -1: starts the machine it built, as henge_vm_boot would have. 0, or -1. */
int henge_vm_start_fresh(void);
/* 1 while the running machine is one henge_vm_boot_saved went on from a snapshot; 0 for a fresh start. */
int henge_vm_resumed(void);
/* After henge_vm_boot_saved's -1, when the person keeps the snapshot for later instead: the machine built is let
   go unstarted (henge_vm_state says 2), so the next Mac needs a fresh Henge, as after any other. */
void henge_vm_drop_built(void);
/* Deletes a snapshot (and an unfinished save of it), the deletion on storage before it returns. 0 when there is
   none any more, -1 if it couldn't be deleted. */
int henge_vm_discard_saved(const char *path);

/* Suspend lab (Phase 0): measurements for the Suspend plan, as "lab:" lines in henge-native.log. After a pause
   that succeeded, on the pause queue: the graphics inventory and readback, the RAM scan and LZ4 sample, and with
   write_test, about 1 GB written to dir and read back (then deleted). After each resume: the network and clock. */
void henge_lab_paused(int background, int write_test, const char *dir);
void henge_lab_resumed(void);
/* Testing: sets a translation knob (core/machine.h) in the running machine; nonzero if it exists. */
int henge_vm_knob(const char *name, int value);
/* Microseconds an idle CPU watches for work before it sleeps (HV_WFI_SPIN), changed while a Mac runs: read at each
   sleep, so nothing is translated again for it (as henge_vm_knob would have). */
void henge_vm_idle_spin(int us);
/* The memory balloon (only with HV_BALLOON set before boot): asks the guest to hand over target_mb (when >= 0)
   and returns how many MB it has handed over, or -1 without a balloon. */
long henge_vm_balloon(long target_mb);
/* Low memory (MemoryWatch.swift): the engine gives translation tables it isn't using back to iPadOS. now: a full flush at
   once, and all of them (every block is translated again after it: on a busy Mac, seconds stopped); otherwise those of the
   next zone of the code cache emptied as it fills (an eighth, core/dispatch.c make_room). Returns the trims done before
   it, or -1 when no Mac runs. henge_vm_trims: the trims done so far, and the bytes the last gave back. */
long henge_vm_trim_request(int now);
long henge_vm_trims(uint64_t *released);

/* Progress, for the session screen. */
uint64_t henge_vm_blocks(void);            /* blocks the CPUs entered */
/* Code the engine has translated so far. A Mac going on from a suspend runs in a new process, so macOS's code is
   all translated afresh: while that goes on fast, macOS barely answers. When it slows, macOS has woken. */
uint64_t henge_vm_translations(void);
/* The share of the CPUs' time asleep (WFI) since the last call with these (0 to 1), or -1 the first time. */
double henge_vm_idle(uint64_t *last_ticks, uint64_t *last_at);
uint64_t henge_vm_net_frames(int sent);    /* Ethernet frames out (sent = 1) and in */
/* The network since the last call, one line for the log (host/net_slirp.h's hv_net_report): 1 when any frame
   went either way in that time, else 0. */
int henge_vm_net_report(char *buf, size_t n);
/* The iPad's own network (NWPathMonitor): 1 when it can reach anything, 0 when it can't. The Mac's Ethernet follows
   it as a cable plugged into the iPad would (host/vm.h's hv_vm_host_network). Any thread, before a boot or after. */
void henge_vm_host_network(int up);
int henge_vm_cpus_on(void);                /* CPUs the guest has started */
int henge_vm_guest_el0(void);              /* any CPU running user code right now */
/* The Mac's network since it started: frames lost on the way to it, from it and as too long, and TCP resets
   (host/net_slirp.h's hv_net_losses). */
void henge_vm_net_losses(uint64_t *dropped_to_mac, uint64_t *dropped_from_mac, uint64_t *too_long, uint64_t *resets);
/* The Mac's ports on the network (host/net_forward.h). hold 1: none open on the device while Henge is off the screen
   or the Mac is paused (iOS takes a suspended app's listeners back); 0: open again. Any thread. */
void henge_vm_forwards_hold(int hold);
/* One entry per port the Mac wants open on the device: its number, 1 for UDP, and 0 if open, else the errno that
   keeps it from being the Mac's (EADDRINUSE: the iPad itself uses it; EACCES or EPERM: iOS keeps it). *helper: 1 when
   the list is what Henge Tools says the Mac listens on, 0 when it is the standard sharing ports; *held: 1 while
   held. Returns how many there are; at most max are written. */
int henge_vm_forward_status(uint16_t *ports, uint8_t *udp, int *err, int max, int *helper, int *held);
/* The Mac's name on the network (host/mdns.h): "My-Mac.local", "" while it has none; *taken 1 when another
   device has the Mac's name, so it is withdrawn until the Mac takes another (as a Mac does); *services how many of
   its services are announced; *denied 1 when Henge's Local Network access is off; *unlisted how many services iOS
   refused for their type; *seen how many of the network's devices the Mac is told of. */
void henge_vm_mdns_status(char *name, int cap, int *taken, int *services, int *denied, int *unlisted, int *seen);
/* Those devices' names, one a line. Returns how many. */
int henge_vm_mdns_seen(char *list, int cap);

/* Input: Linux key codes, relative pointer, buttons 0 left 1 right 2 middle. */
/* The shared folder and clipboard (host/share.c). henge_vm_share goes before henge_vm_boot; agent may be NULL.
   henge_share_take_clipboard returns what the guest copied, once: a malloc'd string, or NULL. */
void henge_vm_share(const char *folder, const char *socket_path, const char *agent);
/* Where this Mac's system key is pinned (its SHA-256, host/share.c): the helper's system part proves with it that it is
   root, and only its list of the Mac's ports is taken. In the Mac's own folder: a guest disk wiped or macOS put in
   again keeps it, and the first link on the next fresh start takes the new key. Before henge_vm_boot. */
void henge_vm_system_pin(const char *path);
/* Before henge_vm_boot: the folder macOS mounts by itself in "My Shared Files" (virtio-fs), which it calls
   shared_name ("iPad"), and the Henge Tools disk, plugged in read-only as a USB drive. Either may be NULL. */
void henge_vm_folders(const char *shared_name, const char *shared, const char *tools_disk);
/* A new disk for the Mac (henge-disk.c): a sparse file of bytes (64 MB to 2 TB) with an empty FAT32 file system
   called name, which macOS mounts at once. path must not exist. 0, or -1 (nothing left behind). */
int henge_make_disk(const char *path, uint64_t bytes, const char *name);
/* Unpacks a gzipped disk image into dest, replacing what was there all at once. 0, or -1 (dest untouched). */
int henge_unpack(const char *archive, const char *dest);
void henge_share_set_clipboard(const char *text);
char *henge_share_take_clipboard(void);
long henge_share_agent_seen(void);
/* The helper's touch link (host/share.h): what is under a finger, and scrolling as a trackpad does.
   hit returns an id (0 without the link); answer is 1 drag, 0 scroll, -1 not yet. scroll's phase is
   1 began, 2 changed (dx, dy: fractions of the screen), 4 ended (dx, dy: screens a second). */
int henge_share_touch_ready(void);
int henge_share_touch_linked(void);
/* 1 while the helper on the Mac is this build's version: Henge Tools is up to date. */
int henge_share_agent_current(void);
/* The benchmark (host/bench.h), asked by the autopilot's "bench": 1 while this build's own helper is linked; the
   video it plays (the app's henge-bench.mp4), before the Mac starts; a run of steps ("all" or names with commas
   between) called label on the build named build, into Documents/henge-bench.jsonl: 0 asked, -1 not (why says);
   the last run's state (0 none, 1 running, 2 done, 3 failed); and a run under way ended as failed. */
int henge_share_can_bench(void);
void henge_vm_bench_video(const char *path);
int henge_vm_bench(const char *label, const char *steps, const char *build, char *why, size_t why_len);
int henge_vm_bench_state(void);
void henge_vm_bench_give_up(const char *why);
/* 1 while a helper that can eject drives and open its installer is linked (an out-of-date one may). */
int henge_share_can_eject(void);
/* 1 while a helper that can set a new drive up as a Mac disk (APFS) is linked. */
int henge_share_can_format(void);
/* What has the Mac's keyboard focus: -1 unknown (no helper that says it linked, as before login),
   0 nothing to type into, 1 a text field or view, 2 a password field. */
int henge_share_focus(void);
/* What the Mac's screen is at, as Henge Tools' system part says: -1 unknown (it isn't linked: not installed, or
   the Mac still starting), 0 macOS starting, 1 the login window, 2 someone's desktop (locked or not). */
int henge_share_session(void);
/* Where the focused field is, as fractions of the Mac's screen from its top left: 1 if known, else 0. */
int henge_share_focus_frame(float *x, float *y, float *width, float *height);
/* The text around the Mac's caret in its focused field (hv_share_context): base64, "" for none. */
unsigned henge_share_context(char *before, int before_cap, char *after, int after_cap, int *selected, int *known, int *kind);
void henge_share_touch_ask(void);
/* The helper opens the Henge Tools installer, waiting up to a minute for the disk: send this, then plug it in. */
void henge_share_tools_open(void);
/* The helper ejects the USB drive with this serial as Finder does; answer: 1 ejected, 0 refused (in use) or not
   there, -1 not yet. format: sets up a new all-zero drive as APFS called name (never one with anything on it);
   answer the same way. Both need the helper of this version (henge_share_agent_current). */
void henge_share_eject(const char *serial);
int henge_share_eject_answer(const char *serial);
/* Shut Down and Restart as the Apple menu does them, through the logged-in user's helper (share.h's hv_share_power):
   an id, or 0 if no helper that can is linked. The answer: -1 waiting, 0 failed, 1 sent, 2 an app stopped it,
   3 macOS is asking the person on the Mac's screen to allow it (the first time). */
unsigned henge_share_power(int restart);
int henge_share_power_answer(unsigned id);
/* The Mac's power button, pressed as a finger would: macOS asks what to do (Restart, Sleep, Shut Down). */
void henge_vm_power_button(void);
void henge_share_format(const char *serial, const char *name);
int henge_share_format_answer(const char *serial);
unsigned henge_share_touch_hit(double x, double y);
int henge_share_touch_answer(unsigned id);
void henge_share_touch_scroll(int phase, double x, double y, double dx, double dy);
/* Stops the helper's glide, early: a click after it (henge_vm_mouse_button and the others) waits until the helper
   says it has, so it lands where the page stopped (input_fence.h). */
void henge_share_touch_stop(void);
/* Once a hit is answered: 1 and the frame (fractions of the screen) if a button that shows itself pressed while it
   is touched is under the finger. */
int henge_share_touch_press(unsigned id, float *x, float *y, float *w, float *h);
/* How many pop-up menus are showing: ask, then the count once answered (-1 until then; ask returns 0 without a
   helper of this version). */
unsigned henge_share_menu_ask(void);
int henge_share_menu_answer(unsigned id);
/* Closes the pop-up menus showing with their own Cancel action (no key pressed). The answer (menu_answer) is how
   many it closed, 0 if it couldn't. Returns 0 without a helper of this version. */
unsigned henge_share_menu_close(void);
/* 1 while a glide is quick enough that a finger landing on it only stops it. */
int henge_share_touch_gliding(void);
/* The Mac's keyboard focus as the helper last said it in one numbered line (hv_share_field, helper 28): serial goes
   up with each line, id with each change of the focused element (the same field keeps it), generation with each link
   that opens or closes. state: -1 unknown (the helper can't tell, or none of this version is linked: nothing is
   known to have changed), 0 nothing to type into, 1 a text field, 2 a password field. kind: 0 prose, 1 code,
   2 an address bar, 3 plain (an email address, a user name). width 0: where it is wasn't said.
   Returns 1 while a helper of this version is linked. */
typedef struct {
    unsigned serial, id, generation;
    int state, kind;
    float x, y, width, height;
} HengeField;
int henge_share_field(HengeField *field);
/* What a tap on the touchscreen at x, y (0..1) lands in: an id (0 without a helper of this version) for
   tap_answer, as is a hit's. tap_answer: -2 not yet, -1 the helper couldn't tell, 0 nothing to type into,
   1 something that takes typing, 2 a password field; *x.. its frame (width 0 if not said), role its role. */
unsigned henge_share_tap(double x, double y);
int henge_share_tap_answer(unsigned id, float *x, float *y, float *width, float *height, char *role, int role_cap);
/* Has the helper look at the focus (hv_share_look, helper 34), as the password's Return is due, once every key and
   click before it has reached the Mac's keyboard and pointer (macOS may not have acted on them yet): an id for look_answer, 0 without a helper of that version. look_answer: -2 not yet, -1 the helper couldn't tell (or the link
   closed), 0 nothing to type into, 1 a text field, 2 a password field; *sure 1 only for one sure to be a password
   field (the field says so, or macOS's own prompt has the keys), not one taken from its app holding secure input. */
unsigned henge_share_look(void);
int henge_share_look_answer(unsigned id, int *sure);
/* Called on the link's thread after each field, tap or look answer and as the link opens or closes: hop to your own
   queue to read them. NULL for none. */
void henge_share_set_listener(void (*listener)(void));

/* USB drives the Mac sees arrive and leave while it runs (up to four), once henge_vm_boot has built it.
   plug: a raw disk image (read-only if readonly, or if it can't be written); name is what the Mac calls the
   device, serial its USB serial number (letters, digits and dashes). The port (2..5), -1 the file can't be
   opened, -2 four are in already, -3 no machine. unplug pulls it out at once: have it ejected first.
   state: 0 empty, 1 plugged in (the Mac hasn't taken it yet), 2 in use, 3 ejected on the Mac, 4 leaving. */
int henge_vm_plug_disk(const char *path, int readonly, const char *name, const char *serial);
void henge_vm_unplug_disk(int port);
int henge_vm_disk_state(int port);
int henge_vm_disk_readonly(int port);
/* The port the drive with this serial number is in (or on its way into), or -1. After henge_vm_boot_saved a
   drive may be in another port than plugging it in gave: the one it was in when the Mac was suspended. */
int henge_vm_disk_port(const char *serial);

/* The size macOS is told its screen is. Before henge_vm_boot. */
void henge_vm_set_display(uint32_t width, uint32_t height);
/* The panel's physical size; 0, 0 for the default (a ~122 ppi, 1x screen). Before boot. */
void henge_vm_set_display_mm(uint32_t width_mm, uint32_t height_mm);
/* The mode macOS will start in, from the two above, and its pixels per point (1 or 2). -1 before they are set. */
int henge_vm_start_mode(uint32_t *width, uint32_t *height, uint32_t *scale);
/* The picture macOS shows now, in its own pixels: what the screen fits to the layer. 0, or -1 before. */
int henge_vm_picture_size(int *width, int *height);
void henge_vm_key(uint16_t code, int down);
void henge_vm_key_hid(uint8_t usage, int down); /* a hardware keyboard: iOS gives each key's HID usage */
/* A key Henge types for the person (TypingQueue: dictation, the password, a paste): not a press of theirs. */
void henge_vm_key_typed(uint16_t code, int down);
/* ⇧, ⌃, ⌥ and ⌘ up on the Mac, whoever holds them (the row, a hardware keyboard): before a key Henge types, so a
   modifier the person holds doesn't make it a shortcut. Nothing is sent when none is down. */
void henge_vm_modifiers_up(void);
/* Keys pressed on the Mac so far, from either: a password typed at the login window shows as some. */
unsigned henge_vm_keys_pressed(void);
/* Clicks and taps on the Mac so far, from a pointer or a finger: someone using a Mac that logged in by itself. */
unsigned henge_vm_clicks(void);
/* Seconds since the person last pressed something on the Mac (a key, a click, a finger landing; not a move), huge
   before they have. */
double henge_vm_press_idle_s(void);
/* When that was, in mach_absolute_time's ticks (an audio buffer's host time is in them too); 0 before they have. */
uint64_t henge_vm_last_press(void);
void henge_vm_mouse_move(int dx, int dy);
void henge_vm_mouse_button(int button, int down);
/* Wheel units: dy > 0 up, dx > 0 right. */
void henge_vm_mouse_scroll(int dx, int dy);
/* Where the guest has its pointer, in display pixels. 0, or -1 before it has one. */
int henge_vm_pointer(int *x, int *y);
/* The Mac's screen in the pixels henge_vm_pointer uses: the display mode macOS runs (a scaled
   resolution is stretched over the panel's own). 0, or -1 before. */
int henge_vm_screen_size(int *w, int *h);
/* The same screen in macOS's points (half its pixels in a Retina mode): what a page scrolls by. */
int henge_vm_screen_points(int *w, int *h);
/* The absolute pointer touch uses: 0 to 1 across the Mac's screen, left/top to right/bottom.
   Ready (1) once macOS has taken the device; until then touch works as a trackpad. */
int henge_vm_touch_ready(void);
void henge_vm_touch(double x, double y);
/* Where the pointer starts (0..1 of the screen) instead of the top-left corner. */
void henge_vm_park_pointer(double x, double y);
void henge_vm_touch_button(double x, double y, int button, int down);
/* Hides the Mac's pointer while a finger is the pointer; the relative mouse brings it back. */
void henge_vm_hide_pointer(int hide);
/* The touchscreen (host/vm.h's hv_vm_touchscreen_*): on macOS 27 and later with Henge Tools, a touchscreen macOS
   takes as its own. Plugged in by the Mac's side on its own; config says whether it may be and the iPad screen's
   size in millimetres. usable: 1 while fingers should go through it, decided at the start of each gesture.
   frame: every finger down, ids 0..9, and each lifted since the last frame once with down 0 (cancel 1 when the
   gesture was taken away); x, y as for henge_vm_touch. 0, or -1 (not taken). */
/* The microphone (host/audio_mac.h): hook(1) just before the iPad's microphone starts because the Mac records,
   hook(0) once it has stopped, so the app's audio session records only meanwhile. Called on a queue of its own:
   it may block (asking the person for the microphone). Set once, before the machine is built. */
void henge_vm_set_mic_hook(void (*hook)(int on));
void henge_vm_touchscreen_config(int enabled, int w_mm, int h_mm);
void henge_vm_touchscreen_always(int always);
int henge_vm_touchscreen_usable(void);
/* As a touchscreen gesture starts: the helper checks macOS does something with it; if not, usable goes 0 (and stays
   so until the Mac restarts), and touch is imitated with the pointer. */
void henge_vm_touchscreen_watch(void);
/* host/vm.h's HV_VM_TS_*: 0 this Mac takes none, 1 turned off here, 2 waiting for macOS, 3 in use (not yet seen
   working), 4 in use and working, 5 macOS ignored its touches (out, touch imitated with the pointer). */
int henge_vm_touchscreen_status(void);
/* One frame of the fingers on the touchscreen (at most HV_FENCE_CONTACTS). 0 taken (sent, or waiting behind a click
   that waits for the helper's glide: input_fence.h), -1 not (no machine, too many). */
int henge_vm_touchscreen_frame(int n, const int *ids, const int *down, const int *cancel, const double *xs, const double *ys);
/* A small picture of the display, BGRA, width * height * 4 bytes. 0, or -1 before anything shows. */
int henge_vm_snapshot(uint8_t *bgra, int width, int height);

/* The boot framebuffer (Apple logo and progress bar), 32-bit BGRA, or -1 if
   the kernel has not got that far. The pixels are the guest's live memory. */
/// Where the Mac's GPU frames go: a CAMetalLayer. Keep it alive while the machine runs.
void henge_vm_set_display_layer(void *layer);
/// How many frames have gone to it.
uint64_t henge_vm_gpu_frames(void);
/// 1 once macOS has shown its pointer: the login window (or the desktop) is up. The boot screens have none.
int henge_vm_pointer_shown(void);
/// 1 once this iPadOS's Metal has refused GPU code macOS sent (macOS 15 on iPadOS 17): the Mac's screen may stay black.
int henge_vm_gpu_code_refused(void);
/// Where the Mac's graphics stand after the iPad's GPU failed the Mac's work (a hang its watchdog stopped, a fault):
/// 0 they run; 1 stopped, and can be tried again; 2 stopped, its last work still on the GPU (try again in a moment);
/// 3 stopped for this run (tried again as often as a run allows): restarting the Mac brings them back.
int henge_vm_gpu_state(void);
/// The person's "Try Again" when that is 1: the Mac's graphics run again. 0 done; -1 not.
int henge_vm_gpu_try_again(void);
/// The boot screen (or what the CPU drew), BGRA, stride bytes a row: copied into to when cap holds stride * height (0);
/// 1: only the size given, to is too small; -1: nothing yet.
int henge_vm_boot_display(uint8_t *to, size_t cap, int *width, int *height, int *stride);

/* The Mac's speaker (host/vm.h's hv_vm_speaker_*): a USB audio device named for this iPad whose volume is the
   iPad's, as a Bluetooth speaker's is. name: before henge_vm_boot. listen: macOS set its volume (level 0..1, mute),
   called on a CPU thread (NULL stops). host_volume: the iPad's volume changed (0..1), macOS moves its slider.
   host_key: a volume button (1 up, -1 down, 0 mute), macOS steps its volume and shows its own display.
   volume: the speaker's volume now, 0 or -1 (no Mac). */
void henge_vm_speaker_name(const char *name);
void henge_vm_speaker_listen(void (*cb)(void *opaque, double level, int mute), void *opaque);
void henge_vm_speaker_host_volume(double level);
void henge_vm_speaker_host_key(int key);
int henge_vm_speaker_volume(double *level, int *mute);

/* The largest single mapping the address space takes right now, in MB (the
   increased virtual addressing entitlement raises it). */
uint64_t henge_largest_reservation_gb(void); /* measuring: the biggest PROT_NONE reservation, in GB */
uint64_t henge_largest_mapping_mb(void);
/* Bytes this process may still allocate before jetsam (os_proc_available_memory). */
size_t henge_available_memory(void);

#endif

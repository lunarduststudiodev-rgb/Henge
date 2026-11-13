#ifndef HENGEVM_SHARE_H
#define HENGEVM_SHARE_H

#include <stddef.h>
#include <stdint.h>

/* What the Mac and the device share: a folder and the clipboard (see share.c). The guest reaches this
   at HV_SHARE_ADDR, port 80, which the network hands to the Unix socket. Returns 0, or -1 if the socket
   can't be made. agent is the gzipped helper that runs inside macOS, served to the guest; it may be NULL. */
#define HV_SHARE_ADDR "10.0.2.4"
int hv_share_start(const char *folder, const char *socket_path, const char *agent);

/* The device's clipboard, for the guest to pick up. */
void hv_share_set_clipboard(const char *text);
/* What the guest last copied, once: a malloc'd string, or NULL if nothing is new. */
char *hv_share_take_clipboard(void);
/* Seconds since the helper in macOS was last heard from, or -1 if it never has been. */
long hv_share_agent_seen(void);

/* The helper's live link, for touch (see share.c). The helper and this check the same version: a helper
   that differs is told to update itself from the one this app carries. */
#ifndef HV_AGENT_VERSION
#define HV_AGENT_VERSION 42 /* 42: one-time Dock badge cache refresh & App Nap/store tuning; 8: sync; 9: focus; 10: focus that is really watched; 11: finds My Shared Files (AppleVirtIOFS); 12: focus at once, and where the field is; 13: ejects USB drives and the Henge Tools disk, opens its installer; 14: links first, and says how long it took; 15: a system part (henge-agent --system, a LaunchDaemon) says whether the Mac is at its login window, and starts the helper the moment someone logs in; 16: says how many pop-up menus are showing, whether a glide is quick enough that a tap only stops it, and which buttons are pressed as a finger lands on them; 17: on macOS 27 and later the system part turns on WindowServer's external-touchscreen support and says so, and the helper says whether macOS took the touchscreen; 18: closes the pop-up menus that are showing with their own Cancel action (AXCancel); 19: says whether macOS did anything with a touch on the touchscreen ("touchwatch"); 20: says it can't tell without Accessibility, watches from the start, leaves out its own events, and says when the Mac started; 21: says the text around a text field's caret ("context"), and takes any text with a caret for a field; 22: says what is selected too, and what kind of field it is (code, an address bar, an email address...); finds apps built on Chrome by their page renderer, takes editable web content for a field, finds a launcher's panel (Spotlight, Raycast, Alfred) that has the keyboard, and doesn't take everything for a password while some app leaves secure input on; 23: the system part tags the touchscreen with the screen's ID (displayUUID), which WindowServer needs to take it, says "touchscreen ready" only once it can, and asks for it to be plugged in again if it was tagged late ("touchscreen replug"); 24: also tags the touchscreen's HID service itself and reports, as a test, what each kind of HID client sees of the tag; 25: says which events macOS made of the first touches (mouse or touch); 26: keeps macOS from idling into display or system sleep (the iPad decides), and says whether the screen is locked, asleep or behind the screen saver ("screen away"), when touches must go with the pointer; 27: a test: the system part records what WindowServer and AppKit say about touches, and the Mac's touch feature flags, in "Henge touch trace.txt" in the shared folder; 28: one numbered "field" line for each change of focus (text, secure, none, or unknown when Accessibility can't tell), with an ID for the focused element, the text around the caret tagged with it ("ctx"), and what a tap lands in ("tap": a text field, a password field, something else, or can't tell); 29: the system part's touch trace and touchscreen tests run only when Henge asks ("trace", from HENGE_TOUCH_TRACE=1 in henge-env.txt), not on every Mac; 30: finds macOS 27's Spotlight (the Siri process's panel) on a slow Mac too, and a tap in the space around a text box's text counts as one in the box; 31: gives what a tap landed in up to 0.9 s (an emulated Mac's apps often take 200-600 ms to say; 150 ms gave up on most), on a queue of its own so no hit waits for it; 32: shuts the Mac down or restarts it as the Apple menu does when asked ("power"), so apps can save first, asking the person once on the Mac's screen for the permission that needs, and says if an app stopped it; 33: answers a numbered "stop" ("stopped") once its glide has stopped, so a click held for it lands where the page stopped, and nothing moves under it; 34: secure input counts only for the app holding it (kCGSSessionSecureInputPID): a field is a password field when it says so, not in another app's (a password manager left it on); SecurityAgent holding it, or loginwindow while the screen is locked, is asked itself rather than the app in front behind it (its password field, or nothing it can say, is a password field, and the lock screen's window too; its user-name field is not; loginwindow counts only while the screen is locked, macOS saying who holds secure input or not); asked "look N" just before the password's Return, it looks at the focus then and answers "looked N" with what has it: "secure" only for a password field it is sure of (the field says so, or macOS's own prompt has the keys), "held" for one taken only from its app holding secure input (Terminal's sudo), after which no Return is pressed; 35: not used; 36: runs the benchmark's steps inside the Mac when asked ("bench"), saying as each starts and ends (host/bench.h), and is built for Intel too, for its Rosetta step; 37: the system part says which ports the Mac listens on, from the kernel's socket tables, once a second when they change ("ports tcp 22 445 udp 88"), so each is the same port on the device (net_forward.c), its link carrying a key only root can read (Henge-Key) so only its list is taken; 38: sends native volume key events (NX_KEYTYPE_SOUND_UP/DOWN/MUTE) to WindowServer for native OSD HUD; 39: battery status reporting and native IOPMPowerSource/status item for macOS Ventura; 40: absolute volume level synchronization via kAudioHardwareServiceDeviceProperty_VirtualMasterVolume; 41: coalesced 1:1 volume synchronization with smooth native HUD */
#endif
/* The oldest helper that does each thing as this build expects. A helper older than this build's is told to update
   itself; until it has (it may not manage to: no network in the Mac, say), Henge uses what that helper can do, rather
   than nothing. When a version changes what a line means, the thing it is for moves up to that version. */
enum {
    HV_AGENT_SINCE_SYNC = 8,         /* sync: macOS writes its disk cache out before a pause */
    HV_AGENT_SINCE_DRIVES = 13,      /* eject, and tools: open the installer */
    HV_AGENT_SINCE_MENUS = 18,       /* menu and menuclose */
    HV_AGENT_SINCE_TOUCH = 20,       /* hit, scrolling, glide, trust, touchscreen on|off and touchwatch */
    HV_AGENT_SINCE_TOUCHSCREEN = 23, /* the system part's "touchscreen ready", once it has tagged the screen's ID */
    HV_AGENT_SINCE_FORMAT = 24,      /* format: a new drive set up as APFS */
    HV_AGENT_SINCE_SCREEN_AWAY = 26, /* screen away 0|1 */
    HV_AGENT_SINCE_FIELD = 28,       /* field, ctx and tap: what has the keyboard's focus, and what a tap lands in */
    HV_AGENT_SINCE_TRACE = 29,       /* the system part records the touch trace only when told ("trace") */
    HV_AGENT_SINCE_POWER = 32,       /* power shutdown|restart: as the Apple menu does it */
    HV_AGENT_SINCE_STOPPED = 33,     /* stop <id>, answered with "stopped <id>" once the glide has stopped */
    HV_AGENT_SINCE_LOOK = 34,        /* "look N": what has the focus now, for the password's Return ("looked N ...") */
    HV_AGENT_SINCE_BENCH = 36,       /* bench STEPS, answered with bench lines */
    HV_AGENT_SINCE_PORTS = 37,       /* the system part's "ports tcp ... udp ...": what the Mac listens on */
    HV_AGENT_SINCE_VOLUME = 38,      /* volume: sends native volume key events (NX_KEYTYPE_SOUND_UP/DOWN/MUTE) */
    HV_AGENT_SINCE_BATTERY = 39,     /* battery: battery status and native power source for macOS Ventura */
    HV_AGENT_SINCE_VOLUME_LEVEL = 40, /* vollevel: absolute volume synchronization */
};
/* What the Mac listens on, as the helper's system part says: called on share.c's thread with each new list (port[i],
   udp[i] 0 or 1), and with n -1 to go back to the standard set (the link it came from has gone, or was replaced by one
   that won't say its own). Only from a link with this Mac's key (hv_share_system_pin). net_slirp.c sets it as the
   Mac's network is made (hv_net_attach), after hv_share_start: atomic, as share.c's threads read it. NULL: not
   wanted. */
#define HV_SHARE_MAX_PORTS 512
extern void (*_Atomic hv_share_ports)(const uint16_t *port, const uint8_t *udp, int n);
/* Before the Mac starts: where this Mac's system key is pinned, a file of the app's own (not the Mac's disk, nor the
   snapshot), kept per Mac so a reinstalled macOS on it still finds it. It holds the SHA-256 of the key the system
   part proved it is root with, never the key; made 0600 when the first key is taken. NULL or "": for this run only. */
void hv_share_system_pin(const char *path);
/* The Mac starts afresh, not from a Suspend or an auto-save: its first system link, before any user's helper has
   linked, may bring a new key (macOS reinstalled). Without this call no new key is taken in place of a pinned one. */
void hv_share_system_cold_boot(void);
/* The linked helper's version, or 0 while none is linked. */
int hv_share_agent_version(void);
/* What the Mac's screen is at, as the helper's system part says (henge-agent --system, a LaunchDaemon that runs from
   the Mac's start, before anyone logs in): still starting up, the login window, or someone's desktop (locked or not).
   Unknown while the system part isn't linked: a Mac without it (installed with the Mac's password, once), or one
   that is still starting. */
enum { HV_SESSION_UNKNOWN = -1, HV_SESSION_STARTING = 0, HV_SESSION_LOGIN = 1, HV_SESSION_USER = 2 };
int hv_share_session(void);
/* The touchscreen (macOS 27 and later; xhci.c's port 6). hv_share_touchscreen_ready: 1 once the system part said
   macOS will take a USB touchscreen as one (it turned WindowServer's support on), 0 if it said it won't (older macOS),
   -1 while it hasn't said (not linked, or a helper older than 17). hv_share_touchscreen: 1 while the logged-in user's
   helper says macOS has a touchscreen on its screen (NSScreen.touchCapabilities), 0 if it says not, -1 unknown. */
int hv_share_touchscreen_ready(void);
int hv_share_touchscreen(void);
/* Whether the Mac's screen is locked, its display asleep or the screen saver on, as the helper last said (version 26):
   1 yes, 0 no, -1 not known (no helper of this version). macOS does nothing with touchscreen touches then, so
   fingers go with the pointer, which wakes and unlocks it. */
int hv_share_screen_away(void);
/* Whether macOS does anything with the touchscreen's touches. It can have the touchscreen (the screen says it has
   touch) and still drop every touch (macOS 27.2 does, when WindowServer finds no screen for it): then a finger would
   do nothing at all. So as each touchscreen gesture starts, hv_share_touchscreen_watch asks the helper whether macOS
   makes any event of it. hv_share_touchscreen_verdict: 1 macOS did, 0 it didn't (or the helper can't tell): don't use
   the touchscreen, -1 not known yet. Kept while the Mac runs; a Mac that restarts is asked afresh. Any thread. */
void hv_share_touchscreen_watch(void);
/* Any thread: the touchscreen's frames: over 1 once no finger is down (the gesture has ended), cancelled 1 if a
   finger lifted as cancelled. The helper's answer counts once the gesture has ended, and not for one cancelled. */
void hv_share_touchscreen_gesture(int over, int cancelled);
/* Any thread: forget what was found (the touchscreen was turned off and on again): it is tried afresh. */
void hv_share_touchscreen_retry(void);
/* 1 once (then 0) after the system part asked for the touchscreen to be plugged in again: it tagged it with the
   screen's ID after WindowServer may have looked (helper 23 and later). */
int hv_share_touchscreen_take_replug(void);
int hv_share_touchscreen_verdict(void);
/* The app's "Native (Always)": only changes what ts_answer logs. */
void hv_share_touchscreen_always(int always);
enum { HV_TOUCH_UNKNOWN = -1, HV_TOUCH_SCROLL = 0, HV_TOUCH_DRAG = 1 };
enum { HV_SCROLL_BEGAN = 1, HV_SCROLL_CHANGED = 2, HV_SCROLL_ENDED = 4 };
/* 1 while the helper is linked and macOS lets it control the Mac (Privacy & Security > Accessibility). */
int hv_share_touch_ready(void);
/* 1 while the helper is linked, allowed or not. */
int hv_share_touch_linked(void);
/* 1 while a helper of this build's version (HV_AGENT_VERSION) is linked: whether Henge Tools is up to date. What a
   helper can do is hv_share_agent_version against HV_AGENT_SINCE_*. */
int hv_share_agent_current(void);
/* Before a pause: has the helper in macOS write everything it holds for its disks out to them, and waits
   up to timeout_ms for it to say so. 0 done; -1 no helper that can (HV_AGENT_SINCE_SYNC) linked, it failed, or no
   answer in time (the Mac's own disk cache is then as macOS left it). The Mac must be running. Any thread. */
int hv_share_guest_sync(unsigned timeout_ms);
/* What has the Mac's keyboard focus, as the helper last said: HV_FOCUS_*. Unknown while no helper that says it
   (HV_AGENT_SINCE_FIELD) is linked (before login, say: the helper runs in the logged-in user's session;
   hv_share_session says whether that is the login window). */
enum { HV_FOCUS_UNKNOWN = -1, HV_FOCUS_NONE = 0, HV_FOCUS_TEXT = 1, HV_FOCUS_SECURE = 2 };
int hv_share_focus(void);
/* Where the focused field is, as fractions of the Mac's screen (x, y from the top left, width, height): 1 if the
   helper said, else 0. */
int hv_share_focus_frame(float frame[4]);
/* The text around the focused field's caret, as the helper last said (version 22): up to 120 characters before it,
   and from it what is selected and 40 more, each base64 (UTF-8) as the helper sent it, "" for none; *selected is how
   many characters are selected. Returns a count that goes up with each thing the helper says; 0 while it says
   nothing. *known is 0 when there is no text field (or it won't say). *kind is the field's: HV_FIELD_PROSE, _CODE
   (a terminal or code editor), _ADDRESS (a browser's address bar) or _PLAIN (an email address, a user name), even
   when its text isn't known. Any thread. */
enum { HV_FIELD_PROSE = 0, HV_FIELD_CODE = 1, HV_FIELD_ADDRESS = 2, HV_FIELD_PLAIN = 3 };
unsigned hv_share_context(char *before, size_t before_cap, char *after, size_t after_cap, int *selected, int *known, int *kind);
/* The focus as the helper last said it in one "field" line (version 28), for the device's keyboard. serial goes up
   with each line; id with each change of the focused element (the same field keeps its id); generation with each
   link that opens or closes (a new link's first line is where things stand, not a change). state is HV_FOCUS_*, and
   HV_FOCUS_UNKNOWN also when the helper can't tell (Accessibility timed out, say): then nothing is known to have
   changed. kind is HV_FIELD_*, frame where the field is (fractions of the screen; width 0 if not said). Returns 1
   while a helper of this version is linked, else 0 (and *f says unknown). Any thread. */
typedef struct {
    unsigned serial, id, generation;
    int state, kind;
    float frame[4];
} HvShareField;
int hv_share_field(HvShareField *f);
/* Asks what a tap at x, y (0..1 across the screen) lands in (version 28): for a tap on the touchscreen, which goes
   to macOS as its own touch and so asks no hit. Returns an id for hv_share_tap_answer, or 0 without a helper of this
   version. A hit (hv_share_touch_hit) is answered the same way, with its own id. */
unsigned hv_share_tap(double x, double y);
/* What tap or hit id landed in: HV_TAP_EDITABLE (it takes typing), HV_TAP_SECURE (a password field), HV_TAP_NO, or
   HV_TAP_UNKNOWN (the helper couldn't tell in time); HV_TAP_WAITING until it answers. frame is the field's
   (fractions of the screen; width 0 if not said), role the element's Accessibility role ("" if not known). */
enum { HV_TAP_WAITING = -2, HV_TAP_UNKNOWN = -1, HV_TAP_NO = 0, HV_TAP_EDITABLE = 1, HV_TAP_SECURE = 2 };
int hv_share_tap_answer(unsigned id, float frame[4], char *role, size_t role_cap);
/* A look at the focus, for the password's Return (version HV_AGENT_SINCE_LOOK): an id for hv_share_look_answer, or 0
   without a helper of that version. It is asked of the helper by hv_share_look_send, once every key and click before
   it has reached the Mac's keyboard and pointer (henge-vm.c sends it through the input fence; macOS may not have
   acted on them yet); its answer comes after anything the helper says before it. A look the link closed on before it was sent isn't sent. */
unsigned hv_share_look(void);
void hv_share_look_send(unsigned id);
/* What look id found: HV_FOCUS_* as hv_share_field's state (HV_FOCUS_UNKNOWN when the helper couldn't tell, or the
   link closed first), or HV_TAP_WAITING until it answers. *sure (may be NULL) is 1 only for HV_FOCUS_SECURE that is
   sure to be a password field: the field says so, or macOS's own prompt has the keys; 0 for one taken only from its
   app holding secure input (Terminal: its Secure Keyboard Entry looks the same as sudo asking). */
int hv_share_look_answer(unsigned id, int *sure);
/* Called (on the link's thread, without any lock held) after each "field", "tap" or "looked" line, and when the link opens
   or closes: it should only note that something changed and read the rest on its own thread. NULL for none. */
void hv_share_set_listener(void (*listener)(void));
/* Has the helper show macOS's request for that permission again. */
void hv_share_touch_ask(void);
/* Has the helper open Install Henge Tools from the Henge Tools disk, waiting up to a minute for it: send this,
   then plug the disk in (the helper ejects the disk on sight otherwise, and again when the installer quits). */
void hv_share_tools_open(void);
/* Has the helper eject the USB drive with this serial number, as Finder's eject button does (version 13). The
   drive then reads as ejected (hv_vm_disk_state) and can be unplugged. hv_share_eject_answer: 1 ejected,
   0 macOS wouldn't (something on it is open) or it isn't there, -1 no answer yet (or no helper linked). */
void hv_share_eject(const char *serial);
int hv_share_eject_answer(const char *serial);
/* Has the helper set up a new, all-zero drive as an APFS disk called name, once it's here: it won't touch one
   macOS finds anything on. hv_share_format_answer: 1 done, 0 not, -1 no answer yet. */
void hv_share_format(const char *serial, const char *name);
int hv_share_format_answer(const char *serial);
/* Has the logged-in user's helper shut the Mac down (restart 0) or restart it (1) as the Apple menu does: every app
   is asked to quit and can save first, or ask about unsaved changes on the Mac's screen, or stop it. Returns an id
   for hv_share_power_answer, or 0 if no helper that can (HV_AGENT_SINCE_POWER) is linked. The answer: HV_POWER_*.
   Nothing says it worked but the Mac turning off (hv_vm_halted_by). */
enum { HV_POWER_WAITING = -1, HV_POWER_FAILED = 0, HV_POWER_SENT = 1, HV_POWER_CANCELLED = 2, HV_POWER_ASKING = 3 };
unsigned hv_share_power(int restart);
int hv_share_power_answer(unsigned id);
/* Sends a volume key command (key > 0: up, key < 0: down, 0: mute) to the linked helper inside macOS, which
   dispatches an Apple media key event to WindowServer to show the native volume HUD overlay. Returns 1 if sent. */
int hv_share_volume_key(int key);
/* Sends an absolute volume level (0.0 to 1.0) to the linked helper inside macOS, which
   sets kAudioHardwareServiceDeviceProperty_VirtualMasterVolume on the default output device. Returns 1 if sent. */
int hv_share_volume_level(double level);
/* Battery state from the host (percent 0..100, external 0|1, charging 0|1, full 0|1).
   Forwarded to the helper so macOS Ventura and systems without the PCI battery driver show it natively. */
void hv_share_set_battery(double percent, int external, int charging, int full);
/* The benchmark (host/bench.h): has the helper run its steps inside the Mac ("all", or names with commas between).
   Returns the link it was asked on (a number, never 0), or 0: no helper that can (HV_AGENT_SINCE_BENCH) is linked, or
   the link went as it was sent. What a helper says back as it goes ("begin NAME TARGET_MS", "end NAME ok|skip|fail
   [WHY] [KEY=NUMBER ...]", "note TEXT", "done", "failed WHY") goes to the listener with the link it came on, on that
   link's thread, printable characters only; and "failed gone" from here, with the link, when a link closes, fails or
   is replaced. A run takes only its own link's. NULL for none. */
unsigned hv_share_bench(const char *steps);
void hv_share_set_bench_listener(void (*listener)(unsigned link, const char *line));
/* The video the benchmark plays, served to the helper at /.henge/bench-video (404 until set). */
void hv_share_set_bench_video(const char *path);
/* Asks what is under a finger at x, y (0..1 across the screen): something a drag moves at once (a title bar,
   a slider, a menu), or something to scroll. Returns an id for hv_share_touch_answer, or 0 without a link. */
unsigned hv_share_touch_hit(double x, double y);
/* HV_TOUCH_DRAG or HV_TOUCH_SCROLL once the helper has answered id, until then HV_TOUCH_UNKNOWN. */
int hv_share_touch_answer(unsigned id);
/* Once id is answered: 1 if what is under the finger is a button that shows itself pressed while it is touched (a
   dialog's button, a checkbox, a window's close button), with its frame as fractions of the screen (x, y from the
   top left, width, height); else 0. A helper older than version 16 never says. */
int hv_share_touch_press(unsigned id, float frame[4]);
/* Asks how many pop-up menus are showing (a context menu, the Dock's, a pop-up button's), each until it has faded
   right out. Returns an id for hv_share_menu_answer, or 0 without a helper of this version. */
unsigned hv_share_menu_ask(void);
/* Asks the helper to close the pop-up menus that are showing, with each menu's own Cancel action (as VoiceOver
   does; no key is pressed). Its answer (hv_share_menu_answer) is how many it closed: 0 if it couldn't (no
   Accessibility permission, or nothing answered). Returns an id, or 0 without a helper of this version. */
unsigned hv_share_menu_close(void);
/* The count once the helper has answered id, until then -1. */
int hv_share_menu_answer(unsigned id);
/* 1 while a page the helper set gliding is moving quickly enough that a finger landing on it only stops it, as on
   an iPad, rather than tapping what is there. */
int hv_share_touch_gliding(void);
/* A finger scrolling what is at x, y, as a trackpad does: began and changed carry the move (dx, dy, in
   fractions of the screen, the way the finger went); ended carries the finger's speed (screens a second),
   and the helper glides on from it. */
void hv_share_touch_scroll(int phase, double x, double y, double dx, double dy);
/* A finger came down, or a button was pressed: a glide stops, as on a trackpad. Sent early (before the click it is
   for), so the answer a click may wait for (hv_share_glide_fence) is on its way. Stops and scroll lines reach the
   helper in the order they are sent here, and stops in the order of their numbers. */
void hv_share_touch_stop(void);
/* For a press (input_fence.h): the stop it must wait to be answered, or 0. Nonzero only while the helper may still be
   posting scroll events, that is after a scroll line not yet followed by an answered stop (version 33); a stop already
   sent after the last scroll line is waited for, otherwise one is sent. 0 too with no helper, one too old to answer,
   or one that hasn't answered in time (hv_share_touch_late) until it answers again. */
unsigned hv_share_glide_fence(void);
/* A press waited too long for the answer to stop `id`: the helper isn't waited on until it answers a stop again.
   Nothing changes if it answered that stop, or a later one, meanwhile, or if the stop went to an earlier link. */
void hv_share_touch_late(unsigned id);
/* Called once every stop up to `id` is over: with gone 0, the helper says it is done with stop id (it answers them in
   order); with gone 1, its link closed or was replaced, and id is the last stop sent on it (0 if none ever was): no
   answer is coming to any of them. Stops sent on a link since are later than id. On the link's thread, or the
   caller's when a send fails; never under share.c's own locks (the caller's may be held: a send failing as the fence
   delivers). NULL for none. */
void hv_share_set_glide_listener(void (*listener)(unsigned id, int gone));

#endif

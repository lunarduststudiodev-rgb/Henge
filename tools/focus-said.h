/* What one look at the Mac's focus says, from what Accessibility answered and which process holds macOS's secure
   input. Pure, so it is checked on its own (tests/test_focus_said.c, make test-focus-said).

   macOS's secure input (IsSecureEventInputEnabled) is the whole Mac's, and an app may leave it on: a password manager
   in the background, say. Taken as the focused field's (helper 33 and before), Safari's address bar read as a password
   field while some other app held secure input (the password key typed into it and pressed Return), and so did the
   desktop with nothing focused. So it counts only by which process holds it: macOS says that (the console session's
   kCGSSessionSecureInputPID).

   - A field that says it is a password field (AXSecureTextField) is one.
   - macOS's own password prompts holding it (SecurityAgent's authentication dialog; loginwindow only while the
     screen is locked, as it may keep it on a moment after unlocking): the keys go to the prompt, not to the app in
     front behind it (the lock screen in front of Safari read as its address bar). So the prompt itself is asked
     (focus_ask_prompt), and its own fields say what they are: the user-name field is not a password field.
   - Nothing focused, or the helper can't tell, in the prompt holding it: a password field (their focused element
     timed out, or said nothing has the focus: tests of 8 and 9 Nov). The lock screen saying its window or a group
     has the focus too: its keys go to its password field, and a Return there only unlocks. Not SecurityAgent's
     dialog with a button focused: Return would press its default button (Allow, OK) with the password typed
     nowhere. Likewise when the prompt holds it and can't be asked, whatever the app asked instead shows.
   - Another field is one when its own app holds secure input: Terminal turns it on by itself while sudo or ssh asks
     for a password, and its text area says nothing more. But Terminal's Secure Keyboard Entry, or an app leaving it
     on after its password field, looks the same: so this is a password field taken from secure input
     (FOCUS_SAID_HELD): "secure" on the field line, for the device to show the password key, but "held" in the answer
     to "look", asked just before the password's Return, which then isn't pressed. Likewise where the helper can't
     tell and the app asked holds it. */
#ifndef HENGE_FOCUS_SAID_H
#define HENGE_FOCUS_SAID_H

#include <string.h>

enum {
    FOCUS_NOTHING,   /* the app says nothing of it has the focus */
    FOCUS_CANT_TELL, /* no answer: no permission, no app in front, a timeout, a role or subrole it couldn't say */
    FOCUS_NOT_FIELD, /* something that takes no typing (a button, a list, a window) */
    FOCUS_FIELD,     /* something that takes typing */
};

enum {
    FOCUS_SAID_NONE,
    FOCUS_SAID_TEXT,
    FOCUS_SAID_SECURE, /* a password field: it says so, or macOS's own prompt has the keys */
    FOCUS_SAID_HELD,   /* a password field only as its app holds secure input: Return isn't pressed after it */
    FOCUS_SAID_UNKNOWN,
};

/* Whether secure input is held by macOS's own password prompt: SecurityAgent, or loginwindow while the screen is
   locked. holder: the process holding it (0 none, -1 macOS doesn't say); holder_name its name. Then that process is
   the one to ask about the focus. */
static inline int focus_ask_prompt(int holder, const char *holder_name, int screen_locked) {
    return holder > 0 && holder_name &&
           (!strcmp(holder_name, "SecurityAgent") || (screen_locked && !strcmp(holder_name, "loginwindow")));
}

/* Who holds secure input, for focus_said. holder, holder_name and screen_locked as for focus_ask_prompt; asked: the
   process asked about the focus (the prompt holding it, a launcher's panel when that has the keyboard, else the app
   in front); asked_bundle its bundle ID. When macOS doesn't say, only its prompts, asked, are taken to (as helper 32
   did): loginwindow only while the screen is locked (it also shows the Log Out and Shut Down confirmations). */
typedef struct {
    int held_here;      /* the process asked holds it */
    int held_by_prompt; /* SecurityAgent, or loginwindow with the screen locked, holds it */
    int lock_screen;    /* that is loginwindow with the screen locked */
} FocusHolder;

static inline FocusHolder focus_holder(int holder, const char *holder_name, int screen_locked, int asked,
                                       const char *asked_bundle) {
    FocusHolder h = {0, 0, 0};
    if (holder > 0) {
        h.held_here = asked > 0 && holder == asked;
        h.held_by_prompt = focus_ask_prompt(holder, holder_name, screen_locked);
        h.lock_screen = h.held_by_prompt && !strcmp(holder_name, "loginwindow");
    } else if (holder < 0 && asked_bundle) {
        h.lock_screen = screen_locked && !strcmp(asked_bundle, "com.apple.loginwindow");
        h.held_by_prompt = h.held_here = h.lock_screen || !strcmp(asked_bundle, "com.apple.SecurityAgent");
    }
    return h;
}

static inline int focus_said(int seen, int field_secure, FocusHolder h) {
    if (h.held_by_prompt && !h.held_here) return FOCUS_SAID_SECURE; /* the prompt, not asked, in front of the app asked */
    switch (seen) {
    case FOCUS_NOT_FIELD: return h.lock_screen ? FOCUS_SAID_SECURE : FOCUS_SAID_NONE;
    case FOCUS_FIELD:
        if (field_secure) return FOCUS_SAID_SECURE;
        return h.held_here && !h.held_by_prompt ? FOCUS_SAID_HELD : FOCUS_SAID_TEXT;
    case FOCUS_NOTHING: return h.held_by_prompt ? FOCUS_SAID_SECURE : FOCUS_SAID_NONE;
    default: return h.held_by_prompt ? FOCUS_SAID_SECURE : h.held_here ? FOCUS_SAID_HELD : FOCUS_SAID_UNKNOWN;
    }
}

/* The field line's state word: "secure" for both kinds of password field. */
static inline const char *focus_said_word(int said) {
    switch (said) {
    case FOCUS_SAID_NONE: return "none";
    case FOCUS_SAID_TEXT: return "text";
    case FOCUS_SAID_SECURE:
    case FOCUS_SAID_HELD: return "secure";
    default: return "unknown";
    }
}

/* The answer to "look" (helper 34): "held" for a password field taken only from secure input. */
static inline const char *focus_look_word(int said) {
    return said == FOCUS_SAID_HELD ? "held" : focus_said_word(said);
}

#endif

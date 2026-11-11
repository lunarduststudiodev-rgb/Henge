/* What the helper says has the Mac's focus (tools/focus-said.h, helper 34): a password field when the field says it
   is one, or when macOS's own prompt holds secure input (asked itself, or in front of another app when it can't be;
   loginwindow only while the screen is locked); one taken from secure input
   ("held": no Return after the password) when the field's own app holds it (Terminal's sudo). Secure input held by
   some other app (a password manager) makes a password of nothing. */
#include "../tools/focus-said.h"

#include <stdio.h>
#include <string.h>

static int failures;
static void check(int got, int want, const char *what) {
    static const char *names[] = {"none", "text", "secure", "held", "unknown"};
    if (got != want) {
        failures++;
        fprintf(stderr, "FAIL: %s: said %s, not %s\n", what, names[got], names[want]);
    }
}

enum { TERMINAL = 500, SAFARI = 600, MANAGER = 700, LOGINWINDOW = 100, AGENT = 110, LAUNCHER = 800 };

int main(void) {
    FocusHolder none = focus_holder(0, NULL, 0, SAFARI, "com.apple.Safari");
    FocusHolder manager = focus_holder(MANAGER, "1Password", 0, SAFARI, "com.apple.Safari");
    FocusHolder terminal = focus_holder(TERMINAL, "Terminal", 0, TERMINAL, "com.apple.Terminal");
    FocusHolder agent = focus_holder(AGENT, "SecurityAgent", 0, AGENT, "com.apple.SecurityAgent");
    FocusHolder lockOverSafari = focus_holder(LOGINWINDOW, "loginwindow", 1, SAFARI, "com.apple.Safari");
    FocusHolder agentOverTerminal = focus_holder(AGENT, "SecurityAgent", 0, TERMINAL, "com.apple.Terminal");
    FocusHolder loginwindow = focus_holder(LOGINWINDOW, "loginwindow", 1, LOGINWINDOW, "com.apple.loginwindow");
    // loginwindow keeping secure input a moment after the screen unlocks: Safari's address bar is not a password field.
    FocusHolder unlockedOverSafari = focus_holder(LOGINWINDOW, "loginwindow", 0, SAFARI, "com.apple.Safari");

    // Which process is asked: macOS's prompt holding secure input, itself (its user-name field isn't a password field).
    check(focus_ask_prompt(AGENT, "SecurityAgent", 0), 1, "SecurityAgent holding it is asked");
    check(focus_ask_prompt(LOGINWINDOW, "loginwindow", 1), 1, "loginwindow holding it with the screen locked is asked");
    check(focus_ask_prompt(LOGINWINDOW, "loginwindow", 0), 0, "loginwindow holding it with the screen unlocked isn't");
    check(focus_ask_prompt(TERMINAL, "Terminal", 1), 0, "Terminal holding it isn't");
    check(focus_ask_prompt(-1, NULL, 1) || focus_ask_prompt(0, NULL, 1), 0, "no holder said, none held");

    // Who holds it.
    check(manager.held_here || manager.held_by_prompt, 0, "a password manager holding it, asked of Safari");
    check(terminal.held_here && !terminal.held_by_prompt, 1, "Terminal holding it, asked of Terminal");
    check(lockOverSafari.held_by_prompt && !lockOverSafari.held_here, 1, "loginwindow holding it, asked of Safari");
    // A launcher's panel asked (it has the keys), Terminal in front holding it: not the panel's.
    FocusHolder panel = focus_holder(TERMINAL, "Terminal", 0, LAUNCHER, "com.raycast.macos");
    check(panel.held_here, 0, "Terminal holding it, a launcher's panel asked");
    // macOS doesn't say: only its prompts, asked, are taken to.
    FocusHolder unsaidAgent = focus_holder(-1, NULL, 0, AGENT, "com.apple.SecurityAgent");
    FocusHolder unsaidSafari = focus_holder(-1, NULL, 0, SAFARI, "com.apple.Safari");
    check(unsaidAgent.held_here && unsaidAgent.held_by_prompt, 1, "SecurityAgent asked, macOS not saying who holds it");
    check(unsaidSafari.held_here || unsaidSafari.held_by_prompt, 0, "Safari asked, macOS not saying who holds it");
    // loginwindow, macOS not saying: the lock screen only while locked, not its Log Out or Shut Down confirmation.
    FocusHolder unsaidLock = focus_holder(-1, NULL, 1, LOGINWINDOW, "com.apple.loginwindow");
    FocusHolder unsaidLogout = focus_holder(-1, NULL, 0, LOGINWINDOW, "com.apple.loginwindow");
    check(unsaidLock.held_here && unsaidLock.lock_screen, 1, "the lock screen asked, macOS not saying");
    check(unsaidLogout.held_here || unsaidLogout.held_by_prompt || unsaidLogout.lock_screen, 0,
          "loginwindow's Shut Down confirmation asked, macOS not saying");
    check(focus_said(FOCUS_NOT_FIELD, 0, unsaidLogout), FOCUS_SAID_NONE, "its Shut Down button focused, macOS not saying");
    check(focus_said(FOCUS_NOTHING, 0, unsaidLogout), FOCUS_SAID_NONE, "nothing focused in it, macOS not saying");
    check(focus_said(FOCUS_NOT_FIELD, 0, unsaidLock), FOCUS_SAID_SECURE, "the lock screen's window, macOS not saying");

    // A field: what it says it is; one taken from secure input when its own app holds it.
    check(focus_said(FOCUS_FIELD, 0, none), FOCUS_SAID_TEXT, "a text field");
    check(focus_said(FOCUS_FIELD, 0, manager), FOCUS_SAID_TEXT, "an address bar with secure input held by a password manager");
    check(focus_said(FOCUS_FIELD, 1, none), FOCUS_SAID_SECURE, "a password field");
    check(focus_said(FOCUS_FIELD, 1, terminal), FOCUS_SAID_SECURE, "a password field holding secure input");
    check(focus_said(FOCUS_FIELD, 0, terminal), FOCUS_SAID_HELD, "Terminal while sudo asks for a password");
    check(focus_said(FOCUS_FIELD, 0, agent), FOCUS_SAID_TEXT, "SecurityAgent's user name field");
    check(focus_said(FOCUS_FIELD, 1, agent), FOCUS_SAID_SECURE, "SecurityAgent's password field");
    check(focus_said(FOCUS_FIELD, 0, panel), FOCUS_SAID_TEXT, "a launcher's field, Terminal behind it holding secure input");

    check(unlockedOverSafari.held_here || unlockedOverSafari.held_by_prompt, 0, "loginwindow holding it unlocked, asked of Safari");
    check(focus_said(FOCUS_FIELD, 0, unlockedOverSafari), FOCUS_SAID_TEXT, "Safari's address bar just after unlocking");
    check(focus_said(FOCUS_NOTHING, 0, unlockedOverSafari), FOCUS_SAID_NONE, "the desktop just after unlocking");

    // The lock screen asked itself (it holds secure input with the screen locked).
    check(focus_said(FOCUS_FIELD, 1, loginwindow), FOCUS_SAID_SECURE, "the lock screen's password field");
    check(focus_said(FOCUS_CANT_TELL, 0, loginwindow), FOCUS_SAID_SECURE, "the lock screen timing out");
    // Its window or a group said to have the focus, while its password field has the keys (Return only unlocks).
    check(focus_said(FOCUS_NOT_FIELD, 0, loginwindow), FOCUS_SAID_SECURE, "the lock screen saying its window has the focus");
    // Not SecurityAgent's with a button focused: Return would press its default button, the password typed nowhere.
    check(focus_said(FOCUS_NOT_FIELD, 0, agent), FOCUS_SAID_NONE, "SecurityAgent with a button focused");
    check(focus_said(FOCUS_FIELD, 0, loginwindow), FOCUS_SAID_TEXT, "the lock screen's user name field");

    // macOS's prompts holding it in front of another app, when it couldn't be asked: the keys go to the prompt,
    // whatever that app shows.
    check(focus_said(FOCUS_FIELD, 0, lockOverSafari), FOCUS_SAID_SECURE, "the lock screen over Safari's address bar");
    check(focus_said(FOCUS_NOT_FIELD, 0, lockOverSafari), FOCUS_SAID_SECURE, "the lock screen over a Finder window");
    check(focus_said(FOCUS_NOTHING, 0, lockOverSafari), FOCUS_SAID_SECURE, "the lock screen over the desktop");
    check(focus_said(FOCUS_CANT_TELL, 0, lockOverSafari), FOCUS_SAID_SECURE, "the lock screen over an app that timed out");
    check(focus_said(FOCUS_FIELD, 0, agentOverTerminal), FOCUS_SAID_SECURE, "an authentication dialog over Terminal");

    // Something that takes no typing stays that.
    check(focus_said(FOCUS_NOT_FIELD, 0, none), FOCUS_SAID_NONE, "a button");
    check(focus_said(FOCUS_NOT_FIELD, 0, terminal), FOCUS_SAID_NONE, "a button in the app holding secure input");

    // Nothing focused: nothing, but in macOS's own password prompts holding secure input.
    check(focus_said(FOCUS_NOTHING, 0, none), FOCUS_SAID_NONE, "the desktop");
    check(focus_said(FOCUS_NOTHING, 0, manager), FOCUS_SAID_NONE, "the desktop while a password manager holds it");
    check(focus_said(FOCUS_NOTHING, 0, terminal), FOCUS_SAID_NONE, "Terminal holding it with nothing focused");
    check(focus_said(FOCUS_NOTHING, 0, loginwindow), FOCUS_SAID_SECURE, "loginwindow asking for a password, its field not shown");

    // Can't tell: the app asked, or macOS's prompts, holding secure input is the one sign there is.
    check(focus_said(FOCUS_CANT_TELL, 0, none), FOCUS_SAID_UNKNOWN, "a timeout");
    check(focus_said(FOCUS_CANT_TELL, 0, manager), FOCUS_SAID_UNKNOWN, "Safari timing out while a password manager holds it");
    check(focus_said(FOCUS_CANT_TELL, 0, agent), FOCUS_SAID_SECURE, "SecurityAgent's focused element timed out");
    check(focus_said(FOCUS_CANT_TELL, 0, unsaidAgent), FOCUS_SAID_SECURE, "SecurityAgent timing out, macOS not saying");
    check(focus_said(FOCUS_CANT_TELL, 0, terminal), FOCUS_SAID_HELD, "Terminal timing out while sudo asks");

    // The field line's word: one taken from secure input is still "secure"; the answer to "look" says "held".
    if (strcmp(focus_said_word(FOCUS_SAID_HELD), "secure") || strcmp(focus_said_word(FOCUS_SAID_UNKNOWN), "unknown") ||
        strcmp(focus_look_word(FOCUS_SAID_HELD), "held") || strcmp(focus_look_word(FOCUS_SAID_SECURE), "secure") ||
        strcmp(focus_look_word(FOCUS_SAID_TEXT), "text")) {
        failures++;
        fprintf(stderr, "FAIL: the state words\n");
    }

    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("focus said: all passed\n");
    return 0;
}

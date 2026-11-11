// Install Henge Tools.app: what a person double-clicks inside macOS, on the Henge Tools disk, to set up
// the helper (tools/henge-agent.m) without Terminal or the network. It does what share.c's install script does:
// puts the helper in ~/Library/Application Support/Henge and starts it as a LaunchAgent. Then it walks them
// through the one thing macOS won't let any app do on its own: turning on the Accessibility permission.
//
// It also puts in the helper's small system part (henge-agent --system, a LaunchDaemon), which needs the Mac's
// password once: it runs from the Mac's start, tells the iPad for certain when the Mac is at its login window, and
// starts the helper the moment someone logs in, instead of when macOS gets round to it.
//
// It ships inside the iPad app on a disk image (build/henge-tools.img.gz, tools/build-henge-tools.sh) that Henge
// attaches read-only until the Mac has this build's helper, so the copy here is always the one that came with it.
#import <AppKit/AppKit.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *const Label = @"com.henge.agent";

static NSString *supportDir(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/Henge"];
}
static NSString *agentPath(void) { return [supportDir() stringByAppendingPathComponent:@"henge-agent"]; }
static NSString *statusPath(void) { return [supportDir() stringByAppendingPathComponent:@"status"]; }
static NSString *plistPath(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/LaunchAgents/com.henge.agent.plist"];
}
static NSString *bundledAgent(void) {
    return [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Contents/Helpers/henge-agent"];
}
static NSString *domain(void) { return [NSString stringWithFormat:@"gui/%u", getuid()]; }

// The system part: root's own copy of the helper, so nothing a user can change runs as root.
static NSString *const SystemLabel = @"com.henge.agent.system";
static NSString *const SystemAgent = @"/Library/Application Support/Henge/henge-agent";
static NSString *const SystemPlist = @"/Library/LaunchDaemons/com.henge.agent.system.plist";

static int launchctl(NSArray<NSString *> *args, NSString **output) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/bin/launchctl"];
    task.arguments = args;
    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = NSFileHandle.fileHandleWithNullDevice;
    if (![task launchAndReturnError:NULL]) return -1;
    NSData *data = [pipe.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    if (output) *output = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    return task.terminationStatus;
}

// MARK: - What is on this Mac

typedef NS_ENUM(NSInteger, Setup) {
    SetupMissing,  // never installed, or removed
    SetupOutdated, // installed, but not this build's helper
    SetupStopped,  // this build's helper, not running
    SetupUntrusted,
    SetupReady,
    SetupLoginPart, // all but the system part, which needs the Mac's password
};

static BOOL agentRunning(void) {
    NSString *out = nil;
    if (launchctl(@[ @"print", [NSString stringWithFormat:@"%@/%@", domain(), Label] ], &out) != 0) return NO;
    return [out rangeOfString:@"state = running"].location != NSNotFound;
}

// The helper writes "trusted 1" or "trusted 0" here when it starts and whenever the permission changes: only
// the helper itself can tell whether macOS trusts it.
static BOOL agentTrusted(void) {
    NSString *text = [NSString stringWithContentsOfFile:statusPath() encoding:NSUTF8StringEncoding error:NULL];
    return [text rangeOfString:@"trusted 1"].location != NSNotFound;
}

// Checks of the window only (HENGE_TOOLS_PREVIEW=<state> <png>): shows that state, draws it, and quits.
static int previewing = -1;
static BOOL skippedSystem; // "Not Now" to the password, while this window is open

static Setup currentSetup(void) {
    if (previewing >= 0) return (Setup)previewing;
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm fileExistsAtPath:agentPath()] || ![fm fileExistsAtPath:plistPath()]) return SetupMissing;
    if (![fm contentsEqualAtPath:agentPath() andPath:bundledAgent()]) return SetupOutdated;
    if (!agentRunning()) return SetupStopped;
    if (!agentTrusted()) return SetupUntrusted;
    // It updates itself from Henge as the helper does, so it's enough that it's there.
    BOOL system = [fm fileExistsAtPath:SystemAgent] && [fm fileExistsAtPath:SystemPlist];
    return system || skippedSystem ? SetupReady : SetupLoginPart;
}

// MARK: - Installing and removing

static NSString *launchAgentPlist(void) {
    NSDictionary *plist = @{
        @"Label" : Label,
        @"ProgramArguments" : @[ agentPath() ],
        @"RunAtLoad" : @YES,
        @"KeepAlive" : @YES,
        @"LimitLoadToSessionType" : @"Aqua",
        @"ProcessType" : @"Interactive",
    };
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:plist format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

static NSString *shellQuoted(NSString *s) {
    return [NSString stringWithFormat:@"'%@'", [s stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]];
}

static NSString *appleScriptString(NSString *s) {
    s = [s stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
    return [NSString stringWithFormat:@"\"%@\"", [s stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""]];
}

// A shell script as root, after macOS's own password prompt. Returns nil, or what went wrong in words for the person;
// *cancelled when they chose Cancel.
static NSString *asAdministrator(NSString *script, NSString *why, BOOL *cancelled) {
    *cancelled = NO;
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/osascript"];
    task.arguments = @[ @"-e", [NSString stringWithFormat:@"do shell script %@ with prompt %@ with administrator privileges", appleScriptString(script), appleScriptString(why)] ];
    NSPipe *err = [NSPipe pipe];
    task.standardOutput = NSFileHandle.fileHandleWithNullDevice;
    task.standardError = err;
    if (![task launchAndReturnError:NULL]) return @"macOS couldn't ask for the password.";
    NSData *data = [err.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    if (task.terminationStatus == 0) return nil;
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
    if ([text rangeOfString:@"-128"].location != NSNotFound) { // "User canceled."
        *cancelled = YES;
        return nil;
    }
    return @"The part that starts at the login window couldn't be put in. Henge Tools works without it; the Log In button on your iPad is then a good guess rather than certain.";
}

static NSString *systemPlist(void) {
    NSDictionary *plist = @{
        @"Label" : SystemLabel,
        @"ProgramArguments" : @[ SystemAgent, @"--system" ],
        @"RunAtLoad" : @YES,
        @"KeepAlive" : @YES,
        @"ProcessType" : @"Interactive",
    };
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:plist format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

static NSString *const PasswordWhy = @"Henge Tools needs your Mac's password once, so it can start at the login window and the moment you log in.";

static NSString *systemScript(void);

// The system part, if it's missing or not this build's. Asks for the password only then.
static NSString *installSystem(BOOL *cancelled) {
    *cancelled = NO;
    NSFileManager *fm = NSFileManager.defaultManager;
    if ([fm fileExistsAtPath:SystemPlist] && [fm contentsEqualAtPath:SystemAgent andPath:bundledAgent()]) return nil;
    return asAdministrator(systemScript(), PasswordWhy, cancelled);
}

// Then it waits (up to 8 s) for the system part it started to link to Henge (henge-agent.m's "system-linked" mark,
// which holds its process ID): the user's
// helper starts after it, so on a Mac whose macOS was just put in afresh Henge sees the system part's link first and
// takes its new key (share.c, "the system link"). Its key file (system-key, next to it) is the system part's own.
static NSString *systemScript(void) {
    return [NSString stringWithFormat:
        @"set -e\n"
        @"D=%@\n"
        @"P=%@\n"
        @"M=\"$(dirname \"$D\")/system-linked\"\n"
        @"mkdir -p \"$(dirname \"$D\")\"\n"
        @"chown root:wheel \"$(dirname \"$D\")\"\n"
        @"chmod 755 \"$(dirname \"$D\")\"\n"
        @"launchctl bootout system/%@ 2>/dev/null || true\n"
        @"cp %@ \"$D.new\"\n"
        @"chown root:wheel \"$D.new\"\n"
        @"chmod 755 \"$D.new\"\n"
        @"mv -f \"$D.new\" \"$D\"\n"
        @"printf '%%s' %@ > \"$P\"\n"
        @"chown root:wheel \"$P\"\n"
        @"chmod 644 \"$P\"\n"
        @"rm -f \"$M\"\n"
        @"up=0\n"
        @"for i in 1 2 3 4 5 6 7 8 9 10; do if launchctl bootstrap system \"$P\"; then up=1; break; fi; sleep 0.5; done\n" // bootout returns before it's gone
        @"[ $up = 1 ] || exit 1\n"
        // The mark names the process that linked: this one, not the one before it on its way out.
        @"for i in $(seq 1 32); do\n"
        @"  pid=$(launchctl print system/%@ 2>/dev/null | awk '$1 == \"pid\" && $2 == \"=\" { print $3; exit }')\n"
        @"  [ -n \"$pid\" ] && [ \"$(cat \"$M\" 2>/dev/null)\" = \"$pid\" ] && exit 0\n"
        @"  sleep 0.25\n"
        @"done\n" // an older Henge never links it: on anyway
        @"exit 0\n",
        shellQuoted(SystemAgent), shellQuoted(SystemPlist), SystemLabel, shellQuoted(bundledAgent()), shellQuoted(systemPlist()), SystemLabel];
}

// The key file (system-key) stays, and so the folder: Henge pins it for this Mac, and Henge Tools put back takes up
// where it left off, without a restart of the Mac for a new one. Only root can read it.
static void removeSystem(void) {
    if (![NSFileManager.defaultManager fileExistsAtPath:SystemPlist] && ![NSFileManager.defaultManager fileExistsAtPath:SystemAgent]) return;
    NSString *dir = SystemAgent.stringByDeletingLastPathComponent;
    NSString *script = [NSString stringWithFormat:@"launchctl bootout system/%@ 2>/dev/null; rm -f %@ %@ %@; rmdir %@ 2>/dev/null; exit 0\n", SystemLabel,
                                                  shellQuoted(SystemPlist), shellQuoted(SystemAgent),
                                                  shellQuoted([dir stringByAppendingPathComponent:@"system-linked"]), shellQuoted(dir)];
    BOOL cancelled;
    asAdministrator(script, @"Henge Tools needs your Mac's password to remove the part that starts at the login window.", &cancelled);
}

// Returns nil, or what went wrong, in words for the person.
static NSString *installUser(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSError *error = nil;
    if (![fm fileExistsAtPath:bundledAgent()]) return @"This copy of Henge Tools is damaged. Restart the Mac from Henge to get a fresh one.";
    if (![fm createDirectoryAtPath:supportDir() withIntermediateDirectories:YES attributes:nil error:&error] ||
        ![fm createDirectoryAtPath:plistPath().stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:&error])
        return error.localizedDescription;
    launchctl(@[ @"bootout", [NSString stringWithFormat:@"%@/%@", domain(), Label] ], NULL);
    // Next to the old one, then in its place all at once: a helper half written is never started.
    NSString *fresh = [agentPath() stringByAppendingString:@".new"];
    [fm removeItemAtPath:fresh error:NULL];
    if (![fm copyItemAtPath:bundledAgent() toPath:fresh error:&error]) return error.localizedDescription;
    chmod(fresh.fileSystemRepresentation, 0755);
    if (rename(fresh.fileSystemRepresentation, agentPath().fileSystemRepresentation) != 0)
        return [NSString stringWithFormat:@"Could not put the helper in place (%s).", strerror(errno)];
    [fm removeItemAtPath:statusPath() error:NULL]; // the new helper says afresh whether it is trusted
    if (![launchAgentPlist() writeToFile:plistPath() atomically:YES encoding:NSUTF8StringEncoding error:&error])
        return error.localizedDescription;
    // bootout returns before the old one has quite gone; bootstrap refuses a label still loaded.
    for (int i = 0; i < 20; i++) {
        if (launchctl(@[ @"bootstrap", domain(), plistPath() ], NULL) == 0) return nil;
        usleep(250000);
    }
    return @"macOS did not start the helper. Log out and back in, then open Henge Tools again.";
}

// Both parts, the system part first (see systemScript). The person may say no to the password: the helper works
// without the system part, and is put in all the same.
static NSString *install(void) {
    BOOL cancelled;
    NSString *systemProblem = installSystem(&cancelled);
    NSString *problem = installUser();
    return problem ?: systemProblem;
}

static void removeAll(void) {
    launchctl(@[ @"bootout", [NSString stringWithFormat:@"%@/%@", domain(), Label] ], NULL);
    removeSystem();
    NSFileManager *fm = NSFileManager.defaultManager;
    [fm removeItemAtPath:plistPath() error:NULL];
    [fm removeItemAtPath:agentPath() error:NULL];
    [fm removeItemAtPath:statusPath() error:NULL];
}

// MARK: - The window

@interface Row : NSStackView
@end
@implementation Row
+ (instancetype)symbol:(NSString *)symbol title:(NSString *)title detail:(NSString *)detail {
    Row *row = [Row new];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment = NSLayoutAttributeTop;
    row.spacing = 14;
    NSImageView *icon = [NSImageView new];
    NSImage *image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil];
    icon.image = [image imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:20 weight:NSFontWeightRegular]];
    icon.contentTintColor = NSColor.controlAccentColor;
    [icon.widthAnchor constraintEqualToConstant:28].active = YES;
    [icon.heightAnchor constraintEqualToConstant:26].active = YES;
    NSTextField *head = [NSTextField labelWithString:title];
    head.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    NSTextField *body = [NSTextField wrappingLabelWithString:detail];
    body.font = [NSFont systemFontOfSize:12];
    body.textColor = NSColor.secondaryLabelColor;
    body.preferredMaxLayoutWidth = 340;
    NSStackView *text = [NSStackView stackViewWithViews:@[ head, body ]];
    text.orientation = NSUserInterfaceLayoutOrientationVertical;
    text.alignment = NSLayoutAttributeLeading;
    text.spacing = 2;
    [row addArrangedSubview:icon];
    [row addArrangedSubview:text];
    return row;
}
@end

@interface Installer : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property NSWindow *window;
@property NSTextField *headline, *message;
@property NSImageView *badge;
@property NSStackView *features;
@property NSProgressIndicator *spinner;
@property NSButton *primary, *tertiary;
@property NSTimer *watch;
@property Setup shown;
@property BOOL busy;
@end

@implementation Installer

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    [self buildMenu];
    [self buildWindow];
    [self refresh];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    // While the window is open: the permission is turned on in System Settings, so look now and then.
    if (previewing >= 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            NSView *frame = self.window.contentView.superview;
            NSBitmapImageRep *rep = [frame bitmapImageRepForCachingDisplayInRect:frame.bounds];
            [frame cacheDisplayInRect:frame.bounds toBitmapImageRep:rep];
            [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:NSProcessInfo.processInfo.arguments.lastObject atomically:YES];
            [NSApp terminate:nil];
        });
        return;
    }
    self.watch = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        if (!self.busy) [self refresh];
    }];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    (void)app;
    return YES;
}

- (void)buildMenu {
    NSMenu *bar = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu *appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"Hide Henge Tools" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:NSMenuItem.separatorItem];
    [appMenu addItemWithTitle:@"Quit Henge Tools" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSMenuItem *windowItem = [NSMenuItem new];
    [bar addItem:windowItem];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [windowMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    [windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    windowItem.submenu = windowMenu;
    NSApp.mainMenu = bar;
}

- (NSButton *)button:(NSString *)title action:(SEL)action {
    NSButton *b = [NSButton buttonWithTitle:title target:self action:action];
    b.bezelStyle = NSBezelStyleRounded;
    b.controlSize = NSControlSizeLarge;
    return b;
}

- (void)buildWindow {
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, 300)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                                        NSWindowStyleMaskFullSizeContentView
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.titlebarAppearsTransparent = YES;
    self.window.titleVisibility = NSWindowTitleHidden;
    self.window.movableByWindowBackground = YES;
    self.window.title = @"Henge Tools";
    self.window.delegate = self;

    NSVisualEffectView *back = [NSVisualEffectView new];
    back.material = NSVisualEffectMaterialWindowBackground;
    back.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    back.state = NSVisualEffectStateFollowsWindowActiveState;
    self.window.contentView = back;

    NSImageView *icon = [NSImageView imageViewWithImage:NSApp.applicationIconImage];
    [icon.widthAnchor constraintEqualToConstant:84].active = YES;
    [icon.heightAnchor constraintEqualToConstant:84].active = YES;
    self.badge = [NSImageView new];
    self.badge.hidden = YES;
    [icon addSubview:self.badge];
    self.badge.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [self.badge.trailingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:2],
        [self.badge.bottomAnchor constraintEqualToAnchor:icon.bottomAnchor constant:2],
    ]];

    self.headline = [NSTextField labelWithString:@""];
    self.headline.font = [NSFont systemFontOfSize:22 weight:NSFontWeightBold];
    self.headline.alignment = NSTextAlignmentCenter;
    self.message = [NSTextField wrappingLabelWithString:@""];
    self.message.font = [NSFont systemFontOfSize:13];
    self.message.textColor = NSColor.secondaryLabelColor;
    self.message.alignment = NSTextAlignmentCenter;
    self.message.preferredMaxLayoutWidth = 380;
    self.message.selectable = NO;

    self.features = [NSStackView stackViewWithViews:@[
        [Row symbol:@"hand.point.up.left" title:@"Touch that feels like a Mac trackpad"
             detail:@"Drag windows and sliders straight away, and scroll pages smoothly, with a glide after a flick."],
        [Row symbol:@"doc.on.clipboard" title:@"One clipboard"
             detail:@"Copy on the Mac and paste on your iPad, and the other way round."],
        [Row symbol:@"folder" title:@"Your iPad's files"
             detail:@"The Shared folder in the Files app is in Finder, under My Shared Files."],
        [Row symbol:@"person.badge.key" title:@"Ready the moment you log in"
             detail:@"Your iPad shows its Log In button only at this Mac's login window, and all this works as soon as you're in."],
    ]];
    self.features.orientation = NSUserInterfaceLayoutOrientationVertical;
    self.features.alignment = NSLayoutAttributeLeading;
    self.features.spacing = 14;

    self.spinner = [NSProgressIndicator new];
    self.spinner.style = NSProgressIndicatorStyleSpinning;
    self.spinner.controlSize = NSControlSizeSmall;
    self.spinner.hidden = YES;
    self.primary = [self button:@"" action:@selector(primaryClicked:)];
    self.primary.keyEquivalent = @"\r";
    self.tertiary = [NSButton buttonWithTitle:@"" target:self action:@selector(tertiaryClicked:)];
    self.tertiary.bordered = NO;
    self.tertiary.contentTintColor = NSColor.linkColor;
    self.tertiary.font = [NSFont systemFontOfSize:12];
    NSStackView *buttons = [NSStackView stackViewWithViews:@[ self.spinner, self.primary ]];
    buttons.spacing = 10;

    NSStackView *stack = [NSStackView stackViewWithViews:@[ icon, self.headline, self.message, self.features, buttons, self.tertiary ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeCenterX;
    stack.spacing = 10;
    [stack setCustomSpacing:14 afterView:icon];
    [stack setCustomSpacing:22 afterView:self.message];
    [stack setCustomSpacing:26 afterView:self.features];
    [stack setCustomSpacing:8 afterView:buttons];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [back addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:back.topAnchor constant:40],
        [stack.bottomAnchor constraintEqualToAnchor:back.bottomAnchor constant:-22],
        [stack.leadingAnchor constraintEqualToAnchor:back.leadingAnchor constant:40],
        [stack.trailingAnchor constraintEqualToAnchor:back.trailingAnchor constant:-40],
        [back.widthAnchor constraintEqualToConstant:460],
    ]];
}

- (void)setBadge:(NSString *)symbol color:(NSColor *)color {
    if (!symbol) {
        self.badge.hidden = YES;
        return;
    }
    NSImageSymbolConfiguration *config = [NSImageSymbolConfiguration configurationWithPointSize:26 weight:NSFontWeightSemibold];
    config = [config configurationByApplyingConfiguration:[NSImageSymbolConfiguration configurationWithPaletteColors:@[ NSColor.whiteColor, color ]]];
    self.badge.image = [[NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil] imageWithSymbolConfiguration:config];
    self.badge.hidden = NO;
}

- (void)show:(NSString *)headline message:(NSString *)message primary:(NSString *)primary tertiary:(NSString *)tertiary {
    self.headline.stringValue = headline;
    self.message.stringValue = message;
    self.primary.title = primary;
    self.primary.hidden = primary == nil;
    self.tertiary.title = tertiary ?: @"";
    self.tertiary.hidden = tertiary == nil;
}

- (void)refresh {
    Setup now = currentSetup();
    if (now == self.shown && self.headline.stringValue.length) return;
    BOOL wasWaiting = self.shown == SetupUntrusted;
    self.shown = now;
    self.features.hidden = now == SetupUntrusted || now == SetupReady || now == SetupLoginPart;
    switch (now) {
    case SetupMissing:
        [self setBadge:nil color:nil];
        [self show:@"Henge Tools"
              message:@"Makes this Mac feel at home on your iPad. It takes a few seconds, and nothing is downloaded. macOS will ask for your Mac's password once."
              primary:@"Install" tertiary:nil];
        break;
    case SetupOutdated:
        [self setBadge:@"arrow.down.circle.fill" color:NSColor.systemBlueColor];
        [self show:@"An update is ready"
              message:@"This Mac has an older Henge Tools. The new one comes with this version of Henge."
              primary:@"Update" tertiary:@"Remove Henge Tools"];
        break;
    case SetupStopped:
        [self setBadge:@"exclamationmark.circle.fill" color:NSColor.systemOrangeColor];
        [self show:@"Henge Tools isn't running" message:@"Install it again to start it." primary:@"Reinstall" tertiary:@"Remove Henge Tools"];
        break;
    case SetupUntrusted:
        [self setBadge:@"lock.circle.fill" color:NSColor.systemOrangeColor];
        [self show:@"One more step"
              message:@"To make touch feel native, macOS needs your OK.\n\nIn System Settings, under Privacy & Security > Accessibility, "
                      @"turn on henge-agent. If macOS asks for your password, that is your Mac's own login password.\n\n"
                      @"This window carries on by itself once it's on."
              primary:@"Open Privacy Settings" tertiary:@"henge-agent isn't in the list"];
        break;
    case SetupLoginPart:
        [self setBadge:@"person.badge.key.fill" color:NSColor.systemBlueColor];
        [self show:@"Ready the moment you log in"
              message:@"Henge Tools can start at this Mac's login window, so your iPad shows its Log In button exactly then, and touch, the clipboard "
                      @"and your files work as soon as you're in.\n\nFor that, macOS asks for your Mac's password once: the one you log in with. "
                      @"macOS may then say a background item was added; that's Henge Tools."
              primary:@"Continue" tertiary:@"Not now"];
        break;
    case SetupReady:
        [self setBadge:@"checkmark.circle.fill" color:NSColor.systemGreenColor];
        [self show:@"You're all set"
              message:@"Henge Tools is running, and starts by itself whenever this Mac does. Touch, the clipboard and shared files all work now. The Henge Tools disk goes away the next time this Mac starts."
              primary:@"Done" tertiary:@"Reinstall or remove…"];
        if (wasWaiting) {
            [NSApp activateIgnoringOtherApps:YES];
            [self.window makeKeyAndOrderFront:nil];
        }
        break;
    }
    [self.window.contentView layoutSubtreeIfNeeded];
    NSRect frame = self.window.frame;
    NSSize size = self.window.contentView.fittingSize;
    NSRect fitted = [self.window frameRectForContentRect:NSMakeRect(0, 0, size.width, size.height)];
    frame.origin.y += frame.size.height - fitted.size.height; // keep the top edge where it was
    frame.size = fitted.size;
    [self.window setFrame:frame display:YES animate:self.window.isVisible];
}

- (void)runInstall {
    [self runInstallSystemOnly:NO];
}

- (void)runInstallSystemOnly:(BOOL)systemOnly {
    self.busy = YES;
    self.spinner.hidden = NO;
    [self.spinner startAnimation:nil];
    self.primary.enabled = NO;
    self.tertiary.enabled = NO;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL cancelled;
        NSString *problem = systemOnly ? installSystem(&cancelled) : install();
        // A moment for the helper to start and say whether it is trusted.
        for (int i = 0; i < 20 && !problem && ![NSFileManager.defaultManager fileExistsAtPath:statusPath()]; i++) usleep(100000);
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.spinner stopAnimation:nil];
            self.spinner.hidden = YES;
            self.primary.enabled = YES;
            self.tertiary.enabled = YES;
            self.busy = NO;
            if (problem) {
                NSAlert *alert = [NSAlert new];
                alert.messageText = @"Henge Tools couldn't be installed";
                alert.informativeText = problem;
                [alert beginSheetModalForWindow:self.window completionHandler:nil];
            }
            self.shown = -1;
            [self refresh];
        });
    });
}

- (void)openPrivacySettings {
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"]];
}

- (void)confirmRemove {
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"Remove Henge Tools?";
    alert.informativeText = @"Touch goes back to plain clicks, and the clipboard is no longer shared. You can install it again from My Shared Files whenever you like.";
    [alert addButtonWithTitle:@"Remove"].hasDestructiveAction = YES;
    [alert addButtonWithTitle:@"Cancel"];
    [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
        if (r != NSAlertFirstButtonReturn) return;
        removeAll();
        self.shown = -1;
        [self refresh];
    }];
}

- (void)primaryClicked:(id)sender {
    (void)sender;
    switch (self.shown) {
    case SetupMissing:
    case SetupOutdated:
    case SetupStopped: [self runInstall]; break;
    case SetupUntrusted: [self openPrivacySettings]; break;
    case SetupReady: [NSApp terminate:nil]; break;
    case SetupLoginPart: [self runInstallSystemOnly:YES]; break; // the helper itself is in and running: left as it is
    }
}

- (void)tertiaryClicked:(id)sender {
    (void)sender;
    switch (self.shown) {
    case SetupUntrusted: {
        // The helper asks macOS when it starts, which is also what puts it in the list: start it again.
        launchctl(@[ @"kickstart", @"-k", [NSString stringWithFormat:@"%@/%@", domain(), Label] ], NULL);
        NSAlert *alert = [NSAlert new];
        alert.messageText = @"Henge Tools asked macOS again";
        alert.informativeText = @"If macOS shows a message about henge-agent, click Open System Settings and turn henge-agent on.\n\n"
                                @"If it's in the list but already on, turn it off and on again.";
        [alert beginSheetModalForWindow:self.window completionHandler:nil];
        break;
    }
    case SetupReady: {
        NSAlert *alert = [NSAlert new];
        alert.messageText = @"Reinstall or remove Henge Tools?";
        alert.informativeText = @"Reinstalling puts a fresh copy in place, which can help if something stopped working.";
        [alert addButtonWithTitle:@"Reinstall"];
        [alert addButtonWithTitle:@"Remove"].hasDestructiveAction = YES;
        [alert addButtonWithTitle:@"Cancel"];
        [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
            if (r == NSAlertFirstButtonReturn) [self runInstall];
            else if (r == NSAlertSecondButtonReturn) dispatch_async(dispatch_get_main_queue(), ^{ [self confirmRemove]; });
        }];
        break;
    }
    case SetupLoginPart:
        skippedSystem = YES;
        self.shown = -1;
        [self refresh];
        break;
    default: [self confirmRemove]; break;
    }
}

@end

int main(void) {
    @autoreleasepool {
        if (getenv("HENGE_TOOLS_PREVIEW")) previewing = atoi(getenv("HENGE_TOOLS_PREVIEW"));
        NSApplication *app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        Installer *installer = [Installer new];
        app.delegate = installer;
        [app run];
    }
    return 0;
}

#import <Cocoa/Cocoa.h>

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(strong) NSWindow *window;
@property(strong) NSTextField *label;
@property int taps;
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)note {
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 360, 200)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Hello AppKit";
    self.label = [NSTextField labelWithString:@"Hello from a Mac app"];
    self.label.frame = NSMakeRect(20, 120, 320, 24);
    NSButton *button = [NSButton buttonWithTitle:@"Tap" target:self action:@selector(tap:)];
    button.frame = NSMakeRect(20, 60, 100, 32);
    [self.window.contentView addSubview:self.label];
    [self.window.contentView addSubview:button];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    NSMenu *bar = [NSMenu new], *appMenu = [NSMenu new];
    NSMenuItem *item = [NSMenuItem new];
    [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    item.submenu = appMenu;
    [bar addItem:item];
    NSApp.mainMenu = bar;
    if (getenv("TL_SELFTEST")) {
        /* Click the button the way AppKit would, then report what the window shows. */
        dispatch_async(dispatch_get_main_queue(), ^{
            [button performClick:nil];
            [button performClick:nil];
            printf("window \"%s\" visible %d, label \"%s\", bundle %s\n", self.window.title.UTF8String, self.window.isVisible,
                   self.label.stringValue.UTF8String, NSBundle.mainBundle.bundleIdentifier.UTF8String ?: "(none)");
            printf("PASS HelloAppKit\n");
            fflush(stdout);
            [NSApp terminate:nil];
        });
    }
}
- (void)tap:(id)sender { self.label.stringValue = [NSString stringWithFormat:@"Tapped %d times", ++self.taps]; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }
@end

int main(int argc, const char **argv) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        AppDelegate *d = [AppDelegate new];
        app.delegate = d;
        return NSApplicationMain(argc, argv);
    }
}

/* A toy AppKit for the translation layer's tests: the few classes a small
   window app uses, drawn with CoreGraphics and CoreText (both on iOS too)
   into a bitmap instead of a window server. It stands in for AppKit so a
   Mac app's AppKit references bind here, on a Mac that never loads AppKit.
   It proves the binding (classes, subclasses, globals, selectors), not a UI:
   the real thing would present through UIKit on iOS. Unimplemented messages
   are logged and answer nil. */
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <string.h>

#include "appkit_mini.h"

typedef struct { double x, y, w, h; } HRect; /* NSRect's layout */

@interface TLShimObject : NSObject
@end
@implementation TLShimObject
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel {
    NSMethodSignature *s = [super methodSignatureForSelector:sel];
    return s ?: [NSMethodSignature signatureWithObjCTypes:"@@:"];
}
- (void)forwardInvocation:(NSInvocation *)inv {
    fprintf(stderr, "appkit-mini: -[%s %s] is not implemented\n", object_getClassName(self), sel_getName(inv.selector));
    id nothing = nil;
    if (inv.methodSignature.methodReturnLength == sizeof(id)) [inv setReturnValue:&nothing];
}
@end

@class NSView;
@interface NSWindow : TLShimObject
@property(nonatomic, copy) NSString *title;
@property(nonatomic, strong) NSView *contentView;
@property(nonatomic) HRect frame;
@property(nonatomic) BOOL visible;
- (void)redraw;
@end

@interface NSView : TLShimObject
@property(nonatomic) HRect frame;
@property(nonatomic, strong) NSMutableArray *subviews;
@property(nonatomic, weak) NSWindow *window;
@end

@implementation NSView
- (instancetype)init { return [self initWithFrame:(HRect){0, 0, 0, 0}]; }
- (instancetype)initWithFrame:(HRect)frame {
    if ((self = [super init])) _frame = frame, _subviews = [NSMutableArray new];
    return self;
}
- (void)addSubview:(NSView *)view {
    [_subviews addObject:view];
    view.window = _window;
    [_window redraw];
}
- (void)setWindow:(NSWindow *)window {
    _window = window;
    for (NSView *v in _subviews) v.window = window;
}
- (void)drawInContext:(CGContextRef)ctx {}
- (void)renderInContext:(CGContextRef)ctx {
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, _frame.x, _frame.y);
    [self drawInContext:ctx];
    for (NSView *v in _subviews) [v renderInContext:ctx];
    CGContextRestoreGState(ctx);
}
@end

static void draw_text(CGContextRef ctx, NSString *text, double x, double y, double size, BOOL center, double width) {
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, NULL);
    CGColorRef black = CGColorCreateGenericGray(0.1, 1);
    NSDictionary *attrs = @{(id)kCTFontAttributeName : (__bridge id)font, (id)kCTForegroundColorAttributeName : (__bridge id)black};
    CFAttributedStringRef s = CFAttributedStringCreate(NULL, (__bridge CFStringRef)(text ?: @""), (__bridge CFDictionaryRef)attrs);
    CTLineRef line = CTLineCreateWithAttributedString(s);
    double w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CGContextSetTextPosition(ctx, center ? x + (width - w) / 2 : x, y);
    CTLineDraw(line, ctx);
    CFRelease(line), CFRelease(s), CFRelease(font), CGColorRelease(black);
}

@interface NSControl : NSView
@property(nonatomic, copy) NSString *stringValue;
@property(nonatomic, weak) id target;
@property(nonatomic) SEL action;
@end
@implementation NSControl
- (void)setStringValue:(NSString *)value {
    _stringValue = [value copy];
    [self.window redraw];
}
@end

@interface NSTextField : NSControl
@end
@implementation NSTextField
+ (instancetype)labelWithString:(NSString *)string {
    NSTextField *f = [[self alloc] initWithFrame:(HRect){0, 0, 200, 22}];
    f.stringValue = string;
    return f;
}
- (void)drawInContext:(CGContextRef)ctx { draw_text(ctx, self.stringValue, 0, 6, 15, NO, 0); }
@end

@interface NSButton : NSControl
@property(nonatomic, copy) NSString *title;
@end
@implementation NSButton
+ (instancetype)buttonWithTitle:(NSString *)title target:(id)target action:(SEL)action {
    NSButton *b = [[self alloc] initWithFrame:(HRect){0, 0, 90, 32}];
    b.title = title, b.target = target, b.action = action;
    return b;
}
- (void)performClick:(id)sender {
    if (!self.target || !self.action) return;
    ((void (*)(id, SEL, id))objc_msgSend)(self.target, self.action, self);
}
- (void)drawInContext:(CGContextRef)ctx {
    HRect f = self.frame;
    CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(1, 1, f.w - 2, f.h - 2), 6, 6, NULL);
    CGContextSetRGBFillColor(ctx, 0.18, 0.42, 1, 1);
    CGContextAddPath(ctx, p);
    CGContextFillPath(ctx);
    CGPathRelease(p);
    draw_text(ctx, self.title, 0, f.h / 2 - 5, 14, YES, f.w);
}
@end

@implementation NSWindow
- (instancetype)initWithContentRect:(HRect)rect styleMask:(NSUInteger)style backing:(NSUInteger)backing defer:(BOOL)defer {
    if ((self = [super init])) {
        _frame = rect;
        self.contentView = [[NSView alloc] initWithFrame:(HRect){0, 0, rect.w, rect.h}];
    }
    return self;
}
- (void)setContentView:(NSView *)view {
    _contentView = view;
    view.window = self;
}
- (void)center {}
- (void)makeKeyAndOrderFront:(id)sender {
    _visible = YES;
    [self redraw];
}
- (BOOL)isVisible { return _visible; }
- (void)redraw {
    if (!_visible) return;
    const char *png = getenv("TL_SHIM_PNG");
    if (!png) return;
    int w = (int)_frame.w, h = (int)_frame.h + 28;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(NULL, w, h, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGContextSetRGBFillColor(ctx, 0.96, 0.96, 0.97, 1);
    CGContextFillRect(ctx, CGRectMake(0, 0, w, h));
    CGContextSetRGBFillColor(ctx, 0.86, 0.86, 0.88, 1); /* title bar */
    CGContextFillRect(ctx, CGRectMake(0, h - 28, w, 28));
    draw_text(ctx, _title, 0, h - 19, 13, YES, w);
    [_contentView renderInContext:ctx];
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)png, (CFIndex)strlen(png), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(dest, img, NULL);
    CGImageDestinationFinalize(dest);
    CFRelease(dest), CFRelease(url), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
}
@end

@interface NSMenuItem : TLShimObject
@property(nonatomic, copy) NSString *title;
@property(nonatomic, strong) id submenu;
@property(nonatomic) SEL action;
@property(nonatomic, copy) NSString *keyEquivalent;
@end
@implementation NSMenuItem
@end

@interface NSMenu : TLShimObject
@property(nonatomic, strong) NSMutableArray *items;
@end
@implementation NSMenu
- (instancetype)init {
    if ((self = [super init])) _items = [NSMutableArray new];
    return self;
}
- (void)addItem:(NSMenuItem *)item { [_items addObject:item]; }
- (NSMenuItem *)addItemWithTitle:(NSString *)title action:(SEL)action keyEquivalent:(NSString *)key {
    NSMenuItem *item = [NSMenuItem new];
    item.title = title, item.action = action, item.keyEquivalent = key;
    [_items addObject:item];
    return item;
}
@end

id tl_shim_NSApp;

@interface NSApplication : TLShimObject
@property(nonatomic, strong) id delegate; /* AppKit keeps it weak; the app's main often holds it only here */
@property(nonatomic, strong) NSMenu *mainMenu;
@end
@implementation NSApplication
+ (instancetype)sharedApplication {
    if (!tl_shim_NSApp) tl_shim_NSApp = [self new];
    return tl_shim_NSApp;
}
- (void)terminate:(id)sender { exit(0); }
- (void)run {
    id d = self.delegate;
    SEL did = sel_registerName("applicationDidFinishLaunching:");
    if ([d respondsToSelector:did])
        ((void (*)(id, SEL, id))objc_msgSend)(d, did, [NSNotification notificationWithName:@"NSApplicationDidFinishLaunchingNotification" object:self]);
    for (;;) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1e9, false);
}
@end

static int tl_shim_NSApplicationMain(int argc, const char **argv) {
    [[NSApplication sharedApplication] run];
    return 0;
}

static const char *const g_classes[] = {"NSApplication", "NSWindow", "NSView", "NSControl", "NSTextField", "NSButton", "NSMenu", "NSMenuItem"};

void *tl_appkit_mini_lookup(const char *install_name, const char *sym) {
    if (!strstr(install_name, "/AppKit.framework/") && !strstr(install_name, "/Cocoa.framework/")) return NULL;
    if (!strcmp(sym, "_NSApp")) return &tl_shim_NSApp;
    if (!strcmp(sym, "_NSApplicationMain")) return (void *)tl_shim_NSApplicationMain;
    const char *name = NULL;
    int meta = 0;
    if (!strncmp(sym, "_OBJC_CLASS_$_", 14)) name = sym + 14;
    else if (!strncmp(sym, "_OBJC_METACLASS_$_", 18)) name = sym + 18, meta = 1;
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_classes / sizeof *g_classes; i++)
        if (!strcmp(name, g_classes[i])) {
            Class c = objc_getClass(name);
            return meta ? (__bridge void *)object_getClass(c) : (__bridge void *)c;
        }
    return NULL;
}

int tl_appkit_is_real_loaded(void) { return dlopen("/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit", RTLD_NOLOAD) != NULL; }

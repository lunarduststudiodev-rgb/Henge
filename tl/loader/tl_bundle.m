/* The guest app's bundle as the process's main bundle. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include "tl_loader.h"

static NSBundle *g_bundle;

static NSBundle *main_bundle(id self, SEL _cmd) { return g_bundle; }

int tl_adopt_main_bundle(const char *app_path) {
    @autoreleasepool {
        NSBundle *b = [NSBundle bundleWithPath:@(app_path)];
        if (!b || !b.bundleIdentifier) return -1;
        g_bundle = b;
        Method m = class_getClassMethod([NSBundle class], @selector(mainBundle));
        method_setImplementation(m, (IMP)main_bundle);
        /* Not yet: CFBundleGetMainBundle() still answers the host's bundle (LiveContainer rewrites the CFBundle). */
        return 0;
    }
}

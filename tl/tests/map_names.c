#include <stdio.h>
#include <string.h>
#include "../loader/tl_loader.h"

static int check(const char *in, const char *want) {
    char buf[1024];
    const char *got = tl_ios_map_install_name(in, buf, sizeof buf);
    int ok = (!got && !want) || (got && want && !strcmp(got, want));
    if (!ok) printf("FAIL %s -> %s (want %s)\n", in, got ? got : "NULL", want ? want : "NULL");
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= check("/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation", "/System/Library/Frameworks/Foundation.framework/Foundation");
    ok &= check("/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit", NULL);
    ok &= check("/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa", NULL);
    ok &= check("/System/iOSSupport/System/Library/Frameworks/UIKit.framework/Versions/A/UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit");
    ok &= check("/System/Library/Frameworks/SwiftUI.framework/Versions/A/SwiftUI", "/System/Library/Frameworks/SwiftUI.framework/SwiftUI");
    ok &= check("/usr/lib/libSystem.B.dylib", "/usr/lib/libSystem.B.dylib");
    ok &= check("/usr/lib/swift/libswiftCore.dylib", "/usr/lib/swift/libswiftCore.dylib");
    ok &= check("/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight", "/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight");
    printf("%s map_names\n", ok ? "PASS" : "FAIL");
    return !ok;
}

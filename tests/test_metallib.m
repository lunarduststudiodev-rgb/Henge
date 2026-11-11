/* pvg_metallib.h: relabelling the GPU libraries macOS 15 sends for an older iOS's Metal (GitHub #44).
   A made-up library checks what changes and what is left alone; a real macOS 15 library, where this Mac has one,
   checks that Metal still reads it relabelled. Loading a library runs nothing on the GPU. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include "pvg_metallib.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v), put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32)); }

/* A header and a function list of two functions, each with a NAME, a VERS and a HASH tag, laid out as macOS 15's. */
static size_t make_lib(uint8_t *b, uint16_t major, uint16_t minor, uint8_t os, uint16_t os_major) {
    memset(b, 0, 512);
    memcpy(b, "MTLB", 4);
    put16(b + 4, 0x8001), put16(b + 6, major), put16(b + 8, minor);
    b[10] = 0, b[11] = os, put16(b + 12, os_major), put16(b + 14, 2);
    uint8_t *p = b + 88 + 4;
    for (int f = 0; f < 2; f++) {
        uint8_t *fn = p;
        p += 4;
        memcpy(p, "NAME", 4), put16(p + 4, 3), memcpy(p + 6, f ? "fb" : "fa", 3), p += 9;
        memcpy(p, "VERS", 4), put16(p + 4, 8), put16(p + 6, 2), put16(p + 8, 7), put16(p + 10, 3), put16(p + 12, 2), p += 14;
        memcpy(p, "HASH", 4), put16(p + 4, 32), memset(p + 6, 0xab, 32), p += 38;
        memcpy(p, "ENDT", 4), p += 4;
        put32(fn, (uint32_t)(p - fn));
    }
    put32(b + 88, 2);
    size_t len = (size_t)(p - b);
    put64(b + 16, len), put64(b + 24, 88), put64(b + 32, len - 88 - 4);
    return len;
}

static const uint8_t *vers_of(const uint8_t *b, int f) { /* the second function's VERS, by the layout above */
    const uint8_t *fn = b + 88 + 4;
    if (f) fn += fn[0] | fn[1] << 8;
    return fn + 4 + 9 + 6;
}

static void made_up(void) {
    uint8_t b[512], before[512];
    size_t len = make_lib(b, 2, 8, 0x81, 15);
    memcpy(before, b, sizeof b);
    CHECK(hv_metallib_format(b, len) == (2u << 16 | 8));
    CHECK(hv_metallib_lower(b, len) == 1);
    CHECK(hv_metallib_format(b, len) == (2u << 16 | 7));
    CHECK(b[12] == 14 && b[13] == 0 && b[14] == 0); /* macOS 15.2 -> 14.0 */
    for (int f = 0; f < 2; f++) {
        const uint8_t *v = vers_of(b, f);
        CHECK(v[0] == 2 && v[2] == 6 && v[4] == 3 && v[6] == 1); /* AIR 2.6, Metal 3.1 */
    }
    /* Nothing else moves: the platform, type, sizes, names and hashes. */
    CHECK(!memcmp(b + 4, before + 4, 2) && b[10] == before[10] && b[11] == before[11]);
    CHECK(!memcmp(b + 16, before + 16, 72));
    CHECK(!memcmp(b + 88, before + 88, 4 + 4 + 9)); /* count, first size, NAME */
    CHECK(!memcmp(vers_of(b, 0) + 8, vers_of(before, 0) + 8, 38 + 4)); /* HASH and ENDT after VERS */
    CHECK(hv_metallib_lower(b, len) == 0); /* already old enough: nothing more to try */

    len = make_lib(b, 2, 9, 0x82, 18); /* an iOS library: iOS 18 -> 17 */
    CHECK(hv_metallib_lower(b, len) == 1 && b[12] == 17);
    len = make_lib(b, 2, 7, 0x81, 14);
    CHECK(hv_metallib_lower(b, len) == 0);

    /* Bytes that don't make sense are left exactly as they were. */
    len = make_lib(b, 2, 8, 0x81, 15);
    memcpy(before, b, sizeof b);
    CHECK(hv_metallib_lower(b, 40) == -1);
    put64(b + 32, len); /* the list runs off the end */
    memcpy(before, b, sizeof b);
    CHECK(hv_metallib_lower(b, len) == -1 && !memcmp(b, before, sizeof b));
    len = make_lib(b, 2, 8, 0x81, 15);
    put16(b + 88 + 4 + 4 + 4, 400); /* NAME says it is longer than its function */
    memcpy(before, b, sizeof b);
    CHECK(hv_metallib_lower(b, len) == -1 && !memcmp(b, before, sizeof b));
    len = make_lib(b, 2, 8, 0x81, 15);
    memcpy(b + 88 + 4 + 4 + 9 + 14 + 38, "XXXX", 4); /* no ENDT */
    CHECK(hv_metallib_lower(b, len) == -1);
    len = make_lib(b, 2, 8, 0x81, 15);
    memcpy(b, "MTLX", 4);
    CHECK(hv_metallib_format(b, len) == 0 && hv_metallib_lower(b, len) == -1);
}

/* A real macOS 15 library (Xcode ships some) and a macOS 27 one, relabelled: Metal still reads every function. */
static void real(void) {
    const char *paths[] = {
        "/Applications/Xcode.app/Contents/SystemFrameworks/VFX.framework/Versions/A/Resources/default.metallib",
        "/System/Library/PrivateFrameworks/Portrait.framework/Versions/A/Resources/default.metallib",
    };
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { printf("  no Metal device: real libraries skipped\n"); return; }
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) @autoreleasepool {
        NSMutableData *d = [NSMutableData dataWithContentsOfFile:@(paths[i])];
        uint32_t format = d ? hv_metallib_format(d.bytes, d.length) : 0;
        if (format <= (2u << 16 | 7)) { printf("  %s: not here (or not newer than 2.7), skipped\n", paths[i]); continue; }
        dispatch_data_t sent = dispatch_data_create(d.bytes, d.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        id<MTLLibrary> as_sent = [dev newLibraryWithData:sent error:nil];
        CHECK(hv_metallib_lower(d.mutableBytes, d.length) == 1);
        CHECK(hv_metallib_format(d.bytes, d.length) == (2u << 16 | 7));
        dispatch_data_t data = dispatch_data_create(d.bytes, d.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithData:data error:&err];
        CHECK(lib != nil);
        if (!lib) { fprintf(stderr, "  %s: %s\n", paths[i], err.localizedDescription.UTF8String); continue; }
        CHECK(lib.functionNames.count == as_sent.functionNames.count);
        id<MTLFunction> fn = [lib newFunctionWithName:lib.functionNames.firstObject];
        CHECK(fn != nil);
        printf("  %s: format %u.%u read as 2.7, %lu functions\n", paths[i], format >> 16, format & 0xffff,
               (unsigned long)lib.functionNames.count);
    }
}

int main(void) {
    @autoreleasepool {
        made_up();
        real();
    }
    if (fails) { fprintf(stderr, "test_metallib: %d failed\n", fails); return 1; }
    printf("test_metallib: ok\n");
    return 0;
}

/* Metal libraries macOS sends, made loadable on an older iOS.

   macOS 15 builds its GPU code as metallib files of format 2.8 (AIR 2.7, Metal 3.2). iOS/iPadOS 17's Metal reads up to
   format 2.7 (AIR 2.6, Metal 3.1: macOS 14's) and refuses newer ones outright ("This library format is not supported on
   this platform (or was built with an old version of the tools)", GitHub #44, #12, #24): every pipeline WindowServer
   builds then fails and nothing is drawn. Formats 2.7 and 2.8 lay a file out the same way (the same header, function
   list and tags; compare a macOS 14 and a macOS 15 metallib), so a refused library is tried once more with its labels
   lowered to what the older Metal reads: the file format, the OS it says it was built for, and each function's AIR and
   Metal language versions. The code itself (AIR bitcode) is left as it is, and whether the older compiler takes all of
   it is up to that compiler: a pipeline it cannot build still fails on its own, as before.

   Only ever applied to a copy, and only after the library as sent was refused, so an iOS that reads it is unaffected.

   The header (88 bytes, little-endian), as macOS 14 to 27 write it:
     0  "MTLB"   4 u16 platform (0x8001)   6 u16 format major   8 u16 format minor
    10  u8 type   11 u8 target OS (0x81 macOS, 0x82 iOS, ...)   12 u16 OS major   14 u16 OS minor
    16  u64 file size   24 u64 function list offset, 32 size   40 public metadata offset, size   56 private, size
    72  bitcode offset, size
   The function list: u32 count, then (the list's size, from here) per function a u32 byte length (counting itself)
   and tags until "ENDT", each
   tag a 4-character name, a u16 length and its bytes. "VERS" is u16 AIR major, minor, language major, minor. */
#ifndef HV_PVG_METALLIB_H
#define HV_PVG_METALLIB_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { HV_MTLB_LEGACY_MAJOR = 2, HV_MTLB_LEGACY_MINOR = 7 };

static inline uint16_t hv_mtlb_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t hv_mtlb_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t hv_mtlb_u64(const uint8_t *p) { return hv_mtlb_u32(p) | (uint64_t)hv_mtlb_u32(p + 4) << 32; }
static inline void hv_mtlb_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }

/* The library's format version, as major << 16 | minor; 0 if the bytes are not a metallib. */
static inline uint32_t hv_metallib_format(const uint8_t *lib, size_t len) {
    if (len < 88 || memcmp(lib, "MTLB", 4)) return 0;
    return (uint32_t)hv_mtlb_u16(lib + 6) << 16 | hv_mtlb_u16(lib + 8);
}

/* Walks the function list: checks it, and with patch set lowers each function's AIR version to 2.6 and its Metal
   language version to 3.1. 0, or -1 if the list does not make sense. */
static inline int hv_metallib_functions(uint8_t *lib, size_t len, int patch) {
    uint64_t list = hv_mtlb_u64(lib + 24), list_len = hv_mtlb_u64(lib + 32);
    if (list < 88 || list > len - 4 || list_len > len - list - 4) return -1;
    const uint8_t *end = lib + list + 4 + list_len; /* the size leaves out the count before it */
    uint32_t count = hv_mtlb_u32(lib + list);
    uint8_t *fn = lib + list + 4;
    for (uint32_t i = 0; i < count; i++) {
        if (end - fn < 8) return -1;
        uint32_t size = hv_mtlb_u32(fn);
        if (size < 8 || size > (size_t)(end - fn)) return -1;
        uint8_t *tag = fn + 4, *fn_end = fn + size;
        for (;;) {
            if (fn_end - tag < 4) return -1;
            if (!memcmp(tag, "ENDT", 4)) break;
            if (fn_end - tag < 6) return -1;
            uint16_t n = hv_mtlb_u16(tag + 4);
            if (n > fn_end - tag - 6) return -1;
            if (patch && !memcmp(tag, "VERS", 4) && n >= 8) {
                uint8_t *v = tag + 6;
                uint16_t air_major = hv_mtlb_u16(v), air_minor = hv_mtlb_u16(v + 2);
                uint16_t lang_major = hv_mtlb_u16(v + 4), lang_minor = hv_mtlb_u16(v + 6);
                if (air_major > 2 || (air_major == 2 && air_minor > 6)) hv_mtlb_put16(v, 2), hv_mtlb_put16(v + 2, 6);
                if (lang_major > 3 || (lang_major == 3 && lang_minor > 1)) hv_mtlb_put16(v + 4, 3), hv_mtlb_put16(v + 6, 1);
            }
            tag += 6 + n;
        }
        fn = fn_end;
    }
    return 0;
}

/* Lowers the labels of a metallib newer than format 2.7 in place, as above. 1 if it changed anything, 0 if the library
   is already 2.7 or older (nothing to try), -1 if the bytes do not make sense (left as they were). */
static inline int hv_metallib_lower(uint8_t *lib, size_t len) {
    uint32_t format = hv_metallib_format(lib, len);
    if (!format) return -1;
    if (format <= (HV_MTLB_LEGACY_MAJOR << 16 | HV_MTLB_LEGACY_MINOR)) return 0;
    if (hv_metallib_functions(lib, len, 0)) return -1; /* the whole list checked before a byte changes */
    hv_mtlb_put16(lib + 6, HV_MTLB_LEGACY_MAJOR);
    hv_mtlb_put16(lib + 8, HV_MTLB_LEGACY_MINOR);
    /* The OS it was built for: no newer than the ones that read format 2.7 (macOS 14, iOS 17). */
    uint8_t os = lib[11];
    uint16_t os_cap = os == 0x81 ? 14 : (os == 0x82 || os == 0x86 || os == 0x87) ? 17 : 0;
    if (os_cap && hv_mtlb_u16(lib + 12) > os_cap) hv_mtlb_put16(lib + 12, os_cap), hv_mtlb_put16(lib + 14, 0);
    hv_metallib_functions(lib, len, 1);
    return 1;
}

#endif

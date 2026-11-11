#ifndef HENGEVM_METALLIB_H
#define HENGEVM_METALLIB_H

#include <stddef.h>
#include <stdint.h>

/* iPadOS and iOS 17's Metal refuse the libraries macOS 15 and later send ("This library format is not supported
   on this platform"): they say macOS, library file version 2.8 and AIR 2.7, where iOS 17 reads iOS, 2.7 and
   AIR 2.6. This relabels a library in place as one built for iOS 17. Stage 1: the header and each function's
   version tag (loads on iOS 17, but its pipelines fail "Target OS is incompatible"). Stage 2 also, in each
   function's bitcode: the module's target triple (its TRIPLE record, bit-packed in the bitstream, and the
   symbol table's copy; same length, "air64_v26-apple-ios17.000.00"), and so its hash. That is the OS Metal's
   compiler checks: for AIR newer than 2.3 it skips the check unless forced, otherwise the triple's OS must be
   its own (MTLCompiler, macOS 26). Stage 3 also: the air.version and air.language_version metadata (each
   number pointed at a metadata constant already holding the new one, else the highest below it). Only for
   trying (HV_PVG_RELABEL=3): an AIR version claimed lower than the code's makes the compiler upgrade code that
   is already new, and on macOS 26 that crashes its compiler service for a third of its own libraries; a claim
   of 2.3 or older turns the OS check on. The code itself is unchanged: an iOS 17 that compiles it accepts AIR
   it can read. Returns how many fields changed, or -1 if it isn't a library this understands. *why, if
   given, is set to the first thing that could not be relabelled (a static string), or NULL. */
int hv_metallib_relabel(uint8_t *buf, size_t len, int stage, const char **why);

/* What a library is relabelled as. */
typedef struct {
    const char *os;                   /* the triple's OS: "ios", "macosx" */
    uint16_t platform;                /* header +4: 0x0001 iOS, 0x8001 macOS */
    uint8_t os_byte;                  /* header +11: 0x82 iOS, 0x81 macOS */
    uint16_t os_major;                /* 17 */
    uint16_t file_minor;              /* library file version 2.N: 7 */
    uint16_t air_major, air_minor;    /* 2.6 */
    uint16_t lang_major, lang_minor;  /* Metal 3.1 */
} HvMetallibTarget;

extern const HvMetallibTarget hv_metallib_ios17;

/* As hv_metallib_relabel, for any target (tests relabel a newer macOS's libraries for the one they run on). */
int hv_metallib_relabel_for(uint8_t *buf, size_t len, int stage, const HvMetallibTarget *t, const char **why);

#endif

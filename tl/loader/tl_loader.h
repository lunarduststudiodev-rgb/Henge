#ifndef HENGE_TL_LOADER_H
#define HENGE_TL_LOADER_H

#include <stddef.h>
#include <stdint.h>

/* Henge's Mach-O loader for the Mac-app translation layer: loads an arm64
   macOS executable and the frameworks inside its bundle without dyld, binds
   their imports to the host's own libraries (iOS's, or a shim), registers
   them with the Objective-C and Swift runtimes, and runs them in-process.

   Why not dyld: on a TXM device (every device iOS 27 supports) an app cannot
   make new pages executable; Henge gets one executable region per launch
   from a debugger. Code goes into that region, written through its writable
   alias; every other segment is ordinary memory placed at the same distance
   from the code as in the file, so PC-relative references still land. */

typedef struct TlImage TlImage;

typedef struct TlHost {
    /* The executable region: code runs at rx, and is written at rx + rw_diff. */
    uint8_t *rx;
    intptr_t rw_diff;
    size_t size, used;
    /* A macOS install name to the path dlopen should use on this host, or NULL
       when the host has no such library (then only shim_lookup can answer). */
    const char *(*map_install_name)(const char *install_name, char *buf, size_t len);
    /* A symbol the host provides itself for a library (AppKit on iOS, say), or
       NULL. Asked before the real library, so a shim can replace it. */
    void *(*shim_lookup)(const char *install_name, const char *symbol);
    /* Messages ("tl: ..."); NULL writes them to stderr. */
    void (*log)(const char *line);
    /* A missing non-weak symbol fails the load (1) or binds to a stub that
       reports it if it is ever called (0). */
    int strict;
    int verbose;
} TlHost;

/* A region shaped like iOS's (an RX mapping plus an RW alias of the same
   pages) made on this Mac, for tests. 0, or -1. */
int tl_host_init_split(TlHost *host, size_t bytes);
/* Use an executable region the host already has (iOS: Henge's JIT region). */
void tl_host_init_region(TlHost *host, void *rx, void *rw, size_t bytes);

/* Loads the executable at path (thin or universal; the arm64 slice), its
   bundle's frameworks, fixes them up, registers them, and runs their
   initializers. NULL with a reason in err. */
TlImage *tl_load(TlHost *host, const char *path, char *err, size_t errlen);
/* Make NSBundle.mainBundle the guest app's bundle (the folder holding
   Contents/MacOS/<executable>). The process then belongs to that app: Henge
   hands it the process, as LiveContainer does. 0, or -1. */
int tl_adopt_main_bundle(const char *app_path);
/* Calls the executable's main. */
int tl_run_main(TlImage *exe, int argc, const char **argv, const char **envp);

/* What was loaded, for the host and tests. */
void *tl_image_base(const TlImage *image);
size_t tl_image_size(const TlImage *image);
const char *tl_image_path(const TlImage *image);
/* The address of an exported symbol ("_main"), or NULL. */
void *tl_image_symbol(TlImage *image, const char *symbol);
/* Symbols that were missing and bound to the reporting stub. */
int tl_missing_count(void);
const char *tl_missing_name(int index);

/* The usual TlHost.map_install_name on iOS: a macOS framework path to iOS's
   (no Versions/X; Mac Catalyst's /System/iOSSupport copies to the originals),
   or NULL for libraries iOS does not have (AppKit, Carbon and the like), which
   the host then answers with shims. */
const char *tl_ios_map_install_name(const char *install_name, char *buf, size_t len);

#endif

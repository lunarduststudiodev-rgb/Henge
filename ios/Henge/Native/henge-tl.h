/* Mac apps without macOS (experimental): the translation layer (../../../tl) as the Swift side sees it. */
#ifndef HENGE_TL_H
#define HENGE_TL_H

#include <stddef.h>
#include <stdint.h>

/* Copies what a .dmg holds at its top level into dir: every .app folder, and every lone Mach-O program. Returns how
   many it copied (0 if the image holds neither), or -1 with a reason in err. */
int henge_tl_import_dmg(const char *dmg, const char *dir, char *err, size_t errlen);

/* What an app or program is. kind: what its code needs, worst first (an app linking AppKit is HENGE_TL_APPKIT even
   if it also links Foundation). 0, or -1 with a reason in err. */
enum HengeTlKind {
    HENGE_TL_PROGRAM,   /* a command-line program: libSystem, Foundation, Swift and the like, all on iOS */
    HENGE_TL_SWIFTUI,   /* a SwiftUI app without AppKit's own calls */
    HENGE_TL_CATALYST,  /* a Mac Catalyst app (UIKit) */
    HENGE_TL_APPKIT,    /* an AppKit app: needs an AppKit, which iOS lacks */
    HENGE_TL_INTEL,     /* no arm64 code at all */
};
typedef struct HengeTlInfo {
    char name[256], identifier[256], min_macos[32];
    char executable[1024]; /* the Mach-O to run */
    int arm64, x86_64, kind;
    int libraries;         /* how many libraries it links */
} HengeTlInfo;
int henge_tl_info(const char *path, HengeTlInfo *info, char *err, size_t errlen);

/* Runs a program's main on a thread of its own, in this process, through Henge's own loader, its standard output
   and error going to the file out_path. Needs JIT on, and no Mac started in this launch (the loader takes the far
   end of the JIT region and remaps part of it). Returns main's result (or the code it passed to exit), or -1 with a
   reason in err if it could not be loaded. Blocks until it returns. */
int henge_tl_run(const char *path, const char *out_path, char *err, size_t errlen);
/* A Mac app ran in this launch: no Mac can start, and no other program run, until Henge is opened again. */
int henge_tl_used(void);
int henge_tl_ran_program(void);

#endif

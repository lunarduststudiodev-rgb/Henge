/* Runs a macOS arm64 executable through Henge's translation-layer loader in
   this process, in a region shaped like iOS's, and never through dyld:
   tlrun [-v] [-strict] [-shim LIB]... EXECUTABLE [ARGS...] */
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../loader/tl_loader.h"

#ifdef TL_APPKIT_MINI
/* tlrun-mini: the toy AppKit's classes are named as AppKit's, so this build must never meet the real AppKit. */
#include "../shim/appkit_mini.h"

static void report_appkit(void) {
    fprintf(stderr, "tlrun: the real AppKit %s loaded in this process\n", tl_appkit_is_real_loaded() ? "WAS" : "was never");
}
#endif

static const char *g_shimmed[16];
static int g_nshimmed;

/* A library named with -shim has no host library: only the shim answers (none here), as AppKit will on iOS. */
static const char *map_name(const char *name, char *buf, size_t len) {
    (void)buf, (void)len;
    for (int i = 0; i < g_nshimmed; i++)
        if (strstr(name, g_shimmed[i])) return NULL;
    return name;
}

int main(int argc, const char **argv, const char **envp) {
    TlHost host;
    if (tl_host_init_split(&host, 512u << 20)) {
        fprintf(stderr, "tlrun: no executable region\n");
        return 120;
    }
    host.map_install_name = map_name;
#ifdef TL_APPKIT_MINI
    /* AppKit (and Cocoa, which would load it) answered by the toy AppKit, never the real one. */
    g_shimmed[g_nshimmed++] = "/AppKit.framework/";
    g_shimmed[g_nshimmed++] = "/Cocoa.framework/";
    host.shim_lookup = tl_appkit_mini_lookup;
    atexit(report_appkit);
#endif
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v")) host.verbose = 1;
        else if (!strcmp(argv[i], "-strict")) host.strict = 1;
        else if (!strcmp(argv[i], "-shim") && i + 1 < argc) g_shimmed[g_nshimmed++] = argv[++i];
        else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: tlrun [-v] [-strict] [-shim LIB]... EXECUTABLE [ARGS...]\n");
        return 121;
    }
    char err[1024] = "";
    TlImage *exe = tl_load(&host, argv[i], err, sizeof err);
    if (!exe) {
        fprintf(stderr, "tlrun: %s\n", err);
        return 122;
    }
    /* Proof that dyld never saw it. */
    const char *path = tl_image_path(exe);
    for (uint32_t k = 0; k < _dyld_image_count(); k++)
        if (!strcmp(_dyld_get_image_name(k), path)) {
            fprintf(stderr, "tlrun: dyld has %s loaded\n", path);
            return 123;
        }
    if (getenv("TLRUN_REPORT"))
        fprintf(stderr, "tlrun: %s at %p, %zu bytes, %d missing symbols; not in dyld's %u images\n", path,
                tl_image_base(exe), tl_image_size(exe), tl_missing_count(), _dyld_image_count());
    for (int k = 0; k < tl_missing_count(); k++) fprintf(stderr, "tlrun: missing %s\n", tl_missing_name(k));
    /* An executable inside an app bundle runs as that app. */
    const char *macos = strstr(path, ".app/Contents/MacOS/");
    if (macos) {
        char app[4096];
        snprintf(app, sizeof app, "%.*s", (int)(macos - path + 4), path);
        if (tl_adopt_main_bundle(app)) fprintf(stderr, "tlrun: %s is not a bundle\n", app);
    }
    return tl_run_main(exe, argc - i, argv + i, envp);
}

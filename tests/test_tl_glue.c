/* The app's glue to the translation layer, on the Mac: a disk image made by `make -C tl test-dmg` is imported, what it
   holds described, and a program in it run through the loader, its output caught in a file. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ftw.h>
#include <unistd.h>

#include "henge-ios-jit.h"
#include "henge-tl.h"
#include "jit_region.h"

static HvJitRegion jit;
static HengeDualMapping mapping;
_Atomic int henge_tl_ran;
int henge_vm_state(void) { return 0; }
HengeDualMapping *henge_ios_jit_get_mapping(void) { return &mapping; }
static int failures;
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *f) { (void)st, (void)flag, (void)f; return remove(p); }
static void rm_tree(const char *dir) { nftw(dir, rm_one, 16, FTW_DEPTH | FTW_PHYS); }
static void check(const char *what, int ok) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

/* test_tl_glue DMG: import it and run every program in it, as the app's Mac apps screen would. */
static int run_all(const char *dmg, const char *only) {
    char dir[] = "build/tl-glue-XXXXXX", err[512] = "", out[300];
    if (!mkdtemp(dir)) return 1;
    int n = henge_tl_import_dmg(dmg, dir, err, sizeof err);
    printf("%s: %d copied%s%s\n", dmg, n, n < 0 ? ": " : "", n < 0 ? err : "");
    DIR *d = opendir(dir);
    struct dirent *e;
    int bad = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[600];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        HengeTlInfo info;
        if (henge_tl_info(path, &info, err, sizeof err)) { printf("  %s: %s\n", e->d_name, err); bad++; continue; }
        if (info.kind != HENGE_TL_PROGRAM) { printf("  %s: kind %d, not run\n", info.name, info.kind); continue; }
        if (!only) { printf("  %s: a program\n", info.name); continue; }
        if (strcmp(info.name, only)) continue;
        snprintf(out, sizeof out, "%s/.out", dir);
        int rc = henge_tl_run(info.executable, out, err, sizeof err);
        char text[256] = "";
        FILE *f = fopen(out, "r");
        if (f) { text[fread(text, 1, sizeof text - 1, f)] = 0; fclose(f); }
        char *nl = strchr(text, '\n');
        if (nl) *nl = 0;
        printf("  %s: returned %d: %s\n", info.name, rc, text);
        if (rc) bad++;
    }
    if (d) closedir(d);
    rm_tree(dir);
    return bad != 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "exit3")) { /* exit() ends the program's thread with its code, not this process */
        setenv("HV_JIT_SPLIT", "1", 1);
        if (hv_jit_region_open(&jit, 512u << 20)) return 1;
        mapping = (HengeDualMapping){.rx_addr = (uint8_t *)jit.base, .rw_addr = (uint8_t *)jit.base + jit.rw_diff, .size = jit.capacity * 4};
        char err[256] = "", text[256] = "";
        int rc = henge_tl_run("build/tl-exit3", "build/tl-exit3.txt", err, sizeof err);
        FILE *f = fopen("build/tl-exit3.txt", "r");
        if (f) text[fread(text, 1, sizeof text - 1, f)] = 0, fclose(f);
        unlink("build/tl-exit3.txt");
        check("exit(3) comes back as 3", rc == 3);
        check("and what it printed before is kept", strstr(text, "leaving with exit(3)") != NULL);
        printf("tl glue exit: %d failures\n", failures);
        return failures != 0;
    }
    if (argc > 1) {
        setenv("HV_JIT_SPLIT", "1", 1);
        if (hv_jit_region_open(&jit, 512u << 20)) return 1;
        mapping = (HengeDualMapping){.rx_addr = (uint8_t *)jit.base, .rw_addr = (uint8_t *)jit.base + jit.rw_diff, .size = jit.capacity * 4};
        return run_all(argv[1], argc > 2 ? argv[2] : NULL);
    }
    char dir[] = "build/tl-glue-XXXXXX", err[512] = "", out[300], app[300], prog[300];
    setenv("HV_JIT_SPLIT", "1", 1); /* the iPhone's shape: an executable region and a writable alias of it */
    if (!mkdtemp(dir) || hv_jit_region_open(&jit, 512u << 20)) return 1;
    mapping = (HengeDualMapping){.rx_addr = (uint8_t *)jit.base, .rw_addr = (uint8_t *)jit.base + jit.rw_diff, .size = jit.capacity * 4};
    int n = henge_tl_import_dmg("tl/build/t-APFS-ULFO.dmg", dir, err, sizeof err);
    if (n < 0) fprintf(stderr, "import: %s\n", err);
    check("the image's app is copied out", n >= 1);
    snprintf(app, sizeof app, "%s/FW.app", dir);
    HengeTlInfo info;
    check("an app is described", henge_tl_info(app, &info, err, sizeof err) == 0 && !strcmp(info.identifier, "dev.henge.tl.fwapp") &&
          info.arm64 && info.kind == HENGE_TL_PROGRAM && strstr(info.executable, "/Contents/MacOS/fwapp"));
    check("Calculator's copy needs AppKit", henge_tl_info("/System/Applications/Calculator.app", &info, err, sizeof err) == 0 &&
          info.kind == HENGE_TL_APPKIT);
    snprintf(prog, sizeof prog, "%s/FW.app/Contents/MacOS/fwapp", dir);
    snprintf(out, sizeof out, "%s/out.txt", dir);
    int rc = henge_tl_run(prog, out, err, sizeof err);
    if (rc) fprintf(stderr, "run: %d %s\n", rc, err);
    char text[4096] = "";
    FILE *f = fopen(out, "r");
    if (f) text[fread(text, 1, sizeof text - 1, f)] = 0, fclose(f);
    check("the program runs and returns 0", rc == 0);
    check("its output is caught", strstr(text, "PASS") != NULL);
    check("no Mac can start after it", atomic_load(&henge_tl_ran) == 1);
    check("a second program in the same launch is refused", henge_tl_run("build/tl-exit3", out, err, sizeof err) == -1 &&
          strstr(err, "One program per launch") != NULL);
    printf("tl glue: %d copied, output %zu bytes, %d failures\n", n, strlen(text), failures);
    rm_tree(dir);
    return failures != 0;
}

/* Mac apps without macOS (experimental): importing, describing and running them through the translation layer. */
#include "henge-tl.h"

#include <errno.h>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "henge-ios-jit.h"
#include "henge-vm.h"
#include "../../../tl/dmg/tl_dmg.h"
#include "../../../tl/loader/tl_loader.h"

HengeDualMapping *henge_ios_jit_get_mapping(void);
extern _Atomic int henge_tl_ran; /* henge-vm.c: no Mac starts once this is set */

#define TL_REGION (64u << 20) /* the far end of the JIT region, for a program's code */

int henge_tl_used(void) { return atomic_load(&henge_tl_ran); }

static int is_macho(uint32_t magic) {
    return magic == MH_MAGIC_64 || magic == FAT_MAGIC || magic == FAT_CIGAM || magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64;
}

int henge_tl_import_dmg(const char *dmg, const char *dir, char *err, size_t errlen) {
    TlDisk *disk = tl_disk_open(dmg, err, errlen);
    if (!disk) return -1;
    TlVolume *vol = tl_volume_open(disk, err, errlen);
    if (!vol) {
        tl_disk_close(disk);
        return -1;
    }
    TlNode root;
    if (tl_volume_lookup(vol, "/", &root) != 0) {
        snprintf(err, errlen, "The disk image's volume has no top folder.");
        tl_volume_close(vol), tl_disk_close(disk);
        return -1;
    }
    /* What to copy: names first, as copying while listing would walk the catalog twice at once. */
    __block char **names = NULL;
    __block int count = 0;
    tl_volume_list(vol, &root, ^int(const char *name, const TlNode *node) {
        if (name[0] == '.') return 0;
        size_t len = strlen(name);
        int app = S_ISDIR(node->mode) && len > 4 && !strcasecmp(name + len - 4, ".app");
        int program = 0;
        if (S_ISREG(node->mode) && node->size >= 4) {
            uint32_t magic = 0;
            if (tl_volume_read(vol, node, 0, &magic, 4) == 4) program = is_macho(magic);
        }
        if (!app && !program) return 0;
        char **more = realloc(names, sizeof *names * (size_t)(count + 1));
        if (!more) return 1;
        names = more;
        names[count++] = strdup(name);
        return 0;
    });
    int copied = 0;
    for (int i = 0; i < count; i++) {
        char from[1100], to[2048];
        snprintf(from, sizeof from, "/%s", names[i]);
        snprintf(to, sizeof to, "%s/%s", dir, names[i]);
        char why[512] = "";
        if (tl_volume_extract(vol, from, to, why, sizeof why) == 0) copied++;
        else snprintf(err, errlen, "Could not copy %s: %s", names[i], why);
        free(names[i]);
    }
    free(names);
    tl_volume_close(vol);
    tl_disk_close(disk);
    if (!copied && count) return -1;
    if (!count) snprintf(err, errlen, "The disk image holds no app or program at its top level.");
    return copied;
}

/* The arm64 slice's load commands: which libraries it links and what platform it was built for. */
static void scan_slice(int fd, uint64_t off, HengeTlInfo *info, int *appkit, int *swiftui, int *catalyst) {
    struct mach_header_64 mh;
    if (pread(fd, &mh, sizeof mh, (off_t)off) != sizeof mh || mh.magic != MH_MAGIC_64 || mh.sizeofcmds > (1u << 24)) return;
    uint8_t *cmds = malloc(mh.sizeofcmds);
    if (!cmds || pread(fd, cmds, mh.sizeofcmds, (off_t)(off + sizeof mh)) != (ssize_t)mh.sizeofcmds) {
        free(cmds);
        return;
    }
    for (uint32_t i = 0, at = 0; i < mh.ncmds && at + sizeof(struct load_command) <= mh.sizeofcmds; i++) {
        const struct load_command *lc = (const void *)(cmds + at);
        if (!lc->cmdsize || at + lc->cmdsize > mh.sizeofcmds) break;
        if (lc->cmd == LC_LOAD_DYLIB || lc->cmd == LC_LOAD_WEAK_DYLIB || lc->cmd == LC_REEXPORT_DYLIB ||
            lc->cmd == LC_LAZY_LOAD_DYLIB || lc->cmd == LC_LOAD_UPWARD_DYLIB) {
            const struct dylib_command *d = (const void *)lc;
            if (d->dylib.name.offset < lc->cmdsize) {
                const char *name = (const char *)lc + d->dylib.name.offset;
                size_t room = lc->cmdsize - d->dylib.name.offset;
                if (memchr(name, 0, room)) {
                    info->libraries++;
                    if (strstr(name, "/AppKit.framework/") || strstr(name, "/Cocoa.framework/")) *appkit = 1;
                    if (strstr(name, "/SwiftUI.framework/")) *swiftui = 1;
                    if (strstr(name, "/System/iOSSupport/") || strstr(name, "/UIKit.framework/")) *catalyst = 1;
                }
            }
        } else if (lc->cmd == LC_BUILD_VERSION && ((const struct build_version_command *)lc)->platform == PLATFORM_MACCATALYST) {
            *catalyst = 1;
        }
        at += lc->cmdsize;
    }
    free(cmds);
}

int henge_tl_info(const char *path, HengeTlInfo *info, char *err, size_t errlen) {
    memset(info, 0, sizeof *info);
    TlAppInfo ai;
    if (tl_app_info(path, &ai, err, errlen) != 0) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return snprintf(err, errlen, "%s", strerror(errno)), -1;
    if (S_ISDIR(st.st_mode)) snprintf(info->executable, sizeof info->executable, "%s/Contents/MacOS/%s", path, ai.executable);
    else snprintf(info->executable, sizeof info->executable, "%s", path);
    const char *base = strrchr(path, '/');
    snprintf(info->name, sizeof info->name, "%s", ai.name[0] ? ai.name : base ? base + 1 : path);
    snprintf(info->identifier, sizeof info->identifier, "%s", ai.identifier);
    snprintf(info->min_macos, sizeof info->min_macos, "%s", ai.min_macos[0] ? ai.min_macos : ai.build_min_macos);
    info->arm64 = ai.has_arm64 || ai.has_arm64e;
    info->x86_64 = ai.has_x86_64;
    int appkit = 0, swiftui = 0, catalyst = 0;
    int fd = open(info->executable, O_RDONLY);
    if (fd >= 0) {
        uint32_t magic = 0;
        if (pread(fd, &magic, 4, 0) == 4) {
            if (magic == MH_MAGIC_64) scan_slice(fd, 0, info, &appkit, &swiftui, &catalyst);
            else if (magic == FAT_CIGAM || magic == FAT_CIGAM_64) { /* universal: big-endian table */
                struct fat_header fh;
                if (pread(fd, &fh, sizeof fh, 0) == sizeof fh) {
                    uint32_t n = OSSwapBigToHostInt32(fh.nfat_arch);
                    int wide = magic == FAT_CIGAM_64;
                    for (uint32_t i = 0; i < n && i < 16; i++) {
                        uint64_t off = 0;
                        int32_t cpu = 0;
                        if (wide) {
                            struct fat_arch_64 a;
                            if (pread(fd, &a, sizeof a, (off_t)(sizeof fh + i * sizeof a)) != sizeof a) break;
                            cpu = (int32_t)OSSwapBigToHostInt32((uint32_t)a.cputype), off = OSSwapBigToHostInt64(a.offset);
                        } else {
                            struct fat_arch a;
                            if (pread(fd, &a, sizeof a, (off_t)(sizeof fh + i * sizeof a)) != sizeof a) break;
                            cpu = (int32_t)OSSwapBigToHostInt32((uint32_t)a.cputype), off = OSSwapBigToHostInt32(a.offset);
                        }
                        if (cpu == CPU_TYPE_ARM64) {
                            scan_slice(fd, off, info, &appkit, &swiftui, &catalyst);
                            break;
                        }
                    }
                }
            }
        }
        close(fd);
    }
    info->kind = !info->arm64 ? HENGE_TL_INTEL : catalyst ? HENGE_TL_CATALYST : appkit ? HENGE_TL_APPKIT
               : swiftui ? HENGE_TL_SWIFTUI : HENGE_TL_PROGRAM;
    return 0;
}

/* ---- running ---- */

static _Atomic int exit_code;

/* The program's exit() ends its own thread, not Henge. */
static void guest_exit(int code) {
    fflush(NULL);
    atomic_store(&exit_code, code);
    pthread_exit(NULL);
}

static void *shim_lookup(const char *install_name, const char *symbol) {
    if (!strstr(install_name, "libSystem")) return NULL;
    if (!strcmp(symbol, "_exit") || !strcmp(symbol, "__exit") || !strcmp(symbol, "__Exit")) return (void *)guest_exit;
    return NULL;
}

static void log_line(const char *line) { fprintf(stderr, "%s\n", line); }

typedef struct RunArgs {
    TlImage *exe;
    const char *path;
} RunArgs;

static void *run_thread(void *opaque) {
    RunArgs *a = opaque;
    const char *argv[] = {a->path, NULL};
    static char home[1100];
    const char *tmp = getenv("TMPDIR");
    snprintf(home, sizeof home, "HOME=%s", tmp && *tmp ? tmp : "/tmp"); /* the app's own temporary folder on iOS */
    const char *envp[] = {home, NULL};
    atomic_store(&exit_code, tl_run_main(a->exe, 1, argv, envp));
    fflush(NULL);
    return NULL;
}

/* One program per launch: the loader has no unloading, and the Objective-C and Swift runtimes keep pointing at what
   a program registered, so a second one cannot reuse the region (nor, with the same classes, share the runtimes). */
static _Atomic int runs;
int henge_tl_ran_program(void) { return atomic_load(&runs); }

int henge_tl_run(const char *path, const char *out_path, char *err, size_t errlen) {
    if (atomic_load(&runs)) return snprintf(err, errlen, "One program per launch: quit Henge (swipe it away) and open it again to run another."), -1;
    int vm = henge_vm_state();
    if (vm == 1 || vm == 3 || vm == 4) return snprintf(err, errlen, "Stop the Mac first: a Mac app and a Mac cannot run at once."), -1;
    HengeDualMapping *jit = henge_ios_jit_get_mapping();
    if (!jit || !jit->rw_addr || !jit->rx_addr) return snprintf(err, errlen, "Turn on JIT first."), -1;
    if (jit->size < 4 * (size_t)TL_REGION) return snprintf(err, errlen, "The JIT region is too small."), -1;
    TlHost host;
    memset(&host, 0, sizeof host);
    size_t at = jit->size - TL_REGION;
    tl_host_init_region(&host, jit->rx_addr + at, jit->rw_addr + at, TL_REGION);
    host.map_install_name = tl_ios_map_install_name;
    host.shim_lookup = shim_lookup;
    host.log = log_line;
    /* From here part of the JIT region may be remapped for the program's data: no Mac in this launch. */
    atomic_store(&henge_tl_ran, 1);
    atomic_store(&runs, 1);
    fprintf(stderr, "henge: tl: loading %s\n", path);
    TlImage *exe = tl_load(&host, path, err, errlen);
    if (!exe) {
        fprintf(stderr, "henge: tl: %s\n", err);
        return -1;
    }
    fprintf(stderr, "henge: tl: loaded at %p, %zu bytes, %d missing symbols\n", tl_image_base(exe), tl_image_size(exe), tl_missing_count());
    /* Not tl_adopt_main_bundle: it hands the whole process's main bundle to the program, and Henge reads its own
       (resources, its build) for the rest of the launch. A program without windows gets by without; an app with
       windows will need it, with Henge's own reads kept apart first. */
    int out = open(out_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    fflush(NULL);
    int saved1 = dup(1), saved2 = dup(2);
    if (out >= 0) dup2(out, 1), dup2(out, 2);
    atomic_store(&exit_code, -1);
    RunArgs a = {exe, path};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t t;
    int started = pthread_create(&t, &attr, run_thread, &a) == 0;
    pthread_attr_destroy(&attr);
    if (started) pthread_join(t, NULL);
    fflush(NULL);
    if (saved1 >= 0) dup2(saved1, 1), close(saved1);
    if (saved2 >= 0) dup2(saved2, 2), close(saved2);
    if (out >= 0) close(out);
    if (!started) return snprintf(err, errlen, "No thread for the program."), -1;
    int code = atomic_load(&exit_code);
    fprintf(stderr, "henge: tl: %s returned %d\n", path, code);
    return code;
}

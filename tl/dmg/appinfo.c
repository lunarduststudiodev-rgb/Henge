/* What a Mac app is, read from its bundle: Info.plist, and which CPUs its executable has code for. */
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <limits.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tl_dmg_private.h"

static void cfstr(CFDictionaryRef d, CFStringRef key, char *out, size_t len) {
    CFTypeRef v = CFDictionaryGetValue(d, key);
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString(v, out, (CFIndex)len, kCFStringEncodingUTF8);
}

static void version_string(uint32_t v, char *out, size_t len) {
    snprintf(out, len, "%u.%u", v >> 16, (v >> 8) & 0xff);
    if (v & 0xff) snprintf(out + strlen(out), len - strlen(out), ".%u", v & 0xff);
}

/* LC_BUILD_VERSION / LC_VERSION_MIN_MACOSX of one thin slice. */
static void slice_min(int fd, uint64_t off, char *out, size_t len) {
    struct mach_header_64 mh;
    if (pread(fd, &mh, sizeof mh, (off_t)off) != sizeof mh || mh.magic != MH_MAGIC_64 || mh.sizeofcmds > (1u << 24)) return;
    uint8_t *cmds = malloc(mh.sizeofcmds);
    if (!cmds || pread(fd, cmds, mh.sizeofcmds, (off_t)(off + sizeof mh)) != (ssize_t)mh.sizeofcmds) { free(cmds); return; }
    for (uint32_t i = 0, at = 0; i < mh.ncmds && at + 8 <= mh.sizeofcmds; i++) {
        const struct load_command *lc = (const void *)(cmds + at);
        if (lc->cmd == LC_BUILD_VERSION) {
            const struct build_version_command *b = (const void *)lc;
            if (b->platform == PLATFORM_MACOS) version_string(b->minos, out, len);
        } else if (lc->cmd == LC_VERSION_MIN_MACOSX) {
            version_string(((const struct version_min_command *)lc)->version, out, len);
        }
        if (!lc->cmdsize) break;
        at += lc->cmdsize;
    }
    free(cmds);
}

static int arch_scan(const char *exe, TlAppInfo *info) {
    int fd = open(exe, O_RDONLY);
    if (fd < 0) return -1;
    uint32_t magic;
    if (pread(fd, &magic, 4, 0) != 4) { close(fd); return -1; }
    if (magic == FAT_CIGAM || magic == FAT_CIGAM_64) {
        struct fat_header fh;
        pread(fd, &fh, sizeof fh, 0);
        uint32_t n = OSSwapBigToHostInt32(fh.nfat_arch);
        int wide = magic == FAT_CIGAM_64;
        for (uint32_t i = 0; i < n && i < 32; i++) {
            uint32_t cpu, sub;
            uint64_t off;
            if (wide) {
                struct fat_arch_64 a;
                pread(fd, &a, sizeof a, (off_t)(sizeof fh + i * sizeof a));
                cpu = OSSwapBigToHostInt32(a.cputype), sub = OSSwapBigToHostInt32(a.cpusubtype), off = OSSwapBigToHostInt64(a.offset);
            } else {
                struct fat_arch a;
                pread(fd, &a, sizeof a, (off_t)(sizeof fh + i * sizeof a));
                cpu = OSSwapBigToHostInt32(a.cputype), sub = OSSwapBigToHostInt32(a.cpusubtype), off = OSSwapBigToHostInt32(a.offset);
            }
            if (cpu == CPU_TYPE_X86_64) info->has_x86_64 = 1;
            if (cpu == CPU_TYPE_ARM64) {
                if ((sub & ~CPU_SUBTYPE_MASK) > CPU_SUBTYPE_ARM64_V8) info->has_arm64e = 1; /* 2, or 12 on macOS 27 */
                else info->has_arm64 = 1, slice_min(fd, off, info->build_min_macos, sizeof info->build_min_macos);
            }
        }
    } else if (magic == MH_MAGIC_64) {
        struct mach_header_64 mh;
        pread(fd, &mh, sizeof mh, 0);
        if (mh.cputype == CPU_TYPE_X86_64) info->has_x86_64 = 1;
        if (mh.cputype == CPU_TYPE_ARM64) {
            if ((mh.cpusubtype & ~CPU_SUBTYPE_MASK) > CPU_SUBTYPE_ARM64_V8) info->has_arm64e = 1;
            else info->has_arm64 = 1, slice_min(fd, 0, info->build_min_macos, sizeof info->build_min_macos);
        }
    } else {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int tl_app_info(const char *path, TlAppInfo *info, char *err, size_t errlen) {
    memset(info, 0, sizeof *info);
    struct stat st;
    if (stat(path, &st)) { tl_set_err(err, errlen, "%s does not exist", path); return -1; }
    if (S_ISREG(st.st_mode)) {
        const char *slash = strrchr(path, '/');
        snprintf(info->executable, sizeof info->executable, "%s", slash ? slash + 1 : path);
        snprintf(info->name, sizeof info->name, "%s", info->executable);
        if (arch_scan(path, info)) { tl_set_err(err, errlen, "%s is not a Mach-O executable", path); return -1; }
        return 0;
    }
    char plist_path[PATH_MAX];
    snprintf(plist_path, sizeof plist_path, "%s/Contents/Info.plist", path);
    CFDataRef data = NULL;
    int fd = open(plist_path, O_RDONLY);
    if (fd >= 0 && !fstat(fd, &st) && st.st_size < (16 << 20)) {
        uint8_t *buf = malloc((size_t)st.st_size);
        if (buf && read(fd, buf, (size_t)st.st_size) == st.st_size) data = CFDataCreate(NULL, buf, st.st_size);
        free(buf);
    }
    if (fd >= 0) close(fd);
    if (!data) { tl_set_err(err, errlen, "%s has no Contents/Info.plist: not a Mac app", path); return -1; }
    CFPropertyListRef pl = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) {
        if (pl) CFRelease(pl);
        tl_set_err(err, errlen, "%s: Info.plist cannot be read", path);
        return -1;
    }
    cfstr(pl, CFSTR("CFBundleExecutable"), info->executable, sizeof info->executable);
    cfstr(pl, CFSTR("CFBundleIdentifier"), info->identifier, sizeof info->identifier);
    cfstr(pl, CFSTR("LSMinimumSystemVersion"), info->min_macos, sizeof info->min_macos);
    cfstr(pl, CFSTR("CFBundleDisplayName"), info->name, sizeof info->name);
    if (!info->name[0]) cfstr(pl, CFSTR("CFBundleName"), info->name, sizeof info->name);
    CFRelease(pl);
    if (!info->executable[0]) { tl_set_err(err, errlen, "%s: Info.plist names no executable", path); return -1; }
    char exe[PATH_MAX];
    snprintf(exe, sizeof exe, "%s/Contents/MacOS/%s", path, info->executable);
    if (arch_scan(exe, info)) { tl_set_err(err, errlen, "%s: the executable is missing or not Mach-O", path); return -1; }
    return 0;
}

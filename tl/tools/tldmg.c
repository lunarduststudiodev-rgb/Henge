/* tldmg info IMAGE | ls IMAGE [PATH] | extract IMAGE PATH DEST | apps IMAGE | appinfo APP */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../dmg/tl_dmg.h"

static int open_vol(const char *image, TlDisk **disk, TlVolume **vol) {
    char err[512] = "";
    *disk = tl_disk_open(image, err, sizeof err);
    if (!*disk) { fprintf(stderr, "tldmg: %s\n", err); return -1; }
    *vol = tl_volume_open(*disk, err, sizeof err);
    if (!*vol) { fprintf(stderr, "tldmg: %s\n", err); return -1; }
    return 0;
}

static void print_app(const char *where, const TlAppInfo *a) {
    printf("%s: %s (%s), executable %s, code:%s%s%s, minimum macOS %s%s%s\n", where, a->name, a->identifier[0] ? a->identifier : "no id",
           a->executable, a->has_arm64 ? " arm64" : "", a->has_arm64e ? " arm64e" : "", a->has_x86_64 ? " x86_64" : "",
           a->min_macos[0] ? a->min_macos : "?", a->build_min_macos[0] ? ", built for " : "", a->build_min_macos);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: tldmg info IMAGE | ls IMAGE [PATH] | extract IMAGE PATH DEST | apps IMAGE DEST | appinfo APP\n");
        return 2;
    }
    const char *cmd = argv[1];
    char err[512] = "";
    if (!strcmp(cmd, "appinfo")) {
        TlAppInfo a;
        if (tl_app_info(argv[2], &a, err, sizeof err)) { fprintf(stderr, "tldmg: %s\n", err); return 1; }
        print_app(argv[2], &a);
        return 0;
    }
    TlDisk *disk;
    TlVolume *vol;
    if (open_vol(argv[2], &disk, &vol)) return 1;
    if (!strcmp(cmd, "info")) {
        printf("disk %llu bytes, %d partitions\n", (unsigned long long)tl_disk_size(disk), tl_disk_partition_count(disk));
        for (int i = 0; i < tl_disk_partition_count(disk); i++)
            printf("  %d: %-28s at %llu, %llu bytes\n", i, tl_disk_partition_name(disk, i), (unsigned long long)tl_disk_partition_offset(disk, i),
                   (unsigned long long)tl_disk_partition_size(disk, i));
        printf("volume %s \"%s\"\n", tl_volume_kind(vol), tl_volume_name(vol));
    } else if (!strcmp(cmd, "ls")) {
        TlNode dir;
        if (tl_volume_lookup(vol, argc > 3 ? argv[3] : "/", &dir)) { fprintf(stderr, "tldmg: no such path\n"); return 1; }
        tl_volume_list(vol, &dir, ^int(const char *name, const TlNode *n) {
            printf("%06o %10llu %s%s\n", n->mode, (unsigned long long)n->size, name, n->flags & 0x20 ? " (compressed)" : "");
            return 0;
        });
    } else if (!strcmp(cmd, "extract") && argc > 4) {
        if (tl_volume_extract(vol, argv[3], argv[4], err, sizeof err)) { fprintf(stderr, "tldmg: %s\n", err); return 1; }
    } else if (!strcmp(cmd, "apps") && argc > 3) {
        /* Every .app at the volume's top level: copied out, then described. */
        TlNode root;
        tl_volume_lookup(vol, "/", &root);
        __block int rc = 0;
        const char *dest = argv[3];
        mkdir(dest, 0755);
        tl_volume_list(vol, &root, ^int(const char *name, const TlNode *n) {
            size_t l = strlen(name);
            if (!S_ISDIR(n->mode) || l < 5 || strcmp(name + l - 4, ".app")) return 0;
            char src[1024], out[1024], e[512] = "";
            snprintf(src, sizeof src, "/%s", name);
            snprintf(out, sizeof out, "%s/%s", dest, name);
            TlAppInfo a;
            if (tl_volume_extract(vol, src, out, e, sizeof e) || tl_app_info(out, &a, e, sizeof e)) { fprintf(stderr, "tldmg: %s\n", e); rc = 1; return 0; }
            print_app(name, &a);
            return 0;
        });
        return rc;
    } else {
        fprintf(stderr, "tldmg: unknown command\n");
        return 2;
    }
    tl_volume_close(vol);
    tl_disk_close(disk);
    return 0;
}

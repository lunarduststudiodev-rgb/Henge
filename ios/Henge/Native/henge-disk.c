// A new, empty disk for the Mac to use as a USB drive: a sparse file (it takes room only as the Mac fills it)
// holding an empty FAT32 file system, the whole disk with no partition map, as a new USB stick comes. macOS
// mounts that at once, with no "not readable" alert to answer, and Henge Tools then makes it a Mac disk (APFS)
// while it's still empty (tools/henge-agent.m). The layout is Microsoft's FAT specification's, with its sizes.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "henge-vm.h"

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v), le16(p + 2, v >> 16); }

static int put(int fd, uint64_t sector, const uint8_t *data, size_t len) {
    return pwrite(fd, data, len, (off_t)(sector * 512)) == (ssize_t)len ? 0 : -1;
}

// The name in the 11 characters FAT has: capitals, digits, a few marks; spaces at the end.
static void fat_label(uint8_t out[11], const char *name) {
    memset(out, ' ', 11);
    size_t n = 0;
    for (const char *p = name; p && *p && n < 11; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr(" !#$%&'()-@^_`{}~", c))) out[n++] = (uint8_t)c;
        else if ((unsigned char)c >= 0x80) continue; // the rest of a letter FAT can't hold
        else out[n++] = '_';
    }
    if (n == 0 || out[0] == ' ') memcpy(out, "NO NAME    ", 11);
}

int henge_make_disk(const char *path, uint64_t bytes, const char *name) {
    uint64_t total = bytes / 512;
    if (total < (64ull << 20) / 512 || total > 0xffffffffull) return -1; // 64 MB to 2 TB
    uint32_t spc = total <= 532480 ? 1 : total <= (8ull << 30) / 512 ? 8 : total <= (16ull << 30) / 512 ? 16
                 : total <= (32ull << 30) / 512 ? 32 : 64;
    const uint32_t reserved = 32, fats = 2;
    uint32_t tmp1 = (uint32_t)total - reserved, tmp2 = (256 * spc + fats) / 2;
    uint32_t fat_sectors = (tmp1 + tmp2 - 1) / tmp2;
    uint32_t clusters = (uint32_t)((total - reserved - fats * fat_sectors) / spc);
    if (clusters < 65525 || clusters >= 0x0ffffff5) return -1;

    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    int rc = ftruncate(fd, (off_t)(total * 512));

    uint8_t label[11];
    fat_label(label, name);
    uint8_t boot[512] = {0xeb, 0x58, 0x90, 'M', 'S', 'W', 'I', 'N', '4', '.', '1'};
    le16(boot + 11, 512);
    boot[13] = (uint8_t)spc;
    le16(boot + 14, reserved);
    boot[16] = (uint8_t)fats;
    boot[21] = 0xf8; // fixed media
    le16(boot + 24, 32), le16(boot + 26, 64); // sectors per track, heads: nothing reads them now
    le32(boot + 32, (uint32_t)total);
    le32(boot + 36, fat_sectors);
    le32(boot + 44, 2); // the root folder's cluster
    le16(boot + 48, 1); // FSInfo
    le16(boot + 50, 6); // the boot sector's copy
    boot[64] = 0x80;
    boot[66] = 0x29;
    le32(boot + 67, (uint32_t)time(NULL) ^ (uint32_t)(total * 2654435761u)); // volume ID
    memcpy(boot + 71, label, 11);
    memcpy(boot + 82, "FAT32   ", 8);
    boot[510] = 0x55, boot[511] = 0xaa;

    uint8_t info[512] = {0};
    le32(info, 0x41615252);
    le32(info + 484, 0x61417272);
    le32(info + 488, clusters - 1); // free: all but the root folder's
    le32(info + 492, 3);
    le32(info + 508, 0xaa550000);

    uint8_t fat[12];
    le32(fat, 0x0ffffff8), le32(fat + 4, 0x0fffffff), le32(fat + 8, 0x0fffffff); // media, reserved, root (one cluster)

    uint8_t root[32] = {0};
    memcpy(root, label, 11);
    root[11] = 0x08; // the volume's name

    if (rc == 0) rc = put(fd, 0, boot, 512) | put(fd, 1, info, 512) | put(fd, 6, boot, 512) | put(fd, 7, info, 512);
    for (uint32_t i = 0; rc == 0 && i < fats; i++) rc = put(fd, reserved + i * fat_sectors, fat, sizeof fat);
    if (rc == 0) rc = put(fd, reserved + fats * fat_sectors, root, sizeof root);
    if (rc == 0) rc = fsync(fd);
    close(fd);
    if (rc != 0) unlink(path);
    return rc == 0 ? 0 : -1;
}

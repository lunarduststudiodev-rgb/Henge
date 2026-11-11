// Unpacks the Henge Tools disk (build/henge-tools.img.gz) for the Mac to see as a read-only disk. It is packed
// in the app because a disk of a Mac app is 16 MB of mostly nothing, and tools that sign an IPA leave a .gz alone.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "henge-vm.h"

int henge_unpack(const char *archive, const char *dest) {
    char fresh[2048];
    if ((size_t)snprintf(fresh, sizeof fresh, "%s.new", dest) >= sizeof fresh) return -1;
    gzFile z = gzopen(archive, "rb");
    if (!z) return -1;
    unlink(fresh);
    int fd = open(fresh, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    int rc = fd < 0 ? -1 : 0;
    char *buf = malloc(1 << 16);
    if (!buf) rc = -1;
    unsigned long long total = 0;
    while (rc == 0) {
        int n = gzread(z, buf, 1 << 16);
        if (n < 0) rc = -1;
        if (n <= 0) break;
        if (gzdirect(z)) rc = -1; // not compressed: zlib would pass it through as it is
        else if (write(fd, buf, (size_t)n) != n) rc = -1;
        total += (unsigned)n;
    }
    if (total == 0 || total % 512) rc = -1; // a disk is whole sectors
    int errnum = 0;
    gzerror(z, &errnum);
    if (errnum != Z_OK && errnum != Z_STREAM_END) rc = -1;
    if (!gzeof(z)) rc = -1; // cut short
    gzclose(z);
    free(buf);
    if (fd >= 0 && close(fd) != 0) rc = -1;
    // In place of the last one all at once: a Mac still reading the old one keeps it until it's done.
    if (rc == 0 && rename(fresh, dest) != 0) rc = -1;
    if (rc != 0) unlink(fresh);
    return rc;
}

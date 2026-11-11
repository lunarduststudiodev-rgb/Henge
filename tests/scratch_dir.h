/* A folder for a test's files: the one named on the command line, or a new one in $TMPDIR (APFS, so clones work)
   that the test removes when it ends (make test runs every test with no arguments). */
#ifndef HENGE_TESTS_SCRATCH_DIR_H
#define HENGE_TESTS_SCRATCH_DIR_H

#define _DARWIN_C_SOURCE
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char scratch_path[1024];
static int scratch_made;

static int scratch_rm(const char *p, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st, (void)flag, (void)ftw;
    return remove(p);
}

static const char *scratch_dir(int argc, char **argv, const char *name) {
    if (argc >= 2) return argv[1];
    const char *tmp = getenv("TMPDIR");
    snprintf(scratch_path, sizeof scratch_path, "%s/%s.XXXXXX", tmp && *tmp ? tmp : "/tmp", name);
    if (!mkdtemp(scratch_path)) {
        perror("mkdtemp");
        exit(2);
    }
    scratch_made = 1;
    return scratch_path;
}

/* Removes the folder if this test made it. */
static void scratch_done(void) {
    if (scratch_made) nftw(scratch_path, scratch_rm, 16, FTW_DEPTH | FTW_PHYS);
}

#endif

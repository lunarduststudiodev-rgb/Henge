/* Zeroed memory as aligned as the type asks: calloc's 16 bytes is less than HvCpu (128) and HvPerCpu (16 KB, its
   arrays given back page by page) ask for. */
#pragma once
#include <stdlib.h>
#include <string.h>

static inline void *zalloc(size_t align, size_t size) {
    void *p = NULL;
    if (posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size)) abort();
    return memset(p, 0, size);
}

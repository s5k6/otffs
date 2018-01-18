#include "common.h"
#include "fmap.h"
#include <err.h>
#include <sys/mman.h>
#include <unistd.h>

void fmap_map(struct mapping *m, int fd, size_t off, size_t len) {
    size_t
        ps = (size_t)sysconf(_SC_PAGE_SIZE),
        adjOff = (off / ps) * ps,
        delta = off - adjOff;
    m->adjLen = len + delta;
    m->adjPtr = mmap(NULL, m->adjLen, PROT_READ, MAP_SHARED, fd, (off_t)adjOff);
    ERRIF(m->adjPtr == MAP_FAILED);
    m->buf = (char*)(m->adjPtr) + delta;
}

void fmap_unmap(struct mapping *m) {
    ERRIF(munmap(m->adjPtr, m->adjLen));
    zero(m);
}

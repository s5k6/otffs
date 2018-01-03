/* Frontend to mapping files into memory.  Need to adjust for page
   sizes, see mmap(2). */

#ifndef fmap_kpsyx6XcOBUD
#define fmap_kpsyx6XcOBUD

#include <stddef.h>

struct mapping {
    char *buf; // points to buffer as requested
    void *adjPtr; // real ptr returned from mmap
    size_t adjLen; // real length mapped by mmap
};

void fmap_map(struct mapping *m, int fd, size_t off, size_t len);

void fmap_unmap(struct mapping *m);


#endif

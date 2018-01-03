
/* BEGIN COPY CODE */

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>



// this is not assert: not intended to be disabled in non-debug mode
#define ERRIF(c) do {                                             \
        if (c)                                                    \
            err(1, #c " at " __FILE__ ":%d because", __LINE__);   \
    } while (0)



#define zero(ptr) memset(ptr, 0, sizeof(*ptr))



/* Frontend to mapping files into memory.  Need to adjust for page
   sizes, see mmap(2). */

struct mapping {
    char *buf; // points to buffer as requested
    void *adjPtr; // real ptr returned from mmap
    size_t adjLen; // real length mapped by mmap
};

static void otf_map(struct mapping *m, int fd, size_t off, size_t len) {
    size_t
        ps = (size_t)sysconf(_SC_PAGE_SIZE),
        adjOff = (off / ps) * ps,
        delta = off - adjOff;
    m->adjLen = len + delta;
    //warnx("off=%zu len=%zu delta=%zu", off, len, delta);
    m->adjPtr = mmap(NULL, m->adjLen, PROT_READ, MAP_SHARED, fd, (off_t)adjOff);
    ERRIF(m->adjPtr == MAP_FAILED);
    m->buf = (char*)(m->adjPtr) + delta;
}

void otf_unmap(struct mapping *m) {
    ERRIF(munmap(m->adjPtr, m->adjLen));
    zero(m);
}

#define mmap DO_NOT_USE
#define munmap DO_NOT_USE

/* END COPY CODE */

int main(int argc, char **argv)
{
    warnx("Comparing two files, repeating the *first* to "
          "match length of second");
    
    if (argc < 3)
        errx(1, "Need two file arguments");

    int fd1, fd2;
    struct mapping m1, m2;
    struct stat sb;
    size_t s1, s2;

    fd1 = open(argv[1], O_RDONLY);
    ERRIF(fd1 < 0);
    fstat(fd1, &sb);
    s1 = (size_t)sb.st_size;
    otf_map(&m1, fd1, 0, s1);
        
    fd2 = open(argv[2], O_RDONLY);
    ERRIF(fd2 < 0);
    fstat(fd2, &sb);
    s2 = (size_t)sb.st_size;
    otf_map(&m2, fd2, 0, s2);

    for (size_t i = 0; i < s2; i++) {
        if ( m1.buf[i % s1] != m2.buf[i] )
            errx(1, "Difference at %zu: %s[%zu] != %s[%zu]",
                 i, argv[1], i % s1, argv[2], i % s2);
    }
    warnx("Compared %zu bytes: All equal", s2);

    return 0;
}

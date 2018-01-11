
/* BEGIN COPY CODE */

#include "fmap.h"
#include "macros.h"
#include <err.h>
#include <fcntl.h>
#include <sys/stat.h>

#define mmap DO_NOT_USE
#define munmap DO_NOT_USE

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
    fmap_map(&m1, fd1, 0, s1);

    fd2 = open(argv[2], O_RDONLY);
    ERRIF(fd2 < 0);
    fstat(fd2, &sb);
    s2 = (size_t)sb.st_size;
    fmap_map(&m2, fd2, 0, s2);

    for (size_t i = 0; i < s2; i++) {
        if ( m1.buf[i % s1] != m2.buf[i] )
            errx(1, "Difference at %zu: %s[%zu] != %s[%zu]",
                 i, argv[1], i % s1, argv[2], i % s2);
    }
    warnx("Compared %zu bytes: All equal", s2);

    return 0;
}

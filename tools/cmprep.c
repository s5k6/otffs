#include "common.h"
#include "fmap.h"
#include <err.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define mmap DO_NOT_USE
#define munmap DO_NOT_USE

/* Given two files A and B, sheck whether B is a prefix of the
   infinite repetition of A, i.e., whether there exists an `n`, so
   that

       (A ++ A ++ …)[0..n] == B

   holds.
 */

enum { BUFLEN = 1024 * 4 };

int main(int argc, char **argv) {
    if (argc < 3)
        errx(1, "Need two file arguments");

    int fd1;
    struct mapping m;
    size_t s1;

    { /* Map first file into memory. */
        struct stat sb;
        fd1 = open(argv[1], O_RDONLY);
        ERRIF(fd1 < 0);
        fstat(fd1, &sb);
        s1 = (size_t)sb.st_size;
        fmap_map(&m, fd1, 0, s1);
    }

    int fd2;
    size_t s2;

    { /* Open second file, don't map, it might be large. */
        struct stat sb;
        fd2 = open(argv[2], O_RDONLY);
        ERRIF(fd2 < 0);
        fstat(fd2, &sb);
        s2 = (size_t)sb.st_size;
    }

    ssize_t r;  // bytes returnedfrom read
    size_t count = 0; // total read bytes
    char buf2[BUFLEN]; // read buffer for 2nd file

    while ((r = read(fd2, buf2, BUFLEN)) > 0) {

        /* compare read buffer with section of mmap'd file */
        for (size_t i = 0; i < (size_t)r; i++) {
            if (m.buf[(count + i) % s1] != buf2[i])
                errx(1, "Difference at %zu: %s[%zu] != %s[%zu]",
                     i, argv[1], i % s1, argv[2], i % s2);
        }

        count += (size_t)r;
    }

    /* check everything was read. */
    if (count != s2)
        errx(1, "Got only %zu/%zu bytes from %s", count, s2, argv[2]);

    return 0;
}

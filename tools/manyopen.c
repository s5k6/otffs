#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <err.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

#define delay(ms) usleep(1000 * ms)

int main(int argc, char **argv)
{
        int fds[10];

        if (argc < 2)
            errx(1, "need file to read from");
        char *fn = argv[1];

        useconds_t t = 0;
        if (argc > 2)
            t = (useconds_t)atol(argv[2]);

        for (size_t i = 0; i < sizeof(fds) / sizeof(*fds); i++) {
            printf("%ld = open(%s)\n", i, fn);
            fds[i] = open(fn, O_RDONLY);
            if (fds[i] < 0)
                err(1, "Opening %s", fn);
            delay(t);
        }

        delay(t);


        char buf1[11];
        ssize_t r1 = read(fds[0], buf1, sizeof(buf1) - 1);
        if (r1 < 1)
            err(1, "Reading 0");
        buf1[r1] = '\0';
        printf("Reference string has length %ld\n", r1);

        for (size_t i = 1; i < sizeof(fds) / sizeof(*fds); i++) {

            char buf2[sizeof(buf1)];

            ssize_t r2 = read(fds[i], buf2, sizeof(buf2) - 1);
            if (r2 < 1)
                err(1, "reading %ld", i);
            buf2[r2] = '\0';
            if (strncmp(buf1, buf2, (size_t)r2))
                err(1, "Differen content @%ld, %s != %s",
                    i, buf1, buf2);
            printf("read(%ld) returned expected %ld bytes\n", i, (size_t)r2);
            delay(t);
        }

        delay(t);
        
        for (size_t i = 0; i < sizeof(fds) / sizeof(*fds); i++) {
            printf("close(%ld)\n", i);
            fds[i] = close(fds[i]);
            if (fds[i] < 0)
                err(1, "closing %ld", i);
            delay(t);
        }
        
        
	return 0;
}

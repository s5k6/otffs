#define _GNU_SOURCE

#include "common.h"
#include "parser.h"
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_NAME_LENGTH 128
#define READ_BUF_SIZE (1<<10)



struct {
    const char *s;
    off_t f;
} suf[] = {
    { "ki", 1UL << 10 }, { "k", 1000 },
    { "Mi", 1UL << 20 }, { "M", 1000000 },
    { "Gi", 1UL << 30 }, { "G", 1000000000 },
    { "Ti", 1UL << 40 }, { "T", 1000000000000 },
    { "Pi", 1UL << 50 }, { "P", 1000000000000000 },
    { "Ei", 1UL << 60 }, { "E", 1000000000000000000 },
    { "x", -1 },
};


static size_t addFun(char *k, size_t v, size_t *o) {
    (void)v; (void)o;
    errx(1, "Conflicting definition of file: %s", k);
    return 0;
}

int parse(struct fileSystem *pr, int fd) {

    ssize_t n;
    char read_buf[READ_BUF_SIZE];

    STACK(char) buf;
    ALLOCATE(buf, 32);

    struct token {
        enum type { tPlain, tQuoted, tColon, tComma, tNewline } ty;
        char *str;
        size_t lin, col;
    };

    STACK(struct token) tok;
    ALLOCATE(tok, 32);

    enum { sSpace, sPlain, sQuoted, sComment } state = sSpace;

    size_t lin = 1, col = 0;

    while ((n = read(fd, read_buf, READ_BUF_SIZE)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = read_buf[i];
            col++;

            switch (state) {

            case sSpace:
                if (isblank(c))
                    break;
                if (isalpha(c)) {
                    i--;
                    col--;
                    state = sPlain;
                    break;
                }
                if (isdigit(c)) {
                    i--;
                    col--;
                    state = sPlain; // do parsing later: octal? decimal?
                    break;
                }
                ENOUGH(tok);
                switch (c) {
                case ':':
                    PUSH(tok, ((struct token){ tColon, 0, lin, col }));
                    break;
                case ',':
                    PUSH(tok, ((struct token){ tComma, 0, lin, col }));
                    break;
                case '"':
                    state = sQuoted;
                    break;
                case '#':
                    state = sComment;
                    break;
                case '\n':
                    lin++;
                    col = 0;
                    PUSH(tok, ((struct token){ tNewline, 0, lin, col }));
                    break;
                default: errx(1, "Unexpected `%c` before %ld:%ld", c, lin, col);
                }
                break;

            case sComment:
                if (c=='\n') {
                    i--;
                    col--;
                    state = sSpace;
                }
                break;

            case sPlain:
                if (isalnum(c)) {
                    PUSH(buf, c);
                    break;
                }
                i--;
                col--;
                PUSH(buf, '\0');
                TRIM(buf);
                ENOUGH(tok);
                PUSH(tok, ((struct token){ tPlain, buf.array, lin, col }));
                ALLOCATE(buf, 32);
                state = sSpace;
                break;

            case sQuoted:
                if (c == '"') {
                    PUSH(buf, '\0');
                    TRIM(buf);
                    ENOUGH(tok);
                    PUSH(tok, ((struct token){ tQuoted, buf.array, lin, col }));
                    ALLOCATE(buf, 32);
                    state = sSpace;
                    break;
                }
                if (c == '\n') {
                    lin++;
                    col=0;
                }
                PUSH(buf, c);
                break;

            } // switch (state)

            ENOUGH(buf);
        }
    }

    free(buf.array);
    close(fd);


    enum {
        pName, pColon, pNext, pKey, pPass, pSize, pMode, pMtime, pFill
    } pState = pName;

    struct file *current = new(struct file);
    *current = uninitFile;
    char *name = NULL;

    for (size_t t = 0; t < tok.used; t++) {
        switch (pState) {

        case pName:
            switch (AT(tok,t).ty) {
            case tPlain:
            case tQuoted:
                name = AT(tok,t).str;
                assert(name);
                pState = pColon;
                break;
            case tNewline:
                break;
            default:
                errx(1, "Expected file name before %ld:%ld",
                     AT(tok,t).lin, AT(tok,t).col);
                break;
            }
            break;

        case pColon:
            if (AT(tok,t).ty == tColon) {
                pState = pKey;
                break;
            }
            errx(1, "Expected `:` before %ld:%ld", AT(tok,t).lin, AT(tok,t).col);
            break;

        case pNext:
            switch (AT(tok,t).ty) {
            case tComma:
                pState = pKey;
                break;
            case tNewline:
                avl_insertWith((avl_AddFun)addFun, pr->names, name,
                               pr->files.used, NULL);
                ENOUGH(pr->files);
                PUSH(pr->files, current);
                current = new(struct file);
                *current = uninitFile;
                pState = pName;
                break;
            default:
                errx(1, "Expected `,` or newline before %ld:%ld",
                     AT(tok,t).lin, AT(tok,t).col);
            }
            break;

        case pKey:
            if (AT(tok,t).ty != tPlain) {
                errx(1, "Expected keyword before %ld:%ld",
                     AT(tok,t).lin, AT(tok,t).col);
                break;
            }
            if (!strcmp("pass", AT(tok,t).str)) {
                pState = pPass;
                break;
            }
            if (!strcmp("fill", AT(tok,t).str)) {
                pState = pFill;
                break;
            }
            if (!strcmp("size", AT(tok,t).str)) {
                pState = pSize;
                break;
            }
            if (!strcmp("mode", AT(tok,t).str)) {
                pState = pMode;
                break;
            }
            if (!strcmp("mtime", AT(tok,t).str)) {
                pState = pMtime;
                break;
            }
            errx(1, "Unexpected key `%s` before %ld:%ld when defining `%s`",
                 AT(tok,t).str, AT(tok,t).lin, AT(tok,t).col, name);
            break;

        case pFill: {
            unsigned int found = 0;
            for (unsigned int i = 1; algorithms[i]; i++) {
                if (!strcmp(algorithms[i], AT(tok,t).str)) {
                    found = i;
                    break;
                }
            }
            if (found) {
                current->srcName = NULL;
                current->srcSize = found;
                pState = pNext;
                break;
            }
            errx(1, "Unexpected fill mode `%s` before %ld:%ld",
                 AT(tok,t).str, AT(tok,t).lin, AT(tok,t).col);
            break;
        }

        case pPass:
            switch (AT(tok,t).ty) {
            case tPlain:
            case tQuoted:
                current->srcName = AT(tok,t).str;
                pState = pNext;
                break;
            case tComma:
            case tNewline:
                current->srcName = name;
                pState = pNext;
                t--;
                break;
            default:
                errx(1, "Expected source name before %ld:%ld",
                     AT(tok,t).lin, AT(tok,t).col);
                break;
            }
            break;

        case pSize:
            switch (AT(tok,t).ty) {
            case tPlain:
                {
                    char *e;
                    long int x = strtol(AT(tok,t).str, &e, 10);
                    if (x < 0 || x == LONG_MAX)
                        errx(1, "Invalid size before %ld:%ld",
                             AT(tok,t).lin, AT(tok,t).col);
                    current->size = (ssize_t)x;
                    if (*e) {
                        off_t f = 0;
                        for (size_t s = 0; s < sizeof(suf)/sizeof(*suf); s++) {
                            if (!strcmp(suf[s].s, e)) {
                                f = suf[s].f;
                                break;
                            }
                        }
                        if (!f)
                            errx(1, "Invalid suffix `%s` before %ld:%ld",
                                 e, AT(tok,t).lin, AT(tok,t).col);
                        current->size *= f;
                    }
                    pState = pNext;
                }
                break;
            default:
                errx(1, "Expected file size before %ld:%ld",
                     AT(tok,t).lin, AT(tok,t).col);
                break;
            }
            break;

        case pMtime:
            switch (AT(tok,t).ty) {
            case tPlain:
                {
                    char *e;
                    long int x = strtol(AT(tok,t).str, &e, 10);
                    if (x < 0 || x == LONG_MAX || *e)
                        errx(1, "Invalid unix time `%s` before %ld:%ld",
                             AT(tok,t).str, AT(tok,t).lin, AT(tok,t).col);
                    current->mtime = x;
                    pState = pNext;
                }
                break;
            default:
                errx(1, "Expected file time before %ld:%ld",
                     AT(tok,t).lin, AT(tok,t).col);
                break;
            }
            break;

        case pMode:
            switch (AT(tok,t).ty) {
            case tPlain:
                {
                    char *e;
                    long int x = strtol(AT(tok,t).str, &e, 8);
                    if (x < 0 || 0777 < x || *e)
                        errx(1, "Invalid file mode `%s` before %ld:%ld",
                             AT(tok,t).str, AT(tok,t).lin, AT(tok,t).col);
                    current->mode = S_IFREG | (mode_t)(x & 0777);
                    pState = pNext;
                }
                break;
            default:
                errx(1, "Expected file mode before %ld:%ld",
                     AT(tok,t).lin, AT(tok,t).col);
                break;
            }
            break;

        }
    }

    free(tok.array);

    return 0;
}

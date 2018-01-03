#define _GNU_SOURCE

#include "macros.h"
#include "parser.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_NAME_LENGTH 128
#define READ_BUF_SIZE (1<<10)


// FIXME: duplicated code: otffs.c
static void *_new(size_t size) {
    void *tmp = malloc(size);
    if (!tmp)
        err(1, "allcating %zu bytes", size);
    return tmp;
}
#define new(ty) _new(sizeof(ty))


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
};


/* New file records are initialisedfrom here.  Values not set by the
   user may be retriefed rom thefile system, or made up. */

struct file uninitFile = {
    .size = -1,
    .srcName = NULL,
    .srcSize = -1,
    .mode = 07000000, // FIXME really invalid?
    .nlink = 0,
    .atime = -1,
    .mtime = -1,
};


size_t addFun(char *k, size_t v, size_t *o) {
    (void)v; (void)o;
    errx(1, "Redefining file: %s", k);
    return 0;
}

int parse(struct parseBuf *retBuf, int fd) {

    ssize_t n;
    char read_buf[READ_BUF_SIZE];

    ARRAY(char, buf);
    ALLOCATE(buf, 32);

    struct token {
        enum type { tPlain, tQuoted, tColon, tComma, tNewline } ty;
        char *str;
        size_t lin, col;
    };

    ARRAY(struct token, tok);
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
                    ADD(tok, ((struct token){ tColon, 0, lin, col }));
                    break;
                case ',':
                    ADD(tok, ((struct token){ tComma, 0, lin, col }));
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
                    ADD(tok, ((struct token){ tNewline, 0, lin, col }));
                    break;
                default: errx(1, "Unexpected `%c`", c);
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
                    ADD(buf, c);
                    break;
                }
                i--;
                col--;
                ADD(buf, '\0');
                ENOUGH(tok);
                ADD(tok, ((struct token){ tPlain, buf, lin, col }));
                ALLOCATE(buf, 32);
                state = sSpace;
                break;

            case sQuoted:
                if (c == '"') {
                    ADD(buf, '\0');
                    ENOUGH(tok);
                    ADD(tok, ((struct token){ tQuoted, buf, lin, col }));
                    ALLOCATE(buf, 32);
                    state = sSpace;
                    break;
                }
                if (c == '\n') {
                    lin++;
                    col=0;
                }
                ADD(buf, c);
                break;

            } // switch (state)

            ENOUGH(buf);
        }
    }

    free(buf);
    close(fd);


    ARRAY(struct file *, files);
    ALLOCATE(files, 16);

    avl_Tree index = avl_new((avl_CmpFun)strcmp);

    enum {
        pName, pColon, pNext, pKey, pPass, pSize, pMode, pMtime, pFill
    } pState = pName;

    struct file *current = new(struct file);
    *current = uninitFile;
    char *name = NULL;

    for (size_t t = 0; t < tok_used; t++) {
        switch (pState) {

        case pName:
            switch (tok[t].ty) {
            case tPlain:
            case tQuoted:
                name = tok[t].str;
                ERRIF(!name);
                pState = pColon;
                break;
            case tNewline:
                break;
            default:
                errx(1, "Expected file name before %ld:%ld",
                     tok[t].lin, tok[t].col);
                break;
            }
            break;

        case pColon:
            if (tok[t].ty == tColon) {
                pState = pKey;
                break;
            }
            errx(1, "Expected `:` before %ld:%ld", tok[t].lin, tok[t].col);
            break;

        case pNext:
            switch (tok[t].ty) {
            case tComma:
                pState = pKey;
                break;
            case tNewline:
                avl_insertWith((avl_AddFun)addFun, index, name,
                               files_used, NULL);
                ENOUGH(files);
                ADD(files, current);
                current = new(struct file);
                *current = uninitFile;
                pState = pName;
                break;
            default:
                errx(1, "Expected `,` or newline before %ld:%ld",
                     tok[t].lin, tok[t].col);
            }
            break;

        case pKey:
            if (tok[t].ty != tPlain) {
                errx(1, "Expected keyword before %ld:%ld",
                     tok[t].lin, tok[t].col);
                break;
            }
            if (!strcmp("pass", tok[t].str)) {
                pState = pPass;
                break;
            }
            if (!strcmp("fill", tok[t].str)) {
                pState = pFill;
                break;
            }
            if (!strcmp("size", tok[t].str)) {
                pState = pSize;
                break;
            }
            if (!strcmp("mode", tok[t].str)) {
                pState = pMode;
                break;
            }
            if (!strcmp("mtime", tok[t].str)) {
                pState = pMtime;
                break;
            }
            errx(1, "Unexpected key `%s` before %ld:%ld when defining `%s`",
                 tok[t].str, tok[t].lin, tok[t].col, name);
            break;

        case pFill:
            if (!strcmp("integers", tok[t].str)) {
                current->srcName = NULL;
                current->srcSize = 1;
                pState = pNext;
                break;
            }
            errx(1, "Unexpected fill mode `%s` before %ld:%ld",
                 tok[t].str, tok[t].lin, tok[t].col);
            break;

        case pPass:
            switch (tok[t].ty) {
            case tPlain:
            case tQuoted:
                current->srcName = tok[t].str;
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
                     tok[t].lin, tok[t].col);
                break;
            }
            break;

        case pSize:
            switch (tok[t].ty) {
            case tPlain:
                {
                    char *e;
                    current->size = strtol(tok[t].str, &e, 10);
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
                                 e, tok[t].lin, tok[t].col);
                        current->size *= f;
                    }
                    pState = pNext;
                }
                break;
            default:
                errx(1, "Expected file size before %ld:%ld",
                     tok[t].lin, tok[t].col);
                break;
            }
            break;

        case pMtime:
            switch (tok[t].ty) {
            case tPlain:
                {
                    char *e;
                    current->mtime = strtol(tok[t].str, &e, 10);
                    if (*e)
                        errx(1, "Invalid unix time `%s` before %ld:%ld",
                             tok[t].str, tok[t].lin, tok[t].col);
                    pState = pNext;
                }
                break;
            default:
                errx(1, "Expected file time before %ld:%ld",
                     tok[t].lin, tok[t].col);
                break;
            }
            break;

        case pMode:
            switch (tok[t].ty) {
            case tPlain:
                {
                    char *e;
                    current->mode = (mode_t)strtol(tok[t].str, &e, 8);
                    if (*e)
                        errx(1, "Invalid file mode `%s` before %ld:%ld",
                             tok[t].str, tok[t].lin, tok[t].col);
                    current->mode = S_IFREG | (current->mode & 0777);
                    pState = pNext;
                }
                break;
            default:
                errx(1, "Expected file mode before %ld:%ld",
                     tok[t].lin, tok[t].col);
                break;
            }
            break;

        }
    }

    free(tok);

    retBuf->files = files;
    retBuf->files_used = files_used;
    retBuf->files_alloc = files_alloc;
    retBuf->index = index;

    return 0;
}

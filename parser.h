#ifndef parser_RlBM2GiE1tuq
#define parser_RlBM2GiE1tuq

#include "avl_tree.h"
#include "macros.h"
#include <sys/types.h>
#include <stdlib.h>

struct file {
    off_t size;
    mode_t mode;
    nlink_t nlink;
    time_t atime, mtime;
    char *srcName;
    off_t srcSize;
};

struct file uninitFile;

struct parseResult {
    STACK(struct file *) files;
    avl_Tree names;
};

int parse(struct parseResult *parseResult, int fd);

#endif

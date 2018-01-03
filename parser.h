#ifndef parser_RlBM2GiE1tuq
#define parser_RlBM2GiE1tuq

#include "avl_tree.h"
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

struct parseBuf {
    avl_Tree index;
    struct file **files;
    size_t files_used;
    size_t files_alloc;
};

int parse(struct parseBuf *buf, int fd);

#endif

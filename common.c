#include "common.h"



struct file uninitFile = {
    .size = -1,
    .srcName = NULL,
    .srcSize = -1, //
    .mode = 07000000, // FIXME really invalid?
    .nlink = 0,
    .atime = -1,
    .mtime = -1,
    .ctime = -1,
};



void *_new(size_t size) {
    void *tmp = malloc(size);
    if (!tmp)
        err(1, "allocating %zu bytes", size);
    return tmp;
}



const char *algorithms[] = {
    [algoRoot] = "root",
    [algoIntegers] = "integers",
    [algoChars] = "chars",
    NULL,
};

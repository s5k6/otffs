#include "parser.h"
#include "macros.h"
#include "avl_tree.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>


int listFun(char *name, size_t ino, struct file **files) {
    printf("%s (%lu B): %s (%ld B) mode=%03o mtime=%ld\n",
           name,
           files[ino]->size,
           files[ino]->srcName,
           files[ino]->srcSize,
           files[ino]->mode,
           files[ino]->mtime
           );
    return 0;
}

int main(void) {

    struct parseBuf buf;
    
    int fd = open("mnt/otffsrc", O_RDONLY);
    ERRIF(!fd);
    parse(&buf, fd);
    close(fd);

    printf("\nParsed %zu entries in config file.\n", buf.files_used);

    avl_traverse(buf.index, (avl_VisitorFun)listFun, buf.files);
    free(buf.files);

    return 0;
}

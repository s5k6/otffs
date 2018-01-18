#include "avl_tree.h"
#include "common.h"
#include "parser.h"
#include <fcntl.h>
#include <stdio.h>
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

    struct fileSystem pr;
    pr.names = avl_new((avl_CmpFun)strcmp);
    ALLOCATE(pr.files, 8);
    {
        int fd = open("../demo/otffsrc", O_RDONLY);
        ERRIF(!fd);
        parse(&pr, fd);
        close(fd);
    }

    printf("\nParsed %zu entries in config file.\n", pr.files.used);

    avl_traverse(pr.names, (avl_VisitorFun)listFun, pr.files.array);
    free(pr.files.array);

    return 0;
}

#define FUSE_USE_VERSION 31
#define _GNU_SOURCE // reallocarray

#include "fmap.h"
#include "macros.h"
#include "parser.h"
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fuse_lowlevel.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define mmap DO_NOT_USE
#define munmap DO_NOT_USE

#define MAX_NAME_LENGTH 128
#define DEFAULT_TIMEOUT 1.0
#define VERBOSE 0


void *_new(size_t size) {
    void *tmp = malloc(size);
    if (!tmp)
        err(1, "allcating %zu bytes", size);
    return tmp;
}
#define new(ty) _new(sizeof(ty))


ARRAY(struct file *, files);

struct timeval now; // time of starting `otffs`

int rootFd = -1; // pre-mount descriptor of mount point



static int otf_stat(struct stat *buf, fuse_ino_t ino) {

    ERRIF(!files[ino]);

    zero(buf);
    *buf = (struct stat){
        .st_ino = ino,
        .st_mode = files[ino]->mode,
        .st_nlink = files[ino]->nlink,
        .st_size = files[ino]->size,
        .st_atime = files[ino]->atime,
        .st_mtime = files[ino]->mtime,
        .st_uid = getuid(),
        .st_gid = getgid(),
    };

    return 0;
}


static void otf_getattr(fuse_req_t req, fuse_ino_t ino,
                    struct fuse_file_info *fi) {
    (void) fi;

    if (VERBOSE)
        warnx("getattr(ino=%ld)", ino);

    struct stat buf;
    if (otf_stat(&buf, ino))
        ERRIF(fuse_reply_err(req, ENOENT));
    else
        ERRIF(fuse_reply_attr(req, &buf, DEFAULT_TIMEOUT));
}



// FIXME review this code
static int otf_replyBufLimited(fuse_req_t req, const char *buf, size_t bufsize,
			     off_t off, size_t maxsize) {

    if (off < (ssize_t)bufsize)
        return fuse_reply_buf(
            req,
            buf + off,
            (size_t)min((ssize_t)bufsize - off, (ssize_t)maxsize)
        );
    else
        return fuse_reply_buf(req, NULL, 0);
}

static void otf_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                    off_t off, struct fuse_file_info *fi)
{
    (void) fi;

    if (VERBOSE)
        warnx("readdir(ino=%ld)", ino);

    if (ino != FUSE_ROOT_ID)
        ERRIF(fuse_reply_err(req, ENOTDIR));
    else {
        char *p = NULL;
        size_t s = 0;

        for (unsigned int i = 0; i < files_used; i++) {
            if (!files[i])
                continue;

            struct stat buf;
            size_t oldsize = s;
            s += fuse_add_direntry(req, NULL, 0, files[i]->name, NULL, 0);
            p = (char *)realloc(p, s);
            ERRIF(!p);

            otf_stat(&buf, i);
            fuse_add_direntry(req, p + oldsize, s - oldsize, files[i]->name,
                              &buf, (off_t)s);

        }

        otf_replyBufLimited(req, p, s, off, size);
        free(p);
    }
}



static void otf_lookup(fuse_req_t req, fuse_ino_t parent,
                            const char *name) {

    if (VERBOSE)
        warnx("lookup(parent=%ld, name=%s)", parent, name);

    ERRIF(!files);

    if (!files[parent]) {
        ERRIF(fuse_reply_err(req, ENOENT));
        return;
    }

    if (!S_ISDIR(files[parent]->mode)) {
        ERRIF(fuse_reply_err(req, ENOTDIR));
        return;
    }

    /* FIXME check permissions?  Should be done by kernel, see `-o
       default_permissions` */

    /* FIXME get list of files for this specific directory.  Currently
       there's only one dir, containing all the files. */

    for (unsigned int i = 0; i < files_used; i++) {
        if (!files[i])
            continue;

        if (!strncmp(name, files[i]->name, MAX_NAME_LENGTH)) {
            struct fuse_entry_param e;
            e = (struct fuse_entry_param){
                .ino = i,
                .attr_timeout = DEFAULT_TIMEOUT,
                .entry_timeout = DEFAULT_TIMEOUT,
            };
            otf_stat(&e.attr, e.ino);
            ERRIF(fuse_reply_entry(req, &e));
            return;
        }
    }

    ERRIF(fuse_reply_err(req, ENOENT));
}



static void otf_open(fuse_req_t req, fuse_ino_t ino,
                       struct fuse_file_info *fi) {

    ERRIF(!files);

    if (!files[ino]) {
        ERRIF(fuse_reply_err(req, ENOENT));
        return;
    }

    warnx("open ino=%ld name=\"%s\"", ino, files[ino]->name);

    if (!S_ISREG(files[ino]->mode)) {
        ERRIF(fuse_reply_err(req, EISDIR)); // FIXME not entirely correct
        return;
    }

    int fd;
    if (files[ino]->srcName) {
        fd = openat(rootFd, files[ino]->srcName, O_RDONLY);
        ERRIF(fd == -1);
    } else {
        fd = 0;
    }

    fi->fh = (unsigned long)fd;
    ERRIF(fuse_reply_open(req, fi));
}



void otf_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {

    (void)req;

    ERRIF(!files);
    ERRIF(!files[ino]);

    warnx("release ino=%ld name=\"%s\"", ino, files[ino]->name);

    if (files[ino]->srcName)
        close((int)(fi->fh));

}



static void otf_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {

    (void)parent;

    ERRIF(!files);

    for (unsigned int i = 0; i < files_used; i++) {
        if (!files[i])
            continue;

        if (!strncmp(name, files[i]->name, MAX_NAME_LENGTH)) {
            free(files[i]->name);
            free(files[i]);
            files[i] = NULL;
            ERRIF(fuse_reply_err(req, 0));
            return;
        }
    }

    ERRIF(fuse_reply_err(req, ENOENT));
}





static void otf_fill_counting(char *buf, off_t off, size_t amount) {

    size_t chunk = sizeof(unsigned int);
    size_t steps = amount / chunk;
    size_t rem = amount % chunk;

    if (VERBOSE)
        warnx("fill_counting(off=%ld, amount=%ld)", off, amount);

    unsigned int value = (unsigned int)off / (unsigned int)chunk;

    for (size_t i = 0; i < steps; i++) {
        memcpy(&buf[i * chunk], &value, chunk);
        value++;
    }
    if (rem) {
        memcpy(&buf[amount - rem], &value, rem);
    }
}



static void otf_read(fuse_req_t req, fuse_ino_t ino, size_t len, off_t off,
                      struct fuse_file_info *fi) {

    if (VERBOSE)
        warnx("read(ino=%ld, len=%zu, off=%zu)", ino, len, off);

    ERRIF(!files);

    if (!files[ino]) {
        ERRIF(fuse_reply_err(req, ENOENT));
        return;
    }

    if (off > files[ino]->size) {
        ERRIF(fuse_reply_buf(req, NULL, 0));
        return;
    }

    size_t amount = min(len, (size_t)(files[ino]->size - off));

    if (amount <=0) {
        ERRIF(fuse_reply_buf(req, NULL, 0));
        return;
    }

    if (files[ino]->srcName) {

        /* Produce a file that is a repetition of the source file. */
        const size_t
            b = (size_t)files[ino]->srcSize,
            k = (size_t)off / b,
            l = ((size_t)off + amount) / b,
            s = (size_t)off % b,
            e = (s + amount) % b,
            c = l-k+1;

#if 0 //U4aByyvCWHMy
        warnx("Chunk range=%zu..%zu b=%zu (k=%zu l=%zu c=%zu) s=%zu e=%zu",
              off, (size_t)off+amount, b, k, l, c, s, e);
#endif //U4aByyvCWHMy

        if (c > IOV_MAX)
            errx(1, "The source `%s` is too short (%zuB).  Request of %zuB"
                 " for `%s` needs IOVEC of size %zu, limit is %d!",
                 files[ino]->srcName, b, amount, files[ino]->name, c, IOV_MAX);

        if (k == l) {
            ERRIF(s >= e);
            ERRIF(amount != e - s);
            ERRIF(e >= b);
            struct mapping m;
            fmap_map(&m, (int)(fi->fh), s, e-s);
            ERRIF(fuse_reply_buf(req, m.buf, amount));
            fmap_unmap(&m);
            return;                
        } else {
            struct mapping m;
            fmap_map(&m, (int)(fi->fh), 0, b);

            struct iovec *vector = calloc(c, sizeof(struct iovec));
            ERRIF(!vector);

            vector[0] = (struct iovec){
                .iov_base = m.buf + s,
                .iov_len = b - s
            };
            for (size_t i = 1; i < c-1; i++)
                vector[i] = (struct iovec){
                    .iov_base = m.buf,
                    .iov_len = b
                };
            vector[c-1] = (struct iovec){
                .iov_base = m.buf,
                .iov_len = e
            };
            
#if 0 //qSciMGi8LFdS
            for (size_t i = 0; i < c; i++) {
                warnx("  %zu: %zu..%zu",
                       i,
                       (char*)(vector[i].iov_base) - buf,
                       (char*)(vector[i].iov_base) + vector[i].iov_len - buf);
            }

            {
                int fd = open("fixme", O_WRONLY|O_CREAT, S_IRUSR|S_IWUSR);
                ERRIF(fd < 0);
                ERRIF(writev(fd, vector, (int)c) != (int)amount);
                close(fd);
            }
#endif //qSciMGi8LFdS
            
            ERRIF(fuse_reply_iov(req, vector, (int)(c)));

            fmap_unmap(&m);
            free(vector);
            return;
        }
    } else {
        /* Fill with integers.  FIXME incorrect if offset is not a
           4-byte boundary. */
        char *buf = malloc(amount);
        ERRIF(!buf);
        otf_fill_counting(buf, off, amount);
        ERRIF(fuse_reply_buf(req, buf, amount));
        free(buf);
    }
}


static struct fuse_lowlevel_ops ops = {
    .lookup = otf_lookup,
    .getattr = otf_getattr,
    .readdir = otf_readdir,
    .open = otf_open,
    .read = otf_read,
    .unlink = otf_unlink,
    .release = otf_release,
};



int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_session *se;
    struct fuse_cmdline_opts opts;
    int ret = -1;

    if (fuse_parse_cmdline(&args, &opts) != 0)
        return 1;
    if (opts.show_help) {
        printf("usage: %s [options] <mountpoint>\n\n", argv[0]);
        fuse_cmdline_help();
        fuse_lowlevel_help();
        ret = 0;
        goto err_out1;
    } else if (opts.show_version) {
        printf("FUSE library version %s\n", fuse_pkgversion());
        fuse_lowlevel_version();
        ret = 0;
        goto err_out1;
    }


    /* BEGIN MY CODE */

    ERRIF(gettimeofday(&now, NULL));
    rootFd = open(opts.mountpoint, O_RDONLY|O_DIRECTORY);

    int fd = openat(rootFd, "otffsrc", O_RDONLY);
    ERRIF(fd < 0);
    parse(&files, &files_used, &files_alloc, fd);
    close(fd);

    { // add root directory
        struct file *buf = new(struct file);
        *buf = uninitFile;
        *buf = (struct file){
            .name = strndup(".", MAX_NAME_LENGTH),
            .mode = S_IFDIR | 0755,
            .nlink = 2,
            .atime = now.tv_sec,
            .mtime = now.tv_sec,
        };
        ERRIF(!buf->name);

        if (files[FUSE_ROOT_ID])
            ADD(files, files[FUSE_ROOT_ID]);
        files[FUSE_ROOT_ID] = buf;
    }

    if (files[0]) {
        ADD(files, files[0]);
        files[0] = NULL;
    }    
    
    { // add entries from config file


        for (unsigned int i = 0; i < files_used; i++) {
            if (!files[i])
                continue;

            ERRIF(!files[i]->name);

#if 0
            printf("%3d: %12s : ", i, files[i]->name);
#endif
            
            if (files[i]->srcName) {
                struct stat buf;
                if (fstatat(rootFd, files[i]->srcName, &buf,
                              AT_SYMLINK_NOFOLLOW))
                    err(1, "Cannot stat `%s`", files[i]->srcName);
                
                if (files[i]->size == uninitFile.size)
                    files[i]->size = buf.st_size;

                if (files[i]->srcSize == uninitFile.srcSize)
                    files[i]->srcSize = buf.st_size;
                        
                if (files[i]->mode == uninitFile.mode)
                    files[i]->mode = buf.st_mode;
                        
                if (files[i]->mtime == uninitFile.mtime)
                    files[i]->mtime = buf.st_mtime;
                        
                if (files[i]->atime == uninitFile.atime)
                    files[i]->atime = buf.st_atime;
                        
            } else {
                if (files[i]->mode == uninitFile.mode)
                    files[i]->mode = S_IFREG | 0600;
            }
            if (files[i]->nlink == uninitFile.nlink)
                files[i]->nlink = 1;

#if 0
            printf("size=%20lu srcName=%12s srcSize=%8ld mode=%03o mtime=%ld\n",
                   files[i]->size,
                   files[i]->srcName,
                   files[i]->srcSize,
                   files[i]->mode,
                   files[i]->mtime
                   );
#endif
        }
    }
    
    /* END MY CODE */




    se = fuse_session_new(&args, &ops, sizeof(ops), NULL);

    if (se == NULL)
        goto err_out1;

    if (fuse_set_signal_handlers(se) != 0)
        goto err_out2;

    if (fuse_session_mount(se, opts.mountpoint) != 0)
        goto err_out3;

    //    fuse_daemonize(opts.foreground);

    /* Block until ctrl+c or fusermount -u */
    if (opts.singlethread)
        ret = fuse_session_loop(se);
    else
        ret = fuse_session_loop_mt(se, opts.clone_fd);

    fuse_session_unmount(se);
 err_out3:
    fuse_remove_signal_handlers(se);
 err_out2:
    fuse_session_destroy(se);
 err_out1:
    free(opts.mountpoint);
    fuse_opt_free_args(&args);

    return ret ? 1 : 0;
}

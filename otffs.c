#define FUSE_USE_VERSION 31
#define _GNU_SOURCE // reallocarray

#include "fmap.h"
#include "common.h"
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
#include <time.h>
#include <assert.h>

#define mmap DO_NOT_USE
#define munmap DO_NOT_USE

#define MAX_NAME_LENGTH 128
#define DEFAULT_TIMEOUT 5.0


struct parseResult pr;

struct timespec startupTime; // time of starting `otffs`

static int rootFh = -1; // handle of pre-mount mount point
static int logFh = -1; // handle of log file, if open

static const char *hello =
    "otffs: This is On The Fly File System version " VERSION ".\n";

#define log(fmt, ...) do {                                       \
        if (logFh >= 0) dprintf(logFh, "oftts: " fmt "\n", __VA_ARGS__);   \
    } while (0)


static int otf_stat(struct stat *buf, fuse_ino_t ino) {

    struct file *fp = AT(pr.files, ino);

    if (! fp)
        return -EBADF;

    ERRIF(fp->size > LONG_MAX); // FIXME: max of off_t?

    zero(buf);
    *buf = (struct stat){
        .st_ino = ino,
        .st_mode = fp->mode,
        .st_nlink = fp->nlink,
        .st_size = (off_t)fp->size,
        .st_atime = fp->atime,
        .st_mtime = fp->mtime,
        .st_ctime = fp->ctime,
        .st_uid = getuid(),
        .st_gid = getgid(),
        .st_blksize = 1 << 10,
        .st_blocks = (blkcnt_t)(fp->size - 1) / 512 + 1, // unchecked
    };

    return 0;
}


static void otf_getattr(fuse_req_t req, fuse_ino_t ino,
                    struct fuse_file_info *fi) {
    (void) fi;

    struct stat buf;
    if (otf_stat(&buf, ino)) {
        log("getattr(%ld) = ENOENT", ino);
        ERRIF(fuse_reply_err(req, ENOENT));
    } else {
        log("getattr(%ld) = { .st_size=%zu, ...}", ino, buf.st_size);
        ERRIF(fuse_reply_attr(req, &buf, DEFAULT_TIMEOUT));
    }
}



struct addFun_ctx {
    char *p;
    size_t s;
    fuse_req_t r;
};

// FIXME: better implement a “cursor” in the avl tree
static int otf_addFun(char *name, ino_t ino, struct addFun_ctx *ptr) {
    ERRIF(! AT(pr.files, ino));

    size_t oldsize = ptr->s;
    ptr->s += fuse_add_direntry(ptr->r, NULL, 0, name, NULL, 0);
    ptr->p = (char *)realloc(ptr->p, ptr->s);
    ERRIF(! ptr->p);

    struct stat buf;
    otf_stat(&buf, ino);
    fuse_add_direntry(ptr->r, ptr->p + oldsize, ptr->s - oldsize,
                      name, &buf, (off_t)ptr->s);

    return 0;
}

static void otf_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                        off_t off, struct fuse_file_info *fi) {
    (void)fi;

    if (ino != FUSE_ROOT_ID) {
        log("readdir(%ld) = ENOTDIR", ino);
        ERRIF(fuse_reply_err(req, ENOTDIR));
        return;
    }

    if ((size_t)off >= size) {
        log("readdir(%ld) returns 0 bytes", ino);
        ERRIF(fuse_reply_buf(req, NULL, 0));
        return;
    }

    struct addFun_ctx buf = {
        .p = NULL,
        .s = 0,
        .r = req,
    };

    ERRIF(avl_traverse(pr.names, (avl_VisitorFun)otf_addFun, &buf));

    size_t ret = min(buf.s - (size_t)off, size);
    log("readdir(%ld) returns %zu bytes", ino, ret);
    ERRIF(fuse_reply_buf(req, buf.p + off, ret));

    free(buf.p);
}



static void otf_lookup(fuse_req_t req, fuse_ino_t parent,
                       const char *name) {

    struct file *pp = AT(pr.files, parent);

    if (! (pp && S_ISDIR(pp->mode))) {
        log("lookup(%ld, %s) = ENOTDIR", parent, name);
        ERRIF(fuse_reply_err(req, ENOTDIR));
        return;
    }

    /* FIXME get list of files for this specific directory.  Currently
       there's only one dir, containing all the files. */

    ino_t ino;
    if (avl_lookup(pr.names, name, &ino)) {
        assert(AT(pr.files, ino));

        struct fuse_entry_param e;
        e = (struct fuse_entry_param){
            .ino = ino,
            .attr_timeout = DEFAULT_TIMEOUT,
            .entry_timeout = DEFAULT_TIMEOUT,
        };
        otf_stat(&e.attr, e.ino);
        log("lookup(%s) = { .ino = %ld, ... }", name, ino);
        ERRIF(fuse_reply_entry(req, &e));
        return;
    }

    log("lookup(%s) = ENOENT", name);
    ERRIF(fuse_reply_err(req, ENOENT));
}



static void otf_open(fuse_req_t req, fuse_ino_t ino,
                       struct fuse_file_info *fi) {

    struct file *fp = AT(pr.files, ino);
    if (! fp) {
        log("open(%ld) = EBADF", ino);
        ERRIF(fuse_reply_err(req, EBADF));
        return;
    }

    if (! S_ISREG(fp->mode)) {
        log("open(%ld) = EISDIR (not regular file)", ino);
        ERRIF(fuse_reply_err(req, EISDIR)); // FIXME not entirely correct
        return;
    }

    struct timespec now;
    ERRIF(clock_gettime(CLOCK_REALTIME, &now));
    fp->atime = now.tv_sec;

    int fh;
    if (fp->srcName) {
        fh = openat(rootFh, fp->srcName, O_RDONLY);
        ERRIF(fh == -1);
    } else { // computed content
        fh = 0; // FIXME is this ok?
    }

    fi->fh = (unsigned long)fh;
    log("open(%ld) = { .fh = %ld, ... } ", ino, fi->fh);
    ERRIF(fuse_reply_open(req, fi));
}



static void otf_release(fuse_req_t req, fuse_ino_t ino,
                        struct fuse_file_info *fi) {

    (void)req;

    struct file *fp = AT(pr.files, ino);

    if (! fp) {
        log("release(%ld) = EBADF", ino);
        ERRIF(fuse_reply_err(req, EBADF));
        return;
    }

    if (fp->srcName)
        close((int)(fi->fh));
    log("release(%ld) = 0", ino);
    ERRIF(fuse_reply_err(req, 0));
}



static int otf_delFun(char *key, ino_t ino, ino_t *old) {
    *old = ino;
    free(key);
    return 0;
}

static void otf_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {

    (void)parent;

    ino_t ino;
    if (avl_deleteWith((avl_VisitorFun)otf_delFun, pr.names, name, &ino)) {
        struct file *fp = AT(pr.files, ino);
        assert(fp);
        free(fp);
        AT(pr.files, ino) = NULL;
        log("unlink(%s) = 0", name);
        ERRIF(fuse_reply_err(req, 0));
        return;
    }

    log("unlink(%s) = ENOENT", name);
    ERRIF(fuse_reply_err(req, ENOENT));
}



static void otf_useFile(fuse_req_t req, int handle, struct file *fp,
                         size_t off, size_t amount) {

    /* Produce a file that is a repetition of the source file. */
    const size_t
        b = (size_t)fp->srcSize, // "block": all file content
        k = off / b, // "block" to start reading in
        l = (off + amount) / b, // "block" to end reading in
        s = off % b, // where in "block" k to start reading
        e = (s + amount) % b, // where in "block" l to end reading
        c = l-k+1;  // total number of "block"s req'd

    if (c > IOV_MAX)
        errx(1, "The source `%s` is too short (%zuB).  Request of %zuB"
             " needs IOVEC of size %zu, system limit is %d!",
             fp->srcName, b, amount, c, IOV_MAX);

    if (k == l) { // start and end in same block

        assert(s < e);
        assert(e < b);
        assert(amount == e - s);

        struct mapping m;
        fmap_map(&m, handle, (size_t)s, (size_t)(e-s));
        ERRIF(fuse_reply_buf(req, m.buf, (size_t)amount));
        fmap_unmap(&m);

    } else { // start and end in different blocks

        struct mapping m;
        fmap_map(&m, handle, 0, (size_t)b);

        struct iovec *vector = calloc((size_t)c, sizeof(struct iovec));
        ERRIF(! vector);

        vector[0] = (struct iovec){
            .iov_base = m.buf + s,
            .iov_len = (size_t)(b - s)
        };
        for (size_t i = 1; i < (size_t)(c-1); i++)
            vector[i] = (struct iovec){
                .iov_base = m.buf,
                .iov_len = (size_t)b
            };
        vector[c-1] = (struct iovec){
            .iov_base = m.buf,
            .iov_len = (size_t)e
        };

        ERRIF(fuse_reply_iov(req, vector, (int)(c)));

        fmap_unmap(&m);
        free(vector);

    }
}

static void otf_useIntegers(fuse_req_t req, size_t off, size_t amount) {
    size_t
        s = sizeof(unsigned int), // size of one item
        d = off % s, // delta between offset and item boundary
        c = (amount + s - 1) / s, // number of items needed in memory
        z = (size_t)off / s; // first item to put in memory

    unsigned int *buf = malloc(c * s);
    ERRIF(! buf);

    for (size_t i = 0; i < c; i++)
        buf[i] = (unsigned int)(z + i);

    ERRIF(fuse_reply_buf(req, (char *)buf + d, amount));

    free(buf);
}

static void otf_useChars(fuse_req_t req, size_t off, size_t amount) {
    size_t
        s = sizeof(unsigned char), // size of one item
        d = off % s, // delta between offset and item boundary
        c = (amount + s - 1) / s, // number of items needed in memory
        z = (size_t)off / s; // first item to put in memory

    unsigned char *buf = malloc(c * s);
    ERRIF(! buf);

    for (size_t i = 0; i < c; i++)
        buf[i] = (unsigned char)(z + i);

    ERRIF(fuse_reply_buf(req, (char *)buf + d, amount));

    free(buf);
}

static void otf_read(fuse_req_t req, fuse_ino_t ino, size_t len, off_t _off,
                     struct fuse_file_info *fi) {

    ERRIF(_off < 0);
    size_t off = (size_t)_off;

    struct file *fp = AT(pr.files, ino);

    if (! fp) {
        log("read(%ld, %zu, %zu) = EBADF", ino, off, len);
        ERRIF(fuse_reply_err(req, EBADF));
        return;
    }

    if (len == 0 || off >= (size_t)fp->size) {
        log("read(%ld, %zu, %zu) returns 0 bytes", ino, off, len);
        ERRIF(fuse_reply_buf(req, NULL, 0));
        return;
    }

    size_t amount = min(len, (size_t)fp->size - off);

    log("read(%ld, %zu, %zu) returns %zu bytes", ino, off, len, amount);
    if (fp->srcName)
        otf_useFile(req, (int)(fi->fh), fp, off, amount);
    else if (fp->srcSize == algoIntegers)
        otf_useIntegers(req, off, amount);
    else if (fp->srcSize == algoChars)
        otf_useChars(req, off, amount);
    else
        assert(0);
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



static int otf_gatherFun(char *name, ino_t ino, void *foo) {
    (void)foo; (void)name;
    struct file *fp =AT(pr.files, ino);

    ERRIF(! fp);

    if (fp->srcName) {
        struct stat buf;
        if (fstatat(rootFh, fp->srcName, &buf,
                    AT_SYMLINK_NOFOLLOW))
            err(1, "Cannot stat `%s`", fp->srcName);

        if ((buf.st_mode & S_IFMT) != S_IFREG)
            errx(1, "Refusing to use non-regular file `%s` as source for `%s`.",
                fp->srcName, name);

        if (fp->srcSize == uninitFile.srcSize) {
            ERRIF(buf.st_size < 0);
            fp->srcSize = (ssize_t)buf.st_size;
        }

        if (fp->size < 0)
            fp->size = -(fp->size * fp->srcSize);

        if (fp->mode == uninitFile.mode)
            fp->mode = buf.st_mode;

        if (fp->mtime == uninitFile.mtime)
            fp->mtime = buf.st_mtime;

        if (fp->atime == uninitFile.atime)
            fp->atime = buf.st_atime;

    } else {
        if (fp->size < 0) {
            switch (fp->srcSize) {
            case 0: // root directory
                break;
            case 1: // fill integers
                fp->size = (ssize_t)((size_t)(-fp->size) *
                                     sizeof(unsigned int) * (UINT_MAX + 1L));
                break;
            case 2: // fill chars
                fp->size = (ssize_t)((size_t)(-fp->size) *
                                     sizeof(unsigned char) * (UCHAR_MAX + 1L));
                break;
            default:
                assert(0);
                break;
            }
        }

        if (fp->mode == uninitFile.mode)
            fp->mode = S_IFREG | 0600;

        if (fp->mtime == uninitFile.mtime)
            fp->mtime = startupTime.tv_sec;

        if (fp->atime == uninitFile.atime)
            fp->atime = startupTime.tv_sec;
    }

    if (fp->nlink == uninitFile.nlink)
        fp->nlink = 1;

    if (fp->ctime == uninitFile.ctime)
        fp->ctime = startupTime.tv_sec;

#if 1 //eJILSvajWpL4

    struct tm tmBuf;
    ERRIF(! localtime_r(&fp->mtime, &tmBuf));

    const char *fmt[] = {
        "%b %d %H:%M",
        " %Y %b %d"
    };
    char dateBuf[128];
    ERRIF(! strftime(dateBuf, 128, fmt[
        startupTime.tv_sec - fp->mtime > 365 * 24 * 60 * 60
    ], &tmBuf));

    if (fp->srcName)
        log("added %05o %s %s %luB (file:%s %ldB)",
              fp->mode & 077777,
              dateBuf,
              name,
              fp->size,
              fp->srcName,
              fp->srcSize);
    else
        log("added %05o %s %s %luB (otffs:%s)",
              fp->mode & 077777,
              dateBuf,
              name,
              fp->size,
              algorithms[fp->srcSize]);
#endif //eJILSvajWpL4

    return 0;
}


int main(int argc, char *argv[]) {

#if 1 //J2RTUVx5yO5P
    //logFh = open("/proc/self/otffs.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    logFh = 2;
    ERRIF(logFh < 0);
#endif //J2RTUVx5yO5P

    dprintf(logFh, hello);

    /* BEGIN Code copied from libfuse docs */
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
    /* END Code copied from libfuse docs */

    ERRIF(clock_gettime(CLOCK_REALTIME, &startupTime));

    if (! opts.mountpoint)
        errx(1, "No mountpoint given on command line.  See the accom"
             "panying README for documentation.");

    rootFh = open(opts.mountpoint, O_RDONLY|O_DIRECTORY);
    if (rootFh < 0)
        err(1, "Failed to open mountpoint directory: %s", opts.mountpoint);

    pr.names = avl_new((avl_CmpFun)strcmp);
    ALLOCATE(pr.files, min(FUSE_ROOT_ID + 1, 8));

    { // add root directory
        char *name = strdup(".");
        ERRIF(! name);
        assert(! avl_insert(pr.names, name, FUSE_ROOT_ID, NULL));

        struct file *buf = new(struct file);
        *buf = uninitFile;
        buf->size = 0;
        buf->srcSize = 0;
        buf->mode = S_IFDIR | 0755;
        buf->nlink = 2;
        buf->atime = startupTime.tv_sec;
        buf->mtime = startupTime.tv_sec;
        buf->ctime = startupTime.tv_sec;

        pr.files.array[FUSE_ROOT_ID] = buf;
        pr.files.used = FUSE_ROOT_ID + 1;
    }

    {
        int fh = openat(rootFh, "otffsrc", O_RDONLY);
        if (fh < 0)
            err(1, "Failed to open config file: %s/otffsrc",
                opts.mountpoint);
        parse(&pr, fh);
        close(fh);
    }

    /* For all files in the config, gather missing information from
       the filesystem, or elsewhere. */
    ERRIF(avl_traverse(pr.names, (avl_VisitorFun)otf_gatherFun, NULL));

    /* BEGIN Code copied from libfuse docs */
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
    /* END Code copied from libfuse docs */
}

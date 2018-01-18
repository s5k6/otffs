#define FUSE_USE_VERSION 31
#define _GNU_SOURCE // reallocarray

#include "common.h"
#include "fmap.h"
#include "parser.h"
#include <assert.h>
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
#include <time.h>
#include <unistd.h>

#define mmap DO_NOT_USE
#define munmap DO_NOT_USE

#define MAX_NAME_LENGTH 128
#define DEFAULT_TIMEOUT 5.0


struct fileSystem fs; // all data of the file system

struct timespec startupTime; // time of starting `otffs`

static int rootFh = -1; // handle of pre-mount mount point
static int logFh = -1; // handle of log file, if open

// logging to logFh
#define log(fmt, ...) do {                                              \
        if (logFh >= 0) dprintf(logFh, "otffs: " fmt "\n", __VA_ARGS__); \
    } while (0)



/* These functions must not fail, but are used quite frequently */

#define fuse_reply_err(...) do {                                        \
        if (fuse_reply_err(__VA_ARGS__))                                \
            err(1, "Fatal fuse_reply_err at " __FILE__ ":%d", __LINE__); \
    } while (0)

#define fuse_reply_buf(...) do {                                        \
        if (fuse_reply_buf(__VA_ARGS__))                                \
            err(1, "Fatal fuse_reply_buf at " __FILE__ ":%d", __LINE__); \
    } while (0)



/* Fill `buf` with the data from inode `ino`.  Some values are
   hard-coded here.  Used by FUSE API and private functions. */

static int otf_stat(struct stat *buf, fuse_ino_t ino) {

    struct file *fp = AT(fs.files, ino);

    if (! fp)
        return -EBADF;

    // FIXME: would be nicer to have `_MAX` constants.
    assert((off_t)fp->size == fp->size);
    assert((blkcnt_t)(fp->size - 1) / 512 + 1 == (fp->size - 1) / 512 + 1);

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
        .st_blksize = 1 << 10, // FIXME: why?
        .st_blocks = (blkcnt_t)(fp->size - 1) / 512 + 1,
    };

    return 0;
}



/* FUSE calls this function to request `struct stat` information. */

static void otf_getattr(fuse_req_t req, fuse_ino_t ino,
                    struct fuse_file_info *fi) {
    (void) fi;

    struct stat buf;
    if (otf_stat(&buf, ino)) {
        log("getattr(%ld) = ENOENT", ino);
        fuse_reply_err(req, ENOENT);
    } else {
        log("getattr(%ld) = { .st_size=%zu, ...}", ino, buf.st_size);
        ERRIF(fuse_reply_attr(req, &buf, DEFAULT_TIMEOUT));
    }
}



/* FUSE uses this function to Look up a directory entry by name and
   get its attributes. */

static void otf_lookup(fuse_req_t req, fuse_ino_t parent,
                       const char *name) {

    struct file *pp = AT(fs.files, parent);

    if (! (pp && S_ISDIR(pp->mode))) {
        log("lookup(%ld, %s) = ENOTDIR", parent, name);
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    /* FIXME get list of files for this specific directory.  Currently
       there's only one dir, containing all the files. */

    ino_t ino;
    if (avl_lookup(fs.names, name, &ino)) {
        assert(AT(fs.files, ino));

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
    fuse_reply_err(req, ENOENT);
}



/* FUSE uses this function to open a file. */

static void otf_open(fuse_req_t req, fuse_ino_t ino,
                     struct fuse_file_info *fi) {

    struct file *fp = AT(fs.files, ino);
    if (! fp) {
        log("open(%ld) = EBADF", ino);
        fuse_reply_err(req, EBADF);
        return;
    }

    /* otffs only serves regular files */
    if (! S_ISREG(fp->mode)) {
        log("open(%ld) = EISDIR (not regular file)", ino);
        fuse_reply_err(req, EISDIR); // FIXME not entirely correct
        return;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now))
        fp->atime = 0;
    else
        fp->atime = now.tv_sec;

    int fh;
    if (fp->srcName) { // file is backed by real file
        fh = openat(rootFh, fp->srcName, O_RDONLY);
        if (fh == -1) {
            fuse_reply_err(req, errno);
            return;
        };
    } else { // computed content
        fh = 0; // FIXME is this a goo idea?
    }

    /* Return handle of open file for later use.  See `otf_read`. */
    fi->fh = (unsigned long)fh;

    log("open(%ld) = { .fh = %ld, ... } ", ino, fi->fh);
    ERRIF(fuse_reply_open(req, fi));
}



/* Used by `otf_read` to implement `pass <realfile>` */

static void otf_useFile(fuse_req_t req, int handle, struct file *fp,
                         size_t off, size_t amount) {

    /* Produce a file that is a repetition of the source file. */
    const size_t
        b = (size_t)fp->srcSize, // "block": all src file content
        k = off / b, // the "block" to start reading in
        l = (off + amount) / b, // the "block" to end reading in
        s = off % b, // where in "block" k to start reading
        e = (off + amount) % b, // how much to read from "block" l, maybe 0
        c = l-k+1;  // total number of "block"s req'd

    /* Note that we cannot reply with a partial response of `IOV_MAX *
       b` bytes size (or similar), since “otherwise the rest of the
       data will be substituted with zeroes” [1]. */
    if (c > IOV_MAX)
        errx(1, "The source `%s` is too short (%zuB).  Request of %zuB"
             " needs IOVEC of size %zu, system limit is %d!",
             fp->srcName, b, amount, c, IOV_MAX);

    if (k == l) { // start and end in same block

        assert(s < e);
        assert(e < b);
        assert(amount == e - s);

        /* Only map req'd region into memory.  Send reply, and
           unmap. */
        struct mapping m;
        fmap_map(&m, handle, (size_t)s, (size_t)(e-s));
        fuse_reply_buf(req, m.buf, (size_t)amount);
        fmap_unmap(&m);

    } else { // start and end in different blocks

        assert(k < l);
        assert(amount == b - s + (c - 2) * b + e);

        /* Map the whole file into memory. */
        struct mapping m;
        fmap_map(&m, handle, 0, (size_t)b);

        /* Create an `iovec` that contains a pointer for each of the
           `c` many blocks identified above. */
        struct iovec *vector = calloc((size_t)c, sizeof(struct iovec));
        ERRIF(! vector);

        vector[0] = (struct iovec){ // first block
            .iov_base = m.buf + s,
            .iov_len = (size_t)(b - s)
        };
        for (size_t i = 1; i < (size_t)(c-1); i++) // intermediate blocks
            vector[i] = (struct iovec){
                .iov_base = m.buf,
                .iov_len = (size_t)b
            };
        vector[c-1] = (struct iovec){ // last block
            .iov_base = m.buf,
            .iov_len = (size_t)e
        };

        /* send reply */
        ERRIF(fuse_reply_iov(req, vector, (int)(c)));

        /* unmap file, free `iovec` */
        fmap_unmap(&m);
        free(vector);
    }
}


/* Used by `otf_read` to implement `fill integers` */

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

    fuse_reply_buf(req, (char *)buf + d, amount);

    free(buf);
}


/* Used by `otf_read` to implement `fill chars` */

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

    fuse_reply_buf(req, (char *)buf + d, amount);

    free(buf);
}


/* FUSE uses this function to read data from a file. */

static void otf_read(fuse_req_t req, fuse_ino_t ino, size_t len, off_t _off,
                     struct fuse_file_info *fi) {

    if (_off < 0) {
        fuse_reply_err(req, EINVAL);
        return;
    }
    size_t off = (size_t)_off;

    struct file *fp = AT(fs.files, ino);

    if (! fp) {
        log("read(%ld, %zu, %zu) = EBADF", ino, off, len);
        fuse_reply_err(req, EBADF);
        return;
    }

    if (len == 0 || off >= (size_t)fp->size) {
        log("read(%ld, %zu, %zu) returns 0 bytes", ino, off, len);
        fuse_reply_buf(req, NULL, 0);
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



/* Used by `otf_readdir`.  Accumulates file metadata in FS
   traversal. */

struct addFun_ctx {
    char *p;
    size_t s;
    fuse_req_t r;
};

/* Used by `otf_readdir`.  Called once for every file in the FS, adds
   its metadata to the response to be sent back to FUSE. */

static int otf_addFun(char *name, ino_t ino, struct addFun_ctx *ptr) {
    assert(AT(fs.files, ino));

    /* First calculate req'd amount of space... */
    size_t oldsize = ptr->s;
    ptr->s += fuse_add_direntry(ptr->r, NULL, 0, name, NULL, 0);
    ptr->p = (char *)realloc(ptr->p, ptr->s);
    ERRIF(! ptr->p);

    /* ...then really fill the buffer.  Construction as in FUSE
       example code. */
    struct stat buf;
    otf_stat(&buf, ino);
    fuse_add_direntry(ptr->r, ptr->p + oldsize, ptr->s - oldsize,
                      name, &buf, (off_t)ptr->s);

    return 0;
}

/* FUSE uses this function to read a directory. */

static void otf_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                        off_t off, struct fuse_file_info *fi) {
    (void)fi;

    /* otffs provides only one directory: root. */
    if (ino != FUSE_ROOT_ID) {
        log("readdir(%ld) = ENOTDIR", ino);
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    if ((size_t)off >= size) {
        log("readdir(%ld) returns 0 bytes", ino);
        fuse_reply_buf(req, NULL, 0);
        return;
    }

    struct addFun_ctx buf = {
        .p = NULL,
        .s = 0,
        .r = req,
    };

    // FIXME: better implement a “cursor” in the avl tree
    avl_traverse(fs.names, (avl_VisitorFun)otf_addFun, &buf);

    size_t ret = min(buf.s - (size_t)off, size);
    log("readdir(%ld) returns %zu bytes", ino, ret);
    fuse_reply_buf(req, buf.p + off, ret);

    free(buf.p);
}



/* FUSE uses this function to release an open file.  For every open
   call there will be exactly one release call. */

static void otf_release(fuse_req_t req, fuse_ino_t ino,
                        struct fuse_file_info *fi) {

    (void)req;

    struct file *fp = AT(fs.files, ino);

    if (! fp) {
        log("release(%ld) = EBADF", ino);
        fuse_reply_err(req, EBADF);
        return;
    }

    /* If backed by real file, release that. */
    if (fp->srcName)
        close((int)(fi->fh));

    log("release(%ld) = 0", ino);
    fuse_reply_err(req, 0);
}



/* Used by `otf_unlink` to remove a file from the file name index. */

static int otf_delFun(char *key, ino_t ino, ino_t *old) {
    assert(AT(fs.files, ino));

    *old = ino;
    free(key);
    return 0;
}

/* Used by FUSE to remove a file. */

static void otf_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {

    (void)parent;

    ino_t ino;
    if (avl_deleteWith((avl_VisitorFun)otf_delFun, fs.names, name, &ino)) {
        struct file *fp = AT(fs.files, ino);
        assert(fp);
        free(fp);
        AT(fs.files, ino) = NULL;
        log("unlink(%s) = 0", name);
        fuse_reply_err(req, 0);
        return;
    }

    log("unlink(%s) = ENOENT", name);
    fuse_reply_err(req, ENOENT);
}



/* Tell FUSE which functions are implemented.  All of them must be
   defined above. */

static struct fuse_lowlevel_ops ops = {
    .getattr = otf_getattr,
    .lookup = otf_lookup,
    .open = otf_open,
    .read = otf_read,
    .readdir = otf_readdir,
    .release = otf_release,
    .unlink = otf_unlink,
};



/* Called once for every file in the FS to fill in metadata not
   specified by the user, either from FS for file-backed files, or
   from algo specifics otherwise. */

static int otf_gatherFun(char *name, ino_t ino, void *foo) {
    (void)foo; (void)name;

    struct file *fp = AT(fs.files, ino);
    assert(fp);

    if (fp->srcName) {
        struct stat buf;
        if (fstatat(rootFh, fp->srcName, &buf,
                    AT_SYMLINK_NOFOLLOW))
            err(1, "Cannot stat `%s`", fp->srcName);

        if ((buf.st_mode & S_IFMT) != S_IFREG)
            errx(1, "Refusing to use non-regular file `%s` as source for `%s`.",
                fp->srcName, name);

        if (fp->srcSize == uninitFile.srcSize) {
            assert(buf.st_size >= 0);
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

#ifdef DEBUG //eJILSvajWpL4

    /* Show a listing of all specified files. */

    struct tm tmBuf;
    localtime_r(&fp->mtime, &tmBuf);

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



/* Main function.  Really could do with some cleanup. */

int main(int argc, char *argv[]) {

    /* Open log file, or use stderr */
#if 0 //J2RTUVx5yO5P
    logFh = open("/proc/self/otffs.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (logFh < 0)
        err(1, "Failed to open log file /proc/self/otffs.log");
#else //J2RTUVx5yO5P
    logFh = 2;
#endif //J2RTUVx5yO5P

    /* say hello */

    printf(
           "\n"
           "This is  On The Fly File System  version " VERSION ".\n"
           "Copyright 2018 Stefan Klinger <http://stefan-klinger.de>.\n"
#ifdef DEBUG
           "%s", "COMPILED IN DEBUG MODE\n"
#endif
           "\n"
           );
    
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

    if (clock_gettime(CLOCK_REALTIME, &startupTime))
        startupTime = (struct timespec){ 0, 0 };

    if (! opts.mountpoint)
        errx(1, "No mountpoint given on command line.  See the accom"
             "panying README for documentation.");

    rootFh = open(opts.mountpoint, O_RDONLY|O_DIRECTORY);
    if (rootFh < 0)
        err(1, "Failed to open mountpoint directory: %s", opts.mountpoint);


    /* Prepare the global file system structure `fs`. */

    /* AVL tree for looking up inode numbers by file name. */
    fs.names = avl_new((avl_CmpFun)strcmp);

    /* Inode to file mapping: A array. */
    ALLOCATE(fs.files, min(FUSE_ROOT_ID + 1, 8));

    { // Add root directory to filesystem
        char *name = strdup(".");
        ERRIF(! name);
        ERRIF(avl_insert(fs.names, name, FUSE_ROOT_ID, NULL));

        struct file *buf = new(struct file);
        *buf = uninitFile;
        buf->size = 0;
        buf->srcSize = 0;
        buf->mode = S_IFDIR | 0755;
        buf->nlink = 2;
        buf->atime = startupTime.tv_sec;
        buf->mtime = startupTime.tv_sec;
        buf->ctime = startupTime.tv_sec;

        fs.files.array[FUSE_ROOT_ID] = buf;
        fs.files.used = FUSE_ROOT_ID + 1;
    }

    { /* Add more files from user config. */
        int fh = openat(rootFh, "otffsrc", O_RDONLY);
        if (fh < 0)
            err(1, "Failed to open config file: %s/otffsrc",
                opts.mountpoint);
        parse(&fs, fh);
        close(fh);
    }

    /* For all files in the config, gather missing information from
       the filesystem. */
    avl_traverse(fs.names, (avl_VisitorFun)otf_gatherFun, NULL);

    log("Serving %ld files...", avl_size(fs.names));

    /* BEGIN Code copied from libfuse docs */
    se = fuse_session_new(&args, &ops, sizeof(ops), NULL);

    if (se == NULL)
        goto err_out1;

    if (fuse_set_signal_handlers(se) != 0)
        goto err_out2;

    if (fuse_session_mount(se, opts.mountpoint) != 0)
        goto err_out3;

    //    fuse_daemonize(opts.foreground);

    /* Block until ctrl+c or fusermount3 -u */
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

/*
  ____________________
  [1] https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html#ab7b740dccdc6ddc388cdcd7897e4c2e3
  [2] https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
 */

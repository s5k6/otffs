#define FUSE_USE_VERSION 31
#define _GNU_SOURCE // reallocarray

#include <err.h>
#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>

#define max(x,y) ((x) > (y) ? (x) : (y))

#define MAX_NAME_LENGTH 128
#define DEFAULT_TIMEOUT 1.0
#define VERBOSE 0

static void *_new(size_t size) {
    void *tmp = malloc(size);
    if (!tmp)
        err(1, "allcating %zu bytes", size);
    return tmp;
}
#define new(ty) _new(sizeof(ty))
#define zero(ptr) memset(ptr, 0, sizeof(*ptr))

struct file {
    const char *name;
    mode_t mode;
    nlink_t nlink;
    off_t size;
    time_t atime, mtime;
};

struct file **files = NULL;
unsigned int files_count = 0;
unsigned int files_alloc = 0;



static int otf_stat(struct stat *buf, fuse_ino_t ino) {

    if (!files[ino])
        return ENOENT;
    
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
        printf("getattr(ino=%ld)\n", ino);

    struct stat buf;
    if (otf_stat(&buf, ino))
        fuse_reply_err(req, ENOENT);
    else
        fuse_reply_attr(req, &buf, DEFAULT_TIMEOUT);
}



#define min(x, y) ((x) < (y) ? (x) : (y))

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
        printf("readdir(ino=%ld)\n", ino);

    if (ino != 1)
        fuse_reply_err(req, ENOTDIR);
    else {
        char *p = NULL;
        size_t s = 0;
        
        for (unsigned int i = 0; i < files_count; i++) {
            if (!files[i])
                continue;

            struct stat buf;
            size_t oldsize = s;
            s += fuse_add_direntry(req, NULL, 0, files[i]->name, NULL, 0);
            p = (char *)realloc(p, s);
            if (!p)
                err(1, "93javfPGcul2");
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
        printf("lookup(parent=%ld, name=%s)\n", parent, name);

    if (!files)
        err(1, "qATg3A0anTGw");

    if (!files[parent]) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    
    if (!S_ISDIR(files[parent]->mode)) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    /* FIXME check permissions?  Should be done by kernel, see `-o
       default_permissions` */

    /* FIXME get list of files for this specific directory.  Currently
       there's only one dir, containing all the files. */
   
    for (unsigned int i = 0; i < files_count; i++) {
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
            fuse_reply_entry(req, &e);
            return;
        }
    }
    fuse_reply_err(req, ENOENT);
}



static void otf_open(fuse_req_t req, fuse_ino_t ino,
                       struct fuse_file_info *fi) {

    (void)fi;
    
    if (VERBOSE)
        printf("open(ino=%ld)\n", ino);

    if (!files)
        err(1, "wPzZilxiVUPO");

    if (!files[ino]) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (!S_ISREG(files[ino]->mode)) {
        fuse_reply_err(req, EISDIR); // FIXME not entirely correct
        return;
    }

    /* FIXME check permissions?  Should be done by kernel, see `-o
       default_permissions` */

    /* maybe set aside resources for this?  multithreaded? */

    fuse_reply_open(req, fi);
}



static void otf_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                      struct fuse_file_info *fi) {
    (void)fi;

    if (VERBOSE)
        printf("read(ino=%ld, size=%zu, off=%zu)\n", ino, size, off);

    if (!files)
        err(1, "K4Zaduz01Box");

    if (!files[ino]) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (off > files[ino]->size) {
        fuse_reply_buf(req, NULL, 0);
        return;
    }

    size_t amount = min(size, (size_t)(files[ino]->size - off));

    char *buf = malloc(amount);
    if (!buf)
        err(1, "uEZgAiOE6fK3");
        
    for (unsigned int i = 0; i < amount; i++)
        buf[i] = (char)((off + i) % 0xff);

    fuse_reply_buf(req, buf, amount);
        
    free(buf);
}


static struct fuse_lowlevel_ops ops = {
    .lookup = otf_lookup,
    .getattr = otf_getattr,
    .readdir = otf_readdir,
    .open = otf_open,
    .read = otf_read,
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

    struct timeval now;
    if (gettimeofday(&now, NULL))
        err(1, "kSIfOmBSAqQ8");

    files_alloc = 10;
    files = calloc(files_alloc, sizeof(*files));
    if (!files)
        err(1, "4SZFhtiLWMCR");
    
    struct file *buf = NULL;
    
    buf = new(struct file);
    zero(buf);
    *buf = (struct file){
        .name = ".",
        .mode = S_IFDIR | 0755,
        .nlink = 2,
        .atime = now.tv_sec,
        .mtime = now.tv_sec,
    };
    files[1] = buf; // Must be inode 1.  Why?

    files_count = 2;
    
    buf = new(struct file);
    zero(buf);
    *buf = (struct file){
        .name = "size_4kiB",
        .mode = S_IFREG | 0444,
        .nlink = 1,
        .size = 4 << 10,
        .atime = now.tv_sec,
        .mtime = now.tv_sec,
    };
    files[files_count++] = buf;

    buf = new(struct file);
    zero(buf);
    *buf = (struct file){
        .name = "size_4MiB",
        .mode = S_IFREG | 0444,
        .nlink = 1,
        .size = 4 << 20,
        .atime = now.tv_sec,
        .mtime = now.tv_sec,
    };
    files[files_count++] = buf;

    buf = new(struct file);
    zero(buf);
    *buf = (struct file){
        .name = "size_4GiB",
        .mode = S_IFREG | 0444,
        .nlink = 1,
        .size = 4UL << 30,
        .atime = now.tv_sec,
        .mtime = now.tv_sec,
    };
    files[files_count++] = buf;

    buf = new(struct file);
    zero(buf);
    *buf = (struct file){
        .name = "size_4TiB",
        .mode = S_IFREG | 0444,
        .nlink = 1,
        .size = 4UL << 40,
        .atime = now.tv_sec,
        .mtime = now.tv_sec,
    };
    files[files_count++] = buf;

    buf = new(struct file);
    zero(buf);
    *buf = (struct file){
        .name = "size_4PiB",
        .mode = S_IFREG | 0444,
        .nlink = 1,
        .size = 4UL << 50,
        .atime = now.tv_sec,
        .mtime = now.tv_sec,
    };
    files[files_count++] = buf;



    
    for (unsigned int i = 0; i < files_count; i++) {
        if (!files[i])
            continue;
        printf("%x: %03o %s\n", i, (files[i]->mode & 0777), files[i]->name);
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



#if 0
void add_file(size_t ino, struct stat *buf) {
    if (files_count >= files_alloc) {
        size_t old = files_alloc;
        files_alloc = max(2 * files_alloc, 10);
        files = reallocarray(files, files_alloc, sizeof(*files));
        if (!files)
            err(1, "gAEGxirdhlF2");
        for (size_t i = old; i < files_alloc; i++)
            files[i] = NULL;
    }

    files[files_count++] = buf;
}
#endif

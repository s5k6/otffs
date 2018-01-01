#define FUSE_USE_VERSION 31
#define _GNU_SOURCE // reallocarray

#include <dirent.h>
#include <err.h>
#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>

#define max(x,y) ((x) > (y) ? (x) : (y))

#define MAX_NAME_LENGTH 128
#define DEFAULT_TIMEOUT 1.0
#define VERBOSE 0

// this is not assert: not intended to be disabled in non-debug mode
#define ERRIF(c) do {                                             \
        if (c)                                                    \
            err(1, #c " at " __FILE__ ":%d because", __LINE__);   \
    } while (0)

static void *_new(size_t size) {
    void *tmp = malloc(size);
    if (!tmp)
        err(1, "allcating %zu bytes", size);
    return tmp;
}
#define new(ty) _new(sizeof(ty))
#define zero(ptr) memset(ptr, 0, sizeof(*ptr))

enum type { real, fake };

struct file {
    const char *name;
    enum type type;
    mode_t mode;
    nlink_t nlink;
    off_t size;
    time_t atime, mtime;
};

int rootFd = -1;

struct file **files = NULL;
unsigned int files_count = 0;
unsigned int files_alloc = 0;

struct timeval now; // time of starting `otffs`



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
        printf("lookup(parent=%ld, name=%s)\n", parent, name);

    ERRIF(!files);

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
    
    if (VERBOSE)
        printf("open(ino=%ld)\n", ino);

    ERRIF(!files);

    if (!files[ino]) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (!S_ISREG(files[ino]->mode)) {
        fuse_reply_err(req, EISDIR); // FIXME not entirely correct
        return;
    }

    fi->fh = 0;
    
    if (files[ino]->type == real) {
        int fd = openat(rootFd, files[ino]->name, O_RDONLY);
        ERRIF(fd == -1);
        fi->fh = (unsigned long)fd;
    }

    fuse_reply_open(req, fi);
}



void otf_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    
    if (VERBOSE)
        printf("release(ino=%ld)\n", ino);

    ERRIF(!files);

    if (!files[ino]) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (files[ino]->type == real) {
        close((int)(fi->fh));
    }

    
}



static void otf_fill_counting(char *buf, off_t off, size_t amount) {

    size_t chunk = sizeof(unsigned int);
    size_t steps = amount / chunk;
    size_t rem = amount % chunk;

    if (VERBOSE)
        printf("fill_counting(off=%ld, amount=%ld)\n", off, amount);

    unsigned int value = (unsigned int)off / (unsigned int)chunk;
    
    for (size_t i = 0; i < steps; i++) {
        memcpy(&buf[i * chunk], &value, chunk);
        value++;
    }
    if (rem) {
        memcpy(&buf[amount - rem], &value, rem);
    }
}



static void otf_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                      struct fuse_file_info *fi) {

    if (VERBOSE)
        printf("read(ino=%ld, size=%zu, off=%zu)\n", ino, size, off);

    ERRIF(!files);

    if (!files[ino]) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (off > files[ino]->size) {
        fuse_reply_buf(req, NULL, 0);
        return;
    }

    size_t amount = min(size, (size_t)(files[ino]->size - off));

    if (amount <=0) {
        fuse_reply_buf(req, NULL, 0);
        return;
    }

    if (files[ino]->type == real) {

        char *buf = mmap(NULL, amount, PROT_READ, MAP_SHARED,
                         (int)(fi->fh), off);
        ERRIF(buf == MAP_FAILED); 

        fuse_reply_buf(req, buf, amount);
        
        ERRIF(munmap(buf, amount));
        
        return;
    }

    char *buf = malloc(amount);
    ERRIF(!buf);

    otf_fill_counting(buf, off, amount);

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



void otf_addFile(const char * name, enum type type, off_t size, mode_t mode) {

    if (files_count >= files_alloc) {
        size_t old = files_alloc;
        files_alloc = max(2 * files_alloc, 10);
        files = reallocarray(files, files_alloc, sizeof(*files));
        ERRIF(!files);
        for (size_t i = old; i < files_alloc; i++)
            files[i] = NULL;
    }

    struct file *buf = new(struct file);    
    zero(buf);

    *buf = (struct file){
        .name = name,
        .type = type,
        .mode = S_IFREG | (mode & 0555),
        .nlink = 1,
        .size = size,
        .atime = now.tv_sec,
        .mtime = now.tv_sec,
    };
    files[files_count++] = buf;
}


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

    files_alloc = 10;
    files = calloc(files_alloc, sizeof(*files));
    ERRIF(!files);

    { // add root directory
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
        files[FUSE_ROOT_ID] = buf; // Must be inode 1.  Why?
        files_count = FUSE_ROOT_ID + 1;
    }



    { // add entries for real files
        DIR *root = opendir(opts.mountpoint);
        ERRIF(!root);

        rootFd = dup(dirfd(root));
        
        struct dirent *e;
        while ((e = readdir(root))) {
            if (e->d_type != DT_REG)
                continue;

            struct stat buf;
            ERRIF(fstatat(rootFd, e->d_name, &buf, AT_SYMLINK_NOFOLLOW));

            char *name = strndup(e->d_name, 127);
            ERRIF(!name);
            
            otf_addFile(name, real, buf.st_size, buf.st_mode);
            
        }
        closedir(root);
    }


    
    otf_addFile("data1", fake, 4UL << 10, 0444);
    otf_addFile("data2", fake, 4UL << 20, 0444);
    otf_addFile("data3", fake, 4UL << 30, 0444);
    otf_addFile("data4", fake, 4UL << 40, 0444);
    otf_addFile("data5", fake, 4UL << 50, 0444);
    
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

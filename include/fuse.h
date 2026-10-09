/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
 * Copyright (c) 2026 Raman Dzehtsiar (USFS modifications)
 *
 * This program can be distributed under the terms of the GNU LGPLv2.
 * See LICENSE.LGPL-2.1-only in this distribution (upstream LGPL2.txt).
 * Upstream's LICENSE and LGPL2.txt specify LGPL version 2.1.
 *
 * Modified for AIX USFS, 2026-07-26 through 2026-10-08: reduced API,
 * AIX extensions, and USFS-specific documentation. The declarations,
 * macros, and inline entry-point wrappers are adapted from libfuse.
 *
 * fuse.h - high-level API of the AIX USFS client library.
 *
 * API-compatible subset of libfuse 3.19's fuse.h. The goal is that
 * filesystems written against the libfuse high-level API (FUSE_USE_VERSION
 * 30..31, e.g. libfuse's high-level examples) compile unchanged against this
 * header and run on AIX over the USFS kernel extension. USFS uses its own
 * wire protocol (see src/common/usfs_proto.h); this header does not imply
 * binary compatibility with Linux libfuse.
 */

#ifndef FUSE_H_
#define FUSE_H_

#include "fuse_common.h"

#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- *
 * Basic FUSE API                                              *
 * ----------------------------------------------------------- */

struct fuse;

/* Callback paths follow libfuse: open-file unlink and rename replacement use
 * owned .fuse_hidden names unless hard_remove is set. nullpath_ok suppresses
 * paths for read, write, flush, fsync, release, readdir, fsyncdir and releasedir.
 * For getattr, chmod, chown, truncate and utimens, it omits paths only when fi
 * is supplied. Ordinary pathname operations retain their resolving path.
 * Both settings default to zero, are initialized from -o options before init,
 * and can be overridden by init. With nullpath_ok unset, named callbacks use
 * the current resolving path, including managed hidden names.
 * A detached object can also have a NULL path after hard_remove or rmdir.
 * Backends that support these cases must resolve the object through fi;
 * never substitute a removed object's former pathname. USFS retains handle
 * ownership through these callbacks. A backend may legitimately leave fh zero. */

enum fuse_readdir_flags
{
    FUSE_READDIR_DEFAULTS = 0,
    FUSE_READDIR_PLUS = (1 << 0)
};

enum fuse_fill_dir_flags
{
    FUSE_FILL_DIR_DEFAULTS = 0,
    FUSE_FILL_DIR_PLUS = (1 << 1)
};

/**
 * Function to add an entry in a readdir() operation. Returns 1 if the buffer
 * is full, zero otherwise.
 */
typedef int (*fuse_fill_dir_t) (void * buf, const char * name, const struct stat * stbuf, off_t off, enum fuse_fill_dir_flags flags);

/**
 * Configuration of the high-level API. Member list follows libfuse 3.19.
 * Most fields are accepted but not yet acted upon by this implementation.
 */
struct fuse_config
{
    int32_t set_gid;
    uint32_t gid;
    int32_t set_uid;
    uint32_t uid;
    int32_t set_mode;
    uint32_t umask;
    double entry_timeout;
    double negative_timeout;
    double attr_timeout;
    int32_t intr;
    int32_t intr_signal;
    int32_t remember;
    /* Delete immediately rather than retaining a hidden backend pathname. */
    int32_t hard_remove;
    int32_t use_ino;
    int32_t readdir_ino;
    int32_t direct_io;
    int32_t kernel_cache;
    int32_t auto_cache;
    int32_t ac_attr_timeout_set;
    double ac_attr_timeout;
    /* Omit paths in handle-based callbacks; metadata paths remain when fi is NULL. */
    int32_t nullpath_ok;
    int32_t show_help;
    char * modules;
    int32_t debug;
    uint32_t fmask;
    uint32_t dmask;
    int32_t no_rofd_flush;
    int32_t parallel_direct_writes;
    uint32_t flags;
    uint64_t reserved[48];
};

/* Only referenced through pointers; never defined on AIX. */
struct statx;
struct fuse_bufvec;
struct flock;

/*
 * The two values utimens uses for "the current time" and "leave this one
 * alone". AIX declares them in <sys/stat.h>, but only under
 * _XOPEN_SOURCE >= 700, which a file system is not obliged to compile with.
 * The values match that header.
 */
#ifndef UTIME_NOW
    #define UTIME_NOW (-2L)
#endif
#ifndef UTIME_OMIT
    #define UTIME_OMIT (-3L)
#endif

/**
 * The file system operations. Member list and order follow libfuse 3.19 so
 * that designated initializers written for libfuse compile unchanged.
 *
 * The USFS backend invokes: getattr (also for lookups), readlink, open,
 * opendir, read, write, flush, fsync, fsyncdir, release, releasedir, readdir,
 * create, mkdir, unlink, rmdir, rename, symlink, link, chmod, chown, truncate,
 * utimens, statfs, syncfs, init, destroy, export_id and resolve_id.
 *
 * Which of these a file system implements also decides whether its mount is
 * writable: a file system that provides none of the mutating operations is
 * mounted read-only, and the kernel then refuses writes without consulting it.
 *
 * The remaining members are accepted but not yet called; operations without
 * kernel support fail with ENOSYS on the AIX side. Extended attributes, ACLs
 * and memory mapping are out of scope.
 */
/* Initial attribute bits for the optional atomic USFS creation callback. */
#define FUSE_INITIAL_MODE  (1u << 0)
#define FUSE_INITIAL_UID   (1u << 1)
#define FUSE_INITIAL_GID   (1u << 2)
#define FUSE_INITIAL_SIZE  (1u << 3)
#define FUSE_INITIAL_ATIME (1u << 4)
#define FUSE_INITIAL_MTIME (1u << 5)
#define FUSE_INITIAL_CTIME (1u << 6)
enum fuse_create_activation
{
    FUSE_CREATE_DEFAULT = 0,
    FUSE_CREATE_LOOKUP = 1,
    FUSE_CREATE_OPEN = 2
};

struct fuse_operations
{
    int (*getattr) (const char *, struct stat *, struct fuse_file_info * fi);
    int (*readlink) (const char *, char *, size_t);
    int (*mknod) (const char *, mode_t, dev_t);
    int (*mkdir) (const char *, mode_t);
    int (*unlink) (const char *);
    int (*rmdir) (const char *);
    int (*symlink) (const char *, const char *);
    int (*rename) (const char *, const char *, unsigned int flags);
    int (*link) (const char *, const char *);
    int (*chmod) (const char *, mode_t, struct fuse_file_info * fi);
    int (*chown) (const char *, uid_t, gid_t, struct fuse_file_info * fi);
    int (*truncate) (const char *, off_t, struct fuse_file_info * fi);
    int (*open) (const char *, struct fuse_file_info *);
    int (*read) (const char *, char *, size_t, off_t, struct fuse_file_info *);
    int (*write) (const char *, const char *, size_t, off_t, struct fuse_file_info *);
    int (*statfs) (const char *, struct statvfs *);
    int (*flush) (const char *, struct fuse_file_info *);
    int (*release) (const char *, struct fuse_file_info *);
    int (*fsync) (const char *, int, struct fuse_file_info *);
    int (*setxattr) (const char *, const char *, const char *, size_t, int);
    int (*getxattr) (const char *, const char *, char *, size_t);
    int (*listxattr) (const char *, char *, size_t);
    int (*removexattr) (const char *, const char *);
    int (*opendir) (const char *, struct fuse_file_info *);
    int (*readdir) (const char *, void *, fuse_fill_dir_t, off_t, struct fuse_file_info *, enum fuse_readdir_flags);
    int (*releasedir) (const char *, struct fuse_file_info *);
    int (*fsyncdir) (const char *, int, struct fuse_file_info *);
    void * (*init) (struct fuse_conn_info * conn, struct fuse_config * cfg);
    void (*destroy) (void * private_data);
    int (*access) (const char *, int);
    int (*create) (const char *, mode_t, struct fuse_file_info *);
    int (*lock) (const char *, struct fuse_file_info *, int cmd, struct flock *);
    int (*utimens) (const char *, const struct timespec tv[2], struct fuse_file_info * fi);
    int (*bmap) (const char *, size_t blocksize, uint64_t * idx);
#if FUSE_USE_VERSION < 35
    int (*ioctl) (const char *, int cmd, void * arg, struct fuse_file_info *, unsigned int flags, void * data);
#else
    int (*ioctl) (const char *, unsigned int cmd, void * arg, struct fuse_file_info *, unsigned int flags, void * data);
#endif
    int (*poll) (const char *, struct fuse_file_info *, struct fuse_pollhandle * ph, unsigned * reventsp);
    int (*write_buf) (const char *, struct fuse_bufvec * buf, off_t off, struct fuse_file_info *);
    int (*read_buf) (const char *, struct fuse_bufvec ** bufp, size_t size, off_t off, struct fuse_file_info *);
    int (*flock) (const char *, struct fuse_file_info *, int op);
    int (*fallocate) (const char *, int, off_t, off_t, struct fuse_file_info *);
    ssize_t (*copy_file_range) (const char * path_in, struct fuse_file_info * fi_in, off_t offset_in, const char * path_out, struct fuse_file_info * fi_out, off_t offset_out, size_t size, int flags);
    off_t (*lseek) (const char *, off_t off, int whence, struct fuse_file_info *);
    int (*statx) (const char *, int flags, int mask, struct statx * stxbuf, struct fuse_file_info * fi);
    int (*syncfs) (const char * path);
    /* Atomic creation of a new regular file. Failure retains neither a new
	 * name nor a handle. Initial mode/uid/gid are normalized; valid selects
	 * explicitly requested fields. Return authoritative attributes. fi is
	 * non-NULL only for OPEN and receives the retained handle. DEFAULT and
	 * LOOKUP must not open the object. No fallible post-create emulation. */
    int (*create_attr) (const char * path, const struct stat * initial, unsigned int valid, enum fuse_create_activation activation, struct stat * result, struct fuse_file_info * fi);
    /* Implement both callbacks to enable AIX fid/vget. Callbacks return zero
     * on success or negative errno on failure. Tokens must be nonzero and
     * never reused for another object during this mount.
     * path_or_null is NULL when the client has lost its last known alias but
     * the backend still reports another link to the object. */
    int (*export_id) (const char * path_or_null, uint64_t backend_dev, uint64_t backend_ino, mode_t backend_type, uint64_t * token);
    /* Resolve a token to a current absolute path. Return -ESTALE when the
     * object has no link or this token was never issued by the backend. */
    int (*resolve_id) (uint64_t token, char * path, size_t path_capacity);
};

/**
 * Extra context that may be needed by some filesystems.
 */
struct fuse_context
{
    struct fuse * fuse;
    uid_t uid;
    gid_t gid;
    pid_t pid;
    void * private_data;
    mode_t umask;
};

/* ----------------------------------------------------------- *
 * fuse_main                                                   *
 * ----------------------------------------------------------- */

int fuse_main_real_versioned (int argc, char * argv[], const struct fuse_operations * op, size_t op_size, struct libfuse_version * version, void * user_data);

static inline int fuse_main_fn (int argc, char * argv[], const struct fuse_operations * op, void * user_data)
{
    struct libfuse_version version = {
        FUSE_MAJOR_VERSION,
        FUSE_MINOR_VERSION,
        FUSE_HOTFIX_VERSION,
        0
    };

    return fuse_main_real_versioned (argc, argv, op, sizeof (*op), &version, user_data);
}

#define fuse_main(argc, argv, op, user_data) \
    fuse_main_fn (argc, argv, op, user_data)

/* ----------------------------------------------------------- *
 * More detailed API                                           *
 * ----------------------------------------------------------- */

struct fuse * _fuse_new_31 (struct fuse_args * args, const struct fuse_operations * op, size_t op_size, struct libfuse_version * version, void * user_data);

static inline struct fuse * fuse_new_fn (struct fuse_args * args, const struct fuse_operations * op, size_t op_size, void * user_data)
{
    struct libfuse_version version = {
        FUSE_MAJOR_VERSION,
        FUSE_MINOR_VERSION,
        FUSE_HOTFIX_VERSION,
        0
    };

    return _fuse_new_31 (args, op, op_size, &version, user_data);
}

#define fuse_new(args, op, op_size, user_data) \
    fuse_new_fn (args, op, op_size, user_data)

int fuse_mount (struct fuse * f, const char * mountpoint);

/* USFS keeps this FUSE signature. errno is zero after confirmed removal (or
 * absence); on failure the instance retains ownership and may be retried. */
void fuse_unmount (struct fuse * f);

/* Requires completed unmount. Otherwise returns with errno=EBUSY without
 * destroying the backend, closing the connection, or invalidating f.
 * Failure to establish callback context likewise retains f and sets errno
 * to ENOMEM or the pthread initialization error, allowing a retry.
 * After unmount, pending owned hidden entries are retried once before backend
 * destruction. Unresolved deletion sets errno even though f is destroyed;
 * fuse_main reports cleanup status 10 when no earlier error takes precedence. */
void fuse_destroy (struct fuse * f);

int fuse_loop (struct fuse * f);

void fuse_exit (struct fuse * f);

struct fuse_loop_config;

#if FUSE_USE_VERSION < 32
int fuse_loop_mt_31 (struct fuse * f, int clone_fd);
    #define fuse_loop_mt(f, clone_fd) fuse_loop_mt_31 (f, clone_fd)
#else
int fuse_loop_mt (struct fuse * f, struct fuse_loop_config * config);
#endif

/* Valid only during a callback; returns NULL outside callback scope.
 * Lifecycle callbacks have no originating request: uid/gid/pid/umask are zero. */
struct fuse_context * fuse_get_context (void);

struct fuse_session * fuse_get_session (const struct fuse * fuse);

/* ----------------------------------------------------------- *
 * Command line parsing                                        *
 * ----------------------------------------------------------- */

struct fuse_cmdline_opts
{
    int singlethread;
    int foreground;
    int debug;
    int nodefault_subtype;
    char * mountpoint;
    int show_version;
    int show_help;
    int clone_fd;
    unsigned int max_idle_threads;
    unsigned int max_threads;
};

int fuse_parse_cmdline (struct fuse_args * args, struct fuse_cmdline_opts * opts);

void fuse_cmdline_help (void);

void fuse_lib_help (struct fuse_args * args);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_H_ */

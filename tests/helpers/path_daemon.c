/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * Test-only pathname backend: no operation can recover an object from fi->fh.
 */
#define FUSE_USE_VERSION 31
#define _ALL_SOURCE
#define _THREAD_SAFE
#include <fuse.h>
#include "tap.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <stdbool.h>

#define PROBE_PATH_CAPACITY  4096
#define PROBE_FILE_MODE      0600
#define PROBE_DIRECTORY_MODE 0700
#define PROBE_SPARSE_OFFSET  4096

static const char * backing_directory;

static int resolve_backing_path (const char * path, char * resolved)
{
    if (path == NULL)
    {
        fprintf (stderr, "Failed to dispatch pathname callback: unexpected NULL path\n");
        return -EIO;
    }

    const int length = snprintf (resolved, PROBE_PATH_CAPACITY, "%s%s", backing_directory, path);

    return length >= 0 && length < PROBE_PATH_CAPACITY ? 0 : -ENAMETOOLONG;
}

/* This fixture owns a private backing tree; external writers are unsupported. */
static pthread_mutex_t backing_namespace_lock = PTHREAD_MUTEX_INITIALIZER;

static int path_identity_matches (const struct stat * metadata, const struct fuse_object_identity * identity)
{
    return identity->has_backend_identity && (uint64_t)metadata->st_dev == identity->backend_dev &&
           (uint64_t)metadata->st_ino == identity->backend_ino && (metadata->st_mode & S_IFMT) == identity->backend_type;
}

static int path_entry_matches (const struct fuse_expected_entry * expected)
{
    char parent_path[PROBE_PATH_CAPACITY];
    const char * separator = strrchr (expected->path, '/');
    if (separator == NULL)
        return -ESTALE;

    const size_t parent_length = (size_t)(separator - expected->path);
    if (parent_length >= sizeof (parent_path))
        return -ENAMETOOLONG;

    if (parent_length == 0)
        strcpy (parent_path, "/");
    else
    {
        memcpy (parent_path, expected->path, parent_length);
        parent_path[parent_length] = '\0';
    }

    char resolved_parent[PROBE_PATH_CAPACITY];
    int rc = resolve_backing_path (parent_path, resolved_parent);
    if (rc != 0)
        return rc;

    struct stat metadata = { 0 };
    if (lstat (resolved_parent, &metadata) != 0 || !S_ISDIR (metadata.st_mode))
        return -ESTALE;

    if (expected->parent.nodeid != FUSE_ROOT_NODE_ID && !path_identity_matches (&metadata, &expected->parent))
        return -ESTALE;

    char resolved_child[PROBE_PATH_CAPACITY];
    rc = resolve_backing_path (expected->path, resolved_child);
    if (rc != 0)
        return rc;

    if (lstat (resolved_child, &metadata) != 0)
        return !expected->exists && errno == ENOENT ? 0 : -ESTALE;

    if (!expected->exists)
        return -ESTALE;

    return path_identity_matches (&metadata, &expected->object) ? 0 : -ESTALE;
}

static int fault_is_enabled (const char * name)
{
    char path[PROBE_PATH_CAPACITY];

    snprintf (path, sizeof (path), "%s/.fault-%s", backing_directory, name);

    return access (path, F_OK) == 0;
}

static int path_getattr (const char * path, struct stat * metadata, struct fuse_file_info * file_info)
{
    (void)file_info;
    char resolved[PROBE_PATH_CAPACITY];
    const int rc = resolve_backing_path (path, resolved);
    if (rc != 0)
        return rc;

    return lstat (resolved, metadata) == 0 ? 0 : -errno;
}

static int path_open (const char * path, struct fuse_file_info * file_info)
{
    char resolved[PROBE_PATH_CAPACITY];
    const int rc = resolve_backing_path (path, resolved);
    if (rc != 0)
        return rc;

    const int descriptor = open (resolved, file_info->flags);
    if (descriptor < 0)
        return -errno;

    close (descriptor);
    file_info->fh = 0;

    return 0;
}

static int path_create (const char * path, mode_t mode, struct fuse_file_info * file_info)
{
    char resolved[PROBE_PATH_CAPACITY];
    const int rc = resolve_backing_path (path, resolved);
    if (rc != 0)
        return rc;

    const int descriptor = open (resolved, file_info->flags | O_CREAT | O_EXCL, mode);
    if (descriptor < 0)
        return -errno;

    close (descriptor);
    file_info->fh = 0;

    return 0;
}

static int path_read (const char * path, char * buffer, size_t size, off_t offset, struct fuse_file_info * file_info)
{
    (void)file_info;
    char resolved[PROBE_PATH_CAPACITY];
    const int rc = resolve_backing_path (path, resolved);
    if (rc != 0)
        return rc;

    const int descriptor = open (resolved, O_RDONLY);
    if (descriptor < 0)
        return -errno;

    const ssize_t count = pread (descriptor, buffer, size, offset);
    const int error = errno;

    close (descriptor);

    return count >= 0 ? (int)count : -error;
}

static int path_write (const char * path, const char * buffer, size_t size, off_t offset, struct fuse_file_info * file_info)
{
    (void)file_info;
    char resolved[PROBE_PATH_CAPACITY];
    const int rc = resolve_backing_path (path, resolved);
    if (rc != 0)
        return rc;

    const int descriptor = open (resolved, O_RDWR);
    if (descriptor < 0)
        return -errno;

    const ssize_t count = pwrite (descriptor, buffer, size, offset);
    const int error = errno;

    close (descriptor);

    return count >= 0 ? (int)count : -error;
}

static int path_flush (const char * path, struct fuse_file_info * file_info)
{
    struct stat metadata;

    return path_getattr (path, &metadata, file_info);
}

static int path_fsync (const char * path, int datasync, struct fuse_file_info * file_info)
{
    (void)datasync;
    (void)file_info;
    char resolved[PROBE_PATH_CAPACITY];
    const int rc = resolve_backing_path (path, resolved);
    if (rc != 0)
        return rc;

    const int descriptor = open (resolved, O_RDWR);
    if (descriptor < 0)
        return -errno;

    const int result = fsync (descriptor);
    const int error = errno;

    close (descriptor);

    return result == 0 ? 0 : -error;
}

static int path_release (const char * path, struct fuse_file_info * file_info)
{
    const int rc = path_flush (path, file_info);

    printf ("RELEASE %s\n", path != NULL ? path : "NULL");
    fflush (stdout);

    return rc;
}

static int path_unlink (const struct fuse_expected_entry * expected)
{
    char resolved[PROBE_PATH_CAPACITY];
    int rc = resolve_backing_path (expected->path, resolved);
    if (rc != 0)
        return rc;

    pthread_mutex_lock (&backing_namespace_lock);
    rc = path_entry_matches (expected);
    if (rc != 0)
    {
        pthread_mutex_unlock (&backing_namespace_lock);
        return rc;
    }

    if (strstr (expected->path, "/.fuse_hidden") != NULL && fault_is_enabled ("cleanup"))
    {
        pthread_mutex_unlock (&backing_namespace_lock);
        return -EACCES;
    }

    printf ("DELETE %s\n", expected->path);
    fflush (stdout);
    rc = unlink (resolved) == 0 ? 0 : -errno;
    pthread_mutex_unlock (&backing_namespace_lock);

    return rc;
}

static int occupy_failed_rename_destination (const char * destination)
{
    if (!fault_is_enabled ("occupied"))
        return -EIO;

    const int descriptor = open (destination, O_CREAT | O_EXCL | O_WRONLY, PROBE_FILE_MODE);
    if (descriptor < 0)
        return -errno;

    const char content[] = "occupied";
    const int written = write (descriptor, content, sizeof (content) - 1) == (ssize_t)(sizeof (content) - 1);

    close (descriptor);

    return written ? -EIO : -ENOSPC;
}

static int path_rename (
    const struct fuse_expected_entry * source,
    const struct fuse_expected_entry * destination
)
{
    char old_path[PROBE_PATH_CAPACITY];
    char new_path[PROBE_PATH_CAPACITY];
    int rc = resolve_backing_path (source->path, old_path);
    if (rc != 0)
        return rc;

    rc = resolve_backing_path (destination->path, new_path);
    if (rc != 0)
        return rc;

    pthread_mutex_lock (&backing_namespace_lock);
    rc = path_entry_matches (source);
    if (rc == 0)
        rc = path_entry_matches (destination);

    if (rc == 0 && strstr (destination->path, "/.fuse_hidden") != NULL && fault_is_enabled ("hide"))
        rc = -EACCES;

    if (rc == 0 && strstr (source->path, "/.fuse_hidden") != NULL && fault_is_enabled ("rollback"))
        rc = -EACCES;

    if (rc == 0 && strstr (source->path, "/source") != NULL && fault_is_enabled ("source"))
        rc = occupy_failed_rename_destination (new_path);

    if (rc == 0)
        rc = rename (old_path, new_path) == 0 ? 0 : -errno;

    pthread_mutex_unlock (&backing_namespace_lock);
    return rc;
}

static int path_mkdir (const char * path, mode_t mode)
{
    char resolved[PROBE_PATH_CAPACITY];
    const int rc = resolve_backing_path (path, resolved);
    if (rc != 0)
        return rc;

    return mkdir (resolved, mode) == 0 ? 0 : -errno;
}

static int path_rmdir (const struct fuse_expected_entry * expected)
{
    char resolved[PROBE_PATH_CAPACITY];
    int rc = resolve_backing_path (expected->path, resolved);
    if (rc != 0)
        return rc;

    pthread_mutex_lock (&backing_namespace_lock);
    rc = path_entry_matches (expected);
    if (rc == 0)
        rc = rmdir (resolved) == 0 ? 0 : -errno;
    pthread_mutex_unlock (&backing_namespace_lock);

    return rc;
}

static int path_link (const char * source, const char * destination)
{
    char old_path[PROBE_PATH_CAPACITY];
    char new_path[PROBE_PATH_CAPACITY];
    int rc = resolve_backing_path (source, old_path);
    if (rc != 0)
        return rc;

    rc = resolve_backing_path (destination, new_path);
    if (rc != 0)
        return rc;

    return link (old_path, new_path) == 0 ? 0 : -errno;
}

static int path_chmod (const char * path, mode_t mode, struct fuse_file_info * file_info)
{
    (void)file_info;
    char resolved[PROBE_PATH_CAPACITY];
    const int rc = resolve_backing_path (path, resolved);
    if (rc != 0)
        return rc;

    return chmod (resolved, mode) == 0 ? 0 : -errno;
}

static int path_chown (const char * path, uid_t uid, gid_t gid, struct fuse_file_info * file_info)
{
    (void)file_info;
    char resolved[PROBE_PATH_CAPACITY];
    const int rc = resolve_backing_path (path, resolved);
    if (rc != 0)
        return rc;

    return chown (resolved, uid, gid) == 0 ? 0 : -errno;
}

static int path_truncate (const char * path, off_t size, struct fuse_file_info * file_info)
{
    (void)file_info;
    char resolved[PROBE_PATH_CAPACITY];
    const int rc = resolve_backing_path (path, resolved);
    if (rc != 0)
        return rc;

    return truncate (resolved, size) == 0 ? 0 : -errno;
}

static int path_utimens (const char * path, const struct timespec times[2], struct fuse_file_info * file_info)
{
    (void)file_info;
    char resolved[PROBE_PATH_CAPACITY];
    const int rc = resolve_backing_path (path, resolved);
    if (rc != 0)
        return rc;

    return utimensat (AT_FDCWD, resolved, times, 0) == 0 ? 0 : -errno;
}

static int path_readdir (
    const char * path,
    void * buffer,
    fuse_fill_dir_t fill,
    off_t offset,
    struct fuse_file_info * file_info,
    enum fuse_readdir_flags flags
)
{
    (void)offset;
    (void)file_info;
    (void)flags;
    char resolved[PROBE_PATH_CAPACITY];
    const int rc = resolve_backing_path (path, resolved);
    if (rc != 0)
        return rc;

    DIR * directory = opendir (resolved);
    if (directory == NULL)
        return -errno;

    for (struct dirent * entry = readdir (directory); entry != NULL; entry = readdir (directory))
        if (fill (buffer, entry->d_name, NULL, 0, FUSE_FILL_DIR_DEFAULTS) != 0)
            break;

    closedir (directory);

    return 0;
}

static const struct fuse_operations operations = { .getattr = path_getattr,
                                                   .create = path_create,
                                                   .open = path_open,
                                                   .read = path_read,
                                                   .write = path_write,
                                                   .flush = path_flush,
                                                   .fsync = path_fsync,
                                                   .release = path_release,
                                                   .unlink_checked = path_unlink,
                                                   .rename_checked = path_rename,
                                                   .mkdir = path_mkdir,
                                                   .rmdir_checked = path_rmdir,
                                                   .link = path_link,
                                                   .chmod = path_chmod,
                                                   .chown = path_chown,
                                                   .truncate = path_truncate,
                                                   .utimens = path_utimens,
                                                   .readdir = path_readdir };

#include "path_lifetime_probe.h"

int main (int argc, char ** argv)
{
    if (argc == 4 && strcmp (argv[1], "--probe") == 0)
        return run_path_lifetime_probe (argv[2], argv[3]);

    if (argc == 4 && strcmp (argv[1], "--cleanup-probe") == 0)
        return run_path_cleanup_probe (argv[2], argv[3]);

    backing_directory = getenv ("USFS_PATH_BACKING_DIRECTORY");
    if (backing_directory == NULL || backing_directory[0] != '/')
    {
        fprintf (stderr, "Failed to start pathname test filesystem: absolute USFS_PATH_BACKING_DIRECTORY is required\n");
        return EXIT_FAILURE;
    }

    return fuse_main (argc, argv, &operations, NULL);
}

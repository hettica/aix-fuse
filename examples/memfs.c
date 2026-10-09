/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * USFS: FUSE-compatible file systems for AIX
 *
 * Writable in-memory file system.
 *
 * Everything lives in this daemon's own memory: the file system starts as an
 * empty root directory and is gone when the daemon exits. It behaves like a
 * small /tmp -- directories, files, symbolic links, hard links, ownership and
 * permissions. The capacity cap bounds retained regular-file buffers,
 * including spare capacity. It does not bound total process RSS, allocator
 * overhead, or namespace metadata.
 *
 * Compile with:
 *
 *     gcc -maix64 -Isrc/client/include memfs.c libfuse3.a -o usfs_memfs
 *
 * Usage:
 *
 *     usfs_memfs [--size=<MB>] [--inodes=<count>] [mountpoint]
 *
 * Objects are inodes, and directories hold names pointing at them, so a file
 * reachable by several hard links is one object with one link count, and a file
 * that is unlinked while still open stays alive until the last handle closes.
 *
 * Extended attributes, ACLs and memory mapping are out of scope.
 */

#define FUSE_USE_VERSION 31

#include <fuse.h>

#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/* ----------------------------------------------------------- *
 * Objects                                                     *
 * ----------------------------------------------------------- */

struct mem_inode;

/* Longest single path component. AIX only exposes NAME_MAX under _ALL_SOURCE,
   and this matches what statvfs reports as f_namemax. */
#define MEMFS_NAME_MAX 255

/* Longest symbolic link target. Matches the cap the kernel extension puts on a
   READLINK reply, so nothing storable here can fail to be read back. */
#define MEMFS_LINK_MAX 1024

struct mem_dirent
{
    struct mem_dirent * next; // Next entry in this directory.
    char * name;              // Name of this entry within its directory.
    struct mem_inode * inode; // Inode referenced by this entry.
};

struct mem_inode
{
    struct mem_inode * index_next; // Next live inode in the mount's identity index.
    uint64_t ino;                  // Stable inode number reported to callers.
    mode_t mode;                   // File type and permission bits.
    uid_t uid;                     // User owning the inode.
    gid_t gid;                     // Group owning the inode.
    nlink_t nlink;                 // Number of directory links retaining the inode.
    int open_count;                // Number of live handles retaining the inode.
    struct timespec atime;         // Last access time.
    struct timespec mtime;         // Last content modification time.
    struct timespec ctime;         // Last metadata change time.
    char * data;                   // Allocated buffer holding regular-file contents.
    size_t size;                 // Logical file size in bytes.
    size_t cap;                  // Retained regular-file buffer capacity in bytes.
    char * target;               // Symbolic-link target, or NULL for other inode types.
    struct mem_dirent * entries; // Linked list of directory entries.
    struct mem_inode * parent;   // Parent used for dot-dot lookup; NULL when detached.
};

struct mem_handle
{
    struct mem_handle * next; // Next live handle.
    uint64_t id;              // Handle identifier before FUSE encoding adds one.
    struct mem_inode * inode; // Inode retained while this handle remains open.
    int flags;                // Open flags controlling access through this handle.
};

static struct mem_inode * g_root;
static struct mem_inode * g_inodes;
static uint64_t g_next_ino = 1;
static uint64_t g_next_handle = 1;
static struct mem_handle * g_handles;

static size_t g_used;      /* bytes of file content held */
static size_t g_allocated; /* retained regular-file buffer bytes */
static size_t g_max = 64u * 1024 * 1024;
static uint64_t g_inode_count;
static uint64_t g_inode_max = 10000;
static pthread_mutex_t g_memfs_lock = PTHREAD_MUTEX_INITIALIZER;

static struct options
{
    int size_mb;             // Maximum retained file-buffer capacity in megabytes.
    int inode_limit;         // Maximum number of live inodes.
    int show_help_requested; // Whether filesystem-specific help was requested.
} options;

#define OPTION(option_pattern, member) { option_pattern, offsetof (struct options, member), 1 }

static const struct fuse_opt option_specs[] = {
    OPTION ("--size=%u", size_mb),
    OPTION ("--inodes=%u", inode_limit),
    OPTION ("-h", show_help_requested),
    OPTION ("--help", show_help_requested),
    FUSE_OPT_END
};

/* ----------------------------------------------------------- *
 * Helpers                                                     *
 * ----------------------------------------------------------- */

static void read_current_time (struct timespec * timestamp)
{
    timestamp->tv_sec = time (NULL);
    timestamp->tv_nsec = 0;
}

static void update_modification_time (struct mem_inode * inode)
{
    read_current_time (&inode->mtime);
    inode->ctime = inode->mtime;
}

static struct mem_inode * inode_new (const mode_t mode, const uid_t uid, const gid_t gid)
{
    struct mem_inode * inode;

    if (g_inode_count >= g_inode_max)
    {
        errno = ENOSPC;
        return NULL;
    }

    if (g_next_ino == 0)
    {
        errno = ENOSPC;
        return NULL;
    }

    inode = calloc (1, sizeof (*inode));

    if (inode == NULL)
    {
        errno = ENOMEM;
        return NULL;
    }

    inode->ino = g_next_ino++;
    inode->mode = mode;
    inode->uid = uid;
    inode->gid = gid;
    inode->nlink = 0;
    read_current_time (&inode->atime);
    inode->mtime = inode->atime;
    inode->ctime = inode->atime;
    inode->index_next = g_inodes;
    g_inodes = inode;

    g_inode_count++;

    return inode;
}

/* Releases an inode once nothing links to it and nothing has it open. */
static void inode_maybe_free (struct mem_inode * inode)
{
    if (inode == NULL)
        return;

    if (inode->nlink > 0)
        return;

    if (inode->open_count > 0)
        return;

    /* Only regular-file contents are charged to g_used.  A symlink reports
       its target length through getattr, but its target storage is governed by
       MEMFS_LINK_MAX rather than the byte-capacity pool. */
    if (S_ISREG (inode->mode))
    {
        g_used -= inode->size;
        g_allocated -= inode->cap;
    }
    g_inode_count--;

    struct mem_inode ** indexed_inode = &g_inodes;
    while (*indexed_inode != NULL && *indexed_inode != inode)
        indexed_inode = &(*indexed_inode)->index_next;

    if (*indexed_inode == inode)
        *indexed_inode = inode->index_next;

    free (inode->data);
    free (inode->target);
    free (inode);
}

static struct mem_dirent * find_directory_entry (struct mem_inode * directory_inode, const char * entry_name)
{
    struct mem_dirent * directory_entry;

    for (directory_entry = directory_inode->entries; directory_entry != NULL; directory_entry = directory_entry->next)
        if (strcmp (directory_entry->name, entry_name) == 0)
            return directory_entry;

    return NULL;
}

static struct mem_dirent * allocate_directory_entry (const char * entry_name, struct mem_inode * inode)
{
    struct mem_dirent * directory_entry = calloc (1, sizeof (*directory_entry));

    if (directory_entry == NULL)
        return NULL;

    directory_entry->name = strdup (entry_name);
    if (directory_entry->name == NULL)
    {
        free (directory_entry);
        return NULL;
    }

    directory_entry->inode = inode;
    return directory_entry;
}

static void free_directory_entry (struct mem_dirent * directory_entry)
{
    if (directory_entry == NULL)
        return;
    free (directory_entry->name);
    free (directory_entry);
}

static void publish_directory_entry (struct mem_inode * directory_inode, struct mem_dirent * directory_entry)
{
    struct mem_inode * inode = directory_entry->inode;
    directory_entry->next = directory_inode->entries;
    directory_inode->entries = directory_entry;

    inode->nlink++;
    if (S_ISDIR (inode->mode))
        inode->parent = directory_inode;

    update_modification_time (directory_inode);
}

static int add_directory_entry (struct mem_inode * directory_inode, const char * entry_name, struct mem_inode * inode)
{
    struct mem_dirent * directory_entry = allocate_directory_entry (entry_name, inode);
    if (directory_entry == NULL)
        return -ENOMEM;

    publish_directory_entry (directory_inode, directory_entry);
    return 0;
}

static void dir_remove (struct mem_inode * directory_inode, struct mem_dirent * removed_entry)
{
    struct mem_dirent ** entry_link = &directory_inode->entries;

    while (*entry_link != NULL && *entry_link != removed_entry)
        entry_link = &(*entry_link)->next;

    if (*entry_link != removed_entry)
        return;

    *entry_link = removed_entry->next;

    removed_entry->inode->nlink--;
    /* A removed directory can remain open after its former parent is freed.
     * It has no authoritative parent relationship while detached. */
    if (S_ISDIR (removed_entry->inode->mode) && removed_entry->inode->nlink == 0)
        removed_entry->inode->parent = NULL;
    update_modification_time (directory_inode);

    free (removed_entry->name);
    free (removed_entry);
}

/*
 * Resolves an absolute path to its inode. When parent_out is not NULL the
 * lookup stops at the last component and reports the containing directory and
 * the final name instead, which is what the creating operations need.
 */
static struct mem_inode * resolve (const char * path, struct mem_inode ** parent_inode_output, const char ** entry_name_output)
{
    static char path_components[PATH_MAX];
    struct mem_inode * current_inode = g_root;
    char * path_token_context = NULL;
    char * path_component;
    char * next_component;

    if (parent_inode_output != NULL)
    {
        *parent_inode_output = NULL;
        *entry_name_output = NULL;
    }

    if (path == NULL)
        return NULL;

    if (path[0] != '/')
        return NULL;

    if (strcmp (path, "/") == 0)
    {
        if (parent_inode_output != NULL)
        {
            *parent_inode_output = NULL; /* the root has no parent entry */
            *entry_name_output = NULL;
        }
        return g_root;
    }

    if (strlen (path) >= sizeof (path_components))
        return NULL;

    strcpy (path_components, path);

    path_component = strtok_r (path_components + 1, "/", &path_token_context);

    while (path_component != NULL)
    {
        struct mem_dirent * directory_entry;

        next_component = strtok_r (NULL, "/", &path_token_context);

        if (next_component == NULL && parent_inode_output != NULL)
        {
            /* Last component: report the parent and the name. */
            if (!S_ISDIR (current_inode->mode))
                return NULL;

            *parent_inode_output = current_inode;
            /* strtok_r left comp NUL-terminated inside buf, which
               is static and outlives this call. */
            *entry_name_output = path_component;

            directory_entry = find_directory_entry (current_inode, path_component);
            return (directory_entry != NULL) ? directory_entry->inode : NULL;
        }

        if (!S_ISDIR (current_inode->mode))
            return NULL;

        directory_entry = find_directory_entry (current_inode, path_component);
        if (directory_entry == NULL)
            return NULL;

        current_inode = directory_entry->inode;
        path_component = next_component;
    }

    return current_inode;
}

static struct mem_handle * find_file_handle (const uint64_t handle_id)
{
    struct mem_handle * file_handle;

    for (file_handle = g_handles; file_handle != NULL; file_handle = file_handle->next)
        if (file_handle->id == handle_id)
            return file_handle;

    return NULL;
}

/* Handles reference the inode, which is what keeps an unlinked file readable. */
static struct mem_handle * allocate_file_handle (struct mem_inode * inode, const int open_flags)
{
    struct mem_handle * file_handle = calloc (1, sizeof (*file_handle));

    if (file_handle == NULL)
        return NULL;

    file_handle->id = g_next_handle++;
    file_handle->inode = inode;
    file_handle->flags = open_flags;
    return file_handle;
}

static void publish_file_handle (struct mem_handle * file_handle)
{
    file_handle->next = g_handles;
    g_handles = file_handle;

    file_handle->inode->open_count++;
}

static uint64_t open_file_handle (struct mem_inode * inode, const int open_flags)
{
    struct mem_handle * file_handle = allocate_file_handle (inode, open_flags);
    if (file_handle == NULL)
        return 0;

    publish_file_handle (file_handle);
    return file_handle->id;
}

static void handle_close (const uint64_t handle_id)
{
    struct mem_handle ** handle_link = &g_handles;

    while (*handle_link != NULL && (*handle_link)->id != handle_id)
        handle_link = &(*handle_link)->next;

    if (*handle_link == NULL)
        return;

    struct mem_handle * file_handle = *handle_link;
    struct mem_inode * inode = file_handle->inode;

    *handle_link = file_handle->next;
    free (file_handle);

    inode->open_count--;
    inode_maybe_free (inode);
}

/* Resolves the inode an operation should act on, preferring an open handle so
   that a file unlinked while open is still reachable. */
static struct mem_inode * find_operation_inode (const char * path, struct fuse_file_info * file_info)
{
    if (file_info != NULL && file_info->fh != 0)
    {
        struct mem_handle * file_handle = find_file_handle (file_info->fh - 1);

        return file_handle != NULL ? file_handle->inode : NULL;
    }

    return resolve (path, NULL, NULL);
}

static void fill_file_attributes (const struct mem_inode * inode, struct stat * file_attributes)
{
    memset (file_attributes, 0, sizeof (*file_attributes));

    file_attributes->st_ino = (ino_t)inode->ino;
    file_attributes->st_mode = inode->mode;
    file_attributes->st_nlink = inode->nlink;
    file_attributes->st_uid = inode->uid;
    file_attributes->st_gid = inode->gid;
    file_attributes->st_size = (off_t) (S_ISLNK (inode->mode) ? strlen (inode->target) : inode->size);
    file_attributes->st_blksize = 4096;
    file_attributes->st_blocks = (blkcnt_t) ((inode->size + 511) / 512);
    file_attributes->st_atime = inode->atime.tv_sec;
    file_attributes->st_mtime = inode->mtime.tv_sec;
    file_attributes->st_ctime = inode->ctime.tv_sec;
    file_attributes->st_atim.tv_nsec = inode->atime.tv_nsec;
    file_attributes->st_mtim.tv_nsec = inode->mtime.tv_nsec;
    file_attributes->st_ctim.tv_nsec = inode->ctime.tv_nsec;
}

static int is_file_accounting_valid (const struct mem_inode * inode)
{
    if (g_used > g_allocated)
        return false;

    if (g_allocated > g_max)
        return false;

    if (inode == NULL)
        return true;

    if (inode->size > inode->cap)
        return false;

    if (inode->size > g_used)
        return false;

    if (inode->cap > g_allocated)
        return false;

    if ((inode->cap == 0) != (inode->data == NULL))
        return false;

    return true;
}

/* Grows a file's buffer to at least `need` bytes, honouring the capacity cap. */
static int reserve_file_capacity (struct mem_inode * inode, const size_t required_capacity)
{
    size_t buffer_capacity, maximum_file_capacity;
    char * resized_buffer;

    if (!is_file_accounting_valid (inode))
        return -EIO;

    if (required_capacity <= inode->cap)
        return 0;

    maximum_file_capacity = inode->cap + (g_max - g_allocated);
    if (required_capacity > maximum_file_capacity)
        return -ENOSPC;

    buffer_capacity = (inode->cap != 0) ? inode->cap : 4096;
    if (buffer_capacity > maximum_file_capacity)
        buffer_capacity = maximum_file_capacity;
    while (buffer_capacity < required_capacity)
    {
        if (buffer_capacity > maximum_file_capacity / 2)
        {
            buffer_capacity = maximum_file_capacity;
            break;
        }
        buffer_capacity *= 2;
    }

    resized_buffer = realloc (inode->data, buffer_capacity);
    if (resized_buffer == NULL)
        return -ENOMEM;

    memset (resized_buffer + inode->cap, 0, buffer_capacity - inode->cap);
    g_allocated += buffer_capacity - inode->cap;
    inode->data = resized_buffer;
    inode->cap = buffer_capacity;

    return 0;
}

static void resize_file_buffer (struct mem_inode * inode, const size_t new_file_size)
{
    if (new_file_size > inode->size)
        memset (inode->data + inode->size, 0, new_file_size - inode->size);
    else if (new_file_size == 0)
    {
        free (inode->data);
        g_allocated -= inode->cap;
        inode->data = NULL;
        inode->cap = 0;
    }
    else if (new_file_size < inode->size)
    {
        /* Shrink failure does not fail truncation or refund retained bytes. */
        char * resized_buffer = realloc (inode->data, new_file_size);
        if (resized_buffer != NULL)
        {
            g_allocated -= inode->cap - new_file_size;
            inode->data = resized_buffer;
            inode->cap = new_file_size;
        }
    }

    g_used = g_used - inode->size + new_file_size;
    inode->size = new_file_size;
}

/* ----------------------------------------------------------- *
 * Operations                                                  *
 * ----------------------------------------------------------- */

static void * initialize_filesystem (struct fuse_conn_info * connection_info, struct fuse_config * filesystem_configuration)
{
    (void)connection_info;
    filesystem_configuration->use_ino = 1;
    filesystem_configuration->hard_remove = 1;
    filesystem_configuration->nullpath_ok = 1;
    return NULL;
}

static void destroy_filesystem (void * private_data)
{
    (void)private_data;
    /* The process is exiting; the whole file system goes with it. */
}

static int get_file_attributes_locked (const char * path, struct stat * file_attributes, struct fuse_file_info * file_info)
{
    struct mem_inode * inode = find_operation_inode (path, file_info);

    if (inode == NULL)
        return -ENOENT;

    fill_file_attributes (inode, file_attributes);

    return 0;
}

struct identity_walk_frame
{
    struct mem_dirent * next; // Next child entry in this directory.
    size_t prefix_length;     // Bytes in the parent path, or path_capacity when too long.
};

static struct mem_inode * find_linked_inode_by_id (
    struct mem_inode * root,
    const uint64_t token,
    char * path,
    const size_t path_capacity,
    int * path_error
)
{
    if (g_inode_count > (SIZE_MAX / sizeof (struct identity_walk_frame)) - 1u)
    {
        *path_error = ENOMEM;
        return NULL;
    }

    const size_t frame_count = (size_t)g_inode_count + 1u;
    struct identity_walk_frame * frames = calloc (frame_count, sizeof (*frames));
    if (frames == NULL)
    {
        *path_error = ENOMEM;
        return NULL;
    }

    size_t depth = 0;
    frames[0].next = root->entries;
    struct mem_inode * found = NULL;

    while (frames[0].next != NULL || depth != 0)
    {
        struct identity_walk_frame * frame = &frames[depth];
        if (frame->next == NULL)
        {
            depth--;
            continue;
        }

        struct mem_dirent * entry = frame->next;
        frame->next = entry->next;
        const size_t name_length = strlen (entry->name);
        const int path_fits = frame->prefix_length < path_capacity && name_length < path_capacity - frame->prefix_length - 1u;
        size_t child_length = path_capacity;

        if (path_fits)
        {
            path[frame->prefix_length] = '/';
            memcpy (path + frame->prefix_length + 1u, entry->name, name_length + 1u);
            child_length = frame->prefix_length + name_length + 1u;
        }

        if (entry->inode->ino == token && entry->inode->nlink != 0)
        {
            if (!path_fits)
                *path_error = ENAMETOOLONG;

            found = entry->inode;
            break;
        }

        if (!S_ISDIR (entry->inode->mode))
            continue;

        if (depth + 1u >= frame_count)
        {
            *path_error = EIO;
            break;
        }

        depth++;
        frames[depth].next = entry->inode->entries;
        frames[depth].prefix_length = child_length;
    }

    free (frames);
    return found;
}

static struct mem_inode * find_live_inode_by_id (const uint64_t token)
{
    for (struct mem_inode * inode = g_inodes; inode != NULL; inode = inode->index_next)
        if (inode->ino == token)
            return inode;

    return NULL;
}

static int export_object_id_locked (
    const char * path_or_null,
    const uint64_t backend_dev,
    const uint64_t backend_ino,
    const mode_t backend_type,
    uint64_t * token
)
{
    if (backend_dev != 0 || backend_ino == 0)
        return -ESTALE;

    struct mem_inode * inode = NULL;
    if (path_or_null != NULL)
        inode = resolve (path_or_null, NULL, NULL);
    else
        inode = find_live_inode_by_id (backend_ino);

    if (inode == NULL || inode->nlink == 0)
        return -ESTALE;

    if (inode->ino != backend_ino || (inode->mode & S_IFMT) != backend_type)
        return -ESTALE;

    *token = inode->ino;

    return 0;
}

static int resolve_object_id_locked (const uint64_t token, char * path, const size_t path_capacity)
{
    struct mem_inode * indexed_inode = find_live_inode_by_id (token);
    if (indexed_inode == NULL || indexed_inode->nlink == 0 || g_root == NULL)
        return -ESTALE;

    if (g_root->ino == token)
    {
        if (path_capacity < sizeof ("/"))
            return -ENAMETOOLONG;

        strcpy (path, "/");
        return 0;
    }

    int path_error = 0;
    struct mem_inode * inode = find_linked_inode_by_id (g_root, token, path, path_capacity, &path_error);
    if (inode == NULL)
        return path_error != 0 ? -path_error : -ESTALE;

    if (path_error != 0)
        return -path_error;

    return 0;
}

static int read_symbolic_link_locked (const char * path, char * buffer, size_t buffer_capacity)
{
    struct mem_inode * inode = resolve (path, NULL, NULL);

    if (inode == NULL)
        return -ENOENT;

    if (!S_ISLNK (inode->mode))
        return -EINVAL;

    if (buffer_capacity == 0)
        return -EINVAL;

    strncpy (buffer, inode->target, buffer_capacity - 1);
    buffer[buffer_capacity - 1] = '\0';

    return 0;
}

static int read_directory_locked (const char * path, void * buffer, fuse_fill_dir_t add_directory_entry_to_buffer, const off_t offset, struct fuse_file_info * file_info, enum fuse_readdir_flags flags)
{
    struct mem_inode * inode = find_operation_inode (path, file_info);
    struct mem_dirent * directory_entry;
    struct stat file_attributes;

    (void)offset;
    (void)flags;

    if (inode == NULL)
        return -ENOENT;

    if (!S_ISDIR (inode->mode))
        return -ENOTDIR;

    fill_file_attributes (inode, &file_attributes);
    if (add_directory_entry_to_buffer (buffer, ".", &file_attributes, 0, FUSE_FILL_DIR_DEFAULTS) != 0)
        return 0;

    fill_file_attributes (inode->parent != NULL ? inode->parent : inode, &file_attributes);
    if (add_directory_entry_to_buffer (buffer, "..", &file_attributes, 0, FUSE_FILL_DIR_DEFAULTS) != 0)
        return 0;

    for (directory_entry = inode->entries; directory_entry != NULL; directory_entry = directory_entry->next)
    {
        fill_file_attributes (directory_entry->inode, &file_attributes);
        if (add_directory_entry_to_buffer (buffer, directory_entry->name, &file_attributes, 0, FUSE_FILL_DIR_DEFAULTS) != 0)
            break;
    }

    return 0;
}

static gid_t get_creation_group_id (const struct mem_inode * parent_inode, const gid_t caller_group_id)
{
    return parent_inode->mode & S_ISGID ? parent_inode->gid : caller_group_id;
}

static int is_initial_time_valid (const unsigned int valid_attribute_mask, const unsigned int attribute_flag, const long nanoseconds)
{
    if ((valid_attribute_mask & attribute_flag) == 0)
        return true;

    if (nanoseconds < 0)
        return false;

    if (nanoseconds >= 1000000000L)
        return false;

    return true;
}

static int are_creation_attributes_valid (const struct stat * initial_attributes, const unsigned int valid_attribute_mask, enum fuse_create_activation creation_activation, const struct fuse_file_info * file_info)
{
    const unsigned int allowed_attribute_mask = FUSE_INITIAL_MODE | FUSE_INITIAL_UID |
                                                FUSE_INITIAL_GID | FUSE_INITIAL_SIZE | FUSE_INITIAL_ATIME |
                                                FUSE_INITIAL_MTIME | FUSE_INITIAL_CTIME;

    if ((valid_attribute_mask & ~allowed_attribute_mask) != 0)
        return false;

    if ((valid_attribute_mask & FUSE_INITIAL_MODE) == 0)
        return false;

    if (!S_ISREG (initial_attributes->st_mode))
        return false;

    if (creation_activation > FUSE_CREATE_OPEN)
        return false;

    if ((creation_activation == FUSE_CREATE_OPEN) != (file_info != NULL))
        return false;

    if ((valid_attribute_mask & FUSE_INITIAL_SIZE) && initial_attributes->st_size < 0)
        return false;

    if (!is_initial_time_valid (valid_attribute_mask, FUSE_INITIAL_ATIME,
                                initial_attributes->st_atim.tv_nsec))
        return false;

    if (!is_initial_time_valid (valid_attribute_mask, FUSE_INITIAL_MTIME,
                                initial_attributes->st_mtim.tv_nsec))
        return false;

    if (!is_initial_time_valid (valid_attribute_mask, FUSE_INITIAL_CTIME,
                                initial_attributes->st_ctim.tv_nsec))
        return false;

    return true;
}

static int create_directory_locked (const char * path, const mode_t mode)
{
    struct mem_inode * parent_inode;
    struct mem_inode * inode;
    const char * entry_name;

    if (resolve (path, &parent_inode, &entry_name) != NULL)
        return -EEXIST;

    if (parent_inode == NULL)
        return -ENOENT;

    if (entry_name == NULL)
        return -ENOENT;

    inode = inode_new (S_IFDIR | (mode & 07777) | (parent_inode->mode & S_ISGID),
                       fuse_get_context ()->uid, get_creation_group_id (parent_inode, fuse_get_context ()->gid));
    if (inode == NULL)
        return -errno;

    const int operation_result = add_directory_entry (parent_inode, entry_name, inode);
    if (operation_result != 0)
    {
        inode_maybe_free (inode);
        return operation_result;
    }

    return 0;
}

static int prepare_file_storage_and_handle (struct mem_inode * inode, const size_t initial_file_size, const struct fuse_file_info * file_info, struct mem_handle ** prepared_handle)
{
    const int allocation_result = reserve_file_capacity (inode, initial_file_size);
    if (allocation_result != 0)
        return allocation_result;

    if (file_info == NULL)
        return 0;

    *prepared_handle = allocate_file_handle (inode, file_info->flags);
    if (*prepared_handle == NULL)
        return -ENOMEM;

    return 0;
}

static void apply_initial_file_attributes (struct mem_inode * inode, const struct stat * initial_attributes, const unsigned int valid_attribute_mask, const size_t initial_file_size)
{
    inode->uid = initial_attributes->st_uid;
    resize_file_buffer (inode, initial_file_size);

    if (valid_attribute_mask & FUSE_INITIAL_ATIME)
    {
        inode->atime.tv_sec = initial_attributes->st_atim.tv_sec;
        inode->atime.tv_nsec = initial_attributes->st_atim.tv_nsec;
    }

    if (valid_attribute_mask & FUSE_INITIAL_MTIME)
    {
        inode->mtime.tv_sec = initial_attributes->st_mtim.tv_sec;
        inode->mtime.tv_nsec = initial_attributes->st_mtim.tv_nsec;
    }

    if (valid_attribute_mask & FUSE_INITIAL_CTIME)
    {
        inode->ctime.tv_sec = initial_attributes->st_ctim.tv_sec;
        inode->ctime.tv_nsec = initial_attributes->st_ctim.tv_nsec;
    }
}

static int create_file_with_attributes_locked (const char * path, const struct stat * initial_attributes, const unsigned int valid_attribute_mask, enum fuse_create_activation creation_activation, struct stat * created_attributes, struct fuse_file_info * file_info)
{
    struct mem_inode * parent_inode;
    struct mem_inode * inode;
    struct mem_dirent * prepared_entry;
    struct mem_handle * prepared_handle = NULL;
    const char * entry_name;
    const size_t initial_file_size = valid_attribute_mask & FUSE_INITIAL_SIZE ? (size_t)initial_attributes->st_size : 0;
    int operation_result;

    if (!are_creation_attributes_valid (initial_attributes, valid_attribute_mask, creation_activation, file_info))
        return -EINVAL;

    if (resolve (path, &parent_inode, &entry_name) != NULL)
        return -EEXIST;

    if (parent_inode == NULL)
        return -ENOENT;

    if (!S_ISDIR (parent_inode->mode))
        return -ENOTDIR;

    if (!is_file_accounting_valid (NULL))
        return -EIO;

    if (initial_file_size > g_max - g_allocated)
        return -ENOSPC;

    inode = inode_new (initial_attributes->st_mode, initial_attributes->st_uid,
                       valid_attribute_mask & FUSE_INITIAL_GID ? initial_attributes->st_gid : get_creation_group_id (parent_inode, initial_attributes->st_gid));
    if (inode == NULL)
        return -ENOMEM;

    prepared_entry = allocate_directory_entry (entry_name, inode);
    if (prepared_entry == NULL)
    {
        inode_maybe_free (inode);
        return -ENOMEM;
    }

    operation_result = prepare_file_storage_and_handle (inode, initial_file_size, file_info, &prepared_handle);
    if (operation_result != 0)
    {
        free_directory_entry (prepared_entry);
        inode_maybe_free (inode);
        return operation_result;
    }
    /* All allocation is complete. Attribute assignment and publication cannot fail. */
    apply_initial_file_attributes (inode, initial_attributes, valid_attribute_mask, initial_file_size);
    publish_directory_entry (parent_inode, prepared_entry);
    if (prepared_handle != NULL)
    {
        publish_file_handle (prepared_handle);
        file_info->fh = prepared_handle->id + 1;
    }

    fill_file_attributes (inode, created_attributes);
    return 0;
}

static int create_file_locked (const char * path, const mode_t mode, struct fuse_file_info * file_info)
{
    struct mem_inode * parent_inode;
    struct mem_inode * inode;
    const char * entry_name;
    struct mem_dirent * prepared_entry = NULL;
    struct mem_handle * prepared_handle;

    inode = resolve (path, &parent_inode, &entry_name);
    if (parent_inode == NULL)
        return -ENOENT;

    if (entry_name == NULL)
        return -ENOENT;

    if (inode == NULL)
    {
        inode = inode_new (S_IFREG | (mode & 07777), fuse_get_context ()->uid,
                           get_creation_group_id (parent_inode, fuse_get_context ()->gid));
        if (inode == NULL)
            return -errno;

        prepared_entry = allocate_directory_entry (entry_name, inode);
        if (prepared_entry == NULL)
        {
            inode_maybe_free (inode);
            return -ENOMEM;
        }
    }

    prepared_handle = allocate_file_handle (inode, file_info->flags);
    if (prepared_handle == NULL)
    {
        if (prepared_entry != NULL)
        {
            free_directory_entry (prepared_entry);
            inode_maybe_free (inode);
        }
        return -ENOMEM;
    }

    /* Namespace and handle publication cannot fail after this point. */
    if (prepared_entry != NULL)
        publish_directory_entry (parent_inode, prepared_entry);
    publish_file_handle (prepared_handle);

    /* Offset by one so that 0 keeps meaning "no handle". */
    file_info->fh = prepared_handle->id + 1;

    return 0;
}

static int open_file_locked (const char * path, struct fuse_file_info * file_info)
{
    struct mem_inode * inode = resolve (path, NULL, NULL);
    uint64_t id;

    if (inode == NULL)
        return -ENOENT;

    if (S_ISDIR (inode->mode))
        return -EISDIR;

    id = open_file_handle (inode, file_info->flags);
    if (id == 0)
        return -ENOMEM;

    file_info->fh = id + 1;

    return 0;
}

static int open_directory_locked (const char * path, struct fuse_file_info * file_info)
{
    struct mem_inode * inode = resolve (path, NULL, NULL);
    uint64_t id;
    if (inode == NULL)
        return -ENOENT;

    if (!S_ISDIR (inode->mode))
        return -ENOTDIR;

    id = open_file_handle (inode, file_info->flags);
    if (id == 0)
        return -ENOMEM;

    file_info->fh = id + 1;
    return 0;
}

static int read_file_locked (const char * path, char * buffer, size_t requested_bytes, const off_t offset, struct fuse_file_info * file_info)
{
    struct mem_inode * inode = find_operation_inode (path, file_info);
    struct mem_handle * file_handle = (file_info != NULL && file_info->fh != 0) ? find_file_handle (file_info->fh - 1) : NULL;
    size_t available_bytes;

    if (inode == NULL)
        return -ENOENT;

    if (S_ISDIR (inode->mode))
        return -EISDIR;

    if (file_handle != NULL && (file_handle->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;

    if ((size_t)offset >= inode->size)
        return 0;

    available_bytes = inode->size - (size_t)offset;
    if (requested_bytes > available_bytes)
        requested_bytes = available_bytes;

    memcpy (buffer, inode->data + offset, requested_bytes);
    read_current_time (&inode->atime);

    return (int)requested_bytes;
}

static int create_symbolic_link_locked (const char * target_path, const char * path)
{
    struct mem_inode * parent_inode;
    struct mem_inode * inode;
    const char * entry_name;
    int operation_result;

    if (strlen (target_path) + 1 > MEMFS_LINK_MAX)
        return -ENAMETOOLONG;

    if (resolve (path, &parent_inode, &entry_name) != NULL)
        return -EEXIST;

    if (parent_inode == NULL)
        return -ENOENT;

    if (entry_name == NULL)
        return -ENOENT;

    /* A symbolic link's own permissions are not consulted; 0777 is what a
       symlink conventionally reports. */
    inode = inode_new (S_IFLNK | 0777, fuse_get_context ()->uid,
                       get_creation_group_id (parent_inode, fuse_get_context ()->gid));
    if (inode == NULL)
        return -errno;

    inode->target = strdup (target_path);
    if (inode->target == NULL)
    {
        inode_maybe_free (inode);
        return -ENOMEM;
    }

    inode->size = strlen (target_path);

    operation_result = add_directory_entry (parent_inode, entry_name, inode);
    if (operation_result != 0)
    {
        inode_maybe_free (inode);
        return operation_result;
    }

    return 0;
}

static int create_hard_link_locked (const char * source_path, const char * destination_path)
{
    char destination_entry_name[MEMFS_NAME_MAX + 1];
    struct mem_inode * destination_directory_inode;
    struct mem_inode * source_inode;
    const char * entry_name;

    source_inode = resolve (source_path, NULL, NULL);
    if (source_inode == NULL)
        return -ENOENT;

    if (S_ISDIR (source_inode->mode))
        return -EPERM;

    if (resolve (destination_path, &destination_directory_inode, &entry_name) != NULL)
        return -EEXIST;

    if (destination_directory_inode == NULL)
        return -ENOENT;

    if (entry_name == NULL)
        return -ENOENT;

    if (strlen (entry_name) >= sizeof (destination_entry_name))
        return -ENAMETOOLONG;

    strcpy (destination_entry_name, entry_name);

    /* One object, one more name: dir_add raises the link count. */
    return add_directory_entry (destination_directory_inode, destination_entry_name, source_inode);
}

static int remove_file_locked (const char * path)
{
    struct mem_inode * parent_inode;
    const char * entry_name;
    struct mem_inode * inode = resolve (path, &parent_inode, &entry_name);
    struct mem_dirent * directory_entry;

    if (inode == NULL)
        return -ENOENT;

    if (parent_inode == NULL)
        return -ENOENT;

    if (S_ISDIR (inode->mode))
        return -EPERM;

    directory_entry = find_directory_entry (parent_inode, entry_name);
    if (directory_entry == NULL)
        return -ENOENT;

    dir_remove (parent_inode, directory_entry);

    /* Only actually released if no handle is still holding it open. */
    inode_maybe_free (inode);

    return 0;
}

static int remove_directory_locked (const char * path)
{
    struct mem_inode * parent_inode;
    const char * entry_name;
    struct mem_inode * inode = resolve (path, &parent_inode, &entry_name);
    struct mem_dirent * directory_entry;

    if (inode == NULL)
        return -ENOENT;

    if (parent_inode == NULL)
        return -ENOENT;

    if (!S_ISDIR (inode->mode))
        return -ENOTDIR;

    if (inode->entries != NULL)
        return -ENOTEMPTY;

    directory_entry = find_directory_entry (parent_inode, entry_name);
    if (directory_entry == NULL)
        return -ENOENT;

    dir_remove (parent_inode, directory_entry);
    inode_maybe_free (inode);

    return 0;
}

static int validate_rename_destination (const struct mem_inode * source_inode, const struct mem_inode * destination_inode, struct mem_inode * destination_directory_inode)
{
    /* Moving a directory beneath itself would cut the subtree loose. */
    if (S_ISDIR (source_inode->mode))
    {
        struct mem_inode * ancestor_inode;

        for (ancestor_inode = destination_directory_inode; ancestor_inode != NULL; ancestor_inode = ancestor_inode->parent)
            if (ancestor_inode == source_inode)
                return -EINVAL;
    }

    if (destination_inode != NULL)
    {
        if (S_ISDIR (destination_inode->mode))
        {
            if (!S_ISDIR (source_inode->mode))
                return -EISDIR;

            if (destination_inode->entries != NULL)
                return -ENOTEMPTY;
        }
        else if (S_ISDIR (source_inode->mode))
        {
            return -ENOTDIR;
        }
    }

    return 0;
}

static int rename_entry_locked (const char * source_path, const char * destination_path, const unsigned int flags)
{
    char source_entry_name[MEMFS_NAME_MAX + 1];
    char destination_entry_name[MEMFS_NAME_MAX + 1];
    struct mem_inode * source_directory_inode;
    struct mem_inode * destination_directory_inode;
    struct mem_inode * source_inode;
    struct mem_inode * destination_inode;
    struct mem_dirent * directory_entry;
    const char * entry_name;
    struct mem_dirent * destination_entry;

    if (flags != 0)
        return -EINVAL;

    /* resolve reports the final name out of one static buffer, so each name
       has to be copied before the next call overwrites it. */
    source_inode = resolve (source_path, &source_directory_inode, &entry_name);
    if (source_inode == NULL)
        return -ENOENT;

    if (source_directory_inode == NULL)
        return -ENOENT;

    if (strlen (entry_name) >= sizeof (source_entry_name))
        return -ENAMETOOLONG;

    strcpy (source_entry_name, entry_name);

    destination_inode = resolve (destination_path, &destination_directory_inode, &entry_name);
    if (destination_directory_inode == NULL)
        return -ENOENT;

    if (entry_name == NULL)
        return -ENOENT;

    if (strlen (entry_name) >= sizeof (destination_entry_name))
        return -ENAMETOOLONG;

    strcpy (destination_entry_name, entry_name);

    /* Two names for one object: POSIX makes this a successful no-op. */
    if (source_inode == destination_inode)
        return 0;

    const int validation_result = validate_rename_destination (source_inode, destination_inode, destination_directory_inode);
    if (validation_result != 0)
        return validation_result;

    /* Reserve the destination before removing either original entry. */
    destination_entry = allocate_directory_entry (destination_entry_name, source_inode);
    if (destination_entry == NULL)
        return -ENOMEM;

    if (destination_inode != NULL)
    {
        directory_entry = find_directory_entry (destination_directory_inode, destination_entry_name);
        if (directory_entry != NULL)
        {
            dir_remove (destination_directory_inode, directory_entry);
            inode_maybe_free (destination_inode);
        }
    }

    publish_directory_entry (destination_directory_inode, destination_entry);

    directory_entry = find_directory_entry (source_directory_inode, source_entry_name);
    if (directory_entry != NULL)
        dir_remove (source_directory_inode, directory_entry);

    return 0;
}

/*
 * The offset is always explicit: O_APPEND is resolved by the library before it
 * gets here, so file_info->flags may carry the flag but the position is already final.
 *
 * A write that does not fit within the capacity is shortened to what does fit
 * rather than refused outright, which is what lets a caller filling the file
 * system see a short count and then ENOSPC, the way a real one behaves.
 */
static int write_file_locked (const char * path, const char * buffer, size_t requested_bytes, const off_t offset, struct fuse_file_info * file_info)
{
    struct mem_inode * inode = find_operation_inode (path, file_info);
    struct mem_handle * file_handle = (file_info != NULL && file_info->fh != 0) ? find_file_handle (file_info->fh - 1) : NULL;
    size_t write_end, maximum_file_capacity;
    int operation_result;

    if (inode == NULL)
        return -ENOENT;

    if (S_ISDIR (inode->mode))
        return -EISDIR;

    if (file_handle != NULL && (file_handle->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;

    if (offset < 0)
        return -EINVAL;

    if (!is_file_accounting_valid (inode))
        return -EIO;

    if (requested_bytes == 0)
        return 0;

    if ((uint64_t)offset > SIZE_MAX)
        return -EFBIG;

    if (requested_bytes > SIZE_MAX - (size_t)offset)
        return -EFBIG;

    write_end = (size_t)offset + requested_bytes;

    /* Existing spare capacity belongs to this file; only new allocation
       consumes the remaining global budget. */
    maximum_file_capacity = inode->cap + (g_max - g_allocated);
    if (write_end > maximum_file_capacity)
    {
        if (maximum_file_capacity <= (size_t)offset)
            return -ENOSPC;

        requested_bytes = maximum_file_capacity - (size_t)offset;
        write_end = maximum_file_capacity;
    }

    operation_result = reserve_file_capacity (inode, write_end);
    if (operation_result != 0)
        return operation_result;

    if (write_end > inode->size)
        resize_file_buffer (inode, write_end); /* zero-fills any hole the offset left */

    memcpy (inode->data + offset, buffer, requested_bytes);
    update_modification_time (inode);

    return (int)requested_bytes;
}

/*
 * Permissions and ownership are stored and reported faithfully, but this file
 * system does not enforce them: it has no business second-guessing the access
 * checks the kernel already made on the caller's behalf.
 */
static int change_file_permissions_locked (const char * path, const mode_t mode, struct fuse_file_info * file_info)
{
    struct mem_inode * inode = find_operation_inode (path, file_info);

    if (inode == NULL)
        return -ENOENT;

    /* Keep the type bits; only the permission bits are being set. */
    inode->mode = (inode->mode & S_IFMT) | (mode & 07777);
    read_current_time (&inode->ctime);

    return 0;
}

static int change_file_owner_locked (const char * path, const uid_t uid, const gid_t gid, struct fuse_file_info * file_info)
{
    struct mem_inode * inode = find_operation_inode (path, file_info);

    if (inode == NULL)
        return -ENOENT;

    /* -1 means "leave this one alone". */
    if (uid != (uid_t)-1)
        inode->uid = uid;
    if (gid != (gid_t)-1)
        inode->gid = gid;

    read_current_time (&inode->ctime);

    return 0;
}

static int truncate_file_locked (const char * path, off_t new_file_size, struct fuse_file_info * file_info)
{
    struct mem_inode * inode = find_operation_inode (path, file_info);
    int operation_result;

    if (inode == NULL)
        return -ENOENT;

    if (S_ISDIR (inode->mode))
        return -EISDIR;

    if (new_file_size < 0)
        return -EINVAL;

    if (!is_file_accounting_valid (inode))
        return -EIO;

    if ((size_t)new_file_size > inode->size)
    {
        operation_result = reserve_file_capacity (inode, (size_t)new_file_size);
        if (operation_result != 0)
            return operation_result;
    }

    /* Growing zero-fills; shrinking refunds only released buffer bytes. */
    resize_file_buffer (inode, (size_t)new_file_size);
    update_modification_time (inode);

    return 0;
}

static int set_file_times_locked (const char * path, const struct timespec requested_times[2], struct fuse_file_info * file_info)
{
    struct mem_inode * inode = find_operation_inode (path, file_info);
    struct timespec current_time;

    if (inode == NULL)
        return -ENOENT;

    read_current_time (&current_time);

    if (requested_times == NULL)
    {
        inode->atime = current_time;
        inode->mtime = current_time;
    }
    else
    {
        if (requested_times[0].tv_nsec == UTIME_NOW)
            inode->atime = current_time;
        else if (requested_times[0].tv_nsec != UTIME_OMIT)
            inode->atime = requested_times[0];

        if (requested_times[1].tv_nsec == UTIME_NOW)
            inode->mtime = current_time;
        else if (requested_times[1].tv_nsec != UTIME_OMIT)
            inode->mtime = requested_times[1];
    }

    inode->ctime = current_time;

    return 0;
}

static int release_file_locked (const char * path, struct fuse_file_info * file_info)
{
    (void)path;

    if (file_info != NULL && file_info->fh != 0)
    {
        handle_close (file_info->fh - 1);
        file_info->fh = 0;
    }

    return 0;
}

static int flush_file (const char * path, struct fuse_file_info * file_info)
{
    (void)path;
    (void)file_info;
    /* Mutations are applied directly to the authoritative in-memory state. */
    return 0;
}

static int synchronize_file (const char * path, const int synchronize_data_only, struct fuse_file_info * file_info)
{
    (void)path;
    (void)synchronize_data_only;
    (void)file_info;
    /* Like tmpfs, this example is intentionally volatile across daemon death,
       but has no deferred data or metadata within its advertised lifetime. */
    return 0;
}

static int synchronize_filesystem (const char * path)
{
    (void)path;
    /* All state is already committed to the in-memory backing store. */
    return 0;
}

static int get_filesystem_statistics_locked (const char * path, struct statvfs * filesystem_statistics)
{
    (void)path;

    if (!is_file_accounting_valid (NULL))
        return -EIO;

    if (g_inode_count > g_inode_max)
        return -EIO;

    memset (filesystem_statistics, 0, sizeof (*filesystem_statistics));

    filesystem_statistics->f_bsize = 4096;
    filesystem_statistics->f_frsize = 4096;
    filesystem_statistics->f_blocks = (fsblkcnt_t) (g_max / 4096);
    filesystem_statistics->f_bfree = (fsblkcnt_t) ((g_max - g_allocated) / 4096);
    filesystem_statistics->f_bavail = filesystem_statistics->f_bfree;
    filesystem_statistics->f_files = (fsfilcnt_t)g_inode_max;
    filesystem_statistics->f_ffree = (fsfilcnt_t) (g_inode_max - g_inode_count);
    filesystem_statistics->f_namemax = MEMFS_NAME_MAX;

    return 0;
}

/* The example backend owns one mutable in-memory tree. libfuse now dispatches
 * callbacks concurrently by default, so serialize access to that tree while
 * still allowing the library and thread-safe backends to run callbacks in
 * parallel. Keeping this lock in the backend matches libfuse's ownership
 * model: callback state synchronization belongs to the filesystem. */
#define MEMFS_LOCKED_CALL(call)               \
    do                                        \
    {                                         \
        int result;                           \
        pthread_mutex_lock (&g_memfs_lock);   \
        result = (call);                      \
        pthread_mutex_unlock (&g_memfs_lock); \
        return result;                        \
    } while (0)

static int get_file_attributes (const char * path, struct stat * file_attributes, struct fuse_file_info * file_info)
{
    MEMFS_LOCKED_CALL (get_file_attributes_locked (path, file_attributes, file_info));
}

static int export_object_id (
    const char * path_or_null,
    const uint64_t backend_dev,
    const uint64_t backend_ino,
    const mode_t backend_type,
    uint64_t * token
)
{
    MEMFS_LOCKED_CALL (export_object_id_locked (path_or_null, backend_dev, backend_ino, backend_type, token));
}

static int resolve_object_id (const uint64_t token, char * path, const size_t path_capacity)
{
    MEMFS_LOCKED_CALL (resolve_object_id_locked (token, path, path_capacity));
}

static int read_symbolic_link (const char * path, char * buffer, size_t size)
{
    MEMFS_LOCKED_CALL (read_symbolic_link_locked (path, buffer, size));
}

static int read_directory (const char * path, void * buffer, fuse_fill_dir_t add_directory_entry_to_buffer, const off_t offset, struct fuse_file_info * file_info, enum fuse_readdir_flags flags)
{
    MEMFS_LOCKED_CALL (read_directory_locked (path, buffer, add_directory_entry_to_buffer, offset, file_info,
                                              flags));
}

static int memfs_mkdir (const char * path, const mode_t mode)
{
    MEMFS_LOCKED_CALL (create_directory_locked (path, mode));
}

static int memfs_create_attr (const char * path, const struct stat * initial_attributes, const unsigned int valid_attribute_mask, enum fuse_create_activation creation_activation, struct stat * attributes, struct fuse_file_info * file_info)
{
    MEMFS_LOCKED_CALL (create_file_with_attributes_locked (path, initial_attributes, valid_attribute_mask, creation_activation, attributes, file_info));
}

static int memfs_create (const char * path, const mode_t mode, struct fuse_file_info * file_info)
{
    MEMFS_LOCKED_CALL (create_file_locked (path, mode, file_info));
}

static int open_file (const char * path, struct fuse_file_info * file_info)
{
    MEMFS_LOCKED_CALL (open_file_locked (path, file_info));
}

static int memfs_opendir (const char * path, struct fuse_file_info * file_info)
{
    MEMFS_LOCKED_CALL (open_directory_locked (path, file_info));
}

static int memfs_read (const char * path, char * buffer, size_t requested_bytes, const off_t offset, struct fuse_file_info * file_info)
{
    MEMFS_LOCKED_CALL (read_file_locked (path, buffer, requested_bytes, offset, file_info));
}

static int memfs_symlink (const char * target, const char * path)
{
    MEMFS_LOCKED_CALL (create_symbolic_link_locked (target, path));
}

static int memfs_link (const char * from, const char * to)
{
    MEMFS_LOCKED_CALL (create_hard_link_locked (from, to));
}

static int memfs_unlink (const char * path)
{
    MEMFS_LOCKED_CALL (remove_file_locked (path));
}

static int remove_directory (const char * path)
{
    MEMFS_LOCKED_CALL (remove_directory_locked (path));
}

static int memfs_rename (const char * source_path, const char * destination_path, const unsigned int flags)
{
    MEMFS_LOCKED_CALL (rename_entry_locked (source_path, destination_path, flags));
}

static int memfs_write (const char * path, const char * buffer, size_t requested_bytes, const off_t offset, struct fuse_file_info * file_info)
{
    MEMFS_LOCKED_CALL (write_file_locked (path, buffer, requested_bytes, offset, file_info));
}

static int change_file_permissions (const char * path, const mode_t mode, struct fuse_file_info * file_info)
{
    MEMFS_LOCKED_CALL (change_file_permissions_locked (path, mode, file_info));
}

static int change_file_owner (const char * path, const uid_t uid, const gid_t gid, struct fuse_file_info * file_info)
{
    MEMFS_LOCKED_CALL (change_file_owner_locked (path, uid, gid, file_info));
}

static int memfs_truncate (const char * path, off_t new_file_size, struct fuse_file_info * file_info)
{
    MEMFS_LOCKED_CALL (truncate_file_locked (path, new_file_size, file_info));
}

static int set_file_times (const char * path, const struct timespec requested_times[2], struct fuse_file_info * file_info)
{
    MEMFS_LOCKED_CALL (set_file_times_locked (path, requested_times, file_info));
}

static int memfs_release (const char * path, struct fuse_file_info * file_info)
{
    MEMFS_LOCKED_CALL (release_file_locked (path, file_info));
}

static int memfs_statfs (const char * path, struct statvfs * filesystem_statistics)
{
    MEMFS_LOCKED_CALL (get_filesystem_statistics_locked (path, filesystem_statistics));
}

#undef MEMFS_LOCKED_CALL

static const struct fuse_operations filesystem_operations = {
    .init = initialize_filesystem,
    .destroy = destroy_filesystem,
    .getattr = get_file_attributes,
    .readlink = read_symbolic_link,
    .readdir = read_directory,
    .mkdir = memfs_mkdir,
    .unlink = memfs_unlink,
    .rmdir = remove_directory,
    .symlink = memfs_symlink,
    .rename = memfs_rename,
    .link = memfs_link,
    .chmod = change_file_permissions,
    .chown = change_file_owner,
    .truncate = memfs_truncate,
    .utimens = set_file_times,
    .create = memfs_create,
    .create_attr = memfs_create_attr,
    .open = open_file,
    .opendir = memfs_opendir,
    .releasedir = memfs_release,
    .read = memfs_read,
    .write = memfs_write,
    .flush = flush_file,
    .fsync = synchronize_file,
    .release = memfs_release,
    .statfs = memfs_statfs,
    .syncfs = synchronize_filesystem,
    .export_id = export_object_id,
    .resolve_id = resolve_object_id,
};

static void print_help (void)
{
    printf ("Supported arguments: [options] [mountpoint]\n\n");
    printf ("File-system specific options:\n"
            "    --size=<MB>         retained file-buffer capacity in megabytes\n"
            "                        (default: 64)\n"
            "                        includes spare buffer capacity, excludes\n"
            "                        allocator overhead and namespace metadata\n"
            "    --inodes=<count>     maximum number of live objects\n"
            "                        (default: 10000)\n"
            "\n"
            "Without a mountpoint argument the file system is mounted at\n"
            "/mnt/<pid>/memfs (created automatically).\n"
            "\n");
}

/* The value following -o is an option list, not a mountpoint. */
static const char * get_mountpoint_argument (const struct fuse_args * arguments)
{
    for (int argument_index = 1; argument_index < arguments->argc;
         argument_index += strcmp (arguments->argv[argument_index], "-o") == 0 ? 2 : 1)
    {
        const char * argument = arguments->argv[argument_index];
        if (argument[0] != '-')
            return argument;
    }

    return NULL;
}

struct automatic_mount_paths
{
    char parent_directory_path[PATH_MAX]; // Process-specific directory containing the mountpoint.
    char mountpoint_path[PATH_MAX];       // Automatically selected mountpoint; empty for an explicit one.
};

static int create_mount_directory (const char * directory_path)
{
    if (mkdir (directory_path, 0755) == 0)
        return 0;

    if (errno == EEXIST)
        return 0;

    fprintf (stderr, "Failed to create mount directory %s: %s\n", directory_path, strerror (errno));
    return -1;
}

static void remove_automatic_mount_directories (const struct automatic_mount_paths * mount_paths)
{
    if (mount_paths->mountpoint_path[0] == '\0')
        return;
    /* Best effort: a killed daemon can leave directories behind. */
    (void)rmdir (mount_paths->mountpoint_path);
    (void)rmdir (mount_paths->parent_directory_path);
}

static int run_filesystem (struct fuse_args * arguments, const struct automatic_mount_paths * mount_paths)
{
    const int command_result = fuse_main (arguments->argc, arguments->argv, &filesystem_operations, NULL);
    fuse_opt_free_args (arguments);
    remove_automatic_mount_directories (mount_paths);
    return command_result;
}

static void add_help_argument (struct fuse_args * arguments)
{
    print_help ();
    assert (fuse_opt_add_arg (arguments, "--help") == 0);
    arguments->argv[0][0] = '\0';
}

static int prepare_mount_arguments (struct fuse_args * arguments, struct automatic_mount_paths * mount_paths)
{
    const char * explicit_mountpoint = get_mountpoint_argument (arguments);
    if (explicit_mountpoint != NULL)
    {
        return 0;
    }

    snprintf (mount_paths->parent_directory_path, sizeof (mount_paths->parent_directory_path), "/mnt/%d", (int)getpid ());
    const int mount_path_length = snprintf (mount_paths->mountpoint_path, sizeof (mount_paths->mountpoint_path), "%s/memfs", mount_paths->parent_directory_path);
    if (mount_path_length < 0)
    {
        fprintf (stderr, "Failed to format automatic mountpoint path: %s\n", strerror (errno));
        return -1;
    }

    if ((size_t)mount_path_length >= sizeof (mount_paths->mountpoint_path))
    {
        fprintf (stderr, "Failed to format automatic mountpoint path: path exceeds %lu bytes\n", (unsigned long)sizeof (mount_paths->mountpoint_path) - 1);
        return -1;
    }

    if (create_mount_directory (mount_paths->parent_directory_path) != 0)
        return -1;

    if (create_mount_directory (mount_paths->mountpoint_path) != 0)
        return -1;

    assert (fuse_opt_add_arg (arguments, mount_paths->mountpoint_path) == 0);
    printf ("Mounting memory filesystem at %s: capacity=%d MB inode_limit=%d\n", mount_paths->mountpoint_path, options.size_mb, options.inode_limit);
    fflush (stdout);
    return 0;
}

static int initialize_memory_filesystem (void)
{
    if (options.size_mb <= 0)
    {
        fprintf (stderr, "Failed to set filesystem capacity: --size must be positive (received %d)\n", options.size_mb);
        return -1;
    }

    if (options.inode_limit <= 0)
    {
        fprintf (stderr, "Failed to set inode limit: --inodes must be between 1 and 1000000 (received %d)\n", options.inode_limit);
        return -1;
    }

    if (options.inode_limit > 1000000)
    {
        fprintf (stderr, "Failed to set inode limit: --inodes must be between 1 and 1000000 (received %d)\n", options.inode_limit);
        return -1;
    }
    g_max = (size_t)options.size_mb * 1024u * 1024u;
    g_inode_max = (uint64_t)options.inode_limit;
    g_root = inode_new (S_IFDIR | 0755, getuid (), getgid ());
    if (g_root == NULL)
    {
        fprintf (stderr, "Failed to allocate root directory: %s\n", strerror (errno));
        return -1;
    }
    g_root->nlink = 2; // The root directory and its own dot entry.
    g_root->parent = NULL;
    return 0;
}

int main (const int argc, char * argv[])
{
    struct fuse_args arguments = FUSE_ARGS_INIT (argc, argv);
    struct automatic_mount_paths mount_paths = {0};
    options.size_mb = 64;
    options.inode_limit = 10000;
    if (fuse_opt_parse (&arguments, &options, option_specs, NULL) == -1)
        return 1;

    if (options.show_help_requested)
    {
        add_help_argument (&arguments);
        return run_filesystem (&arguments, &mount_paths);
    }

    if (initialize_memory_filesystem () != 0)
    {
        fuse_opt_free_args (&arguments);
        return 2;
    }

    if (prepare_mount_arguments (&arguments, &mount_paths) != 0)
    {
        fuse_opt_free_args (&arguments);
        return 2;
    }

    return run_filesystem (&arguments, &mount_paths);
}

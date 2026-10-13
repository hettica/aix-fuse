/*
 * AIX FUSE compatibility adapter over the native USFS client API.
 * Copyright (c) 2026 Raman Dzehtsiar
 * Copyright (C) 2001-2007 Miklos Szeredi <miklos@szeredi.hu>
 * SPDX-License-Identifier: MIT AND LGPL-2.1-only
 *
 * The startup path retains material conservatively treated as adapted from
 * libfuse helper.c. Separated and modified on 2026-10-08. See LICENSE.
 */
#define FUSE_USE_VERSION 31
#include "fuse.h"
#include <usfs/usfs.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUSE_MT_DEFAULT_THREADS USFS_CLIENT_DEFAULT_WORKERS
#define FUSE_MT_MAX_THREADS     USFS_CLIENT_MAX_WORKERS
#define FUSE_DECIMAL_RADIX      10

enum fuse_mount_mode
{
    FUSE_MOUNT_MODE_AUTOMATIC = 0,
    FUSE_MOUNT_MODE_READ_ONLY = 1,
    FUSE_MOUNT_MODE_READ_WRITE = 2
};

struct fuse_session
{
    struct usfs_client * client; // Borrowed native client associated with this session.
};

struct fuse
{
    struct usfs_client * client; // Owned native request engine.
    struct fuse_session session; // Public session wrapper without native internals.
    struct fuse_operations ops;  // Copied caller-supplied FUSE callbacks.
    struct fuse_conn_info conn;  // FUSE-facing capability and limit values.
    struct fuse_config config;   // FUSE-facing configuration retained across initialization.
    void * user_data;            // Initial or init-returned filesystem private data.
    int mounted;                 // Whether native unmount still needs to succeed.
};

struct callback_scope
{
    struct fuse_context context;      // Context borrowed until this callback returns.
    struct fuse_object_identity object_identity; // Storage for the selected native object.
    pthread_t owner;                  // Thread executing this callback.
    struct callback_scope * previous; // Newer active callback in the registry.
    struct callback_scope * next;     // Older active callback in the registry.
    int active;                       // Whether this stack scope is registered.
};

/* Stack-owned scopes avoid an allocation after the native engine has already
 * detached a handle for release. The registry lock never covers user code;
 * the newest scope belonging to a thread is that thread's current context. */
static pthread_mutex_t callback_scope_lock = PTHREAD_MUTEX_INITIALIZER;
static struct callback_scope * callback_scopes;

static void leave_callback (struct callback_scope * scope)
{
    if (!scope->active)
        return;

    pthread_mutex_lock (&callback_scope_lock);

    if (scope->previous != NULL)
        scope->previous->next = scope->next;
    else
        callback_scopes = scope->next;

    if (scope->next != NULL)
        scope->next->previous = scope->previous;

    scope->active = false;
    pthread_mutex_unlock (&callback_scope_lock);
}

static struct fuse_object_identity adapt_object_identity (const struct usfs_object_identity * native)
{
    struct fuse_object_identity identity = { 0 };
    identity.nodeid = native->nodeid;
    identity.backend_dev = native->backend_dev;
    identity.backend_ino = native->backend_ino;
    identity.backend_type = native->backend_type;
    identity.has_backend_identity = native->has_backend_identity;

    return identity;
}

static void enter_callback (struct fuse * fuse, const struct usfs_client_request * request, struct callback_scope * scope)
{
    struct fuse_context context = { 0 };
    context.fuse = fuse;
    context.private_data = fuse->user_data;
    context.uid = usfs_request_uid (request);
    context.gid = usfs_request_gid (request);
    context.pid = usfs_request_pid (request);

    const struct usfs_object_identity * native_identity = usfs_request_object_identity (request);
    if (native_identity != NULL)
    {
        scope->object_identity = adapt_object_identity (native_identity);
        context.object_identity = &scope->object_identity;
    }

    scope->context = context;
    scope->owner = pthread_self ();

    pthread_mutex_lock (&callback_scope_lock);
    scope->previous = NULL;
    scope->next = callback_scopes;
    if (callback_scopes != NULL)
        callback_scopes->previous = scope;

    callback_scopes = scope;
    scope->active = true;
    pthread_mutex_unlock (&callback_scope_lock);
}

struct fuse_context * fuse_get_context (void)
{
    const pthread_t owner = pthread_self ();
    struct fuse_context * context = NULL;

    pthread_mutex_lock (&callback_scope_lock);
    for (struct callback_scope * scope = callback_scopes; scope != NULL; scope = scope->next)
    {
        if (!pthread_equal (scope->owner, owner))
            continue;

        context = &scope->context;
        break;
    }
    pthread_mutex_unlock (&callback_scope_lock);

    return context;
}

static struct fuse_file_info * import_open_file (struct fuse_file_info * adapted, const struct usfs_open_file * file)
{
    if (file == NULL)
        return NULL;

    adapted->fh = file->value;
    adapted->flags = file->open_flags;
    return adapted;
}

static void export_open_file (struct usfs_open_file * file, const struct fuse_file_info * adapted)
{
    if (file == NULL)
        return;

    file->value = adapted->fh;
    file->open_flags = adapted->flags;
}

static void forward_diagnostic (void * user_data, const char * message)
{
    (void)user_data;
    fuse_log (FUSE_LOG_ERR, "%s", message);
}

static int initialize_backend (const struct usfs_client_request * request, const struct usfs_limits * limits, struct usfs_behavior * behavior)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    fuse->conn.proto_major = FUSE_MAJOR_VERSION;
    fuse->conn.proto_minor = FUSE_MINOR_VERSION;
    fuse->conn.max_read = limits->max_read;
    fuse->conn.max_write = limits->max_write;
    fuse->conn.capable = (uint32_t)FUSE_CAP_ASYNC_READ;
    fuse->conn.capable_ext = FUSE_CAP_ASYNC_READ;

    if (fuse->ops.init != NULL)
        fuse->user_data = fuse->ops.init (&fuse->conn, &fuse->config);

    behavior->remove_policy = fuse->config.hard_remove ? USFS_REMOVE_IMMEDIATE : USFS_REMOVE_DEFERRED;
    behavior->handle_paths = fuse->config.nullpath_ok ? USFS_PATH_OMIT_FOR_HANDLE : USFS_PATH_DEFAULT;
    return 0;
}

static void shutdown_backend (const struct usfs_client_request * request)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    fuse->ops.destroy (fuse->user_data);
}

static int fill_directory (void * buffer, const char * name, const struct stat * metadata, off_t offset, enum fuse_fill_dir_flags flags)
{
    (void)offset;
    (void)flags;
    return usfs_directory_add (buffer, name, metadata) == 0 ? 0 : 1;
}

static unsigned int translate_initial_attributes (const unsigned int fields)
{
    unsigned int result = 0;
    if (fields & USFS_INITIAL_MODE)
        result |= FUSE_INITIAL_MODE;
    if (fields & USFS_INITIAL_UID)
        result |= FUSE_INITIAL_UID;
    if (fields & USFS_INITIAL_GID)
        result |= FUSE_INITIAL_GID;
    if (fields & USFS_INITIAL_SIZE)
        result |= FUSE_INITIAL_SIZE;
    if (fields & USFS_INITIAL_ATIME)
        result |= FUSE_INITIAL_ATIME;
    if (fields & USFS_INITIAL_MTIME)
        result |= FUSE_INITIAL_MTIME;
    if (fields & USFS_INITIAL_CTIME)
        result |= FUSE_INITIAL_CTIME;
    return result;
}

static enum fuse_create_activation translate_activation (const enum usfs_create_action action)
{
    if (action == USFS_CREATE_WITH_OPEN)
        return FUSE_CREATE_OPEN;
    if (action == USFS_CREATE_FOR_LOOKUP)
        return FUSE_CREATE_LOOKUP;
    return FUSE_CREATE_DEFAULT;
}

static void translate_timestamps (const struct timespec input[2], struct timespec output[2])
{
    for (size_t index = 0; index < 2; ++index)
    {
        output[index].tv_sec = input[index].tv_sec;
        output[index].tv_nsec = input[index].tv_nsec;
        if (input[index].tv_nsec == USFS_TIME_NOW)
            output[index].tv_nsec = UTIME_NOW;
        if (input[index].tv_nsec == USFS_TIME_OMIT)
            output[index].tv_nsec = UTIME_OMIT;
    }
}

static int bridge_getattr (const struct usfs_client_request * request, const char * path, struct stat * metadata, struct usfs_open_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result = fuse->ops.getattr (path, metadata, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_statfs (const struct usfs_client_request * request, const char * path, struct statvfs * metadata)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    const int result = fuse->ops.statfs (path, metadata);

    return result;
}

static int bridge_readlink (const struct usfs_client_request * request, const char * path, char * target, size_t capacity)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    const int result = fuse->ops.readlink (path, target, capacity);

    return result;
}

static int bridge_chmod (const struct usfs_client_request * request, const char * path, mode_t mode, struct usfs_open_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result = fuse->ops.chmod (path, mode, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_chown (const struct usfs_client_request * request, const char * path, uid_t owner, gid_t group, struct usfs_open_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result = fuse->ops.chown (path, owner, group, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_truncate (const struct usfs_client_request * request, const char * path, off_t length, struct usfs_open_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result = fuse->ops.truncate (path, length, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_utimens (
    const struct usfs_client_request * request,
    const char * path,
    const struct timespec times[2],
    struct usfs_open_file * file
)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    struct timespec translated_times[2] = { 0 };

    translate_timestamps (times, translated_times);

    const int result = fuse->ops.utimens (path, translated_times, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_mkdir (const struct usfs_client_request * request, const char * path, mode_t mode)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    const int result = fuse->ops.mkdir (path, mode);

    return result;
}

static struct fuse_expected_entry adapt_expected_entry (const struct usfs_expected_entry * native)
{
    struct fuse_expected_entry expected = { 0 };
    expected.path = native->path;
    expected.parent = adapt_object_identity (&native->parent);
    expected.object = adapt_object_identity (&native->object);
    expected.exists = native->exists;

    return expected;
}

static int bridge_rmdir (const struct usfs_client_request * request, const struct usfs_expected_entry * native)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    const struct fuse_expected_entry expected = adapt_expected_entry (native);
    const int result = fuse->ops.rmdir_checked (&expected);

    return result;
}

static int bridge_unlink (const struct usfs_client_request * request, const struct usfs_expected_entry * native)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    const struct fuse_expected_entry expected = adapt_expected_entry (native);
    const int result = fuse->ops.unlink_checked (&expected);

    return result;
}

static int bridge_rename (
    const struct usfs_client_request * request,
    const struct usfs_expected_entry * native_source,
    const struct usfs_expected_entry * native_destination
)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    const struct fuse_expected_entry source = adapt_expected_entry (native_source);
    const struct fuse_expected_entry destination = adapt_expected_entry (native_destination);
    const int result = fuse->ops.rename_checked (&source, &destination);

    return result;
}

static int bridge_link (const struct usfs_client_request * request, const char * old_path, const char * new_path)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    const int result = fuse->ops.link (old_path, new_path);

    return result;
}

static int bridge_symlink (const struct usfs_client_request * request, const char * target, const char * path)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    const int result = fuse->ops.symlink (target, path);

    return result;
}

static int bridge_create (const struct usfs_client_request * request, const char * path, mode_t mode, struct usfs_open_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result = fuse->ops.create (path, mode, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_create_attr (
    const struct usfs_client_request * request,
    const char * path,
    const struct stat * initial,
    unsigned int fields,
    enum usfs_create_action action,
    struct stat * metadata,
    struct usfs_open_file * file
)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result =
        fuse->ops.create_attr (path, initial, translate_initial_attributes (fields), translate_activation (action), metadata, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_open (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result = fuse->ops.open (path, file_info);

    export_open_file (file, &adapted);

    return result;
}

static ssize_t bridge_read (
    const struct usfs_client_request * request,
    const char * path,
    char * data,
    size_t length,
    off_t offset,
    struct usfs_open_file * file
)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const ssize_t result = fuse->ops.read (path, data, length, offset, file_info);

    export_open_file (file, &adapted);

    return result;
}

static ssize_t bridge_write (
    const struct usfs_client_request * request,
    const char * path,
    const char * data,
    size_t length,
    off_t offset,
    struct usfs_open_file * file
)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const ssize_t result = fuse->ops.write (path, data, length, offset, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_flush (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result = fuse->ops.flush (path, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_fsync (const struct usfs_client_request * request, const char * path, int data_only, struct usfs_open_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result = fuse->ops.fsync (path, data_only, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_release (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result = fuse->ops.release (path, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_pager_acquire (const struct usfs_client_request * request, const char * path, struct usfs_pager_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_pager_file adapted = { 0 };
    const int result = fuse->ops.pager_acquire (path, scope.context.object_identity, &adapted);
    if (result == 0)
        file->value = adapted.value;

    return result;
}

static int bridge_pager_authorize (const struct usfs_client_request * request, enum usfs_pager_access access)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    return fuse->ops.pager_authorize (scope.context.object_identity, (enum fuse_pager_access)access);
}

static int bridge_pager_enable_write (const struct usfs_client_request * request, struct usfs_pager_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_pager_file adapted = { .value = file->value };
    const int result = fuse->ops.pager_enable_write (&adapted);
    file->value = adapted.value;

    return result;
}

static ssize_t bridge_pager_read (
    const struct usfs_client_request * request,
    char * buffer,
    size_t size,
    off_t offset,
    struct usfs_pager_file * file
)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_pager_file adapted = { .value = file->value };
    const ssize_t result = fuse->ops.pager_read (buffer, size, offset, &adapted);
    file->value = adapted.value;

    return result;
}

static ssize_t bridge_pager_write (
    const struct usfs_client_request * request,
    const char * buffer,
    size_t size,
    off_t offset,
    struct usfs_pager_file * file
)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_pager_file adapted = { .value = file->value };
    const ssize_t result = fuse->ops.pager_write (buffer, size, offset, &adapted);
    file->value = adapted.value;

    return result;
}

static int bridge_pager_release (const struct usfs_client_request * request, struct usfs_pager_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_pager_file adapted = { .value = file->value };
    const int result = fuse->ops.pager_release (&adapted);
    file->value = adapted.value;

    return result;
}

static int bridge_syncfs (const struct usfs_client_request * request, const char * path)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    const int result = fuse->ops.syncfs (path);

    return result;
}

static int bridge_opendir (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result = fuse->ops.opendir (path, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_readdir (
    const struct usfs_client_request * request,
    const char * path,
    const struct usfs_object_identity * identity,
    struct usfs_directory_sink * sink
)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    scope.context.object_identity = NULL;
    if (identity != NULL)
    {
        scope.object_identity = adapt_object_identity (identity);
        scope.context.object_identity = &scope.object_identity;
    }

    const int result = fuse->ops.readdir (path, sink, fill_directory, 0, NULL, FUSE_READDIR_DEFAULTS);

    return result;
}

static int bridge_fsyncdir (const struct usfs_client_request * request, const char * path, int data_only, struct usfs_open_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result = fuse->ops.fsyncdir (path, data_only, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_releasedir (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    struct fuse_file_info adapted = { 0 };
    struct fuse_file_info * file_info = import_open_file (&adapted, file);

    const int result = fuse->ops.releasedir (path, file_info);

    export_open_file (file, &adapted);

    return result;
}

static int bridge_export_id (
    const struct usfs_client_request * request,
    const char * path,
    uint64_t device,
    uint64_t inode,
    mode_t type,
    uint64_t * token
)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    const int result = fuse->ops.export_id (path, device, inode, type, token);

    return result;
}

static int bridge_resolve_id (const struct usfs_client_request * request, uint64_t token, char * path, size_t capacity)
{
    struct fuse * fuse = usfs_request_user_data (request);
    struct callback_scope scope __attribute__ ((cleanup (leave_callback))) = { 0 };
    enter_callback (fuse, request, &scope);

    const int result = fuse->ops.resolve_id (token, path, capacity);

    return result;
}

static void select_native_operations (const struct fuse * fuse, struct usfs_operations * operations)
{
    operations->initialize = initialize_backend;
    if (fuse->ops.destroy != NULL)
        operations->shutdown = shutdown_backend;

    if (fuse->ops.getattr != NULL)
        operations->getattr = bridge_getattr;

    if (fuse->ops.statfs != NULL)
        operations->statfs = bridge_statfs;

    if (fuse->ops.readlink != NULL)
        operations->readlink = bridge_readlink;

    if (fuse->ops.chmod != NULL)
        operations->chmod = bridge_chmod;

    if (fuse->ops.chown != NULL)
        operations->chown = bridge_chown;

    if (fuse->ops.truncate != NULL)
        operations->truncate = bridge_truncate;

    if (fuse->ops.utimens != NULL)
        operations->utimens = bridge_utimens;

    if (fuse->ops.mkdir != NULL)
        operations->mkdir = bridge_mkdir;

    if (fuse->ops.rmdir_checked != NULL)
        operations->rmdir = bridge_rmdir;

    if (fuse->ops.unlink_checked != NULL)
        operations->unlink = bridge_unlink;

    if (fuse->ops.rename_checked != NULL)
        operations->rename = bridge_rename;

    if (fuse->ops.link != NULL)
        operations->link = bridge_link;

    if (fuse->ops.symlink != NULL)
        operations->symlink = bridge_symlink;

    if (fuse->ops.create != NULL)
        operations->create = bridge_create;

    if (fuse->ops.create_attr != NULL)
        operations->create_attr = bridge_create_attr;

    if (fuse->ops.open != NULL)
        operations->open = bridge_open;

    if (fuse->ops.read != NULL)
        operations->read = bridge_read;

    if (fuse->ops.write != NULL)
        operations->write = bridge_write;

    if (fuse->ops.flush != NULL)
        operations->flush = bridge_flush;

    if (fuse->ops.fsync != NULL)
        operations->fsync = bridge_fsync;

    if (fuse->ops.release != NULL)
        operations->release = bridge_release;

    if (fuse->ops.pager_acquire != NULL)
        operations->pager_acquire = bridge_pager_acquire;

    if (fuse->ops.pager_authorize != NULL)
        operations->pager_authorize = bridge_pager_authorize;

    if (fuse->ops.pager_enable_write != NULL)
        operations->pager_enable_write = bridge_pager_enable_write;

    if (fuse->ops.pager_read != NULL)
        operations->pager_read = bridge_pager_read;

    if (fuse->ops.pager_write != NULL)
        operations->pager_write = bridge_pager_write;

    if (fuse->ops.pager_release != NULL)
        operations->pager_release = bridge_pager_release;

    if (fuse->ops.syncfs != NULL)
        operations->syncfs = bridge_syncfs;

    if (fuse->ops.opendir != NULL)
        operations->opendir = bridge_opendir;

    if (fuse->ops.readdir != NULL)
        operations->readdir = bridge_readdir;

    if (fuse->ops.fsyncdir != NULL)
        operations->fsyncdir = bridge_fsyncdir;

    if (fuse->ops.releasedir != NULL)
        operations->releasedir = bridge_releasedir;

    if (fuse->ops.export_id != NULL)
        operations->export_id = bridge_export_id;

    if (fuse->ops.resolve_id != NULL)
        operations->resolve_id = bridge_resolve_id;
}

static int parse_command_options (
    const struct fuse_args * args,
    struct fuse_cmdline_opts * options,
    int * mount_mode,
    struct fuse_config * configuration
);

static void copy_fuse_operations (struct fuse * fuse, const struct fuse_operations * operations, size_t size)
{
    if (size > sizeof (fuse->ops))
        size = sizeof (fuse->ops);

    if (size != 0)
        memcpy (&fuse->ops, operations, size);

    /* The public FUSE table consists of pointers; discard an incomplete tail. */
    const size_t remainder = size % sizeof (fuse->ops.getattr);
    if (remainder != 0)
        memset ((char *)&fuse->ops + size - remainder, 0, remainder);
}

struct fuse * _fuse_new_31 (
    struct fuse_args * args,
    const struct fuse_operations * operations,
    size_t operations_size,
    struct libfuse_version * version,
    void * user_data
)
{
    (void)version;
    if (operations == NULL && operations_size != 0)
    {
        errno = EINVAL;
        return NULL;
    }

    struct fuse_cmdline_opts command_options = { 0 };
    struct fuse_config configuration = { 0 };
    int mount_mode = FUSE_MOUNT_MODE_AUTOMATIC;

    if (args != NULL)
    {
        const int result = parse_command_options (args, &command_options, &mount_mode, &configuration);
        free (command_options.mountpoint);
        if (result != 0)
            return NULL;
    }

    struct fuse * fuse = calloc (1, sizeof (*fuse));
    if (fuse == NULL)
        return NULL;

    copy_fuse_operations (fuse, operations, operations_size);
    fuse->config = configuration;
    fuse->user_data = user_data;
    struct usfs_operations native_operations = { 0 };
    struct usfs_client_options native_options = { 0 };

    select_native_operations (fuse, &native_operations);
    native_options.diagnostic = forward_diagnostic;
    native_options.behavior.remove_policy = configuration.hard_remove ? USFS_REMOVE_IMMEDIATE : USFS_REMOVE_DEFERRED;
    native_options.behavior.handle_paths = configuration.nullpath_ok ? USFS_PATH_OMIT_FOR_HANDLE : USFS_PATH_DEFAULT;
    if (mount_mode == FUSE_MOUNT_MODE_READ_ONLY)
        native_options.mount_access = USFS_MOUNT_READ_ONLY;
    if (mount_mode == FUSE_MOUNT_MODE_READ_WRITE)
        native_options.mount_access = USFS_MOUNT_READ_WRITE;

    const int result =
        usfs_client_create (USFS_CLIENT_API_VERSION, &native_operations, sizeof (native_operations), &native_options, fuse, &fuse->client);
    if (result != 0)
    {
        free (fuse);
        errno = -result;
        return NULL;
    }

    fuse->session.client = fuse->client;
    return fuse;
}

int fuse_version (void)
{
    return FUSE_VERSION;
}

const char * fuse_pkgversion (void)
{
    return "3.19.0-aix-usfs";
}

int fuse_daemonize (const int foreground)
{
    (void)foreground;
    return 0;
}

struct fuse_session * fuse_get_session (const struct fuse * fuse)
{
    return &((struct fuse *)(uintptr_t)fuse)->session;
}

void fuse_exit (struct fuse * fuse)
{
    if (fuse != NULL)
        usfs_client_request_stop (fuse->client);
}

int fuse_set_signal_handlers (struct fuse_session * session)
{
    if (session == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    const int result = usfs_client_install_signals (session->client);
    if (result != 0)
    {
        errno = -result;
        return -1;
    }

    return 0;
}

void fuse_remove_signal_handlers (const struct fuse_session * session)
{
    if (session != NULL)
        usfs_client_remove_signals (session->client);
}

int fuse_mount (struct fuse * fuse, const char * mountpoint)
{
    if (fuse == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    const int result = usfs_client_mount (fuse->client, mountpoint);
    if (result != 0)
    {
        errno = -result;
        return -1;
    }

    fuse->mounted = true;
    return 0;
}

void fuse_unmount (struct fuse * fuse)
{
    errno = 0;
    if (fuse == NULL)
        return;

    const int result = usfs_client_unmount (fuse->client);
    if (result != 0)
    {
        errno = -result;
        return;
    }

    fuse->mounted = false;
    errno = 0;
}

void fuse_destroy (struct fuse * fuse)
{
    errno = 0;
    if (fuse == NULL)
        return;

    if (fuse->mounted)
    {
        fuse_log (FUSE_LOG_ERR, "Failed to destroy filesystem: mount cleanup is unresolved\n");
        errno = EBUSY;
        return;
    }

    const int result = usfs_client_destroy (&fuse->client);
    if (fuse->client == NULL)
        free (fuse);

    errno = result == 0 ? 0 : -result;
}

int fuse_loop (struct fuse * fuse)
{
    if (fuse == NULL)
        return -EINVAL;

    return usfs_client_run (fuse->client, 1);
}

static int run_multithreaded_session (struct fuse * fuse, const unsigned int worker_count)
{
    if (fuse == NULL)
        return -EINVAL;

    return usfs_client_run (fuse->client, worker_count);
}

int fuse_loop_mt_31 (struct fuse * fuse, const int clone_fd)
{
    (void)clone_fd;
    return run_multithreaded_session (fuse, FUSE_MT_DEFAULT_THREADS);
}

#undef fuse_loop_mt
int fuse_loop_mt (struct fuse * fuse, struct fuse_loop_config * config)
{
    (void)config;
    return run_multithreaded_session (fuse, FUSE_MT_DEFAULT_THREADS);
}

/* Preserve the established fuse_main exit-status values. */
enum fuse_main_result
{
    FUSE_MAIN_SUCCESS = 0,
    FUSE_MAIN_INVALID_ARGUMENTS = 1,
    FUSE_MAIN_MISSING_MOUNTPOINT = 2,
    FUSE_MAIN_CREATE_FAILED = 3,
    FUSE_MAIN_MOUNT_FAILED = 4,
    FUSE_MAIN_SIGNAL_HANDLERS_FAILED = 6,
    FUSE_MAIN_LOOP_FAILED = 8,
    FUSE_MAIN_UNMOUNT_FAILED = 9,
    FUSE_MAIN_CLEANUP_FAILED = 10
};

void fuse_cmdline_help (void)
{
    printf ("    -h   --help            print help\n"
            "    -V   --version         print version\n"
            "    -d   -o debug          enable debug output (implies -f)\n"
            "    -f                     foreground operation (always on)\n"
            "    -s                     disable multi-threaded operation\n"
            "    -o max_threads=N       bound worker threads (1..64, default 10)\n"
            "    -o ro | -o rw          request read-only or read/write access\n");
}

void fuse_lib_help (struct fuse_args * args)
{
    (void)args;
    fuse_cmdline_help ();
}

static int set_mount_access_mode (int * mount_mode, const int requested_mode)
{
    if (*mount_mode != FUSE_MOUNT_MODE_AUTOMATIC && *mount_mode != requested_mode)
    {
        errno = EINVAL;
        return -1;
    }

    *mount_mode = requested_mode;

    return 0;
}

static int parse_max_threads_option (struct fuse_cmdline_opts * options, const char * thread_count_text)
{
    char * thread_count_end;

    errno = 0;

    const unsigned long thread_count = strtoul (thread_count_text, &thread_count_end, FUSE_DECIMAL_RADIX);

    if (errno != 0)
        return -1;

    if (*thread_count_end != '\0')
        return -1;

    if (thread_count == 0)
        return -1;

    if (thread_count > FUSE_MT_MAX_THREADS)
        return -1;

    options->max_threads = (unsigned int)thread_count;

    return 0;
}

static int parse_mount_option (struct fuse_cmdline_opts * options, const char * option, int * mount_mode, struct fuse_config * configuration)
{
    static const char max_threads_prefix[] = "max_threads=";

    if (strcmp (option, "hard_remove") == 0 || strcmp (option, "nullpath_ok") == 0)
    {
        if (configuration != NULL)
        {
            if (strcmp (option, "hard_remove") == 0)
                configuration->hard_remove = 1;
            else
                configuration->nullpath_ok = 1;
        }

        return 0;
    }

    if (strcmp (option, "ro") == 0)
        return set_mount_access_mode (mount_mode, FUSE_MOUNT_MODE_READ_ONLY);

    if (strcmp (option, "rw") == 0)
        return set_mount_access_mode (mount_mode, FUSE_MOUNT_MODE_READ_WRITE);

    if (strcmp (option, "debug") == 0)
    {
        options->foreground = 1;
        options->debug = 1;
        return 0;
    }

    if (strncmp (option, max_threads_prefix, sizeof (max_threads_prefix) - 1) == 0)
        return parse_max_threads_option (options, option + sizeof (max_threads_prefix) - 1);

    fuse_log (FUSE_LOG_ERR, "Failed to parse mount options: unsupported mount option `%s'\n", option);
    errno = EINVAL;

    return -1;
}

static int is_mount_option_list_valid (const char * text)
{
    if (text == NULL)
        return false;

    if (text[0] == '\0')
        return false;

    if (text[0] == ',')
        return false;

    if (text[strlen (text) - 1] == ',')
        return false;

    if (strstr (text, ",,") != NULL)
        return false;

    return true;
}

static int parse_mount_option_list (struct fuse_cmdline_opts * options, const char * text, int * mount_mode, struct fuse_config * configuration)
{
    char * next_option = NULL;
    int result = 0;

    if (!is_mount_option_list_valid (text))
    {
        errno = EINVAL;
        return -1;
    }

    char * option_list = strdup (text);
    if (option_list == NULL)
        return -1;

    for (const char * option = strtok_r (option_list, ",", &next_option); option != NULL; option = strtok_r (NULL, ",", &next_option))
    {
        result = parse_mount_option (options, option, mount_mode, configuration);

        if (result != 0)
            break;
    }

    free (option_list);

    return result;
}

static int apply_command_flag (struct fuse_cmdline_opts * options, const char * argument)
{
    if (strcmp (argument, "-h") == 0 || strcmp (argument, "--help") == 0)
    {
        options->show_help = 1;
    }
    else if (strcmp (argument, "-V") == 0 || strcmp (argument, "--version") == 0)
    {
        options->show_version = 1;
    }
    else if (strcmp (argument, "-d") == 0)
    {
        options->debug = 1;
        options->foreground = 1;
    }
    else if (strcmp (argument, "-f") == 0)
    {
        options->foreground = 1;
    }
    else if (strcmp (argument, "-s") == 0)
    {
        options->singlethread = 1;
    }
    else
    {
        return false;
    }

    return true;
}

struct command_parser
{
    const struct fuse_args * args;      // Borrowed command-line arguments.
    struct fuse_cmdline_opts * options; // Caller-owned parsed command options.
    int * mount_mode;                   // Caller-owned mount access selection.
    struct fuse_config * configuration; // Optional callback configuration initialized from options.
};

static int parse_mount_option_argument (const struct command_parser * parser, int * argument_index, const char * option_text)
{
    if (option_text[0] == '\0')
    {
        ++*argument_index;

        if (*argument_index >= parser->args->argc)
            return -1;

        option_text = parser->args->argv[*argument_index];
    }

    return parse_mount_option_list (parser->options, option_text, parser->mount_mode, parser->configuration);
}

static int parse_mountpoint_argument (struct fuse_cmdline_opts * options, const char * argument)
{
    if (options->mountpoint != NULL)
    {
        fuse_log (FUSE_LOG_ERR, "Failed to parse command line: invalid argument `%s'\n", argument);
        return -1;
    }

    options->mountpoint = strdup (argument);
    if (options->mountpoint == NULL)
        return -1;

    return 0;
}

static int parse_command_argument (const struct command_parser * parser, int * argument_index)
{
    static const char mount_option_prefix[] = "-o";
    const char * argument = parser->args->argv[*argument_index];

    if (argument == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (apply_command_flag (parser->options, argument))
        return 0;

    if (strncmp (argument, mount_option_prefix, sizeof (mount_option_prefix) - 1) == 0)
        return parse_mount_option_argument (parser, argument_index, argument + sizeof (mount_option_prefix) - 1);

    if (argument[0] == '-')
    {
        fuse_log (FUSE_LOG_ERR, "Failed to parse command line: unsupported argument `%s'\n", argument);
        errno = EINVAL;
        return -1;
    }

    return parse_mountpoint_argument (parser->options, argument);
}

static int parse_command_options (
    struct fuse_args const * args,
    struct fuse_cmdline_opts * options,
    int * mount_mode,
    struct fuse_config * configuration
)
{
    memset (options, 0, sizeof (*options));
    options->max_idle_threads = (unsigned int)-1;
    options->max_threads = FUSE_MT_DEFAULT_THREADS;
    *mount_mode = FUSE_MOUNT_MODE_AUTOMATIC;

    if (args == NULL || args->argc < 0 || (args->argc != 0 && args->argv == NULL))
    {
        errno = EINVAL;
        return -1;
    }

    struct command_parser parser = { 0 };

    parser.args = args;
    parser.options = options;
    parser.mount_mode = mount_mode;
    parser.configuration = configuration;

    for (int argument_index = 1; argument_index < args->argc; argument_index++)
    {
        if (parse_command_argument (&parser, &argument_index) != 0)
            return -1;
    }

    return 0;
}

int fuse_parse_cmdline (struct fuse_args * args, struct fuse_cmdline_opts * options)
{
    int mount_mode;

    return parse_command_options (args, options, &mount_mode, NULL);
}

static int run_mounted_request_loop (struct fuse * fuse, const struct fuse_cmdline_opts * options)
{
    struct fuse_session * session = fuse_get_session (fuse);

    if (fuse_set_signal_handlers (session) != 0)
        return FUSE_MAIN_SIGNAL_HANDLERS_FAILED;

    int rc = options->singlethread ? fuse_loop (fuse) : run_multithreaded_session (fuse, options->max_threads);
    if (rc != 0)
        rc = FUSE_MAIN_LOOP_FAILED;

    fuse_remove_signal_handlers (session);

    return rc;
}

static int mount_and_run_session (struct fuse * fuse, const struct fuse_cmdline_opts * options)
{
    if (fuse_mount (fuse, options->mountpoint) != 0)
        return FUSE_MAIN_MOUNT_FAILED;

    int rc = run_mounted_request_loop (fuse, options);

    fuse_unmount (fuse);

    if (fuse->mounted)
        rc = FUSE_MAIN_UNMOUNT_FAILED;

    return rc;
}

struct filesystem_implementation
{
    const struct fuse_operations * operations; // Borrowed callback table supplied by the filesystem.
    size_t operations_size;                    // Callback table size used for ABI compatibility.
    struct libfuse_version * version;          // Borrowed version supplied to session creation.
    void * user_data;                          // Initial callback private data.
};

static int process_main_options (
    struct fuse_args * args,
    const struct fuse_cmdline_opts * options,
    const struct filesystem_implementation * implementation
)
{
    if (options->show_version)
    {
        printf ("FUSE library version %s (AIX USFS)\n", fuse_pkgversion ());
        return FUSE_MAIN_SUCCESS;
    }

    if (options->show_help)
    {
        if (args->argv[0][0] != '\0')
            printf ("usage: %s [options] <mountpoint>\n\n", args->argv[0]);

        fuse_cmdline_help ();
        return FUSE_MAIN_SUCCESS;
    }

    if (options->mountpoint == NULL)
    {
        fuse_log (FUSE_LOG_ERR, "Failed to start filesystem: no mountpoint specified\n");
        return FUSE_MAIN_MISSING_MOUNTPOINT;
    }

    struct fuse * fuse =
        _fuse_new_31 (args, implementation->operations, implementation->operations_size, implementation->version, implementation->user_data);

    if (fuse == NULL)
        return FUSE_MAIN_CREATE_FAILED;

    const int rc = mount_and_run_session (fuse, options);

    fuse_destroy (fuse);

    if (rc == FUSE_MAIN_SUCCESS && errno != 0)
        return FUSE_MAIN_CLEANUP_FAILED;

    return rc;
}

int fuse_main_real_versioned (
    const int argc,
    char * argv[],
    const struct fuse_operations * op,
    const size_t op_size,
    struct libfuse_version * version,
    void * user_data
)
{
    struct fuse_args args = FUSE_ARGS_INIT (argc, argv);
    struct fuse_cmdline_opts options;
    int rc = FUSE_MAIN_INVALID_ARGUMENTS;

    if (fuse_parse_cmdline (&args, &options) == 0)
    {
        struct filesystem_implementation implementation = { 0 };

        implementation.operations = op;
        implementation.operations_size = op_size;
        implementation.version = version;
        implementation.user_data = user_data;

        rc = process_main_options (&args, &options, &implementation);
    }

    free (options.mountpoint);
    fuse_opt_free_args (&args);

    return rc;
}

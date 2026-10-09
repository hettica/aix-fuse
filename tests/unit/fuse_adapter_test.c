// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#define FUSE_USE_VERSION 31
#include "fuse.h"
#include <usfs/usfs.h>
#include "tap.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum
{
    REQUEST_UID = 1001,
    REQUEST_GID = 2001,
    REQUEST_PID = 3001,
    NATIVE_READ_LIMIT = 65536,
    NATIVE_WRITE_LIMIT = 32768,
    CONTEXT_REPETITIONS = 1000
};

/* These are test-double definitions of the public API's opaque objects.
 * They contain no native-engine implementation or private representation. */
struct usfs_client_request
{
    struct usfs_client * owner; // Double receiving the explicit request.
    uid_t uid;                  // Credentials selected by the test.
    gid_t gid;                  // Credentials selected by the test.
    pid_t pid;                  // Credentials selected by the test.
};

struct usfs_client
{
    struct usfs_operations operations;  // Callbacks captured at the public boundary.
    struct usfs_client_options options; // Copied native options.
    struct usfs_client_request request; // Explicit callback argument.
    void * application_data;            // Adapter's opaque state passed at creation.
    int initialized;                    // Whether successful initialization was delivered.
    int mounted;                        // Mount state controlled by the fixture.
};

struct usfs_directory_sink
{
    unsigned calls;       // Attempts made through the FUSE directory filler.
    unsigned fail_at;     // Attempt that makes the sink fail permanently.
    int error;            // First positive error retained by the sink.
    const char * name;    // Last successfully supplied borrowed name.
    struct stat metadata; // Last supplied metadata, copied field by field in the test.
};

static struct
{
    struct usfs_client * latest; // Last successfully created native double.
    int create_error;            // Negative result injected into construction.
    int mount_error;             // Negative result injected into mounting.
    int run_error;               // Negative result injected after initialization.
    int unmount_error;           // Negative result that retains mount ownership.
    int destroy_error;           // Negative destruction result.
    int destroy_retains;         // Preserve the owning pointer on destruction failure.
    int signal_error;            // Negative handler-installation result.
    unsigned creates;            // Recorded native lifecycle calls.
    unsigned mounts;
    unsigned runs;
    unsigned unmounts;
    unsigned destroys;
    unsigned signal_installs;
    unsigned signal_removals;
    unsigned stops;
    unsigned workers; // Requested native worker count.
} native;

static unsigned fail_adapter_allocation;

void * adapter_test_calloc (size_t count, size_t size)
{
    if (fail_adapter_allocation != 0 && --fail_adapter_allocation == 0)
    {
        errno = ENOMEM;
        return NULL;
    }
    return calloc (count, size);
}

struct usfs_client * usfs_request_client (const struct usfs_client_request * request)
{
    return request->owner;
}

void * usfs_request_user_data (const struct usfs_client_request * request)
{
    return request->owner->application_data;
}

uid_t usfs_request_uid (const struct usfs_client_request * request)
{
    return request->uid;
}
gid_t usfs_request_gid (const struct usfs_client_request * request)
{
    return request->gid;
}
pid_t usfs_request_pid (const struct usfs_client_request * request)
{
    return request->pid;
}

int usfs_directory_add (struct usfs_directory_sink * sink, const char * name, const struct stat * metadata)
{
    ++sink->calls;
    if (sink->error == 0 && sink->fail_at == sink->calls)
        sink->error = ENOSPC;
    if (sink->error != 0)
        return -sink->error;
    sink->name = name;
    if (metadata != NULL)
        sink->metadata = *metadata;
    return 0;
}

int usfs_client_create (
    unsigned int version,
    const struct usfs_operations * operations,
    size_t size,
    const struct usfs_client_options * options,
    void * application_data,
    struct usfs_client ** result
)
{
    ++native.creates;
    *result = NULL;
    if (native.create_error != 0)
        return native.create_error;
    if (version != USFS_CLIENT_API_VERSION || size != sizeof (*operations))
        abort ();
    struct usfs_client * client = calloc (1, sizeof (*client));
    if (client == NULL)
        abort ();
    client->operations = *operations;
    client->options = *options;
    client->application_data = application_data;
    client->request.owner = client;
    native.latest = client;
    *result = client;
    return 0;
}

int usfs_client_mount (struct usfs_client * client, const char * mountpoint)
{
    ++native.mounts;
    if (mountpoint == NULL)
        abort ();
    if (native.mount_error == 0)
        client->mounted = 1;
    return native.mount_error;
}

int usfs_client_run (struct usfs_client * client, unsigned int workers)
{
    ++native.runs;
    native.workers = workers;
    if (!client->initialized)
    {
        const struct usfs_limits limits = { NATIVE_READ_LIMIT, NATIVE_WRITE_LIMIT };
        const struct usfs_client_request request = { .owner = client };
        int rc = 0;
        if (client->operations.initialize != NULL)
            rc = client->operations.initialize (&request, &limits, &client->options.behavior);
        if (rc != 0)
            return rc;
        client->initialized = 1;
    }
    return native.run_error;
}

void usfs_client_request_stop (struct usfs_client * client)
{
    if (client == NULL)
        abort ();
    ++native.stops;
}

int usfs_client_unmount (struct usfs_client * client)
{
    ++native.unmounts;
    if (native.unmount_error == 0)
        client->mounted = 0;
    return native.unmount_error;
}

int usfs_client_destroy (struct usfs_client ** client_pointer)
{
    ++native.destroys;
    struct usfs_client * client = *client_pointer;
    if (client->mounted)
        return -EBUSY;
    if (native.destroy_retains)
        return native.destroy_error;
    if (client->initialized && client->operations.shutdown != NULL)
    {
        const struct usfs_client_request request = { .owner = client };
        client->operations.shutdown (&request);
    }
    if (native.latest == client)
        native.latest = NULL;
    free (client);
    *client_pointer = NULL;
    return native.destroy_error;
}

int usfs_client_install_signals (struct usfs_client * client)
{
    if (client == NULL)
        abort ();
    ++native.signal_installs;
    return native.signal_error;
}

void usfs_client_remove_signals (struct usfs_client * client)
{
    if (client == NULL)
        abort ();
    ++native.signal_removals;
}

static void reset_native (void)
{
    if (native.latest != NULL)
        abort ();
    memset (&native, 0, sizeof (native));
}

static struct fuse * create_adapter (const struct fuse_operations * operations, size_t size, void * data)
{
    return _fuse_new_31 (NULL, operations, size, NULL, data);
}

static int wait_child (pid_t child)
{
    int status;
    return waitpid (child, &status, 0) == child && WIFEXITED (status) && WEXITSTATUS (status) == 0;
}

static void test_public_basics (struct tap_state * tap)
{
    tap_ok (tap, fuse_version () == FUSE_VERSION, "reported FUSE version matches the public header");
    tap_ok (tap, strcmp (fuse_pkgversion (), "3.19.0-aix-usfs") == 0, "package version identifies the AIX implementation");
    tap_ok (tap, fuse_daemonize (0) == 0 && fuse_daemonize (1) == 0, "foreground and requested daemon modes remain nonfailing");
    tap_ok (tap, fuse_get_context () == NULL, "no callback context leaks outside callback scope");
}

static void test_command_line (struct tap_state * tap)
{
    struct fuse_cmdline_opts options;
    char * defaults_argv[] = { "program" };
    char * full_argv[] = { "program", "-h", "-V", "-d", "-s", "-o", "debug", "/mount" };
    char * extra_argv[] = { "program", "/one", "/two" };
    struct fuse_args defaults = FUSE_ARGS_INIT (1, defaults_argv);
    struct fuse_args full = FUSE_ARGS_INIT (8, full_argv);
    struct fuse_args extra = FUSE_ARGS_INIT (3, extra_argv);
    tap_ok (
        tap,
        fuse_parse_cmdline (&defaults, &options) == 0 && options.mountpoint == NULL && options.max_threads == 10 &&
            options.max_idle_threads == (unsigned)-1,
        "empty command line receives documented defaults"
    );
    tap_ok (
        tap,
        fuse_parse_cmdline (&full, &options) == 0 && options.show_help && options.show_version && options.debug && options.foreground &&
            options.singlethread && strcmp (options.mountpoint, "/mount") == 0,
        "supported command-line switches and mountpoint are parsed"
    );
    free (options.mountpoint);
    tap_ok (tap, fuse_parse_cmdline (&extra, &options) == -1, "second mountpoint is rejected");
    free (options.mountpoint);
    tap_ok (tap, fuse_parse_cmdline (&full, &options) == 0, "parser remains reusable after an invalid command line");
    free (options.mountpoint);
    const char * values[] = { "max_threads=4,debug", "max_threads=0", "max_threads=65", "max_threads=many" };
    for (unsigned index = 0; index < sizeof (values) / sizeof (values[0]); ++index)
    {
        char * argv[] = { "program", "-o", (char *)values[index] };
        struct fuse_args args = FUSE_ARGS_INIT (3, argv);
        const int rc = fuse_parse_cmdline (&args, &options);
        tap_ok (tap, index == 0 ? rc == 0 && options.max_threads == 4 : rc == -1, values[index]);
        free (options.mountpoint);
    }
}

static int timestamp_backend (const char * path, const struct timespec times[2], struct fuse_file_info * file)
{
    (void)path;
    (void)times;
    (void)file;
    return 0;
}

static void test_mount_options (struct tap_state * tap)
{
    const struct
    {
        const char * first;  // First argument after the executable name.
        const char * second; // Optional following argument.
        int access;          // Native mount enum, or negative for rejected syntax.
        const char * description;
    } cases[] = { { "-o", "ro", USFS_MOUNT_READ_ONLY, "separate ro reaches native access selection" },
                  { "-oro", NULL, USFS_MOUNT_READ_ONLY, "attached ro overrides writable callbacks" },
                  { "-orw", NULL, USFS_MOUNT_READ_WRITE, "explicit rw is retained by direct construction" },
                  { "-oro,ro,debug,max_threads=2", NULL, USFS_MOUNT_READ_ONLY, "repeated ro preserves framework options" },
                  { "-orw,rw", NULL, USFS_MOUNT_READ_WRITE, "repeated rw is accepted consistently" },
                  { "-oro,rw", NULL, -1, "conflicting comma-separated modes fail before opening" },
                  { "-oro", "-orw", -1, "conflicting separate options fail before opening" },
                  { "-onosuid", NULL, -1, "unsupported nosuid fails before opening" },
                  { "-onodev", NULL, -1, "unsupported nodev fails before opening" },
                  { "-onoexec", NULL, -1, "unsupported noexec fails before opening" },
                  { "-oallow_other", NULL, -1, "unsupported permission option fails before opening" },
                  { "--unknown", NULL, -1, "unknown command-line option fails before opening" },
                  { "-o", "", -1, "empty mount option list fails before opening" },
                  { "-oro,,debug", NULL, -1, "empty middle option fails before opening" },
                  { "-oro,", NULL, -1, "empty trailing option fails before opening" },
                  { "-odebug", NULL, USFS_MOUNT_AUTOMATIC, "timestamp-only backend retains automatic admission" } };
    const struct fuse_operations operations = { .utimens = timestamp_backend };
    for (unsigned index = 0; index < sizeof (cases) / sizeof (cases[0]); ++index)
    {
        reset_native ();
        char * argv[] = { "program", (char *)cases[index].first, (char *)cases[index].second };
        struct fuse_args args = FUSE_ARGS_INIT (cases[index].second == NULL ? 2 : 3, argv);
        struct fuse_cmdline_opts options;
        const int parsed = fuse_parse_cmdline (&args, &options);
        free (options.mountpoint);
        struct fuse * fuse = _fuse_new_31 (&args, &operations, sizeof (operations), NULL, NULL);
        int valid = parsed == -1 && fuse == NULL && native.creates == 0;
        if (cases[index].access >= 0)
            valid = parsed == 0 && fuse != NULL && native.latest->options.mount_access == (enum usfs_mount_access)cases[index].access &&
                    native.latest->operations.utimens != NULL;
        fuse_destroy (fuse);
        tap_ok (tap, valid, cases[index].description);
    }
}

static int export_backend (const char * path, uint64_t device, uint64_t inode, mode_t type, uint64_t * token)
{
    (void)path;
    (void)device;
    (void)type;
    *token = inode;
    return 0;
}

static int resolve_backend (uint64_t token, char * path, size_t capacity)
{
    (void)token;
    if (capacity < 2)
        return -ERANGE;
    strcpy (path, "/");
    return 0;
}

static void test_table_sizes (struct tap_state * tap)
{
    struct fuse_operations operations = { .export_id = export_backend, .resolve_id = resolve_backend };
    const size_t sizes[] = { offsetof (struct fuse_operations, export_id),
                             offsetof (struct fuse_operations, export_id) + 1,
                             offsetof (struct fuse_operations, resolve_id) + 1 };
    for (unsigned index = 0; index < sizeof (sizes) / sizeof (sizes[0]); ++index)
    {
        reset_native ();
        struct fuse * fuse = create_adapter (&operations, sizes[index], NULL);
        const int valid = fuse != NULL && (native.latest->operations.export_id == NULL || native.latest->operations.resolve_id == NULL);
        fuse_destroy (fuse);
        tap_ok (tap, valid, "old or partial FUSE identity table cannot claim native persistent identity");
    }
    for (unsigned index = 0; index < 2; ++index)
    {
        reset_native ();
        operations.export_id = index == 0 ? export_backend : NULL;
        operations.resolve_id = index == 0 ? NULL : resolve_backend;
        struct fuse * fuse = create_adapter (&operations, sizeof (operations), NULL);
        const int valid = fuse != NULL && (native.latest->operations.export_id == NULL || native.latest->operations.resolve_id == NULL);
        fuse_destroy (fuse);
        tap_ok (tap, valid, "an incomplete FUSE identity callback pair cannot claim native persistent identity");
    }
}

static struct
{
    struct fuse * fuse; // Expected FUSE instance in lifecycle callbacks.
    void * initial;     // Data visible during initialization.
    void * replacement; // Data returned by initialization, including NULL.
    unsigned initializes;
    unsigned releases;
    unsigned directory_releases;
    unsigned shutdowns;
    unsigned errors;
    int override_behavior; // Exercise configuration changes in init.
    int initial_behavior;  // Whether init saw both requested mount policies.
} lifecycle;

static int lifecycle_context_matches (void * data)
{
    const struct fuse_context * context = fuse_get_context ();
    return context != NULL && context->fuse == lifecycle.fuse && context->private_data == data && context->uid == 0 && context->gid == 0 &&
           context->pid == 0 && context->umask == 0;
}

static void * initialize_backend (struct fuse_conn_info * connection, struct fuse_config * configuration)
{
    ++lifecycle.initializes;
    if (!lifecycle_context_matches (lifecycle.initial))
        ++lifecycle.errors;
    if (connection->max_read != NATIVE_READ_LIMIT || connection->max_write != NATIVE_WRITE_LIMIT || connection->proto_major != FUSE_MAJOR_VERSION ||
        connection->proto_minor != FUSE_MINOR_VERSION)
        ++lifecycle.errors;
    lifecycle.initial_behavior = configuration->hard_remove && configuration->nullpath_ok;
    if (lifecycle.override_behavior)
    {
        configuration->hard_remove = 0;
        configuration->nullpath_ok = 0;
    }
    return lifecycle.replacement;
}

static int release_backend (const char * path, struct fuse_file_info * file)
{
    (void)path;
    if (!lifecycle_context_matches (lifecycle.replacement) || file == NULL || file->fh != 0)
        ++lifecycle.errors;
    ++lifecycle.releases;
    return 0;
}

static int releasedir_backend (const char * path, struct fuse_file_info * file)
{
    (void)path;
    if (!lifecycle_context_matches (lifecycle.replacement) || file == NULL || file->fh != 0)
        ++lifecycle.errors;
    ++lifecycle.directory_releases;
    return 0;
}

static void shutdown_backend (void * data)
{
    if (!lifecycle_context_matches (lifecycle.replacement) || data != lifecycle.replacement)
        ++lifecycle.errors;
    ++lifecycle.shutdowns;
}

static int allocation_safe_metadata (const char * path, struct stat * metadata, struct fuse_file_info * file)
{
    (void)path;
    (void)metadata;
    (void)file;
    if (!lifecycle_context_matches (lifecycle.replacement))
        ++lifecycle.errors;
    return 0;
}

static void test_context_allocation (struct tap_state * tap)
{
    const struct fuse_operations operations = { .init = initialize_backend,
                                                .destroy = shutdown_backend,
                                                .getattr = allocation_safe_metadata,
                                                .release = release_backend,
                                                .releasedir = releasedir_backend };
    reset_native ();
    fail_adapter_allocation = 1;
    struct fuse * fuse = create_adapter (&operations, sizeof (operations), NULL);
    const int creation_failed = fuse == NULL && errno == ENOMEM && native.creates == 0;
    if (fuse != NULL)
        fuse_destroy (fuse);
    fail_adapter_allocation = 0;
    tap_ok (tap, creation_failed, "adapter storage allocation failure reports ENOMEM before native creation");

    reset_native ();
    memset (&lifecycle, 0, sizeof (lifecycle));
    int application_data;
    lifecycle.initial = &application_data;
    lifecycle.replacement = &application_data;
    lifecycle.fuse = create_adapter (&operations, sizeof (operations), lifecycle.initial);
    if (lifecycle.fuse == NULL)
        abort ();

    fail_adapter_allocation = 1;
    int valid = fuse_loop (lifecycle.fuse) == 0;
    struct stat metadata = { 0 };
    struct usfs_open_file file = { 0 };
    valid &= native.latest->operations.getattr (&native.latest->request, "/", &metadata, NULL) == 0;
    valid &= native.latest->operations.release (&native.latest->request, NULL, &file) == 0;
    valid &= native.latest->operations.releasedir (&native.latest->request, NULL, &file) == 0;
    fuse_destroy (lifecycle.fuse);
    valid &= errno == 0 && native.latest == NULL && native.destroys == 1 && lifecycle.initializes == 1 && lifecycle.releases == 1 &&
             lifecycle.directory_releases == 1 && lifecycle.shutdowns == 1 && lifecycle.errors == 0 && fail_adapter_allocation == 1 &&
             fuse_get_context () == NULL;
    fail_adapter_allocation = 0;
    tap_ok (tap, valid, "callback contexts and release/shutdown need no further adapter allocation");
}

static void test_lifecycle_context (struct tap_state * tap)
{
    int initial, replacement;
    const struct fuse_operations operations = { .init = initialize_backend,
                                                .destroy = shutdown_backend,
                                                .release = release_backend,
                                                .releasedir = releasedir_backend };
    for (unsigned scenario = 0; scenario < 4; ++scenario)
    {
        reset_native ();
        memset (&lifecycle, 0, sizeof (lifecycle));
        lifecycle.initial = &initial;
        lifecycle.replacement = scenario & 2 ? NULL : &replacement;
        lifecycle.fuse = create_adapter (&operations, sizeof (operations), lifecycle.initial);
        if (lifecycle.fuse == NULL)
            abort ();
        int valid = (scenario & 1 ? fuse_loop_mt (lifecycle.fuse, 0) : fuse_loop (lifecycle.fuse)) == 0;
        valid &= native.workers == (scenario & 1 ? USFS_CLIENT_DEFAULT_WORKERS : 1);
        struct usfs_open_file file = { 0 };
        valid &= native.latest->operations.release (&native.latest->request, NULL, &file) == 0;
        valid &= native.latest->operations.releasedir (&native.latest->request, NULL, &file) == 0;
        fuse_destroy (lifecycle.fuse);
        valid &= lifecycle.initializes == 1 && lifecycle.releases == 1 && lifecycle.directory_releases == 1 && lifecycle.shutdowns == 1 &&
                 lifecycle.errors == 0 && fuse_get_context () == NULL;
        tap_ok (tap, valid, "both FUSE loops preserve post-init data, including NULL, through handle cleanup and shutdown");
    }
}

static void test_configuration (struct tap_state * tap)
{
    reset_native ();
    memset (&lifecycle, 0, sizeof (lifecycle));
    lifecycle.override_behavior = 1;
    char * argv[] = { "test", "-o", "hard_remove,nullpath_ok" };
    struct fuse_args args = FUSE_ARGS_INIT (3, argv);
    const struct fuse_operations operations = { .init = initialize_backend };
    lifecycle.fuse = _fuse_new_31 (&args, &operations, sizeof (operations), NULL, NULL);
    tap_ok (
        tap,
        lifecycle.fuse != NULL && native.latest->options.behavior.remove_policy == USFS_REMOVE_IMMEDIATE &&
            native.latest->options.behavior.handle_paths == USFS_PATH_OMIT_FOR_HANDLE,
        "standard FUSE lifetime options initialize native policies"
    );
    const int rc = fuse_loop (lifecycle.fuse);
    tap_ok (
        tap,
        rc == 0 && lifecycle.initial_behavior && lifecycle.errors == 0 && native.latest->options.behavior.remove_policy == USFS_REMOVE_DEFERRED &&
            native.latest->options.behavior.handle_paths == USFS_PATH_DEFAULT,
        "FUSE init sees and can override both standard lifetime policies"
    );
    fuse_destroy (lifecycle.fuse);
}

struct context_fixture
{
    struct fuse * fuse;              // Instance expected in the thread-local FUSE context.
    struct usfs_client * client;     // Recorded public-native double for dispatch.
    struct context_fixture * nested; // Optional inner callback with different identity.
    unsigned errors;                 // Owned by one worker, inspected after joining.
};

static int context_backend (const char * path, struct stat * metadata, struct fuse_file_info * file)
{
    (void)path;
    (void)metadata;
    (void)file;
    struct fuse_context * context = fuse_get_context ();
    struct context_fixture * fixture = context->private_data;
    if (context->fuse != fixture->fuse || context->uid != REQUEST_UID || context->gid != REQUEST_GID || context->pid != REQUEST_PID ||
        context->umask != 0)
        ++fixture->errors;
    const struct fuse_context saved = *context;
    if (fixture->nested != NULL)
    {
        struct stat nested_metadata;
        struct context_fixture * nested = fixture->nested;
        if (nested->client->operations.getattr (&nested->client->request, "/inner", &nested_metadata, NULL) != 0)
            ++fixture->errors;
    }
    sched_yield ();
    context = fuse_get_context ();
    if (context->fuse != saved.fuse || context->private_data != saved.private_data || context->uid != saved.uid || context->gid != saved.gid ||
        context->pid != saved.pid)
        ++fixture->errors;
    return 0;
}

static void prepare_context_fixture (struct context_fixture * fixture)
{
    const struct fuse_operations operations = { .getattr = context_backend };
    fixture->fuse = create_adapter (&operations, sizeof (operations), fixture);
    if (fixture->fuse == NULL)
        abort ();
    fixture->client = native.latest;
    fixture->client->request.uid = REQUEST_UID;
    fixture->client->request.gid = REQUEST_GID;
    fixture->client->request.pid = REQUEST_PID;
}

static void * context_worker (void * argument)
{
    struct context_fixture * fixture = argument;
    if (fuse_get_context () != NULL)
        ++fixture->errors;
    for (unsigned index = 0; index < CONTEXT_REPETITIONS; ++index)
    {
        struct stat metadata;
        if (fixture->client->operations.getattr (&fixture->client->request, "/", &metadata, NULL) != 0 || fuse_get_context () != NULL)
            ++fixture->errors;
    }
    return NULL;
}

static void test_nested_and_thread_context (struct tap_state * tap)
{
    reset_native ();
    struct context_fixture fixtures[2] = { 0 };
    prepare_context_fixture (&fixtures[0]);
    prepare_context_fixture (&fixtures[1]);
    fixtures[0].nested = &fixtures[1];
    struct stat metadata;
    int rc = fixtures[0].client->operations.getattr (&fixtures[0].client->request, "/outer", &metadata, NULL);
    tap_ok (
        tap,
        rc == 0 && fixtures[0].errors == 0 && fixtures[1].errors == 0 && fuse_get_context () == NULL,
        "nested FUSE callbacks restore all outer credentials and private data"
    );
    fixtures[0].nested = NULL;
    pthread_t workers[2];
    for (unsigned index = 0; index < 2; ++index)
        if (pthread_create (&workers[index], NULL, context_worker, &fixtures[index]) != 0)
            abort ();
    for (unsigned index = 0; index < 2; ++index)
        pthread_join (workers[index], NULL);
    tap_ok (
        tap,
        fixtures[0].errors == 0 && fixtures[1].errors == 0 && fuse_get_context () == NULL,
        "parallel callbacks cannot observe another FUSE instance's context"
    );
    fuse_destroy (fixtures[0].fuse);
    fuse_destroy (fixtures[1].fuse);
}

static struct
{
    unsigned calls;
    unsigned errors;
    int expect_null_file;
    enum fuse_create_activation activation;
    unsigned fields;
    int fill_results[3];
} translated;

static int file_backend (const char * path, struct fuse_file_info * file)
{
    ++translated.calls;
    if (path != NULL)
        ++translated.errors;
    if (translated.expect_null_file)
    {
        if (file != NULL)
            ++translated.errors;
        return 0;
    }
    if (file == NULL || file->fh != 0 || file->flags != (O_RDWR | O_APPEND) || file->direct_io != 0 || file->keep_cache != 0)
        ++translated.errors;
    file->fh = UINT64_C (0x1234567887654321);
    file->flags = O_RDONLY;
    return -EACCES;
}

static int attributes_backend (const char * path, struct stat * metadata, struct fuse_file_info * file)
{
    (void)metadata;
    return file_backend (path, file);
}

static int read_backend (const char * path, char * data, size_t length, off_t offset, struct fuse_file_info * file)
{
    if (path != NULL || length != 3 || offset != 7 || file == NULL || file->fh != 0)
        ++translated.errors;
    memcpy (data, "abc", 3);
    return 3;
}

static int write_backend (const char * path, const char * data, size_t length, off_t offset, struct fuse_file_info * file)
{
    if (path != NULL || length != 3 || offset != 7 || memcmp (data, "abc", 3) != 0 || file == NULL || file->fh != 0)
        ++translated.errors;
    return -ENOSPC;
}

static int atomic_backend (
    const char * path,
    const struct stat * initial,
    unsigned fields,
    enum fuse_create_activation activation,
    struct stat * result,
    struct fuse_file_info * file
)
{
    if (strcmp (path, "/new") != 0 || fields != translated.fields || activation != translated.activation || initial->st_mode != (S_IFREG | 0640))
        ++translated.errors;
    if ((activation == FUSE_CREATE_OPEN) != (file != NULL))
        ++translated.errors;
    if (file != NULL)
        file->fh = UINT64_C (0x100000002);
    *result = *initial;
    result->st_ino = 42;
    return 0;
}

static int directory_backend (
    const char * path,
    void * buffer,
    fuse_fill_dir_t fill,
    off_t offset,
    struct fuse_file_info * file,
    enum fuse_readdir_flags flags
)
{
    if (path != NULL || offset != 0 || file == NULL || file->fh != 0 || flags != FUSE_READDIR_DEFAULTS)
        ++translated.errors;
    const struct stat metadata = { .st_ino = 42 };
    translated.fill_results[0] = fill (buffer, "one", &metadata, 99, FUSE_FILL_DIR_DEFAULTS);
    translated.fill_results[1] = fill (buffer, "two", NULL, 100, FUSE_FILL_DIR_DEFAULTS);
    translated.fill_results[2] = fill (buffer, "three", NULL, 101, FUSE_FILL_DIR_DEFAULTS);
    return 0;
}

static int time_backend (const char * path, const struct timespec times[2], struct fuse_file_info * file)
{
    if (path != NULL || file == NULL || file->fh != 0 || times[0].tv_sec != 17 || times[1].tv_sec != 23 || times[0].tv_nsec != UTIME_NOW ||
        times[1].tv_nsec != UTIME_OMIT)
        ++translated.errors;
    return 0;
}

static void test_callback_translation (struct tap_state * tap)
{
    reset_native ();
    struct fuse_operations operations = { .getattr = attributes_backend,
                                          .open = file_backend,
                                          .read = read_backend,
                                          .write = write_backend,
                                          .readdir = directory_backend,
                                          .create_attr = atomic_backend,
                                          .export_id = export_backend,
                                          .resolve_id = resolve_backend,
                                          .utimens = time_backend };
    struct fuse * fuse = create_adapter (&operations, sizeof (operations), NULL);
    if (fuse == NULL)
        abort ();
    struct usfs_client * client = native.latest;
    tap_ok (
        tap,
        client->operations.open != NULL && client->operations.getattr != NULL && client->operations.unlink == NULL &&
            client->operations.rename == NULL && client->operations.opendir == NULL && client->operations.create == NULL &&
            client->operations.create_attr != NULL,
        "only supplied FUSE callbacks become native backend capabilities"
    );
    memset (&translated, 0, sizeof (translated));
    struct usfs_open_file file = { .open_flags = O_RDWR | O_APPEND };
    int rc = client->operations.open (&client->request, NULL, &file);
    tap_ok (
        tap,
        rc == -EACCES && translated.errors == 0 && file.value == UINT64_C (0x1234567887654321) && file.open_flags == O_RDONLY,
        "zero handles, flags, updates and callback errors cross the adapter explicitly"
    );
    translated.expect_null_file = 1;
    struct stat metadata;
    rc = client->operations.getattr (&client->request, NULL, &metadata, NULL);
    tap_ok (tap, rc == 0 && translated.errors == 0, "absent native handle remains NULL in the FUSE callback");
    translated.expect_null_file = 0;
    file.value = 0;
    char data[3];
    const ssize_t bytes = client->operations.read (&client->request, NULL, data, sizeof (data), 7, &file);
    const ssize_t written = client->operations.write (&client->request, NULL, data, sizeof (data), 7, &file);
    tap_ok (
        tap,
        bytes == 3 && written == -ENOSPC && translated.errors == 0,
        "read and write preserve buffers, offsets, lengths and signed callback results"
    );
    const enum usfs_create_action actions[] = { USFS_CREATE_WITHOUT_OPEN, USFS_CREATE_FOR_LOOKUP, USFS_CREATE_WITH_OPEN };
    const enum fuse_create_activation activations[] = { FUSE_CREATE_DEFAULT, FUSE_CREATE_LOOKUP, FUSE_CREATE_OPEN };
    const unsigned native_fields =
        USFS_INITIAL_MODE | USFS_INITIAL_UID | USFS_INITIAL_GID | USFS_INITIAL_SIZE | USFS_INITIAL_ATIME | USFS_INITIAL_MTIME | USFS_INITIAL_CTIME;
    translated.fields =
        FUSE_INITIAL_MODE | FUSE_INITIAL_UID | FUSE_INITIAL_GID | FUSE_INITIAL_SIZE | FUSE_INITIAL_ATIME | FUSE_INITIAL_MTIME | FUSE_INITIAL_CTIME;
    for (unsigned index = 0; index < 3; ++index)
    {
        const struct stat initial = { .st_mode = S_IFREG | 0640 };
        translated.activation = activations[index];
        file.value = 0;
        rc = client->operations.create_attr (&client->request, "/new", &initial, native_fields, actions[index], &metadata, index == 2 ? &file : NULL);
        tap_ok (
            tap,
            rc == 0 && translated.errors == 0 && metadata.st_ino == 42 && (index != 2 || file.value == UINT64_C (0x100000002)),
            "atomic creation maps activation, every initial attribute and optional open handle"
        );
    }
    file.value = 0;
    struct usfs_directory_sink sink = { .fail_at = 2 };
    rc = client->operations.readdir (&client->request, NULL, &file, &sink);
    tap_ok (
        tap,
        rc == 0 && translated.errors == 0 && sink.calls == 3 && sink.error == ENOSPC && sink.metadata.st_ino == 42 &&
            translated.fill_results[0] == 0 && translated.fill_results[1] == 1 && translated.fill_results[2] == 1,
        "directory filler reports sticky sink failure without losing metadata or zero handles"
    );
    uint64_t token = 0;
    char path[8];
    rc = client->operations.export_id (&client->request, NULL, 7, UINT64_C (0x100000042), S_IFREG, &token);
    tap_ok (
        tap,
        rc == 0 && token == UINT64_C (0x100000042) && client->operations.resolve_id (&client->request, token, path, sizeof (path)) == 0 &&
            strcmp (path, "/") == 0,
        "persistent identity tokens retain all 64 bits through both callbacks"
    );
    const struct timespec times[2] = { { 17, USFS_TIME_NOW }, { 23, USFS_TIME_OMIT } };
    rc = client->operations.utimens (&client->request, NULL, times, &file);
    tap_ok (tap, rc == 0 && translated.errors == 0, "native timestamp sentinels translate without changing seconds or open handles");
    fuse_destroy (fuse);
}

static void test_lifecycle_forwarding (struct tap_state * tap)
{
    const struct fuse_operations operations = { 0 };
    reset_native ();
    native.create_error = -ENOMEM;
    errno = 0;
    struct fuse * fuse = create_adapter (&operations, sizeof (operations), NULL);
    tap_ok (tap, fuse == NULL && errno == ENOMEM, "native construction errors become FUSE NULL and errno");
    native.create_error = 0;
    fuse = create_adapter (&operations, sizeof (operations), NULL);
    if (fuse == NULL)
        abort ();
    native.mount_error = -EACCES;
    tap_ok (tap, fuse_mount (fuse, "/mount") == -1 && errno == EACCES, "native mount failure becomes FUSE minus one and errno");
    native.mount_error = 0;
    if (fuse_mount (fuse, "/mount") != 0)
        abort ();
    native.signal_error = -EBUSY;
    tap_ok (
        tap,
        fuse_set_signal_handlers (fuse_get_session (fuse)) == -1 && errno == EBUSY,
        "native handler registration errors preserve FUSE errno conventions"
    );
    native.signal_error = 0;
    const int installed = fuse_set_signal_handlers (fuse_get_session (fuse));
    fuse_remove_signal_handlers (fuse_get_session (fuse));
    fuse_exit (fuse);
    tap_ok (tap, installed == 0 && native.signal_removals == 1 && native.stops == 1, "FUSE session handlers and stop forward to their native client");
    native.run_error = -EIO;
    int valid = fuse_loop (fuse) == -EIO && native.workers == 1;
    valid &= fuse_loop_mt (fuse, 0) == -EIO && native.workers == USFS_CLIENT_DEFAULT_WORKERS;
    tap_ok (tap, valid, "both FUSE loops retain signed native errors and their documented worker counts");
    native.unmount_error = -EBUSY;
    fuse_unmount (fuse);
    valid = errno == EBUSY && native.latest->mounted;
    fuse_destroy (fuse);
    valid &= errno == EBUSY && native.destroys == 0;
    native.unmount_error = 0;
    fuse_unmount (fuse);
    valid &= errno == 0 && !native.latest->mounted;
    tap_ok (tap, valid, "busy unmount and refused destruction retain an adapter that can retry");
    native.destroy_error = -ENOMEM;
    native.destroy_retains = 1;
    fuse_destroy (fuse);
    valid = errno == ENOMEM && native.latest != NULL;
    native.destroy_retains = 0;
    native.destroy_error = -EACCES;
    fuse_destroy (fuse);
    tap_ok (
        tap,
        valid && errno == EACCES && native.latest == NULL && native.destroys == 2,
        "adapter lifetime follows the native owning pointer on retained and consumed failures"
    );
}

static int run_main_case (unsigned scenario)
{
    const struct fuse_operations operations = { 0 };
    char * argv[] = { "program", "/mount" };
    int argc = 2;
    const int expected[] = { 0, 1, 2, 3, 4, 6, 8, 9, 10 };
    reset_native ();
    switch (scenario)
    {
        case 1:
            argv[1] = "--unsupported";
            break;
        case 2:
            argc = 1;
            break;
        case 3:
            native.create_error = -ENOMEM;
            break;
        case 4:
            native.mount_error = -ENOENT;
            break;
        case 5:
            native.signal_error = -EBUSY;
            break;
        case 6:
            native.run_error = -EIO;
            break;
        case 7:
            native.unmount_error = -EBUSY;
            break;
        case 8:
            native.destroy_error = -EACCES;
            break;
        default:
            break;
    }
    return fuse_main_real_versioned (argc, argv, &operations, sizeof (operations), NULL, NULL) == expected[scenario];
}

static void test_main_results (struct tap_state * tap)
{
    for (unsigned scenario = 0; scenario < 9; ++scenario)
    {
        const pid_t child = fork ();
        if (child == 0)
            _exit (run_main_case (scenario) ? 0 : 1);
        tap_ok (tap, child > 0 && wait_child (child), "FUSE startup preserves the established exit result for each lifecycle phase");
    }
    for (unsigned scenario = 0; scenario < 3; ++scenario)
    {
        reset_native ();
        const struct fuse_operations operations = { 0 };
        char * argv[] = { "program", "/mount", scenario == 1 ? "-s" : "-omax_threads=4" };
        const int argc = scenario == 0 ? 2 : 3;
        const int rc = fuse_main_real_versioned (argc, argv, &operations, sizeof (operations), NULL, NULL);
        const unsigned expected = scenario == 0 ? USFS_CLIENT_DEFAULT_WORKERS : scenario == 1 ? 1 : 4;
        tap_ok (
            tap,
            rc == 0 && native.workers == expected && native.latest == NULL,
            "FUSE startup selects default, single and explicit worker counts"
        );
    }
    for (unsigned scenario = 0; scenario < 2; ++scenario)
    {
        const pid_t child = fork ();
        if (child == 0)
        {
            reset_native ();
            const struct fuse_operations operations = { 0 };
            char * argv[] = { "program", scenario == 0 ? "--help" : "--version" };
            if (freopen ("/dev/null", "w", stdout) == NULL)
                _exit (1);
            const int rc = fuse_main_real_versioned (2, argv, &operations, sizeof (operations), NULL, NULL);
            _exit (rc == 0 && native.creates == 0 ? 0 : 1);
        }
        tap_ok (tap, child > 0 && wait_child (child), "FUSE help and version succeed without constructing a native client");
    }
}

int main (void)
{
    struct tap_state tap;
    /* 43 migrated boundary assertions plus 31 interface and startup cases. */
    tap_plan (&tap, 74);
    test_context_allocation (&tap);        /* 2 */
    test_public_basics (&tap);             /* 4 */
    test_command_line (&tap);              /* 8 */
    test_mount_options (&tap);             /* 16 */
    test_table_sizes (&tap);               /* 5 */
    test_lifecycle_context (&tap);         /* 4 */
    test_configuration (&tap);             /* 2 */
    test_nested_and_thread_context (&tap); /* 2 */
    test_callback_translation (&tap);      /* 10 */
    test_lifecycle_forwarding (&tap);      /* 7 */
    test_main_results (&tap);              /* 14 */
    return tap_finish (&tap);
}

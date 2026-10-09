/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * Mounted checks shared by the test-only pathname daemon's probe mode.
 */
static int create_probe_file (const char * path, const char * content)
{
    const int descriptor = open (path, O_CREAT | O_EXCL | O_RDWR, PROBE_FILE_MODE);
    if (descriptor < 0)
    {
        fprintf (stderr, "Failed to create probe file %s: %s\n", path, strerror (errno));
        return -1;
    }

    const size_t length = strlen (content);
    if (write (descriptor, content, length) != (ssize_t)length || fsync (descriptor) != 0)
    {
        fprintf (stderr, "Failed to write/sync probe file %s: %s\n", path, strerror (errno));
        close (descriptor);
        return -1;
    }

    return descriptor;
}

static unsigned count_probe_hidden_files (const char * directory_path)
{
    DIR * directory = opendir (directory_path);
    if (directory == NULL)
        return UINT_MAX;

    unsigned count = 0;

    for (struct dirent * entry = readdir (directory); entry != NULL; entry = readdir (directory))
        if (strncmp (entry->d_name, ".fuse_hidden", strlen (".fuse_hidden")) == 0)
            count++;

    closedir (directory);

    return count;
}

static int set_probe_fault (const char * backing, const char * fault, const int enabled)
{
    char path[PROBE_PATH_CAPACITY];

    snprintf (path, sizeof (path), "%s/.fault-%s", backing, fault);

    if (!enabled)
        return unlink (path);

    const int descriptor = open (path, O_CREAT | O_WRONLY, PROBE_FILE_MODE);
    if (descriptor < 0)
        return -1;

    return close (descriptor);
}

static int descriptor_has_content (const int descriptor, const char * expected)
{
    char content[PROBE_PATH_CAPACITY] = { 0 };
    const size_t length = strlen (expected);

    return pread (descriptor, content, length, 0) == (ssize_t)length && memcmp (content, expected, length) == 0;
}

static void probe_open_unlink (struct tap_state * tap, const char * mountpoint, const char * backing)
{
    char directory[PROBE_PATH_CAPACITY];
    char moved_directory[PROBE_PATH_CAPACITY];
    char file[PROBE_PATH_CAPACITY];
    char replacement[PROBE_PATH_CAPACITY];
    char backing_moved[PROBE_PATH_CAPACITY];

    snprintf (directory, sizeof (directory), "%s/dir", mountpoint);
    snprintf (moved_directory, sizeof (moved_directory), "%s/moved", mountpoint);
    snprintf (file, sizeof (file), "%s/dir/file", mountpoint);
    snprintf (replacement, sizeof (replacement), "%s/moved/file", mountpoint);
    snprintf (backing_moved, sizeof (backing_moved), "%s/moved", backing);

    mkdir (directory, PROBE_DIRECTORY_MODE);
    const int first = create_probe_file (file, "original");
    const int second = open (file, O_RDWR);
    struct stat metadata = { 0 };

    tap_ok (
        tap,
        first >= 0 && second >= 0 && unlink (file) == 0 && access (file, F_OK) != 0 && count_probe_hidden_files (directory) == 1,
        "pathname backend hides an open file and removes its original name"
    );
    tap_ok (
        tap,
        fstat (first, &metadata) == 0 && metadata.st_nlink == 0 && descriptor_has_content (second, "original"),
        "fstat and read remain valid after unlink with zero-valued backend handles"
    );

    const struct timespec times[2] = { { 1234567890, 0 }, { 1234567891, 0 } };
    const int changed = fchmod (first, PROBE_FILE_MODE) == 0 && fchown (first, getuid (), getgid ()) == 0 &&
                        ftruncate (first, PROBE_SPARSE_OFFSET) == 0 && futimens (first, times) == 0 &&
                        pwrite (first, "!", 1, strlen ("original")) == 1 && fsync (first) == 0;

    tap_ok (
        tap,
        changed && fstat (first, &metadata) == 0 && metadata.st_size == PROBE_SPARSE_OFFSET,
        "write, sync, truncate and metadata callbacks resolve the hidden path"
    );
    errno = 0;
    tap_ok (tap, rmdir (directory) != 0 && errno == ENOTEMPTY, "a live hidden entry keeps its containing directory nonempty");
    tap_ok (
        tap,
        rename (directory, moved_directory) == 0 && descriptor_has_content (second, "original!"),
        "hidden callback paths follow an ancestor rename"
    );

    const int recreated = create_probe_file (replacement, "replacement");

    tap_ok (
        tap,
        recreated >= 0 && descriptor_has_content (first, "original!") && descriptor_has_content (recreated, "replacement"),
        "recreated names have independent contents and identities"
    );
    close (recreated);
    close (first);
    tap_ok (
        tap,
        count_probe_hidden_files (backing_moved) == 1 && descriptor_has_content (second, "original!"),
        "a hidden object survives until its last independent open closes"
    );
    close (second);
    tap_ok (tap, count_probe_hidden_files (backing_moved) == 0, "final close deletes the hidden entry after releasing its backend handle");
    unlink (replacement);
    rmdir (moved_directory);
}

static void probe_rename_and_links (struct tap_state * tap, const char * mountpoint, const char * backing)
{
    char source[PROBE_PATH_CAPACITY];
    char destination[PROBE_PATH_CAPACITY];
    char alias[PROBE_PATH_CAPACITY];

    snprintf (source, sizeof (source), "%s/source", mountpoint);
    snprintf (destination, sizeof (destination), "%s/destination", mountpoint);
    snprintf (alias, sizeof (alias), "%s/alias", mountpoint);

    int source_descriptor = create_probe_file (source, "source");
    int destination_descriptor = create_probe_file (destination, "destination");

    close (source_descriptor);
    tap_ok (
        tap,
        rename (source, destination) == 0 && descriptor_has_content (destination_descriptor, "destination"),
        "rename replacement preserves the open destination's contents"
    );
    source_descriptor = open (destination, O_RDWR);
    tap_ok (
        tap,
        descriptor_has_content (source_descriptor, "source") && count_probe_hidden_files (backing) == 1,
        "the replacement pathname resolves to the source and the old destination is hidden"
    );
    close (destination_descriptor);
    close (source_descriptor);
    unlink (destination);

    source_descriptor = create_probe_file (source, "linked");
    struct stat metadata = { 0 };

    tap_ok (
        tap,
        link (source, alias) == 0 && unlink (source) == 0 && fstat (source_descriptor, &metadata) == 0 && metadata.st_nlink == 1,
        "hidden links do not inflate the visible hard-link count"
    );
    tap_ok (
        tap,
        unlink (alias) == 0 && fstat (source_descriptor, &metadata) == 0 && metadata.st_nlink == 0 &&
            descriptor_has_content (source_descriptor, "linked"),
        "all removed aliases retain one original open object"
    );
    close (source_descriptor);
    tap_ok (tap, count_probe_hidden_files (backing) == 0, "final close cleans every owned hidden hard-link alias");
}

static void probe_rename_failures (struct tap_state * tap, const char * mountpoint, const char * backing)
{
    char source[PROBE_PATH_CAPACITY];
    char destination[PROBE_PATH_CAPACITY];

    snprintf (source, sizeof (source), "%s/source", mountpoint);
    snprintf (destination, sizeof (destination), "%s/destination", mountpoint);

    const int source_descriptor = create_probe_file (source, "source");
    const int destination_descriptor = create_probe_file (destination, "destination");

    set_probe_fault (backing, "hide", true);
    errno = 0;
    tap_ok (
        tap,
        unlink (destination) != 0 && errno == EACCES && descriptor_has_content (destination_descriptor, "destination") &&
            count_probe_hidden_files (backing) == 0,
        "failed hiding leaves the original pathname and data unchanged"
    );
    set_probe_fault (backing, "hide", false);
    set_probe_fault (backing, "source", true);
    errno = 0;
    tap_ok (
        tap,
        rename (source, destination) != 0 && errno == EIO && access (destination, F_OK) == 0 &&
            descriptor_has_content (destination_descriptor, "destination") && count_probe_hidden_files (backing) == 0,
        "a failed source rename restores the hidden destination"
    );
    set_probe_fault (backing, "rollback", true);
    errno = 0;
    tap_ok (
        tap,
        rename (source, destination) != 0 && errno == EIO && access (destination, F_OK) != 0 &&
            descriptor_has_content (destination_descriptor, "destination") && count_probe_hidden_files (backing) == 1,
        "failed rollback remains tracked and the original destination handle stays valid"
    );
    set_probe_fault (backing, "source", false);
    set_probe_fault (backing, "rollback", false);
    close (destination_descriptor);
    close (source_descriptor);
    unlink (source);
    tap_ok (tap, count_probe_hidden_files (backing) == 0, "rollback failure does not leak hidden ownership after final close");
}

static void probe_occupied_rollback (struct tap_state * tap, const char * mountpoint, const char * backing)
{
    char source[PROBE_PATH_CAPACITY];
    char destination[PROBE_PATH_CAPACITY];

    snprintf (source, sizeof (source), "%s/source", mountpoint);
    snprintf (destination, sizeof (destination), "%s/destination", mountpoint);
    const int source_descriptor = create_probe_file (source, "source");
    const int destination_descriptor = create_probe_file (destination, "destination");

    set_probe_fault (backing, "source", true);
    set_probe_fault (backing, "occupied", true);
    errno = 0;
    const int failed = rename (source, destination) != 0 && errno == EIO;
    const int occupied = open (destination, O_RDONLY);

    tap_ok (
        tap,
        failed && descriptor_has_content (occupied, "occupied") && descriptor_has_content (destination_descriptor, "destination") &&
            count_probe_hidden_files (backing) == 1,
        "rollback never overwrites an occupied destination and retains its hidden object"
    );
    close (occupied);
    close (destination_descriptor);
    close (source_descriptor);
    set_probe_fault (backing, "source", false);
    set_probe_fault (backing, "occupied", false);
    unlink (source);
    unlink (destination);
}

static void probe_cross_directory_rename (struct tap_state * tap, const char * mountpoint, const char * backing)
{
    char source_directory[PROBE_PATH_CAPACITY];
    char destination_directory[PROBE_PATH_CAPACITY];
    char moved_directory[PROBE_PATH_CAPACITY];
    char source[PROBE_PATH_CAPACITY];
    char destination[PROBE_PATH_CAPACITY];
    char alias[PROBE_PATH_CAPACITY];
    char user_hidden[PROBE_PATH_CAPACITY];

    snprintf (source_directory, sizeof (source_directory), "%s/from", mountpoint);
    snprintf (destination_directory, sizeof (destination_directory), "%s/to", mountpoint);
    snprintf (moved_directory, sizeof (moved_directory), "%s/moved", mountpoint);
    snprintf (source, sizeof (source), "%s/from/source", mountpoint);
    snprintf (destination, sizeof (destination), "%s/to/destination", mountpoint);
    mkdir (source_directory, PROBE_DIRECTORY_MODE);
    mkdir (destination_directory, PROBE_DIRECTORY_MODE);
    const int source_descriptor = create_probe_file (source, "source");
    const int destination_descriptor = create_probe_file (destination, "destination");

    tap_ok (
        tap,
        rename (source, destination) == 0 && rename (destination_directory, moved_directory) == 0 &&
            descriptor_has_content (source_descriptor, "source") && descriptor_has_content (destination_descriptor, "destination"),
        "cross-directory replacement follows an ancestor rename for both retained open objects"
    );
    close (destination_descriptor);
    close (source_descriptor);
    snprintf (destination, sizeof (destination), "%s/moved/destination", mountpoint);
    snprintf (alias, sizeof (alias), "%s/moved/alias", mountpoint);
    struct stat metadata = { 0 };

    tap_ok (
        tap,
        link (destination, alias) == 0 && rename (destination, alias) == 0 && rename (alias, alias) == 0 && stat (destination, &metadata) == 0 &&
            metadata.st_nlink == 2,
        "identical paths and hard-link aliases of the same object are rename no-ops"
    );
    unlink (destination);
    unlink (alias);
    rmdir (source_directory);
    rmdir (moved_directory);
    snprintf (user_hidden, sizeof (user_hidden), "%s/.fuse_hidden_user_file", mountpoint);
    const int user_descriptor = create_probe_file (user_hidden, "user");

    close (user_descriptor);
    tap_ok (
        tap,
        access (user_hidden, F_OK) == 0 && count_probe_hidden_files (mountpoint) == 1 && count_probe_hidden_files (backing) == 1,
        "similarly prefixed user files remain visible and are never selected for automatic cleanup"
    );
    unlink (user_hidden);
}

static int run_path_lifetime_probe (const char * mountpoint, const char * backing)
{
    struct tap_state tap = { 0 };

    tap_plan (&tap, 21);
    probe_open_unlink (&tap, mountpoint, backing);
    probe_rename_and_links (&tap, mountpoint, backing);
    probe_rename_failures (&tap, mountpoint, backing);
    probe_occupied_rollback (&tap, mountpoint, backing);
    probe_cross_directory_rename (&tap, mountpoint, backing);

    return tap_finish (&tap);
}

static int run_path_cleanup_probe (const char * mountpoint, const char * backing)
{
    struct tap_state tap = { 0 };
    char path[PROBE_PATH_CAPACITY];

    tap_plan (&tap, 2);
    snprintf (path, sizeof (path), "%s/pending", mountpoint);
    const int descriptor = create_probe_file (path, "pending");

    set_probe_fault (backing, "cleanup", true);
    unlink (path);
    close (descriptor);
    tap_ok (&tap, count_probe_hidden_files (backing) == 1, "failed final-close deletion remains owned for shutdown retry");
    snprintf (path, sizeof (path), "%s/unrelated", mountpoint);
    const int unrelated = create_probe_file (path, "unrelated");

    tap_ok (&tap, unrelated >= 0 && descriptor_has_content (unrelated, "unrelated"), "cleanup failure leaves unrelated I/O available");
    close (unrelated);
    unlink (path);

    return tap_finish (&tap);
}

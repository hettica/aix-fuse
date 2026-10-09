/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#define _LARGE_FILES

#include <time.h>

#include <sys/dir.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syncvfs.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <limits.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Fixture chunk sizes exercise requests across multiple transport windows. */
#define PROBE_TRANSFER_CHUNK_SIZE (64u * 1024u)
#define PROBE_INVALID_DESCRIPTOR  (-1)

static sigjmp_buf mapping_fault_context;
static volatile sig_atomic_t mapping_fault_armed;
static volatile sig_atomic_t mapping_fault_signal;

static void on_mapping_fault (int signal_number)
{
    mapping_fault_signal = signal_number;
    if (mapping_fault_armed)
        siglongjmp (mapping_fault_context, 1);
    _exit (128 + signal_number);
}

static int install_mapping_fault_handlers (void)
{
    struct sigaction action;

    memset (&action, 0, sizeof (action));
    action.sa_handler = on_mapping_fault;
    sigemptyset (&action.sa_mask);
    if (sigaction (SIGSEGV, &action, NULL) != 0)
        return -1;
#ifdef SIGBUS
    if (sigaction (SIGBUS, &action, NULL) != 0)
        return -1;
#endif
    return 0;
}

static int mapping_read_byte (volatile unsigned char * address, unsigned char * value)
{
    mapping_fault_signal = 0;
    if (sigsetjmp (mapping_fault_context, 1) != 0)
    {
        mapping_fault_armed = 0;
        return -1;
    }
    mapping_fault_armed = 1;
    *value = *address;
    mapping_fault_armed = 0;
    return 0;
}

static int mapping_write_byte (volatile unsigned char * address, unsigned char value)
{
    mapping_fault_signal = 0;
    if (sigsetjmp (mapping_fault_context, 1) != 0)
    {
        mapping_fault_armed = 0;
        return -1;
    }
    mapping_fault_armed = 1;
    *address = value;
    mapping_fault_armed = 0;
    return 0;
}

static int parse_u64 (const char * text, uint64_t maximum, uint64_t * value)
{
    char * parse_end;
    unsigned long long parsed_value;

    errno = 0;
    parsed_value = strtoull (text, &parse_end, 0);
    if (errno != 0 || text[0] == '\0' || *parse_end != '\0' || parsed_value > maximum)
        return -1;
    *value = (uint64_t)parsed_value;
    return 0;
}

static ssize_t transfer_io (
    const int file_descriptor,
    unsigned char * buffer,
    const uint64_t count_value,
    const uint64_t offset_value,
    const int positioned,
    const int flags
)
{
    errno = 0;
    if (positioned && flags == O_RDONLY)
        return pread (file_descriptor, buffer, (size_t)count_value, (off_t)offset_value);

    if (positioned)
        return pwrite (file_descriptor, buffer, (size_t)count_value, (off_t)offset_value);

    if (flags == O_RDONLY)
        return read (file_descriptor, buffer, (size_t)count_value);

    return write (file_descriptor, buffer, (size_t)count_value);
}

static int io_open_flags (const char * operation)
{
    if (strcmp (operation, "read") == 0 || strcmp (operation, "pread") == 0)
        return O_RDONLY;
    if (strcmp (operation, "write") == 0 || strcmp (operation, "pwrite") == 0)
        return O_WRONLY;

    return -1;
}

static int run_io (const char * operation, const char * path, const char * count_text, const char * offset_text)
{
    const uint64_t maximum_count = (uint64_t)PROBE_TRANSFER_CHUNK_SIZE * 4u;
    uint64_t count_value;
    uint64_t offset_value;
    unsigned char * buffer;
    off_t final_offset;
    ssize_t result;
    int saved_errno;
    int file_descriptor;
    int flags;
    int positioned;
    size_t byte_index;
    int pattern = 1;

    if (parse_u64 (count_text, maximum_count, &count_value) != 0 || parse_u64 (offset_text, (uint64_t)INT64_MAX, &offset_value) != 0)
    {
        fprintf (stderr, "invalid count or offset\n");
        return 2;
    }
    flags = io_open_flags (operation);
    positioned = operation[0] == 'p';
    if (flags == -1)
    {
        fprintf (stderr, "invalid operation: %s\n", operation);
        return 2;
    }

    buffer = malloc (count_value == 0 ? 1u : (size_t)count_value);
    if (buffer == NULL)
    {
        perror ("malloc");
        return 1;
    }
    if (flags == O_WRONLY)
        for (byte_index = 0; byte_index < (size_t)count_value; ++byte_index)
            buffer[byte_index] = (unsigned char)((offset_value + byte_index) & 0xffu);

    file_descriptor = open (path, flags);
    if (file_descriptor < 0)
    {
        perror (path);
        free (buffer);
        return 1;
    }
    if (!positioned && lseek (file_descriptor, (off_t)offset_value, SEEK_SET) == (off_t)-1)
    {
        perror ("lseek");
        close (file_descriptor);
        free (buffer);
        return 1;
    }

    result = transfer_io (file_descriptor, buffer, count_value, offset_value, positioned, flags);
    saved_errno = errno;
    final_offset = lseek (file_descriptor, 0, SEEK_CUR);

    if (flags == O_RDONLY && result > 0)
        for (byte_index = 0; byte_index < (size_t)result; ++byte_index)
            if (buffer[byte_index] != (unsigned char)((offset_value + byte_index) & 0xffu))
            {
                pattern = 0;
                break;
            }

    printf ("result=%lld errno=%d final=%lld pattern=%d\n", (long long)result, saved_errno, (long long)final_offset, pattern);
    printf ("EFBIG=%d\npid=%ld\n", EFBIG, (long)getpid ());
    close (file_descriptor);
    free (buffer);
    return 0;
}

static int run_readdir (const char * path, const char * count_text)
{
    uint64_t count_value;
    char * buffer;
    off_t base = 0;
    ssize_t result;
    int saved_errno;
    int file_descriptor;

    if (parse_u64 (count_text, (uint64_t)PROBE_TRANSFER_CHUNK_SIZE, &count_value) != 0)
    {
        fprintf (stderr, "invalid directory count\n");
        return 2;
    }
    buffer = malloc (count_value == 0 ? 1u : (size_t)count_value);
    if (buffer == NULL)
    {
        perror ("malloc");
        return 1;
    }
    file_descriptor = open (path, O_RDONLY);
    if (file_descriptor < 0)
    {
        perror (path);
        free (buffer);
        return 1;
    }

    errno = 0;
    result = getdirentries (file_descriptor, buffer, (size_t)count_value, &base);
    saved_errno = errno;
    printf ("result=%lld errno=%d base=%lld\n", (long long)result, saved_errno, (long long)base);
    printf ("EINVAL=%d\n", EINVAL);
    close (file_descriptor);
    free (buffer);
    return 0;
}

static int run_readdir_retry (const char * path)
{
    char data[4096];
    off_t base = 0;
    int file_descriptor = open (path, O_RDONLY), result = 1;
    if (file_descriptor < 0)
        return 1;
    errno = 0;
    ssize_t count = getdirentries (file_descriptor, data, 16, &base);
    int error = errno;
    off_t position = lseek (file_descriptor, 0, SEEK_CUR);
    printf ("small=%lld errno=%d base=%lld position=%lld\n", (long long)count, error, (long long)base, (long long)position);
    if (count != -1 || error != EINVAL || base != 0 || position != 0)
        goto out;
    count = getdirentries (file_descriptor, data, sizeof (data), &base);
    printf ("retry=%lld base=%lld\n", (long long)count, (long long)base);
    if (count <= 0)
        goto out;
    puts ("readdir-retry=valid");
    result = 0;
out:
    if (close (file_descriptor) != 0)
        result = 1;
    return result;
}

static const unsigned large_directory_entry_count = 140000u;

static int verify_large_directory_records (const char * bytes, const size_t length, unsigned * next_entry_index, unsigned * prefix_count)
{
    size_t position = 0;

    while (position < length)
    {
        if (length - position < offsetof (struct dirent, d_name))
            return 1;

        const struct dirent * entry = (const struct dirent *)(bytes + position);
        if (entry->d_reclen <= offsetof (struct dirent, d_name) || entry->d_reclen > length - position)
            return 1;

        if (*prefix_count < 2)
        {
            const char * expected_prefix = *prefix_count == 0 ? "." : "..";
            if (entry->d_namlen != strlen (expected_prefix) || memcmp (entry->d_name, expected_prefix, entry->d_namlen) != 0)
                return 1;

            *prefix_count += 1;
            position += entry->d_reclen;
            continue;
        }

        char expected_name[32];
        snprintf (expected_name, sizeof (expected_name), "entry-%06u", *next_entry_index);
        if (*next_entry_index >= large_directory_entry_count || entry->d_namlen != strlen (expected_name) ||
            memcmp (entry->d_name, expected_name, entry->d_namlen) != 0)
            return 1;

        *next_entry_index += 1;
        position += entry->d_reclen;
    }

    return 0;
}

static int run_readdir_large (const char * path)
{
    char * buffer = malloc (PROBE_TRANSFER_CHUNK_SIZE);
    if (buffer == NULL)
        return 1;

    const int directory_fd = open (path, O_RDONLY);
    if (directory_fd < 0)
    {
        free (buffer);
        return 1;
    }

    off_t base = 0;
    unsigned next_entry_index = 0;
    unsigned prefix_count = 0;
    unsigned windows = 0;
    int result = 1;

    while (windows <= large_directory_entry_count)
    {
        const ssize_t received = getdirentries (directory_fd, buffer, PROBE_TRANSFER_CHUNK_SIZE, &base);
        if (received < 0)
            break;

        if (received == 0)
        {
            if (prefix_count == 2 && next_entry_index == large_directory_entry_count && windows > 2)
                result = 0;

            if (getdirentries (directory_fd, buffer, PROBE_TRANSFER_CHUNK_SIZE, &base) != 0)
                result = 1;

            break;
        }

        if (verify_large_directory_records (buffer, (size_t)received, &next_entry_index, &prefix_count) != 0)
            break;

        if (windows == 0)
        {
            const off_t next_cursor = lseek (directory_fd, 0, SEEK_CUR);
            if (next_cursor <= 0)
                break;

            errno = 0;
            const ssize_t small_result = getdirentries (directory_fd, buffer, 1, &base);
            const int small_error = errno;
            if (small_result != -1 || small_error != EINVAL)
                break;

            if (lseek (directory_fd, 0, SEEK_CUR) != next_cursor)
                break;

            if (lseek (directory_fd, next_cursor, SEEK_SET) != next_cursor)
                break;
        }

        ++windows;
    }

    printf ("large-directory=%s entries=%u windows=%u\n", result == 0 ? "valid" : "failed", next_entry_index, windows);
    if (close (directory_fd) != 0)
        result = 1;
    free (buffer);
    return result;
}

static int run_mmap_read (const char * path, const char * count_text, const char * offset_text)
{
    const uint64_t maximum_count = (uint64_t)PROBE_TRANSFER_CHUNK_SIZE * 1024u;
    uint64_t count_value;
    uint64_t offset_value;
    unsigned char * mapping;
    int saved_errno;
    int pattern = 1;
    int file_descriptor;
    size_t byte_index;

    if (parse_u64 (count_text, maximum_count, &count_value) != 0 || parse_u64 (offset_text, (uint64_t)INT64_MAX, &offset_value) != 0 ||
        count_value == 0)
    {
        fprintf (stderr, "invalid mmap count or offset\n");
        return 2;
    }

    file_descriptor = open (path, O_RDONLY);
    if (file_descriptor < 0)
    {
        perror (path);
        return 1;
    }

    errno = 0;
    mapping = mmap (NULL, (size_t)count_value, PROT_READ, MAP_SHARED, file_descriptor, (off_t)offset_value);
    saved_errno = errno;
    if (mapping == MAP_FAILED)
    {
        printf ("result=-1 errno=%d pattern=0\n", saved_errno);
        close (file_descriptor);
        return 0;
    }

    close (file_descriptor);
    for (byte_index = 0; byte_index < (size_t)count_value; ++byte_index)
    {
        if (mapping[byte_index] != (unsigned char)((offset_value + byte_index) & 0xffu))
        {
            pattern = 0;
            break;
        }
    }

    if (munmap (mapping, (size_t)count_value) != 0)
    {
        perror ("munmap");
        return 1;
    }

    printf ("result=0 errno=%d pattern=%d\n", saved_errno, pattern);
    return 0;
}

static int check_file_pattern (const char * path, uint64_t count, uint64_t offset, unsigned delta)
{
    unsigned char * buffer;
    ssize_t result;
    int matches = 1;
    int file_descriptor;
    size_t byte_index;

    buffer = malloc ((size_t)count);
    if (buffer == NULL)
        return 0;

    file_descriptor = open (path, O_RDONLY);
    if (file_descriptor < 0)
    {
        free (buffer);
        return 0;
    }
    result = pread (file_descriptor, buffer, (size_t)count, (off_t)offset);
    close (file_descriptor);
    if (result != (ssize_t)count)
    {
        free (buffer);
        return 0;
    }

    for (byte_index = 0; byte_index < (size_t)count; ++byte_index)
    {
        if (buffer[byte_index] != (unsigned char)((offset + byte_index + delta) & 0xffu))
        {
            matches = 0;
            break;
        }
    }
    free (buffer);
    return matches;
}

static int exercise_mapped_write (
    const char * path,
    unsigned char * mapping,
    const uint64_t count_value,
    const uint64_t offset_value,
    const unsigned delta,
    const int map_flags
)
{
    size_t byte_index;
    int saved_errno;
    int source;

    for (byte_index = 0; byte_index < (size_t)count_value; ++byte_index)
        mapping[byte_index] = (unsigned char)((offset_value + byte_index + delta) & 0xffu);

    if (map_flags == MAP_SHARED)
    {
        errno = 0;
        if (msync (mapping, (size_t)count_value, MS_SYNC) != 0)
        {
            saved_errno = errno;
            munmap (mapping, (size_t)count_value);
            printf ("result=-1 errno=%d source=0\n", saved_errno);
            return 0;
        }
    }
    if (munmap (mapping, (size_t)count_value) != 0)
    {
        perror ("munmap");
        return 1;
    }

    source = check_file_pattern (path, count_value, offset_value, map_flags == MAP_SHARED ? delta : 0u);
    printf ("result=0 errno=0 source=%d\n", source);
    return 0;
}

static int run_mmap_write (const char * operation, const char * path, const char * count_text, const char * offset_text)
{
    const uint64_t maximum_count = (uint64_t)PROBE_TRANSFER_CHUNK_SIZE * 1024u;
    const unsigned delta = 0x5au;
    uint64_t count_value;
    uint64_t offset_value;
    unsigned char * mapping;
    int map_flags;
    int saved_errno;
    int readonly_fd;
    int file_descriptor;

    if (parse_u64 (count_text, maximum_count, &count_value) != 0 || parse_u64 (offset_text, (uint64_t)INT64_MAX, &offset_value) != 0 ||
        count_value == 0)
    {
        fprintf (stderr, "invalid mmap count or offset\n");
        return 2;
    }

    map_flags = strstr (operation, "shared") != NULL ? MAP_SHARED : MAP_PRIVATE;
    readonly_fd = strstr (operation, "readonly") != NULL;
    file_descriptor = open (path, readonly_fd ? O_RDONLY : O_RDWR);
    if (file_descriptor < 0)
    {
        perror (path);
        return 1;
    }

    errno = 0;
    mapping = mmap (NULL, (size_t)count_value, PROT_READ | PROT_WRITE, map_flags, file_descriptor, (off_t)offset_value);
    saved_errno = errno;
    if (mapping == MAP_FAILED)
    {
        printf ("result=-1 errno=%d source=0\n", saved_errno);
        close (file_descriptor);
        return 0;
    }

    close (file_descriptor);
    return exercise_mapped_write (path, mapping, count_value, offset_value, delta, map_flags);
}

static int exercise_mapping_touch (unsigned char * mapping, const uint64_t count_value, const uint64_t index_value, const int write_access)
{
    unsigned char value = 0;

    if (install_mapping_fault_handlers () != 0)
    {
        perror ("sigaction");
        munmap (mapping, (size_t)count_value);
        return 1;
    }
    if (write_access)
    {
        if (mapping_write_byte (mapping + (size_t)index_value, 0xa5u) != 0)
        {
            printf ("result=-1 errno=0 signal=%d value=0\n", (int)mapping_fault_signal);
            munmap (mapping, (size_t)count_value);
            return 0;
        }
        value = mapping[(size_t)index_value];
    }
    else if (mapping_read_byte (mapping + (size_t)index_value, &value) != 0)
    {
        printf ("result=-1 errno=0 signal=%d value=0\n", (int)mapping_fault_signal);
        munmap (mapping, (size_t)count_value);
        return 0;
    }
    if (munmap (mapping, (size_t)count_value) != 0)
    {
        perror ("munmap");
        return 1;
    }
    printf ("result=0 errno=0 signal=0 value=%u\n", (unsigned)value);
    return 0;
}

static int run_mmap_touch (const char * operation, const char * path, const char * count_text, const char * offset_text, const char * index_text)
{
    uint64_t count_value;
    uint64_t offset_value;
    uint64_t index_value;
    unsigned char * mapping;
    int write_access;
    int saved_errno;
    int file_descriptor;

    if (parse_u64 (count_text, (uint64_t)SIZE_MAX, &count_value) != 0 || parse_u64 (offset_text, (uint64_t)INT64_MAX, &offset_value) != 0 ||
        parse_u64 (index_text, (uint64_t)SIZE_MAX, &index_value) != 0 || count_value == 0 || index_value >= count_value)
    {
        fprintf (stderr, "invalid mmap touch range\n");
        return 2;
    }
    write_access = strcmp (operation, "mmap-touch-write") == 0;
    file_descriptor = open (path, write_access ? O_RDWR : O_RDONLY);
    if (file_descriptor < 0)
    {
        perror (path);
        return 1;
    }
    errno = 0;
    mapping = mmap (NULL, (size_t)count_value, write_access ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, file_descriptor, (off_t)offset_value);
    saved_errno = errno;
    close (file_descriptor);
    if (mapping == MAP_FAILED)
    {
        printf ("result=-1 errno=%d signal=0 value=0\n", saved_errno);
        return 0;
    }
    return exercise_mapping_touch (mapping, count_value, index_value, write_access);
}

static int run_mmap_unlink (const char * path, const char * count_text)
{
    uint64_t count_value;
    unsigned char * mapping;
    unsigned char value;
    int pattern = 1;
    int file_descriptor;
    size_t byte_index;

    if (parse_u64 (count_text, (uint64_t)SIZE_MAX, &count_value) != 0 || count_value == 0)
    {
        fprintf (stderr, "invalid mmap unlink size\n");
        return 2;
    }
    file_descriptor = open (path, O_RDONLY);
    if (file_descriptor < 0)
    {
        perror (path);
        return 1;
    }
    mapping = mmap (NULL, (size_t)count_value, PROT_READ, MAP_SHARED, file_descriptor, 0);
    if (mapping == MAP_FAILED)
    {
        perror ("mmap");
        close (file_descriptor);
        return 1;
    }
    close (file_descriptor);
    if (unlink (path) != 0)
    {
        perror ("unlink");
        munmap (mapping, (size_t)count_value);
        return 1;
    }
    if (install_mapping_fault_handlers () != 0)
    {
        perror ("sigaction");
        munmap (mapping, (size_t)count_value);
        return 1;
    }
    for (byte_index = 0; byte_index < (size_t)count_value; ++byte_index)
    {
        if (mapping_read_byte (mapping + byte_index, &value) != 0 || value != (unsigned char)(byte_index & 0xffu))
        {
            pattern = 0;
            break;
        }
    }
    if (munmap (mapping, (size_t)count_value) != 0)
    {
        perror ("munmap");
        return 1;
    }
    printf ("result=0 pattern=%d signal=%d absent=%d\n", pattern, (int)mapping_fault_signal, access (path, F_OK) != 0);
    return 0;
}

static int run_mmap_fork (const char * path, const char * count_text)
{
    uint64_t count_value;
    unsigned char * mapping;
    int status;
    int file_descriptor;
    pid_t child;
    size_t byte_index;

    if (parse_u64 (count_text, (uint64_t)SIZE_MAX, &count_value) != 0 || count_value == 0)
    {
        fprintf (stderr, "invalid mmap fork size\n");
        return 2;
    }
    file_descriptor = open (path, O_RDONLY);
    if (file_descriptor < 0)
    {
        perror (path);
        return 1;
    }
    mapping = mmap (NULL, (size_t)count_value, PROT_READ, MAP_SHARED, file_descriptor, 0);
    close (file_descriptor);
    if (mapping == MAP_FAILED)
    {
        perror ("mmap");
        return 1;
    }
    child = fork ();
    if (child < 0)
    {
        perror ("fork");
        munmap (mapping, (size_t)count_value);
        return 1;
    }
    if (child == 0)
    {
        for (byte_index = 0; byte_index < (size_t)count_value; ++byte_index)
            if (mapping[byte_index] != (unsigned char)(byte_index & 0xffu))
                _exit (1);
        _exit (0);
    }
    if (waitpid (child, &status, 0) < 0)
    {
        perror ("waitpid");
        munmap (mapping, (size_t)count_value);
        return 1;
    }
    if (munmap (mapping, (size_t)count_value) != 0)
    {
        perror ("munmap");
        return 1;
    }
    printf (
        "result=%d exited=%d status=%d\n",
        WIFEXITED (status) && WEXITSTATUS (status) == 0 ? 0 : -1,
        WIFEXITED (status) ? 1 : 0,
        WIFEXITED (status) ? WEXITSTATUS (status) : -1
    );
    return 0;
}

static int run_mmap_repeat (const char * path, const char * count_text, const char * iterations_text)
{
    uint64_t count_value;
    uint64_t iterations;
    unsigned char * mapping;
    unsigned char value;
    uint64_t iteration_index;
    int file_descriptor;

    if (parse_u64 (count_text, (uint64_t)SIZE_MAX, &count_value) != 0 || parse_u64 (iterations_text, 100000u, &iterations) != 0 || count_value == 0 ||
        iterations == 0)
    {
        fprintf (stderr, "invalid mmap repeat arguments\n");
        return 2;
    }
    if (install_mapping_fault_handlers () != 0)
    {
        perror ("sigaction");
        return 1;
    }
    for (iteration_index = 0; iteration_index < iterations; ++iteration_index)
    {
        file_descriptor = open (path, O_RDONLY);
        if (file_descriptor < 0)
            break;
        mapping = mmap (NULL, (size_t)count_value, PROT_READ, MAP_SHARED, file_descriptor, 0);
        close (file_descriptor);
        if (mapping == MAP_FAILED)
            break;
        if (mapping_read_byte (mapping + (size_t)(iteration_index % count_value), &value) != 0 ||
            value != (unsigned char)((iteration_index % count_value) & 0xffu))
        {
            munmap (mapping, (size_t)count_value);
            break;
        }
        if (munmap (mapping, (size_t)count_value) != 0)
            break;
    }
    printf (
        "result=%d completed=%llu signal=%d\n",
        iteration_index == iterations ? 0 : -1,
        (unsigned long long)iteration_index,
        (int)mapping_fault_signal
    );
    return 0;
}

static int run_mmap_tail (const char * path, const char * file_size_text, const char * map_size_text)
{
    uint64_t file_size;
    uint64_t map_size;
    unsigned char * mapping;
    unsigned char value;
    int pattern = 1;
    int zero = 1;
    int file_descriptor;
    size_t byte_index;

    if (parse_u64 (file_size_text, (uint64_t)SIZE_MAX, &file_size) != 0 || parse_u64 (map_size_text, (uint64_t)SIZE_MAX, &map_size) != 0 ||
        file_size == 0 || map_size < file_size)
    {
        fprintf (stderr, "invalid mmap tail sizes\n");
        return 2;
    }
    file_descriptor = open (path, O_RDONLY);
    if (file_descriptor < 0)
    {
        perror (path);
        return 1;
    }
    mapping = mmap (NULL, (size_t)map_size, PROT_READ, MAP_SHARED, file_descriptor, 0);
    close (file_descriptor);
    if (mapping == MAP_FAILED)
    {
        perror ("mmap");
        return 1;
    }
    if (install_mapping_fault_handlers () != 0)
    {
        perror ("sigaction");
        munmap (mapping, (size_t)map_size);
        return 1;
    }
    for (byte_index = 0; byte_index < (size_t)file_size; ++byte_index)
    {
        if (mapping_read_byte (mapping + byte_index, &value) != 0 || value != (unsigned char)(byte_index & 0xffu))
        {
            pattern = 0;
            break;
        }
    }
    for (byte_index = (size_t)file_size; pattern && byte_index < (size_t)map_size; ++byte_index)
    {
        if (mapping_read_byte (mapping + byte_index, &value) != 0 || value != 0)
        {
            zero = 0;
            break;
        }
    }
    if (munmap (mapping, (size_t)map_size) != 0)
    {
        perror ("munmap");
        return 1;
    }
    printf ("result=0 pattern=%d zero=%d signal=%d\n", pattern, zero, (int)mapping_fault_signal);
    return 0;
}

static void run_mapped_mode_child (const char * path, int file_descriptor, int ready[2], int proceed[2], char byte)
{
    unsigned char * mapping;
    close (ready[0]);
    close (proceed[1]);
    close (file_descriptor);
    if (setgid (1001) != 0 || setuid (1001) != 0)
        _exit (2);
    file_descriptor = open (path, O_RDWR);
    if (file_descriptor < 0)
        _exit (3);
    mapping = mmap (NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, 0);
    if (mapping == MAP_FAILED)
        _exit (4);
    mapping[0] = 'A';
    if (msync (mapping, 4096, MS_SYNC) != 0 || write (ready[1], &byte, 1) != 1)
        _exit (5);
    if (read (proceed[0], &byte, 1) != 1)
        _exit (6);
    mapping[0] = 'B';
    if (msync (mapping, 4096, MS_SYNC) != 0 || munmap (mapping, 4096) != 0 || close (file_descriptor) != 0)
        _exit (7);
    _exit (0);
}

static int run_mapped_mode_change (const char * root)
{
    char path[PATH_MAX], byte = 'x';
    int ready[2], proceed[2], file_descriptor = PROBE_INVALID_DESCRIPTOR, status = 0, result = 1;
    pid_t child;
    struct stat value;
    if (snprintf (path, sizeof (path), "%s/mapped-mode-change", root) >= (int)sizeof (path))
        return 1;
    file_descriptor = open (path, O_CREAT | O_EXCL | O_RDWR, 0777);
    if (file_descriptor < 0)
        return 1;
    if (fchmod (file_descriptor, 0777) != 0 || ftruncate (file_descriptor, 4096) != 0 || pipe (ready) != 0 || pipe (proceed) != 0)
        goto out;
    child = fork ();
    if (child == 0)
    {
        run_mapped_mode_child (path, file_descriptor, ready, proceed, byte);
    }
    close (ready[1]);
    close (proceed[0]);
    if (child < 0)
        goto out;
    if (read (ready[0], &byte, 1) != 1 || fchmod (file_descriptor, 06777) != 0 || fstat (file_descriptor, &value) != 0)
    {
        kill (child, SIGKILL);
    }
    else
    {
        printf ("mapped-mode-immediately-after-chmod=%04o\n", (unsigned)(value.st_mode & 07777));
        if (write (proceed[1], &byte, 1) != 1)
            kill (child, SIGKILL);
    }
    close (ready[0]);
    close (proceed[1]);
    if (waitpid (child, &status, 0) != child || !WIFEXITED (status) || WEXITSTATUS (status) != 0 || fstat (file_descriptor, &value) != 0)
        goto out;
    printf ("mapped-mode-after-chmod=%04o\n", (unsigned)(value.st_mode & 07777));
    result = 0;
out:
    if (file_descriptor >= 0)
        close (file_descriptor);
    if (unlink (path) != 0)
        result = 1;
    return result;
}

/* Bounded handshakes keep every permission change outside the child's store. */
static int mapped_policy_byte (int file_descriptor, char expected)
{
    struct pollfd event = { file_descriptor, POLLIN, 0 };
    char byte = 0;
    int rc;
    do
    {
        rc = poll (&event, 1, 15000);
    }
    while (rc < 0 && errno == EINTR);
    return rc > 0 && read (file_descriptor, &byte, 1) == 1 && byte == expected ? 0 : -1;
}

static void run_setid_race_child (const int file_descriptor, int ready[2], int proceed[2], char byte)
{
    unsigned char * mapping;
    alarm (15);
    close (ready[0]);
    close (proceed[1]);
    if (write (ready[1], "A", 1) != 1 || mapped_policy_byte (proceed[0], 'B') != 0)
        _exit (2);
    errno = 0;
    mapping = mmap (NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, 0);
    if (mapping == MAP_FAILED && errno != EBUSY)
        _exit (3);
    if (mapping != MAP_FAILED)
        mapping[0] = 'R';
    byte = mapping == MAP_FAILED ? 'D' : 'B';
    if (write (ready[1], &byte, 1) != 1 || mapped_policy_byte (proceed[0], 'C') != 0)
        _exit (4);
    if (mapping != MAP_FAILED && (msync (mapping, 4096, MS_SYNC) != 0 || munmap (mapping, 4096) != 0))
        _exit (5);
    _exit (close (file_descriptor) != 0 ? 6 : 0);
}

static int run_mapped_setid_race (const char * root)
{
    unsigned iteration;
    signal (SIGPIPE, SIG_IGN);
    for (iteration = 0; iteration < 12; ++iteration)
    {
        char path[PATH_MAX], byte = 0;
        int file_descriptor = PROBE_INVALID_DESCRIPTOR, ready[2] = { PROBE_INVALID_DESCRIPTOR, PROBE_INVALID_DESCRIPTOR },
            proceed[2] = { PROBE_INVALID_DESCRIPTOR, PROBE_INVALID_DESCRIPTOR };
        int status = 0, result = 1, mode_rc, mode_error;
        pid_t child = -1;
        struct stat value;
        if (snprintf (path, sizeof (path), "%s/mapped-setid-race-%u", root, iteration) >= (int)sizeof (path))
            return 1;
        file_descriptor = open (path, O_CREAT | O_EXCL | O_RDWR, 0777);
        if (file_descriptor < 0)
            return 1;
        if (fchmod (file_descriptor, 0777) != 0 || ftruncate (file_descriptor, 4096) != 0 || pipe (ready) != 0 || pipe (proceed) != 0)
            goto out;
        child = fork ();
        if (child == 0)
        {
            run_setid_race_child (file_descriptor, ready, proceed, byte);
        }
        if (child < 0)
            goto out;
        close (ready[1]);
        ready[1] = PROBE_INVALID_DESCRIPTOR;
        close (proceed[0]);
        proceed[0] = PROBE_INVALID_DESCRIPTOR;
        if (mapped_policy_byte (ready[0], 'A') != 0 || write (proceed[1], "B", 1) != 1)
            goto out;
        errno = 0;
        mode_rc = fchmod (file_descriptor, 06777);
        mode_error = errno;
        {
            struct pollfd event = { ready[0], POLLIN, 0 };
            if (poll (&event, 1, 15000) <= 0 || read (ready[0], &byte, 1) != 1)
                goto out;
        }
        if (fstat (file_descriptor, &value) != 0)
            goto out;
        if (byte == 'B')
        {
            if (mode_rc != -1 || mode_error != EBUSY || (value.st_mode & 06000) != 0)
                goto out;
        }
        else if (byte != 'D' || mode_rc != 0 || (value.st_mode & 07777) != 06777)
            goto out;
        if (write (proceed[1], "C", 1) != 1 || waitpid (child, &status, 0) != child)
            goto out;
        child = -1;
        if (!WIFEXITED (status) || WEXITSTATUS (status) != 0)
            goto out;
        result = 0;
out:
        if (child > 0)
        {
            kill (child, SIGKILL);
            (void)waitpid (child, &status, 0);
        }
        if (file_descriptor >= 0 && close (file_descriptor) != 0)
            result = 1;
        if (ready[0] >= 0)
            close (ready[0]);
        if (ready[1] >= 0)
            close (ready[1]);
        if (proceed[0] >= 0)
            close (proceed[0]);
        if (proceed[1] >= 0)
            close (proceed[1]);
        if (unlink (path) != 0)
            result = 1;
        if (result)
        {
            fprintf (stderr, "mapped set-ID race failed: iteration=%u response=%c errno=%d\n", iteration, byte, errno);
            return 1;
        }
    }
    printf ("mapped-setid-race=valid iterations=%u\n", iteration);
    return 0;
}

static int run_mapped_policy_direct (
    const char * kind,
    const char * path,
    const char * alias,
    int * file_descriptor,
    unsigned char ** mapping,
    struct stat * after,
    int * stage
)
{
    const int private_map = strcmp (kind, "private") == 0;
    const int privileged = strcmp (kind, "privileged") == 0;

    if (!private_map && !privileged)
    {
        if (close (*file_descriptor) != 0)
            return 1;
        *file_descriptor = open (path, O_RDONLY);
        if (*file_descriptor < 0)
            return 1;
    }
    if (chmod (alias, 06777) != 0)
        return 1;

    errno = 0;
    *mapping =
        mmap (NULL, 4096, PROT_READ | (private_map || privileged ? PROT_WRITE : 0), private_map ? MAP_PRIVATE : MAP_SHARED, *file_descriptor, 0);
    if (privileged)
    {
        if (*mapping != MAP_FAILED || errno != EBUSY || fstat (*file_descriptor, after) != 0 || (after->st_mode & 07777) != 06777)
            return 1;
        if (chmod (alias, 0777) != 0)
            return 1;
        *mapping = mmap (NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, *file_descriptor, 0);
        if (*mapping == MAP_FAILED)
            return 1;
    }
    else
    {
        if (*mapping == MAP_FAILED)
            return 1;
        if (private_map)
            (*mapping)[0] = 'P';
        else if ((*mapping)[0] != 0)
            return 1;
        if (fstat (*file_descriptor, after) != 0 || (after->st_mode & 07777) != 06777)
            return 1;
    }

    *stage = 2;
    if (munmap (*mapping, 4096) != 0)
        return 1;
    *mapping = MAP_FAILED;
    return 0;
}

static void run_mapped_policy_child (const char * kind, const char * path, int file_descriptor, int ready[2], int proceed[2], int upgrade)
{
    unsigned char * mapping;
    pid_t inherited;
    int status;
    char byte;

    alarm (15);
    if (setpgid (0, 0) != 0)
        _exit (2);
    close (ready[0]);
    close (proceed[1]);
    close (file_descriptor);
    if (strcmp (kind, "upgrade-privileged") != 0 && (setgid (1001) != 0 || setuid (1001) != 0))
        _exit (3);

    file_descriptor = open (path, O_RDWR);
    if (file_descriptor < 0)
        _exit (4);
    mapping = mmap (NULL, 4096, PROT_READ | (upgrade ? 0 : PROT_WRITE), MAP_SHARED, file_descriptor, 0);
    if (mapping == MAP_FAILED || close (file_descriptor) != 0)
        _exit (5);

    if (strcmp (kind, "fork") == 0)
    {
        inherited = fork ();
        if (inherited < 0)
            _exit (6);
        if (inherited > 0)
        {
            if (munmap (mapping, 4096) != 0)
                _exit (7);
            if (waitpid (inherited, &status, 0) != inherited || !WIFEXITED (status))
                _exit (8);
            _exit (WEXITSTATUS (status));
        }
        alarm (15);
    }

    if (!upgrade)
        mapping[0] = 'A';
    if (write (ready[1], "A", 1) != 1 || mapped_policy_byte (proceed[0], 'B') != 0)
        _exit (9);
    if (upgrade && mprotect (mapping, 4096, PROT_READ | PROT_WRITE) != 0)
    {
        if (errno != EBUSY && errno != EACCES)
            _exit (10);
        byte = 'D';
    }
    else
    {
        mapping[0] = 'B';
        byte = 'B';
    }
    if (write (ready[1], &byte, 1) != 1 || mapped_policy_byte (proceed[0], 'C') != 0)
        _exit (11);
    if (msync (mapping, 4096, MS_SYNC) != 0 || munmap (mapping, 4096) != 0)
        _exit (12);
    _exit (0);
}

static int check_mapped_policy_grant (const int file_descriptor, const char * alias, const int upgrade, int * grant_blocked)
{
    struct stat before, after;

    if (fstat (file_descriptor, &before) != 0)
        return 1;
    errno = 0;
    {
        const int mode_rc = chmod (alias, 06777);
        (*grant_blocked) = mode_rc == -1 && errno == EBUSY;
        if (!(*grant_blocked) && (!upgrade || mode_rc != 0))
            return 1;
    }
    if ((*grant_blocked) && (fstat (file_descriptor, &after) != 0 || after.st_mode != before.st_mode || after.st_ctime != before.st_ctime))
        return 1;
    if (!upgrade)
    {
        /* Adding execute later must not bypass the combined-mode check. */
        if (fchmod (file_descriptor, 06666) != 0 || fstat (file_descriptor, &before) != 0)
            return 1;
        errno = 0;
        if (fchmod (file_descriptor, 06777) != -1 || errno != EBUSY || fstat (file_descriptor, &after) != 0 || after.st_mode != before.st_mode ||
            after.st_ctime != before.st_ctime || fchmod (file_descriptor, 0777) != 0)
            return 1;
    }
    return 0;
}

static int exercise_mapped_policy_parent (
    const char * kind,
    const char * path,
    const char * alias,
    const int file_descriptor,
    int ready[2],
    int proceed[2],
    const int upgrade,
    pid_t * child,
    int * stage
)
{
    struct stat after;
    int grant_blocked = 0;
    int status = 0;
    char byte;

    if (pipe (ready) != 0 || pipe (proceed) != 0)
        return 1;
    (*child) = fork ();
    if ((*child) == 0)
        run_mapped_policy_child (kind, path, file_descriptor, ready, proceed, upgrade);
    if ((*child) < 0)
        return 1;
    (void)setpgid ((*child), (*child));
    close (ready[1]);
    ready[1] = PROBE_INVALID_DESCRIPTOR;
    close (proceed[0]);
    proceed[0] = PROBE_INVALID_DESCRIPTOR;
    if (mapped_policy_byte (ready[0], 'A') != 0)
        return 1;
    (*stage) = 3;
    if (check_mapped_policy_grant (file_descriptor, alias, upgrade, &grant_blocked) != 0)
        return 1;
    (*stage) = 4;
    if (write (proceed[1], "B", 1) != 1)
        return 1;
    {
        struct pollfd event = { ready[0], POLLIN, 0 };
        if (poll (&event, 1, 15000) <= 0 || read (ready[0], &byte, 1) != 1 || (byte != 'B' && byte != 'D'))
            return 1;
    }
    if (upgrade)
        printf ("%s: set-ID grant=%s, protection upgrade=%s\n", kind, grant_blocked ? "blocked" : "allowed", byte == 'D' ? "blocked" : "allowed");
    /* Inspect before allowing msync: pageout-only stripping is too late. */
    if (byte == 'B' && (fstat (file_descriptor, &after) != 0 || ((after.st_mode & 0111) && (after.st_mode & (S_ISUID | S_ISGID)))))
        return 1;
    if (write (proceed[1], "C", 1) != 1)
        return 1;
    if (waitpid ((*child), &status, 0) != (*child))
        return 1;
    (*child) = -1;
    if (!WIFEXITED (status) || WEXITSTATUS (status) != 0)
        return 1;
    (*stage) = 5;
    if (!upgrade)
    {
        errno = 0;
        if (fchmod (file_descriptor, 06777) != -1 || errno != EBUSY)
            return 1;
    }
    return 0;
}

static int run_mapped_setid_policy (const char * root, const char * kind)
{
    char path[PATH_MAX], alias[PATH_MAX];
    int file_descriptor = PROBE_INVALID_DESCRIPTOR, ready[2] = { PROBE_INVALID_DESCRIPTOR, PROBE_INVALID_DESCRIPTOR },
        proceed[2] = { PROBE_INVALID_DESCRIPTOR, PROBE_INVALID_DESCRIPTOR };
    int result = 1, status = 0, stage = 0;
    int upgrade = strcmp (kind, "upgrade") == 0 || strcmp (kind, "upgrade-privileged") == 0;
    pid_t child = -1;
    unsigned char * mapping = MAP_FAILED;
    struct stat after;
    if (snprintf (path, sizeof (path), "%s/mapped-policy-%s", root, kind) >= (int)sizeof (path) ||
        snprintf (alias, sizeof (alias), "%s.alias", path) >= (int)sizeof (alias))
        return 1;
    signal (SIGPIPE, SIG_IGN);
    file_descriptor = open (path, O_CREAT | O_EXCL | O_RDWR, 0777);
    if (file_descriptor < 0)
        return 1;
    if (fchmod (file_descriptor, 0777) != 0 || ftruncate (file_descriptor, 4096) != 0 || link (path, alias) != 0)
        goto out;
    stage = 1;
    if (strcmp (kind, "private") == 0 || strcmp (kind, "readonly") == 0 || strcmp (kind, "privileged") == 0)
    {
        if (run_mapped_policy_direct (kind, path, alias, &file_descriptor, &mapping, &after, &stage) != 0)
            goto out;
    }
    else
    {
        if (exercise_mapped_policy_parent (kind, path, alias, file_descriptor, ready, proceed, upgrade, &child, &stage) != 0)
            goto out;
    }
    if (close (file_descriptor) != 0)
    {
        file_descriptor = PROBE_INVALID_DESCRIPTOR;
        goto out;
    }
    file_descriptor = PROBE_INVALID_DESCRIPTOR;
    stage = 6;
    /* No mapping/descriptor keeps the old vnode: a fresh lookup may grant mode. */
    if (chmod (alias, 06777) != 0 || stat (alias, &after) != 0 || (after.st_mode & 07777) != 06777)
        goto out;
    result = 0;
out:
    if (result)
        fprintf (stderr, "mapped set-ID policy failed: kind=%s stage=%d errno=%d\n", kind, stage, errno);
    if (child > 0)
    {
        kill (-child, SIGKILL);
        kill (child, SIGKILL);
        (void)waitpid (child, &status, 0);
    }
    if (mapping != MAP_FAILED)
        munmap (mapping, 4096);
    if (file_descriptor >= 0)
        close (file_descriptor);
    if (ready[0] >= 0)
        close (ready[0]);
    if (ready[1] >= 0)
        close (ready[1]);
    if (proceed[0] >= 0)
        close (proceed[0]);
    if (proceed[1] >= 0)
        close (proceed[1]);
    if (unlink (alias) != 0 && errno != ENOENT)
        result = 1;
    if (unlink (path) != 0)
        result = 1;
    if (!result)
        printf ("mapped-setid-%s=valid\n", kind);
    return result;
}

static int run_held_close (const char * path, const char * ready, const char * release)
{
    int file_descriptor = open (path, O_RDONLY), marker, result, error;
    unsigned attempts = 0;
    if (file_descriptor < 0)
        return 1;
    marker = open (ready, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (marker < 0)
    {
        close (file_descriptor);
        return 1;
    }
    close (marker);
    while (access (release, F_OK) != 0 && attempts++ < 200)
        usleep (100000);
    if (attempts >= 200)
    {
        close (file_descriptor);
        return 1;
    }
    errno = 0;
    result = close (file_descriptor);
    error = errno;
    printf ("close=%d errno=%d\n", result, error);
    return result == 0 || error == EAGAIN ? 0 : 1;
}

/* The scenario backend supplies sparse deterministic bytes. Only a handful of
 * pages are touched while the same vnode segment grows beyond 32-bit offsets. */
static int run_mmap_grow_large (const char * path)
{
    const off_t high = (off_t)4294967296ULL;
    volatile unsigned char *low = MAP_FAILED, *upper = MAP_FAILED;
    int file_descriptor = open (path, O_RDWR), result = 1;
    unsigned char value = 0;
    struct stat attributes;
    const char * stage = "initial map";
    if (file_descriptor < 0)
        return 1;
    low = mmap (NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, 0);
    if (low == MAP_FAILED || low[17] != 17)
        goto out;
    stage = "grow";
    if (ftruncate (file_descriptor, high + 4096) != 0 || fstat (file_descriptor, &attributes) != 0 || attributes.st_size != high + 4096)
        goto out;
    stage = "upper map";
    upper = mmap (NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, high);
    if (upper == MAP_FAILED || upper[17] != 17)
        goto out;
    stage = "ordinary cached I/O";
    if (pwrite (file_descriptor, "Z", 1, high + 17) != 1 || upper[17] != 'Z')
        goto out;
    upper[18] = 'Y';
    if (pread (file_descriptor, &value, 1, high + 18) != 1 || value != 'Y' || fsync (file_descriptor) != 0)
        goto out;
    result = 0;
out:
    if (upper != MAP_FAILED && munmap ((void *)upper, 4096) != 0)
        result = 1;
    if (low != MAP_FAILED && munmap ((void *)low, 4096) != 0)
        result = 1;
    if (close (file_descriptor) != 0)
        result = 1;
    printf ("mmap-grow-large=%s stage=%s errno=%d\n", result ? "failed" : "valid", stage, errno);
    return result;
}

enum mmap_sync_operation
{
    MMAP_SYNC_FILE,
    MMAP_SYNC_RANGE,
    MMAP_SYNC_FILESYSTEM,
    MMAP_SYNC_ZERO_RANGE
};

struct mmap_sync_case
{
    enum mmap_sync_operation operation; // Selected synchronization operation.
    int range_flags;                    // Flags passed to range synchronization.
    off_t range_offset;                 // Starting range offset in bytes.
    size_t mapping_size;                // Length of the shared mapping in bytes.
};

static struct mmap_sync_case mmap_sync_case_from_name (const char * kind)
{
    struct mmap_sync_case test_case;

    memset (&test_case, 0, sizeof (test_case));
    test_case.mapping_size = 4096;
    if (strncmp (kind, "range-zero", 10) == 0)
    {
        test_case.operation = MMAP_SYNC_ZERO_RANGE;
        test_case.range_flags = strstr (kind, "nocache") ? O_NOCACHE : 0;
        test_case.range_offset = strstr (kind, "eof") ? 16384 : 4097;
        test_case.mapping_size = 8192;
    }
    else if (strcmp (kind, "range") == 0)
    {
        test_case.operation = MMAP_SYNC_RANGE;
    }
    else if (strcmp (kind, "filesystem") == 0)
    {
        test_case.operation = MMAP_SYNC_FILESYSTEM;
    }
    else
    {
        test_case.operation = MMAP_SYNC_FILE;
    }
    return test_case;
}

static int mmap_sync_parent_path (const char * path, char * parent, size_t parent_size)
{
    char * slash;

    if (snprintf (parent, parent_size, "%s", path) >= (int)parent_size)
        return 1;
    slash = strrchr (parent, '/');
    if (slash == NULL || slash == parent)
        return 1;
    *slash = '\0';
    return 0;
}

static int mmap_sync_first_operation (const struct mmap_sync_case * test_case, int file_descriptor, char * mountpoint)
{
    switch (test_case->operation)
    {
        case MMAP_SYNC_ZERO_RANGE:
            return fsync_range (file_descriptor, test_case->range_flags, test_case->range_offset, 0);
        case MMAP_SYNC_RANGE:
            return fsync_range (file_descriptor, 0, 0, 4096);
        case MMAP_SYNC_FILESYSTEM:
            return syncvfs (mountpoint, FS_SYNCVFS_FS | FS_SYNCVFS_FORCE);
        case MMAP_SYNC_FILE:
            return fsync (file_descriptor);
    }
    return -1;
}

static int run_mmap_sync (const char * path, const char * kind)
{
    const struct mmap_sync_case test_case = mmap_sync_case_from_name (kind);
    char mountpoint[PATH_MAX];
    unsigned char * mapping;
    int first;
    int second;
    int first_error;
    int second_error;
    int file_descriptor;

    file_descriptor = open (path, O_RDWR);
    if (file_descriptor < 0)
        return 1;
    mapping = mmap (NULL, test_case.mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, 0);
    if (mapping == MAP_FAILED)
    {
        close (file_descriptor);
        return 1;
    }
    if (test_case.operation == MMAP_SYNC_FILESYSTEM)
    {
        if (mmap_sync_parent_path (path, mountpoint, sizeof (mountpoint)) != 0)
            goto invalid;
        if (close (file_descriptor) != 0)
        {
            file_descriptor = PROBE_INVALID_DESCRIPTOR;
            goto invalid;
        }
        file_descriptor = PROBE_INVALID_DESCRIPTOR;
    }
    /* Filesystem sync must retain a backing handle even after all descriptors
     * close, including for the first mapped fault and subsequent writeback. */
    mapping[17] = 0x5a;
    if (test_case.operation == MMAP_SYNC_ZERO_RANGE)
        mapping[4096 + 17] = 0x6b;
    errno = 0;
    first = mmap_sync_first_operation (&test_case, file_descriptor, mountpoint);
    first_error = errno;
    errno = 0;
    second = test_case.operation == MMAP_SYNC_FILESYSTEM ? syncvfs (mountpoint, FS_SYNCVFS_FS | FS_SYNCVFS_FORCE) : fsync (file_descriptor);
    second_error = errno;
    printf (
        "first=%d first_errno=%d second=%d second_errno=%d pid=%ld descriptors_closed=%d\n",
        first,
        first_error,
        second,
        second_error,
        (long)getpid (),
        file_descriptor < 0
    );
    (void)munmap (mapping, test_case.mapping_size);
    if (file_descriptor >= 0)
        (void)close (file_descriptor);
    return 0;
invalid:
    (void)munmap (mapping, test_case.mapping_size);
    if (file_descriptor >= 0)
        (void)close (file_descriptor);
    return 1;
}

static int exercise_mapping_truncate (const int file_descriptor, unsigned char * mapping, const uint64_t old_size, const uint64_t new_size)
{
    unsigned char value;
    struct stat attributes;
    int beyond_fault = 0;
    int sync_result;
    int sync_errno;

    if (install_mapping_fault_handlers () != 0)
    {
        perror ("sigaction");
        munmap (mapping, (size_t)old_size);
        close (file_descriptor);
        return 1;
    }
    if (mapping_read_byte (mapping, &value) != 0)
    {
        printf ("result=-1 initial_signal=%d\n", (int)mapping_fault_signal);
        munmap (mapping, (size_t)old_size);
        close (file_descriptor);
        return 0;
    }
    if (ftruncate (file_descriptor, (off_t)new_size) != 0)
    {
        perror ("ftruncate");
        munmap (mapping, (size_t)old_size);
        close (file_descriptor);
        return 1;
    }
    if (mapping_write_byte (mapping + (size_t)old_size - 1u, 0x5au) != 0)
        beyond_fault = 1;
    errno = 0;
    sync_result = msync (mapping, (size_t)old_size, MS_SYNC);
    sync_errno = errno;
    if (fstat (file_descriptor, &attributes) != 0)
    {
        perror ("fstat");
        munmap (mapping, (size_t)old_size);
        close (file_descriptor);
        return 1;
    }
    if (munmap (mapping, (size_t)old_size) != 0)
    {
        perror ("munmap");
        close (file_descriptor);
        return 1;
    }
    close (file_descriptor);
    printf (
        "result=0 fault=%d signal=%d sync=%d sync_errno=%d size=%lld\n",
        beyond_fault,
        (int)mapping_fault_signal,
        sync_result,
        sync_errno,
        (long long)attributes.st_size
    );
    return 0;
}

static int run_mmap_truncate (const char * path, const char * old_size_text, const char * new_size_text)
{
    uint64_t old_size;
    uint64_t new_size;
    unsigned char * mapping;
    int file_descriptor;

    if (parse_u64 (old_size_text, (uint64_t)SIZE_MAX, &old_size) != 0 || parse_u64 (new_size_text, (uint64_t)SIZE_MAX, &new_size) != 0 ||
        old_size == 0 || new_size >= old_size)
    {
        fprintf (stderr, "invalid mmap truncate sizes\n");
        return 2;
    }
    file_descriptor = open (path, O_RDWR);
    if (file_descriptor < 0)
    {
        perror (path);
        return 1;
    }
    mapping = mmap (NULL, (size_t)old_size, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, 0);
    if (mapping == MAP_FAILED)
    {
        perror ("mmap");
        close (file_descriptor);
        return 1;
    }
    return exercise_mapping_truncate (file_descriptor, mapping, old_size, new_size);
}

static int wait_mapping_release (const char * ready_path, const char * release_path)
{
    FILE * ready;
    unsigned elapsed = 0;

    ready = fopen (ready_path, "w");
    if (ready == NULL)
    {
        perror (ready_path);
        return 1;
    }
    fputs ("ready\n", ready);
    if (fclose (ready) != 0)
    {
        perror ("fclose");
        return 1;
    }
    while (access (release_path, F_OK) != 0 && elapsed < 30000u)
    {
        usleep (10000u);
        elapsed += 10u;
    }
    if (access (release_path, F_OK) != 0)
    {
        fprintf (stderr, "release timeout\n");
        return 1;
    }
    return 0;
}

static int run_mmap_hold (
    const char * path,
    const char * count_text,
    const char * index_text,
    const char * ready_path,
    const char * release_path,
    int release_mode
)
{
    uint64_t count_value;
    uint64_t index_value;
    unsigned char * mapping;
    unsigned char value;
    int result;
    int file_descriptor;

    if (parse_u64 (count_text, (uint64_t)SIZE_MAX, &count_value) != 0 || parse_u64 (index_text, (uint64_t)SIZE_MAX, &index_value) != 0 ||
        count_value == 0 || index_value >= count_value)
    {
        fprintf (stderr, "invalid mmap hold range\n");
        return 2;
    }
    file_descriptor = open (path, release_mode == 2 ? O_RDWR : O_RDONLY);
    if (file_descriptor < 0)
    {
        perror (path);
        return 1;
    }
    mapping = mmap (NULL, (size_t)count_value, release_mode == 2 ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, file_descriptor, 0);
    close (file_descriptor);
    if (mapping == MAP_FAILED)
    {
        perror ("mmap");
        return 1;
    }
    if (install_mapping_fault_handlers () != 0 || mapping_read_byte (mapping, &value) != 0)
    {
        printf ("result=-1 stage=initial signal=%d\n", (int)mapping_fault_signal);
        munmap (mapping, (size_t)count_value);
        return 0;
    }
    if (wait_mapping_release (ready_path, release_path) != 0)
    {
        munmap (mapping, (size_t)count_value);
        return 1;
    }

    if (release_mode != 0)
    {
        result = munmap (mapping, (size_t)count_value);
        printf ("result=%d stage=release errno=%d\n", result, result == 0 ? 0 : errno);
        return result == 0 ? 0 : 1;
    }
    result = mapping_read_byte (mapping + (size_t)index_value, &value);
    printf ("result=%d stage=second signal=%d value=%u\n", result == 0 ? 0 : -1, (int)mapping_fault_signal, (unsigned)value);
    munmap (mapping, (size_t)count_value);
    return 0;
}

static int run_readdir_loop (const char * path, const char * count_text)
{
    uint64_t count_value;
    char * buffer;
    off_t base = 0;
    ssize_t result;
    ssize_t total = 0;
    unsigned calls = 0;
    int saved_errno = 0;
    int file_descriptor;

    if (parse_u64 (count_text, (uint64_t)PROBE_TRANSFER_CHUNK_SIZE, &count_value) != 0 || count_value == 0)
    {
        fprintf (stderr, "invalid directory count\n");
        return 2;
    }
    buffer = malloc ((size_t)count_value);
    if (buffer == NULL)
    {
        perror ("malloc");
        return 1;
    }
    file_descriptor = open (path, O_RDONLY);
    if (file_descriptor < 0)
    {
        perror (path);
        free (buffer);
        return 1;
    }

    do
    {
        errno = 0;
        result = getdirentries (file_descriptor, buffer, (size_t)count_value, &base);
        saved_errno = errno;
        calls += 1;
        if (result > 0)
            total += result;
    }
    while (result > 0 && calls < 16);

    printf ("result=%lld errno=%d base=%lld calls=%u total=%lld\n", (long long)result, saved_errno, (long long)base, calls, (long long)total);
    close (file_descriptor);
    free (buffer);
    return 0;
}

static int run_dual_open (const char * path)
{
    char value = '\0';
    const char first = 'P';
    const char second = 'Q';
    int read_fd = PROBE_INVALID_DESCRIPTOR;
    int write_fd = PROBE_INVALID_DESCRIPTOR;
    int result = -1;

    read_fd = open (path, O_RDONLY);
    if (read_fd < 0)
        goto done;
    write_fd = open (path, O_WRONLY);
    if (write_fd < 0)
        goto done;
    if (pwrite (write_fd, &first, 1, 0) != 1)
        goto done;
    if (pread (read_fd, &value, 1, 0) != 1)
        goto done;
    if (close (read_fd) != 0)
        goto done;
    read_fd = PROBE_INVALID_DESCRIPTOR;
    if (pwrite (write_fd, &second, 1, 1) != 1)
        goto done;
    result = 0;

done:
    if (read_fd >= 0)
        close (read_fd);
    if (write_fd >= 0)
        close (write_fd);
    printf ("result=%d errno=%d first=%u\n", result, errno, (unsigned)(unsigned char)value);
    return 0;
}

static int execute_vnode_operation (const char * operation, const char * path, int * file_descriptor, long * result)
{
    struct flock lock;

    errno = 0;
    if (strcmp (operation, "fsync") == 0)
    {
        (*result) = fsync ((*file_descriptor));
    }
    else if (strcmp (operation, "fsync-range") == 0)
    {
        (*result) = fsync_range ((*file_descriptor), 0, 0, 1);
    }
    else if (strcmp (operation, "close") == 0)
    {
        (*result) = close ((*file_descriptor));
        (*file_descriptor) = PROBE_INVALID_DESCRIPTOR;
    }
    else if (strcmp (operation, "syncfs") == 0 || strcmp (operation, "syncfs-try") == 0)
    {
        (*result) = syncvfs ((char *)path, FS_SYNCVFS_FS | (strcmp (operation, "syncfs-try") == 0 ? FS_SYNCVFS_TRY : FS_SYNCVFS_FORCE));
    }
    else if (strcmp (operation, "syncfs-type") == 0)
    {
        (*result) = syncvfs ((char *)"usfs", FS_SYNCVFS_FSTYPE | FS_SYNCVFS_FORCE);
    }
    else if (strcmp (operation, "fclear") == 0)
    {
        (*result) = fclear ((*file_descriptor), 1);
    }
    else if (strcmp (operation, "lock") == 0)
    {
        memset (&lock, 0, sizeof (lock));
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        lock.l_len = 1;
        (*result) = fcntl ((*file_descriptor), F_SETLK, &lock);
    }
    else
    {
        fprintf (stderr, "invalid vnode operation: %s\n", operation);
        if ((*file_descriptor) >= 0)
            close ((*file_descriptor));
        return 2;
    }
    return 0;
}

static int run_vnode_operation (const char * operation, const char * path)
{
    off_t final_offset;
    long result;
    int saved_errno;
    int file_descriptor;

    file_descriptor = PROBE_INVALID_DESCRIPTOR;
    if (strcmp (operation, "syncfs") != 0 && strcmp (operation, "syncfs-try") != 0 && strcmp (operation, "syncfs-type") != 0)
    {
        file_descriptor = open (path, O_RDWR);
        if (file_descriptor < 0)
        {
            perror (path);
            return 1;
        }
    }

    const int rc = execute_vnode_operation (operation, path, &file_descriptor, &result);
    if (rc != 0)
        return rc;
    saved_errno = errno;
    final_offset = file_descriptor >= 0 ? lseek (file_descriptor, 0, SEEK_CUR) : 0;

    printf ("result=%lld errno=%d final=%lld\n", (long long)result, saved_errno, (long long)final_offset);
    printf ("ENOSYS=%d\n", ENOSYS);
    printf ("EIO=%d\n", EIO);
    if (file_descriptor >= 0)
        close (file_descriptor);
    return 0;
}

static int run_retained_handles (const char * path)
{
    struct stat attributes;
    int first = PROBE_INVALID_DESCRIPTOR, second = PROBE_INVALID_DESCRIPTOR, original = PROBE_INVALID_DESCRIPTOR;
    int result = 1;
    volatile unsigned char * mapping = MAP_FAILED;

    first = open (path, O_RDONLY);
    if (first < 0)
        goto done;
    original = first;
    mapping = mmap (NULL, 4096, PROT_READ, MAP_SHARED, first, 0);
    if (mapping == MAP_FAILED)
        goto done;
    if (close (first) != 0)
    {
        first = PROBE_INVALID_DESCRIPTOR;
        goto done;
    }
    first = PROBE_INVALID_DESCRIPTOR;
    second = open (path, O_RDWR);
    if (second < 0 || second != original || fstat (second, &attributes) != 0 || attributes.st_size <= 0)
        goto done;
    if (fsync (second) != 0)
    {
        perror ("fsync replacement");
        goto done;
    }
    /* First page fault occurs after descriptor reuse. The old mapping must
     * still route through its original handle, not the replacement open. */
    if (mapping[0] != (unsigned char)'c')
        goto done;
    result = 0;
done:
    if (first >= 0 && close (first) != 0)
        result = 1;
    if (second >= 0 && close (second) != 0)
        result = 1;
    if (mapping != MAP_FAILED && munmap ((void *)mapping, 4096) != 0)
        result = 1;
    printf ("result=%d descriptor_reused=%d\n", result, second >= 0 && second == original);
    return result;
}

static volatile sig_atomic_t request_signal_seen;

static void on_request_signal (int signal_number)
{
    (void)signal_number;
    request_signal_seen = 1;
}

static int run_signaled_request (const char * operation, const char * path)
{
    struct sigaction action;
    struct stat attributes;
    int result, saved_errno, file_descriptor = PROBE_INVALID_DESCRIPTOR;
    char value;
    const int read_operation = strcmp (operation, "signaled-read") == 0;
    const int observe = strcmp (operation, "signaled-stat") == 0 || read_operation;

    memset (&action, 0, sizeof (action));
    action.sa_handler = on_request_signal;
    sigemptyset (&action.sa_mask);
    /* Deliberately omit SA_RESTART: transport completion must preserve the
     * result without having libc restart an interrupted operation. */
    if (sigaction (SIGUSR1, &action, NULL) != 0)
        return 1;
    if (read_operation)
    {
        file_descriptor = open (path, O_RDONLY);
        if (file_descriptor < 0)
            return 1;
    }
    errno = 0;
    result = read_operation ? (int)read (file_descriptor, &value, 1)
             : observe      ? stat (path, &attributes)
                            : open (path, strcmp (operation, "signaled-create") == 0 ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY, 0600);
    saved_errno = errno;
    const int close_rc = file_descriptor >= 0 ? close (file_descriptor) : 0;
    printf ("result=%d errno=%d signal=%d\n", result < 0 ? -1 : 0, saved_errno, (int)request_signal_seen);
    fflush (stdout);
    if (result < 0 || !request_signal_seen || (read_operation && (result != 1 || close_rc != 0)))
        return 1;
    if (!observe)
    {
        /* Leave the successful open to AIX process teardown. Its handle must
         * receive exactly one RELEASE even after the delivered-call signal. */
        raise (SIGTERM);
        return 1;
    }
    return 0;
}

static int readonly_result (const char * operation, int result)
{
    const int error = errno;
    printf ("readonly %s result=%d errno=%d\n", operation, result, error);
    if (result >= 0 && strcmp (operation, "create") == 0)
        close (result);
    return result == -1 && error == EROFS ? 0 : 1;
}

static int run_readonly (const char * root, int populated)
{
    char file[PATH_MAX], directory[PATH_MAX], destination[PATH_MAX];
    struct timeval times[2] = { { 1, 0 }, { 1, 0 } };
    int failed = 0, file_descriptor;
    if (snprintf (file, sizeof (file), "%s/file", root) >= (int)sizeof (file) ||
        snprintf (directory, sizeof (directory), "%s/dir", root) >= (int)sizeof (directory) ||
        snprintf (destination, sizeof (destination), "%s/new", root) >= (int)sizeof (destination))
        return 1;
    failed |= readonly_result ("create", open (destination, O_CREAT | O_EXCL | O_RDWR, 0600));
    failed |= readonly_result ("mkdir", mkdir (destination, 0700));
    failed |= readonly_result ("symlink", symlink ("file", destination));
    failed |= readonly_result ("chmod", chmod (root, 0700));
    failed |= readonly_result ("chown", chown (root, 1, 1));
    failed |= readonly_result ("utimes", utimes (root, times));
    if (!populated)
        return failed;
    failed |= readonly_result ("truncate", truncate (file, 0));
    failed |= readonly_result ("link", link (file, destination));
    failed |= readonly_result ("rename", rename (file, destination));
    failed |= readonly_result ("unlink", unlink (file));
    failed |= readonly_result ("rmdir", rmdir (directory));
    file_descriptor = open (file, O_WRONLY | O_APPEND);
    failed |= readonly_result ("open-write", file_descriptor);
    if (file_descriptor >= 0)
        close (file_descriptor);
    file_descriptor = open (file, O_RDWR | O_TRUNC);
    failed |= readonly_result ("open-truncate", file_descriptor);
    if (file_descriptor >= 0)
        close (file_descriptor);
    file_descriptor = open (file, O_RDONLY);
    if (file_descriptor < 0)
        return 1;
    {
        char contents[14] = { 0 };
        void * mapping;
        failed |= read (file_descriptor, contents, 13) != 13 || strcmp (contents, "contract-data") != 0;
        mapping = mmap (NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, 0);
        failed |= mapping != MAP_FAILED;
        if (mapping != MAP_FAILED)
            munmap (mapping, 4096);
    }
    failed |= close (file_descriptor) != 0;
    return failed;
}

static int run_create_flags (const char * root)
{
    const char * names[] = { "create-ro", "create-wo", "create-rw" };
    const int modes[] = { O_RDONLY, O_WRONLY, O_RDWR };
    char path[PATH_MAX], byte;
    int index, failed = 0;
    for (index = 0; index < 3; ++index)
    {
        int file_descriptor;
        if (snprintf (path, sizeof (path), "%s/%s", root, names[index]) >= (int)sizeof (path))
            return 1;
        file_descriptor = open (path, O_CREAT | O_EXCL | modes[index], 0600);
        if (file_descriptor < 0)
        {
            perror ("create flags");
            return 1;
        }
        failed |= (fcntl (file_descriptor, F_GETFL) & O_ACCMODE) != modes[index];
        errno = 0;
        if (modes[index] == O_WRONLY)
            failed |= read (file_descriptor, &byte, 1) != -1 || errno != EBADF;
        else
            failed |= read (file_descriptor, &byte, 1) != 0;
        errno = 0;
        if (modes[index] == O_RDONLY)
            failed |= write (file_descriptor, "x", 1) != -1 || errno != EBADF;
        else
            failed |= write (file_descriptor, "x", 1) != 1;
        if (modes[index] == O_RDWR)
        {
            failed |= lseek (file_descriptor, 0, SEEK_SET) != 0;
            failed |= read (file_descriptor, &byte, 1) != 1 || byte != 'x';
        }
        failed |= close (file_descriptor) != 0;
        printf ("create-mode=%d valid=%d\n", modes[index], !failed);
    }
    snprintf (path, sizeof (path), "%s/unsupported", root);
    {
        struct stat attributes;
        int file_descriptor;
        errno = 0;
        file_descriptor = open (path, O_CREAT | O_EXCL | O_RDWR | O_SYNC, 0600);
        failed |= file_descriptor != PROBE_INVALID_DESCRIPTOR || errno != EOPNOTSUPP;
        if (file_descriptor >= 0)
            close (file_descriptor);
        errno = 0;
        failed |= lstat (path, &attributes) != -1 || errno != ENOENT;
    }
    return failed;
}

static int hardlink_mappings_verify_coherence (
    int first_descriptor,
    int second_descriptor,
    unsigned char * first_mapping,
    unsigned char * second_mapping,
    char * data,
    size_t size,
    const char ** stage
)
{
    *stage = "shared page identity";
    first_mapping[0] = 'A';
    second_mapping[1] = 'B';
    if (pread (first_descriptor, data, 2, 0) != 2 || memcmp (data, "AB", 2) != 0)
    {
        *stage = "ordinary read observes dirty mapped data";
        return 1;
    }
    if (pwrite (first_descriptor, "P", 1, 17) != 1 || first_mapping[17] != 'P' || second_mapping[17] != 'P')
    {
        *stage = "mapped read observes ordinary write";
        return 1;
    }
    if (first_mapping[1] != 'B' || second_mapping[0] != 'A' || msync (first_mapping, size, MS_SYNC) != 0 ||
        msync (second_mapping, size, MS_SYNC) != 0 || pread (first_descriptor, data, 2, 0) != 2 || memcmp (data, "AB", 2) != 0)
        return 1;
    first_mapping[2] = 'C';
    second_mapping[3] = 'D';
    *stage = "fsync of dirty mappings without preceding msync";
    if (fsync (first_descriptor) != 0 || pread (second_descriptor, data, 4, 0) != 4 || memcmp (data, "ABCD", 4) != 0)
        return 1;
    if (pread (first_descriptor, data, 1, 17) != 1 || data[0] != 'P')
    {
        *stage = "pageout preserves acknowledged ordinary write";
        return 1;
    }

    return 0;
}

static int hardlink_mappings_verify_resize (
    int first_descriptor,
    int second_descriptor,
    unsigned char * first_mapping,
    unsigned char * second_mapping,
    char * data,
    size_t size,
    const char ** stage
)
{
    unsigned char value;
    struct stat attributes;

    *stage = "cross-alias truncate";
    if (second_mapping[4096] != '0')
        return 1;
    second_mapping[4097] = 'T';
    if (install_mapping_fault_handlers () != 0 || ftruncate (first_descriptor, 4096) != 0 || mapping_read_byte (second_mapping + 4096, &value) == 0 ||
        fstat (second_descriptor, &attributes) != 0 || attributes.st_size != 4096)
        return 1;

    *stage = "regrow discarded dirty pages";
    if (ftruncate (first_descriptor, size) != 0 || second_mapping[4096] != 0 || second_mapping[4097] != 0 ||
        pread (first_descriptor, data, 2, 4096) != 2 || data[0] != 0 || data[1] != 0)
        return 1;

    *stage = "partial final page shrink and regrow";
    first_mapping[4080] = 'K';
    first_mapping[4081] = 'S';
    if (ftruncate (first_descriptor, 4081) != 0 || ftruncate (first_descriptor, size) != 0 || second_mapping[4080] != 'K' ||
        second_mapping[4081] != 0 || second_mapping[4096] != 0 || fsync (first_descriptor) != 0)
        return 1;

    return 0;
}

static int hardlink_mappings_verify_write_only (
    const char * second,
    unsigned char * first_mapping,
    unsigned char * second_mapping,
    const char ** stage
)
{
    int failed;
    int writer;

    *stage = "write-only descriptor shares mapped cache";
    writer = open (second, O_WRONLY);
    if (writer < 0)
        return 1;
    failed = pwrite (writer, "W", 1, 4079) != 1;
    return close (writer) != 0 || failed || first_mapping[4079] != 'W' || second_mapping[4079] != 'W';
}

static int hardlink_mappings_unlink (
    const char * first,
    const char * second,
    int * first_descriptor,
    int * second_descriptor,
    int * first_owned,
    int * second_owned,
    unsigned char * first_mapping,
    unsigned char * second_mapping,
    const char ** stage
)
{
    struct stat attributes;

    *stage = "unlink aliases with retained mappings";
    if (unlink (first) != 0)
        return 1;
    *first_owned = 0;
    if (fstat (*second_descriptor, &attributes) != 0 || attributes.st_nlink != 1 || first_mapping[0] != 'A' || second_mapping[3] != 'D' ||
        unlink (second) != 0)
        return 1;
    *second_owned = 0;
    if (fstat (*second_descriptor, &attributes) != 0 || attributes.st_nlink != 0)
        return 1;
    if (close (*first_descriptor) != 0)
    {
        *first_descriptor = PROBE_INVALID_DESCRIPTOR;
        return 1;
    }
    *first_descriptor = PROBE_INVALID_DESCRIPTOR;
    if (close (*second_descriptor) != 0)
    {
        *second_descriptor = PROBE_INVALID_DESCRIPTOR;
        return 1;
    }
    *second_descriptor = PROBE_INVALID_DESCRIPTOR;
    return first_mapping[0] != 'A' || second_mapping[3] != 'D';
}

static int run_hardlink_mappings (const char * root)
{
    char first[PATH_MAX], second[PATH_MAX], data[8192];
    unsigned char *first_mapping = MAP_FAILED, *second_mapping = MAP_FAILED;
    struct statvfs before, after;
    int first_descriptor = PROBE_INVALID_DESCRIPTOR, second_descriptor = PROBE_INVALID_DESCRIPTOR, first_owned = 0, second_owned = 0, result = 1;
    const char * stage = "prepare";
    if (snprintf (first, sizeof (first), "%s/hardlink-map-a", root) >= (int)sizeof (first) ||
        snprintf (second, sizeof (second), "%s/hardlink-map-b", root) >= (int)sizeof (second) || statvfs (root, &before) != 0)
        return 1;
    first_descriptor = open (first, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (first_descriptor < 0)
        goto out;
    first_owned = 1;
    memset (data, '0', sizeof (data));
    if (write (first_descriptor, data, sizeof (data)) != sizeof (data) || link (first, second) != 0)
        goto out;
    second_owned = 1;
    second_descriptor = open (second, O_RDWR);
    if (second_descriptor < 0)
        goto out;
    first_mapping = mmap (NULL, sizeof (data), PROT_READ | PROT_WRITE, MAP_SHARED, first_descriptor, 0);
    second_mapping = mmap (NULL, sizeof (data), PROT_READ | PROT_WRITE, MAP_SHARED, second_descriptor, 0);
    if (first_mapping == MAP_FAILED || second_mapping == MAP_FAILED || first_mapping[0] != '0' || second_mapping[0] != '0')
        goto out;

    if (hardlink_mappings_verify_coherence (first_descriptor, second_descriptor, first_mapping, second_mapping, data, sizeof (data), &stage) != 0 ||
        hardlink_mappings_verify_resize (first_descriptor, second_descriptor, first_mapping, second_mapping, data, sizeof (data), &stage) != 0 ||
        hardlink_mappings_verify_write_only (second, first_mapping, second_mapping, &stage) != 0 ||
        hardlink_mappings_unlink (
            first,
            second,
            &first_descriptor,
            &second_descriptor,
            &first_owned,
            &second_owned,
            first_mapping,
            second_mapping,
            &stage
        ) != 0)
        goto out;
    result = 0;
out:
    if (first_mapping != MAP_FAILED && munmap (first_mapping, sizeof (data)) != 0)
        result = 1;
    if (second_mapping != MAP_FAILED && munmap (second_mapping, sizeof (data)) != 0)
        result = 1;
    if (first_descriptor >= 0 && close (first_descriptor) != 0)
        result = 1;
    if (second_descriptor >= 0 && close (second_descriptor) != 0)
        result = 1;
    if (first_owned && unlink (first) != 0)
        result = 1;
    if (second_owned && unlink (second) != 0)
        result = 1;
    if (statvfs (root, &after) != 0 || before.f_ffree != after.f_ffree || before.f_bfree != after.f_bfree)
        result = 1;
    printf ("hardlink-mappings=%s stage=%s\n", result ? "failed" : "valid", stage);
    return result;
}

static int run_rename_cycle (const char * root)
{
    char parent[PATH_MAX], source[PATH_MAX], moved[PATH_MAX], cycle[PATH_MAX];
    int result = 1;
    if (snprintf (parent, sizeof (parent), "%s/cycle-parent", root) >= (int)sizeof (parent) ||
        snprintf (source, sizeof (source), "%s/cycle-child", root) >= (int)sizeof (source) ||
        snprintf (moved, sizeof (moved), "%s/cycle-parent/child", root) >= (int)sizeof (moved) ||
        snprintf (cycle, sizeof (cycle), "%s/cycle-parent/child/cycle", root) >= (int)sizeof (cycle))
        return 1;
    if (mkdir (parent, 0700) != 0)
        return 1;
    if (mkdir (source, 0700) != 0)
    {
        rmdir (parent);
        return 1;
    }
    if (rename (source, moved) == 0)
    {
        errno = 0;
        result = rename (parent, cycle) != -1 || errno != EINVAL;
        if (rmdir (moved) != 0)
            result = 1;
    }
    else
    {
        rmdir (source);
    }
    if (rmdir (parent) != 0)
        result = 1;
    printf ("rename-cycle=%s\n", result ? "failed" : "rejected");
    return result;
}

static int run_identity_churn (const char * root)
{
    char path[PATH_MAX], alias[PATH_MAX], held_path[PATH_MAX];
    int held = PROBE_INVALID_DESCRIPTOR, file_descriptor = PROBE_INVALID_DESCRIPTOR, result = 1;
    unsigned iteration_index;
    char byte;
    path[0] = alias[0] = held_path[0] = '\0';
    if (snprintf (held_path, sizeof (held_path), "%s/churn-held", root) >= (int)sizeof (held_path))
        return 1;
    held = open (held_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (held < 0 || write (held, "H", 1) != 1 || unlink (held_path) != 0)
        goto done;
    for (iteration_index = 0; iteration_index < 256; ++iteration_index)
    {
        struct stat first, second;
        if (snprintf (path, sizeof (path), "%s/churn-%u", root, iteration_index) >= (int)sizeof (path) ||
            snprintf (alias, sizeof (alias), "%s/churn-alias-%u", root, iteration_index) >= (int)sizeof (alias))
            goto done;
        file_descriptor = open (path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (file_descriptor < 0 || write (file_descriptor, "N", 1) != 1 || link (path, alias) != 0 || stat (path, &first) != 0 ||
            stat (alias, &second) != 0 || first.st_ino != second.st_ino || unlink (path) != 0 || unlink (alias) != 0 ||
            pread (file_descriptor, &byte, 1, 0) != 1 || byte != 'N')
            goto done;
        if (close (file_descriptor) != 0)
        {
            file_descriptor = PROBE_INVALID_DESCRIPTOR;
            goto done;
        }
        file_descriptor = PROBE_INVALID_DESCRIPTOR;
        if (pread (held, &byte, 1, 0) != 1 || byte != 'H')
            goto done;
    }
    result = 0;
done:
    if (file_descriptor >= 0 && close (file_descriptor) != 0)
        result = 1;
    if (held >= 0 && close (held) != 0)
        result = 1;
    if (path[0])
        unlink (path);
    if (alias[0])
        unlink (alias);
    if (held_path[0])
        unlink (held_path);
    printf ("identity-churn=%s\n", result ? "failed" : "valid");
    return result;
}

static int verify_recreated_mappings (
    unsigned char * old_map,
    unsigned char * new_map,
    const int old_fd,
    const int new_fd,
    char * data,
    const size_t mapping_size,
    const char ** stage
)
{
    struct stat old_attributes, new_attributes;

    (*stage) = "independent mappings";
    old_map[1] = 'x';
    new_map[2] = 'y';
    if (msync (new_map, mapping_size, MS_SYNC) != 0 || msync (old_map, mapping_size, MS_SYNC) != 0 || pread (old_fd, data, 3, 0) != 3 ||
        memcmp (data, "AxA", 3) != 0 || pread (new_fd, data, 3, 0) != 3 || memcmp (data, "BBy", 3) != 0)
        return 1;
    (*stage) = "independent ownership";
    if (fstat (old_fd, &old_attributes) != 0 || fstat (new_fd, &new_attributes) != 0 || old_attributes.st_ino == new_attributes.st_ino ||
        old_attributes.st_uid != 1001 || new_attributes.st_uid != 1002 || old_attributes.st_nlink != 0 || new_attributes.st_nlink != 1 ||
        (old_attributes.st_mode & 0777) != 0640 || (new_attributes.st_mode & 0777) != 0604)
        return 1;
    return 0;
}

static int run_recreated_identity (const char * root)
{
    char path[PATH_MAX], directory[PATH_MAX], data[4096];
    struct stat old_attributes, new_attributes;
    struct statvfs before, after;
    unsigned char *old_map = MAP_FAILED, *new_map = MAP_FAILED;
    int old_fd = PROBE_INVALID_DESCRIPTOR, new_fd = PROBE_INVALID_DESCRIPTOR, dir_fd = PROBE_INVALID_DESCRIPTOR, result = 1;
    int file_owned = 0, directory_owned = 0, directory_is_file = 0;
    const char * stage = "prepare";
    if (snprintf (path, sizeof (path), "%s/recreated-identity", root) >= (int)sizeof (path) ||
        snprintf (directory, sizeof (directory), "%s/recreated-directory", root) >= (int)sizeof (directory) || statvfs (root, &before) != 0)
        return 1;
    old_fd = open (path, O_CREAT | O_EXCL | O_RDWR, 0640);
    if (old_fd < 0)
        goto out;
    file_owned = 1;
    memset (data, 'A', sizeof (data));
    if (write (old_fd, data, sizeof (data)) != sizeof (data) || chown (path, 1001, 1001) != 0)
        goto out;
    old_map = mmap (NULL, sizeof (data), PROT_READ | PROT_WRITE, MAP_SHARED, old_fd, 0);
    if (old_map == MAP_FAILED || old_map[0] != 'A' || unlink (path) != 0)
        goto out;
    file_owned = 0;
    stage = "replacement";
    new_fd = open (path, O_CREAT | O_EXCL | O_RDWR, 0604);
    if (new_fd < 0)
        goto out;
    file_owned = 1;
    memset (data, 'B', sizeof (data));
    if (write (new_fd, data, sizeof (data)) != sizeof (data) || chown (path, 1002, 1002) != 0)
        goto out;
    new_map = mmap (NULL, sizeof (data), PROT_READ | PROT_WRITE, MAP_SHARED, new_fd, 0);
    if (new_map == MAP_FAILED || new_map[0] != 'B' || old_map[0] != 'A')
        goto out;
    if (verify_recreated_mappings (old_map, new_map, old_fd, new_fd, data, sizeof (data), &stage) != 0)
        goto out;
    if (close (old_fd) != 0)
    {
        old_fd = PROBE_INVALID_DESCRIPTOR;
        goto out;
    }
    old_fd = PROBE_INVALID_DESCRIPTOR;
    if (old_map[1] != 'x' || new_map[2] != 'y')
        goto out;
    stage = "directory replacement";
    if (mkdir (directory, 0700) != 0)
        goto out;
    directory_owned = 1;
    dir_fd = open (directory, O_RDONLY);
    if (dir_fd < 0 || rmdir (directory) != 0)
        goto out;
    directory_owned = 0;
    {
        int replacement = open (directory, O_CREAT | O_EXCL | O_RDWR, 0600);
        int valid;
        if (replacement < 0)
            goto out;
        directory_owned = directory_is_file = 1;
        valid = fstat (dir_fd, &old_attributes) == 0 && fstat (replacement, &new_attributes) == 0 && S_ISDIR (old_attributes.st_mode) &&
                S_ISREG (new_attributes.st_mode) && old_attributes.st_ino != new_attributes.st_ino;
        valid &= close (replacement) == 0;
        if (!valid)
            goto out;
    }
    result = 0;
out:
    if (old_map != MAP_FAILED && munmap (old_map, sizeof (data)) != 0)
        result = 1;
    if (new_map != MAP_FAILED && munmap (new_map, sizeof (data)) != 0)
        result = 1;
    if (old_fd >= 0 && close (old_fd) != 0)
        result = 1;
    if (new_fd >= 0 && close (new_fd) != 0)
        result = 1;
    if (dir_fd >= 0 && close (dir_fd) != 0)
        result = 1;
    if (file_owned && unlink (path) != 0)
        result = 1;
    if (directory_owned && (directory_is_file ? unlink (directory) : rmdir (directory)) != 0)
        result = 1;
    if (statvfs (root, &after) != 0 || before.f_ffree != after.f_ffree || before.f_bfree != after.f_bfree)
        result = 1;
    printf ("recreated-identity=%s stage=%s\n", result ? "failed" : "valid", stage);
    return result;
}

static void run_append_records_child (unsigned actor, size_t record_size, char * data, int files[2], int start[2][2], int done[2])
{
    unsigned round;
    unsigned char command, reply = 1;

    close (start[0][1]);
    close (start[1][1]);
    close (start[1 - actor][0]);
    close (done[0]);
    if (write (done[1], &reply, 1) != 1)
        _exit (2);

    for (round = 0; round < 3 && read (start[actor][0], &command, 1) == 1; ++round)
    {
        memset (data, 'A' + actor * 4 + round, record_size);
        reply = write (files[actor], data, record_size) == (ssize_t)record_size;
        if (write (done[1], &reply, 1) != 1 || !reply)
            _exit (3);
    }
    _exit (0);
}

static int coordinate_append_record_writers (int start[2][2], int done[2])
{
    unsigned actor, round;

    for (actor = 0; actor < 2; ++actor)
    {
        unsigned char ready;
        if (read (done[0], &ready, 1) != 1 || ready != 1)
            return 1;
    }

    for (round = 0; round < 3; ++round)
    {
        unsigned char command = 1, reply;
        if (write (start[0][1], &command, 1) != 1 || write (start[1][1], &command, 1) != 1)
            return 1;
        for (actor = 0; actor < 2; ++actor)
            if (read (done[0], &reply, 1) != 1 || reply != 1)
                return 1;
    }
    return 0;
}

static int verify_append_records (int file_descriptor, char * data, size_t record_size, const volatile unsigned char * mapping, int mapped)
{
    struct stat attributes;
    unsigned round, seen = 0;

    if (fstat (file_descriptor, &attributes) != 0 || attributes.st_size != (off_t)(6 * record_size))
        return 1;
    for (round = 0; round < 6; ++round)
    {
        unsigned bit;
        size_t byte;
        if (pread (file_descriptor, data, record_size, (off_t)(round * record_size)) != (ssize_t)record_size)
            return 1;
        if (data[0] < 'A' || data[0] > 'G' || data[0] == 'D')
            return 1;
        bit = 1u << (data[0] - 'A');
        if (seen & bit)
            return 1;
        seen |= bit;
        for (byte = 1; byte < record_size; ++byte)
            if (data[byte] != data[0])
                return 1;
    }
    if (seen != 0x77)
        return 1;
    if (mapped && (pread (file_descriptor, data, 1, 0) != 1 || mapping[0] != (unsigned char)data[0]))
        return 1;
    return 0;
}

static int start_append_writers (pid_t children[2], const size_t record_size, char * data, int files[2], int start[2][2], int done[2])
{
    unsigned actor;

    for (actor = 0; actor < 2; ++actor)
    {
        children[actor] = fork ();
        if (children[actor] == 0)
            run_append_records_child (actor, record_size, data, files, start, done);
        if (children[actor] < 0)
            return 1;
    }
    close (start[0][0]);
    start[0][0] = PROBE_INVALID_DESCRIPTOR;
    close (start[1][0]);
    start[1][0] = PROBE_INVALID_DESCRIPTOR;
    close (done[1]);
    done[1] = PROBE_INVALID_DESCRIPTOR;
    return 0;
}

static int verify_append_replacement (
    const char * path,
    const char * alias,
    int files[2],
    char * data,
    const size_t record_size,
    int * replacement,
    int * owned,
    int * alias_owned,
    const char ** stage
)
{
    struct stat attributes;

    (*stage) = "append after unlink and replacement";
    if (unlink (path) != 0)
        return 1;
    (*owned) = 0;
    if (unlink (alias) != 0)
        return 1;
    (*alias_owned) = 0;
    (*replacement) = open (path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if ((*replacement) < 0)
        return 1;
    (*owned) = 1;
    if (write ((*replacement), "new", 3) != 3 || write (files[0], "tail", 4) != 4 || fstat (files[0], &attributes) != 0 ||
        attributes.st_size != (off_t)(6 * record_size + 4) || pread (files[0], data, 4, (off_t)(6 * record_size)) != 4 || memcmp (data, "tail", 4))
        return 1;
    (*stage) = "cross-alias truncate and append";
    if (ftruncate (files[1], 5) != 0 || write (files[0], "Z", 1) != 1 || fstat (files[0], &attributes) != 0 || attributes.st_size != 6 ||
        pread (files[0], data, 1, 5) != 1 || data[0] != 'Z' || pread ((*replacement), data, 3, 0) != 3 || memcmp (data, "new", 3))
        return 1;
    return 0;
}

static int run_append_records (const char * root, int mapped)
{
    const size_t record_size = 2 * PROBE_TRANSFER_CHUNK_SIZE + 17;
    volatile unsigned char * mapping = MAP_FAILED;
    char path[PATH_MAX], alias[PATH_MAX];
    char * data = NULL;
    int files[2] = { PROBE_INVALID_DESCRIPTOR, PROBE_INVALID_DESCRIPTOR }, replacement = PROBE_INVALID_DESCRIPTOR;
    int start[2][2] = { { PROBE_INVALID_DESCRIPTOR, PROBE_INVALID_DESCRIPTOR }, { PROBE_INVALID_DESCRIPTOR, PROBE_INVALID_DESCRIPTOR } },
        done[2] = { PROBE_INVALID_DESCRIPTOR, PROBE_INVALID_DESCRIPTOR };
    pid_t children[2] = { -1, -1 };
    int owned = 0, alias_owned = 0, result = 1, status;
    unsigned actor;
    const char * stage = "prepare";
    if (snprintf (path, sizeof (path), "%s/append-records", root) >= (int)sizeof (path) ||
        snprintf (alias, sizeof (alias), "%s/append-alias", root) >= (int)sizeof (alias))
        return 1;
    files[0] = open (path, O_CREAT | O_EXCL | O_RDWR | O_APPEND, 0600);
    if (files[0] < 0)
        goto out;
    owned = 1;
    if (mapped)
    {
        mapping = mmap (NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, files[0], 0);
        if (mapping == MAP_FAILED)
            goto out;
    }
    if (link (path, alias) != 0)
        goto out;
    alias_owned = 1;
    files[1] = open (alias, O_WRONLY | O_APPEND);
    data = malloc (record_size);
    if (files[1] < 0 || data == NULL || pipe (start[0]) != 0 || pipe (start[1]) != 0 || pipe (done) != 0)
        goto out;
    if (start_append_writers (children, record_size, data, files, start, done) != 0)
        goto out;
    stage = "concurrent multi-message append";
    if (coordinate_append_record_writers (start, done) != 0 || verify_append_records (files[0], data, record_size, mapping, mapped) != 0)
        goto out;
    if (verify_append_replacement (path, alias, files, data, record_size, &replacement, &owned, &alias_owned, &stage) != 0)
        goto out;
    result = 0;
out:
    for (actor = 0; actor < 2; ++actor)
    {
        if (start[actor][1] >= 0)
            close (start[actor][1]);
        if (start[actor][0] >= 0)
            close (start[actor][0]);
    }
    for (actor = 0; actor < 2; ++actor)
    {
        if (children[actor] > 0 && (waitpid (children[actor], &status, 0) != children[actor] || !WIFEXITED (status) || WEXITSTATUS (status)))
            result = 1;
        if (files[actor] >= 0 && close (files[actor]) != 0)
            result = 1;
    }
    if (done[0] >= 0)
        close (done[0]);
    if (done[1] >= 0)
        close (done[1]);
    if (mapping != MAP_FAILED && munmap ((void *)mapping, 4096) != 0)
        result = 1;
    if (replacement >= 0 && close (replacement) != 0)
        result = 1;
    if (owned && unlink (path) != 0)
        result = 1;
    if (alias_owned && unlink (alias) != 0)
        result = 1;
    free (data);
    printf ("append-records=%s stage=%s\n", result ? "failed" : "valid", stage);
    return result;
}

static int run_append_full (const char * path)
{
    struct stat attributes;
    char * data = malloc (2 * PROBE_TRANSFER_CHUNK_SIZE);
    int file_descriptor = open (path, O_WRONLY | O_APPEND), result = 1;
    if (data == NULL || file_descriptor < 0)
        goto out;
    memset (data, 'Z', 2 * PROBE_TRANSFER_CHUNK_SIZE);
    if (fstat (file_descriptor, &attributes) != 0 || attributes.st_size < 3 || ftruncate (file_descriptor, attributes.st_size - 3) != 0 ||
        write (file_descriptor, data, 2 * PROBE_TRANSFER_CHUNK_SIZE) != 3 || lseek (file_descriptor, 0, SEEK_CUR) != attributes.st_size)
        goto out;
    errno = 0;
    if (write (file_descriptor, data, 1) != -1 || errno != ENOSPC || lseek (file_descriptor, 0, SEEK_CUR) != attributes.st_size)
        goto out;
    result = 0;
out:
    if (file_descriptor >= 0 && close (file_descriptor) != 0)
        result = 1;
    free (data);
    printf ("append-full=%s\n", result ? "failed" : "valid");
    return result;
}

enum moved_parent_location
{
    MOVED_PARENT_LEFT = 0,
    MOVED_PARENT_RIGHT = 1,
    MOVED_PARENT_DETACHED = 2
};

static int moved_parent_child_state_valid (
    unsigned char command,
    int held_fd,
    ino_t held_inode,
    const struct stat parent_attributes[2],
    ino_t root_inode
)
{
    struct stat attributes;

    if (command == MOVED_PARENT_DETACHED)
        return stat (".", &attributes) == 0 && attributes.st_ino == held_inode;
    if (command > MOVED_PARENT_RIGHT || stat (".", &attributes) != 0 || attributes.st_ino != held_inode || stat ("..", &attributes) != 0 ||
        attributes.st_ino != parent_attributes[command].st_ino)
        return 0;
    if (chdir ("../..") != 0 || stat (".", &attributes) != 0 || attributes.st_ino != root_inode)
        return 0;
    return fchdir (held_fd) == 0;
}

static void serve_moved_parent_checks (
    int commands[2],
    int replies[2],
    int held,
    const struct stat * held_attributes,
    const struct stat parent_attributes[2],
    const struct stat * root_attributes
)
{
    unsigned char command;
    unsigned char reply;

    close (commands[1]);
    close (replies[0]);
    reply = fchdir (held) == 0;
    if (write (replies[1], &reply, 1) != 1 || !reply)
        _exit (2);

    while (read (commands[0], &command, 1) == 1)
    {
        reply = moved_parent_child_state_valid (command, held, held_attributes->st_ino, parent_attributes, root_attributes->st_ino);
        if (write (replies[1], &reply, 1) != 1)
            _exit (3);
    }
    _exit (0);
}

static int request_moved_parent_check (int command_fd, int reply_fd, unsigned char command)
{
    unsigned char reply;

    return write (command_fd, &command, 1) == 1 && read (reply_fd, &reply, 1) == 1 && reply == 1;
}

static int run_moved_parent (const char * root)
{
    char left[PATH_MAX], right[PATH_MAX], old_path[PATH_MAX], new_path[PATH_MAX];
    struct stat root_stat, parents[2], held_stat;
    int root_descriptor = PROBE_INVALID_DESCRIPTOR, held = PROBE_INVALID_DESCRIPTOR,
        parent_descriptors[2] = { PROBE_INVALID_DESCRIPTOR, PROBE_INVALID_DESCRIPTOR };
    int commands[2] = { PROBE_INVALID_DESCRIPTOR, PROBE_INVALID_DESCRIPTOR }, replies[2] = { PROBE_INVALID_DESCRIPTOR, PROBE_INVALID_DESCRIPTOR };
    int left_owned = 0, right_owned = 0, child_owned = 0, side = 0, result = 1, status;
    unsigned rename_round;
    pid_t child = -1;
    if (snprintf (left, sizeof (left), "%s/parent-left", root) >= (int)sizeof (left) ||
        snprintf (right, sizeof (right), "%s/parent-right", root) >= (int)sizeof (right) ||
        snprintf (old_path, sizeof (old_path), "%s/child", left) >= (int)sizeof (old_path) ||
        snprintf (new_path, sizeof (new_path), "%s/child", right) >= (int)sizeof (new_path))
        return 1;
    root_descriptor = open (root, O_RDONLY);
    if (root_descriptor < 0 || fstat (root_descriptor, &root_stat) != 0 || mkdir (left, 0755) != 0)
        goto out;
    left_owned = 1;
    if (mkdir (right, 0755) != 0)
        goto out;
    right_owned = 1;
    if (mkdir (old_path, 0755) != 0)
        goto out;
    child_owned = 1;
    parent_descriptors[0] = open (left, O_RDONLY);
    parent_descriptors[1] = open (right, O_RDONLY);
    held = open (old_path, O_RDONLY);
    if (held < 0 || parent_descriptors[0] < 0 || parent_descriptors[1] < 0 || fstat (held, &held_stat) != 0 ||
        fstat (parent_descriptors[0], &parents[0]) != 0 || fstat (parent_descriptors[1], &parents[1]) != 0 || pipe (commands) != 0 ||
        pipe (replies) != 0)
        goto out;
    child = fork ();
    if (child == 0)
        serve_moved_parent_checks (commands, replies, held, &held_stat, parents, &root_stat);
    if (child < 0)
        goto out;
    close (commands[0]);
    commands[0] = PROBE_INVALID_DESCRIPTOR;
    close (replies[1]);
    replies[1] = PROBE_INVALID_DESCRIPTOR;
    {
        unsigned char ready;
        if (read (replies[0], &ready, 1) != 1 || ready != 1)
            goto out;
    }
    for (rename_round = 0; rename_round < 20; ++rename_round)
    {
        if (rename (side ? new_path : old_path, side ? old_path : new_path) != 0)
            goto out;
        side = !side;
        if (!request_moved_parent_check (commands[1], replies[0], (unsigned char)side))
            goto out;
    }
    if (rmdir (side ? new_path : old_path) != 0)
        goto out;
    child_owned = 0;
    if (!request_moved_parent_check (commands[1], replies[0], MOVED_PARENT_DETACHED))
        goto out;
    result = 0;
out:
    if (commands[1] >= 0)
        close (commands[1]);
    if (child > 0 && (waitpid (child, &status, 0) != child || !WIFEXITED (status) || WEXITSTATUS (status)))
        result = 1;
    if (commands[0] >= 0)
        close (commands[0]);
    if (replies[0] >= 0)
        close (replies[0]);
    if (replies[1] >= 0)
        close (replies[1]);
    if (held >= 0 && close (held) != 0)
        result = 1;
    if (parent_descriptors[0] >= 0 && close (parent_descriptors[0]) != 0)
        result = 1;
    if (parent_descriptors[1] >= 0 && close (parent_descriptors[1]) != 0)
        result = 1;
    if (root_descriptor >= 0 && close (root_descriptor) != 0)
        result = 1;
    if (child_owned && rmdir (side ? new_path : old_path) != 0)
        result = 1;
    if (left_owned && rmdir (left) != 0)
        result = 1;
    if (right_owned && rmdir (right) != 0)
        result = 1;
    printf ("moved-parent=%s\n", result ? "failed" : "valid");
    return result;
}

static int verify_retained_owner_access (const int file_descriptor, const struct timespec times[2], const char ** stage)
{
    pid_t child;
    int status;

    (*stage) = "original owner authorization";
    child = fork ();
    if (child == 0)
    {
        if (setuid (1001) != 0)
            _exit (2);
        _exit (fchmod (file_descriptor, 0640) == 0 && futimens (file_descriptor, times) == 0 ? 0 : 3);
    }
    if (child < 0 || waitpid (child, &status, 0) != child || !WIFEXITED (status) || WEXITSTATUS (status) != 0)
        return 1;
    (*stage) = "replacement owner denied";
    child = fork ();
    if (child == 0)
    {
        if (setuid (1002) != 0)
            _exit (2);
        errno = 0;
        _exit (fchmod (file_descriptor, 0777) == -1 && errno == EPERM ? 0 : 3);
    }
    if (child < 0 || waitpid (child, &status, 0) != child || !WIFEXITED (status) || WEXITSTATUS (status) != 0)
        return 1;
    return 0;
}

static int run_retained_attributes (const char * root)
{
    unsigned replacement;
    for (replacement = 0; replacement < 2; ++replacement)
    {
        char path[PATH_MAX], source[PATH_MAX];
        struct stat old, current;
        struct timespec times[2] = { { 123456, 0 }, { 234567, 0 } };
        int file_descriptor = PROBE_INVALID_DESCRIPTOR, other = PROBE_INVALID_DESCRIPTOR, owned = 0, source_owned = 0, result = 1;
        const char * stage = "prepare";
        if (snprintf (path, sizeof (path), "%s/retained-attrs", root) >= (int)sizeof (path) ||
            snprintf (source, sizeof (source), "%s/retained-source", root) >= (int)sizeof (source))
            return 1;
        file_descriptor = open (path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (file_descriptor < 0)
            goto out;
        owned = 1;
        if (write (file_descriptor, "old", 3) != 3 || fchown (file_descriptor, 1001, 1001) != 0)
            goto out;
        if (!replacement)
        {
            if (unlink (path) != 0)
                goto out;
            owned = 0;
        }
        other = open (replacement ? source : path, O_CREAT | O_EXCL | O_RDWR, 0644);
        if (other < 0)
            goto out;
        if (replacement)
            source_owned = 1;
        else
            owned = 1;
        if (write (other, "new", 3) != 3 || fchown (other, 1002, 1002) != 0)
            goto out;
        if (replacement)
        {
            if (rename (source, path) != 0)
                goto out;
            source_owned = 0;
        }
        if (verify_retained_owner_access (file_descriptor, times, &stage) != 0)
            goto out;
        stage = "independent attributes";
        if (fstat (file_descriptor, &old) != 0 || fstat (other, &current) != 0 || old.st_uid != 1001 || old.st_nlink != 0 ||
            old.st_ino == current.st_ino || (old.st_mode & 0777) != 0640 || old.st_atime != 123456 || old.st_mtime != 234567 ||
            current.st_uid != 1002 || (current.st_mode & 0777) != 0644 || current.st_mtime == old.st_mtime ||
            fchown (file_descriptor, 1003, 1003) != 0 || fstat (file_descriptor, &old) != 0 || fstat (other, &current) != 0 || old.st_uid != 1003 ||
            old.st_gid != 1003 || current.st_uid != 1002)
            goto out;
        result = 0;
out:
        if (file_descriptor >= 0 && close (file_descriptor) != 0)
            result = 1;
        if (other >= 0 && close (other) != 0)
            result = 1;
        if (owned && unlink (path) != 0)
            result = 1;
        if (source_owned && unlink (source) != 0)
            result = 1;
        printf ("retained-attributes=%s replacement=%u stage=%s\n", result ? "failed" : "valid", replacement, stage);
        if (result)
            return result;
    }
    return 0;
}

static int run_stat_held (const char * path, const char * ready_path, const char * command_path)
{
    struct stat attributes;
    char command;
    int file = open (path, O_RDONLY);
    int barrier, result;
    FILE * ready;
    if (file < 0)
        return 1;
    ready = fopen (ready_path, "w");
    if (ready == NULL)
    {
        close (file);
        return 1;
    }
    fputs ("ready\n", ready);
    if (fclose (ready) != 0)
    {
        close (file);
        return 1;
    }
    barrier = open (command_path, O_RDONLY);
    if (barrier < 0)
    {
        close (file);
        return 1;
    }
    result = read (barrier, &command, 1) == 1 ? 0 : 1;
    close (barrier);
    if (result == 0)
    {
        ready = fopen (ready_path, "w");
        if (ready == NULL)
        {
            close (file);
            return 1;
        }

        fputs ("issued\n", ready);
        if (fclose (ready) != 0)
        {
            close (file);
            return 1;
        }

        result = fstat (file, &attributes) != 0 || !S_ISREG (attributes.st_mode);
        printf ("held-stat=%s\n", result == 0 ? "valid" : "failed");
    }
    result |= close (file) != 0;
    return result;
}

static int run_unchanged_owner (const char * path)
{
    struct stat before, after;
    if (stat (path, &before) != 0 || before.st_uid == geteuid ())
        return 1;
    for (unsigned field = 0; field < 3; ++field)
    {
        errno = 0;
        if (chown (path, field == 1 ? (uid_t)-1 : before.st_uid, field == 2 ? (gid_t)-1 : before.st_gid) != -1 || errno != EPERM)
            return 1;
        if (stat (path, &after) != 0 || before.st_uid != after.st_uid || before.st_gid != after.st_gid || before.st_mode != after.st_mode ||
            before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
            return 1;
    }
    return 0;
}

typedef int (*path_probe_handler) (const char * path);

struct path_probe_command
{
    const char * name;          // Command name accepted by the probe.
    path_probe_handler handler; // Handler receiving the validated path.
};

static int run_readiness (const char * path);
static int run_statfs (const char * path);
static int run_stat (const char * path);

static int dispatch_path_probe (const char * name, const char * path, int * result)
{
    static const struct path_probe_command commands[] = { { "unchanged-owner", run_unchanged_owner },
                                                          { "readdir-retry", run_readdir_retry },
                                                          { "readdir-large", run_readdir_large },
                                                          { "mapped-mode-change", run_mapped_mode_change },
                                                          { "hardlink-mappings", run_hardlink_mappings },
                                                          { "retained-attributes", run_retained_attributes },
                                                          { "moved-parent", run_moved_parent },
                                                          { "mmap-grow-large", run_mmap_grow_large },
                                                          { "append-full", run_append_full },
                                                          { "rename-cycle", run_rename_cycle },
                                                          { "recreated-identity", run_recreated_identity },
                                                          { "identity-churn", run_identity_churn },
                                                          { "create-flags", run_create_flags },
                                                          { "retained-handles", run_retained_handles },
                                                          { "dual-open", run_dual_open },
                                                          { "readiness", run_readiness },
                                                          { "statfs", run_statfs },
                                                          { "stat", run_stat } };
    size_t index;

    for (index = 0; index < sizeof (commands) / sizeof (commands[0]); ++index)
    {
        if (strcmp (name, commands[index].name) == 0)
        {
            *result = commands[index].handler (path);
            return 1;
        }
    }
    return 0;
}

static int run_readiness (const char * path)
{
    struct pollfd event;
    int result, saved;

    event.fd = open (path, O_RDONLY | O_NONBLOCK);
    if (event.fd < 0)
        return 1;
    event.events = POLLIN;
    event.revents = 0;
    result = poll (&event, 1, 0);
    saved = errno;
    printf ("poll=%d errno=%d events=%x\n", result, saved, event.revents);
    close (event.fd);
    return result != 0;
}

static int run_statfs (const char * path)
{
    struct statvfs value;
    int result, error;

    memset (&value, 0, sizeof (value));
    errno = 0;
    result = statvfs (path, &value);
    error = errno;
    printf ("result=%d errno=%d\n", result, error);
    if (result == 0)
        printf (
            "blocks=%llu free=%llu available=%llu files=%llu ffree=%llu bsize=%lu\n",
            (unsigned long long)value.f_blocks,
            (unsigned long long)value.f_bfree,
            (unsigned long long)value.f_bavail,
            (unsigned long long)value.f_files,
            (unsigned long long)value.f_ffree,
            (unsigned long)value.f_frsize
        );
    return 0;
}

static int run_stat (const char * path)
{
    struct stat value;
    int result;
    int saved_errno;

    errno = 0;
    result = stat (path, &value);
    saved_errno = errno;
    printf ("result=%d errno=%d\n", result, saved_errno);
    return 0;
}

static int print_usage (void)
{
    fprintf (
        stderr,
        "Usage: io_probe stat PATH|read|write|pread|pwrite PATH COUNT OFFSET"
        "|mmap-read PATH COUNT OFFSET"
        "|mmap-write-shared|mmap-write-private"
        "|mmap-write-shared-readonly|mmap-write-private-readonly"
        " PATH COUNT OFFSET"
        "|mmap-touch-read|mmap-touch-write PATH COUNT OFFSET INDEX"
        "|mmap-unlink|mmap-fork PATH COUNT"
        "|mmap-repeat PATH COUNT ITERATIONS"
        "|mmap-tail PATH FILE_SIZE MAP_SIZE"
        "|mmap-truncate PATH OLD_SIZE NEW_SIZE"
        "|mmap-hold PATH COUNT INDEX READY RELEASE"
        "|mmap-release-read|mmap-release-write PATH COUNT INDEX READY RELEASE"
        "|readdir|readdir-loop PATH COUNT|readdir-large PATH"
        "|dual-open|retained-handles PATH"
        "|stat-held PATH READY COMMAND_FIFO|rename SOURCE DESTINATION"
        "|recreated-identity ROOT"
        "|identity-churn ROOT"
        "|rename-cycle ROOT"
        "|hardlink-mappings ROOT"
        "|retained-attributes ROOT"
        "|moved-parent ROOT"
        "|append-records ROOT|append-full FILE"
        "|signaled-stat|signaled-read|signaled-open|signaled-create PATH"
        "|fsync|fsync-range|close|syncfs|syncfs-try|syncfs-type|fclear|lock PATH\n"
    );
    return 2;
}

int main (int argc, char ** argv)
{
    int result;

    if (argc == 4 && strcmp (argv[1], "mapped-setid-policy") == 0)
        return strcmp (argv[3], "race") == 0 ? run_mapped_setid_race (argv[2]) : run_mapped_setid_policy (argv[2], argv[3]);
    if (argc == 5 && strcmp (argv[1], "held-close") == 0)
    {
        return run_held_close (argv[2], argv[3], argv[4]);
    }

    if (argc == 4 && strcmp (argv[1], "mmap-sync") == 0)
    {
        return run_mmap_sync (argv[2], argv[3]);
    }

    if (argc == 3 && dispatch_path_probe (argv[1], argv[2], &result))
        return result;
    if (argc == 3 && strcmp (argv[1], "append-records") == 0)
        return run_append_records (argv[2], 0);
    if (argc == 3 && strcmp (argv[1], "mapped-append-records") == 0)
        return run_append_records (argv[2], 1);
    if (argc == 5 && strcmp (argv[1], "stat-held") == 0)
        return run_stat_held (argv[2], argv[3], argv[4]);
    if (argc == 4 && strcmp (argv[1], "rename") == 0)
        return rename (argv[2], argv[3]) == 0 ? 0 : 1;
    if (argc == 3 && (strcmp (argv[1], "readonly-root") == 0 || strcmp (argv[1], "readonly-all") == 0))
        return run_readonly (argv[2], strcmp (argv[1], "readonly-all") == 0);
    if (argc == 3 && (strcmp (argv[1], "signaled-stat") == 0 || strcmp (argv[1], "signaled-read") == 0 || strcmp (argv[1], "signaled-open") == 0 ||
                      strcmp (argv[1], "signaled-create") == 0))
        return run_signaled_request (argv[1], argv[2]);
    if (argc == 6 && (strcmp (argv[1], "mmap-touch-read") == 0 || strcmp (argv[1], "mmap-touch-write") == 0))
        return run_mmap_touch (argv[1], argv[2], argv[3], argv[4], argv[5]);
    if (argc == 4 && strcmp (argv[1], "mmap-unlink") == 0)
        return run_mmap_unlink (argv[2], argv[3]);
    if (argc == 4 && strcmp (argv[1], "mmap-fork") == 0)
        return run_mmap_fork (argv[2], argv[3]);
    if (argc == 5 && strcmp (argv[1], "mmap-repeat") == 0)
        return run_mmap_repeat (argv[2], argv[3], argv[4]);
    if (argc == 5 && strcmp (argv[1], "mmap-tail") == 0)
        return run_mmap_tail (argv[2], argv[3], argv[4]);
    if (argc == 5 && strcmp (argv[1], "mmap-truncate") == 0)
        return run_mmap_truncate (argv[2], argv[3], argv[4]);
    if (argc == 7 && strcmp (argv[1], "mmap-hold") == 0)
        return run_mmap_hold (argv[2], argv[3], argv[4], argv[5], argv[6], 0);
    if (argc == 7 && (strcmp (argv[1], "mmap-release-read") == 0 || strcmp (argv[1], "mmap-release-write") == 0))
        return run_mmap_hold (argv[2], argv[3], argv[4], argv[5], argv[6], strcmp (argv[1], "mmap-release-write") == 0 ? 2 : 1);
    if (argc == 5 && strcmp (argv[1], "mmap-read") == 0)
        return run_mmap_read (argv[2], argv[3], argv[4]);
    if (argc == 5 && (strcmp (argv[1], "mmap-write-shared") == 0 || strcmp (argv[1], "mmap-write-private") == 0 ||
                      strcmp (argv[1], "mmap-write-shared-readonly") == 0 || strcmp (argv[1], "mmap-write-private-readonly") == 0))
        return run_mmap_write (argv[1], argv[2], argv[3], argv[4]);
    if (argc == 5 &&
        (strcmp (argv[1], "read") == 0 || strcmp (argv[1], "write") == 0 || strcmp (argv[1], "pread") == 0 || strcmp (argv[1], "pwrite") == 0))
        return run_io (argv[1], argv[2], argv[3], argv[4]);
    if (argc == 4 && strcmp (argv[1], "readdir") == 0)
        return run_readdir (argv[2], argv[3]);
    if (argc == 4 && strcmp (argv[1], "readdir-loop") == 0)
        return run_readdir_loop (argv[2], argv[3]);
    if (argc == 3 && (strcmp (argv[1], "fsync") == 0 || strcmp (argv[1], "fsync-range") == 0 || strcmp (argv[1], "close") == 0 ||
                      strcmp (argv[1], "syncfs") == 0 || strcmp (argv[1], "syncfs-try") == 0 || strcmp (argv[1], "syncfs-type") == 0 ||
                      strcmp (argv[1], "fclear") == 0 || strcmp (argv[1], "lock") == 0))
        return run_vnode_operation (argv[1], argv[2]);

    return print_usage ();
}

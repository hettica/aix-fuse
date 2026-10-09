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
 * Modified for AIX USFS, 2026-07-26 through 2026-10-08: reduced definitions,
 * AIX configuration, inline flag helpers, and USFS-specific documentation.
 * This adapted header, including its inline helpers, is licensed as a whole
 * under LGPL-2.1-only.
 *
 * fuse_common.h - common definitions of the AIX USFS client library.
 *
 * API-compatible subset of libfuse 3.19's fuse_common.h: the structures and
 * declarations that high-level filesystems (like the examples in this repository) consume.
 * The layouts follow the upstream member lists so designated initializers and
 * field accesses compile unchanged; no binary compatibility with the Linux
 * libfuse is implied.
 */

#ifndef FUSE_COMMON_H_
#define FUSE_COMMON_H_

#include "libfuse_config.h"
#include "fuse_opt.h"
#include "fuse_log.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define FUSE_MAKE_VERSION(maj, min) ((maj)*100 + (min))
#define FUSE_VERSION                FUSE_MAKE_VERSION (FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION)

#if !defined(FUSE_USE_VERSION) || FUSE_USE_VERSION < 30
    #error only API version 30 or greater is supported
#endif

/* The whole API assumes a 64-bit off_t; on AIX build with -maix64. */
typedef char _fuse_off_t_must_be_64bit[sizeof (off_t) == 8 ? 1 : -1];

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Information about an open file, shared between all high-level operations.
 * Member list follows libfuse 3.19.
 */
struct fuse_file_info
{
    int32_t flags;

    uint32_t writepage : 1;
    uint32_t direct_io : 1;
    uint32_t keep_cache : 1;
    uint32_t flush : 1;
    uint32_t nonseekable : 1;
    uint32_t flock_release : 1;
    uint32_t cache_readdir : 1;
    uint32_t noflush : 1;
    uint32_t parallel_direct_writes : 1;
    uint32_t padding : 23;
    uint32_t padding2 : 32;
    uint32_t padding3 : 32;

    uint64_t fh;
    uint64_t lock_owner;
    uint32_t poll_events;
    int32_t backing_id;
    uint64_t compat_flags;
    uint64_t reserved[2];
};

/* Capability flags (subset; values follow libfuse 3.19). Only advisory in
 * this implementation. */
#define FUSE_CAP_ASYNC_READ          (1UL << 0)
#define FUSE_CAP_POSIX_LOCKS         (1UL << 1)
#define FUSE_CAP_ATOMIC_O_TRUNC      (1UL << 3)
#define FUSE_CAP_EXPORT_SUPPORT      (1UL << 4)
#define FUSE_CAP_DONT_MASK           (1UL << 6)
#define FUSE_CAP_SPLICE_WRITE        (1UL << 7)
#define FUSE_CAP_SPLICE_MOVE         (1UL << 8)
#define FUSE_CAP_SPLICE_READ         (1UL << 9)
#define FUSE_CAP_FLOCK_LOCKS         (1UL << 10)
#define FUSE_CAP_IOCTL_DIR           (1UL << 11)
#define FUSE_CAP_AUTO_INVAL_DATA     (1UL << 12)
#define FUSE_CAP_READDIRPLUS         (1UL << 13)
#define FUSE_CAP_READDIRPLUS_AUTO    (1UL << 14)
#define FUSE_CAP_ASYNC_DIO           (1UL << 15)
#define FUSE_CAP_WRITEBACK_CACHE     (1UL << 16)
#define FUSE_CAP_NO_OPEN_SUPPORT     (1UL << 17)
#define FUSE_CAP_PARALLEL_DIROPS     (1UL << 18)
#define FUSE_CAP_POSIX_ACL           (1UL << 19)
#define FUSE_CAP_HANDLE_KILLPRIV     (1UL << 20)
#define FUSE_CAP_CACHE_SYMLINKS      (1UL << 23)
#define FUSE_CAP_NO_OPENDIR_SUPPORT  (1UL << 24)
#define FUSE_CAP_EXPLICIT_INVAL_DATA (1UL << 25)

/**
 * Connection information. Member list follows libfuse 3.19.
 */
struct fuse_conn_info
{
    uint32_t proto_major;
    uint32_t proto_minor;
    uint32_t max_write;
    uint32_t max_read;
    uint32_t max_readahead;
    uint32_t capable;
    uint32_t want;
    uint32_t max_background;
    uint32_t congestion_threshold;
    uint32_t time_gran;
    uint32_t max_backing_stack_depth;

    uint32_t no_interrupt : 1;
    uint32_t io_uring_single_issuer : 1;
    uint32_t padding : 30;

    uint64_t capable_ext;
    uint64_t want_ext;
    uint16_t request_timeout;
    uint16_t reserved[31];
};

#define FUSE_CONN_FLAG_SINGLE_ISSUER (1u << 0)
#define FUSE_CONN_FLAG_NO_INTERRUPT  (1u << 1)

/* Feature/connection flag helpers. This implementation records the requests
 * but attaches no behavior to them yet. */
static inline bool fuse_set_feature_flag (struct fuse_conn_info * conn, uint64_t flag)
{
    if (conn->capable_ext & flag)
    {
        conn->want_ext |= flag;
        return true;
    }
    return false;
}

static inline void fuse_unset_feature_flag (struct fuse_conn_info * conn, uint64_t flag)
{
    conn->want_ext &= ~flag;
}

static inline bool fuse_get_feature_flag (const struct fuse_conn_info * conn, uint64_t flag)
{
    return (conn->want_ext & flag) != 0;
}

static inline bool fuse_set_conn_flag (struct fuse_conn_info * conn, uint64_t flag)
{
    if (flag == FUSE_CONN_FLAG_SINGLE_ISSUER)
        conn->io_uring_single_issuer = 1;
    else if (flag == FUSE_CONN_FLAG_NO_INTERRUPT)
        conn->no_interrupt = 1;
    return true;
}

struct fuse_session;
struct fuse_pollhandle;
struct fuse_conn_info_opts;

struct libfuse_version
{
    uint32_t major;
    uint32_t minor;
    uint32_t hotfix;
    uint32_t padding;
};

int fuse_set_signal_handlers (struct fuse_session * se);

void fuse_remove_signal_handlers (const struct fuse_session * session);

int fuse_daemonize (int foreground);

int fuse_version (void);

const char * fuse_pkgversion (void);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_COMMON_H_ */

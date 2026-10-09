/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2019  Red Hat, Inc.
 * Copyright (c) 2026 Raman Dzehtsiar (USFS modifications)
 *
 * This program can be distributed under the terms of the GNU LGPLv2.
 * See LICENSE.LGPL-2.1-only in this distribution (upstream LGPL2.txt).
 * Upstream's LICENSE and LGPL2.txt specify LGPL version 2.1.
 *
 * Modified for AIX USFS, 2026-07-26 through 2026-10-08: reduced logging API
 * and documentation of the USFS handler and errno behavior.
 *
 * fuse_log.h - logging API of the AIX USFS client library.
 *
 * API-compatible subset of libfuse 3.19's fuse_log.h.
 */

#ifndef FUSE_LOG_H_
#define FUSE_LOG_H_

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

enum fuse_log_level
{
    FUSE_LOG_EMERG,
    FUSE_LOG_ALERT,
    FUSE_LOG_CRIT,
    FUSE_LOG_ERR,
    FUSE_LOG_WARNING,
    FUSE_LOG_NOTICE,
    FUSE_LOG_INFO,
    FUSE_LOG_DEBUG
};

/* Handlers must be thread-safe and may be called before a filesystem is created. */
typedef void (*fuse_log_func_t) (enum fuse_log_level level, const char * fmt, va_list ap);

/* Replace the process-wide handler. NULL restores the default stderr handler.
 * An in-flight call may still use the previous handler after this returns. */
void fuse_set_log_func (fuse_log_func_t func);

/* Forward the level and arguments to the current handler, preserving errno. */
void fuse_log (enum fuse_log_level level, const char * fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_LOG_H_ */

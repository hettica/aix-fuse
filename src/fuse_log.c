/*
  SPDX-License-Identifier: LGPL-2.1-only
  FUSE: Filesystem in Userspace
  Copyright (C) 2019  Red Hat, Inc.

  Copyright (c) 2026 Raman Dzehtsiar (AIX USFS modifications)
  Modified for AIX USFS in 2026: removed syslog integration, added
  thread-safe handler replacement, and preserved errno across logging.

  Logging API.

  This program can be distributed under the terms of the GNU LGPLv2.
 */

#include "fuse_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>

static void default_log_handler (enum fuse_log_level level, const char * format, va_list arguments)
{
    (void)level;
    vfprintf (stderr, format, arguments);
}

static pthread_mutex_t log_handler_mutex = PTHREAD_MUTEX_INITIALIZER;
static fuse_log_func_t current_log_handler = default_log_handler;

void fuse_set_log_func (fuse_log_func_t log_function)
{
    pthread_mutex_lock (&log_handler_mutex);
    current_log_handler = log_function != NULL ? log_function : default_log_handler;
    pthread_mutex_unlock (&log_handler_mutex);
}

void fuse_log (enum fuse_log_level level, const char * format, ...)
{
    const int saved_error = errno;

    pthread_mutex_lock (&log_handler_mutex);
    fuse_log_func_t log_function = current_log_handler;
    pthread_mutex_unlock (&log_handler_mutex);

    /* Call application code outside the lock so handlers can log or replace themselves. */
    va_list arguments;

    va_start (arguments, format);
    log_function (level, format, arguments);
    va_end (arguments);

    errno = saved_error;
}

// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "tap.h"
#define FUSE_USE_VERSION 31
#include "fuse.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum logging_test_limits
{
    LOG_MESSAGE_CAPACITY = 1024,
    LOG_THREAD_COUNT = 4,
    LOG_MESSAGES_PER_THREAD = 200,
    LOG_TEST_ASSERTIONS = 13
};

struct stream_capture
{
    FILE * stream;        // Borrowed standard stream being redirected.
    FILE * temporary;     // Owned temporary file containing captured output.
    int saved_descriptor; // Owned duplicate used to restore the standard stream.
};

static unsigned captured_messages;
static enum fuse_log_level captured_level;
static char captured_message[LOG_MESSAGE_CAPACITY];
static pthread_mutex_t capture_mutex = PTHREAD_MUTEX_INITIALIZER;

static void capture_log (enum fuse_log_level level, const char * format, va_list arguments)
{
    pthread_mutex_lock (&capture_mutex);
    ++captured_messages;
    captured_level = level;
    vsnprintf (captured_message, sizeof (captured_message), format, arguments);
    pthread_mutex_unlock (&capture_mutex);

    errno = ERANGE;
}

static void discard_log (enum fuse_log_level level, const char * format, va_list arguments)
{
    (void)level;
    (void)format;
    (void)arguments;
}

static struct stream_capture capture_stream (FILE * stream)
{
    struct stream_capture capture = { 0 };

    capture.stream = stream;
    capture.temporary = tmpfile ();
    capture.saved_descriptor = dup (fileno (stream));

    if (capture.temporary == NULL || capture.saved_descriptor < 0)
        abort ();

    if (fflush (stream) != 0 || dup2 (fileno (capture.temporary), fileno (stream)) < 0)
        abort ();

    return capture;
}

static void finish_capture (struct stream_capture * capture, char * output, const size_t capacity)
{
    if (fflush (capture->stream) != 0)
        abort ();

    if (dup2 (capture->saved_descriptor, fileno (capture->stream)) < 0)
        abort ();

    close (capture->saved_descriptor);
    rewind (capture->temporary);

    const size_t length = fread (output, 1, capacity - 1, capture->temporary);

    output[length] = '\0';

    if (ferror (capture->temporary))
        abort ();

    fclose (capture->temporary);
}

static void test_default_handler (struct tap_state * tap)
{
    char output[LOG_MESSAGE_CAPACITY];
    struct stream_capture capture = capture_stream (stderr);

    fuse_log (FUSE_LOG_WARNING, "default %s %d\n", "message", 7);
    finish_capture (&capture, output, sizeof (output));
    tap_ok (tap, strcmp (output, "default message 7\n") == 0, "initial handler formats messages to stderr");

    fuse_set_log_func (capture_log);
    fuse_set_log_func (NULL);
    capture = capture_stream (stderr);
    fuse_log (FUSE_LOG_ERR, "restored\n");
    finish_capture (&capture, output, sizeof (output));
    tap_ok (tap, strcmp (output, "restored\n") == 0, "NULL restores the stderr handler");
}

static void test_custom_handler (struct tap_state * tap)
{
    char output[LOG_MESSAGE_CAPACITY];
    struct stream_capture capture = capture_stream (stderr);

    captured_messages = 0;
    fuse_set_log_func (capture_log);
    errno = EACCES;
    fuse_log (FUSE_LOG_DEBUG, "%s %d %.1f\n", "custom", 7, 2.5);
    const int saved_error = errno;

    finish_capture (&capture, output, sizeof (output));
    tap_ok (
        tap,
        captured_messages == 1 && captured_level == FUSE_LOG_DEBUG && strcmp (captured_message, "custom 7 2.5\n") == 0,
        "custom handler receives severity and variadic arguments"
    );
    tap_ok (tap, saved_error == EACCES && output[0] == '\0', "custom handler preserves errno and replaces stderr output");

    fuse_set_log_func (discard_log);
    capture = capture_stream (stderr);
    fuse_log (FUSE_LOG_ERR, "discarded\n");
    finish_capture (&capture, output, sizeof (output));
    tap_ok (tap, output[0] == '\0' && captured_messages == 1, "a replacement handler can suppress diagnostics");
}

static void replace_and_log (enum fuse_log_level level, const char * format, va_list arguments)
{
    (void)format;
    (void)arguments;
    fuse_set_log_func (capture_log);
    fuse_log (level, "nested message\n");
}

static void test_reentrant_handler (struct tap_state * tap)
{
    captured_messages = 0;
    fuse_set_log_func (replace_and_log);
    fuse_log (FUSE_LOG_NOTICE, "outer message\n");
    tap_ok (
        tap,
        captured_messages == 1 && captured_level == FUSE_LOG_NOTICE && strcmp (captured_message, "nested message\n") == 0,
        "handler can replace itself and log without deadlock"
    );
}

static void * log_messages (void * unused)
{
    (void)unused;

    for (unsigned message_index = 0; message_index < LOG_MESSAGES_PER_THREAD; ++message_index)
    {
        fuse_set_log_func (capture_log);
        fuse_log (FUSE_LOG_INFO, "concurrent %u\n", message_index);
    }

    return NULL;
}

static void test_concurrent_handlers (struct tap_state * tap)
{
    pthread_t threads[LOG_THREAD_COUNT];

    captured_messages = 0;

    for (unsigned thread_index = 0; thread_index < LOG_THREAD_COUNT; ++thread_index)
    {
        if (pthread_create (&threads[thread_index], NULL, log_messages, NULL) != 0)
            abort ();
    }

    for (unsigned thread_index = 0; thread_index < LOG_THREAD_COUNT; ++thread_index)
    {
        if (pthread_join (threads[thread_index], NULL) != 0)
            abort ();
    }

    tap_ok (
        tap,
        captured_messages == LOG_THREAD_COUNT * LOG_MESSAGES_PER_THREAD,
        "concurrent handler registration and logging deliver every message"
    );
}

static void test_option_diagnostics (struct tap_state * tap)
{
    char output[LOG_MESSAGE_CAPACITY];
    char * numeric_argv[] = { (char *)"program", (char *)"--number=invalid" };
    char * missing_argv[] = { (char *)"program", (char *)"-o" };
    struct fuse_args numeric_args = FUSE_ARGS_INIT (2, numeric_argv);
    struct fuse_args missing_args = FUSE_ARGS_INIT (2, missing_argv);
    const struct fuse_opt options[] = { { "--number=%d", 0, 0 }, FUSE_OPT_END };
    int number = 0;

    captured_messages = 0;
    fuse_set_log_func (capture_log);
    struct stream_capture capture = capture_stream (stderr);
    const int numeric_result = fuse_opt_parse (&numeric_args, &number, options, NULL);
    const int numeric_logged = captured_messages == 1 && captured_level == FUSE_LOG_ERR && strstr (captured_message, "invalid numeric value") != NULL;
    const int missing_result = fuse_opt_parse (&missing_args, NULL, NULL, NULL);
    const int missing_logged = captured_messages == 2 && captured_level == FUSE_LOG_ERR && strstr (captured_message, "missing option list") != NULL;

    finish_capture (&capture, output, sizeof (output));
    fuse_opt_free_args (&numeric_args);
    fuse_opt_free_args (&missing_args);
    tap_ok (tap, numeric_result == -1 && numeric_logged && output[0] == '\0', "numeric option errors use the custom handler");
    tap_ok (tap, missing_result == -1 && missing_logged && output[0] == '\0', "missing option-list errors use the custom handler");
}

static void test_client_diagnostics (struct tap_state * tap)
{
    char output[LOG_MESSAGE_CAPACITY];
    char * unsupported_argv[] = { (char *)"program", (char *)"--unsupported" };
    char * missing_argv[] = { (char *)"program" };
    struct fuse_args args = FUSE_ARGS_INIT (2, unsupported_argv);
    struct fuse_cmdline_opts options = { 0 };
    struct fuse_operations operations = { 0 };

    captured_messages = 0;
    fuse_set_log_func (capture_log);
    struct stream_capture capture = capture_stream (stderr);
    const int parse_result = fuse_parse_cmdline (&args, &options);
    const int parse_logged = captured_messages == 1 && captured_level == FUSE_LOG_ERR && strstr (captured_message, "unsupported argument") != NULL;
    const int main_result = fuse_main (1, missing_argv, &operations, NULL);
    const int main_logged = captured_messages == 2 && captured_level == FUSE_LOG_ERR && strstr (captured_message, "mountpoint") != NULL;

    finish_capture (&capture, output, sizeof (output));
    free (options.mountpoint);
    tap_ok (tap, parse_result == -1 && parse_logged && output[0] == '\0', "command-line errors use the custom handler");
    tap_ok (tap, main_result != 0 && main_logged && output[0] == '\0', "fuse_main errors use the custom handler");
}

static void test_explicit_output (struct tap_state * tap)
{
    char output[LOG_MESSAGE_CAPACITY];
    char * help_argv[] = { (char *)"program", (char *)"--help" };
    char * version_argv[] = { (char *)"program", (char *)"--version" };
    struct fuse_operations operations = { 0 };

    captured_messages = 0;
    fuse_set_log_func (capture_log);
    struct stream_capture capture = capture_stream (stdout);
    const int help_result = fuse_main (2, help_argv, &operations, NULL);

    finish_capture (&capture, output, sizeof (output));
    tap_ok (
        tap,
        help_result == 0 && captured_messages == 0 && strstr (output, "usage: program") != NULL,
        "explicit help remains on stdout independently of the log handler"
    );

    capture = capture_stream (stdout);
    const int version_result = fuse_main (2, version_argv, &operations, NULL);

    finish_capture (&capture, output, sizeof (output));
    tap_ok (
        tap,
        version_result == 0 && captured_messages == 0 && strstr (output, fuse_pkgversion ()) != NULL,
        "explicit version remains on stdout independently of the log handler"
    );
}

int main (void)
{
    struct tap_state tap;

    tap_plan (&tap, LOG_TEST_ASSERTIONS);
    test_default_handler (&tap);
    test_custom_handler (&tap);
    test_reentrant_handler (&tap);
    test_concurrent_handlers (&tap);
    test_option_diagnostics (&tap);
    test_client_diagnostics (&tap);
    test_explicit_output (&tap);
    fuse_set_log_func (NULL);

    return tap_finish (&tap);
}

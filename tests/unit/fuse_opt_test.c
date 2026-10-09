// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "tap.h"
#include "fuse_opt.h"

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

struct options
{
    int verbose;
    int count;
    char * name;
};

static int keep_nonoptions (void * data, const char * arg, int key, struct fuse_args * outargs)
{
    (void)data;
    (void)arg;
    (void)outargs;
    return key == FUSE_OPT_KEY_NONOPT || key == FUSE_OPT_KEY_OPT;
}

static int reject_options (void * data, const char * arg, int key, struct fuse_args * outargs)
{
    (void)data;
    (void)arg;
    (void)outargs;
    return key == FUSE_OPT_KEY_OPT ? -1 : 1;
}

static int discard_arguments (void * data, const char * arg, int key, struct fuse_args * outargs)
{
    (void)data;
    (void)arg;
    (void)key;
    (void)outargs;
    return 0;
}

static int numeric_case (const char * format, const char * value, void * data, size_t offset, int expected)
{
    char templ[64], argument[256];
    char * argv[] = { (char *)"prog", argument };
    struct fuse_args args = FUSE_ARGS_INIT (2, argv);
    struct fuse_opt spec[] = { { templ, offset, 0 }, FUSE_OPT_END };
    int valid, result;
    snprintf (templ, sizeof (templ), "--number=%s", format);
    snprintf (argument, sizeof (argument), "--number=%s", value);
    result = fuse_opt_parse (&args, data, spec, NULL);
    valid = result == expected;
    if (expected == -1)
        valid &= args.argv == argv && args.argc == 2 && !args.allocated;
    else
        valid &= args.argc == 1 && strcmp (args.argv[0], "prog") == 0;
    fuse_opt_free_args (&args);
    return valid;
}

/* Exact typed fields, guarded on both sides, catch partial wide stores and
   writes past narrow destinations on the real target ABI. */
#define NUMERIC_TYPE_TEST(name, type, format, minimum, maximum, signed_type)                                                                         \
    static int name (void)                                                                                                                           \
    {                                                                                                                                                \
        struct guarded                                                                                                                               \
        {                                                                                                                                            \
            unsigned char before[16];                                                                                                                \
            type field;                                                                                                                              \
            unsigned char after[16];                                                                                                                 \
        };                                                                                                                                           \
        struct guarded actual, expected;                                                                                                             \
        char value[80];                                                                                                                              \
        int valid = 1;                                                                                                                               \
        memset (&actual, 0xa5, sizeof (actual));                                                                                                     \
        memcpy (&expected, &actual, sizeof (actual));                                                                                                \
        expected.field = (type)1;                                                                                                                    \
        valid &= numeric_case (format, "+1", &actual, offsetof (struct guarded, field), 0) && memcmp (&actual, &expected, sizeof (actual)) == 0;     \
        expected.field = (type)0;                                                                                                                    \
        valid &= numeric_case (format, "0", &actual, offsetof (struct guarded, field), 0) && memcmp (&actual, &expected, sizeof (actual)) == 0;      \
        snprintf (value, sizeof (value), "%ju", (uintmax_t)(maximum));                                                                               \
        expected.field = (type)(maximum);                                                                                                            \
        valid &= numeric_case (format, value, &actual, offsetof (struct guarded, field), 0) && memcmp (&actual, &expected, sizeof (actual)) == 0;    \
        if (signed_type)                                                                                                                             \
            snprintf (value, sizeof (value), "%jd", (intmax_t)(minimum));                                                                            \
        else                                                                                                                                         \
            strcpy (value, "0");                                                                                                                     \
        expected.field = (type)(minimum);                                                                                                            \
        valid &= numeric_case (format, value, &actual, offsetof (struct guarded, field), 0) && memcmp (&actual, &expected, sizeof (actual)) == 0;    \
        if ((uintmax_t)(maximum) < UINTMAX_MAX)                                                                                                      \
            snprintf (value, sizeof (value), "%ju", (uintmax_t)(maximum) + 1);                                                                       \
        else                                                                                                                                         \
            strcpy (value, "18446744073709551616");                                                                                                  \
        valid &= numeric_case (format, value, &actual, offsetof (struct guarded, field), -1) && memcmp (&actual, &expected, sizeof (actual)) == 0;   \
        if (signed_type)                                                                                                                             \
            snprintf (value, sizeof (value), "-%ju", (uintmax_t)(-((intmax_t)(minimum) + 1)) + 2);                                                   \
        else                                                                                                                                         \
            strcpy (value, "-1");                                                                                                                    \
        valid &= numeric_case (format, value, &actual, offsetof (struct guarded, field), -1) && memcmp (&actual, &expected, sizeof (actual)) == 0;   \
        return valid;                                                                                                                                \
    }

NUMERIC_TYPE_TEST (numeric_int, int, "%d", INT_MIN, INT_MAX, 1)
NUMERIC_TYPE_TEST (numeric_uint, unsigned int, "%u", 0, UINT_MAX, 0)
NUMERIC_TYPE_TEST (numeric_char, signed char, "%hhd", SCHAR_MIN, SCHAR_MAX, 1)
NUMERIC_TYPE_TEST (numeric_uchar, unsigned char, "%hhu", 0, UCHAR_MAX, 0)
NUMERIC_TYPE_TEST (numeric_short, short, "%hd", SHRT_MIN, SHRT_MAX, 1)
NUMERIC_TYPE_TEST (numeric_ushort, unsigned short, "%hu", 0, USHRT_MAX, 0)
NUMERIC_TYPE_TEST (numeric_long, long, "%ld", LONG_MIN, LONG_MAX, 1)
NUMERIC_TYPE_TEST (numeric_ulong, unsigned long, "%lu", 0, ULONG_MAX, 0)
NUMERIC_TYPE_TEST (numeric_llong, long long, "%lld", LLONG_MIN, LLONG_MAX, 1)
NUMERIC_TYPE_TEST (numeric_ullong, unsigned long long, "%llu", 0, ULLONG_MAX, 0)
NUMERIC_TYPE_TEST (numeric_max, intmax_t, "%jd", INTMAX_MIN, INTMAX_MAX, 1)
NUMERIC_TYPE_TEST (numeric_umax, uintmax_t, "%ju", 0, UINTMAX_MAX, 0)
NUMERIC_TYPE_TEST (numeric_size, ssize_t, "%zd", -SSIZE_MAX - 1, SSIZE_MAX, 1)
NUMERIC_TYPE_TEST (numeric_usize, size_t, "%zu", 0, SIZE_MAX, 0)
#undef NUMERIC_TYPE_TEST

static void test_numeric_options (struct tap_state * tap)
{
    const char * invalid_formats[] = { "%", "%n", "%f", "%3d", "%*d", "%td", "%Ld", "%hld", "%d%u", "%d-tail", "%s%d" };
    const char * invalid_values[] = { "", "+", "-", "abc", "1x", " 1", "1 ", "1\t2", "1\n", "0x10", "999999999999999999999999999999999999" };
    int field = 73, valid = 1;
    unsigned int unsigned_field = 0;
    unsigned long wide = 0;
    size_t i;
    tap_ok (tap, numeric_int () && numeric_uint (), "int fields honor values, limits and canaries");
    tap_ok (tap, numeric_char () && numeric_uchar (), "char fields honor values, limits and canaries");
    tap_ok (tap, numeric_short () && numeric_ushort (), "short fields honor values, limits and canaries");
    tap_ok (tap, numeric_long () && numeric_ulong (), "long fields honor values, limits and canaries");
    tap_ok (tap, numeric_llong () && numeric_ullong (), "long long fields honor values, limits and canaries");
    tap_ok (tap, numeric_max () && numeric_umax (), "intmax fields honor values, limits and canaries");
    tap_ok (tap, numeric_size () && numeric_usize (), "size fields honor values, limits and canaries");
    tap_ok (
        tap,
        numeric_case ("%lu", "4294967297", &wide, 0, 0) && wide == UINT64_C (4294967297),
        "64-bit option preserves both halves above UINT32_MAX"
    );
    valid &= numeric_case ("%d", "010", &field, 0, 0) && field == 10;
    valid &= numeric_case ("%i", "010", &field, 0, 0) && field == 8;
    valid &= numeric_case ("%i", "-0x10", &field, 0, 0) && field == -16;
    valid &= numeric_case ("%u", "010", &unsigned_field, 0, 0) && unsigned_field == 10;
    valid &= numeric_case ("%o", "755", &unsigned_field, 0, 0) && unsigned_field == 0755;
    valid &= numeric_case ("%x", "ff", &unsigned_field, 0, 0) && unsigned_field == 255;
    valid &= numeric_case ("%X", "+0XFF", &unsigned_field, 0, 0) && unsigned_field == 255;
    tap_ok (tap, valid, "numeric conversions honor decimal, automatic, octal and hexadecimal bases");
    valid = 1;
    field = 73;
    for (i = 0; i < sizeof (invalid_formats) / sizeof (invalid_formats[0]); i++)
        valid &= numeric_case (invalid_formats[i], "1", &field, 0, -1) && field == 73;
    tap_ok (tap, valid, "unsupported field formats fail without modifying fields or argv");
    valid = 1;
    for (i = 0; i < sizeof (invalid_values) / sizeof (invalid_values[0]); i++)
        valid &= numeric_case ("%d", invalid_values[i], &field, 0, -1) && field == 73;
    valid &= numeric_case ("%i", "08", &field, 0, -1) && field == 73;
    unsigned_field = 73;
    valid &= numeric_case ("%o", "8", &unsigned_field, 0, -1) && unsigned_field == 73;
    valid &= numeric_case ("%x", "0x", &unsigned_field, 0, -1) && unsigned_field == 73;
    tap_ok (tap, valid, "malformed numbers fail without modifying fields or argv");
}

int main (void)
{
    struct tap_state tap = { 0, 0, 0 };
    char * original[] = { (char *)"prog", (char *)"-v", (char *)"-o", (char *)"count=7,name=alpha,unknown", (char *)"mount" };
    struct fuse_args args = FUSE_ARGS_INIT (5, original);
    struct options options;
    const struct fuse_opt spec[] = { { "-v", offsetof (struct options, verbose), 1 },
                                     { "count=%d", offsetof (struct options, count), 0 },
                                     { "auto=%i", offsetof (struct options, count), 0 },
                                     { "name=%s", offsetof (struct options, name), 0 },
                                     FUSE_OPT_END };
    char * list = NULL;
    char * empty_argv[] = { (char *)"prog" };
    char * missing_o_argv[] = { (char *)"prog", (char *)"-o" };
    char * attached_argv[] = { (char *)"prog", (char *)"-oauto=0x10,kept" };
    char * discard_argv[] = { (char *)"prog", (char *)"value" };
    char * reject_argv[] = { (char *)"prog", (char *)"-obad" };
    struct fuse_args empty_args = FUSE_ARGS_INIT (0, NULL);
    struct fuse_args missing_o_args = FUSE_ARGS_INIT (2, missing_o_argv);
    struct fuse_args attached_args = FUSE_ARGS_INIT (2, attached_argv);
    struct fuse_args discard_args = FUSE_ARGS_INIT (2, discard_argv);
    struct fuse_args reject_args = FUSE_ARGS_INIT (2, reject_argv);
    struct fuse_args invalid_args = FUSE_ARGS_INIT (1, NULL);
    struct fuse_args one_arg = FUSE_ARGS_INIT (1, empty_argv);

    memset (&options, 0, sizeof (options));
    tap_plan (&tap, 41);
    test_numeric_options (&tap);
    tap_ok (&tap, fuse_opt_match (spec, "-v"), "exact template matches");
    tap_ok (&tap, fuse_opt_match (spec, "count=9"), "numeric template matches");
    tap_ok (&tap, !fuse_opt_match (spec, "--missing"), "unknown template rejected");
    tap_ok (&tap, fuse_opt_add_opt (&list, "one") == 0, "first option appended");
    tap_ok (&tap, fuse_opt_add_opt (&list, "two") == 0, "second option appended");
    tap_ok (&tap, strcmp (list, "one,two") == 0, "option list uses comma framing");
    free (list);

    tap_ok (&tap, fuse_opt_parse (&args, &options, spec, keep_nonoptions) == 0, "option parse succeeds");
    tap_ok (&tap, options.verbose == 1, "exact flag stored");
    tap_ok (&tap, options.count == 7, "numeric value stored");
    tap_ok (&tap, options.name != NULL && strcmp (options.name, "alpha") == 0, "string value stored");
    tap_ok (
        &tap,
        args.argc == 4 && strcmp (args.argv[1], "-o") == 0 && strcmp (args.argv[2], "unknown") == 0 && strcmp (args.argv[3], "mount") == 0,
        "unknown and non-option arguments retained exactly"
    );
    tap_ok (
        &tap,
        fuse_opt_insert_arg (&args, 1, "--inserted") == 0 && strcmp (args.argv[1], "--inserted") == 0,
        "argument insertion preserves ownership"
    );
    tap_ok (&tap, fuse_opt_add_arg (NULL, "x") == -1, "null argument structure rejected");
    tap_ok (&tap, fuse_opt_add_opt (NULL, "x") == -1, "null option destination rejected");
    args.argc = -1;
    tap_ok (&tap, fuse_opt_add_arg (&args, "x") == -1, "negative argc rejected before size conversion");
    args.argc = 5;

    list = NULL;
    tap_ok (
        &tap,
        fuse_opt_add_opt_escaped (&list, "escaped") == 0 && strcmp (list, "escaped") == 0,
        "escaped option follows ordinary option ownership"
    );
    free (list);
    tap_ok (&tap, fuse_opt_add_opt (&list, NULL) == -1, "null option text rejected");
    tap_ok (&tap, fuse_opt_insert_arg (&one_arg, -1, "x") == -1, "negative insertion position rejected");
    tap_ok (&tap, fuse_opt_insert_arg (&one_arg, 2, "x") == -1, "insertion beyond argc rejected");
    tap_ok (&tap, fuse_opt_insert_arg (NULL, 0, "x") == -1, "null insertion arguments rejected");
    tap_ok (&tap, fuse_opt_add_arg (&invalid_args, "x") == -1, "nonempty argument vector cannot have null storage");
    tap_ok (&tap, fuse_opt_parse (NULL, &options, spec, keep_nonoptions) == 0, "null parse argument is a no-op");
    tap_ok (&tap, fuse_opt_parse (&empty_args, &options, spec, keep_nonoptions) == 0, "empty parse argument is a no-op");
    tap_ok (&tap, fuse_opt_parse (&missing_o_args, &options, spec, keep_nonoptions) == -1, "missing value after -o is rejected");
    tap_ok (
        &tap,
        fuse_opt_parse (&attached_args, &options, spec, keep_nonoptions) == 0 && options.count == 16 && attached_args.argc == 3 &&
            strcmp (attached_args.argv[1], "-o") == 0 && strcmp (attached_args.argv[2], "kept") == 0,
        "attached option list parses numeric bases and keeps unknowns"
    );
    tap_ok (
        &tap,
        fuse_opt_parse (&discard_args, &options, spec, discard_arguments) == 0 && discard_args.argc == 1,
        "callback can discard a non-option argument"
    );
    tap_ok (&tap, fuse_opt_parse (&reject_args, &options, spec, reject_options) == -1, "callback error aborts option-list parsing");
    tap_ok (&tap, !fuse_opt_match (NULL, "anything"), "null option table has no matches");
    fuse_opt_free_args (NULL);
    tap_ok (&tap, 1, "freeing null arguments is harmless");
    fuse_opt_free_args (&one_arg);
    tap_ok (&tap, one_arg.argc == 0 && one_arg.argv == NULL && one_arg.allocated == 0, "free resets an unallocated caller argument vector");

    free (options.name);
    fuse_opt_free_args (&args);
    fuse_opt_free_args (&missing_o_args);
    fuse_opt_free_args (&attached_args);
    fuse_opt_free_args (&discard_args);
    fuse_opt_free_args (&reject_args);
    return tap_finish (&tap);
}

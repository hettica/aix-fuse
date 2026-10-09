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
 * Modified for AIX USFS, 2026-07-26 through 2026-10-08: reduced option API,
 * a 64-bit option-key sentinel, and USFS-specific parsing documentation.
 *
 * fuse_opt.h - option parsing API of the AIX USFS client library.
 *
 * API-compatible subset of libfuse 3.19's fuse_opt.h.
 */

#ifndef FUSE_OPT_H_
#define FUSE_OPT_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Option description.
 *
 * templ may be:
 *   - a plain matching template ("-x", "--foo"): matches exactly
 *   - "foo=%s" / "--foo=%s" etc.: matches the prefix, the rest is the value
 *   - numeric parameters: d/i/u/o/x/X with no modifier or hh/h/l/ll/j/z.
 *     The destination must have the corresponding signed/unsigned C type;
 *     z uses ssize_t/size_t on AIX. d/u are decimal, i detects the base,
 *     o is octal and x/X are hexadecimal. A leading plus is accepted;
 *     minus is accepted only for signed conversions. The entire value must
 *     be a number without whitespace and within the destination's range.
 *     Other field formats (including widths and extra conversions) fail.
 *   - NULL: terminates the list (use FUSE_OPT_END)
 *
 * offset is the offsetof() the field in the user's data structure that
 * receives the value, or -1U to pass the whole argument to the processing
 * function with `value` as the key.
 * A numeric conversion failure returns -1 without changing that field or
 * replacing args. Previously processed fields are not rolled back.
 */
struct fuse_opt
{
    const char * templ;
    unsigned long offset;
    int value;
};

/* The cast matters: offset is an unsigned long, and a bare -1U would store
   0x00000000FFFFFFFF in a 64-bit build while fuse_opt_parse tests against
   (unsigned long)-1, so the entry would be taken for an offset and written
   through as a wild pointer. */
#define FUSE_OPT_KEY(templ, key)      \
    {                                 \
        templ, (unsigned long)-1, key \
    }

#define FUSE_OPT_END \
    {                \
        NULL, 0, 0   \
    }

struct fuse_args
{
    int argc;
    char ** argv;
    int allocated;
};

#define FUSE_ARGS_INIT(argc, argv) \
    {                              \
        argc, argv, 0              \
    }

#define FUSE_OPT_KEY_OPT     -1
#define FUSE_OPT_KEY_NONOPT  -2
#define FUSE_OPT_KEY_KEEP    -3
#define FUSE_OPT_KEY_DISCARD -4

/**
 * Processing function called for unmatched arguments and FUSE_OPT_KEY
 * options.
 *
 * Return -1 on error, 0 to discard the argument, 1 to keep it.
 */
typedef int (*fuse_opt_proc_t) (void * data, const char * arg, int key, struct fuse_args * outargs);

int fuse_opt_parse (struct fuse_args * args, void * data, const struct fuse_opt opts[], fuse_opt_proc_t proc);

int fuse_opt_add_opt (char ** opts, const char * opt);

int fuse_opt_add_opt_escaped (char ** opts, const char * opt);

int fuse_opt_add_arg (struct fuse_args * args, const char * arg);

int fuse_opt_insert_arg (struct fuse_args * args, int pos, const char * arg);

void fuse_opt_free_args (struct fuse_args * args);

int fuse_opt_match (const struct fuse_opt opts[], const char * opt);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_OPT_H_ */

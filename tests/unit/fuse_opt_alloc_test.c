// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "tap.h"
#include "fuse_opt.h"

#include <stdlib.h>
#include <string.h>

#ifdef USFS_AIX_ALLOC_SEAM
void *__real_calloc(size_t count, size_t size)
{
	return calloc(count, size);
}

void *__real_realloc(void *pointer, size_t size)
{
	return realloc(pointer, size);
}

char *__real_strdup(const char *text)
{
	return strdup(text);
}
#else
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
char *__real_strdup(const char *text);
#endif

static int fail_calloc;
static int fail_realloc;
static int fail_strdup;

void *__wrap_calloc(size_t count, size_t size)
{
	if (fail_calloc) {
		fail_calloc = 0;
		return NULL;
	}
	return __real_calloc(count, size);
}

void *__wrap_realloc(void *pointer, size_t size)
{
	if (fail_realloc) {
		fail_realloc -= 1;
		if (fail_realloc == 0)
			return NULL;
	}
	return __real_realloc(pointer, size);
}

char *__wrap_strdup(const char *text)
{
	if (fail_strdup) {
		fail_strdup -= 1;
		if (fail_strdup == 0)
			return NULL;
	}
	return __real_strdup(text);
}

int main(void)
{
	struct tap_state tap = { 0, 0, 0 };
	char *original[] = { (char *)"program" };
	char *nonoption_argv[] = { (char *)"program", (char *)"mount" };
	char *unknown_argv[] = { (char *)"program", (char *)"--unknown" };
	char *option_argv[] = { (char *)"program", (char *)"-okept" };
	struct fuse_args args;
	char *options = NULL;

	tap_plan(&tap, 18);

	args = (struct fuse_args)FUSE_ARGS_INIT(1, original);
	fail_calloc = 1;
	tap_ok(&tap, fuse_opt_add_arg(&args, "value") == -1,
	       "initial argv allocation failure is reported");
	tap_ok(&tap, !args.allocated && args.argv == original && args.argc == 1,
	       "calloc failure preserves caller-owned argv");

	fail_strdup = 1;
	tap_ok(&tap, fuse_opt_add_arg(&args, "value") == -1,
	       "argv copy failure is reported");
	tap_ok(&tap, !args.allocated && args.argv == original && args.argc == 1,
	       "argv copy failure rolls back ownership");

	args = (struct fuse_args){ 0, NULL, 1 };
	fail_strdup = 1;
	tap_ok(&tap, fuse_opt_add_arg(&args, "value") == -1 && args.argc == 0,
	       "new argument copy failure leaves allocated argv empty");

	fail_realloc = 1;
	tap_ok(&tap, fuse_opt_add_arg(&args, "value") == -1 && args.argc == 0 &&
	       args.argv == NULL,
	       "argv growth failure releases the temporary argument");
	fuse_opt_free_args(&args);

	fail_strdup = 1;
	tap_ok(&tap, fuse_opt_add_opt(&options, "one") == -1 && options == NULL,
	       "first option allocation failure preserves null list");
	tap_ok(&tap, fuse_opt_add_opt(&options, "one") == 0,
	       "option list can be initialized after failure");
	fail_realloc = 1;
	tap_ok(&tap, fuse_opt_add_opt(&options, "two") == -1 &&
	       strcmp(options, "one") == 0,
	       "option growth failure preserves existing list");
	tap_ok(&tap, fuse_opt_add_opt(&options, "two") == 0 &&
	       strcmp(options, "one,two") == 0,
	       "option list remains usable after allocation failure");
	free(options);

	args = (struct fuse_args)FUSE_ARGS_INIT(1, original);
	fail_calloc = 1;
	tap_ok(&tap, fuse_opt_parse(&args, NULL, NULL, NULL) == -1,
	       "parse reports initial output-vector allocation failure");
	tap_ok(&tap, !args.allocated && args.argv == original && args.argc == 1,
	       "initial parse failure preserves caller arguments");

	args = (struct fuse_args)FUSE_ARGS_INIT(2, nonoption_argv);
	fail_strdup = 2;
	tap_ok(&tap, fuse_opt_parse(&args, NULL, NULL, NULL) == -1,
	       "parse reports a kept non-option copy failure");
	tap_ok(&tap, !args.allocated && args.argv == nonoption_argv && args.argc == 2,
	       "non-option copy failure rolls back output ownership");

	args = (struct fuse_args)FUSE_ARGS_INIT(2, unknown_argv);
	fail_realloc = 2;
	tap_ok(&tap, fuse_opt_parse(&args, NULL, NULL, NULL) == -1,
	       "parse reports an unknown-option vector growth failure");
	tap_ok(&tap, !args.allocated && args.argv == unknown_argv && args.argc == 2,
	       "unknown-option failure preserves caller arguments");

	args = (struct fuse_args)FUSE_ARGS_INIT(2, option_argv);
	fail_strdup = 3;
	tap_ok(&tap, fuse_opt_parse(&args, NULL, NULL, NULL) == -1,
	       "parse reports failure while rebuilding a kept option list");
	tap_ok(&tap, !args.allocated && args.argv == option_argv && args.argc == 2,
	       "option-list rebuild failure rolls back temporary arguments");

	return tap_finish(&tap);
}

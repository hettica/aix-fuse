/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raman Dzehtsiar
 *
 * fuse_opt.c - option parsing for the AIX USFS libfuse reimplementation.
 *
 * Behavior-compatible subset of libfuse's option parser, covering the
 * patterns high-level filesystems actually use:
 *   - exact flag templates ("-h", "--help"): store `value` or invoke proc
 *   - "name=%s" templates: strdup the value into the data structure,
 *     freeing any previous value
 *   - "name=%d" / "name=%u" templates: parse an integer into the field
 *   - FUSE_OPT_KEY templates: forward to the processing function
 *   - "-o opt1,opt2,..." option lists, matched element-wise
 *   - non-option arguments: forwarded with FUSE_OPT_KEY_NONOPT
 * Matched template options are consumed; everything else is kept unless the
 * processing function says otherwise.
 */

#include "fuse_opt.h"
#include "fuse_log.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifdef USFS_ALLOC_SEAM
void * __wrap_calloc (size_t count, size_t size);
void * __wrap_realloc (void * pointer, size_t size);
char * __wrap_strdup (const char * text);

    #define calloc  __wrap_calloc
    #define realloc __wrap_realloc
    #define strdup  __wrap_strdup
#endif

enum option_process_action
{
    OPTION_PROCESS_ERROR = -1,
    OPTION_PROCESS_DISCARD = 0,
    OPTION_PROCESS_KEEP = 1
};

static const unsigned long option_key_offset = (unsigned long)-1;
static const char option_list_prefix[] = "-o";

struct option_parser
{
    void * data;                         // Caller-owned option fields and callback data.
    const struct fuse_opt * options;     // Borrowed option table, or NULL when none is supplied.
    fuse_opt_proc_t process_option;      // Optional callback for keyed and unmatched arguments.
    struct fuse_args * output_arguments; // Filtered arguments owned by fuse_opt_parse until publication.
};

struct option_match
{
    const struct fuse_opt * option; // Borrowed matching table entry, or NULL if unmatched.
    const char * argument;          // Borrowed complete argument or option-list element.
    const char * value_text;        // Borrowed value suffix, or NULL for an exact match.
};

static int append_argument (struct fuse_args * arguments, const char * argument)
{
    if (arguments == NULL)
        return -1;

    if (argument == NULL)
        return -1;

    if (arguments->argc < 0)
        return -1;

    const size_t pointer_count = (size_t)arguments->argc + 2u;
    char * argument_copy = strdup (argument);
    if (argument_copy == NULL)
        return -1;

    char ** expanded_argv = realloc (arguments->argv, pointer_count * sizeof (char *));
    if (expanded_argv == NULL)
    {
        free (argument_copy);

        return -1;
    }

    expanded_argv[arguments->argc] = argument_copy;
    expanded_argv[arguments->argc + 1] = NULL;
    arguments->argv = expanded_argv;
    arguments->argc += 1;

    return 0;
}

/* Copy caller-owned argv before growing it, so all entries can be freed uniformly. */
static int copy_caller_arguments (struct fuse_args * arguments)
{
    const size_t pointer_count = (size_t)arguments->argc + 1u;
    char ** copied_argv = calloc (pointer_count, sizeof (char *));
    if (copied_argv == NULL)
        return -1;

    for (int argument_index = 0; argument_index < arguments->argc; argument_index++)
    {
        copied_argv[argument_index] = strdup (arguments->argv[argument_index]);

        if (copied_argv[argument_index] == NULL)
        {
            while (--argument_index >= 0)
                free (copied_argv[argument_index]);

            free (copied_argv);

            return -1;
        }
    }

    arguments->argv = copied_argv;
    arguments->allocated = true;

    return 0;
}

int fuse_opt_add_arg (struct fuse_args * arguments, const char * argument)
{
    if (arguments == NULL)
        return -1;

    if (argument == NULL)
        return -1;

    if (arguments->argc < 0)
        return -1;

    if (arguments->argc > 0 && arguments->argv == NULL)
        return -1;

    if (!arguments->allocated && copy_caller_arguments (arguments) == -1)
        return -1;

    return append_argument (arguments, argument);
}

int fuse_opt_insert_arg (struct fuse_args * arguments, const int position, const char * argument)
{
    if (arguments == NULL)
        return -1;

    if (argument == NULL)
        return -1;

    if (arguments->argc < 0)
        return -1;

    if (position < 0)
        return -1;

    if (position > arguments->argc)
        return -1;

    if (fuse_opt_add_arg (arguments, argument) == -1)
        return -1;

    /* fuse_opt_add_arg appended it; rotate it into place. */
    for (int argument_index = arguments->argc - 1; argument_index > position; argument_index--)
    {
        char * const moved_argument = arguments->argv[argument_index];

        arguments->argv[argument_index] = arguments->argv[argument_index - 1];
        arguments->argv[argument_index - 1] = moved_argument;
    }

    return 0;
}

void fuse_opt_free_args (struct fuse_args * arguments)
{
    if (arguments == NULL)
        return;

    if (arguments->allocated && arguments->argv != NULL)
    {
        for (int argument_index = 0; argument_index < arguments->argc; argument_index++)
            free (arguments->argv[argument_index]);

        free (arguments->argv);
    }

    arguments->argc = 0;
    arguments->argv = NULL;
    arguments->allocated = false;
}

int fuse_opt_add_opt (char ** options, const char * option)
{
    if (options == NULL)
        return -1;

    if (option == NULL)
        return -1;

    if (*options == NULL)
    {
        char * option_copy = strdup (option);
        if (option_copy == NULL)
            return -1;

        *options = option_copy;

        return 0;
    }

    const size_t options_length = strlen (*options);
    char * expanded_options = realloc (*options, options_length + 1 + strlen (option) + 1);
    if (expanded_options == NULL)
        return -1;

    expanded_options[options_length] = ',';
    strcpy (expanded_options + options_length + 1, option);
    *options = expanded_options;

    return 0;
}

int fuse_opt_add_opt_escaped (char ** options, const char * option)
{
    /* Escaping of ',' and '\' is not needed by the current callers. */
    return fuse_opt_add_opt (options, option);
}

/* Match exact templates or prefixes before '%'; expose the remaining value text.
   "-x %s" style (space-separated) arguments are not supported. */
static int matches_template (const char * option_template, const char * argument, const char ** value_text)
{
    const char * conversion_marker = strchr (option_template, '%');

    *value_text = NULL;

    if (conversion_marker == NULL)
        return strcmp (option_template, argument) == 0;

    const size_t prefix_length = (size_t)(conversion_marker - option_template);
    if (prefix_length == 0)
        return false;

    if (strncmp (argument, option_template, prefix_length) != 0)
        return false;

    *value_text = argument + prefix_length;
    return true;
}

static struct option_match find_option (const struct fuse_opt options[], const char * argument)
{
    struct option_match match = { 0 };

    match.argument = argument;

    if (options == NULL)
        return match;

    for (const struct fuse_opt * option = options; option->templ != NULL; option++)
    {
        if (!matches_template (option->templ, argument, &match.value_text))
            continue;

        match.option = option;

        return match;
    }

    return match;
}

int fuse_opt_match (const struct fuse_opt options[], const char * option)
{
    const struct option_match match = find_option (options, option);

    return match.option != NULL;
}

enum numeric_width
{
    NUMERIC_WIDTH_INT,
    NUMERIC_WIDTH_CHAR,
    NUMERIC_WIDTH_SHORT,
    NUMERIC_WIDTH_LONG,
    NUMERIC_WIDTH_LONG_LONG,
    NUMERIC_WIDTH_INTMAX,
    NUMERIC_WIDTH_SIZE
};

enum numeric_base
{
    NUMERIC_BASE_AUTOMATIC = 0,
    NUMERIC_BASE_OCTAL = 8,
    NUMERIC_BASE_DECIMAL = 10,
    NUMERIC_BASE_HEXADECIMAL = 16
};

struct numeric_format
{
    enum numeric_width destination_width; // Destination integer type selected by the length modifier.
    int conversion_base;                  // Numeric base, or zero for base detection.
    int is_signed;                        // Whether the conversion accepts a signed value.
};

static const char * parse_short_numeric_width (const char * conversion, struct numeric_format * parsed_format)
{
    conversion++;
    parsed_format->destination_width = NUMERIC_WIDTH_SHORT;

    if (*conversion == 'h')
    {
        parsed_format->destination_width = NUMERIC_WIDTH_CHAR;
        conversion++;
    }

    return conversion;
}

static const char * parse_long_numeric_width (const char * conversion, struct numeric_format * parsed_format)
{
    conversion++;
    parsed_format->destination_width = NUMERIC_WIDTH_LONG;

    if (*conversion == 'l')
    {
        parsed_format->destination_width = NUMERIC_WIDTH_LONG_LONG;
        conversion++;
    }

    return conversion;
}

static const char * parse_numeric_width (const char * conversion, struct numeric_format * parsed_format)
{
    parsed_format->destination_width = NUMERIC_WIDTH_INT;

    if (*conversion == 'h')
        return parse_short_numeric_width (conversion, parsed_format);

    if (*conversion == 'l')
        return parse_long_numeric_width (conversion, parsed_format);

    if (*conversion == 'j')
    {
        parsed_format->destination_width = NUMERIC_WIDTH_INTMAX;

        return conversion + 1;
    }

    if (*conversion == 'z')
    {
        parsed_format->destination_width = NUMERIC_WIDTH_SIZE;

        return conversion + 1;
    }

    return conversion;
}

static int parse_numeric_format (const char * format, struct numeric_format * parsed_format)
{
    const char * conversion = parse_numeric_width (format + 1, parsed_format);

    if (*conversion == '\0')
        return -1;

    if (conversion[1] != '\0')
        return -1;

    parsed_format->is_signed = *conversion == 'd' || *conversion == 'i';

    switch (*conversion)
    {
        case 'd':
        case 'u':
            parsed_format->conversion_base = NUMERIC_BASE_DECIMAL;
            break;
        case 'i':
            parsed_format->conversion_base = NUMERIC_BASE_AUTOMATIC;
            break;
        case 'o':
            parsed_format->conversion_base = NUMERIC_BASE_OCTAL;
            break;
        case 'x':
        case 'X':
            parsed_format->conversion_base = NUMERIC_BASE_HEXADECIMAL;
            break;
        default:
            return -1;
    }

    return 0;
}

static int validate_numeric_text (const char * value_text, const struct numeric_format * parsed_format)
{
    if (*value_text == '\0')
        return -1;

    if (!parsed_format->is_signed)
    {
        if (*value_text == '-')
            return -1;
    }

    for (const unsigned char * character = (const unsigned char *)value_text; *character != '\0'; character++)
    {
        if (isspace (*character))
            return -1;
    }

    return 0;
}

static int store_signed_number (void * destination, const enum numeric_width destination_width, const intmax_t numeric_value)
{
#define STORE_SIGNED(kind, type, minimum, maximum)                                                                                                   \
    case kind:                                                                                                                                       \
        if (numeric_value < (intmax_t)(minimum))                                                                                                     \
            return -1;                                                                                                                               \
                                                                                                                                                     \
        if (numeric_value > (intmax_t)(maximum))                                                                                                     \
            return -1;                                                                                                                               \
                                                                                                                                                     \
        *(type *)destination = (type)numeric_value;                                                                                                  \
                                                                                                                                                     \
        return 0

    switch (destination_width)
    {
        STORE_SIGNED (NUMERIC_WIDTH_INT, int, INT_MIN, INT_MAX);
        STORE_SIGNED (NUMERIC_WIDTH_CHAR, signed char, SCHAR_MIN, SCHAR_MAX);
        STORE_SIGNED (NUMERIC_WIDTH_SHORT, short, SHRT_MIN, SHRT_MAX);
        STORE_SIGNED (NUMERIC_WIDTH_LONG, long, LONG_MIN, LONG_MAX);
        STORE_SIGNED (NUMERIC_WIDTH_LONG_LONG, long long, LLONG_MIN, LLONG_MAX);
        STORE_SIGNED (NUMERIC_WIDTH_INTMAX, intmax_t, INTMAX_MIN, INTMAX_MAX);
        STORE_SIGNED (NUMERIC_WIDTH_SIZE, ssize_t, -SSIZE_MAX - 1, SSIZE_MAX);
    }
#undef STORE_SIGNED

    return -1;
}

static int store_unsigned_number (void * destination, const enum numeric_width destination_width, const uintmax_t numeric_value)
{
#define STORE_UNSIGNED(kind, type, maximum)                                                                                                          \
    case kind:                                                                                                                                       \
        if (numeric_value > (uintmax_t)(maximum))                                                                                                    \
            return -1;                                                                                                                               \
                                                                                                                                                     \
        *(type *)destination = (type)numeric_value;                                                                                                  \
                                                                                                                                                     \
        return 0

    switch (destination_width)
    {
        STORE_UNSIGNED (NUMERIC_WIDTH_INT, unsigned int, UINT_MAX);
        STORE_UNSIGNED (NUMERIC_WIDTH_CHAR, unsigned char, UCHAR_MAX);
        STORE_UNSIGNED (NUMERIC_WIDTH_SHORT, unsigned short, USHRT_MAX);
        STORE_UNSIGNED (NUMERIC_WIDTH_LONG, unsigned long, ULONG_MAX);
        STORE_UNSIGNED (NUMERIC_WIDTH_LONG_LONG, unsigned long long, ULLONG_MAX);
        STORE_UNSIGNED (NUMERIC_WIDTH_INTMAX, uintmax_t, UINTMAX_MAX);
        STORE_UNSIGNED (NUMERIC_WIDTH_SIZE, size_t, SIZE_MAX);
    }
#undef STORE_UNSIGNED

    return -1;
}

static int convert_signed_option (void * destination, const struct numeric_format * parsed_format, const char * value_text)
{
    char * conversion_end = NULL;
    const intmax_t numeric_value = strtoimax (value_text, &conversion_end, parsed_format->conversion_base);

    if (conversion_end == value_text)
        return -1;

    if (*conversion_end != '\0')
        return -1;

    if (errno == ERANGE)
        return -1;

    return store_signed_number (destination, parsed_format->destination_width, numeric_value);
}

static int convert_unsigned_option (void * destination, const struct numeric_format * parsed_format, const char * value_text)
{
    char * conversion_end = NULL;
    const uintmax_t numeric_value = strtoumax (value_text, &conversion_end, parsed_format->conversion_base);

    if (conversion_end == value_text)
        return -1;

    if (*conversion_end != '\0')
        return -1;

    if (errno == ERANGE)
        return -1;

    return store_unsigned_number (destination, parsed_format->destination_width, numeric_value);
}

/* Convert fully before touching the caller's field. In particular, a length
   modifier controls the destination type, not just the accepted value range. */
static int convert_numeric_option (void * destination, const char * format, const char * value_text)
{
    struct numeric_format parsed_format = { 0 };

    if (parse_numeric_format (format, &parsed_format) != 0)
        return -1;

    if (validate_numeric_text (value_text, &parsed_format) != 0)
        return -1;

    errno = 0;

    if (parsed_format.is_signed)
        return convert_signed_option (destination, &parsed_format, value_text);

    return convert_unsigned_option (destination, &parsed_format, value_text);
}

static int apply_numeric_option (void * destination, const char * format, const struct option_match * match)
{
    if (convert_numeric_option (destination, format, match->value_text) == 0)
        return 0;

    fuse_log (FUSE_LOG_ERR, "Failed to parse option `%s': invalid numeric value for format `%s'\n", match->argument, format);
    return -1;
}

static int apply_string_option (char ** destination, const char * value_text)
{
    free (*destination);
    *destination = strdup (value_text);
    if (*destination == NULL)
        return -1;

    return 0;
}

static int apply_option (const struct option_parser * parser, const struct option_match * match)
{
    const struct fuse_opt * option = match->option;

    if (option->offset == option_key_offset)
    {
        if (parser->process_option == NULL)
            return OPTION_PROCESS_KEEP;

        return parser->process_option (parser->data, match->argument, option->value, parser->output_arguments);
    }

    if (match->value_text == NULL)
    {
        int * destination = (int *)((char *)parser->data + option->offset);

        *destination = option->value;

        return OPTION_PROCESS_DISCARD;
    }

    const char * format = strchr (option->templ, '%');

    if (format == NULL)
        return OPTION_PROCESS_DISCARD;

    if (strcmp (format, "%s") != 0)
        return apply_numeric_option ((char *)parser->data + option->offset, format, match);

    return apply_string_option ((char **)((char *)parser->data + option->offset), match->value_text);
}

static int process_option_element (const struct option_parser * parser, const char * element, char ** kept_options)
{
    const struct option_match match = find_option (parser->options, element);

    if (match.option != NULL)
    {
        const int action = apply_option (parser, &match);
        if (action == OPTION_PROCESS_ERROR)
            return -1;

        if (action == OPTION_PROCESS_KEEP)
            return fuse_opt_add_opt (kept_options, element) == -1 ? -1 : 0;

        return 0;
    }

    if (parser->process_option != NULL)
    {
        const int action = parser->process_option (parser->data, element, FUSE_OPT_KEY_OPT, parser->output_arguments);
        if (action == OPTION_PROCESS_ERROR)
            return -1;

        if (action == OPTION_PROCESS_DISCARD)
            return 0;
    }

    return fuse_opt_add_opt (kept_options, element) == -1 ? -1 : 0;
}

static int process_option_elements (const struct option_parser * parser, char * option_list, char ** kept_options)
{
    char * tokenizer_state = NULL;

    for (const char * element = strtok_r (option_list, ",", &tokenizer_state); element != NULL; element = strtok_r (NULL, ",", &tokenizer_state))
    {
        if (process_option_element (parser, element, kept_options) == -1)
            return -1;
    }

    return 0;
}

static int append_kept_options (struct fuse_args * output_arguments, const char * kept_options)
{
    if (kept_options == NULL)
        return 0;

    if (fuse_opt_add_arg (output_arguments, option_list_prefix) == -1)
        return -1;

    if (fuse_opt_add_arg (output_arguments, kept_options) == -1)
        return -1;

    return 0;
}

static int process_option_list (const struct option_parser * parser, const char * option_list)
{
    char * list_copy = strdup (option_list);
    if (list_copy == NULL)
        return -1;

    char * kept_options = NULL;
    int rc = process_option_elements (parser, list_copy, &kept_options);

    free (list_copy);

    if (rc != -1)
        rc = append_kept_options (parser->output_arguments, kept_options);

    free (kept_options);

    return rc == -1 ? -1 : 0;
}

static int process_nonoption_argument (const struct option_parser * parser, const char * argument)
{
    int action = OPTION_PROCESS_KEEP;

    if (parser->process_option != NULL)
    {
        action = parser->process_option (parser->data, argument, FUSE_OPT_KEY_NONOPT, parser->output_arguments);
        if (action == OPTION_PROCESS_ERROR)
            return -1;
    }

    if (action == OPTION_PROCESS_KEEP)
        return fuse_opt_add_arg (parser->output_arguments, argument);

    return 0;
}

static int process_named_option_argument (const struct option_parser * parser, const char * argument)
{
    const struct option_match match = find_option (parser->options, argument);

    if (match.option != NULL)
    {
        const int action = apply_option (parser, &match);
        if (action == OPTION_PROCESS_ERROR)
            return -1;

        if (action == OPTION_PROCESS_KEEP)
            return fuse_opt_add_arg (parser->output_arguments, argument);

        return 0;
    }

    if (parser->process_option != NULL)
    {
        const int action = parser->process_option (parser->data, argument, FUSE_OPT_KEY_OPT, parser->output_arguments);
        if (action == OPTION_PROCESS_ERROR)
            return -1;

        if (action == OPTION_PROCESS_DISCARD)
            return 0;
    }

    return fuse_opt_add_arg (parser->output_arguments, argument);
}

static int process_separate_option_list (const struct option_parser * parser, const struct fuse_args * arguments, int * argument_index)
{
    if (*argument_index + 1 >= arguments->argc)
    {
        fuse_log (FUSE_LOG_ERR, "Failed to parse option -o: missing option list\n");
        return -1;
    }

    *argument_index += 1;

    return process_option_list (parser, arguments->argv[*argument_index]);
}

static int process_argument (const struct option_parser * parser, const struct fuse_args * arguments, int * argument_index)
{
    const char * argument = arguments->argv[*argument_index];

    if (strcmp (argument, option_list_prefix) == 0)
        return process_separate_option_list (parser, arguments, argument_index);

    const size_t prefix_length = sizeof (option_list_prefix) - 1;

    if (strncmp (argument, option_list_prefix, prefix_length) == 0)
    {
        if (argument[prefix_length] != '\0')
            return process_option_list (parser, argument + prefix_length);
    }

    if (argument[0] != '-')
        return process_nonoption_argument (parser, argument);

    return process_named_option_argument (parser, argument);
}

int fuse_opt_parse (struct fuse_args * arguments, void * data, const struct fuse_opt options[], fuse_opt_proc_t process_option)
{
    if (arguments == NULL)
        return 0;

    if (arguments->argc == 0)
        return 0;

    struct fuse_args output_arguments = FUSE_ARGS_INIT (0, NULL);

    if (fuse_opt_add_arg (&output_arguments, arguments->argv[0]) == -1)
        return -1;

    struct option_parser parser = { 0 };

    parser.data = data;
    parser.options = options;
    parser.process_option = process_option;
    parser.output_arguments = &output_arguments;

    for (int argument_index = 1; argument_index < arguments->argc; argument_index++)
    {
        if (process_argument (&parser, arguments, &argument_index) == -1)
        {
            fuse_opt_free_args (&output_arguments);

            return -1;
        }
    }

    fuse_opt_free_args (arguments);
    arguments->argc = output_arguments.argc;
    arguments->argv = output_arguments.argv;
    arguments->allocated = output_arguments.allocated;

    return 0;
}

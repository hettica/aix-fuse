// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "tap.h"

#include <stdarg.h>
#include <stdio.h>

void tap_plan(struct tap_state *state, unsigned count)
{
    state->planned = count;
    state->executed = 0;
    state->failed = 0;
    printf("TAP version 13\n1..%u\n", count);
}

void tap_ok(struct tap_state *state, int condition, const char *name)
{
    state->executed++;
    if (!condition)
        state->failed++;
    printf("%s %u - %s\n", condition ? "ok" : "not ok",
           state->executed, name);
}

void tap_diag(const char *format, ...)
{
    va_list args;

    fputs("# ", stdout);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fputc('\n', stdout);
}

int tap_finish(const struct tap_state *state)
{
    if (state->executed != state->planned) {
        tap_diag("plan mismatch: planned=%u executed=%u",
                 state->planned, state->executed);
        return 2;
    }
    return state->failed == 0 ? 0 : 1;
}

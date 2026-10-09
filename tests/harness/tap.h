// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_TEST_TAP_H
#define USFS_TEST_TAP_H

#include <stddef.h>
#include <stdint.h>

struct tap_state {
    unsigned planned;
    unsigned executed;
    unsigned failed;
};

void tap_plan(struct tap_state *state, unsigned count);
void tap_ok(struct tap_state *state, int condition, const char *name);
void tap_diag(const char *format, ...);
int tap_finish(const struct tap_state *state);

#define TAP_EQ_U64(state, actual, expected, name)                           \
    do {                                                                    \
        uint64_t _actual = (uint64_t)(actual);                              \
        uint64_t _expected = (uint64_t)(expected);                          \
        if (_actual != _expected)                                           \
            tap_diag("%s: actual=%llu expected=%llu", (name),               \
                     (unsigned long long)_actual,                            \
                     (unsigned long long)_expected);                         \
        tap_ok((state), _actual == _expected, (name));                       \
    } while (0)

#define TAP_EQ_SIZE(state, actual, expected, name)                          \
    TAP_EQ_U64((state), (size_t)(actual), (size_t)(expected), (name))

#endif

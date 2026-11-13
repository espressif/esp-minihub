/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Minimal check/run scaffolding shared by the host test binaries. */

#pragma once

#include <stdio.h>
#include <string.h>

static int s_tests;
static int s_checks;
static int s_failures;
static const char *s_test;

#define CHECK(cond)                                                           \
    do {                                                                      \
        s_checks++;                                                           \
        if (!(cond)) {                                                        \
            s_failures++;                                                     \
            printf("FAIL %s (%s:%d): %s\n", s_test, __FILE__, __LINE__, #cond); \
        }                                                                     \
    } while (0)

#define CHECK_STR(got, want)                                                  \
    do {                                                                      \
        s_checks++;                                                           \
        const char *g_ = (got);                                               \
        const char *w_ = (want);                                              \
        if (!g_ || !w_ || strcmp(g_, w_) != 0) {                              \
            s_failures++;                                                     \
            printf("FAIL %s (%s:%d): %s = \"%s\", want \"%s\"\n", s_test,      \
                   __FILE__, __LINE__, #got, g_ ? g_ : "(null)",              \
                   w_ ? w_ : "(null)");                                       \
        }                                                                     \
    } while (0)

#define CHECK_DBL(got, want)                                                  \
    do {                                                                      \
        s_checks++;                                                           \
        double g_ = (got);                                                    \
        double w_ = (want);                                                   \
        double d_ = g_ - w_;                                                  \
        if (d_ < 0) {                                                         \
            d_ = -d_;                                                         \
        }                                                                     \
        if (d_ > 1e-9) {                                                      \
            s_failures++;                                                     \
            printf("FAIL %s (%s:%d): %s = %g, want %g\n", s_test, __FILE__,    \
                   __LINE__, #got, g_, w_);                                   \
        }                                                                     \
    } while (0)

#define RUN(fn)                                                               \
    do {                                                                      \
        s_test = #fn;                                                         \
        s_tests++;                                                            \
        int before_ = s_failures;                                             \
        fn();                                                                 \
        printf("%s %s\n", s_failures == before_ ? "ok" : "not ok", #fn);       \
    } while (0)

static int test_summary(void)
{
    printf("\nRan %d tests, %d checks, %d failures\n", s_tests, s_checks, s_failures);
    return s_failures ? 1 : 0;
}

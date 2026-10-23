// Tiny assertion harness for the host tests (tests/run.sh). Each test binary returns 0 when every check passed.
#pragma once
#include <stdio.h>
#include <string.h>

static int t_checks, t_failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        t_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            t_failures++;                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        t_checks++;                                                                                                    \
        long long a_ = (long long)(a), b_ = (long long)(b);                                                            \
        if (a_ != b_) {                                                                                                \
            t_failures++;                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, a_, b_);              \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(a, b)                                                                                                \
    do {                                                                                                               \
        t_checks++;                                                                                                    \
        const char *a_ = (a), *b_ = (b);                                                                               \
        if (strcmp(a_, b_)) {                                                                                          \
            t_failures++;                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s == %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, #a, #b, a_, b_);          \
        }                                                                                                              \
    } while (0)

#define CHECK_MEM(a, b, n)                                                                                             \
    do {                                                                                                               \
        t_checks++;                                                                                                    \
        const void *a_ = (a), *b_ = (b);                                                                               \
        size_t n_ = (size_t)(n);                                                                                       \
        if (memcmp(a_, b_, n_)) {                                                                                      \
            t_failures++;                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%zu bytes)\n", __FILE__, __LINE__, #a, #b, n_);                     \
        }                                                                                                              \
    } while (0)

// main() ends with this: the count and the exit status.
#define TEST_END()                                                                                                     \
    do {                                                                                                               \
        if (t_failures)                                                                                                \
            fprintf(stderr, "%d of %d checks failed\n", t_failures, t_checks);                                         \
        else                                                                                                           \
            fprintf(stderr, "%d checks passed\n", t_checks);                                                           \
        return t_failures ? 1 : 0;                                                                                     \
    } while (0)

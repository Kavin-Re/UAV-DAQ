/* Minimal assertion helpers for the host-side tests. */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <string.h>

static int test_failures;
static int test_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        test_checks++;                                                           \
        if (!(cond)) {                                                           \
            test_failures++;                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

#define CHECK_INT(actual, expected)                                              \
    do {                                                                         \
        long a_ = (long)(actual), e_ = (long)(expected);                         \
        test_checks++;                                                           \
        if (a_ != e_) {                                                          \
            test_failures++;                                                     \
            printf("  FAIL %s:%d: %s = %ld, expected %ld\n", __FILE__, __LINE__, #actual, a_, e_); \
        }                                                                        \
    } while (0)

#define CHECK_STR(actual, expected)                                              \
    do {                                                                         \
        test_checks++;                                                           \
        if (strcmp((actual), (expected)) != 0) {                                 \
            test_failures++;                                                     \
            printf("  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (actual), (expected)); \
        }                                                                        \
    } while (0)

#define RUN(test)                                                                \
    do {                                                                         \
        printf("- %s\n", #test);                                                 \
        test();                                                                  \
    } while (0)

#define TEST_SUMMARY()                                                           \
    (printf("%d checks, %d failure(s)\n", test_checks, test_failures), test_failures ? 1 : 0)

#endif

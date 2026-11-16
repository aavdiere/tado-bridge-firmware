/* Minimal host test helpers (#17). Each test file is one program; a failed
 * check prints its location and the program exits non-zero at the end. */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

static int t_checks, t_failed;

#define CHECK(cond)                                                \
    do {                                                           \
        t_checks++;                                                \
        if (!(cond)) {                                             \
            t_failed++;                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        long long t_a = (long long)(a), t_b = (long long)(b);                              \
        t_checks++;                                                                        \
        if (t_a != t_b) {                                                                  \
            t_failed++;                                                                    \
            printf("FAIL %s:%d: %s = %lld, want %s = %lld\n", __FILE__, __LINE__, #a, t_a, \
                   #b, t_b);                                                               \
        }                                                                                  \
    } while (0)

#define CHECK_MEM(a, b, n)                                                          \
    do {                                                                            \
        t_checks++;                                                                 \
        if (memcmp((a), (b), (n)) != 0) {                                           \
            t_failed++;                                                             \
            printf("FAIL %s:%d: %s differs from %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                           \
    } while (0)

#define TEST_REPORT(name) \
    (printf("%s: %d checks, %d failed\n", name, t_checks, t_failed), t_failed ? 1 : 0)

#endif

/*
 * tests/regress.c -- checks for code generation bugs that CHANGES.md
 * lists as fixed, plus a few things that must keep working.  Prints
 * "All tests passed" (or the failing lines) on the serial port.
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("FAIL: line %d: %s\n", __LINE__, #c);            \
            failures++;                                             \
        }                                                           \
    } while (0)

/* volatile: the values reach the code at run time, not folded */
static volatile uint64_t u64a = 1000000000123ULL, u64b = 97;
static volatile uint64_t u64c = 0xFEDCBA9876543210ULL, u64d = 0x12345;
static volatile float f1 = 0.1f, f2 = 3.14159265f;
static volatile unsigned bits = 0x80, word = 0xABCDE123, field = 0x1A5;
static volatile int big = INT_MAX, small = INT_MIN;

static uint64_t bits_of(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

int main(void)
{
    /* CHANGES 2: 64-bit unsigned division and modulo at -O2 and -Os */
    uint64_t q = u64a / u64b, r = u64a % u64b;
    CHECK(q == 10309278351ULL && r == 76);
    q = u64c / u64d;
    r = u64c % u64d;
    CHECK(q == 0xE0004FA01C4DULL && r == 68175);

    /* CHANGES 3: float to double, exactly */
    CHECK(bits_of(f1) == 0x3FB99999A0000000ULL);
    CHECK(bits_of(f2) == 0x400921FB60000000ULL);

    /* CHANGES 5: __builtin_ctz links and works */
    CHECK(__builtin_ctz(bits) == 7);

    /* New patterns: 9-bit field inserts (MOVS, MOVD, MOVI) */
    unsigned w = word, v = field;
    CHECK(((w & ~0x1FFu) | (v & 0x1FF)) == 0xABCDE1A5);
    CHECK(((w & ~(0x1FFu << 9)) | ((v & 0x1FF) << 9)) == 0xABCF4B23);
    CHECK(((w & ~(0x1FFu << 23)) | ((v & 0x1FF) << 23)) == 0xD2CDE123);

    /* New patterns: signed overflow (ADDS, SUBS) */
    int res;
    CHECK(__builtin_add_overflow(big, 1, &res));
    CHECK(__builtin_sub_overflow(small, 1, &res));
    CHECK(!__builtin_add_overflow(big - 1, 1, &res) && res == INT_MAX);

    if (failures == 0)
        printf("All tests passed\n");
    else
        printf("%d checks failed\n", failures);
    return 0;
}

/*
 * tests/align.c -- objects aligned to more than the 4 bytes any type
 * needs: globals initialised and not, static, const, common, arrays of
 * an over-aligned type, an over-aligned member, a function, and locals.
 * Each must be at an address that is a multiple of its alignment and
 * keep its contents through the start-up copies.  Prints "All alignment
 * tests passed" (or what failed) on the serial port.
 */
#include <stdint.h>
#include <stdio.h>

#define A(n) __attribute__((aligned(n)))

/* small objects between the aligned ones, so that they do not land on an
   aligned address by chance */
char pad1 = 1;
_Alignas(8) char g8_init[3] = { 1, 2, 3 };
char pad2 = 2;
_Alignas(16) char g16_bss[5];
char pad3;
static char s32_bss[7] A(32);
static char pad4 = 4;
static char s64_init[9] A(64) = "abc";
const char c128[3] A(128) = { 7, 8, 9 };
char pad5 = 5;
__attribute__((common)) char comm256[11] A(256);
typedef struct { int a, b; } A(8) T;
T tarr[3] = { { 1, 2 }, { 3, 4 }, { 5, 6 } };
struct S { char c; long long v A(8); } sv = { 1, 2 };
char big4096[17] A(4096);

A(256) int fn(int x) { return x + 1; }

static int failures;

static void check(const char *what, const void *p, unsigned a)
{
    if ((uintptr_t)p & (a - 1)) {
        printf("FAIL: %s at %p is not %u-aligned\n", what, p, a);
        failures++;
    }
}

__attribute__((noinline)) static void use(void *p)
{
    __asm__ volatile ("" :: "r"(p) : "memory");
}

/* locals: sp moves by a different amount at each depth */
__attribute__((noinline)) static void locals(int depth)
{
    _Alignas(8) char b[3];
    T t;
    struct S s;
    _Alignas(16) char c16[5];
    char pad[depth & 7];

    use(pad); use(b); use(&t); use(&s); use(c16);
    check("local _Alignas(8)", b, 8);
    check("local of an aligned(8) type", &t, 8);
    check("local's aligned(8) member", &s.v, 8);
    check("local _Alignas(16)", c16, 16);
    if (depth < 12)
        locals(depth + 1);
}

int main(void)
{
    check("g8_init", g8_init, 8);
    check("g16_bss", g16_bss, 16);
    check("s32_bss", s32_bss, 32);
    check("s64_init", s64_init, 64);
    check("c128", c128, 128);
    check("comm256", comm256, 256);
    check("tarr[1]", &tarr[1], 8);
    check("sv.v", &sv.v, 8);
    check("big4096", big4096, 4096);
    check("fn", (const void *)fn, 256);
    if (g8_init[2] != 3 || s64_init[1] != 'b' || c128[2] != 9 || tarr[2].b != 6 || sv.v != 2
        || g16_bss[4] || s32_bss[6] || comm256[10] || big4096[16] || pad3
        || pad1 + pad2 + pad4 + pad5 != 12 || fn(1) != 2) {
        printf("FAIL: contents\n");
        failures++;
    }
    locals(0);
    if (failures == 0)
        printf("All alignment tests passed\n");
    else
        printf("%d checks failed\n", failures);
    return 0;
}

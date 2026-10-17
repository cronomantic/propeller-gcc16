/*
 * tests/heap.c -- malloc, aligned_alloc, realloc (growing and shrinking)
 * and free mixed at random; after every step each live block must still
 * hold its own pattern and be aligned as asked, so that an operation
 * that corrupts the heap shows.  Also aligned_alloc's argument checks.
 * Prints "All heap tests passed" (or what failed) on the serial port.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 24
#define STEPS 600

static struct { unsigned char *p; size_t n; unsigned align; unsigned char fill; } slot[SLOTS];
static int failures;
static unsigned seed = 12345;

static unsigned rnd(unsigned n)
{
    seed = seed * 1103515245 + 12345;
    return (seed >> 16) % n;
}

static int intact(int i)
{
    for (size_t k = 0; k < slot[i].n; k++)
        if (slot[i].p[k] != (unsigned char)(slot[i].fill + k))
            return 0;
    return 1;
}

static void fill(int i)
{
    for (size_t k = 0; k < slot[i].n; k++)
        slot[i].p[k] = (unsigned char)(slot[i].fill + k);
}

static void fail(int step, const char *what, int i)
{
    if (failures++ < 10)
        printf("FAIL: step %d: %s (slot %d, %p, %u bytes, align %u)\n", step, what, i,
               (void *)slot[i].p, (unsigned)slot[i].n, slot[i].align);
}

/* through a volatile pointer: GCC knows aligned_alloc, and removes a call
   whose result is only compared with NULL, taking it as successful */
static void *(*volatile aligned_alloc_p)(size_t, size_t) = aligned_alloc;

int main(void)
{
    static const unsigned aligns[] = { 4, 8, 16, 32, 64, 256 };

    if (aligned_alloc_p(0, 8) || aligned_alloc_p(12, 8) || aligned_alloc_p(24, 8)) {
        printf("FAIL: aligned_alloc accepted an alignment that is not a power of two\n");
        failures++;
    }
    for (int step = 0; step < STEPS && failures < 10; step++) {
        int i = rnd(SLOTS);
        if (!slot[i].p) {
            slot[i].n = 1 + rnd(rnd(4) ? 24 : 200);
            slot[i].fill = (unsigned char)rnd(256);
            if (rnd(2)) {
                slot[i].align = aligns[rnd(6)];
                slot[i].p = aligned_alloc(slot[i].align, slot[i].n);
            } else {
                slot[i].align = 4;
                slot[i].p = malloc(slot[i].n);
            }
            if (!slot[i].p) {
                fail(step, "out of memory", i);
                break;
            }
            if ((uintptr_t)slot[i].p & (slot[i].align - 1))
                fail(step, "misaligned", i);
            fill(i);
        } else if (rnd(3) == 0) {
            /* realloc keeps the contents up to the smaller size; it only
               promises malloc's alignment */
            size_t n = 1 + rnd(rnd(2) ? slot[i].n : 200);
            unsigned char *q = realloc(slot[i].p, n);
            if (!q) {
                fail(step, "realloc returned NULL", i);
                break;
            }
            slot[i].p = q;
            if (n < slot[i].n)
                slot[i].n = n;
            if (!intact(i))
                fail(step, "realloc lost the contents", i);
            slot[i].n = n;
            slot[i].align = 4;
            if ((uintptr_t)q & 3)
                fail(step, "realloc misaligned", i);
            fill(i);
        } else {
            free(slot[i].p);
            slot[i].p = NULL;
        }
        if (step % 4 == 0 || step == STEPS - 1)
            for (int j = 0; j < SLOTS; j++)
                if (slot[j].p && !intact(j))
                    fail(step, "block overwritten", j);
    }
    if (failures == 0)
        printf("All heap tests passed\n");
    else
        printf("%d checks failed\n", failures);
    return 0;
}

/*
 * tests/bench.c -- the BYTE sieve (8191 flags) and fib(20), timed with
 * CNT.  Prints the times and "All benchmarks passed" on the serial port.
 * From PropDuo (tests/c/bench.c), where it compares PropGCC's memory
 * models with other compilers.
 *
 *   -DFIB_IN_HUB: fib() in hub RAM (HUBTEXT) in the XMM models
 */
#include <propeller.h>
#include <stdio.h>

#define FLAGS 8191

static char flags[FLAGS];

static int sieve(void)
{
    int i, prime, k, count = 0;
    for (i = 0; i < FLAGS; i++)
        flags[i] = 1;
    for (i = 0; i < FLAGS; i++)
        if (flags[i]) {
            prime = i + i + 3;
            for (k = i + prime; k < FLAGS; k += prime)
                flags[k] = 0;
            count++;
        }
    return count;
}

#ifdef FIB_IN_HUB
HUBTEXT
#endif
static int fib(int n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static unsigned long us(unsigned long clocks)
{
    unsigned long per_ms = CLKFREQ / 1000;
    return clocks / per_ms * 1000 + clocks % per_ms * 1000 / per_ms;
}

int main(void)
{
    unsigned long t;
    int r, ok = 1;

    t = CNT;
    r = sieve();
    t = CNT - t;
    printf("Sieve: %d primes, %lu us\n", r, us(t));
    ok = ok && r == 1899;

    t = CNT;
    r = fib(20);
    t = CNT - t;
    printf("fib(20) = %d, %lu us\n", r, us(t));
    ok = ok && r == 6765;

    printf(ok ? "All benchmarks passed\n" : "WRONG RESULT\n");
    return 0;
}

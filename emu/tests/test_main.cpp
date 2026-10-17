// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Cronomantic
// test_main.cpp -- Tests of the P1 emulator (make -C emu test).
#include <cstdio>
#include "check.h"

int checks = 0, failures = 0;

int main()
{
    runP1Tests();
    runCounterTests();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

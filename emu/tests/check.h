// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Cronomantic
// check.h -- Minimal test helpers for the emulator tests.
#pragma once
#include <cstdio>

extern int checks;
extern int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

void runP1Tests();
void runCounterTests();
void runSpiSramTests();

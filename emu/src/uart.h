// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Cronomantic
// uart.h -- Serial receiver (8N1) watching one pin of an emulated P1: the
// terminal side of a Prop Plug.  Decodes from the pin's edges, using the
// bit time in clocks (the program's clkfreq / baud).
#pragma once
#include <cstdint>
#include "p1.h"

class Uart {
public:
    static constexpr int TEXT_SIZE = 1 << 16;

    char text[TEXT_SIZE + 1];   // everything received, NUL-terminated
    int length;

    Uart();
    // Watch 'pin' of 'chip' (installs the chip's pin hook).
    void attach(P1 &chip, int pin, uint32_t clkfreq, uint32_t baud);
    // Decode up to clock t (call before reading text at the end of a run).
    void flush(uint64_t t);
    // Characters received since the last call (for live output).
    const char *take(int *count);
    // Forget the first n characters of text (already used): text keeps
    // only what came after, so that a long output does not fill it.
    void discard(int n);
    // The pin hook attach() installs; a program that watches other pins
    // too installs its own and calls this one from it (ctx: the Uart).
    static void onPins(P1 &chip, uint64_t when, void *ctx);

private:
    P1 *chip;
    int pin;
    uint64_t bitClocks;
    bool level;
    bool inFrame;
    uint64_t nextSample;
    int bit;
    uint32_t shift;
    int taken;

    void update(bool newLevel, uint64_t t);
};

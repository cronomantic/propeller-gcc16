// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Cronomantic
// uart.cpp -- Serial receiver on an emulated P1 pin (see uart.h).
#include "uart.h"

Uart::Uart()
{
    chip = nullptr;
    pin = 30;
    bitClocks = 1;
    level = false;
    inFrame = false;
    nextSample = 0;
    bit = 0;
    shift = 0;
    length = 0;
    taken = 0;
    text[0] = 0;
}

void Uart::attach(P1 &c, int p, uint32_t clkfreq, uint32_t baud)
{
    chip = &c;
    pin = p;
    bitClocks = clkfreq / baud;
    level = (c.ina() >> pin) & 1;
    c.pinHook = onPins;
    c.pinCtx = this;
}

void Uart::onPins(P1 &chip, uint64_t when, void *ctx)
{
    Uart *u = (Uart *)ctx;
    u->update((chip.ina() >> u->pin) & 1, when);
}

// Samples between the last edge and t see the level held until t; a
// falling edge on an idle line starts a frame, sampled mid-bit.
void Uart::update(bool newLevel, uint64_t t)
{
    flush(t);
    if (!inFrame && level && !newLevel) {
        inFrame = true;
        bit = 0;
        shift = 0;
        nextSample = t + bitClocks + bitClocks / 2;
    }
    level = newLevel;
}

void Uart::flush(uint64_t t)
{
    while (inFrame && nextSample < t) {
        if (bit < 8) {
            if (level)
                shift |= 1u << bit;
            bit++;
            nextSample += bitClocks;
        } else {
            if (level && length < TEXT_SIZE) {      // stop bit: frame OK
                text[length++] = (char)shift;
                text[length] = 0;
            }
            inFrame = false;
        }
    }
}

const char *Uart::take(int *count)
{
    const char *p = text + taken;
    *count = length - taken;
    taken = length;
    return p;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Cronomantic
// spisram.h -- A 23LC1024 serial SRAM (128 KB, SPI mode 0) on four pins of
// an emulated P1: the external memory of propeller-load's
// spi_sram24_xmem.spin driver, and of the XMM tests.
//
// Commands: READ $03 and WRITE $02 with a 24-bit address (the chip uses
// the low 17 bits), RDMR $05 and WRMR $01 for the mode register (byte,
// page of 32 bytes, or sequential: where the address wraps in a
// transfer), RSTIO $FF.  The chip samples SI on the rising edge of SCK
// and changes SO after the falling edge; SO is not driven while /CS is
// high, and an undriven /CS reads high (the boards' pull-up).
//
// No timing is checked: the P1 cannot clock SPI faster than the chip's
// 20 MHz.  Call pins() from the chip's pin hook.
#pragma once
#include <cstdint>
#include "p1.h"

class SpiSram {
public:
    static constexpr uint32_t SIZE = 128 * 1024;

    uint8_t mem[SIZE];
    uint32_t transfers;         // commands completed (for tests)

    SpiSram();
    // Wire it to 'chip': the pins of /CS, SCK, SI (the P1's MOSI) and SO
    // (its MISO).
    void attach(P1 &chip, int cs, int sck, int si, int so);
    // The pins may have changed at clock 'when'.
    void pins(P1 &chip, uint64_t when);
    // Contents from a file, from address 0; false if it cannot be read or
    // does not fit.
    bool load(const char *path);

private:
    enum State { IDLE, COMMAND, ADDRESS, DATA_IN, DATA_OUT, MODE_IN, MODE_OUT };
    int csPin, sckPin, siPin, soPin;
    bool selected, sck;
    State state;
    uint8_t command, mode;
    uint32_t shift, address;
    int bits;                   // bits shifted in or out of the current field
    uint8_t outByte;
    bool so;

    bool level(const P1 &chip, int pin, bool undriven) const;
    void risingEdge(bool si);
    void fallingEdge();
    void next();                // the address after a byte, as the mode says
    void drive(P1 &chip, uint64_t when);
};

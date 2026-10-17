// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Cronomantic
// spisram.cpp -- 23LC1024 serial SRAM on an emulated P1 (see spisram.h).
#include "spisram.h"
#include <cstdio>
#include <cstring>

static constexpr uint8_t READ = 0x03, WRITE = 0x02, RDMR = 0x05, WRMR = 0x01;
static constexpr uint8_t MODE_BYTE = 0x00, MODE_PAGE = 0x80, MODE_SEQUENTIAL = 0x40;

SpiSram::SpiSram()
{
    memset(mem, 0, sizeof(mem));
    transfers = 0;
    csPin = sckPin = siPin = soPin = -1;
    selected = sck = so = false;
    state = IDLE;
    command = 0;
    mode = MODE_SEQUENTIAL;         // the 23LC1024's mode at power-up
    shift = address = 0;
    bits = 0;
    outByte = 0;
}

void SpiSram::attach(P1 &chip, int cs, int sckp, int si, int sop)
{
    csPin = cs;
    sckPin = sckp;
    siPin = si;
    soPin = sop;
    selected = !level(chip, csPin, true);
    sck = level(chip, sckPin, false);
    state = selected ? COMMAND : IDLE;
}

bool SpiSram::load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(mem, 1, SIZE, f);
    bool more = fgetc(f) != EOF;
    fclose(f);
    return n > 0 && !more;
}

// What the chip sees on a pin: the P1's output if a cog drives it, else
// 'undriven' (a pull-up or pull-down on the board).
bool SpiSram::level(const P1 &chip, int pin, bool undriven) const
{
    if (chip.pinDir >> pin & 1)
        return chip.pinOut >> pin & 1;
    return undriven;
}

void SpiSram::pins(P1 &chip, uint64_t when)
{
    bool sel = !level(chip, csPin, true);
    if (sel != selected) {
        selected = sel;
        if (selected) {
            state = COMMAND;
            shift = 0;
            bits = 0;
        } else {
            if (state == DATA_IN || state == DATA_OUT || state == MODE_IN || state == MODE_OUT)
                transfers++;
            state = IDLE;
        }
        sck = level(chip, sckPin, false);
        drive(chip, when);
        return;
    }
    bool clk = level(chip, sckPin, false);
    if (clk == sck)
        return;
    sck = clk;
    if (!selected)
        return;
    if (clk)
        risingEdge(level(chip, siPin, false));
    else
        fallingEdge();
    drive(chip, when);
}

void SpiSram::risingEdge(bool si)
{
    switch (state) {
    case COMMAND:
        shift = shift << 1 | si;
        if (++bits == 8) {
            command = (uint8_t)shift;
            shift = 0;
            bits = 0;
            if (command == READ || command == WRITE)
                state = ADDRESS;
            else if (command == WRMR)
                state = MODE_IN;
            else if (command == RDMR) {
                state = MODE_OUT;
                outByte = mode;
            } else
                state = IDLE;           // RSTIO and others: nothing until /CS rises
        }
        break;
    case ADDRESS:
        shift = shift << 1 | si;
        if (++bits == 24) {
            address = shift % SIZE;
            shift = 0;
            bits = 0;
            if (command == WRITE)
                state = DATA_IN;
            else {
                state = DATA_OUT;
                outByte = mem[address];
            }
        }
        break;
    case DATA_IN:
        shift = shift << 1 | si;
        if (++bits == 8) {
            mem[address] = (uint8_t)shift;
            next();
            shift = 0;
            bits = 0;
        }
        break;
    case MODE_IN:
        shift = shift << 1 | si;
        if (++bits == 8) {
            mode = (uint8_t)(shift & 0xC0);
            state = IDLE;
        }
        break;
    case DATA_OUT:
    case MODE_OUT:
        if (++bits == 8) {              // the master has sampled bit 0
            bits = 0;
            if (state == DATA_OUT) {
                next();
                outByte = mem[address];
            }
        }
        break;
    case IDLE:
        break;
    }
}

// SO changes after SCK falls: bit 7 of the byte first, from the falling
// edge that ends the address (or the command, for RDMR).
void SpiSram::fallingEdge()
{
    if (state == DATA_OUT || state == MODE_OUT)
        so = outByte >> (7 - bits) & 1;
}

void SpiSram::next()
{
    if (mode == MODE_SEQUENTIAL)
        address = (address + 1) % SIZE;
    else if (mode == MODE_PAGE)
        address = (address & ~31u) | ((address + 1) & 31u);
    // byte mode: the address stays (the chip ignores further bytes)
}

void SpiSram::drive(P1 &chip, uint64_t when)
{
    uint32_t bit = 1u << soPin;
    bool out = selected && (state == DATA_OUT || state == MODE_OUT) && so;
    uint32_t levels = out ? chip.inputs | bit : chip.inputs & ~bit;
    chip.setInputs(levels, when);
}

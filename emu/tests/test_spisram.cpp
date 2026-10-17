// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Cronomantic
// test_spisram.cpp -- Tests of the 23LC1024 model: the pins are driven
// from a cog's OUTA and DIRA, as an SPI master in mode 0 would.
#include <cstdio>
#include <cstring>
#include "../src/p1.h"
#include "../src/spisram.h"
#include "check.h"

static P1 chip;
static SpiSram sram;
static uint64_t t;

static const int CS = 0, SCK = 1, SI = 2, SO = 3;

static void onPins(P1 &c, uint64_t when, void *)
{
    sram.pins(c, when);
}

static void setPins(bool cs, bool sck, bool si)
{
    Cog &cog = chip.cogs[0];
    cog.ram[Cog::OUTA] = (uint32_t)cs << CS | (uint32_t)sck << SCK | (uint32_t)si << SI;
    t += 4;
    chip.refreshPins(t);
}

static bool so()
{
    return chip.ina() >> SO & 1;
}

// One byte each way, MSB first: SI set while SCK is low, both sides
// sample on the rising edge
static uint8_t spiByte(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        bool bit = out >> i & 1;
        setPins(false, false, bit);
        setPins(false, true, bit);
        in = (uint8_t)(in << 1 | so());
        setPins(false, false, bit);
    }
    return in;
}

static void select()
{
    setPins(true, false, false);
    setPins(false, false, false);
}

static void release()
{
    setPins(true, false, false);
}

static void command(uint8_t cmd, uint32_t addr)
{
    spiByte(cmd);
    spiByte((uint8_t)(addr >> 16));
    spiByte((uint8_t)(addr >> 8));
    spiByte((uint8_t)addr);
}

static void setup()
{
    chip.reset();
    Cog &cog = chip.cogs[0];
    cog.reset(0);
    cog.running = true;
    cog.ram[Cog::DIRA] = 1u << CS | 1u << SCK | 1u << SI;
    cog.ram[Cog::OUTA] = 1u << CS;
    t = 0;
    chip.refreshPins(t);
    memset(sram.mem, 0, sizeof(sram.mem));
    sram.attach(chip, CS, SCK, SI, SO);
    chip.pinHook = onPins;
    chip.pinCtx = nullptr;
}

static void testSequential()
{
    printf("SPI SRAM: sequential write and read, wrapping at 128 KB\n");
    setup();
    select();
    command(0x02, 0x1FFFE);
    const uint8_t data[4] = { 0x12, 0x34, 0xA5, 0x5A };
    for (uint8_t b : data)
        spiByte(b);
    release();
    CHECK(sram.mem[0x1FFFE] == 0x12 && sram.mem[0x1FFFF] == 0x34);
    CHECK(sram.mem[0] == 0xA5 && sram.mem[1] == 0x5A);

    select();
    command(0x03, 0x1FFFE);
    uint8_t got[4];
    for (uint8_t &b : got)
        b = spiByte(0);
    release();
    CHECK(memcmp(got, data, 4) == 0);
    CHECK(!so());                       // not driven with /CS high
}

static void testModes()
{
    printf("SPI SRAM: mode register, page mode wraps in 32 bytes\n");
    setup();
    select();
    spiByte(0x01);                      // WRMR: page mode
    spiByte(0x80);
    release();
    select();
    spiByte(0x05);                      // RDMR
    CHECK(spiByte(0) == 0x80);
    release();
    select();
    command(0x02, 0x3E);
    spiByte(1);
    spiByte(2);
    spiByte(3);
    release();
    CHECK(sram.mem[0x3E] == 1 && sram.mem[0x3F] == 2 && sram.mem[0x20] == 3);
    CHECK(sram.mem[0x40] == 0);
}

static void testCommandEndsAtCs()
{
    printf("SPI SRAM: bits after a mode write are ignored until /CS rises\n");
    setup();
    sram.mem[0x100] = 0x77;
    select();
    spiByte(0x01);                      // WRMR, sequential ...
    spiByte(0x40);
    command(0x03, 0x100);               // ... and READ without raising /CS
    CHECK(spiByte(0) == 0);             // SO not driven: ignored
    release();
    select();
    command(0x03, 0x100);
    CHECK(spiByte(0) == 0x77);
    release();
}

void runSpiSramTests()
{
    testSequential();
    testModes();
    testCommandEndsAtCs();
}

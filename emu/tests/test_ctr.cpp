// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Cronomantic
// test_ctr.cpp -- Tests of the P1 counters (CTRA/CTRB), against the
// behaviour of cog_ctr.v in the P1 Verilog (spinsim does not model them):
// PHS adds FRQ on every clock the counter triggers, bit 32 is the carry
// (DUTY output), bit 31 the NCO output, inputs are seen one clock late,
// edge modes compare the last two samples, logic modes index a truth
// table with (B, A).
#include <cstdio>
#include <cstring>
#include "../src/p1.h"
#include "check.h"

static P1 chip;

// Mode, PLLDIV, BPIN, APIN
static uint32_t ctrValue(int mode, int apin, int bpin = 0, int plldiv = 0)
{
    return ((uint32_t)mode << 26) | ((uint32_t)plldiv << 23) | ((uint32_t)bpin << 9) | (uint32_t)apin;
}

// A counter set up directly, starting to count at clock 0
static Counter counterAt0(int mode, uint32_t frq, int apin = 0, int bpin = 1, int plldiv = 0)
{
    Counter c;
    c.reset();
    c.ctr = ctrValue(mode, apin, bpin, plldiv);
    c.frq = frq;
    return c;
}

static void testFreeRunning()
{
    printf("Counters: NCO, DUTY, PLL and LOGIC always\n");

    // LOGIC always (%11111) with FRQ 1 counts clocks
    Counter c = counterAt0(31, 1);
    CHECK(c.phsAt(1000) == 1000);

    // NCO, FRQ = $2000_0000: PHS[31] has a period of 8 clocks, high 4
    c = counterAt0(4, 0x20000000, 3);
    int high = 0;
    bool pattern = true;
    for (uint64_t t = 1; t <= 64; t++) {
        bool out = c.outputsAt(t) & (1u << 3);
        high += out;
        pattern = pattern && out == (((t >> 2) & 1) != 0);
    }
    CHECK(high == 32);
    CHECK(pattern);

    // NCO differential: B is the inverse of A
    c = counterAt0(5, 0x20000000, 3, 4);
    CHECK((c.outputsAt(5) & 0x18) == 0x08);   // t = 5: PHS[31] = 1
    CHECK((c.outputsAt(9) & 0x18) == 0x10);

    // DUTY, FRQ = $4000_0000: a carry on every 4th clock -> 25 %
    c = counterAt0(6, 0x40000000, 2);
    high = 0;
    for (uint64_t t = 1; t <= 1000; t++)
        high += (c.outputsAt(t) >> 2) & 1;
    CHECK(high == 250);

    // PLL single: PLLDIV 3 gives the NCO frequency, PLLDIV 4 twice it
    Counter nco = counterAt0(4, 0x01000000, 1);
    Counter pll3 = counterAt0(2, 0x01000000, 1, 0, 3);
    Counter pll4 = counterAt0(2, 0x01000000, 1, 0, 4);
    int same = 0, edges3 = 0, edges4 = 0;
    bool p3 = false, p4 = false;
    for (uint64_t t = 1; t <= 4096; t++) {
        bool a = nco.outputsAt(t) & 2, b = pll3.outputsAt(t) & 2, d = pll4.outputsAt(t) & 2;
        same += a == b;
        edges3 += b != p3;
        edges4 += d != p4;
        p3 = b;
        p4 = d;
    }
    CHECK(same == 4096);
    CHECK(edges4 == 2 * edges3);

    // PHS keeps counting from a written value
    c = counterAt0(31, 3);
    c.write(2, 100, 0, 10);                   // PHS = 100 from clock 14
    CHECK(c.phsAt(14) == 100);
    CHECK(c.phsAt(20) == 118);
}

static void testInputs()
{
    printf("Counters: POS/NEG, edges and LOGIC modes follow the pins one clock late\n");

    // POS detector on pin 0: high from clock 100 to 150 -> 50 counts
    Counter c = counterAt0(8, 1, 0);
    c.pinsChanged(0x1, 100);
    CHECK(c.phsAt(120) == 19);                // seen from clock 101
    c.pinsChanged(0x0, 150);
    CHECK(c.phsAt(1000) == 50);

    // NEG detector: counts while low
    c = counterAt0(12, 1, 0);
    c.pinsChanged(0x1, 100);
    CHECK(c.phsAt(150) == 101);               // clocks 0..100 low

    // POSEDGE: five rising edges, FRQ 7
    c = counterAt0(10, 7, 0);
    for (int i = 0; i < 5; i++) {
        c.pinsChanged(0x1, 100 + 20 * (uint64_t)i);
        c.pinsChanged(0x0, 110 + 20 * (uint64_t)i);
    }
    CHECK(c.phsAt(1000) == 35);

    // NEGEDGE: the same pulses give five falling edges
    c = counterAt0(14, 1, 0);
    for (int i = 0; i < 5; i++) {
        c.pinsChanged(0x1, 100 + 20 * (uint64_t)i);
        c.pinsChanged(0x0, 110 + 20 * (uint64_t)i);
    }
    CHECK(c.phsAt(1000) == 5);

    // LOGIC A & !B (%10010): A = pin 0, B = pin 1
    c = counterAt0(18, 1, 0, 1);
    c.pinsChanged(0x1, 100);                  // A = 1, B = 0: counts
    c.pinsChanged(0x3, 200);                  // B = 1: stops
    c.pinsChanged(0x1, 300);                  // B = 0: counts again
    CHECK(c.phsAt(400) == 100 + 99);          // [101, 201) and [301, 400)

    // POS with feedback: B output = !A (delayed)
    c = counterAt0(9, 1, 0, 5);
    CHECK(c.outputsAt(10) == (1u << 5));
    c.pinsChanged(0x1, 20);
    CHECK(c.outputsAt(30) == 0);
}

// ---------------------------------------------------------------- programs

enum { IF_ALWAYS = 0xF };
static uint32_t I(int op, int d, int s, bool imm, bool wr = true)
{
    return ((uint32_t)op << 26) | (wr ? 1u << 23 : 0) | (imm ? 1u << 22 : 0) |
           ((uint32_t)IF_ALWAYS << 18) | ((uint32_t)(d & 0x1FF) << 9) | (uint32_t)(s & 0x1FF);
}
enum { MOV = 0x28, OR = 0x1A, ANDN = 0x19, ADD = 0x20, JMP = 0x17 };
static const uint32_t NOP = 0;

static void testPrograms()
{
    printf("Counters: written and read by PASM (timing of CTR/PHS accesses)\n");
    // Results in 40-43: phsa just after the counter starts; phsb after
    // writing 100; phsb after "add phsb,#0" (PHSB as a destination reads
    // the shadow); a POS detector on pin 1 held high for 12 clocks.
    const int R0 = 40, CK = 50, POS = 51;
    const uint32_t code[] = {
        I(MOV, Cog::FRQA, 1, true),             // 0
        I(MOV, Cog::CTRA, CK, false),           // 1: logic always, from t1+4
        I(MOV, R0, Cog::PHSA, false),           // 2: reads at t1+5 -> 1
        I(MOV, Cog::FRQB, 1, true),             // 3
        I(MOV, Cog::CTRB, CK, false),           // 4
        I(MOV, Cog::PHSB, 100, true),           // 5: 100 from t5+4
        I(MOV, R0 + 1, Cog::PHSB, false),       // 6: read at t5+5 -> 101
        I(ADD, Cog::PHSB, 0, true),             // 7: shadow 100 -> 100 again
        I(MOV, R0 + 2, Cog::PHSB, false),       // 8: -> 101
        I(MOV, Cog::CTRA, 0, true),             // 9
        I(MOV, Cog::PHSA, 0, true),             // 10
        I(MOV, Cog::CTRA, POS, false),          // 11: POS detector, pin 1
        I(OR, Cog::DIRA, 2, true),              // 12
        I(OR, Cog::OUTA, 2, true),              // 13: high at t13
        NOP, NOP,                               // 14, 15
        I(ANDN, Cog::OUTA, 2, true),            // 16: low at t13+12
        NOP, NOP,                               // 17, 18
        I(MOV, R0 + 3, Cog::PHSA, false),       // 19
        I(JMP, 0, 20, true, false),             // 20: loop
    };
    chip.reset();
    for (int i = 0; i < 21; i++)
        chip.wrlong((uint16_t)(4 * i), code[i]);
    chip.wrlong(4 * CK, ctrValue(31, 0));
    chip.wrlong(4 * POS, ctrValue(8, 1));
    chip.coginit(0, 0, 0, 0);
    chip.run(chip.cogs[0].time + 400);
    Cog &cog = chip.cogs[0];
    CHECK(cog.ram[R0] == 1);
    CHECK(cog.ram[R0 + 1] == 101);
    CHECK(cog.ram[R0 + 2] == 101);
    CHECK(cog.ram[R0 + 3] == 12);

    // Counter outputs reach INA when DIRA has the pin; they stop with the cog
    chip.reset();
    const uint32_t prog[] = {
        I(MOV, Cog::FRQA, 0, false),            // 0: frqa = [5]
        I(MOV, Cog::CTRA, 6, false),            // 1: ctra = [6]
        I(OR, Cog::DIRA, 8, true),              // 2
        I(JMP, 0, 3, true, false),              // 3: loop
        0, 0x80000000u, ctrValue(4, 3),         // 4, 5, 6
    };
    for (int i = 0; i < 7; i++)
        chip.wrlong((uint16_t)(4 * i), prog[i]);
    chip.coginit(0, 0, 0, 0);
    // FRQ $8000_0000: PHS[31] toggles every clock
    chip.run(chip.cogs[0].time + 100);
    uint32_t a = chip.ina() & 8;
    chip.run(chip.clock + 1);
    uint32_t b = chip.ina() & 8;
    CHECK(a != b);
    chip.cogstop(0);
    chip.run(chip.clock + 1);
    CHECK((chip.ina() & 8) == 0);
}

void runCounterTests()
{
    testFreeRunning();
    testInputs();
    testPrograms();
}

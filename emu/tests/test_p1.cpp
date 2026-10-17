// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Cronomantic
// test_p1.cpp -- Tests of the Propeller P1 emulation.
//
// 1. Instruction vectors generated with spinsim (emu/tools/gen_pasm_vectors.py):
//    every ALU instruction, the jumps and the hub reads, with their flags.
// 2. Hand-written checks of what the vectors do not cover: pipeline,
//    timing, CALL/RET, cogs, locks, pins.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../src/p1.h"
#include "check.h"
#include "pasm_vectors.inc"

static const char *const OP_NAMES[64] = {
    "RDBYTE", "RDWORD", "RDLONG", "HUBOP", "?04", "?05", "?06", "?07",
    "ROR", "ROL", "SHR", "SHL", "RCR", "RCL", "SAR", "REV",
    "MINS", "MAXS", "MIN", "MAX", "MOVS", "MOVD", "MOVI", "JMPRET",
    "AND", "ANDN", "OR", "XOR", "MUXC", "MUXNC", "MUXZ", "MUXNZ",
    "ADD", "SUB", "ADDABS", "SUBABS", "SUMC", "SUMNC", "SUMZ", "SUMNZ",
    "MOV", "NEG", "ABS", "ABSNEG", "NEGC", "NEGNC", "NEGZ", "NEGNZ",
    "CMPS", "CMPSX", "ADDX", "SUBX", "ADDS", "SUBS", "ADDSX", "SUBSX",
    "CMPSUB", "DJNZ", "TJNZ", "TJZ", "WAITPEQ", "WAITPNE", "WAITCNT", "WAITVID",
};

static P1 chip;

static void testVectors()
{
    printf("P1 instructions against spinsim (%d vectors)\n",
           (int)(sizeof(VECTORS) / sizeof(VECTORS[0])));
    const uint16_t dataBase = 0x0100;
    int bad[64];
    memset(bad, 0, sizeof(bad));
    for (unsigned n = 0; n < sizeof(VECTORS) / sizeof(VECTORS[0]); n++) {
        const uint32_t *v = VECTORS[n];
        chip.reset();
        memcpy(chip.hub + dataBase, VEC_DATA, sizeof(VEC_DATA));
        Cog &cog = chip.cogs[0];
        cog.running = true;

        uint32_t ins = v[0];
        uint32_t s = v[2];
        if (v[3] & 16)
            s += dataBase;                          // hub data address
        if (v[3] & 8)
            ins = (ins & ~0x1FFu) | VEC_SREG;        // register source
        if (v[3] & 4)
            ins = (ins & ~0x1FFu) | VEC_AFTER;       // jump target: next
        ins = (ins & ~(0x1FFu << 9)) | ((uint32_t)VEC_DREG << 9);

        cog.ram[VEC_DREG] = v[1];
        cog.ram[VEC_SREG] = s;
        cog.ram[VEC_EXEC] = ins;
        cog.c = v[3] & 1;
        cog.z = (v[3] & 2) != 0;
        cog.pc = (uint16_t)VEC_EXEC;
        cog.ir = ins;
        cog.execute(chip);

        uint32_t d = cog.ram[VEC_DREG];
        uint32_t f = (cog.c ? 1u : 0u) | (cog.z ? 2u : 0u);

        // Known spinsim slip: CMPSX with S = $7FFFFFFF and C = 1 computes
        // S + C in 32 bits ($80000000, negative), so "D < S + C" comes out
        // false.  The chip subtracts in 33 bits (cog_alu.v): C = 1 for any
        // negative D.  The emulator follows the chip; check that instead.
        if ((v[0] >> 26) == 0x31 && s == 0x7FFFFFFF && (v[3] & 1) && (v[1] >> 31)) {
            checks++;
            if (d != v[4] || !cog.c) {
                failures++;
                printf("  FAIL CMPSX overflow case: got %08X C=%d\n", d, cog.c);
            }
            continue;
        }
        checks++;
        if (d != v[4] || f != v[5]) {
            failures++;
            int op = (int)(v[0] >> 26);
            if (bad[op]++ < (getenv("VEC_ALL") ? 1000 : 2))
                printf("  FAIL %-7s D=%08X S=%08X C=%u Z=%u: got %08X C=%u Z=%u, spinsim %08X C=%u Z=%u\n",
                       OP_NAMES[op], v[1], s, v[3] & 1, (v[3] >> 1) & 1,
                       d, f & 1, f >> 1, v[4], v[5] & 1, v[5] >> 1);
        }
    }
}

// ---------------------------------------------------------------- assembler

enum { IF_ALWAYS = 0xF, IF_C = 0xC, IF_NC = 0x3, IF_Z = 0xA, IF_NZ = 0x5, IF_NEVER = 0 };
enum { WZ = 1, WC = 2, NR = 4, IMM = 8 };

// op: opcode; flags: WZ/WC/NR/IMM; the R bit is set unless NR
static uint32_t I(int op, int d, int s, int flags = 0, int cond = IF_ALWAYS)
{
    bool wr = !(flags & NR);
    return ((uint32_t)op << 26) | ((flags & WZ) ? 1u << 25 : 0) | ((flags & WC) ? 1u << 24 : 0) |
           (wr ? 1u << 23 : 0) | ((flags & IMM) ? 1u << 22 : 0) | ((uint32_t)cond << 18) |
           ((uint32_t)(d & 0x1FF) << 9) | (uint32_t)(s & 0x1FF);
}

enum {
    RDLONG = 0x02, HUBOP = 0x03, MOVS = 0x14, JMPRET = 0x17, ADD = 0x20, SUB = 0x21,
    MOV = 0x28, DJNZ = 0x39, WAITPEQ = 0x3C, WAITCNT = 0x3E, OR = 0x1A
};
static const uint32_t NOP = 0;

// Write a program into hub RAM at 'addr' and start it in cog 'id'
static void load(uint16_t addr, const uint32_t *code, int n)
{
    for (int i = 0; i < n; i++)
        chip.wrlong((uint16_t)(addr + 4 * i), code[i]);
}

// Run a program in cog 0 from hub $0000 until 'clocks' after it starts
static Cog &runProgram(const uint32_t *code, int n, uint64_t clocks, uint16_t par = 0)
{
    chip.reset();
    load(0, code, n);
    chip.coginit(0, 0, par, 0);
    chip.run(chip.cogs[0].time + clocks);
    return chip.cogs[0];
}

static void testPipeline()
{
    printf("P1 pipeline: the next instruction is fetched before the result is written\n");
    // 0: movs 1, #5      changes instruction 1, which is already fetched
    // 1: mov 20, #1
    // 2: movs 4, #7      changes instruction 4, two ahead
    // 3: nop
    // 4: mov 21, #2
    // 5: jmp #5
    const uint32_t code[] = {
        I(MOVS, 1, 5, IMM), I(MOV, 20, 1, IMM), I(MOVS, 4, 7, IMM), NOP,
        I(MOV, 21, 2, IMM), I(JMPRET, 0, 5, IMM | NR),
    };
    Cog &cog = runProgram(code, 6, 100);
    CHECK(cog.ram[20] == 1);            // old instruction executed
    CHECK(cog.ram[21] == 7);            // modified one executed
}

static void testCallReturn()
{
    printf("P1 CALL/RET with JMPRET\n");
    // 0: jmpret 4, #3    call sub (return address into 4's S field)
    // 1: mov 20, #9
    // 2: jmp #2
    // 3: mov 21, #5      sub
    // 4: jmp #0          sub_ret (S field patched)
    const uint32_t code[] = {
        I(JMPRET, 4, 3, IMM), I(MOV, 20, 9, IMM), I(JMPRET, 0, 2, IMM | NR),
        I(MOV, 21, 5, IMM), I(JMPRET, 0, 0, IMM | NR),
    };
    Cog &cog = runProgram(code, 5, 100);
    CHECK(cog.ram[21] == 5);
    CHECK(cog.ram[20] == 9);
    CHECK((cog.ram[4] & 0x1FF) == 1);
}

static void testTiming()
{
    printf("P1 timing: 4 clocks, DJNZ 4/8, hub windows 8-23\n");
    chip.reset();
    Cog &cog = chip.cogs[0];
    cog.running = true;
    cog.time = 0;

    cog.pc = 0;
    cog.ram[0] = I(MOV, 10, 1, IMM);
    cog.ir = cog.ram[0];
    CHECK(cog.execute(chip) == 4);

    cog.ram[10] = 2;
    cog.pc = 1;
    cog.ram[1] = I(DJNZ, 10, 1, IMM);
    cog.ir = cog.ram[1];
    CHECK(cog.execute(chip) == 4);      // 2 -> 1: jumps
    CHECK(cog.execute(chip) == 8);      // 1 -> 0: falls through

    // RDLONG: cog 0's window is at clocks 0, 16, 32...
    cog.ram[2] = I(RDLONG, 11, 12, 0);
    cog.ram[12] = 0x100;
    cog.pc = 2;
    cog.ir = cog.ram[2];
    cog.time = 32;
    CHECK(cog.execute(chip) == 8);      // on its window
    cog.pc = 2;
    cog.ir = cog.ram[2];
    cog.time = 33;
    CHECK(cog.execute(chip) == 23);     // just missed it
    cog.pc = 2;
    cog.ir = cog.ram[2];
    cog.time = 48 + 8;                  // 8 clocks after a window
    CHECK(cog.execute(chip) == 16);
    // cog 3's window is 6 clocks later
    CHECK(P1::hubWindow(3, 32) == 38);

    // A false condition takes 4 clocks, even for a hub instruction
    cog.pc = 3;
    cog.ram[3] = I(RDLONG, 11, 12, 0, IF_NEVER);
    cog.ir = cog.ram[3];
    CHECK(cog.execute(chip) == 4);
}

static void testWaitcnt()
{
    printf("P1 WAITCNT: mov t,cnt / add t,#9 / waitcnt is the shortest delay\n");
    for (int delay = 8; delay <= 9; delay++) {
        const uint32_t code[] = {
            I(MOV, 20, Cog::CNT), I(ADD, 20, delay, IMM), I(WAITCNT, 20, 0, IMM),
            I(MOV, 21, 1, IMM), I(JMPRET, 0, 4, IMM | NR),
        };
        Cog &cog = runProgram(code, 5, 1000);
        if (delay == 9)
            CHECK(cog.ram[21] == 1);    // done within 1000 clocks
        else
            CHECK(cog.ram[21] == 0);    // target passed: waits for the wrap
    }
}

static void testHubCogsLocks()
{
    printf("P1 COGINIT, COGID, PAR, locks\n");
    // Cog 0 starts the code at $0100 in a free cog with PAR = $0200; that
    // cog writes its id at PAR.  Then cog 0 takes a lock twice.
    const uint32_t main[] = {
        I(MOV, 20, 21),                 // 0: 20 = coginit value
        I(HUBOP, 20, 2, IMM | WC),      // 1: coginit
        I(HUBOP, 22, 4, IMM | WC),      // 2: locknew -> 22
        I(HUBOP, 22, 6, IMM | WC | NR), // 3: lockset: C = 0
        I(MOV, 23, 0, IMM),             // 4
        I(0x1C, 23, 1, IMM),            // 5: muxc 23, #1
        I(HUBOP, 22, 6, IMM | WC | NR), // 6: lockset again: C = 1
        I(0x1C, 23, 2, IMM),            // 7: muxc 23, #2
        I(JMPRET, 0, 8, IMM | NR),      // 8: jmp #8
    };
    // par $0200 -> bits 31..18 = $0200 >> 2, code $0100 -> bits 17..4, bit 3 = new cog
    uint32_t initValue = ((0x0200u >> 2) << 18) | ((0x0100u >> 2) << 4) | 8;
    const uint32_t child[] = {
        I(HUBOP, 20, 1, IMM),           // cogid 20
        I(MOV, 21, Cog::PAR),           // 21 = par
        I(0x00, 20, 21, NR),            // wrbyte 20, 21: hub[par] = id
        I(JMPRET, 0, 3, IMM | NR),
    };
    chip.reset();
    load(0, main, 9);
    load(0x0100, child, 4);
    chip.coginit(0, 0, 0, 0);
    chip.cogs[0].ram[21] = initValue;   // after loading: patch the constant
    chip.run(chip.cogs[0].time + 20000);
    CHECK(chip.cogs[1].running);
    CHECK(chip.hub[0x0200] == 1);       // child is cog 1
    CHECK(chip.cogs[0].ram[22] == 0);   // first lock
    CHECK((chip.cogs[0].ram[23] & 3) == 2);   // first C = 0, second C = 1
}

static void testPins()
{
    printf("P1 pins: outputs ORed, inputs when undriven, WAITPEQ, shadow INA\n");
    chip.reset();
    // cog 1 drives P4 high after a while; cog 0 waits for it
    const uint32_t waiter[] = {
        I(WAITPEQ, 20, 21),             // waitpeq 20, 21 (D = mask, S = mask)
        I(MOV, 22, Cog::CNT),
        I(JMPRET, 0, 2, IMM | NR),
    };
    const uint32_t driver[] = {
        I(MOV, 20, Cog::CNT),
        I(ADD, 20, 400, IMM),
        I(WAITCNT, 20, 0, IMM),
        I(OR, Cog::DIRA, 16, IMM),
        I(OR, Cog::OUTA, 16, IMM),
        I(JMPRET, 0, 5, IMM | NR),
    };
    load(0, waiter, 3);
    load(0x0100, driver, 6);
    chip.coginit(0, 0, 0, 0);
    chip.cogs[0].ram[20] = 16;
    chip.cogs[0].ram[21] = 16;
    chip.coginit(1, 0x0100, 0, 100);
    uint64_t start = chip.cogs[1].time;
    chip.run(start + 1000);
    CHECK((chip.ina() & 16) != 0);
    CHECK(chip.cogs[0].ram[22] != 0);                       // released
    CHECK(chip.cogs[0].ram[22] >= (uint32_t)(start + 400)); // not before

    // External input on an undriven pin; a driven-low pin wins over it
    chip.setInputs(0x3, chip.clock);
    chip.cogs[1].ram[Cog::DIRA] |= 2;                       // P1 driven low by cog 1
    chip.refreshPins(chip.clock);
    CHECK((chip.ina() & 3) == 1);

    // INA as a destination reads the shadow register, not the pins
    Cog &cog = chip.cogs[2];
    cog.reset(2);
    cog.running = true;
    cog.ram[0] = I(MOV, Cog::INA, 7, IMM);                  // shadow = 7
    cog.ram[1] = I(0x21, Cog::INA, 7, IMM | WZ | NR);       // cmp ina, #7 wz
    cog.ram[2] = I(MOV, 30, Cog::INA);                      // 30 = pins
    cog.pc = 0;
    cog.ir = cog.ram[0];
    cog.time = chip.clock + 10;                             // after the pin changes above
    cog.execute(chip);
    cog.execute(chip);
    CHECK(cog.z);
    cog.execute(chip);
    CHECK(cog.ram[30] == chip.ina());

    // A pin written by an instruction starting at t is read through INA
    // from t + 2 on (cog.v: INA is latched at the end of the third clock,
    // OUTA at the end of the fourth), whichever cog has the lower number
    for (int order = 0; order < 2; order++) {
        Cog &w = chip.cogs[order ? 5 : 4];               // writer
        Cog &r = chip.cogs[order ? 4 : 5];               // reader
        for (int delay = 1; delay <= 2; delay++) {
            w.reset(w.id);
            r.reset(r.id);
            w.running = r.running = true;
            w.ram[Cog::DIRA] = 1u << 9;
            w.ram[0] = I(MOV, Cog::OUTA, 0, IMM);           // P9 low
            w.ram[1] = I(OR, Cog::OUTA, 2);                 // or outa, ram[2]: P9 high
            w.ram[2] = 1u << 9;
            r.ram[0] = I(MOV, 30, Cog::INA);
            w.pc = 0;
            w.ir = w.ram[0];
            r.pc = 0;
            r.ir = r.ram[0];
            uint64_t t = chip.clock + 100;
            w.time = t;
            chip.stepCog(w);                                // P9 low at t
            w.time = t + 8;
            chip.stepCog(w);                                // P9 high at t + 8
            r.time = t + 8 + (uint64_t)delay;
            chip.stepCog(r);
            bool seen = (r.ram[30] >> 9) & 1;
            CHECK(seen == (delay == 2));
            w.running = r.running = false;
            w.ram[Cog::DIRA] = 0;
            chip.refreshPins(chip.clock);
        }
    }
}

void runP1Tests()
{
    testVectors();
    testPipeline();
    testCallReturn();
    testTiming();
    testWaitcnt();
    testHubCogsLocks();
    testPins();
}

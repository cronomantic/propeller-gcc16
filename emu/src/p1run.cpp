// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Cronomantic
// p1run.cpp -- Run a Spin program on one emulated Propeller P1 and print
// what it sends on its serial port (P30), like spinsim -b.
//
//   p1run program.binary [--baud 115200] [--seconds 10] [--rom path] [--until text]
//         [--spi-sram image]
//
// The program boots as on the chip (ROM boot loader steps, then the Spin
// interpreter from the ROM).  It stops when every cog has stopped, when
// its output contains the --until text, or after the given time, counted
// at the program's own clkfreq (long 0 of the image; 80 MHz if it is 0).
// The ROM comes from --rom, $P1ROM, or ../rom/p1rom.bin next to the
// program (emu/rom/p1rom.bin, see emu/rom/README.md).  --spi-sram puts a
// 23LC1024 SPI SRAM on P0 (/CS), P1 (SCK), P2 (SI) and P3 (SO), loaded
// with the image from address 0: the external memory of the XMM tests
// (tests/xmm).
// Exit status: 0, 1 for a usage or file error, 2 if --until was given
// and its text never came.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "p1.h"
#include "spisram.h"
#include "uart.h"

static Uart uart;
static SpiSram sram;

static void onPins(P1 &chip, uint64_t when, void *)
{
    Uart::onPins(chip, when, &uart);
    sram.pins(chip, when);
}

static bool loadRom(P1 &chip, const char *path, const char *argv0)
{
    if (path)
        return chip.loadRom(path);
    const char *env = getenv("P1ROM");
    if (env && *env)
        return chip.loadRom(env);
    // emu/build/p1run -> emu/rom/p1rom.bin
    static char buf[4096];
    const char *slash = strrchr(argv0, '/');
    const char *bslash = strrchr(argv0, '\\');
    if (bslash && (!slash || bslash > slash))
        slash = bslash;
    int dir = slash ? (int)(slash - argv0) + 1 : 0;
    snprintf(buf, sizeof(buf), "%.*s../rom/p1rom.bin", dir, argv0);
    return chip.loadRom(buf) || chip.loadRom("emu/rom/p1rom.bin") || chip.loadRom("rom/p1rom.bin");
}

int main(int argc, char *argv[])
{
    const char *program = nullptr, *rom = nullptr, *until = nullptr, *sramImage = nullptr;
    uint32_t baud = 115200;
    double seconds = 10.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--baud") && i + 1 < argc)
            baud = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rom") && i + 1 < argc)
            rom = argv[++i];
        else if (!strcmp(argv[i], "--until") && i + 1 < argc)
            until = argv[++i];
        else if (!strcmp(argv[i], "--spi-sram") && i + 1 < argc)
            sramImage = argv[++i];
        else if (argv[i][0] != '-' && !program)
            program = argv[i];
        else {
            program = nullptr;
            break;
        }
    }
    if (!program) {
        fprintf(stderr, "usage: p1run program.binary [--baud 115200] [--seconds 10] [--rom path] "
                        "[--until text] [--spi-sram image]\n");
        return 1;
    }

    static P1 chip;
    if (!loadRom(chip, rom, argv[0])) {
        fprintf(stderr, "no P1 ROM: give --rom or set P1ROM (see emu/rom/README.md)\n");
        return 1;
    }
    if (!chip.bootFile(program, 0)) {
        fprintf(stderr, "cannot boot %s\n", program);
        return 1;
    }
    uint32_t clkfreq = chip.rdlong(0);
    if (!clkfreq)
        clkfreq = 80000000;

    uart.attach(chip, 30, clkfreq, baud);
    if (sramImage) {
        if (!sram.load(sramImage)) {
            fprintf(stderr, "cannot load %s into the %u KB SPI SRAM\n", sramImage,
                    SpiSram::SIZE / 1024);
            return 1;
        }
        sram.attach(chip, 0, 1, 2, 3);
        chip.pinHook = onPins;
    }

    uint64_t end = (uint64_t)(seconds * clkfreq);
    uint64_t step = clkfreq / 1000;             // 1 ms
    uint64_t t = 0;
    bool found = false;
    while (t < end && chip.anyRunning() && !found) {
        t += step;
        chip.run(t);
        uart.flush(t);
        int n;
        const char *s = uart.take(&n);
        fwrite(s, 1, (size_t)n, stdout);
        fflush(stdout);
        found = until && strstr(uart.text, until);
    }
    if (!found) {
        uart.flush(t + step);
        int n;
        const char *s = uart.take(&n);
        fwrite(s, 1, (size_t)n, stdout);
        found = until && strstr(uart.text, until);
    }
    fprintf(stderr, "\n[p1run: %.3f s emulated, %s]\n", (double)t / clkfreq,
            found ? "text found" : chip.anyRunning() ? "time limit" : "all cogs stopped");
    return until && !found ? 2 : 0;
}

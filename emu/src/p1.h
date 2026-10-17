// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Cronomantic
// p1.h -- Parallax Propeller P1 (P8X32A), emulated instruction by
// instruction: 8 cogs running PASM, the hub (RAM, locks, round-robin
// access windows) and the I/O pins.
//
// Timing model (clocks, from the Propeller manual):
//   - ordinary instructions 4; an instruction whose condition is false 4;
//   - hub instructions 8 to 23: each cog gets a hub window every 16
//     clocks (cog k at clocks 2k mod 16), the instruction ends 8 clocks
//     after its window;
//   - DJNZ, TJNZ, TJZ: 4 when they jump, 8 when they do not;
//   - WAITCNT, WAITPEQ, WAITPNE: 6 at least; the condition is checked from
//     one clock after the start and the instruction ends 5 clocks after it
//     holds (so "mov t,cnt / add t,#9 / waitcnt t,#0" is the shortest delay
//     that does not wrap, as on the real chip).
// Pipeline: the instruction that follows is fetched before the current
// one writes its result, so code that modifies the very next instruction
// sees the old one (on the chip one instruction must sit in between).
//
// ROM: loaded from a file (emu/rom/p1rom.bin); the Spin interpreter and
// the boot loader in it are scrambled, and a cog loading code from ROM
// addresses gets it unscrambled, as in the hub (hub.v).  bootBinary() does
// what the boot loader does after loading a program, then starts the Spin
// interpreter.
//
// Counters (CTRA/CTRB): every mode of cog_ctr.v, the P1 Verilog.  They
// are computed lazily: between two changes of the pins a counter evolves
// predictably, so PHS and the outputs are worked out when someone reads
// them, and input edges are taken at the pin change events.  Limits:
// counter outputs are seen by INA (and so by every cog) but do not wake
// WAITPEQ/WAITPNE, do not reach the pin hook, and do not feed other
// counters' inputs.
//
// Not emulated yet: the video generator (WAITVID returns at once), CLKSET.
#pragma once
#include <cstdint>

class P1;

// One counter (cog_ctr.v).  PHS is 33 bits: each clock the counter
// triggers, PHS[31:0] + FRQ is added and bit 32 keeps the carry (the DUTY
// output).  Inputs go through one clock of delay (dly); edge modes
// compare the last two samples.
struct Counter {
    uint32_t ctr, frq, phs;
    uint64_t last;          // phs is valid at this clock
    uint64_t phase;         // PHS with the bits above 31, for the PLL
    bool a, b;              // input levels as seen by the counter

    void reset();
    int mode() const { return (int)((ctr >> 26) & 31); }
    int apin() const { return (int)(ctr & 31); }
    int bpin() const { return (int)((ctr >> 9) & 31); }
    bool drivesPins() const;            // NCO, DUTY, PLL, feedback modes
    bool usesInputs() const { return mode() >= 8; }

    uint32_t phsAt(uint64_t t) const;
    // PHS with the bits above 31 (PLL, NCO, DUTY): in DUTY mode, the upper
    // half counts the carries, i.e. the clocks the A pin was high.
    uint64_t phaseAt(uint64_t t) const { return t > last ? phase + (t - last) * (uint64_t)frq : phase; }
    uint32_t outputsAt(uint64_t t) const;   // pins driven high at clock t
    void advance(uint64_t t);
    void pinsChanged(uint32_t pins, uint64_t t);
    void write(int reg, uint32_t value, uint32_t pins, uint64_t t);
};

// One frame of a cog's video generator (WAITVID): from 'start', in
// 1/65536 clocks, FrameClocks (VSCL[11:0], 0 = 4096) cycles of the video
// clock, each 'period' 1/65536 clocks long; a new pixel every PixelClocks
// (VSCL[19:12], 0 = 256) cycles.  Pixel i takes pixels[2i+1:2i] (4-colour
// mode, VCFG[28]) or pixels[i] (2-colour) as an index into the 4 colour
// bytes of 'colors'; past the 16th (32nd) pixel the last one repeats, as
// the shifter in cog_vid.v does.
struct VideoFrame {
    uint64_t start;
    uint64_t period;
    uint32_t vcfg, vscl;
    uint32_t colors, pixels;

    int frameClocks() const { int n = (int)(vscl & 0xFFF); return n ? n : 4096; }
    int pixelClocks() const { int n = (int)((vscl >> 12) & 0xFF); return n ? n : 256; }
    uint8_t color(int pixel) const;     // colour byte of pixel i
};

class Cog {
public:
    static constexpr int RAM_LONGS = 512;
    enum Reg {
        PAR = 0x1F0, CNT, INA, INB, OUTA, OUTB, DIRA, DIRB,
        CTRA, CTRB, FRQA, FRQB, PHSA, PHSB, VCFG, VSCL
    };

    uint32_t ram[RAM_LONGS];
    Counter counter[2];     // CTRA, CTRB
    uint32_t par;
    uint16_t pc;
    uint32_t ir;            // instruction at pc, already fetched
    bool c, z;
    bool running;
    bool waiting;           // stalled in WAITPEQ/WAITPNE
    uint32_t waitVersion;   // pin version seen when it stalled
    bool pinWrite;          // the last instruction wrote OUTA or DIRA
    int id;
    uint64_t time;          // clock at which the current instruction starts

    // Video generator (WAITVID; clocked by counter A's PLL)
    bool vidOn;
    uint64_t vidNext;       // next frame boundary, 1/65536 clocks
    uint32_t vidColors, vidPixels;
    uint32_t vidLate;       // frames the cog missed (the generator repeated one)

    void reset(int cogId);
    void start(P1 &chip, uint16_t codeAddr, uint32_t parValue, uint64_t when);

    // Execute (or keep waiting in) the current instruction.  A cog whose
    // WAITPEQ/WAITPNE does not hold stays waiting until the pins change
    // (see P1::pinsChanged).
    void step(P1 &chip);

    // Execute one instruction at once, outside any timing (tests): returns
    // the clocks it took.
    uint64_t execute(P1 &chip);

    // The current instruction only touches the cog's own state: no hub,
    // no pins, no counters (see P1::runCog).
    bool isLocal() const;

private:
    uint32_t readSource(P1 &chip, int s) const;
    uint64_t waitvid(P1 &chip, uint32_t colors, uint32_t pixels);
    void resetVideo();
    void writeDest(P1 &chip, int d, uint32_t value);
    uint64_t run(P1 &chip, bool *stalled);
};

class P1 {
public:
    static constexpr uint32_t HUB_RAM = 0x8000;

    uint8_t hub[0x10000];       // RAM $0000-$7FFF, ROM $8000-$FFFF (empty)
    Cog cogs[8];
    uint64_t clock;
    uint32_t inputs;            // levels of pins not driven by any cog
    uint32_t pinVersion;        // changes whenever the pin levels may change
    uint32_t pinOut, pinDir;    // outputs() and directions(), kept up to date
    uint32_t cogChanges;        // counts COGINIT/COGSTOP

    // Called when the pins driven by the cogs change (e.g. a serial port
    // decoder watching P30).
    typedef void (*PinHook)(P1 &chip, uint64_t when, void *ctx);
    PinHook pinHook;
    void *pinCtx;
    // Called for every frame a video generator starts (in time order per cog)
    typedef void (*VideoHook)(P1 &chip, int cog, const VideoFrame &frame, void *ctx);
    VideoHook videoHook;
    void *videoCtx;
    bool lockUsed[8];
    bool lockState[8];

    P1();
    void reset();                   // RAM cleared, cogs stopped, ROM kept

    // ROM image: 32 KB, hub addresses $8000-$FFFF.
    bool loadRom(const char *path);
    bool loadRomImage(const uint8_t *data, uint32_t size);
    bool hasRom() const { return romLoaded; }

    // Load a Spin program (.binary or 32 KB .eeprom image) into RAM and
    // start the Spin interpreter in cog 0, as the boot loader does.
    bool bootBinary(const uint8_t *data, uint32_t size, uint64_t when);
    bool bootFile(const char *path, uint64_t when);

    uint32_t ina() const;
    // INA as read by an instruction that starts at clock t (a source
    // operand): cog.v latches it at the end of the instruction's third
    // clock, and an OUTA/DIRA write reaches the pins at the end of the
    // writing instruction's fourth clock, so a change made by an
    // instruction starting at t0 is seen from t0 + 2 on (whatever the
    // cogs' numbers).  WAITPxx sees it at once (see pinsChanged).
    uint32_t inaAt(uint64_t t) const;
    uint32_t pinLevels() const { return pinOut | (inputs & ~pinDir); }   // without counters
    uint32_t counterOutputs(uint64_t t) const;
    // The DUTY counter (mode 6 or 7) that drives 'pin' as its A pin, with
    // the pin an output: its phase at clock t (see Counter::phaseAt) and
    // an id that changes when another counter takes over.  For audio.
    bool dutyPhase(int pin, uint64_t t, uint64_t &phase, int &id) const;
    void countersChanged();         // a CTR register was written
    uint32_t outputs() const;   // pins driven high (computed from the cogs)
    uint32_t directions() const;
    // Recompute the pin caches (after OUTA/DIRA changed from outside);
    // returns true and wakes waiting cogs if the pins changed.
    bool refreshPins(uint64_t when);

    uint8_t  rdbyte(uint16_t a) const { return hub[a]; }
    uint16_t rdword(uint16_t a) const;
    uint32_t rdlong(uint16_t a) const;
    void wrbyte(uint16_t a, uint8_t v);
    void wrword(uint16_t a, uint16_t v);
    void wrlong(uint16_t a, uint32_t v);

    // COGINIT: id 0-7, or -1 for the first free cog.  code and par are hub
    // addresses (long aligned).  Returns the cog started, or -1.
    int coginit(int id, uint16_t code, uint16_t par, uint64_t when);
    void cogstop(int id);

    // Clock of the next hub window of cog id at or after t.
    static uint64_t hubWindow(int id, uint64_t t);

    // The cog whose instruction starts first, skipping cogs that wait for
    // pins that have not changed since; null if none.
    Cog *earliest();
    // Step one cog; wakes the waiting cogs and returns true when it
    // changed the pins.
    bool stepCog(Cog &cog);
    // Step 'cog' while it is the first to run: until its time reaches
    // 'limit', it waits, or it changes the pins or the running cogs.
    // Past 'limit' (and up to 'until') it goes on with instructions that
    // only touch its own state, which nothing else can see: what other
    // cogs and chips observe still happens in time order.  Returns true if
    // the pins changed.
    bool runCog(Cog &cog, uint64_t limit, uint64_t until);
    // Run this chip's cogs in time order (runCog on the first one, up to
    // the second) while the first starts before 'limit'.  Returns true as
    // soon as the pins changed.
    bool runChip(uint64_t limit, uint64_t until);
    // The second-earliest eligible start time, other than 'cog'.
    uint64_t earliestOther(const Cog *cog) const;
    // The outside world changed the levels of undriven pins at 'when'.
    void setInputs(uint32_t levels, uint64_t when);
    // Wake the cogs waiting for pins, from clock 'when' on.
    void pinsChanged(uint64_t when);

    // Run every cog until clock 'until'.
    void run(uint64_t until);

    bool anyRunning() const;

private:
    // The last pin changes (time of the instruction, levels before, after)
    static constexpr int HISTORY = 8;
    uint64_t histTime[HISTORY];
    uint32_t histBefore[HISTORY], histAfter[HISTORY];
    int histNewest, histCount;
    uint32_t lastLevels;

    bool romLoaded;
    bool counterPins;               // some counter drives pins
};

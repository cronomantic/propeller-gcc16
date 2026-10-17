// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Cronomantic
// p1.cpp -- Parallax Propeller P1 emulation (see p1.h).
#include "p1.h"
#include <cstdio>
#include <cstring>

// Opcodes (instruction bits 31..26)
enum {
    OP_RDBYTE = 0x00, OP_RDWORD, OP_RDLONG, OP_HUBOP,
    OP_ROR = 0x08, OP_ROL, OP_SHR, OP_SHL, OP_RCR, OP_RCL, OP_SAR, OP_REV,
    OP_MINS = 0x10, OP_MAXS, OP_MIN, OP_MAX, OP_MOVS, OP_MOVD, OP_MOVI, OP_JMPRET,
    OP_AND = 0x18, OP_ANDN, OP_OR, OP_XOR, OP_MUXC, OP_MUXNC, OP_MUXZ, OP_MUXNZ,
    OP_ADD = 0x20, OP_SUB, OP_ADDABS, OP_SUBABS, OP_SUMC, OP_SUMNC, OP_SUMZ, OP_SUMNZ,
    OP_MOV = 0x28, OP_NEG, OP_ABS, OP_ABSNEG, OP_NEGC, OP_NEGNC, OP_NEGZ, OP_NEGNZ,
    OP_CMPS = 0x30, OP_CMPSX, OP_ADDX, OP_SUBX, OP_ADDS, OP_SUBS, OP_ADDSX, OP_SUBSX,
    OP_CMPSUB = 0x38, OP_DJNZ, OP_TJNZ, OP_TJZ, OP_WAITPEQ, OP_WAITPNE, OP_WAITCNT, OP_WAITVID
};

// HUBOP functions (source value)
enum { HUB_CLKSET, HUB_COGID, HUB_COGINIT, HUB_COGSTOP, HUB_LOCKNEW, HUB_LOCKRET,
       HUB_LOCKSET, HUB_LOCKCLR };

static bool parity(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

static uint32_t reverse32(uint32_t v)
{
    uint32_t r = 0;
    for (int i = 0; i < 32; i++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

static uint32_t absolute(uint32_t v)
{
    return (v & 0x80000000u) ? (uint32_t)(0u - v) : v;
}

// Signed overflow of a + b (+ carry) and a - b (- borrow), 32 bits
static bool addOverflow(uint32_t a, uint32_t b, uint32_t r)
{
    return ((~(a ^ b)) & (a ^ r)) >> 31;
}

static bool subOverflow(uint32_t a, uint32_t b, uint32_t r)
{
    return ((a ^ b) & (a ^ r)) >> 31;
}

// A cog loading code from ROM addresses gets the scrambled interpreter and
// boot loader unscrambled (hub.v): output bit 31 - k is input bit PERM[k].
static uint32_t unscramble(uint32_t w)
{
    static const int8_t PERM[32] = {
        3, 7, 21, 12, 6, 19, 4, 17, 20, 15, 8, 11, 0, 14, 30, 1,
        23, 31, 16, 5, 9, 18, 25, 2, 28, 22, 13, 27, 29, 24, 26, 10,
    };
    uint32_t r = 0;
    for (int k = 0; k < 32; k++)
        if ((w >> PERM[k]) & 1)
            r |= 1u << (31 - k);
    return r;
}

// ---------------------------------------------------------------- Counter

void Counter::reset()
{
    ctr = frq = phs = 0;
    last = 0;
    phase = 0;
    a = b = false;
}

bool Counter::drivesPins() const
{
    int m = mode();
    return (m >= 2 && m <= 7) || (m >= 8 && m <= 15 && (m & 1));
}

// Clocks in [last, t) in which the counter triggers, times FRQ
uint32_t Counter::phsAt(uint64_t t) const
{
    if (t <= last)
        return phs;
    uint64_t n = t - last;
    int m = mode();
    bool count;
    if (m == 0)
        count = false;
    else if (m <= 7)
        count = true;                           // PLL, NCO, DUTY
    else if (m <= 15) {
        int kind = (m >> 1) & 3;                // 0 pos, 1 posedge, 2 neg, 3 negedge
        count = kind == 0 ? a : kind == 2 ? !a : false;   // edges: at events
    } else
        count = ((m & 15) >> ((b ? 2 : 0) + (a ? 1 : 0))) & 1;
    return count ? phs + (uint32_t)(n * frq) : phs;
}

uint32_t Counter::outputsAt(uint64_t t) const
{
    int m = mode();
    bool outA = false, outB = false;
    if (m == 2 || m == 3) {                     // PLL: NCO x 2^(PLLDIV-3)
        uint64_t p = phase + (t > last ? (t - last) * (uint64_t)frq : 0);
        int plldiv = (int)((ctr >> 23) & 7);
        outA = (p >> (34 - plldiv)) & 1;
        outB = !outA;
    } else if (m == 4 || m == 5) {              // NCO: PHS[31]
        outA = phsAt(t) >> 31;
        outB = !outA;
    } else if (m == 6 || m == 7) {              // DUTY: carry of the last add
        uint32_t p = phsAt(t);
        outA = frq != 0 && t > last && p < frq;
        outB = !outA;
    } else if (m >= 8 && m <= 15) {             // feedback: B = !A (delayed)
        outB = !a;
    }
    uint32_t pins = 0;
    if (outA && m >= 2 && m <= 7)
        pins |= 1u << apin();
    bool diff = (m == 3 || m == 5 || m == 7) || (m >= 8 && m <= 15 && (m & 1));
    if (outB && diff)
        pins |= 1u << bpin();
    return pins;
}

void Counter::advance(uint64_t t)
{
    if (t <= last)
        return;
    int m = mode();
    if (m >= 1 && m <= 7)
        phase += (t - last) * (uint64_t)frq;
    phs = phsAt(t);
    if (m >= 1 && m <= 7)
        phase = (phase & ~0xFFFFFFFFull) | phs;
    last = t;
}

// The pins changed at clock t: the counter sees it one clock later.
void Counter::pinsChanged(uint32_t pins, uint64_t t)
{
    if (!usesInputs())
        return;
    bool na = (pins >> apin()) & 1, nb = (pins >> bpin()) & 1;
    advance(t + 1);
    int m = mode();
    if ((m == 10 || m == 11) && !a && na)       // positive edge
        phs += frq;
    if ((m == 14 || m == 15) && a && !na)       // negative edge
        phs += frq;
    a = na;
    b = nb;
}

// CTR, FRQ or PHS written by an instruction starting at t: it takes effect
// when the instruction ends (t + 4).
void Counter::write(int reg, uint32_t value, uint32_t pins, uint64_t t)
{
    uint64_t te = t + 4;
    advance(te);
    if (reg == 0) {
        ctr = value;
        a = (pins >> apin()) & 1;
        b = (pins >> bpin()) & 1;
    } else if (reg == 1) {
        frq = value;
    } else {
        phs = value;
        phase = value;
    }
    if (te > last)
        last = te;
}

// ---------------------------------------------------------------- Cog

void Cog::reset(int cogId)
{
    memset(ram, 0, sizeof(ram));
    counter[0].reset();
    counter[1].reset();
    par = 0;
    pc = 0;
    ir = 0;
    c = z = false;
    running = false;
    waiting = false;
    waitVersion = 0;
    pinWrite = false;
    id = cogId;
    time = 0;
    resetVideo();
}

void Cog::resetVideo()
{
    vidOn = false;
    vidNext = 0;
    vidColors = vidPixels = 0;
    vidLate = 0;
}

uint8_t VideoFrame::color(int pixel) const
{
    int index;
    if (vcfg & (1u << 28)) {                    // 4 colours, 2 bits per pixel
        int p = pixel < 16 ? pixel : 15;
        index = (int)((pixels >> (2 * p)) & 3);
    } else {
        int p = pixel < 32 ? pixel : 31;
        index = (int)((pixels >> p) & 1);
    }
    return (uint8_t)(colors >> (8 * index));
}

// WAITVID: the generator takes colors and pixels at the start of its next
// frame (cog_vid.v: new_set), and the cog goes on two clocks later (the
// acknowledge crosses into the cog clock).  A cog that comes after the
// frame started has missed it: the generator repeats the last data.
// A cog that stopped calling WAITVID for this long starts the generator
// afresh instead of catching up
static const uint64_t VIDEO_RESYNC = 100000;

uint64_t Cog::waitvid(P1 &chip, uint32_t colors, uint32_t pixels)
{
    uint32_t vcfg = ram[VCFG], vscl = ram[VSCL];
    const Counter &pll = counter[0];
    int m = pll.mode();
    if (!((vcfg >> 29) & 3) || m < 1 || m > 3 || pll.frq == 0) {
        vidOn = false;                          // no video clock: not emulated
        return 4;
    }
    int plldiv = (int)((pll.ctr >> 23) & 7);
    uint64_t period = ((uint64_t)1 << (51 - plldiv)) / pll.frq;    // PLL output, 1/65536 clocks
    uint64_t waitFrom = (time + 3) << 16;
    VideoFrame f;
    f.period = period;
    f.vcfg = vcfg;
    f.vscl = vscl;
    uint64_t len = (uint64_t)f.frameClocks() * period;
    if (!vidOn || waitFrom > vidNext + (VIDEO_RESYNC << 16)) {
        vidOn = true;                           // (re)started: the first frame comes at once
        vidNext = waitFrom + period;
    }
    while (vidNext < waitFrom) {                // late: frames go by with the old data
        f.start = vidNext;
        f.colors = vidColors;
        f.pixels = vidPixels;
        if (chip.videoHook)
            chip.videoHook(chip, id, f, chip.videoCtx);
        vidNext += len;
        vidLate++;
    }
    f.start = vidNext;
    f.colors = colors;
    f.pixels = pixels;
    if (chip.videoHook)
        chip.videoHook(chip, id, f, chip.videoCtx);
    vidColors = colors;
    vidPixels = pixels;
    uint64_t taken = vidNext >> 16;
    vidNext += len;
    uint64_t done = taken + 3;
    return done > time + 4 ? done - time : 4;
}

void Cog::start(P1 &chip, uint16_t codeAddr, uint32_t parValue, uint64_t when)
{
    memset(ram, 0, sizeof(ram));
    for (int i = 0; i < 496; i++) {
        uint16_t a = (uint16_t)(codeAddr + i * 4);
        uint32_t v = chip.rdlong(a);
        ram[i] = a >= P1::HUB_RAM ? unscramble(v) : v;
    }
    par = parValue & 0xFFFC;
    counter[0].reset();
    counter[1].reset();
    resetVideo();
    pc = 0;
    ir = ram[0];
    c = z = false;
    waiting = false;
    running = true;
    // Loading 496 longs takes one hub window each, plus a little setup
    time = when + 8 + 496 * 16;
}

uint32_t Cog::readSource(P1 &chip, int s) const
{
    switch (s) {
    case PAR: return par;
    case CNT: return (uint32_t)time;
    case INA: return chip.inaAt(time);
    case INB: return 0;
    case PHSA: return counter[0].phsAt(time + 1);
    case PHSB: return counter[1].phsAt(time + 1);
    default:  return ram[s];
    }
}

// PAR, CNT, INA and INB used as a destination only reach the shadow RAM.
// CTRx, FRQx and PHSx also go to the counters.
void Cog::writeDest(P1 &chip, int d, uint32_t value)
{
    ram[d] = value;
    if (d >= CTRA && d <= PHSB) {
        int which = (d - CTRA) & 1;             // CTRA FRQA PHSA: even
        int reg = (d - CTRA) >> 1;              // 0 CTR, 1 FRQ, 2 PHS
        counter[which].write(reg, value, chip.pinLevels(), time);
        if (reg == 0)
            chip.countersChanged();
    }
}

void Cog::step(P1 &chip)
{
    bool stalled = false;
    uint64_t clocks = run(chip, &stalled);
    if (stalled) {
        // still waiting for pins: sleep until they change
        waiting = true;
        waitVersion = chip.pinVersion;
    } else {
        waiting = false;
        time += clocks;
    }
}

// Hub instructions, WAITPxx and WAITVID, reads of INA and PHSx, and
// writes to OUTA and the registers after it (pins, counters, video) are
// seen by, or see, the rest of the chip.  Everything else stays in the cog
// (CNT is the cog's own time; PAR, OUTA and DIRA its own registers).
bool Cog::isLocal() const
{
    int op = (int)(ir >> 26);
    if (op <= 3 || op == OP_WAITPEQ || op == OP_WAITPNE || op == OP_WAITVID)
        return false;
    int d = (int)((ir >> 9) & 0x1FF), s = (int)(ir & 0x1FF);
    if (d >= OUTA)
        return false;
    bool imm = (ir >> 22) & 1;
    return imm || (s != INA && s != PHSA && s != PHSB);
}

uint64_t Cog::execute(P1 &chip)
{
    bool stalled = false;
    uint64_t clocks = run(chip, &stalled);
    time += clocks;
    return clocks;
}

// Executes the instruction in ir at clock 'time'.  Returns its length in
// clocks; for a WAITPEQ/WAITPNE whose condition does not hold, sets
// *stalled and returns 0 (the instruction stays current).
uint64_t Cog::run(P1 &chip, bool *stalled)
{
    uint32_t i = ir;
    int op = (int)(i >> 26);
    bool wz = (i >> 25) & 1, wc = (i >> 24) & 1, wr = (i >> 23) & 1, imm = (i >> 22) & 1;
    int cond = (int)((i >> 18) & 15);
    int d = (int)((i >> 9) & 0x1FF), s = (int)(i & 0x1FF);
    uint16_t next = (uint16_t)((pc + 1) & 0x1FF);

    if (!((cond >> ((c ? 2 : 0) + (z ? 1 : 0))) & 1)) {
        pc = next;
        ir = ram[pc];
        return 4;
    }

    uint32_t sv = imm ? (uint32_t)s : readSource(chip, s);
    uint32_t dv = ram[d];
    uint32_t r = dv;
    bool nc = c, nz = z;
    uint64_t clocks = 4;

    switch (op) {
    case OP_RDBYTE:
    case OP_RDWORD:
    case OP_RDLONG: {
        clocks = P1::hubWindow(id, time) - time + 8;
        uint16_t a = (uint16_t)sv;
        if (wr) {
            r = op == OP_RDBYTE ? chip.rdbyte(a)
              : op == OP_RDWORD ? chip.rdword(a)
              : chip.rdlong(a);
        } else {
            if (op == OP_RDBYTE) chip.wrbyte(a, (uint8_t)dv);
            else if (op == OP_RDWORD) chip.wrword(a, (uint16_t)dv);
            else chip.wrlong(a, dv);
        }
        nz = r == 0;              // C is left as it was
        break;
    }
    case OP_HUBOP: {
        clocks = P1::hubWindow(id, time) - time + 8;
        uint64_t when = time + clocks;
        nc = false;
        switch (sv & 7) {
        case HUB_CLKSET:
            break;
        case HUB_COGID:
            r = (uint32_t)id;
            break;
        case HUB_COGINIT: {
            int target = (dv & 8) ? -1 : (int)(dv & 7);
            int started = chip.coginit(target, (uint16_t)((dv >> 2) & 0xFFFC),
                                       (uint16_t)((dv >> 16) & 0xFFFC), when);
            nc = started < 0;
            r = started < 0 ? 7 : (uint32_t)started;
            if (started == id)
                return 0;                   // restarted itself: this instruction is gone
            break;
        }
        case HUB_COGSTOP:
            chip.cogstop((int)(dv & 7));
            r = dv & 7;
            break;
        case HUB_LOCKNEW: {
            int n = 0;
            while (n < 8 && chip.lockUsed[n])
                n++;
            if (n == 8) {
                nc = true;
                r = 7;
            } else {
                chip.lockUsed[n] = true;
                chip.lockState[n] = false;
                r = (uint32_t)n;
            }
            break;
        }
        case HUB_LOCKRET:
            chip.lockUsed[dv & 7] = false;
            r = dv & 7;
            break;
        case HUB_LOCKSET:
            nc = chip.lockState[dv & 7];
            chip.lockState[dv & 7] = true;
            r = dv & 7;
            break;
        case HUB_LOCKCLR:
            nc = chip.lockState[dv & 7];
            chip.lockState[dv & 7] = false;
            r = dv & 7;
            break;
        }
        nz = r == 0;
        break;
    }

    // Undefined opcodes 04-07 do nothing
    case 0x04: case 0x05: case 0x06: case 0x07:
        wr = wz = wc = false;
        break;

    case OP_ROR: {
        int n = sv & 31;
        r = n ? (dv >> n) | (dv << (32 - n)) : dv;
        nc = dv & 1;
        nz = r == 0;
        break;
    }
    case OP_ROL: {
        int n = sv & 31;
        r = n ? (dv << n) | (dv >> (32 - n)) : dv;
        nc = dv >> 31;
        nz = r == 0;
        break;
    }
    case OP_SHR:
        r = dv >> (sv & 31);
        nc = dv & 1;
        nz = r == 0;
        break;
    case OP_SHL:
        r = dv << (sv & 31);
        nc = dv >> 31;
        nz = r == 0;
        break;
    case OP_RCR: {
        int n = sv & 31;
        r = dv >> n;
        if (c && n)
            r |= ~(0xFFFFFFFFu >> n);
        nc = dv & 1;
        nz = r == 0;
        break;
    }
    case OP_RCL: {
        int n = sv & 31;
        r = dv << n;
        if (c && n)
            r |= (1u << n) - 1;
        nc = dv >> 31;
        nz = r == 0;
        break;
    }
    case OP_SAR:
        r = (uint32_t)((int32_t)dv >> (sv & 31));
        nc = dv & 1;
        nz = r == 0;
        break;
    case OP_REV:
        r = reverse32(dv) >> (sv & 31);
        nc = dv & 1;
        nz = r == 0;
        break;

    case OP_MINS:
        nc = (int32_t)dv < (int32_t)sv;
        r = nc ? sv : dv;
        nz = sv == 0;
        break;
    case OP_MAXS:
        nc = (int32_t)dv < (int32_t)sv;
        r = nc ? dv : sv;
        nz = sv == 0;
        break;
    case OP_MIN:
        nc = dv < sv;
        r = nc ? sv : dv;
        nz = sv == 0;
        break;
    case OP_MAX:
        nc = dv < sv;
        r = nc ? dv : sv;
        nz = sv == 0;
        break;
    // MOVS/MOVD/MOVI/JMPRET: C = D < S (unsigned), as spinsim shows
    case OP_MOVS:
        r = (dv & ~0x1FFu) | (sv & 0x1FF);
        nc = dv < sv;
        nz = r == 0;
        break;
    case OP_MOVD:
        r = (dv & ~(0x1FFu << 9)) | ((sv & 0x1FF) << 9);
        nc = dv < sv;
        nz = r == 0;
        break;
    case OP_MOVI:
        r = (dv & ~(0x1FFu << 23)) | ((sv & 0x1FF) << 23);
        nc = dv < sv;
        nz = r == 0;
        break;
    case OP_JMPRET:
        r = (dv & ~0x1FFu) | next;
        nc = dv < sv;
        nz = r == 0;
        next = (uint16_t)(sv & 0x1FF);
        break;

    case OP_AND:   r = dv & sv;  nz = r == 0; nc = parity(r); break;
    case OP_ANDN:  r = dv & ~sv; nz = r == 0; nc = parity(r); break;
    case OP_OR:    r = dv | sv;  nz = r == 0; nc = parity(r); break;
    case OP_XOR:   r = dv ^ sv;  nz = r == 0; nc = parity(r); break;
    case OP_MUXC:  r = c ? (dv | sv) : (dv & ~sv);  nz = r == 0; nc = parity(r); break;
    case OP_MUXNC: r = !c ? (dv | sv) : (dv & ~sv); nz = r == 0; nc = parity(r); break;
    case OP_MUXZ:  r = z ? (dv | sv) : (dv & ~sv);  nz = r == 0; nc = parity(r); break;
    case OP_MUXNZ: r = !z ? (dv | sv) : (dv & ~sv); nz = r == 0; nc = parity(r); break;

    case OP_ADD:
        r = dv + sv;
        nc = r < dv;
        nz = r == 0;
        break;
    case OP_SUB:
        r = dv - sv;
        nc = dv < sv;
        nz = r == 0;
        break;
    case OP_ADDABS: {
        uint32_t a = absolute(sv);
        r = dv + a;
        nc = (sv & 0x80000000u) ? dv < sv : r < dv;    // S < 0: borrow of D - S
        nz = r == 0;
        break;
    }
    case OP_SUBABS: {
        uint32_t a = absolute(sv);
        r = dv - a;
        nc = (sv & 0x80000000u) ? r < dv : dv < a;
        nz = r == 0;
        break;
    }
    case OP_SUMC: case OP_SUMNC: case OP_SUMZ: case OP_SUMNZ: {
        bool negate = op == OP_SUMC ? c : op == OP_SUMNC ? !c : op == OP_SUMZ ? z : !z;
        if (negate) {
            r = dv - sv;
            nc = subOverflow(dv, sv, r);
        } else {
            r = dv + sv;
            nc = addOverflow(dv, sv, r);
        }
        nz = r == 0;
        break;
    }

    case OP_MOV:    r = sv;                          nc = sv >> 31; nz = r == 0; break;
    case OP_NEG:    r = 0u - sv;                     nc = sv >> 31; nz = r == 0; break;
    case OP_ABS:    r = absolute(sv);                nc = sv >> 31; nz = r == 0; break;
    case OP_ABSNEG: r = 0u - absolute(sv);           nc = sv >> 31; nz = r == 0; break;
    case OP_NEGC:   r = c ? 0u - sv : sv;            nc = sv >> 31; nz = r == 0; break;
    case OP_NEGNC:  r = c ? sv : 0u - sv;            nc = sv >> 31; nz = r == 0; break;
    case OP_NEGZ:   r = z ? 0u - sv : sv;            nc = sv >> 31; nz = r == 0; break;
    case OP_NEGNZ:  r = z ? sv : 0u - sv;            nc = sv >> 31; nz = r == 0; break;

    case OP_CMPS:
        r = dv - sv;
        nc = (int32_t)dv < (int32_t)sv;
        nz = r == 0;
        break;
    case OP_CMPSX: {
        r = dv - sv - (c ? 1 : 0);
        int64_t diff = (int64_t)(int32_t)dv - (int64_t)(int32_t)sv - (c ? 1 : 0);
        nc = diff < 0;
        nz = z && r == 0;
        break;
    }
    case OP_ADDX: {
        uint64_t sum = (uint64_t)dv + sv + (c ? 1 : 0);
        r = (uint32_t)sum;
        nc = sum >> 32;
        nz = z && r == 0;
        break;
    }
    case OP_SUBX: {
        uint64_t sub = (uint64_t)sv + (c ? 1 : 0);
        r = (uint32_t)((uint64_t)dv - sub);
        nc = (uint64_t)dv < sub;
        nz = z && r == 0;
        break;
    }
    case OP_ADDS:
        r = dv + sv;
        nc = addOverflow(dv, sv, r);
        nz = r == 0;
        break;
    case OP_SUBS:
        r = dv - sv;
        nc = subOverflow(dv, sv, r);
        nz = r == 0;
        break;
    case OP_ADDSX: {
        int64_t sum = (int64_t)(int32_t)dv + (int64_t)(int32_t)sv + (c ? 1 : 0);
        r = (uint32_t)sum;
        nc = sum > 0x7FFFFFFFLL || sum < -0x80000000LL;
        nz = z && r == 0;
        break;
    }
    case OP_SUBSX: {
        int64_t diff = (int64_t)(int32_t)dv - (int64_t)(int32_t)sv - (c ? 1 : 0);
        r = (uint32_t)diff;
        nc = diff > 0x7FFFFFFFLL || diff < -0x80000000LL;
        nz = z && r == 0;
        break;
    }

    case OP_CMPSUB:
        nc = dv >= sv;
        r = nc ? dv - sv : dv;
        nz = r == 0;
        break;
    case OP_DJNZ:
        r = dv - 1;
        nc = dv == 0;
        nz = r == 0;
        if (r != 0)
            next = (uint16_t)(sv & 0x1FF);
        else
            clocks = 8;
        break;
    case OP_TJNZ:
    case OP_TJZ: {
        bool jump = op == OP_TJNZ ? dv != 0 : dv == 0;
        nz = dv == 0;
        nc = false;
        if (jump)
            next = (uint16_t)(sv & 0x1FF);
        else
            clocks = 8;
        break;
    }

    case OP_WAITCNT: {
        // Ends 5 clocks after the first clock >= time + 1 where CNT == D
        // (a target already passed waits for the counter to wrap, ~37 s)
        uint32_t delta = dv - (uint32_t)(time + 1);
        clocks = (uint64_t)delta + 1 + 5;
        r = dv + sv;
        nc = r < dv;
        nz = r == 0;
        break;
    }
    case OP_WAITPEQ:
    case OP_WAITPNE: {
        uint32_t pins = chip.ina() & sv;
        bool done = op == OP_WAITPEQ ? pins == dv : pins != dv;
        if (!done) {
            *stalled = true;
            return 0;
        }
        clocks = 6;
        wr = false;
        break;
    }
    case OP_WAITVID:
        clocks = waitvid(chip, dv, sv);
        wr = false;
        break;
    }

    // Fetch the next instruction before the result is written
    uint32_t nextIr = ram[next];
    if (wr) {
        writeDest(chip, d, r);
        if (d == OUTA || d == DIRA)
            pinWrite = true;
    }
    if (wc)
        c = nc;
    if (wz)
        z = nz;
    pc = next;
    ir = nextIr;
    return clocks;
}

// ---------------------------------------------------------------- P1

P1::P1()
{
    romLoaded = false;
    memset(hub, 0, sizeof(hub));
    reset();
}

void P1::reset()
{
    memset(hub, 0, HUB_RAM);
    for (int i = 0; i < 8; i++)
        cogs[i].reset(i);
    clock = 0;
    inputs = 0;
    pinVersion = 0;
    pinOut = pinDir = 0;
    histNewest = 0;
    histCount = 0;
    lastLevels = 0;
    cogChanges = 0;
    pinHook = nullptr;
    videoHook = nullptr;
    videoCtx = nullptr;
    pinCtx = nullptr;
    counterPins = false;
    for (int i = 0; i < 8; i++)
        lockUsed[i] = lockState[i] = false;
}

uint32_t P1::outputs() const
{
    uint32_t o = 0;
    for (int i = 0; i < 8; i++)
        if (cogs[i].running)
            o |= cogs[i].ram[Cog::OUTA] & cogs[i].ram[Cog::DIRA];
    return o;
}

uint32_t P1::directions() const
{
    uint32_t d = 0;
    for (int i = 0; i < 8; i++)
        if (cogs[i].running)
            d |= cogs[i].ram[Cog::DIRA];
    return d;
}

// A pin reads high when a cog drives it high, or when no cog drives it
// and the outside world holds it high (see ina() in p1.h).
bool P1::refreshPins(uint64_t when)
{
    uint32_t out = outputs(), dir = directions();
    if (out == pinOut && dir == pinDir)
        return false;
    pinOut = out;
    pinDir = dir;
    pinsChanged(when);
    if (pinHook)
        pinHook(*this, when, pinCtx);
    return true;
}

bool P1::loadRom(const char *path)
{
    static uint8_t buffer[0x8000];
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(buffer, 1, sizeof(buffer), f);
    fclose(f);
    return loadRomImage(buffer, (uint32_t)n);
}

bool P1::loadRomImage(const uint8_t *data, uint32_t size)
{
    if (size != 0x8000)
        return false;
    memcpy(hub + HUB_RAM, data, size);
    romLoaded = true;
    return true;
}

// What the boot loader (booter.spin) does once a program is in RAM: clear
// the rest of RAM, put $FFF9FFFF in the two longs below dbase (so that
// returning from the first method runs the ROM's "runner", which stops
// the cog), check pbase, and restart cog 0 with the interpreter at $F004,
// PAR = $0004 (the program header).
bool P1::bootBinary(const uint8_t *data, uint32_t size, uint64_t when)
{
    if (!romLoaded || size < 16 || size > HUB_RAM)
        return false;
    memset(hub, 0, HUB_RAM);
    memcpy(hub, data, size);
    if (rdword(0x0006) != 0x0010)
        return false;
    uint16_t dbase = rdword(0x000A);
    wrlong((uint16_t)(dbase - 4), 0xFFF9FFFF);
    wrlong((uint16_t)(dbase - 8), 0xFFF9FFFF);
    coginit(0, 0xF004, 0x0004, when);
    return true;
}

bool P1::bootFile(const char *path, uint64_t when)
{
    static uint8_t buffer[HUB_RAM];
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(buffer, 1, sizeof(buffer), f);
    fclose(f);
    return bootBinary(buffer, (uint32_t)n, when);
}

bool P1::anyRunning() const
{
    for (int i = 0; i < 8; i++)
        if (cogs[i].running)
            return true;
    return false;
}

uint16_t P1::rdword(uint16_t a) const
{
    a &= 0xFFFE;
    return (uint16_t)(hub[a] | (hub[a + 1] << 8));
}

uint32_t P1::rdlong(uint16_t a) const
{
    a &= 0xFFFC;
    return (uint32_t)hub[a] | ((uint32_t)hub[a + 1] << 8) |
           ((uint32_t)hub[a + 2] << 16) | ((uint32_t)hub[a + 3] << 24);
}

void P1::wrbyte(uint16_t a, uint8_t v)
{
    if (a < HUB_RAM)
        hub[a] = v;
}

void P1::wrword(uint16_t a, uint16_t v)
{
    a &= 0xFFFE;
    wrbyte(a, (uint8_t)v);
    wrbyte((uint16_t)(a + 1), (uint8_t)(v >> 8));
}

void P1::wrlong(uint16_t a, uint32_t v)
{
    a &= 0xFFFC;
    for (int i = 0; i < 4; i++)
        wrbyte((uint16_t)(a + i), (uint8_t)(v >> (8 * i)));
}

int P1::coginit(int id, uint16_t code, uint16_t par, uint64_t when)
{
    if (id < 0) {
        for (int i = 0; i < 8; i++)
            if (!cogs[i].running) {
                id = i;
                break;
            }
        if (id < 0)
            return -1;
    }
    cogs[id].start(*this, code, par, when);
    cogChanges++;
    refreshPins(when);
    countersChanged();
    return id;
}

void P1::cogstop(int id)
{
    cogs[id & 7].running = false;
    cogChanges++;
    refreshPins(clock);
    countersChanged();
}

uint64_t P1::hubWindow(int id, uint64_t t)
{
    uint64_t slot = (uint64_t)(2 * id);
    uint64_t phase = t & 15;
    uint64_t wait = (slot + 16 - phase) & 15;
    return t + wait;
}

Cog *P1::earliest()
{
    Cog *best = nullptr;
    for (int i = 0; i < 8; i++) {
        Cog &c = cogs[i];
        if (!c.running || (c.waiting && c.waitVersion == pinVersion))
            continue;
        if (!best || c.time < best->time)
            best = &c;
    }
    return best;
}

bool P1::stepCog(Cog &cog)
{
    uint64_t t = cog.time;
    clock = t;
    cog.pinWrite = false;
    cog.step(*this);
    return cog.pinWrite && refreshPins(t);
}

bool P1::runCog(Cog &cog, uint64_t limit, uint64_t until)
{
    uint32_t changes = cogChanges;
    do {
        if (stepCog(cog))
            return true;
    } while (cog.running && !cog.waiting && cogChanges == changes &&
             (cog.time < limit || (cog.time < until && cog.isLocal())));
    return false;
}

bool P1::runChip(uint64_t limit, uint64_t until)
{
    const uint64_t NEVER = ~(uint64_t)0;
    for (;;) {
        Cog *best = nullptr;
        uint64_t second = NEVER;
        for (int i = 0; i < 8; i++) {
            Cog &c = cogs[i];
            if (!c.running || (c.waiting && c.waitVersion == pinVersion))
                continue;
            if (!best || c.time < best->time) {
                if (best)
                    second = best->time;
                best = &c;
            } else if (c.time < second) {
                second = c.time;
            }
        }
        if (!best || best->time >= limit)
            return false;
        if (runCog(*best, second < limit ? second : limit, until))
            return true;
    }
}

uint64_t P1::earliestOther(const Cog *cog) const
{
    uint64_t t = ~(uint64_t)0;
    for (int i = 0; i < 8; i++) {
        const Cog &c = cogs[i];
        if (&c == cog || !c.running || (c.waiting && c.waitVersion == pinVersion))
            continue;
        if (c.time < t)
            t = c.time;
    }
    return t;
}

uint32_t P1::inaAt(uint64_t t) const
{
    uint32_t v = lastLevels;
    for (int k = 0, i = histNewest; k < histCount; k++, i = (i + HISTORY - 1) % HISTORY) {
        if (histTime[i] + 2 <= t)
            break;
        v = histBefore[i];              // too recent for this reader
    }
    if (counterPins)
        v |= counterOutputs(t);
    return v;
}

uint32_t P1::ina() const
{
    uint32_t v = pinLevels();
    if (counterPins)
        v |= counterOutputs(clock);
    return v;
}

uint32_t P1::counterOutputs(uint64_t t) const
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        const Cog &c = cogs[i];
        if (c.running)
            v |= (c.counter[0].outputsAt(t) | c.counter[1].outputsAt(t)) & c.ram[Cog::DIRA];
    }
    return v;
}

bool P1::dutyPhase(int pin, uint64_t t, uint64_t &phase, int &id) const
{
    for (int i = 0; i < 8; i++) {
        const Cog &c = cogs[i];
        if (!c.running || !((c.ram[Cog::DIRA] >> pin) & 1))
            continue;
        for (int k = 0; k < 2; k++) {
            const Counter &ctr = c.counter[k];
            if ((ctr.mode() == 6 || ctr.mode() == 7) && ctr.apin() == pin) {
                phase = ctr.phaseAt(t);
                id = 2 * i + k;
                return true;
            }
        }
    }
    return false;
}

void P1::countersChanged()
{
    counterPins = false;
    for (int i = 0; i < 8; i++)
        if (cogs[i].running && (cogs[i].counter[0].drivesPins() || cogs[i].counter[1].drivesPins()))
            counterPins = true;
}

// Pin changes take effect at once: a waiting cog checks again one clock
// after the instruction that changed them started.  Counters with inputs
// see them one clock later.
void P1::pinsChanged(uint64_t when)
{
    pinVersion++;
    uint32_t levels = pinLevels();
    histNewest = (histNewest + 1) % HISTORY;
    histTime[histNewest] = when;
    histBefore[histNewest] = lastLevels;
    histAfter[histNewest] = levels;
    if (histCount < HISTORY)
        histCount++;
    lastLevels = levels;
    for (int i = 0; i < 8; i++)
        if (cogs[i].running) {
            cogs[i].counter[0].pinsChanged(levels, when);
            cogs[i].counter[1].pinsChanged(levels, when);
        }
    for (int i = 0; i < 8; i++)
        if (cogs[i].running && cogs[i].waiting && cogs[i].time < when + 1)
            cogs[i].time = when + 1;
}

void P1::setInputs(uint32_t levels, uint64_t when)
{
    if (levels != inputs) {
        inputs = levels;
        pinsChanged(when);
    }
}

void P1::run(uint64_t until)
{
    for (;;) {
        Cog *best = earliest();
        if (!best || best->time >= until)
            break;
        runChip(until, until);
    }
    clock = until;
}

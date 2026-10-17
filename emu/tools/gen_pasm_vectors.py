#!/usr/bin/env python3
"""Generate PASM instruction test vectors with spinsim.

Runs a test program in spinsim (from PropGCC / SimpleIDE): a cog executes
each test instruction with given D, S, C and Z and records the result and
the flags.  The results become emu/tests/pasm_vectors.inc, which the
emulator tests (make -C emu test) check against.  The emulator is then compared
with an independent P1 simulator, not with my reading of the manual.

Usage: emu/tools/gen_pasm_vectors.py      (needs openspin and spinsim in the PATH)
"""
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SPECIAL = [0, 1, 2, 0x1F, 0x20, 0x21, 0x1FF, 0x200, 0x7FFFFFFF, 0x80000000,
           0x80000001, 0xFFFFFFFF, 0xFFFFFFFE, 0x55555555, 0xAAAAAAAA, 0x12345678]

# ALU opcodes with a register source (0x17 JMPRET and the jumps go apart)
ALU_OPS = list(range(0x08, 0x17)) + list(range(0x18, 0x39))
JUMP_OPS = [0x17, 0x39, 0x3A, 0x3B]       # JMPRET, DJNZ, TJNZ, TJZ
READ_OPS = [0x00, 0x01, 0x02]             # RDBYTE, RDWORD, RDLONG (R = 1)

# flags word of a case
F_C, F_Z, F_JUMP, F_SREG, F_DATA = 1, 2, 4, 8, 16

DATA = bytes((i * 37 + 11) & 0xFF for i in range(64))


def value(rng):
    return rng.choice(SPECIAL) if rng.random() < 0.5 else rng.getrandbits(32)


def instr(op, r, imm, s=0):
    return ((op << 26) | (1 << 25) | (1 << 24) | (r << 23) | (imm << 22) |
            (0xF << 18) | (s & 0x1FF))


def cases():
    rng = random.Random(2700)
    out = []
    for op in ALU_OPS:
        for _ in range(20):
            d = value(rng)
            s = value(rng)
            if 0x08 <= op <= 0x0F and rng.random() < 0.7:
                s = rng.randrange(64)                       # shift counts
            f = (rng.getrandbits(1) * F_C) | (rng.getrandbits(1) * F_Z)
            out.append((instr(op, 1, 0), d, s, f | F_SREG))
        for _ in range(4):                                  # immediate source
            d = value(rng)
            s = rng.randrange(512)
            f = (rng.getrandbits(1) * F_C) | (rng.getrandbits(1) * F_Z)
            out.append((instr(op, 1, 1, s), d, 0, f))
    for op in JUMP_OPS:
        for _ in range(12):
            d = rng.choice([0, 1, 2, 0x1FF, 0x200, 0xFFFFFFFF]) if rng.random() < 0.6 else rng.getrandbits(32)
            f = (rng.getrandbits(1) * F_C) | (rng.getrandbits(1) * F_Z)
            r = 0 if op in (0x3A, 0x3B) and rng.random() < 0.5 else 1
            out.append((instr(op, r, 1), d, 0, f | F_JUMP))
    for op in READ_OPS:
        for off in range(0, 64, 3):
            f = (rng.getrandbits(1) * F_C) | (rng.getrandbits(1) * F_Z)
            out.append((instr(op, 1, 0), rng.getrandbits(32), off, f | F_SREG | F_DATA))
    return out


def spin_source(table):
    lines = []
    for ins, d, s, f in table:
        lines.append(f"              long ${ins:08X}, ${d:08X}, ${s:08X}, ${f:08X}")
    data = ", ".join(f"${b:02X}" for b in DATA)
    return f"""CON
    _CLKMODE = XTAL1 + PLL8X
    _XINFREQ = 14_318_181
    NCASES = {len(table)}

OBJ
    ser : "serial_tx"

PUB main | i, p
    ser.start(30, 9600)
    cognew(@entry, @info)
    repeat until info[4]
    ser.str(string("ADDR "))
    repeat i from 0 to 3
        ser.hex(info[i], 3)
        ser.tx(" ")
    ser.crlf
    p := @cases
    repeat i from 0 to NCASES - 1
        ser.hex(long[p][1], 8)
        ser.tx(" ")
        ser.hex(long[p][3], 1)
        ser.crlf
        p += 16
    ser.str(string("END", 13, 10))
    waitcnt(cnt + clkfreq / 50)
    cogstop(cogid)

DAT
              org     0
entry         mov     t, par               ' info block: cog addresses
              mov     u, #exec
              wrlong  u, t
              add     t, #4
              mov     u, #dreg
              wrlong  u, t
              add     t, #4
              mov     u, #sreg
              wrlong  u, t
              add     t, #4
              mov     u, #after
              wrlong  u, t
              add     t, #4
              mov     done_ptr, t
              add     t, #4
              mov     base, t              ' data
              mov     ptr, t
              add     ptr, #64             ' cases
              mov     n, ncases_
loop          mov     cur, ptr
              rdlong  ins, ptr
              add     ptr, #4
              rdlong  dreg, ptr
              add     ptr, #4
              rdlong  sreg, ptr
              add     ptr, #4
              rdlong  flg, ptr
              add     ptr, #4
              test    flg, #{F_DATA}       wz
        if_nz add     sreg, base
              test    flg, #{F_SREG}       wz
        if_nz movs    ins, #sreg
              test    flg, #{F_JUMP}       wz
        if_nz movs    ins, #after
              movd    ins, #dreg
              mov     exec, ins
              mov     rflags, #0
              mov     t, flg
              and     t, #{F_Z}
              cmp     t, #{F_Z}            wz
              mov     t, flg
              shr     t, #1                wc
exec          nop
after         muxc    rflags, #1
              muxz    rflags, #2
              mov     t, cur
              add     t, #4
              wrlong  dreg, t
              add     t, #8
              wrlong  rflags, t
              djnz    n, #loop
              mov     t, #1
              wrlong  t, done_ptr
              cogid   t
              cogstop t

ncases_       long    NCASES
done_ptr      long    0
base          res     1
ptr           res     1
n             res     1
cur           res     1
ins           res     1
dreg          res     1
sreg          res     1
flg           res     1
rflags        res     1
t             res     1
u             res     1

DAT
info          long    0[5]                 ' exec, dreg, sreg, after, done
data          byte    {data}
cases
{chr(10).join(lines)}
"""


def main():
    table = cases()
    openspin = shutil.which("openspin")         # installed with the toolchain
    spinsim = shutil.which("spinsim")
    if not openspin or not spinsim:
        sys.exit("needs openspin and spinsim in the PATH")

    tmp = tempfile.mkdtemp(prefix="pasmvec")
    src = spin_source(table)
    with open(os.path.join(tmp, "vectors.spin"), "w") as f:
        f.write(src)
    shutil.copy(os.path.join(ROOT, "tools", "serial_tx.spin"), tmp)
    subprocess.run([openspin, "-q", "-b", "-o", os.path.join(tmp, "vectors.binary"),
                    os.path.join(tmp, "vectors.spin")], check=True)
    out = subprocess.run([spinsim, "-b9600", os.path.join(tmp, "vectors.binary")],
                         capture_output=True, timeout=3600).stdout
    text = out.replace(b"\0", b"").decode("ascii", "replace")
    lines = [l.strip() for l in text.splitlines()
             if l.strip() and "XXXXXXXXXX BAD" not in l]
    addr = next(l for l in lines if l.startswith("ADDR"))
    exec_a, dreg_a, sreg_a, after_a = (int(x, 16) for x in addr.split()[1:])
    results = [l for l in lines if re.fullmatch(r"[0-9A-F]{8} [0-9A-F]", l)]
    if len(results) != len(table):
        sys.exit(f"expected {len(table)} results, got {len(results)}")

    path = os.path.join(ROOT, "tests", "pasm_vectors.inc")
    with open(path, "w", newline="\n") as f:
        f.write("// SPDX-License-Identifier: GPL-3.0-or-later\n")
        f.write("// pasm_vectors.inc -- generated by emu/tools/gen_pasm_vectors.py with spinsim.\n")
        f.write("// Do not edit.  { instruction (D/S fields filled at run time), D, S, flags in, D out, flags out }\n")
        f.write(f"static const int VEC_EXEC = 0x{exec_a:03X}, VEC_DREG = 0x{dreg_a:03X}, "
                f"VEC_SREG = 0x{sreg_a:03X}, VEC_AFTER = 0x{after_a:03X};\n")
        f.write("static const uint8_t VEC_DATA[64] = {\n")
        for i in range(0, 64, 16):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in DATA[i:i + 16]) + ",\n")
        f.write("};\n")
        f.write("static const uint32_t VECTORS[][6] = {\n")
        for (ins, d, s, fl), res in zip(table, results):
            rd, rf = res.split()
            f.write(f"    {{ 0x{ins:08X}, 0x{d:08X}, 0x{s:08X}, {fl}, 0x{rd}, {int(rf, 16)} }},\n")
        f.write("};\n")
    print(f"{len(table)} vectors written to {path}")
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()

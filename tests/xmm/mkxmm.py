#!/usr/bin/env python3
"""Turn a PropGCC XMMC or XMM program (.elf) into what p1run needs to run it
from a 23LC1024 SPI SRAM (p1run --spi-sram):

  NAME.xmm     the external image, for SRAM address 0, as propeller-load's
               BuildExternalImage2 builds it: the sections above the
               program header as they are, then copies of the sections
               below it (hub RAM, and external RAM in XMM and XMM split)
               and a table that the kernel's start-up code uses to copy
               or clear them
  NAME.binary  xmm_boot.spin with the program's .xmmkernel section, a
               Spin binary that starts the driver and the kernel

  p1run NAME.binary --spi-sram NAME.xmm

XMM and XMM split use both of the kernel's spaces: their RAM space
($20000000) goes after the image, from the next 1 KB.  XMMC uses only
the flash space ($30000000), XMM single only the RAM space: both start at
SRAM address 0.

Usage: tests/xmm/mkxmm.py program.elf [--openspin PATH] [--xmem-include DIR]
  --xmem-include: propeller-load's xmem-drivers (xmem_common.spin and the
                  SPI code); default deps/propeller-load/xmem-drivers
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SRAM_SIZE = 128 * 1024
# xmm_boot.spin puts the cache (8 KB), its tags and the mailboxes at the top
# of hub RAM; the stack grows down from the mailboxes, towards what the
# program keeps in the hub (all its data in XMMC)
MBOX = 32768 - 8192 - 128 * 4 - 8 * 2 * 4 - 4


class Elf:
    def __init__(self, data):
        self.data = data
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            sys.exit("not a 32-bit little-endian ELF file")
        (self.type, self.machine, self.version, self.entry, self.phoff, self.shoff,
         self.flags, _, self.phentsize, self.phnum, self.shentsize, self.shnum,
         self.shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
        self.programs = [struct.unpack_from("<IIIIIIII", data, self.phoff + i * self.phentsize)
                         for i in range(self.phnum)]   # type offset vaddr paddr filesz memsz flags align
        sections = [struct.unpack_from("<IIIIIIIIII", data, self.shoff + i * self.shentsize)
                    for i in range(self.shnum)]
        strtab = sections[self.shstrndx]
        self.sections = {}
        for s in sections:
            start = strtab[4] + s[0]
            name = data[start:data.index(b"\0", start)].decode()
            self.sections[name] = s      # name type flags addr offset size ...

    def segment_of(self, name):
        """Index of the program segment holding section 'name' (by file offset)."""
        s = self.sections.get(name)
        if not s:
            return None
        offset, size = s[4], s[5]
        for i, p in enumerate(self.programs):
            if p[4] and p[1] <= offset and offset + size <= p[1] + p[4]:
                return i
        return None

    def segment_bytes(self, p):
        return self.data[p[1]:p[1] + p[4]]


def external_image(elf):
    """propeller-load's BuildExternalImage2 (loader/src/xmm-image2.c)."""
    ki = elf.segment_of(".xmmkernel")
    if ki is None:
        sys.exit("no .xmmkernel segment: not an XMM/XMMC program")
    header_name = ".header" if ((elf.flags >> 16) & 0xFF) == 0 else ".init"
    hi = elf.segment_of(header_name)
    if hi is None:
        sys.exit("no %s segment" % header_name)
    base = elf.programs[hi][3]
    end = base
    inits = []          # (vaddr, bytes or None, size)
    for i, p in enumerate(elf.programs):
        _, _, vaddr, paddr, filesz, memsz, _, _ = p
        if i == ki:
            continue
        if paddr >= base:
            if filesz and paddr + memsz > end:
                end = paddr + memsz
            elif not filesz and memsz:
                # a .bss of its own (an alignment gap kept it out of the
                # .data segment): outside the image, so zeroed at start-up
                inits.append((paddr, None, memsz))
        else:
            if filesz:
                inits.append((paddr, elf.segment_bytes(p), filesz))
            if memsz > filesz:
                inits.append((paddr + filesz, None, memsz - filesz))
    text = bytearray(end - base)
    for i, p in enumerate(elf.programs):
        if i != ki and p[3] >= base and p[4]:
            text[p[3] - base:p[3] - base + p[4]] = elf.segment_bytes(p)
    data = bytearray()
    table = bytearray()
    for vaddr, content, size in inits:
        if content is None:
            table += struct.pack("<III", vaddr, vaddr, size)     # vaddr == paddr: zero it
        else:
            table += struct.pack("<III", vaddr, base + len(text) + len(data), size)
            data += content
    image = text + data + table
    struct.pack_into("<II", image, 4, len(inits), len(text) + len(data))   # initCount, initTableOffset

    # The kernel's cog image, as propeller-load's LoadExternalImage makes
    # it: .xmmkernel, and .start.kerext preloaded at its cog address
    kernel = bytearray(496 * 4)
    kbytes = elf.segment_bytes(elf.programs[ki])
    kernel[:len(kbytes)] = kbytes
    si = elf.segment_of(".start.kerext")
    if si is not None:
        p = elf.programs[si]
        kernel[p[2]:p[2] + p[4]] = elf.segment_bytes(p)
    return base, bytes(image), bytes(kernel)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--openspin", default=shutil.which("openspin") or "openspin")
    ap.add_argument("--min-stack", type=int, default=1024,
                    help="bytes of hub RAM the stack needs at least (default 1024)")
    ap.add_argument("--xmem-include",
                    default=os.path.join(ROOT, "deps", "propeller-load", "xmem-drivers"))
    args = ap.parse_args()

    elf = Elf(open(args.elf, "rb").read())
    base, image, kernel = external_image(elf)
    ram_base = 0
    if base >= 0x30000000 and any(0x20000000 <= p[3] < 0x30000000 for p in elf.programs):
        ram_base = (len(image) + 1023) & ~1023
    # what the program uses of the RAM space, .bss included: the image
    # only holds the initialised part
    ram_end = max([p[3] + p[5] - 0x20000000 for p in elf.programs
                   if 0x20000000 <= p[3] < 0x30000000] + [0])
    need = max(len(image), ram_base + ram_end)
    if need > SRAM_SIZE:
        sys.exit("%s: %d bytes do not fit in the %d KB SRAM" % (args.elf, need, SRAM_SIZE // 1024))
    # what the program puts in hub RAM (data, .bss, kernel extensions):
    # it must end below the cache and the mailboxes, with room for the stack
    hub_end = max([p[3] + p[5] for i, p in enumerate(elf.programs) if p[3] < 0x8000 and p[5]] + [0])
    if hub_end + args.min_stack > MBOX:
        sys.exit("%s: %d bytes in hub RAM do not fit below the cache and the mailboxes ($%04X) "
                 "with %d bytes for the stack" % (args.elf, hub_end, MBOX, args.min_stack))
    stem = os.path.splitext(args.elf)[0]
    open(stem + ".xmm", "wb").write(image)

    tmp = tempfile.mkdtemp(prefix="xmm")
    open(os.path.join(tmp, "xmmkernel.bin"), "wb").write(kernel)
    src = open(os.path.join(HERE, "xmm_boot.spin"), encoding="utf-8").read()
    for line, value in (("    CODE_BASE  = $3000_0000", "    CODE_BASE  = $%08X" % base),
                        ("    RAM_BASE   = 0", "    RAM_BASE   = $%05X" % ram_base)):
        if line not in src:
            sys.exit("xmm_boot.spin: %r not found" % line.strip())
        src = src.replace(line, value)
    open(os.path.join(tmp, "xmm_boot.spin"), "w", encoding="utf-8").write(src)
    boot = stem + ".binary"
    subprocess.run([args.openspin, "-q", "-b", "-I", HERE, "-I", args.xmem_include, "-I", tmp,
                    "-o", boot, os.path.join(tmp, "xmm_boot.spin")], check=True)
    shutil.rmtree(tmp, ignore_errors=True)
    print("%s: image %d bytes at $%08X, RAM space at SRAM $%05X, hub RAM to $%04X (%d bytes for "
          "the stack), boot %d bytes" % (args.elf, len(image), base, ram_base, hub_end, MBOX - hub_end,
                                         os.path.getsize(boot)))


if __name__ == "__main__":
    main()

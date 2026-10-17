# P1 ROM

`p1rom.bin` is the ROM of the Parallax Propeller P1 (P8X32A): 32 KB,
hub addresses $8000-$FFFF, little-endian longs.

| Address     | Contents                                       |
| ----------- | ---------------------------------------------- |
| $8000-$BFFF | Character font                                 |
| $C000-$DFFF | Log and antilog tables                         |
| $E000-$F001 | Sine table                                     |
| $F004       | Spin interpreter (scrambled, as in the chip)   |
| $F800       | Boot loader (scrambled)                        |
| $FF00       | Copyright message                              |
| $FF70       | Runner (Spin bytecode)                         |

## Origin and license

- **Author:** Parallax Inc., Propeller 1 Design, copyright 2014.
- **License:** GNU General Public License, version 3 or (at your option)
  any later version. The full text is in [`../../LICENSE`](../../LICENSE).
- **Source:** the P1V repository, <https://github.com/MicroNut/P1V>, at
  commit `feada86ade7a5a0dcabaf90a990483c243f7f4f9`, files
  `HDL/hub_rom_low.hex` ($8000-$BFFF) and `HDL/hub_rom_high.hex`
  ($C000-$FFFF), the ROM contents of Parallax's Verilog release.
- **Conversion:** `emu/tools/get_p1_rom.py` downloads those two files and
  writes them as one binary image, without changing any data. SHA-256 of
  the result: `f83da048d8e1f73fc04657ff6087a16200cb27d5cfae47a96a093294f0262488`.
  Run the script again to check or rebuild it.

The Spin interpreter and the boot loader are stored scrambled, as in the
real chip; the emulator unscrambles them when a cog loads them, with the
bit permutation of the hub (`HDL/hub.v` in P1V). Their sources are
`Spin/interpreter.spin` and `Spin/booter.spin` in the same repository.

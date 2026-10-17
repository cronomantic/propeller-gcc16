CON
{{
  xmm_boot.spin -- Starts a PropGCC XMMC or XMM program whose image is
  already in the external memory (a 23LC1024 SPI SRAM, which p1run
  preloads with --spi-sram), as propeller-load's flash_loader2.spin does
  after the loader has written it:
    - the 8 KB cache, its tags and the driver's mailboxes at the top of
      hub RAM;
    - the external memory driver (sram_xmem.spin) in a new cog, with its
      pins and the SRAM address of the kernel's RAM space;
    - the XMM kernel (the program's .xmmkernel section, xmmkernel.bin,
      written by mkxmm.py) in this cog, with the mailboxes, the tags, the
      cache, its geometry and the start address on its stack.
  mkxmm.py sets CODE_BASE and RAM_BASE for each program.
  Based on flash_loader2.spin of propeller-load (David Betz, MIT).
  MIT License
}}
    _CLKMODE = XTAL1 + PLL16X
    _XINFREQ = 5_000_000
    HUB_SIZE   = 32 * 1024
    CODE_BASE  = $3000_0000     ' where the program starts
    RAM_BASE   = 0              ' SRAM address of $20000000
    INDEX_BITS = 7              ' 128 lines ...
    LINE_BITS  = 6              ' ... of 64 bytes: 8 KB
    MBOXES     = 8              ' one per cog, 2 longs each
    MBOX_END   = %1000
    ' the SRAM's pins, as p1run wires them
    PIN_CS     = 0
    PIN_SCK    = 1
    PIN_MOSI   = 2
    PIN_MISO   = 3
    CS_CLR_PIN_MASK = $01       ' xmem_spi_pins.spin: byte aa of param2 is /CS

OBJ
    xmem : "sram_xmem"

PUB start | cacheLines, cacheTags, mbox, sp, drv
    cacheLines := HUB_SIZE - (1 << (INDEX_BITS + LINE_BITS))
    cacheTags := cacheLines - (1 << INDEX_BITS) * 4
    mbox := cacheTags - MBOXES * 2 * 4 - 4
    longfill(mbox, 0, MBOXES * 2)
    long[mbox][MBOXES * 2] := MBOX_END
    ' xmem_param1 and xmem_param2 of xmem_common.spin, as propeller-load
    ' fills them from the board's configuration
    drv := xmem.image
    long[drv][1] := PIN_MOSI << 24 | PIN_MISO << 16 | PIN_SCK << 8 | CS_CLR_PIN_MASK
    long[drv][2] := PIN_CS << 24
    xmem.ram_base(RAM_BASE)
    cognew(drv, mbox)
    ' the kernel pops: mailboxes, tags, cache lines, geometry, pc
    sp := mbox
    long[sp -= 4] := CODE_BASE
    long[sp -= 4] := INDEX_BITS << 8 | LINE_BITS
    long[sp -= 4] := cacheLines
    long[sp -= 4] := cacheTags
    long[sp -= 4] := mbox
    coginit(cogid, @kernel, sp)

DAT
                    long    0                       ' (long alignment)
kernel              file    "xmmkernel.bin"

{
  SPI SRAM External Memory Driver (24 bit addresses)
  Copyright (c) 2013 by David Betz

  propeller-gcc16's tests/xmm copy of propeller-load's spi_sram24_xmem.spin,
  for one 23LC1024 holding both of the kernel's spaces:
    - the flash space ($3xxxxxxx: code in XMMC, XMM and XMM split) starts
      at SRAM address 0, the RAM space ($2xxxxxxx: everything in XMM
      single, data in XMM and XMM split) at ram_base, which the boot
      program sets before starting the driver;
    - /CS goes high after the mode register is written: the chip only
      takes a command when /CS rises, and ignores the bits that follow
      until then, so the first transfer was lost.
  
  Based on code from Chip Gracey's Propeller II SDRAM Driver
  Copyright (c) 2013 by Chip Gracey

  TERMS OF USE: MIT License

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
}

#include "xmem_common.spin"
#include "xmem_spi_pins.spin"
#include "xmem_spi.spin"

PUB ram_base(addr)
'' SRAM address of the kernel's RAM space ($20000000); before image is started
  ram_offset := addr

DAT

init
        
        ' build the pin masks
        call    #get_spi_pins
  
        ' set the pin directions
        mov     outa, pinout
        mov     dira, pindir
        call    #release
        
        ' select sequential access mode for the SRAM chip
        call    #select
        mov     data, ramseq
        mov     bits, #16
        call    #spiSend
        call    #release
        
init_ret
        ret
        
'----------------------------------------------------------------------------------------------------
'
' read_write_init
'
' select the chip and send the address for the read/write operation
'
' on input:
'   extaddr is the sram transfer address
'
'----------------------------------------------------------------------------------------------------

read_write_init
        call    #select          ' select SRAM chip
        mov     data, extaddr
        test    extaddr, flash_bit wz   ' RAM space: from ram_base
  if_z  add     data, ram_offset
        and     data, sram_mask
        or      data, fn
        mov     bits, #32
        call    #spiSend
read_write_init_ret
        ret

'----------------------------------------------------------------------------------------------------
'
' read_bytes
'
' on input:
'   extaddr is the external memory address to read
'   hubaddr is the hub memory address to write
'   count is the number of bytes to read
'
'----------------------------------------------------------------------------------------------------

read_bytes
        mov     fn, read
        call    #read_write_init 
:loop   call    #spiRecvByte
        wrbyte  data, hubaddr
        add     hubaddr, #1
        djnz    count, #:loop
        call    #release
read_bytes_ret
        ret

'----------------------------------------------------------------------------------------------------
'
' write_bytes
'
' on input:
'   extaddr is the external memory address to write
'   hubaddr is the hub memory address to read
'   count is the number of bytes to write
'
'----------------------------------------------------------------------------------------------------

write_bytes
        mov     fn, write
        call    #read_write_init
:loop   rdbyte  data, hubaddr
        call    #spiSendByte
        add     hubaddr, #1
        djnz    count, #:loop
        call    #release
write_bytes_ret
        ret
        
fn      long    0

pindir      long    0
pinout      long    0

' variables used by the spi send/receive functions
cmd         long    0
bytes       long    0

' spi commands
read        long    $03000000       ' read command
write       long    $02000000       ' write command
ramseq      long    $01400000       ' %00000001_01000000 << 16 ' set sequential mode

' mask off the base address of external memory
sram_mask   long    $00ffffff
flash_bit   long    $10000000       ' set in $3xxxxxxx, clear in $2xxxxxxx
ram_offset  long    0

            fit     496

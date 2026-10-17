CON
{{
  serial_tx.spin -- Minimal transmit-only serial port written in Spin.

  Used by gen_pasm_vectors.py to report its results (P30 = TX).  No COG
  is needed: it bit-bangs from the calling COG with WAITCNT, which is
  reliable up to 9600 baud from Spin at 114 MHz.
}}
VAR
    long pin, bit_ticks

PUB start(txPin, baud)
    pin := txPin
    bit_ticks := clkfreq / baud
    outa[pin] := 1
    dira[pin] := 1
    waitcnt(cnt + clkfreq / 100)                ' let the line idle high

PUB tx(c) | t
    c := (c | $100) << 1                        ' start bit (0), 8 data bits, stop bit (1)
    t := cnt
    repeat 10
        outa[pin] := c & 1
        c >>= 1
        waitcnt(t += bit_ticks)

PUB str(addr)
    repeat strsize(addr)
        tx(byte[addr++])

PUB dec(value) | i, started
    if value < 0
        tx("-")
        ||value
    i := 1_000_000_000
    started := false
    repeat 10
        if value => i
            tx(value / i + "0")
            value //= i
            started := true
        elseif started or i == 1
            tx("0")
        i /= 10

PUB hex(value, digits)
    value <<= (8 - digits) << 2
    repeat digits
        tx(lookupz((value <-= 4) & $F : "0".."9", "A".."F"))

PUB crlf
    tx(13)
    tx(10)

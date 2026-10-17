/* exit.c -- Linked into every torture test: exit() and abort() end with
   propgcc's serial exit sequence ($FF $00 status, as drivers/serial_exit.c
   in the C library sends it), which p1run turns into its exit status.
   abort() exits with 255.  Defined here rather than taken from the
   library, where serial_exit.o would only be linked if something asked
   for it; sent with _serial_tx, so that a status of 10 is not preceded
   by a carriage return as _serial_putchar would do.  */
#include <propeller.h>
#include <sys/serial.h>

extern unsigned int _txpin;
extern unsigned int _baud;

void
_ExitHook (int n)
{
  unsigned int mask = 1U << _txpin;
  unsigned int bit = _clkfreq / _baud;

  _serial_tx (0xff, mask, bit);
  _serial_tx (0x00, mask, bit);
  _serial_tx (n & 0xff, mask, bit);
  __builtin_propeller_cogstop (__builtin_propeller_cogid ());
}

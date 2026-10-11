/* Rim on the RP2350 (Adafruit Fruit Jam): for now, a rio REPL over USB serial. Video (DVI over HSTX,
   the screen double-buffered by DMA at vblank), USB host input, SD and audio come next.

   Memory, of the RP2350's 520 KB of SRAM:
     app space            256 KB  (rio: the program's code and data; the compiler borrows it while
                                   compiling; the REPL keeps a quarter of it for code)
     REPL compiler tables  32 KB  (only while a REPL session is open)
     screen                75 KB  (one buffer so far; the DMA copy for video adds another 75 KB)
     rio's interpreter     10 KB  (its loop runs from SRAM: see RIM_RIO_IN_RAM) */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "rim.h"

#define RIO_MEM (256 << 10)
#define REPL_SCRATCH (32 << 10)

static _Alignas(16) uint8_t rio_mem[RIO_MEM];
static _Alignas(16) uint8_t repl_scratch[REPL_SCRATCH];
static Rio vm;
static RimRepl repl;

static void put(const char *s, int n) { fwrite(s, 1, (size_t)n, stdout); fflush(stdout); }
static float now(void) { return (float)((double)time_us_64() / 1e6); }

int main(void) {
  set_sys_clock_khz(252000, true); /* 10x the 25.2 MHz pixel clock of 640x480 DVI; fine at the default core voltage */
  stdio_init_all();
  while (!stdio_usb_connected()) sleep_ms(50); /* wait for a terminal on the USB serial port */
  rim_time = now;
  if (rim_start(&vm, rio_mem, sizeof rio_mem, put) || rim_repl_begin(&repl, &vm, repl_scratch, sizeof repl_scratch)) {
    printf("rim: can't start: %s\r\n", rio_error(&vm));
    for (;;) sleep_ms(1000);
  }
  for (;;) rim_repl_char(&repl, getchar());
}

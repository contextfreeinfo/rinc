/* Rim on the RP2350 (Adafruit Fruit Jam): for now, a rio REPL over USB serial. Video (DVI over HSTX,
   the screen double-buffered by DMA at vblank), USB host input, SD and audio come next.

   Memory, of the RP2350's 520 KB of SRAM:
     rio memory           256 KB  (the compiler borrows it while compiling, then the program has it)
     bytecode              64 KB  (8 bytes an instruction; likely moves to PSRAM, to make room)
     REPL compiler tables  32 KB  (only while a REPL session is open)
     screen                75 KB  (one buffer so far; the DMA copy for video adds another 75 KB) */
#include <stdio.h>
#include "pico/stdlib.h"
#include "rim.h"

#define RIO_MEM (256 << 10)
#define RIO_CODE 8192
#define REPL_SCRATCH (32 << 10)

static _Alignas(16) uint8_t rio_mem[RIO_MEM];
static RioIns rio_code[RIO_CODE];
static _Alignas(16) uint8_t repl_scratch[REPL_SCRATCH];
static Rio vm;
static RimRepl repl;

static void put(const char *s, int n) { fwrite(s, 1, (size_t)n, stdout); fflush(stdout); }
static float now(void) { return (float)((double)time_us_64() / 1e6); }

int main(void) {
  stdio_init_all();
  while (!stdio_usb_connected()) sleep_ms(50); /* wait for a terminal on the USB serial port */
  rim_time = now;
  if (rim_start(&vm, rio_mem, sizeof rio_mem, rio_code, RIO_CODE, put) || rim_repl_begin(&repl, &vm, repl_scratch, sizeof repl_scratch)) {
    printf("rim: can't start: %s\r\n", rio_error(&vm));
    for (;;) sleep_ms(1000);
  }
  for (;;) rim_repl_char(&repl, getchar());
}

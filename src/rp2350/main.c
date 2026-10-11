/* Rim on the RP2350 (Adafruit Fruit Jam): for now, a rio REPL over USB serial. Video (DVI over HSTX,
   the screen double-buffered by DMA at vblank), USB host input, SD and audio come next.

   Memory, of the RP2350's 520 KB of SRAM:
     app space            256 KB  (rio: the program's code and data; the compiler borrows it while
                                   compiling; the REPL keeps a quarter of it for code)
     REPL compiler tables  32 KB  (only while a REPL session is open)
     screen                75 KB  (one buffer so far; the DMA copy for video adds another 75 KB)
     rio's interpreter     10 KB  (its loop runs from SRAM: see RIM_RIO_IN_RAM)
     native loops          16 KB  (rio_jit: loops of integer math compiled to Thumb-2) */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "rim.h"
#include "video.h"
#include "keyboard.h"
#include "tusb.h" /* (HID key codes) */

#define RIO_MEM (256 << 10)
#define REPL_SCRATCH (32 << 10)

static _Alignas(16) uint8_t rio_mem[RIO_MEM];
static _Alignas(8) uint8_t jit_code[16 << 10]; /* simple loops, compiled to machine code (SRAM runs code) */
static _Alignas(16) uint8_t repl_scratch[REPL_SCRATCH];
static Rio vm;
static RimRepl repl;

/* output goes to the serial port, and to the screen unless a game has it */
static void put(const char *s, int n) { fwrite(s, 1, (size_t)n, stdout); fflush(stdout); if (!repl.run) rim_console_put(s, n); }
#define DOWN(k) (rim_keys_down[(k) >> 3] >> ((k) & 7) & 1)

/* a game: a frame at each vertical blank, right after the screen was copied for showing */
static void game_frame(void) {
  static uint32_t last; int res;
  while (rim_frames == last) rim_keyboard_char(); /* (keeps the USB host going; typing waits) */
  last = rim_frames;
  if (DOWN(HID_KEY_ESCAPE) || (res = getchar_timeout_us(0)) == 27 || res == 3) { /* Esc, on the keyboard or over serial (or Ctrl-C) */
    rim_console_put("\r\n", 2); rim_repl_stop(&repl, "stopped"); return;
  }
  rim_buttons = (uint8_t)(DOWN(HID_KEY_ARROW_LEFT) * RIM_LEFT | DOWN(HID_KEY_ARROW_RIGHT) * RIM_RIGHT |
    DOWN(HID_KEY_ARROW_UP) * RIM_UP | DOWN(HID_KEY_ARROW_DOWN) * RIM_DOWN | (DOWN(HID_KEY_Z) | DOWN(HID_KEY_C)) * RIM_O |
    (DOWN(HID_KEY_X) | DOWN(HID_KEY_V)) * RIM_X);
  res = rim_frame(&vm);
  if (res) { repl.run = 0; rim_repl_stop(&repl, res > 0 ? "nothing to run: define update* or draw*" : rio_error(&vm)); }
}
static float now(void) { return (float)((double)time_us_64() / 1e6); }

int main(void) {
  int ok;
  set_sys_clock_khz(252000, true); /* 10x the 25.2 MHz pixel clock of 640x480 DVI; fine at the default core voltage */
  rim_keyboard_setup();
  stdio_init_all();
  rim_time = now;
  ok = !rim_start(&vm, rio_mem, sizeof rio_mem, put) && rio_jit(&vm, jit_code, sizeof jit_code) >= 0;
  rim_console_clear(1);
  rim_console_put("Rim\r\n\r\n", 7);
  rim_video_start();
  rim_keyboard_start();
  if (!ok || rim_repl_begin(&repl, &vm, repl_scratch, sizeof repl_scratch)) {
    for (;;) { printf("rim: can't start: %s\r\n", rio_error(&vm)); sleep_ms(1000); }
  }
  for (;;) { /* the REPL takes typing from a USB keyboard and from the USB serial port */
    static int was_connected;
    if (repl.run) { game_frame(); continue; }
    int c = rim_keyboard_char(), connected = stdio_usb_connected();
    if (connected && !was_connected) { sleep_ms(100); printf("%s", rim_keyboard_events()); } /* a terminal arrived */
    was_connected = connected;
    if (c >= 0) rim_repl_char(&repl, c);
    if ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) rim_repl_char(&repl, c);
  }
}

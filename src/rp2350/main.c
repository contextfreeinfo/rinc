/* Rim on the RP2350 (Adafruit Fruit Jam): for now, a rio REPL over USB serial. Video (DVI over HSTX,
   the screen double-buffered by DMA at vblank), USB host input, SD and audio come next.

   Memory, of the RP2350's 520 KB of SRAM:
     app space            256 KB  (rio: the program's code and data; the compiler borrows it while
                                   compiling. The REPL takes its top 32 KB for its compiler tables,
                                   and a quarter of the rest for code, as code keeps coming)
     screen               150 KB  (drawn into, and the copy being shown)
     native loops          16 KB  (rio_jit: loops compiled to Thumb-2)
     rio's interpreter     10 KB  (its loop runs from SRAM: see RIM_RIO_IN_RAM)
     USB, console, stacks  the rest */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "rim.h"
#include "video.h"
#include "keyboard.h"
#include "gamepad.h"
#include "rim_audio.h"
#include "tusb.h" /* (HID key codes) */

#define RIO_MEM (256 << 10)      /* the app space */
#define REPL_SCRATCH (32 << 10)  /* the REPL's compiler tables: the app space's top, while the REPL runs */

static _Alignas(16) uint8_t rio_mem[RIO_MEM];
static _Alignas(8) uint8_t jit_code[16 << 10]; /* simple loops, compiled to machine code (SRAM runs code) */
static Rio vm;
static RimRepl repl;

/* output goes to the serial port, and to the screen unless a game has it */
static void put(const char *s, int n) { fwrite(s, 1, (size_t)n, stdout); fflush(stdout); if (!repl.run) rim_console_put(s, n); }
#define DOWN(k) (rim_keys_down[(k) >> 3] >> ((k) & 7) & 1)

/* a game: a frame at each vertical blank, right after the screen was copied for showing */
static void game_frame(void) {
  static uint32_t last; int res;
  while (rim_frames == last) { int c = rim_keyboard_char(); if (c >= 0 && c != 27) rim_text_push(c); } /* (typing goes to key()) */
  last = rim_frames;
  while ((res = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT && res != 27 && res != 3) rim_text_push(res); /* (serial typing too) */
  if (DOWN(HID_KEY_ESCAPE) || res == 27 || res == 3) { /* Esc, on the keyboard or over serial (or Ctrl-C) */
    rim_console_put("\r\n", 2); rim_repl_stop(&repl, "stopped"); return;
  }
  rim_pad_read();
  rim_buttons = rim_pad_buttons | (uint16_t)(DOWN(HID_KEY_ARROW_LEFT) * RIM_LEFT | DOWN(HID_KEY_ARROW_RIGHT) * RIM_RIGHT |
    DOWN(HID_KEY_ARROW_UP) * RIM_UP | DOWN(HID_KEY_ARROW_DOWN) * RIM_DOWN | DOWN(HID_KEY_V) * RIM_A |
    DOWN(HID_KEY_C) * RIM_B | DOWN(HID_KEY_X) * RIM_X | DOWN(HID_KEY_Z) * RIM_Y);
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
  ok = !rim_start(&vm, rio_mem, RIO_MEM - REPL_SCRATCH, put) && rio_jit(&vm, jit_code, sizeof jit_code) >= 0;
  rim_console_clear(1);
  rim_console_put("Rim\r\n\r\n", 7);
  rim_audio_setup();
  rim_video_start(); /* (core 1: video, then sound) */
  rim_keyboard_start();
  if (!ok || rim_repl_begin(&repl, &vm, rio_mem + RIO_MEM - REPL_SCRATCH, REPL_SCRATCH)) {
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

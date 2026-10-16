/* Rinc on the RP2350 (Adafruit Fruit Jam): for now, a rio REPL over USB serial. Video (DVI over HSTX,
   the screen double-buffered by DMA at vblank), USB host input, SD and audio come next.

   Memory, of the RP2350's 520 KB of SRAM:
     app space            256 KB  (rio: the program's code and data; the compiler borrows it while
                                   compiling. The REPL takes its top 32 KB for its compiler tables,
                                   and a quarter of the rest for code, as code keeps coming)
     screen               150 KB  (drawn into, and the copy being shown)
     native loops          16 KB  (rio_jit: loops compiled to Thumb-2)
     rio's interpreter     10 KB  (its loop runs from SRAM: see RINC_RIO_IN_RAM)
     USB, console, stacks  the rest */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "rinc.h"
#include "video.h"
#include "keyboard.h"
#include "gamepad.h"
void rinc_sdSetup(void); /* sd.c: the SD card, as rinc_fs */
extern const char rinc_launcherSrc[]; extern const int rinc_launcherSrc_len; /* system/launcher.rio, built in */
#include "rinc_audio.h"
#include "tusb.h" /* (HID key codes) */

#define rinc_APP_SPACE (256 << 10)    /* the app space */
#define rinc_REPL_SCRATCH (32 << 10)  /* the REPL's compiler tables: the app space's top, while the REPL runs */

static _Alignas(16) uint8_t rinc_appSpace[rinc_APP_SPACE];
static _Alignas(8) uint8_t rinc_jitCode[16 << 10]; /* simple loops, compiled to machine code (SRAM runs code) */
static rio_Vm rinc_vm;
static rinc_Repl rinc_repl;

/* output goes to the serial port, and to the screen unless a game has it */
static void rinc_put(const char *s, int n) { fwrite(s, 1, (size_t)n, stdout); fflush(stdout); if (!rinc_repl.run) rinc_consolePut(s, n); }
#define rinc_DOWN(k) (rinc_keysDown[(k) >> 3] >> ((k) & 7) & 1)

static int rinc_restart(void);
/* home: the launcher, in a fresh session (the system's way out of anything) */
static void rinc_home(void) {
  rinc_audioReset(); rinc_consoleClear(1);
  if (rinc_replSource(&rinc_repl, rinc_launcherSrc, rinc_launcherSrc_len)) rinc_replStop(&rinc_repl, "the launcher didn't start");
}
static int rinc_systemKey(void) { /* the Windows key, or a controller's Xbox button: pressed just now */
  static int was; int now = rinc_DOWN(HID_KEY_GUI_LEFT) || rinc_DOWN(HID_KEY_GUI_RIGHT) || rinc_padGuide, pressed = now && !was;
  was = now;
  return pressed;
}
/* a game: a frame at each vertical blank, right after the screen was copied for showing */
static void rinc_gameFrame(void) {
  static uint32_t last; int res;
  while (rinc_frames == last) { /* (typing goes to key(); editing keys, sent as ESC [ ..., are buttons instead) */
    int c = rinc_keyboardChar();
    if (c == 27) { /* (Esc itself, for the app; or the start of an editing key's escape, which isn't typing) */
      c = rinc_keyboardChar();
      if (c != '[') { rinc_textEsc(); if (c >= 0) rinc_textPush(c); continue; }
      do c = rinc_keyboardChar(); while (c >= 0 && !((c >= 'A' && c <= 'Z') || c == '~'));
      continue;
    }
    if (c >= 0) rinc_textPush(c); /* (Ctrl+C is the app's too: copy, someday) */
  }
  last = rinc_frames;
  while ((res = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT && res != 3) rinc_textPush(res); /* (serial typing too) */
  rinc_padRead();
  if (res == 3) { /* Ctrl-C over serial (a developer's tools, such as send.py; not the keyboard's): stop, into the console, with what was running still there */
    rinc_audioReset(); rinc_consoleClear(1); rinc_replStop(&rinc_repl, "stopped"); return;
  }
  if (rinc_systemKey()) { /* home: the Windows key, or a controller's Xbox button (Esc is the app's) */ /* home: the system key, or (for now) Esc, on the keyboard or over serial */
    rinc_home(); return;
  }
  rinc_buttons = rinc_padButtons | (uint16_t)(rinc_DOWN(HID_KEY_ARROW_LEFT) * rinc_Button_left | rinc_DOWN(HID_KEY_ARROW_RIGHT) * rinc_Button_right |
    rinc_DOWN(HID_KEY_ARROW_UP) * rinc_Button_up | rinc_DOWN(HID_KEY_ARROW_DOWN) * rinc_Button_down | rinc_DOWN(HID_KEY_V) * rinc_Button_a |
    rinc_DOWN(HID_KEY_C) * rinc_Button_b | rinc_DOWN(HID_KEY_X) * rinc_Button_x | rinc_DOWN(HID_KEY_Z) * rinc_Button_y);
  res = rinc_frame(&rinc_vm);
  if (res) { rinc_repl.run = 0; rinc_replStop(&rinc_repl, res > 0 ? "nothing to run: define update* or draw*" : rio_error(&rinc_vm)); }
  if (rinc_launch) { /* the launcher chose */
    int which = rinc_launch; rinc_launch = 0;
    rinc_audioReset(); rinc_consoleClear(1);
    if (which == 1 && rinc_replApp(&rinc_repl, rinc_launchName, 1)) { rinc_consolePut("\r\n(Windows key: back to the launcher)\r\n", 39); rinc_replStop(&rinc_repl, 0); }
    if (which == 2) { rinc_repl.run = 0; if (!rinc_restart()) rinc_consolePut("Console: rio, typed and run. The Windows key goes back.\r\n", 56); rinc_replStop(&rinc_repl, 0); }
  }
}
static float rinc_now(void) { return (float)((double)time_us_64() / 1e6); }
/* a fresh session, for the REPL's load: the app space as at the start */
static int rinc_restart(void) {
  return rinc_start(&rinc_vm, rinc_appSpace, rinc_APP_SPACE - rinc_REPL_SCRATCH, rinc_put) || rio_jit(&rinc_vm, rinc_jitCode, sizeof rinc_jitCode) < 0 ||
         rio_replBegin(&rinc_vm, rinc_appSpace + rinc_APP_SPACE - rinc_REPL_SCRATCH, rinc_REPL_SCRATCH, 0);
}

int main(void) {
  int ok;
  set_sys_clock_khz(252000, true); /* 10x the 25.2 MHz pixel clock of 640x480 DVI; fine at the default core voltage */
  rinc_keyboardSetup();
  stdio_init_all();
  rinc_time = rinc_now;
  ok = !rinc_start(&rinc_vm, rinc_appSpace, rinc_APP_SPACE - rinc_REPL_SCRATCH, rinc_put) && rio_jit(&rinc_vm, rinc_jitCode, sizeof rinc_jitCode) >= 0;
  rinc_consoleClear(1);
  rinc_consolePut("Rinc\r\n\r\n", 8);
  rinc_audioSetup();
  rinc_videoStart(); /* (core 1: video, then sound) */
  rinc_keyboardStart();
  rinc_sdSetup();
  if (!ok || rinc_replBegin(&rinc_repl, &rinc_vm, rinc_appSpace + rinc_APP_SPACE - rinc_REPL_SCRATCH, rinc_REPL_SCRATCH)) {
    for (;;) { printf("rinc: can't start: %s\r\n", rio_error(&rinc_vm)); sleep_ms(1000); }
  }
  rinc_repl.restart = rinc_restart;
  rinc_home(); /* (the board starts in the launcher) */
  for (;;) { /* the REPL takes typing from a USB keyboard and from the USB serial port */
    static int was_connected;
    if (rinc_repl.run) { rinc_gameFrame(); continue; }
    rinc_padRead();
    if (rinc_systemKey()) { rinc_home(); continue; }
    int c = rinc_keyboardChar(), connected = stdio_usb_connected();
    if (connected && !was_connected) { sleep_ms(100); printf("%s", rinc_keyboardEvents()); } /* a terminal arrived */
    was_connected = connected;
    if (c >= 0) rinc_replChar(&rinc_repl, c);
    if ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) rinc_replChar(&rinc_repl, c);
    if (rinc_launch == 3) { rinc_launch = 0; rinc_home(); }
  }
}

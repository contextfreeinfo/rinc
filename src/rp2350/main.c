/* Rinc on the RP2350 (Adafruit Fruit Jam): it runs one app at a time, starting at home. Every
   app, home and the console included, is an app compiled whole into the app space
   (docs/spec/running.md); the system key, or Ctrl-C over USB serial, switches.

   Memory, of the RP2350's 520 KB of SRAM:
     app space            256 KB  (Rio: an app's code and data; an app's source, read from the SD card,
                                   sits at its top while it compiles, where its data will be zero)
     screen               150 KB  (drawn into, and the copy being shown)
     native loops          16 KB  (rio_jit: loops compiled to Thumb-2)
     rio's interpreter     10 KB  (its loop runs from SRAM: see RINC_RIO_IN_RAM)
     USB, console, stacks  the rest */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "rinc.h"
#include "video.h"
#include "keyboard.h"
#include "gamepad.h"
void rinc_sdSetup(void); /* sd.c: the SD card, as rinc_fs */
extern const uint8_t rinc_firmwareBundle[]; extern const uint32_t rinc_firmwareBundleSize; /* (bundle.c, from RINC_BUNDLE) */
#include "rinc_audio.h"
#include "tusb.h" /* (HID key codes) */

#define rinc_APP_SPACE (256 << 10) /* the app space */

static _Alignas(16) uint8_t rinc_appSpace[rinc_APP_SPACE];
static _Alignas(8) uint8_t rinc_jitCode[16 << 10]; /* simple loops, compiled to machine code (SRAM runs code) */
static rio_Vm rinc_vm;
/* log output: to the serial port (and, while an app starts and once it's stopped, the screen: rinc_say) */
static void rinc_put(const char *s, int n) { fwrite(s, 1, (size_t)n, stdout); fflush(stdout); }
#define rinc_DOWN(k) (rinc_keysDown[(k) >> 3] >> ((k) & 7) & 1)

/* the system key (rinc_systemKey): the Windows key, a controller's Xbox button, or Shift tapped
   twice. With Esc (or the controller's Back), Tab, Q or P: 1 to start rinc_launchName */
static int rinc_systemKeys(void) {
  int k, other = rinc_padButtons != 0;
  int keys = (rinc_DOWN(HID_KEY_ESCAPE) || (rinc_padButtons & rinc_Button_back) ? rinc_SysKey_esc : 0) | (rinc_DOWN(HID_KEY_TAB) ? rinc_SysKey_tab : 0) |
             (rinc_DOWN(HID_KEY_Q) ? rinc_SysKey_q : 0) | (rinc_DOWN(HID_KEY_P) ? rinc_SysKey_p : 0) |
             (rinc_DOWN(HID_KEY_ENTER) || rinc_DOWN(HID_KEY_KEYPAD_ENTER) ? rinc_SysKey_enter : 0) |
             (rinc_padButtons & rinc_Button_rb ? rinc_SysKey_next : 0) | (rinc_padButtons & rinc_Button_lb ? rinc_SysKey_prev : 0);
  for (k = 0; k < 32; k++) other |= k == (0xE0 >> 3) ? rinc_keysDown[k] & ~0xAA : rinc_keysDown[k]; /* (all but Shift and GUI: E1 E3 E5 E7) */
  return rinc_systemKey(rinc_DOWN(HID_KEY_GUI_LEFT) || rinc_DOWN(HID_KEY_GUI_RIGHT) || rinc_padGuide,
                        rinc_DOWN(HID_KEY_SHIFT_LEFT) || rinc_DOWN(HID_KEY_SHIFT_RIGHT), other, keys);
}

/* an app's source: from the SD card (read into the app space's top), or else the firmware's bundle.
   Its length, or -1 (said why) */
static int rinc_source(const char *name, const char **src) {
  char *at = (char *)rinc_appSpace + rinc_APP_SPACE - rinc_MAX_SOURCE; const rinc_App *b; int n = rinc_readApp(name, at, rinc_MAX_SOURCE);
  if (n == -2) { rinc_appFail(name, ".rio: too big (64 KB is the most)"); return -1; }
  if (n >= 0) {
    memmove(rinc_appSpace + rinc_APP_SPACE - n, at, (size_t)n); /* (right at the top: all below is the compiler's) */
    *src = (const char *)rinc_appSpace + rinc_APP_SPACE - n;
    return n;
  }
  if ((b = rinc_bundled(name))) { *src = b->data; return (int)(b->len ? b->len : strlen(b->data)); }
  rinc_appFail(name, ": no app of this name");
  return -1;
}

/* an app, by name, compiled whole and started (rinc_appBegin, rinc_appRun): its top-level code, then
   main; then, if it's a game, its frames. What goes wrong stays on the screen */
static void rinc_open(const char *name) {
  const char *src; int n;
  name = rinc_appFor(name); /* (home: the card's, else the bundle's) */
  if (rinc_appBegin(&rinc_vm, rinc_appSpace, rinc_APP_SPACE, name, rinc_put)) return;
  if (rio_jit(&rinc_vm, rinc_jitCode, sizeof rinc_jitCode) < 0) { rinc_appFail(name, ": can't set up native code"); return; }
  if ((n = rinc_source(name, &src)) < 0) return;
  rinc_appRun(&rinc_vm, rinc_appSpace, rinc_APP_SPACE, name, src, (uint32_t)n, 0);
}

/* typing, from the USB keyboard and the serial port, for pc.key(); Ctrl-C over serial (a developer's
   tools, such as send.py; not the keyboard's) opens the console, in place of whatever's running */
static int rinc_type(void) {
  int c;
  while (rinc_textRoom() > 8 && (c = rinc_keyboardChar()) >= 0) {
    if (rinc_systemHeld()) continue; /* (a combination with the system key: not the app's typing) */
    if (c == 27) { /* (Esc itself, or the start of an editing key's escape: ESC [ ...) */
      int d = rinc_keyboardChar();
      if (d == '[') { rinc_textPush(27); rinc_textPush('['); continue; }
      rinc_textEsc();
      c = d;
      if (c < 0) continue;
    }
    rinc_textPush(c);
  }
  while (rinc_textRoom() > 8 && (c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
    if (c == 3) return 1;
    rinc_textPush(c);
  }
  return 0;
}

/* a frame, at each vertical blank, right after the screen was copied for showing */
static void rinc_frameNow(void) {
  static uint32_t last; static int wasConnected; int stop = 0, connected;
  while (rinc_frames == last) { rinc_keyboardPoll(); stop |= rinc_type(); } /* (the USB host runs all the while) */
  last = rinc_frames;
  stop |= rinc_type();
  if ((connected = stdio_usb_connected()) && !wasConnected) printf("%s", rinc_keyboardEvents()); /* (a terminal arrived: what's plugged in) */
  wasConnected = connected;
  rinc_padRead();
  if (stop) { strcpy(rinc_launchName, "console"); rinc_launch = 1; }
  if (rinc_systemKeys()) rinc_launch = 1; /* (the one key no app gets: to rinc_launchName) */
  if (rinc_launch) { rinc_launch = 0; rinc_open(rinc_launchName); return; }
  if (!rinc_game) return;
  rinc_buttons = rinc_padButtons | (uint16_t)(rinc_DOWN(HID_KEY_ARROW_LEFT) * rinc_Button_left | rinc_DOWN(HID_KEY_ARROW_RIGHT) * rinc_Button_right |
    rinc_DOWN(HID_KEY_ARROW_UP) * rinc_Button_up | rinc_DOWN(HID_KEY_ARROW_DOWN) * rinc_Button_down | rinc_DOWN(HID_KEY_V) * rinc_Button_a |
    rinc_DOWN(HID_KEY_C) * rinc_Button_b | rinc_DOWN(HID_KEY_X) * rinc_Button_x | rinc_DOWN(HID_KEY_Z) * rinc_Button_y);
  if (rinc_systemHeld()) rinc_buttons = 0; /* (a system combination: not the app's) */
  rinc_gameFrame(&rinc_vm); /* (an error goes on the screen) */
  if (rinc_launch) { rinc_launch = 0; rinc_open(rinc_launchName); } /* (pc.launch: after the frame) */
}

static float rinc_now(void) { return (float)((double)time_us_64() / 1e6); }

int main(void) {
  set_sys_clock_khz(252000, true); /* 10x the 25.2 MHz pixel clock of 640x480 DVI; fine at the default core voltage */
  rinc_keyboardSetup();
  stdio_init_all();
  rinc_time = rinc_now;
  rinc_consoleClear(1);
  rinc_consolePut("Rinc\r\n\r\n", 8);
  rinc_audioSetup();
  rinc_videoStart(); /* (core 1: video, then sound) */
  rinc_keyboardStart();
  rinc_sdSetup();
  rinc_bundleRead(rinc_firmwareBundle, rinc_firmwareBundleSize); /* (home and the console, by default; the card's override them) */
  rinc_open("home"); /* (the board starts at home) */
  for (;;) rinc_frameNow();
}

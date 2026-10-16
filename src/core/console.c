/* text on the screen: 53 columns by 26 rows of 6x9 cells in the 5x8 font, a pixel in from the left
   and top (320 = 53 x 6 + 2: a pixel each side; 240 = 26 x 9 + 6: the bottom gets the other 5).
   Writing past the last row scrolls the whole screen up a row, drawings included, as on PICO-8. A
   line cursor marks where the next character goes: a pixel wide, in the gap just before it (each
   cell is 5 pixels of glyph and 1 of gap; at a line's start, the padding).
   It understands the few terminal escapes the REPL's line editor uses, as serial terminals do, so the
   same output works for both: ESC [ n m colors (30-37 and 90-97 as below; 38;5;n palette color n,
   so the PICO-8 colors by number; 0 or 39 back to the text color), ESC [ n D and C cursor left and right (left wraps back a row: long lines wrap), ESC [ K
   blanks the rest of the row, ESC [ J that and the rows below. Others are ignored. */
#include <string.h>
#include "rinc.h"

#define rinc_CW 6
#define rinc_CH 9
#define rinc_COLS (rinc_W / rinc_CW)
#define rinc_ROWS (rinc_H / rinc_CH)
#define rinc_X0 ((rinc_W - rinc_COLS * rinc_CW) / 2) /* (1: the padding) */
#define rinc_Y0 rinc_X0
#define rinc_CURSOR 10 /* yellow: clear on the dark blue, and apart from the text and highlighting colors */

static int rinc_cx, rinc_cy, rinc_shown; /* shown: the cursor is drawn */
uint8_t rinc_textFg = 7, rinc_textBg = 0;
static uint8_t rinc_ink = 7, rinc_under[rinc_CH]; /* ink: color now; under: the pixels beneath the cursor */
static int rinc_esc; /* escapes: 0 none, 1 ESC seen, 2 in ESC [ */
/* ANSI colors 0-7 (30-37) and bright (90-97), as their nearest PICO-8 colors: black, red, green,
   yellow, blue, pink (for magenta), blue (for cyan), light gray; bright: dark gray, red, green,
   yellow, blue, pink, blue, white */
static const uint8_t rinc_ansi[16] = {0, 8, 11, 10, 12, 14, 12, 6, 5, 8, 11, 10, 12, 14, 12, 7};
static int rinc_escp[3], rinc_nescp; /* an escape's numbers so far */

/* character c at pixel x, y, in fg; bg < 0 leaves the cell's other pixels alone */
void rinc_char(int c, int x, int y, int fg, int bg) {
  const uint8_t *g = c > 32 && c < 127 ? rinc_font[c - 32] : rinc_font[0];
  int r, k;
  for (r = 0; r < rinc_CH; r++) {
    int py = y + r, bits = r < 8 ? g[r] : 0;
    if (py < 0 || py >= rinc_H) continue;
    for (k = 0; k < rinc_CW; k++) {
      int px = x + k;
      if (px < 0 || px >= rinc_W) continue;
      if (bits >> k & 1) rinc_screen[py][px] = (uint8_t)fg;
      else if (bg >= 0) rinc_screen[py][px] = (uint8_t)bg;
    }
  }
}

static void rinc_cursor(int on) { /* a line in its own color, with the pixels beneath kept */
  int r, x = rinc_X0 + rinc_cx * rinc_CW - 1;
  if (on == rinc_shown) return;
  for (r = 0; r < rinc_CH - 1 && x >= 0; r++) { /* (as tall as the glyphs: the cell's last row is the gap below) */
    uint8_t *p = &rinc_screen[rinc_Y0 + rinc_cy * rinc_CH + r][x];
    if (on) { rinc_under[r] = *p; *p = rinc_CURSOR; } else *p = rinc_under[r];
  }
  rinc_shown = on;
}
static void rinc_blankRowFrom(int x, int y) { int r; for (r = 0; r < rinc_CH; r++) memset(&rinc_screen[rinc_Y0 + y * rinc_CH + r][rinc_X0 + x * rinc_CW], rinc_textBg, (size_t)(rinc_COLS - x) * rinc_CW); }
static void rinc_escape(int c) { /* the end of ESC [ n ; ... c */
  int n = rinc_escp[0], y;
  if (c == 'm') {
    if (n == 38 && rinc_nescp == 3 && rinc_escp[1] == 5) rinc_ink = (uint8_t)rinc_escp[2];
    else if (n == 0 || n == 39) rinc_ink = rinc_textFg;
    else if (n >= 30 && n <= 37) rinc_ink = rinc_ansi[n - 30];
    else if (n >= 90 && n <= 97) rinc_ink = rinc_ansi[n - 90 + 8];
  } else if (c == 'D') {
    for (n = n ? n : 1; n > 0; n--) { if (rinc_cx > 0) rinc_cx--; else if (rinc_cy > 0) { rinc_cy--; rinc_cx = rinc_COLS - 1; } }
  } else if (c == 'C') {
    for (n = n ? n : 1; n > 0; n--) { if (rinc_cx < rinc_COLS - 1) rinc_cx++; else if (rinc_cy < rinc_ROWS - 1) { rinc_cy++; rinc_cx = 0; } }
  } else if (c == 'K') rinc_blankRowFrom(rinc_cx, rinc_cy);
  else if (c == 'J') { rinc_blankRowFrom(rinc_cx, rinc_cy); for (y = rinc_cy + 1; y < rinc_ROWS; y++) rinc_blankRowFrom(0, y); }
}

static void rinc_newline(void) {
  rinc_cx = 0;
  if (++rinc_cy < rinc_ROWS) return;
  rinc_cy = rinc_ROWS - 1;
  memmove(rinc_screen[0], rinc_screen[rinc_CH], (size_t)(rinc_H - rinc_CH) * rinc_W);
  memset(rinc_screen[rinc_Y0 + (rinc_ROWS - 1) * rinc_CH], rinc_textBg, (size_t)(rinc_H - rinc_Y0 - (rinc_ROWS - 1) * rinc_CH) * rinc_W); /* (the last row, and the padding below) */
}

void rinc_consolePut(const char *s, int n) {
  rinc_cursor(0);
  while (n-- > 0) {
    int c = (unsigned char)*s++;
    if (rinc_esc == 1) { rinc_esc = c == '[' ? 2 : 0; rinc_escp[0] = rinc_escp[1] = rinc_escp[2] = 0; rinc_nescp = 1; continue; }
    if (rinc_esc == 2) {
      if (c >= '0' && c <= '9') rinc_escp[rinc_nescp - 1] = rinc_escp[rinc_nescp - 1] * 10 + c - '0';
      else if (c == ';') { if (rinc_nescp < 3) rinc_nescp++; }
      else { rinc_escape(c); rinc_esc = 0; }
      continue;
    }
    if (c == 27) { rinc_esc = 1; continue; }
    if (c == '\n') rinc_newline();
    else if (c == '\r') rinc_cx = 0;
    else if (c == '\b') { if (rinc_cx > 0) rinc_cx--; }
    else if (c >= 32) {
      if (rinc_cx >= rinc_COLS) rinc_newline();
      rinc_char(c, rinc_X0 + rinc_cx * rinc_CW, rinc_Y0 + rinc_cy * rinc_CH, rinc_ink, rinc_textBg);
      rinc_cx++;
    }
  }
  if (rinc_cx >= rinc_COLS) rinc_newline();
  rinc_cursor(1);
}

void rinc_consoleClear(int bg) {
  rinc_textBg = (uint8_t)bg; rinc_ink = rinc_textFg; rinc_esc = 0;
  memset(rinc_screen, bg, sizeof rinc_screen);
  rinc_cx = rinc_cy = 0; rinc_shown = 0;
  rinc_cursor(1);
}

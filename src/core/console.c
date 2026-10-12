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
#include "rim.h"

#define CW 6
#define CH 9
#define COLS (RIM_W / CW)
#define ROWS (RIM_H / CH)
#define X0 ((RIM_W - COLS * CW) / 2) /* (1: the padding) */
#define Y0 X0
#define CURSOR 10 /* yellow: clear on the dark blue, and apart from the text and highlighting colors */

static int cx, cy, shown; /* shown: the cursor is drawn */
uint8_t rim_text_fg = 7, rim_text_bg = 0;
static uint8_t ink = 7, under[CH]; /* ink: color now; under: the pixels beneath the cursor */
static int esc; /* escapes: 0 none, 1 ESC seen, 2 in ESC [ */
/* ANSI colors 0-7 (30-37) and bright (90-97), as their nearest PICO-8 colors: black, red, green,
   yellow, blue, pink (for magenta), blue (for cyan), light gray; bright: dark gray, red, green,
   yellow, blue, pink, blue, white */
static const uint8_t ansi[16] = {0, 8, 11, 10, 12, 14, 12, 6, 5, 8, 11, 10, 12, 14, 12, 7};
static int escp[3], nescp; /* an escape's numbers so far */

/* character c at pixel x, y, in fg; bg < 0 leaves the cell's other pixels alone */
void rim_char(int c, int x, int y, int fg, int bg) {
  const uint8_t *g = c > 32 && c < 127 ? rim_font[c - 32] : rim_font[0];
  int r, k;
  for (r = 0; r < CH; r++) {
    int py = y + r, bits = r < 8 ? g[r] : 0;
    if (py < 0 || py >= RIM_H) continue;
    for (k = 0; k < CW; k++) {
      int px = x + k;
      if (px < 0 || px >= RIM_W) continue;
      if (bits >> k & 1) rim_screen[py][px] = (uint8_t)fg;
      else if (bg >= 0) rim_screen[py][px] = (uint8_t)bg;
    }
  }
}

static void cursor(int on) { /* a line in its own color, with the pixels beneath kept */
  int r, x = X0 + cx * CW - 1;
  if (on == shown) return;
  for (r = 0; r < CH - 1 && x >= 0; r++) { /* (as tall as the glyphs: the cell's last row is the gap below) */
    uint8_t *p = &rim_screen[Y0 + cy * CH + r][x];
    if (on) { under[r] = *p; *p = CURSOR; } else *p = under[r];
  }
  shown = on;
}
static void blank_row_from(int x, int y) { int r; for (r = 0; r < CH; r++) memset(&rim_screen[Y0 + y * CH + r][X0 + x * CW], rim_text_bg, (size_t)(COLS - x) * CW); }
static void escape(int c) { /* the end of ESC [ n ; ... c */
  int n = escp[0], y;
  if (c == 'm') {
    if (n == 38 && nescp == 3 && escp[1] == 5) ink = (uint8_t)escp[2];
    else if (n == 0 || n == 39) ink = rim_text_fg;
    else if (n >= 30 && n <= 37) ink = ansi[n - 30];
    else if (n >= 90 && n <= 97) ink = ansi[n - 90 + 8];
  } else if (c == 'D') {
    for (n = n ? n : 1; n > 0; n--) { if (cx > 0) cx--; else if (cy > 0) { cy--; cx = COLS - 1; } }
  } else if (c == 'C') {
    for (n = n ? n : 1; n > 0; n--) { if (cx < COLS - 1) cx++; else if (cy < ROWS - 1) { cy++; cx = 0; } }
  } else if (c == 'K') blank_row_from(cx, cy);
  else if (c == 'J') { blank_row_from(cx, cy); for (y = cy + 1; y < ROWS; y++) blank_row_from(0, y); }
}

static void newline(void) {
  cx = 0;
  if (++cy < ROWS) return;
  cy = ROWS - 1;
  memmove(rim_screen[0], rim_screen[CH], (size_t)(RIM_H - CH) * RIM_W);
  memset(rim_screen[Y0 + (ROWS - 1) * CH], rim_text_bg, (size_t)(RIM_H - Y0 - (ROWS - 1) * CH) * RIM_W); /* (the last row, and the padding below) */
}

void rim_console_put(const char *s, int n) {
  cursor(0);
  while (n-- > 0) {
    int c = (unsigned char)*s++;
    if (esc == 1) { esc = c == '[' ? 2 : 0; escp[0] = escp[1] = escp[2] = 0; nescp = 1; continue; }
    if (esc == 2) {
      if (c >= '0' && c <= '9') escp[nescp - 1] = escp[nescp - 1] * 10 + c - '0';
      else if (c == ';') { if (nescp < 3) nescp++; }
      else { escape(c); esc = 0; }
      continue;
    }
    if (c == 27) { esc = 1; continue; }
    if (c == '\n') newline();
    else if (c == '\r') cx = 0;
    else if (c == '\b') { if (cx > 0) cx--; }
    else if (c >= 32) {
      if (cx >= COLS) newline();
      rim_char(c, X0 + cx * CW, Y0 + cy * CH, ink, rim_text_bg);
      cx++;
    }
  }
  if (cx >= COLS) newline();
  cursor(1);
}

void rim_console_clear(int bg) {
  rim_text_bg = (uint8_t)bg; ink = rim_text_fg; esc = 0;
  memset(rim_screen, bg, sizeof rim_screen);
  cx = cy = 0; shown = 0;
  cursor(1);
}

/* text on the screen: 53 columns by 26 rows of 6x9 cells in the 5x8 font. Writing past the last row
   scrolls the whole screen up a row, drawings included, as on PICO-8. A cursor block marks where the
   next character goes. */
#include <string.h>
#include "rim.h"

#define CW 6
#define CH 9
#define COLS (RIM_W / CW)
#define ROWS (RIM_H / CH)

static int cx, cy, shown; /* shown: the cursor is drawn */
uint8_t rim_text_fg = 7, rim_text_bg = 0;

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

static void cursor(int on) {
  int r;
  if (on == shown) return;
  for (r = 0; r < CH; r++) { /* invert the cell's colors */
    uint8_t *p = &rim_screen[cy * CH + r][cx * CW]; int k;
    for (k = 0; k < CW; k++) p[k] = p[k] == rim_text_bg ? rim_text_fg : p[k] == rim_text_fg ? rim_text_bg : p[k];
  }
  shown = on;
}

static void newline(void) {
  cx = 0;
  if (++cy < ROWS) return;
  cy = ROWS - 1;
  memmove(rim_screen[0], rim_screen[CH], (size_t)(RIM_H - CH) * RIM_W);
  memset(rim_screen[RIM_H - CH], rim_text_bg, (size_t)CH * RIM_W);
}

void rim_console_put(const char *s, int n) {
  cursor(0);
  while (n-- > 0) {
    int c = (unsigned char)*s++;
    if (c == '\n') newline();
    else if (c == '\r') cx = 0;
    else if (c == '\b') { if (cx > 0) cx--; }
    else if (c >= 32) {
      if (cx >= COLS) newline();
      rim_char(c, cx * CW, cy * CH, rim_text_fg, rim_text_bg);
      cx++;
    }
  }
  if (cx >= COLS) newline();
  cursor(1);
}

void rim_console_clear(int bg) {
  rim_text_bg = (uint8_t)bg;
  memset(rim_screen, bg, sizeof rim_screen);
  cx = cy = 0; shown = 0;
  cursor(1);
}

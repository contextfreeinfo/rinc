#include <string.h>
#include "rim.h"

_Alignas(4) uint8_t rim_screen[RIM_H][RIM_W]; /* (aligned for DMA) */
uint16_t rim_palette[256];
float (*rim_time)(void);
uint16_t rim_buttons;
int16_t rim_axes[6];
static void (*rim_put)(const char *s, int n);

static uint16_t rgb565(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); }

static void init_palette(void) {
  static const uint32_t pico8[16] = {0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
                                     0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA};
  static const int level[6] = {0, 95, 135, 175, 215, 255};
  int i;
  for (i = 0; i < 16; i++) rim_palette[i] = rgb565((int)(pico8[i] >> 16), (int)(pico8[i] >> 8 & 255), (int)(pico8[i] & 255));
  for (i = 0; i < 216; i++) rim_palette[16 + i] = rgb565(level[i / 36], level[i / 6 % 6], level[i % 6]);
  for (i = 0; i < 24; i++) rim_palette[232 + i] = rgb565(8 + 10 * i, 8 + 10 * i, 8 + 10 * i);
}

/* host procs: arguments arrive in a[], a result goes in a[0]. Drawing outside the screen is clipped */
static void h_cls(Rio *v, RioVal *a) { (void)v; memset(rim_screen, (uint8_t)a[0].i, sizeof rim_screen); }
static void h_pset(Rio *v, RioVal *a) {
  uint32_t x = a[0].u, y = a[1].u;
  (void)v;
  if (x < RIM_W && y < RIM_H) rim_screen[y][x] = (uint8_t)a[2].i;
}
static void h_pget(Rio *v, RioVal *a) {
  uint32_t x = a[0].u, y = a[1].u;
  (void)v;
  a[0].i = x < RIM_W && y < RIM_H ? rim_screen[y][x] : 0;
}
static void h_rect(Rio *v, RioVal *a) {
  int x0 = a[0].i, y0 = a[1].i, x1 = x0 + a[2].i, y1 = y0 + a[3].i, y;
  (void)v;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > RIM_W) x1 = RIM_W;
  if (y1 > RIM_H) y1 = RIM_H;
  for (y = y0; y < y1 && x0 < x1; y++) memset(&rim_screen[y][x0], (uint8_t)a[4].i, (size_t)(x1 - x0));
}
static void h_print(Rio *v, RioVal *a) { /* print(s, x, y, c): text at pixel x, y */
  const char *s = (const char *)rio_ptr(v, a); int i, x = a[2].i, y = a[3].i;
  if (!s) return;
  for (i = 0; i < a[1].i; i++, x += 6) rim_char((unsigned char)s[i], x, y, a[4].i & 255, -1);
}
static void h_btn(Rio *v, RioVal *a) { (void)v; a[0].i = a[0].u < 16 ? rim_buttons >> a[0].i & 1 : 0; }
/* stick(i) -> Float: 0 the left stick's x, 1 its y, 2 and 3 the right one's; -1 to 1, + right and down,
   0 when it's near the middle (it gets there on its own only roughly). trigger(i) -> Float: 0 left, 1
   right; 0 to 1 */
#define DEAD 0.15f
static float stick_at(int i) {
  float v = (float)rim_axes[i] / 32767.0f, m = v < 0 ? -v : v;
  if (m < DEAD) return 0;
  m = (m - DEAD) / (1 - DEAD);
  return v < 0 ? -(m > 1 ? 1 : m) : (m > 1 ? 1 : m);
}
static void h_stick(Rio *v, RioVal *a) { (void)v; a[0].f = a[0].u < 4 ? stick_at(a[0].i) : 0; }
static void h_trigger(Rio *v, RioVal *a) { (void)v; a[0].f = a[0].u < 2 ? (float)rim_axes[4 + a[0].i] / 32767.0f : 0; }
static void h_time(Rio *v, RioVal *a) { (void)v; a[0].f = rim_time ? rim_time() : 0; }
/* fromHex(out, text): bytes from hex digits (pairs; anything else between them, such as spaces and
   line breaks, is skipped) into out, until it's full. Returns how many. For binary data typed into a
   cart: sound, pictures */
static int hexdigit(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void h_fromhex(Rio *v, RioVal *a) {
  uint8_t *out = (uint8_t *)rio_ptr(v, a); const char *t = (const char *)rio_ptr(v, a + 2);
  uint32_t cap = a[1].u, n = a[3].u, i, k = 0; int hi = -1;
  for (i = 0; out && t && i < n && k < cap; i++) {
    int d = hexdigit(t[i]);
    if (d < 0) continue;
    if (hi < 0) hi = d; else { out[k++] = (uint8_t)(hi << 4 | d); hi = -1; }
  }
  a[0].u = k;
}

static void log_line(void *ud, const char *s, int n) { (void)ud; rim_put(s, n); rim_put("\r\n", 2); }

int rim_start(Rio *vm, void *mem, uint32_t memsize, void (*put)(const char *s, int n)) {
  rim_put = put;
  init_palette();
  if (rio_init(vm, mem, memsize, 0, 0)) return -1; /* no code buffer: the code goes in mem too */
  rim_audio_reset();
  rio_set_log(vm, log_line, 0);
  if (rio_ffi(vm, "cls", "i", h_cls) || rio_ffi(vm, "pset", "iii", h_pset) || rio_ffi(vm, "pget", "ii>i", h_pget) ||
      rio_ffi(vm, "rect", "iiiii", h_rect) || rio_ffi(vm, "print", "siii", h_print) || rio_ffi(vm, "btn", "i>i", h_btn) || rim_audio_procs(vm) || rio_ffi(vm, "time", ">f", h_time) ||
      rio_ffi(vm, "fromHex", "bs>i", h_fromhex) || rio_ffi(vm, "stick", "i>f", h_stick) || rio_ffi(vm, "trigger", "i>f", h_trigger)) return -1;
  return 0;
}

/* a game's frame: update then draw */
int rim_frame(Rio *vm) {
  int u = rio_func(vm, "update"), d = rio_func(vm, "draw");
  /* the left stick pushed well over also counts as the arrows, as on PICO-8 */
  if (rim_axes[0] < -16000) rim_buttons |= RIM_LEFT;
  if (rim_axes[0] > 16000) rim_buttons |= RIM_RIGHT;
  if (rim_axes[1] < -16000) rim_buttons |= RIM_UP;
  if (rim_axes[1] > 16000) rim_buttons |= RIM_DOWN;
  if (u < 0 && d < 0) return 1;
  if (u >= 0 && rio_call(vm, u)) return -1;
  if (d >= 0 && rio_call(vm, d)) return -1;
  return 0;
}

#ifndef RIO_NO_COMPILER /* (a vm that only runs images has no REPL) */
/* the REPL: lines gather until rio has a complete input; a blank line gives up on an unfinished one.
   Each next line of an unfinished input starts indented (two spaces a step, as rio_repl_indent says),
   end, else and case go a step back out once typed, and Backspace in the indent takes a step */
static void prompt(RimRepl *r) { r->put(r->n ? ". " : "> ", 2); } /* (as wide: lines line up) */
static int blank(const RimRepl *r) { int i; for (i = r->line0; i < r->n; i++) if (r->acc[i] != ' ') return 0; return 1; }
static void indent(RimRepl *r) {
  int k = rio_repl_indent(r->acc, (uint32_t)r->n) * 2;
  while (k-- > 0 && r->n < (int)sizeof r->acc - 2) { r->acc[r->n++] = ' '; if (r->echo) r->put(" ", 1); }
}
/* the line so far is its indent and one word: end, else or case go a step out */
static void outdent(RimRepl *r) {
  int s = r->line0, w, i;
  while (s < r->n && r->acc[s] == ' ') s++;
  w = r->n - s;
  for (i = s; i < r->n; i++) if (r->acc[i] == ' ') return;
  if (s - r->line0 < 2 || !w || !rio_repl_outdent(r->acc + s, (uint32_t)w)) return;
  memmove(r->acc + s - 2, r->acc + s, (size_t)w); r->n -= 2;
  if (r->echo) { /* (back over the indent's last step and the word, then the word again, and blank what's left) */
    for (i = 0; i < w + 2; i++) r->put("\b", 1);
    r->put(r->acc + s - 2, w); r->put("  \b\b", 4);
  }
}

int rim_repl_begin(RimRepl *r, Rio *vm, void *scratch, uint32_t size) {
  r->vm = vm; r->put = rim_put; r->n = r->line0 = 0; r->echo = 1; r->run = 0;
  if (rio_repl_begin(vm, scratch, size, 0)) return -1;
  r->put("Rim: rio REPL\r\n", 15);
  prompt(r);
  return 0;
}

void rim_repl_stop(RimRepl *r, const char *why) {
  r->run = 0;
  rim_audio_reset(); /* a game's sounds stop with it */
  if (why) { r->put(why, (int)strlen(why)); r->put("\r\n", 2); }
  prompt(r);
}

void rim_repl_char(RimRepl *r, int c) {
  if (c == '\r' || c == '\n') {
    int res;
    outdent(r);
    if (r->echo) r->put("\r\n", 2);
    if (r->line0 && blank(r)) { r->put("(unfinished input dropped)\r\n", 28); r->n = r->line0 = 0; prompt(r); return; }
    if (!r->line0 && r->n == 3 && !memcmp(r->acc, "run", 3)) { r->n = 0; r->run = 1; return; } /* the platform takes it from here */
    if (r->n >= (int)sizeof r->acc - 1) { r->put("(input too long)\r\n", 18); r->n = r->line0 = 0; prompt(r); return; }
    r->acc[r->n++] = '\n';
    res = rio_repl_eval(r->vm, r->acc, (uint32_t)r->n);
    if (res == RIO_MORE) { r->line0 = r->n; prompt(r); indent(r); return; }
    if (res < 0) { const char *e = rio_error(r->vm); r->put(e, (int)strlen(e)); r->put("\r\n", 2); }
    r->n = r->line0 = 0;
    prompt(r);
  } else if (c == 8 || c == 127) { /* backspace, within the current line (in the indent, a step) */
    int k = r->n > r->line0 && blank(r) && r->n - r->line0 >= 2 ? 2 - (r->n - r->line0) % 2 : 1;
    while (k-- > 0 && r->n > r->line0) { r->n--; if (r->echo) r->put("\b \b", 3); }
  } else if (c == 3) { /* Ctrl-C: drop what's typed */
    r->put("^C\r\n", 4); r->n = r->line0 = 0; prompt(r);
  } else if (c >= 32 && c < 127 && r->n < (int)sizeof r->acc - 2) {
    char ch = (char)c;
    if (c == ' ') outdent(r);
    r->acc[r->n++] = ch; if (r->echo) r->put(&ch, 1);
  }
}
#endif

#include <stdio.h>
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
/* key() -> Int: the next character typed (32-126, 13 Enter, 8 Backspace), or -1 when there's none
   waiting. The keys also press buttons as they're held: a cart reads whichever it wants */
static uint8_t typed[64]; static int thead, ttail, tesc;
void rim_text_push(int c) {
  int next = (thead + 1) % (int)sizeof typed;
  if (tesc) { if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') tesc = 0; else if (tesc == 1 && c != '[' && c != 'O') tesc = 0; else tesc = 2; return; }
  if (c == 27) { tesc = 1; return; }
  if (c == '\n') c = 13;
  if (c == 127) c = 8;
  if (!((c >= 32 && c < 127) || c == 13 || c == 8) || next == ttail) return;
  typed[thead] = (uint8_t)c; thead = next;
}
/* apps, for the launcher (with storage): apps() -> Int looks again and says how many;
   appName(i, out) -> Int and appInfo(i, out) -> Int copy app i's folder name, or its first line (its
   comment marker left off: "Title: what it does"), into out, and say how long it is. launch(name) and
   openConsole() ask the platform to switch, after this frame (rim_launch) */
const RimFs *rim_fs;
int rim_launch; char rim_launch_name[33];
static char app_names[64][33]; static int napps;
static void app_found(const char *name, int dir, void *ud) {
  size_t n = strlen(name); (void)ud;
  if (dir && name[0] != '.' && n < sizeof app_names[0] && napps < 64) memcpy(app_names[napps++], name, n + 1);
}
static int lower_cmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
    if (x != y || !x) return x - y;
  }
}
/* the apps on storage, in order of name (ignoring case): how many, or -1 without storage */
static int find_apps(void) {
  int i, j;
  napps = 0;
  if (!rim_fs || rim_fs->list("/rinc/apps", app_found, 0)) return -1;
  for (i = 1; i < napps; i++) /* (a few dozen at most: insertion) */
    for (j = i; j > 0 && lower_cmp(app_names[j - 1], app_names[j]) > 0; j--) {
      char t[sizeof app_names[0]]; memcpy(t, app_names[j], sizeof t); memcpy(app_names[j], app_names[j - 1], sizeof t); memcpy(app_names[j - 1], t, sizeof t);
    }
  return napps;
}
static void h_apps(Rio *v, RioVal *a) { (void)v; a[0].i = find_apps() < 0 ? 0 : napps; }
static int copy_out(Rio *v, RioVal *out, const char *s, int n) {
  char *p = (char *)rio_ptr(v, out); int cap = (int)out[1].u;
  if (p) memcpy(p, s, (size_t)(n < cap ? n : cap));
  return n;
}
static void h_app_name(Rio *v, RioVal *a) {
  a[0].i = a[0].u < (uint32_t)napps ? copy_out(v, a + 1, app_names[a[0].i], (int)strlen(app_names[a[0].i])) : 0;
}
static void h_app_info(Rio *v, RioVal *a) {
  char path[96], line[128]; int n = 0, i = 0, k = a[0].i;
  a[0].i = 0;
  if (k < 0 || k >= napps || !rim_fs) return;
  snprintf(path, sizeof path, "/rinc/apps/%s/%s.rio", app_names[k], app_names[k]);
  if (rim_fs->open(path, 0)) return;
  n = rim_fs->read(line, sizeof line);
  rim_fs->close();
  if (n <= 0 || line[0] != '#') return;
  for (i = 1; i < n && line[i] == ' '; i++) {}
  for (k = i; k < n && line[k] != '\n' && line[k] != '\r'; k++) {}
  a[0].i = copy_out(v, a + 1, line + i, k - i);
}
static void h_launch(Rio *v, RioVal *a) {
  const char *s = (const char *)rio_ptr(v, a); int n = (int)a[1].u;
  if (!s || n < 1 || n >= (int)sizeof rim_launch_name) return;
  memcpy(rim_launch_name, s, (size_t)n); rim_launch_name[n] = 0; rim_launch = 1;
}
static void h_open_console(Rio *v, RioVal *a) { (void)v; (void)a; rim_launch = 2; }
static void h_key(Rio *v, RioVal *a) { (void)v; if (ttail == thead) a[0].i = -1; else { a[0].i = typed[ttail]; ttail = (ttail + 1) % (int)sizeof typed; } }
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
      rio_ffi(vm, "fromHex", "bs>i", h_fromhex) || rio_ffi(vm, "stick", "i>f", h_stick) || rio_ffi(vm, "trigger", "i>f", h_trigger) ||
      rio_ffi(vm, "key", ">i", h_key) || rio_ffi(vm, "apps", ">i", h_apps) || rio_ffi(vm, "appName", "ib>i", h_app_name) ||
      rio_ffi(vm, "appInfo", "ib>i", h_app_info) || rio_ffi(vm, "launch", "s", h_launch) || rio_ffi(vm, "openConsole", "", h_open_console)) return -1;
  thead = ttail = tesc = 0;
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
   The line being typed can be edited anywhere: left and right, Home and End (or Ctrl-A, Ctrl-E),
   Backspace and Delete, and up and down go back through the lines typed before. It's in a little
   color as it's typed: keywords, constants (numbers and strings), comments. Each next line of an unfinished input starts
   indented (two spaces a step, as rio_repl_indent says), end, else and case go a step back out once
   typed, and Backspace in the indent takes a step.
   Keys arrive as a terminal sends them (ESC [ A and so on: the keyboard sends the same), and the echo
   is in terminal escapes too, which the console understands, so a serial terminal and the screen show
   the same. Typing at the end of the line, the usual case (and all of a pasted cart), sends little
   more than what's typed: a word takes its color as soon as it is one (the end of the line ends it,
   for now), recolored as it changes. Edits elsewhere redraw the line. */
#define PAINT_KEYWORD "\033[38;5;12m" /* (PICO-8 colors by number: blue) */
#define PAINT_NUMBER "\033[38;5;9m"   /* (orange, as strings) */
#define PAINT_STRING "\033[38;5;9m"
#define PAINT_COMMENT "\033[38;5;13m" /* (lavender: readable on the dark blue, apart from white) */
#define PAINT_PLAIN "\033[0m"
enum { MODE_PLAIN, MODE_STRING, MODE_COMMENT };
static void puts_(RimRepl *r, const char *s) { r->put(s, (int)strlen(s)); }
static void prompt(RimRepl *r) { if (!r->loading) r->put(r->n ? ". " : "> ", 2); } /* (as wide: lines line up) */
static int blank(const RimRepl *r) { int i; for (i = r->line0; i < r->n; i++) if (r->acc[i] != ' ') return 0; return 1; }
static int wordch(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
/* a word's color, or 0 for none (types too: plain) */
static const char *word_paint(const char *w, int n) {
  static const char *const keywords[] = {"proc", "struct", "if", "else", "for", "in", "end", "return", "break", "continue",
    "import", "include", "enum", "union", "switch", "case", "is", "nil", "and", "or", "not", "true", "false"};
  size_t i;
  if (n > 0 && w[0] >= '0' && w[0] <= '9') return PAINT_NUMBER;
  for (i = 0; i < sizeof keywords / sizeof keywords[0]; i++) if ((int)strlen(keywords[i]) == n && !memcmp(keywords[i], w, (size_t)n)) return PAINT_KEYWORD;
  return 0;
}
static void move_left(RimRepl *r, int k) { char b[16]; if (k > 0) r->put(b, snprintf(b, sizeof b, "\033[%dD", k)); }
/* the line from its start, in color, leaving the paint state as at its end */
static void paint_line(RimRepl *r) {
  int i = r->line0, j;
  puts_(r, PAINT_PLAIN); /* (from whatever color the cursor was last in: a comment's, say) */
  r->mode = rio_repl_indent(r->acc, (uint32_t)r->line0) < 0 ? MODE_STRING : MODE_PLAIN; r->word = -1;
  if (r->mode == MODE_STRING) puts_(r, PAINT_STRING);
  while (i < r->n) {
    char c = r->acc[i];
    if (r->mode == MODE_STRING) {
      for (j = i; j < r->n && r->acc[j] != '"'; j++) if (r->acc[j] == '\\' && j + 1 < r->n) j++;
      if (j < r->n) { r->put(r->acc + i, j + 1 - i); puts_(r, PAINT_PLAIN); r->mode = MODE_PLAIN; i = j + 1; }
      else { r->put(r->acc + i, r->n - i); i = r->n; }
    } else if (c == '#') { puts_(r, PAINT_COMMENT); r->put(r->acc + i, r->n - i); r->mode = MODE_COMMENT; i = r->n; }
    else if (c == '"') { puts_(r, PAINT_STRING); r->put(&c, 1); r->mode = MODE_STRING; i++; }
    else if (wordch(c)) {
      const char *p;
      for (j = i; j < r->n && (wordch(r->acc[j]) || (r->acc[i] >= '0' && r->acc[i] <= '9' && r->acc[j] == '.')); j++) {}
      if ((p = word_paint(r->acc + i, j - i))) { puts_(r, p); r->put(r->acc + i, j - i); puts_(r, PAINT_PLAIN); }
      else r->put(r->acc + i, j - i);
      if (j == r->n) { r->word = i; r->wp = p; } /* (the last word: still being typed) */
      i = j;
    } else { r->put(&c, 1); i++; }
  }
}
/* redraw the line: back to its start, paint it, blank what's left of the old one, and the cursor back */
static void redraw(RimRepl *r) {
  if (!r->echo) return;
  move_left(r, r->scr);
  paint_line(r); puts_(r, "\033[J");
  move_left(r, r->n - r->cur); r->scr = r->cur - r->line0;
}
/* the word at the end of the line ended (it's already in its color) */
static void finish_word(RimRepl *r) { r->word = -1; r->wp = 0; }
/* c typed at the end of the line */
static void append(RimRepl *r, char c) {
  r->acc[r->n++] = c; r->cur = r->n;
  if (!r->echo) return;
  r->scr = r->cur - r->line0;
  if (r->mode == MODE_COMMENT) r->put(&c, 1);
  else if (r->mode == MODE_STRING) {
    int k = 0, i; /* (a quote ends it unless an odd number of backslashes come before) */
    for (i = r->n - 2; i >= r->line0 && r->acc[i] == '\\'; i--) k++;
    r->put(&c, 1);
    if (c == '"' && !(k & 1)) { puts_(r, PAINT_PLAIN); r->mode = MODE_PLAIN; }
  } else if (wordch(c) || (c == '.' && r->word >= 0 && r->acc[r->word] >= '0' && r->acc[r->word] <= '9')) {
    const char *p;
    if (r->word < 0) { r->word = r->n - 1; r->wp = 0; }
    p = word_paint(r->acc + r->word, r->n - r->word);
    if (p != r->wp) { /* (its color changed: the whole word again) */
      move_left(r, r->n - 1 - r->word);
      if (p) puts_(r, p);
      r->put(r->acc + r->word, r->n - r->word);
      if (p) puts_(r, PAINT_PLAIN);
      r->wp = p;
    } else if (p) { puts_(r, p); r->put(&c, 1); puts_(r, PAINT_PLAIN); }
    else r->put(&c, 1);
  } else {
    r->n--; finish_word(r); r->n++; /* (the word ended before c) */
    if (c == '"') { puts_(r, PAINT_STRING); r->mode = MODE_STRING; }
    else if (c == '#') { puts_(r, PAINT_COMMENT); r->mode = MODE_COMMENT; }
    r->put(&c, 1);
  }
}
static void indent(RimRepl *r) {
  int k = rio_repl_indent(r->acc, (uint32_t)r->n) * 2;
  while (k-- > 0 && r->n < (int)sizeof r->acc - 2) append(r, ' ');
}
/* the line so far is its indent and one word: end, else or case go a step out */
static void outdent(RimRepl *r) {
  int s = r->line0, w, i;
  if (r->cur != r->n) return;
  while (s < r->n && r->acc[s] == ' ') s++;
  w = r->n - s;
  for (i = s; i < r->n; i++) if (r->acc[i] == ' ') return;
  if (s - r->line0 < 2 || !w || !rio_repl_outdent(r->acc + s, (uint32_t)w)) return;
  memmove(r->acc + s - 2, r->acc + s, (size_t)w); r->n -= 2; r->cur = r->n;
  redraw(r);
}
/* the line becomes text (from history): replacing what's typed on it */
static void set_line(RimRepl *r, const char *text, int n) {
  if (n > (int)sizeof r->acc - 2 - r->line0) n = (int)sizeof r->acc - 2 - r->line0;
  memcpy(r->acc + r->line0, text, (size_t)n); r->n = r->cur = r->line0 + n;
  redraw(r);
}
/* history: past lines, oldest first, each ending in \n; back counts how far up the list we've gone */
static void remember(RimRepl *r) {
  int n = r->n - r->line0, last;
  if (r->loading || blank(r) || n + 1 > (int)sizeof r->hist) return;
  for (last = r->hlen - 1; last > 0 && r->hist[last - 1] != '\n'; last--) {} /* (the newest, to skip repeats) */
  if (r->hlen && r->hlen - 1 - last == n && !memcmp(r->hist + last, r->acc + r->line0, (size_t)n)) return;
  while (r->hlen + n + 1 > (int)sizeof r->hist) { /* (room: the oldest go) */
    int k = 0; while (k < r->hlen && r->hist[k] != '\n') k++;
    memmove(r->hist, r->hist + k + 1, (size_t)(r->hlen - k - 1)); r->hlen -= k + 1;
  }
  memcpy(r->hist + r->hlen, r->acc + r->line0, (size_t)n); r->hlen += n; r->hist[r->hlen++] = '\n';
}
static void recall(RimRepl *r, int dir) { /* dir 1: older, -1: newer */
  int count = 0, i, k, back = r->back + dir, end;
  for (i = 0; i < r->hlen; i++) count += r->hist[i] == '\n';
  if (back < 0 || back > count) return;
  if (r->back == 0) { r->draftn = r->n - r->line0; if (r->draftn > (int)sizeof r->draft) r->draftn = (int)sizeof r->draft; memcpy(r->draft, r->acc + r->line0, (size_t)r->draftn); }
  r->back = back;
  if (!back) { set_line(r, r->draft, r->draftn); return; }
  for (end = r->hlen, k = 0; end > 0; ) { /* the back-th line from the end */
    int start = end - 1; while (start > 0 && r->hist[start - 1] != '\n') start--;
    if (++k == back) { set_line(r, r->hist + start, end - 1 - start); return; }
    end = start;
  }
}

int rim_repl_begin(RimRepl *r, Rio *vm, void *scratch, uint32_t size) {
  memset(r, 0, sizeof *r);
  r->vm = vm; r->put = rim_put; r->echo = 1; r->word = -1;
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

/* the commands for apps on storage */
static void say(RimRepl *r, const char *s) { r->put(s, (int)strlen(s)); }
static int app_path(char *path, size_t cap, const char *name, int n) {
  int i;
  if (n < 1 || n > 32) return 0;
  for (i = 0; i < n; i++) if (!(wordch(name[i]) || name[i] == '-')) return 0;
  return snprintf(path, cap, "/rinc/apps/%.*s/%.*s.rio", n, name, n, name) < (int)cap;
}

/* source, fed in as if typed, into a fresh session: from storage (src NULL) or from memory */
static int feed(RimRepl *r, const char *src, int len) {
  char buf[256]; int got, i, echo = r->echo, at = 0;
  if (r->restart && r->restart()) { say(r, "can't start a fresh session\r\n"); return -1; }
  r->loading = 1; r->errors = 0; r->echo = 0; r->n = 0; r->line0 = r->cur = r->scr = 0; r->run = 0;
  for (;;) {
    if (src) { got = len - at < (int)sizeof buf ? len - at : (int)sizeof buf; memcpy(buf, src + at, (size_t)got); at += got; }
    else got = rim_fs->read(buf, sizeof buf);
    if (got <= 0) break;
    for (i = 0; i < got; i++) rim_repl_char(r, buf[i] == '\n' ? '\r' : buf[i] == '\r' ? -1 : (unsigned char)buf[i]);
  }
  if (r->n) rim_repl_char(r, '\r'); /* (an unfinished last line) */
  r->loading = 0; r->echo = echo; r->n = r->line0 = r->cur = r->scr = 0; r->run = 0;
  return r->errors;
}
static void load(RimRepl *r, const char *name, int n, int run) {
  char path[96]; int errors;
  if (!app_path(path, sizeof path, name, n)) { say(r, "names are letters, digits, _ and -\r\n"); return; }
  if (!rim_fs || rim_fs->open(path, 0)) { say(r, rim_fs ? "can't open " : "no storage: "); say(r, path); say(r, "\r\n"); return; }
  errors = feed(r, 0, 0);
  rim_fs->close();
  if (errors < 0) return;
  say(r, errors ? "loaded, with errors: " : "loaded "); say(r, path); say(r, "\r\n");
  if (run && !errors) r->run = 1;
}
int rim_repl_app(RimRepl *r, const char *name, int run) { load(r, name, (int)strlen(name), run); return r->run ? 0 : -1; }
int rim_repl_source(RimRepl *r, const char *src, int len) {
  int errors = feed(r, src, len);
  if (!errors) r->run = 1;
  return errors ? -1 : 0;
}
static void save_flush(RimRepl *r) { if (r->nout && rim_fs->write(r->out, r->nout) < 0) r->saving = -1; r->saved += r->nout; r->nout = 0; }
static void save_char(RimRepl *r, int c) { /* while saving: everything typed goes to the file, until Ctrl-D */
  if (c == 4 || c == 3) {
    save_flush(r);
    say(r, PAINT_PLAIN "\r\n");
    if (rim_fs->close() || r->saving < 0) say(r, "couldn't save it all\r\n");
    else if (c == 4) { char b[48]; say(r, "saved "); r->put(b, snprintf(b, sizeof b, "%d bytes\r\n", r->saved)); }
    else say(r, "gave up (the file has what came before)\r\n");
    r->saving = 0; prompt(r); return;
  }
  if (c == '\n' && r->cr) { r->cr = 0; return; } /* (\r\n: one line break) */
  r->cr = c == '\r';
  if (c == '\r') c = '\n';
  if (c != '\n' && c != '\t' && (c < 32 || c > 126)) return;
  r->out[r->nout++] = (char)c;
  if (r->nout == (int)sizeof r->out) save_flush(r);
  if (r->echo) { if (c == '\n') r->put("\r\n", 2); else { char ch = (char)c; r->put(&ch, 1); } }
}
static void save(RimRepl *r, const char *name, int n) {
  char path[96];
  if (!app_path(path, sizeof path, name, n)) { say(r, "names are letters, digits, _ and -\r\n"); return; }
  if (!rim_fs || rim_fs->open(path, 1)) { say(r, rim_fs ? "can't write " : "no storage: "); say(r, path); say(r, "\r\n"); return; }
  say(r, "saving "); say(r, path); say(r, ": type or paste it, then Ctrl-D (Ctrl-C gives up)\r\n");
  r->saving = 1; r->saved = 0; r->nout = 0; r->cr = 0;
}
/* a command line? (only as a whole input of its own) */
static int command(RimRepl *r) {
  const char *s = r->acc; int n = r->n, w;
  if (r->line0 || r->loading) return 0;
  while (n && s[n - 1] == ' ') n--;
  while (n && *s == ' ') { s++; n--; }
  for (w = 0; w < n && s[w] != ' '; w++) {}
  if (w == 4 && n == 4 && !memcmp(s, "home", 4)) { rim_launch = 3; return 1; } /* (the platform goes back to the launcher) */
  if (w == 2 && n == 2 && !memcmp(s, "ls", 2)) {
    if (!rim_fs) say(r, "no storage\r\n");
    else if (find_apps() < 0) say(r, "no apps (or no card): /rinc/apps\r\n");
    else { int i; for (i = 0; i < napps; i++) { say(r, app_names[i]); say(r, "  "); } say(r, napps ? "\r\n" : "(none yet)\r\n"); }
    return 1;
  }
  if (n > w + 1 && ((w == 4 && !memcmp(s, "load", 4)) || (w == 3 && !memcmp(s, "run", 3)))) { load(r, s + w + 1, n - w - 1, w == 3); return 1; }
  if (n > w + 1 && w == 4 && !memcmp(s, "save", 4)) { save(r, s + w + 1, n - w - 1); return 1; }
  if (n > w + 1 && w == 6 && !memcmp(s, "delete", 6)) {
    char path[96]; const char *name = s + w + 1; int k = n - w - 1;
    if (!app_path(path, sizeof path, name, k)) say(r, "names are letters, digits, _ and -\r\n");
    else if (!rim_fs || rim_fs->remove(path)) { say(r, "can't delete "); say(r, path); say(r, "\r\n"); }
    else { path[strlen(path) - (size_t)k - 5] = 0; rim_fs->remove(path); say(r, "deleted "); r->put(name, k); say(r, "\r\n"); } /* (then its folder, if empty) */
    return 1;
  }
  return 0;
}

static void new_line(RimRepl *r) { r->line0 = r->n; r->cur = r->n; r->scr = 0; r->mode = MODE_PLAIN; r->word = -1; r->back = 0; }
static void key(RimRepl *r, int k) { /* an editing key: A up, B down, C right, D left, H home, F end, 3 delete */
  if (k == 'A' || k == 'B') { recall(r, k == 'A' ? 1 : -1); return; }
  if (k == 'C' && r->cur < r->n) r->cur++;
  else if (k == 'D' && r->cur > r->line0) r->cur--;
  else if (k == 'H') r->cur = r->line0;
  else if (k == 'F') r->cur = r->n;
  else if (k == '3' && r->cur < r->n) { memmove(r->acc + r->cur, r->acc + r->cur + 1, (size_t)(r->n - r->cur - 1)); r->n--; redraw(r); return; }
  else return;
  if (r->echo) { /* (just the cursor) */
    char b[16]; int d = r->cur - r->line0 - r->scr;
    if (d < 0) move_left(r, -d); else if (d > 0) r->put(b, snprintf(b, sizeof b, "\033[%dC", d));
    r->scr = r->cur - r->line0;
  }
}

void rim_repl_char(RimRepl *r, int c) {
  if (c < 0) return;
  if (r->saving) { save_char(r, c); return; }
  if (r->esc == 1) { r->esc = c == '[' || c == 'O' ? 2 : 0; r->escn = 0; if (r->esc) return; } /* (else: a lone ESC, then c) */
  else if (r->esc == 2) {
    if (c >= '0' && c <= '9') { r->escn = r->escn * 10 + c - '0'; return; }
    r->esc = 0;
    if (c == '~') key(r, r->escn == 1 || r->escn == 7 ? 'H' : r->escn == 4 || r->escn == 8 ? 'F' : r->escn == 3 ? '3' : 0);
    else key(r, c);
    return;
  }
  if (c == 27) { r->esc = 1; return; }
  if (c == '\r' || c == '\n') {
    int res;
    outdent(r);
    key(r, 'F'); /* (to the end, then the last word's color, if it's done typing one) */
    if (r->echo) { finish_word(r); puts_(r, PAINT_PLAIN "\r\n"); }
    if (r->line0 && blank(r)) { r->put("(unfinished input dropped)\r\n", 28); r->n = 0; new_line(r); prompt(r); return; }
    remember(r);
    if (!r->line0 && r->n == 3 && !memcmp(r->acc, "run", 3)) { r->n = 0; new_line(r); r->run = 1; return; } /* the platform takes it from here */
    if (command(r)) { r->n = 0; new_line(r); if (!r->saving && !r->run) prompt(r); return; }
    if (r->n >= (int)sizeof r->acc - 1) { r->put("(input too long)\r\n", 18); r->n = 0; new_line(r); prompt(r); return; }
    r->acc[r->n++] = '\n';
    res = rio_repl_eval(r->vm, r->acc, (uint32_t)r->n);
    if (res == RIO_MORE) { new_line(r); prompt(r); indent(r); return; }
    if (res < 0) { const char *e = rio_error(r->vm); r->errors++; r->put(e, (int)strlen(e)); r->put("\r\n", 2); }
    r->n = 0; new_line(r);
    prompt(r);
  } else if (c == 8 || c == 127) { /* backspace (in the indent, a step) */
    if (r->cur == r->n) {
      int k = r->n > r->line0 && blank(r) && r->n - r->line0 >= 2 ? 2 - (r->n - r->line0) % 2 : 1;
      while (k-- > 0 && r->n > r->line0) r->n--;
      r->cur = r->n;
    } else if (r->cur > r->line0) { memmove(r->acc + r->cur - 1, r->acc + r->cur, (size_t)(r->n - r->cur)); r->n--; r->cur--; }
    redraw(r);
  } else if (c == 1) key(r, 'H'); /* Ctrl-A, Ctrl-E: the start and end of the line */
  else if (c == 5) key(r, 'F');
  else if (c == 3) { /* Ctrl-C: drop what's typed */
    r->put(PAINT_PLAIN "^C\r\n", 8); r->n = 0; new_line(r); prompt(r);
  } else if (c >= 32 && c < 127 && r->n < (int)sizeof r->acc - 2) {
    if (c == ' ') outdent(r);
    if (r->cur == r->n) append(r, (char)c);
    else { memmove(r->acc + r->cur + 1, r->acc + r->cur, (size_t)(r->n - r->cur)); r->acc[r->cur++] = (char)c; r->n++; redraw(r); }
  }
}
#endif

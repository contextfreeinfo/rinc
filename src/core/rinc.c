#include <stdio.h>
#include <string.h>
#include "rinc.h"

_Alignas(4) uint8_t rinc_screen[rinc_H][rinc_W]; /* (aligned for DMA) */
uint16_t rinc_palette[256];
float (*rinc_time)(void);
uint16_t rinc_buttons;
int16_t rinc_axes[6];
static void (*rinc_put)(const char *s, int n);

static uint16_t rinc_rgb565(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); }

static void rinc_initPalette(void) {
  static const uint32_t pico8[16] = {0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
                                     0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA};
  static const int level[6] = {0, 95, 135, 175, 215, 255};
  int i;
  for (i = 0; i < 16; i++) rinc_palette[i] = rinc_rgb565((int)(pico8[i] >> 16), (int)(pico8[i] >> 8 & 255), (int)(pico8[i] & 255));
  for (i = 0; i < 216; i++) rinc_palette[16 + i] = rinc_rgb565(level[i / 36], level[i / 6 % 6], level[i % 6]);
  for (i = 0; i < 24; i++) rinc_palette[232 + i] = rinc_rgb565(8 + 10 * i, 8 + 10 * i, 8 + 10 * i);
}

/* host procs: arguments arrive in a[], a result goes in a[0]. Drawing outside the screen is clipped */
static void rinc_hostCls(rio_Vm *v, rio_Val *a) { (void)v; memset(rinc_screen, (uint8_t)a[0].i, sizeof rinc_screen); }
static void rinc_hostPset(rio_Vm *v, rio_Val *a) {
  uint32_t x = a[0].u, y = a[1].u;
  (void)v;
  if (x < rinc_W && y < rinc_H) rinc_screen[y][x] = (uint8_t)a[2].i;
}
static void rinc_hostPget(rio_Vm *v, rio_Val *a) {
  uint32_t x = a[0].u, y = a[1].u;
  (void)v;
  a[0].i = x < rinc_W && y < rinc_H ? rinc_screen[y][x] : 0;
}
static void rinc_hostRect(rio_Vm *v, rio_Val *a) {
  int x0 = a[0].i, y0 = a[1].i, x1 = x0 + a[2].i, y1 = y0 + a[3].i, y;
  (void)v;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > rinc_W) x1 = rinc_W;
  if (y1 > rinc_H) y1 = rinc_H;
  for (y = y0; y < y1 && x0 < x1; y++) memset(&rinc_screen[y][x0], (uint8_t)a[4].i, (size_t)(x1 - x0));
}
static void rinc_hostPrint(rio_Vm *v, rio_Val *a) { /* print(s, x, y, c): text at pixel x, y */
  const char *s = (const char *)rio_sliceBytes(v, a); int i, x = a[2].i, y = a[3].i;
  if (!s) return;
  for (i = 0; i < a[1].i; i++, x += 6) rinc_char((unsigned char)s[i], x, y, a[4].i & 255, -1);
}
/* key() -> Int: the next character typed (32-126, 13 Enter, 8 Backspace, 27 Esc), or -1 when there's
   none waiting. The keys also press buttons as they're held: a cart reads whichever it wants */
static uint8_t rinc_typed[64]; static int rinc_typedHead, rinc_typedTail, rinc_typedEsc;
void rinc_textPush(int c) {
  int next = (rinc_typedHead + 1) % (int)sizeof rinc_typed;
  if (rinc_typedEsc) { if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') rinc_typedEsc = 0; else if (rinc_typedEsc == 1 && c != '[' && c != 'O') rinc_typedEsc = 0; else rinc_typedEsc = 2; return; }
  if (c == 27) { rinc_typedEsc = 1; return; }
  if (c == '\n') c = 13;
  if (c == 127) c = 8;
  if (!((c >= 32 && c < 127) || c == 13 || c == 8) || next == rinc_typedTail) return;
  rinc_typed[rinc_typedHead] = (uint8_t)c; rinc_typedHead = next;
}
/* apps, for the launcher (with storage): apps() -> Int looks again and says how many;
   appName(i, out) -> Int and appInfo(i, out) -> Int copy app i's folder name, or its first line (its
   comment marker left off: "Title: what it does"), into out, and say how long it is. launch(name) and
   openConsole() ask the platform to switch, after this frame (rinc_launch) */
const rinc_Fs *rinc_fs;
int rinc_launch; char rinc_launchName[33];
static char rinc_appNames[64][33]; static int rinc_appCount;
static void rinc_appFound(const char *name, int dir, void *ud) {
  size_t n = strlen(name); (void)ud;
  if (dir && name[0] != '.' && n < sizeof rinc_appNames[0] && rinc_appCount < 64) memcpy(rinc_appNames[rinc_appCount++], name, n + 1);
}
static int rinc_lowerCmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
    if (x != y || !x) return x - y;
  }
}
/* the apps on storage, in order of name (ignoring case): how many, or -1 without storage */
static int rinc_findApps(void) {
  int i, j;
  rinc_appCount = 0;
  if (!rinc_fs || rinc_fs->list("/rinc/apps", rinc_appFound, 0)) return -1;
  for (i = 1; i < rinc_appCount; i++) /* (a few dozen at most: insertion) */
    for (j = i; j > 0 && rinc_lowerCmp(rinc_appNames[j - 1], rinc_appNames[j]) > 0; j--) {
      char t[sizeof rinc_appNames[0]]; memcpy(t, rinc_appNames[j], sizeof t); memcpy(rinc_appNames[j], rinc_appNames[j - 1], sizeof t); memcpy(rinc_appNames[j - 1], t, sizeof t);
    }
  return rinc_appCount;
}
static void rinc_hostApps(rio_Vm *v, rio_Val *a) { (void)v; a[0].i = rinc_findApps() < 0 ? 0 : rinc_appCount; }
static int rinc_copyOut(rio_Vm *v, rio_Val *out, const char *s, int n) {
  char *p = (char *)rio_sliceBytes(v, out); int cap = (int)out[1].u;
  if (p) memcpy(p, s, (size_t)(n < cap ? n : cap));
  return n;
}
static void rinc_hostAppName(rio_Vm *v, rio_Val *a) {
  a[0].i = a[0].u < (uint32_t)rinc_appCount ? rinc_copyOut(v, a + 1, rinc_appNames[a[0].i], (int)strlen(rinc_appNames[a[0].i])) : 0;
}
static void rinc_hostAppInfo(rio_Vm *v, rio_Val *a) {
  char path[96], line[128]; int n = 0, i = 0, k = a[0].i;
  a[0].i = 0;
  if (k < 0 || k >= rinc_appCount || !rinc_fs) return;
  snprintf(path, sizeof path, "/rinc/apps/%s/%s.rio", rinc_appNames[k], rinc_appNames[k]);
  if (rinc_fs->open(path, 0)) return;
  n = rinc_fs->read(line, sizeof line);
  rinc_fs->close();
  if (n <= 0 || line[0] != '#') return;
  for (i = 1; i < n && line[i] == ' '; i++) {}
  for (k = i; k < n && line[k] != '\n' && line[k] != '\r'; k++) {}
  a[0].i = rinc_copyOut(v, a + 1, line + i, k - i);
}
static void rinc_hostLaunch(rio_Vm *v, rio_Val *a) {
  const char *s = (const char *)rio_sliceBytes(v, a); int n = (int)a[1].u;
  if (!s || n < 1 || n >= (int)sizeof rinc_launchName) return;
  memcpy(rinc_launchName, s, (size_t)n); rinc_launchName[n] = 0; rinc_launch = 1;
}
static void rinc_hostOpenConsole(rio_Vm *v, rio_Val *a) { (void)v; (void)a; rinc_launch = 2; }
void rinc_textEsc(void) { int next = (rinc_typedHead + 1) % (int)sizeof rinc_typed; if (next != rinc_typedTail) { rinc_typed[rinc_typedHead] = 27; rinc_typedHead = next; } }
static void rinc_hostKey(rio_Vm *v, rio_Val *a) { (void)v; if (rinc_typedTail == rinc_typedHead) a[0].i = -1; else { a[0].i = rinc_typed[rinc_typedTail]; rinc_typedTail = (rinc_typedTail + 1) % (int)sizeof rinc_typed; } }
static void rinc_hostBtn(rio_Vm *v, rio_Val *a) { (void)v; a[0].i = a[0].u < 16 ? rinc_buttons >> a[0].i & 1 : 0; }
/* stick(i) -> Float: 0 the left stick's x, 1 its y, 2 and 3 the right one's; -1 to 1, + right and down,
   0 when it's near the middle (it gets there on its own only roughly). trigger(i) -> Float: 0 left, 1
   right; 0 to 1 */
#define rinc_DEAD 0.15f
static float rinc_stickAt(int i) {
  float v = (float)rinc_axes[i] / 32767.0f, m = v < 0 ? -v : v;
  if (m < rinc_DEAD) return 0;
  m = (m - rinc_DEAD) / (1 - rinc_DEAD);
  return v < 0 ? -(m > 1 ? 1 : m) : (m > 1 ? 1 : m);
}
static void rinc_hostStick(rio_Vm *v, rio_Val *a) { (void)v; a[0].f = a[0].u < 4 ? rinc_stickAt(a[0].i) : 0; }
static void rinc_hostTrigger(rio_Vm *v, rio_Val *a) { (void)v; a[0].f = a[0].u < 2 ? (float)rinc_axes[4 + a[0].i] / 32767.0f : 0; }
static void rinc_hostTime(rio_Vm *v, rio_Val *a) { (void)v; a[0].f = rinc_time ? rinc_time() : 0; }
/* fromHex(out, text): bytes from hex digits (pairs; anything else between them, such as spaces and
   line breaks, is skipped) into out, until it's full. Returns how many. For binary data typed into a
   cart: sound, pictures */
static int rinc_hexDigit(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void rinc_hostFromhex(rio_Vm *v, rio_Val *a) {
  uint8_t *out = (uint8_t *)rio_sliceBytes(v, a); const char *t = (const char *)rio_sliceBytes(v, a + 2);
  uint32_t cap = a[1].u, n = a[3].u, i, k = 0; int hi = -1;
  for (i = 0; out && t && i < n && k < cap; i++) {
    int d = rinc_hexDigit(t[i]);
    if (d < 0) continue;
    if (hi < 0) hi = d; else { out[k++] = (uint8_t)(hi << 4 | d); hi = -1; }
  }
  a[0].u = k;
}

static void rinc_logLine(void *ud, const char *s, int n) { (void)ud; rinc_put(s, n); rinc_put("\r\n", 2); }

int rinc_start(rio_Vm *vm, void *mem, uint32_t memsize, void (*put)(const char *s, int n)) {
  rinc_put = put;
  rinc_initPalette();
  if (rio_init(vm, mem, memsize, 0, 0)) return -1; /* no code buffer: the code goes in mem too */
  rinc_audioReset();
  rio_setLog(vm, rinc_logLine, 0);
  if (rinc_hostProcs(vm) || rio_autoImport(vm, "pc")) return -1; /* (pc.cls and so on, everywhere) */
  rinc_typedHead = rinc_typedTail = rinc_typedEsc = 0;
  return 0;
}

int rinc_hostProcs(rio_Vm *vm) {
  return rio_addFfi(vm, "pc.cls", "i", rinc_hostCls) || rio_addFfi(vm, "pc.pset", "iii", rinc_hostPset) || rio_addFfi(vm, "pc.pget", "ii>i", rinc_hostPget) ||
         rio_addFfi(vm, "pc.rect", "iiiii", rinc_hostRect) || rio_addFfi(vm, "pc.print", "siii", rinc_hostPrint) || rio_addFfi(vm, "pc.btn", "i>i", rinc_hostBtn) || rinc_audioProcs(vm) || rio_addFfi(vm, "pc.time", ">f", rinc_hostTime) ||
         rio_addFfi(vm, "pc.fromHex", "bs>i", rinc_hostFromhex) || rio_addFfi(vm, "pc.stick", "i>f", rinc_hostStick) || rio_addFfi(vm, "pc.trigger", "i>f", rinc_hostTrigger) ||
         rio_addFfi(vm, "pc.key", ">i", rinc_hostKey) || rio_addFfi(vm, "pc.apps", ">i", rinc_hostApps) || rio_addFfi(vm, "pc.appName", "ib>i", rinc_hostAppName) ||
         rio_addFfi(vm, "pc.appInfo", "ib>i", rinc_hostAppInfo) || rio_addFfi(vm, "pc.launch", "s", rinc_hostLaunch) || rio_addFfi(vm, "pc.openConsole", "", rinc_hostOpenConsole);
}

/* a game's frame: update then draw */
int rinc_frame(rio_Vm *vm) {
  int u = rio_findProc(vm, "update"), d = rio_findProc(vm, "draw");
  /* the left stick pushed well over also counts as the arrows, as on PICO-8 */
  if (rinc_axes[0] < -16000) rinc_buttons |= rinc_Button_left;
  if (rinc_axes[0] > 16000) rinc_buttons |= rinc_Button_right;
  if (rinc_axes[1] < -16000) rinc_buttons |= rinc_Button_up;
  if (rinc_axes[1] > 16000) rinc_buttons |= rinc_Button_down;
  if (u < 0 && d < 0) return 1;
  if (u >= 0 && rio_call(vm, u)) return -1;
  if (d >= 0 && rio_call(vm, d)) return -1;
  return 0;
}

#ifndef rio_NO_COMPILER /* (a vm that only runs images has no REPL) */
/* the REPL: lines gather until rio has a complete input; a blank line gives up on an unfinished one.
   The line being typed can be edited anywhere: left and right, Home and End (or Ctrl-A, Ctrl-E),
   Backspace and Delete, and up and down go back through the lines typed before. It's in a little
   color as it's typed: keywords, constants (numbers and strings), comments. Each next line of an unfinished input starts
   indented (two spaces a step, as rio_replIndent says), end, else and case go a step back out once
   typed, and Backspace in the indent takes a step.
   Keys arrive as a terminal sends them (ESC [ A and so on: the keyboard sends the same), and the echo
   is in terminal escapes too, which the console understands, so a serial terminal and the screen show
   the same. Typing at the end of the line, the usual case (and all of a pasted cart), sends little
   more than what's typed: a word takes its color as soon as it is one (the end of the line ends it,
   for now), recolored as it changes. Edits elsewhere redraw the line. */
#define rinc_PAINT_KEYWORD "\033[38;5;12m" /* (PICO-8 colors by number: blue) */
#define rinc_PAINT_NUMBER "\033[38;5;9m"   /* (orange, as strings) */
#define rinc_PAINT_STRING "\033[38;5;9m"
#define rinc_PAINT_COMMENT "\033[38;5;13m" /* (lavender: readable on the dark blue, apart from white) */
#define rinc_PAINT_PLAIN "\033[0m"
enum { rinc_Mode_plain, rinc_Mode_string, rinc_Mode_comment };
static void rinc_puts(rinc_Repl *r, const char *s) { r->put(s, (int)strlen(s)); }
static void rinc_prompt(rinc_Repl *r) { if (!r->loading) r->put(r->n ? ". " : "> ", 2); } /* (as wide: lines line up) */
static int rinc_blank(const rinc_Repl *r) { int i; for (i = r->line0; i < r->n; i++) if (r->acc[i] != ' ') return 0; return 1; }
static int rinc_wordChar(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
/* a word's color, or 0 for none (types too: plain) */
static const char *rinc_wordPaint(const char *w, int n) {
  static const char *const keywords[] = {"proc", "struct", "if", "else", "for", "in", "end", "return", "break", "continue",
    "import", "include", "enum", "union", "switch", "case", "is", "nil", "and", "or", "not", "true", "false"};
  size_t i;
  if (n > 0 && w[0] >= '0' && w[0] <= '9') return rinc_PAINT_NUMBER;
  for (i = 0; i < sizeof keywords / sizeof keywords[0]; i++) if ((int)strlen(keywords[i]) == n && !memcmp(keywords[i], w, (size_t)n)) return rinc_PAINT_KEYWORD;
  return 0;
}
static void rinc_moveLeft(rinc_Repl *r, int k) { char b[16]; if (k > 0) r->put(b, snprintf(b, sizeof b, "\033[%dD", k)); }
/* the line from its start, in color, leaving the paint state as at its end */
static void rinc_paintLine(rinc_Repl *r) {
  int i = r->line0, j;
  rinc_puts(r, rinc_PAINT_PLAIN); /* (from whatever color the cursor was last in: a comment's, say) */
  r->mode = rio_replIndent(r->acc, (uint32_t)r->line0) < 0 ? rinc_Mode_string : rinc_Mode_plain; r->word = -1; r->numdot = -1;
  if (r->mode == rinc_Mode_string) rinc_puts(r, rinc_PAINT_STRING);
  while (i < r->n) {
    char c = r->acc[i];
    if (r->mode == rinc_Mode_string) {
      for (j = i; j < r->n && r->acc[j] != '"'; j++) if (r->acc[j] == '\\' && j + 1 < r->n) j++;
      if (j < r->n) { r->put(r->acc + i, j + 1 - i); rinc_puts(r, rinc_PAINT_PLAIN); r->mode = rinc_Mode_plain; i = j + 1; }
      else { r->put(r->acc + i, r->n - i); i = r->n; }
    } else if (c == '#') { rinc_puts(r, rinc_PAINT_COMMENT); r->put(r->acc + i, r->n - i); r->mode = rinc_Mode_comment; i = r->n; }
    else if (c == '"') { rinc_puts(r, rinc_PAINT_STRING); r->put(&c, 1); r->mode = rinc_Mode_string; i++; }
    else if (rinc_wordChar(c)) {
      const char *p;
      int dot = 0; /* (a number's one dot, if a digit follows it: 1.5, but 0..<n) */
      for (j = i; j < r->n && (rinc_wordChar(r->acc[j]) || (r->acc[i] >= '0' && r->acc[i] <= '9' && r->acc[j] == '.' && !dot++ &&
                               j + 1 < r->n && r->acc[j + 1] >= '0' && r->acc[j + 1] <= '9')); j++) {}
      if ((p = rinc_wordPaint(r->acc + i, j - i))) { rinc_puts(r, p); r->put(r->acc + i, j - i); rinc_puts(r, rinc_PAINT_PLAIN); }
      else r->put(r->acc + i, j - i);
      if (j == r->n) { r->word = i; r->wp = p; r->numdot = -1; } /* (the last word: still being typed) */
      i = j;
    } else { r->put(&c, 1); i++; }
  }
}
/* redraw the line: back to its start, paint it, blank what's left of the old one, and the cursor back */
static void rinc_redraw(rinc_Repl *r) {
  if (!r->echo) return;
  rinc_moveLeft(r, r->scr);
  rinc_paintLine(r); rinc_puts(r, "\033[J");
  rinc_moveLeft(r, r->n - r->cur); r->scr = r->cur - r->line0;
}
/* the word at the end of the line ended (it's already in its color) */
static void rinc_finishWord(rinc_Repl *r) { r->word = -1; r->wp = 0; r->numdot = -1; }
static void rinc_appendTail(rinc_Repl *r, char c);
/* c typed at the end of the line */
static void rinc_append(rinc_Repl *r, char c) {
  r->acc[r->n++] = c; r->cur = r->n;
  if (!r->echo) return;
  r->scr = r->cur - r->line0;
  if (r->mode == rinc_Mode_comment) r->put(&c, 1);
  else if (r->mode == rinc_Mode_string) {
    int k = 0, i; /* (a quote ends it unless an odd number of backslashes come before) */
    for (i = r->n - 2; i >= r->line0 && r->acc[i] == '\\'; i--) k++;
    r->put(&c, 1);
    if (c == '"' && !(k & 1)) { rinc_puts(r, rinc_PAINT_PLAIN); r->mode = rinc_Mode_plain; }
  } else if (r->numdot >= 0 && r->numdot == r->n - 2 && !(c >= '0' && c <= '9')) { /* (a number's dot, but no digit after: not its dot) */
    rinc_moveLeft(r, 1); rinc_puts(r, rinc_PAINT_PLAIN "."); rinc_finishWord(r);
    r->n--; rinc_appendTail(r, c);
  } else if (rinc_wordChar(c) || (c == '.' && r->word >= 0 && r->acc[r->word] >= '0' && r->acc[r->word] <= '9' && r->numdot < r->word)) {
    const char *p;
    if (c == '.') r->numdot = r->n - 1; /* (for now: if a digit comes next, it's the number's) */
    if (r->word < 0) { r->word = r->n - 1; r->wp = 0; }
    p = rinc_wordPaint(r->acc + r->word, r->n - r->word);
    if (p != r->wp) { /* (its color changed: the whole word again) */
      rinc_moveLeft(r, r->n - 1 - r->word);
      if (p) rinc_puts(r, p);
      r->put(r->acc + r->word, r->n - r->word);
      if (p) rinc_puts(r, rinc_PAINT_PLAIN);
      r->wp = p;
    } else if (p) { rinc_puts(r, p); r->put(&c, 1); rinc_puts(r, rinc_PAINT_PLAIN); }
    else r->put(&c, 1);
  } else {
    r->n--; rinc_finishWord(r); r->n++; /* (the word ended before c) */
    if (c == '"') { rinc_puts(r, rinc_PAINT_STRING); r->mode = rinc_Mode_string; }
    else if (c == '#') { rinc_puts(r, rinc_PAINT_COMMENT); r->mode = rinc_Mode_comment; }
    r->put(&c, 1);
  }
}
static void rinc_appendTail(rinc_Repl *r, char c) { rinc_append(r, c); } /* (append again, after taking back a dot) */
/* the next line's indent, two spaces a step. Spaces typed at its start take its place first (so
   typed or pasted indenting doesn't double it) */
static void rinc_indent(rinc_Repl *r) {
  int k = rio_replIndent(r->acc, (uint32_t)r->n) * 2;
  r->autoind = 0;
  while (k-- > 0 && r->n < (int)sizeof r->acc - 2) { rinc_append(r, ' '); r->autoind++; }
}
/* the line so far is its indent and one word: end, else or case go a step out */
static void rinc_outdent(rinc_Repl *r) {
  int s = r->line0, w, i;
  if (r->cur != r->n) return;
  while (s < r->n && r->acc[s] == ' ') s++;
  w = r->n - s;
  for (i = s; i < r->n; i++) if (r->acc[i] == ' ') return;
  if (s - r->line0 < 2 || !w || !rio_replOutdent(r->acc + s, (uint32_t)w)) return;
  memmove(r->acc + s - 2, r->acc + s, (size_t)w); r->n -= 2; r->cur = r->n;
  rinc_redraw(r);
}
/* the line becomes text (from history): replacing what's typed on it */
static void rinc_setLine(rinc_Repl *r, const char *text, int n) {
  if (n > (int)sizeof r->acc - 2 - r->line0) n = (int)sizeof r->acc - 2 - r->line0;
  memcpy(r->acc + r->line0, text, (size_t)n); r->n = r->cur = r->line0 + n;
  rinc_redraw(r);
}
/* history: past lines, oldest first, each ending in \n; back counts how far up the list we've gone */
static void rinc_remember(rinc_Repl *r) {
  int n = r->n - r->line0, last;
  if (r->loading || rinc_blank(r) || n + 1 > (int)sizeof r->hist) return;
  for (last = r->hlen - 1; last > 0 && r->hist[last - 1] != '\n'; last--) {} /* (the newest, to skip repeats) */
  if (r->hlen && r->hlen - 1 - last == n && !memcmp(r->hist + last, r->acc + r->line0, (size_t)n)) return;
  while (r->hlen + n + 1 > (int)sizeof r->hist) { /* (room: the oldest go) */
    int k = 0; while (k < r->hlen && r->hist[k] != '\n') k++;
    memmove(r->hist, r->hist + k + 1, (size_t)(r->hlen - k - 1)); r->hlen -= k + 1;
  }
  memcpy(r->hist + r->hlen, r->acc + r->line0, (size_t)n); r->hlen += n; r->hist[r->hlen++] = '\n';
}
static void rinc_recall(rinc_Repl *r, int dir) { /* dir 1: older, -1: newer */
  int count = 0, i, k, back = r->back + dir, end;
  for (i = 0; i < r->hlen; i++) count += r->hist[i] == '\n';
  if (back < 0 || back > count) return;
  if (r->back == 0) { r->draftn = r->n - r->line0; if (r->draftn > (int)sizeof r->draft) r->draftn = (int)sizeof r->draft; memcpy(r->draft, r->acc + r->line0, (size_t)r->draftn); }
  r->back = back;
  if (!back) { rinc_setLine(r, r->draft, r->draftn); return; }
  for (end = r->hlen, k = 0; end > 0; ) { /* the back-th line from the end */
    int start = end - 1; while (start > 0 && r->hist[start - 1] != '\n') start--;
    if (++k == back) { rinc_setLine(r, r->hist + start, end - 1 - start); return; }
    end = start;
  }
}

int rinc_replBegin(rinc_Repl *r, rio_Vm *vm, void *scratch, uint32_t size) {
  memset(r, 0, sizeof *r);
  r->vm = vm; r->put = rinc_put; r->echo = 1; r->word = -1; r->numdot = -1;
  if (rio_replBegin(vm, scratch, size, 0)) return -1;
  r->put("Rinc: rio REPL\r\n", 16);
  rinc_prompt(r);
  return 0;
}

void rinc_replStop(rinc_Repl *r, const char *why) {
  r->run = 0;
  rinc_audioReset(); /* a game's sounds stop with it */
  if (why) { r->put(why, (int)strlen(why)); r->put("\r\n", 2); }
  rinc_prompt(r);
}

/* the commands for apps on storage */
static void rinc_say(rinc_Repl *r, const char *s) { r->put(s, (int)strlen(s)); }
static int rinc_appPath(char *path, size_t cap, const char *name, int n) {
  int i;
  if (n < 1 || n > 32) return 0;
  for (i = 0; i < n; i++) if (!(rinc_wordChar(name[i]) || name[i] == '-')) return 0;
  return snprintf(path, cap, "/rinc/apps/%.*s/%.*s.rio", n, name, n, name) < (int)cap;
}

/* source, fed in as if typed, into a fresh session: from storage (src NULL) or from memory */
static int rinc_feed(rinc_Repl *r, const char *src, int len) {
  char buf[256]; int got, i, echo = r->echo, at = 0;
  if (r->restart && r->restart()) { rinc_say(r, "can't start a fresh session\r\n"); return -1; }
  r->loading = 1; r->errors = 0; r->echo = 0; r->n = 0; r->line0 = r->cur = r->scr = 0; r->run = 0;
  r->esc = 0; r->saving = 0; /* (nothing half typed carries over: an Esc that began a key's escape, say) */
  for (;;) {
    if (src) { got = len - at < (int)sizeof buf ? len - at : (int)sizeof buf; memcpy(buf, src + at, (size_t)got); at += got; }
    else got = rinc_fs->read(buf, sizeof buf);
    if (got <= 0) break;
    for (i = 0; i < got; i++) rinc_replChar(r, buf[i] == '\n' ? '\r' : buf[i] == '\r' ? -1 : (unsigned char)buf[i]);
  }
  if (r->n) rinc_replChar(r, '\r'); /* (an unfinished last line) */
  r->loading = 0; r->echo = echo; r->n = r->line0 = r->cur = r->scr = 0; r->run = 0;
  return r->errors;
}
static void rinc_load(rinc_Repl *r, const char *name, int n, int run) {
  char path[96]; int errors;
  if (!rinc_appPath(path, sizeof path, name, n)) { rinc_say(r, "names are letters, digits, _ and -\r\n"); return; }
  if (!rinc_fs || rinc_fs->open(path, 0)) { rinc_say(r, rinc_fs ? "can't open " : "no storage: "); rinc_say(r, path); rinc_say(r, "\r\n"); return; }
  errors = rinc_feed(r, 0, 0);
  rinc_fs->close();
  if (errors < 0) return;
  rinc_say(r, errors ? "loaded, with errors: " : "loaded "); rinc_say(r, path); rinc_say(r, "\r\n");
  if (run && !errors) r->run = 1;
}
int rinc_replApp(rinc_Repl *r, const char *name, int run) { rinc_load(r, name, (int)strlen(name), run); return r->run ? 0 : -1; }
int rinc_replSource(rinc_Repl *r, const char *src, int len) {
  int errors = rinc_feed(r, src, len);
  if (!errors) r->run = 1;
  return errors ? -1 : 0;
}
static void rinc_saveFlush(rinc_Repl *r) { if (r->nout && rinc_fs->write(r->out, r->nout) < 0) r->saving = -1; r->saved += r->nout; r->nout = 0; }
static void rinc_saveChar(rinc_Repl *r, int c) { /* while saving: everything typed goes to the file, until Ctrl-D */
  if (c == 4 || c == 3) {
    rinc_saveFlush(r);
    rinc_say(r, rinc_PAINT_PLAIN "\r\n");
    if (rinc_fs->close() || r->saving < 0) rinc_say(r, "couldn't save it all\r\n");
    else if (c == 4) { char b[48]; rinc_say(r, "saved "); r->put(b, snprintf(b, sizeof b, "%d bytes\r\n", r->saved)); }
    else rinc_say(r, "gave up (the file has what came before)\r\n");
    r->saving = 0; rinc_prompt(r); return;
  }
  if (c == '\n' && r->cr) { r->cr = 0; return; } /* (\r\n: one line break) */
  r->cr = c == '\r';
  if (c == '\r') c = '\n';
  if (c != '\n' && c != '\t' && (c < 32 || c > 126)) return;
  r->out[r->nout++] = (char)c;
  if (r->nout == (int)sizeof r->out) rinc_saveFlush(r);
  if (r->echo) { if (c == '\n') r->put("\r\n", 2); else { char ch = (char)c; r->put(&ch, 1); } }
}
static void rinc_save(rinc_Repl *r, const char *name, int n) {
  char path[96];
  if (!rinc_appPath(path, sizeof path, name, n)) { rinc_say(r, "names are letters, digits, _ and -\r\n"); return; }
  if (!rinc_fs || rinc_fs->open(path, 1)) { rinc_say(r, rinc_fs ? "can't write " : "no storage: "); rinc_say(r, path); rinc_say(r, "\r\n"); return; }
  rinc_say(r, "saving "); rinc_say(r, path); rinc_say(r, ": type or paste it, then Ctrl-D (Ctrl-C gives up)\r\n");
  r->saving = 1; r->saved = 0; r->nout = 0; r->cr = 0;
}
/* a command line? (only as a whole input of its own) */
static int rinc_command(rinc_Repl *r) {
  const char *s = r->acc; int n = r->n, w;
  if (r->line0 || r->loading) return 0;
  while (n && s[n - 1] == ' ') n--;
  while (n && *s == ' ') { s++; n--; }
  for (w = 0; w < n && s[w] != ' '; w++) {}
  if (w == 4 && n == 4 && !memcmp(s, "home", 4)) { rinc_launch = 3; return 1; } /* (the platform goes back to the launcher) */
  if (w == 2 && n == 2 && !memcmp(s, "ls", 2)) {
    if (!rinc_fs) rinc_say(r, "no storage\r\n");
    else if (rinc_findApps() < 0) rinc_say(r, "no apps (or no card): /rinc/apps\r\n");
    else { int i; for (i = 0; i < rinc_appCount; i++) { rinc_say(r, rinc_appNames[i]); rinc_say(r, "  "); } rinc_say(r, rinc_appCount ? "\r\n" : "(none yet)\r\n"); }
    return 1;
  }
  if (n > w + 1 && ((w == 4 && !memcmp(s, "load", 4)) || (w == 3 && !memcmp(s, "run", 3)))) { rinc_load(r, s + w + 1, n - w - 1, w == 3); return 1; }
  if (n > w + 1 && w == 4 && !memcmp(s, "save", 4)) { rinc_save(r, s + w + 1, n - w - 1); return 1; }
  if (n > w + 1 && w == 6 && !memcmp(s, "delete", 6)) {
    char path[96]; const char *name = s + w + 1; int k = n - w - 1;
    if (!rinc_appPath(path, sizeof path, name, k)) rinc_say(r, "names are letters, digits, _ and -\r\n");
    else if (!rinc_fs || rinc_fs->remove(path)) { rinc_say(r, "can't delete "); rinc_say(r, path); rinc_say(r, "\r\n"); }
    else { path[strlen(path) - (size_t)k - 5] = 0; rinc_fs->remove(path); rinc_say(r, "deleted "); r->put(name, k); rinc_say(r, "\r\n"); } /* (then its folder, if empty) */
    return 1;
  }
  return 0;
}

static void rinc_newLine(rinc_Repl *r) { r->line0 = r->n; r->cur = r->n; r->scr = 0; r->mode = rinc_Mode_plain; r->word = -1; r->numdot = -1; r->back = 0; r->autoind = 0; }
static void rinc_key(rinc_Repl *r, int k) { /* an editing key: A up, B down, C right, D left, H home, F end, 3 delete */
  if (k == 'A' || k == 'B') { rinc_recall(r, k == 'A' ? 1 : -1); return; }
  if (k == 'C' && r->cur < r->n) r->cur++;
  else if (k == 'D' && r->cur > r->line0) r->cur--;
  else if (k == 'H') r->cur = r->line0;
  else if (k == 'F') r->cur = r->n;
  else if (k == '3' && r->cur < r->n) { memmove(r->acc + r->cur, r->acc + r->cur + 1, (size_t)(r->n - r->cur - 1)); r->n--; rinc_redraw(r); return; }
  else return;
  if (r->echo) { /* (just the cursor) */
    char b[16]; int d = r->cur - r->line0 - r->scr;
    if (d < 0) rinc_moveLeft(r, -d); else if (d > 0) r->put(b, snprintf(b, sizeof b, "\033[%dC", d));
    r->scr = r->cur - r->line0;
  }
}

void rinc_replChar(rinc_Repl *r, int c) {
  if (c < 0) return;
  if (r->saving) { rinc_saveChar(r, c); return; }
  if (r->esc == 1) { r->esc = c == '[' || c == 'O' ? 2 : 0; r->escn = 0; if (r->esc) return; } /* (else: a lone ESC, then c) */
  else if (r->esc == 2) {
    if (c >= '0' && c <= '9') { r->escn = r->escn * 10 + c - '0'; return; }
    r->esc = 0;
    if (c == '~') rinc_key(r, r->escn == 1 || r->escn == 7 ? 'H' : r->escn == 4 || r->escn == 8 ? 'F' : r->escn == 3 ? '3' : 0);
    else rinc_key(r, c);
    return;
  }
  if (c == 27) { r->esc = 1; return; }
  if (c == '\r' || c == '\n') {
    int res;
    rinc_outdent(r);
    rinc_key(r, 'F'); /* (to the end, then the last word's color, if it's done typing one) */
    if (r->echo) { rinc_finishWord(r); rinc_puts(r, rinc_PAINT_PLAIN "\r\n"); }
    if (r->line0 && rinc_blank(r)) { r->put("(unfinished input dropped)\r\n", 28); r->n = 0; rinc_newLine(r); rinc_prompt(r); return; }
    rinc_remember(r);
    if (!r->line0 && r->n == 3 && !memcmp(r->acc, "run", 3)) { r->n = 0; rinc_newLine(r); r->run = 1; return; } /* the platform takes it from here */
    if (rinc_command(r)) { r->n = 0; rinc_newLine(r); if (!r->saving && !r->run) rinc_prompt(r); return; }
    if (r->n >= (int)sizeof r->acc - 1) { r->put("(input too long)\r\n", 18); r->n = 0; rinc_newLine(r); rinc_prompt(r); return; }
    r->acc[r->n++] = '\n';
    res = rio_replEval(r->vm, r->acc, (uint32_t)r->n);
    if (res == rio_ReplResult_more) { rinc_newLine(r); rinc_prompt(r); rinc_indent(r); return; }
    if (res < 0) { const char *e = rio_error(r->vm); r->errors++; r->put(e, (int)strlen(e)); r->put("\r\n", 2); }
    r->n = 0; rinc_newLine(r);
    rinc_prompt(r);
  } else if (c == 8 || c == 127) { /* backspace (in the indent, a step) */
    r->autoind = 0;
    if (r->cur == r->n) {
      int k = r->n > r->line0 && rinc_blank(r) && r->n - r->line0 >= 2 ? 2 - (r->n - r->line0) % 2 : 1;
      while (k-- > 0 && r->n > r->line0) r->n--;
      r->cur = r->n;
    } else if (r->cur > r->line0) { memmove(r->acc + r->cur - 1, r->acc + r->cur, (size_t)(r->n - r->cur)); r->n--; r->cur--; }
    rinc_redraw(r);
  } else if (c == 1) rinc_key(r, 'H'); /* Ctrl-A, Ctrl-E: the start and end of the line */
  else if (c == 5) rinc_key(r, 'F');
  else if (c == 3) { /* Ctrl-C: drop what's typed */
    r->put(rinc_PAINT_PLAIN "^C\r\n", 8); r->n = 0; rinc_newLine(r); rinc_prompt(r);
  } else if (c >= 32 && c < 127 && r->n < (int)sizeof r->acc - 2) {
    if (c == ' ' && r->autoind > 0 && r->cur == r->n && rinc_blank(r)) { r->autoind--; return; } /* (in place of the auto indent) */
    if (c != ' ') r->autoind = 0;
    if (c == ' ') rinc_outdent(r);
    if (r->cur == r->n) rinc_append(r, (char)c);
    else { memmove(r->acc + r->cur + 1, r->acc + r->cur, (size_t)(r->n - r->cur)); r->acc[r->cur++] = (char)c; r->n++; rinc_redraw(r); }
  }
}
#endif

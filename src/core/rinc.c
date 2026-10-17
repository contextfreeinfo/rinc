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
/* key() -> Int: the next key typed, or -1 when there's none waiting (docs/spec/input.md#typed-text):
   printable ASCII, 13 Enter, 8 Backspace, 9 Tab, 27 Esc, Ctrl+letter as 1-26, and editing keys as
   rinc_Key_*; the keys that aren't characters with the modifiers held (rinc_Mod_*). The keys also press
   buttons as they're held: an app reads whichever it wants. A nested session (pc.rioStart) has a queue
   of its own, which its app fills with pc.rioType */
#define rinc_TEXT_CAP 64
typedef struct { uint16_t q[rinc_TEXT_CAP]; int head, tail, esc, escn, escp[3]; } rinc_Text;
static rinc_Text rinc_typed, rinc_nestTyped;
static void rinc_queue(rinc_Text *t, int c) {
  int next = (t->head + 1) % rinc_TEXT_CAP;
  if (next != t->tail) { t->q[t->head] = (uint16_t)c; t->head = next; }
}
static int rinc_dequeue(rinc_Text *t) {
  int c;
  if (t->tail == t->head) return -1;
  c = t->q[t->tail]; t->tail = (t->tail + 1) % rinc_TEXT_CAP;
  return c;
}
int rinc_textMods(int shift, int ctrl, int alt) { return (shift ? rinc_Mod_shift : 0) | (ctrl ? rinc_Mod_ctrl : 0) | (alt ? rinc_Mod_alt : 0); }
/* a terminal's bytes: ESC [ (or ESC O) and a letter, or numbers and ~, is an editing key. Numbers are
   separated by ; and a key's modifiers are its second (1 + 1 Shift, 2 Alt, 4 Ctrl), as xterm sends
   them: ESC [ 1 ; 5 C is Ctrl+right, ESC [ 3 ; 2 ~ Shift+Delete, and ESC [ 27 ; m ; k ~ key k (Tab,
   Enter, Backspace, a letter for Ctrl+letter) with modifiers m. ESC [ Z is Shift+Tab */
void rinc_textPush(int c) {
  rinc_Text *t = &rinc_typed;
  if (t->esc == 1) {
    if (c == '[' || c == 'O') { t->esc = 2; t->escn = 0; t->escp[0] = t->escp[1] = t->escp[2] = 0; return; }
    t->esc = 0; rinc_queue(t, 27); /* (a lone ESC: Esc itself, then c) */
  } else if (t->esc == 2) {
    int n = t->escp[0], m = t->escp[1] > 1 ? t->escp[1] - 1 : 0, mods = rinc_textMods(m & 1, m & 4, m & 2), k = t->escp[2];
    if (c >= '0' && c <= '9') { if (t->escp[t->escn] < 10000) t->escp[t->escn] = t->escp[t->escn] * 10 + c - '0'; return; }
    if (c == ';') { if (t->escn < 2) t->escn++; return; }
    t->esc = 0;
    if (c == '~' && n == 27) { /* (xterm's modifyOtherKeys) */
      if (k >= 'A' && k <= 'Z') k += 32;
      c = k == 9 || k == 13 ? k : k == 8 || k == 127 ? 8 : k >= 'a' && k <= 'z' && (mods & rinc_Mod_ctrl) ? k - 'a' + 1 : 0;
      if (c >= 1 && c <= 26 && c != 8 && c != 9 && c != 13) mods &= ~rinc_Mod_ctrl; /* (Ctrl+letter says Ctrl itself) */
    } else
      c = c == 'A' ? rinc_Key_up : c == 'B' ? rinc_Key_down : c == 'C' ? rinc_Key_right : c == 'D' ? rinc_Key_left :
          c == 'H' ? rinc_Key_home : c == 'F' ? rinc_Key_end : c == 'Z' ? 9 | rinc_Mod_shift : c != '~' ? 0 :
          n == 1 || n == 7 ? rinc_Key_home : n == 4 || n == 8 ? rinc_Key_end : n == 3 ? rinc_Key_delete :
          n == 5 ? rinc_Key_pageUp : n == 6 ? rinc_Key_pageDown : 0;
    if (c) rinc_queue(t, c | mods);
    return;
  }
  if (c == 27) { t->esc = 1; return; }
  if (c == '\n') c = 13;
  if (c == 127) c = 8;
  if ((c >= 1 && c <= 26) || (c >= 32 && c < 127)) rinc_queue(t, c);
}
void rinc_textKey(int key) { rinc_typed.esc = 0; if (key > 0 && key < 2048) rinc_queue(&rinc_typed, key); }
int rinc_textRoom(void) { return rinc_TEXT_CAP - 1 - (rinc_typed.head - rinc_typed.tail + rinc_TEXT_CAP) % rinc_TEXT_CAP; }
/* apps, for home: apps() -> Int looks again, in storage and in the bundle, and says how many (home
   aside); appName(i, out) -> Int and appInfo(i, out) -> Int copy app i's name, or its description (its
   source's first line, the comment marker left off: "Title: what it does"), into out, and say how long
   it is. launch(name) asks the platform to switch to an app, after this frame */
const rinc_Fs *rinc_fs;
static const uint8_t *rinc_bundle; static int rinc_bundleCount; /* (read where it is: on the Fruit Jam, flash) */
static char rinc_bundleStorage[33], rinc_bundleHome[33] = "home";
int rinc_launch; char rinc_launchName[33];
/* the bundle format (little-endian): "RINCBUN1", the file count (4 bytes), the storage folder's name and
   the home app's name (32 bytes each, 0-padded), then each file's entry: its path (96 bytes, 0-padded,
   as in storage without /rinc/: apps/chord/chord.rio), and where its bytes are (4, from the bundle's
   start) and how many (4); then the bytes */
#define rinc_BUNDLE_HEAD 76
#define rinc_BUNDLE_ENTRY 104
static uint32_t rinc_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void rinc_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void rinc_text(char *out, const uint8_t *in, int n) { memcpy(out, in, (size_t)n); out[n] = 0; }
int rinc_bundleRead(const void *bundle, uint32_t size) {
  const uint8_t *b = (const uint8_t *)bundle; uint32_t n, i;
  if (size < rinc_BUNDLE_HEAD || memcmp(b, rinc_BUNDLE_MAGIC, 8) || (n = rinc_le32(b + 8)) > rinc_BUNDLE_MOST || rinc_BUNDLE_HEAD + n * rinc_BUNDLE_ENTRY > size) return -1;
  for (i = 0; i < n; i++) {
    const uint8_t *e = b + rinc_BUNDLE_HEAD + i * rinc_BUNDLE_ENTRY; uint32_t at = rinc_le32(e + 96), len = rinc_le32(e + 100);
    if (at > size || len > size - at) return -1;
  }
  rinc_text(rinc_bundleStorage, b + 12, 32);
  rinc_text(rinc_bundleHome, b + 44, 32);
  rinc_bundle = b;
  rinc_bundleCount = (int)n;
  return 0;
}
static void rinc_fill(uint8_t *out, const char *s, size_t n) { size_t k = strlen(s); memcpy(out, s, k < n ? k : n); } /* (the rest stays 0) */
uint32_t rinc_bundleWrite(uint8_t *out, uint32_t cap, const char *name, const char *home, const rinc_File *files, int n) {
  uint32_t at = rinc_BUNDLE_HEAD + (uint32_t)n * rinc_BUNDLE_ENTRY; int i;
  if (n > rinc_BUNDLE_MOST || at > cap) return 0;
  memset(out, 0, at);
  memcpy(out, rinc_BUNDLE_MAGIC, 8); rinc_put32(out + 8, (uint32_t)n);
  rinc_fill(out + 12, name, 32); rinc_fill(out + 44, home, 32);
  for (i = 0; i < n; i++) {
    uint8_t *e = out + rinc_BUNDLE_HEAD + i * rinc_BUNDLE_ENTRY;
    if (files[i].len > cap - at) return 0;
    if (strlen(files[i].path) >= 96) return 0; /* (paths end with a 0 in the bundle) */
    rinc_fill(e, files[i].path, 96);
    rinc_put32(e + 96, at); rinc_put32(e + 100, files[i].len);
    memcpy(out + at, files[i].data, files[i].len);
    at += files[i].len;
  }
  return at;
}
const char *rinc_bundleName(void) { return rinc_bundleStorage; }
const char *rinc_appFor(const char *name) {
  if (strcmp(name, "home") || rinc_readApp("home", 0, 0) != -1) return name;
  return rinc_bundleHome;
}
static char rinc_appNow[33] = "app"; /* the app running, for its save space */
void rinc_setAppName(const char *name) {
  size_t n = strlen(name);
  if (n >= sizeof rinc_appNow) n = sizeof rinc_appNow - 1;
  memcpy(rinc_appNow, name, n); rinc_appNow[n] = 0;
}
static const char *rinc_entryPath(int i) { return (const char *)rinc_bundle + rinc_BUNDLE_HEAD + i * rinc_BUNDLE_ENTRY; }
const rinc_File *rinc_bundleFile(const char *path) {
  static rinc_File f; int i;
  for (i = 0; i < rinc_bundleCount; i++)
    if (!strncmp(rinc_entryPath(i), path, 96)) {
      const uint8_t *e = (const uint8_t *)rinc_entryPath(i);
      f.path = (const char *)e; f.data = (const char *)rinc_bundle + rinc_le32(e + 96); f.len = rinc_le32(e + 100);
      return &f;
    }
  return 0;
}
/* app name in the bundle: apps/name/name.rio (a source), or apps/name/name.img (an image, its
   description in apps/name/name.info) */
const rinc_App *rinc_bundled(const char *name) {
  static rinc_App a; char path[96]; const rinc_File *f, *info; uint32_t k; const char *data; uint32_t len;
  if (strlen(name) > 32) return 0;
  snprintf(path, sizeof path, "apps/%s/%s.rio", name, name);
  a.image = !(f = rinc_bundleFile(path));
  if (a.image) {
    snprintf(path, sizeof path, "apps/%s/%s.img", name, name);
    if (!(f = rinc_bundleFile(path))) return 0;
  }
  data = f->data; len = f->len; /* (before the next lookup, which reuses f) */
  snprintf(a.name, sizeof a.name, "%s", name);
  a.data = data; a.len = len; a.info[0] = 0;
  snprintf(path, sizeof path, "apps/%s/%s.info", name, name);
  if (a.image && (info = rinc_bundleFile(path))) {
    for (k = 0; k < info->len && k < sizeof a.info - 1 && info->data[k] != '\n' && info->data[k] != '\r'; k++) a.info[k] = info->data[k];
    a.info[k] = 0;
  }
  return &a;
}
static uint32_t rinc_appLen(const rinc_App *a) { return a->len; }
/* a name for an app (letters, digits, _ and -) or, with dots, for a file in a save space (not starting
   with one): 1 to 32 of them */
static int rinc_nameOk(const char *s, int n, int dots) {
  int i;
  if (!s || n < 1 || n > 32 || (dots && s[0] == '.')) return 0;
  for (i = 0; i < n; i++)
    if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-' || (dots && s[i] == '.'))) return 0;
  return 1;
}
static char rinc_appNames[64][33]; static int rinc_appCount;
static void rinc_appAdd(const char *name) {
  size_t n = strlen(name); int i;
  if (!rinc_nameOk(name, (int)n, 0) || !strcmp(name, "home") || !strcmp(name, rinc_bundleHome) || rinc_appCount >= 64) return; /* (home: where this list is shown) */
  for (i = 0; i < rinc_appCount; i++) if (!strcmp(rinc_appNames[i], name)) return;
  memcpy(rinc_appNames[rinc_appCount++], name, n + 1);
}
static void rinc_appFound(const char *name, int dir, void *ud) { (void)ud; if (dir) rinc_appAdd(name); }
static int rinc_lowerCmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
    if (x != y || !x) return x - y;
  }
}
/* the apps, in storage and in the bundle, in order of name (ignoring case): how many */
static int rinc_findApps(void) {
  int i, j;
  rinc_appCount = 0;
  if (rinc_fs) rinc_fs->list("/rinc/apps", rinc_appFound, 0);
  for (i = 0; i < rinc_bundleCount; i++) { /* (apps/name/name.rio or .img) */
    const char *q = rinc_entryPath(i), *slash; char name[33]; size_t k;
    if (strncmp(q, "apps/", 5) || !(slash = strchr(q + 5, '/')) || (k = (size_t)(slash - q - 5)) > 32) continue;
    memcpy(name, q + 5, k); name[k] = 0;
    if (!strncmp(slash + 1, name, k) && (!strcmp(slash + 1 + k, ".rio") || !strcmp(slash + 1 + k, ".img"))) rinc_appAdd(name);
  }
  for (i = 1; i < rinc_appCount; i++) /* (a few dozen at most: insertion) */
    for (j = i; j > 0 && rinc_lowerCmp(rinc_appNames[j - 1], rinc_appNames[j]) > 0; j--) {
      char t[sizeof rinc_appNames[0]]; memcpy(t, rinc_appNames[j], sizeof t); memcpy(rinc_appNames[j], rinc_appNames[j - 1], sizeof t); memcpy(rinc_appNames[j - 1], t, sizeof t);
    }
  return rinc_appCount;
}
/* a file in storage, into buf as far as it fits: 0 (*total: its whole length), or -1 */
static int rinc_readFile(const char *path, char *buf, uint32_t cap, uint32_t *total) {
  char chunk[256]; int got;
  *total = 0;
  if (!rinc_fs || rinc_fs->open(path, 0)) return -1;
  while ((got = rinc_fs->read(chunk, sizeof chunk)) > 0) {
    if (buf && *total < cap) memcpy(buf + *total, chunk, (size_t)got < cap - *total ? (size_t)got : cap - *total);
    *total += (uint32_t)got;
  }
  rinc_fs->close();
  return got < 0 ? -1 : 0;
}
static int rinc_appPath(const char *name, char *path, size_t cap) {
  return rinc_nameOk(name, (int)strlen(name), 0) && snprintf(path, cap, "/rinc/apps/%s/%s.rio", name, name) < (int)cap;
}
int rinc_readApp(const char *name, char *buf, uint32_t cap) {
  char path[96]; uint32_t total;
  if (!rinc_appPath(name, path, sizeof path) || rinc_readFile(path, buf, cap, &total)) return -1;
  return total > cap ? -2 : (int)total;
}
static void rinc_hostApps(rio_Vm *v, rio_Val *a) { (void)v; a[0].i = rinc_findApps(); }
static int rinc_copyOut(rio_Vm *v, rio_Val *out, const char *s, int n) {
  char *p = (char *)rio_sliceBytes(v, out); int cap = (int)out[1].u;
  if (p) memcpy(p, s, (size_t)(n < cap ? n : cap));
  return n;
}
static void rinc_hostAppName(rio_Vm *v, rio_Val *a) {
  a[0].i = a[0].u < (uint32_t)rinc_appCount ? rinc_copyOut(v, a + 1, rinc_appNames[a[0].i], (int)strlen(rinc_appNames[a[0].i])) : 0;
}
static void rinc_hostAppInfo(rio_Vm *v, rio_Val *a) {
  char path[96], line[128]; const char *t = line; uint32_t n = 0; int i, k = a[0].i; const rinc_App *b;
  a[0].i = 0;
  if (k < 0 || k >= rinc_appCount) return;
  if (rinc_appPath(rinc_appNames[k], path, sizeof path) && !rinc_readFile(path, line, sizeof line, &n)) {
    if (n > sizeof line) n = sizeof line;
  } else if ((b = rinc_bundled(rinc_appNames[k])) && b->image) {
    a[0].i = rinc_copyOut(v, a + 1, b->info, (int)strlen(b->info));
    return;
  } else if (b) { t = b->data; n = rinc_appLen(b); }
  if (n == 0 || t[0] != '#') return;
  for (i = 1; i < (int)n && t[i] == ' '; i++) {}
  for (k = i; k < (int)n && t[k] != '\n' && t[k] != '\r'; k++) {}
  a[0].i = rinc_copyOut(v, a + 1, t + i, k - i);
}
static void rinc_hostLaunch(rio_Vm *v, rio_Val *a) {
  const char *s = (const char *)rio_sliceBytes(v, a); int n = (int)a[1].u;
  if (!s || n < 1 || n >= (int)sizeof rinc_launchName) return;
  memcpy(rinc_launchName, s, (size_t)n); rinc_launchName[n] = 0; rinc_launch = 1;
}
void rinc_textEsc(void) { rinc_typed.esc = 0; rinc_queue(&rinc_typed, 27); }
static rio_Vm rinc_nest; static int rinc_nestOn, rinc_nestTried; /* (below: Rio inside an app) */
static void rinc_hostKey(rio_Vm *v, rio_Val *a) { a[0].i = rinc_dequeue(v == &rinc_nest ? &rinc_nestTyped : &rinc_typed); }

/* apps' sources, for a console or an editor: appSource(name, out) -> Int copies app name's source (from
   storage, else the bundle) into out, as much as fits, and gives its whole length (-1: no such source);
   appSave(name, text) -> Int and appDelete(name) -> Int change storage: 0, or -1. And an app's own save
   space, /rinc/save/<app>: saveRead(file, out) -> Int (as appSource), saveWrite(file, data) -> Int and
   saveDelete(file) -> Int (0, or -1). Each reads its name first: a[0], the name's address, takes the
   result */
static int rinc_argName(rio_Vm *v, rio_Val *name, char *out, int dots) {
  const char *s = (const char *)rio_sliceBytes(v, name); int n = (int)name[1].u;
  if (!rinc_nameOk(s, n, dots)) return 0;
  memcpy(out, s, (size_t)n); out[n] = 0;
  return 1;
}
static int rinc_savePath(rio_Vm *v, rio_Val *file, char *path, size_t cap) {
  char own[33];
  return rinc_argName(v, file, own, 1) && snprintf(path, cap, "/rinc/save/%s/%s", rinc_appNow, own) < (int)cap;
}
static void rinc_hostAppSource(rio_Vm *v, rio_Val *a) {
  char name[33], path[96]; uint8_t *out = (uint8_t *)rio_sliceBytes(v, a + 2); uint32_t cap = a[3].u, total; const rinc_App *b;
  int named = rinc_argName(v, a, name, 0);
  a[0].i = -1;
  if (!named) return;
  if (rinc_appPath(name, path, sizeof path) && !rinc_readFile(path, (char *)out, out ? cap : 0, &total)) a[0].i = (int32_t)total;
  else if ((b = rinc_bundled(name)) && !b->image) {
    total = rinc_appLen(b);
    if (out) memcpy(out, b->data, total < cap ? total : cap);
    a[0].i = (int32_t)total;
  }
}
static void rinc_writeFile(rio_Vm *v, rio_Val *a, const char *path) {
  const char *text = (const char *)rio_sliceBytes(v, a + 2); int n = (int)a[3].u, bad;
  a[0].i = -1;
  if (!rinc_fs || rinc_fs->open(path, 1)) return;
  bad = n > 0 && (!text || rinc_fs->write(text, n) != n);
  a[0].i = rinc_fs->close() || bad ? -1 : 0;
}
static void rinc_hostAppSave(rio_Vm *v, rio_Val *a) {
  char name[33], path[96]; int named = rinc_argName(v, a, name, 0) && rinc_appPath(name, path, sizeof path);
  if (named) rinc_writeFile(v, a, path); else a[0].i = -1;
}
static void rinc_hostAppDelete(rio_Vm *v, rio_Val *a) {
  char name[33], path[96]; int named = rinc_argName(v, a, name, 0) && rinc_appPath(name, path, sizeof path);
  a[0].i = -1;
  if (!named || !rinc_fs || rinc_fs->remove(path)) return;
  *strrchr(path, '/') = 0; rinc_fs->remove(path); /* (then its folder, if that's empty now) */
  a[0].i = 0;
}
static void rinc_hostSaveRead(rio_Vm *v, rio_Val *a) {
  char path[96]; uint8_t *out = (uint8_t *)rio_sliceBytes(v, a + 2); uint32_t cap = a[3].u, total; int named = rinc_savePath(v, a, path, sizeof path);
  a[0].i = named && !rinc_readFile(path, (char *)out, out ? cap : 0, &total) ? (int32_t)total : -1;
}
static void rinc_hostSaveWrite(rio_Vm *v, rio_Val *a) {
  char path[96]; int named = rinc_savePath(v, a, path, sizeof path);
  if (named) rinc_writeFile(v, a, path); else a[0].i = -1;
}
static void rinc_hostSaveDelete(rio_Vm *v, rio_Val *a) {
  char path[96]; int named = rinc_savePath(v, a, path, sizeof path);
  a[0].i = named && rinc_fs && !rinc_fs->remove(path) ? 0 : -1;
}

/* Rio inside an app (docs/spec/rio.md): one session at a time, a REPL in memory the app hands over.
   rioStart(mem, tables) -> Int: a fresh one, its code and data in mem and its compiler's tables in
   tables (0, or -1); rioEval(text) -> Int: compile and run a piece (0 done, 1 unfinished: it needs more,
   -1 an error); rioFrame() -> Int: its update then draw (0, 1 if it has neither, -1 an error);
   rioType(key): a key for its pc.key(); rioError(out) -> Int and rioLog(out) -> Int: the last error, and
   what it has logged since (taken), as text; rioIndent(text) -> Int: the indent steps an unfinished
   input's next line takes. The session has pc too, but not a session of its own */
static char rinc_nestLog[2048]; static int rinc_nestLogLen;
static void rinc_nestLogLine(void *ud, const char *s, int n) {
  (void)ud;
  if (n > (int)sizeof rinc_nestLog - 1 - rinc_nestLogLen) n = (int)sizeof rinc_nestLog - 1 - rinc_nestLogLen; /* (full: the rest is lost) */
  if (n < 0) return;
  memcpy(rinc_nestLog + rinc_nestLogLen, s, (size_t)n); rinc_nestLogLen += n;
  rinc_nestLog[rinc_nestLogLen++] = '\n';
}
static int rinc_copyText(rio_Vm *v, rio_Val *out, const char *s, int n) {
  char *p = (char *)rio_sliceBytes(v, out); int cap = (int)out[1].u;
  if (n > cap) n = cap;
  if (p && n > 0) memcpy(p, s, (size_t)n);
  return p ? n : 0;
}
#ifdef rio_NO_COMPILER /* (a vm that only runs images, as the web player's: no sessions, nothing to compile) */
static int rinc_noCompiler(void) { return -1; }
#define rio_replBegin(vm, scratch, size, lim) rinc_noCompiler()
#define rio_replEval(vm, src, len) rinc_noCompiler()
#define rio_replIndent(src, len) 0
#define rio_compile(vm, src, len) rinc_noCompiler()
#endif
static void rinc_hostRioStart(rio_Vm *v, rio_Val *a) {
  uint8_t *mem = (uint8_t *)rio_sliceBytes(v, a), *tables = (uint8_t *)rio_sliceBytes(v, a + 2);
  a[0].i = -1;
  rinc_nestOn = rinc_nestLogLen = 0; rinc_nestTried = 1;
  memset(&rinc_nestTyped, 0, sizeof rinc_nestTyped);
  if (v == &rinc_nest || !mem || !tables || rio_init(&rinc_nest, mem, a[1].u, 0, 0)) return;
  rio_setLog(&rinc_nest, rinc_nestLogLine, 0);
  if (rinc_hostProcs(&rinc_nest) || rio_autoImport(&rinc_nest, "pc") || rio_replBegin(&rinc_nest, tables, a[3].u, 0)) return;
  rinc_nestOn = 1; a[0].i = 0;
}
static void rinc_hostRioEval(rio_Vm *v, rio_Val *a) {
  const char *s = (const char *)rio_sliceBytes(v, a);
  a[0].i = v != &rinc_nest && rinc_nestOn && s ? rio_replEval(&rinc_nest, s, a[1].u) : -1;
}
static void rinc_hostRioFrame(rio_Vm *v, rio_Val *a) { a[0].i = v != &rinc_nest && rinc_nestOn ? rinc_frame(&rinc_nest) : -1; }
static void rinc_hostRioType(rio_Vm *v, rio_Val *a) { if (v != &rinc_nest && a[0].i > 0 && a[0].i < 2048) rinc_queue(&rinc_nestTyped, a[0].i); }
static void rinc_hostRioError(rio_Vm *v, rio_Val *a) {
  const char *e = rinc_nestOn || rinc_nestTried ? rio_error(&rinc_nest) : "no session (pc.rioStart starts one)";
  a[0].i = v == &rinc_nest ? 0 : rinc_copyText(v, a, e, (int)strlen(e));
}
static void rinc_hostRioLog(rio_Vm *v, rio_Val *a) {
  int n = v == &rinc_nest ? 0 : rinc_copyText(v, a, rinc_nestLog, rinc_nestLogLen);
  memmove(rinc_nestLog, rinc_nestLog + n, (size_t)(rinc_nestLogLen - n)); rinc_nestLogLen -= n;
  a[0].i = n;
}
static void rinc_hostRioIndent(rio_Vm *v, rio_Val *a) { const char *s = (const char *)rio_sliceBytes(v, a); a[0].i = s ? rio_replIndent(s, a[1].u) : 0; }
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
   line breaks, is skipped) into out, until it's full. Returns how many. For binary data typed into an
   app: sound, pictures */
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

int rinc_game, rinc_showLog;
/* what an app logs, and what Rinc says about it: to the platform, and on the screen while rinc_showLog */
static void rinc_out(const char *s, int n) { rinc_put(s, n); if (rinc_showLog) rinc_consolePut(s, n); }
void rinc_say(const char *s) { rinc_out(s, (int)strlen(s)); }
static void rinc_logLine(void *ud, const char *s, int n) { (void)ud; rinc_out(s, n); rinc_out("\r\n", 2); }
/* the way home: on the screen (it's for whoever's looking at it), not in what's printed */
static void rinc_back(void) { static const char s[] = "\r\n(Sys+Esc: back home)\r\n"; if (rinc_showLog) rinc_consolePut(s, (int)sizeof s - 1); }

int rinc_start(rio_Vm *vm, void *mem, uint32_t memsize, void (*put)(const char *s, int n)) {
  rinc_put = put;
  rinc_initPalette();
  if (rio_init(vm, mem, memsize, 0, 0)) return -1; /* no code buffer: the code goes in mem too */
  rinc_audioReset();
  rio_setLog(vm, rinc_logLine, 0);
  if (rinc_hostProcs(vm) || rio_autoImport(vm, "pc")) return -1; /* (pc.cls and so on, everywhere) */
  memset(&rinc_typed, 0, sizeof rinc_typed);
  rinc_nestOn = rinc_nestTried = 0; /* (a session lives in an app's memory, which is starting over) */
  return 0;
}

int rinc_compile(rio_Vm *vm, void *space, uint32_t size, const char *src, uint32_t len) {
  char *at = (char *)space + size - len;
  (void)vm; /* (unused without a compiler) */
  if (len > rinc_MAX_SOURCE || len > size) return 1;
  if (src != at) memmove(at, src, len);
  return rio_compile(vm, at, len) ? -1 : 0;
}

int rinc_atLimit(const char *e) { return strstr(e, "out of") || strstr(e, "too many") || strstr(e, "too complex") || strstr(e, "too deep"); }

static int rinc_systemDown;
int rinc_systemHeld(void) { return rinc_systemDown; }
uint8_t rinc_overlay[rinc_OVERLAY_H][rinc_W];
volatile int rinc_overlayOn;
/* text into the overlay, at pixel x on its row */
static int rinc_overlayText(const char *s, int x, int fg) {
  for (; *s; s++, x += 6) {
    const uint8_t *g = *s > 32 && *s < 127 ? rinc_font[*s - 32] : rinc_font[0];
    int r, k;
    for (r = 0; r < 8; r++)
      for (k = 0; k < 6 && x + k < rinc_W; k++) if (g[r] >> k & 1) rinc_overlay[2 + r][x + k] = (uint8_t)fg;
  }
  return x;
}
static void rinc_overlayDraw(void) {
  int x;
  memset(rinc_overlay, 0, sizeof rinc_overlay);
  memset(rinc_overlay[0], 10, rinc_W); /* (a yellow edge) */
  x = rinc_overlayText("SYSTEM", 4, 10);
  rinc_overlayText("Esc: home", x + 12, 7);
}
int rinc_systemKey(int sysDown, int shiftDown, int otherDown, int escDown) {
  /* Shift as the system key: tapped (pressed and let go with nothing else), then pressed again within
     0.4 s: the system key while it's held, and for 0.7 s after it's let go (so Esc can come after, as
     browsers keep Shift+Esc). shift: 0 nothing yet, 1 the tap's press, 2 tapped, 3 held as the system
     key, 4 let go, still the system key for the next key */
  static int shift, frames, wasShift, wasEsc;
  int esc = escDown && !wasEsc, home;
  if (frames < 1000) frames++;
  if (shiftDown && !wasShift) { shift = shift == 2 && frames <= 24 ? 3 : 1; frames = 0; }
  else if (!shiftDown && wasShift) { shift = shift == 1 && frames <= 24 ? 2 : shift == 3 ? 4 : 0; frames = 0; }
  else if (shift == 1 && otherDown) shift = 0; /* (Shift held for typing capitals) */
  else if (shift == 4 && ((otherDown && !escDown) || frames > 42)) shift = 0; /* (another key, or too late) */
  wasShift = shiftDown;
  wasEsc = escDown;
  rinc_systemDown = sysDown || shift >= 3;
  home = rinc_systemDown && esc; /* (system + Esc: home. Alone, it's nothing, so it isn't pressed by accident) */
  if (home && shift == 4) shift = 0;
  if (rinc_systemDown && !rinc_overlayOn) rinc_overlayDraw();
  rinc_overlayOn = rinc_systemDown && !home;
  return home;
}

int rinc_hostProcs(rio_Vm *vm) {
  return rio_addFfi(vm, "pc.cls", "i", rinc_hostCls) || rio_addFfi(vm, "pc.pset", "iii", rinc_hostPset) || rio_addFfi(vm, "pc.pget", "ii>i", rinc_hostPget) ||
         rio_addFfi(vm, "pc.rect", "iiiii", rinc_hostRect) || rio_addFfi(vm, "pc.print", "siii", rinc_hostPrint) || rio_addFfi(vm, "pc.btn", "i>i", rinc_hostBtn) || rinc_audioProcs(vm) || rio_addFfi(vm, "pc.time", ">f", rinc_hostTime) ||
         rio_addFfi(vm, "pc.fromHex", "bs>i", rinc_hostFromhex) || rio_addFfi(vm, "pc.stick", "i>f", rinc_hostStick) || rio_addFfi(vm, "pc.trigger", "i>f", rinc_hostTrigger) ||
         rio_addFfi(vm, "pc.key", ">i", rinc_hostKey) || rio_addFfi(vm, "pc.apps", ">i", rinc_hostApps) || rio_addFfi(vm, "pc.appName", "ib>i", rinc_hostAppName) ||
         rio_addFfi(vm, "pc.appInfo", "ib>i", rinc_hostAppInfo) || rio_addFfi(vm, "pc.launch", "s", rinc_hostLaunch) ||
         rio_addFfi(vm, "pc.appSource", "sb>i", rinc_hostAppSource) || rio_addFfi(vm, "pc.appSave", "ss>i", rinc_hostAppSave) ||
         rio_addFfi(vm, "pc.appDelete", "s>i", rinc_hostAppDelete) ||
         rio_addFfi(vm, "pc.saveRead", "sb>i", rinc_hostSaveRead) || rio_addFfi(vm, "pc.saveWrite", "ss>i", rinc_hostSaveWrite) ||
         rio_addFfi(vm, "pc.saveDelete", "s>i", rinc_hostSaveDelete) || rio_addFfi(vm, "pc.rioStart", "bb>i", rinc_hostRioStart) ||
         rio_addFfi(vm, "pc.rioEval", "s>i", rinc_hostRioEval) || rio_addFfi(vm, "pc.rioFrame", ">i", rinc_hostRioFrame) ||
         rio_addFfi(vm, "pc.rioType", "i", rinc_hostRioType) || rio_addFfi(vm, "pc.rioError", "b>i", rinc_hostRioError) ||
         rio_addFfi(vm, "pc.rioLog", "b>i", rinc_hostRioLog) || rio_addFfi(vm, "pc.rioIndent", "s>i", rinc_hostRioIndent);
}

/* a game's frame: update then draw */
/* starting an app, the same on every Rinc: rinc_appBegin, then its source read (into the top of the app
   space, where it compiles), then rinc_appRun. Whatever goes wrong goes on the screen, with the way home
   (and, when it ran out of something, how near each limit it came) */
static rio_Usage rinc_usage;
int rinc_appBegin(rio_Vm *vm, void *space, uint32_t size, const char *name, void (*put)(const char *s, int n)) {
  rinc_game = 0;
  rinc_showLog = 1;
  rinc_put = put;
  rinc_consoleClear(1);
  if (rinc_start(vm, space, size, put)) { rinc_say("can't start rio"); rinc_back(); return -1; }
  rinc_setAppName(name);
  return 0;
}
void rinc_appFail(const char *name, const char *why) {
  rinc_say(name); rinc_say(why); rinc_back();
}
int rinc_appRun(rio_Vm *vm, void *space, uint32_t size, const char *name, const char *src, uint32_t len, const void *image) {
  int res, fn;
  if (image) res = rio_loadImage(vm, image, len);
  else {
#ifdef rio_NO_COMPILER
    (void)space; (void)size; (void)src;
    rinc_appFail(name, ": this Rinc runs compiled apps only");
    return -1;
#else
    rio_trackUsage(vm, &rinc_usage);
    if ((res = rinc_compile(vm, space, size, src, len)) > 0) { rinc_appFail(name, ".rio: too big (64 KB is the most)"); return -1; }
#endif
  }
  if (res) {
    rinc_say(name); rinc_say(image ? ": " : ".rio:"); rinc_say(rio_error(vm));
#ifndef rio_NO_COMPILER
    if (!image && rinc_atLimit(rio_error(vm))) {
      static char text[1024];
      int k, m = rio_usageText(&rinc_usage, text, sizeof text);
      rinc_say("\r\n");
      for (k = 0; k < m; k++) rinc_out(text[k] == '\n' ? "\r\n" : text + k, text[k] == '\n' ? 2 : 1);
    }
#endif
    rinc_back();
    return -1;
  }
  if (rio_run(vm) || ((fn = rio_findProc(vm, "main")) >= 0 && rio_call(vm, fn))) {
    rinc_say(name); rinc_say(".rio:"); rinc_say(rio_error(vm)); rinc_back();
    return -1;
  }
  if (rio_findProc(vm, "update") >= 0 || rio_findProc(vm, "draw") >= 0) { rinc_game = 1; rinc_showLog = 0; }
  else { rinc_consolePut("\r\n(done)", 8); rinc_back(); }
  return 0;
}
int rinc_gameFrame(rio_Vm *vm) {
  int res;
  if (!rinc_game) return 1;
  if ((res = rinc_frame(vm)) != 0) { /* an error, or no update or draw any more */
    rinc_game = 0; rinc_showLog = 1; rinc_audioReset(); rinc_consoleClear(1);
    rinc_say(res < 0 ? rio_error(vm) : "nothing to run: define update* or draw*");
    rinc_back();
  }
  return res;
}

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
   the same. Typing at the end of the line, the usual case (and all of a pasted app), sends little
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
static void rinc_replSay(rinc_Repl *r, const char *s) { r->put(s, (int)strlen(s)); }
static int rinc_replAppPath(char *path, size_t cap, const char *name, int n) {
  int i;
  if (n < 1 || n > 32) return 0;
  for (i = 0; i < n; i++) if (!(rinc_wordChar(name[i]) || name[i] == '-')) return 0;
  return snprintf(path, cap, "/rinc/apps/%.*s/%.*s.rio", n, name, n, name) < (int)cap;
}

/* source, fed in as if typed, into a fresh session: from storage (src NULL) or from memory */
static int rinc_feed(rinc_Repl *r, const char *src, int len) {
  char buf[256]; int got, i, echo = r->echo, at = 0;
  if (r->restart && r->restart()) { rinc_replSay(r, "can't start a fresh session\r\n"); return -1; }
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
  if (!rinc_replAppPath(path, sizeof path, name, n)) { rinc_replSay(r, "names are letters, digits, _ and -\r\n"); return; }
  if (!rinc_fs || rinc_fs->open(path, 0)) { rinc_replSay(r, rinc_fs ? "can't open " : "no storage: "); rinc_replSay(r, path); rinc_replSay(r, "\r\n"); return; }
  errors = rinc_feed(r, 0, 0);
  rinc_fs->close();
  if (errors < 0) return;
  rinc_replSay(r, errors ? "loaded, with errors: " : "loaded "); rinc_replSay(r, path); rinc_replSay(r, "\r\n");
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
    rinc_replSay(r, rinc_PAINT_PLAIN "\r\n");
    if (rinc_fs->close() || r->saving < 0) rinc_replSay(r, "couldn't save it all\r\n");
    else if (c == 4) { char b[48]; rinc_replSay(r, "saved "); r->put(b, snprintf(b, sizeof b, "%d bytes\r\n", r->saved)); }
    else rinc_replSay(r, "gave up (the file has what came before)\r\n");
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
  if (!rinc_replAppPath(path, sizeof path, name, n)) { rinc_replSay(r, "names are letters, digits, _ and -\r\n"); return; }
  if (!rinc_fs || rinc_fs->open(path, 1)) { rinc_replSay(r, rinc_fs ? "can't write " : "no storage: "); rinc_replSay(r, path); rinc_replSay(r, "\r\n"); return; }
  rinc_replSay(r, "saving "); rinc_replSay(r, path); rinc_replSay(r, ": type or paste it, then Ctrl-D (Ctrl-C gives up)\r\n");
  r->saving = 1; r->saved = 0; r->nout = 0; r->cr = 0;
}
/* a command line? (only as a whole input of its own) */
static int rinc_command(rinc_Repl *r) {
  const char *s = r->acc; int n = r->n, w;
  if (r->line0 || r->loading) return 0;
  while (n && s[n - 1] == ' ') n--;
  while (n && *s == ' ') { s++; n--; }
  for (w = 0; w < n && s[w] != ' '; w++) {}
  if (w == 4 && n == 4 && !memcmp(s, "home", 4)) { memcpy(rinc_launchName, "home", 5); rinc_launch = 1; return 1; } /* (the platform goes back to it) */
  if (w == 2 && n == 2 && !memcmp(s, "ls", 2)) {
    if (!rinc_fs) rinc_replSay(r, "no storage\r\n");
    else if (rinc_findApps() < 0) rinc_replSay(r, "no apps (or no card): /rinc/apps\r\n");
    else { int i; for (i = 0; i < rinc_appCount; i++) { rinc_replSay(r, rinc_appNames[i]); rinc_replSay(r, "  "); } rinc_replSay(r, rinc_appCount ? "\r\n" : "(none yet)\r\n"); }
    return 1;
  }
  if (n > w + 1 && ((w == 4 && !memcmp(s, "load", 4)) || (w == 3 && !memcmp(s, "run", 3)))) { rinc_load(r, s + w + 1, n - w - 1, w == 3); return 1; }
  if (n > w + 1 && w == 4 && !memcmp(s, "save", 4)) { rinc_save(r, s + w + 1, n - w - 1); return 1; }
  if (n > w + 1 && w == 6 && !memcmp(s, "delete", 6)) {
    char path[96]; const char *name = s + w + 1; int k = n - w - 1;
    if (!rinc_replAppPath(path, sizeof path, name, k)) rinc_replSay(r, "names are letters, digits, _ and -\r\n");
    else if (!rinc_fs || rinc_fs->remove(path)) { rinc_replSay(r, "can't delete "); rinc_replSay(r, path); rinc_replSay(r, "\r\n"); }
    else { path[strlen(path) - (size_t)k - 5] = 0; rinc_fs->remove(path); rinc_replSay(r, "deleted "); r->put(name, k); rinc_replSay(r, "\r\n"); } /* (then its folder, if empty) */
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

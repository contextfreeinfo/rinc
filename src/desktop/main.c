/* Rinc on the desktop:
     rinc                       a Rio REPL in the terminal
     rinc app.rio               run an app in a window: its top-level code and main, then update* and
                               draw* 60 times a second if it's a game (arrows, V C X Z: btn; Shift
                               tapped then held, with Esc: home; closing the window quits)
     -frames n                 instead of a window, run n frames (A held for the first few)
     -o screen.ppm             instead of a window, afterwards write the screen as an image
     -wav sound.wav            with -frames: the sound those frames made, as a WAV file
     -i app.img                compile to an image (see rio_loadImage) instead, and list the host
                               procs its code calls
     -export game[.exe]        a copy of this rinc with the app in it, compiled: it runs the app
                               (with the options above, too), so it's a game to hand out
     -runner rinc-linux-x86_64  with -export: copy that rinc instead (another system's, say, from a
                               release), for a game for that system
     -web game.html            one page that plays the app in a browser: the web player (built in
                               when RINC_WEB_WASM is set; with the compiler, from RINC_WEB_WASM_FULL,
                               if an app runs Rio itself) with the app, compiled, all inline
     -m                        how near the app comes to its limits, compiling and running (as on
                               every Rinc), instead of running it
     -app console              a built-in app instead of a file: home or console (pc.launch
                               switches to these too)
     -type text                type this, as fast as the app takes it (\n Enter, \e Esc, \t Tab)
   The app space is 256 KB, as on the Fruit Jam and the web, so an app that fits one fits all three.
   The window is SDL3, linked in statically (rinc_SDL, on by default). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rinc.h"
#include "internal.h" /* (rio's instruction set, for -i's list of host procs) */
#ifdef _WIN32
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *module, char *name, unsigned long size);
#else
#include <sys/stat.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#if rinc_SDL
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

static uint8_t rinc_appSpace[256 << 10], rinc_replScratch[1 << 18], rinc_image[512 << 10];
static int16_t rinc_wav[rinc_RATE * 2 * 60]; /* -wav: up to a minute */
static char rinc_src[1 << 20];
static rio_Vm rinc_vm;
static rinc_Repl rinc_repl;
static clock_t rinc_t0;

static void rinc_put(const char *s, int n) { fwrite(s, 1, (size_t)n, stdout); }
static float rinc_now(void) { return (float)(clock() - rinc_t0) / CLOCKS_PER_SEC; }

static int rinc_savePpm(const char *path) { /* the screen through the palette, as 8-bit RGB */
  FILE *f = fopen(path, "wb"); int x, y;
  if (!f) return -1;
  fprintf(f, "P6 %d %d 255\n", rinc_W, rinc_H);
  for (y = 0; y < rinc_H; y++)
    for (x = 0; x < rinc_W; x++) {
      uint16_t c = rinc_palette[rinc_screen[y][x]];
      unsigned char rgb[3];
      rgb[0] = (unsigned char)((c >> 11) * 255 / 31); rgb[1] = (unsigned char)((c >> 5 & 63) * 255 / 63); rgb[2] = (unsigned char)((c & 31) * 255 / 31);
      fwrite(rgb, 1, 3, f);
    }
  return fclose(f);
}

/* the app in rinc_src compiled as every Rinc compiles one (rinc_compile): 0, or -1 after saying why
   (and, if it ran out of something, how near each of its limits it came) */
static rio_Usage rinc_usage;
static int rinc_compileApp(const char *app, size_t n) {
  static char text[1024];
  int res;
  rio_trackUsage(&rinc_vm, &rinc_usage);
  if (!(res = rinc_compile(&rinc_vm, rinc_appSpace, sizeof rinc_appSpace, rinc_src, (uint32_t)n))) return 0;
  fflush(stdout);
  if (res > 0) fprintf(stderr, "%s: too big (64 KB is the most)\n", app);
  else {
    fprintf(stderr, "%s:%s\n", app, rio_error(&rinc_vm));
    if (rinc_atLimit(rio_error(&rinc_vm))) { rio_usageText(&rinc_usage, text, sizeof text); fputs(text, stderr); }
  }
  return -1;
}

/* the app compiled, as an image in image[]: its size, or 0 after reporting why not. list: print the
   host procs its code calls, one per line */
static int rinc_nested; /* whether an app made runs Rio itself (pc.rio*): its page needs the compiler */
static uint32_t rinc_makeImage(const char *app, size_t n, int list) {
  uint32_t size, pc; uint8_t used[rio_MAX_FFI] = {0}; int k;
  if (rinc_compileApp(app, n)) return 0;
  for (pc = 0; pc < rinc_vm.codeLen; pc++) if (rinc_vm.code[pc].op == rio_Op_ffi && rinc_vm.code[pc].c < rio_MAX_FFI) used[rinc_vm.code[pc].c] = 1;
  for (k = 0; k < rinc_vm.ffiCount; k++) {
    if (!used[k]) continue;
    if (list) printf("host %s\n", rinc_vm.ffi[k].name);
    if (!strncmp(rinc_vm.ffi[k].name, "pc.rio", 6)) rinc_nested = 1;
  }
  if (!(size = rio_saveImage(&rinc_vm, rinc_image, sizeof rinc_image))) fprintf(stderr, "rinc: can't make an image\n");
  return size;
}
static int rinc_writeFile(const char *path, const void *a, size_t an, const void *b, size_t bn, const void *c, size_t cn) {
  FILE *f = fopen(path, "wb"); int bad;
  if (!f) { fprintf(stderr, "rinc: can't write %s\n", path); return 1; }
  bad = fwrite(a, 1, an, f) != an || fwrite(b, 1, bn, f) != bn || fwrite(c, 1, cn, f) != cn;
  if (fclose(f) || bad) { fprintf(stderr, "rinc: can't write %s\n", path); return 1; }
  return 0;
}

/* an app's name from its file: hello for apps/hello.rio */
static void rinc_stem(const char *path, char *out, size_t cap) {
  const char *base = path, *p, *dot; size_t n;
  for (p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
  dot = strstr(base, ".rio");
  n = dot ? (size_t)(dot - base) : strlen(base);
  if (n >= cap) n = cap - 1;
  memcpy(out, base, n); out[n] = 0;
}

/* -export: this program's own file, with a bundle after it (docs/spec/apps.md#bundles) and then a
   trailer: the bundle's size (4 bytes, little-endian) and its magic again. At startup rinc looks for one,
   and runs its home app in place of the REPL */
static uint8_t rinc_bundleBuf[2 << 20], rinc_images[2 << 20];
static char *rinc_readAll(const char *path, size_t *len) {
  FILE *f; char *b; long n;
  if (!(f = fopen(path, "rb"))) return 0;
  if (fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) || !(b = malloc((size_t)n))) { fclose(f); return 0; }
  if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = 0; }
  fclose(f); *len = (size_t)n;
  return b;
}
static char *rinc_selfFile(size_t *len) {
  char path[4096];
#if defined(_WIN32)
  unsigned long k = GetModuleFileNameA(0, path, sizeof path);
  if (!k || k >= sizeof path) return 0;
#elif defined(__APPLE__)
  uint32_t k = sizeof path;
  if (_NSGetExecutablePath(path, &k)) return 0;
#else
  strcpy(path, "/proc/self/exe");
#endif
  return rinc_readAll(path, len);
}
/* the size of a bundle at the end of file b (n long), or 0 if there's none */
static uint32_t rinc_carried(const char *b, size_t n) {
  uint32_t k;
  if (n < 12 || memcmp(b + n - 8, rinc_BUNDLE_MAGIC, 8)) return 0;
  memcpy(&k, b + n - 12, 4);
  return k <= n - 12 && k <= sizeof rinc_bundleBuf ? k : 0;
}

/* apps (their .rio files), each compiled to an image, as a bundle in rinc_bundleBuf: home names the app
   home means (by default the only one, or one named home), and name the storage folder (by default the
   home app's). Its size, or 0 after saying why not */
static uint32_t rinc_makeBundle(const char *const *apps, int n, const char *home, const char *name) {
  static rinc_App list[rinc_BUNDLE_MOST];
  uint32_t used = 0, size; int i, k;
  if (n > rinc_BUNDLE_MOST / 2) { fprintf(stderr, "rinc: a bundle has %d apps at most\n", rinc_BUNDLE_MOST / 2); return 0; }
  for (i = 0; i < n; i++) {
    rinc_App *a = &list[i]; FILE *f = fopen(apps[i], "rb"); size_t len; const char *line;
    if (!f) { fprintf(stderr, "rinc: can't open %s\n", apps[i]); return 0; }
    len = fread(rinc_src, 1, sizeof rinc_src, f); fclose(f);
    rinc_stem(apps[i], a->name, sizeof a->name);
    for (k = 0; k < i; k++) if (!strcmp(list[k].name, a->name)) { fprintf(stderr, "rinc: two apps named %s\n", a->name); return 0; }
    a->info[0] = 0; /* (its description: the first line, without the #) */
    if (len && rinc_src[0] == '#') {
      for (line = rinc_src + 1; *line == ' '; line++) {}
      for (k = 0; k < (int)sizeof a->info - 1 && line + k < rinc_src + len && line[k] != '\n' && line[k] != '\r'; k++) a->info[k] = line[k];
      a->info[k] = 0;
    }
    rinc_setAppName(a->name);
    if (rinc_start(&rinc_vm, rinc_appSpace, sizeof rinc_appSpace, rinc_put) || !(size = rinc_makeImage(apps[i], len, 0))) return 0;
    if (size > sizeof rinc_images - used) { fprintf(stderr, "rinc: too much to bundle\n"); return 0; }
    memcpy(rinc_images + used, rinc_image, size);
    a->data = (const char *)rinc_images + used; a->len = size; a->image = 1;
    used += size;
  }
  if (!home) { /* (the only app, or one named home) */
    for (i = 0; i < n && strcmp(list[i].name, "home"); i++) {}
    if (n == 1) home = list[0].name;
    else if (i < n) home = "home";
    else { fprintf(stderr, "rinc: which app is home? (-home name)\n"); return 0; }
  }
  for (i = 0; i < n && strcmp(list[i].name, home); i++) {}
  if (i == n) { fprintf(stderr, "rinc: -home %s isn't one of the apps\n", home); return 0; }
  { /* the files: apps/name/name.img, and apps/name/name.info for a description */
    static rinc_File files[rinc_BUNDLE_MOST]; static char paths[rinc_BUNDLE_MOST][96]; int nf = 0;
    for (i = 0; i < n; i++) {
      snprintf(paths[nf], sizeof paths[nf], "apps/%.32s/%.32s.img", list[i].name, list[i].name);
      files[nf].path = paths[nf]; files[nf].data = list[i].data; files[nf++].len = list[i].len;
      if (!list[i].info[0]) continue;
      snprintf(paths[nf], sizeof paths[nf], "apps/%.32s/%.32s.info", list[i].name, list[i].name);
      files[nf].path = paths[nf];
      files[nf].data = list[i].info; files[nf++].len = (uint32_t)strlen(list[i].info);
    }
    if (!(size = rinc_bundleWrite(rinc_bundleBuf, sizeof rinc_bundleBuf, name ? name : home, home, files, nf))) fprintf(stderr, "rinc: too much to bundle\n");
  }
  return size;
}
static int rinc_exportNative(const char *path, const char *runner, uint32_t size) {
  size_t n; char *self = runner ? rinc_readAll(runner, &n) : rinc_selfFile(&n), tail[12]; int r;
  if (!self) { fprintf(stderr, runner ? "rinc: can't read %s\n" : "rinc: can't read my own program file\n", runner); return 1; }
  if (rinc_carried(self, n)) n -= rinc_carried(self, n) + 12; /* (exporting from an export: just this bundle) */
  memcpy(tail, &size, 4); memcpy(tail + 4, rinc_BUNDLE_MAGIC, 8);
  r = rinc_writeFile(path, self, n, rinc_bundleBuf, size, tail, 12);
  free(self);
#ifndef _WIN32
  if (!r) chmod(path, 0755);
#endif
  return r;
}

#if rinc_WEB
extern const unsigned char rinc_webWasm[], rinc_webWasmFull[]; /* (gzipped) */
extern const unsigned rinc_webWasmSize, rinc_webWasmFullSize;
extern const char rinc_webPage[];
#endif
/* base64 of b (n long) into out, which has room for 4 * ((n + 2) / 3) characters */
static size_t rinc_base64(const uint8_t *b, size_t n, char *out) {
  static const char d[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i, k = 0;
  for (i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < n ? (uint32_t)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
    out[k++] = d[v >> 18]; out[k++] = d[v >> 12 & 63];
    out[k++] = i + 1 < n ? d[v >> 6 & 63] : '='; out[k++] = i + 2 < n ? d[v & 63] : '=';
  }
  return k;
}
/* -web: the page, with the title (the home app's name), the player (gzipped: with the compiler if an app
   runs Rio itself) and the bundle in place of {{TITLE}}, {{WASM}} and {{APP}} */
static int rinc_exportWeb(const char *path, const char *app, uint32_t size) {
#if rinc_WEB
  const char *p = rinc_webPage, *name = app, *q; char *wasm, *img, title[256]; size_t nw, ni, k = 0; FILE *f; int bad = 0;
  const unsigned char *player = rinc_nested ? rinc_webWasmFull : rinc_webWasm;
  unsigned nplayer = rinc_nested ? rinc_webWasmFullSize : rinc_webWasmSize;
  if (!nplayer) { fprintf(stderr, "rinc: an app runs Rio itself, and this rinc was built without the web player that can (RINC_WEB_WASM_FULL)\n"); return 1; }
  for (q = app; *q; q++) if (*q == '/' || *q == '\\') name = q + 1;
  while (name[k] && name[k] != '.' && k < sizeof title - 1) { title[k] = strchr("<>&\"", name[k]) ? '_' : name[k]; k++; }
  title[k] = 0;
  wasm = malloc(4 * (nplayer / 3 + 1)); img = malloc(4 * (size / 3 + 1));
  if (!wasm || !img || !(f = fopen(path, "wb"))) { free(wasm); free(img); fprintf(stderr, "rinc: can't write %s\n", path); return 1; }
  nw = rinc_base64(player, nplayer, wasm); ni = rinc_base64(rinc_bundleBuf, size, img);
  while (*p) {
    if (!strncmp(p, "{{TITLE}}", 9)) { fputs(title, f); p += 9; }
    else if (!strncmp(p, "{{WASM}}", 8)) { fwrite(wasm, 1, nw, f); p += 8; }
    else if (!strncmp(p, "{{APP}}", 7)) { fwrite(img, 1, ni, f); p += 7; }
    else fputc(*p++, f);
  }
  bad = ferror(f); if (fclose(f)) bad = 1;
  free(wasm); free(img);
  if (bad) { fprintf(stderr, "rinc: can't write %s\n", path); return 1; }
  return 0;
#else
  (void)path; (void)app; (void)size;
  fprintf(stderr, "rinc: this rinc was built without the web player (RINC_WEB_WASM)\n");
  return 1;
#endif
}

/* the sound -frames made, as a 16-bit stereo WAV file */
static int rinc_saveWav(const char *path, uint32_t frames) {
  FILE *f = fopen(path, "wb"); uint32_t bytes = frames * 4, v;
  unsigned char h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                         0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a'};
  if (!f) return -1;
  v = 36 + bytes; memcpy(h + 4, &v, 4); v = rinc_RATE; memcpy(h + 24, &v, 4); v = rinc_RATE * 4; memcpy(h + 28, &v, 4);
  fwrite(h, 1, 40, f); fwrite(&bytes, 4, 1, f); fwrite(rinc_wav, 4, frames, f);
  return fclose(f);
}

/* -type: what to type, as fast as the app takes it (\n Enter, \e Esc, \t Tab, \\ a backslash) */
static const char *rinc_typing;
static void rinc_typeMore(void) {
  while (rinc_typing && *rinc_typing && rinc_textRoom() > 0) {
    int c = (unsigned char)*rinc_typing++;
    if (c == '\\' && *rinc_typing) {
      c = (unsigned char)*rinc_typing++;
      if (c == 'e') { rinc_textEsc(); continue; }
      c = c == 'n' ? 13 : c == 't' ? 9 : c;
    }
    rinc_textPush(c);
  }
}

/* storage (files.c) */
const char *rinc_storageStart(const char *name);
const char *rinc_storageAt(const char *dir);
void rinc_mkdirsFor(const char *file);

/* an app by name: from storage, or else the bundle; compiled (or, an image, loaded) and started. 0,
   with *game saying whether it has frames; -1: not (said why) */
static int rinc_openApp(const char *name, int *game) {
  const rinc_App *b = 0; int n, fn, bad;
  name = rinc_appFor(name); /* (home: storage's, else the bundle's) */
  n = rinc_readApp(name, rinc_src, sizeof rinc_src);
  if (n == -2) { fprintf(stderr, "%s.rio: too big\n", name); return -1; }
  if (n < 0 && !(b = rinc_bundled(name))) { fprintf(stderr, "rinc: no app named %s (rinc -install %s, if it's a standard one)\n", name, name); return -1; }
  rinc_audioReset();
  rinc_setAppName(name);
  if (rinc_start(&rinc_vm, rinc_appSpace, sizeof rinc_appSpace, rinc_put)) return -1;
  if (b && b->image) bad = rio_loadImage(&rinc_vm, b->data, b->len);
  else bad = rinc_compile(&rinc_vm, rinc_appSpace, sizeof rinc_appSpace, b ? b->data : rinc_src, b ? (uint32_t)(b->len ? b->len : strlen(b->data)) : (uint32_t)n);
  if (bad || rio_run(&rinc_vm) || ((fn = rio_findProc(&rinc_vm, "main")) >= 0 && rio_call(&rinc_vm, fn))) {
    fflush(stdout); fprintf(stderr, "%s.rio:%s\n", name, rio_error(&rinc_vm)); return -1;
  }
  *game = rio_findProc(&rinc_vm, "update") >= 0 || rio_findProc(&rinc_vm, "draw") >= 0;
  return 0;
}
/* pc.launch, or the system key: to rinc_launchName */
static int rinc_switch(int *game) { rinc_launch = 0; return rinc_openApp(rinc_launchName, game); }

/* -install: standard apps (home, console, the demos), by name, fetched with curl from the Rinc
   repository's main branch; or app files (name.rio) copied. Into storage. 0, or 1 (said why) */
static int rinc_install(const char *what, const char *root) {
  char name[40], tmp[1100], cmd[1400]; const char *base = strrchr(what, '/'), *dot; size_t k, n; FILE *f; int got = 0, i;
  base = base ? base + 1 : what;
  if (strrchr(base, '\\')) base = strrchr(base, '\\') + 1;
  dot = strstr(base, ".rio");
  n = dot ? (size_t)(dot - base) : strlen(base);
  for (k = 0; k < n && k < sizeof name - 1; k++) {
    char c = base[k];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) { fprintf(stderr, "rinc: %s: names are letters, digits, _ and -\n", what); return 1; }
    name[k] = c;
  }
  name[k] = 0;
  if (dot) { /* a file of one's own */
    if (!(f = fopen(what, "rb"))) { fprintf(stderr, "rinc: can't open %s\n", what); return 1; }
    got = (int)fread(rinc_src, 1, sizeof rinc_src, f); fclose(f);
  } else { /* a standard one: system/name.rio, else apps/name.rio, from the repository, through curl */
    static const char *const dirs[] = {"system", "apps"};
    snprintf(tmp, sizeof tmp, "%s/download.tmp", root);
    for (i = 0; i < 2 && got <= 0; i++) {
      snprintf(cmd, sizeof cmd, "curl -fsL -o \"%s\" https://raw.githubusercontent.com/rincpc/rinc/main/%s/%s.rio", tmp, dirs[i], name);
      rinc_mkdirsFor(tmp);
      if (system(cmd) == 0 && (f = fopen(tmp, "rb"))) { got = (int)fread(rinc_src, 1, sizeof rinc_src, f); fclose(f); }
      remove(tmp);
    }
    if (got <= 0) { fprintf(stderr, "rinc: couldn't get %s (installing a standard app takes curl, and the internet)\n", name); return 1; }
  }
  snprintf(cmd, sizeof cmd, "/rinc/apps/%s/%s.rio", name, name);
  if (rinc_fs->open(cmd, 1) || rinc_fs->write(rinc_src, got) != got || rinc_fs->close()) { fprintf(stderr, "rinc: can't write %s\n", name); return 1; }
  printf("installed %s, in %s/apps/%s\n", name, root, name);
  return 0;
}

#if rinc_SDL
static SDL_AudioStream *rinc_audio;
static void rinc_lockAudio(int on) { if (on) SDL_LockAudioStream(rinc_audio); else SDL_UnlockAudioStream(rinc_audio); }
/* SDL wants more sound: mix it (SDL holds the stream's lock meanwhile, as host procs do) */
static void rinc_audioMore(void *ud, SDL_AudioStream *s, int more, int total) {
  static int16_t buf[1024 * 2];
  (void)ud; (void)total;
  while (more > 0) {
    int frames = more / 4 > 1024 ? 1024 : (more + 3) / 4;
    rinc_audioMix(buf, frames);
    SDL_PutAudioStreamData(s, buf, frames * 4);
    more -= frames * 4;
  }
}
static float rinc_sdlNow(void) { return (float)((double)SDL_GetTicksNS() / 1e9); }

/* a controller (SDL's gamepads: Xbox and the rest), as player 1: its buttons as Rinc's, its sticks and
   triggers into rinc_axes */
static SDL_Gamepad *rinc_pad;
static uint16_t rinc_readPad(void) {
  static const struct { int sdl; uint16_t rinc; } map[] = {
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, rinc_Button_left}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, rinc_Button_right}, {SDL_GAMEPAD_BUTTON_DPAD_UP, rinc_Button_up},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, rinc_Button_down}, {SDL_GAMEPAD_BUTTON_SOUTH, rinc_Button_a}, {SDL_GAMEPAD_BUTTON_EAST, rinc_Button_b},
    {SDL_GAMEPAD_BUTTON_WEST, rinc_Button_x}, {SDL_GAMEPAD_BUTTON_NORTH, rinc_Button_y}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, rinc_Button_lb},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, rinc_Button_rb}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, rinc_Button_ls}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, rinc_Button_rs},
    {SDL_GAMEPAD_BUTTON_START, rinc_Button_start}, {SDL_GAMEPAD_BUTTON_BACK, rinc_Button_back}};
  static const int axes[6] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};
  uint16_t b = 0; size_t i;
  if (!rinc_pad) { memset(rinc_axes, 0, sizeof rinc_axes); return 0; }
  for (i = 0; i < sizeof map / sizeof map[0]; i++) if (SDL_GetGamepadButton(rinc_pad, (SDL_GamepadButton)map[i].sdl)) b |= map[i].rinc;
  for (i = 0; i < 6; i++) { int v = SDL_GetGamepadAxis(rinc_pad, (SDL_GamepadAxis)axes[i]); rinc_axes[i] = (int16_t)(v < -32767 ? -32767 : v); }
  return b;
}

/* editing keys and Ctrl+letter, for key() (typing arrives as text) */
static void rinc_sdlKey(const SDL_KeyboardEvent *k) {
  SDL_Keycode c = k->key;
  int code = c == SDLK_LEFT ? rinc_Key_left : c == SDLK_RIGHT ? rinc_Key_right : c == SDLK_UP ? rinc_Key_up : c == SDLK_DOWN ? rinc_Key_down :
             c == SDLK_HOME ? rinc_Key_home : c == SDLK_END ? rinc_Key_end : c == SDLK_DELETE ? rinc_Key_delete :
             c == SDLK_PAGEUP ? rinc_Key_pageUp : c == SDLK_PAGEDOWN ? rinc_Key_pageDown : c == SDLK_TAB ? 9 : 0;
  if (!code && (k->mod & SDL_KMOD_CTRL) && c >= SDLK_A && c <= SDLK_Z) code = (int)(c - SDLK_A) + 1;
  if (code) rinc_textKey(code);
}

/* the screen in a window, scaled up by whole steps; a game gets a frame every 1/60 s */
static int rinc_window(const char *title) {
  SDL_Window *w; SDL_Renderer *r; SDL_Texture *tex; uint64_t next; int game = 1, running = 1;
  SDL_AudioSpec spec;
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "rinc: %s\n", SDL_GetError()); return 1; }
  spec.format = SDL_AUDIO_S16; spec.channels = 2; spec.freq = rinc_RATE;
  if ((rinc_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, rinc_audioMore, 0))) { /* (no sound device: play on silently) */
    rinc_audioLock = rinc_lockAudio;
    SDL_ResumeAudioStreamDevice(rinc_audio);
  }
  if (!SDL_CreateWindowAndRenderer(title, rinc_W * 2, rinc_H * 2, SDL_WINDOW_RESIZABLE, &w, &r)) { fprintf(stderr, "rinc: %s\n", SDL_GetError()); return 1; }
  SDL_SetRenderLogicalPresentation(r, rinc_W, rinc_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
  SDL_SetRenderVSync(r, 1);
  SDL_StartTextInput(w);
  tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, rinc_W, rinc_H);
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  rinc_time = rinc_sdlNow;
  next = SDL_GetTicksNS();
  while (running) {
    SDL_Event e; const bool *k; uint16_t *px; int pitch, x, y;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) running = 0;
      if (rinc_systemHeld() && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_TEXT_INPUT)) continue; /* (a system combination: not the app's) */
      if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) rinc_textEsc(); /* (Esc is the app's: closing the window quits) */
      if (e.type == SDL_EVENT_TEXT_INPUT) { const char *t = e.text.text; while (*t) rinc_textPush((unsigned char)*t++); } /* (key(): typing) */
      if (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)) rinc_textPush(13);
      if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_BACKSPACE) rinc_textPush(8);
      if (e.type == SDL_EVENT_KEY_DOWN) rinc_sdlKey(&e.key);
      if (e.type == SDL_EVENT_GAMEPAD_ADDED && !rinc_pad) rinc_pad = SDL_OpenGamepad(e.gdevice.which);
      if (e.type == SDL_EVENT_GAMEPAD_REMOVED && rinc_pad && SDL_GetGamepadID(rinc_pad) == e.gdevice.which) { SDL_CloseGamepad(rinc_pad); rinc_pad = 0; }
    }
    k = SDL_GetKeyboardState(0);
    rinc_buttons = rinc_readPad() | (uint16_t)(k[SDL_SCANCODE_LEFT] * rinc_Button_left | k[SDL_SCANCODE_RIGHT] * rinc_Button_right | k[SDL_SCANCODE_UP] * rinc_Button_up |
      k[SDL_SCANCODE_DOWN] * rinc_Button_down | k[SDL_SCANCODE_V] * rinc_Button_a | k[SDL_SCANCODE_C] * rinc_Button_b | k[SDL_SCANCODE_X] * rinc_Button_x | k[SDL_SCANCODE_Z] * rinc_Button_y);
    { /* the system key: Shift tapped then held (the Windows key is the desktop's own), or a controller's guide button */
      int other = 0, s;
      for (s = 0; s < SDL_SCANCODE_COUNT; s++) other |= k[s] && s != SDL_SCANCODE_LSHIFT && s != SDL_SCANCODE_RSHIFT;
      if (rinc_systemKey(rinc_pad && SDL_GetGamepadButton(rinc_pad, SDL_GAMEPAD_BUTTON_GUIDE), k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT], other,
                         k[SDL_SCANCODE_ESCAPE] || (rinc_pad && SDL_GetGamepadButton(rinc_pad, SDL_GAMEPAD_BUTTON_BACK)))) {
        strcpy(rinc_launchName, "home"); rinc_launch = 1;
      }
    }
    while (game && SDL_GetTicksNS() >= next) { /* 60 frames a second, whatever the display's rate */
      int res;
      rinc_typeMore();
      res = rinc_frame(&rinc_vm);
      if (res) { game = 0; if (res < 0) { fprintf(stderr, "%s\n", rio_error(&rinc_vm)); running = 0; } }
      if (rinc_launch) break;
      next += 1000000000ull / 60;
      if (SDL_GetTicksNS() > next + 100000000ull) next = SDL_GetTicksNS(); /* (fell far behind: skip ahead) */
    }
    if (rinc_launch) { /* pc.launch, or the system key (a failed switch leaves what was running) */
      int was = game;
      if (rinc_switch(&game)) game = was;
      else running = 1;
      next = SDL_GetTicksNS();
    }
    if (SDL_LockTexture(tex, 0, (void **)&px, &pitch)) {
      for (y = 0; y < rinc_H; y++)
        for (x = 0; x < rinc_W; x++) px[y * (pitch / 2) + x] = rinc_palette[rinc_screen[y][x]];
      SDL_UnlockTexture(tex);
    }
    SDL_RenderClear(r);
    SDL_RenderTexture(r, tex, 0, 0);
    SDL_RenderPresent(r);
    if (!game) SDL_Delay(16);
  }
  SDL_Quit();
  return 0;
}
#endif

int main(int argc, char **argv) {
  const char *apps[rinc_BUNDLE_MOST], *out = 0, *img = 0, *wavout = 0, *exp = 0, *web = 0, *runner = 0, *named = 0, *data = 0, *root = 0, *home = 0, *storage = 0, *app = 0;
  const char *installs[32]; int napps = 0, ninstall = 0, i, fn, frames = 0, game = 0, memReport = 0, opened = 0; uint32_t size = 0;
  char stem[40], *self = 0;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "-frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-i") && i + 1 < argc) img = argv[++i];
    else if (!strcmp(argv[i], "-wav") && i + 1 < argc) wavout = argv[++i];
    else if (!strcmp(argv[i], "-export") && i + 1 < argc) exp = argv[++i];
    else if (!strcmp(argv[i], "-web") && i + 1 < argc) web = argv[++i];
    else if (!strcmp(argv[i], "-runner") && i + 1 < argc) runner = argv[++i];
    else if (!strcmp(argv[i], "-home") && i + 1 < argc) home = argv[++i];
    else if (!strcmp(argv[i], "-name") && i + 1 < argc) storage = argv[++i];
    else if (!strcmp(argv[i], "-app") && i + 1 < argc) named = argv[++i];
    else if (!strcmp(argv[i], "-data") && i + 1 < argc) data = argv[++i];
    else if (!strcmp(argv[i], "-install")) while (i + 1 < argc && argv[i + 1][0] != '-' && ninstall < 32) installs[ninstall++] = argv[++i];
    else if (!strcmp(argv[i], "-m")) memReport = 1;
    else if (!strcmp(argv[i], "-type") && i + 1 < argc) rinc_typing = argv[++i];
    else if (argv[i][0] == '-' || napps == rinc_BUNDLE_MOST) {
      fprintf(stderr, "usage: rinc [app.rio | -app name] [-m] [-frames n] [-o screen.ppm] [-wav sound.wav] [-type text] [-i app.img]\n"
                      "       rinc app.rio... -export program [-runner rinc] [-home name] [-name folder]\n"
                      "       rinc app.rio... -web page.html [-home name]\n"
                      "       rinc -install name|app.rio...      (and -data folder with any of these: storage there)\n");
      return 2;
    }
    else apps[napps++] = argv[i];
  }
  rinc_t0 = clock(); rinc_time = rinc_now;
  if (exp || web) { /* a bundle of the apps, as a program or a page */
    if (!napps) { fprintf(stderr, "rinc: -export and -web take the apps to bundle: rinc a.rio b.rio -export game\n"); return 2; }
    rinc_storageStart("rinc");
    if (!(size = rinc_makeBundle(apps, napps, home, storage))) return 1;
    if (exp && rinc_exportNative(exp, runner, size)) return 1;
    if (web) { rinc_bundleRead(rinc_bundleBuf, size); return rinc_exportWeb(web, rinc_appFor("home"), size); }
    return 0;
  }
  if (napps > 1) { fprintf(stderr, "rinc: several apps are for a bundle: -export or -web\n"); return 2; }
  app = napps ? apps[0] : 0;
  if (!app && !named) { /* an exported program: the bundle it carries */
    size_t n;
    if ((self = rinc_selfFile(&n)) && (size = rinc_carried(self, n)) && !rinc_bundleRead(self + n - 12 - size, size)) named = "home";
  }
  if (data) root = rinc_storageAt(data); /* (apps, and their save spaces) */
  else root = rinc_storageStart(named && size ? (*rinc_bundleName() ? rinc_bundleName() : "game") : "rinc");
  if (ninstall) {
    int bad = 0;
    for (i = 0; i < ninstall; i++) bad |= rinc_install(installs[i], root);
    return bad;
  }
  if (rinc_start(&rinc_vm, rinc_appSpace, sizeof rinc_appSpace, rinc_put)) { fprintf(stderr, "rinc: can't start\n"); return 1; }
  if (!app && !named && (rinc_readApp("home", 0, 0) != -1 || rinc_bundled(rinc_appFor("home")))) named = "home"; /* (plain rinc: home, if it's installed) */
  if (named) {
    if (app || rinc_openApp(named, &game)) return 2;
    app = named;
    opened = 1;
  }
  if (app) {
    if (!opened) {
      FILE *f = fopen(app, "rb"); size_t n;
      if (!f) { fprintf(stderr, "rinc: can't open %s\n", app); return 2; }
      n = fread(rinc_src, 1, sizeof rinc_src, f); fclose(f);
      rinc_stem(app, stem, sizeof stem);
      rinc_setAppName(stem);
      if (img) {
        if (!(size = rinc_makeImage(app, n, 1))) return 1;
        return rinc_writeFile(img, rinc_image, size, 0, 0, 0, 0);
      }
      if (rinc_compileApp(app, n)) return 1;
      if (memReport) { /* -m: how near its limits it came, compiling and running, as on every Rinc */
        static char text[1024];
        rio_usageText(&rinc_usage, text, sizeof text);
        printf("%s: fits in the 256 KB app space\n%s", app, text);
        return 0;
      }
      if (rio_run(&rinc_vm) || ((fn = rio_findProc(&rinc_vm, "main")) >= 0 && rio_call(&rinc_vm, fn))) {
        fflush(stdout); fprintf(stderr, "%s:%s\n", app, rio_error(&rinc_vm)); return 1;
      }
    }
#if rinc_SDL
    if (!out && !frames) return rinc_window(app);
#endif
    for (i = 0; i < frames; i++) { /* a game, without a window: A held for the first frames (to start it) */
      rinc_buttons = i < 3 ? rinc_Button_a : 0;
      rinc_typeMore();
      if (rinc_frame(&rinc_vm) < 0) { fflush(stdout); fprintf(stderr, "%s:%s\n", app, rio_error(&rinc_vm)); return 1; }
      if (rinc_launch && rinc_switch(&game)) return 1;
      if (wavout && i < 60 * 60) rinc_audioMix(rinc_wav + i * (rinc_RATE / 60) * 2, rinc_RATE / 60); /* (a frame's worth of sound) */
    }
    if (wavout && rinc_saveWav(wavout, (uint32_t)(frames < 3600 ? frames : 3600) * (rinc_RATE / 60))) { fprintf(stderr, "rinc: can't write %s\n", wavout); return 1; }
  } else {
    int c;
    fprintf(stderr, "(no home app here yet: rinc -install home console, with curl; or rinc app.rio)\n");
    if (rinc_replBegin(&rinc_repl, &rinc_vm, rinc_replScratch, sizeof rinc_replScratch)) { fprintf(stderr, "rinc: %s\n", rio_error(&rinc_vm)); return 1; }
    rinc_repl.echo = 0;
    while ((c = getchar()) != EOF) rinc_replChar(&rinc_repl, c);
    rinc_put("\n", 1);
  }
  return out && rinc_savePpm(out) ? 1 : 0;
}

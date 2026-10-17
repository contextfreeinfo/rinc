/* Rinc in a browser, as wasm: the page (rinc -web) hands over a bundle of apps
   (docs/spec/apps.md#bundles), then calls frame() 60 times a second and draws the screen. Imports from
   the page: put (text out), now (seconds), and storage (fs_*: the browser's localStorage, a key for each
   file). No emscripten: plain clang/zig wasm32, with only the C library rio needs. Built with
   rio_NO_COMPILER, it runs compiled apps (images) only, and the wasm is a quarter the size; with the
   compiler, sources too, and Rio inside an app (the console). */
#include <stdio.h>
#include <string.h>
#include "rinc.h"

/* exported to the page under name (the C function is rinc_web<Name>) */
#define rinc_EXPORT(name) __attribute__((export_name(#name)))
#define rinc_IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
rinc_IMPORT(put) extern void js_put(const char *s, int n);
rinc_IMPORT(now) extern float js_now(void);
/* storage: a file is a key in localStorage, its path (as rinc_Fs gives it) under the storage name */
rinc_IMPORT(fs_open) extern int js_fsOpen(const char *key, int n, int write);
rinc_IMPORT(fs_read) extern int js_fsRead(char *buf, int n);
rinc_IMPORT(fs_write) extern int js_fsWrite(const char *buf, int n);
rinc_IMPORT(fs_close) extern int js_fsClose(void);
rinc_IMPORT(fs_list) extern int js_fsList(const char *key, int n, char *out, int cap); /* "d name\n" or "f name\n" each: how long, or -1 */
rinc_IMPORT(fs_remove) extern int js_fsRemove(const char *key, int n);

static _Alignas(16) uint8_t rinc_appSpace[256 << 10]; /* the same app space as on the board */
static char rinc_src[512 << 10]; /* the bundle */
static rio_Vm rinc_vm;
static uint32_t rinc_rgba[rinc_W * rinc_H];
static int16_t rinc_sound[2048 * 2];

static void rinc_put(const char *s, int n) { js_put(s, n); }

/* ------------------------------------------------------------------ storage */

static char rinc_store[33] = "rinc"; /* the storage name: the bundle's, else rinc */
static int rinc_key(const char *path, char *key, int cap) { /* /rinc/apps/x -> rinc/apps/x */
  if (strncmp(path, "/rinc/", 6) || strstr(path, "..")) return -1;
  return snprintf(key, (size_t)cap, "%s/%s", rinc_store, path + 6);
}
static int rinc_open(const char *path, int write) { char k[160]; int n = rinc_key(path, k, sizeof k); return n < 0 ? -1 : js_fsOpen(k, n, write); }
static int rinc_read(char *buf, int n) { return js_fsRead(buf, n); }
static int rinc_write(const char *buf, int n) { return js_fsWrite(buf, n); }
static int rinc_close(void) { return js_fsClose(); }
static int rinc_list(const char *dir, void (*each)(const char *name, int dir, void *ud), void *ud) {
  static char out[4096]; char k[160], *p, *e; int n = rinc_key(dir, k, sizeof k), got;
  if (n < 0 || (got = js_fsList(k, n, out, (int)sizeof out - 1)) < 0) return -1;
  out[got] = 0;
  for (p = out; *p; p = e + 1) {
    if (!(e = strchr(p, '\n'))) break;
    *e = 0;
    if (p[0] && p[1] == ' ') each(p + 2, p[0] == 'd', ud);
  }
  return 0;
}
static int rinc_remove(const char *path) { char k[160]; int n = rinc_key(path, k, sizeof k); return n < 0 ? -1 : js_fsRemove(k, n); }
static const rinc_Fs rinc_webFs = {rinc_open, rinc_read, rinc_write, rinc_close, rinc_list, rinc_remove};

/* ------------------------------------------------------------------ apps */

rinc_EXPORT(source) char *rinc_webSource(void) { return rinc_src; }
rinc_EXPORT(sourceCap) int rinc_webSourceCap(void) { return (int)sizeof rinc_src; }
/* an app, by name, started (rinc_appBegin, rinc_appRun): from storage (with the compiler), else the
   bundle: an image (or, with the compiler, a source too). Whatever goes wrong is on the screen */
static void rinc_webOpen(const char *name) {
  const rinc_App *b = 0; int n = -1; char *top = (char *)rinc_appSpace + sizeof rinc_appSpace - rinc_MAX_SOURCE;
  name = rinc_appFor(name);
  if (rinc_appBegin(&rinc_vm, rinc_appSpace, sizeof rinc_appSpace, name, rinc_put)) return;
#ifndef rio_NO_COMPILER
  n = rinc_readApp(name, top, rinc_MAX_SOURCE); /* (at the top, where it compiles) */
#endif
  if (n == -2) { rinc_appFail(name, ".rio: too big (64 KB is the most)"); return; }
  if (n < 0 && !(b = rinc_bundled(name))) { rinc_appFail(name, ": no app of this name"); return; }
  if (n >= 0) rinc_appRun(&rinc_vm, rinc_appSpace, sizeof rinc_appSpace, name, top, (uint32_t)n, 0);
  else rinc_appRun(&rinc_vm, rinc_appSpace, sizeof rinc_appSpace, name, b->data, b->len, b->image ? b->data : 0);
}
/* start the bundle in source() at its home app: 0, or -1 if it's no bundle (error() says why) */
rinc_EXPORT(start) int rinc_webStart(int len) {
  rinc_time = js_now;
  if (rinc_bundleRead(rinc_src, (uint32_t)len)) return -1;
  if (*rinc_bundleName()) snprintf(rinc_store, sizeof rinc_store, "%s", rinc_bundleName());
  rinc_fs = &rinc_webFs;
  rinc_webOpen("home");
  return 0;
}
/* a frame with these buttons held (keys, for the system key: 1 Shift, 2 another key, 4 Esc or a
   controller's Back, 8 its guide button, 16 Tab, 32 Q, 64 P, 128 Enter): 0 a game's frame ran, 1 nothing's running (the page keeps
   calling, for the system key). An app's error goes on the screen; then any switch, to an app or
   (Sys+Esc) home */
rinc_EXPORT(frame) int rinc_webFrame(int buttons, int keys) {
  int sys = (keys & 4 ? rinc_SysKey_esc : 0) | (keys & 16 ? rinc_SysKey_tab : 0) | (keys & 32 ? rinc_SysKey_q : 0) |
            (keys & 64 ? rinc_SysKey_p : 0) | (keys & 128 ? rinc_SysKey_enter : 0) |
            (buttons & rinc_Button_rb ? rinc_SysKey_next : 0) | (buttons & rinc_Button_lb ? rinc_SysKey_prev : 0); /* (a controller's RB, LB) */
  if (rinc_systemKey(keys & 8, keys & 1, keys & 2, sys)) rinc_launch = 1; /* (to rinc_launchName) */
  else {
    rinc_buttons = rinc_systemHeld() ? 0 : (uint16_t)buttons; /* (a system combination: not the app's) */
    rinc_gameFrame(&rinc_vm);
  }
  if (rinc_launch) { rinc_launch = 0; rinc_webOpen(rinc_launchName); }
  return !rinc_game;
}
rinc_EXPORT(error) const char *rinc_webError(void) { return rio_error(&rinc_vm); }
/* a character typed, and an editing key (rinc_Key_*, Ctrl+letter), for key(): not while the system key
   is held */
rinc_EXPORT(typed) void rinc_webTyped(int c) { if (!rinc_systemHeld()) rinc_textPush(c); }
rinc_EXPORT(key) void rinc_webKey(int k) { if (!rinc_systemHeld()) { if (k == 27) rinc_textEsc(); else rinc_textKey(k); } }
/* a controller's sticks and triggers, before a frame (as in rinc_axes) */
rinc_EXPORT(axes) void rinc_webAxes(int lx, int ly, int rx, int ry, int lt, int rt) {
  rinc_axes[0] = (int16_t)lx; rinc_axes[1] = (int16_t)ly; rinc_axes[2] = (int16_t)rx; rinc_axes[3] = (int16_t)ry; rinc_axes[4] = (int16_t)lt; rinc_axes[5] = (int16_t)rt;
}
/* the next frames of sound (at most 2048), as 16-bit stereo */
rinc_EXPORT(mix) int16_t *rinc_webMix(int frames) { rinc_audioMix(rinc_sound, frames < 2048 ? frames : 2048); return rinc_sound; }
/* the screen as RGBA bytes for a canvas (with the system overlay, while it's up) */
rinc_EXPORT(pixels) uint32_t *rinc_webPixels(void) {
  int i;
  for (i = 0; i < rinc_W * rinc_H; i++) {
    uint16_t c = rinc_palette[rinc_shownRow(i / rinc_W)[i % rinc_W]];
    uint32_t r = (uint32_t)(c >> 11) * 255 / 31, g = (uint32_t)(c >> 5 & 63) * 255 / 63, b = (uint32_t)(c & 31) * 255 / 31;
    rinc_rgba[i] = 0xFF000000u | b << 16 | g << 8 | r;
  }
  return rinc_rgba;
}

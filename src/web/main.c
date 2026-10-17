/* Rinc in a browser, as wasm: the page (tools/web.py makes it) hands over an app, then calls frame()
   60 times a second and draws the screen. Imports from the page: put (text out), now (seconds).
   No emscripten: plain clang/zig wasm32, with only the C library rio needs. Built with rio_NO_COMPILER,
   the app is an image (made by rinc -i), and the wasm is a quarter the size; otherwise it's source. */
#include "rinc.h"

/* exported to the page under name (the C function is rinc_web<Name>) */
#define rinc_EXPORT(name) __attribute__((export_name(#name)))
__attribute__((import_module("env"), import_name("put"))) extern void js_put(const char *s, int n);
__attribute__((import_module("env"), import_name("now"))) extern float js_now(void);

static _Alignas(16) uint8_t rinc_appSpace[256 << 10]; /* the same app space as on the board */
static char rinc_src[128 << 10]; /* the app: its source, or its image */
static rio_Vm rinc_vm;
static uint32_t rinc_rgba[rinc_W * rinc_H];
static int16_t rinc_sound[2048 * 2];

static void rinc_put(const char *s, int n) { js_put(s, n); }

rinc_EXPORT(source) char *rinc_webSource(void) { return rinc_src; }
/* a bundled app, by name, started: an image (or, with the compiler, a source too). 0, or -1 */
static int rinc_webOpen(const char *name) {
  const rinc_App *b = rinc_bundled(rinc_appFor(name)); int fn;
  if (!b || rinc_start(&rinc_vm, rinc_appSpace, sizeof rinc_appSpace, rinc_put)) return -1;
  rinc_setAppName(b->name);
#ifdef rio_NO_COMPILER
  if (!b->image || rio_loadImage(&rinc_vm, b->data, b->len) || rio_run(&rinc_vm)) return -1;
#else
  if ((b->image ? rio_loadImage(&rinc_vm, b->data, b->len) : rinc_compile(&rinc_vm, rinc_appSpace, sizeof rinc_appSpace, b->data, b->len)) || rio_run(&rinc_vm)) return -1;
#endif
  return (fn = rio_findProc(&rinc_vm, "main")) >= 0 ? rio_call(&rinc_vm, fn) : 0;
}
/* start the bundle in source() (docs/spec/apps.md#bundles), at its home app: 0, or -1 (error() says why) */
rinc_EXPORT(start) int rinc_webStart(int len) {
  rinc_time = js_now;
  if (rinc_bundleRead(rinc_src, (uint32_t)len)) return -1;
  return rinc_webOpen("home");
}
/* a frame with these buttons held: 0, 1 (not a game), -1 (error); then any pc.launch */
rinc_EXPORT(frame) int rinc_webFrame(int buttons) {
  int res;
  rinc_buttons = (uint16_t)buttons;
  res = rinc_frame(&rinc_vm);
  if (rinc_launch) { rinc_launch = 0; return rinc_webOpen(rinc_launchName) < 0 ? -1 : 0; }
  return res;
}
rinc_EXPORT(error) const char *rinc_webError(void) { return rio_error(&rinc_vm); }
rinc_EXPORT(typed) void rinc_webTyped(int c) { rinc_textPush(c); } /* a character typed, for key() */
/* a controller's sticks and triggers, before a frame (as in rinc_axes) */
rinc_EXPORT(axes) void rinc_webAxes(int lx, int ly, int rx, int ry, int lt, int rt) {
  rinc_axes[0] = (int16_t)lx; rinc_axes[1] = (int16_t)ly; rinc_axes[2] = (int16_t)rx; rinc_axes[3] = (int16_t)ry; rinc_axes[4] = (int16_t)lt; rinc_axes[5] = (int16_t)rt;
}
/* the next frames of sound (at most 2048), as 16-bit stereo */
rinc_EXPORT(mix) int16_t *rinc_webMix(int frames) { rinc_audioMix(rinc_sound, frames < 2048 ? frames : 2048); return rinc_sound; }
/* the screen as RGBA bytes for a canvas */
rinc_EXPORT(pixels) uint32_t *rinc_webPixels(void) {
  int i;
  for (i = 0; i < rinc_W * rinc_H; i++) {
    uint16_t c = rinc_palette[((const uint8_t *)rinc_screen)[i]];
    uint32_t r = (uint32_t)(c >> 11) * 255 / 31, g = (uint32_t)(c >> 5 & 63) * 255 / 63, b = (uint32_t)(c & 31) * 255 / 31;
    rinc_rgba[i] = 0xFF000000u | b << 16 | g << 8 | r;
  }
  return rinc_rgba;
}

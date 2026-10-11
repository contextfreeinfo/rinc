/* Rim in a browser, as wasm: the page (tools/web.py makes it) hands over a cart's source, then calls
   frame() 60 times a second and draws the screen. Imports from the page: put (text out), now (seconds).
   No emscripten: plain clang/zig wasm32, with only the C library rio needs. */
#include "rim.h"

#define EXPORT(name) __attribute__((export_name(#name)))
__attribute__((import_module("env"), import_name("put"))) extern void js_put(const char *s, int n);
__attribute__((import_module("env"), import_name("now"))) extern float js_now(void);

static _Alignas(16) uint8_t rio_mem[256 << 10]; /* the same app space as on the board */
static char src[64 << 10];
static Rio vm;
static uint32_t rgba[RIM_W * RIM_H];

static void put(const char *s, int n) { js_put(s, n); }

EXPORT(source) char *source(void) { return src; }
/* compile and start the cart in source(): 0, or -1 (error() says why) */
EXPORT(start) int start(int len) {
  int fn;
  rim_time = js_now;
  if (rim_start(&vm, rio_mem, sizeof rio_mem, put)) return -1;
  if (rio_compile(&vm, src, (uint32_t)len) || rio_run(&vm)) return -1;
  return (fn = rio_func(&vm, "main")) >= 0 ? rio_call(&vm, fn) : 0;
}
/* a frame with these buttons held: 0, 1 (not a game), -1 (error) */
EXPORT(frame) int frame(int buttons) { rim_buttons = (uint8_t)buttons; return rim_frame(&vm); }
EXPORT(error) const char *error(void) { return rio_error(&vm); }
/* the screen as RGBA bytes for a canvas */
EXPORT(pixels) uint32_t *pixels(void) {
  int i;
  for (i = 0; i < RIM_W * RIM_H; i++) {
    uint16_t c = rim_palette[((const uint8_t *)rim_screen)[i]];
    uint32_t r = (uint32_t)(c >> 11) * 255 / 31, g = (uint32_t)(c >> 5 & 63) * 255 / 63, b = (uint32_t)(c & 31) * 255 / 31;
    rgba[i] = 0xFF000000u | b << 16 | g << 8 | r;
  }
  return rgba;
}

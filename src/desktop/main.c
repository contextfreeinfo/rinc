/* Rim on the desktop:
     rim                       a rio REPL in the terminal
     rim cart.rio              run a cart in a window: its top-level code and main, then update* and
                               draw* 60 times a second if it's a game (arrows, Z/X: btn; Esc quits)
     -frames n                 instead of a window, run n frames (Z held for the first few)
     -o screen.ppm             instead of a window, afterwards write the screen as an image
     -i cart.img               compile to an image (see rio_image_load) instead, and list the host
                               procs its code calls (tools/web.py uses both)
   The app space is 256 KB, as on the Fruit Jam and the web, so a cart that fits one fits all three.
   The window is SDL3, linked in statically (RIM_SDL, on by default). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rim.h"
#if RIM_SDL
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

static uint8_t rio_mem[256 << 10], repl_scratch[1 << 18], image[512 << 10];
static char src[1 << 20];
static Rio vm;
static RimRepl repl;
static clock_t t0;

static void put(const char *s, int n) { fwrite(s, 1, (size_t)n, stdout); }
static float now(void) { return (float)(clock() - t0) / CLOCKS_PER_SEC; }

static int save_ppm(const char *path) { /* the screen through the palette, as 8-bit RGB */
  FILE *f = fopen(path, "wb"); int x, y;
  if (!f) return -1;
  fprintf(f, "P6 %d %d 255\n", RIM_W, RIM_H);
  for (y = 0; y < RIM_H; y++)
    for (x = 0; x < RIM_W; x++) {
      uint16_t c = rim_palette[rim_screen[y][x]];
      unsigned char rgb[3];
      rgb[0] = (unsigned char)((c >> 11) * 255 / 31); rgb[1] = (unsigned char)((c >> 5 & 63) * 255 / 63); rgb[2] = (unsigned char)((c & 31) * 255 / 31);
      fwrite(rgb, 1, 3, f);
    }
  return fclose(f);
}

/* -i: the compiled cart as an image, and the host procs its code calls, one per line */
#define OPENUM(o) OP_##o,
enum { RIO_OPS(OPENUM) OP_COUNT };
static int save_image(const char *cart, const char *path, size_t n) {
  uint32_t size, pc; uint8_t used[RIO_MAX_FFI] = {0}; FILE *f; int k;
  if (rio_compile(&vm, src, (uint32_t)n)) { fprintf(stderr, "%s:%s\n", cart, rio_error(&vm)); return 1; }
  for (pc = 0; pc < vm.pc; pc++) if (vm.code[pc].op == OP_FFI && vm.code[pc].c < RIO_MAX_FFI) used[vm.code[pc].c] = 1;
  for (k = 0; k < vm.nffi; k++) if (used[k]) printf("host %s\n", vm.ffi[k].name);
  if (!(size = rio_image_save(&vm, image, sizeof image))) { fprintf(stderr, "rim: can't make an image\n"); return 1; }
  if (!(f = fopen(path, "wb")) || fwrite(image, 1, size, f) != size || fclose(f)) { fprintf(stderr, "rim: can't write %s\n", path); return 1; }
  return 0;
}

#if RIM_SDL
static float sdl_now(void) { return (float)((double)SDL_GetTicksNS() / 1e9); }

/* the screen in a window, scaled up by whole steps; a game gets a frame every 1/60 s */
static int window(const char *title) {
  SDL_Window *w; SDL_Renderer *r; SDL_Texture *tex; uint64_t next; int game = 1, running = 1;
  if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "rim: %s\n", SDL_GetError()); return 1; }
  if (!SDL_CreateWindowAndRenderer(title, RIM_W * 2, RIM_H * 2, SDL_WINDOW_RESIZABLE, &w, &r)) { fprintf(stderr, "rim: %s\n", SDL_GetError()); return 1; }
  SDL_SetRenderLogicalPresentation(r, RIM_W, RIM_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
  SDL_SetRenderVSync(r, 1);
  tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, RIM_W, RIM_H);
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  rim_time = sdl_now;
  next = SDL_GetTicksNS();
  while (running) {
    SDL_Event e; const bool *k; uint16_t *px; int pitch, x, y;
    while (SDL_PollEvent(&e))
      if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) running = 0;
    k = SDL_GetKeyboardState(0);
    rim_buttons = (uint8_t)(k[SDL_SCANCODE_LEFT] * RIM_LEFT | k[SDL_SCANCODE_RIGHT] * RIM_RIGHT | k[SDL_SCANCODE_UP] * RIM_UP |
      k[SDL_SCANCODE_DOWN] * RIM_DOWN | (k[SDL_SCANCODE_Z] | k[SDL_SCANCODE_C]) * RIM_O | (k[SDL_SCANCODE_X] | k[SDL_SCANCODE_V]) * RIM_X);
    while (game && SDL_GetTicksNS() >= next) { /* 60 frames a second, whatever the display's rate */
      int res = rim_frame(&vm);
      if (res) { game = 0; if (res < 0) { fprintf(stderr, "%s\n", rio_error(&vm)); running = 0; } }
      next += 1000000000ull / 60;
      if (SDL_GetTicksNS() > next + 100000000ull) next = SDL_GetTicksNS(); /* (fell far behind: skip ahead) */
    }
    if (SDL_LockTexture(tex, 0, (void **)&px, &pitch)) {
      for (y = 0; y < RIM_H; y++)
        for (x = 0; x < RIM_W; x++) px[y * (pitch / 2) + x] = rim_palette[rim_screen[y][x]];
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
  const char *cart = 0, *out = 0, *img = 0; int i, fn, frames = 0;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "-frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-i") && i + 1 < argc) img = argv[++i];
    else if (argv[i][0] == '-' || cart) { fprintf(stderr, "usage: rim [cart.rio] [-frames n] [-o screen.ppm]\n"); return 2; }
    else cart = argv[i];
  }
  t0 = clock(); rim_time = now;
  if (rim_start(&vm, rio_mem, sizeof rio_mem, put)) { fprintf(stderr, "rim: can't start\n"); return 1; }
  if (cart) {
    FILE *f = fopen(cart, "rb"); size_t n;
    if (!f) { fprintf(stderr, "rim: can't open %s\n", cart); return 2; }
    n = fread(src, 1, sizeof src, f); fclose(f);
    if (img) return save_image(cart, img, n);
    if (rio_compile(&vm, src, (uint32_t)n) || rio_run(&vm) || ((fn = rio_func(&vm, "main")) >= 0 && rio_call(&vm, fn))) {
      fflush(stdout); fprintf(stderr, "%s:%s\n", cart, rio_error(&vm)); return 1;
    }
#if RIM_SDL
    if (!out && !frames) return window(cart);
#endif
    for (i = 0; i < frames; i++) { /* a game, without a window: Z held for the first frames (to start it) */
      rim_buttons = i < 3 ? RIM_O : 0;
      if (rim_frame(&vm) < 0) { fflush(stdout); fprintf(stderr, "%s:%s\n", cart, rio_error(&vm)); return 1; }
    }
  } else {
    int c;
    if (rim_repl_begin(&repl, &vm, repl_scratch, sizeof repl_scratch)) { fprintf(stderr, "rim: %s\n", rio_error(&vm)); return 1; }
    repl.echo = 0;
    while ((c = getchar()) != EOF) rim_repl_char(&repl, c);
    put("\n", 1);
  }
  return out && save_ppm(out) ? 1 : 0;
}

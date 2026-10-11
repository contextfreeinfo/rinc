/* Rim on the desktop. For now, without a window:
     rim                       a rio REPL in the terminal
     rim cart.rio              run a cart (its top-level code, then main if it has one)
     -o screen.ppm             afterwards, write the screen as an image
   A window (SDL3) comes later, sharing everything in core/. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "rim.h"

static uint8_t rio_mem[1 << 20], repl_scratch[1 << 18];
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

int main(int argc, char **argv) {
  const char *cart = 0, *out = 0; int i, fn;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (argv[i][0] == '-' || cart) { fprintf(stderr, "usage: rim [cart.rio] [-o screen.ppm]\n"); return 2; }
    else cart = argv[i];
  }
  t0 = clock(); rim_time = now;
  if (rim_start(&vm, rio_mem, sizeof rio_mem, put)) { fprintf(stderr, "rim: can't start\n"); return 1; }
  if (cart) {
    FILE *f = fopen(cart, "rb"); size_t n;
    if (!f) { fprintf(stderr, "rim: can't open %s\n", cart); return 2; }
    n = fread(src, 1, sizeof src, f); fclose(f);
    if (rio_compile(&vm, src, (uint32_t)n) || rio_run(&vm) || ((fn = rio_func(&vm, "main")) >= 0 && rio_call(&vm, fn))) {
      fflush(stdout); fprintf(stderr, "%s:%s\n", cart, rio_error(&vm)); return 1;
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

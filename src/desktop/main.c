/* Rim on the desktop:
     rim                       a rio REPL in the terminal
     rim cart.rio              run a cart in a window: its top-level code and main, then update* and
                               draw* 60 times a second if it's a game (arrows, V C X Z: btn; Esc quits)
     -frames n                 instead of a window, run n frames (Z held for the first few)
     -o screen.ppm             instead of a window, afterwards write the screen as an image
     -wav sound.wav            with -frames: the sound those frames made, as a WAV file
     -i cart.img               compile to an image (see rio_image_load) instead, and list the host
                               procs its code calls (tools/web.py uses both)
     -export game[.exe]        a copy of this rim with the cart in it, compiled: it runs the cart
                               (with the options above, too), so it's a game to hand out
     -runner rim-linux-x86_64  with -export: copy that rim instead (another system's, say, from a
                               release), for a game for that system
     -web game.html            one page that plays the cart in a browser: the web player (built in
                               when RIM_WEB_WASM is set) with the cart, compiled, all inline
   The app space is 256 KB, as on the Fruit Jam and the web, so a cart that fits one fits all three.
   The window is SDL3, linked in statically (RIM_SDL, on by default). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rim.h"
#ifdef _WIN32
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *module, char *name, unsigned long size);
#else
#include <sys/stat.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#if RIM_SDL
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

static uint8_t rio_mem[256 << 10], repl_scratch[1 << 18], image[512 << 10];
static int16_t wav[RIM_RATE * 2 * 60]; /* -wav: up to a minute */
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

/* the cart compiled, as an image in image[]: its size, or 0 after reporting why not. list: print the
   host procs its code calls, one per line */
#define OPENUM(o) OP_##o,
enum { RIO_OPS(OPENUM) OP_COUNT };
static uint32_t make_image(const char *cart, size_t n, int list) {
  uint32_t size, pc; uint8_t used[RIO_MAX_FFI] = {0}; int k;
  if (rio_compile(&vm, src, (uint32_t)n)) { fprintf(stderr, "%s:%s\n", cart, rio_error(&vm)); return 0; }
  for (pc = 0; pc < vm.pc; pc++) if (vm.code[pc].op == OP_FFI && vm.code[pc].c < RIO_MAX_FFI) used[vm.code[pc].c] = 1;
  for (k = 0; list && k < vm.nffi; k++) if (used[k]) printf("host %s\n", vm.ffi[k].name);
  if (!(size = rio_image_save(&vm, image, sizeof image))) fprintf(stderr, "rim: can't make an image\n");
  return size;
}
static int write_file(const char *path, const void *a, size_t an, const void *b, size_t bn, const void *c, size_t cn) {
  FILE *f = fopen(path, "wb"); int bad;
  if (!f) { fprintf(stderr, "rim: can't write %s\n", path); return 1; }
  bad = fwrite(a, 1, an, f) != an || fwrite(b, 1, bn, f) != bn || fwrite(c, 1, cn, f) != cn;
  if (fclose(f) || bad) { fprintf(stderr, "rim: can't write %s\n", path); return 1; }
  return 0;
}

/* -export: this program's own file, with an image after it and then a trailer: its size (4 bytes,
   little-endian) and MAGIC. At startup rim looks for one (so a cart in it runs instead of the REPL) */
#define MAGIC "RIMCART1"
static char *read_all(const char *path, size_t *len) {
  FILE *f; char *b; long n;
  if (!(f = fopen(path, "rb"))) return 0;
  if (fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) || !(b = malloc((size_t)n))) { fclose(f); return 0; }
  if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = 0; }
  fclose(f); *len = (size_t)n;
  return b;
}
static char *self_file(size_t *len) {
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
  return read_all(path, len);
}
/* the size of a cart at the end of file b (n long), or 0 if there's none */
static uint32_t carried(const char *b, size_t n) {
  uint32_t k;
  if (n < 12 || memcmp(b + n - 8, MAGIC, 8)) return 0;
  memcpy(&k, b + n - 12, 4);
  return k <= n - 12 && k <= sizeof image ? k : 0;
}
static int export_native(const char *path, const char *runner, uint32_t size) {
  size_t n; char *self = runner ? read_all(runner, &n) : self_file(&n), tail[12]; int r;
  if (!self) { fprintf(stderr, runner ? "rim: can't read %s\n" : "rim: can't read my own program file\n", runner); return 1; }
  if (carried(self, n)) n -= carried(self, n) + 12; /* (exporting from an export: just this cart) */
  memcpy(tail, &size, 4); memcpy(tail + 4, MAGIC, 8);
  r = write_file(path, self, n, image, size, tail, 12);
  free(self);
#ifndef _WIN32
  if (!r) chmod(path, 0755);
#endif
  return r;
}

#if RIM_WEB
extern const unsigned char rim_web_wasm[];
extern const unsigned rim_web_wasm_size;
extern const char rim_web_page[];
#endif
/* base64 of b (n long) into out, which has room for 4 * ((n + 2) / 3) characters */
static size_t base64(const uint8_t *b, size_t n, char *out) {
  static const char d[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i, k = 0;
  for (i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < n ? (uint32_t)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
    out[k++] = d[v >> 18]; out[k++] = d[v >> 12 & 63];
    out[k++] = i + 1 < n ? d[v >> 6 & 63] : '='; out[k++] = i + 2 < n ? d[v & 63] : '=';
  }
  return k;
}
/* -web: the page, with the title (the cart's name), the wasm and the image in place of {{TITLE}},
   {{WASM}} and {{CART}} */
static int export_web(const char *path, const char *cart, uint32_t size) {
#if RIM_WEB
  const char *p = rim_web_page, *name = cart, *q; char *wasm, *img, title[256]; size_t nw, ni, k = 0; FILE *f; int bad = 0;
  for (q = cart; *q; q++) if (*q == '/' || *q == '\\') name = q + 1;
  while (name[k] && name[k] != '.' && k < sizeof title - 1) { title[k] = strchr("<>&\"", name[k]) ? '_' : name[k]; k++; }
  title[k] = 0;
  wasm = malloc(4 * (rim_web_wasm_size / 3 + 1)); img = malloc(4 * (size / 3 + 1));
  if (!wasm || !img || !(f = fopen(path, "wb"))) { free(wasm); free(img); fprintf(stderr, "rim: can't write %s\n", path); return 1; }
  nw = base64(rim_web_wasm, rim_web_wasm_size, wasm); ni = base64(image, size, img);
  while (*p) {
    if (!strncmp(p, "{{TITLE}}", 9)) { fputs(title, f); p += 9; }
    else if (!strncmp(p, "{{WASM}}", 8)) { fwrite(wasm, 1, nw, f); p += 8; }
    else if (!strncmp(p, "{{CART}}", 8)) { fwrite(img, 1, ni, f); p += 8; }
    else fputc(*p++, f);
  }
  bad = ferror(f); if (fclose(f)) bad = 1;
  free(wasm); free(img);
  if (bad) { fprintf(stderr, "rim: can't write %s\n", path); return 1; }
  return 0;
#else
  (void)path; (void)cart; (void)size;
  fprintf(stderr, "rim: this rim was built without the web player (RIM_WEB_WASM)\n");
  return 1;
#endif
}

/* the sound -frames made, as a 16-bit stereo WAV file */
static int save_wav(const char *path, uint32_t frames) {
  FILE *f = fopen(path, "wb"); uint32_t bytes = frames * 4, v;
  unsigned char h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                         0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a'};
  if (!f) return -1;
  v = 36 + bytes; memcpy(h + 4, &v, 4); v = RIM_RATE; memcpy(h + 24, &v, 4); v = RIM_RATE * 4; memcpy(h + 28, &v, 4);
  fwrite(h, 1, 40, f); fwrite(&bytes, 4, 1, f); fwrite(wav, 4, frames, f);
  return fclose(f);
}

#if RIM_SDL
static SDL_AudioStream *audio;
static void audio_lock(int on) { if (on) SDL_LockAudioStream(audio); else SDL_UnlockAudioStream(audio); }
/* SDL wants more sound: mix it (SDL holds the stream's lock meanwhile, as host procs do) */
static void audio_more(void *ud, SDL_AudioStream *s, int more, int total) {
  static int16_t buf[1024 * 2];
  (void)ud; (void)total;
  while (more > 0) {
    int frames = more / 4 > 1024 ? 1024 : (more + 3) / 4;
    rim_audio_mix(buf, frames);
    SDL_PutAudioStreamData(s, buf, frames * 4);
    more -= frames * 4;
  }
}
static float sdl_now(void) { return (float)((double)SDL_GetTicksNS() / 1e9); }

/* a controller (SDL's gamepads: Xbox and the rest), as player 1: its buttons as Rim's, its sticks and
   triggers into rim_axes */
static SDL_Gamepad *pad;
static uint16_t pad_read(void) {
  static const struct { int sdl; uint16_t rim; } map[] = {
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, RIM_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, RIM_RIGHT}, {SDL_GAMEPAD_BUTTON_DPAD_UP, RIM_UP},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, RIM_DOWN}, {SDL_GAMEPAD_BUTTON_SOUTH, RIM_A}, {SDL_GAMEPAD_BUTTON_EAST, RIM_B},
    {SDL_GAMEPAD_BUTTON_WEST, RIM_X}, {SDL_GAMEPAD_BUTTON_NORTH, RIM_Y}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, RIM_LB},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, RIM_RB}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, RIM_LS}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, RIM_RS},
    {SDL_GAMEPAD_BUTTON_START, RIM_START}, {SDL_GAMEPAD_BUTTON_BACK, RIM_BACK}};
  static const int axes[6] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};
  uint16_t b = 0; size_t i;
  if (!pad) { memset(rim_axes, 0, sizeof rim_axes); return 0; }
  for (i = 0; i < sizeof map / sizeof map[0]; i++) if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)map[i].sdl)) b |= map[i].rim;
  for (i = 0; i < 6; i++) { int v = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)axes[i]); rim_axes[i] = (int16_t)(v < -32767 ? -32767 : v); }
  return b;
}

/* the screen in a window, scaled up by whole steps; a game gets a frame every 1/60 s */
static int window(const char *title) {
  SDL_Window *w; SDL_Renderer *r; SDL_Texture *tex; uint64_t next; int game = 1, running = 1;
  SDL_AudioSpec spec;
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "rim: %s\n", SDL_GetError()); return 1; }
  spec.format = SDL_AUDIO_S16; spec.channels = 2; spec.freq = RIM_RATE;
  if ((audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_more, 0))) { /* (no sound device: play on silently) */
    rim_audio_lock = audio_lock;
    SDL_ResumeAudioStreamDevice(audio);
  }
  if (!SDL_CreateWindowAndRenderer(title, RIM_W * 2, RIM_H * 2, SDL_WINDOW_RESIZABLE, &w, &r)) { fprintf(stderr, "rim: %s\n", SDL_GetError()); return 1; }
  SDL_SetRenderLogicalPresentation(r, RIM_W, RIM_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
  SDL_SetRenderVSync(r, 1);
  SDL_StartTextInput(w);
  tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, RIM_W, RIM_H);
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  rim_time = sdl_now;
  next = SDL_GetTicksNS();
  while (running) {
    SDL_Event e; const bool *k; uint16_t *px; int pitch, x, y;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) running = 0;
      if (e.type == SDL_EVENT_TEXT_INPUT) { const char *t = e.text.text; while (*t) rim_text_push((unsigned char)*t++); } /* (key(): typing) */
      if (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)) rim_text_push(13);
      if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_BACKSPACE) rim_text_push(8);
      if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(e.gdevice.which);
      if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && SDL_GetGamepadID(pad) == e.gdevice.which) { SDL_CloseGamepad(pad); pad = 0; }
    }
    k = SDL_GetKeyboardState(0);
    rim_buttons = pad_read() | (uint16_t)(k[SDL_SCANCODE_LEFT] * RIM_LEFT | k[SDL_SCANCODE_RIGHT] * RIM_RIGHT | k[SDL_SCANCODE_UP] * RIM_UP |
      k[SDL_SCANCODE_DOWN] * RIM_DOWN | k[SDL_SCANCODE_V] * RIM_A | k[SDL_SCANCODE_C] * RIM_B | k[SDL_SCANCODE_X] * RIM_X | k[SDL_SCANCODE_Z] * RIM_Y);
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
  const char *cart = 0, *out = 0, *img = 0, *wavout = 0, *exp = 0, *web = 0, *runner = 0; int i, fn, frames = 0; uint32_t size = 0;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "-frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-i") && i + 1 < argc) img = argv[++i];
    else if (!strcmp(argv[i], "-wav") && i + 1 < argc) wavout = argv[++i];
    else if (!strcmp(argv[i], "-export") && i + 1 < argc) exp = argv[++i];
    else if (!strcmp(argv[i], "-web") && i + 1 < argc) web = argv[++i];
    else if (!strcmp(argv[i], "-runner") && i + 1 < argc) runner = argv[++i];
    else if (argv[i][0] == '-' || cart) {
      fprintf(stderr, "usage: rim [cart.rio] [-frames n] [-o screen.ppm] [-wav sound.wav] [-i cart.img] [-export game [-runner rim]] [-web game.html]\n");
      return 2;
    }
    else cart = argv[i];
  }
  t0 = clock(); rim_time = now;
  if (rim_start(&vm, rio_mem, sizeof rio_mem, put)) { fprintf(stderr, "rim: can't start\n"); return 1; }
  if (!cart) { /* an exported game: the cart it carries */
    size_t n; char *self = self_file(&n);
    if (self && (size = carried(self, n))) memcpy(image, self + n - 12 - size, size);
    free(self);
    if (size) cart = argc > 0 ? argv[0] : "rim";
  }
  if (cart) {
    if (!size) {
      FILE *f = fopen(cart, "rb"); size_t n;
      if (!f) { fprintf(stderr, "rim: can't open %s\n", cart); return 2; }
      n = fread(src, 1, sizeof src, f); fclose(f);
      if (img || exp || web) {
        if (!(size = make_image(cart, n, img != 0))) return 1;
        if (img && write_file(img, image, size, 0, 0, 0, 0)) return 1;
        if (exp && export_native(exp, runner, size)) return 1;
        return web ? export_web(web, cart, size) : 0;
      }
      if (rio_compile(&vm, src, (uint32_t)n)) { fprintf(stderr, "%s:%s\n", cart, rio_error(&vm)); return 1; }
    } else if (rio_image_load(&vm, image, size)) { fprintf(stderr, "%s: %s\n", cart, rio_error(&vm)); return 1; }
    if (rio_run(&vm) || ((fn = rio_func(&vm, "main")) >= 0 && rio_call(&vm, fn))) {
      fflush(stdout); fprintf(stderr, "%s:%s\n", cart, rio_error(&vm)); return 1;
    }
#if RIM_SDL
    if (!out && !frames) return window(cart);
#endif
    for (i = 0; i < frames; i++) { /* a game, without a window: Z held for the first frames (to start it) */
      rim_buttons = i < 3 ? RIM_A : 0;
      if (rim_frame(&vm) < 0) { fflush(stdout); fprintf(stderr, "%s:%s\n", cart, rio_error(&vm)); return 1; }
      if (wavout && i < 60 * 60) rim_audio_mix(wav + i * (RIM_RATE / 60) * 2, RIM_RATE / 60); /* (a frame's worth of sound) */
    }
    if (wavout && save_wav(wavout, (uint32_t)(frames < 3600 ? frames : 3600) * (RIM_RATE / 60))) { fprintf(stderr, "rim: can't write %s\n", wavout); return 1; }
  } else {
    int c;
    if (rim_repl_begin(&repl, &vm, repl_scratch, sizeof repl_scratch)) { fprintf(stderr, "rim: %s\n", rio_error(&vm)); return 1; }
    repl.echo = 0;
    while ((c = getchar()) != EOF) rim_repl_char(&repl, c);
    put("\n", 1);
  }
  return out && save_ppm(out) ? 1 : 0;
}

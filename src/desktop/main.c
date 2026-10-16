/* Rinc on the desktop:
     rinc                       a rio REPL in the terminal
     rinc cart.rio              run a cart in a window: its top-level code and main, then update* and
                               draw* 60 times a second if it's a game (arrows, V C X Z: btn; Esc quits)
     -frames n                 instead of a window, run n frames (Z held for the first few)
     -o screen.ppm             instead of a window, afterwards write the screen as an image
     -wav sound.wav            with -frames: the sound those frames made, as a WAV file
     -i cart.img               compile to an image (see rio_loadImage) instead, and list the host
                               procs its code calls (tools/web.py uses both)
     -export game[.exe]        a copy of this rinc with the cart in it, compiled: it runs the cart
                               (with the options above, too), so it's a game to hand out
     -runner rinc-linux-x86_64  with -export: copy that rinc instead (another system's, say, from a
                               release), for a game for that system
     -web game.html            one page that plays the cart in a browser: the web player (built in
                               when RINC_WEB_WASM is set) with the cart, compiled, all inline
   The app space is 256 KB, as on the Fruit Jam and the web, so a cart that fits one fits all three.
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

/* the cart compiled, as an image in image[]: its size, or 0 after reporting why not. list: print the
   host procs its code calls, one per line */
static uint32_t rinc_makeImage(const char *cart, size_t n, int list) {
  uint32_t size, pc; uint8_t used[rio_MAX_FFI] = {0}; int k;
  if (rio_compile(&rinc_vm, rinc_src, (uint32_t)n)) { fprintf(stderr, "%s:%s\n", cart, rio_error(&rinc_vm)); return 0; }
  for (pc = 0; pc < rinc_vm.codeLen; pc++) if (rinc_vm.code[pc].op == rio_Op_ffi && rinc_vm.code[pc].c < rio_MAX_FFI) used[rinc_vm.code[pc].c] = 1;
  for (k = 0; list && k < rinc_vm.ffiCount; k++) if (used[k]) printf("host %s\n", rinc_vm.ffi[k].name);
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

/* -export: this program's own file, with an image after it and then a trailer: its size (4 bytes,
   little-endian) and rinc_MAGIC. At startup rinc looks for one (so a cart in it runs instead of the REPL) */
#define rinc_MAGIC "RINCAPP0"
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
/* the size of a cart at the end of file b (n long), or 0 if there's none */
static uint32_t rinc_carried(const char *b, size_t n) {
  uint32_t k;
  if (n < 12 || memcmp(b + n - 8, rinc_MAGIC, 8)) return 0;
  memcpy(&k, b + n - 12, 4);
  return k <= n - 12 && k <= sizeof rinc_image ? k : 0;
}
static int rinc_exportNative(const char *path, const char *runner, uint32_t size) {
  size_t n; char *self = runner ? rinc_readAll(runner, &n) : rinc_selfFile(&n), tail[12]; int r;
  if (!self) { fprintf(stderr, runner ? "rinc: can't read %s\n" : "rinc: can't read my own program file\n", runner); return 1; }
  if (rinc_carried(self, n)) n -= rinc_carried(self, n) + 12; /* (exporting from an export: just this cart) */
  memcpy(tail, &size, 4); memcpy(tail + 4, rinc_MAGIC, 8);
  r = rinc_writeFile(path, self, n, rinc_image, size, tail, 12);
  free(self);
#ifndef _WIN32
  if (!r) chmod(path, 0755);
#endif
  return r;
}

#if rinc_WEB
extern const unsigned char rinc_webWasm[];
extern const unsigned rinc_webWasmSize;
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
/* -web: the page, with the title (the cart's name), the wasm and the image in place of {{TITLE}},
   {{WASM}} and {{CART}} */
static int rinc_exportWeb(const char *path, const char *cart, uint32_t size) {
#if rinc_WEB
  const char *p = rinc_webPage, *name = cart, *q; char *wasm, *img, title[256]; size_t nw, ni, k = 0; FILE *f; int bad = 0;
  for (q = cart; *q; q++) if (*q == '/' || *q == '\\') name = q + 1;
  while (name[k] && name[k] != '.' && k < sizeof title - 1) { title[k] = strchr("<>&\"", name[k]) ? '_' : name[k]; k++; }
  title[k] = 0;
  wasm = malloc(4 * (rinc_webWasmSize / 3 + 1)); img = malloc(4 * (size / 3 + 1));
  if (!wasm || !img || !(f = fopen(path, "wb"))) { free(wasm); free(img); fprintf(stderr, "rinc: can't write %s\n", path); return 1; }
  nw = rinc_base64(rinc_webWasm, rinc_webWasmSize, wasm); ni = rinc_base64(rinc_image, size, img);
  while (*p) {
    if (!strncmp(p, "{{TITLE}}", 9)) { fputs(title, f); p += 9; }
    else if (!strncmp(p, "{{WASM}}", 8)) { fwrite(wasm, 1, nw, f); p += 8; }
    else if (!strncmp(p, "{{CART}}", 8)) { fwrite(img, 1, ni, f); p += 8; }
    else fputc(*p++, f);
  }
  bad = ferror(f); if (fclose(f)) bad = 1;
  free(wasm); free(img);
  if (bad) { fprintf(stderr, "rinc: can't write %s\n", path); return 1; }
  return 0;
#else
  (void)path; (void)cart; (void)size;
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
      if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) running = 0;
      if (e.type == SDL_EVENT_TEXT_INPUT) { const char *t = e.text.text; while (*t) rinc_textPush((unsigned char)*t++); } /* (key(): typing) */
      if (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)) rinc_textPush(13);
      if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_BACKSPACE) rinc_textPush(8);
      if (e.type == SDL_EVENT_GAMEPAD_ADDED && !rinc_pad) rinc_pad = SDL_OpenGamepad(e.gdevice.which);
      if (e.type == SDL_EVENT_GAMEPAD_REMOVED && rinc_pad && SDL_GetGamepadID(rinc_pad) == e.gdevice.which) { SDL_CloseGamepad(rinc_pad); rinc_pad = 0; }
    }
    k = SDL_GetKeyboardState(0);
    rinc_buttons = rinc_readPad() | (uint16_t)(k[SDL_SCANCODE_LEFT] * rinc_Button_left | k[SDL_SCANCODE_RIGHT] * rinc_Button_right | k[SDL_SCANCODE_UP] * rinc_Button_up |
      k[SDL_SCANCODE_DOWN] * rinc_Button_down | k[SDL_SCANCODE_V] * rinc_Button_a | k[SDL_SCANCODE_C] * rinc_Button_b | k[SDL_SCANCODE_X] * rinc_Button_x | k[SDL_SCANCODE_Z] * rinc_Button_y);
    while (game && SDL_GetTicksNS() >= next) { /* 60 frames a second, whatever the display's rate */
      int res = rinc_frame(&rinc_vm);
      if (res) { game = 0; if (res < 0) { fprintf(stderr, "%s\n", rio_error(&rinc_vm)); running = 0; } }
      next += 1000000000ull / 60;
      if (SDL_GetTicksNS() > next + 100000000ull) next = SDL_GetTicksNS(); /* (fell far behind: skip ahead) */
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
      fprintf(stderr, "usage: rinc [cart.rio] [-frames n] [-o screen.ppm] [-wav sound.wav] [-i cart.img] [-export game [-runner rinc]] [-web game.html]\n");
      return 2;
    }
    else cart = argv[i];
  }
  rinc_t0 = clock(); rinc_time = rinc_now;
  if (rinc_start(&rinc_vm, rinc_appSpace, sizeof rinc_appSpace, rinc_put)) { fprintf(stderr, "rinc: can't start\n"); return 1; }
  if (!cart) { /* an exported game: the cart it carries */
    size_t n; char *self = rinc_selfFile(&n);
    if (self && (size = rinc_carried(self, n))) memcpy(rinc_image, self + n - 12 - size, size);
    free(self);
    if (size) cart = argc > 0 ? argv[0] : "rinc";
  }
  if (cart) {
    if (!size) {
      FILE *f = fopen(cart, "rb"); size_t n;
      if (!f) { fprintf(stderr, "rinc: can't open %s\n", cart); return 2; }
      n = fread(rinc_src, 1, sizeof rinc_src, f); fclose(f);
      if (img || exp || web) {
        if (!(size = rinc_makeImage(cart, n, img != 0))) return 1;
        if (img && rinc_writeFile(img, rinc_image, size, 0, 0, 0, 0)) return 1;
        if (exp && rinc_exportNative(exp, runner, size)) return 1;
        return web ? rinc_exportWeb(web, cart, size) : 0;
      }
      if (rio_compile(&rinc_vm, rinc_src, (uint32_t)n)) { fprintf(stderr, "%s:%s\n", cart, rio_error(&rinc_vm)); return 1; }
    } else if (rio_loadImage(&rinc_vm, rinc_image, size)) { fprintf(stderr, "%s: %s\n", cart, rio_error(&rinc_vm)); return 1; }
    if (rio_run(&rinc_vm) || ((fn = rio_findProc(&rinc_vm, "main")) >= 0 && rio_call(&rinc_vm, fn))) {
      fflush(stdout); fprintf(stderr, "%s:%s\n", cart, rio_error(&rinc_vm)); return 1;
    }
#if rinc_SDL
    if (!out && !frames) return rinc_window(cart);
#endif
    for (i = 0; i < frames; i++) { /* a game, without a window: Z held for the first frames (to start it) */
      rinc_buttons = i < 3 ? rinc_Button_a : 0;
      if (rinc_frame(&rinc_vm) < 0) { fflush(stdout); fprintf(stderr, "%s:%s\n", cart, rio_error(&rinc_vm)); return 1; }
      if (wavout && i < 60 * 60) rinc_audioMix(rinc_wav + i * (rinc_RATE / 60) * 2, rinc_RATE / 60); /* (a frame's worth of sound) */
    }
    if (wavout && rinc_saveWav(wavout, (uint32_t)(frames < 3600 ? frames : 3600) * (rinc_RATE / 60))) { fprintf(stderr, "rinc: can't write %s\n", wavout); return 1; }
  } else {
    int c;
    if (rinc_replBegin(&rinc_repl, &rinc_vm, rinc_replScratch, sizeof rinc_replScratch)) { fprintf(stderr, "rinc: %s\n", rio_error(&rinc_vm)); return 1; }
    rinc_repl.echo = 0;
    while ((c = getchar()) != EOF) rinc_replChar(&rinc_repl, c);
    rinc_put("\n", 1);
  }
  return out && rinc_savePpm(out) ? 1 : 0;
}

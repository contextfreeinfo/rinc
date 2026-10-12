/* sound: 8 voices mixed to 48 kHz 16-bit stereo, the same on every platform. A voice plays sound data
   from the cart's own memory as it goes (8- or 16-bit signed, mono or stereo, at any rate), with its
   volume, rate and pan changeable while it plays, and optionally a loop. Linear interpolation between
   samples. The platform asks for samples (rim_audio_mix) from wherever its output wants them, and
   brackets changes from host procs with its own lock (rim_audio_lock). */
#include <string.h>
#include "rim.h"

typedef struct {
  const uint8_t *data; uint32_t frames; /* the sound, in frames (a sample, or a stereo pair) */
  uint64_t pos, step;                   /* 32.32 frames: where, and how far each output frame */
  uint32_t loop0, loop1;                /* loop1 > loop0: play [loop0, loop1) over and over */
  int32_t gl, gr;                       /* gains, 16.16: volume with pan */
  float vol, pan;
  uint8_t bits16, stereo, on;
} Voice;

static Voice voices[RIM_VOICES];
void (*rim_audio_lock)(int on);
static void lock(int on) { if (rim_audio_lock) rim_audio_lock(on); }

static void gains(Voice *v) { /* balance: the center is full on both sides */
  float l = v->pan > 0 ? 1 - v->pan : 1, r = v->pan < 0 ? 1 + v->pan : 1;
  v->gl = (int32_t)(v->vol * l * 65536.0f); v->gr = (int32_t)(v->vol * r * 65536.0f);
}
static float clampf(float x, float lo, float hi) { return x != x ? lo : x < lo ? lo : x > hi ? hi : x; }
static uint64_t stepfor(float rate) { return (uint64_t)((double)clampf(rate, 0, RIM_RATE * 16.0f) / RIM_RATE * 4294967296.0); }

/* sample at frame i, channel c (mono: c ignored), as 16-bit */
static int32_t sample(const Voice *v, uint32_t i, int c) {
  uint32_t k = i * (v->stereo ? 2u : 1u) + (v->stereo ? (uint32_t)c : 0u);
  if (v->bits16) return (int16_t)(v->data[k * 2] | v->data[k * 2 + 1] << 8);
  return (int8_t)v->data[k] * 256;
}

void rim_audio_mix(int16_t *out, int frames) {
  int f, k;
  for (f = 0; f < frames; f++) {
    int32_t l = 0, r = 0;
    for (k = 0; k < RIM_VOICES; k++) {
      Voice *v = &voices[k]; uint32_t i, j; int32_t t, a, b;
      if (!v->on) continue;
      i = (uint32_t)(v->pos >> 32);
      if (v->loop1 > v->loop0) { while (i >= v->loop1) { v->pos -= (uint64_t)(v->loop1 - v->loop0) << 32; i = (uint32_t)(v->pos >> 32); } }
      else if (i >= v->frames) { v->on = 0; continue; }
      j = i + 1 < (v->loop1 > v->loop0 ? v->loop1 : v->frames) ? i + 1 : v->loop1 > v->loop0 ? v->loop0 : i; /* the next frame, for interpolating */
      t = (int32_t)(v->pos >> 17 & 0x7FFF); /* (15 bits: (b - a) * t stays in 32) */
      a = sample(v, i, 0); b = sample(v, j, 0);
      l += (int32_t)(((int64_t)(a + ((b - a) * t >> 15)) * v->gl) >> 16);
      if (v->stereo) { a = sample(v, i, 1); b = sample(v, j, 1); }
      r += (int32_t)(((int64_t)(a + ((b - a) * t >> 15)) * v->gr) >> 16);
      v->pos += v->step;
    }
    out[f * 2] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
    out[f * 2 + 1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
  }
}

void rim_audio_reset(void) { lock(1); memset(voices, 0, sizeof voices); lock(0); }

/* host procs. sound(voice, data, bits, channels, rate): play data (8 or 16 bits, 1 or 2 channels) at
   rate frames a second, full volume, centered, once. A voice out of 0-7, or bad data, does nothing */
static Voice *voice(RioVal *a) { return a[0].u < RIM_VOICES ? &voices[a[0].u] : 0; }
static void h_sound(Rio *vm, RioVal *a) {
  Voice *v = voice(a); const uint8_t *p = (const uint8_t *)rio_ptr(vm, a + 1); uint32_t bytes = a[2].u, size;
  if (!v || !p || (a[3].i != 8 && a[3].i != 16) || (a[4].i != 1 && a[4].i != 2)) return;
  size = (uint32_t)(a[3].i / 8 * a[4].i);
  lock(1);
  v->data = p; v->frames = bytes / size; v->bits16 = a[3].i == 16; v->stereo = a[4].i == 2;
  v->pos = 0; v->step = stepfor(a[5].f); v->loop0 = v->loop1 = 0; v->vol = 1; v->pan = 0; gains(v);
  v->on = v->frames > 0;
  lock(0);
}
static void h_rate(Rio *vm, RioVal *a) { Voice *v = voice(a); (void)vm; if (v) { lock(1); v->step = stepfor(a[1].f); lock(0); } }
static void h_volume(Rio *vm, RioVal *a) { Voice *v = voice(a); (void)vm; if (v) { lock(1); v->vol = clampf(a[1].f, 0, 1); gains(v); lock(0); } }
static void h_pan(Rio *vm, RioVal *a) { Voice *v = voice(a); (void)vm; if (v) { lock(1); v->pan = clampf(a[1].f, -1, 1); gains(v); lock(0); } }
/* soundLoop(voice, start, end): from end, go back to start (frames); end 0 or less: no loop */
static void h_loop(Rio *vm, RioVal *a) {
  Voice *v = voice(a); (void)vm;
  if (!v) return;
  lock(1);
  v->loop0 = a[1].i > 0 ? a[1].u : 0; v->loop1 = a[2].i > 0 ? a[2].u : 0;
  if (v->loop1 > v->frames) v->loop1 = v->frames;
  if (v->loop0 >= v->loop1) v->loop0 = v->loop1 = 0;
  lock(0);
}
static void h_stop(Rio *vm, RioVal *a) { Voice *v = voice(a); (void)vm; if (v) { lock(1); v->on = 0; lock(0); } }
static void h_playing(Rio *vm, RioVal *a) { Voice *v = voice(a); (void)vm; a[0].i = v && v->on; }

int rim_audio_procs(Rio *vm) {
  return rio_ffi(vm, "sound", "ibiif", h_sound) || rio_ffi(vm, "soundRate", "if", h_rate) || rio_ffi(vm, "soundVolume", "if", h_volume) ||
         rio_ffi(vm, "soundPan", "if", h_pan) || rio_ffi(vm, "soundLoop", "iii", h_loop) || rio_ffi(vm, "soundStop", "i", h_stop) ||
         rio_ffi(vm, "soundPlaying", "i>i", h_playing);
}

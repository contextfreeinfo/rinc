/* sound: 8 voices mixed to 48 kHz 16-bit stereo, the same on every platform. A voice plays sound data
   from the cart's own memory as it goes (8- or 16-bit signed, or 4-bit IMA ADPCM; mono or stereo; at
   any rate), with its volume, rate and pan changeable while it plays, and optionally a loop. Linear
   interpolation between samples. The platform asks for samples (rinc_audioMix) from wherever its
   output wants them, and brackets changes from host procs with its own lock (rinc_audioLock).

   ADPCM is IMA's, laid out as in WAV files (format 0x11): blocks that each start with every channel's
   first sample and step index, then 4-bit codes (mono: in byte order, low half first; stereo: 4 bytes,
   8 codes, of each channel in turn). A voice decodes it as it plays, one frame at a time; a block is
   where decoding can start over, so going back (a loop) starts at a block, and soundLoop keeps the
   decoder's state at the loop's start so wrapping around costs nothing. A quarter the size of 16-bit,
   and close to it in sound. */
#include <math.h>
#include <string.h>
#include "rinc.h"

/* ADPCM decoding, one frame at a time: at frame at, each channel's value there (a) and at the next
   frame (b); blk and k: where the next frame is (its block, and which frame of it); pred and index:
   the decoder's state after it */
typedef struct { uint32_t at, k; const uint8_t *blk; int32_t a[2], b[2], pred[2]; uint8_t index[2]; } rinc_AdpcmState;
typedef struct {
  const uint8_t *data; uint32_t frames; /* the sound, in frames (a sample, or a stereo pair) */
  uint32_t pos, frac, step, fstep;      /* where (a frame, and 32 bits of fraction), and how far each output frame */
  uint32_t loop0, loop1;                /* loop1 > loop0: play [loop0, loop1) over and over */
  int32_t gl, gr;                       /* gains, 1.15 (32768 is 1): volume with pan */
  float vol, pan;
  uint8_t bits, stereo, on;             /* bits: 8, 16 or 4 (ADPCM) */
  uint32_t block, per;                  /* ADPCM: bytes in a block, and frames */
  rinc_AdpcmState dec, at0;                         /* ADPCM: the decoder, and its state at loop0 */
} rinc_Voice;

static rinc_Voice rinc_voices[rinc_VOICES];
static volatile int32_t rinc_master = 32768; /* the system's volume, over all of it (1.15) */
static int rinc_level = rinc_VOLUME_STEPS, rinc_muted;
void (*rinc_audioLock)(int on);
static void rinc_lock(int on) { if (rinc_audioLock) rinc_audioLock(on); }

static void rinc_gains(rinc_Voice *v) { /* balance: the center is full on both sides */
  float l = v->pan > 0 ? 1 - v->pan : 1, r = v->pan < 0 ? 1 + v->pan : 1;
  v->gl = (int32_t)(v->vol * l * 32768.0f); v->gr = (int32_t)(v->vol * r * 32768.0f);
}
static float rinc_clampf(float x, float lo, float hi) { return x != x ? lo : x < lo ? lo : x > hi ? hi : x; }
static void rinc_stepFor(rinc_Voice *v, float rate) {
  uint64_t s = (uint64_t)((double)rinc_clampf(rate, 0, rinc_RATE * 16.0f) / rinc_RATE * 4294967296.0);
  v->step = (uint32_t)(s >> 32); v->fstep = (uint32_t)s;
}

/* IMA ADPCM */
static const int16_t rinc_steps[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
  73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
  876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
  6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t rinc_moves[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
/* channel c of the frame at (blk, k), into the decoder's state */
static int32_t rinc_HOT rinc_adpcmFrame(const rinc_Voice *v, rinc_AdpcmState *d, int c) {
  const uint8_t *b = d->blk; uint32_t k = d->k, step; int32_t diff, p; int code, x;
  if (!k) { /* a block's header: the sample itself, and the step index */
    d->pred[c] = (int16_t)(b[c * 4] | b[c * 4 + 1] << 8); d->index[c] = b[c * 4 + 2] > 88 ? 88 : b[c * 4 + 2];
    return d->pred[c];
  }
  k--; /* (which code) */
  code = (v->stereo ? b[8 + (k >> 3) * 8 + (uint32_t)c * 4 + (k & 7) / 2] : b[4 + k / 2]) >> (k & 1) * 4 & 15;
  step = (uint32_t)rinc_steps[d->index[c]];
  diff = (int32_t)(step >> 3);
  if (code & 1) diff += (int32_t)(step >> 2);
  if (code & 2) diff += (int32_t)(step >> 1);
  if (code & 4) diff += (int32_t)step;
  p = d->pred[c] + (code & 8 ? -diff : diff);
  d->pred[c] = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
  x = d->index[c] + rinc_moves[code & 7]; d->index[c] = (uint8_t)(x < 0 ? 0 : x > 88 ? 88 : x);
  return d->pred[c];
}
/* the decoder a frame on */
static void rinc_HOT rinc_adpcmNext(const rinc_Voice *v, rinc_AdpcmState *d) {
  int c, n = v->stereo ? 2 : 1;
  d->at++;
  if (++d->k == v->per) { d->k = 0; d->blk += v->block; }
  for (c = 0; c < n; c++) { d->a[c] = d->b[c]; if (d->at + 1 < v->frames) d->b[c] = rinc_adpcmFrame(v, d, c); }
}
/* the decoder at frame i: from i's block if it's past i, then forward */
static void rinc_HOT rinc_adpcmTo(const rinc_Voice *v, rinc_AdpcmState *d, uint32_t i) {
  if (d->at > i || d->at == 0xFFFFFFFFu) {
    int c, n = v->stereo ? 2 : 1;
    d->at = i - i % v->per; d->k = 0; d->blk = v->data + i / v->per * v->block;
    for (c = 0; c < n; c++) d->a[c] = d->b[c] = rinc_adpcmFrame(v, d, c);
    if (d->at + 1 < v->frames) { d->k = 1; for (c = 0; c < n; c++) d->b[c] = rinc_adpcmFrame(v, d, c); }
    else d->k = 1; /* (a one-frame sound) */
  }
  while (d->at < i) rinc_adpcmNext(v, d);
}
/* frames in n bytes of ADPCM (blocks of block bytes, the last maybe short) */
static uint32_t rinc_adpcmFrames(uint32_t n, uint32_t block, int stereo) {
  uint32_t ch = stereo ? 2 : 1, per = (block / ch - 4) * 2 + 1, r = n % block, last = 0;
  if (r >= 4 * ch) last = stereo ? 1 + (r - 4 * ch) / 8 * 8 : 1 + (r - 4) * 2;
  return n / block * per + last;
}

/* one voice into the sums for n frames, as it goes: where it is, its loop, its next frame (to
   interpolate toward), its gains */
#define rinc_CHUNK 64
static int32_t rinc_sums[rinc_CHUNK * 2];
static void rinc_HOT rinc_mixVoice(rinc_Voice *v, int n) {
  uint32_t i = v->pos, fr = v->frac, step = v->step, fstep = v->fstep, loop0 = v->loop0, loop1 = v->loop1;
  uint32_t end = loop1 > loop0 ? loop1 : v->frames;
  int32_t gl = v->gl, gr = v->gr, *out = rinc_sums; int f, looping = loop1 > loop0, bits = v->bits, stereo = v->stereo;
  const uint8_t *data = v->data;
  for (f = 0; f < n; f++, out += 2) {
    int32_t t, a, b, l, r; uint32_t j;
    if (i >= end) {
      if (!looping) { v->on = 0; break; }
      do i -= loop1 - loop0; while (i >= end);
      if (bits == 4 && i < v->dec.at) v->dec = v->at0; /* (wrapped around: the decoder from the loop's start) */
    }
    j = i + 1 < end ? i + 1 : looping ? loop0 : i; /* the next frame, for interpolating */
    t = (int32_t)(fr >> 17); /* (15 bits: (b - a) * t stays in 32) */
    if (bits == 16) {
      const uint8_t *p = data + i * (stereo ? 4u : 2u), *q = data + j * (stereo ? 4u : 2u);
      a = (int16_t)(p[0] | p[1] << 8); b = (int16_t)(q[0] | q[1] << 8); l = a + ((b - a) * t >> 15);
      if (stereo) { a = (int16_t)(p[2] | p[3] << 8); b = (int16_t)(q[2] | q[3] << 8); r = a + ((b - a) * t >> 15); } else r = l;
    } else if (bits == 8) {
      const int8_t *p = (const int8_t *)data + i * (stereo ? 2u : 1u), *q = (const int8_t *)data + j * (stereo ? 2u : 1u);
      a = p[0] * 256; b = q[0] * 256; l = a + ((b - a) * t >> 15);
      if (stereo) { a = p[1] * 256; b = q[1] * 256; r = a + ((b - a) * t >> 15); } else r = l;
    } else { /* ADPCM: frame i, and the next (at a loop's end, its start: kept since soundLoop) */
      rinc_AdpcmState *d = &v->dec;
      if (d->at != i) rinc_adpcmTo(v, d, i);
      a = d->a[0]; b = j == i + 1 ? d->b[0] : j == i ? a : v->at0.a[0]; l = a + ((b - a) * t >> 15);
      if (stereo) { a = d->a[1]; b = j == i + 1 ? d->b[1] : j == i ? a : v->at0.a[1]; r = a + ((b - a) * t >> 15); } else r = l;
    }
    out[0] += l * gl >> 15; out[1] += r * gr >> 15;
    fr += fstep; i += step + (fr < fstep); /* (the fraction's carry) */
  }
  v->pos = i; v->frac = fr;
}
/* the common cases, with nothing in the way: mono 16-bit, and mono ADPCM (its decoder in locals) */
static void rinc_HOT rinc_mixPcm16(rinc_Voice *v, int n) {
  uint32_t i = v->pos, fr = v->frac, step = v->step, fstep = v->fstep, loop0 = v->loop0, loop1 = v->loop1;
  uint32_t end = loop1 > loop0 ? loop1 : v->frames;
  int32_t gl = v->gl, gr = v->gr, *out = rinc_sums; int f, looping = loop1 > loop0;
  const int16_t *data = (const int16_t *)(const void *)v->data; /* (data from rio_sliceBytes: 2-byte aligned for 16-bit) */
  for (f = 0; f < n; f++, out += 2) {
    int32_t a, b, x; uint32_t j;
    if (i >= end) {
      if (!looping) { v->on = 0; break; }
      do i -= loop1 - loop0; while (i >= end);
    }
    j = i + 1 < end ? i + 1 : looping ? loop0 : i;
    a = data[i]; b = data[j]; x = a + ((b - a) * (int32_t)(fr >> 17) >> 15);
    out[0] += x * gl >> 15; out[1] += x * gr >> 15;
    fr += fstep; i += step + (fr < fstep);
  }
  v->pos = i; v->frac = fr;
}
static void rinc_HOT rinc_mixAdpcm1(rinc_Voice *v, int n) {
  uint32_t i = v->pos, fr = v->frac, step = v->step, fstep = v->fstep, loop0 = v->loop0, loop1 = v->loop1;
  uint32_t end = loop1 > loop0 ? loop1 : v->frames, frames = v->frames, per = v->per, block = v->block;
  int32_t gl = v->gl, gr = v->gr, *out = rinc_sums; int f, looping = loop1 > loop0;
  uint32_t at = v->dec.at, k = v->dec.k; const uint8_t *blk = v->dec.blk; int32_t a = v->dec.a[0], b = v->dec.b[0], pred = v->dec.pred[0];
  int index = v->dec.index[0];
  for (f = 0; f < n; f++, out += 2) {
    int32_t x, bb; uint32_t j;
    if (i >= end) {
      if (!looping) { v->on = 0; break; }
      do i -= loop1 - loop0; while (i >= end);
    }
    if (at != i) {
      if (at > i) { /* (back: to the loop's start, or a block) */
        rinc_AdpcmState *d = &v->dec;
        if (looping && i >= loop0 && v->at0.at <= i) *d = v->at0; else d->at = 0xFFFFFFFFu;
        rinc_adpcmTo(v, d, i);
        at = d->at; k = d->k; blk = d->blk; a = d->a[0]; b = d->b[0]; pred = d->pred[0]; index = d->index[0];
      }
      while (at < i) { /* a frame on: decode the one after */
        at++; a = b;
        if (++k == per) { k = 0; blk += block; }
        if (at + 1 < frames) {
          if (!k) { pred = (int16_t)(blk[0] | blk[1] << 8); index = blk[2] > 88 ? 88 : blk[2]; }
          else {
            uint32_t c = (uint32_t)(blk[4 + (k - 1) / 2] >> ((k - 1) & 1) * 4 & 15), st = (uint32_t)rinc_steps[index];
            int32_t diff = (int32_t)(st >> 3);
            if (c & 1) diff += (int32_t)(st >> 2);
            if (c & 2) diff += (int32_t)(st >> 1);
            if (c & 4) diff += (int32_t)st;
            pred += c & 8 ? -diff : diff;
            pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
            index += rinc_moves[c & 7]; index = index < 0 ? 0 : index > 88 ? 88 : index;
          }
          b = pred;
        }
      }
    }
    j = i + 1 < end ? i + 1 : looping ? loop0 : i;
    bb = j == i + 1 ? b : j == i ? a : v->at0.a[0];
    x = a + ((bb - a) * (int32_t)(fr >> 17) >> 15);
    out[0] += x * gl >> 15; out[1] += x * gr >> 15;
    fr += fstep; i += step + (fr < fstep);
  }
  v->pos = i; v->frac = fr;
  v->dec.at = at; v->dec.k = k; v->dec.blk = blk; v->dec.a[0] = a; v->dec.b[0] = b; v->dec.pred[0] = pred; v->dec.index[0] = (uint8_t)index;
}
void rinc_HOT rinc_audioMix(int16_t *out, int frames) {
  while (frames > 0) {
    int n = frames < rinc_CHUNK ? frames : rinc_CHUNK, k, f;
    memset(rinc_sums, 0, (size_t)n * 2 * sizeof rinc_sums[0]);
    for (k = 0; k < rinc_VOICES; k++) {
      rinc_Voice *v = &rinc_voices[k];
      if (!v->on) continue;
      if (v->stereo) rinc_mixVoice(v, n);
      else if (v->bits == 4) rinc_mixAdpcm1(v, n);
      else if (v->bits == 16 && !((uintptr_t)v->data & 1)) rinc_mixPcm16(v, n);
      else rinc_mixVoice(v, n);
    }
    if (rinc_master != 32768) { int32_t m = rinc_master; for (f = 0; f < n * 2; f++) rinc_sums[f] = (int32_t)((int64_t)rinc_sums[f] * m >> 15); }
    for (f = 0; f < n * 2; f++) out[f] = (int16_t)(rinc_sums[f] < -32768 ? -32768 : rinc_sums[f] > 32767 ? 32767 : rinc_sums[f]);
    out += n * 2; frames -= n;
  }
}

/* the system's volume (a keyboard's volume knob, say; not the cart's to change): a step up or down,
   2.5 dB each, from full down to -40 dB; 0 turns mute on or off */
void rinc_audioVolume(int step) {
  if (!step) rinc_muted = !rinc_muted;
  else rinc_level = rinc_level + step < 0 ? 0 : rinc_level + step > rinc_VOLUME_STEPS ? rinc_VOLUME_STEPS : rinc_level + step;
  rinc_master = rinc_muted ? 0 : (int32_t)(32768.0f * powf(10.0f, -(float)(rinc_VOLUME_STEPS - rinc_level) * 2.5f / 20.0f) + 0.5f);
}
int rinc_audioLevel(void) { return rinc_muted ? -1 : rinc_level; }

void rinc_audioReset(void) { rinc_lock(1); memset(rinc_voices, 0, sizeof rinc_voices); rinc_lock(0); }

/* host procs. sound(voice, data, bits, channels, rate): play data (8 or 16 bits, or 4 for ADPCM in
   blocks of rinc_ADPCM_BLOCK bytes a channel; 1 or 2 channels) at rate frames a second, full volume,
   centered, once. A voice out of 0-7, or bad data, does nothing */
static rinc_Voice *rinc_voice(rio_Val *a) { return a[0].u < rinc_VOICES ? &rinc_voices[a[0].u] : 0; }
static void rinc_play(rinc_Voice *v, const uint8_t *p, uint32_t bytes, int bits, int channels, float rate, uint32_t block) {
  rinc_lock(1);
  v->data = p; v->bits = (uint8_t)bits; v->stereo = channels == 2;
  if (bits == 4) {
    v->block = block; v->per = (block / (uint32_t)channels - 4) * 2 + 1;
    v->frames = rinc_adpcmFrames(bytes, block, v->stereo);
    v->dec.at = 0xFFFFFFFFu;
  } else v->frames = bytes / (uint32_t)(bits / 8 * channels);
  v->pos = v->frac = 0; rinc_stepFor(v, rate); v->loop0 = v->loop1 = 0; v->vol = 1; v->pan = 0; rinc_gains(v);
  v->on = v->frames > 0;
  rinc_lock(0);
}
static void rinc_hostSound(rio_Vm *vm, rio_Val *a) {
  rinc_Voice *v = rinc_voice(a); const uint8_t *p = (const uint8_t *)rio_sliceBytes(vm, a + 1);
  if (!v || !p || (a[3].i != 4 && a[3].i != 8 && a[3].i != 16) || (a[4].i != 1 && a[4].i != 2)) return;
  rinc_play(v, p, a[2].u, a[3].i, a[4].i, a[5].f, rinc_ADPCM_BLOCK * a[4].u);
}
/* soundWav(voice, data): play a WAV file's sound (8- or 16-bit PCM, or IMA ADPCM; mono or stereo) at
   its own rate. Returns whether it could */
static uint32_t rinc_le(const uint8_t *p, int n) { return n == 2 ? (uint32_t)(p[0] | p[1] << 8) : (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void rinc_hostWav(rio_Vm *vm, rio_Val *a) {
  rinc_Voice *v = rinc_voice(a); const uint8_t *p = (const uint8_t *)rio_sliceBytes(vm, a + 1), *fmt = 0; uint32_t n = a[2].u, k = 12, size;
  int ok = 0;
  if (v && p && n >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WAVE", 4))
    for (; k + 8 <= n; k += 8 + size + (size & 1)) {
      size = rinc_le(p + k + 4, 4);
      if (!memcmp(p + k, "fmt ", 4) && size >= 16 && k + 8 + size <= n) fmt = p + k + 8;
      if (!memcmp(p + k, "data", 4) && fmt) {
        uint32_t tag = rinc_le(fmt, 2), ch = rinc_le(fmt + 2, 2), rate = rinc_le(fmt + 4, 4), align = rinc_le(fmt + 12, 2), bits = rinc_le(fmt + 14, 2);
        if (size > n - k - 8) size = n - k - 8; /* (a cut-short file: what's there) */
        if ((ch == 1 || ch == 2) && rate && ((tag == 1 && (bits == 8 || bits == 16)) || (tag == 0x11 && bits == 4 && align >= 4 * ch + ch * 4 && align % (4 * ch) == 0))) {
          if (tag == 1 && bits == 8) { /* (WAV's 8-bit is unsigned: Rinc's is signed, so not this) */
            break;
          }
          rinc_play(v, p + k + 8, size, (int)bits, (int)ch, (float)rate, align);
          ok = 1;
        }
        break;
      }
    }
  a[0].i = ok;
}
static void rinc_hostRate(rio_Vm *vm, rio_Val *a) { rinc_Voice *v = rinc_voice(a); (void)vm; if (v) { rinc_lock(1); rinc_stepFor(v, a[1].f); rinc_lock(0); } }
static void rinc_hostVolume(rio_Vm *vm, rio_Val *a) { rinc_Voice *v = rinc_voice(a); (void)vm; if (v) { rinc_lock(1); v->vol = rinc_clampf(a[1].f, 0, 1); rinc_gains(v); rinc_lock(0); } }
static void rinc_hostPan(rio_Vm *vm, rio_Val *a) { rinc_Voice *v = rinc_voice(a); (void)vm; if (v) { rinc_lock(1); v->pan = rinc_clampf(a[1].f, -1, 1); rinc_gains(v); rinc_lock(0); } }
/* soundLoop(voice, start, end): from end, go back to start (frames); end 0 or less: no loop */
static void rinc_hostLoop(rio_Vm *vm, rio_Val *a) {
  rinc_Voice *v = rinc_voice(a); (void)vm;
  if (!v) return;
  rinc_lock(1);
  v->loop0 = a[1].i > 0 ? a[1].u : 0; v->loop1 = a[2].i > 0 ? a[2].u : 0;
  if (v->loop1 > v->frames) v->loop1 = v->frames;
  if (v->loop0 >= v->loop1) v->loop0 = v->loop1 = 0;
  if (v->bits == 4 && v->loop1) { v->at0.at = 0xFFFFFFFFu; rinc_adpcmTo(v, &v->at0, v->loop0); } /* (the decoder at the loop's start) */
  rinc_lock(0);
}
static void rinc_hostStop(rio_Vm *vm, rio_Val *a) { rinc_Voice *v = rinc_voice(a); (void)vm; if (v) { rinc_lock(1); v->on = 0; rinc_lock(0); } }
static void rinc_hostPlaying(rio_Vm *vm, rio_Val *a) { rinc_Voice *v = rinc_voice(a); (void)vm; a[0].i = v && v->on; }

int rinc_audioProcs(rio_Vm *vm) {
  return rio_addFfi(vm, "sound", "ibiif", rinc_hostSound) || rio_addFfi(vm, "soundRate", "if", rinc_hostRate) || rio_addFfi(vm, "soundVolume", "if", rinc_hostVolume) ||
         rio_addFfi(vm, "soundPan", "if", rinc_hostPan) || rio_addFfi(vm, "soundLoop", "iii", rinc_hostLoop) || rio_addFfi(vm, "soundStop", "i", rinc_hostStop) ||
         rio_addFfi(vm, "soundPlaying", "i>i", rinc_hostPlaying) || rio_addFfi(vm, "soundWav", "ib>i", rinc_hostWav);
}

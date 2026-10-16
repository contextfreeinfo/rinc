"""Sound into IMA ADPCM, the 4-bit format Rinc's voices play: a quarter the size of 16-bit.

  python tools/adpcm.py in.wav out.wav       an ADPCM WAV file (for soundWav)
  python tools/adpcm.py in.wav out.rio       rio source: the sound as hex (for fromHex), and how to play it
  python tools/adpcm.py --tone out.rio       a test sound instead of a file: a plucked chord

in.wav is 16-bit PCM, mono or stereo, at any rate. Blocks are 256 bytes a channel (505 frames), as
Rinc's sound(voice, data, 4, channels, rate) expects. The coding is IMA's, as in WAV files (format 0x11).
"""
import math, struct, sys, wave

STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
         118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
         1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
         6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
         32767]
MOVES = [-1, -1, -1, -1, 2, 4, 6, 8]
BLOCK = 256  # bytes a channel
PER = (BLOCK - 4) * 2 + 1  # frames a block

def code_for(sample, pred, index):
  """the 4-bit code that gets closest to sample, and the decoder's state after it (exactly as Rinc decodes)"""
  step = STEPS[index]
  diff = sample - pred
  code = 8 if diff < 0 else 0
  diff = abs(diff)
  if diff >= step: code |= 4; diff -= step
  if diff >= step >> 1: code |= 2; diff -= step >> 1
  if diff >= step >> 2: code |= 1
  d = step >> 3
  if code & 1: d += step >> 2
  if code & 2: d += step >> 1
  if code & 4: d += step
  pred = max(-32768, min(32767, pred - d if code & 8 else pred + d))
  return code, pred, max(0, min(88, index + MOVES[code & 7]))

def encode(channels):
  """channels: lists of 16-bit samples, one list per channel (all as long). Returns the ADPCM bytes"""
  n, nch = len(channels[0]), len(channels)
  index = [0] * nch
  out = bytearray()
  for b0 in range(0, n, PER):
    count = min(PER, n - b0)
    if count < 2: break  # (a block of just its header sample: not worth it)
    if nch == 2: count = 1 + (count - 1) // 8 * 8  # (stereo blocks hold whole groups of 8)
    if count < 2: break
    pred = [channels[c][b0] for c in range(nch)]
    for c in range(nch): out += struct.pack('<hBB', pred[c], index[c], 0)
    codes = [[] for _ in range(nch)]
    for c in range(nch):
      for i in range(b0 + 1, b0 + count):
        code, pred[c], index[c] = code_for(channels[c][i], pred[c], index[c])
        codes[c].append(code)
    if nch == 1:
      cs = codes[0] + [0] * (len(codes[0]) & 1)
      out += bytes(cs[i] | cs[i + 1] << 4 for i in range(0, len(cs), 2))
    else:
      for g in range(0, count - 1, 8):
        for c in range(nch):
          cs = codes[c][g:g + 8]
          out += bytes(cs[i] | cs[i + 1] << 4 for i in range(0, 8, 2))
  return bytes(out)

def frames_in(nbytes, nch):
  block = BLOCK * nch
  r, last = nbytes % block, 0
  if r >= 4 * nch: last = 1 + (r - 4 * nch) // 8 * 8 if nch == 2 else 1 + (r - 4) * 2
  return nbytes // block * PER + last

def read_wav(path):
  with wave.open(path, 'rb') as w:
    if w.getsampwidth() != 2: sys.exit('adpcm.py: %s: 16-bit PCM only' % path)
    nch, rate, raw = w.getnchannels(), w.getframerate(), w.readframes(w.getnframes())
  pcm = struct.unpack('<%dh' % (len(raw) // 2), raw)
  return [list(pcm[c::nch]) for c in range(nch)], rate

def tone():
  """a plucked A minor chord, half a second at 22050 a second, mono (small enough to type into a cart)"""
  rate, n = 22050, 11025
  s = [0.0] * n
  for k, f in enumerate([220.0, 261.63, 329.63, 440.0]):
    start = k * 1102
    for i in range(start, n):
      t = (i - start) / rate
      s[i] += 0.22 * math.exp(-6 * t) * (math.sin(2 * math.pi * f * t) + 0.3 * math.sin(4 * math.pi * f * t))
  return [[max(-32768, min(32767, int(round(x * 32767)))) for x in s]], rate

def write_wav(path, data, nch, rate):
  block = BLOCK * nch
  fmt = struct.pack('<HHIIHHHH', 0x11, nch, rate, rate * block // PER, block, 4, 2, PER)
  fact = struct.pack('<I', frames_in(len(data), nch))
  body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'fact' + struct.pack('<I', 4) + fact + b'data' + struct.pack('<I', len(data)) + data + b'\0' * (len(data) & 1)
  open(path, 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)

def write_rio(path, data, nch, rate, name):
  """the bytes as hex, in statements of at most 1.5 KB of sound (3 KB of text: the board's REPL takes
  4 KB at a time), each filling the next part of the Blob"""
  out = ['# %s: %d frames of %s IMA ADPCM at %d a second (%d bytes; tools/adpcm.py made this)' % (
           name, frames_in(len(data), nch), 'stereo' if nch == 2 else 'mono', rate, len(data)),
         '%s: Blob[%d]' % (name, len(data)), '%s_n := 0' % name]
  for k in range(0, len(data), 1536):
    hexed = data[k:k + 1536].hex()
    lines = '\n'.join(hexed[i:i + 96] for i in range(0, len(hexed), 96))
    out.append('%s_n += fromHex(%s[%s_n:%s.len()], "\n%s\n")' % (name, name, name, name, lines))
  out.append('# play it: sound(0, %s, 4, %d, %d.0)' % (name, nch, rate))
  open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')

if __name__ == '__main__':
  args = [a for a in sys.argv[1:] if not a.startswith('--')]
  if '--tone' in sys.argv: (channels, rate), out = tone(), args[0]
  else: (channels, rate), out = read_wav(args[0]), args[1]
  data = encode(channels)
  if out.endswith('.wav'): write_wav(out, data, len(channels), rate)
  else: write_rio(out, data, len(channels), rate, 'sound_data')
  print('%s: %d frames -> %d bytes of ADPCM (16-bit: %d)' % (out, len(channels[0]), len(data), len(channels[0]) * 2 * len(channels)))

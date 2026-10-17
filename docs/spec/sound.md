# Sound

A Rinc mixes 8 voices, numbered 0 to 7, into 48,000 frames a second of 16-bit stereo. Each voice plays
sound data straight from the app's memory, reading it as it plays.

## The procs

Each proc takes a voice number first; a number outside 0 to 7 does nothing (and `soundPlaying` gives 0).

| proc | |
|---|---|
| `pc.sound(v, data, bits, channels, rate)` | play `data` from its start, once, at `rate` frames a second, full volume, centered, with no loop, in place of whatever `v` was playing; `bits` 16, 8 or 4 and `channels` 1 or 2, else nothing happens |
| `pc.soundWav(v, data) -> Int` | play a WAV file's sound data at the file's own rate, as `sound` would; 1 if it could, 0 if the file isn't one it plays |
| `pc.soundRate(v, rate)` | frames a second from now on, from 0 (paused) to 768,000 |
| `pc.soundVolume(v, volume)` | from 0 to 1 (outside: the nearest) |
| `pc.soundPan(v, pan)` | from -1 (left only) through 0 (both, full) to 1 (right only) |
| `pc.soundLoop(v, start, stop)` | on reaching frame `stop`, go on from frame `start`; `stop` past the end is the end; `stop` 0 or less, or `start` not before `stop`, ends any loop |
| `pc.soundStop(v)` | stops |
| `pc.soundPlaying(v) -> Int` | 1 while `v` plays, else 0 |

## Sound data

A frame is one sample for each channel; stereo samples come left first.

- **16 bits:** signed, little-endian.
- **8 bits:** signed, one byte each.
- **4 bits, IMA ADPCM,** laid out as in WAV files (format 0x11), in blocks of 256 bytes for each channel
  (256 mono, 512 stereo). A block starts with each channel's first sample (16 bits) and step index (a
  byte, then a byte of 0), then 4-bit codes: for mono, in byte order, the low half of a byte first; for
  stereo, 4 bytes (8 codes) of the left channel, then 4 of the right, and so on. A 256-byte mono block is
  505 frames.

`soundWav` plays WAV files with 16-bit PCM or IMA ADPCM (any block size), mono or stereo. It reads the
`fmt ` and `data` chunks, and plays what there is of a cut-short file. (WAV's 8-bit samples are unsigned,
so it doesn't take those.)

The number of frames is the data's length divided by the bytes a frame takes (rounded down), or for ADPCM
the frames its blocks hold.

## Mixing

A voice moves through its data at `rate / 48000` frames for each frame of output, and takes the value
between the two nearest frames in a straight line. A mono voice plays on both sides. Its volume and pan
scale each side: left by `volume * (1 - pan)` when `pan` is above 0, right by `volume * (1 + pan)` when
it's below 0, and each side by `volume` otherwise. The voices are added together, scaled by the Rinc's
own volume (where it has one), and cut off at the 16-bit range.

Changing a voice's data while it plays changes what's heard. A voice that reaches the end of its data
without a loop stops.

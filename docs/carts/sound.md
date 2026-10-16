# Sound

Rinc plays sound on 8 voices, numbered 0 to 7. A voice plays bytes from the cart's own memory, at
whatever rate it's told, so a cart can make sound by computing it, or carry recorded sound as data.

```rio
wave: Blob[2048]
for i in 0..<wave.len()
  wave[i] = (i / 16 % 2) * 60 - 30 # a square wave: 8-bit signed samples
end
pc.sound(0, wave[:], 8, 1, 22050)  # voice 0: 8 bits, 1 channel, 22050 samples a second
log(pc.soundPlaying(0))
```

```output
1
```

`pc.sound(voice, data, bits, channels, rate)` starts `data` playing on that voice, once, from the start,
at full volume and centered. `bits` is 16 or 8 (signed samples) or 4 (IMA ADPCM, below), and `channels`
1 or 2. While it plays:

- `pc.soundRate(v, rate)` changes its speed, and so its pitch.
- `pc.soundVolume(v, volume)` from 0 to 1, and `pc.soundPan(v, pan)` from -1 (left) to 1 (right).
- `pc.soundLoop(v, start, stop)` makes it go back to frame `start` each time it reaches frame `stop`,
  for music and engine hums (`stop` 0: no loop).
- `pc.soundStop(v)` stops it, and `pc.soundPlaying(v)` is 1 while it plays.

The voice reads the bytes as it plays, so they have to stay put: keep sound in globals.

## Recorded sound

IMA ADPCM keeps sound in a quarter of the space of 16-bit, and sounds close to it: a second of 22 kHz
mono is 11 KB. `tools/adpcm.py` turns a 16-bit WAV file into Rio source for a cart, which `pc.fromHex`
reads into a `Blob` (see `carts/chord.rio`). `pc.soundWav(voice, data)` plays a whole WAV file's bytes,
16-bit or ADPCM, at its own rate.

Next: [apps and the launcher](apps.md).

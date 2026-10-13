# Rim OS

An instant-on, PICO-8-flavored computer built on [rio](https://github.com/contextfreeinfo/rio), for
RP2350 boards (the Adafruit Fruit Jam first). Carts are rio programs: on the board they run in rio's
VM straight from source, so edit and run is instant, with simple loops compiled to machine code as
they load. The same carts run in a desktop window and in a web page.

**[Play the demo carts in a browser](https://contextfreeinfo.github.io/rinc/)** (built from `carts/` on
each push).

## Layout

```
lib/rio/           rio, as a git submodule
lib/Pico-PIO-USB/  USB host over PIO, for keyboards on the Fruit Jam
lib/fatfs/         FatFs, the SD card's filesystem (vendored)
src/core/          shared by every platform: the screen, palette, font and console, host procs, the REPL
src/rp2350/        the Fruit Jam: HDMI out (video.c), USB keyboards (keyboard.c) and controllers
                   (gamepad.c), sound (audio.c), the SD card (sd.c), the main loop
src/desktop/       a window (SDL3, built in) or headless
src/web/           wasm for a web page, and the page around it (page.html)
tools/             font.py (the font table), web.py (a cart as one HTML page, or just the wasm), send.py
                   (a cart to the board), embed.cmake (the web player, built into the desktop rim),
                   adpcm.py (sound into IMA ADPCM)
carts/             rio programs
```

## Carts

Carts draw into one 320x240 screen of 8-bit palette indexes, through host procs:

| proc | |
|---|---|
| `cls(c)` | fill the screen with color c |
| `pset(x, y, c)`, `pget(x, y) -> c` | one pixel |
| `rect(x, y, w, h, c)` | a filled rectangle |
| `print(s, x, y, c)` | text, in the 5x8 font, 6 pixels a character |
| `btn(i) -> Int` | 1 while button i is held: 0 left, 1 right, 2 up, 3 down (arrows), then the face buttons, most important first and named as on an Xbox controller: 4 A (V key), 5 B (C), 6 X (X), 7 Y (Z); then a controller's 8 LB, 9 RB, 10 and 11 the sticks pressed in, 12 Start, 13 Back |
| `key() -> Int` | the next character typed (32-126, 13 Enter, 8 Backspace, 27 Esc), or -1: for text. Held keys also press buttons, so a cart reads whichever suits it |
| `stick(i) -> Float` | a controller's sticks: 0 left x, 1 left y, 2 right x, 3 right y; -1 to 1, + right and down, 0 near the middle |
| `trigger(i) -> Float` | its triggers: 0 left, 1 right; 0 to 1 |
| `time() -> Float` | seconds since start |
| `apps() -> Int`, `appName(i, out) -> Int`, `appInfo(i, out) -> Int` | the apps on the SD card (sorted): how many, and app i's folder name or first line, into a Blob (how long) |
| `launch(name)`, `openConsole()` | switch to an app, or the console, after this frame |
| `fromHex(out, text) -> Int` | bytes from hex digits into a Blob (anything else in the text is skipped), until it's full: how many. For data typed into a cart |

The palette is the PICO-8 16 colors, then a 6x6x6 color cube, then 24 grays.

**Controllers.** An Xbox-style controller is player 1 alongside the keyboard: its d-pad and face
buttons are the same `btn` numbers as the keys (and its left stick pushed well over counts as the
arrows), and `stick` and `trigger` read it, or give 0 without one. On the desktop that's any controller
SDL knows (Xbox, PlayStation, Switch and more); on the web, any the browser maps to the standard layout;
on the Fruit Jam, wired Xbox 360 and Xbox One/Series controllers, recognized by the USB interfaces they
offer (`src/rp2350/gamepad.c`).

**Sound.** 8 voices, numbered 0 to 7, mixed to 48 kHz 16-bit stereo, out of HDMI and the 3.5 mm jack
on the board. A voice plays sound straight from the cart's memory, at any rate, with linear
interpolation:

| proc | |
|---|---|
| `sound(v, data, bits, channels, rate)` | play data on voice v, once, at rate frames a second: bits 16 or 8 (signed), or 4 for IMA ADPCM; 1 or 2 channels |
| `soundWav(v, data) -> Int` | play a WAV file's sound at its own rate (16-bit PCM or IMA ADPCM, mono or stereo): 1 if it could |
| `soundRate(v, rate)`, `soundVolume(v, 0..1)`, `soundPan(v, -1..1)` | change it while it plays |
| `soundLoop(v, start, end)` | from frame end, go back to start, over and over (end 0: no loop) |
| `soundStop(v)`, `soundPlaying(v) -> Int` | |

IMA ADPCM is a quarter the size of 16-bit and close to it in sound (a test chord: 38 dB signal to
noise), so it's the way to keep recorded sound in memory: a second of 22 kHz mono is 11 KB. It's laid
out as in WAV files (format 0x11), in blocks of 256 bytes a channel for `sound`. `tools/adpcm.py`
makes it from a 16-bit WAV, as a WAV file or as rio source for `fromHex` (`--tone` makes a test
chord): see `carts/chord.rio`. Mixing 8 ADPCM voices takes about 13% of a core on the board; 16-bit,
about 6%.

A game defines `update* :: proc()` and `draw* :: proc()` (both optional, marked `*` so Rim can find
them), as on PICO-8: Rim calls `update` then `draw` 60 times a second. The top-level code runs once
first. See `carts/breakout.rio`.

## The Fruit Jam

Rim boots to its REPL on the HDMI screen (640x480, each pixel shown as 2x2), typed on a USB keyboard
plugged into the board, and at the same time on the USB serial port:

```sh
python -m serial.tools.miniterm COM3 115200 --eol CR   # pip install pyserial; Ctrl+] quits
```

A keyboard's volume knob or keys set the system volume (both outputs, in 2.5 dB steps; mute too).

**The launcher.** The board starts in the launcher (`system/launcher.rio`, a cart built into the
firmware): the apps on the SD card, sorted, each with the title and description from its first line
(`# Bounce: the arrow keys tilt gravity...`), and the console (the REPL). Type to narrow the list, up
and down (or the d-pad) to choose, Enter (or A) to start. The Windows key, or a controller's Xbox
button, comes back to it from anywhere: it's the one system key. Esc and Ctrl+C belong to apps (`key()`
gives 27 for Esc). Over the USB serial port (not the keyboard), Ctrl-C stops what's running and drops
into the console with it still loaded, for tools such as `send.py`.

**Apps on the SD card.** A FAT32 card in the slot holds apps, each in its own folder named for it, its
source named the same way (as rio's modules are): `/rinc/apps/chord/chord.rio`. In the console:

```
ls              the apps on the card
run chord       load it into a fresh session, and start it
load chord      just load it (its procs and globals are there to use)
save chord      what's typed or pasted next, until Ctrl-D, becomes the app (Ctrl-C gives up)
delete chord    its source, and its folder if that's all there was
home            back to the launcher
```

`uv run tools/send.py carts/*.rio --save` puts carts on the card over USB serial; a card reader works
too.

The REPL edits the line being typed (left and right, Home and End or Ctrl-A and Ctrl-E, Backspace and
Delete; up and down go back through earlier lines), colors it (keywords, constants, comments),
and indents each next line of an unfinished input. A serial terminal gets the same, as terminal
escapes, which the screen's console understands too. `carts/combo.rio` tries everything at once:
sound, a controller and the keyboard, both as buttons and as typing.

Type or paste a cart, then `run` to start its game loop; Esc stops it (from the keyboard, or Esc or
Ctrl-C over serial). From a PC, `uv run tools/send.py carts/bounce.rio` sends a cart and starts it, in
a couple of seconds (`carts/bounce.rio` is the short one to try things with).

**Building.** Install the Pico SDK (2.x) and an ARM GCC; the Raspberry Pi Pico extension for VS Code
sets up both. Then:

```sh
cmake -S . -B build -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk   # or set PICO_SDK_PATH in the environment
cmake --build build --target flash   # picotool reboots the running board into BOOT mode, writes rim, starts it
```

The first time (or if the board isn't answering on USB), hold BOOT while resetting it and copy
`build/rim.uf2` to the drive that appears.

## The desktop

```sh
cmake -S . -B build-desktop -DRIM_PLATFORM=desktop   # fetches and builds SDL3 the first time
cmake --build build-desktop --config Release
build-desktop/rim carts/breakout.rio                      # play, in a window (Esc quits)
build-desktop/rim                                         # a REPL in the terminal
build-desktop/rim carts/breakout.rio -frames 120 -o s.ppm # no window: run 120 frames, save the screen
build-desktop/rim carts/breakout.rio -export breakout     # a game to hand out: rim with the cart in it
build-desktop/rim carts/breakout.rio -web breakout.html   # a page that plays it (see below)
```

The result is one executable: SDL3 and (with MSVC) the C runtime are linked in. `-DRIM_SDL=OFF` builds
it without a window. `ctest --test-dir build-desktop -C Release` runs every cart headless, and checks
that an exported game draws exactly what rim does.

`-export` writes a copy of rim with the cart after it, compiled (`breakout.exe` on Windows: give the
name you want). Started with no cart, it runs the one it carries, in a window, or headless with
`-frames`. With `-runner rim-linux-x86_64` it copies that rim instead, such as another system's from a
release, so one machine can make games for all three (on Linux and macOS, `chmod +x` a game made
elsewhere).

## The web

```sh
rim carts/breakout.rio -web breakout.html
```

makes one self-contained page: the cart compiled to an image, Rim's VM as wasm with no compiler in
it, and a small JS host (`src/web/page.html`). Breakout comes to 52 KB (24 KB gzipped): 33 KB of wasm
and a 2.5 KB image. Open it from disk or any server.

The desktop rim has the wasm built in when it's configured with `-DRIM_WEB_WASM=rim.wasm`, which the
CI builds and releases do. Making that wasm takes zig (`zig cc`, no emscripten):
`python tools/web.py --wasm=rim.wasm`. `python tools/web.py carts/breakout.rio breakout.html` makes
the same page directly, with zig. A cart whose code calls a host proc that compiles rio at runtime
would need the compiler in the wasm too (none exist yet): only web.py would do that.

## CI and releases

`.github/workflows/ci.yml` builds and tests on every push: the web player's wasm (with zig), the
desktop rim for Windows, macOS (universal) and Linux with that wasm built in (each runs the tests), and
`rim.uf2` for the Fruit Jam (Arm's toolchain and the Pico SDK). Each is kept as a download on the run.
A tag like `v0.1.0` also publishes them all as a GitHub release. Each night that `main` has changed, and whenever
the ci workflow is run by hand (Actions > ci > Run workflow), they're published as the rolling
[nightly](https://github.com/contextfreeinfo/rinc/releases/tag/nightly) prerelease.

## Working on rio at the same time

`RIO_DIR` picks the rio to build against. It defaults to the `lib/rio` submodule; point it at a working
copy to change both without committing in between (`tools/web.py` takes `--rio=path`):

```sh
cmake -S . -B build-desktop -DRIM_PLATFORM=desktop -DRIO_DIR=../rio
```

When a rio change is pushed, move the submodule to it: `git -C lib/rio pull` and commit `lib/rio` here.

## Memory on the RP2350

Of 520 KB of SRAM: apps get 256 KB, for their code and data both (rio compiles inside the same memory,
so nothing extra is needed for the compiler, and the code takes only its own size). While the REPL
runs, it keeps its compiler tables in the top 32 KB of that. The screen and the copy being shown take
150 KB, native loops 16 KB, rio's interpreter loop 10 KB (in SRAM, as it runs faster there), and USB,
the console and stacks most of the rest: about 32 KB is free, for audio next. No PSRAM needed.
Run `rio -m cart.rio` on the desktop to see where a cart's memory goes.

## License

CC0 1.0: dedicated to the public domain (see LICENSE). Code from others keeps its own license:
see THIRD_PARTY.md.

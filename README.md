# Rim OS

An instant-on, PICO-8-flavored computer built on [rio](https://github.com/contextfreeinfo/rio), for
RP2350 boards (the Adafruit Fruit Jam first). Carts are rio programs: on the board they run in rio's
VM straight from source, so edit and run is instant, with simple loops compiled to machine code as
they load. The same carts run in a desktop window and in a web page.

## Layout

```
lib/rio/           rio, as a git submodule
lib/Pico-PIO-USB/  USB host over PIO, for keyboards on the Fruit Jam
src/core/          shared by every platform: the screen, palette, font and console, host procs, the REPL
src/rp2350/        the Fruit Jam: HDMI out (video.c), USB keyboards (keyboard.c), the main loop
src/desktop/       a window (SDL3, built in) or headless
src/web/           wasm for a web page, and the page around it (page.html)
tools/             font.py (the font table), web.py (a cart as one HTML page, or just the wasm), send.py
                   (a cart to the board), embed.cmake (the web player, built into the desktop rim)
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
| `btn(i) -> Int` | 1 while button i is held: 0 left, 1 right, 2 up, 3 down (arrows), 4 O (Z or C), 5 X (X or V) |
| `time() -> Float` | seconds since start |

The palette is the PICO-8 16 colors, then a 6x6x6 color cube, then 24 grays.

A game defines `update* :: proc()` and `draw* :: proc()` (both optional, marked `*` so Rim can find
them), as on PICO-8: Rim calls `update` then `draw` 60 times a second. The top-level code runs once
first. See `carts/breakout.rio`.

## The Fruit Jam

Rim boots to its REPL on the HDMI screen (640x480, each pixel shown as 2x2), typed on a USB keyboard
plugged into the board, and at the same time on the USB serial port:

```sh
python -m serial.tools.miniterm COM3 115200 --eol CR   # pip install pyserial; Ctrl+] quits
```

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
`-frames`. Exports are as portable as the rim that made them: Windows, macOS or Linux.

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
A tag like `v0.1.0` also publishes them all as a GitHub release.

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

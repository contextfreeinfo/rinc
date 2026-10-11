# Rim OS

An instant-on, PICO-8-flavored computer built on [rio](https://github.com/contextfreeinfo/rio), for
RP2350 boards (the Adafruit Fruit Jam first). Carts are rio programs: on the board they run in rio's
VM straight from source, so edit and run is instant; the same carts can also become native and web
games through rio's C output.

## Layout

```
lib/rio/           rio, as a git submodule
src/core/          shared by every platform: the screen, palette, host procs for carts, the REPL
src/rp2350/        the Fruit Jam
src/desktop/       native builds (a window comes later)
carts/             rio programs
```

Carts draw into one 320x240 screen of 8-bit palette indexes, through host procs:
`cls(c)`, `pset(x, y, c)`, `pget(x, y) -> c`, `rect(x, y, w, h, c)` and `time() -> Float`.
The palette is the PICO-8 16 colors, then a 6x6x6 color cube, then 24 grays.

## Building

**Fruit Jam.** Install the Pico SDK (2.x) and an ARM GCC; the Raspberry Pi Pico extension for VS Code
sets up both. Then:

```sh
cmake -S . -B build -DPICO_SDK_PATH=/path/to/pico-sdk   # or set PICO_SDK_PATH in the environment
cmake --build build
```

Hold BOOT while plugging in (or resetting) the board, copy `build/rim.uf2` to the drive that appears,
and open its USB serial port in a terminal for the rio REPL.

**Desktop.**

```sh
cmake -S . -B build-desktop -DRIM_PLATFORM=desktop
cmake --build build-desktop
build-desktop/rim                                   # REPL
build-desktop/rim carts/hello.rio -o screen.ppm     # run a cart, save the screen
```

## Working on rio at the same time

`RIO_DIR` picks the rio to build against. It defaults to the `lib/rio` submodule; point it at a working
copy to change both without committing in between:

```sh
cmake -S . -B build-desktop -DRIM_PLATFORM=desktop -DRIO_DIR=../rio
```

When a rio change is pushed, move the submodule to it: `git -C lib/rio pull` and commit `lib/rio` here.

## Memory on the RP2350

Of 520 KB of SRAM: apps get 256 KB, for their code and data both (rio compiles inside the same
memory, so nothing extra is needed for the compiler, and the code takes only its own size), the
screen and its DMA copy for video take 150 KB, and the rest is for the system. No PSRAM needed.
Run `rio -m cart.rio` on the desktop to see where a cart's memory goes.

## License

MIT

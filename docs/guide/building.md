# Building

This guide is about the Rinc in this repository: the Fruit Jam firmware, the desktop program and the
web player, all from one C core. What any Rinc provides is in [the platform](../spec/platform.md).

```sh
git clone --recursive https://github.com/rincpc/rinc     # (or: git submodule update --init)
```

- **The Fruit Jam:** the Pico SDK and an ARM GCC, then
  `cmake -S . -B build -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk && cmake --build build`. See
  [the Fruit Jam](fruit-jam.md).
- **The desktop:** `cmake -S . -B build-desktop -DRINC_PLATFORM=desktop && cmake --build build-desktop
  --config Release`. See [the desktop](desktop.md).
- **The web:** `python tools/web.py --wasm=rinc.wasm --wasm-full=rinc-full.wasm`, with zig. See [the web](web.md).

Prebuilt desktop programs, web player and firmware are in the
[nightly release](https://github.com/rincpc/rinc/releases/tag/nightly).

## Source layout

```
lib/rio/           Rio, as a git submodule
lib/Pico-PIO-USB/  USB host over PIO, for keyboards on the Fruit Jam
lib/fatfs/         FatFs, the SD card's filesystem (vendored)
src/core/          shared by every platform: the screen, palette, font and console, host procs
                   (rinc.c, audio.c), bundles, the terminal REPL, and pc.rio (the host procs,
                   written out in Rio)
src/rp2350/        the Fruit Jam: HDMI out (video.c), USB keyboards (keyboard.c) and controllers
                   (gamepad.c), sound (audio.c), the SD card (sd.c), the main loop
src/desktop/       a window (SDL3, built in) or headless
src/web/           wasm for a web page, and the page around it (page.html)
system/            the built-in apps, ordinary apps: home.rio and console.rio
apps/              demo apps
tests/             pc.c: pc.rio matches what Rinc registers
tools/             font.py (the font table), web.py (the web players' wasm), send.py (an app to
                   the board), embed.cmake (the web players, built into the desktop rinc),
                   bundle.cmake (the firmware's apps), adpcm.py (sound into IMA ADPCM)
docs/              these docs: SUMMARY.md sets the order; mdbook builds them as a site
```

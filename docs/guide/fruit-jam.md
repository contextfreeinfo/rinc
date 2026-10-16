# The Fruit Jam

Rinc runs on the Adafruit Fruit Jam, an RP2350 board with HDMI out, USB host ports, an SD card slot and
a headphone jack.

## Building and flashing

Install the Pico SDK (2.x) and an ARM GCC; the Raspberry Pi Pico extension for VS Code sets up both.

```sh
cmake -S . -B build -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk   # or set PICO_SDK_PATH in the environment
cmake --build build --target flash   # picotool reboots the running board into BOOT mode, writes rinc, starts it
```

The first time (or if the board isn't answering on USB), hold BOOT while resetting it and copy
`build/rinc.uf2` to the drive that appears.

## Using it

Rinc starts in the launcher on the HDMI screen (640x480, each pixel 2x2): the apps on the SD card,
sorted, and the console. Type to narrow the list, up and down (or the d-pad) to choose, Enter (or A) to
start. The Windows key, or a controller's Xbox button, comes back to the launcher from anywhere. Esc and
Ctrl+C belong to apps. A keyboard's volume knob or keys set the volume (both outputs, in 2.5 dB steps,
and mute).

Keyboards plug into the USB host ports. Controllers there can be wired Xbox 360 and Xbox One/Series
ones, recognized by the USB interfaces they offer (`src/rp2350/gamepad.c`).

The console is also on the USB serial port, alongside the keyboard:

```sh
python -m serial.tools.miniterm COM3 115200 --eol CR   # pip install pyserial; Ctrl+] quits
```

Over serial (not the keyboard), Ctrl-C stops what's running and drops into the console with it still
loaded, for tools such as `send.py`.

## Apps on the SD card

A FAT32 card holds apps as `/rinc/apps/name/name.rio`, managed from the console with `ls`, `run`,
`load`, `save`, `delete` and `home` (see [the console](../spec/apps.md#the-console)). From a PC,
`uv run tools/send.py carts/*.rio --save` puts carts on the card over USB serial, and
`uv run tools/send.py carts/bounce.rio` sends one and starts it, in a couple of seconds. A card reader
works too.

The console edits the line being typed (left and right, Home and End or Ctrl-A and Ctrl-E, Backspace and
Delete; up and down go back through earlier lines), colors it (keywords, constants, comments), and
indents each next line of an unfinished input. A serial terminal gets the same, as terminal escapes, which
the screen's console understands too. `carts/combo.rio` tries everything at once: sound, a controller and
the keyboard, both as buttons and as typing.

## Memory

Of the RP2350's 520 KB of SRAM:

| | |
|---|---|
| the app space | 256 KB: Rio's memory, for a cart's code and data |
| the screen | 150 KB: the one drawn into, and the copy being shown |
| native loops | 16 KB: Rio compiles a cart's simple loops to Thumb-2 code here |
| Rio's interpreter loop | 10 KB: it runs from SRAM, as that's faster than flash |
| USB, the console, sound and stacks | most of the rest; about 32 KB is free |

No PSRAM is needed. For now the console's REPL keeps its compiler tables in the top 32 KB of the app
space, and apps run in a console session (see [memory](../spec/memory.md#status)).

Sound goes out over HDMI and the 3.5 mm jack at once, mixed on the second core along with the video.
Mixing 8 ADPCM voices takes about 13% of a core; 16-bit, about 6%.

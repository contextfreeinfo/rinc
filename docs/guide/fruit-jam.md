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

Rinc starts at home on the HDMI screen (640x480, each pixel 2x2): the apps on the SD card, sorted, and
the console. Type to narrow the list, up and down (or the d-pad) to choose, Enter (or A) to start. The
system key comes back home from anywhere: the Windows key, a controller's Xbox button, or Shift tapped
and then held. Esc and Ctrl+C belong to apps. A keyboard's volume knob or keys set the volume (both
outputs, in 2.5 dB steps, and mute).

An app that isn't a game, or one that stops with an error, leaves its log and the error on the screen,
until the system key goes home. One that runs out of memory shows how near each limit it came too.

Keyboards plug into the USB host ports. Controllers there can be wired Xbox 360 and Xbox One/Series
ones, recognized by the USB interfaces they offer (`src/rp2350/gamepad.c`).

What the console shows also goes out the USB serial port, a line at a time, and what's typed there
goes to the console too:

```sh
python -m serial.tools.miniterm COM3 115200 --eol CR   # pip install pyserial; Ctrl+] quits
```

Over serial (not the keyboard), Ctrl-C opens the console in place of whatever's running, for tools such
as `send.py`. A serial terminal sees each line once it's entered, not as it's typed.

## Apps on the SD card

A FAT32 card holds apps as `/rinc/apps/name/name.rio`, managed from the console with `ls`, `run`,
`load`, `save`, `delete` and `home` (see [the console](../spec/apps.md#the-console)). From a PC,
`uv run tools/send.py apps/*.rio --save` puts apps on the card over USB serial, and
`uv run tools/send.py apps/bounce.rio` sends one and starts it, in a couple of seconds. A card reader
works too.

The console edits the line being typed (left and right, Home and End or Ctrl-A and Ctrl-E, Backspace and
Delete; up and down go back through earlier lines), colors it (keywords, constants, comments), and
indents each next line of an unfinished input. A serial terminal gets the same, as terminal escapes, which
the screen's console understands too. `apps/combo.rio` tries everything at once: sound, a controller and
the keyboard, both as buttons and as typing.

## Memory

Of the RP2350's 520 KB of SRAM:

| | |
|---|---|
| the app space | 256 KB: Rio's memory, for an app's code and data (and its source, read from the card, while it compiles) |
| the screen | 150 KB: the one drawn into, and the copy being shown |
| native loops | 16 KB: Rio compiles an app's simple loops to Thumb-2 code here |
| Rio's interpreter loop | 10 KB: it runs from SRAM, as that's faster than flash |
| USB, the console, sound and stacks | most of the rest; about 32 KB is free |

No PSRAM is needed. Every app, home and the console included, gets the whole app space (see
[memory](../spec/memory.md)); the console's own Rio session lives in the console's memory.

Sound goes out over HDMI and the 3.5 mm jack at once, mixed on the second core along with the video.
Mixing 8 ADPCM voices takes about 13% of a core; 16-bit, about 6%.

# Rinc OS

An instant-on, PICO-8-flavored computer for making and playing little games, built on
[Rio](https://github.com/contextfreeinfo/rio). It runs on RP2350 boards (the Adafruit Fruit Jam
first), where it boots to a launcher and a console in an instant, and the same carts run on the desktop
and in the browser.

**[Play the demo carts](https://rincpc.com/)** in a browser, or read **[the docs](https://rincpc.com/docs/)**.

```rio
x := 0
update* :: proc()
  x += pc.btn(1) - pc.btn(0)       # the arrow keys move it
end
draw* :: proc()
  pc.cls(1)
  pc.rect(150 + x, 110, 20, 20, 8)
  pc.print("HELLO", 10, 10, 7)
end
```

A cart is a Rio program: its top-level code runs once, then Rinc calls `update` and `draw` 60 times a
second. It draws on a 320x240 screen of 256 colors, plays sound on 8 voices, and reads the keyboard and
a controller, all through the `pc` module. On the board, carts run straight from source, with simple
loops compiled to machine code as they load.

## Docs

- [Making carts](docs/carts/README.md): drawing, input, sound and apps, by example.
- [The Rinc platform](docs/spec/README.md): exactly what any Rinc provides: the screen, input, sound,
  memory, and apps.
- [The guide](docs/guide/README.md): building and using this Rinc on the Fruit Jam, the desktop and the
  web.

## Try it

```sh
git clone --recursive https://github.com/rincpc/rinc && cd rinc
cmake -S . -B build-desktop -DRINC_PLATFORM=desktop && cmake --build build-desktop --config Release
build-desktop/rinc carts/breakout.rio        # (build-desktop/Release/rinc.exe with MSVC)
```

Or get the desktop program or the Fruit Jam's firmware from the
[nightly release](https://github.com/rincpc/rinc/releases/tag/nightly).

## License

CC0 1.0: dedicated to the public domain (see LICENSE). Code from others keeps its own license: see
THIRD_PARTY.md.

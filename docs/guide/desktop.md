# The desktop

```sh
cmake -S . -B build-desktop -DRINC_PLATFORM=desktop   # fetches and builds SDL3 the first time
cmake --build build-desktop --config Release
```

The result is one program, with SDL3 (and, with MSVC, the C runtime) linked in. `-DRINC_SDL=OFF` builds
it without a window.

```
rinc cart.rio                      play, in a window (Esc quits)
rinc                               a REPL in the terminal
rinc cart.rio -frames 120          no window: run 120 frames (A held for the first 3)
  -o screen.ppm                    then save the screen
  -wav sound.wav                   and the sound those frames made
rinc cart.rio -i cart.img          compile to an image, and list the host procs its code calls
rinc cart.rio -export game         a game to hand out: rinc with the cart in it
  -runner rinc-linux-x86_64        a copy of that rinc instead, such as another system's from a release
rinc cart.rio -web game.html       a page that plays it (see the web)
```

In a window, the arrows and V, C, X and Z are the buttons, and any controller SDL knows is player 1
(Xbox, PlayStation, Switch and more). `ctest --test-dir build-desktop -C Release` runs every cart headless,
checks that an exported game draws exactly what rinc does, checks `pc.rio`, and runs the examples in these
docs.

## Exported games

`-export` writes a copy of rinc (`game.exe` on Windows: give the name you want) with the cart after it:
the cart compiled to an image, then its size (4 bytes, little-endian) and `RINCAPP0`. Started without a
cart, rinc looks for that at its own end, and runs the cart it carries, in a window, or headless with
`-frames`. With `-runner`, one machine can make games for all three systems (on Linux and macOS,
`chmod +x` a game made elsewhere).

# The desktop

```sh
cmake -S . -B build-desktop -DRINC_PLATFORM=desktop   # fetches and builds SDL3 the first time
cmake --build build-desktop --config Release
```

The result is one program, with SDL3 (and, with MSVC, the C runtime) linked in. `-DRINC_SDL=OFF` builds
it without a window.

```
rinc app.rio                      play, in a window (closing it quits)
rinc -app home                    a built-in app: home or console
rinc                              a REPL in the terminal
rinc app.rio -m                   how near the app comes to its limits (see memory)
rinc app.rio -frames 120          no window: run 120 frames (A held for the first 3)
  -o screen.ppm                   then save the screen
  -wav sound.wav                  and the sound those frames made
  -type 'x := 1\nlog(x)\n'        and type this, as fast as the app takes it (\n Enter, \e Esc, \t Tab)
rinc app.rio -i app.img           compile to an image, and list the host procs its code calls
rinc app.rio -export game         a game to hand out: rinc with the app in it
  -runner rinc-linux-x86_64       a copy of that rinc instead, such as another system's from a release
rinc app.rio -web game.html       a page that plays it (see the web)
```

In a window, the arrows and V, C, X and Z are the buttons, any controller SDL knows is player 1 (Xbox,
PlayStation, Switch and more), and Esc is the app's. The system key is Shift tapped and then held (the
operating system keeps the Windows key), or a controller's guide button: it goes to the built-in home.
The desktop has no apps on storage yet, so `pc.launch` reaches only `home` and `console`.

`ctest --test-dir build-desktop -C Release` runs every app headless, checks that an exported game draws
exactly what rinc does, runs home and the console (typing at it), checks `pc.rio`, and runs the examples
in these docs.

## Exported games

`-export` writes a copy of rinc (`game.exe` on Windows: give the name you want) with the app after it:
the app compiled to an image, then its size (4 bytes, little-endian) and `RINCAPP0`. Started without an
app, rinc looks for that at its own end, and runs the app it carries, in a window, or headless with
`-frames`. With `-runner`, one machine can make games for all three systems (on Linux and macOS,
`chmod +x` a game made elsewhere).

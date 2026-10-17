# The desktop

```sh
cmake -S . -B build-desktop -DRINC_PLATFORM=desktop   # fetches and builds SDL3 the first time
cmake --build build-desktop --config Release
```

The result is one program, with SDL3 (and, with MSVC, the C runtime) linked in. `-DRINC_SDL=OFF` builds
it without a window. A release is that one program, zipped on Windows and a `.tar.gz` on macOS and Linux (x86-64, and
arm64 for a Raspberry Pi 4 or 5 with 64-bit Raspberry Pi OS), so `rinc -help` and `rinc -license`
carry what would otherwise be files beside it.

```
rinc app.rio                      play, in a window (closing it quits)
rinc                              home, if it's installed; else a REPL in the terminal
rinc -app chord                   an app from storage, by name
rinc -install home console        standard apps into storage, fetched with curl (or app.rio files, copied)
  -data folder                    with any of these: storage in this folder instead
rinc app.rio -m                   how near the app comes to its limits (see memory)
rinc app.rio -frames 120          no window: run 120 frames (A held for the first 3)
  -o screen.ppm                   then save the screen
  -wav sound.wav                  and the sound those frames made
  -type 'x := 1\nlog(x)\n'        and type this, as fast as the app takes it (\n Enter, \e Esc, \t Tab)
rinc app.rio -i app.img           compile to an image, and list the host procs its code calls
rinc a.rio b.rio -export game     a program to hand out (game.exe on Windows): rinc with the apps in it
  -home a                         the one it starts at (by default the only one, or one named home)
  -name folder                    its storage folder's name (by default its home app's)
  -runner path/to/rinc            a copy of that rinc instead, such as another system's from a release
rinc a.rio b.rio -web game.html   a page that plays them (see the web)
rinc -help                        all this; and rinc -license, the licenses of what's in it
```

In a window, the arrows and V, C, X and Z are the buttons, any controller SDL knows is player 1 (Xbox,
PlayStation, Switch and more), and Esc is the app's. The system key is Shift tapped and then held (the
operating system keeps the Windows key), or a controller's guide button; with Esc (or Back) it goes home.

## Storage

The desktop rinc carries no apps of its own, so the program stays small. Its storage is a folder in your
user data folder, laid out as on the Fruit Jam's SD card (`apps/name/name.rio`, and each app's
`save/name`):

| | |
|---|---|
| Windows | `%APPDATA%inc` |
| macOS | `~/Library/Application Support/rinc` |
| Linux and others | `$XDG_DATA_HOME/rinc`, or `~/.local/share/rinc` |

`rinc -install home console` gets the same setup as the Fruit Jam: it fetches the standard apps from the
Rinc repository with `curl`, which needs to be installed (Windows 10 and later, macOS and most Linux
systems have it). Rinc itself never goes on the network. `rinc -install mygame.rio` copies an app of
your own in. Apps only see their Rinc's folder, never the rest of the computer.

`ctest --test-dir build-desktop -C Release` runs every app headless, checks that an exported game draws
exactly what rinc does, runs home and the console (typing at it), checks `pc.rio`, and runs the examples
in these docs.

## Exported games

`-export` writes a copy of rinc with a bundle of the apps after it (see
[bundles](../spec/apps.md#bundles)): each app compiled to an image. Started without an app, rinc looks
for a bundle at its own end, and starts its home app, in a window, or headless with `-frames`. An
exported game keeps its save files in a storage folder of its own name (`-name`), not Rinc's. With
`-runner`, one machine can make games for all three systems (on Linux and macOS, `chmod +x` a game made
elsewhere).

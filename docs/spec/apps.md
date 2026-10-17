# Apps and storage

Every app is one Rio file, run as [running apps](running.md) says, with the whole app space to
itself. A Rinc finds apps in two places: its **storage** (the Fruit Jam's SD card, a folder on the
desktop, the browser's localStorage), and its **bundle**, the apps it carries with it. Storage comes first, so an app there takes
the place of a bundled one of the same name.

## Storage

A Rinc's storage is a folder of its own, `/rinc`, and apps can't reach outside it:

```
/rinc/apps/chord/chord.rio    an app: a folder of its name, holding its Rio file of the same name
/rinc/save/chord/...          that app's own files (pc.saveRead and so on)
```

An app's name is 1 to 32 letters, digits, `_` and `-`. If its first line is a comment, that's its
description, by convention a title, a colon, and what it does:

```rio fragment
# Chord: an ADPCM chord on 8 voices, with pitch and pan
```

## Bundles

A bundle is the files a Rinc carries with it, laid out as in storage: an app is `apps/<name>/<name>.rio`,
or, compiled for a Rinc without a compiler, `apps/<name>/<name>.img` (a Rio image for a 256 KB app
space) with its description in `apps/<name>/<name>.info`. A bundle also names its **home app**, the one
`home` means, and its **storage folder**, where its apps' save spaces go.

- **The Fruit Jam's firmware** bundles home, the console and the editor by default, home as home.
- **The desktop and the web** carry none, so their programs stay small.
- **An exported game** carries its own apps, starting at its home app. One app on its own is home.
  It carries them compiled, unless one of them runs Rio itself or writes apps (the console, the
  editor): then their sources, so they can be read and changed too.

`home` means an app named `home` in storage if there is one, else the bundle's home app. An app on
storage of the same name as a bundled one takes its place.

Every Rinc reads the same format, all numbers little-endian:

| bytes | |
|---|---|
| 8 | `RINCBUN1` |
| 4 | how many files |
| 32 | the storage folder's name, 0-padded (empty: Rinc's own) |
| 32 | the home app's name, 0-padded |
| 104 each | each file's entry: its path (96 bytes, 0-padded, at most 95 long), where its bytes start (4, from the bundle's start) and how many (4) |
| | the files' bytes |

A desktop program carries its bundle at its end, followed by the bundle's size (4 bytes) and `RINCBUN1`
again. A web page carries it in base64.

## Finding apps

| proc | |
|---|---|
| `pc.apps() -> Int` | looks again, in storage and in the bundle, and says how many apps there are (the first 64, by name; `home` aside) |
| `pc.appName(i, out) -> Int` | app `i`'s name, from 0, in order of name ignoring case |
| `pc.appInfo(i, out) -> Int` | app `i`'s description: its first line after `#` and any spaces, if it starts with `#` |
| `pc.appSource(name, out) -> Int` | app `name`'s source (from storage, else the bundle), as much as fits in `out`: its whole length, or -1 if there's none |
| `pc.appSave(name, text) -> Int` | `text` becomes app `name`'s source in storage (its folder too, if it's new): 0, or -1 |
| `pc.appDelete(name) -> Int` | removes app `name`'s source from storage, and its folder if that's all it held: 0, or -1 |
| `pc.launch(name)` | after this frame, leave this app and run app `name` |

`appName`, `appInfo` and `appSource` copy as much as fits, and give the whole length (0 for an `i` that
isn't an app), so an app can tell when its buffer was too small.

## An app's own files

Every app has a save space of its own, `/rinc/save/<app>`, for high scores, settings and the like. A
file's name is 1 to 32 letters, digits, `_`, `-` and `.`, not starting with a dot:

| proc | |
|---|---|
| `pc.saveRead(file, out) -> Int` | the file's contents, as much as fits in `out`: its whole length, or -1 if there's no such file |
| `pc.saveWrite(file, data) -> Int` | `data` becomes the file's contents: 0, or -1 |
| `pc.saveDelete(file) -> Int` | removes it: 0, or -1 |

```rio
log(pc.saveWrite("best.txt", "42"))
out: Blob[16]
n := pc.saveRead("best.txt", out[:])
log(n, out[:n].asString(), pc.saveRead("nope.txt", out[:]))
log(pc.saveDelete("best.txt"), pc.saveRead("best.txt", out[:]))
```

```output
0
2 42 -1
0 -1
```

Where a Rinc has no storage, an app can't save: these give -1.

## Home

A Rinc with apps starts at `home` (on the Fruit Jam, `system/home.rio`), which lists the apps in order of
title, and starts what's picked with `pc.launch`.

## The system key

One key belongs to the Rinc, not to apps: the **system key**. Held, it makes combinations for the Rinc
itself; on its own it does nothing, so it isn't pressed by accident. While it's held, what's typed isn't
the app's. It's:

- the Windows key, on a keyboard that has one and a Rinc that gets it (the Fruit Jam);
- a controller's guide button (the Xbox button);
- or, anywhere, Shift tapped twice, within 0.4 seconds: the system key while the second press is held,
  and for the next key within 0.7 seconds after it's let go (so it works where Shift+Esc belongs to a
  browser). A Shift held while typing other keys doesn't count, so capitals are fine.

| combination | |
|---|---|
| system + Esc (or a controller's Back) | home, from any app |
| system + Tab (or a controller's RB) | the switcher: the recent apps, the one before this picked |
| system + Shift+Tab (or LB) | the switcher, from the other end |
| system + Q | back to the app before, this one left out of the recent apps |
| system + P | the performance strip, on or off |

While the system key is held, a strip across the bottom of the screen shows these, apart from the app's
own screen (which it doesn't change), and the app gets none of the keys or buttons that make them. In
the switcher, Tab and Shift+Tab (RB and LB) move along the recent apps;
letting go of the system key, or Enter, switches to the one picked, and Esc stays. (With Shift tapped
twice and let go, the switcher stays up until Enter, or switches after a second untouched.)

The performance strip shows how long each frame's `update*` and `draw*` take, against the 1/60 s a
frame has: the latest, and a bar for each of the last few seconds' frames, 8 pixels high for the whole
1/60 s (green under half of it, yellow up to all of it, red over).

## The console

The console (`system/console.rio`) is an ordinary app: Rio, typed and run in a session of its own (see
[Rio inside an app](rio.md)). A line on its own can be a command instead:

| command | |
|---|---|
| `run` | run what's been typed as a game, until Esc |
| `ls` | list the apps |
| `load name` | app `name`'s source, into a fresh session |
| `run name` | run app `name`, as itself |
| `save name` | what's typed or pasted next, until Ctrl-D, becomes app `name`'s source (Ctrl-C gives up) |
| `delete name` | remove app `name` |
| `home` | back home |

## The editor

The editor (`system/edit.rio`) is an ordinary app too: one app's source at a time, up to 32 KB, with
the keys of most editors. It starts with the app it had last, where it was.

| keys | |
|---|---|
| arrows, Home, End, Page Up, Page Down | move; with Ctrl, by words and to either end; with Shift, select |
| Ctrl+O, Ctrl+S, Ctrl+R, Ctrl+N | open an app (from a list, or a new name), save it, save and run it, start a new one |
| Ctrl+Z, Ctrl+Y (or Ctrl+Shift+Z) | undo, redo |
| Ctrl+X, Ctrl+C, Ctrl+V | cut, copy, paste (the line, with nothing selected) |
| Ctrl+A, Ctrl+L | select all, select the line (and the next, again) |
| Tab, Shift+Tab | indent, outdent (the selected lines) |
| Alt+Up, Alt+Down, Ctrl+Shift+K | move the lines, delete them |
| Ctrl+Backspace, Ctrl+Delete | delete a word |
| Ctrl+F, Ctrl+G | find (Enter: the next), go to a line |

Enter keeps the line's indent, and a step more after a line that opens a block (`if`, `for`, a proc and
so on); `end`, `else` and `case` step back out as they're typed.

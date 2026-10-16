# Apps and storage

A Rinc with storage keeps apps there. Without storage, the app procs find no apps, and `pc.launch`
does nothing a cart can rely on.

## Apps

An app is a folder in `/rinc/apps` holding a cart of the folder's name: `/rinc/apps/chord/chord.rio`. Its
name is 1 to 32 letters, digits, `_` and `-`. If its first line is a comment, that's its description,
by convention a title, a colon, and what it does:

```rio fragment
# Chord: an ADPCM chord on 8 voices, with pitch and pan
```

| proc | |
|---|---|
| `pc.apps() -> Int` | looks at storage again, and says how many apps there are (the first 64, by name) |
| `pc.appName(i, out) -> Int` | app `i`'s name, from 0, in order of name ignoring case |
| `pc.appInfo(i, out) -> Int` | app `i`'s description: its first line after `#` and any spaces, if it starts with `#` |
| `pc.launch(name)` | after this frame, leave this cart and run app `name` |
| `pc.openConsole()` | after this frame, leave this cart for the console |

`appName` and `appInfo` copy into `out` as much as fits, and give the whole length (0 for an `i` that
isn't an app), so a cart can tell when its buffer was too small.

## The launcher

A Rinc with apps starts in its launcher, which is a cart (`system/launcher.rio`, built into the
firmware): it lists the apps and the console, and starts what's picked with `pc.launch` or
`pc.openConsole`. One key (the Fruit Jam's system key: the Windows key, or a controller's Xbox button)
always goes back to the launcher, from any app or the console, and isn't given to carts.

## The console

The console is a Rio REPL for typing Rio, with a few commands, each on a line of its own:

| command | |
|---|---|
| `run` | run what's been typed as a cart: its frames, until it's left |
| `ls` | list the apps |
| `run name`, `load name` | load app `name` into a fresh session, and run it (or don't) |
| `save name` | what's typed or pasted next, until Ctrl-D, becomes app `name`'s source (Ctrl-C gives up) |
| `delete name` | remove app `name`'s source, and its folder if that's all it held |
| `home` | back to the launcher |

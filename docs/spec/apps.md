# Apps and storage

Every app is one Rio file, run as [running apps](running.md) says, with the whole app space to
itself. A Rinc has two built in, `home` and `console`, and where it has storage, apps there too.

## Apps on storage

An app on storage is a folder in `/rinc/apps` holding its Rio file, of the folder's name:
`/rinc/apps/chord/chord.rio`.
Its name is 1 to 32 letters, digits, `_` and `-`. If its first line is a comment, that's its
description, by convention a title, a colon, and what it does:

```rio fragment
# Chord: an ADPCM chord on 8 voices, with pitch and pan
```

| proc | |
|---|---|
| `pc.apps() -> Int` | looks at storage again, and says how many apps there are (the first 64, by name) |
| `pc.appName(i, out) -> Int` | app `i`'s name, from 0, in order of name ignoring case |
| `pc.appInfo(i, out) -> Int` | app `i`'s description: its first line after `#` and any spaces, if it starts with `#` |
| `pc.appSource(name, out) -> Int` | app `name`'s source, as much as fits in `out`: its whole length, or -1 if there's no such app |
| `pc.appSave(name, text) -> Int` | `text` becomes app `name`'s source (its folder too, if it's new): 0, or -1 |
| `pc.appDelete(name) -> Int` | removes app `name`'s source, and its folder if that's all it held: 0, or -1 |
| `pc.launch(name)` | after this frame, leave this app and run app `name` (or `home` or `console`) |

`appName`, `appInfo` and `appSource` copy as much as fits, and give the whole length (0 for an `i` that
isn't an app), so an app can tell when its buffer was too small. A Rinc without storage has no apps:
`pc.apps()` is 0, and the others give -1 (or do nothing).

## Home

A Rinc with apps starts at `home` (`system/home.rio`), which lists the apps and the console, and starts
what's picked with `pc.launch`.

## The system key

One key belongs to the Rinc, not to apps: the **system key**. Let go on its own, it goes home, from any
app. Pressed together with another key it's for the Rinc's own tools (none yet), so it doesn't go home
then. It's:

- the Windows key, on a keyboard that has one and a Rinc that gets it (the Fruit Jam);
- a controller's guide button (the Xbox button);
- or, anywhere, Shift tapped and then pressed again and held, within 0.4 seconds: the system key for as
  long as it's held. (A Shift held while typing other keys doesn't count, so capitals are fine.)

Apps never see the system key, or a Shift that's the system key.

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

# Apps and home

On a Rinc with storage, such as the Fruit Jam's SD card, each app is a Rio file in a folder of its own name:
`/rinc/apps/bounce/bounce.rio`. Home lists them, and its first line describes it:

```rio fragment
# Bounce: the arrow keys tilt gravity
```

Home is an app too (`system/home.rio`), using procs any app can:

- `pc.apps()` looks for the apps and says how many there are.
- `pc.appName(i, out)` and `pc.appInfo(i, out)` copy app `i`'s name, or its first line without the `#`,
  into a `Blob`, and say how long it is.
- `pc.launch(name)` starts an app (or `home`, or `console`) after the current frame.

So is the console (`system/console.rio`): it runs the Rio you type in a session of its own, with
`pc.rioStart`, `pc.rioEval` and the rest (see [Rio inside an app](../spec/rio.md)), and reads and writes
apps with `pc.appSource`, `pc.appSave` and `pc.appDelete`. And so is the editor (`system/edit.rio`), which
opens, saves and runs apps with the same procs.

The system key with Esc always goes home: Windows+Esc, a controller's Xbox+Back, or Shift tapped twice
and then Esc.

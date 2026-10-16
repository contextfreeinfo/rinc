# Apps and the launcher

On a Rinc with storage, such as the Fruit Jam's SD card, each app is a cart in a folder of its own name:
`/rinc/apps/bounce/bounce.rio`. The launcher lists them, and its first line describes it:

```rio fragment
# Bounce: the arrow keys tilt gravity
```

The launcher is a cart too (`system/launcher.rio`), using procs any cart can:

- `pc.apps()` looks for the apps and says how many there are.
- `pc.appName(i, out)` and `pc.appInfo(i, out)` copy app `i`'s name, or its first line without the `#`,
  into a `Blob`, and say how long it is.
- `pc.launch(name)` starts an app, and `pc.openConsole()` the console, after the current frame.

On the Fruit Jam, the Windows key (or a controller's Xbox button) always comes back to the launcher.

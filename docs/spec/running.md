# Running apps

An app is one Rio source file. To run it, a Rinc:

1. **Starts fresh:** an empty [app space](memory.md), every voice stopped, no typed text waiting.
2. **Compiles the whole app,** with `pc` auto-imported, as [memory](memory.md) says. If it doesn't
   compile, it doesn't run, and the first error is shown. Nothing of it runs.
3. **Runs its top-level code,** then its `main` proc if it has one.
4. **Runs frames,** if the app has `update*` or `draw*` (exported, so the Rinc can find them), 60 times
   a second until the app is left. Each frame:
   1. The Rinc sets the [buttons, sticks and triggers](input.md) as they are now, and queues what was
      typed since the last frame.
   2. It calls `update`, then `draw`, each if the app has it.
   3. It shows the [screen](screen.md) as the app left it. The screen isn't cleared between frames.

   An app with neither `update` nor `draw` isn't a game: it's done once `main` returns.

A runtime error stops the app, showing the error and its line. `log` writes a line where the Rinc
shows text: on the screen while an app starts or once it's done (but not over a game's frames), and to
a serial port or a terminal.

An app is left for another with `pc.launch`, or for home with the [system key](apps.md#the-system-key).
Leaving stops every voice.

Frames come 60 a second. An app that takes too long gets its frames late (how a Rinc catches up, if it
does, is its own), so for real time, use `pc.time()`.

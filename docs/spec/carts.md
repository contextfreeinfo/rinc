# Running carts

A cart is one Rio source file. To run it, a Rinc:

1. **Starts fresh:** an empty [app space](memory.md), every voice stopped, no typed text waiting.
2. **Compiles the whole cart,** with `pc` auto-imported. If it doesn't compile, it doesn't run, and the
   first error is shown. Nothing of it runs.
3. **Runs its top-level code,** then its `main` proc if it has one.
4. **Runs frames,** if the cart has `update*` or `draw*` (exported, so the Rinc can find them), 60 times
   a second until the cart is left. Each frame:
   1. The Rinc sets the [buttons, sticks and triggers](input.md) as they are now, and queues what was
      typed since the last frame.
   2. It calls `update`, then `draw`, each if the cart has it.
   3. It shows the [screen](screen.md) as the cart left it. The screen isn't cleared between frames.

   A cart with neither `update` nor `draw` isn't a game: it's done once `main` returns.

A runtime error stops the cart, showing the error and its line. `log` writes a line to the Rinc's
console (and anywhere else it shows text, such as a serial port).

How a cart is left depends on the Rinc: the Fruit Jam's system key goes back to the launcher, and Esc
closes the desktop's window. Leaving stops every voice.

Frames come 60 a second. A cart that takes too long gets its frames late (how a Rinc catches up, if it
does, is its own), so for real time, use `pc.time()`.

**Status:** the Fruit Jam doesn't compile an app as a whole yet: it reads the app into a console
session a top-level statement at a time, so an app with an error partly runs, `main` isn't called, and
its memory is smaller (see [memory](memory.md#status)).

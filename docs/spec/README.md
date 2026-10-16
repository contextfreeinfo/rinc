# What a Rinc is

This part says what every Rinc provides, so a cart that runs on one runs the same on all of them: the
Fruit Jam, the desktop and the web today, and any other Rinc someone makes. Where this implementation
doesn't match yet, the page says so.

A Rinc provides:

- **[Rio](https://contextfreeinfo.github.io/rio/)**, the language carts are written in, at the version
  in `lib/rio`.
- **The `pc` module:** the host procs in [`src/core/pc.rio`](https://github.com/rincpc/rinc/blob/main/src/core/pc.rio),
  with exactly those names and types, visible in every module without an import. The tests check
  `pc.rio` against what this Rinc registers.
- **[Running carts](carts.md):** compiling one, then its frames, 60 a second.
- **[The screen](screen.md):** 320x240, a 256-color palette, and a 5x8 font.
- **[Input](input.md):** buttons, a controller's sticks and triggers, typed text and time.
- **[Sound](sound.md):** 8 voices playing from the cart's memory.
- **[Memory](memory.md):** 256 KB for each cart, and nothing allocated while it runs.
- **[Apps and storage](apps.md),** where a Rinc has storage: the apps, the launcher and the console.

A Rinc can have more than this, such as a debugger or a volume knob, but a cart can't depend on it.

# Memory

A cart gets one block of memory, its **app space**: 256 KB, for everything it is. Nothing is allocated
while it runs. So whether a cart fits is known before it runs, and it's the same on every Rinc.

## The app space

A Rinc compiles a cart with Rio's whole-program compile into the 256 KB app space itself (in Rio's C
API: `rio_init` with the app space as its memory and no separate code buffer, then `rio_compile`, with
the limits Rio picks for that memory). Rio's [memory model](https://contextfreeinfo.github.io/rio/guide/memory.html)
then decides what fits:

- **Compiling** borrows the app space for the compiler's tables, so the most a cart can declare (symbols,
  procs, types, constants) grows with the room it leaves.
- **Running,** the app space holds the cart's code (8 bytes an instruction), its constants, globals and
  arrays, one frame for each proc (procs that never run at the same time share one), its strings, and
  Rio's small tables (exported names, line numbers).

A cart that doesn't fit gets a compile error ("out of memory", "too many symbols" and so on) and doesn't
run. Any Rinc gives the same answer for the same cart, since it's Rio's alone: `rinc -i cart.img cart.rio`
on the desktop checks a cart without running it.

```rio
big: [60000]Int                    # 240 KB of the 256
big[59999] = 1
log(big.len())
```

```output
60000
```

```rio
toobig: [70000]Int  # error: out of memory
```

## Outside the app space

A Rinc keeps its own things outside the app space, where carts can't reach them except through the `pc`
procs: the screen (75 KB, a byte a pixel), the voices' state, the typed-text queue, and its console.
They don't count against a cart's 256 KB, and a Rinc needs room for them besides. The voices read sound
data from the app space as they play, so they need no copies.

## Images

A cart can also be compiled ahead of time into a Rio image, as the web pages and exported games are.
An image's memory addresses are absolute, so images for a Rinc are compiled for a 256 KB app space, and
load into one.

## Console sessions

A Rinc's console (where it has one) keeps a Rio REPL session going, whose compiler tables stay in memory
for the whole session, so its room for code and data is the Rinc's own business. Carts run from the
launcher or as apps get the full app space above.

## Status

The Fruit Jam doesn't give apps the full app space yet. It runs every app in a console session: Rio gets
224 KB of the app space, a quarter of that (56 KB, 7,168 instructions) is kept for code, and the
compiler's tables take the other 32 KB. So a cart's data there tops out near 168 KB, and its symbols at
what 32 KB of tables holds. The plan is to load apps with the whole-program compile, as the desktop and
the web do, and to start a console session only for the console.

# Rio inside an app

An app can run Rio of its own: a console, an editor, a tutorial with exercises to type. It hands a
**session** some of its own memory, gives it Rio to compile and run, and runs the session's frames from
its own. The session has the `pc` module too, so what it draws shows on the screen. There's one session
at a time, and a session can't start one of its own.

| proc | |
|---|---|
| `pc.rioStart(mem, tables) -> Int` | a fresh session: its code and data in `mem`, its compiler's tables in `tables`, both the app's own memory: 0, or -1 |
| `pc.rioEval(text) -> Int` | compile and run a piece of Rio, as a REPL does: 0 done, 1 unfinished (it needs more text), -1 an error |
| `pc.rioFrame() -> Int` | the session's `update` then `draw`: 0, 1 if it has neither, -1 an error |
| `pc.rioType(key)` | a key for the session's `pc.key()` |
| `pc.rioError(out) -> Int` | the last error, as text, into `out`: how long it is |
| `pc.rioLog(out) -> Int` | what the session has logged since the last call, taken into `out`: how long (each line ends with 10) |
| `pc.rioIndent(text) -> Int` | how many steps an unfinished input's next line is indented |

Like a REPL, a session keeps its globals, procs and types from one piece to the next, and a bare
expression logs its value:

```rio
mem: Blob[65536]
tables: Blob[32768]
out: Blob[256]
log(pc.rioStart(mem[:], tables[:]))
r := pc.rioEval("x := 6 * 7\nx + 1\n")
n := pc.rioLog(out[:])
log(r, out[:n].asString())
log(pc.rioEval("y := nope\n"))
n = pc.rioError(out[:])
log(out[:n].asString())
```

```output
0
0 43

-1
1:6: undefined name near 'nope'
```

`tables` needs room for the compiler's tables, the `pc` module's included: 32 KB is plenty, and too
little is an error from `pc.rioStart`, which `pc.rioError` explains. The memory handed over is the
session's from then on: the app mustn't use it for anything else while the session lives. An app
starting over, or `pc.rioStart` again, ends the session. A session's keys come only from `pc.rioType`,
so the app decides what it gets: the console keeps Esc, to stop what's running.

The built-in console, `system/console.rio`, is an ordinary app built on these procs.

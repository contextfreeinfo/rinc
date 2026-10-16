# A first cart

A cart is a Rio program, in one `.rio` file. Its top-level code runs once, and then, if it defines
`update*` and `draw*`, Rinc calls them 60 times a second. Everything Rinc can do is in the module `pc`,
which every cart has without an import:

```rio
x := 0
update* :: proc()
  x += 1
end
draw* :: proc()
  pc.cls(1)                       # dark blue
  pc.rect(x % 320, 100, 20, 20, 8)  # a red square, moving right
  pc.print("HELLO", 10, 10, 7)
end
```

The `*` marks `update` and `draw` so Rinc can find them. Both are optional: a cart with neither just
runs its top-level code, which suits trying things out.

## Running it

- **In a browser:** the [demo page](https://rincpc.com/) runs the carts in this repository.
- **On the desktop:** `rinc hello.rio` opens a window (Esc quits). See [the desktop](../guide/desktop.md).
- **On the Fruit Jam:** put it on the SD card as `/rinc/apps/hello/hello.rio`, and pick it in the
  launcher; or type or paste it into the console and type `run`. See [the Fruit Jam](../guide/fruit-jam.md).

## log

`log` prints to the console (the terminal on the desktop, the console and the USB serial port on the
board), which is handy while working on a cart:

```rio
total := 0
for i in 1..=10
  total += i
end
log("total", total)
```

```output
total 55
```

Next: [drawing](drawing.md).

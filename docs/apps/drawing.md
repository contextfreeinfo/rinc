# Drawing

The screen is 320 by 240 pixels, `x` from the left and `y` from the top. Each pixel is a color number
from 0 to 255, picked from Rinc's palette: the 16 PICO-8 colors first, then a cube of 216 colors, then
24 grays. The screen keeps what's drawn until something draws over it, so a game usually starts its
`draw` with `pc.cls`.

```rio
pc.cls(0)                        # fill the screen: black
pc.pset(5, 5, 8)                 # one red pixel
pc.rect(10, 20, 30, 10, 12)      # x, y, width, height, color: blue
pc.print("SCORE 12", 0, 40, 7)   # text, 6 pixels a character
log(pc.pget(5, 5), pc.pget(15, 25), pc.pget(400, 0))
```

```output
8 12 0
```

Drawing off the screen is fine: it's cut off at the edges, and reading a pixel off the screen gives 0.

## The palette

| colors | |
|---|---|
| 0 to 15 | PICO-8's: 0 black, 1 dark blue, 2 dark purple, 3 dark green, 4 brown, 5 dark gray, 6 light gray, 7 white, 8 red, 9 orange, 10 yellow, 11 green, 12 blue, 13 lavender, 14 pink, 15 peach |
| 16 to 231 | a color cube: `16 + 36 * r + 6 * g + b`, each of `r g b` from 0 to 5 |
| 232 to 255 | grays, dark to light |

```rio
cube :: proc(r, g, b: Int) -> Int
  return 16 + 36 * r + 6 * g + b
end
for x in 0..<6
  pc.rect(x * 20, 0, 20, 20, cube(5, x, 0))   # red through orange to yellow
end
log(cube(5, 5, 5))
```

```output
231
```

## Pictures as data

Rinc has no sprite format of its own: a picture is just bytes, one color number each, drawn with
`pset`. `hexBlob` writes them in the app:

```rio
heart := hexBlob("
  00 08 00 08 00
  08 08 08 08 08
  00 08 08 08 00
  00 00 08 00 00")
drawPic :: proc(pic: Blob, w, x, y: Int)
  for c, i in pic
    if c != 0                    # 0: see-through
      pc.pset(x + i % w, y + i / w, c)
    end
  end
end
drawPic(heart, 5, 100, 100)
log(pc.pget(101, 100), pc.pget(100, 100))
```

```output
8 1
```

(The 1 is the screen's dark blue, from before anything was drawn there.)

Next: [input](input.md).

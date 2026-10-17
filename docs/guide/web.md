# The web

```sh
rinc apps/breakout.rio -web breakout.html
```

makes one self-contained page: the app compiled to an image, Rinc's VM as wasm with no compiler in it,
and a small JS host (`src/web/page.html`). Breakout comes to 52 KB (24 KB gzipped): 33 KB of wasm and a
2.5 KB image. Open it from disk or any server. The arrows and V, C, X and Z are the buttons, and any
controller the browser maps to the standard layout is player 1. Sound starts with the first key or
click, as browsers require.

The desktop rinc has the wasm built in when it's configured with `-DRINC_WEB_WASM=rinc.wasm`, which the
CI builds and the releases do. Making that wasm takes zig (`zig cc`, no emscripten):
`python tools/web.py --wasm=rinc.wasm`. `python tools/web.py apps/breakout.rio breakout.html` makes the
same page directly, with zig.

The [demo site](https://rincpc.com/) is made this way from `apps/` on each push to `main`, with these
docs at [rincpc.com/docs](https://rincpc.com/docs/).

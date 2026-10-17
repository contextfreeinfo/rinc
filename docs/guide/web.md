# The web

```sh
rinc apps/breakout.rio -web breakout.html
```

makes one self-contained page: a bundle of the app compiled to an image, Rinc's VM as wasm (gzipped,
and unzipped by the browser), and a small JS host (`src/web/page.html`). Breakout comes to 54 KB: a
31 KB player and a 3.5 KB bundle, in base64. Open it from disk or any server. The arrows and V, C, X and
Z are the buttons, and any controller the browser maps to the standard layout is player 1. Sound starts
with the first key or click, as browsers require.

A page can carry several apps, as an exported program does (see [the desktop](desktop.md)):

```sh
rinc system/home.rio system/console.rio apps/*.rio -home home -name rinc -web rinc.html
```

is the whole Rinc, as on the Fruit Jam. [Storage](../spec/apps.md#storage) is the browser's
localStorage, under the bundle's storage name, so what's saved stays in that browser, for pages from the
same site (or, from disk, the same folder). Shift tapped twice and then Esc goes home (a browser keeps
Shift+Esc for itself, so the second tap needn't be held); on a controller it's the guide button and Back.

## The two players

The desktop rinc carries two web players and gives a page the smaller one it can:

- **VM-only** (31 KB gzipped): runs compiled apps, the common case.
- **With the compiler** (68 KB gzipped): for a page with an app that runs Rio itself
  ([`pc.rio*`](../spec/rio.md), as the console does) or writes apps (`pc.appSave`, as the editor does).
  It also runs apps saved to storage, from source, and such a page carries its apps as source, so the
  editor can open them.

They're built in when the desktop rinc is configured with `-DRINC_WEB_WASM=rinc.wasm` and
`-DRINC_WEB_WASM_FULL=rinc-full.wasm`, which the CI does for the releases. Making them takes zig
(`zig cc`, no emscripten): `python tools/web.py --wasm=rinc.wasm --wasm-full=rinc-full.wasm`.

The [demo site](https://rincpc.com/) is made this way from `apps/` on each push to `main`, with a page
for each app, one of the whole Rinc, and these docs at [rincpc.com/docs](https://rincpc.com/docs/).

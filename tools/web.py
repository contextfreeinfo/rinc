"""Make a cart into one HTML page that plays it: Rim's VM as wasm (built with zig cc; no emscripten), a
small JS host and the cart, all inline. Open the page from disk or any server.

  python tools/web.py carts/breakout.rio breakout.html [--rio=path/to/rio] [--rim=path/to/rim]

The cart goes in compiled, as an image (rim -i makes it), with a VM that has no compiler: about a
quarter of the wasm. A cart whose code calls a host proc that compiles rio at runtime gets the
compiler too, with its source instead.
"""
import base64, html, os, shutil, subprocess, sys, tempfile

COMPILER_PROCS = {'rioCompile', 'rioEval'}  # (none exist yet: host procs that would need the compiler)

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
cart, out = args[0], args[1]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rio = opts.get('rio', os.path.join(root, 'lib', 'rio'))
rim = opts.get('rim') or next((p for p in [os.path.join(root, 'build-desktop', 'Release', 'rim.exe'),
                                           os.path.join(root, 'build-desktop', 'rim')] if os.path.exists(p)), None)
if not rim: sys.exit('web.py: build the desktop rim first (it makes the image), or give --rim=path')
title = os.path.splitext(os.path.basename(cart))[0]

with tempfile.TemporaryDirectory() as tmp:
  img = os.path.join(tmp, 'cart.img')
  r = subprocess.run([rim, '-i', img, cart], capture_output=True, text=True)
  if r.returncode: sys.exit(r.stderr.strip() or 'web.py: rim -i failed')
  procs = {l.split(' ', 1)[1] for l in r.stdout.splitlines() if l.startswith('host ')}
  full = bool(procs & COMPILER_PROCS)
  wasm = os.path.join(tmp, 'rim.wasm')
  sources = [os.path.join(root, 'src', 'web', 'main.c')] + [os.path.join(root, 'src', 'core', f) for f in ('rim.c', 'console.c', 'font.c')] + [os.path.join(rio, 'src', 'rio.c')]
  subprocess.run(['zig', 'cc', '-target', 'wasm32-wasi', '-mexec-model=reactor', '-Oz', '-DRIO_SMALL'] + ([] if full else ['-DRIO_NO_COMPILER']) +
                 ['-I' + os.path.join(rio, 'src'), '-I' + os.path.join(root, 'src', 'core'), '-Wl,--strip-all', '-o', wasm] + sources, check=True)
  code = base64.b64encode(open(wasm, 'rb').read()).decode()
  cartdata = base64.b64encode(open(cart, 'rb').read() if full else open(img, 'rb').read()).decode()

page = '''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>TITLE</title>
<style>
html, body { margin: 0; height: 100%; background: #000; overflow: hidden; }
canvas { position: absolute; inset: 0; margin: auto; image-rendering: pixelated; aspect-ratio: 4 / 3; width: min(100vw, 133.33vh); }
#err { position: absolute; left: 0; right: 0; bottom: 0; margin: 0; padding: 8px; color: #ff77a8; font: 13px monospace; white-space: pre-wrap; }
</style>
</head>
<body>
<canvas width="320" height="240"></canvas>
<pre id="err" hidden></pre>
<script>
(async () => {
  const b64 = s => Uint8Array.from(atob(s), c => c.charCodeAt(0));
  const canvas = document.querySelector("canvas"), ctx = canvas.getContext("2d"), image = ctx.createImageData(320, 240);
  const err = document.getElementById("err"), t0 = performance.now(), cart = b64("CART");
  let ex, out = "";
  const text = (p, n) => new TextDecoder().decode(new Uint8Array(ex.memory.buffer, p, n));
  const fail = p => { const m = new Uint8Array(ex.memory.buffer, p); err.textContent = text(p, m.indexOf(0)); err.hidden = false; };
  const env = {
    put: (p, n) => { out += text(p, n); const lines = out.split("\\n"); out = lines.pop(); lines.forEach(l => console.log(l)); },
    now: () => (performance.now() - t0) / 1000,
  };
  ex = (await WebAssembly.instantiate(b64("WASM"), { env })).instance.exports;
  ex._initialize();
  new Uint8Array(ex.memory.buffer, ex.source(), cart.length).set(cart);
  const draw = () => { image.data.set(new Uint8Array(ex.memory.buffer, ex.pixels(), 320 * 240 * 4)); ctx.putImageData(image, 0, 0); };
  if (ex.start(cart.length)) { draw(); fail(ex.error()); return; }
  // btn: arrows, then O (Z or C) and X (X or V)
  const keys = { ArrowLeft: 1, ArrowRight: 2, ArrowUp: 4, ArrowDown: 8, KeyZ: 16, KeyC: 16, KeyX: 32, KeyV: 32 };
  let buttons = 0, next = performance.now(), game = true;
  addEventListener("keydown", e => { if (keys[e.code]) { buttons |= keys[e.code]; e.preventDefault(); } });
  addEventListener("keyup", e => { if (keys[e.code]) buttons &= ~keys[e.code]; });
  addEventListener("blur", () => { buttons = 0; });
  const tick = now => {
    while (game && now >= next) { // 60 frames a second, whatever the display's rate
      const r = ex.frame(buttons);
      if (r) { game = false; if (r < 0) fail(ex.error()); }
      next += 1000 / 60;
      if (now - next > 100) next = now;
    }
    draw();
    if (game) requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
})();
</script>
</body>
</html>
'''
page = page.replace('TITLE', html.escape(title)).replace('WASM', code).replace('CART', cartdata)
open(out, 'w', encoding='utf-8', newline='\n').write(page)
print('%s: %d bytes (wasm %d, cart %d; %s)' % (out, len(page), len(code) * 3 // 4, len(cartdata) * 3 // 4,
      'compiler included: the cart calls ' + ', '.join(sorted(procs & COMPILER_PROCS)) if full else 'image, no compiler'))

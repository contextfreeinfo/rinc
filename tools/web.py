"""Make a cart into one HTML page that plays it: rio and Rim's core as wasm (built with zig cc; no
emscripten), a small JS host, and the cart's source, all inline. Open the page from disk or any server.

  python tools/web.py carts/breakout.rio breakout.html [--rio path/to/rio]
"""
import base64, html, json, os, subprocess, sys, tempfile

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
cart, out = args[0], args[1]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rio = opts.get('rio', os.path.join(root, 'lib', 'rio'))
title = os.path.splitext(os.path.basename(cart))[0]

with tempfile.TemporaryDirectory() as tmp:
  wasm = os.path.join(tmp, 'rim.wasm')
  subprocess.run(['zig', 'cc', '-target', 'wasm32-wasi', '-mexec-model=reactor', '-Os', '-DRIO_SMALL',
                  '-I' + os.path.join(rio, 'src'), '-I' + os.path.join(root, 'src', 'core'), '-Wl,--strip-all', '-o', wasm,
                  os.path.join(root, 'src', 'web', 'main.c'), os.path.join(root, 'src', 'core', 'rim.c'),
                  os.path.join(root, 'src', 'core', 'console.c'), os.path.join(root, 'src', 'core', 'font.c'),
                  os.path.join(rio, 'src', 'rio.c')], check=True)
  code = base64.b64encode(open(wasm, 'rb').read()).decode()

page = '''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>TITLE</title>
<style>
  :root { color-scheme: dark; }
  html, body { margin: 0; height: 100%; background: #000; color: #c2c3c7; font: 14px system-ui, sans-serif; }
  body { display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 8px; }
  canvas { image-rendering: pixelated; width: min(100vw, 133.33vh - 60px); aspect-ratio: 4 / 3; background: #000; }
  #msg { min-height: 1.2em; padding: 0 16px; text-align: center; white-space: pre-wrap; }
</style>
</head>
<body>
<canvas id="screen" width="320" height="240"></canvas>
<div id="msg">Arrows move, Z and X are the buttons.</div>
<script>
const wasmBase64 = "WASM";
const cartSource = SOURCE;
(async () => {
  const msg = document.getElementById("msg"), canvas = document.getElementById("screen");
  const ctx = canvas.getContext("2d"), image = ctx.createImageData(320, 240), start0 = performance.now();
  const bytes = Uint8Array.from(atob(wasmBase64), c => c.charCodeAt(0));
  let ex, outText = "";
  const text = (p, n) => new TextDecoder().decode(new Uint8Array(ex.memory.buffer, p, n));
  const cstr = p => { const m = new Uint8Array(ex.memory.buffer, p); return text(p, m.indexOf(0)); };
  const env = {
    put: (p, n) => { outText += text(p, n); const lines = outText.split("\\n"); outText = lines.pop(); lines.forEach(l => console.log(l)); },
    now: () => (performance.now() - start0) / 1000,
  };
  const { instance } = await WebAssembly.instantiate(bytes, { env });
  ex = instance.exports; ex._initialize();
  const src = new TextEncoder().encode(cartSource);
  new Uint8Array(ex.memory.buffer, ex.source(), src.length).set(src);
  const draw = () => { image.data.set(new Uint8Array(ex.memory.buffer, ex.pixels(), 320 * 240 * 4)); ctx.putImageData(image, 0, 0); };
  if (ex.start(src.length)) { msg.textContent = cstr(ex.error()); draw(); return; }
  // buttons, as on PICO-8: arrows, Z (or C) and X (or V)
  const keys = { ArrowLeft: 1, ArrowRight: 2, ArrowUp: 4, ArrowDown: 8, KeyZ: 16, KeyC: 16, KeyX: 32, KeyV: 32 };
  let buttons = 0;
  addEventListener("keydown", e => { if (keys[e.code]) { buttons |= keys[e.code]; e.preventDefault(); } });
  addEventListener("keyup", e => { if (keys[e.code]) buttons &= ~keys[e.code]; });
  addEventListener("blur", () => { buttons = 0; });
  let next = performance.now(), game = true;
  const tick = now => {
    while (game && now >= next) { // 60 frames a second, whatever the display's rate
      const r = ex.frame(buttons);
      if (r) { game = false; if (r < 0) msg.textContent = cstr(ex.error()); }
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
page = page.replace('TITLE', html.escape(title)).replace('WASM', code).replace('SOURCE', json.dumps(open(cart, encoding='utf-8').read()).replace('</', '<\\/'))
open(out, 'w', encoding='utf-8', newline='\n').write(page)
print(out, len(page), 'bytes (wasm', len(code) * 3 // 4, 'bytes)')

"""Make a cart into one HTML page that plays it: Rinc's VM as wasm (built with zig cc; no emscripten), a
small JS host (src/web/page.html) and the cart, all inline. Open the page from disk or any server.

  python tools/web.py carts/breakout.rio breakout.html [--rio=path/to/rio] [--rinc=path/to/rinc]
  python tools/web.py --wasm=rinc.wasm      just the VM-only wasm: rinc -web builds it in (RINC_WEB_WASM)

The cart goes in compiled, as an image (rinc -i makes it), with a VM that has no compiler: about a
quarter of the wasm. A cart whose code calls a host proc that compiles rio at runtime gets the
compiler too, with its source instead. (rinc -web does the same without zig or Python, but only with
the VM-only wasm.)
"""
import base64, html, os, shutil, subprocess, sys, tempfile

COMPILER_PROCS = {'pc.rioCompile', 'pc.rioEval'}  # (none exist yet: host procs that would need the compiler)

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rio = opts.get('rio', os.path.join(root, 'lib', 'rio'))

def build_wasm(path, full):
  # rio's core: every source but its CLI (main.c), its C backend (aot.c) and the ARM-only jit.c (with
  # rio_NO_COMPILER, the compiler's files compile to nothing)
  rio_core = sorted(f for f in os.listdir(os.path.join(rio, 'src')) if f.endswith('.c') and f not in ('main.c', 'aot.c', 'jit.c'))
  sources = [os.path.join(root, 'src', 'web', 'main.c')] + [os.path.join(root, 'src', 'core', f) for f in ('rinc.c', 'console.c', 'font.c', 'audio.c')] + [os.path.join(rio, 'src', f) for f in rio_core]
  subprocess.run(['zig', 'cc', '-target', 'wasm32-wasi', '-mexec-model=reactor', '-Oz', '-Drio_SMALL', '-Drio_MAX_FFI=64'] + ([] if full else ['-Drio_NO_COMPILER']) +
                 ['-I' + os.path.join(rio, 'src'), '-I' + os.path.join(root, 'src', 'core'), '-Wl,--strip-all', '-o', path] + sources, check=True)

if 'wasm' in opts:
  build_wasm(opts['wasm'], False)
  print('%s: %d bytes' % (opts['wasm'], os.path.getsize(opts['wasm'])))
  sys.exit(0)

cart, out = args[0], args[1]
rinc = opts.get('rinc') or next((p for p in [os.path.join(root, 'build-desktop', 'Release', 'rinc.exe'),
                                           os.path.join(root, 'build-desktop', 'rinc')] if os.path.exists(p)), None)
if not rinc: sys.exit('web.py: build the desktop rinc first (it makes the image), or give --rinc=path')
title = os.path.splitext(os.path.basename(cart))[0]

with tempfile.TemporaryDirectory() as tmp:
  img = os.path.join(tmp, 'cart.img')
  r = subprocess.run([rinc, '-i', img, cart], capture_output=True, text=True)
  if r.returncode: sys.exit(r.stderr.strip() or 'web.py: rinc -i failed')
  procs = {l.split(' ', 1)[1] for l in r.stdout.splitlines() if l.startswith('host ')}
  full = bool(procs & COMPILER_PROCS)
  wasm = os.path.join(tmp, 'rinc.wasm')
  build_wasm(wasm, full)
  code = base64.b64encode(open(wasm, 'rb').read()).decode()
  cartdata = base64.b64encode(open(cart, 'rb').read() if full else open(img, 'rb').read()).decode()

page = open(os.path.join(root, 'src', 'web', 'page.html'), encoding='utf-8').read()
page = page.replace('{{TITLE}}', html.escape(title)).replace('{{WASM}}', code).replace('{{CART}}', cartdata)
open(out, 'w', encoding='utf-8', newline='\n').write(page)
print('%s: %d bytes (wasm %d, cart %d; %s)' % (out, len(page), len(code) * 3 // 4, len(cartdata) * 3 // 4,
      'compiler included: the cart calls ' + ', '.join(sorted(procs & COMPILER_PROCS)) if full else 'image, no compiler'))

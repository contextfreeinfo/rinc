# /// script
# dependencies = ["pyserial"]
# ///
"""Send a cart to Rim's REPL over USB serial and start it.

  uv run tools/send.py carts/bounce.rio [COM3] [--no-run]     (uv fetches pyserial)

Esc (or Ctrl-C) stops a running game first, so this also replaces one that's playing. Prints what the
board says back: errors show up with their lines.
"""
import sys, time
import serial  # pyserial

args = [a for a in sys.argv[1:] if not a.startswith('--')]
cart, port = args[0], args[1] if len(args) > 1 else 'COM3'
run = '--no-run' not in sys.argv
src = open(cart, encoding='utf-8').read().replace('\r\n', '\n')

with serial.Serial(port, 115200, timeout=0.2) as s:
  s.dtr = True
  s.write(b'\x1b')                      # stop a game that's running, if any
  time.sleep(0.2); s.reset_input_buffer()
  s.write(src.replace('\n', '\r').encode() + (b'\r' if not src.endswith('\n') else b''))
  if run: s.write(b'run\r')
  out, quiet = b'', 0
  while quiet < 3:                       # read until the board has been quiet for a moment
    chunk = s.read(4096)
    out += chunk; quiet = 0 if chunk else quiet + 1
  text = out.decode(errors='replace').replace('\r', '')
  errors = [l for l in text.split('\n') if 'error' in l or (l[:1].isdigit() and ':' in l and ': ' in l)]
  print('\n'.join(errors) if errors else 'sent %d lines%s' % (src.count('\n'), ', running' if run else ''))

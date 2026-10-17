# /// script
# dependencies = ["pyserial"]
# ///
"""Send an app to Rinc's console over USB serial and start it, or save it to the board's SD card.

  uv run tools/send.py apps/bounce.rio [COM3] [--no-run]     (uv fetches pyserial)
  uv run tools/send.py apps/bounce.rio [COM3] --save         to /rinc/apps/bounce/bounce.rio
  uv run tools/send.py apps/*.rio --save                     several at once

Ctrl-C over serial opens the console, in place of whatever's running, so this also replaces a game
that's playing. Prints what the board says back: errors show up with their lines. A saved app runs
from home, or with `run bounce` in the console.
"""
import glob, os, sys, time
import serial  # pyserial

args = [a for a in sys.argv[1:] if not a.startswith('--')]
port = next((a for a in args if a.upper().startswith('COM') or a.startswith('/dev/')), 'COM3')
apps = [c for a in args if a != port for c in (glob.glob(a) or [a])]
run = '--no-run' not in sys.argv
save = '--save' in sys.argv

def talk(s, data):
  """send, then read until the board has been quiet for a moment"""
  s.write(data)
  out, quiet = b'', 0
  while quiet < 3:
    chunk = s.read(4096)
    out += chunk; quiet = 0 if chunk else quiet + 1
  return out.decode(errors='replace').replace('\r', '')

with serial.Serial(port, 115200, timeout=0.2) as s:
  s.dtr = True
  s.write(b'\x03')                      # stop what's running (home, a game), into the console
  time.sleep(0.2); s.reset_input_buffer()
  for app in apps:
    src = open(app, encoding='utf-8').read().replace('\r\n', '\n')
    body = src.replace('\n', '\r').encode() + (b'\r' if not src.endswith('\n') else b'')
    if save:
      name = os.path.splitext(os.path.basename(app))[0]
      text = talk(s, b'save ' + name.encode() + b'\r' + body + b'\x04')
      saved = [l for l in text.split('\n') if l.startswith(('saved', "can't", 'no storage', 'couldn', 'names are'))]
      print('%s: %s' % (name, saved[-1] if saved else 'no answer'))
    else:
      text = talk(s, body + (b'run\r' if run else b''))
      errors = [l for l in text.split('\n') if 'error' in l or (l[:1].isdigit() and ':' in l and ': ' in l)]
      print('\n'.join(errors) if errors else 'sent %d lines%s' % (src.count('\n'), ', running' if run else ''))

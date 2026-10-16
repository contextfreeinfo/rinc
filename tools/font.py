"""Make src/core/font.c from a font made in the spry font editor (JSON: a character code -> 16 rows,
each row's bit 2 the leftmost column), for the 5x8 font: rows 4-11, columns from bit 2.

  python tools/font.py spry5x8-font.json > src/core/font.c
"""
import json, sys

d = json.load(open(sys.argv[1]))
out = ['/* the 5x8 font, ASCII 32-126: 8 rows each, bit 0 the leftmost column. Made by tools/font.py from',
       '   ' + sys.argv[1].replace('\\', '/').split('/')[-1] + ' (from spryjam); shown in 6x9 cells */',
       '#include "rinc.h"', '', 'const uint8_t rinc_font[95][8] = {']
for c in range(32, 127):
  rows = d.get(str(c), [0] * 16)[4:12]
  out.append('  {' + ', '.join('0x%02X' % ((v >> 2) & 0x1F) for v in rows) + '}, /* ' + (repr(chr(c)) if c != 92 else "'\\\\'").replace('*/', '* /') + ' */')
out.append('};')
print('\n'.join(out))

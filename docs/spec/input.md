# Input

A Rinc reads its keyboard and controller before each frame, and an app sees the same values for the whole
frame. A Rinc may have no controller, or (in principle) no keyboard; the procs then give 0 or -1.

## Buttons

`pc.btn(i)` is 1 while button `i` is held, and 0 otherwise or for an `i` outside 0 to 15:

| `i` | button | keyboard | controller |
|---|---|---|---|
| 0, 1, 2, 3 | left, right, up, down | arrows | d-pad, or the left stick pushed past half way |
| 4 | A | V | A |
| 5 | B | C | B |
| 6 | X | X | X |
| 7 | Y | Z | Y |
| 8, 9 | LB, RB | | shoulder buttons |
| 10, 11 | LS, RS | | sticks pressed in |
| 12, 13 | Start, Back | | |

The keyboard and the controller are both player 1: a button is held if either holds it. The face
buttons are named and placed as on an Xbox controller, in order of importance, so an app that needs
one button uses A.

## Sticks and triggers

`pc.stick(i)` is a stick's position from -1 to 1: `i` 0 is the left stick's x (+ right), 1 its y
(+ down), and 2 and 3 the right stick's. Within 0.15 of the middle it's 0, and beyond that it's rescaled
to reach 1 at the edge, so a resting stick reads 0. `pc.trigger(i)` is a trigger from 0 to 1, `i` 0 the
left one and 1 the right. Without a controller, or for another `i`, they're 0.

## Typed text

`pc.key()` takes the next key typed from a queue, or gives -1 when it's empty, for text and for editing
it:

| key | |
|---|---|
| 32 to 126 | printable ASCII, as typed |
| 8, 9, 13, 27 | Backspace, Tab, Enter, Esc |
| 1 to 26 | Ctrl with a letter: 1 for Ctrl-A, 3 for Ctrl-C and so on (Ctrl-H, Ctrl-I and Ctrl-M are 8, 9 and 13) |
| 128, 129, 130, 131 | left, right, up, down |
| 132, 133, 134 | Home, End, Delete |
| 135, 136 | Page Up, Page Down |

A key that isn't a character (all but printable ASCII and Esc) also says which modifiers were held
with it, as bits added on: 256 Shift, 512 Ctrl, 1024 Alt. Ctrl+Right is 129 + 512, Shift+Tab 9 + 256, and
Ctrl+Shift+Z 26 + 256 (Ctrl+letter already says Ctrl). Enter and Backspace don't take Shift, as it's
easily still held from typing. So `c & 255` is the key without its modifiers, and an app that doesn't
care about them can use that.

Other keys (function keys, the system key) add nothing. A Rinc drops what's typed beyond 63 keys waiting,
and one fed text faster than that (a serial port, a paste) waits for room. Keys also hold buttons down
while they're held (the arrows, V, C, X and Z), so an app reads them either way.

A terminal's escape sequences (`ESC [ A` for up, `ESC [ 3 ~` for Delete, and so on) become the key codes
above, so a Rinc fed by a serial terminal types the same keys. Modifiers come as xterm sends them:
`ESC [ 1 ; 5 C` is Ctrl+Right, `ESC [ Z` Shift+Tab, and `ESC [ 27 ; m ; k ~` key `k` with modifiers `m`.

## Time

`pc.time()` is the seconds since the Rinc started, as a `Float`.

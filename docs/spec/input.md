# Input

A Rinc reads its keyboard and controller before each frame, and a cart sees the same values for the whole
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
buttons are named and placed as on an Xbox controller, in order of importance, so a cart that needs
one button uses A.

## Sticks and triggers

`pc.stick(i)` is a stick's position from -1 to 1: `i` 0 is the left stick's x (+ right), 1 its y
(+ down), and 2 and 3 the right stick's. Within 0.15 of the middle it's 0, and beyond that it's rescaled
to reach 1 at the edge, so a resting stick reads 0. `pc.trigger(i)` is a trigger from 0 to 1, `i` 0 the
left one and 1 the right. Without a controller, or for another `i`, they're 0.

## Typed text

`pc.key()` takes the next character typed from a queue, or gives -1 when it's empty. The queue holds
printable ASCII (32 to 126), 13 for Enter, 8 for Backspace and 27 for Esc. Other keys (arrows, function
keys) add nothing, and a Rinc drops what's typed beyond 63 characters waiting. Typing also holds buttons
down as keys are held, so a cart reads keys either way.

## Time

`pc.time()` is the seconds since the Rinc started, as a `Float`.

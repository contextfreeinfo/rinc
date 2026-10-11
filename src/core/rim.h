/* Rim: what every platform shares. The screen is plain memory that carts draw into through the host
   procs registered here; a platform shows it (the Fruit Jam over DVI, the desktop in a window or a
   file) and feeds keys to the REPL. No allocation: the platform hands over every buffer. */
#ifndef RIM_CORE_H
#define RIM_CORE_H
#include <stdint.h>
#include "rio.h"

#define RIM_W 320
#define RIM_H 240

extern uint8_t rim_screen[RIM_H][RIM_W]; /* 8 bits per pixel: an index into rim_palette */
extern uint16_t rim_palette[256];        /* RGB565: the PICO-8 16, a 6x6x6 color cube, 24 grays */
extern float (*rim_time)(void);          /* seconds since start, from the platform */
/* buttons held, as on PICO-8: bit 0 left, 1 right, 2 up, 3 down, 4 O (Z key), 5 X (X key). Each
   platform sets them from its keys before a frame */
extern uint8_t rim_buttons;
enum { RIM_LEFT = 1, RIM_RIGHT = 2, RIM_UP = 4, RIM_DOWN = 8, RIM_O = 16, RIM_X = 32 };

/* set up rio in the given memory (the app space: code and data both live there), with log output
   going to put and the host procs carts call: cls(c), pset(x, y, c), pget(x, y) -> c,
   rect(x, y, w, h, c), print(s, x, y, c), btn(i) -> Int (1 held), time() -> Float */
int rim_start(Rio *vm, void *mem, uint32_t memsize, void (*put)(const char *s, int n));
/* one frame of a game: the cart's update* and then draw* (each optional). 0: ran, 1: the cart has
   neither (it isn't a game), -1: a runtime error (rio_error says) */
int rim_frame(Rio *vm);

/* text: the 5x8 font (ASCII 32-126, bit 0 leftmost) in 6x9 cells, and a console of 53x26 of them
   that scrolls the screen (console.c) */
extern const uint8_t rim_font[95][8];
extern uint8_t rim_text_fg, rim_text_bg;
void rim_char(int c, int x, int y, int fg, int bg); /* bg < 0: transparent */
void rim_console_put(const char *s, int n);
void rim_console_clear(int bg);

/* a REPL over any text stream: feed it characters, it echoes them and runs each complete input */
typedef struct {
  Rio *vm;
  void (*put)(const char *s, int n);
  char acc[4096];
  int n, line0; /* acc holds the unfinished input; the current line starts at line0 */
  int echo;      /* echo typed characters (serial terminals don't; a desktop terminal already did) */
  int run;       /* the line "run" asks for the game loop: the platform runs frames while it's set */
} RimRepl;
int rim_repl_begin(RimRepl *r, Rio *vm, void *scratch, uint32_t size);
void rim_repl_stop(RimRepl *r, const char *why); /* the game loop ended (why: NULL, or a message) */
void rim_repl_char(RimRepl *r, int c);

#endif

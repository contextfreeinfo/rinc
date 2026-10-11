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

/* set up rio in the given memory, with log output going to put and the host procs carts call:
   cls(c), pset(x, y, c), pget(x, y) -> c, rect(x, y, w, h, c), time() -> Float */
int rim_start(Rio *vm, void *mem, uint32_t memsize, RioIns *code, uint32_t codecap, void (*put)(const char *s, int n));

/* a REPL over any text stream: feed it characters, it echoes them and runs each complete input */
typedef struct {
  Rio *vm;
  void (*put)(const char *s, int n);
  char acc[8192];
  int n, line0; /* acc holds the unfinished input; the current line starts at line0 */
  int echo;      /* echo typed characters (serial terminals don't; a desktop terminal already did) */
} RimRepl;
int rim_repl_begin(RimRepl *r, Rio *vm, void *scratch, uint32_t size);
void rim_repl_char(RimRepl *r, int c);

#endif

/* typed keys, as a terminal sends them, become pc.key()'s codes (docs/spec/input.md#typed-text): editing
   keys, their modifiers (xterm's ESC [ 1 ; m X, ESC [ n ; m ~ and ESC [ 27 ; m ; k ~), Shift+Tab, a
   lone Esc, Ctrl+letter and plain characters */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include "rinc.h"

static _Alignas(16) uint8_t space[256 << 10];
static rio_Vm vm;
static char out[1024];
static int outLen;

static void put(const char *s, int n) {
  if (n > (int)sizeof out - 1 - outLen) n = (int)sizeof out - 1 - outLen;
  memcpy(out + outLen, s, (size_t)n);
  outLen += n;
  out[outLen] = 0;
}

int main(void) {
  static const char typed[] =
    "\033[C"        /* right: 129 */
    "\033[1;5C"     /* Ctrl+right: 129 + 512 */
    "\033[1;2D"     /* Shift+left: 128 + 256 */
    "\033[3;6~"     /* Ctrl+Shift+Delete: 134 + 256 + 512 */
    "\033[Z"        /* Shift+Tab: 9 + 256 */
    "\033[27;5;8~"  /* Ctrl+Backspace: 8 + 512 */
    "\033[27;6;122~" /* Ctrl+Shift+Z: 26 + 256 */
    "\032"          /* Ctrl+Z: 26 */
    "a\r";          /* a, Enter */
  static const char app[] = "c := pc.key()\nfor c >= 0\n  log(c)\n  c = pc.key()\nend\n";
  static const char want[] = "129\n641\n384\n902\n265\n520\n282\n26\n97\n13\n";
  const char *p;
  if (rinc_start(&vm, space, sizeof space, put) || rinc_compile(&vm, space, sizeof space, app, sizeof app - 1)) {
    printf("FAIL: %s\n", rio_error(&vm));
    return 1;
  }
  for (p = typed; *p; p++) rinc_textPush((unsigned char)*p); /* (after the start, which empties the queue) */
  if (rio_run(&vm)) {
    printf("FAIL: %s\n", rio_error(&vm));
    return 1;
  }
  for (p = out; *p; p++) if (*p == '\r') memmove((char *)p, p + 1, strlen(p)); /* (lines as \n) */
  if (strcmp(out, want)) { printf("FAIL: got\n%swanted\n%s", out, want); return 1; }
  { /* the system key: Shift tapped twice, a frame a letter (s Shift, o another key, S both, e Esc, . nothing),
       then Esc: 1 for home on the frame after the last letter, if wanted */
    static const struct { const char *frames; int home; } cases[] = {
      {"s.s.e", 1},    /* tapped twice, then Esc */
      {"s.se", 1},     /* tapped, then held with Esc */
      {"s.s.oe", 0},   /* another key in between */
      {"sS.s.e", 0},   /* a capital typed, not a tap */
      {"s.e", 0},      /* tapped once */
      {"..e", 0},      /* Esc alone */
    };
    int i, f;
    for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
      const char *fr = cases[i].frames; int home = 0;
      for (f = 0; f < 60; f++) rinc_systemKey(0, 0, 0, 0); /* (settled) */
      for (f = 0; fr[f]; f++) home = rinc_systemKey(0, fr[f] == 's' || fr[f] == 'S', fr[f] == 'o' || fr[f] == 'S', fr[f] == 'e');
      if (home != cases[i].home) { printf("FAIL: system key %s gave %d\n", fr, home); return 1; }
    }
    /* the overlay: up while the system key is (tapped twice, armed), its edge drawn; down after home */
    for (f = 0; f < 60; f++) rinc_systemKey(0, 0, 0, 0);
    rinc_systemKey(0, 1, 0, 0); rinc_systemKey(0, 0, 0, 0); rinc_systemKey(0, 1, 0, 0); rinc_systemKey(0, 0, 0, 0);
    if (!rinc_overlayOn || rinc_shownRow(rinc_H - rinc_OVERLAY_H) != rinc_overlay[0] || rinc_overlay[0][0] != 10) {
      printf("FAIL: no overlay while the system key is up\n");
      return 1;
    }
    if (!rinc_systemKey(0, 0, 0, 1) || rinc_overlayOn || rinc_shownRow(rinc_H - 1) != rinc_screen[rinc_H - 1]) {
      printf("FAIL: the overlay stays after going home\n");
      return 1;
    }
  }
  printf("all keys came through\n");
  return 0;
}

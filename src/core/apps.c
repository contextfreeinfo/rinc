/* the built-in apps: home and the console, ordinary apps built in from system/ (where a
   platform has apps: the Fruit Jam, and the desktop to try them) */
#include <string.h>
#include "rinc.h"

extern const char rinc_homeSrc[]; extern const int rinc_homeSrc_len;
extern const char rinc_consoleSrc[]; extern const int rinc_consoleSrc_len;

const char *rinc_builtinApp(const char *name, int *len) {
  if (!strcmp(name, "home")) { *len = rinc_homeSrc_len; return rinc_homeSrc; }
  if (!strcmp(name, "console")) { *len = rinc_consoleSrc_len; return rinc_consoleSrc; }
  return 0;
}

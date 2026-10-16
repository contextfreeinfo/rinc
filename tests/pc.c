/* pc.rio is what Rinc's host procs are: compiled as the pc module against what rinc_hostProcs
   registers, each declaration's types must match, and every host proc must be declared.
   argv[1]: src/core/pc.rio */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include "rinc.h"

static unsigned char mem[256 << 10];
static rio_Vm vm;
static char src[16 << 10];
static uint32_t len;

/* the loader has just pc */
static int loader(void *ud, const char *path, int kind, rio_Source *o) {
  (void)ud;
  if (!(kind & rio_LoadKind_package) || strcmp(path, "pc")) return 1;
  o->src = src; o->len = len; o->isDir = 0; o->name = "pc.rio";
  return 0;
}

int main(int argc, char **argv) {
  static const char use[] = "import pc\npc.cls(1)\n";
  FILE *f = argc > 1 ? fopen(argv[1], "rb") : 0;
  if (!f) { printf("FAIL: can't read pc.rio\n"); return 1; }
  len = (uint32_t)fread(src, 1, sizeof src, f);
  fclose(f);
  if (len >= sizeof src) { printf("FAIL: pc.rio is too big for this test\n"); return 1; }
  rio_init(&vm, mem, sizeof mem, 0, 0);
  rio_setLoader(&vm, loader, 0);
  if (rinc_hostProcs(&vm)) { printf("FAIL: can't register the host procs\n"); return 1; }
  if (rio_compile(&vm, use, sizeof use - 1)) {
    rio_Error e = rio_errorInfo(&vm);
    printf("FAIL: %s:%s\n", e.file ? e.file : "(main)", rio_error(&vm));
    return 1;
  }
  printf("pc.rio declares all %d host procs\n", vm.ffiCount);
  return 0;
}

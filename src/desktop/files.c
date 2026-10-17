/* storage on the desktop: Rinc's folder in the user's own data folder (%APPDATA%\rinc on Windows,
   ~/Library/Application Support/rinc on macOS, $XDG_DATA_HOME/rinc or ~/.local/share/rinc elsewhere),
   as rinc_fs. Rinc's paths, /rinc/apps/... and /rinc/save/..., map into it, and nothing else does:
   apps stay inside their Rinc's folder, as on the Fruit Jam's SD card */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rinc.h"
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define rinc_mkdir(p) _mkdir(p)
#define rinc_rmdir(p) _rmdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define rinc_mkdir(p) mkdir(p, 0755)
#define rinc_rmdir(p) rmdir(p)
#endif

static char rinc_root[512];
static FILE *rinc_file;

/* where Rinc's path lives on this computer, into out: 0, or -1 for a path outside Rinc's */
static int rinc_local(const char *path, char *out, size_t cap) {
  if (strncmp(path, "/rinc/", 6) || strstr(path, "..") || strchr(path, '\\') || strchr(path, ':')) return -1;
  return snprintf(out, cap, "%s/%s", rinc_root, path + 6) < (int)cap ? 0 : -1;
}
/* every folder along the way to a file on this computer (those there already: no harm) */
void rinc_mkdirsFor(const char *file) {
  char local[1024], *p;
  snprintf(local, sizeof local, "%s", file);
  for (p = strchr(local + 1, '/'); p; p = strchr(p + 1, '/')) { *p = 0; rinc_mkdir(local); *p = '/'; }
}
static int rinc_open(const char *path, int write) {
  char local[1024];
  if (rinc_file) { fclose(rinc_file); rinc_file = 0; }
  if (rinc_local(path, local, sizeof local)) return -1;
  if (write) rinc_mkdirsFor(local);
  return (rinc_file = fopen(local, write ? "wb" : "rb")) ? 0 : -1;
}
static int rinc_read(char *buf, int n) { return rinc_file ? (int)fread(buf, 1, (size_t)n, rinc_file) : -1; }
static int rinc_write(const char *buf, int n) { return rinc_file && fwrite(buf, 1, (size_t)n, rinc_file) == (size_t)n ? n : -1; }
static int rinc_close(void) { int bad = !rinc_file || fclose(rinc_file); rinc_file = 0; return bad ? -1 : 0; }
static int rinc_list(const char *dir, void (*each)(const char *name, int dir, void *ud), void *ud) {
  char local[1024];
  if (rinc_local(dir, local, sizeof local)) return -1;
#ifdef _WIN32
  {
    struct _finddata_t f; intptr_t h;
    if (strlen(local) + 3 > sizeof local) return -1;
    strcat(local, "/*");
    if ((h = _findfirst(local, &f)) == -1) return -1;
    do if (f.name[0] != '.') each(f.name, (f.attrib & _A_SUBDIR) != 0, ud); while (!_findnext(h, &f));
    _findclose(h);
  }
#else
  {
    DIR *d = opendir(local); struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d))) {
      char full[1300]; struct stat st;
      if (e->d_name[0] == '.') continue;
      snprintf(full, sizeof full, "%s/%s", local, e->d_name);
      each(e->d_name, !stat(full, &st) && S_ISDIR(st.st_mode), ud);
    }
    closedir(d);
  }
#endif
  return 0;
}
static int rinc_remove(const char *path) {
  char local[1024];
  if (rinc_local(path, local, sizeof local)) return -1;
  return remove(local) && rinc_rmdir(local) ? -1 : 0;
}
static const rinc_Fs rinc_desktopFs = {rinc_open, rinc_read, rinc_write, rinc_close, rinc_list, rinc_remove};

/* storage in folder dir itself (rinc -data dir: tests, or a Rinc on a USB stick) */
const char *rinc_storageAt(const char *dir) {
  snprintf(rinc_root, sizeof rinc_root, "%s", dir);
  rinc_fs = &rinc_desktopFs;
  return rinc_root;
}

/* storage in the user's data folder, in a folder of this name (rinc, or a game's own) */
const char *rinc_storageStart(const char *name) {
  const char *base;
#if defined(_WIN32)
  base = getenv("APPDATA");
  snprintf(rinc_root, sizeof rinc_root, "%s/%s", base ? base : ".", name);
#elif defined(__APPLE__)
  base = getenv("HOME");
  snprintf(rinc_root, sizeof rinc_root, "%s/Library/Application Support/%s", base ? base : ".", name);
#else
  if ((base = getenv("XDG_DATA_HOME")) && *base) snprintf(rinc_root, sizeof rinc_root, "%s/%s", base, name);
  else snprintf(rinc_root, sizeof rinc_root, "%s/.local/share/%s", (base = getenv("HOME")) ? base : ".", name);
#endif
  rinc_fs = &rinc_desktopFs;
  return rinc_root;
}

/* Rinc: what every platform shares. The screen is plain memory that apps draw into through the host
   procs registered here; a platform shows it (the Fruit Jam over DVI, the desktop in a window or a
   file) and feeds keys to the REPL. No allocation: the platform hands over every buffer. */
#ifndef rinc_CORE_H
#define rinc_CORE_H
#include <stdint.h>
#include "rio.h"

#define rinc_W 320
#define rinc_H 240

extern uint8_t rinc_screen[rinc_H][rinc_W]; /* 8 bits per pixel: an index into rinc_palette */
extern uint16_t rinc_palette[256];        /* RGB565: the PICO-8 16, a 6x6x6 color cube, 24 grays */
extern float (*rinc_time)(void);          /* seconds since start, from the platform */
/* buttons held: bit 0 left, 1 right, 2 up, 3 down, then the face buttons, named as on an Xbox
   controller and in order of importance: 4 A (V key), 5 B (C), 6 X (X), 7 Y (Z), then the
   controller's others: 8 LB, 9 RB, 10 left stick, 11 right stick (pressed in), 12 Start, 13 Back.
   Each platform sets them from its keys and controllers before a frame */
extern uint16_t rinc_buttons;
/* a controller's sticks and triggers, also set by the platform: 0 left stick x, 1 its y, 2 and 3 the
   right stick's (-32767 to 32767; y is + down, as on the screen), 4 left trigger, 5 right (0 to 32767) */
extern int16_t rinc_axes[6];
/* typed text, for an app's key() (docs/spec/input.md): the platform passes what's typed, as a terminal
   sends it: printable ASCII, 13 Enter (or \n), 8 Backspace (or 127), 9 Tab, Ctrl+letter as 1-26, and
   editing keys as ESC [ sequences, which become the key codes below. A key that isn't a character
   (these, Tab, Enter, Backspace, Ctrl+letter) carries the modifiers held with it, as bits: as a
   terminal sends them, ESC [ 1 ; 5 C for Ctrl+right, ESC [ Z for Shift+Tab, ESC [ 27 ; m ; key ~
   for the rest */
enum { rinc_Key_left = 128, rinc_Key_right, rinc_Key_up, rinc_Key_down, rinc_Key_home, rinc_Key_end, rinc_Key_delete,
       rinc_Key_pageUp, rinc_Key_pageDown };
enum { rinc_Mod_shift = 256, rinc_Mod_ctrl = 512, rinc_Mod_alt = 1024 };
void rinc_textPush(int c);
void rinc_textEsc(void);    /* the Esc key itself, for key() (the platform knows it from a terminal escape) */
void rinc_textKey(int key); /* a key code itself (rinc_Key_*, with rinc_Mod_*), from a platform that knows the key */
int rinc_textMods(int shift, int ctrl, int alt); /* the rinc_Mod_* bits for these */
int rinc_textRoom(void);    /* how many more can wait: a platform reads its serial port no faster */
enum { rinc_Button_left = 1, rinc_Button_right = 2, rinc_Button_up = 4, rinc_Button_down = 8, rinc_Button_a = 16, rinc_Button_b = 32, rinc_Button_x = 64, rinc_Button_y = 128,
       rinc_Button_lb = 256, rinc_Button_rb = 512, rinc_Button_ls = 1024, rinc_Button_rs = 2048, rinc_Button_start = 4096, rinc_Button_back = 8192 };

/* set up rio in the given memory (the app space: code and data both live there), with log output
   going to put and the host procs apps call, in module pc (auto-imported; src/core/pc.rio declares
   them): cls(c), pset(x, y, c), pget(x, y) -> c, rect(x, y, w, h, c), print(s, x, y, c),
   btn(i) -> Int (1 held), time() -> Float */
int rinc_start(rio_Vm *vm, void *mem, uint32_t memsize, void (*put)(const char *s, int n));
/* just the host procs, registered as pc.cls and so on (rinc_start does this, and auto-imports pc) */
int rinc_hostProcs(rio_Vm *vm);
/* an app compiled as every Rinc compiles one (docs/spec/memory.md), after rinc_start: its source, at
   most rinc_MAX_SOURCE bytes, moved to the top of the app space (where the program's memory is still
   zero), and compiled there, the compiler below it. 0, or -1 (rio_error says why; a source too big: 1) */
#define rinc_MAX_SOURCE (64 << 10)
int rinc_compile(rio_Vm *vm, void *space, uint32_t size, const char *src, uint32_t len);
int rinc_atLimit(const char *error); /* is this compile error about running out of something? */
/* the system key (docs/spec/apps.md): the Windows key or a controller's guide button (sysDown), or,
   where the system around Rinc keeps the Windows key, Shift tapped twice (held the second time, or
   with the next key right after).
   Held, it makes combinations: with Esc (escDown: Esc, or a controller's Back), home. Call each frame
   with what's down (otherDown: any other key): 1 for home. While it's held (rinc_systemHeld), what's
   typed isn't the app's */
int rinc_systemKey(int sysDown, int shiftDown, int otherDown, int escDown);
int rinc_systemHeld(void);
/* the system overlay: a strip of Rinc's own over the screen's bottom rows, shown while the system key is
   held (what it does), apart from the app's screen: a platform shows each row as rinc_shownRow gives it */
#define rinc_OVERLAY_H 11
extern uint8_t rinc_overlay[rinc_OVERLAY_H][rinc_W];
extern volatile int rinc_overlayOn;
static inline const uint8_t *rinc_shownRow(int y) {
  return rinc_overlayOn && y >= rinc_H - rinc_OVERLAY_H ? rinc_overlay[y - (rinc_H - rinc_OVERLAY_H)] : rinc_screen[y];
}
/* one frame of a game: the app's update* and then draw* (each optional). 0: ran, 1: the app has
   neither (it isn't a game), -1: a runtime error (rio_error says) */
int rinc_frame(rio_Vm *vm);
/* starting an app, the same on every Rinc: rinc_appBegin (0, or -1), then the platform reads its source
   (into the top of the app space, where it compiles) or finds its image, then rinc_appRun (0: running, or
   done; -1). rinc_appFail is a reason it can't start (name, then why). What goes wrong goes on the screen
   (rinc_say: and to put), with the way home. Each frame after, rinc_gameFrame: a game's frame, and its
   error on the screen if it fails (0: ran, 1 nothing running, -1 it failed) */
extern int rinc_game;    /* the app has frames to run (update*, draw*) */
extern int rinc_showLog; /* what's logged goes on the screen too: while an app starts, and once it's stopped */
int rinc_appBegin(rio_Vm *vm, void *space, uint32_t size, const char *name, void (*put)(const char *s, int n));
int rinc_appRun(rio_Vm *vm, void *space, uint32_t size, const char *name, const char *src, uint32_t len, const void *image);
void rinc_appFail(const char *name, const char *why);
void rinc_say(const char *s);
int rinc_gameFrame(rio_Vm *vm);

/* sound (audio.c): rinc_VOICES voices mixed to rinc_RATE frames a second of 16-bit stereo. Host procs,
   in pc: sound(voice, data, bits, channels, rate), soundRate(voice, rate), soundVolume(voice, 0..1),
   soundPan(voice, -1..1), soundLoop(voice, start, end), soundStop(voice), soundPlaying(voice) -> Int.
   A platform pulls samples with rinc_audioMix, and sets rinc_audioLock when that runs on another
   thread or in an interrupt (called with 1 before a host proc changes a voice, 0 after) */
#define rinc_RATE 48000
#define rinc_VOICES 8
#ifdef rinc_HOT_SECTION /* where code that runs all the time goes: the RP2350 puts it in RAM, not flash */
#define rinc_HOT __attribute__((section(rinc_HOT_SECTION)))
#else
#define rinc_HOT
#endif
#define rinc_ADPCM_BLOCK 256 /* sound(..., 4, ...): ADPCM blocks of this many bytes a channel (505 frames) */
void rinc_audioMix(int16_t *stereo, int frames);
void rinc_audioReset(void); /* every voice stops */
#define rinc_VOLUME_STEPS 16
void rinc_audioVolume(int step); /* the system volume: +1 or -1 steps (2.5 dB), 0 to mute or unmute */
int rinc_audioLevel(void);       /* 0 to rinc_VOLUME_STEPS (full), or -1 muted */
extern void (*rinc_audioLock)(int on);
int rinc_audioProcs(rio_Vm *vm);

/* text: the 5x8 font (ASCII 32-126, bit 0 leftmost) in 6x9 cells, and a console of 53x26 of them
   that scrolls the screen (console.c) */
extern const uint8_t rinc_font[95][8];
extern uint8_t rinc_textFg, rinc_textBg;
void rinc_char(int c, int x, int y, int fg, int bg); /* bg < 0: transparent */
void rinc_consolePut(const char *s, int n);
void rinc_consoleClear(int bg);

/* files, where the platform has storage (else rinc_fs is NULL): one open at a time. open: 0 or -1
   (writing makes the directories along the path, and replaces the file); read: bytes read, 0 at its
   end, -1 failed; write: n, or -1; list: each entry of a directory, with whether it's one too.
   Apps live in /rinc/apps/<name>/<name>.rio */
typedef struct {
  int (*open)(const char *path, int write);
  int (*read)(char *buf, int n);
  int (*write)(const char *buf, int n);
  int (*close)(void);
  int (*list)(const char *dir, void (*each)(const char *name, int dir, void *ud), void *ud);
  int (*remove)(const char *path); /* a file, or an empty directory: 0 or -1 */
} rinc_Fs;
extern const rinc_Fs *rinc_fs;
/* an app asked to switch (pc.launch): to the app named rinc_launchName, which may be a built-in one,
   "home" or "console". The platform does it, after the frame, and sets rinc_launch back to 0 */
extern int rinc_launch;
extern char rinc_launchName[33];
/* the apps a Rinc carries with it, its bundle (docs/spec/apps.md#bundles), in one format everywhere:
   the Fruit Jam's firmware has one built in (RINC_BUNDLE: home and the console, by default), an exported
   desktop program has one at its end, and a web page has one in it. Storage, where there is some, comes
   first, by name. Each app is a source, or (image: 1) a compiled Rio image; info is its description
   (for a source, its first line, without the #) */
#define rinc_BUNDLE_MAGIC "RINCBUN1"
#define rinc_BUNDLE_MOST 256 /* (files) */
/* a bundle is files, by path as in storage without /rinc/ (apps/chord/chord.rio, apps/chord/assets/...):
   an app is apps/name/name.rio (a source), or apps/name/name.img (a compiled image, its description in
   apps/name/name.info) */
typedef struct { const char *path, *data; uint32_t len; } rinc_File;
typedef struct { char name[33], info[97]; const char *data; uint32_t len; int image; } rinc_App;
/* takes a bundle, which stays where it is: 0, or -1 if it isn't one */
int rinc_bundleRead(const void *bundle, uint32_t size);
/* writes one: name, its storage folder's name ("" for Rinc's own); home, its home app's name. Its size,
   or 0 if it doesn't fit in cap */
uint32_t rinc_bundleWrite(uint8_t *out, uint32_t cap, const char *name, const char *home, const rinc_File *files, int n);
const rinc_File *rinc_bundleFile(const char *path); /* NULL if the bundle has no such file */
const rinc_App *rinc_bundled(const char *name); /* NULL if the bundle has no such app */
const char *rinc_bundleName(void); /* the bundle's storage folder's name ("": Rinc's own) */
/* the app "home" means: an app named home in storage, else the bundle's home app; and any other name,
   itself */
const char *rinc_appFor(const char *name);
/* an app's source from storage, read into buf: its length, -1 if storage has no such app, or -2 if it
   doesn't fit */
int rinc_readApp(const char *name, char *buf, uint32_t cap);
/* the app running now, by name: its save space is /rinc/save/<name> (pc.saveRead and so on) */
void rinc_setAppName(const char *name);

/* a REPL over any text stream: feed it characters, it echoes them and runs each complete input.
   Besides rio, it knows a few commands, each a line on its own: run (the game loop, for what's been
   typed), home (back home, where there is one), and with storage: ls (the apps), load name (an app's source, as if typed, into a fresh
   session: restart gives one), run name (load, then run), save name (what's typed next, until
   Ctrl-D, becomes the app's source; Ctrl-C gives up), delete name (its source and folder, if that's
   all the folder holds) */
typedef struct {
  rio_Vm *vm;
  void (*put)(const char *s, int n);
  char acc[4096];
  int n, line0; /* acc holds the unfinished input; the current line starts at line0 */
  int echo;      /* echo typed characters (serial terminals don't; a desktop terminal already did) */
  int run;       /* the line "run" asks for the game loop: the platform runs frames while it's set */
  /* editing: the cursor (in acc), where the screen's cursor is (from line0), a key's escape sequence
     arriving, the color state at the line's end (mode, and where its unfinished word starts) */
  int cur, scr, esc, escn, mode, word, numdot; const char *wp; /* wp: the unfinished word's color now; numdot: a number's dot, for now */
  int autoind; /* spaces of the current line's auto indent that typed spaces can still take the place of */
  char hist[1024], draft[256]; int hlen, back, draftn; /* lines typed before, and the one being typed */
  int (*restart)(void); /* a fresh rio session, for load (0: done) */
  int loading, errors, saving, saved, cr; char out[256]; int nout; /* load and save as they go */
} rinc_Repl;
int rinc_replBegin(rinc_Repl *r, rio_Vm *vm, void *scratch, uint32_t size);
void rinc_replStop(rinc_Repl *r, const char *why); /* the game loop ended (why: NULL, or a message) */
void rinc_replChar(rinc_Repl *r, int c);
/* a fresh session with an app from storage, or with source from memory (home), running:
   0, or -1 (what went wrong is said) */
int rinc_replApp(rinc_Repl *r, const char *name, int run);
int rinc_replSource(rinc_Repl *r, const char *src, int len);

#endif

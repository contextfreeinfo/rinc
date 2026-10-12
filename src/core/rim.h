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
/* buttons held: bit 0 left, 1 right, 2 up, 3 down, then the face buttons, named as on an Xbox
   controller and in order of importance: 4 A (V key), 5 B (C), 6 X (X), 7 Y (Z), then the
   controller's others: 8 LB, 9 RB, 10 left stick, 11 right stick (pressed in), 12 Start, 13 Back.
   Each platform sets them from its keys and controllers before a frame */
extern uint16_t rim_buttons;
/* a controller's sticks and triggers, also set by the platform: 0 left stick x, 1 its y, 2 and 3 the
   right stick's (-32767 to 32767; y is + down, as on the screen), 4 left trigger, 5 right (0 to 32767) */
extern int16_t rim_axes[6];
/* typed text, for a cart's key(): the platform passes each character typed while a cart runs
   (printable ASCII, 13 Enter, 8 Backspace; terminal escapes are skipped) */
void rim_text_push(int c);
enum { RIM_LEFT = 1, RIM_RIGHT = 2, RIM_UP = 4, RIM_DOWN = 8, RIM_A = 16, RIM_B = 32, RIM_X = 64, RIM_Y = 128,
       RIM_LB = 256, RIM_RB = 512, RIM_LS = 1024, RIM_RS = 2048, RIM_START = 4096, RIM_BACK = 8192 };

/* set up rio in the given memory (the app space: code and data both live there), with log output
   going to put and the host procs carts call: cls(c), pset(x, y, c), pget(x, y) -> c,
   rect(x, y, w, h, c), print(s, x, y, c), btn(i) -> Int (1 held), time() -> Float */
int rim_start(Rio *vm, void *mem, uint32_t memsize, void (*put)(const char *s, int n));
/* one frame of a game: the cart's update* and then draw* (each optional). 0: ran, 1: the cart has
   neither (it isn't a game), -1: a runtime error (rio_error says) */
int rim_frame(Rio *vm);

/* sound (audio.c): RIM_VOICES voices mixed to RIM_RATE frames a second of 16-bit stereo. Host procs:
   sound(voice, data, bits, channels, rate), soundRate(voice, rate), soundVolume(voice, 0..1),
   soundPan(voice, -1..1), soundLoop(voice, start, end), soundStop(voice), soundPlaying(voice) -> Int.
   A platform pulls samples with rim_audio_mix, and sets rim_audio_lock when that runs on another
   thread or in an interrupt (called with 1 before a host proc changes a voice, 0 after) */
#define RIM_RATE 48000
#define RIM_VOICES 8
#ifdef RIM_HOT_SECTION /* where code that runs all the time goes: the RP2350 puts it in RAM, not flash */
#define RIM_HOT __attribute__((section(RIM_HOT_SECTION)))
#else
#define RIM_HOT
#endif
#define RIM_ADPCM_BLOCK 256 /* sound(..., 4, ...): ADPCM blocks of this many bytes a channel (505 frames) */
void rim_audio_mix(int16_t *stereo, int frames);
void rim_audio_reset(void); /* every voice stops */
#define RIM_VOLUME_STEPS 16
void rim_audio_volume(int step); /* the system volume: +1 or -1 steps (2.5 dB), 0 to mute or unmute */
int rim_audio_level(void);       /* 0 to RIM_VOLUME_STEPS (full), or -1 muted */
extern void (*rim_audio_lock)(int on);
int rim_audio_procs(Rio *vm);

/* text: the 5x8 font (ASCII 32-126, bit 0 leftmost) in 6x9 cells, and a console of 53x26 of them
   that scrolls the screen (console.c) */
extern const uint8_t rim_font[95][8];
extern uint8_t rim_text_fg, rim_text_bg;
void rim_char(int c, int x, int y, int fg, int bg); /* bg < 0: transparent */
void rim_console_put(const char *s, int n);
void rim_console_clear(int bg);

/* files, where the platform has storage (else rim_fs is NULL): one open at a time. open: 0 or -1
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
} RimFs;
extern const RimFs *rim_fs;
/* a cart (the launcher) asked to switch: 1 to the app named rim_launch_name, 2 to the console; or the
   console's home command, 3: back to the launcher. The platform does it (after the frame) and sets it
   back to 0 */
extern int rim_launch;
extern char rim_launch_name[33];

/* a REPL over any text stream: feed it characters, it echoes them and runs each complete input.
   Besides rio, it knows a few commands, each a line on its own: run (the game loop, for what's been
   typed), home (back to the launcher, where there is one), and with storage: ls (the apps), load name (an app's source, as if typed, into a fresh
   session: restart gives one), run name (load, then run), save name (what's typed next, until
   Ctrl-D, becomes the app's source; Ctrl-C gives up), delete name (its source and folder, if that's
   all the folder holds) */
typedef struct {
  Rio *vm;
  void (*put)(const char *s, int n);
  char acc[4096];
  int n, line0; /* acc holds the unfinished input; the current line starts at line0 */
  int echo;      /* echo typed characters (serial terminals don't; a desktop terminal already did) */
  int run;       /* the line "run" asks for the game loop: the platform runs frames while it's set */
  /* editing: the cursor (in acc), where the screen's cursor is (from line0), a key's escape sequence
     arriving, the color state at the line's end (mode, and where its unfinished word starts) */
  int cur, scr, esc, escn, mode, word; const char *wp; /* wp: the unfinished word's color now */
  char hist[1024], draft[256]; int hlen, back, draftn; /* lines typed before, and the one being typed */
  int (*restart)(void); /* a fresh rio session, for load (0: done) */
  int loading, errors, saving, saved, cr; char out[256]; int nout; /* load and save as they go */
} RimRepl;
int rim_repl_begin(RimRepl *r, Rio *vm, void *scratch, uint32_t size);
void rim_repl_stop(RimRepl *r, const char *why); /* the game loop ended (why: NULL, or a message) */
void rim_repl_char(RimRepl *r, int c);
/* a fresh session with an app from storage, or with source from memory (the launcher), running:
   0, or -1 (what went wrong is said) */
int rim_repl_app(RimRepl *r, const char *name, int run);
int rim_repl_source(RimRepl *r, const char *src, int len);

#endif

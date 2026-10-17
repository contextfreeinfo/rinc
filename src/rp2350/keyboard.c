/* USB keyboards on the Fruit Jam's USB-A ports: TinyUSB's host stack over PIO-USB (D+ on GPIO 1, D- on
   GPIO 2, port power switched by GPIO 11), boot-protocol reports. Keys become characters for the REPL
   (with repeat when held), and the set of keys held down is kept for games. A keyboard's volume knob
   or keys (HID consumer control: volume up, down, mute, sent as 16-bit usages, the usual way) set the
   system volume. */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pio_usb.h"
#include "tusb.h"
#include "keyboard.h"
#include "gamepad.h"
#include "rinc.h"

#define rinc_REPEAT_DELAY_US 500000
#define rinc_REPEAT_EVERY_US 40000

static const uint8_t rinc_ascii[128][2] = {HID_KEYCODE_TO_ASCII};
static void rinc_note(const char *fmt, unsigned a, unsigned b, unsigned c);
static char rinc_queue[64];
static volatile unsigned rinc_qhead, rinc_qtail;
static hid_keyboard_report_t rinc_last;
static uint8_t rinc_repeatKey, rinc_repeatMods; static uint64_t rinc_repeatAt;
uint8_t rinc_keysDown[32]; /* bit k: HID usage k is held */

static void rinc_push(int c) {
  unsigned next = (rinc_qhead + 1) % sizeof rinc_queue;
  if (next != rinc_qtail) { rinc_queue[rinc_qhead] = (char)c; rinc_qhead = next; }
}
/* editing keys, as xterm sends them, with the modifiers held (m: 1 + 1 Shift, 2 Alt, 4 Ctrl): ESC [ and a
   letter (arrows, Home, End: ESC [ 1 ; m C with modifiers), or n ~ (Delete, Page Up and Down: n ; m ~).
   And the keys a terminal sends no differently with modifiers: Shift+Tab as ESC [ Z, and Tab, Enter,
   Backspace and Ctrl+letter with other modifiers as ESC [ 27 ; m ; key ~ */
static int rinc_editing(uint8_t key, uint8_t mods) {
  int shift = (mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
  int ctrl = (mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;
  int alt = (mods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) != 0;
  int m = 1 + shift + 2 * alt + 4 * ctrl, n = 0, k = 0;
  char letter = key == HID_KEY_ARROW_UP ? 'A' : key == HID_KEY_ARROW_DOWN ? 'B' : key == HID_KEY_ARROW_RIGHT ? 'C' :
    key == HID_KEY_ARROW_LEFT ? 'D' : key == HID_KEY_HOME ? 'H' : key == HID_KEY_END ? 'F' : 0;
  char seq[24];
  if (key == HID_KEY_DELETE) n = 3;
  else if (key == HID_KEY_PAGE_UP) n = 5;
  else if (key == HID_KEY_PAGE_DOWN) n = 6;
  else if (key == HID_KEY_TAB && shift && !ctrl && !alt) letter = 'Z';
  else if (key == HID_KEY_TAB && (ctrl || alt)) k = 9;
  else if ((key == HID_KEY_ENTER || key == HID_KEY_KEYPAD_ENTER) && (ctrl || alt)) k = 13;
  else if (key == HID_KEY_BACKSPACE && (ctrl || alt)) k = 8;
  else if (key >= HID_KEY_A && key <= HID_KEY_Z && ctrl && (shift || alt)) k = 'a' + key - HID_KEY_A;
  if (letter == 'Z') snprintf(seq, sizeof seq, "[Z");
  else if (letter && m > 1) snprintf(seq, sizeof seq, "[1;%d%c", m, letter);
  else if (letter) snprintf(seq, sizeof seq, "[%c", letter);
  else if (n) snprintf(seq, sizeof seq, m > 1 ? "[%d;%d~" : "[%d~", n, m);
  else if (k) snprintf(seq, sizeof seq, "[27;%d;%d~", m, k);
  else return 0;
  rinc_push(27);
  for (k = 0; seq[k]; k++) rinc_push(seq[k]);
  return 1;
}
/* a key press as a character: ASCII, Enter as \r, Backspace as 8, Ctrl+letter as its control code */
static int rinc_character(uint8_t key, uint8_t mods) {
  int shift = mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT), c;
  if (key == HID_KEY_ENTER || key == HID_KEY_KEYPAD_ENTER) return '\r';
  if (key == HID_KEY_BACKSPACE) return 8;
  if (key >= 128) return -1;
  c = rinc_ascii[key][shift ? 1 : 0];
  if (!c) return -1;
  if (mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 1;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
  }
  return c == '\n' ? '\r' : c;
}

static void rinc_report(const hid_keyboard_report_t *r) {
  int i, j;
  memset(rinc_keysDown, 0, sizeof rinc_keysDown);
  rinc_keysDown[0xE0 >> 3] = r->modifier; /* (the modifiers, Ctrl Shift Alt and GUI left then right, are keys 0xE0-0xE7: a byte of their own) */
  for (i = 0; i < 6; i++) {
    uint8_t k = r->keycode[i], was = 0;
    if (!k) continue;
    rinc_keysDown[k >> 3] |= (uint8_t)(1u << (k & 7));
    for (j = 0; j < 6; j++) if (rinc_last.keycode[j] == k) was = 1;
    if (!was) { /* newly pressed: type it, and repeat it if held */
      int c = rinc_editing(k, r->modifier) ? -1 : rinc_character(k, r->modifier);
      if (c >= 0) rinc_push(c);
      rinc_repeatKey = k; rinc_repeatMods = r->modifier; rinc_repeatAt = time_us_64() + rinc_REPEAT_DELAY_US;
    }
  }
  for (i = 0; i < 6 && rinc_repeatKey; i++) if (r->keycode[i] == rinc_repeatKey) break;
  if (i == 6) rinc_repeatKey = 0; /* let go */
  rinc_last = *r;
}

/* consumer control (volume) reports: for each device's HID interface, its report id + 1 (1: the
   reports have no id), or 0 for none; and which of up, down, mute were held last time */
#define rinc_NDEV (CFG_TUH_DEVICE_MAX + 2)
static uint8_t rinc_consumer[rinc_NDEV][CFG_TUH_HID], rinc_held[rinc_NDEV][CFG_TUH_HID];
static void rinc_consumerReport(uint8_t dev, uint8_t instance, const uint8_t *r, uint16_t len) {
  int id = rinc_consumer[dev][instance] - 1, now = 0, i;
  if (id > 0) { if (!len || r[0] != id) return; r++; len--; }
  for (i = 0; i + 1 < len; i += 2) { /* (16-bit usages, pressed) */
    int u = r[i] | r[i + 1] << 8;
    now |= u == HID_USAGE_CONSUMER_VOLUME_INCREMENT ? 1 : u == HID_USAGE_CONSUMER_VOLUME_DECREMENT ? 2 : u == HID_USAGE_CONSUMER_MUTE ? 4 : 0;
  }
  i = now & ~rinc_held[dev][instance]; /* newly pressed */
  if (i & 1) rinc_audioVolume(1);
  if (i & 2) rinc_audioVolume(-1);
  if (i & 4) rinc_audioVolume(0);
  rinc_held[dev][instance] = (uint8_t)now;
}

void tuh_hid_mount_cb(uint8_t dev, uint8_t instance, uint8_t const *desc, uint16_t len) {
  tuh_hid_report_info_t info[8]; uint8_t p = tuh_hid_interface_protocol(dev, instance), n, k;
  rinc_note("[usb] device %u: hid %u, protocol %u (1 keyboard, 2 mouse)\r\n", dev, instance, p);
  if (dev < rinc_NDEV && instance < CFG_TUH_HID) {
    rinc_consumer[dev][instance] = rinc_held[dev][instance] = 0;
    n = tuh_hid_parse_report_descriptor(info, 8, desc, len);
    for (k = 0; k < n; k++)
      if (info[k].usage_page == HID_USAGE_PAGE_CONSUMER && info[k].usage == HID_USAGE_CONSUMER_CONTROL) {
        rinc_consumer[dev][instance] = (uint8_t)(info[k].report_id + 1);
        rinc_note("[usb] device %u: hid %u has volume keys\r\n", dev, instance, 0);
      }
  }
  /* (a mouse interface that carries the volume keys too has been set to the boot protocol, which has
     only the mouse: once the whole device is set up, it goes back to full reports; see
     tuh_mount_cb) */
  if (p == HID_ITF_PROTOCOL_KEYBOARD || (dev < rinc_NDEV && instance < CFG_TUH_HID && rinc_consumer[dev][instance] && p != HID_ITF_PROTOCOL_MOUSE))
    tuh_hid_receive_report(dev, instance);
}
/* the next of a device's volume interfaces that needs full reports (one request at a time) */
static void rinc_fullReports(uint8_t dev, uint8_t from) {
  uint8_t i;
  for (i = from; dev < rinc_NDEV && i < CFG_TUH_HID; i++)
    if (rinc_consumer[dev][i] && tuh_hid_interface_protocol(dev, i) == HID_ITF_PROTOCOL_MOUSE) {
      if (tuh_hid_set_protocol(dev, i, HID_PROTOCOL_REPORT)) return;
      tuh_hid_receive_report(dev, i);
    }
}
void tuh_hid_set_protocol_complete_cb(uint8_t dev, uint8_t instance, uint8_t protocol) {
  (void)protocol; tuh_hid_receive_report(dev, instance); rinc_fullReports(dev, (uint8_t)(instance + 1));
}
void tuh_hid_umount_cb(uint8_t dev, uint8_t instance) {
  if (dev < rinc_NDEV && instance < CFG_TUH_HID) rinc_consumer[dev][instance] = 0;
  rinc_repeatKey = 0; memset(rinc_keysDown, 0, sizeof rinc_keysDown);
}
void tuh_hid_report_received_cb(uint8_t dev, uint8_t instance, uint8_t const *r, uint16_t len) {
  if (tuh_hid_interface_protocol(dev, instance) == HID_ITF_PROTOCOL_KEYBOARD && len >= sizeof(hid_keyboard_report_t))
    rinc_report((const hid_keyboard_report_t *)(const void *)r);
  else if (dev < rinc_NDEV && instance < CFG_TUH_HID && rinc_consumer[dev][instance]) rinc_consumerReport(dev, instance, r, len);
  tuh_hid_receive_report(dev, instance);
}

/* before stdio_init_all: starting the USB serial port starts TinyUSB, host side included */
void rinc_keyboardSetup(void) {
  static pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
  gpio_init(11); gpio_set_dir(11, GPIO_OUT); gpio_put(11, 1); /* power the USB-A ports */
  cfg.pin_dp = 1;
  cfg.tx_ch = 11; /* a DMA channel well past the video's */
  tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg);
}
void rinc_keyboardStart(void) { rinc_padNotes(rinc_note); if (!tuh_inited()) tuh_init(BOARD_TUH_RHPORT); }

/* what's plugged in: kept, since it happens before a serial terminal can be there to see it */
static char rinc_events[512]; static int rinc_nevents;
static void rinc_note(const char *fmt, unsigned a, unsigned b, unsigned c) {
  int n = snprintf(rinc_events + rinc_nevents, sizeof rinc_events - (size_t)rinc_nevents, fmt, a, b, c);
  if (n > 0 && rinc_nevents + n < (int)sizeof rinc_events) rinc_nevents += n;
}
const char *rinc_keyboardEvents(void) { rinc_events[rinc_nevents] = 0; return rinc_events; }
void tuh_mount_cb(uint8_t dev) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(dev, &vid, &pid);
  rinc_note("[usb] device %u: %04x:%04x\r\n", dev, vid, pid);
  rinc_fullReports(dev, 0);
}
void tuh_umount_cb(uint8_t dev) { rinc_note("[usb] device %u gone\r\n", dev, 0, 0); }

void rinc_keyboardPoll(void) { tuh_task(); rinc_padTask(); }
int rinc_keyboardChar(void) {
  int c;
  rinc_keyboardPoll();
  if (rinc_repeatKey && time_us_64() >= rinc_repeatAt) {
    int r = rinc_editing(rinc_repeatKey, rinc_repeatMods) ? -1 : rinc_character(rinc_repeatKey, rinc_repeatMods);
    if (r >= 0) rinc_push(r);
    rinc_repeatAt = time_us_64() + rinc_REPEAT_EVERY_US;
  }
  if (rinc_qtail == rinc_qhead) return -1;
  c = (unsigned char)rinc_queue[rinc_qtail]; rinc_qtail = (rinc_qtail + 1) % sizeof rinc_queue;
  return c;
}

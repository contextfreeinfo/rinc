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
#include "rim.h"

#define REPEAT_DELAY_US 500000
#define REPEAT_EVERY_US 40000

static const uint8_t ascii[128][2] = {HID_KEYCODE_TO_ASCII};
static void note(const char *fmt, unsigned a, unsigned b, unsigned c);
static char queue[64];
static volatile unsigned qhead, qtail;
static hid_keyboard_report_t last;
static uint8_t repeat_key, repeat_mods; static uint64_t repeat_at;
uint8_t rim_keys_down[32]; /* bit k: HID usage k is held */

static void push(int c) {
  unsigned next = (qhead + 1) % sizeof queue;
  if (next != qtail) { queue[qhead] = (char)c; qhead = next; }
}
/* a key press as a character: ASCII, Enter as \r, Backspace as 8, Ctrl+letter as its control code */
static int character(uint8_t key, uint8_t mods) {
  int shift = mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT), c;
  if (key == HID_KEY_ENTER || key == HID_KEY_KEYPAD_ENTER) return '\r';
  if (key == HID_KEY_BACKSPACE) return 8;
  if (key >= 128) return -1;
  c = ascii[key][shift ? 1 : 0];
  if (!c) return -1;
  if (mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 1;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
  }
  return c == '\n' ? '\r' : c;
}

static void report(const hid_keyboard_report_t *r) {
  int i, j;
  memset(rim_keys_down, 0, sizeof rim_keys_down);
  for (i = 0; i < 6; i++) {
    uint8_t k = r->keycode[i], was = 0;
    if (!k) continue;
    rim_keys_down[k >> 3] |= (uint8_t)(1u << (k & 7));
    for (j = 0; j < 6; j++) if (last.keycode[j] == k) was = 1;
    if (!was) { /* newly pressed: type it, and repeat it if held */
      int c = character(k, r->modifier);
      if (c >= 0) push(c);
      repeat_key = k; repeat_mods = r->modifier; repeat_at = time_us_64() + REPEAT_DELAY_US;
    }
  }
  for (i = 0; i < 6 && repeat_key; i++) if (r->keycode[i] == repeat_key) break;
  if (i == 6) repeat_key = 0; /* let go */
  last = *r;
}

/* consumer control (volume) reports: for each device's HID interface, its report id + 1 (1: the
   reports have no id), or 0 for none; and which of up, down, mute were held last time */
#define NDEV (CFG_TUH_DEVICE_MAX + 2)
static uint8_t consumer[NDEV][CFG_TUH_HID], held[NDEV][CFG_TUH_HID];
static void consumer_report(uint8_t dev, uint8_t instance, const uint8_t *r, uint16_t len) {
  int id = consumer[dev][instance] - 1, now = 0, i;
  if (id > 0) { if (!len || r[0] != id) return; r++; len--; }
  for (i = 0; i + 1 < len; i += 2) { /* (16-bit usages, pressed) */
    int u = r[i] | r[i + 1] << 8;
    now |= u == HID_USAGE_CONSUMER_VOLUME_INCREMENT ? 1 : u == HID_USAGE_CONSUMER_VOLUME_DECREMENT ? 2 : u == HID_USAGE_CONSUMER_MUTE ? 4 : 0;
  }
  i = now & ~held[dev][instance]; /* newly pressed */
  if (i & 1) rim_audio_volume(1);
  if (i & 2) rim_audio_volume(-1);
  if (i & 4) rim_audio_volume(0);
  held[dev][instance] = (uint8_t)now;
}

void tuh_hid_mount_cb(uint8_t dev, uint8_t instance, uint8_t const *desc, uint16_t len) {
  tuh_hid_report_info_t info[8]; uint8_t p = tuh_hid_interface_protocol(dev, instance), n, k;
  note("[usb] device %u: hid %u, protocol %u (1 keyboard, 2 mouse)\r\n", dev, instance, p);
  if (dev < NDEV && instance < CFG_TUH_HID) {
    consumer[dev][instance] = held[dev][instance] = 0;
    n = tuh_hid_parse_report_descriptor(info, 8, desc, len);
    for (k = 0; k < n; k++)
      if (info[k].usage_page == HID_USAGE_PAGE_CONSUMER && info[k].usage == HID_USAGE_CONSUMER_CONTROL) {
        consumer[dev][instance] = (uint8_t)(info[k].report_id + 1);
        note("[usb] device %u: hid %u has volume keys\r\n", dev, instance, 0);
      }
  }
  /* (a mouse interface that carries the volume keys too has been set to the boot protocol, which has
     only the mouse: once the whole device is set up, it goes back to full reports; see
     tuh_mount_cb) */
  if (p == HID_ITF_PROTOCOL_KEYBOARD || (dev < NDEV && instance < CFG_TUH_HID && consumer[dev][instance] && p != HID_ITF_PROTOCOL_MOUSE))
    tuh_hid_receive_report(dev, instance);
}
/* the next of a device's volume interfaces that needs full reports (one request at a time) */
static void full_reports(uint8_t dev, uint8_t from) {
  uint8_t i;
  for (i = from; dev < NDEV && i < CFG_TUH_HID; i++)
    if (consumer[dev][i] && tuh_hid_interface_protocol(dev, i) == HID_ITF_PROTOCOL_MOUSE) {
      if (tuh_hid_set_protocol(dev, i, HID_PROTOCOL_REPORT)) return;
      tuh_hid_receive_report(dev, i);
    }
}
void tuh_hid_set_protocol_complete_cb(uint8_t dev, uint8_t instance, uint8_t protocol) {
  (void)protocol; tuh_hid_receive_report(dev, instance); full_reports(dev, (uint8_t)(instance + 1));
}
void tuh_hid_umount_cb(uint8_t dev, uint8_t instance) {
  if (dev < NDEV && instance < CFG_TUH_HID) consumer[dev][instance] = 0;
  repeat_key = 0; memset(rim_keys_down, 0, sizeof rim_keys_down);
}
void tuh_hid_report_received_cb(uint8_t dev, uint8_t instance, uint8_t const *r, uint16_t len) {
  if (tuh_hid_interface_protocol(dev, instance) == HID_ITF_PROTOCOL_KEYBOARD && len >= sizeof(hid_keyboard_report_t))
    report((const hid_keyboard_report_t *)(const void *)r);
  else if (dev < NDEV && instance < CFG_TUH_HID && consumer[dev][instance]) consumer_report(dev, instance, r, len);
  tuh_hid_receive_report(dev, instance);
}

/* before stdio_init_all: starting the USB serial port starts TinyUSB, host side included */
void rim_keyboard_setup(void) {
  static pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
  gpio_init(11); gpio_set_dir(11, GPIO_OUT); gpio_put(11, 1); /* power the USB-A ports */
  cfg.pin_dp = 1;
  cfg.tx_ch = 11; /* a DMA channel well past the video's */
  tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg);
}
void rim_keyboard_start(void) { rim_pad_notes(note); if (!tuh_inited()) tuh_init(BOARD_TUH_RHPORT); }

/* what's plugged in: kept, since it happens before a serial terminal can be there to see it */
static char events[512]; static int nevents;
static void note(const char *fmt, unsigned a, unsigned b, unsigned c) {
  int n = snprintf(events + nevents, sizeof events - (size_t)nevents, fmt, a, b, c);
  if (n > 0 && nevents + n < (int)sizeof events) nevents += n;
}
const char *rim_keyboard_events(void) { events[nevents] = 0; return events; }
void tuh_mount_cb(uint8_t dev) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(dev, &vid, &pid);
  note("[usb] device %u: %04x:%04x\r\n", dev, vid, pid);
  full_reports(dev, 0);
}
void tuh_umount_cb(uint8_t dev) { note("[usb] device %u gone\r\n", dev, 0, 0); }

int rim_keyboard_char(void) {
  int c;
  tuh_task(); rim_pad_task();
  if (repeat_key && time_us_64() >= repeat_at) {
    int r = character(repeat_key, repeat_mods);
    if (r >= 0) push(r);
    repeat_at = time_us_64() + REPEAT_EVERY_US;
  }
  if (qtail == qhead) return -1;
  c = (unsigned char)queue[qtail]; qtail = (qtail + 1) % sizeof queue;
  return c;
}

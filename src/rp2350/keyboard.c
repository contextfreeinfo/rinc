/* USB keyboards on the Fruit Jam's USB-A ports: TinyUSB's host stack over PIO-USB (D+ on GPIO 1, D- on
   GPIO 2, port power switched by GPIO 11), boot-protocol reports. Keys become characters for the REPL
   (with repeat when held), and the set of keys held down is kept for games. */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pio_usb.h"
#include "tusb.h"
#include "keyboard.h"

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

void tuh_hid_mount_cb(uint8_t dev, uint8_t instance, uint8_t const *desc, uint16_t len) {
  (void)desc; (void)len;
  uint8_t p = tuh_hid_interface_protocol(dev, instance);
  note("[usb] device %u: hid %u, protocol %u (1 keyboard, 2 mouse)\r\n", dev, instance, p);
  if (p == HID_ITF_PROTOCOL_KEYBOARD) tuh_hid_receive_report(dev, instance);
}
void tuh_hid_umount_cb(uint8_t dev, uint8_t instance) { (void)dev; (void)instance; repeat_key = 0; memset(rim_keys_down, 0, sizeof rim_keys_down); }
void tuh_hid_report_received_cb(uint8_t dev, uint8_t instance, uint8_t const *r, uint16_t len) {
  if (tuh_hid_interface_protocol(dev, instance) == HID_ITF_PROTOCOL_KEYBOARD && len >= sizeof(hid_keyboard_report_t))
    report((const hid_keyboard_report_t *)(const void *)r);
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
void rim_keyboard_start(void) { if (!tuh_inited()) tuh_init(BOARD_TUH_RHPORT); }

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
}
void tuh_umount_cb(uint8_t dev) { note("[usb] device %u gone\r\n", dev, 0, 0); }

int rim_keyboard_char(void) {
  int c;
  tuh_task();
  if (repeat_key && time_us_64() >= repeat_at) {
    int r = character(repeat_key, repeat_mods);
    if (r >= 0) push(r);
    repeat_at = time_us_64() + REPEAT_EVERY_US;
  }
  if (qtail == qhead) return -1;
  c = (unsigned char)queue[qtail]; qtail = (qtail + 1) % sizeof queue;
  return c;
}

/* Xbox controllers on the Fruit Jam's USB-A ports, wired: a TinyUSB host class driver for their two
   protocols, told apart by the interface they offer (so a controller is recognized whatever its maker):
     360 (XInput): class 0xFF, subclass 0x5D, protocol 0x01. 20-byte reports: buttons, triggers, sticks.
     One and Series (GIP): class 0xFF, subclass 0x47, protocol 0xD0. Silent until told to power on;
       then input messages (0x20) with the same things in another layout, and the Xbox button apart (0x07).
   The first controller plugged in is player 1's: its buttons join the keyboard's in rim_buttons, and
   its sticks and triggers go to rim_axes. The Xbox button is kept for the system (rim_pad_guide). */
#include <string.h>
#include "tusb.h"
#include "host/usbh_pvt.h"
#include "rim.h"
#include "gamepad.h"

#define PADS 4
enum { NONE, X360, XONE };
typedef struct {
  uint8_t dev, itf, kind, ep_in, ep_out, seq; uint16_t in_size;
  uint16_t buttons; int16_t axes[6]; uint8_t guide;
  CFG_TUH_MEM_ALIGN uint8_t in[64], out[64];
} Pad;
CFG_TUH_MEM_SECTION static Pad pads[PADS];
uint16_t rim_pad_buttons; uint8_t rim_pad_guide, rim_pad_count;
static void (*note_fn)(const char *fmt, unsigned a, unsigned b, unsigned c);
void rim_pad_notes(void (*note)(const char *fmt, unsigned a, unsigned b, unsigned c)) { note_fn = note; }
#define NOTE(f, a, b, c) do { if (note_fn) note_fn(f, a, b, c); } while (0)

static Pad *find(uint8_t dev, uint8_t ep) {
  int k;
  for (k = 0; k < PADS; k++) if (pads[k].kind && pads[k].dev == dev && (!ep || pads[k].ep_in == ep || pads[k].ep_out == ep)) return &pads[k];
  return 0;
}
static bool receive(Pad *p) {
  if (!usbh_edpt_claim(p->dev, p->ep_in)) return false;
  if (!usbh_edpt_xfer(p->dev, p->ep_in, p->in, p->in_size)) { usbh_edpt_release(p->dev, p->ep_in); return false; }
  return true;
}
static bool send(Pad *p, const uint8_t *msg, uint16_t len) {
  if (!p->ep_out || !usbh_edpt_claim(p->dev, p->ep_out)) return false;
  memcpy(p->out, msg, len);
  if (!usbh_edpt_xfer(p->dev, p->ep_out, p->out, len)) { usbh_edpt_release(p->dev, p->ep_out); return false; }
  return true;
}
static int16_t rd16(const uint8_t *b) { return (int16_t)(b[0] | b[1] << 8); }
static int16_t flip(int16_t v) { return (int16_t)(v == -32768 ? 32767 : -v); } /* (up is + on the controller, - on the screen) */
/* each 360 button bit, then each One one: as Rim's */
static const uint16_t x360_map[16] = {RIM_UP, RIM_DOWN, RIM_LEFT, RIM_RIGHT, RIM_START, RIM_BACK, RIM_LS, RIM_RS,
  RIM_LB, RIM_RB, 0 /* (Xbox button) */, 0, RIM_A, RIM_B, RIM_X, RIM_Y};
static const uint16_t xone_map[16] = {0, 0, RIM_START, RIM_BACK, RIM_A, RIM_B, RIM_X, RIM_Y,
  RIM_UP, RIM_DOWN, RIM_LEFT, RIM_RIGHT, RIM_LB, RIM_RB, RIM_LS, RIM_RS};
static uint16_t mapped(uint16_t bits, const uint16_t *map) { uint16_t b = 0; int i; for (i = 0; i < 16; i++) if (bits >> i & 1) b |= map[i]; return b; }

static void input(Pad *p, const uint8_t *r, uint32_t n) {
  if (p->kind == X360 && n >= 14 && r[0] == 0x00) {
    uint16_t bits = (uint16_t)(r[2] | r[3] << 8);
    p->buttons = mapped(bits, x360_map); p->guide = bits >> 10 & 1;
    p->axes[4] = (int16_t)(r[4] * 32767 / 255); p->axes[5] = (int16_t)(r[5] * 32767 / 255);
    p->axes[0] = rd16(r + 6); p->axes[1] = flip(rd16(r + 8)); p->axes[2] = rd16(r + 10); p->axes[3] = flip(rd16(r + 12));
  } else if (p->kind == XONE && n >= 18 && r[0] == 0x20) {
    p->buttons = mapped((uint16_t)(r[4] | r[5] << 8), xone_map);
    p->axes[4] = (int16_t)((r[6] | r[7] << 8) * 32767 / 1023); p->axes[5] = (int16_t)((r[8] | r[9] << 8) * 32767 / 1023);
    p->axes[0] = rd16(r + 10); p->axes[1] = flip(rd16(r + 12)); p->axes[2] = rd16(r + 14); p->axes[3] = flip(rd16(r + 16));
  } else if (p->kind == XONE && n >= 5 && r[0] == 0x07) {
    p->guide = r[4] & 1;
    if (r[1] & 0x10) { const uint8_t ack[] = {0x01, 0x20, r[2], 0x09, 0x00, 0x07, 0x20, 0x02, 0, 0, 0, 0, 0}; send(p, ack, sizeof ack); }
  }
}

/* the class driver */
static bool pad_init(void) { memset(pads, 0, sizeof pads); return true; }
static bool pad_deinit(void) { return true; }
static bool pad_open(uint8_t rhport, uint8_t dev, tusb_desc_interface_t const *itf, uint16_t max_len) {
  const uint8_t *d = (const uint8_t *)itf, *end = d + max_len; Pad *p = 0; int k, kind = NONE, eps = 0;
  (void)rhport;
  if (itf->bInterfaceClass == 0xFF && itf->bInterfaceSubClass == 0x5D && itf->bInterfaceProtocol == 0x01) kind = X360;
  if (itf->bInterfaceClass == 0xFF && itf->bInterfaceSubClass == 0x47 && itf->bInterfaceProtocol == 0xD0) kind = XONE;
  NOTE("[usb] device %u: interface class %02x subclass %02x", dev, itf->bInterfaceClass, itf->bInterfaceSubClass);
  NOTE(" protocol %02x\r\n", itf->bInterfaceProtocol, 0, 0);
  if (kind == NONE || itf->bNumEndpoints < 1) return false;
  if (kind == XONE && find(dev, 0)) return false; /* (a One's other interfaces: audio and such) */
  for (k = 0; k < PADS && !p; k++) if (!pads[k].kind) p = &pads[k];
  if (!p) return false;
  memset(p, 0, sizeof *p);
  for (d = tu_desc_next(d); d < end && eps < itf->bNumEndpoints; d = tu_desc_next(d)) {
    if (tu_desc_type(d) == TUSB_DESC_INTERFACE) break;
    if (tu_desc_type(d) == TUSB_DESC_ENDPOINT) {
      const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)(const void *)d;
      if (ep->bmAttributes.xfer != TUSB_XFER_INTERRUPT || !tuh_edpt_open(dev, ep)) continue;
      if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) { p->ep_in = ep->bEndpointAddress; p->in_size = tu_edpt_packet_size(ep); }
      else p->ep_out = ep->bEndpointAddress;
      eps++;
    }
  }
  if (!p->ep_in) return false;
  if (p->in_size > sizeof p->in) p->in_size = sizeof p->in;
  p->dev = dev; p->itf = itf->bInterfaceNumber; p->kind = (uint8_t)kind;
  NOTE(kind == X360 ? "[usb] device %u: an Xbox 360 controller\r\n" : "[usb] device %u: an Xbox One controller\r\n", dev, 0, 0);
  return true;
}
static bool pad_set_config(uint8_t dev, uint8_t itf) {
  Pad *p = find(dev, 0);
  if (p && p->itf == itf) {
    if (p->kind == X360) { const uint8_t led[] = {0x01, 0x03, 0x06}; send(p, led, sizeof led); } /* (the ring: player 1) */
    receive(p);
    rim_pad_count++;
  }
  usbh_driver_set_config_complete(dev, itf);
  return true;
}
static bool pad_xfer(uint8_t dev, uint8_t ep, xfer_result_t result, uint32_t n) {
  Pad *p = find(dev, ep);
  if (!p) return false;
  if (ep == p->ep_in) {
    if (result == XFER_RESULT_SUCCESS) input(p, p->in, n);
    receive(p);
  }
  return true;
}
static void pad_close(uint8_t dev) {
  int k;
  for (k = 0; k < PADS; k++) if (pads[k].kind && pads[k].dev == dev) { pads[k].kind = NONE; rim_pad_count--; }
}
static const usbh_class_driver_t driver = {
  .name = "XBOX", .init = pad_init, .deinit = pad_deinit, .open = pad_open, .set_config = pad_set_config,
  .xfer_cb = pad_xfer, .close = pad_close};
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *count) { *count = 1; return &driver; }

/* once set up: a One needs telling to start (after the 360's LED message, nothing else waits) */
void rim_pad_task(void) {
  int k;
  for (k = 0; k < PADS; k++) {
    Pad *p = &pads[k];
    if (p->kind == XONE && p->seq == 0 && p->ep_out && !usbh_edpt_busy(p->dev, p->ep_out)) {
      const uint8_t power_on[] = {0x05, 0x20, 0x00, 0x01, 0x00};
      if (send(p, power_on, sizeof power_on)) p->seq = 1;
    }
  }
}

/* player 1: the first controller there */
void rim_pad_read(void) {
  int k;
  rim_pad_buttons = 0; rim_pad_guide = 0; memset(rim_axes, 0, sizeof rim_axes);
  for (k = 0; k < PADS; k++)
    if (pads[k].kind) { rim_pad_buttons = pads[k].buttons; rim_pad_guide = pads[k].guide; memcpy(rim_axes, pads[k].axes, sizeof rim_axes); break; }
}

/* Xbox controllers on the Fruit Jam's USB-A ports, wired: a TinyUSB host class driver for their two
   protocols, told apart by the interface they offer (so a controller is recognized whatever its maker):
     360 (XInput): class 0xFF, subclass 0x5D, protocol 0x01. 20-byte reports: buttons, triggers, sticks.
     One and Series (GIP): class 0xFF, subclass 0x47, protocol 0xD0. Silent until told to power on;
       then input messages (0x20) with the same things in another layout, and the Xbox button apart (0x07).
   The first controller plugged in is player 1's: its buttons join the keyboard's in rinc_buttons, and
   its sticks and triggers go to rinc_axes. The Xbox button is kept for the system (rinc_padGuide). */
#include <string.h>
#include "tusb.h"
#include "host/usbh_pvt.h"
#include "rinc.h"
#include "gamepad.h"

#define rinc_PADS 4
enum { rinc_PadKind_none, rinc_PadKind_x360, rinc_PadKind_xone };
typedef struct {
  uint8_t dev, itf, kind, ep_in, ep_out, seq; uint16_t in_size;
  uint16_t buttons; int16_t axes[6]; uint8_t guide;
  CFG_TUH_MEM_ALIGN uint8_t in[64], out[64];
} rinc_Pad;
CFG_TUH_MEM_SECTION static rinc_Pad rinc_pads[rinc_PADS];
uint16_t rinc_padButtons; uint8_t rinc_padGuide, rinc_padCount;
static void (*note_fn)(const char *fmt, unsigned a, unsigned b, unsigned c);
void rinc_padNotes(void (*note)(const char *fmt, unsigned a, unsigned b, unsigned c)) { note_fn = note; }
#define rinc_NOTE(f, a, b, c) do { if (note_fn) note_fn(f, a, b, c); } while (0)

static rinc_Pad *rinc_find(uint8_t dev, uint8_t ep) {
  int k;
  for (k = 0; k < rinc_PADS; k++) if (rinc_pads[k].kind && rinc_pads[k].dev == dev && (!ep || rinc_pads[k].ep_in == ep || rinc_pads[k].ep_out == ep)) return &rinc_pads[k];
  return 0;
}
static bool rinc_receive(rinc_Pad *p) {
  if (!usbh_edpt_claim(p->dev, p->ep_in)) return false;
  if (!usbh_edpt_xfer(p->dev, p->ep_in, p->in, p->in_size)) { usbh_edpt_release(p->dev, p->ep_in); return false; }
  return true;
}
static bool rinc_send(rinc_Pad *p, const uint8_t *msg, uint16_t len) {
  if (!p->ep_out || !usbh_edpt_claim(p->dev, p->ep_out)) return false;
  memcpy(p->out, msg, len);
  if (!usbh_edpt_xfer(p->dev, p->ep_out, p->out, len)) { usbh_edpt_release(p->dev, p->ep_out); return false; }
  return true;
}
static int16_t rinc_rd16(const uint8_t *b) { return (int16_t)(b[0] | b[1] << 8); }
static int16_t rinc_flip(int16_t v) { return (int16_t)(v == -32768 ? 32767 : -v); } /* (up is + on the controller, - on the screen) */
/* each 360 button bit, then each One one: as Rinc's */
static const uint16_t rinc_x360Map[16] = {rinc_Button_up, rinc_Button_down, rinc_Button_left, rinc_Button_right, rinc_Button_start, rinc_Button_back, rinc_Button_ls, rinc_Button_rs,
  rinc_Button_lb, rinc_Button_rb, 0 /* (Xbox button) */, 0, rinc_Button_a, rinc_Button_b, rinc_Button_x, rinc_Button_y};
static const uint16_t rinc_xoneMap[16] = {0, 0, rinc_Button_start, rinc_Button_back, rinc_Button_a, rinc_Button_b, rinc_Button_x, rinc_Button_y,
  rinc_Button_up, rinc_Button_down, rinc_Button_left, rinc_Button_right, rinc_Button_lb, rinc_Button_rb, rinc_Button_ls, rinc_Button_rs};
static uint16_t rinc_mapped(uint16_t bits, const uint16_t *map) { uint16_t b = 0; int i; for (i = 0; i < 16; i++) if (bits >> i & 1) b |= map[i]; return b; }

static void rinc_input(rinc_Pad *p, const uint8_t *r, uint32_t n) {
  if (p->kind == rinc_PadKind_x360 && n >= 14 && r[0] == 0x00) {
    uint16_t bits = (uint16_t)(r[2] | r[3] << 8);
    p->buttons = rinc_mapped(bits, rinc_x360Map); p->guide = bits >> 10 & 1;
    p->axes[4] = (int16_t)(r[4] * 32767 / 255); p->axes[5] = (int16_t)(r[5] * 32767 / 255);
    p->axes[0] = rinc_rd16(r + 6); p->axes[1] = rinc_flip(rinc_rd16(r + 8)); p->axes[2] = rinc_rd16(r + 10); p->axes[3] = rinc_flip(rinc_rd16(r + 12));
  } else if (p->kind == rinc_PadKind_xone && n >= 18 && r[0] == 0x20) {
    p->buttons = rinc_mapped((uint16_t)(r[4] | r[5] << 8), rinc_xoneMap);
    p->axes[4] = (int16_t)((r[6] | r[7] << 8) * 32767 / 1023); p->axes[5] = (int16_t)((r[8] | r[9] << 8) * 32767 / 1023);
    p->axes[0] = rinc_rd16(r + 10); p->axes[1] = rinc_flip(rinc_rd16(r + 12)); p->axes[2] = rinc_rd16(r + 14); p->axes[3] = rinc_flip(rinc_rd16(r + 16));
  } else if (p->kind == rinc_PadKind_xone && n >= 5 && r[0] == 0x07) {
    p->guide = r[4] & 1;
    if (r[1] & 0x10) { const uint8_t ack[] = {0x01, 0x20, r[2], 0x09, 0x00, 0x07, 0x20, 0x02, 0, 0, 0, 0, 0}; rinc_send(p, ack, sizeof ack); }
  }
}

/* the class driver */
static bool rinc_padInit(void) { memset(rinc_pads, 0, sizeof rinc_pads); return true; }
static bool rinc_padDeinit(void) { return true; }
static bool rinc_padOpen(uint8_t rhport, uint8_t dev, tusb_desc_interface_t const *itf, uint16_t max_len) {
  const uint8_t *d = (const uint8_t *)itf, *end = d + max_len; rinc_Pad *p = 0; int k, kind = rinc_PadKind_none, eps = 0;
  (void)rhport;
  if (itf->bInterfaceClass == 0xFF && itf->bInterfaceSubClass == 0x5D && itf->bInterfaceProtocol == 0x01) kind = rinc_PadKind_x360;
  if (itf->bInterfaceClass == 0xFF && itf->bInterfaceSubClass == 0x47 && itf->bInterfaceProtocol == 0xD0) kind = rinc_PadKind_xone;
  rinc_NOTE("[usb] device %u: interface class %02x subclass %02x", dev, itf->bInterfaceClass, itf->bInterfaceSubClass);
  rinc_NOTE(" protocol %02x\r\n", itf->bInterfaceProtocol, 0, 0);
  if (kind == rinc_PadKind_none || itf->bNumEndpoints < 1) return false;
  if (kind == rinc_PadKind_xone && rinc_find(dev, 0)) return false; /* (a One's other interfaces: audio and such) */
  for (k = 0; k < rinc_PADS && !p; k++) if (!rinc_pads[k].kind) p = &rinc_pads[k];
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
  rinc_NOTE(kind == rinc_PadKind_x360 ? "[usb] device %u: an Xbox 360 controller\r\n" : "[usb] device %u: an Xbox One controller\r\n", dev, 0, 0);
  return true;
}
static bool rinc_padSetConfig(uint8_t dev, uint8_t itf) {
  rinc_Pad *p = rinc_find(dev, 0);
  if (p && p->itf == itf) {
    if (p->kind == rinc_PadKind_x360) { const uint8_t led[] = {0x01, 0x03, 0x06}; rinc_send(p, led, sizeof led); } /* (the ring: player 1) */
    rinc_receive(p);
    rinc_padCount++;
  }
  usbh_driver_set_config_complete(dev, itf);
  return true;
}
static bool rinc_padXfer(uint8_t dev, uint8_t ep, xfer_result_t result, uint32_t n) {
  rinc_Pad *p = rinc_find(dev, ep);
  if (!p) return false;
  if (ep == p->ep_in) {
    if (result == XFER_RESULT_SUCCESS) rinc_input(p, p->in, n);
    rinc_receive(p);
  }
  return true;
}
static void rinc_padClose(uint8_t dev) {
  int k;
  for (k = 0; k < rinc_PADS; k++) if (rinc_pads[k].kind && rinc_pads[k].dev == dev) { rinc_pads[k].kind = rinc_PadKind_none; rinc_padCount--; }
}
static const usbh_class_driver_t rinc_driver = {
  .name = "XBOX", .init = rinc_padInit, .deinit = rinc_padDeinit, .open = rinc_padOpen, .set_config = rinc_padSetConfig,
  .xfer_cb = rinc_padXfer, .close = rinc_padClose};
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *count) { *count = 1; return &rinc_driver; }

/* once set up: a One needs telling to start (after the 360's LED message, nothing else waits) */
void rinc_padTask(void) {
  int k;
  for (k = 0; k < rinc_PADS; k++) {
    rinc_Pad *p = &rinc_pads[k];
    if (p->kind == rinc_PadKind_xone && p->seq == 0 && p->ep_out && !usbh_edpt_busy(p->dev, p->ep_out)) {
      const uint8_t powerOn[] = {0x05, 0x20, 0x00, 0x01, 0x00};
      if (rinc_send(p, powerOn, sizeof powerOn)) p->seq = 1;
    }
  }
}

/* player 1: the first controller there */
void rinc_padRead(void) {
  int k;
  rinc_padButtons = 0; rinc_padGuide = 0; memset(rinc_axes, 0, sizeof rinc_axes);
  for (k = 0; k < rinc_PADS; k++)
    if (rinc_pads[k].kind) { rinc_padButtons = rinc_pads[k].buttons; rinc_padGuide = rinc_pads[k].guide; memcpy(rinc_axes, rinc_pads[k].axes, sizeof rinc_axes); break; }
}

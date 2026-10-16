/* TinyUSB for Rinc: device on the native USB port (pico_stdio_usb's serial, as configured there), and a
   host on the USB-A ports through PIO-USB, for keyboards (through the Fruit Jam's hub).
   This takes the place of pico_stdio_usb's tusb_config.h (same include guard, found first). */
#ifndef _PICO_STDIO_USB_TUSB_CONFIG_H
#define _PICO_STDIO_USB_TUSB_CONFIG_H

#include "pico/stdio_usb.h"

/* the device side, as pico_stdio_usb sets it up */
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE)
#define CFG_TUD_CDC 1
#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 64
#define CFG_TUD_CDC_EP_BUFSIZE 64
#if !PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_MS_OS_20_DESCRIPTOR
#define CFG_TUD_VENDOR 0
#else
#define CFG_TUD_VENDOR 1
#define CFG_TUD_VENDOR_RX_BUFSIZE 256
#define CFG_TUD_VENDOR_TX_BUFSIZE 256
#endif
#define CFG_TUD_ENDPOINT0_SIZE 64

/* the host side: PIO-USB as root port 1 */
#define CFG_TUH_ENABLED 1
#define CFG_TUH_RPI_PIO_USB 1
#define BOARD_TUH_RHPORT 1
#define CFG_TUH_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUSB_RHPORT1_MODE (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB 3 /* the board's own, and room for keyboards and such with one inside */
#define CFG_TUH_HID 4
#define CFG_TUH_DEVICE_MAX 6
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#endif

/* The Fruit Jam's microSD card, over SPI (SPI0: clock GPIO 34, out 35, in 36; chip select 39 as a plain
   GPIO; card detect 33), as FatFs's disk (the diskio functions it calls), and through FatFs as Rim's
   files (rim_fs). The card is set up the usual way (CMD0, CMD8, ACMD41 until ready, CMD58 for its
   addressing) at 400 kHz, then run at 25 MHz; blocks are 512 bytes, read and written one at a time.
   The filesystem is mounted when first used, so a card can go in after starting. */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "ff.h"
#include "diskio.h"
#include "rim.h"

#define SD_SPI spi0
#define PIN_SCK 34
#define PIN_MOSI 35
#define PIN_MISO 36
#define PIN_CS 39
#define PIN_DETECT 33

static int ready, blocks; /* blocks: the card addresses blocks, not bytes (SDHC and up) */

static uint8_t xfer(uint8_t b) { uint8_t r; spi_write_read_blocking(SD_SPI, &b, &r, 1); return r; }
static void cs(int on) { gpio_put(PIN_CS, !on); if (!on) xfer(0xFF); } /* (a byte after: the card lets go of MISO) */
static int wait_ready(uint32_t ms) { /* the card holds MISO low while busy */
  absolute_time_t until = make_timeout_time_ms(ms);
  while (xfer(0xFF) != 0xFF) if (time_reached(until)) return 0;
  return 1;
}
/* a command, selected: its R1 response (bit 7 set: none) */
static uint8_t command(uint8_t cmd, uint32_t arg) {
  uint8_t b[6] = {(uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg,
                  cmd == 0 ? 0x95 : cmd == 8 ? 0x87 : 0x01}, r = 0xFF; int i;
  cs(0); cs(1);
  if (cmd != 0 && !wait_ready(500)) return 0xFF;
  spi_write_blocking(SD_SPI, b, 6);
  if (cmd == 12) xfer(0xFF); /* (a stop's stuff byte) */
  for (i = 0; i < 10 && ((r = xfer(0xFF)) & 0x80); i++) {}
  return r;
}
static uint8_t app_command(uint8_t cmd, uint32_t arg) { uint8_t r = command(55, 0); return r > 1 ? r : command(cmd, arg); }

static int card_init(void) {
  int i, v2; uint8_t r, ocr[4]; absolute_time_t until;
  spi_init(SD_SPI, 400 * 1000);
  gpio_set_function(PIN_SCK, GPIO_FUNC_SPI); gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI); gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
  gpio_pull_up(PIN_MISO);
  gpio_init(PIN_CS); gpio_set_dir(PIN_CS, GPIO_OUT); gpio_put(PIN_CS, 1);
  for (i = 0; i < 10; i++) xfer(0xFF); /* 80 clocks, deselected: the card wakes into SPI mode with CMD0 */
  for (i = 0; i < 10 && (r = command(0, 0)) != 1; i++) {}
  if (r != 1) { cs(0); return 0; }
  v2 = command(8, 0x1AA) == 1;
  if (v2) { for (i = 0; i < 4; i++) ocr[i] = xfer(0xFF); if (ocr[2] != 0x01 || ocr[3] != 0xAA) { cs(0); return 0; } }
  until = make_timeout_time_ms(1500);
  while ((r = app_command(41, v2 ? 0x40000000u : 0)) != 0) if (r > 1 || time_reached(until)) { cs(0); return 0; }
  blocks = 0;
  if (v2 && command(58, 0) == 0) { for (i = 0; i < 4; i++) ocr[i] = xfer(0xFF); blocks = (ocr[0] & 0x40) != 0; }
  if (!blocks && command(16, 512) != 0) { cs(0); return 0; }
  cs(0);
  spi_set_baudrate(SD_SPI, 25 * 1000 * 1000);
  return 1;
}

static int read_block(uint8_t *buf, uint32_t n) {
  absolute_time_t until; uint8_t t;
  if (command(17, blocks ? n : n * 512) != 0) { cs(0); return 0; }
  until = make_timeout_time_ms(200);
  while ((t = xfer(0xFF)) == 0xFF) if (time_reached(until)) { cs(0); return 0; }
  if (t != 0xFE) { cs(0); return 0; }
  memset(buf, 0xFF, 512); spi_read_blocking(SD_SPI, 0xFF, buf, 512);
  xfer(0xFF); xfer(0xFF); /* (its CRC) */
  cs(0);
  return 1;
}
static int write_block(const uint8_t *buf, uint32_t n) {
  uint8_t r;
  if (command(24, blocks ? n : n * 512) != 0) { cs(0); return 0; }
  xfer(0xFE); spi_write_blocking(SD_SPI, buf, 512); xfer(0xFF); xfer(0xFF);
  r = xfer(0xFF);
  if ((r & 0x1F) != 0x05) { cs(0); return 0; } /* (not accepted) */
  r = (uint8_t)wait_ready(500);
  cs(0);
  return r;
}

/* FatFs's disk */
DSTATUS disk_status(BYTE drive) { return drive || !ready ? STA_NOINIT : 0; }
DSTATUS disk_initialize(BYTE drive) { if (drive) return STA_NOINIT; ready = card_init(); return ready ? 0 : STA_NOINIT; }
DRESULT disk_read(BYTE drive, BYTE *buf, LBA_t sector, UINT count) {
  if (drive || !ready) return RES_NOTRDY;
  for (; count; count--, sector++, buf += 512) if (!read_block(buf, (uint32_t)sector)) return RES_ERROR;
  return RES_OK;
}
DRESULT disk_write(BYTE drive, const BYTE *buf, LBA_t sector, UINT count) {
  if (drive || !ready) return RES_NOTRDY;
  for (; count; count--, sector++, buf += 512) if (!write_block(buf, (uint32_t)sector)) return RES_ERROR;
  return RES_OK;
}
DRESULT disk_ioctl(BYTE drive, BYTE cmd, void *buf) {
  if (drive || !ready) return RES_NOTRDY;
  if (cmd == CTRL_SYNC) { int ok; cs(1); ok = wait_ready(500); cs(0); return ok ? RES_OK : RES_ERROR; }
  if (cmd == GET_BLOCK_SIZE) { *(DWORD *)buf = 1; return RES_OK; }
  return RES_PARERR;
}

/* Rim's files, through FatFs: one file open at a time */
static FATFS fs; static FIL file; static int mounted, open_;
static int mount(void) {
  if (!mounted) mounted = f_mount(&fs, "", 1) == FR_OK;
  return mounted;
}
static int mkdirs(const char *path) { /* every directory along path (its last part is the file) */
  char d[128]; const char *p;
  for (p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
    size_t n = (size_t)(p - path);
    if (n >= sizeof d) return 0;
    memcpy(d, path, n); d[n] = 0;
    { FRESULT r = f_mkdir(d); if (r != FR_OK && r != FR_EXIST) return 0; }
  }
  return 1;
}
static int fs_open(const char *path, int write) {
  if (open_) { f_close(&file); open_ = 0; }
  if (!mount()) { mounted = 0; return -1; }
  if (write && !mkdirs(path)) return -1;
  if (f_open(&file, path, write ? FA_WRITE | FA_CREATE_ALWAYS : FA_READ) != FR_OK) { mounted = 0; return -1; }
  open_ = 1;
  return 0;
}
static int fs_read(char *buf, int n) { UINT got = 0; if (!open_ || f_read(&file, buf, (UINT)n, &got) != FR_OK) return -1; return (int)got; }
static int fs_write(const char *buf, int n) { UINT put = 0; if (!open_ || f_write(&file, buf, (UINT)n, &put) != FR_OK || (int)put != n) return -1; return n; }
static int fs_close(void) { int ok = open_ && f_close(&file) == FR_OK; open_ = 0; return ok ? 0 : -1; }
static int fs_list(const char *dir, void (*each)(const char *name, int dir, void *ud), void *ud) {
  DIR d; FILINFO info;
  if (!mount() || f_opendir(&d, dir) != FR_OK) { mounted = 0; return -1; }
  while (f_readdir(&d, &info) == FR_OK && info.fname[0]) each(info.fname, (info.fattrib & AM_DIR) != 0, ud);
  f_closedir(&d);
  return 0;
}
static int fs_remove(const char *path) { if (open_) { f_close(&file); open_ = 0; } return mount() && f_unlink(path) == FR_OK ? 0 : -1; }
static const RimFs sd_fs = {fs_open, fs_read, fs_write, fs_close, fs_list, fs_remove};
void rim_sd_setup(void) { gpio_init(PIN_DETECT); gpio_pull_up(PIN_DETECT); rim_fs = &sd_fs; }

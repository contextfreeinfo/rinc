/* HDMI out (DVI signaling) through the RP2350's HSTX, run by core 1: 640x480 at 60 Hz, each of the
   screen's 320x240 pixels shown as 2x2, colored through the palette.

   The HSTX's TMDS encoder takes RGB565 and sends each 32-bit word twice (doubling horizontally); a
   DMA ping-pong feeds it one block at a time: a line's blanking and sync commands, then its pixels.
   Each screen line goes through the palette once, into one of two line buffers, and is sent for two
   lines in a row (doubling vertically). That's 320 lookups per screen line, about 6% of core 1.

   Carts draw into rim_screen. At each vertical blank a DMA copies it to the buffer being shown, so
   the picture is always a whole frame (rim_frames counts them, for a flip() to wait on). */
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "rim.h"
#include "video.h"
#include "rim_audio.h"

/* 640x480 at 60 Hz (CEA VIC 1): 25.2 MHz pixels, from a 252 MHz clk_sys / 2 for the HSTX, which
   sends 2 bits a cycle: 252 Mbps, 10 bits a pixel */
#define H_FRONT 16
#define H_SYNC 96
#define H_BACK 48
#define H_ACTIVE 640
#define V_FRONT 10
#define V_SYNC 2
#define V_BACK 33
#define V_ACTIVE 480
#define V_TOTAL (V_FRONT + V_SYNC + V_BACK + V_ACTIVE)

/* TMDS control symbols: lane 0 carries the syncs (both negative in this mode), lanes 1 and 2 idle */
#define CTRL_00 0x354u
#define CTRL_01 0x0ABu
#define CTRL_10 0x154u
#define CTRL_11 0x2ABu
#define SYNC(v, h) ((v ? (h ? CTRL_11 : CTRL_10) : (h ? CTRL_01 : CTRL_00)) | CTRL_00 << 10 | CTRL_00 << 20)
#define RAW_REPEAT (0x1u << 12)
#define TMDS (0x2u << 12)
#define NOP (0xFu << 12)

static uint32_t vblank_line[] = { /* (1: sync inactive) */
  RAW_REPEAT | H_FRONT, SYNC(1, 1), RAW_REPEAT | H_SYNC, SYNC(1, 0), RAW_REPEAT | (H_BACK + H_ACTIVE), SYNC(1, 1), NOP};
static uint32_t vsync_line[] = {
  RAW_REPEAT | H_FRONT, SYNC(0, 1), RAW_REPEAT | H_SYNC, SYNC(0, 0), RAW_REPEAT | (H_BACK + H_ACTIVE), SYNC(0, 1), NOP};
static uint32_t active_line[] = { /* blanking, then H_ACTIVE pixels from the next block */
  RAW_REPEAT | H_FRONT, SYNC(1, 1), NOP, RAW_REPEAT | H_SYNC, SYNC(1, 0), NOP, RAW_REPEAT | H_BACK, SYNC(1, 1), TMDS | H_ACTIVE};

static _Alignas(4) uint8_t shown[RIM_H][RIM_W];
static uint32_t line[2][RIM_W]; /* RGB565 in each word's low half; the HSTX sends each word twice */
static int ping, pong, copy;
static uint32_t vline;   /* the line the next posted block belongs to */
static bool pixels_next; /* posted an active line's commands; its pixels come next */
volatile uint32_t rim_frames;

static void __scratch_x("") convert(int y) {
  const uint8_t *s = shown[y]; uint32_t *d = line[y & 1]; int x;
  for (x = 0; x < RIM_W; x += 4) {
    d[x] = rim_palette[s[x]]; d[x + 1] = rim_palette[s[x + 1]]; d[x + 2] = rim_palette[s[x + 2]]; d[x + 3] = rim_palette[s[x + 3]];
  }
}

/* a block finished: point its channel at the block after the one now running */
static void __scratch_x("") dma_done(void) {
  int ch = dma_hw->ints1 & (1u << ping) ? ping : pong; int y = -1;
  dma_channel_hw_t *c = &dma_hw->ch[ch];
  dma_hw->ints1 = 1u << ch;
  if (vline < V_FRONT + V_SYNC + V_BACK) {
    int sync = vline >= V_FRONT && vline < V_FRONT + V_SYNC;
    c->read_addr = (uintptr_t)(sync ? vsync_line : vblank_line); c->transfer_count = sync ? count_of(vsync_line) : count_of(vblank_line);
    if (vline == 0) { /* blanking starts: show the newest frame */
      dma_channel_set_read_addr(copy, rim_screen, false);
      dma_channel_set_write_addr(copy, shown, true);
      rim_frames++;
    }
    if (vline == V_FRONT + V_SYNC + V_BACK - 1) y = 0; /* (the copy finished long ago) */
    vline++;
  } else if (!pixels_next) {
    c->read_addr = (uintptr_t)active_line; c->transfer_count = count_of(active_line);
    pixels_next = true;
  } else {
    uint32_t l = vline - (V_FRONT + V_SYNC + V_BACK);
    c->read_addr = (uintptr_t)line[(l / 2) & 1]; c->transfer_count = RIM_W;
    pixels_next = false;
    if ((l & 1) && l / 2 + 1 < RIM_H) y = (int)(l / 2 + 1); /* this line's second showing: get the next one ready */
    vline = vline + 1 == V_TOTAL ? 0 : vline + 1;
  }
  if (y >= 0) convert(y);
}

static void core1(void) {
  dma_channel_config c;
  ping = dma_claim_unused_channel(true); pong = dma_claim_unused_channel(true);
  c = dma_channel_get_default_config(ping);
  channel_config_set_chain_to(&c, pong); channel_config_set_dreq(&c, DREQ_HSTX);
  dma_channel_configure(ping, &c, &hstx_fifo_hw->fifo, vblank_line, count_of(vblank_line), false);
  c = dma_channel_get_default_config(pong);
  channel_config_set_chain_to(&c, ping); channel_config_set_dreq(&c, DREQ_HSTX);
  dma_channel_configure(pong, &c, &hstx_fifo_hw->fifo, vblank_line, count_of(vblank_line), false);
  vline = 2; /* the first two blocks are those blank lines */
  dma_hw->ints1 = (1u << ping) | (1u << pong);
  dma_hw->inte1 = (1u << ping) | (1u << pong);
  irq_set_exclusive_handler(DMA_IRQ_1, dma_done);
  irq_set_enabled(DMA_IRQ_1, true);
  bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
  dma_channel_start(ping);
  rim_audio_start();
  for (;;) __wfi();
}

void rim_video_start(void) {
  dma_channel_config c; int lane, i;
  copy = dma_claim_unused_channel(true);
  c = dma_channel_get_default_config(copy);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
  channel_config_set_read_increment(&c, true); channel_config_set_write_increment(&c, true);
  dma_channel_configure(copy, &c, shown, rim_screen, sizeof shown / 4, true); /* (the first frame now) */
  clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, clock_get_hz(clk_sys), 2);
  /* RGB565 into the TMDS lanes: red (lane 2) from bits 15:11, green 10:5, blue 4:0 */
  hstx_ctrl_hw->expand_tmds = 4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 8 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
                              5 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 3 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
                              4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
  /* each pixel word twice, unshifted: that's the horizontal doubling; commands are whole words */
  hstx_ctrl_hw->expand_shift = 2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
                               1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
  hstx_ctrl_hw->csr = 0;
  hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS | 5u << HSTX_CTRL_CSR_CLKDIV_LSB | 5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
                      2u << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;
  /* the Fruit Jam's HDMI pins: GPIO 12-19 are HSTX bits 0-7, pairs (-, +): clock 12-13, then lanes 0-2 */
  hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
  hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
  for (lane = 0; lane < 3; lane++) {
    uint32_t sel = (uint32_t)(lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB | (uint32_t)(lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
    hstx_ctrl_hw->bit[3 + lane * 2] = sel;
    hstx_ctrl_hw->bit[2 + lane * 2] = sel | HSTX_CTRL_BIT0_INV_BITS;
  }
  for (i = 12; i <= 19; i++) gpio_set_function((uint)i, GPIO_FUNC_HSTX);
  dma_channel_wait_for_finish_blocking(copy);
  multicore_launch_core1(core1);
}

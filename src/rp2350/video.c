/* HDMI out through the RP2350's HSTX, run by core 1: 640x480 at 60 Hz, each of the screen's 320x240
   pixels shown as 2x2, colored through the palette, and 48 kHz stereo sound.

   The HSTX's rinc_TMDS encoder takes RGB565 and sends each 32-bit word twice (doubling horizontally); a
   DMA ping-pong feeds it one block at a time: a line's blanking and sync commands, then its pixels.
   Each screen line goes through the palette once, into one of two line buffers, and is sent for two
   lines in a row (doubling vertically). That's 320 lookups per screen line, about 6% of core 1.

   Carts draw into rinc_screen. At each vertical blank a DMA copies it to the buffer being shown, so
   the picture is always a whole frame (rinc_frames counts them, for a flip() to wait on).

   Sound goes in data islands, one in each line's sync pulse: audio packets of 4 samples (32 samples
   every 21 lines: exactly 48 kHz), and now and then the audio clock (N 6144, CTS 25200: 48 kHz from
   the 25.2 MHz pixel clock) and the AVI and audio InfoFrames, which also make this HDMI, not DVI.
   Core 1's main loop encodes audio packets ahead of time from the samples the mixer passes along
   (rinc_hdmiSound); the line interrupt only copies one in. Packets: hdmi_packet.c (from pico_hdmi). */
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "rinc.h"
#include "video.h"
#include "rinc_audio.h"
#include "hdmi_packet.h"

/* 640x480 at 60 Hz (CEA VIC 1): 25.2 MHz pixels, from a 252 MHz clk_sys / 2 for the HSTX, which
   sends 2 bits a cycle: 252 Mbps, 10 bits a pixel */
#define rinc_H_FRONT 16
#define rinc_H_SYNC 96
#define rinc_H_BACK 48
#define rinc_H_ACTIVE 640
#define rinc_V_FRONT 10
#define rinc_V_SYNC 2
#define rinc_V_BACK 33
#define rinc_V_ACTIVE 480
#define rinc_V_TOTAL (rinc_V_FRONT + rinc_V_SYNC + rinc_V_BACK + rinc_V_ACTIVE)

/* rinc_TMDS control symbols: lane 0 carries the syncs (both negative in this mode), lanes 1 and 2 idle */
#define rinc_CTRL_00 0x354u
#define rinc_CTRL_01 0x0ABu
#define rinc_CTRL_10 0x154u
#define rinc_CTRL_11 0x2ABu
#define rinc_SYNC(v, h) ((v ? (h ? rinc_CTRL_11 : rinc_CTRL_10) : (h ? rinc_CTRL_01 : rinc_CTRL_00)) | rinc_CTRL_00 << 10 | rinc_CTRL_00 << 20)
#define rinc_RAW (0x0u << 12)
#define rinc_RAW_REPEAT (0x1u << 12)
#define rinc_TMDS (0x2u << 12)
#define rinc_NOP (0xFu << 12)

/* (rinc_SYNC: 1 is inactive.) Before a data island, 8 pixels of preamble; before a line's pixels, 8 of
   video preamble and a 2-pixel guard band */
#define rinc_ISLAND_PREAMBLE(v) (rinc_SYNC(v, 0) & 0x3FF | rinc_CTRL_01 << 10 | rinc_CTRL_01 << 20)
#define rinc_VIDEO_PREAMBLE (rinc_CTRL_11 | rinc_CTRL_01 << 10 | rinc_CTRL_00 << 20)
#define rinc_VIDEO_GUARD (0x2CCu | 0x133u << 10 | 0x2CCu << 20)
static uint32_t rinc_cmd[2][64]; /* each DMA channel's line of commands, rebuilt as it's posted */
static hstx_data_island_t rinc_acr[2], rinc_avi, rinc_audioInfo; /* the packets that never change ([v]: vsync inactive) */
static const uint32_t *rinc_nullIsland[2];
/* audio packets, encoded ahead (core 1's main loop) from the mixer's samples */
#define rinc_QUEUE 16
static hstx_data_island_t rinc_queue[rinc_QUEUE];
static volatile uint32_t rinc_qhead, rinc_qtail;
#define rinc_RING 1024
static int16_t rinc_ring[rinc_RING * 2];
static volatile uint32_t rinc_rhead, rinc_rtail;
static uint32_t rinc_due; /* audio owed: +32 a line, 84 for a 4-sample packet */

/* a line's commands into cmd[k]: v: vsync inactive; active: the pixels come next */
static uint32_t __scratch_x("") rinc_lineCmds(int k, int v, int active, const uint32_t *island) {
  uint32_t *p = rinc_cmd[k], *start = p; int i;
  *p++ = rinc_RAW_REPEAT | rinc_H_FRONT; *p++ = rinc_SYNC(v, 1);
  *p++ = rinc_RAW_REPEAT | 8; *p++ = rinc_ISLAND_PREAMBLE(v);
  *p++ = rinc_RAW | W_DATA_ISLAND; for (i = 0; i < W_DATA_ISLAND; i++) *p++ = island[i];
  *p++ = rinc_RAW_REPEAT | (rinc_H_SYNC - 8 - W_DATA_ISLAND); *p++ = rinc_SYNC(v, 0);
  if (active) {
    *p++ = rinc_RAW_REPEAT | (rinc_H_BACK - 10); *p++ = rinc_SYNC(v, 1);
    *p++ = rinc_RAW_REPEAT | 8; *p++ = rinc_VIDEO_PREAMBLE;
    *p++ = rinc_RAW_REPEAT | 2; *p++ = rinc_VIDEO_GUARD;
    *p++ = rinc_TMDS | rinc_H_ACTIVE;
  } else { *p++ = rinc_RAW_REPEAT | (rinc_H_BACK + rinc_H_ACTIVE); *p++ = rinc_SYNC(v, 1); }
  return (uint32_t)(p - start);
}
/* this line's island: the fixed packets on their lines, else audio when it's due */
static const uint32_t *__scratch_x("") rinc_islandFor(uint32_t l) {
  rinc_due += 32; if (rinc_due > 84 * 4) rinc_due = 84 * 4;
  if (l == 0) return rinc_avi.words;
  if (l == rinc_V_FRONT) return rinc_acr[0].words;
  if (l == rinc_V_FRONT + 1) return rinc_audioInfo.words;
  if (l >= rinc_V_FRONT + rinc_V_SYNC && l < rinc_V_FRONT + rinc_V_SYNC + rinc_V_BACK && l % 4 == 0) return rinc_acr[1].words;
  if (rinc_due >= 84 && rinc_qtail != rinc_qhead) { const uint32_t *w = rinc_queue[rinc_qtail].words; rinc_qtail = (rinc_qtail + 1) % rinc_QUEUE; rinc_due -= 84; return w; }
  return rinc_nullIsland[l < rinc_V_FRONT || l >= rinc_V_FRONT + rinc_V_SYNC];
}

static _Alignas(4) uint8_t shown[rinc_H][rinc_W];
static uint32_t rinc_line[2][rinc_W]; /* RGB565 in each word's low half; the HSTX sends each word twice */
static int rinc_ping, rinc_pong, rinc_copy;
static uint32_t rinc_vline;   /* the line the next posted block belongs to */
static bool rinc_pixelsNext; /* posted an active line's commands; its pixels come next */
volatile uint32_t rinc_frames;

static void __scratch_x("") rinc_convert(int y) {
  const uint8_t *s = shown[y]; uint32_t *d = rinc_line[y & 1]; int x;
  for (x = 0; x < rinc_W; x += 4) {
    d[x] = rinc_palette[s[x]]; d[x + 1] = rinc_palette[s[x + 1]]; d[x + 2] = rinc_palette[s[x + 2]]; d[x + 3] = rinc_palette[s[x + 3]];
  }
}

/* a block finished: point its channel at the block after the one now running */
static void __scratch_x("") rinc_dmaDone(void) {
  int ch = dma_hw->ints1 & (1u << rinc_ping) ? rinc_ping : rinc_pong; int y = -1;
  dma_channel_hw_t *c = &dma_hw->ch[ch];
  dma_hw->ints1 = 1u << ch;
  if (rinc_vline < rinc_V_FRONT + rinc_V_SYNC + rinc_V_BACK) {
    int sync = rinc_vline >= rinc_V_FRONT && rinc_vline < rinc_V_FRONT + rinc_V_SYNC;
    c->read_addr = (uintptr_t)rinc_cmd[ch == rinc_ping ? 0 : 1]; c->transfer_count = rinc_lineCmds(ch == rinc_ping ? 0 : 1, !sync, 0, rinc_islandFor(rinc_vline));
    if (rinc_vline == 0) { /* blanking starts: show the newest frame */
      dma_channel_set_read_addr(rinc_copy, rinc_screen, false);
      dma_channel_set_write_addr(rinc_copy, shown, true);
      rinc_frames++;
    }
    if (rinc_vline == rinc_V_FRONT + rinc_V_SYNC + rinc_V_BACK - 1) y = 0; /* (the copy finished long ago) */
    rinc_vline++;
  } else if (!rinc_pixelsNext) {
    c->read_addr = (uintptr_t)rinc_cmd[ch == rinc_ping ? 0 : 1]; c->transfer_count = rinc_lineCmds(ch == rinc_ping ? 0 : 1, 1, 1, rinc_islandFor(rinc_vline));
    rinc_pixelsNext = true;
  } else {
    uint32_t l = rinc_vline - (rinc_V_FRONT + rinc_V_SYNC + rinc_V_BACK);
    c->read_addr = (uintptr_t)rinc_line[(l / 2) & 1]; c->transfer_count = rinc_W;
    rinc_pixelsNext = false;
    if ((l & 1) && l / 2 + 1 < rinc_H) y = (int)(l / 2 + 1); /* this line's second showing: get the next one ready */
    rinc_vline = rinc_vline + 1 == rinc_V_TOTAL ? 0 : rinc_vline + 1;
  }
  if (y >= 0) rinc_convert(y);
}

static void rinc_core1(void) {
  dma_channel_config c;
  rinc_ping = dma_claim_unused_channel(true); rinc_pong = dma_claim_unused_channel(true);
  c = dma_channel_get_default_config(rinc_ping);
  channel_config_set_chain_to(&c, rinc_pong); channel_config_set_dreq(&c, DREQ_HSTX);
  dma_channel_configure(rinc_ping, &c, &hstx_fifo_hw->fifo, rinc_cmd[0], rinc_lineCmds(0, 1, 0, rinc_nullIsland[1]), false);
  c = dma_channel_get_default_config(rinc_pong);
  channel_config_set_chain_to(&c, rinc_ping); channel_config_set_dreq(&c, DREQ_HSTX);
  dma_channel_configure(rinc_pong, &c, &hstx_fifo_hw->fifo, rinc_cmd[1], rinc_lineCmds(1, 1, 0, rinc_nullIsland[1]), false);
  rinc_vline = 2; /* the first two blocks are those blank lines */
  dma_hw->ints1 = (1u << rinc_ping) | (1u << rinc_pong);
  dma_hw->inte1 = (1u << rinc_ping) | (1u << rinc_pong);
  irq_set_exclusive_handler(DMA_IRQ_1, rinc_dmaDone);
  irq_set_enabled(DMA_IRQ_1, true);
  bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
  dma_channel_start(rinc_ping);
  rinc_audioStart();
  for (;;) { /* encode audio packets ahead, while there's room and 4 samples to send */
    static int frame; hstx_packet_t pk; audio_sample_t a[4]; int i;
    if ((rinc_qhead + 1) % rinc_QUEUE == rinc_qtail || (rinc_rhead - rinc_rtail) % rinc_RING < 4) { __wfi(); continue; }
    for (i = 0; i < 4; i++) { a[i].left = rinc_ring[rinc_rtail * 2]; a[i].right = rinc_ring[rinc_rtail * 2 + 1]; rinc_rtail = (rinc_rtail + 1) % rinc_RING; }
    frame = hstx_packet_set_audio_samples_cs(&pk, a, 4, frame);
    hstx_encode_data_island(&rinc_queue[rinc_qhead], &pk, false, true);
    rinc_qhead = (rinc_qhead + 1) % rinc_QUEUE;
  }
}

/* the mixer's latest frames, for HDMI (from audio.c's interrupt, on core 1); if the encoder ever falls
   behind, the newest are dropped (both run off 252 MHz: the rates match exactly) */
void __not_in_flash_func(rinc_hdmiSound)(const int16_t *stereo, int frames) {
  int f;
  for (f = 0; f < frames; f++) {
    uint32_t next = (rinc_rhead + 1) % rinc_RING;
    if (next == rinc_rtail) return;
    rinc_ring[rinc_rhead * 2] = stereo[f * 2]; rinc_ring[rinc_rhead * 2 + 1] = stereo[f * 2 + 1]; rinc_rhead = next;
  }
}

void rinc_videoStart(void) {
  dma_channel_config c; int lane, i;
  rinc_copy = dma_claim_unused_channel(true);
  c = dma_channel_get_default_config(rinc_copy);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
  channel_config_set_read_increment(&c, true); channel_config_set_write_increment(&c, true);
  dma_channel_configure(rinc_copy, &c, shown, rinc_screen, sizeof shown / 4, true); /* (the first frame now) */
  { /* the packets that never change */
    hstx_packet_t pk;
    hstx_packet_set_acr(&pk, 6144, 25200); hstx_encode_data_island(&rinc_acr[0], &pk, true, true); hstx_encode_data_island(&rinc_acr[1], &pk, false, true);
    hstx_packet_set_avi_infoframe(&pk, 1, 0); hstx_encode_data_island(&rinc_avi, &pk, false, true);
    hstx_packet_set_audio_infoframe(&pk, rinc_RATE, 2, 16); hstx_encode_data_island(&rinc_audioInfo, &pk, true, true);
    rinc_nullIsland[0] = hstx_get_null_data_island(true, true); rinc_nullIsland[1] = hstx_get_null_data_island(false, true);
  }
  clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, clock_get_hz(clk_sys), 2);
  /* RGB565 into the rinc_TMDS lanes: red (lane 2) from bits 15:11, green 10:5, blue 4:0 */
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
  dma_channel_wait_for_finish_blocking(rinc_copy);
  multicore_launch_core1(rinc_core1);
}

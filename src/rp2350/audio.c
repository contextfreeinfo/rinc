/* Sound out of the Fruit Jam's 3.5 mm jack: a TLV320DAC3100, set up over I2C, fed 48 kHz 16-bit
   stereo over I2S by a PIO state machine (on pio2, clear of USB's). Two DMA channels take turns
   sending buffers of 256 frames (5.3 ms); as one finishes, an interrupt on core 1 mixes the next into
   it, below the video's priority. Host procs on core 0 and the mixer share the voices through a
   spin lock that leaves interrupts alone, so neither core's timing (video, USB) waits on it.
   The rinc_DAC's register settings follow wili8jam (MIT, github.com/freewili/wili8jam), which got them
   from fruitjam-doom; the speaker amplifier stays off: only the jack. */
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "rinc.h"
#include "rinc_audio.h"
#include "video.h"
#include "i2s.pio.h"

#define rinc_PIN_DIN 24
#define rinc_PIN_BCLK 26 /* (word select is the next pin, 27) */
#define rinc_PIN_SDA 20
#define rinc_PIN_SCL 21
#define rinc_PIN_RESET 22 /* (shared with the ESP32-C6) */
#define rinc_DAC 0x18
#define rinc_FRAMES 256

static uint32_t rinc_buf[2][rinc_FRAMES]; /* left << 16 | right, for the PIO */
static int rinc_ch[2];
static spin_lock_t *rinc_lock;

static void rinc_reg(uint8_t r, uint8_t v) { uint8_t b[2] = {r, v}; i2c_write_timeout_us(i2c0, rinc_DAC, b, 2, false, 2000); }
static uint8_t rinc_get(uint8_t r) { uint8_t v = r; i2c_write_timeout_us(i2c0, rinc_DAC, &v, 1, true, 2000); i2c_read_timeout_us(i2c0, rinc_DAC, &v, 1, false, 2000); return v; }
static void rinc_mod(uint8_t r, uint8_t mask, uint8_t v) { rinc_reg(r, (uint8_t)((rinc_get(r) & ~mask) | (v & mask))); }

/* the rinc_DAC: I2S 16-bit, clocked from the bit clock (1.536 MHz: PLL x64 = 98.304 MHz, / 8 / 2 / 128 =
   48 kHz), DACs on, routed to the headphone drivers */
static void rinc_dacSetup(void) {
  gpio_init(rinc_PIN_RESET); gpio_set_dir(rinc_PIN_RESET, GPIO_OUT); gpio_put(rinc_PIN_RESET, 1);
  i2c_init(i2c0, 100000);
  gpio_set_function(rinc_PIN_SDA, GPIO_FUNC_I2C); gpio_set_function(rinc_PIN_SCL, GPIO_FUNC_I2C);
  sleep_ms(50);
  rinc_reg(0x00, 0); rinc_reg(0x01, 0x01); sleep_ms(10);           /* page 0; software reset */
  rinc_mod(0x1B, 0xF0, 0x00);                                  /* I2S, 16 bits */
  rinc_mod(0x04, 0x0F, 0x07);                                  /* PLL from the bit clock; the codec from the PLL */
  rinc_reg(0x06, 0x20); rinc_reg(0x07, 0x00); rinc_reg(0x08, 0x00);     /* J = 32, D = 0 */
  rinc_mod(0x05, 0x7F, 0x12);                                  /* P = 1, R = 2 */
  rinc_reg(0x0B, 0x88); rinc_reg(0x0C, 0x82);                       /* NDAC = 8, MDAC = 2, on */
  rinc_mod(0x05, 0x80, 0x80);                                  /* PLL on */
  rinc_reg(0x00, 1); rinc_mod(0x2E, 0xFF, 0x0B); rinc_reg(0x00, 0);     /* headset detection */
  rinc_mod(0x43, 0x80, 0x80); rinc_mod(0x30, 0x80, 0x80); rinc_mod(0x33, 0x3C, 0x14);
  rinc_mod(0x3F, 0xC0, 0xC0);                                  /* both DACs on */
  rinc_reg(0x00, 1); rinc_mod(0x23, 0xCC, 0x44); rinc_reg(0x00, 0);     /* DACs to the output mixers */
  rinc_mod(0x40, 0x0C, 0x00); rinc_reg(0x41, 0x00); rinc_reg(0x42, 0x00); /* unmuted, 0 dB */
  rinc_reg(0x00, 1);                                           /* headphone drivers: on, unmuted, 0 dB of gain */
  rinc_mod(0x1F, 0xC0, 0xC0); rinc_mod(0x28, 0x04, 0x04); rinc_mod(0x29, 0x04, 0x04);
  rinc_reg(0x24, 0x0A); rinc_reg(0x25, 0x0A);
  rinc_mod(0x28, 0x78, 0x00); rinc_mod(0x29, 0x78, 0x00);           /* (wili8jam: 8 dB; full-scale sound was too loud on headphones) */
  rinc_reg(0x00, 0);
}

static void rinc_lockAudio(int on) { if (on) spin_lock_unsafe_blocking(rinc_lock); else spin_unlock_unsafe(rinc_lock); }

/* a buffer went out: mix the next frames into it, in short turns at the lock */
static void __not_in_flash_func(rinc_refill)(uint32_t *b) {
  int16_t *s = (int16_t *)(void *)b; int f, k;
  for (k = 0; k < rinc_FRAMES; k += 64) { spin_lock_unsafe_blocking(rinc_lock); rinc_audioMix(s + k * 2, 64); spin_unlock_unsafe(rinc_lock); }
  rinc_hdmiSound(s, rinc_FRAMES); /* the same sound over HDMI */
  for (f = 0; f < rinc_FRAMES; f++) b[f] = b[f] << 16 | b[f] >> 16; /* (memory has left first, in the low half) */
}
static void __not_in_flash_func(rinc_audioDone)(void) {
  int k;
  for (k = 0; k < 2; k++)
    if (dma_hw->ints0 & (1u << rinc_ch[k])) {
      dma_hw->ints0 = 1u << rinc_ch[k];
      dma_channel_set_read_addr(rinc_ch[k], rinc_buf[k], false); /* (it starts when the other one chains to it) */
      rinc_refill(rinc_buf[k]);
    }
}

void rinc_audioSetup(void) {
  rinc_lock = spin_lock_init(spin_lock_claim_unused(true));
  rinc_audioLock = rinc_lockAudio;
  rinc_dacSetup();
}

/* on core 1, after the video: the PIO, the DMA turns, and the interrupt that mixes */
void rinc_audioStart(void) {
  PIO pio = pio2; uint sm = (uint)pio_claim_unused_sm(pio, true), off = pio_add_program(pio, &rinc_i2s_program);
  pio_sm_config c = rinc_i2s_program_get_default_config(off); int k;
  pio_gpio_init(pio, rinc_PIN_DIN); pio_gpio_init(pio, rinc_PIN_BCLK); pio_gpio_init(pio, rinc_PIN_BCLK + 1);
  pio_sm_set_consecutive_pindirs(pio, sm, rinc_PIN_DIN, 1, true);
  pio_sm_set_consecutive_pindirs(pio, sm, rinc_PIN_BCLK, 2, true);
  sm_config_set_out_pins(&c, rinc_PIN_DIN, 1);
  sm_config_set_sideset_pins(&c, rinc_PIN_BCLK);
  sm_config_set_out_shift(&c, false, true, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (rinc_RATE * 64.0f));
  pio_sm_init(pio, sm, off, &c);
  rinc_refill(rinc_buf[0]); rinc_refill(rinc_buf[1]);
  rinc_ch[0] = dma_claim_unused_channel(true); rinc_ch[1] = dma_claim_unused_channel(true);
  for (k = 0; k < 2; k++) {
    dma_channel_config d = dma_channel_get_default_config(rinc_ch[k]);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, true); channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&d, rinc_ch[k ^ 1]);
    dma_channel_configure(rinc_ch[k], &d, &pio->txf[sm], rinc_buf[k], rinc_FRAMES, false);
  }
  dma_hw->ints0 = (1u << rinc_ch[0]) | (1u << rinc_ch[1]);
  dma_hw->inte0 |= (1u << rinc_ch[0]) | (1u << rinc_ch[1]);
  irq_set_exclusive_handler(DMA_IRQ_0, rinc_audioDone);
  irq_set_priority(DMA_IRQ_0, PICO_LOWEST_IRQ_PRIORITY); /* (the video's goes first) */
  irq_set_enabled(DMA_IRQ_0, true);
  dma_channel_start(rinc_ch[0]);
  pio_sm_set_enabled(pio, sm, true);
}

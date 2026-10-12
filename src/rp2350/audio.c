/* Sound out of the Fruit Jam's 3.5 mm jack: a TLV320DAC3100, set up over I2C, fed 48 kHz 16-bit
   stereo over I2S by a PIO state machine (on pio2, clear of USB's). Two DMA channels take turns
   sending buffers of 256 frames (5.3 ms); as one finishes, an interrupt on core 1 mixes the next into
   it, below the video's priority. Host procs on core 0 and the mixer share the voices through a
   spin lock that leaves interrupts alone, so neither core's timing (video, USB) waits on it.
   The DAC's register settings follow wili8jam (MIT, github.com/freewili/wili8jam), which got them
   from fruitjam-doom; the speaker amplifier stays off: only the jack. */
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "rim.h"
#include "rim_audio.h"
#include "i2s.pio.h"

#define PIN_DIN 24
#define PIN_BCLK 26 /* (word select is the next pin, 27) */
#define PIN_SDA 20
#define PIN_SCL 21
#define PIN_RESET 22 /* (shared with the ESP32-C6) */
#define DAC 0x18
#define FRAMES 256

static uint32_t buf[2][FRAMES]; /* left << 16 | right, for the PIO */
static int ch[2];
static spin_lock_t *lock;

static void reg(uint8_t r, uint8_t v) { uint8_t b[2] = {r, v}; i2c_write_timeout_us(i2c0, DAC, b, 2, false, 2000); }
static uint8_t get(uint8_t r) { uint8_t v = r; i2c_write_timeout_us(i2c0, DAC, &v, 1, true, 2000); i2c_read_timeout_us(i2c0, DAC, &v, 1, false, 2000); return v; }
static void mod(uint8_t r, uint8_t mask, uint8_t v) { reg(r, (uint8_t)((get(r) & ~mask) | (v & mask))); }

/* the DAC: I2S 16-bit, clocked from the bit clock (1.536 MHz: PLL x64 = 98.304 MHz, / 8 / 2 / 128 =
   48 kHz), DACs on, routed to the headphone drivers */
static void dac_setup(void) {
  gpio_init(PIN_RESET); gpio_set_dir(PIN_RESET, GPIO_OUT); gpio_put(PIN_RESET, 1);
  i2c_init(i2c0, 100000);
  gpio_set_function(PIN_SDA, GPIO_FUNC_I2C); gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
  sleep_ms(50);
  reg(0x00, 0); reg(0x01, 0x01); sleep_ms(10);           /* page 0; software reset */
  mod(0x1B, 0xF0, 0x00);                                  /* I2S, 16 bits */
  mod(0x04, 0x0F, 0x07);                                  /* PLL from the bit clock; the codec from the PLL */
  reg(0x06, 0x20); reg(0x07, 0x00); reg(0x08, 0x00);     /* J = 32, D = 0 */
  mod(0x05, 0x7F, 0x12);                                  /* P = 1, R = 2 */
  reg(0x0B, 0x88); reg(0x0C, 0x82);                       /* NDAC = 8, MDAC = 2, on */
  mod(0x05, 0x80, 0x80);                                  /* PLL on */
  reg(0x00, 1); mod(0x2E, 0xFF, 0x0B); reg(0x00, 0);     /* headset detection */
  mod(0x43, 0x80, 0x80); mod(0x30, 0x80, 0x80); mod(0x33, 0x3C, 0x14);
  mod(0x3F, 0xC0, 0xC0);                                  /* both DACs on */
  reg(0x00, 1); mod(0x23, 0xCC, 0x44); reg(0x00, 0);     /* DACs to the output mixers */
  mod(0x40, 0x0C, 0x00); reg(0x41, 0x00); reg(0x42, 0x00); /* unmuted, 0 dB */
  reg(0x00, 1);                                           /* headphone drivers: on, unmuted, some gain */
  mod(0x1F, 0xC0, 0xC0); mod(0x28, 0x04, 0x04); mod(0x29, 0x04, 0x04);
  reg(0x24, 0x0A); reg(0x25, 0x0A);
  mod(0x28, 0x78, 0x40); mod(0x29, 0x78, 0x40);
  reg(0x00, 0);
}

static void audio_lock(int on) { if (on) spin_lock_unsafe_blocking(lock); else spin_unlock_unsafe(lock); }

/* a buffer went out: mix the next frames into it, in short turns at the lock */
static void __not_in_flash_func(refill)(uint32_t *b) {
  int16_t *s = (int16_t *)(void *)b; int f, k;
  for (k = 0; k < FRAMES; k += 64) { spin_lock_unsafe_blocking(lock); rim_audio_mix(s + k * 2, 64); spin_unlock_unsafe(lock); }
  for (f = 0; f < FRAMES; f++) b[f] = b[f] << 16 | b[f] >> 16; /* (memory has left first, in the low half) */
}
static void __not_in_flash_func(audio_done)(void) {
  int k;
  for (k = 0; k < 2; k++)
    if (dma_hw->ints0 & (1u << ch[k])) {
      dma_hw->ints0 = 1u << ch[k];
      dma_channel_set_read_addr(ch[k], buf[k], false); /* (it starts when the other one chains to it) */
      refill(buf[k]);
    }
}

void rim_audio_setup(void) {
  lock = spin_lock_init(spin_lock_claim_unused(true));
  rim_audio_lock = audio_lock;
  dac_setup();
}

/* on core 1, after the video: the PIO, the DMA turns, and the interrupt that mixes */
void rim_audio_start(void) {
  PIO pio = pio2; uint sm = (uint)pio_claim_unused_sm(pio, true), off = pio_add_program(pio, &rim_i2s_program);
  pio_sm_config c = rim_i2s_program_get_default_config(off); int k;
  pio_gpio_init(pio, PIN_DIN); pio_gpio_init(pio, PIN_BCLK); pio_gpio_init(pio, PIN_BCLK + 1);
  pio_sm_set_consecutive_pindirs(pio, sm, PIN_DIN, 1, true);
  pio_sm_set_consecutive_pindirs(pio, sm, PIN_BCLK, 2, true);
  sm_config_set_out_pins(&c, PIN_DIN, 1);
  sm_config_set_sideset_pins(&c, PIN_BCLK);
  sm_config_set_out_shift(&c, false, true, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (RIM_RATE * 64.0f));
  pio_sm_init(pio, sm, off, &c);
  refill(buf[0]); refill(buf[1]);
  ch[0] = dma_claim_unused_channel(true); ch[1] = dma_claim_unused_channel(true);
  for (k = 0; k < 2; k++) {
    dma_channel_config d = dma_channel_get_default_config(ch[k]);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, true); channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&d, ch[k ^ 1]);
    dma_channel_configure(ch[k], &d, &pio->txf[sm], buf[k], FRAMES, false);
  }
  dma_hw->ints0 = (1u << ch[0]) | (1u << ch[1]);
  dma_hw->inte0 |= (1u << ch[0]) | (1u << ch[1]);
  irq_set_exclusive_handler(DMA_IRQ_0, audio_done);
  irq_set_priority(DMA_IRQ_0, PICO_LOWEST_IRQ_PRIORITY); /* (the video's goes first) */
  irq_set_enabled(DMA_IRQ_0, true);
  dma_channel_start(ch[0]);
  pio_sm_set_enabled(pio, sm, true);
}

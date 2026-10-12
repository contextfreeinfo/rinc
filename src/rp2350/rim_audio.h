/* sound out of the 3.5 mm jack (see audio.c) */
#ifndef RIM_RP2350_AUDIO_H
#define RIM_RP2350_AUDIO_H
void rim_audio_setup(void); /* on core 0, at boot: the DAC, over I2C */
void rim_audio_start(void); /* on core 1, after the video: I2S, DMA, mixing */
#endif

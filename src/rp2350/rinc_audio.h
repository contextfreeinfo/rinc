/* sound out of the 3.5 mm jack (see audio.c) */
#ifndef rinc_RP2350_AUDIO_H
#define rinc_RP2350_AUDIO_H
void rinc_audioSetup(void); /* on core 0, at boot: the DAC, over I2C */
void rinc_audioStart(void); /* on core 1, after the video: I2S, DMA, mixing */
#endif

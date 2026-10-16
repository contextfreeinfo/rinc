/* HDMI out, run by core 1, showing rinc_screen (see video.c) */
#ifndef rinc_VIDEO_H
#define rinc_VIDEO_H
#include <stdint.h>
void rinc_videoStart(void);
void rinc_hdmiSound(const int16_t *stereo, int frames); /* mixed sound, for HDMI too (core 1 only) */
extern volatile uint32_t rinc_frames; /* frames shown: it changes at each vertical blank */
#endif

/* HDMI out, run by core 1, showing rim_screen (see video.c) */
#ifndef RIM_VIDEO_H
#define RIM_VIDEO_H
#include <stdint.h>
void rim_video_start(void);
void rim_hdmi_sound(const int16_t *stereo, int frames); /* mixed sound, for HDMI too (core 1 only) */
extern volatile uint32_t rim_frames; /* frames shown: it changes at each vertical blank */
#endif

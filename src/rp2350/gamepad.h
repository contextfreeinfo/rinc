/* Xbox controllers (gamepad.c): player 1's buttons (as rim_buttons' bits) and Xbox button, read with
   rim_pad_read; rim_axes too */
#ifndef RIM_GAMEPAD_H
#define RIM_GAMEPAD_H
#include <stdint.h>
extern uint16_t rim_pad_buttons;
extern uint8_t rim_pad_guide, rim_pad_count;
void rim_pad_notes(void (*note)(const char *fmt, unsigned a, unsigned b, unsigned c));
void rim_pad_task(void); /* with tuh_task */
void rim_pad_read(void); /* before a frame */
#endif

/* Xbox controllers (gamepad.c): player 1's buttons (as rinc_buttons' bits) and Xbox button, read with
   rinc_padRead; rinc_axes too */
#ifndef rinc_GAMEPAD_H
#define rinc_GAMEPAD_H
#include <stdint.h>
extern uint16_t rinc_padButtons;
extern uint8_t rinc_padGuide, rinc_padCount;
void rinc_padNotes(void (*note)(const char *fmt, unsigned a, unsigned b, unsigned c));
void rinc_padTask(void); /* with tuh_task */
void rinc_padRead(void); /* before a frame */
#endif

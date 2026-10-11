/* USB keyboards (see keyboard.c) */
#ifndef RIM_KEYBOARD_H
#define RIM_KEYBOARD_H
#include <stdint.h>
void rim_keyboard_setup(void);       /* before stdio_init_all */
void rim_keyboard_start(void);
const char *rim_keyboard_events(void); /* devices seen, as text */
int rim_keyboard_char(void);         /* runs the USB host; the next typed character, or -1 */
extern uint8_t rim_keys_down[32];    /* bit k: HID usage k is held down */
#endif

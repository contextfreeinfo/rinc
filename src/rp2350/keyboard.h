/* USB keyboards (see keyboard.c) */
#ifndef rinc_KEYBOARD_H
#define rinc_KEYBOARD_H
#include <stdint.h>
void rinc_keyboardSetup(void);       /* before stdio_init_all */
void rinc_keyboardStart(void);
const char *rinc_keyboardEvents(void); /* devices seen, as text */
int rinc_keyboardChar(void);         /* runs the USB host; the next typed character, or -1 */
extern uint8_t rinc_keysDown[32];    /* bit k: HID usage k is held down */
#endif

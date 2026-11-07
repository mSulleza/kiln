#ifndef INPUT_H
#define INPUT_H

enum {
	K_NONE = -1,
	K_UP, K_DOWN, K_LEFT, K_RIGHT, /* d-pad first: these auto-repeat */
	K_A, K_B, K_X, K_Y, K_L, K_R, K_SELECT, K_START, K_MENU,
	K_COUNT
};

int  input_open(const char *name);  /* evdev device by name, e.g. "TRIMUI Player1" */
int  input_next(int timeout_ms);    /* next key press, or K_NONE on timeout/signal */
int  input_held(int k);
int  input_down_now(int code);      /* kernel's current state of an evdev key code */
void input_flush(void);
const char *input_last(void);       /* last raw press, for mapping checks */

#endif

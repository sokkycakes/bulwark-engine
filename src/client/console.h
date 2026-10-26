/* Bulwark engine - drop-down console text and line editing.
 * ISC licence, see LICENSE. */
#ifndef BW_CONSOLE_H
#define BW_CONSOLE_H

#include <stdbool.h>

#define CON_LINES 1024
#define CON_INPUT 256

void Con_Init(void);
void Con_ToggleConsole(void);
bool Con_IsActive(void);
void Con_SetActive(bool active);
void Con_KeyEvent(int key, int unicode);
/* lines[0] is the newest; returns NULL past the end */
const char *Con_Line(int back);
const char *Con_InputLine(int *cursor);
double Con_LineTime(int back); /* realtime when the line was printed */
int Con_Scroll(void);

#endif

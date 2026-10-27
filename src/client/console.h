/* Bulwark engine - drop-down console text and line editing.
 * ISC licence, see LICENSE. */
#ifndef BW_CONSOLE_H
#define BW_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>

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
/* what Tab would complete at the cursor: the number of matches, -1 when
 * there is only a hint (the command's description), 0 for nothing */
int Con_Suggestions(void);
/* match i, with its value or description in info; selected when Tab put it in */
const char *Con_Suggestion(int i, char *info, size_t size, bool *selected);
/* chat input opened by messagemode (1) or messagemode2 (2), 0 when closed */
int Con_ChatMode(void);
const char *Con_ChatInput(void);
void Con_ChatKey(int key, int unicode);

#endif

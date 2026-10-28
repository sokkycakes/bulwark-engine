/* --- CSNZ front end ------------------------------------------------------- */

/* the layouts are drawn for 1024x768 (INFERRED: the loading dialog is
   1024x768 and the lobby panels tile that size); the viewport scales them
   to fit and centres them */
#define VP_WIDTH 1024
#define VP_HEIGHT 768
vector g_vp;
float g_vpScale;

/* each panel's root position in the 1024x768 layout */
.vector csnzResPos;
.entity csnzRes;

vguiWidget g_uiDesktop;
vguiWindow g_modal;	/* while set, only this window gets input */

void MsgBox_Show(string title, string text);
void Nav_Command(vguiWidget, string);

/* raised to the top at the start of the next frame: a window opened from a
   click must wait, since the clicked window refocuses itself afterwards */
vguiWindow g_focusNext;
bool g_leftMatch;	/* left the match with the in-game menu's Leave */

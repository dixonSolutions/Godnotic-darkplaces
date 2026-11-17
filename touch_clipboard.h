#ifndef TOUCH_CLIPBOARD_H
#define TOUCH_CLIPBOARD_H

/*
Godnotic: the engine's side of the system clipboard (touch_clipboard.c).

    TouchClip_Init        the clip_copy / clip_paste / clip_line commands
    TouchClip_Set         put text on the system clipboard; false when this
                          build has no SDL text API
    TouchClip_Frame       a long press on a console line copies it (touch)

Ctrl+C and Ctrl+V in the console and the chat line are in keys.c, with a
Touch-marked hook: they copy the line being edited and paste the clipboard.
*/

qbool TouchClip_Set(const char *text);
void TouchClip_Init(void);
void TouchClip_Frame(void);

#endif

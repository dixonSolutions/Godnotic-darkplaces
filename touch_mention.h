#ifndef TOUCH_MENTION_H
#define TOUCH_MENTION_H

/* Chat @mentions, the "is typing" line, and the chat log (Godnotic). */

void TouchMention_Init(void);
/* touch_chatlog.c: a printed chat line, to learn who is talking. */
void TouchMention_NoteChat(const char *line);
/* console.c, Con_DrawInput: where the chat line is being drawn this frame. */
void TouchMention_NoteInput(float x, float y, float size);
/* keys.c: is this key (or mouse button) held? */
qbool TouchKey_IsDown(int key);
/* netconn.c: a connectionless "statusResponse" to our roster query; true when it was ours. */
qbool TouchMention_StatusPacket(const char *string, int length);
/* cl_screen.c, SCR_DrawScreen: once a frame, publishes the roster and who is typing. */
void TouchMention_Frame(void);
/* console.c, Con_DrawNotify: x, v is where the chat input line goes, size its height. */
void TouchMention_Draw(float x, float v, float size);
/* keys.c, Key_Message: true when the picker took the key. */
qbool TouchMention_Key(int key, int ascii);

/* console.c, Con_MaskPrint: the first byte of each printed line, and each finished line. */
void TouchChatLog_Lead(char lead);
void TouchChatLog_Line(const char *line);
/* console.c: how long a chat line stays in the feed (the stock con_chattime unless the log is on). */
float TouchChatLog_Fade(float stock);
/* keys.c, Key_Event in the game: wheel and pointer over the log box. True when taken. */
qbool TouchChatLog_Key(int key, qbool down);
/* Draws the scrolled-back view and the export icon; x, y, w, h is the chat area. */
void TouchChatLog_Draw(float x, float y, float w, float size, int stock_lines);
void TouchChatLog_Init(void);

#endif

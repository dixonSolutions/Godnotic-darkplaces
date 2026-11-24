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
/* console.c: the same for the notify area, so a cleared log leaves no stock line behind. */
float TouchChatLog_NotifyFade(float stock);
struct cvar_s;
void TouchChatLog_NotifyBegin(struct cvar_s *notifytime);
void TouchChatLog_NotifyEnd(struct cvar_s *notifytime);
/* keys.c, Key_Message: wheel, page keys and a click on the log's icons while the chat input is open. True when taken. */
qbool TouchChatLog_Key(int key, qbool down);
/* keys.c, Key_Event in the game: while the log is open, the wheel, page keys and Escape are its. True when taken. */
qbool TouchChatLog_GameKey(int key, qbool down);
/* cl_screen.c, SCR_DrawScreen: the open log, drawn last so it is over every touch control. */
void TouchChatLog_DrawOverlay(void);
/* touch_vote.c, TouchVote_Finger: a finger on the feed's icons or names, or anywhere while the log is open. True when taken. */
qbool TouchChatLog_Finger(int type, int id, float nx, float ny);
/* console.c, Con_DrawNotify: the open log draws the chat input itself. */
qbool TouchChatLog_PanelHasInput(void);
/* Draws the chat feed (events with icons), its toolbar and, while open, the log; x, y, w, size, lines is the chat area. */
void TouchChatLog_Draw(float x, float y, float w, float size, int stock_lines);
void TouchChatLog_Init(void);

#endif

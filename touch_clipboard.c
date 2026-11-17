/*
Godnotic: the system clipboard, for the engine's own text.

The QuakeC renderer already has a Copy (touch_uib_stream.c sets
`_uib_clipboard`, which the engine puts on the system clipboard). This gives
the engine's text the same:

  - Ctrl+C copies the line being edited (the console's or the chat's) and
    Ctrl+V pastes, through the hook in keys.c (darkplaces already pasted);
  - a long press on a console line copies that line, and the chat is printed
    into the console, so a chat line is copied the same way;
  - `clip_copy [text]`, `clip_paste` and `clip_line [n]` are the same three
    actions as commands, so a menu's Copy/Paste button can call them.

Reading goes through Sys_SDL_GetClipboardData (sys_sdl.c, and the no-op
backends of the server and the browser build). Writing goes through
SDL_SetClipboardText, declared weak so a build without SDL's text API still
links and simply cannot copy.
*/
#include "quakedef.h"
#include "console.h"
#include "keys.h"
#include "vid.h"
#include "touch_clipboard.h"

#if defined(__GNUC__)
extern int SDL_SetClipboardText(const char *text) __attribute__((weak));
#else
extern int SDL_SetClipboardText(const char *text);
#endif

// A finger held this long without moving, on the console, is a long press.
#define TOUCHCLIP_HOLD 0.6
#define TOUCHCLIP_SLOP 0.02f

qbool TouchClip_Set(const char *text)
{
	if (!SDL_SetClipboardText)
		return false;
	SDL_SetClipboardText(text ? text : "");
	return true;
}

static void TouchClip_CopyLine(int n)
{
	char buf[MAX_INPUTLINE];
	if (!Con_LineFromBottom(n, buf, sizeof(buf)) || !buf[0])
	{
		Con_Printf("clip: there is no console line there\n");
		return;
	}
	if (TouchClip_Set(buf))
		Con_Printf("Copied: %s\n", buf);
	else
		Con_Printf("This build cannot write the system clipboard\n");
}

// clip_copy [text]: the argument, else the console line being edited
static void TouchClip_Copy_f(cmd_state_t *cmd)
{
	const char *text = Cmd_Argc(cmd) > 1 ? Cmd_Args(cmd) : key_line;
	while (*text == ' ' || *text == ']')
		text++;
	if (!TouchClip_Set(text))
		Con_Printf("This build cannot write the system clipboard\n");
	else if (text[0])
		Con_Printf("Copied: %s\n", text);
}

// clip_paste: the clipboard into the console line, at the cursor (one line:
// a newline would run the command)
static void TouchClip_Paste_f(cmd_state_t *cmd)
{
	char *cbd, *p;
	int used, len;
	cbd = Sys_SDL_GetClipboardData();
	if (!cbd)
		return;
	for (p = cbd; *p; p++)
		if (*p == '\n' || *p == '\r' || *p == '\t')
			*p = ' ';
	used = (int)strlen(key_line);
	len = (int)strlen(cbd);
	if (key_linepos > used)
		key_linepos = used;
	if (len > (int)sizeof(key_line) - 1 - used)
		len = (int)sizeof(key_line) - 1 - used;
	if (len > 0)
	{
		memmove(key_line + key_linepos + len, key_line + key_linepos, strlen(key_line + key_linepos) + 1);
		memcpy(key_line + key_linepos, cbd, len);
		key_linepos += len;
	}
	Z_Free(cbd);
}

// clip_line [n]: a console line, counted from the newest (1 is the last one)
static void TouchClip_Line_f(cmd_state_t *cmd)
{
	int n = Cmd_Argc(cmd) > 1 ? atoi(Cmd_Argv(cmd, 1)) : 1;
	TouchClip_CopyLine(n < 0 ? 0 : n);
}

static struct
{
	qbool down;      // a finger was on the console last frame
	qbool copied;    // it already copied: one line per press
	double at;       // realtime it went down
	float x, y;      // where it went down
} press;

// The long press: a finger on the console, still, for TOUCHCLIP_HOLD.
void TouchClip_Frame(void)
{
	int active = 0;
	float x = 0, y = 0;
	char buf[MAX_INPUTLINE];
	double now = host.realtime;

	if (key_dest != key_console)
	{
		press.down = false;
		return;
	}
	VID_GetTouchFinger(0, &active, &x, &y);
	if (!active)
	{
		press.down = false;
		return;
	}
	if (!press.down)
	{
		press.down = true;
		press.copied = false;
		press.at = now;
		press.x = x;
		press.y = y;
		return;
	}
	if (press.copied || now - press.at < TOUCHCLIP_HOLD)
		return;
	if (fabs(x - press.x) > TOUCHCLIP_SLOP || fabs(y - press.y) > TOUCHCLIP_SLOP)
		return; // a scroll or a drag, not a long press
	if (Con_LineAtScreenY(y, buf, sizeof(buf)) && buf[0])
	{
		if (TouchClip_Set(buf))
			Con_Printf("Copied: %s\n", buf);
	}
	press.copied = true;
}

void TouchClip_Init(void)
{
	Cmd_AddCommand(CF_CLIENT, "clip_copy", TouchClip_Copy_f, "Godnotic: copy text (or the console line being edited) to the system clipboard");
	Cmd_AddCommand(CF_CLIENT, "clip_paste", TouchClip_Paste_f, "Godnotic: paste the system clipboard into the console line");
	Cmd_AddCommand(CF_CLIENT, "clip_line", TouchClip_Line_f, "Godnotic: copy a console line to the system clipboard (clip_line [n], 1 is the last line)");
}

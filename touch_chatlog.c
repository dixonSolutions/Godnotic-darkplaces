/*
 * touch_chatlog.c -- the chat log (Godnotic).
 *
 * Keeps what stock Xonotic puts in its chat box: player chat (first byte \1)
 * and the game's own chat-box lines (\3: joins, leaves, spectating, votes,
 * whatever the server's notifications print there), read from the console
 * print stream in console.c. Nothing is invented; a line the client does not
 * print is not here.
 *
 *  - ring buffer of TCL_CAP lines (touch_chatlog_logic.h)
 *  - lines fade from the feed after touch_chatlog_fade seconds (60); the log
 *    keeps them
 *  - an invisible box over the chat area scrolls back through the whole log.
 *    It never fades or disappears: it grows with the lines on show and the
 *    chat size, between a minimum (so it can be grabbed with nothing on show)
 *    and a maximum. Desktop: wheel / PageUp / PageDown while the chat input is
 *    open. Touch: the QuakeC chat layer drags in the box and runs
 *    touch_chatlog_scroll (the box is published in _touch_chatlog_rect).
 *  - one icon beside the box saves the log as text (touch_chatlog_export).
 *    Colour codes stripped, timestamps added. Written to touch_chatlog.txt in
 *    the user's data directory, which the Android app picks up and offers to
 *    the share sheet and saves to Downloads; elsewhere it is copied to
 *    ~/Downloads (the XDG download directory).
 */
#include "quakedef.h"
#include "touch_mention.h"
#define TCL_SNPRINTF dpsnprintf
#include "touch_chatlog_logic.h"

static cvar_t touch_chatlog = {CF_CLIENT | CF_ARCHIVE, "touch_chatlog", "1",
	"1 keeps a chat log (messages and the game's chat-box lines), fades feed lines after touch_chatlog_fade seconds and lets the chat area be scrolled back"};
static cvar_t touch_chatlog_fade = {CF_CLIENT | CF_ARCHIVE, "touch_chatlog_fade", "60",
	"seconds a chat line stays in the feed before it fades (it stays in the log)"};
static cvar_t touch_chatlog_rect = {CF_CLIENT, "_touch_chatlog_rect", "",
	"the scroll box and export icon, as fractions of the screen: x y w h ix iy iw ih (read by the touch layer)"};

static tcl_log_t cl_log;
static char tcl_pending_lead;
static int tcl_scroll;      /* lines back from the newest; 0 is the live feed */
static double tcl_scroll_time;
static int tcl_box_lines = 2;
static float tcl_ix, tcl_iy, tcl_isz;

void TouchChatLog_Lead(char lead)
{
	tcl_pending_lead = (lead == 1 || lead == 3) ? lead : 0;
}

void TouchChatLog_Line(const char *line)
{
	if (!tcl_pending_lead)
		return;
	tcl_pending_lead = 0;
	if (!touch_chatlog.integer)
		return;
	/* the printer prefixes "^7^3"; the text follows */
	TouchChatLog_Add(&cl_log, host.realtime, time(NULL), line);
	if (tcl_scroll) /* keep the same lines under the finger */
		tcl_scroll++;
}

float TouchChatLog_Fade(float stock)
{
	if (!touch_chatlog.integer)
		return stock;
	if (tcl_scroll > 0)
		return 0.0001f; /* the scrolled view replaces the live feed */
	return touch_chatlog_fade.value > 0 ? touch_chatlog_fade.value : stock;
}

static void TCL_Scroll(int delta)
{
	tcl_scroll = TouchChatLog_ClampScroll(&cl_log, tcl_scroll + delta, tcl_box_lines);
	tcl_scroll_time = host.realtime;
}

qbool TouchChatLog_Key(int key, qbool down)
{
	if (!touch_chatlog.integer || !down || key_dest != key_message)
		return false;
	switch (key)
	{
	case K_MWHEELUP: TCL_Scroll(3); return true;
	case K_MWHEELDOWN: TCL_Scroll(-3); return true;
	case K_PGUP: TCL_Scroll(tcl_box_lines); return true;
	case K_PGDN: TCL_Scroll(-tcl_box_lines); return true;
	}
	return false;
}

static void TCL_Scroll_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) >= 2)
		TCL_Scroll(atoi(Cmd_Argv(cmd, 1)));
}

static void TCL_Export_f(cmd_state_t *cmd)
{
	static char text[TCL_CAP * (TCL_LEN + 16)];
	size_t n = TouchChatLog_Export(&cl_log, text, sizeof(text));
	(void)cmd;
	if (!n)
	{
		Con_Printf("chat log: nothing to save\n");
		return;
	}
	if (!FS_WriteFile("touch_chatlog.txt", text, (fs_offset_t)n))
	{
		Con_Printf("chat log: could not write touch_chatlog.txt\n");
		return;
	}
#ifdef __ANDROID__
	Con_Printf("chat log: %d bytes written; the app offers it to the share sheet and Downloads\n", (int)n);
#else
	{
		const char *home = getenv("XDG_DOWNLOAD_DIR");
		char path[MAX_OSPATH], base[MAX_OSPATH];
		FILE *f;
		struct tm t;
		time_t now = time(NULL);
		localtime_r(&now, &t);
		if (home && *home)
			dpsnprintf(base, sizeof(base), "%s", home);
		else if (getenv("HOME"))
			dpsnprintf(base, sizeof(base), "%s/Downloads", getenv("HOME"));
		else
			dpsnprintf(base, sizeof(base), "%s", fs_gamedir);
		dpsnprintf(path, sizeof(path), "%s/godnotic-chat-%04d%02d%02d-%02d%02d%02d.txt", base,
			t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
		f = fopen(path, "wb");
		if (!f)
		{
			Con_Printf("chat log: could not write %s (saved in the data directory instead)\n", path);
			return;
		}
		fwrite(text, 1, n, f);
		fclose(f);
		Con_Printf("chat log saved to %s\n", path);
	}
#endif
}

void TouchChatLog_Init(void)
{
	Cvar_RegisterVariable(&touch_chatlog);
	Cvar_RegisterVariable(&touch_chatlog_fade);
	Cvar_RegisterVariable(&touch_chatlog_rect);
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_scroll", TCL_Scroll_f, "touch_chatlog_scroll <lines>: scroll the chat log back (positive) or forward");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_export", TCL_Export_f, "save the chat log as text (share sheet and Downloads on Android, ~/Downloads elsewhere)");
}

void TouchChatLog_Draw(float x, float y, float w, float size, int stock_lines)
{
	unsigned visible;
	int lines, i, maxl = stock_lines > 0 ? stock_lines : 8;
	char rect[128];
	if (!touch_chatlog.integer || cls.state != ca_connected || size <= 0)
		return;
	if (tcl_scroll && host.realtime - tcl_scroll_time > 20)
		tcl_scroll = 0; /* nobody is reading back: return to the live feed */
	visible = TouchChatLog_Visible(&cl_log, host.realtime, touch_chatlog_fade.value > 0 ? touch_chatlog_fade.value : 15);
	lines = TouchChatLog_BoxLines(visible, 2, maxl);
	tcl_box_lines = lines;
	tcl_scroll = TouchChatLog_ClampScroll(&cl_log, tcl_scroll, lines);

	/* the export icon: three linked dots (share), just outside the box's top right */
	tcl_isz = size * 1.6f;
	tcl_ix = x + w - tcl_isz;
	tcl_iy = max(0.0f, y - tcl_isz - 2);
	{
		float a = tcl_scroll ? 0.9f : 0.35f, s = tcl_isz;
		DrawQ_Fill(tcl_ix, tcl_iy, s, s, 0.02f, 0.04f, 0.08f, a * 0.6f, 0);
		DrawQ_Fill(tcl_ix + s * 0.2f, tcl_iy + s * 0.4f, s * 0.2f, s * 0.2f, 1, 1, 1, a, 0);
		DrawQ_Fill(tcl_ix + s * 0.6f, tcl_iy + s * 0.15f, s * 0.2f, s * 0.2f, 1, 1, 1, a, 0);
		DrawQ_Fill(tcl_ix + s * 0.6f, tcl_iy + s * 0.65f, s * 0.2f, s * 0.2f, 1, 1, 1, a, 0);
		DrawQ_Line(1, tcl_ix + s * 0.3f, tcl_iy + s * 0.5f, tcl_ix + s * 0.7f, tcl_iy + s * 0.25f, 1, 1, 1, a, 0);
		DrawQ_Line(1, tcl_ix + s * 0.3f, tcl_iy + s * 0.5f, tcl_ix + s * 0.7f, tcl_iy + s * 0.75f, 1, 1, 1, a, 0);
	}
	dpsnprintf(rect, sizeof(rect), "%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
		x / vid_conwidth.value, y / vid_conheight.value, w / vid_conwidth.value, lines * size / vid_conheight.value,
		tcl_ix / vid_conwidth.value, tcl_iy / vid_conheight.value, tcl_isz / vid_conwidth.value, tcl_isz / vid_conheight.value, size / vid_conheight.value);
	if (strcmp(touch_chatlog_rect.string, rect))
		Cvar_SetQuick(&touch_chatlog_rect, rect);

	if (!tcl_scroll)
		return; /* the engine draws the live feed */
	DrawQ_Fill(x, y, w, lines * size, 0.02f, 0.04f, 0.08f, 0.7f, 0);
	for (i = 0; i < lines; i++)
	{
		int idx = (int)cl_log.count - tcl_scroll - lines + i;
		const tcl_entry_t *e = idx >= 0 ? TouchChatLog_Get(&cl_log, (unsigned)idx) : NULL;
		if (e)
			DrawQ_String(x, y + i * size, e->text, 0, size, size, 1, 1, 1, 1, 0, NULL, false, FONT_CHAT);
	}
}

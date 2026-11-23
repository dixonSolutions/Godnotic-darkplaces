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
 *  - four plain outline icons in a row beside the box, left to right: clear
 *    (touch_chatlog_clear: empties the log, the scroll and the lines the stock
 *    chat and notify areas still hold, see TouchChatLog_Fade), copy
 *    (touch_chatlog_copy: the export text on the clipboard), download
 *    (touch_chatlog_download) and share (touch_chatlog_share, Android's share
 *    sheet). touch_chatlog_export does share and download together. On a
 *    desktop the mouse can click the icons while the chat input is open.
 *    The export text:
 *    Colour codes stripped, timestamps added. Written to touch_chatlog.txt in
 *    the user's data directory, which the Android app picks up and offers to
 *    the share sheet and saves to Downloads; elsewhere it is copied to
 *    ~/Downloads (the XDG download directory).
 */
#include "quakedef.h"
#include "touch_mention.h"
#include "touch_clipboard.h"
#define TCL_SNPRINTF dpsnprintf
#include "touch_chatlog_logic.h"

static cvar_t touch_chatlog = {CF_CLIENT | CF_ARCHIVE, "touch_chatlog", "1",
	"1 keeps a chat log (messages and the game's chat-box lines), fades feed lines after touch_chatlog_fade seconds and lets the chat area be scrolled back"};
static cvar_t touch_chatlog_fade = {CF_CLIENT | CF_ARCHIVE, "touch_chatlog_fade", "60",
	"seconds a chat line stays in the feed before it fades (it stays in the log)"};
static cvar_t touch_chatlog_rect = {CF_CLIENT, "_touch_chatlog_rect", "",
	"the scroll box, export icon and clear icon, as fractions of the screen: x y w h ix iy iw ih lineh, then clear, copy and download icons as x y w h each (read by the touch layer; fields are only ever appended)"};

static tcl_log_t cl_log;
static char tcl_pending_lead;
static int tcl_scroll;      /* lines back from the newest; 0 is the live feed */
static double tcl_scroll_time;
static int tcl_box_lines = 2;
static float tcl_ix[TCL_ICONS], tcl_iy, tcl_isz; /* slot 0 share, 1 download, 2 copy, 3 clear */
static double tcl_copied_at = -100;
static double tcl_cleared_at = -1; /* cl.time of the last clear: stock lines older than this stay hidden */

void TouchChatLog_Lead(char lead)
{
	tcl_pending_lead = (lead == 1 || lead == 3) ? lead : 0;
}

void TouchChatLog_Line(const char *line)
{
	if (!tcl_pending_lead)
		return;
	if (tcl_pending_lead == 1)
		TouchMention_NoteChat(line);
	tcl_pending_lead = 0;
	if (!touch_chatlog.integer)
		return;
	/* the printer prefixes "^7^3"; the text follows */
	TouchChatLog_Add(&cl_log, host.realtime, time(NULL), line);
	if (tcl_scroll) /* keep the same lines under the finger */
		tcl_scroll++;
}

/* The stock notify area (chat lines too when the chat area is off) hides what the clear emptied. */
float TouchChatLog_NotifyFade(float stock)
{
	return TouchChatLog_FadeAfterClear(stock, cl.time, tcl_cleared_at);
}

/* console.c's notify draw: swap con_notifytime for the cleared-log fade around
 * the one upstream call, then put it back (hooks on their own lines there). */
static float tcl_saved_notifytime;
void TouchChatLog_NotifyBegin(struct cvar_s *notifytime)
{
	tcl_saved_notifytime = notifytime->value;
	notifytime->value = TouchChatLog_NotifyFade(tcl_saved_notifytime);
}

void TouchChatLog_NotifyEnd(struct cvar_s *notifytime)
{
	notifytime->value = tcl_saved_notifytime;
}

float TouchChatLog_Fade(float stock)
{
	stock = TouchChatLog_NotifyFade(stock);
	if (!touch_chatlog.integer)
		return stock;
	if (tcl_scroll > 0)
		return 0.0001f; /* the scrolled view replaces the live feed */
	return touch_chatlog_fade.value > 0 ? TouchChatLog_FadeAfterClear(touch_chatlog_fade.value, cl.time, tcl_cleared_at) : stock;
}

static void TCL_Scroll(int delta)
{
	tcl_scroll = TouchChatLog_ClampScroll(&cl_log, tcl_scroll + delta, tcl_box_lines);
	tcl_scroll_time = host.realtime;
}

static void TCL_Share_f(cmd_state_t *cmd);
static void TCL_Download_f(cmd_state_t *cmd);
static void TCL_Copy_f(cmd_state_t *cmd);
static void TCL_Clear_f(cmd_state_t *cmd);

qbool TouchChatLog_Key(int key, qbool down)
{
	if (!touch_chatlog.integer || !down || key_dest != key_message)
		return false;
	if (key == K_MOUSE1 && vid.mode.width > 0 && tcl_isz > 0)
	{
		/* a mouse (or a finger's click) on an icon while the chat input is open */
		float mx = in_windowmouse_x * vid_conwidth.value / vid.mode.width;
		float my = in_windowmouse_y * vid_conheight.value / vid.mode.height;
		static void (*const act[TCL_ICONS])(cmd_state_t *) = {TCL_Share_f, TCL_Download_f, TCL_Copy_f, TCL_Clear_f};
		int k;
		for (k = 0; k < TCL_ICONS; k++)
			if (TouchChatLog_InIcon(mx, my, tcl_ix[k], tcl_iy, tcl_isz))
			{
				act[k](NULL);
				return true;
			}
		return false;
	}
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

static void TCL_Clear_f(cmd_state_t *cmd)
{
	(void)cmd;
	TouchChatLog_Clear(&cl_log);
	tcl_scroll = 0;
	tcl_scroll_time = host.realtime;
	tcl_cleared_at = cl.time;
	Con_Printf("chat log cleared\n");
}

/* mode bits: 1 share sheet (Android), 2 download. The Android app tells them
 * apart by the file name: touch_chatlog_share.txt, touch_chatlog_download.txt,
 * or touch_chatlog.txt for both (touch_chatlog_export). */
static void TCL_Write(int mode)
{
	static char text[TCL_CAP * (TCL_LEN + 16)];
	size_t n = TouchChatLog_Export(&cl_log, text, sizeof(text));
	if (!n)
	{
		Con_Printf("chat log: nothing to save\n");
		return;
	}
#ifdef __ANDROID__
	{
		const char *name = mode == 3 ? "touch_chatlog.txt" : mode == 1 ? "touch_chatlog_share.txt" : "touch_chatlog_download.txt";
		if (!FS_WriteFile(name, text, (fs_offset_t)n))
		{
			Con_Printf("chat log: could not write %s\n", name);
			return;
		}
		Con_Printf("chat log: %d bytes written; the app %s\n", (int)n, mode == 1 ? "offers it to the share sheet" : mode == 2 ? "saves it to Downloads" : "offers it to the share sheet and Downloads");
	}
#else
	if (!(mode & 2))
	{
		Con_Printf("chat log: the share sheet is a phone feature; use download or copy here\n");
		return;
	}
	if (!FS_WriteFile("touch_chatlog.txt", text, (fs_offset_t)n))
	{
		Con_Printf("chat log: could not write touch_chatlog.txt\n");
		return;
	}
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

static void TCL_Export_f(cmd_state_t *cmd) { (void)cmd; TCL_Write(3); }
static void TCL_Share_f(cmd_state_t *cmd) { (void)cmd; TCL_Write(1); }
static void TCL_Download_f(cmd_state_t *cmd) { (void)cmd; TCL_Write(2); }

static void TCL_Copy_f(cmd_state_t *cmd)
{
	static char text[TCL_CAP * (TCL_LEN + 16)];
	size_t n = TouchChatLog_Export(&cl_log, text, sizeof(text));
	(void)cmd;
	if (!n)
	{
		Con_Printf("chat log: nothing to copy\n");
		return;
	}
	if (!TouchClip_Set(text))
	{
		Con_Printf("chat log: this build cannot write the system clipboard\n");
		return;
	}
	tcl_copied_at = host.realtime;
	Con_Printf("chat log copied (%d bytes)\n", (int)n);
}

void TouchChatLog_Init(void)
{
	Cvar_RegisterVariable(&touch_chatlog);
	Cvar_RegisterVariable(&touch_chatlog_fade);
	Cvar_RegisterVariable(&touch_chatlog_rect);
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_scroll", TCL_Scroll_f, "touch_chatlog_scroll <lines>: scroll the chat log back (positive) or forward");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_clear", TCL_Clear_f, "empty the chat log and the chat lines on show");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_export", TCL_Export_f, "save the chat log as text (share sheet and Downloads on Android, ~/Downloads elsewhere)");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_share", TCL_Share_f, "open the share sheet with the chat log (Android)");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_download", TCL_Download_f, "save the chat log as text to Downloads (~/Downloads elsewhere)");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_copy", TCL_Copy_f, "put the chat log text on the clipboard");
}

void TouchChatLog_Draw(float x, float y, float w, float size, int stock_lines)
{
	unsigned visible;
	int lines, i, maxl = stock_lines > 0 ? stock_lines : 8;
	char rect[320];
	if (!touch_chatlog.integer || cls.state != ca_connected || size <= 0)
		return;
	if (tcl_scroll && host.realtime - tcl_scroll_time > 20)
		tcl_scroll = 0; /* nobody is reading back: return to the live feed */
	visible = TouchChatLog_Visible(&cl_log, host.realtime, touch_chatlog_fade.value > 0 ? touch_chatlog_fade.value : 15);
	lines = TouchChatLog_BoxLines(visible, 2, maxl);
	tcl_box_lines = lines;
	tcl_scroll = TouchChatLog_ClampScroll(&cl_log, tcl_scroll, lines);

	/* four plain outline icons, one size, evenly spaced in a row just outside the
	 * box's top right: clear, copy, download, share (right-most) */
	tcl_isz = size * 1.6f;
	for (i = 0; i < TCL_ICONS; i++)
		tcl_ix[i] = TouchChatLog_IconX(x, w, tcl_isz, i);
	tcl_iy = max(0.0f, y - tcl_isz - 2);
	{
		float a = tcl_scroll ? 0.9f : 0.5f, s = tcl_isz, t = max(1.0f, s * 0.07f);
		for (i = 0; i < TCL_ICONS; i++)
		{
			float ix = tcl_ix[i], iy = tcl_iy;
			DrawQ_Fill(ix, iy, s, s, 0.02f, 0.04f, 0.08f, a * 0.6f, 0);
			switch (i)
			{
			case 0: /* share: three linked dots */
				DrawQ_Fill(ix + s * 0.2f, iy + s * 0.4f, s * 0.2f, s * 0.2f, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.6f, iy + s * 0.15f, s * 0.2f, s * 0.2f, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.6f, iy + s * 0.65f, s * 0.2f, s * 0.2f, 1, 1, 1, a, 0);
				DrawQ_Line(1, ix + s * 0.3f, iy + s * 0.5f, ix + s * 0.7f, iy + s * 0.25f, 1, 1, 1, a, 0);
				DrawQ_Line(1, ix + s * 0.3f, iy + s * 0.5f, ix + s * 0.7f, iy + s * 0.75f, 1, 1, 1, a, 0);
				break;
			case 1: /* download: an arrow down into a tray */
				DrawQ_Fill(ix + s * 0.47f, iy + s * 0.18f, t, s * 0.38f, 1, 1, 1, a, 0);
				DrawQ_Line(1, ix + s * 0.3f, iy + s * 0.42f, ix + s * 0.5f, iy + s * 0.62f, 1, 1, 1, a, 0);
				DrawQ_Line(1, ix + s * 0.5f, iy + s * 0.62f, ix + s * 0.7f, iy + s * 0.42f, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.22f, iy + s * 0.74f, s * 0.56f, t, 1, 1, 1, a, 0);
				break;
			case 2: /* copy: two overlapping outline pages */
				DrawQ_Fill(ix + s * 0.22f, iy + s * 0.22f, s * 0.32f, t, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.22f, iy + s * 0.22f, t, s * 0.4f, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.22f, iy + s * 0.62f, s * 0.12f, t, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.44f, iy + s * 0.38f, s * 0.34f, t, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.44f, iy + s * 0.38f, t, s * 0.4f, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.44f, iy + s * 0.78f, s * 0.34f + t, t, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.78f, iy + s * 0.38f, t, s * 0.4f, 1, 1, 1, a, 0);
				break;
			default: /* clear: a bin, lid and handle above a body with two slots */
				DrawQ_Fill(ix + s * 0.38f, iy + s * 0.14f, s * 0.24f, t, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.2f, iy + s * 0.24f, s * 0.6f, t, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.28f, iy + s * 0.34f, t, s * 0.44f, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.72f - t, iy + s * 0.34f, t, s * 0.44f, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.28f, iy + s * 0.78f, s * 0.44f, t, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.43f, iy + s * 0.42f, t, s * 0.28f, 1, 1, 1, a, 0);
				DrawQ_Fill(ix + s * 0.57f, iy + s * 0.42f, t, s * 0.28f, 1, 1, 1, a, 0);
				break;
			}
		}
		if (host.realtime - tcl_copied_at < 1.5)
			DrawQ_String(tcl_ix[2] - s * 0.4f, tcl_iy - size - 2, "Copied", 0, size, size, 0.6f, 1, 0.6f, 1, 0, NULL, false, FONT_CHAT);
	}
	/* rect: box, share icon, lineh, then clear, copy, download (fields are only appended) */
	dpsnprintf(rect, sizeof(rect), "%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
		x / vid_conwidth.value, y / vid_conheight.value, w / vid_conwidth.value, lines * size / vid_conheight.value,
		tcl_ix[0] / vid_conwidth.value, tcl_iy / vid_conheight.value, tcl_isz / vid_conwidth.value, tcl_isz / vid_conheight.value, size / vid_conheight.value,
		tcl_ix[3] / vid_conwidth.value, tcl_iy / vid_conheight.value, tcl_isz / vid_conwidth.value, tcl_isz / vid_conheight.value,
		tcl_ix[2] / vid_conwidth.value, tcl_iy / vid_conheight.value, tcl_isz / vid_conwidth.value, tcl_isz / vid_conheight.value,
		tcl_ix[1] / vid_conwidth.value, tcl_iy / vid_conheight.value, tcl_isz / vid_conwidth.value, tcl_isz / vid_conheight.value);
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

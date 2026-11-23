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
static float tcl_off;       /* pixels (screen units) the log is pulled down from the live state; 0 is the live feed */
static float tcl_vel;       /* momentum, screen units per second */
static double tcl_last_frame;
static float tcl_line_h = 12;
static int tcl_icon_px;     /* side of the icon art as last made, in device pixels */
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
	tcl_off = TouchChatLog_PinAdjust(tcl_off, tcl_line_h); /* pinned to the newest unless scrolled up */
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
	if (tcl_off > 0.5f)
		return 0.0001f; /* the scrolled view replaces the live feed */
	return touch_chatlog_fade.value > 0 ? TouchChatLog_FadeAfterClear(touch_chatlog_fade.value, cl.time, tcl_cleared_at) : stock;
}

static void TCL_Scroll(int delta)
{
	tcl_off = TouchChatLog_ClampPx(&cl_log, tcl_off + (float)delta * tcl_line_h, tcl_box_lines, tcl_line_h);
	tcl_vel = 0;
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

/* touch_chatlog_drag <fraction of the screen height>: a finger moved that far down (older lines come
 * into view; negative goes back). It also stops any coasting, so a drag of 0 is a touch-down. */
static void TCL_Drag_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) < 2)
		return;
	tcl_off = TouchChatLog_ClampPx(&cl_log, tcl_off + (float)atof(Cmd_Argv(cmd, 1)) * vid_conheight.value, tcl_box_lines, tcl_line_h);
	tcl_vel = 0;
	tcl_scroll_time = host.realtime;
}

/* touch_chatlog_fling <screen heights per second>: the finger lifted at that speed; the log coasts on. */
static void TCL_Fling_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) < 2)
		return;
	tcl_vel = (float)atof(Cmd_Argv(cmd, 1)) * vid_conheight.value;
	tcl_scroll_time = host.realtime;
}

static void TCL_Clear_f(cmd_state_t *cmd)
{
	(void)cmd;
	TouchChatLog_Clear(&cl_log);
	tcl_off = tcl_vel = 0;
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
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_drag", TCL_Drag_f, "touch_chatlog_drag <screen heights>: pixel scroll of the chat log by a finger (positive shows older lines)");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_fling", TCL_Fling_f, "touch_chatlog_fling <screen heights per second>: let the chat log coast after a flick");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_clear", TCL_Clear_f, "empty the chat log and the chat lines on show");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_export", TCL_Export_f, "save the chat log as text (share sheet and Downloads on Android, ~/Downloads elsewhere)");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_share", TCL_Share_f, "open the share sheet with the chat log (Android)");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_download", TCL_Download_f, "save the chat log as text to Downloads (~/Downloads elsewhere)");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_copy", TCL_Copy_f, "put the chat log text on the clipboard");
}

/* The icon art at the screen's real pixel size (see TouchChatLog_IconRaster), kept as pixels and
 * redone only when the size changes. It is drawn as runs of equal pixels (DrawQ_Fill on the device
 * pixel grid), so it is crisp whatever the renderer does with textures and costs a few dozen quads. */
static unsigned char tcl_icon_pix[TCL_ICONS][128 * 128 * 4];

static void TCL_MakeIcons(int px)
{
	int k;
	if (px > 128)
		px = 128;
	if (px < 8)
		px = 8;
	if (px == tcl_icon_px)
		return;
	for (k = 0; k < TCL_ICONS; k++)
		TouchChatLog_IconRaster(k, px, tcl_icon_pix[k]);
	tcl_icon_px = px;
}

static void TCL_DrawIcon(int k, float x, float y, float pxs, float alpha)
{
	int px = tcl_icon_px, row, col;
	const unsigned char *p = tcl_icon_pix[k];
	for (row = 0; row < px; row++)
	{
		col = 0;
		while (col < px)
		{
			const unsigned char *c = p + ((size_t)row * px + col) * 4;
			int run = 1;
			if (c[3] == 0)
			{
				col++;
				continue;
			}
			while (col + run < px && !memcmp(c, c + (size_t)run * 4, 4))
				run++;
			DrawQ_Fill(x + (float)col / pxs, y + (float)row / pxs, (float)run / pxs, 1.0f / pxs,
				c[2] / 255.0f, c[1] / 255.0f, c[0] / 255.0f, c[3] / 255.0f * alpha, 0);
			col += run;
		}
	}
}

void TouchChatLog_Draw(float x, float y, float w, float size, int stock_lines)
{
	unsigned visible;
	int lines, i, maxl = stock_lines > 0 ? stock_lines : 8, show;
	float dt, maxoff;
	char rect[320];
	if (!touch_chatlog.integer || cls.state != ca_connected || size <= 0)
		return;
	dt = tcl_last_frame > 0 ? (float)(host.realtime - tcl_last_frame) : 0;
	tcl_last_frame = host.realtime;
	if (dt > 0.1f)
		dt = 0.1f;
	tcl_line_h = size;
	if (tcl_off > 0 && host.realtime - tcl_scroll_time > 20)
		tcl_off = tcl_vel = 0; /* nobody is reading back: return to the live feed */
	visible = TouchChatLog_Visible(&cl_log, host.realtime, touch_chatlog_fade.value > 0 ? touch_chatlog_fade.value : 15);
	lines = TouchChatLog_BoxLines(visible, 2, maxl);
	tcl_box_lines = lines;
	maxoff = TouchChatLog_MaxPx(&cl_log, lines, size);
	if (tcl_vel != 0 && TouchChatLog_Coast(&tcl_off, &tcl_vel, dt, maxoff))
		tcl_scroll_time = host.realtime;
	tcl_off = TouchChatLog_ClampPx(&cl_log, tcl_off, lines, size);
	/* the stock chat area draws its lines bottom-aligned in a rect of maxl lines:
	 * the box, its icons and the scrolled view sit on those lines, not on the rect's top */
	y += (float)(max(maxl, lines) - lines) * size;

	/* four icons, one size, evenly spaced in a row docked just above the box's first line, from
	 * the chat text's x: clear, copy, download, share. Only while the log holds a line. The art is
	 * made at the screen's own pixel size and drawn 1:1 on the pixel grid. */
	show = TouchChatLog_IconsShown(&cl_log) && vid.mode.width > 0 && vid_conwidth.value > 0;
	if (show)
	{
		float pxs = (float)vid.mode.width / vid_conwidth.value; /* device pixels per screen unit */
		int px = (int)(size * 1.6f * pxs + 0.5f);
		float a = tcl_off > 0.5f ? 0.95f : 0.6f;
		float sz;
		TCL_MakeIcons(px);
		sz = (float)tcl_icon_px / pxs;
		tcl_isz = sz;
		for (i = 0; i < TCL_ICONS; i++)
			tcl_ix[i] = floorf(TouchChatLog_IconX(x, w, sz, i) * pxs + 0.5f) / pxs;
		tcl_iy = floorf(max(0.0f, y - sz - 1) * pxs + 0.5f) / pxs;
		for (i = 0; i < TCL_ICONS; i++)
			TCL_DrawIcon(i, tcl_ix[i], tcl_iy, pxs, a);
		if (host.realtime - tcl_copied_at < 1.5)
			DrawQ_String(tcl_ix[2] - sz * 0.4f, tcl_iy - size - 2, "Copied", 0, size, size, 0.6f, 1, 0.6f, 1, 0, NULL, false, FONT_CHAT);
	}
	else
	{
		tcl_isz = 0;
		for (i = 0; i < TCL_ICONS; i++)
			tcl_ix[i] = -10000;
		tcl_iy = -10000;
	}
	/* rect: box, share icon, lineh, then clear, copy, download (fields are only appended); an icon
	 * that is not shown is published off the screen, with no size, so nothing can hit it */
	{
		float is = tcl_isz, ix[TCL_ICONS], iy = tcl_iy;
		for (i = 0; i < TCL_ICONS; i++)
			ix[i] = show ? tcl_ix[i] : -1;
		if (!show)
			iy = -1;
		dpsnprintf(rect, sizeof(rect), "%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
			x / vid_conwidth.value, y / vid_conheight.value, w / vid_conwidth.value, lines * size / vid_conheight.value,
			ix[0] / vid_conwidth.value, iy / vid_conheight.value, is / vid_conwidth.value, is / vid_conheight.value, size / vid_conheight.value,
			ix[3] / vid_conwidth.value, iy / vid_conheight.value, is / vid_conwidth.value, is / vid_conheight.value,
			ix[2] / vid_conwidth.value, iy / vid_conheight.value, is / vid_conwidth.value, is / vid_conheight.value,
			ix[1] / vid_conwidth.value, iy / vid_conheight.value, is / vid_conwidth.value, is / vid_conheight.value);
	}
	if (strcmp(touch_chatlog_rect.string, rect))
		Cvar_SetQuick(&touch_chatlog_rect, rect);

	if (tcl_off <= 0.5f)
		return; /* the engine draws the live feed */
	/* the scrolled view: only the rows that touch the box are drawn, clipped to it, at a pixel offset */
	DrawQ_Fill(x, y, w, lines * size, 0.02f, 0.04f, 0.08f, 0.7f, 0);
	{
		tcl_window_t win = TouchChatLog_Window(cl_log.count, lines, tcl_off, size);
		int m;
		DrawQ_SetClipArea(x, y, w, lines * size);
		for (m = win.mfirst; m <= win.mlast; m++)
		{
			const tcl_entry_t *e = TouchChatLog_Get(&cl_log, (unsigned)(win.j0 + m));
			if (e)
				DrawQ_String(x, y + (float)m * size + win.frac, e->text, 0, size, size, 1, 1, 1, 1, 0, NULL, false, FONT_CHAT);
		}
		DrawQ_ResetClipArea();
	}
}

/*
 * touch_chatlog.c -- the chat feed, the player events and the chat log (Godnotic).
 *
 * Keeps what stock Xonotic puts in its chat box: player chat (first byte \1)
 * and the game's own chat-box lines (\3: joins, leaves, spectating, teams,
 * votes...), read from the console print stream in console.c. The game's
 * player events are classified (touch_chatlog_logic.h) and shown the same way
 * everywhere: "<name> joined the server", "started playing" / "joined the Red
 * team", "moved to spectators", "left the server", and "is now known as" when
 * a name changes (worked out here from the scoreboard names).
 *
 *  - the FEED over play replaces the stock chat lines: a compact coloured line
 *    per message or event, a small icon per kind, a backdrop and a shadow so it
 *    reads over any scene, text at least 12 dp tall. Lines leave the feed after
 *    touch_chatlog_fade seconds (60); the log keeps them.
 *  - the LOG is a panel over the screen: every line of the session (1000, until
 *    cleared) with its time, an icon, filter chips (All, Chat, Events), scrolled
 *    by mouse wheel, PageUp/PageDown, and by touch drag with a fling. Only the
 *    lines in view are laid out (the virtual list in touch_chatlog_logic.h).
 *    It is kept across maps and after the match ends; touch_chatlog_open (the
 *    pause menu, a bind, the log icon on the feed) shows it.
 *  - ICONS are pictures drawn at exactly the device pixel size they are shown
 *    at, from distance fields: crisp at any density. Every button has a hit
 *    target of at least 44 dp whatever its drawn size. Tapping a name or an
 *    event icon opens actions for that player (mention, copy name, copy line);
 *    the toolbar has log, clear, copy, download and share.
 *  - INPUT is one pointer model (touch_chatlog_ptr down|move|up <fx> <fy>, window
 *    fractions): the QuakeC touch layer forwards fingers that land in
 *    _touch_chatlog_claim, a mouse click arrives from the key hook, tests use the
 *    command. Export text: colour codes stripped, times added; written to
 *    touch_chatlog.txt (the Android app offers it to the share sheet and saves
 *    it to Downloads); elsewhere copied to ~/Downloads.
 */
#include "quakedef.h"
#include "touch_mention.h"
#include "touch_clipboard.h"
#include "touch_ui.h"
#include "touch_hud.h"
#define TCL_SNPRINTF dpsnprintf
#include "touch_chatlog_logic.h"

static cvar_t touch_chatlog = {CF_CLIENT | CF_ARCHIVE, "touch_chatlog", "1",
	"1 keeps a chat log (messages, the game's chat-box lines and player events), shows the chat feed with event icons and lets the log be opened and scrolled"};
static cvar_t touch_chatlog_fade = {CF_CLIENT | CF_ARCHIVE, "touch_chatlog_fade", "60",
	"seconds a chat line stays in the feed before it fades (it stays in the log)"};
static cvar_t touch_chatlog_feed = {CF_CLIENT | CF_ARCHIVE, "touch_chatlog_feed", "1",
	"1 draws the chat feed with event lines and icons; 0 leaves the stock chat lines (the log and its icons stay)"};
static cvar_t touch_chatlog_sound_file = {CF_CLIENT | CF_ARCHIVE, "touch_chatlog_sound_file", "sound/menu/focus.ogg",
	"sound played for player events in the chat log"};
static cvar_t touch_chatlog_sound_volume = {CF_CLIENT | CF_ARCHIVE, "touch_chatlog_sound_volume", "0.25",
	"player event sound volume (0 disables it; maximum 1)"};
static cvar_t touch_chatlog_claim = {CF_CLIENT, "_touch_chatlog_claim", "",
	"where the chat feed, its icons and the log take a finger, as fractions of the screen: x y w h (the whole screen while the log is open; read by the touch layer)"};

static tcl_log_t cl_log;
static char tcl_pending_lead;
static double tcl_cleared_at = -1; /* cl.time of the last clear: stock lines older than this stay hidden */

/* the log panel */
static int tcl_panel;            /* open */
static int tcl_return_chat;      /* return to the composer after its log closes */
static int tcl_filter;           /* 0 all, 1 chat, 2 events */
static tcl_scroller_t tcl_sc;    /* the panel's scroll, in console units from the newest end */
static double tcl_last_frame;
static double tcl_scroll_at = -100;
static unsigned tcl_serial, tcl_slot_serial[TCL_CAP], tcl_selected[TCL_CAP];

static unsigned TCL_Slot(unsigned idx)
{
	return (cl_log.head + TCL_CAP - cl_log.count + idx) % TCL_CAP;
}

static int TCL_Selected(unsigned idx)
{
	unsigned slot = TCL_Slot(idx);
	return tcl_slot_serial[slot] && tcl_selected[slot] == tcl_slot_serial[slot];
}

static unsigned TCL_SelectedCount(void)
{
	unsigned i, n = 0;
	for (i = 0; i < cl_log.count; i++)
		n += TCL_Selected(i);
	return n;
}

static void TCL_ClearSelection(void)
{
	memset(tcl_selected, 0, sizeof(tcl_selected));
}
static unsigned tcl_anchor_slot, tcl_anchor_serial;
static float tcl_anchor_acc;
static int tcl_anchor_valid, tcl_anchor_filter;

static void TCL_Added(void)
{
	tcl_slot_serial[(cl_log.head + TCL_CAP - 1) % TCL_CAP] = ++tcl_serial;
}
static float tcl_body_h, tcl_content_h, tcl_row_h, tcl_newest_top, tcl_newest_bottom, tcl_bottom_pad;
static int tcl_popup;            /* the actions for a player are open */
static char tcl_popup_name[TCL_NAME];
static unsigned tcl_popup_entry;
static float tcl_popup_x, tcl_popup_y;
static char tcl_toast[48];
static double tcl_toast_at = -100;

/* what can be pressed this frame (rebuilt by every draw) */
enum { TG_NONE, TG_LOG, TG_CLEAR, TG_COPY, TG_DOWNLOAD, TG_SHARE, TG_CLOSE, TG_CHIP, TG_NAME, TG_ROW, TG_LATEST, TG_POP, TG_BODY, TG_PANEL };
#define TG_MAX 192
static tcl_target_t tcl_tg[TG_MAX];
static int tcl_ntg;
static float tcl_min_target; /* console units: 44 dp */
static float tcl_slop;
static float tcl_pointer_x, tcl_pointer_y; /* the last press, console units */
static float tcl_claim_r[4];   /* the area that takes a finger, console units */
static double tcl_claim_at = -100;
static float tcl_panel_rect[4];
static int tcl_panel_due;				  /* the log is to be drawn by TouchChatLog_DrawOverlay this frame */
static float tcl_panel_size, tcl_panel_dt;            /* the open log's panel: a press inside it, on nothing, does nothing */

/* the pointer */
static int tcl_ptr_down;
static int tcl_ptr_id, tcl_ptr_arg;
static double tcl_ptr_at;
static int tcl_ptr_held;
static unsigned tcl_ptr_serial;

/* names on the scoreboard, to notice a rename */
static char tcl_seen[MAX_SCOREBOARD][MAX_SCOREBOARDNAME];
static double tcl_conn_at, tcl_event_at = -100;
static int tcl_was_connected;
static double tcl_sound_at = -100;
static unsigned tcl_sound_count;

static void TCL_EventSound(void)
{
	float sound_volume = bound(0, touch_chatlog_sound_volume.value, 1);
	if (!tcl_was_connected || cls.state != ca_connected || cls.signon != SIGNONS ||
	    cls.demoplayback || host.realtime - tcl_conn_at <= 3 ||
	    host.realtime - tcl_sound_at < 2 || sound_volume <= 0 || !touch_chatlog_sound_file.string[0])
		return;
	tcl_sound_at = host.realtime;
	tcl_sound_count++; /* attempts, including when sound output is disabled */
	S_LocalSoundEx(touch_chatlog_sound_file.string, 0, sound_volume);
}

static void TCL_Toast(const char *s)
{
	dp_strlcpy(tcl_toast, s, sizeof(tcl_toast));
	tcl_toast_at = host.realtime;
}

void TouchChatLog_Lead(char lead)
{
	tcl_pending_lead = (lead == 1 || lead == 3) ? lead : 0;
}

void TouchChatLog_Line(const char *line)
{
	char lead = tcl_pending_lead;
	if (!lead)
		return;
	if (lead == 1)
		TouchMention_NoteChat(line);
	tcl_pending_lead = 0;
	if (!touch_chatlog.integer)
		return;
	/* the printer prefixes "^7^3"; the text follows */
	if (TouchChatLog_AddLead(&cl_log, host.realtime, time(NULL), lead, line))
	{
		TCL_Added();
		if (lead == 3)
		{
			const tcl_entry_t *e = TouchChatLog_Get(&cl_log, cl_log.count - 1);
			if (e->kind == TCL_JOIN || e->kind == TCL_LEAVE)
				tcl_event_at = host.realtime;
			if (TCL_EVENTS & TCL_BIT(e->kind))
				TCL_EventSound();
		}
	}
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

/* The stock chat lines: hidden while this feed draws them instead. */
float TouchChatLog_Fade(float stock)
{
	stock = TouchChatLog_NotifyFade(stock);
	if (!touch_chatlog.integer)
		return stock;
	if (touch_chatlog_feed.integer)
		return 0.0001f;
	return touch_chatlog_fade.value > 0 ? TouchChatLog_FadeAfterClear(touch_chatlog_fade.value, cl.time, tcl_cleared_at) : stock;
}

/* the game's own lines in the feed: the stock switch, touch_uib.c */
static qbool TCL_SystemLines(void)
{
	cvar_t *v = Cvar_FindVar(&cvars_all, "touch_chat_system_lines", ~0);
	return v && v->integer;
}

/* ------------------------------------------------------------------ */
/* renames: the scoreboard's names change                               */
/* ------------------------------------------------------------------ */
/* A rename the scoreboard shows is logged a moment later, unless the server announced it ("X changed name to Y"), which it does. */
typedef struct { char old_[MAX_SCOREBOARDNAME], new_[MAX_SCOREBOARDNAME]; double at; } tcl_pending_t;
#define TCL_PENDING 8
static tcl_pending_t tcl_pend[TCL_PENDING];

static void TCL_RenamePoll(void)
{
	int i;
	if (cls.state != ca_connected || !cl.scores)
	{
		tcl_was_connected = 0;
		memset(tcl_seen, 0, sizeof(tcl_seen));
		memset(tcl_pend, 0, sizeof(tcl_pend));
		return;
	}
	if (!tcl_was_connected)
	{
		tcl_was_connected = 1;
		tcl_conn_at = host.realtime;
		memset(tcl_seen, 0, sizeof(tcl_seen));
	}
	for (i = 0; i < cl.maxclients && i < MAX_SCOREBOARD; i++)
	{
		const char *now = cl.scores[i].name;
		if (!now[0])
		{
			tcl_seen[i][0] = 0; /* the slot emptied: whoever takes it next is not a rename */
			continue;
		}
		/* a slot that changed name within a moment of someone joining or leaving was reused */
		if (tcl_seen[i][0] && strcmp(tcl_seen[i], now) && host.realtime - tcl_conn_at > 3 && host.realtime - tcl_event_at > 2)
		{
			int k;
			for (k = 0; k < TCL_PENDING && tcl_pend[k].at; k++)
				;
			if (k < TCL_PENDING)
			{
				dp_strlcpy(tcl_pend[k].old_, tcl_seen[i], sizeof(tcl_pend[k].old_));
				dp_strlcpy(tcl_pend[k].new_, now, sizeof(tcl_pend[k].new_));
				tcl_pend[k].at = host.realtime;
			}
		}
		dp_strlcpy(tcl_seen[i], now, sizeof(tcl_seen[i]));
	}
	for (i = 0; i < TCL_PENDING; i++)
		if (tcl_pend[i].at && host.realtime - tcl_pend[i].at > 1.5)
		{
			char plain[TCL_NAME];
			TouchMention_StripColors(tcl_pend[i].new_, plain, sizeof(plain));
			if (!TouchChatLog_HasRecent(&cl_log, host.realtime, TCL_RENAME, plain, 3.0))
			{
				if (TouchChatLog_AddRename(&cl_log, host.realtime, time(NULL), tcl_pend[i].old_, tcl_pend[i].new_))
				{
					TCL_Added();
					TCL_EventSound();
				}
			}
			tcl_pend[i].at = 0;
		}
}

/* ------------------------------------------------------------------ */
/* metrics and drawing helpers                                          */
/* ------------------------------------------------------------------ */
typedef struct
{
	float fs;     /* text size */
	float rowh;   /* one row of text */
	float pad;    /* between two lines */
	float dp;     /* console units per dp */
	float sx, sy; /* device pixels per console unit */
} tcl_metrics_t;

static void TCL_Metrics(float size, tcl_metrics_t *m)
{
	m->dp = TouchUI_ConPerDp();
	if (m->dp <= 0)
		m->dp = 1;
	m->sx = vid_conwidth.value > 0 ? (float)vid.mode.width / vid_conwidth.value : 1;
	m->sy = vid_conheight.value > 0 ? (float)vid.mode.height / vid_conheight.value : 1;
	/* legible: at least 12 dp, never more than twice the HUD's chat size */
	m->fs = max(size, 12.0f * m->dp);
	if (size > 0 && m->fs > size * 2.2f)
		m->fs = size * 2.2f;
	m->rowh = m->fs * 1.12f;
	m->pad = m->fs * 0.22f;
	tcl_min_target = 44.0f * m->dp;
	tcl_slop = 8.0f * m->dp;
}

static float TCL_SnapX(const tcl_metrics_t *m, float v) { return floorf(v * m->sx + 0.5f) / m->sx; }
static float TCL_SnapY(const tcl_metrics_t *m, float v) { return floorf(v * m->sy + 0.5f) / m->sy; }

/* The icons: their coverage at the exact device pixel size (TouchChatLog_IconPixels),
 * drawn as runs of device pixels (a row of equal coverage is one fill), snapped
 * to the pixel grid: nothing is ever scaled, so edges stay clean at any density. */
static struct { int px; unsigned char *rgba; } tcl_pics[TCL_I_COUNT];

static void TCL_Icon(const tcl_metrics_t *m, int icon, float x, float y, float size, float r, float g, float b, float a)
{
	int px = (int)(size * m->sx + 0.5f), ix, iy;
	float x0, y0;
	const unsigned char *c;
	if (px < 8)
		px = 8;
	if (px > 192)
		px = 192;
	if (tcl_pics[icon].px != px || !tcl_pics[icon].rgba)
	{
		if (!tcl_pics[icon].rgba)
			tcl_pics[icon].rgba = (unsigned char *)Mem_Alloc(cls.permanentmempool, 192 * 192 * 4);
		TouchChatLog_IconPixels(icon, px, tcl_pics[icon].rgba);
		tcl_pics[icon].px = px;
	}
	c = tcl_pics[icon].rgba;
	x0 = floorf(x * m->sx + 0.5f);
	y0 = floorf(y * m->sy + 0.5f);
	for (iy = 0; iy < px; iy++)
		for (ix = 0; ix < px;)
		{
			int al = c[((size_t)iy * (size_t)px + (size_t)ix) * 4 + 3], run = 1;
			if (al < 8)
			{
				ix++;
				continue;
			}
			while (ix + run < px && c[((size_t)iy * (size_t)px + (size_t)(ix + run)) * 4 + 3] == al)
				run++;
			DrawQ_Fill((x0 + (float)ix) / m->sx, (y0 + (float)iy) / m->sy, (float)run / m->sx, 1.0f / m->sy, r, g, b, a * (float)al / 255.0f, 0);
			ix += run;
		}
}

static void TCL_KindRGB(int kind, float *r, float *g, float *b)
{
	static const float c[TCL_NKINDS][3] = {
		{0.85f, 0.87f, 0.92f}, {0.47f, 0.85f, 0.47f}, {0.47f, 0.73f, 1.0f}, {0.73f, 0.73f, 0.73f},
		{1.0f, 0.53f, 0.53f}, {1.0f, 0.84f, 0.4f}, {0.73f, 0.73f, 0.73f}};
	if (kind < 0 || kind >= TCL_NKINDS)
		kind = TCL_NOTE;
	*r = c[kind][0];
	*g = c[kind][1];
	*b = c[kind][2];
}

/* wrapping and drawing a line of coloured text in rows */
typedef struct
{
	const dp_font_t *font;
	float fs, rowh;
	int rows, colorindex;
	float maxw;     /* the widest row */
	float x, y;     /* where to draw (draw mode) */
	float alpha;
	int draw;
	const tcl_metrics_t *m;
} tcl_wrap_t;

static float TCL_WordWidth(void *pt, const char *w, size_t *length, float maxwidth)
{
	tcl_wrap_t *t = (tcl_wrap_t *)pt;
	if (w == NULL)
	{
		t->colorindex = -1;
		return t->fs * t->font->maxwidth;
	}
	if (maxwidth >= 0)
		return DrawQ_TextWidth_UntilWidth(w, length, t->fs, t->fs, false, t->font, -maxwidth);
	if (maxwidth == -1)
		return DrawQ_TextWidth(w, *length, t->fs, t->fs, false, t->font);
	return 0;
}

static int TCL_Line(void *pt, const char *line, size_t length, float width, qbool isContinuation)
{
	tcl_wrap_t *t = (tcl_wrap_t *)pt;
	(void)isContinuation;
	if (width > t->maxw)
		t->maxw = width;
	if (t->draw && length > 0)
	{
		char plain[TCL_LEN * 2], slice[TCL_LEN * 2];
		float y = t->y + (float)t->rows * t->rowh;
		size_t n = min(length, sizeof(slice) - 1);
		memcpy(slice, line, n);
		slice[n] = 0;
		TouchMention_StripColors(slice, plain, sizeof(plain));
		/* a shadow one device pixel down and right, so any colour reads on any scene */
		DrawQ_String(t->x + 1.0f / t->m->sx, y + 1.0f / t->m->sy, plain, 0, t->fs, t->fs, 0, 0, 0, t->alpha * 0.85f, 0, NULL, true, t->font);
		DrawQ_String(t->x, y, line, length, t->fs, t->fs, 1, 1, 1, t->alpha, 0, &t->colorindex, false, t->font);
	}
	t->rows++;
	return 1;
}

static int TCL_Wrap(const tcl_metrics_t *m, const char *text, float width, int draw, float x, float y, float alpha, float *maxw)
{
	tcl_wrap_t t;
	memset(&t, 0, sizeof(t));
	t.font = FONT_CHAT;
	t.fs = m->fs;
	t.rowh = m->rowh;
	t.colorindex = -1;
	t.x = x;
	t.y = y;
	t.alpha = alpha;
	t.draw = draw;
	t.m = m;
	if (width < m->fs * 4)
		width = m->fs * 4;
	COM_Wordwrap(text, strlen(text), 0, width, TCL_WordWidth, &t, TCL_Line, &t);
	if (maxw)
		*maxw = t.maxw;
	return t.rows > 0 ? t.rows : 1;
}

/* the virtual list's measure callback: rows are kept per line, for this width and size */
static float tcl_measure_w;
static const tcl_metrics_t *tcl_measure_m;
static float tcl_cache_w = -1, tcl_cache_fs = -1;
static int tcl_cache_key;
static int TCL_Measure(void *ctx, tcl_entry_t *e)
{
	int key = tcl_cache_key;
	(void)ctx;
	if (e->rows > 0 && e->rowsw == key)
		return e->rows;
	e->rowsw = key;
	return TCL_Wrap(tcl_measure_m, e->text, tcl_measure_w, 0, 0, 0, 0, NULL);
}

static void TCL_AddTarget(float x, float y, float w, float h, int id, int arg)
{
	if (tcl_ntg >= TG_MAX)
		return;
	tcl_tg[tcl_ntg].x = x;
	tcl_tg[tcl_ntg].y = y;
	tcl_tg[tcl_ntg].w = w;
	tcl_tg[tcl_ntg].h = h;
	tcl_tg[tcl_ntg].id = id;
	tcl_tg[tcl_ntg].arg = arg;
	tcl_ntg++;
}

/* ------------------------------------------------------------------ */
/* actions                                                              */
/* ------------------------------------------------------------------ */
static void TCL_Share_f(cmd_state_t *cmd);
static void TCL_Download_f(cmd_state_t *cmd);
static void TCL_Copy_f(cmd_state_t *cmd);
static void TCL_Clear_f(cmd_state_t *cmd);

static void TCL_Open(int open)
{
	if (open == tcl_panel)
		return;
	tcl_panel = open;
	if (!open)
	{
		TCL_ClearSelection();
		if (tcl_return_chat && cls.state == ca_connected)
			Cbuf_AddText(cmd_local, "\ntouch_chat\n");
		tcl_return_chat = 0;
	}
	tcl_anchor_valid = 0;
	if (open)
		tcl_filter = 0;
	tcl_popup = 0;
	tcl_ptr_down = 0;
	memset(&tcl_sc, 0, sizeof(tcl_sc));
	if (open && !vid_touchscreen.integer && key_dest == key_game)
	{
		/* a mouse needs the pointer, and typing a reply is natural: the chat input */
		key_dest = key_message;
		chat_mode = 0;
	}
}

static void TCL_Mention(const char *name)
{
	if (!name[0])
		return;
	tcl_return_chat = 0; /* mention opens its own input */
	TCL_Open(0);
	key_dest = key_message;
	chat_mode = 0;
	chat_bufferpos = dpsnprintf(chat_buffer, sizeof(chat_buffer), "@%s ", name);
	if (chat_bufferpos < 0)
		chat_bufferpos = 0;
}

static void TCL_CopyLine(unsigned idx)
{
	const tcl_entry_t *e = TouchChatLog_Get(&cl_log, idx);
	char plain[TCL_LEN], stamp[16], line[TCL_LEN + 16];
	if (!e)
		return;
	TouchMention_StripColors(e->text, plain, sizeof(plain));
	TouchChatLog_Stamp(e->wall, stamp, sizeof(stamp));
	dpsnprintf(line, sizeof(line), "[%s] %s", stamp, plain);
	TCL_Toast(TouchClip_Set(line) ? "Copied" : "Cannot copy here");
}

static void TCL_Activate(int id, int arg)
{
	switch (id)
	{
	case TG_LOG: TCL_Open(1); break;
	case TG_CLOSE: TCL_Open(0); break;
	case TG_CLEAR: TCL_Clear_f(NULL); break;
	case TG_COPY: TCL_Copy_f(NULL); break;
	case TG_DOWNLOAD: TCL_Download_f(NULL); break;
	case TG_SHARE: TCL_Share_f(NULL); break;
	case TG_CHIP:
		tcl_filter = arg;
		memset(&tcl_sc, 0, sizeof(tcl_sc));
		break;
	case TG_LATEST:
		tcl_sc.pos = 0;
		tcl_sc.vel = 0;
		break;
	case TG_ROW:
		TCL_Open(1); /* a tap on the feed opens the log */
		break;
	case TG_NAME:
	{
		const tcl_entry_t *e = TouchChatLog_Get(&cl_log, (unsigned)arg);
		if (e && e->name[0])
		{
			dp_strlcpy(tcl_popup_name, e->name, sizeof(tcl_popup_name));
			tcl_popup_entry = (unsigned)arg;
			tcl_popup = 1;
			tcl_popup_x = tcl_pointer_x;
			tcl_popup_y = tcl_pointer_y;
			if (!tcl_panel)
				TCL_Open(1); /* the actions sit over the log */
			tcl_popup = 1;
		}
		break;
	}
	case TG_POP:
		if (arg == 0) TCL_Mention(tcl_popup_name);
		else if (arg == 1) TCL_Toast(TouchClip_Set(tcl_popup_name) ? "Copied" : "Cannot copy here");
		else TCL_CopyLine(tcl_popup_entry);
		tcl_popup = 0;
		break;
	}
}

static void TCL_Clear_f(cmd_state_t *cmd)
{
	(void)cmd;
	TouchChatLog_Clear(&cl_log);
	TCL_ClearSelection();
	tcl_anchor_valid = 0;
	memset(&tcl_sc, 0, sizeof(tcl_sc));
	tcl_cleared_at = cl.time;
	tcl_popup = 0;
	TCL_Toast("Log cleared");
	Con_Printf("chat log cleared\n");
}

/* mode bits: 1 share sheet (Android), 2 download. The Android app tells them
 * apart by the file name: touch_chatlog_share.txt, touch_chatlog_download.txt,
 * or touch_chatlog.txt for both (touch_chatlog_export). */
static size_t TCL_Export(char *out, size_t size)
{
	unsigned i, selected = TCL_SelectedCount();
	unsigned mask = tcl_filter == 1 ? TCL_BIT(TCL_CHAT) : tcl_filter == 2 ? (TCL_EVENTS | TCL_BIT(TCL_NOTE)) : TCL_ALL;
	size_t used = 0;
	if (!size)
		return 0;
	out[0] = 0;
	for (i = 0; i < cl_log.count; i++)
	{
		const tcl_entry_t *e = TouchChatLog_Get(&cl_log, i);
		char plain[TCL_LEN], stamp[16], line[TCL_LEN + 16];
		int n;
		if (selected ? !TCL_Selected(i) : !(mask & TCL_BIT(e->kind)))
			continue;
		TouchMention_StripColors(e->text, plain, sizeof(plain));
		TouchChatLog_Stamp(e->wall, stamp, sizeof(stamp));
		n = dpsnprintf(line, sizeof(line), "[%s] %s\n", stamp, plain);
		if (n < 0 || (size_t)n >= sizeof(line) || used + (size_t)n + 1 > size)
			break;
		memcpy(out + used, line, (size_t)n + 1);
		used += (size_t)n;
	}
	return used;
}

static void TCL_Write(int mode)
{
	static char text[TCL_CAP * (TCL_LEN + 16)];
	size_t n = TCL_Export(text, sizeof(text));
	if (!n)
	{
		TCL_Toast("Nothing to save");
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
		TCL_Toast(mode == 1 ? "Opening share sheet" : "Saved to Downloads");
		Con_Printf("chat log: %d bytes written; the app %s\n", (int)n, mode == 1 ? "offers it to the share sheet" : mode == 2 ? "saves it to Downloads" : "offers it to the share sheet and Downloads");
	}
#else
	if (!(mode & 2))
	{
		TCL_Toast("Share is a phone feature");
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
		TCL_Toast("Saved to Downloads");
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
	size_t n = TCL_Export(text, sizeof(text));
	(void)cmd;
	if (!n)
	{
		TCL_Toast("Nothing to copy");
		Con_Printf("chat log: nothing to copy\n");
		return;
	}
	if (!TouchClip_Set(text))
	{
		TCL_Toast("Cannot copy here");
		Con_Printf("chat log: this build cannot write the system clipboard\n");
		return;
	}
	TCL_Toast("Copied");
	Con_Printf("chat log copied (%d bytes)\n", (int)n);
}

/* ------------------------------------------------------------------ */
/* the pointer: one model for a finger, a mouse click and a test        */
/* ------------------------------------------------------------------ */
static int TCL_HitAt(float x, float y)
{
	int i, row = -1, name = -1;
	if (tcl_panel && !tcl_popup)
	{
		for (i = 0; i < tcl_ntg; i++)
			if ((tcl_tg[i].id == TG_ROW || tcl_tg[i].id == TG_NAME) &&
			    x >= tcl_tg[i].x && x <= tcl_tg[i].x + tcl_tg[i].w &&
			    y >= tcl_tg[i].y && y <= tcl_tg[i].y + tcl_tg[i].h)
			{
				if (tcl_tg[i].id == TG_ROW) row = i;
				else name = i;
			}
		if (row >= 0)
		{
			for (i = 0; i < tcl_ntg; i++)
				if (tcl_tg[i].id == TG_LATEST && x >= tcl_tg[i].x && x <= tcl_tg[i].x + tcl_tg[i].w && y >= tcl_tg[i].y && y <= tcl_tg[i].y + tcl_tg[i].h)
					return i;
			return !TCL_SelectedCount() && name >= 0 && tcl_tg[name].arg == tcl_tg[row].arg ? name : row;
		}
	}
	return TouchChatLog_HitTarget(tcl_tg, tcl_ntg, x, y, tcl_min_target, tcl_min_target);
}

static int TCL_Scrolls(int id)
{
	return tcl_panel && (id == TG_BODY || id == TG_NAME || id == TG_ROW);
}

/* phase 0 down, 1 move, 2 up; x, y in console units */
static void TCL_Pointer(int phase, float x, float y)
{
	double now = host.realtime;
	int k;
	if (phase == 0)
	{
		tcl_ptr_down = 1;
		tcl_ptr_at = now;
		tcl_ptr_held = 0;
		tcl_ptr_serial = 0;
		tcl_pointer_x = x;
		tcl_pointer_y = y;
		k = TCL_HitAt(x, y);
		if (tcl_popup)
		{
			/* the actions are modal: a press elsewhere closes them */
			if (k >= 0 && tcl_tg[k].id == TG_POP)
			{
				tcl_ptr_id = TG_POP;
				tcl_ptr_arg = tcl_tg[k].arg;
			}
			else
			{
				tcl_popup = 0;
				tcl_ptr_down = 0;
			}
			return;
		}
		if (k < 0)
		{
			if (tcl_panel) TCL_ClearSelection();
			/* a press outside the panel closes it; inside it, on nothing, does nothing */
			if (tcl_panel && !(x >= tcl_panel_rect[0] && x <= tcl_panel_rect[0] + tcl_panel_rect[2] && y >= tcl_panel_rect[1] && y <= tcl_panel_rect[1] + tcl_panel_rect[3]))
				TCL_Open(0);
			tcl_ptr_down = 0;
			return;
		}
		tcl_ptr_id = tcl_tg[k].id;
		tcl_ptr_arg = tcl_tg[k].arg;
		if (tcl_panel && (tcl_ptr_id == TG_ROW || tcl_ptr_id == TG_NAME))
			tcl_ptr_serial = tcl_slot_serial[TCL_Slot((unsigned)tcl_ptr_arg)];
		if (TCL_Scrolls(tcl_ptr_id))
			TouchChatLog_ScrollDown(&tcl_sc, y, now);
		return;
	}
	if (!tcl_ptr_down)
		return;
	if (phase == 1)
	{
		if (TCL_Scrolls(tcl_ptr_id))
			{
			if (TouchChatLog_ScrollMove(&tcl_sc, y, now, max(0.0f, tcl_content_h - tcl_body_h), tcl_slop))
				tcl_scroll_at = now;
		}
		return;
	}
	/* up */
	tcl_ptr_down = 0;
	k = TCL_HitAt(x, y);
	if (TCL_Scrolls(tcl_ptr_id))
	{
		if (!TouchChatLog_ScrollUp(&tcl_sc, now) || tcl_ptr_held)
			return; /* a drag: it coasts */
		if (tcl_panel && (tcl_ptr_id == TG_NAME || tcl_ptr_id == TG_ROW) &&
		    k >= 0 && (tcl_tg[k].id == TG_NAME || tcl_tg[k].id == TG_ROW) && tcl_tg[k].arg == tcl_ptr_arg &&
		    TCL_SelectedCount())
		{
			unsigned slot = TCL_Slot((unsigned)tcl_ptr_arg);
			tcl_selected[slot] = TCL_Selected((unsigned)tcl_ptr_arg) ? 0 : tcl_slot_serial[slot];
			return;
		}
		if (tcl_ptr_id == TG_BODY || tcl_ptr_id == TG_ROW)
		{
			if (tcl_panel && tcl_ptr_id == TG_BODY)
				TCL_ClearSelection();
			return;
		}
	}
	if (tcl_panel && (tcl_ptr_id == TG_PANEL || tcl_ptr_id == TG_NONE))
		TCL_ClearSelection();
	if (k >= 0 && tcl_tg[k].id == tcl_ptr_id && tcl_tg[k].arg == tcl_ptr_arg)
		TCL_Activate(tcl_ptr_id, tcl_ptr_arg);
}

static void TCL_Ptr_f(cmd_state_t *cmd)
{
	const char *p = Cmd_Argv(cmd, 1);
	if (Cmd_Argc(cmd) < 4)
	{
		Con_Printf("usage: touch_chatlog_ptr down|move|up <fx> <fy> (window fractions)\n");
		return;
	}
	TCL_Pointer(!strcmp(p, "down") ? 0 : !strcmp(p, "move") ? 1 : 2,
		(float)atof(Cmd_Argv(cmd, 2)) * vid_conwidth.value, (float)atof(Cmd_Argv(cmd, 3)) * vid_conheight.value);
}

static void TCL_PanelScroll(float notches)
{
	float maxpos = max(0.0f, tcl_content_h - tcl_body_h);
	float step = (tcl_row_h > 0 ? tcl_row_h : 12) * 3.0f;
	if (!tcl_panel)
		TCL_Open(1);
	TouchChatLog_ScrollWheel(&tcl_sc, notches, step, maxpos);
	tcl_scroll_at = host.realtime;
}

static float TCL_PageNotches(void)
{
	return max(1.0f, tcl_body_h / max(1.0f, tcl_row_h * 3.0f));
}

static void TCL_Scroll_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) >= 2)
	{
		float rows = (float)atoi(Cmd_Argv(cmd, 1));
		if (!tcl_panel)
			TCL_Open(1);
		tcl_sc.pos = tcl_clampf(tcl_sc.pos + rows * (tcl_row_h > 0 ? tcl_row_h : 12), 0, max(0.0f, tcl_content_h - tcl_body_h));
		tcl_sc.vel = 0;
		tcl_scroll_at = host.realtime;
	}
}

/* keys.c, Key_Event in the game: while the log is open the wheel, page keys and Escape belong to it */
qbool TouchChatLog_GameKey(int key, qbool down)
{
	if (!tcl_panel || !touch_chatlog.integer)
		return false;
	switch (key)
	{
	case K_ESCAPE: if (down) TCL_Open(0); return true;
	case K_MWHEELUP: if (down) TCL_PanelScroll(1); return true;
	case K_MWHEELDOWN: if (down) TCL_PanelScroll(-1); return true;
	case K_PGUP: if (down) TCL_PanelScroll(TCL_PageNotches()); return true;
	case K_PGDN: if (down) TCL_PanelScroll(-TCL_PageNotches()); return true;
	}
	return false;
}

/* The chat input is open (a desktop's T key): the mouse clicks the log's icons and chips, the wheel scrolls it. */
qbool TouchChatLog_Key(int key, qbool down)
{
	if (!touch_chatlog.integer || !down || key_dest != key_message)
		return false;
	if (key == K_MOUSE1 && vid.mode.width > 0 && tcl_ntg > 0)
	{
		float mx = in_windowmouse_x * vid_conwidth.value / vid.mode.width;
		float my = in_windowmouse_y * vid_conheight.value / vid.mode.height;
		int k = TCL_HitAt(mx, my);
		if (k < 0 || tcl_tg[k].id == TG_BODY)
		{
			if (tcl_panel) TCL_ClearSelection();
			return false;
		}
		/* a click is a press and a release in one place */
		TCL_Pointer(0, mx, my);
		TCL_Pointer(2, mx, my);
		return true;
	}
	switch (key)
	{
	case K_ESCAPE:
		if (!tcl_panel)
			return false;
		TCL_Open(0);
		return true;
	case K_MWHEELUP: TCL_PanelScroll(1); return true;
	case K_MWHEELDOWN: if (tcl_panel) TCL_PanelScroll(-1); return tcl_panel;
	case K_PGUP: TCL_PanelScroll(TCL_PageNotches()); return true;
	case K_PGDN: if (tcl_panel) TCL_PanelScroll(-TCL_PageNotches()); return tcl_panel;
	}
	return false;
}

/* vid_sdl.c, every finger (TouchVote_Finger): the feed's icons and names and the open log take a finger
 * before the game's touch layer sees it, so they work under any server's CSQC (the stock one too).
 * A finger on nothing of the feed is not taken: it looks around as before. True when taken. */
qbool TouchChatLog_Finger(int type, int id, float nx, float ny)
{
	static int held;
	float x = nx * vid_conwidth.value, y = ny * vid_conheight.value;
	cvar_t *sheet;
	if (type == 0)
	{
		if (held || !touch_chatlog.integer || cls.state != ca_connected || host.realtime - tcl_claim_at > 1.0)
			return false;
		sheet = Cvar_FindVar(&cvars_all, "_touch_chat_sheet", CF_CLIENT);
		if ((sheet && sheet->integer) || (key_dest != key_game && !tcl_panel))
			return false;
		if (!tcl_panel && (x < tcl_claim_r[0] || x > tcl_claim_r[0] + tcl_claim_r[2] || y < tcl_claim_r[1] || y > tcl_claim_r[1] + tcl_claim_r[3] || TCL_HitAt(x, y) < 0))
			return false;
		held = id;
		TCL_Pointer(0, x, y);
		return true;
	}
	if (!held || held != id)
		return false;
	TCL_Pointer(type == 1 ? 1 : 2, x, y);
	if (type == 2)
		held = 0;
	return true;
}

/* console.c: the panel draws the chat input itself */
qbool TouchChatLog_PanelHasInput(void)
{
	return tcl_panel && touch_chatlog.integer && key_dest == key_message;
}

/* ------------------------------------------------------------------ */
/* drawing                                                              */
/* ------------------------------------------------------------------ */
static void TCL_Claim(float x, float y, float w, float h)
{
	char s[96];
	tcl_claim_r[0] = x; tcl_claim_r[1] = y; tcl_claim_r[2] = w; tcl_claim_r[3] = h;
	tcl_claim_at = host.realtime;
	if (w <= 0 || h <= 0 || vid_conwidth.value <= 0)
		s[0] = 0;
	else
		dpsnprintf(s, sizeof(s), "%.4f %.4f %.4f %.4f", x / vid_conwidth.value, y / vid_conheight.value, w / vid_conwidth.value, h / vid_conheight.value);
	if (strcmp(touch_chatlog_claim.string, s))
		Cvar_SetQuick(&touch_chatlog_claim, s);
}

static float TCL_TextW(const char *s, float fs)
{
	return DrawQ_TextWidth(s, 0, fs, fs, false, FONT_CHAT);
}

/* a labelled text button (a filter chip) with a drawn size and its 44 dp target */
static float TCL_Chip(const tcl_metrics_t *m, float x, float y, float h, const char *label, int on, int arg)
{
	float fs = min(m->fs, 12.0f * m->dp) * 0.6f;
	float w = max(tcl_min_target, TCL_TextW(label, fs) + 16.0f * m->dp);
	float drawnw = max(TCL_TextW(label, fs) + 10.0f * m->dp, w * 0.6f);
	DrawQ_RoundedBox(x + w * 0.5f, y + h * 0.5f, drawnw * 0.5f, h * 0.5f, h * 0.5f, 0, 0.18f, 0.25f, 0.32f, on ? 0.48f : 0.22f, 0);
	DrawQ_RoundedBox(x + w * 0.5f, y + h * 0.5f, drawnw * 0.5f, h * 0.5f, h * 0.5f, m->dp, 0.75f, 0.85f, 1, on ? 0.5f : 0.25f, 0);
	DrawQ_String(x + (w - TCL_TextW(label, fs)) * 0.5f, y + (h - fs) * 0.5f, label, 0, fs, fs, 0.9f, 0.94f, 1, 0.9f, 0, NULL, true, FONT_CHAT);
	TCL_AddTarget(x, y + (h - tcl_min_target) * 0.5f, w, tcl_min_target, TG_CHIP, arg);
	return w;
}

/* an icon button: the picture at its drawn size, centred in its 44 dp target */
static void TCL_IconButton(const tcl_metrics_t *m, int icon, float cx, float cy, float size, int id, float alpha)
{
	float r = 9.0f * m->dp;
	DrawQ_RoundedBox(cx, cy, r, r, r, 0, 0.25f, 0.32f, 0.4f, 0.2f * alpha, 0);
	DrawQ_RoundedBox(cx, cy, r, r, r, m->dp, 0.8f, 0.9f, 1, 0.28f * alpha, 0);
	TCL_Icon(m, icon, cx - size * 0.5f, cy - size * 0.5f, size, 0.85f, 0.91f, 1, 0.75f * alpha);
	TCL_AddTarget(cx - tcl_min_target * 0.5f, cy - tcl_min_target * 0.5f, tcl_min_target, tcl_min_target, id, 0);
}

static void TCL_DrawPopup(const tcl_metrics_t *m)
{
	static const char *const label[3] = {"Mention", "Copy name", "Copy line"};
	float W = vid_conwidth.value, H = vid_conheight.value;
	float itemh = tcl_min_target, w = 0, x, y;
	char title[TCL_NAME + 8];
	int i;
	for (i = 0; i < 3; i++)
		w = max(w, TCL_TextW(label[i], m->fs));
	w = max(w + m->fs * 3.0f, 160.0f * m->dp);
	dpsnprintf(title, sizeof(title), "@%s", tcl_popup_name);
	w = max(w, min(TCL_TextW(title, m->fs) + m->fs * 3.0f, W * 0.7f));
	x = bound(m->fs, tcl_popup_x - w * 0.5f, max(m->fs, W - w - m->fs));
	y = bound(m->fs, tcl_popup_y - itemh * 0.5f, max(m->fs, H - itemh * 4 - m->fs));
	DrawQ_Fill(0, 0, W, H, 0, 0, 0, 0.35f, 0);
	TouchHUD_GlassPanel(x, y, w, itemh * 4, m->dp * 14, 1);
	TCL_Icon(m, TCL_I_CHAT, x + m->fs * 0.6f, y + (itemh - m->fs * 1.2f) * 0.5f, m->fs * 1.2f, 0.7f, 0.8f, 1, 1);
	DrawQ_String(x + m->fs * 2.2f, y + (itemh - m->fs) * 0.5f, title, 0, m->fs, m->fs, 1, 1, 1, 1, 0, NULL, true, FONT_CHAT);
	for (i = 0; i < 3; i++)
	{
		float iy = y + itemh * (float)(i + 1);
		TouchHUD_GlassControl(x + w * 0.5f, iy + itemh * 0.5f, w * 0.46f, itemh * 0.36f, tcl_ptr_down && tcl_ptr_id == TG_POP && tcl_ptr_arg == i, false, 1);
		DrawQ_String(x + m->fs * 1.2f, iy + (itemh - m->fs) * 0.5f, label[i], 0, m->fs, m->fs, 1, 1, 1, 1, 0, NULL, true, FONT_CHAT);
		TCL_AddTarget(x, iy, w, itemh, TG_POP, i);
	}
}

static void TCL_DrawToast(const tcl_metrics_t *m, float cx, float y)
{
	float age = (float)(host.realtime - tcl_toast_at), a, tw;
	if (age > 1.8f || !tcl_toast[0])
		return;
	a = age > 1.2f ? (1.8f - age) / 0.6f : 1.0f;
	tw = TCL_TextW(tcl_toast, m->fs);
	TouchHUD_GlassPanel(cx - tw * 0.5f - m->fs, y, tw + m->fs * 2, m->fs * 1.9f, m->fs * 0.8f, a);
	DrawQ_String(cx - tw * 0.5f, y + m->fs * 0.45f, tcl_toast, 0, m->fs, m->fs, 1, 1, 1, a, 0, NULL, true, FONT_CHAT);
}

/* One log line: time, icon, text. y is its top. */
static void TCL_DrawLogRow(const tcl_metrics_t *m, unsigned idx, float x, float y, float h, float tw, float iconw, float textx, float textw, float bodyx, float bodyw)
{
	const tcl_entry_t *e = TouchChatLog_Get(&cl_log, idx);
	char stamp[16];
	float r, g, b, maxw = 0, small = m->fs * 0.8f;
	if (!e)
		return;
	TCL_KindRGB(e->kind, &r, &g, &b);
	if (TCL_Selected(idx))
		DrawQ_Fill(bodyx, y, bodyw, h, 0.2f, 0.55f, 1.0f, 0.38f, 0);
	TCL_AddTarget(bodyx, y, bodyw, h, TG_ROW, (int)idx);
	TouchChatLog_Stamp(e->wall, stamp, sizeof(stamp));
	if (e->kind != TCL_CHAT)
		DrawQ_Fill(bodyx, y, bodyw, h, r * 0.2f, g * 0.2f, b * 0.2f, 0.28f, 0);
	DrawQ_String(x, y + m->pad * 0.5f + (m->rowh - small) * 0.5f, stamp, 0, small, small, 0.6f, 0.65f, 0.72f, 1, 0, NULL, true, FONT_CHAT);
	TCL_Icon(m, TouchChatLog_KindIcon(e->kind), x + tw, y + m->pad * 0.5f + (m->rowh - m->fs * 1.05f) * 0.5f, m->fs * 1.05f, r, g, b, 1);
	TCL_Wrap(m, e->text, textw, 1, textx, y + m->pad * 0.5f, 1, &maxw);
	/* a tap on the icon or the name: actions for that player */
	if (e->name[0])
		TCL_AddTarget(x + tw, y, iconw + min(maxw, m->fs * 12), min(h, m->rowh + m->pad), TG_NAME, (int)idx);
}

static void TCL_DrawPanel(const tcl_metrics_t *m, float dt)
{
	float W = vid_conwidth.value, H = vid_conheight.value;
	float inl = 0, int_ = 0, inr = 0, inb = 0, dens = 0;
	float pxl, pyt, pw, ph, hdr, footer = 0, rowtop, bodyh, x;
	float tw, iconw, textx, textw, title_h, maxpos, iconsz = 12.0f * m->dp, edge;
	int wide = W >= 560.0f * m->dp, i;
	tcl_window_t win;
	unsigned mask = tcl_filter == 1 ? TCL_BIT(TCL_CHAT) : tcl_filter == 2 ? (TCL_EVENTS | TCL_BIT(TCL_NOTE)) : TCL_ALL;
	char s[64];
	const char *ins = Cvar_VariableString(&cvars_all, "_touch_safe_insets", ~0);

	if (ins && ins[0] && sscanf(ins, "%f %f %f %f %f", &inl, &int_, &inr, &inb, &dens) >= 4)
	{
		inl /= m->sx; inr /= m->sx; int_ /= m->sy; inb /= m->sy;
		inl = min(inl, W * 0.08f); inr = min(inr, W * 0.08f); int_ = min(int_, H * 0.08f); inb = min(inb, H * 0.08f);
	}
	edge = max(6.0f * m->dp, min(W, H) * 0.02f);
	pxl = inl + edge;
	pyt = int_ + edge;
	if (key_dest == key_message)
		pyt = max(pyt, TouchUI_SheetHeaderHeight(H) + 4.0f * m->dp); /* the typing sheet's KEYBOARD tab sits in the top right */
	if (vid_touchscreen.integer)
		pyt = max(pyt, H * 0.21f); /* the touch HUD's top row (info, console, MENU, SCORE, CHAT) stays reachable: the panel starts below it */
	pw = W - pxl - inr - edge;
	ph = H - pyt - inb - edge;
	if (TouchChatLog_PanelHasInput())
		footer = m->fs * 2.4f;
	title_h = tcl_min_target;
	hdr = (wide ? title_h : title_h * 2.0f) + 8.0f * m->dp;
	{
		/* the console button (touch_shellbtn.c) sits in the top right corner over everything: keep the panel off it */
		float rx, ry, rw, rh;
		if (sscanf(Cvar_VariableString(&cvars_all, "_touch_shell_rect", ~0), "%f %f %f %f", &rx, &ry, &rw, &rh) == 4 && rw > 0)
		{
			rx *= W; ry *= H; rw *= W; rh *= H;
			if (ry < pyt + hdr && ry + rh > pyt && rx + rw > pxl + pw * 0.5f && rx < pxl + pw)
				pw = max(pw * 0.5f, rx - edge - pxl);
		}
	}
	/* a short log makes a short panel: the lines' height (as measured so far), a few rows at least,
	 * up to what the screen has; it hangs from the top so nothing below it is covered for nothing */
	{
		float want = TouchChatLog_Content(&cl_log, mask, m->rowh, m->pad), maxbody = ph - hdr - footer;
		want = max(want + m->rowh * 0.5f, m->rowh * 5);
		if (want < maxbody)
			ph = hdr + footer + want;
	}
	rowtop = pyt + hdr;
	bodyh = ph - hdr - footer - 6.0f * m->dp;
	if (bodyh < m->rowh * 2)
		return;

	/* the panel takes every press inside it (a press outside closes it) */
	tcl_panel_rect[0] = pxl; tcl_panel_rect[1] = pyt; tcl_panel_rect[2] = pw; tcl_panel_rect[3] = ph;

	{
		float r = 14.0f * m->dp, cx = pxl + pw * 0.5f, cy = pyt + ph * 0.5f;
		DrawQ_RoundedBox(cx, cy + 3 * m->dp, pw * 0.5f + 3 * m->dp, ph * 0.5f + 3 * m->dp, r, 0, 0, 0, 0, 0.16f, 0);
		DrawQ_RoundedBox(cx, cy, pw * 0.5f, ph * 0.5f, r, 0, 0.025f, 0.04f, 0.065f, 0.74f, 0);
		DrawQ_RoundedBox(cx, cy, pw * 0.5f, ph * 0.5f, r, m->dp, 0.8f, 0.9f, 1, 0.32f, 0);
	}

	/* header: filter chips, count, actions (no title: the chips and the lines say what it is) */
	{
		float cx = pxl + pw - tcl_min_target * 0.5f, cy = pyt + title_h * 0.5f;
		static const int ic[5] = {TCL_I_CLOSE, TCL_I_SHARE, TCL_I_DOWNLOAD, TCL_I_COPY, TCL_I_CLEAR};
		static const int id[5] = {TG_CLOSE, TG_SHARE, TG_DOWNLOAD, TG_COPY, TG_CLEAR};
		float chiph = 18.0f * m->dp, left, chipx, chipy;
		static const char *const chip[3] = {"All", "Chat", "Events"};
		unsigned total = 0, k;
		for (i = 0; i < 5; i++)
		{
			TCL_IconButton(m, ic[i], cx, cy, iconsz, id[i], 0.95f);
			cx -= tcl_min_target;
		}
		left = cx + tcl_min_target * 0.5f;
		chipx = pxl + m->fs;
		chipy = pyt + (wide ? 0 : title_h) + (title_h - chiph) * 0.5f;
		for (i = 0; i < 3; i++)
		{
			if (wide && chipx + max(tcl_min_target, TCL_TextW(chip[i], min(m->fs, 12.0f * m->dp)) + 16.0f * m->dp) > left)
				break;
			chipx += TCL_Chip(m, chipx, chipy, chiph, chip[i], tcl_filter == i, i) + m->fs * 0.5f;
		}
		for (k = 0; k < cl_log.count; k++)
			if (mask & TCL_BIT(TouchChatLog_Get(&cl_log, k)->kind))
				total++;
		if (TCL_SelectedCount())
			dpsnprintf(s, sizeof(s), "%u selected", TCL_SelectedCount());
		else
			dpsnprintf(s, sizeof(s), "%u line%s", total, total == 1 ? "" : "s");
		if (TCL_SelectedCount())
		{
			float fs = min(m->fs, 11.0f * m->dp), sw = TCL_TextW(s, fs), pillw = sw + 12.0f * m->dp;
			float end = left;
			float pill_y = wide ? chipy : pyt + (title_h - chiph) * 0.5f;
			if ((wide ? chipx : pxl + 4.0f * m->dp) + pillw <= end)
			{
				float pc = end - pillw * 0.5f;
				DrawQ_RoundedBox(pc, pill_y + chiph * 0.5f, pillw * 0.5f, chiph * 0.4f, chiph * 0.4f, 0, 0.2f, 0.3f, 0.4f, 0.45f, 0);
				DrawQ_RoundedBox(pc, pill_y + chiph * 0.5f, pillw * 0.5f, chiph * 0.4f, chiph * 0.4f, m->dp, 0.8f, 0.9f, 1, 0.3f, 0);
				DrawQ_String(pc - sw * 0.5f, pill_y + (chiph - fs) * 0.5f, s, 0, fs, fs, 0.9f, 0.95f, 1, 1, 0, NULL, true, FONT_CHAT);
			}
		}
	}

	/* columns: time, icon, text */
	tw = TCL_TextW("00:00:00", m->fs * 0.8f) + m->fs * 0.5f;
	iconw = m->fs * 1.05f + m->fs * 0.5f;
	x = pxl + max(m->fs * 0.8f, 14.0f * m->dp);
	textx = x + tw + iconw;
	textw = pxl + pw - m->fs * 1.8f - textx;
	tcl_row_h = m->rowh;
	tcl_body_h = bodyh;
	tcl_measure_w = textw;
	tcl_measure_m = m;

	/* Measure each changed entry once; all subsequent layout uses cached heights. */
	if (tcl_cache_w != textw || tcl_cache_fs != m->fs)
	{
		unsigned k;
		tcl_cache_w = textw; tcl_cache_fs = m->fs;
		/* Clear keys rather than allowing a generation counter to wrap. */
		for (k = 0; k < cl_log.count; k++)
			((tcl_entry_t *)TouchChatLog_Get(&cl_log, k))->rowsw = 0;
		tcl_cache_key = 1;
	}
	{
		float acc = 0, anchor_acc = 0;
		unsigned k;
		int found = 0;
		for (k = cl_log.count; k-- > 0;)
		{
			tcl_entry_t *e = (tcl_entry_t *)TouchChatLog_Get(&cl_log, k);
			unsigned slot = (unsigned)(e - cl_log.e);
			if (!(mask & TCL_BIT(e->kind))) continue;
			if (tcl_anchor_valid && slot == tcl_anchor_slot && tcl_slot_serial[slot] == tcl_anchor_serial)
			{
				anchor_acc = acc;
				found = 1;
			}
			e->rows = (short)TCL_Measure(NULL, e);
			acc += tcl_entry_h(e, m->rowh, m->pad);
		}
		if (found && tcl_sc.pos > 0 && tcl_anchor_filter == tcl_filter)
			tcl_sc.pos += anchor_acc - tcl_anchor_acc;
		tcl_content_h = acc;
	}
	maxpos = max(0.0f, tcl_content_h - bodyh);
	if (tcl_ptr_down && !tcl_ptr_held && tcl_sc.down && !tcl_sc.moved &&
	    (tcl_ptr_id == TG_ROW || tcl_ptr_id == TG_NAME) && host.realtime - tcl_ptr_at >= 0.5 &&
	    (unsigned)tcl_ptr_arg < cl_log.count && tcl_ptr_serial &&
	    tcl_slot_serial[TCL_Slot((unsigned)tcl_ptr_arg)] == tcl_ptr_serial)
	{
		tcl_selected[TCL_Slot((unsigned)tcl_ptr_arg)] = tcl_ptr_serial;
		tcl_ptr_held = 1;
	}
	TouchChatLog_ScrollStep(&tcl_sc, dt, maxpos);
	if (tcl_sc.down || tcl_sc.vel != 0)
		tcl_scroll_at = host.realtime;
	tcl_sc.pos = tcl_clampf(tcl_sc.pos, 0, maxpos);
	TouchChatLog_Window(&cl_log, mask, NULL, NULL, m->rowh, m->pad, bodyh, tcl_sc.pos, &win);
	tcl_newest_top = win.n ? win.top[0] : 0;
	tcl_newest_bottom = win.n ? win.top[0] + win.h[0] : 0;
	tcl_bottom_pad = 6.0f * m->dp;
	tcl_anchor_valid = win.n > 0 && tcl_sc.pos > 0;
	if (tcl_anchor_valid)
	{
		const tcl_entry_t *e = TouchChatLog_Get(&cl_log, win.idx[0]);
		tcl_anchor_slot = (unsigned)(e - cl_log.e);
		tcl_anchor_serial = tcl_slot_serial[tcl_anchor_slot];
		tcl_anchor_acc = bodyh - win.top[0] - win.h[0] + tcl_sc.pos;
		tcl_anchor_filter = tcl_filter;
	}

	TCL_AddTarget(pxl, rowtop, pw, bodyh, TG_BODY, 0);
	DrawQ_SetClipArea(pxl + 14.0f * m->dp, rowtop, max(0.0f, pw - 28.0f * m->dp), bodyh);
	if (win.n == 0)
		DrawQ_String(pxl + pw * 0.5f - TCL_TextW("No messages or events yet", m->fs) * 0.5f, rowtop + bodyh * 0.4f, "No messages or events yet", 0, m->fs, m->fs, 0.6f, 0.65f, 0.72f, 1, 0, NULL, true, FONT_CHAT);
	for (i = 0; i < win.n; i++)
	{
		int first = tcl_ntg, j;
		TCL_DrawLogRow(m, win.idx[i], x, rowtop + win.top[i], win.h[i], tw, iconw, textx, textw, pxl, pw);
		for (j = first; j < tcl_ntg; j++)
		{
			float bottom = min(tcl_tg[j].y + tcl_tg[j].h, rowtop + bodyh);
			tcl_tg[j].y = max(tcl_tg[j].y, rowtop);
			tcl_tg[j].h = max(0.0f, bottom - tcl_tg[j].y);
		}
	}
	DrawQ_ResetClipArea();

	/* the scroll bar, and the way back to the newest line */
	if (tcl_content_h > bodyh + 1 && host.realtime - tcl_scroll_at < 1.5)
	{
		float th = max(m->fs * 2, bodyh * bodyh / tcl_content_h);
		float ty = rowtop + (bodyh - th) * (1.0f - tcl_sc.pos / max(1.0f, maxpos));
		DrawQ_Fill(pxl + pw - 3.0f * m->dp, ty, 2.0f * m->dp, th, 0.85f, 0.9f, 1, 0.7f * tcl_clampf((float)(1.5 - (host.realtime - tcl_scroll_at)) / 0.7f, 0, 1), 0);
	}
	if (tcl_sc.pos > 1.0f)
	{
		float bs = tcl_min_target * 0.6f;
		float bx = pxl + pw - bs - m->fs * 1.2f, by = rowtop + bodyh - bs - m->fs;
		TouchHUD_GlassPanel(bx, by, bs, bs, bs * 0.3f, 1);
		TCL_Icon(m, TCL_I_LATEST, bx + bs * 0.18f, by + bs * 0.18f, bs * 0.64f, 1, 1, 1, 1);
		TCL_AddTarget(bx + bs * 0.5f - tcl_min_target * 0.5f, by + bs * 0.5f - tcl_min_target * 0.5f, tcl_min_target, tcl_min_target, TG_LATEST, 0);
	}
	if (footer > 0)
	{
		Con_DrawChatInputAt(pxl + m->fs, rowtop + bodyh + 6.0f * m->dp + (footer - m->fs) * 0.5f, m->fs);
	}
	TCL_DrawToast(m, pxl + pw * 0.5f, rowtop + m->fs);
	if (tcl_popup)
		TCL_DrawPopup(m);
	TCL_Claim(0, 0, W, H);
}

#define TCL_FEED_MAX 24
static void TCL_DrawFeed(const tcl_metrics_t *m, float x, float y, float w, float size, int stock_lines)
{
	unsigned idx[TCL_FEED_MAX];
	unsigned n, k, shown = 0;
	unsigned mask = TCL_FEED | (TCL_SystemLines() ? TCL_BIT(TCL_NOTE) : 0);
	float fade = touch_chatlog_fade.value > 0 ? touch_chatlog_fade.value : 15;
	float bottom = y + (float)stock_lines * size;
	float areah = max((float)stock_lines * size, m->rowh * 2), top, ly, used = 0;
	float pad = m->fs * 0.3f, iconw = m->fs * 1.1f + m->fs * 0.4f, textw;
	float rowsh[TCL_FEED_MAX], rowsw[TCL_FEED_MAX];
	float claim_top;

	/* The feed is lines and nothing else: no icon strip over the game. The log (and its clear, copy,
	 * download and share icons) opens from a tap on a line, the pause menu or touch_chatlog_open;
	 * the icons live in the open log's header, and a toast shows where the lines are. */
	n = TouchChatLog_FeedLines(&cl_log, mask, host.realtime, fade, idx, TCL_FEED_MAX);
	if (n == 0 && !(tcl_toast[0] && host.realtime - tcl_toast_at < 1.8f))
	{
		TCL_Claim(0, 0, 0, 0);
		return;
	}
	if (bottom - areah < 0)
		areah = bottom;
	top = bottom - areah;
	textw = max(w - iconw - pad * 2, m->fs * 8);
	claim_top = top;
	TCL_DrawToast(m, x + w * 0.5f, max(2.0f, top - m->fs * 2.2f));

	for (k = 0; k < n; k++)
	{
		const tcl_entry_t *e = TouchChatLog_Get(&cl_log, idx[k]);
		float maxw = 0, h;
		int rows = TCL_Wrap(m, e->text, textw, 0, 0, 0, 0, &maxw);
		h = (float)rows * m->rowh + pad * 0.6f;
		if (used + h > areah && k > 0)
			break;
		rowsh[k] = h;
		rowsw[k] = maxw;
		used += h;
		shown++;
	}
	ly = bottom - used;
	claim_top = min(claim_top, ly);
	for (k = shown; k-- > 0;)
	{
		const tcl_entry_t *e = TouchChatLog_Get(&cl_log, idx[k]);
		float age = (float)(host.realtime - e->time), a = age > fade - 3 ? max(0.0f, (fade - age) / 3.0f) : 1.0f;
		float r, g, b, bw = iconw + rowsw[k] + pad * 2;
		TCL_KindRGB(e->kind, &r, &g, &b);
		DrawQ_Fill(TCL_SnapX(m, x), TCL_SnapY(m, ly), bw, rowsh[k], 0.02f, 0.04f, 0.08f, 0.58f * a, 0);
		if (e->kind != TCL_CHAT)
			DrawQ_Fill(TCL_SnapX(m, x), TCL_SnapY(m, ly), max(2.0f, 2.0f / m->sx), rowsh[k], r, g, b, a, 0); /* the kind's colour as an edge */
		TCL_Icon(m, TouchChatLog_KindIcon(e->kind), x + pad, ly + pad * 0.3f + (m->rowh - m->fs * 1.1f) * 0.5f, m->fs * 1.1f, r, g, b, a);
		TCL_Wrap(m, e->text, textw, 1, x + pad + iconw, ly + pad * 0.3f, a, NULL);
		/* the icon and name: a tap opens that player's actions; the rest of the line opens the log */
		TCL_AddTarget(x, ly, bw, rowsh[k], TG_ROW, (int)idx[k]);
		if (e->name[0])
			TCL_AddTarget(x, ly, iconw + min(rowsw[k], m->fs * 10), min(rowsh[k], m->rowh + pad), TG_NAME, (int)idx[k]);
		ly += rowsh[k];
	}
	TCL_Claim(x, claim_top, w, bottom - claim_top);
}

/* cl_screen.c, last thing drawn over the game: the open log is above every touch control (TURN, DUCK, the ammo,
 * the weapon rail...): they are drawn before it and take no finger while it is open. */
void TouchChatLog_DrawOverlay(void)
{
	tcl_metrics_t m;
	if (!tcl_panel_due)
		return;
	tcl_panel_due = 0;
	if (!tcl_panel || !touch_chatlog.integer || cls.state != ca_connected)
		return;
	TCL_Metrics(tcl_panel_size, &m);
	TCL_DrawPanel(&m, tcl_panel_dt);
}

void TouchChatLog_Draw(float x, float y, float w, float size, int stock_lines)
{
	tcl_metrics_t m;
	float dt;
	TCL_RenamePoll();
	if (!touch_chatlog.integer || cls.state != ca_connected || size <= 0)
	{
		if (touch_chatlog_claim.string[0])
			Cvar_SetQuick(&touch_chatlog_claim, "");
		tcl_ntg = 0;
		tcl_panel = 0;
		tcl_return_chat = 0;
		return;
	}
	dt = bound(0.0f, (float)(host.realtime - tcl_last_frame), 0.1f);
	tcl_last_frame = host.realtime;
	TCL_Metrics(size, &m);
	if (tcl_panel && !vid_touchscreen.integer && key_dest == key_game)
	{
		/* a desktop's open log keeps the chat input (and the pointer) with it: Escape closes the log */
		key_dest = key_message;
		chat_mode = 0;
	}
	tcl_ntg = 0;
	if (tcl_panel)
	{
		/* the log is drawn last (TouchChatLog_DrawOverlay), over the touch controls */
		tcl_panel_due = 1;
		tcl_panel_size = size;
		tcl_panel_dt = dt;
		return;
	}
	tcl_popup = 0;
	if (stock_lines <= 0)
	{
		TCL_Claim(0, 0, 0, 0);
		return;
	}
	TCL_DrawFeed(&m, x, y, w, size, stock_lines);
}

/* ------------------------------------------------------------------ */
/* commands (and what tests read)                                       */
/* ------------------------------------------------------------------ */
static void TCL_Open_f(cmd_state_t *cmd) { TCL_Open(1); tcl_return_chat = Cmd_Argc(cmd) > 1 && !strcmp(Cmd_Argv(cmd, 1), "chat"); }
static void TCL_Close_f(cmd_state_t *cmd) { (void)cmd; TCL_Open(0); }
static void TCL_Toggle_f(cmd_state_t *cmd) { (void)cmd; TCL_Open(!tcl_panel); }

static void TCL_Filter_f(cmd_state_t *cmd)
{
	const char *a = Cmd_Argv(cmd, 1);
	tcl_filter = !strcmp(a, "chat") ? 1 : !strcmp(a, "events") ? 2 : 0;
	memset(&tcl_sc, 0, sizeof(tcl_sc));
}

/* test: inject a line as the console would deliver it: touch_chatlog_inject <1|3> <text> */
static void TCL_Inject_f(cmd_state_t *cmd)
{
	char line[TCL_LEN];
	const char *a = Cmd_Args(cmd);
	int lead = atoi(Cmd_Argv(cmd, 1));
	if (Cmd_Argc(cmd) < 3)
	{
		Con_Printf("usage: touch_chatlog_inject <1|3> <text>\n");
		return;
	}
	while (*a == ' ')
		a++;
	while (*a && *a != ' ')
		a++; /* the lead */
	while (*a == ' ')
		a++;
	dpsnprintf(line, sizeof(line), "^7^3%s", a);
	TouchChatLog_Lead((char)lead);
	TouchChatLog_Line(line);
}

/* test: the log's lines: "CHATLOG <n> <kind> <name> | <plain text>" */
static void TCL_SoundState_f(cmd_state_t *cmd)
{
	(void)cmd;
	Con_Printf("CHATLOG-SOUNDS %u\n", tcl_sound_count);
}

static void TCL_Dump_f(cmd_state_t *cmd)
{
	static const char *const kn[TCL_NKINDS] = {"chat", "join", "play", "spec", "leave", "rename", "note"};
	unsigned i, from = Cmd_Argc(cmd) > 1 ? (unsigned)atoi(Cmd_Argv(cmd, 1)) : 0;
	for (i = from; i < cl_log.count; i++)
	{
		const tcl_entry_t *e = TouchChatLog_Get(&cl_log, i);
		char plain[TCL_LEN];
		TouchMention_StripColors(e->text, plain, sizeof(plain));
		Con_Printf("CHATLOG %u %s %s | %s\n", i, kn[e->kind], e->name[0] ? e->name : "-", plain);
	}
	Con_Printf("CHATLOG-END %u\n", cl_log.count);
}

/* test: a finger through the claim the touch layer uses: touch_chatlog_finger down|move|up <id> <fx> <fy> (prints taken or not) */
static void TCL_Finger_f(cmd_state_t *cmd)
{
	const char *p = Cmd_Argv(cmd, 1);
	qbool taken;
	if (Cmd_Argc(cmd) < 5)
		return;
	taken = TouchChatLog_Finger(!strcmp(p, "down") ? 0 : !strcmp(p, "move") ? 1 : 2, atoi(Cmd_Argv(cmd, 2)), (float)atof(Cmd_Argv(cmd, 3)), (float)atof(Cmd_Argv(cmd, 4)));
	Con_Printf("CHATLOG-FINGER %s %s\n", p, taken ? "taken" : "not taken");
}

/* test: where things are: the targets as window fractions, and the panel's state */
static void TCL_State_f(cmd_state_t *cmd)
{
	static const char *const tn[] = {"none", "log", "clear", "copy", "download", "share", "close", "chip", "name", "row", "latest", "pop", "body", "panel"};
	int i;
	(void)cmd;
	for (i = 0; i < tcl_ntg; i++)
		Con_Printf("CHATLOG-TARGET %s %d %.4f %.4f %.4f %.4f\n", tn[tcl_tg[i].id], tcl_tg[i].arg,
			(tcl_tg[i].x + tcl_tg[i].w * 0.5f) / vid_conwidth.value, (tcl_tg[i].y + tcl_tg[i].h * 0.5f) / vid_conheight.value,
			tcl_tg[i].w / vid_conwidth.value, tcl_tg[i].h / vid_conheight.value);
	Con_Printf("CHATLOG-LAYOUT newest_top=%.2f newest_bottom=%.2f body=%.2f bottom_pad=%.2f\n", tcl_newest_top, tcl_newest_bottom, tcl_body_h, tcl_bottom_pad);
	Con_Printf("CHATLOG-STATE conh=%.0f panel=%d filter=%d pos=%.1f vel=%.1f content=%.1f body=%.1f popup=%d popup_name=%s lines=%u dest=%d selected=%u\n",
		vid_conheight.value, tcl_panel, tcl_filter, tcl_sc.pos, tcl_sc.vel, tcl_content_h, tcl_body_h, tcl_popup, tcl_popup ? tcl_popup_name : "-", cl_log.count, (int)key_dest, TCL_SelectedCount());
}

void TouchChatLog_Init(void)
{
	Cvar_RegisterVariable(&touch_chatlog);
	Cvar_RegisterVariable(&touch_chatlog_fade);
	Cvar_RegisterVariable(&touch_chatlog_feed);
	Cvar_RegisterVariable(&touch_chatlog_sound_file);
	Cvar_RegisterVariable(&touch_chatlog_sound_volume);
	Cvar_RegisterVariable(&touch_chatlog_claim);
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_scroll", TCL_Scroll_f, "touch_chatlog_scroll <rows>: open the chat log and scroll it back (positive) or forward");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_open", TCL_Open_f, "open the chat log (every line of the session, with times)");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_close", TCL_Close_f, "close the chat log");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_toggle", TCL_Toggle_f, "open or close the chat log");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_filter", TCL_Filter_f, "touch_chatlog_filter all|chat|events: what the open log lists");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_ptr", TCL_Ptr_f, "touch_chatlog_ptr down|move|up <fx> <fy>: a finger or click on the chat feed or log (window fractions)");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_clear", TCL_Clear_f, "empty the chat log and the chat lines on show");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_export", TCL_Export_f, "save the chat log as text (share sheet and Downloads on Android, ~/Downloads elsewhere)");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_share", TCL_Share_f, "open the share sheet with the chat log (Android)");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_download", TCL_Download_f, "save the chat log as text to Downloads (~/Downloads elsewhere)");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_copy", TCL_Copy_f, "put the chat log text on the clipboard");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_inject", TCL_Inject_f, "test: touch_chatlog_inject <1|3> <text> adds a line as the console would deliver it");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_sound_state", TCL_SoundState_f, "test: print player event sound attempts");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_dump", TCL_Dump_f, "test: print the chat log's lines with their kinds");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_finger", TCL_Finger_f, "test: touch_chatlog_finger down|move|up <id> <fx> <fy>: a finger through the claim the touch layer uses");
	Cmd_AddCommand(CF_CLIENT, "touch_chatlog_state", TCL_State_f, "test: print the chat log's hit targets and state");
}

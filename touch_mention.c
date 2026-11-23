/*
 * touch_mention.c -- chat @mentions and the live "is typing" line (Godnotic).
 *
 * Typing: an unmodified Xonotic server shows a chat bubble over a player who
 * has the chat button down (usercmd button 9, which the stock client sets
 * whenever key_dest is not the game: see cl_input.c). The bubble is a plain
 * networked sprite entity (models/misc/chatbubble.spr) attached to the player.
 * Nothing else is sent, so this is all the protocol carries: we read those
 * entities. Limits: only players the server sends us (in view range), only
 * players (spectators have no bubble), and the server's ShowChatBubble hook
 * can hide it. Demos show it as recorded.
 *
 * Mentions: the picker reads the scoreboard names the server already sends
 * (cl.scores), edits only the local chat line, and the server gets the
 * finished text through the usual say command.
 *
 * Roster and typing text are also published as _touch_roster / _touch_typing
 * so the QuakeC chat sheet can use the same data.
 */
#include "quakedef.h"
#include "touch_mention.h"
#include "touch_mention_logic.h"
#include "touch_ui.h"

static cvar_t touch_chat_mentions = {CF_CLIENT | CF_ARCHIVE, "touch_chat_mentions", "1",
	"1 opens a picker of the players and spectators when @ is typed in chat"};
static cvar_t touch_chat_typing = {CF_CLIENT | CF_ARCHIVE, "touch_chat_typing", "1",
	"1 shows who is typing (the server's chat bubble over their head) in the chat area"};
static cvar_t touch_roster = {CF_CLIENT, "_touch_roster", "",
	"player names the server sent, one per line, spectators marked with a leading tab (read by the chat sheet)"};
static cvar_t touch_typing = {CF_CLIENT, "_touch_typing", "",
	"the \"is typing\" text the chat area shows (read by the chat sheet)"};

static cvar_t touch_mention_draws = {CF_CLIENT, "_touch_mention_draws", "0", "test: how many times the chat area drew the typing line"};

static cvar_t touch_mention_pick = {CF_CLIENT, "_touch_mention_pick", "", "test: the engine @ list as \"count:first name\""};
static cvar_t touch_sheet_pick = {CF_CLIENT, "_touch_sheet_pick", "", "test: the touch sheet @ list as \"count:first name\""};
static cvar_t touch_mention_dbg = {CF_CLIENT, "_touch_mention_dbg", "", "test: key_dest, chat line and cursor the @ list sees"};
static cvar_t touch_sheet_dbg = {CF_CLIENT, "_touch_sheet_dbg", "", "test: the touch sheet's open state and text"};
static cvar_t touch_chat_roster_source = {CF_CLIENT, "touch_chat_roster_source", "auto", "where the @ list takes names from: auto (cl.scores, then getstatus, plus QuakeC), scores, status (getstatus only: for tests)"};
static cvar_t touch_roster_all = {CF_CLIENT, "_touch_roster_all", "", "the merged roster the @ lists use (cl.scores, getstatus, QuakeC, chat lines): \"slot<TAB>p|s<TAB>name\" per line, colours stripped; the QuakeC chat sheet reads this"};
static cvar_t touch_roster_n = {CF_CLIENT, "_touch_roster_n", "0", "test: how many names _touch_roster holds"};

#define TM_ROSTER_MAX 64
typedef struct { char name[TM_MAX_NAME]; qbool spectator; } tm_entry_t;
static tm_entry_t tm_roster[TM_ROSTER_MAX];
static int tm_roster_n;

static int tm_sel;        // highlighted row
static int tm_dismissed = -1; // the @ position the player closed the picker on
#define TM_MAX_FOUND 32
static int tm_off; /* first row shown when more names fit than rows */
static struct { qbool down, moved; float y0, last_y, vel, acc; double last_t; int row; } tm_drag; /* a finger or button held on the list */
static struct { int n, idx[TM_MAX_FOUND]; int at; float x, y, w, rowh; } tm_pick;

static void TM_SetStr(cvar_t *c, const char *s)
{
	if (strcmp(c->string, s))
		Cvar_SetQuick(c, s);
}

/* Where the names come from, in order of preference:
 *  1. cl.scores: the server's own svc_updatename / svc_updatefrags for every client,
 *     players and spectators, whatever CSQC is running (-666 frags = spectator);
 *  2. a connectionless getstatus to the current server, asked every ~10 s while the
 *     chat line is open and cl.scores has no names (touch_mention_status below);
 *  3. _touch_roster, which our QuakeC publishes ("slot<TAB>p|s<TAB>name"), as an
 *     extra source: a public server's own csprogs.dat does not run that code.
 * The picker and the typing line use the merged list. */
static int tm_roster_slot[TM_ROSTER_MAX]; /* cl.scores slot, -1 when the source has none */
static int tm_src_scores, tm_src_status, tm_src_qc, tm_src_chat;
/* names seen speaking this session ("name: text" chat lines), newest last */
static char tm_chat_name[TM_ROSTER_MAX][TM_MAX_NAME];
static int tm_chat_n;

/* getstatus answer (TouchMention_StatusPacket) */
static struct { char name[TM_ROSTER_MAX][TM_MAX_NAME]; qbool spec[TM_ROSTER_MAX]; int n; double got, asked; } tm_status;

static qbool TM_Has(const char *plain)
{
	int i;
	for (i = 0; i < tm_roster_n; i++)
		if (!strcasecmp(tm_roster[i].name, plain))
			return true;
	return false;
}

static void TM_Add(const char *raw, qbool spec, int slot, int *counter)
{
	char plain[TM_MAX_NAME];
	tm_entry_t *e;
	if (tm_roster_n >= TM_ROSTER_MAX)
		return;
	TouchMention_StripColors(raw, plain, sizeof(plain));
	if (!plain[0] || TM_Has(plain))
		return;
	e = &tm_roster[tm_roster_n];
	dp_strlcpy(e->name, plain, sizeof(e->name));
	e->spectator = spec;
	tm_roster_slot[tm_roster_n] = slot;
	tm_roster_n++;
	(*counter)++;
}

static void TM_BuildRoster(void)
{
	const char *p = touch_roster.string;
	char me[TM_MAX_NAME];
	int i;
	static int last_sig = -1;
	tm_roster_n = 0;
	tm_src_scores = tm_src_status = tm_src_qc = tm_src_chat = 0;
	if (cls.state != ca_connected)
		return;
	TouchMention_StripColors(Cvar_VariableString(&cvars_all, "_cl_name", ~0u), me, sizeof(me));
	/* our own name goes in first so that nobody lists us, and is dropped again below */
	if (me[0])
	{
		dp_strlcpy(tm_roster[0].name, me, sizeof(tm_roster[0].name));
		tm_roster_n = 1;
	}
	if (cl.scores && strcmp(touch_chat_roster_source.string, "status"))
		for (i = 0; i < cl.maxclients; i++)
			if (cl.scores[i].name[0] && i != cl.realplayerentity - 1)
				TM_Add(cl.scores[i].name, cl.scores[i].frags == -666 /* FRAGS_SPECTATOR */, i, &tm_src_scores);
	if (!tm_src_scores && strcmp(touch_chat_roster_source.string, "scores") && tm_status.n && host.realtime - tm_status.got < 60)
		for (i = 0; i < tm_status.n; i++)
			TM_Add(tm_status.name[i], tm_status.spec[i], -1, &tm_src_status);
	while (*p && tm_roster_n < TM_ROSTER_MAX)
	{
		const char *eol = strchr(p, '\n'), *t1, *t2;
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		char line[160];
		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = 0;
		p += len + (eol ? 1 : 0);
		t1 = strchr(line, '\t');
		t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		if (t2)
			TM_Add(t2 + 1, t1[1] == 's', atoi(line), &tm_src_qc);
	}
	for (i = 0; i < tm_chat_n; i++)
		TM_Add(tm_chat_name[i], false, -1, &tm_src_chat);
	if (me[0]) /* drop our own placeholder entry */
	{
		memmove(&tm_roster[0], &tm_roster[1], sizeof(tm_roster[0]) * (size_t)(tm_roster_n - 1));
		memmove(&tm_roster_slot[0], &tm_roster_slot[1], sizeof(tm_roster_slot[0]) * (size_t)(tm_roster_n - 1));
		tm_roster_n--;
	}
	if (key_dest == key_message)
	{
		int sig = tm_roster_n * 10000 + tm_src_scores * 1000 + tm_src_status * 100 + tm_src_qc * 10 + tm_src_chat;
		if (sig != last_sig)
			Con_Printf("touch chat: roster %d entries (cl.scores %d, getstatus %d, QuakeC %d, chat lines %d)\n",
				tm_roster_n, tm_src_scores, tm_src_status, tm_src_qc, tm_src_chat);
		last_sig = sig;
	}
	else
		last_sig = -1;
}

/* A chat line the client printed ("\1name^7: text", or team chat "(team) name: text"):
 * remember who spoke, so "spec" is on the list before any scoreboard has it. */
void TouchMention_NoteChat(const char *line)
{
	char name[TM_MAX_NAME];
	int i;
	if (!TouchMention_ChatName(line, name, sizeof(name)))
		return;
	for (i = 0; i < tm_chat_n; i++)
		if (!strcasecmp(tm_chat_name[i], name))
			return;
	if (tm_chat_n >= TM_ROSTER_MAX)
	{
		memmove(tm_chat_name[0], tm_chat_name[1], sizeof(tm_chat_name[0]) * (TM_ROSTER_MAX - 1));
		tm_chat_n--;
	}
	dp_strlcpy(tm_chat_name[tm_chat_n++], name, sizeof(tm_chat_name[0]));
}

/* The getstatus fallback. net: netconn.c hands every connectionless packet to
 * TouchMention_StatusPacket first. */
qbool TouchMention_StatusPacket(const char *string, int length)
{
	const char *p, *eol;
	if (length < 15 || memcmp(string, "statusResponse\n", 15))
		return false;
	p = string + 15;
	eol = strchr(p, '\n');
	if (!eol || !strstr(p, "chatroster")) /* not our query: the server list's */
		return false;
	tm_status.n = 0;
	p = eol + 1;
	while (*p && tm_status.n < TM_ROSTER_MAX)
	{
		int score = 0, ping = 0;
		const char *q1, *q2;
		eol = strchr(p, '\n');
		q1 = strchr(p, '"');
		q2 = q1 ? strchr(q1 + 1, '"') : NULL;
		if (q1 && q2 && (!eol || q2 < eol) && sscanf(p, "%d %d", &score, &ping) >= 1)
		{
			size_t l = (size_t)(q2 - q1 - 1);
			if (l >= TM_MAX_NAME)
				l = TM_MAX_NAME - 1;
			memcpy(tm_status.name[tm_status.n], q1 + 1, l);
			tm_status.name[tm_status.n][l] = 0;
			tm_status.spec[tm_status.n] = score == -666;
			tm_status.n++;
		}
		if (!eol)
			break;
		p = eol + 1;
	}
	tm_status.got = host.realtime;
	return true;
}

/* While the chat line is open and cl.scores has no names: ask the server who is on. */
static void TM_AskStatus(void)
{
	if (key_dest != key_message || cls.state != ca_connected || !cls.connect_mysocket || tm_src_scores)
		return;
	if (host.realtime - tm_status.asked < 10)
		return;
	tm_status.asked = host.realtime;
	NetConn_WriteString(cls.connect_mysocket, "\377\377\377\377getstatus chatroster", &cls.connect_address);
}

static void TM_PublishRoster(void)
{
	char buf[2048];
	size_t o = 0;
	int i;
	for (i = 0; i < tm_roster_n; i++)
	{
		int n = dpsnprintf(buf + o, sizeof(buf) - o, "%d\t%c\t%s\n", tm_roster_slot[i], tm_roster[i].spectator ? 's' : 'p', tm_roster[i].name);
		if (n < 0 || o + (size_t)n >= sizeof(buf))
			break;
		o += (size_t)n;
	}
	buf[o] = 0;
	TM_SetStr(&touch_roster_all, buf);
	if (touch_roster_n.integer != tm_roster_n)
		Cvar_SetValueQuick(&touch_roster_n, tm_roster_n);
}

/* ---- typing ---- */

static int tm_bubble_model;

static int TM_BubbleModel(void)
{
	int i;
	if (tm_bubble_model > 0 && tm_bubble_model < MAX_MODELS
		&& !strcmp(cl.model_name[tm_bubble_model], "models/misc/chatbubble.spr"))
		return tm_bubble_model;
	tm_bubble_model = 0;
	for (i = 1; i < MAX_MODELS && cl.model_name[i][0]; i++)
		if (!strcmp(cl.model_name[i], "models/misc/chatbubble.spr"))
			return tm_bubble_model = i;
	return 0;
}

static void TM_BuildTyping(char *out, size_t size)
{
	const char *names[TM_ROSTER_MAX];
	char store[TM_ROSTER_MAX][TM_MAX_NAME];
	int count = 0, i, r, model, n = min(cl.num_entities, MAX_EDICTS);
	out[0] = 0;
	if (cls.state != ca_connected || !touch_chat_typing.integer || !(model = TM_BubbleModel()))
		return;
	for (i = 1; i < n && count < TM_ROSTER_MAX; i++)
	{
		const entity_state_t *s = &cl.entities[i].state_current;
		int slot;
		if (!s->active || s->modelindex != model || !s->tagentity)
			continue;
		slot = s->tagentity - 1;
		for (r = 0; r < tm_roster_n && tm_roster_slot[r] != slot; r++)
			;
		if (r >= tm_roster_n)
			continue;
		dp_strlcpy(store[count], tm_roster[r].name, TM_MAX_NAME);
		names[count] = store[count];
		count++;
	}
	TouchMention_TypingText(names, count, out, size);
}

/* ---- picker ---- */

static void TM_Matches(void)
{
	const char *names[TM_ROSTER_MAX];
	int i, at, qlen;
	tm_pick.n = 0;
	if (key_dest != key_message || chat_mode < 0 || !touch_chat_mentions.integer)
		return;
	at = TouchMention_Token(chat_buffer, chat_bufferpos);
	if (at < 0 || at == tm_dismissed)
		return;
	qlen = chat_bufferpos - at - 1;
	for (i = 0; i < tm_roster_n; i++)
		names[i] = tm_roster[i].name;
	tm_pick.n = TouchMention_Filter(names, tm_roster_n, chat_buffer + at + 1, (size_t)qlen, tm_pick.idx, TM_MAX_FOUND);
	tm_pick.at = at;
	if (tm_sel >= tm_pick.n)
		tm_sel = 0;
	tm_off = bound(0, tm_off, max(0, tm_pick.n - TM_MAX_SHOWN));
	{
		char t[TM_MAX_NAME + 24];
		dpsnprintf(t, sizeof(t), "%d:%s off=%d", tm_pick.n, tm_pick.n ? tm_roster[tm_pick.idx[0]].name : "", tm_off);
		if (strcmp(touch_mention_pick.string, t))
			Cvar_SetQuick(&touch_mention_pick, t);
	}
}

/* keeps the highlighted row among the rows shown */
static void TM_ShowSel(void)
{
	if (tm_sel < tm_off)
		tm_off = tm_sel;
	else if (tm_sel >= tm_off + TM_MAX_SHOWN)
		tm_off = tm_sel - TM_MAX_SHOWN + 1;
}

static void TM_Accept(int row);

/* Dragging the list with the pointer (or a finger the engine maps to the pointer):
 * it follows in whole rows, a flick coasts and slows, a press that never moved is a tap. */
static void TM_Drag(void)
{
	float rowh = tm_pick.rowh > 0 ? tm_pick.rowh : 16.0f, my = in_windowmouse_y * vid_conheight.value / vid.mode.height;
	double now = host.realtime;
	int maxoff = max(0, tm_pick.n - TM_MAX_SHOWN), whole;
	if (tm_drag.down)
	{
		if (TouchKey_IsDown(K_MOUSE1))
		{
			float dy = my - tm_drag.y0;
			if (fabs(dy) > rowh * 0.5f)
				tm_drag.moved = true;
			if (now > tm_drag.last_t)
				tm_drag.vel = tm_drag.vel * 0.6f + (-(my - tm_drag.last_y) / rowh / (float)(now - tm_drag.last_t)) * 0.4f;
			tm_drag.last_y = my; tm_drag.last_t = now;
			whole = (int)(fabs(dy) / rowh);
			if (whole)
			{
				tm_off = bound(0, tm_off + (dy > 0 ? -whole : whole), maxoff);
				tm_drag.y0 += (dy > 0 ? 1 : -1) * whole * rowh;
			}
		}
		else
		{
			tm_drag.down = false;
			if (!tm_drag.moved)
				TM_Accept(tm_drag.row);
		}
		return;
	}
	if (fabs(tm_drag.vel) > 0.6f && maxoff)
	{
		float dt = (float)min(0.1, now - tm_drag.last_t);
		tm_drag.last_t = now;
		tm_drag.acc += tm_drag.vel * dt;
		whole = (int)tm_drag.acc;
		if (whole)
		{
			tm_off = bound(0, tm_off + whole, maxoff);
			tm_drag.acc -= whole;
		}
		tm_drag.vel *= powf(0.04f, dt);
	}
	else
		tm_drag.vel = 0;
}

static void TM_Accept(int row)
{
	const tm_entry_t *e;
	int c;
	if (row < 0 || row >= tm_pick.n)
		return;
	e = &tm_roster[tm_pick.idx[row]];
	c = TouchMention_Insert(chat_buffer, sizeof(chat_buffer), tm_pick.at, chat_bufferpos, e->name);
	if (c >= 0)
		chat_bufferpos = c;
	tm_pick.n = 0;
	tm_sel = 0;
}

qbool TouchMention_Key(int key, int ascii)
{
	(void)ascii;
	TM_BuildRoster();
	TM_Matches();
	if (tm_pick.n > TM_MAX_SHOWN && (key == K_MWHEELUP || key == K_MWHEELDOWN))
	{
		tm_off = bound(0, tm_off + (key == K_MWHEELUP ? -1 : 1), tm_pick.n - TM_MAX_SHOWN);
		return true;
	}
	if (TouchChatLog_Key(key, true))
		return true;
	/* Space after "@fi": when "fi" is not already a player's exact name and something
	 * matches, the best match replaces it ("@fishman "); otherwise the space is typed. */
	if ((key == K_SPACE || ascii == ' ') && tm_pick.n && !(key == K_ENTER))
	{
		const char *q = chat_buffer + tm_pick.at + 1;
		size_t qlen = (size_t)(chat_bufferpos - tm_pick.at - 1);
		int i, exact = 0;
		for (i = 0; i < tm_roster_n; i++)
			if (strlen(tm_roster[i].name) == qlen && !strncasecmp(tm_roster[i].name, q, qlen))
				exact = 1;
		if (qlen && !exact)
		{
			TM_Accept(tm_sel);
			return true;
		}
		return false;
	}
	if (key != K_ESCAPE && key != K_ENTER && key != K_KP_ENTER && key != K_TAB
		&& key != K_UPARROW && key != K_DOWNARROW && key != K_MOUSE1)
		return false;
	if (!tm_pick.n)
	{
		if (key != K_ESCAPE)
			tm_dismissed = -1;
		return false;
	}
	switch (key)
	{
	case K_UPARROW:   tm_sel = (tm_sel + tm_pick.n - 1) % tm_pick.n; TM_ShowSel(); return true;
	case K_DOWNARROW: tm_sel = (tm_sel + 1) % tm_pick.n; TM_ShowSel(); return true;
	case K_TAB: case K_ENTER: case K_KP_ENTER: TM_Accept(tm_sel); return true;
	case K_ESCAPE: tm_dismissed = tm_pick.at; tm_pick.n = 0; return true; /* the next Esc closes the chat */
	case K_MOUSE1:
	{
		float mx = in_windowmouse_x * vid_conwidth.value / vid.mode.width;
		float my = in_windowmouse_y * vid_conheight.value / vid.mode.height;
		int row = (int)((my - tm_pick.y) / tm_pick.rowh);
		if (mx < tm_pick.x || mx > tm_pick.x + tm_pick.w || my < tm_pick.y || row < 0
			|| row >= TM_MAX_SHOWN || tm_off + row >= tm_pick.n)
			return false;
		/* the press may be the start of a drag: Draw settles it against the button state */
		tm_drag.down = true; tm_drag.moved = false; tm_drag.y0 = tm_drag.last_y = my;
		tm_drag.vel = 0; tm_drag.acc = 0; tm_drag.last_t = host.realtime; tm_drag.row = tm_off + row;
		return true;
	}
	}
	return false;
}

void TouchMention_Init(void)
{
	Cvar_RegisterVariable(&touch_chat_mentions);
	Cvar_RegisterVariable(&touch_chat_typing);
	Cvar_RegisterVariable(&touch_roster);
	Cvar_RegisterVariable(&touch_typing);
	Cvar_RegisterVariable(&touch_mention_draws);
	Cvar_RegisterVariable(&touch_roster_n);
	Cvar_RegisterVariable(&touch_roster_all);
	Cvar_RegisterVariable(&touch_chat_roster_source);
	Cvar_RegisterVariable(&touch_sheet_dbg);
	Cvar_RegisterVariable(&touch_mention_dbg);
	Cvar_RegisterVariable(&touch_mention_pick);
	Cvar_RegisterVariable(&touch_sheet_pick);
}

static char tm_typing[160];
static float tm_input_x, tm_input_y, tm_input_size;
static unsigned tm_input_frame, tm_frame; /* the frame the chat line was last drawn in */

void TouchMention_NoteInput(float x, float y, float size)
{
	tm_input_x = x;
	tm_input_y = y;
	tm_input_size = size;
	tm_input_frame = tm_frame;
}

/* Once per frame, from SCR_DrawScreen, whatever the chat area is doing. */
void TouchMention_Frame(void)
{
	tm_frame++;
	if (cls.state != ca_connected || cls.demoplayback)
	{
		if (tm_typing[0] || touch_roster.string[0])
		{
			tm_typing[0] = 0;
			TM_SetStr(&touch_typing, "");
			TM_SetStr(&touch_roster, "");
		}
		return;
	}
	TM_BuildRoster();
	TM_AskStatus();
	TM_PublishRoster();
	TM_BuildTyping(tm_typing, sizeof(tm_typing));
	TM_SetStr(&touch_typing, tm_typing);
}

void TouchMention_Draw(float x, float v, float size)
{
	const char *typing = tm_typing;
	float y = v;
	int i;
	if (cls.state != ca_connected || cls.demoplayback)
		return;
	Cvar_SetValueQuick(&touch_mention_draws, touch_mention_draws.value + 1);

	{
		char t[256];
		dpsnprintf(t, sizeof(t), "dest=%d mode=%d pos=%d buf=%s mouse=%.0f,%.0f drag=%d moved=%d list=%.0f,%.0f,%.0f rowh=%.1f", (int)key_dest, (int)chat_mode, chat_bufferpos, chat_buffer, in_windowmouse_x * vid_conwidth.value / vid.mode.width, in_windowmouse_y * vid_conheight.value / vid.mode.height, tm_drag.down, tm_drag.moved, tm_pick.x, tm_pick.y, tm_pick.w, tm_pick.rowh);
		if (strcmp(touch_mention_dbg.string, t))
			Cvar_SetQuick(&touch_mention_dbg, t);
	}
	/* Wherever the chat line was drawn this frame (the chat area, or the touch overlay's
	 * place at the top or above its keys): the list goes under it, or above it. */
	if (key_dest == key_message && tm_frame - tm_input_frame <= 1)
	{
		x = tm_input_x;
		v = tm_input_y;
		size = tm_input_size;
		y = v;
	}
	if (key_dest == key_message)
		y += size; /* the input line sits at v */
	if (typing[0])
	{
		DrawQ_String(x, y, typing, 0, size, size, 0.75f, 0.85f, 1.0f, 0.9f, 0, NULL, true, FONT_CHAT);
		y += size;
	}
	TM_Matches();
	if (!tm_pick.n)
	{
		tm_drag.down = false;
		return;
	}
	TM_Drag();
	{
		int shown = min(tm_pick.n, TM_MAX_SHOWN);
		float rowh = size * 1.4f, w = size * 18.0f, h = rowh * shown;
		/* below the input line, where the chat wedge at the top of the screen leaves room; above
		 * it only when there is none (a soft keyboard covers the lower half on a phone) */
		float limit = vid_conheight.value;
		int below;
#ifdef __ANDROID__
		limit *= 0.55f;
#endif
		y = TouchMention_ListY(v, y, h, limit, &below);
		(void)below;
		tm_pick.x = x; tm_pick.y = y; tm_pick.w = w; tm_pick.rowh = rowh;
		/* tinted glass, as the other Touch panels: translucent dark body, light rim, rounded corners */
		DrawQ_RoundedBox(x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, rowh * 0.3f, 0, 0.07f, 0.08f, 0.10f, 0.62f, 0);
		DrawQ_RoundedBox(x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, rowh * 0.3f, max(1.0f, rowh / 24.0f), 1, 1, 1, 0.35f, 0);
		for (i = 0; i < shown; i++)
		{
			const tm_entry_t *e = &tm_roster[tm_pick.idx[tm_off + i]];
			char label[TM_MAX_NAME + 16];
			if (tm_off + i == tm_sel)
				DrawQ_RoundedBox(x + w * 0.5f, y + (i + 0.5f) * rowh, w * 0.5f - 2, rowh * 0.5f - 1, rowh * 0.25f, 0, 0.208f, 0.518f, 0.894f, 0.55f, 0);
			dpsnprintf(label, sizeof(label), "@%s%s", e->name, e->spectator ? "  (spectator)" : "");
			DrawQ_String(x + size * 0.4f, y + i * rowh + (rowh - size) * 0.5f, label, 0, size, size,
				1, 1, 1, 1, 0, NULL, true, FONT_CHAT);
		}
	}
}

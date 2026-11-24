#ifndef TOUCH_CHATLOG_LOGIC_H
#define TOUCH_CHATLOG_LOGIC_H

/*
Godnotic chat log: the pure pieces of touch_chatlog.c (ring buffer, event
classification, fade, scroll physics, the virtual window over the lines, hit
targets, icon bitmaps, text export), free of engine state so
tests/mention/chatlog_test.c can check them with a plain C compiler.
*/

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "touch_mention_logic.h"

#ifndef TCL_SNPRINTF
#define TCL_SNPRINTF snprintf /* the engine defines its own (dpsnprintf) before including this */
#endif

#define TCL_CAP 1000 // a long session: lines are kept until the player clears them
#define TCL_LEN 256
#define TCL_NAME 48

// What a line is. CHAT is a player speaking, the next five are player events
// (the game's own chat-box lines, worded the same way for every one), NOTE is
// any other line the game puts in the chat box.
enum { TCL_CHAT, TCL_JOIN, TCL_PLAY, TCL_SPEC, TCL_LEAVE, TCL_RENAME, TCL_NOTE, TCL_NKINDS };
#define TCL_BIT(k) (1u << (k))
#define TCL_EVENTS (TCL_BIT(TCL_JOIN) | TCL_BIT(TCL_PLAY) | TCL_BIT(TCL_SPEC) | TCL_BIT(TCL_LEAVE) | TCL_BIT(TCL_RENAME))
#define TCL_ALL (TCL_BIT(TCL_CHAT) | TCL_EVENTS | TCL_BIT(TCL_NOTE))
#define TCL_FEED (TCL_BIT(TCL_CHAT) | TCL_EVENTS) // what the live feed shows (NOTE too when the game's own lines are on)

typedef struct
{
	double time;  // engine clock, for the fade
	time_t wall;  // wall clock, for the export and the log's time column
	unsigned char kind; // TCL_*
	short rows;         // wrapped rows at width rowsw, 0 when not measured yet (the engine fills these)
	int rowsw;          // the width (and size) the rows were measured at
	char name[TCL_NAME]; // who the line is about or from, no colour codes ("" when unknown)
	char text[TCL_LEN];  // coloured: what is drawn
} tcl_entry_t;

typedef struct
{
	tcl_entry_t e[TCL_CAP];
	unsigned head;  // next slot to write
	unsigned count; // lines held, at most TCL_CAP
} tcl_log_t;

static inline const tcl_entry_t *TouchChatLog_Get(const tcl_log_t *l, unsigned i);

// Where plain character pi of a coloured string starts in it (the colour codes
// are the ones TouchMention_StripColors drops). Past the end: the string's length.
static inline size_t tcl_raw_of_plain(const char *raw, size_t pi)
{
	const char *in = raw;
	size_t o = 0;
	while (*in)
	{
		if (in[0] == '^')
		{
			if (in[1] >= '0' && in[1] <= '9')
			{
				in += 2;
				continue;
			}
			if ((in[1] == 'x' || in[1] == 'X') && strspn(in + 2, "0123456789abcdefABCDEF") >= 3)
			{
				in += 5;
				continue;
			}
			if (in[1] == '^')
				in++;
		}
		if (o == pi)
			return (size_t)(in - raw);
		o++;
		in++;
	}
	return (size_t)(in - raw);
}

// The game words its chat-box lines in English ("<name> connected", "<name> is
// now playing on the Red team"): each pattern below is one of those, and the
// verb is what the log says instead, the same for every game and server.
// %T is what followed the pattern (a team name with its colours).
typedef struct { const char *pat; unsigned char kind; unsigned char at_end; const char *verb; } tcl_pattern_t;
static const tcl_pattern_t tcl_patterns[] = {
	{" is now playing on the ", TCL_PLAY, 0, "joined the %T"},
	{" is now playing", TCL_PLAY, 1, "started playing"},
	{" is now spectating", TCL_SPEC, 1, "moved to spectators"},
	{" is now spectating", TCL_SPEC, 0, "moved to spectators"},
	{" was forced to spectate", TCL_SPEC, 0, "was forced to spectate%T"},
	{" switched to the ", TCL_PLAY, 0, "switched to the %T"},
	{" was moved into the ", TCL_PLAY, 0, "was moved into the %T"},
	{" changed name to ", TCL_RENAME, 0, "is now known as %T"},
	{" left the game", TCL_LEAVE, 0, "left the server%T"},
	{" disconnected", TCL_LEAVE, 1, "left the server"},
	{" was kicked", TCL_LEAVE, 0, "was kicked%T"},
	{" has left the join queue", TCL_LEAVE, 1, "left the join queue"},
	{" connected", TCL_JOIN, 1, "joined the server"},
};

// The colour a verb is drawn in, per kind (a ^xRGB code, so it needs no palette).
static inline const char *TouchChatLog_KindColor(int kind)
{
	switch (kind)
	{
	case TCL_JOIN: return "^x7d7";
	case TCL_PLAY: return "^x7bf";
	case TCL_SPEC: return "^xbbb";
	case TCL_LEAVE: return "^xf88";
	case TCL_RENAME: return "^xfd6";
	case TCL_NOTE: return "^xbbb";
	}
	return "^7";
}

static inline const char *tcl_skip_prefix(const char *raw)
{
	return strncmp(raw, "^7^3", 4) ? raw : raw + 4; // what the console printer puts first
}

// The line as the log shows it for an event: "<name> <verb>". Returns the kind
// (TCL_CHAT for a speaker, TCL_NOTE for anything else) and fills name (plain)
// and, for an event, out (coloured). lead is the line's first byte: 1 a player
// speaking, 3 the game's own line.
static int TouchChatLog_Classify(const char *raw, int lead, char *name, size_t namesz, char *out, size_t outsz)
{
	char plain[TCL_LEN];
	size_t n, k;
	name[0] = 0;
	out[0] = 0;
	TouchMention_StripColors(tcl_skip_prefix(raw), plain, sizeof(plain));
	n = strlen(plain);
	while (n && (plain[n - 1] == ' ' || plain[n - 1] == '\n' || plain[n - 1] == '\r' || plain[n - 1] == '\t'))
		plain[--n] = 0;
	if (lead == 1)
	{
		const char *c = strstr(plain, ": ");
		if (c && c > plain && (size_t)(c - plain) < namesz)
		{
			memcpy(name, plain, (size_t)(c - plain));
			name[c - plain] = 0;
		}
		return TCL_CHAT;
	}
	if (lead != 3 && lead != 0)
		return TCL_NOTE;
	for (k = 0; k < sizeof(tcl_patterns) / sizeof(tcl_patterns[0]); k++)
	{
		const tcl_pattern_t *p = &tcl_patterns[k];
		const char *at = NULL;
		size_t pl = strlen(p->pat), pi;
		if (p->at_end)
		{
			if (n >= pl && !strcmp(plain + n - pl, p->pat))
				at = plain + n - pl;
		}
		else
			at = strstr(plain, p->pat);
		if (!at || at == plain || (size_t)(at - plain) >= namesz)
			continue;
		pi = (size_t)(at - plain);
		memcpy(name, plain, pi);
		name[pi] = 0;
		{
			const char *base = tcl_skip_prefix(raw);
			size_t nameend = tcl_raw_of_plain(base, pi);
			const char *tail = base + tcl_raw_of_plain(base, pi + pl - 1) + 1; /* just past the pattern: its colour codes belong to the tail */
			const char *v = p->verb;
			size_t o;
			int w = TCL_SNPRINTF(out, outsz, "^7%.*s^7 %s", (int)nameend, base, TouchChatLog_KindColor(p->kind));
			if (w < 0 || (size_t)w >= outsz)
			{
				out[0] = 0;
				return TCL_NOTE;
			}
			o = (size_t)w;
			while (*v && o + 1 < outsz)
			{
				if (v[0] == '%' && v[1] == 'T')
				{
					size_t tl = strlen(tail);
					if (o + tl + 1 >= outsz)
						tl = outsz - o - 1;
					memcpy(out + o, tail, tl);
					o += tl;
					v += 2;
				}
				else
					out[o++] = *v++;
			}
			out[o] = 0;
		}
		if (p->kind == TCL_RENAME)
		{
			/* the line is about who they are now */
			const char *t = plain + pi + pl;
			size_t tl = strlen(t);
			if (tl && tl < namesz)
				memcpy(name, t, tl + 1);
		}
		return p->kind;
	}
	return TCL_NOTE;
}

// Is there a line of this kind about this player in the last window seconds,
// with nothing about them since? (The server says some changes twice: "left the
// game (reason)" and then "disconnected"; a rename the client noticed may also
// have been announced.)
static inline int TouchChatLog_HasRecent(const tcl_log_t *l, double now, int kind, const char *name, double window)
{
	unsigned k;
	for (k = 0; k < l->count && k < 12; k++)
	{
		const tcl_entry_t *e = TouchChatLog_Get(l, l->count - 1 - k);
		if (now - e->time > window)
			break;
		if (!strcmp(e->name, name))
			return e->kind == kind;
	}
	return 0;
}

// "HH:MM:SS" of a wall clock time.
static inline void TouchChatLog_Stamp(time_t w, char *out, size_t size)
{
	struct tm tmv;
#ifdef _WIN32
	localtime_s(&tmv, &w);
#else
	localtime_r(&w, &tmv);
#endif
	TCL_SNPRINTF(out, size, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static int tcl_store(tcl_log_t *l, double now, time_t wall, int kind, const char *name, const char *text)
{
	tcl_entry_t *e = &l->e[l->head];
	size_t n = 0;
	while (*text && n + 1 < TCL_LEN)
	{
		if (*text != '\n' && *text != '\r')
			e->text[n++] = *text;
		text++;
	}
	e->text[n] = 0;
	{
		char plain[TCL_LEN];
		TouchMention_StripColors(e->text, plain, sizeof(plain));
		if (!plain[strspn(plain, " \t")])
			return 0;
	}
	e->time = now;
	e->wall = wall;
	e->kind = (unsigned char)kind;
	e->rows = 0;
	e->rowsw = 0;
	TCL_SNPRINTF(e->name, sizeof(e->name), "%s", name);
	l->head = (l->head + 1) % TCL_CAP;
	if (l->count < TCL_CAP)
		l->count++;
	return 1;
}

// Keep one line. lead is the first byte the console saw (1 a player speaking,
// 3 the game's own chat-box line; 0 unknown, and a leading control byte in
// text counts too). Colour codes stay; the newline and any chat-marker byte
// are dropped; an empty line is not a line. The game's player events are
// reworded ("<name> joined the server"); any other line is kept as printed.
static int TouchChatLog_AddLead(tcl_log_t *l, double now, time_t wall, int lead, const char *text)
{
	char name[TCL_NAME], ev[TCL_LEN];
	int kind;
	if (!lead && *text && (unsigned char)*text < 4)
		lead = *text;
	while (*text && (unsigned char)*text < 32 && *text != '\t')
		text++;
	kind = TouchChatLog_Classify(text, lead, name, sizeof(name), ev, sizeof(ev));
	if (kind != TCL_CHAT && kind != TCL_NOTE)
	{
		if (TouchChatLog_HasRecent(l, now, kind, name, 3.0))
			return 0;
		return tcl_store(l, now, wall, kind, name, ev);
	}
	return tcl_store(l, now, wall, kind, name, text);
}

static inline int TouchChatLog_Add(tcl_log_t *l, double now, time_t wall, const char *text)
{
	return TouchChatLog_AddLead(l, now, wall, 0, text);
}

// "old is now known as new" (the client works a rename out itself), names with their colours.
static int TouchChatLog_AddRename(tcl_log_t *l, double now, time_t wall, const char *oldraw, const char *newraw)
{
	char text[TCL_LEN], plain[TCL_NAME];
	TCL_SNPRINTF(text, sizeof(text), "^7%s^7 %sis now known as ^7%s", oldraw, TouchChatLog_KindColor(TCL_RENAME), newraw);
	TouchMention_StripColors(newraw, plain, sizeof(plain));
	return tcl_store(l, now, wall, TCL_RENAME, plain, text);
}

// Empty the log: no line survives, so nothing can come back through Get,
// Visible or Export, and the ring starts over.
static inline void TouchChatLog_Clear(tcl_log_t *l)
{
	l->head = 0;
	l->count = 0;
}

// How long a stock chat or notify line may stay on screen once the log was
// cleared at engine time cleared_at (<0: never cleared): only lines newer than
// the clear. The stock chat box shows a line when its age is within the window,
// and a window of 0 means "no limit", so this never returns 0. A clear stamped
// in the future (the clock restarted on a new map) no longer applies.
static inline float TouchChatLog_FadeAfterClear(float stock, double now, double cleared_at)
{
	float w;
	if (cleared_at < 0 || cleared_at > now)
		return stock;
	w = (float)(now - cleared_at) - 0.001f; // a line stamped at the clear is older than the window
	if (w < 0.0001f)
		w = 0.0001f;
	return (stock > 0 && stock < w) ? stock : w;
}

// The icons docked on the box, in a row starting at the box's left edge (the
// chat text's x), all the same size with a gap of icon_size * 0.4 between
// neighbours. Slot 0 is the right-most (share), then download, copy, clear, log
// going left; the row reads log, clear, copy, download, share.
#define TCL_ICONS 5
static inline float TouchChatLog_IconX(float x, float w, float icon_size, int slot)
{
	(void)w;
	return x + (float)(TCL_ICONS - 1 - slot) * icon_size * 1.4f;
}

// Is the point inside the square icon at ix, iy of side s?
static inline int TouchChatLog_InIcon(float px, float py, float ix, float iy, float s)
{
	return px >= ix && px <= ix + s && py >= iy && py <= iy + s;
}

// 0 is the oldest line held.
static inline const tcl_entry_t *TouchChatLog_Get(const tcl_log_t *l, unsigned i)
{
	if (i >= l->count)
		return NULL;
	return &l->e[(l->head + TCL_CAP - l->count + i) % TCL_CAP];
}

// How many of the newest lines are younger than fade seconds. Lines go in in
// time order, so this walks back from the newest and stops at the first old one.
static inline unsigned TouchChatLog_Visible(const tcl_log_t *l, double now, double fade)
{
	unsigned n = 0;
	while (n < l->count && now - TouchChatLog_Get(l, l->count - 1 - n)->time < fade)
		n++;
	return n;
}

// The scroll box over the chat area: as tall as the lines being shown, never
// below min_lines (so it can still be grabbed when everything has faded) and
// never above max_lines.
static inline int TouchChatLog_BoxLines(unsigned visible, int min_lines, int max_lines)
{
	int n = (int)visible;
	if (max_lines < min_lines)
		max_lines = min_lines;
	return n < min_lines ? min_lines : (n > max_lines ? max_lines : n);
}

// Scroll offset in lines back from the newest, kept inside the log.
static inline int TouchChatLog_ClampScroll(const tcl_log_t *l, int offset, int box_lines)
{
	int max = (int)l->count - box_lines;
	if (max < 0)
		max = 0;
	return offset < 0 ? 0 : (offset > max ? max : offset);
}

// The export: "[HH:MM:SS] text" per line, colour codes stripped. Returns the
// length written. Lines that do not fit are cut off at a line boundary.
static inline size_t TouchChatLog_Export(const tcl_log_t *l, char *out, size_t size)
{
	size_t o = 0;
	unsigned i;
	if (!size)
		return 0;
	out[0] = 0;
	for (i = 0; i < l->count; i++)
	{
		const tcl_entry_t *e = TouchChatLog_Get(l, i);
		char plain[TCL_LEN], line[TCL_LEN + 16];
		struct tm tmv;
		int n;
		time_t w = e->wall;
		TouchMention_StripColors(e->text, plain, sizeof(plain));
#ifdef _WIN32
		localtime_s(&tmv, &w);
#else
		localtime_r(&w, &tmv);
#endif
		n = TCL_SNPRINTF(line, sizeof(line), "[%02d:%02d:%02d] %s\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, plain);
		if (n < 0 || o + (size_t)n + 1 > size)
			break;
		memcpy(out + o, line, (size_t)n + 1);
		o += (size_t)n;
	}
	return o;
}


// ---------------------------------------------------------------------------
// The live feed: the newest lines of the kinds in mask that are younger than
// fade seconds.
// ---------------------------------------------------------------------------

// How many of the lines (of these kinds) are younger than fade seconds, newest
// first, up to max. idx gets their log indexes, newest first.
static inline unsigned TouchChatLog_FeedLines(const tcl_log_t *l, unsigned mask, double now, double fade, unsigned *idx, unsigned max)
{
	unsigned n = 0, i;
	for (i = l->count; i-- > 0 && n < max;)
	{
		const tcl_entry_t *e = TouchChatLog_Get(l, i);
		if (now - e->time >= fade)
			break; // lines are in time order: the rest are older
		if (mask & TCL_BIT(e->kind))
			idx[n++] = i;
	}
	return n;
}

// ---------------------------------------------------------------------------
// Scrolling: one model for a finger and a wheel. pos is how far the content is
// scrolled from its newest end, in the same units as the drawing (0 shows the
// newest line at the bottom; larger shows older lines).
// ---------------------------------------------------------------------------

typedef struct
{
	float pos;     // distance from the newest end
	float vel;     // units per second, positive towards older lines (coasting after a flick)
	float y0, last_y;
	double last_t;
	int down;      // a finger or button is on the list
	int moved;     // ... and has moved past the slop: this is a drag, not a tap
} tcl_scroller_t;

#define TCL_FLICK_MAX 6000.0f   // fastest coast, units per second
#define TCL_FRICTION 3.2f       // the coast loses this fraction of its speed per second (exponential)
#define TCL_STOP_SPEED 14.0f    // below this a coast stops

static inline float tcl_clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline void TouchChatLog_ScrollDown(tcl_scroller_t *s, float y, double t)
{
	s->down = 1;
	s->moved = 0;
	s->y0 = s->last_y = y;
	s->last_t = t;
	s->vel = 0; // a finger on the list stops a coast
}

// A finger moved to y. maxpos is the furthest the content can scroll. Returns
// 1 once this is a drag (it moved further than slop).
static inline int TouchChatLog_ScrollMove(tcl_scroller_t *s, float y, double t, float maxpos, float slop)
{
	float dy;
	double dt;
	if (!s->down)
		return 0;
	if (!s->moved && fabsf(y - s->y0) < slop)
		return 0;
	if (!s->moved)
	{
		s->moved = 1;
		s->last_y = s->y0; // the content catches up with the finger without a jump at the slop
	}
	dy = y - s->last_y;
	s->pos = tcl_clampf(s->pos + dy, 0, maxpos);
	dt = t - s->last_t;
	if (dt > 0.0005)
		s->vel = tcl_clampf(0.5f * s->vel + 0.5f * (float)(dy / dt), -TCL_FLICK_MAX, TCL_FLICK_MAX);
	s->last_y = y;
	s->last_t = t;
	return 1;
}

// The finger lifted. Returns 1 when it was a tap (it never became a drag); a
// drag that ends moving carries on as a coast, one that stopped first does not.
static inline int TouchChatLog_ScrollUp(tcl_scroller_t *s, double t)
{
	int tap = !s->moved;
	if (!s->down)
		return 0;
	s->down = 0;
	if (tap || t - s->last_t > 0.1)
		s->vel = 0;
	return tap;
}

// The wheel: notches > 0 is "up" (older lines), step units per notch.
static inline void TouchChatLog_ScrollWheel(tcl_scroller_t *s, float notches, float step, float maxpos)
{
	s->pos = tcl_clampf(s->pos + notches * step, 0, maxpos);
	s->vel = 0;
}

// Once a frame: the coast.
static inline void TouchChatLog_ScrollStep(tcl_scroller_t *s, float dt, float maxpos)
{
	if (s->down)
		return;
	if (fabsf(s->vel) < TCL_STOP_SPEED)
	{
		s->vel = 0;
		s->pos = tcl_clampf(s->pos, 0, maxpos);
		return;
	}
	s->pos += s->vel * dt;
	s->vel *= expf(-TCL_FRICTION * dt);
	if (s->pos <= 0 || s->pos >= maxpos)
	{
		s->pos = tcl_clampf(s->pos, 0, maxpos);
		s->vel = 0; // the end of the log stops it
	}
}

// ---------------------------------------------------------------------------
// The virtual window: only the lines that can be seen are laid out. A line is
// rows * rowh + pad tall. Rows are measured when a line first comes into view
// (the callback fills e->rows) and kept; lines never reached count as one row.
// ---------------------------------------------------------------------------

#define TCL_WIN_MAX 96

typedef struct
{
	unsigned idx[TCL_WIN_MAX]; // log indexes of the lines in view, newest first
	float top[TCL_WIN_MAX];    // where each line starts, from the top of the view
	float h[TCL_WIN_MAX];      // its height
	int n;
} tcl_window_t;

typedef int (*tcl_measure_fn)(void *ctx, tcl_entry_t *e); // rows the line takes (>= 1)

static inline float tcl_entry_h(const tcl_entry_t *e, float rowh, float pad)
{
	return (float)(e->rows > 0 ? e->rows : 1) * rowh + pad;
}

// The height of everything in mask: where the scroll ends.
static inline float TouchChatLog_Content(const tcl_log_t *l, unsigned mask, float rowh, float pad)
{
	float h = 0;
	unsigned i;
	for (i = 0; i < l->count; i++)
	{
		const tcl_entry_t *e = TouchChatLog_Get(l, i);
		if (mask & TCL_BIT(e->kind))
			h += tcl_entry_h(e, rowh, pad);
	}
	return h;
}

static inline void TouchChatLog_Window(tcl_log_t *l, unsigned mask, tcl_measure_fn measure, void *ctx, float rowh, float pad, float view_h, float pos, tcl_window_t *w)
{
	float acc = 0;
	unsigned i;
	w->n = 0;
	for (i = l->count; i-- > 0 && acc < pos + view_h;)
	{
		tcl_entry_t *e = (tcl_entry_t *)TouchChatLog_Get(l, i);
		float h;
		if (!(mask & TCL_BIT(e->kind)))
			continue;
		if (measure)
		{
			int r = measure(ctx, e);
			e->rows = (short)(r > 0 ? r : 1);
		}
		h = tcl_entry_h(e, rowh, pad);
		if (acc + h > pos && w->n < TCL_WIN_MAX)
		{
			w->idx[w->n] = i;
			w->h[w->n] = h;
			w->top[w->n] = view_h - (acc + h - pos);
			w->n++;
		}
		acc += h;
	}
}

// ---------------------------------------------------------------------------
// Hit targets: what a finger or a click can press. Every target is hit over at
// least min_w x min_h around its middle (a small icon is still a real target),
// and where two overlap the nearer middle wins.
// ---------------------------------------------------------------------------

typedef struct { float x, y, w, h; int id, arg; } tcl_target_t;

static inline int TouchChatLog_HitTarget(const tcl_target_t *t, int n, float px, float py, float min_w, float min_h)
{
	int i, best = -1;
	float bestd = 0;
	for (i = 0; i < n; i++)
	{
		float w = t[i].w < min_w ? min_w : t[i].w, h = t[i].h < min_h ? min_h : t[i].h;
		float cx = t[i].x + t[i].w * 0.5f, cy = t[i].y + t[i].h * 0.5f;
		float d;
		if (px < cx - w * 0.5f || px > cx + w * 0.5f || py < cy - h * 0.5f || py > cy + h * 0.5f)
			continue;
		d = (px - cx) * (px - cx) + (py - cy) * (py - cy);
		if (best < 0 || d < bestd)
		{
			best = i;
			bestd = d;
		}
	}
	return best;
}

// ---------------------------------------------------------------------------
// Icons: drawn here as pictures at exactly the pixel size they are shown at
// (white on transparent, coverage worked out from distances, so edges are
// clean at any density), never a bitmap stretched to fit.
// ---------------------------------------------------------------------------

enum
{
	TCL_I_CHAT, TCL_I_JOIN, TCL_I_PLAY, TCL_I_SPEC, TCL_I_LEAVE, TCL_I_RENAME, TCL_I_NOTE,
	TCL_I_CLEAR, TCL_I_COPY, TCL_I_DOWNLOAD, TCL_I_SHARE, TCL_I_LOG, TCL_I_CLOSE, TCL_I_LATEST,
	TCL_I_COUNT
};

static inline int TouchChatLog_KindIcon(int kind)
{
	static const unsigned char m[TCL_NKINDS] = {TCL_I_CHAT, TCL_I_JOIN, TCL_I_PLAY, TCL_I_SPEC, TCL_I_LEAVE, TCL_I_RENAME, TCL_I_NOTE};
	return kind >= 0 && kind < TCL_NKINDS ? m[kind] : TCL_I_NOTE;
}

static inline float tcl_seg(float px, float py, float ax, float ay, float bx, float by)
{
	float dx = bx - ax, dy = by - ay, t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy + 1e-9f);
	float cx, cy;
	t = tcl_clampf(t, 0, 1);
	cx = ax + dx * t - px;
	cy = ay + dy * t - py;
	return sqrtf(cx * cx + cy * cy);
}
static inline float tcl_ring(float px, float py, float cx, float cy, float r)
{
	return fabsf(sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - r);
}
static inline float tcl_disc(float px, float py, float cx, float cy, float r)
{
	return sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - r; // negative inside: a filled shape
}
static inline float tcl_boxring(float px, float py, float cx, float cy, float hx, float hy, float rad)
{
	float qx = fabsf(px - cx) - hx + rad, qy = fabsf(py - cy) - hy + rad;
	float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
	float m = qx > qy ? qx : qy;
	float sd = sqrtf(ox * ox + oy * oy) + (m < 0 ? m : 0) - rad;
	return fabsf(sd);
}
static inline float tcl_min(float a, float b) { return a < b ? a : b; }
// Is the point inside the triangle (filled)? distance-like: negative inside.
static inline float tcl_tri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy)
{
	float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
	float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
	float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
	int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
	float e = tcl_min(tcl_seg(px, py, ax, ay, bx, by), tcl_min(tcl_seg(px, py, bx, by, cx, cy), tcl_seg(px, py, cx, cy, ax, ay)));
	return (neg && pos) ? e : -e;
}

// The signed distance (unit square, 0..1) from the point to the icon's ink:
// negative or zero inside a stroke, positive outside. Strokes are 0.1 wide.
static inline float tcl_icon_d(int icon, float x, float y)
{
	const float r = 0.055f;
	float d = 9;
#define S(ax, ay, bx, by) d = tcl_min(d, tcl_seg(x, y, ax, ay, bx, by) - r)
	switch (icon)
	{
	case TCL_I_CHAT:
		d = tcl_boxring(x, y, 0.5f, 0.44f, 0.36f, 0.26f, 0.12f) - r;
		S(0.3f, 0.7f, 0.26f, 0.88f);
		S(0.26f, 0.88f, 0.5f, 0.7f);
		break;
	case TCL_I_JOIN: /* a plus in a ring */
		d = tcl_ring(x, y, 0.5f, 0.5f, 0.38f) - r;
		S(0.5f, 0.3f, 0.5f, 0.7f);
		S(0.3f, 0.5f, 0.7f, 0.5f);
		break;
	case TCL_I_PLAY: /* a triangle */
		d = tcl_tri(x, y, 0.3f, 0.2f, 0.3f, 0.8f, 0.8f, 0.5f);
		break;
	case TCL_I_SPEC: /* an eye */
	{
		float ex = (x - 0.5f) / 0.42f, ey = (y - 0.5f) / 0.26f;
		d = fabsf(sqrtf(ex * ex + ey * ey) - 1.0f) * 0.26f - r;
		d = tcl_min(d, tcl_disc(x, y, 0.5f, 0.5f, 0.12f));
		break;
	}
	case TCL_I_LEAVE: /* an arrow out of a doorway */
		S(0.42f, 0.2f, 0.2f, 0.2f);
		S(0.2f, 0.2f, 0.2f, 0.8f);
		S(0.2f, 0.8f, 0.42f, 0.8f);
		S(0.38f, 0.5f, 0.82f, 0.5f);
		S(0.64f, 0.32f, 0.82f, 0.5f);
		S(0.64f, 0.68f, 0.82f, 0.5f);
		break;
	case TCL_I_RENAME: /* a pencil over a line */
		S(0.3f, 0.66f, 0.68f, 0.28f);
		S(0.3f, 0.66f, 0.24f, 0.76f);
		S(0.24f, 0.76f, 0.34f, 0.7f);
		S(0.2f, 0.88f, 0.8f, 0.88f);
		d = tcl_min(d, tcl_seg(x, y, 0.34f, 0.62f, 0.64f, 0.32f) - 0.1f);
		break;
	case TCL_I_NOTE: /* an i in a ring */
		d = tcl_ring(x, y, 0.5f, 0.5f, 0.38f) - r;
		d = tcl_min(d, tcl_disc(x, y, 0.5f, 0.32f, 0.065f));
		S(0.5f, 0.46f, 0.5f, 0.7f);
		break;
	case TCL_I_CLEAR: /* a bin */
		S(0.2f, 0.28f, 0.8f, 0.28f);
		S(0.4f, 0.18f, 0.6f, 0.18f);
		S(0.4f, 0.18f, 0.4f, 0.28f);
		S(0.6f, 0.18f, 0.6f, 0.28f);
		S(0.28f, 0.28f, 0.32f, 0.84f);
		S(0.32f, 0.84f, 0.68f, 0.84f);
		S(0.68f, 0.84f, 0.72f, 0.28f);
		S(0.5f, 0.42f, 0.5f, 0.7f);
		break;
	case TCL_I_COPY: /* two pages */
		d = tcl_boxring(x, y, 0.42f, 0.42f, 0.2f, 0.22f, 0.04f) - r;
		d = tcl_min(d, tcl_boxring(x, y, 0.58f, 0.58f, 0.2f, 0.22f, 0.04f) - r);
		break;
	case TCL_I_DOWNLOAD: /* an arrow into a tray */
		S(0.5f, 0.16f, 0.5f, 0.6f);
		S(0.3f, 0.42f, 0.5f, 0.62f);
		S(0.7f, 0.42f, 0.5f, 0.62f);
		S(0.22f, 0.72f, 0.22f, 0.84f);
		S(0.22f, 0.84f, 0.78f, 0.84f);
		S(0.78f, 0.84f, 0.78f, 0.72f);
		break;
	case TCL_I_SHARE: /* three linked dots */
		d = tcl_disc(x, y, 0.26f, 0.5f, 0.11f);
		d = tcl_min(d, tcl_disc(x, y, 0.74f, 0.24f, 0.11f));
		d = tcl_min(d, tcl_disc(x, y, 0.74f, 0.76f, 0.11f));
		S(0.26f, 0.5f, 0.74f, 0.24f);
		S(0.26f, 0.5f, 0.74f, 0.76f);
		break;
	case TCL_I_LOG: /* a list */
		d = tcl_min(d, tcl_disc(x, y, 0.22f, 0.3f, 0.06f));
		d = tcl_min(d, tcl_disc(x, y, 0.22f, 0.5f, 0.06f));
		d = tcl_min(d, tcl_disc(x, y, 0.22f, 0.7f, 0.06f));
		S(0.38f, 0.3f, 0.8f, 0.3f);
		S(0.38f, 0.5f, 0.8f, 0.5f);
		S(0.38f, 0.7f, 0.8f, 0.7f);
		break;
	case TCL_I_CLOSE:
		S(0.26f, 0.26f, 0.74f, 0.74f);
		S(0.74f, 0.26f, 0.26f, 0.74f);
		break;
	case TCL_I_LATEST: /* a double chevron down */
		S(0.28f, 0.28f, 0.5f, 0.5f);
		S(0.72f, 0.28f, 0.5f, 0.5f);
		S(0.28f, 0.5f, 0.5f, 0.72f);
		S(0.72f, 0.5f, 0.5f, 0.72f);
		break;
	}
#undef S
	return d;
}

// Fills rgba (px * px * 4 bytes): white with the coverage in alpha.
static void TouchChatLog_IconPixels(int icon, int px, unsigned char *rgba)
{
	int ix, iy;
	for (iy = 0; iy < px; iy++)
		for (ix = 0; ix < px; ix++)
		{
			// a pixel is 1/px wide in the unit square: the distance in pixels is the edge's coverage
			float d = tcl_icon_d(icon, ((float)ix + 0.5f) / (float)px, ((float)iy + 0.5f) / (float)px) * (float)px;
			float a = tcl_clampf(0.5f - d, 0, 1);
			unsigned char *o = rgba + ((size_t)iy * (size_t)px + (size_t)ix) * 4;
			o[0] = o[1] = o[2] = 255;
			o[3] = (unsigned char)(a * 255.0f + 0.5f);
		}
}


// The toolbar strip of the chat feed. The box is [bottom - areah, bottom]; the strip goes
// above it, or, when there is no room above (the box is at the top of the screen), is a header
// row inside it and the lines get what is left. Out: the strip's top (tb_y) and height
// (tb_h), and the box's lines area [lines_top, bottom]. The strip and the lines never overlap.
typedef struct { float tb_y, tb_h, lines_top, lines_h; } tcl_strip_t;
static inline void TouchChatLog_Strip(float bottom, float areah, float isz, float fs, float rowh, tcl_strip_t *o)
{
	float top = bottom - areah;
	o->tb_h = isz * 1.24f;
	if (top < o->tb_h + fs * 0.5f)
	{
		o->tb_y = top < 0 ? 0 : top;
		o->lines_top = o->tb_y + o->tb_h + fs * 0.25f;
		if (bottom - o->lines_top < rowh)
			o->lines_top = bottom - rowh; /* tiny box: the lines win a row, the strip stays above them */
		if (o->lines_top - o->tb_h < 0)
			o->tb_y = 0;
	}
	else
	{
		o->tb_y = top - o->tb_h - fs * 0.25f;
		o->lines_top = top;
	}
	o->lines_h = bottom - o->lines_top;
}

#endif

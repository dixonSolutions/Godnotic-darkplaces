#ifndef TOUCH_CHATLOG_LOGIC_H
#define TOUCH_CHATLOG_LOGIC_H

/*
Godnotic chat log: the pure pieces of touch_chatlog.c (ring buffer, fade,
scroll box size, text export), free of engine state so
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

#define TCL_CAP 256
#define TCL_LEN 256

typedef struct
{
	double time;  // engine clock, for the fade
	time_t wall;  // wall clock, for the export
	char text[TCL_LEN];
} tcl_entry_t;

typedef struct
{
	tcl_entry_t e[TCL_CAP];
	unsigned head;  // next slot to write
	unsigned count; // lines held, at most TCL_CAP
} tcl_log_t;

// Keep one line (colour codes and all). The newline and any chat-marker byte
// are dropped; an empty line is not a line.
static inline int TouchChatLog_Add(tcl_log_t *l, double now, time_t wall, const char *text)
{
	tcl_entry_t *e;
	size_t n = 0;
	while (*text && (unsigned char)*text < 32 && *text != '\t')
		text++;
	e = &l->e[l->head];
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
	l->head = (l->head + 1) % TCL_CAP;
	if (l->count < TCL_CAP)
		l->count++;
	return 1;
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
// neighbours. Slot 0 is the right-most (share), then download, copy, clear
// going left; the row reads clear, copy, download, share.
#define TCL_ICONS 4
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

// ---------------------------------------------------------------------------
// Icons: shown only while the log holds a line (so not before the first
// message, and not after Clear).
static inline int TouchChatLog_IconsShown(const tcl_log_t *l)
{
	return l->count > 0;
}

// ---------------------------------------------------------------------------
// Pixel scroll. offset is how far the content has been pulled down from the
// live state, in screen units (0 is the newest line sitting on the box's last
// row); it is a float, so a drag moves smoothly and not a line at a time.

// The furthest it can go: the oldest line at the top of the box.
static inline float TouchChatLog_MaxPx(const tcl_log_t *l, int box_lines, float line_h)
{
	float m = (float)((int)l->count - box_lines) * line_h;
	return m < 0 ? 0 : m;
}

static inline float TouchChatLog_ClampPx(const tcl_log_t *l, float off, int box_lines, float line_h)
{
	float m = TouchChatLog_MaxPx(l, box_lines, line_h);
	return off < 0 ? 0 : (off > m ? m : off);
}

// A new line arrived. Pinned to the bottom (off at the live state) it stays
// there and the new line shows; scrolled up, the offset grows by one line so the
// lines being read stay where they are under the finger. The QuakeC chat sheet
// applies the same rule to its own backlog.
static inline float TouchChatLog_PinAdjust(float off, float line_h)
{
	return off > 0.5f ? off + line_h : 0.0f;
}

// Which rows to draw for an offset. Row m (mfirst..mlast) shows line j0 + m
// (0 is the oldest) with its top at box_y + m * line_h + frac; only rows that
// exist and touch the box are listed, at most box_lines + 1. mlast < mfirst
// means nothing to draw.
typedef struct
{
	int j0, mfirst, mlast;
	float frac;
} tcl_window_t;

static inline tcl_window_t TouchChatLog_Window(unsigned count, int box_lines, float off, float line_h)
{
	tcl_window_t w;
	int whole, mlo, mhi;
	if (off < 0)
		off = 0;
	whole = line_h > 0 ? (int)floorf(off / line_h) : 0;
	w.frac = line_h > 0 ? off - (float)whole * line_h : 0;
	w.j0 = (int)count - box_lines - whole;
	mlo = w.frac > 0.001f ? -1 : 0;
	mhi = box_lines - 1;
	w.mfirst = mlo;
	w.mlast = mhi;
	if (w.j0 + w.mfirst < 0)
		w.mfirst = -w.j0;
	if (w.j0 + w.mlast > (int)count - 1)
		w.mlast = (int)count - 1 - w.j0;
	return w;
}

// Momentum: one step of dt seconds. vel is in screen units per second and
// decays; stops (returns 0) when slow or when an end of the log is reached.
static inline int TouchChatLog_Coast(float *off, float *vel, float dt, float max_off)
{
	float v = *vel, o = *off + v * dt;
	if (fabsf(v) < 4.0f)
	{
		*vel = 0;
		return 0;
	}
	if (o <= 0 || o >= max_off)
	{
		*off = o <= 0 ? 0 : max_off;
		*vel = 0;
		return 0;
	}
	*off = o;
	*vel = v * expf(-3.5f * dt);
	return 1;
}

// ---------------------------------------------------------------------------
// Icon art, rasterised at the screen's real pixel size so it is crisp: each
// pixel's coverage comes from its distance to the strokes (a capsule per
// stroke, a disc being a zero-length one), antialiased over one pixel. Output
// is BGRA, size x size: a dark rounded plate with the white glyph on it.
// Slot as TouchChatLog_IconX: 0 share, 1 download, 2 copy, 3 clear.
typedef struct { float x0, y0, x1, y1, r; } tcl_stroke_t; // r < 0: the line width

static const tcl_stroke_t tcl_art_share[] = {
	{.30f,.50f,.30f,.50f,.105f}, {.70f,.25f,.70f,.25f,.105f}, {.70f,.75f,.70f,.75f,.105f},
	{.30f,.50f,.70f,.25f,-1}, {.30f,.50f,.70f,.75f,-1}};
static const tcl_stroke_t tcl_art_download[] = {
	{.50f,.18f,.50f,.60f,-1}, {.31f,.43f,.50f,.62f,-1}, {.69f,.43f,.50f,.62f,-1},
	{.24f,.62f,.24f,.77f,-1}, {.24f,.77f,.76f,.77f,-1}, {.76f,.77f,.76f,.62f,-1}};
static const tcl_stroke_t tcl_art_copy[] = {
	{.52f,.22f,.24f,.22f,-1}, {.24f,.22f,.24f,.62f,-1}, {.24f,.62f,.36f,.62f,-1},
	{.40f,.36f,.76f,.36f,-1}, {.76f,.36f,.76f,.80f,-1}, {.76f,.80f,.40f,.80f,-1}, {.40f,.80f,.40f,.36f,-1}};
static const tcl_stroke_t tcl_art_clear[] = {
	{.40f,.15f,.60f,.15f,-1}, {.20f,.26f,.80f,.26f,-1},
	{.29f,.34f,.33f,.79f,-1}, {.71f,.34f,.67f,.79f,-1}, {.33f,.79f,.67f,.79f,-1},
	{.45f,.44f,.45f,.68f,-1}, {.55f,.44f,.55f,.68f,-1}};

static inline float TouchChatLog_SegDist(float px, float py, const tcl_stroke_t *s)
{
	float dx = s->x1 - s->x0, dy = s->y1 - s->y0, t = 0, ex, ey;
	float l2 = dx * dx + dy * dy;
	if (l2 > 0)
	{
		t = ((px - s->x0) * dx + (py - s->y0) * dy) / l2;
		t = t < 0 ? 0 : (t > 1 ? 1 : t);
	}
	ex = px - (s->x0 + t * dx);
	ey = py - (s->y0 + t * dy);
	return sqrtf(ex * ex + ey * ey);
}

static inline void TouchChatLog_IconRaster(int slot, int size, unsigned char *bgra)
{
	const tcl_stroke_t *art;
	int n, x, y, i;
	float stroke, corner = 0.2f;
	switch (slot)
	{
	case 0: art = tcl_art_share; n = (int)(sizeof(tcl_art_share) / sizeof(*tcl_art_share)); break;
	case 1: art = tcl_art_download; n = (int)(sizeof(tcl_art_download) / sizeof(*tcl_art_download)); break;
	case 2: art = tcl_art_copy; n = (int)(sizeof(tcl_art_copy) / sizeof(*tcl_art_copy)); break;
	default: art = tcl_art_clear; n = (int)(sizeof(tcl_art_clear) / sizeof(*tcl_art_clear)); break;
	}
	if (size < 2)
		return;
	// at least 1.6 device pixels wide, 7% of the icon when it is large
	stroke = 0.07f > 1.6f / (float)size ? 0.07f : 1.6f / (float)size;
	for (y = 0; y < size; y++)
		for (x = 0; x < size; x++)
		{
			float px = ((float)x + 0.5f) / (float)size, py = ((float)y + 0.5f) / (float)size;
			float cov = 0, pc, qx, qy, d, ga, pa, oa;
			unsigned char *o = bgra + ((size_t)y * (size_t)size + (size_t)x) * 4;
			for (i = 0; i < n; i++)
			{
				float r = art[i].r < 0 ? stroke * 0.5f : art[i].r;
				float c = 0.5f + (r - TouchChatLog_SegDist(px, py, &art[i])) * (float)size;
				c = c < 0 ? 0 : (c > 1 ? 1 : c);
				if (c > cov)
					cov = c;
			}
			// rounded plate: signed distance to a rounded square
			qx = fabsf(px - 0.5f) - (0.5f - corner);
			qy = fabsf(py - 0.5f) - (0.5f - corner);
			d = sqrtf((qx > 0 ? qx : 0) * (qx > 0 ? qx : 0) + (qy > 0 ? qy : 0) * (qy > 0 ? qy : 0))
				+ ((qx > qy ? qx : qy) < 0 ? (qx > qy ? qx : qy) : 0) - corner;
			pc = 0.5f - d * (float)size;
			pc = pc < 0 ? 0 : (pc > 1 ? 1 : pc);
			ga = cov;
			pa = pc * 0.6f;
			oa = ga + pa * (1 - ga);
			if (oa <= 0)
			{
				o[0] = o[1] = o[2] = o[3] = 0;
				continue;
			}
			{
				float pr = 5.0f, pg = 10.0f, pb = 20.0f;
				float w = 255.0f * ga, k = pa * (1 - ga);
				o[0] = (unsigned char)((w + pb * k) / oa + 0.5f);
				o[1] = (unsigned char)((w + pg * k) / oa + 0.5f);
				o[2] = (unsigned char)((w + pr * k) / oa + 0.5f);
				o[3] = (unsigned char)(oa * 255.0f + 0.5f);
			}
		}
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

#endif

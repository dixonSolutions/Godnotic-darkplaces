#ifndef TOUCH_CHATLOG_LOGIC_H
#define TOUCH_CHATLOG_LOGIC_H

/*
Godnotic chat log: the pure pieces of touch_chatlog.c (ring buffer, fade,
scroll box size, text export), free of engine state so
tests/mention/chatlog_test.c can check them with a plain C compiler.
*/

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

#endif

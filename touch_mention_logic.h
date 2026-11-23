#ifndef TOUCH_MENTION_LOGIC_H
#define TOUCH_MENTION_LOGIC_H

/*
Godnotic chat @mentions and the "is typing" line: the pure pieces of
touch_mention.c, kept free of engine state so tests/mention/mention_test.c can
check them with a plain C compiler.

Nothing here talks to a server. The picker edits the local chat line; what is
sent is the finished text, as before.
*/

#include <stddef.h>
#include <string.h>

#define TM_MAX_NAME 64
#define TM_MAX_SHOWN 6

// A name without colour codes: ^0-^9, ^xRGB, and ^^ for a literal ^. A ^ that
// starts no code is kept, as the engine draws it.
static inline void TouchMention_StripColors(const char *in, char *out, size_t outsize)
{
	size_t o = 0;
	if (!outsize)
		return;
	while (*in && o + 1 < outsize)
	{
		if (in[0] == '^')
		{
			if (in[1] >= '0' && in[1] <= '9')
			{
				in += 2;
				continue;
			}
			if ((in[1] == 'x' || in[1] == 'X')
				&& strspn(in + 2, "0123456789abcdefABCDEF") >= 3)
			{
				in += 5;
				continue;
			}
			if (in[1] == '^')
				in++; // "^^" is one ^
		}
		out[o++] = *in++;
	}
	out[o] = 0;
}

static inline char TouchMention_Lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static inline int TouchMention_IsWordChar(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		|| c == '_' || (c & 0x80);
}

// The "@query" being typed at the cursor. The @ has to start the line or follow
// a space, so an address (a@b.c) is not a mention. Returns the index of the @,
// or -1. The query is text[@+1 .. cursor), spaces allowed (names have them);
// it is the caller's job to drop the picker when nothing matches it.
static inline int TouchMention_Token(const char *text, int cursor)
{
	int i;
	if (cursor < 1 || cursor > (int)strlen(text))
		return -1;
	for (i = cursor - 1; i >= 0; i--)
	{
		if (text[i] == '@')
			return (i == 0 || text[i - 1] == ' ') ? i : -1;
		if (cursor - i > TM_MAX_NAME)
			return -1;
	}
	return -1;
}

// How well a name fits a query (lower case compare): 2 prefix, 1 anywhere, 0 no.
static inline int TouchMention_Score(const char *name, const char *query, size_t qlen)
{
	size_t n = strlen(name), i, j;
	if (!qlen)
		return 2;
	for (i = 0; i + qlen <= n; i++)
	{
		for (j = 0; j < qlen; j++)
			if (TouchMention_Lower(name[i + j]) != TouchMention_Lower(query[j]))
				break;
		if (j == qlen)
			return i == 0 ? 2 : 1;
	}
	return 0;
}

// Fills out[] with the indexes of names[] that fit, prefix matches first, each
// group in roster order. Returns how many (at most max).
static inline int TouchMention_Filter(const char *const *names, int count,
	const char *query, size_t qlen, int *out, int max)
{
	int n = 0, pass, i;
	for (pass = 2; pass >= 1; pass--)
		for (i = 0; i < count && n < max; i++)
			if (names[i][0] && TouchMention_Score(names[i], query, qlen) == pass)
				out[n++] = i;
	return n;
}

// Replace "@query" (from the @ to the cursor) with "@name ". Returns the new
// cursor, or -1 when it would not fit in size bytes.
static inline int TouchMention_Insert(char *text, size_t size, int at, int cursor,
	const char *name)
{
	size_t len = strlen(text), nlen = strlen(name);
	size_t tail = len - (size_t)cursor;
	size_t newlen = (size_t)at + 1 + nlen + 1 + tail;
	if (at < 0 || cursor < at + 1 || (size_t)cursor > len || newlen + 1 > size)
		return -1;
	memmove(text + at + 1 + nlen + 1, text + cursor, tail + 1);
	text[at] = '@';
	memcpy(text + at + 1, name, nlen);
	text[at + 1 + nlen] = ' ';
	return (int)(at + 1 + nlen + 1);
}

// Does msg mention name: "@name" in any case, not followed by more of a word,
// so @bob does not light up for bobby. Colour codes in msg are not looked
// through: the sender typed plain text.
static inline int TouchMention_Mentions(const char *msg, const char *name)
{
	size_t nlen = strlen(name), i, j;
	if (!nlen)
		return 0;
	for (i = 0; msg[i]; i++)
	{
		if (msg[i] != '@' || (i > 0 && TouchMention_IsWordChar(msg[i - 1])))
			continue;
		for (j = 0; j < nlen; j++)
			if (TouchMention_Lower(msg[i + 1 + j]) != TouchMention_Lower(name[j]))
				break;
		if (j == nlen && !TouchMention_IsWordChar(msg[i + 1 + nlen]))
			return 1;
	}
	return 0;
}

// "A is typing…", "A, B are typing…", "A, B and 2 others are typing…".
static inline void TouchMention_TypingText(const char *const *names, int count,
	char *out, size_t size)
{
	size_t o = 0;
	int i, shown = count <= 3 ? count : 2;
	out[0] = 0;
	if (count <= 0 || size < 8)
		return;
	for (i = 0; i < shown; i++)
	{
		size_t l = strlen(names[i]);
		if (i && o + 2 < size)
		{
			out[o++] = ',';
			out[o++] = ' ';
		}
		if (o + l + 1 >= size)
			l = size - o - 1;
		memcpy(out + o, names[i], l);
		o += l;
	}
	out[o] = 0;
	if (count > 3)
	{
		char more[40];
		int k = count - 2, d = 0;
		char digits[12];
		size_t m = 0;
		static const char tail[] = " others are typing\xE2\x80\xA6";
		while (k > 0 && d < 11) { digits[d++] = (char)('0' + k % 10); k /= 10; }
		memcpy(more, " and ", 5);
		m = 5;
		while (d > 0) more[m++] = digits[--d];
		more[m] = 0;
		if (o + m + sizeof(tail) < size)
		{
			memcpy(out + o, more, m + 1);
			o += m;
			memcpy(out + o, tail, sizeof(tail));
		}
		return;
	}
	{
		static const char one[] = " is typing\xE2\x80\xA6", many[] = " are typing\xE2\x80\xA6";
		const char *t = (count == 1) ? one : many;
		size_t tl = strlen(t);
		if (o + tl + 1 <= size)
			memcpy(out + o, t, tl + 1);
	}
}

// The speaker of a printed chat line ("name^7: text" or team chat "(team) name: text"),
// colour codes stripped. 1 and the name in out, or 0 when the line has no speaker.
static inline int TouchMention_ChatName(const char *line, char *out, size_t size)
{
	char plain[256];
	const char *start, *colon;
	size_t l;
	TouchMention_StripColors(line, plain, sizeof(plain));
	start = plain;
	while (*start && (unsigned char)*start < 32)
		start++;
	if (*start == '(')
	{
		const char *close = strstr(start, ") ");
		if (close)
			start = close + 2;
	}
	colon = strstr(start, ": ");
	if (!colon || colon == start)
		return 0;
	l = (size_t)(colon - start);
	if (l >= size || l > 40)
		return 0;
	memcpy(out, start, l);
	out[l] = 0;
	return 1;
}

#endif

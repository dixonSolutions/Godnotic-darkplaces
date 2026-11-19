/*
Godnotic: the vote lines a Xonotic server prints (server/command/vote.qc),
read back into events for touch_vote.c. Pure C, no engine state, so
scripts/test-touch-vote.sh can check it without a build.

Every server prints these whatever CSQC it sends, in English (the strings are
not translated), so they are how the engine learns of a vote when the CSQC's
own vote globals cannot be read. They carry who called the vote and for what,
how it ended, the final counts, and this player's own answer; not the live
counts, which only the CSQC's vote entity has.
*/
#ifndef TOUCH_VOTE_PARSE_H
#define TOUCH_VOTE_PARSE_H

#include <stdio.h>
#include <string.h>

enum
{
	TOUCHVOTE_MSG_NONE,
	TOUCHVOTE_MSG_CALL,        // "* A calls a vote for X": caller, text
	TOUCHVOTE_MSG_ACCEPTED,    // "* A's vote for X was accepted"
	TOUCHVOTE_MSG_REJECTED,    // "... was rejected"
	TOUCHVOTE_MSG_TIMEOUT,     // "... timed out"
	TOUCHVOTE_MSG_STOPPED,     // stopped by its caller or another player, or canceled (map change)
	TOUCHVOTE_MSG_RESULTS,     // "* vote results: 1:0 (1 needed), 0 didn't care, 1 didn't vote"
	TOUCHVOTE_MSG_MINE,        // "You accepted the vote." etc.: choice
	TOUCHVOTE_MSG_ALREADY,     // "You have already voted.": the server keeps the first answer
	TOUCHVOTE_MSG_NOVOTE       // "No vote called."
};

// Choices, as the server's vote_selection (and the CSQC's vote_highlighted).
#define TOUCHVOTE_YES      1
#define TOUCHVOTE_NO      -1
#define TOUCHVOTE_ABSTAIN -2

#define TOUCHVOTE_NAME_LEN 64
#define TOUCHVOTE_TEXT_LEN 192

typedef struct touchvote_msg_s
{
	int kind;
	char caller[TOUCHVOTE_NAME_LEN];
	char text[TOUCHVOTE_TEXT_LEN];
	int yes, no, abstain, notvoted, needed; // RESULTS; needed -1 when not printed
	int choice;                             // MINE
} touchvote_msg_t;

// Copy without Xonotic colour codes (^0..^9, ^xRGB; ^^ is a caret) and
// control characters (the \1 that asks for the chat sound), trimmed.
static void TouchVote_StripColors(const char *in, char *out, size_t outsize)
{
	size_t n = 0;
	if (!outsize)
		return;
	while (*in && n + 1 < outsize)
	{
		if (in[0] == '^')
		{
			if (in[1] >= '0' && in[1] <= '9')
			{
				in += 2;
				continue;
			}
			if (in[1] == 'x' && in[2] && in[3] && in[4]
				&& strchr("0123456789abcdefABCDEF", in[2]) && strchr("0123456789abcdefABCDEF", in[3]) && strchr("0123456789abcdefABCDEF", in[4]))
			{
				in += 5;
				continue;
			}
			if (in[1] == '^')
			{
				out[n++] = '^';
				in += 2;
				continue;
			}
		}
		if ((unsigned char)*in >= 32)
			out[n++] = *in;
		in++;
	}
	out[n] = 0;
	while (n > 0 && out[n - 1] == ' ')
		out[--n] = 0;
	if (out[0] == ' ')
	{
		size_t skip = 0;
		while (out[skip] == ' ')
			skip++;
		memmove(out, out + skip, n - skip + 1);
	}
}

static void touchvote_copy(char *dst, size_t dstsize, const char *src, size_t len)
{
	if (!dstsize)
		return;
	if (len >= dstsize)
		len = dstsize - 1;
	memcpy(dst, src, len);
	dst[len] = 0;
}

static int touchvote_ends_with(const char *s, const char *tail)
{
	size_t a = strlen(s), b = strlen(tail);
	return a >= b && !strcmp(s + a - b, tail);
}

// Server-provided choices are data, never console syntax.  Reject punctuation
// that could change the command sent when a row is selected.
#define TOUCHVOTE_LIST_MAX 512
#define TOUCHVOTE_ITEM_LEN 64
typedef struct touchvote_list_s
{
	int count;
	char item[TOUCHVOTE_LIST_MAX][TOUCHVOTE_ITEM_LEN];
} touchvote_list_t;

static int TouchVote_SafeToken(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	if (!*p) return 0;
	for (; *p; ++p)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
			return 0;
	return 1;
}

// Recognize the unmodified Xonotic help and lsmaps replies, after colour
// stripping. The asterisk on new maps is a display marker, not part of its ID.
static int TouchVote_ParseList(const char *raw, touchvote_list_t *out, int maps)
{
	char line[16384], *p, *end;
	const char *prefix = maps ? "Maps available (" : "You can call a vote for or execute these commands: ";
	TouchVote_StripColors(raw, line, sizeof(line));
	if (strncmp(line, prefix, strlen(prefix))) return 0;
	p = maps ? strstr(line, ": ") : line + strlen(prefix);
	if (!p) return 0;
	if (maps) p += 2;
	else if ((end = strstr(p, " and maybe further arguments"))) *end = 0;
	out->count = 0;
	while (*p && out->count < TOUCHVOTE_LIST_MAX)
	{
		char *start;
		while (*p == ' ') ++p;
		if (!*p || *p == '(') break;
		start = p;
		while (*p && *p != ' ') ++p;
		if (*p) *p++ = 0;
		if (maps && *start == '*') ++start;
		if (strlen(start) < TOUCHVOTE_ITEM_LEN && TouchVote_SafeToken(start))
			touchvote_copy(out->item[out->count++], TOUCHVOTE_ITEM_LEN, start, strlen(start));
	}
	return 1;
}

// A called vote remains visible regardless of the player's own choice. Match-end
// map and gametype voting have no input here and cannot create a called vote.
static int TouchVote_CalledVisible(int globals_available, int called, int printed, double print_age)
{
	if (globals_available)
		return called || (printed && print_age >= 0 && print_age < 5.0);
	return printed && print_age >= 0 && print_age <= 300.0;
}

// One line of a server print (no newline). Returns the kind and fills m.
static int TouchVote_ParseLine(const char *raw, touchvote_msg_t *m)
{
	char line[512];
	const char *p, *q;
	memset(m, 0, sizeof(*m));
	m->needed = -1;
	TouchVote_StripColors(raw, line, sizeof(line));

	// print_to answers to this player's own vote commands
	if (!strcmp(line, "You accepted the vote."))
		m->choice = TOUCHVOTE_YES;
	else if (!strcmp(line, "You rejected the vote."))
		m->choice = TOUCHVOTE_NO;
	else if (!strcmp(line, "You abstained from your vote."))
		m->choice = TOUCHVOTE_ABSTAIN;
	if (m->choice)
		return m->kind = TOUCHVOTE_MSG_MINE;
	if (!strcmp(line, "You have already voted."))
		return m->kind = TOUCHVOTE_MSG_ALREADY;
	if (!strcmp(line, "No vote called."))
		return m->kind = TOUCHVOTE_MSG_NOVOTE;

	// A status reply also recovers a vote that started before we joined.
	if (!strncmp(line, "Vote for ", 9) && (q = strstr(line + 9, " called by ")))
	{
		const char *caller = q + strlen(" called by ");
		size_t n = strlen(caller);
		if (n && caller[n - 1] == '.') n--;
		touchvote_copy(m->text, sizeof(m->text), line + 9, q - (line + 9));
		touchvote_copy(m->caller, sizeof(m->caller), caller, n);
		return m->kind = TOUCHVOTE_MSG_CALL;
	}

	// bprint lines all start "* "
	if (line[0] != '*' || line[1] != ' ')
		return m->kind = TOUCHVOTE_MSG_NONE;
	p = line + 2;

	if (!strncmp(p, "vote results: ", 14))
	{
		const char *r = p + 14;
		if (sscanf(r, "%d:%d", &m->yes, &m->no) != 2)
			return m->kind = TOUCHVOTE_MSG_NONE;
		q = strstr(r, " needed)");
		if (q && (r = strchr(r, '(')) && r < q)
			sscanf(r + 1, "%d", &m->needed);
		q = strstr(p, ", ");
		if (q)
			sscanf(q + 2, "%d", &m->abstain);
		if (q && (q = strstr(q + 2, ", ")))
			sscanf(q + 2, "%d", &m->notvoted);
		return m->kind = TOUCHVOTE_MSG_RESULTS;
	}

	if ((q = strstr(p, " calls a vote for ")))
	{
		touchvote_copy(m->caller, sizeof(m->caller), p, q - p);
		q += strlen(" calls a vote for ");
		touchvote_copy(m->text, sizeof(m->text), q, strlen(q));
		return m->kind = TOUCHVOTE_MSG_CALL;
	}
	if ((q = strstr(p, " calls a vote to become master")))
	{
		touchvote_copy(m->caller, sizeof(m->caller), p, q - p);
		touchvote_copy(m->text, sizeof(m->text), "master", 6);
		return m->kind = TOUCHVOTE_MSG_CALL;
	}

	if (touchvote_ends_with(p, " was accepted"))
		m->kind = TOUCHVOTE_MSG_ACCEPTED;
	else if (touchvote_ends_with(p, " was rejected"))
		m->kind = TOUCHVOTE_MSG_REJECTED;
	else if (touchvote_ends_with(p, " timed out"))
		m->kind = TOUCHVOTE_MSG_TIMEOUT;
	if (m->kind)
	{
		size_t end = strlen(p) - (m->kind == TOUCHVOTE_MSG_TIMEOUT ? strlen(" timed out") : strlen(" was accepted"));
		q = strstr(p, "'s vote for ");
		if (!q || (size_t)(q - p) > end)
			return m->kind = TOUCHVOTE_MSG_NONE;
		touchvote_copy(m->caller, sizeof(m->caller), p, q - p);
		q += strlen("'s vote for ");
		touchvote_copy(m->text, sizeof(m->text), q, end - (q - p));
		return m->kind;
	}

	// VoteStop: canceled by the game (a map change), stopped by its caller,
	// or stopped by someone else (a vote master, an admin)
	if (touchvote_ends_with(p, "'s vote was canceled"))
	{
		touchvote_copy(m->caller, sizeof(m->caller), p, strlen(p) - strlen("'s vote was canceled"));
		return m->kind = TOUCHVOTE_MSG_STOPPED;
	}
	if (touchvote_ends_with(p, " stopped their vote"))
	{
		touchvote_copy(m->caller, sizeof(m->caller), p, strlen(p) - strlen(" stopped their vote"));
		return m->kind = TOUCHVOTE_MSG_STOPPED;
	}
	if (touchvote_ends_with(p, "'s vote") && (q = strstr(p, " stopped ")))
	{
		q += strlen(" stopped ");
		touchvote_copy(m->caller, sizeof(m->caller), q, strlen(q) - strlen("'s vote"));
		return m->kind = TOUCHVOTE_MSG_STOPPED;
	}
	return m->kind = TOUCHVOTE_MSG_NONE;
}

#endif

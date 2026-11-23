/*
Godnotic: the name of the server a join is for (touch_joinname.h).

Every screen of a join named the server by its address ("Joining
10.1.1.239:26150"). This file keeps, for the server being joined or played on,
its name, and the label every join screen shows: "<name> (<address>)", or the
address alone until a name is known.

Where the name comes from, first that answers:
  - the server list (serverlist_cache), when the server is in it: a join from
    Multiplayer has its name at once;
  - the server itself: a getinfo with a challenge of our own, answered with an
    infoResponse that carries its hostname. A typed address or a link gets its
    name a moment after the join starts (and still while it is being refused,
    so the Could not join dialog can name it). The answer is ours alone:
    netconn.c hands it here and drops it, so the server list never learns of
    a server the player only typed.

The name keeps until another server is joined, so a reconnect, a failed join
and the pause page name the same server. A local game clears it.

No hook of its own in an upstream file besides the infoResponse one: the frame
comes from TouchJoinFail_Frame (touch_joinfail.c), Init from TouchJoinFail_Init.
*/
#include "quakedef.h"
#include "netconn.h"
#include "utf8lib.h"
#include "touch_joinname.h"
#include "touch_packname.h" // Touch: the pack named for the map
#include "touch_joinpreview.h" // Touch: the map a join is for

static cvar_t touch_join_address = {CF_CLIENT | CF_READONLY, "_touch_join_address", "", "Godnotic: the address of the server being joined or played on (kept after a failed join; empty in a local game)"};
static cvar_t touch_join_name = {CF_CLIENT | CF_READONLY, "_touch_join_name", "", "Godnotic: that server's name, colour codes out, cut to 32 characters; empty while unknown"};
static cvar_t touch_join_label = {CF_CLIENT | CF_READONLY, "_touch_join_label", "", "Godnotic: how the join screens name that server: \"name (address)\", or the address while no name is known"};

#define JN_ASKS 4           // getinfo tries (a server blocks a second getinfo from one address within 1 s)
#define JN_ASK_EVERY 1.25   // seconds between them

extern void TouchCurl_ClearJoinShot(void);
extern qbool TouchCurl_ShotPending(void);

static struct
{
	char address[128];  // LHNETADDRESS_ToString of the server
	lhnetaddress_t peer;
	char name[128];
	char map[MAX_QPATH];
	char token[32];     // the challenge of our getinfo
	qbool replied;
	int asks;
	double next_ask, next_list;
	// the first fragments of the server's signon message (TouchJoinName_PeekSignon)
	int peek_lines;     // curl lines of it handled so far
	qbool peek_done;
	int sig_bytes;      // bytes of the signon message received so far
	char early_map[MAX_QPATH]; // the map named by its first "curl --pak ... --for maps/<map>.bsp"
	char early_url[1024];      // and the URL of the pack that holds it
	char early_name[MAX_QPATH]; // and the file name it is stored under in dlcache/
	char held[8][1100];        // early curl commands waiting for the picture's requests to finish (TS, touch_curlshot.h)
	int nheld;
	double hold_until;
} tjn;
static cvar_t touch_early_downloads = {CF_CLIENT | CF_ARCHIVE, "touch_early_downloads", "1", "start the packs a server asks for (its curl --pak lines) from the first fragment of its signon message, not after the whole message has arrived, so they download while a stock server's stop-and-wait transfer of the precache lists is still running (Godnotic)"};

void TouchJoinName_Clean(const char *in, char *out, size_t outsize)
{
	char tmp[512], *s, *d;
	size_t chars, bytes;
	const char *ellipsis;
	if (!outsize)
		return;
	out[0] = 0;
	if (!in || !*in)
		return;
	COM_StringDecolorize(in, 0, tmp, sizeof(tmp), false);
	// one line, single spaces, nothing at either end
	for (s = d = tmp; *s; s++)
	{
		unsigned char c = (unsigned char)*s;
		if (c < 32 || c == 127)
			c = ' ';
		if (c == ' ' && (d == tmp || d[-1] == ' '))
			continue;
		*d++ = (char)c;
	}
	while (d > tmp && d[-1] == ' ')
		d--;
	*d = 0;
	chars = u8_strlen(tmp);
	if (chars <= TOUCHJOINNAME_MAXCHARS)
	{
		dp_strlcpy(out, tmp, outsize);
		return;
	}
	ellipsis = utf8_enable.integer ? "\xe2\x80\xa6" : "...";
	bytes = u8_bytelen(tmp, TOUCHJOINNAME_MAXCHARS - (utf8_enable.integer ? 1 : 3));
	while (bytes > 0 && tmp[bytes - 1] == ' ')
		bytes--;
	tmp[bytes] = 0;
	dpsnprintf(out, outsize, "%s%s", tmp, ellipsis);
}

const char *TouchJoinName_For(const char *addr)
{
	if (!addr || !*addr || strcmp(addr, tjn.address))
		return "";
	return tjn.name;
}

const char *TouchJoinName_Label(const char *addr, char *buf, size_t size)
{
	const char *name = TouchJoinName_For(addr);
	if (*name)
		dpsnprintf(buf, size, "%s (%s)", name, addr);
	else
		dp_strlcpy(buf, addr ? addr : "", size);
	return buf;
}

static void publish(void)
{
	char label[256];
	TouchJoinName_Label(tjn.address, label, sizeof(label));
	if (strcmp(touch_join_address.string, tjn.address))
		Cvar_SetQuick(&touch_join_address, tjn.address);
	if (strcmp(touch_join_name.string, tjn.name))
		Cvar_SetQuick(&touch_join_name, tjn.name);
	if (strcmp(touch_join_label.string, label))
		Cvar_SetQuick(&touch_join_label, label);
}

static void forget(void)
{
	if (!tjn.address[0] && !touch_join_label.string[0])
		return;
	memset(&tjn, 0, sizeof(tjn));
	publish();
}

// the server list's name for addr, when it has one
static qbool from_list(void)
{
#ifdef CONFIG_MENU
	unsigned i;
	for (i = 0; i < serverlist_cachecount; i++)
	{
		serverlist_info_t *info = &serverlist_cache[i].info;
		if (!info->name[0] || strcmp(info->cname, tjn.address))
			continue;
		TouchJoinName_Clean(info->name, tjn.name, sizeof(tjn.name));
		if (tjn.name[0])
			return true;
	}
#endif
	return false;
}

// Only a fresh response to this join can establish its map. Server-list rows
// may have been populated before a map vote or a reconnect.
const char *TouchJoinName_MapFor(const char *addr)
{
	if (addr && !strcmp(addr, tjn.address) && tjn.early_map[0])
		return tjn.early_map; // the server's own first fragment: what it is loading now, whatever the list or getinfo said
	return addr && !strcmp(addr, tjn.address) && tjn.replied ? tjn.map : "";
}

qbool TouchJoinName_EarlyPack(const char *map, char *url, size_t size, char *name, size_t namesize)
{
	if (!map || !tjn.early_map[0] || strcmp(map, tjn.early_map) || !tjn.early_url[0])
		return false;
	dp_strlcpy(url, tjn.early_url, size);
	if (name && namesize)
		dp_strlcpy(name, tjn.early_name, namesize);
	return true;
}

int TouchJoinName_SignonBytes(void)
{
	return cls.state == ca_connected && cls.signon == 0 ? tjn.sig_bytes : 0;
}

// a token of a curl line the server stuffed: nothing the console would read as more than a word
static qbool peek_token_ok(const char *t, size_t n)
{
	size_t i;
	if (!n || n > 900)
		return false;
	for (i = 0; i < n; i++)
	{
		unsigned char c = (unsigned char)t[i];
		if (!(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/' || c == ':' || c == '%' || c == '?' || c == '&' || c == '=' || c == '~' || c == '+' || c == ','))
			return false;
	}
	return true;
}

// One "curl --pak [--forthismap] [--as name] [--for file ...] url" line: remember the map it names and start the
// download now (without --forthismap: the real stufftext that follows, once the whole message is in, counts it
// for the join; a pack already complete by then is simply there).
static void peek_curl_line(const char *line, size_t n)
{
	char buf[1100], out[1100] = "curl";
	char *tok[64], *p;
	int nt = 0, i, forthismap = 0;
	const char *url = NULL;
	char bsp[MAX_QPATH] = "", as[MAX_QPATH] = "";
	if (n >= sizeof(buf) || n < 12)
		return;
	memcpy(buf, line, n);
	buf[n] = 0;
	for (p = buf; *p; p++)
		if ((unsigned char)*p < 32 && *p != ' ')
			return;
	for (p = buf; *p && nt < 64; )
	{
		while (*p == ' ')
			*p++ = 0;
		if (!*p)
			break;
		tok[nt++] = p;
		while (*p && *p != ' ')
			p++;
	}
	if (nt < 3 || strcmp(tok[0], "curl") || strcmp(tok[1], "--pak"))
		return;
	for (i = 1; i < nt; i++)
		if (!peek_token_ok(tok[i], strlen(tok[i])) && strncmp(tok[i], "--maxspeed=", 11))
			return;
	url = tok[nt - 1];
	if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))
		return;
	for (i = 1; i < nt; i++)
	{
		if (!strcmp(tok[i], "--forthismap"))
		{
			forthismap = 1;
			continue;
		}
		if (!strcmp(tok[i], "--as") && i + 1 < nt - 1 && strlen(tok[i + 1]) < sizeof(as) && strspn(tok[i + 1], "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.") == strlen(tok[i + 1]) && !strstr(tok[i + 1], ".."))
			dp_strlcpy(as, tok[i + 1], sizeof(as));
		if (!strcmp(tok[i], "--for") && i + 1 < nt - 1 && !bsp[0])
		{
			const char *f = tok[i + 1];
			size_t fl = strlen(f);
			if (!strncmp(f, "maps/", 5) && fl > 9 && !strcmp(f + fl - 4, ".bsp") && fl - 9 < sizeof(bsp))
			{
				memcpy(bsp, f + 5, fl - 9);
				bsp[fl - 9] = 0;
			}
		}
		if (strlen(out) + strlen(tok[i]) + 2 < sizeof(out))
		{
			dp_strlcat(out, " ", sizeof(out));
			dp_strlcat(out, tok[i], sizeof(out));
		}
	}
	(void)forthismap;
	// no "--for maps/<map>.bsp" (a server that does not say what a pack is for): the pack named for the map the join
	// is for (cts_mccormic-mark.pk3) is read for its picture just the same, from the first fragment
	if (!bsp[0] && !tjn.early_map[0])
	{
		char jm[MAX_QPATH];
		if (TouchJoinPreview_MapName(jm, sizeof(jm)) && tpn_url_names_map(url, jm))
			dp_strlcpy(bsp, jm, sizeof(bsp));
	}
	if (bsp[0] && !tjn.early_map[0])
	{
		dp_strlcpy(tjn.early_map, bsp, sizeof(tjn.early_map));
		dp_strlcpy(tjn.early_url, url, sizeof(tjn.early_url));
		dp_strlcpy(tjn.early_name, as, sizeof(tjn.early_name));
		Con_DPrintf("Join: the first fragment names %s (%s)\n", bsp, url);
	}
	if (touch_early_downloads.integer && tjn.nheld < (int)(sizeof(tjn.held) / sizeof(tjn.held[0])))
	{
		// held until the picture's own requests are through (a moment), then run as if the server had sent them now
		if (!tjn.nheld)
			tjn.hold_until = host.realtime + 2.5;
		dp_strlcpy(tjn.held[tjn.nheld++], out, sizeof(tjn.held[0]));
	}
}

static void release_held(void)
{
	int i;
	if (!tjn.nheld || (host.realtime < tjn.hold_until && TouchCurl_ShotPending()))
		return;
	for (i = 0; i < tjn.nheld; i++)
	{
		Con_DPrintf("Join: starting early: %s\n", tjn.held[i]);
		Cbuf_AddText(cmd_local, "\n");
		Cbuf_AddText(cmd_local, tjn.held[i]);
		Cbuf_AddText(cmd_local, "\n");
	}
	tjn.nheld = 0;
}

// netconn.c, every fragment of a reliable message from the server: the first one holds the `curl --pak` lines
// (SV_SendServerinfo writes them before the precache lists, which are most of the message and, from a stock
// server, arrive one round trip per fragment, ~10 s over a mobile link). They are read here, from the part of
// the message that has arrived, so the map's picture and the downloads start one round trip after the connect.
void TouchJoinName_PeekSignon(qbool from_server, const unsigned char *data, int length)
{
	int pos, limit = length < 8192 ? length : 8192, seen = 0;
	if (!from_server || cls.state != ca_connected || cls.signon != 0 || !tjn.address[0])
		return;
	tjn.sig_bytes = length;
	if (tjn.peek_done)
		return;
	for (pos = 0; pos + 11 < limit; pos++)
	{
		int end;
		if (memcmp(data + pos, "curl --", 7))
			continue;
		for (end = pos; end < limit && data[end] != '\n' && data[end]; end++)
			;
		if (end >= limit)
			break; // the line is not all here yet
		if (++seen > tjn.peek_lines)
		{
			tjn.peek_lines++;
			if (!memcmp(data + pos, "curl --finish_autodownload", 26))
				tjn.peek_done = true;
			else
				peek_curl_line((const char *)data + pos, (size_t)(end - pos));
		}
		pos = end;
	}
	if (limit >= 8192)
		tjn.peek_done = true; // past the head of the message
}

void TouchJoinName_Frame(void)
{
	static qbool was_trying;
	char addr[128], ask[64];
	lhnetaddress_t *peer = NULL;
	lhnetsocket_t *sock = NULL;
	if (cls.state == ca_dedicated)
		return;
	release_held();
	if (sv.active || cls.demoplayback)
	{
		was_trying = false;
		// a local game (hosted on LAN or not) or a demo: no server to name
		forget();
		return;
	}
	if (cls.connect_trying)
	{
		peer = &cls.connect_address;
		sock = cls.connect_mysocket;
	}
	else if (cls.state == ca_connected && cls.netcon)
	{
		peer = &cls.netcon->peeraddress;
		sock = cls.netcon->mysocket;
	}
	// not joining: the last server keeps its name (Could not join, a reconnect)
	if (!peer)
	{
		was_trying = false;
		return;
	}
	if (LHNETADDRESS_GetAddressType(peer) == LHNETADDRESSTYPE_LOOP)
	{
		forget();
		return;
	}
	LHNETADDRESS_ToString(peer, addr, sizeof(addr), true);
	if (strcmp(addr, tjn.address) || (cls.connect_trying && !was_trying))
	{
		TouchCurl_ClearJoinShot();
		memset(&tjn, 0, sizeof(tjn));
		dp_strlcpy(tjn.address, addr, sizeof(tjn.address));
		tjn.peer = *peer;
		dpsnprintf(tjn.token, sizeof(tjn.token), "gnjoin%08x", (unsigned)(rand() ^ (int)(Sys_DirtyTime() * 1000.0)));
		tjn.next_ask = tjn.next_list = host.realtime;
		publish();
	}
	was_trying = cls.connect_trying;
	if (tjn.replied)
		return;
	if (host.realtime >= tjn.next_list)
	{
		tjn.next_list = host.realtime + 0.5;
		if (!tjn.name[0] && from_list())
			publish();
	}
	if (sock && tjn.asks < JN_ASKS && host.realtime >= tjn.next_ask)
	{
		tjn.asks++;
		tjn.next_ask = host.realtime + JN_ASK_EVERY;
		dpsnprintf(ask, sizeof(ask), "\377\377\377\377getinfo %s", tjn.token);
		NetConn_WriteString(sock, ask, &tjn.peer);
	}
}

qbool TouchJoinName_Connectionless(const char *string, int length, lhnetaddress_t *peeraddress)
{
	char value[64], host_name[512];
	if (!tjn.token[0] || length < 13 || memcmp(string, "infoResponse\x0A", 13))
		return false;
	if (!InfoString_GetValue(string + 13, "challenge", value, sizeof(value)) || strcmp(value, tjn.token))
		return false;
	// ours: never the server list's business, wherever it came from
	if (!peeraddress || LHNETADDRESS_Compare(peeraddress, &tjn.peer))
		return true;
	tjn.replied = true;
	InfoString_GetValue(string + 13, "mapname", tjn.map, sizeof(tjn.map));
	if (!tjn.map[0]) InfoString_GetValue(string + 13, "map", tjn.map, sizeof(tjn.map));
	if (!tjn.name[0] && InfoString_GetValue(string + 13, "hostname", host_name, sizeof(host_name)))
	{
		TouchJoinName_Clean(host_name, tjn.name, sizeof(tjn.name));
		if (tjn.name[0])
			Con_DPrintf("Join: %s is \"%s\"\n", tjn.address, tjn.name);
	}
	publish();
	return true;
}

void TouchJoinName_Init(void)
{
	Cvar_RegisterVariable(&touch_join_address);
	Cvar_RegisterVariable(&touch_join_name);
	Cvar_RegisterVariable(&touch_join_label);
	Cvar_RegisterVariable(&touch_early_downloads);
}

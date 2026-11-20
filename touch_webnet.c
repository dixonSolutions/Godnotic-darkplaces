/* Touch: UDP for the browser build. A browser has no UDP, so on the web every
 * IPv4/IPv6 datagram the engine sends or reads goes through the page's WebRTC
 * data channel to an opt-in native relay (web/bridge-net.mjs, which sets
 * Module.godnoticNet; native side touch_bridge_relay.c). Loopback (local
 * games) is untouched. lhnet.c and netconn.c call in with one hook line each.
 * Natively this file compiles to nothing. */
#ifdef __EMSCRIPTEN__
#include "quakedef.h"
#include "lhnet.h"
#include "netconn.h"
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

int TouchWeb_Read(int family, void *content, int maxcontentlength, lhnetaddress_t *address);
int TouchWeb_Write(const void *content, int contentlength, const lhnetaddress_t *address);
int TouchWeb_ServerListReady(qbool consoleoutput);
void xt_web_relay_connected(const char *masters);
void xt_web_relay_message(const char *text);

EM_JS(int, touchweb_js_send, (const char *address, const void *data, int length), {
	var net = Module.godnoticNet;
	if (!net) return -1;
	return net.send(UTF8ToString(address), HEAPU8.slice(data, data + length));
});
/* meta: [0] family 4 or 6, [1..16] address bytes, [17..18] big-endian port */
EM_JS(int, touchweb_js_recv, (int family, void *data, int max, unsigned char *meta), {
	var net = Module.godnoticNet;
	if (!net) return 0;
	var p = net.recv(family);
	if (!p) return 0;
	if (p.payload.length > max) return -1;
	HEAPU8.set(p.payload, data);
	HEAPU8[meta] = p.address.length === 4 ? 4 : 6;
	HEAPU8.set(p.address, meta + 1);
	HEAPU8[meta + 17] = p.port >> 8;
	HEAPU8[meta + 18] = p.port & 255;
	return p.payload.length;
});
EM_JS(int, touchweb_js_ready, (int request), {
	var net = Module.godnoticNet;
	if (!net) return 0;
	if (request && !net.ready()) net.request();
	return net.ready() ? 1 : 0;
});

int TouchWeb_Write(const void *content, int contentlength, const lhnetaddress_t *address)
{
	char text[128];
	int sent;
	if (!LHNETADDRESS_ToString(address, text, sizeof(text), 1))
		return -1;
	sent = touchweb_js_send(text, content, contentlength);
	/* Nothing connected yet: the page starts looking for a relay; the packet
	 * is dropped like a lost UDP datagram. */
	return sent < 0 ? -1 : contentlength;
}

int TouchWeb_Read(int family, void *content, int maxcontentlength, lhnetaddress_t *address)
{
	unsigned char meta[19];
	char text[128];
	int n = touchweb_js_recv(family, content, maxcontentlength, meta);
	if (n <= 0)
		return n;
	if (meta[0] == 4)
		dpsnprintf(text, sizeof(text), "%u.%u.%u.%u:%u", meta[1], meta[2], meta[3], meta[4], (meta[17] << 8) | meta[18]);
	else
	{
		int i, at = dpsnprintf(text, sizeof(text), "[");
		for (i = 0; i < 8; i++)
			at += dpsnprintf(text + at, sizeof(text) - at, i ? ":%x" : "%x", (meta[1 + i * 2] << 8) | meta[2 + i * 2]);
		dpsnprintf(text + at, sizeof(text) - at, "]:%u", (meta[17] << 8) | meta[18]);
	}
	if (!LHNETADDRESS_FromString(address, text, 0))
		return 0;
	return n;
}

/* The server list asks masters and servers over the relay; until one is
 * connected the page looks for one and the list stays as it is. */
int TouchWeb_ServerListReady(qbool consoleoutput)
{
	if (touchweb_js_ready(1))
		return 1;
	if (consoleoutput)
		Con_Printf("Looking for a web relay to reach the server list...\n");
	return 0;
}

/* The page connected a relay: ask the masters it allows ("ip:port ..."),
 * which a browser cannot resolve itself (no DNS), then refresh the list. */
EMSCRIPTEN_KEEPALIVE void xt_web_relay_connected(const char *masters)
{
	static const char *const extras[] = {"sv_masterextra1", "sv_masterextra2", "sv_masterextra3"};
	const char *p = masters;
	size_t i;
	for (i = 0; i < sizeof(extras) / sizeof(extras[0]); i++)
	{
		char one[80];
		size_t n;
		for (;;)
		{
			while (*p == ' ')
				p++;
			n = strcspn(p, " ");
#ifdef NOSUPPORTIPV6
			if (*p == '[') // this build has IPv4 sockets only
			{
				p += n;
				continue;
			}
#endif
			break;
		}
		if (n >= sizeof(one))
			n = 0;
		memcpy(one, p, n);
		one[n] = 0;
		p += n;
		Cvar_Set(&cvars_all, extras[i], one);
	}
	Con_Printf("Web relay connected; masters: %s\n", masters);
	ServerList_QueryList(true, true, false, false);
}

EMSCRIPTEN_KEEPALIVE void xt_web_relay_message(const char *text)
{
	Con_Printf("%s\n", text);
}
#endif

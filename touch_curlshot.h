/* Fetch a missing map levelshot from the ZIP directory and member, alongside
 * the ordinary pack download. Requests are bounded and never delay the pack.
 *
 * Where the pack's URL comes from: a running or queued download of it, or, before any download
 * exists, the `curl --pak` lines at the very start of the server's signon message
 * (touch_joinname.c TouchJoinName_PeekSignon): that is one round trip after the connect, not the
 * ~10 s a stock server's stop-and-wait signon transfer takes to deliver its precache lists.
 *
 * Requests (each one HTTP range request): the tail of the pack (a suffix range, which also tells
 * its size), the directory when it is not in the tail, then the member with its local header in
 * one go. Typically two requests. The member is the map's picture; a pack that has none but holds
 * the BSP of a Quake 1 map gets a picture made from it (touch_bspshot.h). */
#include "touch_zipshot.h"
#include "touch_bspshot.h"
#include "touch_joinpreview.h"
#include "touch_loadfs.h"
extern qbool TouchZip_Inflate(const unsigned char *, size_t, unsigned char *, size_t);
extern qbool TouchJoinName_EarlyPack(const char *map, char *url, size_t size, char *name, size_t namesize);
typedef struct ts_fetch_s {
	struct ts_fetch_s *next;
	downloadinfo *di;     // the download this was started beside; NULL for one started from the signon message
	CURL *curl;
	unsigned char *buf;
	size_t have, want;
	uint64_t total;
	int stage;            // 1 tail, 2 directory, 3 member (+ its local header), 4 member data only; -1 over
	int suffix;           // the request asked for "the last N bytes": the size is learned from the answer
	int whole;            // the server ignores Range (a plain static file server) and the pack is small: the whole file is read
	uint64_t clen;        // Content-Length of the answer
	unsigned generation;
	double began;         // Sys_DirtyTime() of the first request
	char map[MAX_QPATH], url[1024];
	char packname[MAX_QPATH]; // dlcache/<packname>: where a pack read whole is kept (the download's own file), "" = not
	tzs_entry_t entry;
} ts_fetch_t;
static void TS_WholePack(ts_fetch_t *s, unsigned char *buf, size_t have, int save);
static ts_fetch_t *ts_fetches;
static char ts_map[MAX_QPATH], ts_path[MAX_QPATH];
static unsigned ts_generation;
static cvar_t touch_joinshot_early = {CF_CLIENT | CF_ARCHIVE, "touch_joinshot_early", "1", "start the map picture's range reads from the first fragment of the server's signon message, and make a picture from a Quake 1 BSP when its pack has none (Godnotic)"};

static void TS_Cancel(ts_fetch_t *s)
{
	if (s->curl) { qcurl_multi_remove_handle(curlm, s->curl); qcurl_easy_cleanup(s->curl); s->curl = NULL; }
	if (s->buf) { Z_Free(s->buf); s->buf = NULL; }
}
// a new join: its fetches are over (a record that is still talking to curl is cancelled first)
void TouchCurl_ClearJoinShot(void)
{
	ts_fetch_t *s;
	ts_map[0] = ts_path[0] = 0; ++ts_generation;
	for (s = ts_fetches; s; s = s->next)
		TS_Cancel(s);
	while (ts_fetches) { s = ts_fetches; ts_fetches = s->next; Z_Free(s); }
}
unsigned TouchCurl_JoinShotGeneration(void) { return ts_generation; }

qbool TouchCurl_JoinShot(const char *map, char *path, size_t size)
{
	if (!map || strcmp(map, ts_map) || !ts_path[0]) return false;
	dp_strlcpy(path, ts_path, size);
	return true;
}
#define TS_MAX_WHOLE (12u * 1024u * 1024u)
static size_t TS_Write(void *data, size_t size, size_t n, void *arg)
{
	ts_fetch_t *s = (ts_fetch_t *)arg;
	size_t bytes;
	long code = 0;
	if (size && n > (size_t)-1 / size) return 0;
	bytes = size * n;
	qcurl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &code);
	if (code == 200 && !s->whole && s->clen > 0 && s->clen <= TS_MAX_WHOLE && s->have == 0)
	{
		// no Range support: a small pack is cheaper read whole than not at all (a big one is refused, the answer is cut here)
		unsigned char *all = (unsigned char *)Z_Malloc((size_t)s->clen);
		if (all) { Z_Free(s->buf); s->buf = all; s->want = (size_t)s->clen; s->whole = 1; }
	}
	if ((code != 206 && !(code == 200 && s->whole)) || bytes > s->want - s->have) return 0;
	memcpy(s->buf + s->have, data, bytes); s->have += bytes;
	return bytes;
}
static size_t TS_Header(char *data, size_t size, size_t n, void *arg)
{
	// "Content-Range: bytes 0-0/12345": the size of a pack whose download has not started
	ts_fetch_t *s = (ts_fetch_t *)arg;
	size_t bytes = size * n;
	if (bytes > 16 && !strncasecmp(data, "Content-Length:", 15))
		s->clen = (uint64_t)strtoull(data + 15, NULL, 10);
	if (bytes > 15 && !strncasecmp(data, "Content-Range:", 14))
	{
		const char *slash = (const char *)memchr(data, '/', bytes);
		if (slash) s->total = (uint64_t)strtoull(slash + 1, NULL, 10);
	}
	return bytes;
}
// one range request: bytes [from, from+size) (from < 0: the last `size` bytes)
static qbool TS_Start(ts_fetch_t *s, int64_t from, size_t size, int stage)
{
	char range[64];
	if (!size || (from >= 0 && ((uint64_t)from > s->total || size > s->total - (uint64_t)from))) return false;
	s->buf = (unsigned char *)Z_Malloc(size);
	if (!s->buf) return false;
	s->curl = qcurl_easy_init();
	if (!s->curl) { TS_Cancel(s); return false; }
	s->stage = stage; s->want = size; s->have = 0; s->suffix = from < 0; s->whole = 0; s->clen = 0;
	if (from < 0)
		dpsnprintf(range, sizeof(range), "-%llu", (unsigned long long)size);
	else
		dpsnprintf(range, sizeof(range), "%llu-%llu", (unsigned long long)from, (unsigned long long)((uint64_t)from + size - 1));
	qcurl_easy_setopt(s->curl, CURLOPT_URL, s->url);
	qcurl_easy_setopt(s->curl, (CURLoption)(CURLOPTTYPE_OBJECTPOINT + 7), range);
	if (s->di)
		qcurl_easy_setopt(s->curl, CURLOPT_REFERER, s->di->referer);
	else if (cls.netcon)
	{
		static char referer[256];
		dpsnprintf(referer, sizeof(referer), "dp://%s/", cls.netcon->address);
		qcurl_easy_setopt(s->curl, CURLOPT_REFERER, referer);
	}
	qcurl_easy_setopt(s->curl, CURLOPT_FOLLOWLOCATION, 1);
	qcurl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, TS_Write);
	qcurl_easy_setopt(s->curl, CURLOPT_WRITEDATA, s);
	qcurl_easy_setopt(s->curl, (CURLoption)(CURLOPTTYPE_FUNCTIONPOINT + 79), TS_Header); // CURLOPT_HEADERFUNCTION
	qcurl_easy_setopt(s->curl, (CURLoption)(CURLOPTTYPE_OBJECTPOINT + 29), s); // CURLOPT_HEADERDATA
	qcurl_easy_setopt(s->curl, CURLOPT_PRIVATE, s);
	qcurl_easy_setopt(s->curl, CURLOPT_LOW_SPEED_LIMIT, (long)256);
	qcurl_easy_setopt(s->curl, CURLOPT_LOW_SPEED_TIME, (long)15);
	qcurl_easy_setopt(s->curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
	qcurl_easy_setopt(s->curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#ifdef __ANDROID__
	if (curl_android_capath) qcurl_easy_setopt(s->curl, CURLOPT_CAPATH, curl_android_capath);
#endif
	qcurl_multi_add_handle(curlm, s->curl);
	return true;
}
static qbool TS_ValidMap(const char *map)
{
	return map[0] && !strstr(map, "..") && strspn(map, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == strlen(map);
}
static qbool TS_HavePicture(const char *map)
{
	static const char *dirs[] = {"maps/", "levelshots/", NULL};
	static const char *exts[] = {".jpg", ".png", ".tga", NULL};
	char path[MAX_QPATH];
	int d, e;
	for (d = 0; dirs[d]; d++)
		for (e = 0; exts[e]; e++)
		{
			dpsnprintf(path, sizeof(path), "%s%s%s", dirs[d], map, exts[e]);
			if (FS_FileExists(path)) return true;
		}
	return false;
}
static void TS_Begin(ts_fetch_t *s)
{
	s->began = Sys_DirtyTime();
	// a size known already (a running download told us): the tail by its offset. Else the last bytes by a suffix range,
	// which also answers with the size, saving the 1-byte probe's round trip.
	if (s->total > 22)
	{
		size_t t = (size_t)min(s->total, (uint64_t)65557);
		if (!TS_Start(s, (int64_t)(s->total - t), t, 1)) s->stage = -1;
	}
	else if (!TS_Start(s, -65557, 65557, 1)) s->stage = -1;
}
static ts_fetch_t *TS_Add(downloadinfo *di, const char *map, const char *url, uint64_t total)
{
	ts_fetch_t *s = (ts_fetch_t *)Z_Malloc(sizeof(*s));
	s->di = di; s->generation = ts_generation; s->total = total; s->stage = -1;
	dp_strlcpy(s->map, map, sizeof(s->map)); dp_strlcpy(s->url, url, sizeof(s->url));
	s->next = ts_fetches; ts_fetches = s;
	return s;
}
static int TS_Active(void)
{
	ts_fetch_t *s;
	int count = 0;
	for (s = ts_fetches; s; s = s->next) if (s->curl) count++;
	return count;
}
static qbool TS_Known(const char *map, const char *url)
{
	ts_fetch_t *s;
	for (s = ts_fetches; s; s = s->next)
		if (!strcmp(s->map, map) && !strcmp(s->url, url)) return true;
	return false;
}
// The pack's URL is known from the signon message, no download exists yet (or the pack's is still queued):
// the picture does not wait for either.
static void TS_Early(void)
{
	char map[MAX_QPATH], url[1024], name[MAX_QPATH];
	if (!touch_joinshot_early.integer || cls.state != ca_connected) return;
	if (!TouchJoinPreview_MapName(map, sizeof(map)) || !TS_ValidMap(map)) return;
	if (TouchCurl_JoinShot(map, url, sizeof(url)) || TS_HavePicture(map)) return;
	if (TS_Active() >= 2 || !TouchJoinName_EarlyPack(map, url, sizeof(url), name, sizeof(name)) || TS_Known(map, url)) return;
	if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8))
	{
		ts_fetch_t *s = TS_Add(NULL, map, url, 0);
		char fn[MAX_QPATH];
		dp_strlcpy(s->packname, name, sizeof(s->packname));
		s->began = Sys_DirtyTime();
		dpsnprintf(fn, sizeof(fn), "dlcache/%s", name);
		if (name[0] && FS_FileExists(fn))
		{
			// a pack of an earlier visit (or download): the picture is made from the file, no network
			fs_offset_t size = 0, onDisk = 0;
			qfile_t *f = FS_OpenRealFile(fn, "rb", false);
			unsigned char *data;
			if (f) { onDisk = FS_FileSize(f); FS_Close(f); }
			data = onDisk > 0 && onDisk <= (fs_offset_t)TS_MAX_WHOLE ? FS_LoadFile(fn, tempmempool, true, &size) : NULL;
			if (data) { TS_WholePack(s, data, (size_t)size, 0); Mem_Free(data); }
			return;
		}
		TS_Begin(s);
	}
}
// true while the map's picture is still on its way (the join holds the big early downloads back for it,
// touch_joinname.c: the picture's few requests would otherwise share the link with hundreds of MB)
qbool TouchCurl_ShotPending(void)
{
	char map[MAX_QPATH], url[1024];
	if (!touch_joinshot_early.integer || cls.state != ca_connected) return false;
	if (!TouchJoinPreview_MapName(map, sizeof(map)) || !TS_ValidMap(map)) return false;
	if (TouchCurl_JoinShot(map, url, sizeof(url)) || TS_HavePicture(map)) return false;
	if (!TouchJoinName_EarlyPack(map, url, sizeof(url), NULL, 0)) return false;
	return TS_Active() > 0 || !TS_Known(map, url);
}
static void TS_Frame(void)
{
	downloadinfo *di;
	ts_fetch_t *s;
	char map[MAX_QPATH], existing[MAX_QPATH];
	if (!TouchJoinPreview_MapName(map, sizeof(map)) || !TS_ValidMap(map)) return;
	if (TouchCurl_JoinShot(map, existing, sizeof(existing))) return;
	if (TS_Active() >= 2) return;
	List_For_Each_Entry(di, &downloads, downloadinfo, list)
	{
		double length = -1;
		long code = 0;
		const char *url = NULL;
		qbool running = di->started && di->curle && di->stream;
		if (di->loadtype != LOADTYPE_PAK || di->maxspeed > 0 || curl_maxspeed.value > 0) continue;
		for (s = ts_fetches; s && s->di != di; s = s->next) {}
		if (s) continue;
		if (TS_HavePicture(map)) return;
		if (!running)
		{
			// Touch: the map's pack waits in the queue behind the others (4 at a time): ask its size and tail
			// now, beside the downloads, so the levelshot does not wait for the queue
			if (!di->url[0] || !strstr(di->url, map) || (strncmp(di->url, "http://", 7) && strncmp(di->url, "https://", 8)) || TS_Known(map, di->url)) continue;
			TS_Begin(TS_Add(di, map, di->url, 0));
			return;
		}
		qcurl_easy_getinfo(di->curle, CURLINFO_RESPONSE_CODE, &code);
		qcurl_easy_getinfo(di->curle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &length);
		if ((code != 200 && code != 206) || length <= 0 || length > 4294967295.0 || qcurl_easy_getinfo(di->curle, CURLINFO_EFFECTIVE_URL, &url) != CURLE_OK || !url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) || TS_Known(map, url)) continue;
		TS_Begin(TS_Add(di, map, url, di->startpos + (uint64_t)length));
		return;
	}
}
// the picture is ready: remember it for this join, keep it for the next one
static void TS_Keep(ts_fetch_t *s, const unsigned char *data, size_t size, const char *ext, uint32_t crc, const char *how)
{
	char path[MAX_QPATH];
	dpsnprintf(path, sizeof(path), "touch_joinshots/%s_%08x%s", s->map, (unsigned)crc, ext);
	if (!FS_WriteFile(path, data, size)) return;
	dp_strlcpy(ts_map, s->map, sizeof(ts_map)); dp_strlcpy(ts_path, path, sizeof(ts_path));
	// keep it for the next time: maps/<map>.<ext> is where the map list, the server dialog and the join card look
	if ((!strcmp(ext, ".jpg") || !strcmp(ext, ".png")) && TS_ValidMap(s->map))
	{
		char keep[MAX_QPATH];
		dpsnprintf(keep, sizeof(keep), "maps/%s%s", s->map, ext);
		if (!TS_HavePicture(s->map)) FS_WriteFile(keep, data, size);
	}
	TouchLoadFS_Changed();
	Con_DPrintf("JOINSHOT %s %s ready %.0f ms after its first request (%u bytes)\n", s->map, how, (Sys_DirtyTime() - s->began) * 1000.0, (unsigned)size);
}
static unsigned char *TS_Deflate(const unsigned char *in, size_t n, size_t *outn)
{
	// the engine's deflater hands back a mempool block; the PNG writer wants malloc's
	size_t dn = 0;
	unsigned char *d = FS_Deflate(in, n, &dn, 6, tempmempool), *copy;
	*outn = 0;
	if (!d || !dn) return NULL;
	copy = (unsigned char *)malloc(dn);
	if (copy) { memcpy(copy, d, dn); *outn = dn; }
	Mem_Free(d);
	return copy;
}
// the member's compressed bytes are in: inflate, check, and use them
static void TS_Finish(ts_fetch_t *s, const unsigned char *comp, size_t compn)
{
	unsigned char *out = (unsigned char *)Z_Malloc(s->entry.size);
	char map[MAX_QPATH];
	qbool ok = out && compn >= s->entry.compressed && (s->entry.method == 0 ? (memcpy(out, comp, s->entry.size), true) : TouchZip_Inflate(comp, compn, out, s->entry.size));
	if (ok && tzs_crc(out, s->entry.size) == s->entry.crc && TouchJoinPreview_MapName(map, sizeof(map)) && !strcmp(map, s->map))
	{
		if (!s->entry.bsp)
			TS_Keep(s, out, s->entry.size, s->entry.ext, s->entry.crc, "levelshot");
		else if (tbs_is_q1(out, s->entry.size))
		{
			unsigned char *rgb = (unsigned char *)malloc((size_t)TBS_W * TBS_H * 3);
			if (rgb && tbs_render_q1(out, s->entry.size, TBS_W, TBS_H, rgb))
			{
				size_t pn = 0;
				unsigned char *png = tbs_png(rgb, TBS_W, TBS_H, TS_Deflate, &pn);
				if (png) { TS_Keep(s, png, pn, ".png", s->entry.crc, "bsp picture"); free(png); }
			}
			free(rgb);
		}
	}
	if (out) Z_Free(out);
}
// the member's local header is at `buf` (`have` bytes from there on): its data follows it
static void TS_Member(ts_fetch_t *s, unsigned char *buf, size_t have, int more)
{
	if (have >= 30 && tzs_u32(buf) == 0x04034b50u && tzs_u16(buf+8) == s->entry.method && !(tzs_u16(buf+6) & 1))
	{
		size_t hl = 30 + (size_t)tzs_u16(buf+26) + tzs_u16(buf+28);
		if (hl + s->entry.compressed <= have) TS_Finish(s, buf + hl, s->entry.compressed);
		else if (more && (uint64_t)s->entry.offset + hl + s->entry.compressed <= s->total) TS_Start(s, (int64_t)s->entry.offset + (int64_t)hl, s->entry.compressed, 4);
	}
}
// the directory is in `dir`: which member to read, and the request for it (or, with the whole file in `all`, the member itself)
static void TS_Resolve(ts_fetch_t *s, const unsigned char *dir, size_t dirn, unsigned char *all, size_t alln)
{
	uint64_t room;
	size_t len;
	s->stage = -1;
	if (!tzs_find(dir, dirn, s->map, &s->entry) && !tzs_find_bsp(dir, dirn, s->map, &s->entry))
	{
		Con_DPrintf("JOINSHOT %s: no picture and no BSP in the pack\n", s->map);
		return;
	}
	if ((uint64_t)s->entry.offset + 30 > s->total) return;
	if (all)
	{
		if (s->entry.offset < alln) TS_Member(s, all + s->entry.offset, alln - s->entry.offset, 0);
		return;
	}
	// the local header, its name and a little extra, then the data: one request when the extra field is short
	room = s->total - s->entry.offset;
	len = (size_t)min(room, (uint64_t)30 + s->entry.namelen + 128 + s->entry.compressed);
	if (!TS_Start(s, s->entry.offset, len, 3)) s->stage = -1;
}
// a whole pack in memory (read from the network on a server that ignores Range, or from dlcache): the member is in it
static void TS_WholePack(ts_fetch_t *s, unsigned char *buf, size_t have, int save)
{
	uint32_t off = 0, len = 0;
	s->total = have;
	if (tzs_directory(buf, have, have, &off, &len)) TS_Resolve(s, buf + off, len, buf, have);
	// the whole pack came over the network: it is the download too, so it is kept where the download would have put it
	if (save && s->packname[0])
	{
		char fn[MAX_QPATH];
		dpsnprintf(fn, sizeof(fn), "dlcache/%s", s->packname);
		if (!FS_FileExists(fn) && FS_WriteFile(fn, buf, (fs_offset_t)have))
			Con_DPrintf("JOINSHOT %s: kept as %s (%u bytes), no second download\n", s->map, fn, (unsigned)have);
	}
}
static qbool TS_Done(CURL *curl, CURLcode result)
{
	ts_fetch_t *s;
	for (s = ts_fetches; s; s = s->next)
	{
		uint32_t off = 0, len = 0;
		long code = 0;
		int keep = 0;
		if (s->curl != curl) continue;
		qcurl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		if (s->generation == ts_generation && result == CURLE_OK && (code == 206 || (code == 200 && s->whole)) && (s->have == s->want || (s->suffix && s->have > 0 && s->have < s->want)) && (code == 206 || s->whole))
		{
			unsigned char *buf = s->buf;
			size_t have = s->have;
			int stage = s->stage;
			s->buf = NULL; // the request is over; its bytes are read here, then freed
			qcurl_multi_remove_handle(curlm, s->curl); qcurl_easy_cleanup(s->curl); s->curl = NULL;
			s->stage = -1;
			if (s->whole)
			{
				// the answer is the whole file
				TS_WholePack(s, buf, have, have == s->clen);
				Z_Free(buf);
				return true;
			}
			switch (stage)
			{
			case 1:
				if (!s->total && s->suffix) s->total = have; // the whole file came back
				if (s->total > 22 && tzs_directory(buf, have, s->total, &off, &len))
				{
					uint64_t tailstart = s->total - have;
					if ((uint64_t)off >= tailstart) TS_Resolve(s, buf + (off - tailstart), len, NULL, 0);
					else TS_Start(s, off, len, 2);
					keep = 1;
				}
				break;
			case 2:
				TS_Resolve(s, buf, have, NULL, 0);
				keep = 1;
				break;
			case 3:
				TS_Member(s, buf, have, 1);
				break;
			case 4:
				TS_Finish(s, buf, have);
				break;
			}
			(void)keep;
			Z_Free(buf);
			return true;
		}
		TS_Cancel(s);
		s->stage = -1;
		return true;
	}
	return false;
}
static void TS_End(downloadinfo *di)
{
	ts_fetch_t *s;
	for (s = ts_fetches; s; s = s->next)
		if (s->di == di) { TS_Cancel(s); s->di = NULL; s->stage = -1; return; }
}
// Curl_Frame with no download: only the picture's own requests run
static void TS_Pump(void)
{
	int remaining;
	CURLMcode mc;
	CURLMsg *msg;
	if (!ts_fetches || !TS_Active() || !List_Is_Empty(&downloads)) return;
	do { mc = qcurl_multi_perform(curlm, &remaining); } while (mc == CURLM_CALL_MULTI_PERFORM);
	while ((msg = qcurl_multi_info_read(curlm, &remaining)))
		if (msg->msg == CURLMSG_DONE) TS_Done(msg->easy_handle, msg->data.result);
}
// Curl_Frame, first thing (touch_curl.h TouchCurl_RetryFrame)
static void TS_Tick(void)
{
	TS_Early();
	TS_Pump();
}

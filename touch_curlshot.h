/* Fetch a missing map levelshot from the ZIP directory and member, alongside
 * the ordinary pack download. Requests are bounded and never delay the pack. */
#include "touch_zipshot.h"
#include "touch_joinpreview.h"
#include "touch_loadfs.h"
extern qbool TouchZip_Inflate(const unsigned char *, size_t, unsigned char *, size_t);
typedef struct ts_fetch_s {
	struct ts_fetch_s *next;
	downloadinfo *di;
	CURL *curl;
	unsigned char *buf;
	size_t have, want;
	uint64_t total;
	int stage;
	unsigned generation;
	char map[MAX_QPATH], url[1024];
	tzs_entry_t entry;
} ts_fetch_t;
static ts_fetch_t *ts_fetches;
static char ts_map[MAX_QPATH], ts_path[MAX_QPATH];
static unsigned ts_generation;

void TouchCurl_ClearJoinShot(void) { ts_map[0] = ts_path[0] = 0; ++ts_generation; }
unsigned TouchCurl_JoinShotGeneration(void) { return ts_generation; }

qbool TouchCurl_JoinShot(const char *map, char *path, size_t size)
{
	if (!map || strcmp(map, ts_map) || !ts_path[0]) return false;
	dp_strlcpy(path, ts_path, size);
	return true;
}
static void TS_Cancel(ts_fetch_t *s)
{
	if (s->curl) { qcurl_multi_remove_handle(curlm, s->curl); qcurl_easy_cleanup(s->curl); s->curl = NULL; }
	if (s->buf) { Z_Free(s->buf); s->buf = NULL; }
}
static size_t TS_Write(void *data, size_t size, size_t n, void *arg)
{
	ts_fetch_t *s = (ts_fetch_t *)arg;
	size_t bytes;
	long code = 0;
	if (size && n > (size_t)-1 / size) return 0;
	bytes = size * n;
	qcurl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &code);
	if (code != 206 || bytes > s->want - s->have) return 0;
	memcpy(s->buf + s->have, data, bytes); s->have += bytes;
	return bytes;
}
static qbool TS_Start(ts_fetch_t *s, uint64_t from, size_t size, int stage)
{
	char range[64];
	if (!size || from > s->total || size > s->total - from) return false;
	s->buf = (unsigned char *)Z_Malloc(size);
	if (!s->buf) return false;
	s->curl = qcurl_easy_init();
	if (!s->curl) { TS_Cancel(s); return false; }
	s->stage = stage; s->want = size; s->have = 0;
	dpsnprintf(range, sizeof(range), "%llu-%llu", (unsigned long long)from, (unsigned long long)(from + size - 1));
	qcurl_easy_setopt(s->curl, CURLOPT_URL, s->url);
	qcurl_easy_setopt(s->curl, (CURLoption)(CURLOPTTYPE_OBJECTPOINT + 7), range);
	qcurl_easy_setopt(s->curl, CURLOPT_REFERER, s->di->referer);
	qcurl_easy_setopt(s->curl, CURLOPT_FOLLOWLOCATION, 1);
	qcurl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, TS_Write);
	qcurl_easy_setopt(s->curl, CURLOPT_WRITEDATA, s);
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
static void TS_Frame(void)
{
	downloadinfo *di;
	ts_fetch_t *s;
	char map[MAX_QPATH], existing[MAX_QPATH];
	int count = 0;
	if (!TouchJoinPreview_MapName(map, sizeof(map)) || !map[0] || strstr(map, "..") || strspn(map, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != strlen(map)) return;
	if (TouchCurl_JoinShot(map, existing, sizeof(existing))) return;
	for (s = ts_fetches; s; s = s->next) count++;
	if (count >= 2) return;
	List_For_Each_Entry(di, &downloads, downloadinfo, list)
	{
		double length = -1;
		long code = 0;
		const char *url = NULL;
		char path[MAX_QPATH];
		size_t tail;
		if (!di->started || !di->curle || !di->stream || di->loadtype != LOADTYPE_PAK || di->maxspeed > 0 || curl_maxspeed.value > 0) continue;
		for (s = ts_fetches; s && s->di != di; s = s->next) {}
		if (s) continue;
		dpsnprintf(path, sizeof(path), "maps/%s.jpg", map);
		if (FS_FileExists(path)) return;
		dpsnprintf(path, sizeof(path), "maps/%s.png", map);
		if (FS_FileExists(path)) return;
		qcurl_easy_getinfo(di->curle, CURLINFO_RESPONSE_CODE, &code);
		qcurl_easy_getinfo(di->curle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &length);
		if ((code != 200 && code != 206) || length <= 0 || length > 4294967295.0 || qcurl_easy_getinfo(di->curle, CURLINFO_EFFECTIVE_URL, &url) != CURLE_OK || !url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) continue;
		s = (ts_fetch_t *)Z_Malloc(sizeof(*s));
		s->di = di; s->generation = ts_generation; s->total = di->startpos + (uint64_t)length;
		dp_strlcpy(s->map, map, sizeof(s->map)); dp_strlcpy(s->url, url, sizeof(s->url));
		s->next = ts_fetches; ts_fetches = s;
		tail = (size_t)min(s->total, (uint64_t)65557);
		if (!TS_Start(s, s->total - tail, tail, 1)) s->stage = -1;
		return;
	}
}
static qbool TS_Done(CURL *curl, CURLcode result)
{
	ts_fetch_t *s;
	for (s = ts_fetches; s; s = s->next)
	{
		uint32_t off = 0, len = 0;
		uint64_t at = 0;
		int next = 0;
		long code = 0;
		if (s->curl != curl) continue;
		qcurl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		if (s->generation == ts_generation && result == CURLE_OK && code == 206 && s->have == s->want)
		{
			if (s->stage == 1 && tzs_directory(s->buf, s->have, s->total, &off, &len)) { at = off; next = 2; }
			else if (s->stage == 2 && tzs_find(s->buf, s->have, s->map, &s->entry) && (uint64_t)s->entry.offset + 30 <= s->total) { at = s->entry.offset; len = 30; next = 3; }
			else if (s->stage == 3 && tzs_u32(s->buf) == 0x04034b50u && tzs_u16(s->buf+8) == s->entry.method && !(tzs_u16(s->buf+6) & 1)) { at = (uint64_t)s->entry.offset + 30 + tzs_u16(s->buf+26) + tzs_u16(s->buf+28); len = s->entry.compressed; next = 4; }
			else if (s->stage == 4)
			{
				unsigned char *out = (unsigned char *)Z_Malloc(s->entry.size);
				char map[MAX_QPATH], path[MAX_QPATH];
				qbool ok = out && (s->entry.method == 0 ? (memcpy(out, s->buf, s->entry.size), true) : TouchZip_Inflate(s->buf, s->have, out, s->entry.size));
				if (ok && tzs_crc(out, s->entry.size) == s->entry.crc && TouchJoinPreview_MapName(map, sizeof(map)) && !strcmp(map, s->map))
				{
					dpsnprintf(path, sizeof(path), "touch_joinshots/%s_%08x%s", s->map, (unsigned)s->entry.crc, s->entry.ext);
					if (FS_WriteFile(path, out, s->entry.size))
					{
						dp_strlcpy(ts_map, s->map, sizeof(ts_map)); dp_strlcpy(ts_path, path, sizeof(ts_path));
						// keep it for the next time: maps/<map>.<ext> is where the map list, the server dialog and the join card look
						if ((!strcmp(s->entry.ext, ".jpg") || !strcmp(s->entry.ext, ".png")) && s->map[0] && strspn(s->map, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == strlen(s->map))
						{
							char keep[MAX_QPATH];
							dpsnprintf(keep, sizeof(keep), "maps/%s%s", s->map, s->entry.ext);
							if (!FS_FileExists(keep)) FS_WriteFile(keep, out, s->entry.size);
						}
						TouchLoadFS_Changed();
					}
				}
				if (out) Z_Free(out);
			}
		}
		TS_Cancel(s);
		if (!next || !TS_Start(s, at, len, next)) s->stage = -1;
		return true;
	}
	return false;
}
static void TS_End(downloadinfo *di)
{
	ts_fetch_t **p, *s;
	for (p = &ts_fetches; (s = *p); p = &s->next)
		if (s->di == di) { TS_Cancel(s); *p = s->next; Z_Free(s); return; }
}

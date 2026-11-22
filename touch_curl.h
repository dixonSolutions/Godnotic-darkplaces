// Touch: one large HTTP download fetched as several ranges at once
// (docs/NETWORK.md). Included by libcurl.c, whose download list and curl
// function pointers it uses; the hooks there are single "// Touch:" lines.
//
// A map pack is one file of tens of MB. A single HTTP connection over a link
// with latency and some loss, or from a server that limits each connection,
// moves far less than the link can. Once the server has answered the stock
// download (its size is then known) and more than two curl_segment_size
// pieces are left, the rest is split: the stock transfer keeps the first
// piece, up to curl_segments - 1 more connections fetch the others with
// Range requests into <file>.partN, and when every piece is in, the parts are
// appended to the file in order and the stock code finishes the download as
// usual (FS_AddPack). The stock transfer is only cut short once every extra
// connection has answered 206 Partial Content: a server without ranges, an
// error or a redirect to something else leaves the stock download alone.
// A failed piece is asked for once more from where it stopped; if that fails
// too, the download fails as a stock one would. A download that is cancelled
// or interrupted drops its parts; the next attempt resumes the main file
// (stock CURLOPT_RESUME_FROM). Downloads with a speed limit (curl_maxspeed,
// sv_curl_maxspeed) are never split, so the limit holds.

static cvar_t curl_segments = {CF_CLIENT | CF_ARCHIVE, "curl_segments", "6", "connections one large HTTP download may use at once (Godnotic, docs/NETWORK.md); 1 = one, as stock"};
static cvar_t curl_segment_size = {CF_CLIENT | CF_ARCHIVE, "curl_segment_size", "2097152", "smallest piece in bytes worth its own connection when a download is split"};

static cvar_t curl_retries = {CF_CLIENT | CF_ARCHIVE, "curl_retries", "6", "times a failed download the current map needs (curl --forthismap) is tried again, resuming the file from where it stopped, 1, 2, 4, 8, 15 and 15 s apart, before the join goes on without it (Godnotic); 0 = stock, the first failure ends it"};

// downloads waiting to be tried again (touch_curl.h, TouchCurl_Retry)
typedef struct tc_retry_s
{
	struct tc_retry_s *next;
	char url[1024];
	char name[MAX_QPATH]; // as Curl_Begin_ToFile wants it, without dlcache/
	char fn[MAX_QPATH];   // dlcache/<name>
	double maxspeed;
	int loadtype;
	int tries;
	double due;           // realtime of the next attempt; 0 while one is running
}
tc_retry_t;
static tc_retry_t *tc_retries;

static void TouchCurl_RetryFrame(void);
static downloadinfo *Curl_Find(const char *filename);
#include "touch_net.h" // TouchCurl_ManualRetry
#ifndef _WIN32
#include <dirent.h>
#include <unistd.h>
#endif

#define TC_MAXSEG 7

struct tc_dl_s;

typedef struct tc_seg_s
{
	CURL *curle;
	qfile_t *file;
	char path[MAX_OSPATH];
	fs_offset_t from, to;   // bytes [from, to) of the file
	fs_offset_t have;
	int state;              // 0 running, 1 complete, 2 failed for good
	int tries;
	qbool bad;              // the server did not answer 206
	struct tc_dl_s *dl;
}
tc_seg_t;

typedef struct tc_dl_s
{
	struct tc_dl_s *next;
	downloadinfo *di;
	int nseg;               // 0: not split (checked, or given up)
	tc_seg_t seg[TC_MAXSEG];
	fs_offset_t total;      // file size
	fs_offset_t limit;      // the stock transfer stops here once confirmed
	qbool confirmed;
	qbool maindone;
	int result;             // 0 pending, 1 complete, 2 failed
	qbool reported;
	double seen;            // when the server's answer arrived
	qbool decided;
}
tc_dl_t;

#include "touch_curlshot.h"

static tc_dl_t *tc_list;
static int touchcurl_ok;

static tc_dl_t *TC_Find(downloadinfo *di)
{
	tc_dl_t *d;
	for (d = tc_list; d; d = d->next)
		if (d->di == di)
			return d;
	return NULL;
}

static void TC_DropSeg(tc_seg_t *s)
{
	if (s->curle)
	{
		qcurl_multi_remove_handle(curlm, s->curle);
		qcurl_easy_cleanup(s->curle);
		s->curle = NULL;
	}
	if (s->file)
	{
		FS_RemoveOnClose(s->file);
		FS_Close(s->file);
		s->file = NULL;
	}
}

// give up splitting: the stock transfer carries on alone
static void TC_Abandon(tc_dl_t *d)
{
	int i;
	for (i = 0; i < d->nseg; i++)
		TC_DropSeg(&d->seg[i]);
	d->nseg = 0;
	d->limit = 0;
	d->confirmed = false;
}

static size_t TC_Write(void *data, size_t size, size_t nmemb, void *vseg)
{
	tc_seg_t *s = (tc_seg_t *)vseg;
	size_t bytes = size * nmemb;
	long code = 0;
	qcurl_easy_getinfo(s->curle, CURLINFO_RESPONSE_CODE, &code);
	if (code != 206)
	{
		// the whole file, or an error page: not this piece
		s->bad = true;
		return 0;
	}
	if (s->from + s->have + (fs_offset_t)bytes > s->to)
		bytes = (size_t)(s->to - s->from - s->have);
	if (bytes && FS_Write(s->file, data, bytes) != (fs_offset_t)bytes)
		return 0;
	s->have += bytes;
	return bytes;
}

static qbool TC_StartSeg(tc_dl_t *d, tc_seg_t *s, const char *url)
{
	char range[64];
	const char *ua = engineversion;
	s->curle = qcurl_easy_init();
	if (!s->curle)
		return false;
	dpsnprintf(range, sizeof(range), "%lld-%lld", (long long)(s->from + s->have), (long long)(s->to - 1));
	qcurl_easy_setopt(s->curle, CURLOPT_URL, url);
	qcurl_easy_setopt(s->curle, (CURLoption)(CURLOPTTYPE_OBJECTPOINT + 7), range); // CURLOPT_RANGE
	if (curl_useragent.integer)
		qcurl_easy_setopt(s->curle, CURLOPT_USERAGENT, ua ? ua : "");
	qcurl_easy_setopt(s->curle, CURLOPT_REFERER, d->di->referer);
	qcurl_easy_setopt(s->curle, CURLOPT_FOLLOWLOCATION, 1);
	qcurl_easy_setopt(s->curle, CURLOPT_WRITEFUNCTION, TC_Write);
	qcurl_easy_setopt(s->curle, CURLOPT_WRITEDATA, (void *)s);
	qcurl_easy_setopt(s->curle, CURLOPT_PRIVATE, (void *)s);
	qcurl_easy_setopt(s->curle, CURLOPT_LOW_SPEED_LIMIT, (long)256);
	qcurl_easy_setopt(s->curle, CURLOPT_LOW_SPEED_TIME, (long)45);
	qcurl_easy_setopt(s->curle, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
	qcurl_easy_setopt(s->curle, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#ifdef __ANDROID__
	if (curl_android_capath)
		qcurl_easy_setopt(s->curle, CURLOPT_CAPATH, curl_android_capath);
#endif
	qcurl_multi_add_handle(curlm, s->curle);
	return true;
}

// a download the server answered: note it, and half a second later split
// what is left if it would still take more than three seconds
static void TC_Split(downloadinfo *di, tc_dl_t *d)
{
	double length = -1, speed = 0;
	long code = 0;
	const char *url = NULL;
	fs_offset_t pos, remaining;
	int n, i;

	qcurl_easy_getinfo(di->curle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &length);
	qcurl_easy_getinfo(di->curle, CURLINFO_RESPONSE_CODE, &code);
	if (!d)
	{
		if (length <= 0 || !code)
			return; // no answer yet: ask again next frame
		d = (tc_dl_t *)Z_Malloc(sizeof(*d));
		d->di = di;
		d->seen = host.realtime;
		d->next = tc_list;
		tc_list = d;
		return;
	}
	if (host.realtime - d->seen < 0.5)
		return;
	d->decided = true;
	if (code != 200 && code != 206)
		return;
	if (qcurl_easy_getinfo(di->curle, CURLINFO_EFFECTIVE_URL, &url) != CURLE_OK || !url || strncmp(url, "http", 4))
		return;
	d->total = di->startpos + (fs_offset_t)length;
	pos = di->startpos + (fs_offset_t)di->bytes_received;
	remaining = d->total - pos;
	qcurl_easy_getinfo(di->curle, CURLINFO_SPEED_DOWNLOAD, &speed);
	if (speed > 0 && remaining / speed < 3)
		return; // nearly there: another connection would not pay for itself
	n = (int)min(remaining / max(curl_segment_size.integer, 65536), (fs_offset_t)curl_segments.integer);
	n = min(n, TC_MAXSEG + 1);
	if (n < 2)
		return;
	// the stock transfer keeps [pos, B1); piece i takes [B(i+1), B(i+2))
	d->nseg = n - 1;
	for (i = 0; i < d->nseg; i++)
	{
		tc_seg_t *s = &d->seg[i];
		s->dl = d;
		s->from = pos + remaining * (i + 1) / n;
		s->to = i == d->nseg - 1 ? d->total : pos + remaining * (i + 2) / n;
		dpsnprintf(s->path, sizeof(s->path), "%s.part%d", di->filename, i + 1);
		s->file = FS_OpenRealFile(s->path, "wb", false);
		if (!s->file || !TC_StartSeg(d, s, url))
		{
			TC_Abandon(d);
			return;
		}
	}
	Con_DPrintf("%s: %d connections for %lld bytes\n", di->filename, n, (long long)remaining);
}

static void TC_CheckComplete(tc_dl_t *d)
{
	int i;
	unsigned char buf[65536];
	fs_offset_t got;
	qfile_t *f;
	if (d->result || !d->maindone)
		return;
	for (i = 0; i < d->nseg; i++)
	{
		if (d->seg[i].state == 2)
		{
			d->result = 2;
			return;
		}
		if (d->seg[i].state != 1)
			return;
	}
	// every piece is in: append them in order
	for (i = 0; i < d->nseg; i++)
	{
		tc_seg_t *s = &d->seg[i];
		if (s->file)
		{
			FS_Close(s->file);
			s->file = NULL;
		}
		f = FS_OpenRealFile(s->path, "rb", false);
		if (!f)
		{
			d->result = 2;
			return;
		}
		while ((got = FS_Read(f, buf, sizeof(buf))) > 0)
			FS_Write(d->di->stream, buf, got);
		FS_RemoveOnClose(f);
		FS_Close(f);
	}
	d->result = FS_Tell(d->di->stream) == d->total ? 1 : 2;
	if (d->result == 2)
		Con_Printf("%s: pieces do not add up to %lld bytes\n", d->di->filename, (long long)d->total);
}

// libcurl.c Curl_Frame, after curl ran: split new downloads, confirm splits
static void TouchCurl_Frame(void)
{
	downloadinfo *di;
	tc_dl_t *d;
	int i;
	long code;
	TS_Frame();
	if (curl_segments.integer < 2 || curl_maxspeed.value > 0)
		return;
	List_For_Each_Entry(di, &downloads, downloadinfo, list)
		if (di->started && di->curle && di->stream && !di->buffer && !di->post_content_type && di->maxspeed <= 0 && (!(d = TC_Find(di)) || !d->decided))
			TC_Split(di, d);
	for (d = tc_list; d; d = d->next)
	{
		if (!d->nseg || d->confirmed)
			continue;
		for (i = 0; i < d->nseg; i++)
		{
			code = 0;
			qcurl_easy_getinfo(d->seg[i].curle, CURLINFO_RESPONSE_CODE, &code);
			if (d->seg[i].bad || (code && code != 206) || d->seg[i].state == 2)
			{
				TC_Abandon(d);
				break;
			}
			if (!code)
				break;
		}
		if (d->nseg && i == d->nseg)
		{
			// every piece is coming: the stock transfer stops at the first
			if (d->di->startpos + (fs_offset_t)d->di->bytes_received >= d->seg[0].from)
				TC_Abandon(d); // it got there first
			else
			{
				d->limit = d->seg[0].from;
				d->confirmed = true;
			}
		}
	}
}

// CURL_fwrite: the stock transfer writes no further than its piece
static size_t TouchCurl_Clip(downloadinfo *di, size_t bytes)
{
	tc_dl_t *d;
	fs_offset_t pos;
	if (!tc_list || !di->stream || !(d = TC_Find(di)) || !d->confirmed)
		return bytes;
	pos = di->startpos + (fs_offset_t)di->bytes_received;
	if (pos + (fs_offset_t)bytes > d->limit)
		bytes = pos < d->limit ? (size_t)(d->limit - pos) : 0;
	return bytes;
}

// Curl_Frame, a finished transfer: true when it was one of ours
static qbool TouchCurl_Done(CURL *curle, CURLcode result)
{
	tc_dl_t *d;
	int i;
	if (TS_Done(curle, result)) return true;
	for (d = tc_list; d; d = d->next)
	{
		if (d->confirmed && d->di->curle == curle)
		{
			if (d->di->startpos + (fs_offset_t)d->di->bytes_received != d->limit)
				return false; // a real failure before its piece was done: stock handling
			// its piece is done (curl stopped it with a write error); the
			// handle stays for the progress line until Curl_EndDownload
			qcurl_multi_remove_handle(curlm, curle);
			d->maindone = true;
			TC_CheckComplete(d);
			return true;
		}
		for (i = 0; i < d->nseg; i++)
		{
			tc_seg_t *s = &d->seg[i];
			if (s->curle != curle)
				continue;
			qcurl_multi_remove_handle(curlm, curle);
			qcurl_easy_cleanup(curle);
			s->curle = NULL;
			if (s->from + s->have == s->to)
				s->state = 1;
			else if (d->confirmed && !s->bad && ++s->tries < 2 && TC_StartSeg(d, s, d->di->url))
				Con_DPrintf("%s: piece %d again from %lld (%d)\n", d->di->filename, i + 1, (long long)(s->from + s->have), (int)result);
			else
				s->state = 2;
			TC_CheckComplete(d);
			return true;
		}
	}
	return false;
}

// Curl_Frame: a split download that is complete or failed, for Curl_EndDownload
static downloadinfo *TouchCurl_Finished(void)
{
	tc_dl_t *d;
	for (d = tc_list; d; d = d->next)
	{
		if (d->result && !d->reported)
		{
			d->reported = true;
			touchcurl_ok = d->result == 1;
			return d->di;
		}
	}
	return NULL;
}

// Curl_EndDownload: the pieces go with the download
static void TouchCurl_End(downloadinfo *di)
{
	tc_dl_t **p, *d;
	TS_End(di);
	for (p = &tc_list; (d = *p); p = &d->next)
	{
		if (d->di == di)
		{
			TC_Abandon(d);
			*p = d->next;
			Z_Free(d);
			return;
		}
	}
}

// Curl_GetDownloadInfo: progress and speed over all pieces
static void TouchCurl_Info(downloadinfo *di, double *progress, double *speed, int index)
{
	tc_dl_t *d = tc_list ? TC_Find(di) : NULL;
	double have, v;
	int i;
	if (!d || !d->nseg || d->total <= 0)
	{
		TouchNet_CurlProgress(index, (double)di->startpos + di->bytes_received);
		return;
	}
	have = (double)di->startpos + di->bytes_received;
	if (d->maindone)
		*speed = 0; // the stock transfer is through its piece
	for (i = 0; i < d->nseg; i++)
	{
		have += d->seg[i].have;
		v = 0;
		if (d->seg[i].curle)
			qcurl_easy_getinfo(d->seg[i].curle, CURLINFO_SPEED_DOWNLOAD, &v);
		*speed += v;
	}
	*progress = have / (double)d->total;
	TouchNet_CurlProgress(index, have);
}

void TouchCurl_Init(void)
{
	Cvar_RegisterVariable(&curl_retries);
	Cvar_RegisterVariable(&curl_segments);
	Cvar_RegisterVariable(&curl_segment_size);
}

// libcurl.c Curl_EndDownload, before it counts a download for this map as a
// failure. Stock DarkPlaces counts the first failure (a reset connection, 45 s
// under 256 B/s, a 5xx, a pack that did not open) and, when the last download
// is done, goes on to the precache without the file: "cannot load map". A map
// pack from a public server is tens of MB over Wi-Fi, and servers send a dozen
// packs; one stumble was the whole join. Here a failure waits and starts the
// same download again: the partial file is resumed (Curl_Begin's own resume),
// and while the attempt is pending the download still counts as "added", so
// the client keeps waiting. Returns true when a new attempt is planned.
static qbool TouchCurl_Retry(downloadinfo *di, int status, CURLcode error)
{
	tc_retry_t *r;
	int max = curl_retries.integer;
	if (max <= 0 || status == 2 /* CURL_DOWNLOAD_ABORTED, declared below */ || di->buffer || di->post_content_type || strncmp(di->filename, "dlcache/", 8))
		return false;
	for (r = tc_retries; r; r = r->next)
		if (!strcmp(r->fn, di->filename))
			break;
	if (!r)
	{
		r = (tc_retry_t *)Z_Malloc(sizeof(*r));
		dp_strlcpy(r->fn, di->filename, sizeof(r->fn));
		r->next = tc_retries;
		tc_retries = r;
	}
	if (r->tries >= max)
	{
		Con_Printf("Download of %s failed for good after %d tries (curl error %d)\n", di->filename + 8, r->tries + 1, (int)error);
		r->due = -1; // dropped by the sweep in TouchCurl_RetryFrame
		return false;
	}
	dp_strlcpy(r->url, di->url, sizeof(r->url));
	dp_strlcpy(r->name, di->filename + 8, sizeof(r->name));
	r->maxspeed = di->maxspeed;
	r->loadtype = di->loadtype;
	r->due = host.realtime + (r->tries < 4 ? (double)(1 << r->tries) : 15.0);
	r->tries++;
	Con_Printf("Download of %s failed (curl error %d, %.0f KB so far): trying again in %.0f s (%d of %d)\n", r->name, (int)error,
		(double)(di->startpos + (fs_offset_t)di->bytes_received) / 1024.0, r->due - host.realtime, r->tries, max);
	return true;
}

// a download finished: its retry entry is done
static void TouchCurl_RetryDone(downloadinfo *di)
{
	tc_retry_t *r;
	for (r = tc_retries; r; r = r->next)
		if (!strcmp(r->fn, di->filename))
			r->due = -1;
}

// every Curl_Frame: start the attempts that are due
static void TouchCurl_RetryFrame(void)
{
	tc_retry_t **link = &tc_retries, *r;
	while ((r = *link))
	{
		if (r->due < 0)
		{
			*link = r->next;
			Z_Free(r);
			continue;
		}
		if (r->due > 0 && host.realtime >= r->due)
		{
			int before = numdownloads_added;
			qbool began;
			r->due = 0;
			began = Curl_Begin_ToFile(r->url, r->maxspeed, r->name, r->loadtype, true);
			// the attempt that failed was already counted as added
			if (numdownloads_added > before)
				numdownloads_added = before;
			else if (!began)
				++numdownloads_fail; // could not even start: now it is a failure
		}
		link = &r->next;
	}
}


/*
Resuming only onto the same file, and using only a pack that is whole
-----------------------------------------------------------------------
A partial download is kept with the server's validator (ETag, else
Last-Modified) in <file>.meta. A resume asks for the bytes after what is on
disk, which is only right if the file on the server is still the one the
partial came from; a partial with no .meta (an older version's, or a crash)
or with another validator starts again from byte 0. A finished pack is read
before it is used: its end-of-central-directory record, and every central
directory entry must point at a local file header. That catches a truncated
file, one with a hole in the middle, or an HTML error page, none of which the
pack opener (which reads the directory only) reliably does. A file that
fails is emptied and the download counts as a failed attempt, tried again from
byte 0 (TouchCurl_Retry). Startup, and a manual Retry, delete every pk3 in
dlcache that has no .meta and fails the same check.
*/

typedef struct tc_hdr_s
{
	struct tc_hdr_s *next;
	downloadinfo *di;
	char stored[200]; // the validator the partial came from ("" = none known)
	qbool resuming;
	qbool mismatch;   // the file on the server changed: start again
} tc_hdr_t;
static tc_hdr_t *tc_hdrs;
static qbool tc_swept;

static void TC_RealPath(char *out, size_t size, const char *fn, const char *suffix)
{
	dpsnprintf(out, size, "%s%s%s", fs_gamedir, fn, suffix);
}

static void TC_ReadMeta(const char *fn, char *out, size_t size)
{
	char path[MAX_OSPATH];
	FILE *f;
	out[0] = 0;
	TC_RealPath(path, sizeof(path), fn, ".meta");
	if ((f = fopen(path, "rb")))
	{
		size_t n = fread(out, 1, size - 1, f);
		out[n] = 0;
		fclose(f);
	}
}

static void TC_WriteMeta(const char *fn, const char *value)
{
	char path[MAX_OSPATH];
	FILE *f;
	TC_RealPath(path, sizeof(path), fn, ".meta");
	if ((f = fopen(path, "wb")))
	{
		fputs(value, f);
		fclose(f);
	}
}

static void TC_DropMeta(const char *fn)
{
	char path[MAX_OSPATH];
	TC_RealPath(path, sizeof(path), fn, ".meta");
	remove(path);
}

static void TC_Empty(const char *fn)
{
	char path[MAX_OSPATH];
	FILE *f;
	TC_RealPath(path, sizeof(path), fn, "");
	if ((f = fopen(path, "wb")))
		fclose(f);
}

static unsigned TC_U16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned TC_U32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

// a zip (pk3) whose directory is whole and whose entries all start at a local file header
static qbool TC_ZipOK(const char *path, const char **why)
{
	FILE *f = fopen(path, "rb");
	unsigned char head[4], tail[65557 + 22], *eocd = NULL, *cd = NULL, local[4], *p;
	long size, tailstart, tailsize;
	unsigned entries, cdsize, cdoff, i, nlen, elen, clen, off;
	qbool ok = false;
	*why = "unreadable";
	if (!f)
		return false;
	if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 22 || fseek(f, 0, SEEK_SET) || fread(head, 1, 4, f) != 4)
	{
		*why = "too short";
		goto done;
	}
	if (!memcmp(head, "PACK", 4))
	{
		ok = true; // a quake pak: not checked here
		goto done;
	}
	if (memcmp(head, "PK\3\4", 4))
	{
		*why = "not a zip";
		goto done;
	}
	tailsize = size < (long)sizeof(tail) ? size : (long)sizeof(tail);
	tailstart = size - tailsize;
	if (fseek(f, tailstart, SEEK_SET) || fread(tail, 1, (size_t)tailsize, f) != (size_t)tailsize)
		goto done;
	for (p = tail + tailsize - 22; p >= tail; p--)
		if (!memcmp(p, "PK\5\6", 4))
		{
			eocd = p;
			break;
		}
	if (!eocd)
	{
		*why = "no end-of-directory record";
		goto done;
	}
	entries = TC_U16(eocd + 10);
	cdsize = TC_U32(eocd + 12);
	cdoff = TC_U32(eocd + 16);
	if (cdoff == 0xFFFFFFFFu || entries == 0xFFFFu)
	{
		ok = true; // zip64: not checked here
		goto done;
	}
	if ((long)cdoff + (long)cdsize > size || cdsize > (32u << 20))
	{
		*why = "directory beyond the end of the file";
		goto done;
	}
	cd = (unsigned char *)Z_Malloc(cdsize + 1);
	if (fseek(f, (long)cdoff, SEEK_SET) || fread(cd, 1, cdsize, f) != cdsize)
	{
		*why = "directory unreadable";
		goto done;
	}
	for (i = 0, p = cd; i < entries; i++)
	{
		if ((size_t)(p - cd) + 46 > cdsize || memcmp(p, "PK\1\2", 4))
		{
			*why = "damaged directory";
			goto done;
		}
		off = TC_U32(p + 42);
		nlen = TC_U16(p + 28); elen = TC_U16(p + 30); clen = TC_U16(p + 32);
		if ((long)off + 4 > (long)cdoff || fseek(f, (long)off, SEEK_SET) || fread(local, 1, 4, f) != 4 || memcmp(local, "PK\3\4", 4))
		{
			*why = "an entry does not start at a file header";
			goto done;
		}
		p += 46 + nlen + elen + clen;
	}
	ok = true;
done:
	if (cd)
		Z_Free(cd);
	if (f)
		fclose(f);
	return ok;
}

// libcurl.c, a download finished: usable? A pack that is not is emptied, and the
// attempt is a failure (TouchCurl_Ended tries it again from byte 0)
static qbool TouchCurl_Verify(downloadinfo *di)
{
	char path[MAX_OSPATH];
	const char *why;
	if (di->loadtype != LOADTYPE_PAK || di->buffer || strncmp(di->filename, "dlcache/", 8))
		return true;
	TC_RealPath(path, sizeof(path), di->filename, "");
	if (TC_ZipOK(path, &why))
		return true;
	Con_Printf("Download of %s is not a whole pack (%s): deleted, fetching it again\n", di->filename + 8, why);
	TC_Empty(di->filename);
	TC_DropMeta(di->filename);
	return false;
}

static size_t TC_Header(char *buf, size_t size, size_t nmemb, void *vdi)
{
	size_t len = size * nmemb;
	downloadinfo *di = (downloadinfo *)vdi;
	tc_hdr_t *h;
	char line[260], *colon;
	size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
	for (h = tc_hdrs; h && h->di != di; h = h->next) {}
	if (!h)
		return len;
	memcpy(line, buf, n);
	line[n] = 0;
	while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' '))
		line[--n] = 0;
	if (!strncasecmp(line, "HTTP/", 5))
		return len; // a new response (a redirect has several)
	if (!(colon = strchr(line, ':')))
		return len;
	*colon++ = 0;
	while (*colon == ' ')
		colon++;
	if (!strcasecmp(line, "etag") || !strcasecmp(line, "last-modified"))
	{
		char value[220];
		dpsnprintf(value, sizeof(value), "%s: %s", line, colon);
		if (h->resuming && h->stored[0] && strcmp(h->stored, value) && !strncmp(h->stored, value, strlen(line) + 1))
		{
			h->mismatch = true;
			return 0; // abort: not the file the partial came from
		}
		if (!h->resuming || !h->stored[0] || !strcmp(h->stored, value))
			TC_WriteMeta(di->filename, value);
	}
	return len;
}

// libcurl.c, a download is set up (before RESUME_FROM is set): decide whether
// the partial on disk may be resumed, and listen for the server's validator
static void TouchCurl_Headers(downloadinfo *di)
{
	tc_hdr_t *h;
	if (di->buffer || di->post_content_type || strncmp(di->filename, "dlcache/", 8) || !qcurl_easy_setopt)
		return;
	h = (tc_hdr_t *)Z_Malloc(sizeof(*h));
	h->di = di;
	TC_ReadMeta(di->filename, h->stored, sizeof(h->stored));
	if (di->startpos > 0 && !h->stored[0])
	{
		// no record of where the partial came from: not resumed
		Con_Printf("Download of %s: %ld KB on disk with no record of its source, starting again\n", di->filename + 8, (long)(di->startpos / 1024));
		FS_Close(di->stream);
		TC_Empty(di->filename);
		di->stream = FS_OpenRealFile(di->filename, "ab", false);
		di->startpos = 0;
	}
	h->resuming = di->startpos > 0;
	if (!h->resuming)
		TC_WriteMeta(di->filename, "none"); // a partial with no validator from the server still resumes
	h->next = tc_hdrs;
	tc_hdrs = h;
	qcurl_easy_setopt(di->curle, (CURLoption)(CURLOPTTYPE_FUNCTIONPOINT + 79), TC_Header); // CURLOPT_HEADERFUNCTION
	qcurl_easy_setopt(di->curle, (CURLoption)(CURLOPTTYPE_OBJECTPOINT + 29), (void *)di);  // CURLOPT_HEADERDATA
}

static qbool TC_HdrMismatch(downloadinfo *di, qbool free_it)
{
	tc_hdr_t **link = &tc_hdrs, *h;
	qbool m = false;
	while ((h = *link))
	{
		if (h->di == di)
		{
			m = h->mismatch;
			if (free_it)
			{
				*link = h->next;
				Z_Free(h);
			}
			return m;
		}
		link = &h->next;
	}
	return false;
}

// delete every pk3 in dlcache that has no .meta (so is no partial download) and is not a whole pack
static void TouchCurl_Sweep(void)
{
#ifndef _WIN32
	char dir[MAX_OSPATH], path[MAX_OSPATH], fn[MAX_QPATH], meta[MAX_OSPATH];
	struct dirent *e;
	DIR *d;
	const char *why;
	dpsnprintf(dir, sizeof(dir), "%sdlcache", fs_gamedir);
	if (!(d = opendir(dir)))
		return;
	while ((e = readdir(d)))
	{
		size_t n = strlen(e->d_name);
		if (n < 5 || strcasecmp(e->d_name + n - 4, ".pk3"))
			continue;
		dpsnprintf(fn, sizeof(fn), "dlcache/%s", e->d_name);
		dpsnprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		dpsnprintf(meta, sizeof(meta), "%s.meta", path);
		if (access(meta, F_OK) == 0 || Curl_Find(fn))
			continue;
		if (!TC_ZipOK(path, &why))
		{
			Con_Printf("dlcache/%s is not a whole pack (%s): deleted\n", e->d_name, why);
			remove(path);
		}
	}
	closedir(d);
#endif
}

// join_retry (touch_joinfail.c): waiting retries go at once, the cache is swept
void TouchCurl_ManualRetry(void)
{
	tc_retry_t *r;
	for (r = tc_retries; r; r = r->next)
		if (r->due > 0)
		{
			r->due = host.realtime;
			r->tries = 0;
		}
	TouchCurl_Sweep();
}

// libcurl.c Curl_EndDownload, before the download is counted: true = a new
// attempt is planned, count nothing yet
static qbool TouchCurl_Ended(downloadinfo *di, int status, qbool ok, CURLcode error)
{
	qbool changed = TC_HdrMismatch(di, true), retry;
	if (ok)
	{
		TouchCurl_RetryDone(di);
		if (!strncmp(di->filename, "dlcache/", 8))
			TC_DropMeta(di->filename);
		return false;
	}
	if (changed || status == 3 /* CURL_DOWNLOAD_SERVERERROR: a 416 or another HTTP error */)
	{
		// the partial is no good (another file on the server, a range it cannot serve)
		if (!strncmp(di->filename, "dlcache/", 8))
		{
			TC_Empty(di->filename);
			TC_DropMeta(di->filename);
		}
		if (changed)
			Con_Printf("Download of %s: the file on the server changed, starting again\n", di->filename + 8);
	}
	retry = di->forthismap && TouchCurl_Retry(di, status, error);
	return retry;
}

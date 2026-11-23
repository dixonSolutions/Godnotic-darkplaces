/*
Godnotic: a picture of a map made from its BSP, for a join card when the map's
pack carries no levelshot (the Quake 1 maps of "Quake 1 Nostalgia Trip!" have
none: the pack holds maps/<map>.bsp/.ent/.mapinfo/.txt and nothing else).

A Quake 1 BSP (version 29) is read straight from its lumps: the world model's
faces are filled as triangles in an oblique top-down view (an orthographic
camera looking north-east from 52 degrees up), shaded by the face normal and
tinted by height, with a depth buffer. Sky and trigger/clip brushes are left
out, liquids are blue. No texture is needed, nothing is loaded into the
engine, so it can run the moment the BSP's bytes are in memory (a range read
of the server's map pack, touch_curlshot.h), long before the pack is mounted.

Pure C: no engine types. tests/touch-zipshot/bspshot_test.c includes this header.
*/
#ifndef TOUCH_BSPSHOT_H
#define TOUCH_BSPSHOT_H
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TBS_W 1024
#define TBS_H 576
#define TBS_MAX_FACES 200000
#define TBS_MAX_POLY 256

static int tbs_i32(const unsigned char *p) { return (int)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }
static int tbs_i16(const unsigned char *p) { return (int)(short)(p[0] | (p[1] << 8)); }
static int tbs_u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static float tbs_f32(const unsigned char *p) { union { uint32_t u; float f; } v; v.u = (uint32_t)tbs_i32(p); return v.f; }

/* 1 when the bytes are a Quake 1 BSP the renderer understands */
static int tbs_is_q1(const unsigned char *bsp, size_t n)
{
	return n > 4 + 15 * 8 && tbs_i32(bsp) == 29;
}

typedef struct { float x, y, z; } tbs_v3;

static int tbs_lump(const unsigned char *bsp, size_t n, int which, size_t *off, size_t *len)
{
	int o = tbs_i32(bsp + 4 + which * 8), l = tbs_i32(bsp + 8 + which * 8);
	if (o < 0 || l < 0 || (size_t)o > n || (size_t)l > n - (size_t)o) return 0;
	*off = (size_t)o; *len = (size_t)l;
	return 1;
}

/* the colour of a pixel: height 0..1 and light 0..1 -> rgb */
static void tbs_colour(float height, float light, int kind, unsigned char *rgb)
{
	/* kind 0 solid ground and walls, 1 liquid. Teal valleys to warm sand peaks. */
	static const float lo[3] = { 0.16f, 0.42f, 0.56f }, mid[3] = { 0.55f, 0.62f, 0.45f }, hi[3] = { 0.96f, 0.80f, 0.52f };
	float c[3], k, a, b;
	int i;
	if (height < 0) height = 0;
	if (height > 1) height = 1;
	for (i = 0; i < 3; i++)
	{
		if (height < 0.5f) { k = height * 2; a = lo[i]; b = mid[i]; }
		else { k = (height - 0.5f) * 2; a = mid[i]; b = hi[i]; }
		c[i] = a + (b - a) * k;
	}
	if (kind == 1) { c[0] = 0.10f; c[1] = 0.32f; c[2] = 0.78f; }
	for (i = 0; i < 3; i++)
	{
		float v = c[i] * light;
		rgb[i] = (unsigned char)(v < 0 ? 0 : v > 1 ? 255 : v * 255.0f);
	}
}

/*
Render into rgb (w*h*3). Returns 1 when something was drawn. `scratch` is not
needed: the depth buffer is allocated here.
*/
static int tbs_render_q1(const unsigned char *bsp, size_t n, int w, int h, unsigned char *rgb)
{
	size_t vo, vl, eo, el, so, sl, fo, fl, to, tl, po, pl, mo, ml, xo, xl;
	const unsigned char *V, *E, *S, *F, *T, *P, *M, *X;
	int nvert, nedge, nsurf, nface, ntexinfo, nplanes, nmiptex = 0, first, count, f, i, x, y;
	float *depth = NULL;
	tbs_v3 *pts = NULL;
	float minz = 1e30f, maxz = -1e30f, minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
	const float yaw = 0.7853982f, pitch = 0.9075712f; /* 45 and 52 degrees */
	float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
	float scale, ox, oy, cx, cyy, lightdir[3];
	int drawn = 0;
	if (!tbs_is_q1(bsp, n) || w < 16 || h < 16) return 0;
	if (!tbs_lump(bsp, n, 3, &vo, &vl) || !tbs_lump(bsp, n, 12, &eo, &el) || !tbs_lump(bsp, n, 13, &so, &sl) ||
	    !tbs_lump(bsp, n, 7, &fo, &fl) || !tbs_lump(bsp, n, 6, &to, &tl) || !tbs_lump(bsp, n, 1, &po, &pl) ||
	    !tbs_lump(bsp, n, 14, &mo, &ml) || !tbs_lump(bsp, n, 2, &xo, &xl))
		return 0;
	V = bsp + vo; E = bsp + eo; S = bsp + so; F = bsp + fo; T = bsp + to; P = bsp + po; M = bsp + mo; X = bsp + xo;
	nvert = (int)(vl / 12); nedge = (int)(el / 4); nsurf = (int)(sl / 4); nface = (int)(fl / 20); ntexinfo = (int)(tl / 40); nplanes = (int)(pl / 20);
	if (ml < 64 || nvert < 3 || nface < 1 || nface > TBS_MAX_FACES) return 0;
	if (xl >= 4) nmiptex = tbs_i32(X);
	if (nmiptex < 0 || (size_t)nmiptex * 4 + 4 > xl) nmiptex = 0;
	/* model 0: mins, maxs, origin (36 bytes), headnode[4], visleafs, then firstface and numfaces */
	first = tbs_i32(M + 56);
	count = tbs_i32(M + 60);
	if (first < 0 || count <= 0 || first > nface || count > nface - first) return 0;

	pts = (tbs_v3 *)malloc(sizeof(tbs_v3) * (size_t)nvert);
	depth = (float *)malloc(sizeof(float) * (size_t)w * (size_t)h);
	if (!pts || !depth) { free(pts); free(depth); return 0; }

	/* the camera: rotate about z by yaw, then tilt */
	for (i = 0; i < nvert; i++)
	{
		float px = tbs_f32(V + i * 12), py = tbs_f32(V + i * 12 + 4), pz = tbs_f32(V + i * 12 + 8);
		float rx = px * cy - py * sy, ry = px * sy + py * cy;
		pts[i].x = rx;                       /* screen x */
		pts[i].y = ry * sp + pz * cp;        /* screen up */
		pts[i].z = -ry * cp + pz * sp;       /* depth toward the camera */
		if (!(px == px) || !(py == py) || !(pz == pz)) { free(pts); free(depth); return 0; }
	}
	/* bounds of the world model's own vertices */
	for (f = first; f < first + count; f++)
	{
		int fe = tbs_i32(F + f * 20 + 4), ne = tbs_u16(F + f * 20 + 8);
		if (fe < 0 || ne < 3 || ne > TBS_MAX_POLY || fe > nsurf || ne > nsurf - fe) continue;
		for (i = 0; i < ne; i++)
		{
			int se = tbs_i32(S + (fe + i) * 4), ei = se < 0 ? -se : se, vi;
			if (ei >= nedge) break;
			vi = tbs_u16(E + ei * 4 + (se < 0 ? 2 : 0));
			if (vi >= nvert) break;
			if (pts[vi].x < minx) minx = pts[vi].x;
			if (pts[vi].x > maxx) maxx = pts[vi].x;
			if (pts[vi].y < miny) miny = pts[vi].y;
			if (pts[vi].y > maxy) maxy = pts[vi].y;
			/* height from the true z: recover it from screen up and depth */
			{
				float z = pts[vi].y * cp + pts[vi].z * sp;
				if (z < minz) minz = z;
				if (z > maxz) maxz = z;
			}
		}
	}
	if (maxx <= minx || maxy <= miny) { free(pts); free(depth); return 0; }
	scale = 0.94f * ((float)w / (maxx - minx) < (float)h / (maxy - miny) ? (float)w / (maxx - minx) : (float)h / (maxy - miny));
	cx = (minx + maxx) * 0.5f; cyy = (miny + maxy) * 0.5f;
	ox = w * 0.5f; oy = h * 0.5f;

	/* a dark blue backdrop, the card's own colours */
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			float t = (float)y / (float)h, d;
			float dx = ((float)x / (float)w - 0.5f), dy = (t - 0.5f);
			d = 1.0f - 0.9f * (dx * dx + dy * dy);
			unsigned char *p = rgb + ((size_t)y * w + x) * 3;
			p[0] = (unsigned char)((11 + (29 - 11) * t) * d);
			p[1] = (unsigned char)((23 + (74 - 23) * t) * d);
			p[2] = (unsigned char)((48 + (140 - 48) * t) * d);
			depth[(size_t)y * w + x] = -1e30f;
		}

	lightdir[0] = -0.45f; lightdir[1] = -0.35f; lightdir[2] = 0.82f;
	for (f = first; f < first + count; f++)
	{
		const unsigned char *fp = F + f * 20;
		int plane = tbs_u16(fp), side = tbs_i16(fp + 2), fe = tbs_i32(fp + 4), ne = tbs_u16(fp + 8), ti = tbs_i16(fp + 10);
		int kind = 0, k;
		float nx = 0, ny = 0, nz = 1, light;
		float sxp[TBS_MAX_POLY], syp[TBS_MAX_POLY], szp[TBS_MAX_POLY], hgt[TBS_MAX_POLY];
		if (fe < 0 || ne < 3 || ne > TBS_MAX_POLY || fe > nsurf || ne > nsurf - fe) continue;
		if (ti >= 0 && ti < ntexinfo && nmiptex)
		{
			int mt = tbs_i32(T + ti * 40 + 32), mflags = tbs_i32(T + ti * 40 + 36);
			if (mt >= 0 && mt < nmiptex)
			{
				int mofs = tbs_i32(X + 4 + mt * 4);
				if (mofs > 0 && (size_t)mofs + 16 <= xl)
				{
					const char *name = (const char *)(X + mofs);
					if (!strncmp(name, "sky", 3) || !strncmp(name, "trigger", 7) || !strncmp(name, "clip", 4) ||
					    !strncmp(name, "origin", 6) || !strncmp(name, "aaatrigger", 10) || !strncmp(name, "skip", 4))
						continue;
					if (name[0] == '*') kind = 1;
				}
			}
			(void)mflags;
		}
		if (plane >= 0 && plane < nplanes)
		{
			nx = tbs_f32(P + plane * 20); ny = tbs_f32(P + plane * 20 + 4); nz = tbs_f32(P + plane * 20 + 8);
			if (side) { nx = -nx; ny = -ny; nz = -nz; }
		}
		/* a dollhouse view: faces turned away from the camera (ceilings, the outer side of walls) are left out */
		{
			float rny = nx * sy + ny * cy;
			if (-rny * cp + nz * sp <= 0.02f) continue;
		}
		light = 0.42f + 0.58f * (nx * lightdir[0] + ny * lightdir[1] + nz * lightdir[2]);
		if (light < 0.40f) light = 0.40f;
		if (light > 1.0f) light = 1.0f;
		for (i = 0, k = 0; i < ne; i++)
		{
			int se = tbs_i32(S + (fe + i) * 4), ei = se < 0 ? -se : se, vi;
			if (ei >= nedge) { k = 0; break; }
			vi = tbs_u16(E + ei * 4 + (se < 0 ? 2 : 0));
			if (vi >= nvert) { k = 0; break; }
			sxp[k] = ox + (pts[vi].x - cx) * scale;
			syp[k] = oy - (pts[vi].y - cyy) * scale;
			szp[k] = pts[vi].z;
			hgt[k] = maxz > minz ? ((pts[vi].y * cp + pts[vi].z * sp) - minz) / (maxz - minz) : 0.5f;
			k++;
		}
		if (k < 3) continue;
		/* a fan: every triangle (0, j, j+1) */
		for (i = 1; i + 1 < k; i++)
		{
			float x0 = sxp[0], y0 = syp[0], x1 = sxp[i], y1 = syp[i], x2 = sxp[i + 1], y2 = syp[i + 1];
			float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
			int bx0, bx1, by0, by1;
			if (area > -1e-6f && area < 1e-6f) continue;
			bx0 = (int)floorf(fminf(x0, fminf(x1, x2))); bx1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
			by0 = (int)floorf(fminf(y0, fminf(y1, y2))); by1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
			if (bx0 < 0) bx0 = 0;
			if (by0 < 0) by0 = 0;
			if (bx1 > w - 1) bx1 = w - 1;
			if (by1 > h - 1) by1 = h - 1;
			for (y = by0; y <= by1; y++)
				for (x = bx0; x <= bx1; x++)
				{
					float px = (float)x + 0.5f, py = (float)y + 0.5f;
					float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
					float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
					float w2 = 1.0f - w0 - w1;
					float z, hh, shade, edge;
					unsigned char *p;
					if (w0 < -0.002f || w1 < -0.002f || w2 < -0.002f) continue;
					z = w0 * szp[0] + w1 * szp[i] + w2 * szp[i + 1];
					if (z <= depth[(size_t)y * w + x]) continue;
					depth[(size_t)y * w + x] = z;
					hh = w0 * hgt[0] + w1 * hgt[i] + w2 * hgt[i + 1];
					/* a faint edge line on the polygon's own outline, and the far side a little darker */
					edge = (w0 < 0.012f || w1 < 0.012f || w2 < 0.012f) && i == 1 ? 0.78f : 1.0f;
					shade = light * edge;
					p = rgb + ((size_t)y * w + x) * 3;
					tbs_colour(hh, shade, kind, p);
					drawn++;
				}
		}
	}
	free(pts);
	free(depth);
	return drawn > 64;
}

/* ---- a PNG of the rgb buffer ---- */
static uint32_t tbs_crc_table[256];
static int tbs_crc_ready;
static uint32_t tbs_crc32(uint32_t crc, const unsigned char *p, size_t n)
{
	size_t i;
	if (!tbs_crc_ready)
	{
		uint32_t c, k; int b;
		for (k = 0; k < 256; k++) { c = k; for (b = 0; b < 8; b++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1; tbs_crc_table[k] = c; }
		tbs_crc_ready = 1;
	}
	crc = ~crc;
	for (i = 0; i < n; i++) crc = tbs_crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
	return ~crc;
}
static void tbs_be32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
static unsigned char *tbs_chunk(unsigned char *o, const char *type, const unsigned char *data, size_t n)
{
	uint32_t crc;
	tbs_be32(o, (uint32_t)n); memcpy(o + 4, type, 4); if (n) memcpy(o + 8, data, n);
	crc = tbs_crc32(0, o + 4, n + 4);
	tbs_be32(o + 8 + n, crc);
	return o + 12 + n;
}
/*
 rgb w*h*3 -> PNG bytes (malloc'ed, *outn set), 0 on failure. `deflate` is an optional raw-deflate
 compressor (the engine's FS_Deflate); without one the data is stored, which is bigger but always valid.
*/
typedef unsigned char *(*tbs_deflate_fn)(const unsigned char *in, size_t n, size_t *outn);
static unsigned char *tbs_png(const unsigned char *rgb, int w, int h, tbs_deflate_fn deflate, size_t *outn)
{
	size_t raw = (size_t)(w * 3 + 1) * (size_t)h, i, zn = 0, cap;
	unsigned char *scan, *z = NULL, *png, *o, hdr[13];
	uint32_t a = 1, b = 0;
	int y, owned = 0;
	scan = (unsigned char *)malloc(raw);
	if (!scan) return NULL;
	for (y = 0; y < h; y++)
	{
		const unsigned char *r = rgb + (size_t)y * w * 3;
		unsigned char *d = scan + (size_t)y * (w * 3 + 1);
		int x;
		d[0] = 2; /* filter Up: the rows of a shaded render repeat a lot, which the deflater likes */
		for (x = 0; x < w * 3; x++) d[1 + x] = y ? (unsigned char)(r[x] - r[x - w * 3]) : r[x];
	}
	for (i = 0; i < raw; i++) { a = (a + scan[i]) % 65521u; b = (b + a) % 65521u; }
	if (deflate)
	{
		size_t dn = 0;
		unsigned char *d = deflate(scan, raw, &dn);
		if (d && dn)
		{
			z = (unsigned char *)malloc(dn + 6);
			if (z) { z[0] = 0x78; z[1] = 0x9c; memcpy(z + 2, d, dn); tbs_be32(z + 2 + dn, (b << 16) | a); zn = dn + 6; owned = 1; }
			free(d);
		}
	}
	if (!z)
	{
		size_t blocks = (raw + 65534) / 65535, pos = 0;
		z = (unsigned char *)malloc(raw + blocks * 5 + 6);
		if (!z) { free(scan); return NULL; }
		owned = 1; z[0] = 0x78; z[1] = 0x01; zn = 2;
		while (pos < raw)
		{
			size_t chunk = raw - pos > 65535 ? 65535 : raw - pos;
			z[zn++] = pos + chunk == raw; z[zn++] = (unsigned char)chunk; z[zn++] = (unsigned char)(chunk >> 8);
			z[zn++] = (unsigned char)~chunk; z[zn++] = (unsigned char)(~chunk >> 8);
			memcpy(z + zn, scan + pos, chunk); zn += chunk; pos += chunk;
		}
		tbs_be32(z + zn, (b << 16) | a); zn += 4;
	}
	free(scan);
	cap = 8 + 25 + (zn + 12) + 12;
	png = (unsigned char *)malloc(cap);
	if (!png) { if (owned) free(z); return NULL; }
	memcpy(png, "\x89PNG\r\n\x1a\n", 8);
	tbs_be32(hdr, (uint32_t)w); tbs_be32(hdr + 4, (uint32_t)h); hdr[8] = 8; hdr[9] = 2; hdr[10] = 0; hdr[11] = 0; hdr[12] = 0;
	o = tbs_chunk(png + 8, "IHDR", hdr, 13);
	o = tbs_chunk(o, "IDAT", z, zn);
	o = tbs_chunk(o, "IEND", NULL, 0);
	free(z);
	*outn = (size_t)(o - png);
	return png;
}
#endif

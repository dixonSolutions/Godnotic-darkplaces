/* Bounded ZIP central-directory parsing for the early levelshot fetch. */
#ifndef TOUCH_ZIPSHOT_H
#define TOUCH_ZIPSHOT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define TZS_MAX_DIR (2u * 1024u * 1024u)
#define TZS_MAX_IMAGE (4u * 1024u * 1024u)
#define TZS_MAX_BSP_PACKED (8u * 1024u * 1024u)
#define TZS_MAX_BSP (48u * 1024u * 1024u)
static unsigned tzs_u16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t tzs_u32(const unsigned char *p) { return tzs_u16(p) | ((uint32_t)tzs_u16(p+2) << 16); }
typedef struct tzs_entry_s { uint32_t offset, compressed, size, crc; unsigned method; char ext[5]; unsigned namelen; int bsp; } tzs_entry_t;
/* EOCD must end at the end of the file, avoiding signatures inside comments. */
static int tzs_directory(const unsigned char *p, size_t n, uint64_t total, uint32_t *offset, uint32_t *size)
{
	size_t i;
	if (n < 22) return 0;
	for (i = n - 22 + 1; i-- > 0; )
	{
		uint32_t off, len;
		if (tzs_u32(p+i) != 0x06054b50u || i + 22u + tzs_u16(p+i+20) != n) continue;
		if (tzs_u16(p+i+4) || tzs_u16(p+i+6) || tzs_u16(p+i+8) != tzs_u16(p+i+10)) return 0;
		len = tzs_u32(p+i+12); off = tzs_u32(p+i+16);
		if (!len || len > TZS_MAX_DIR || (uint64_t)off + len > total - (n-i)) return 0;
		*offset = off; *size = len; return 1;
	}
	return 0;
}
/* one directory pass: maps/<map><ext> or levelshots/<map><ext>, an image, or (bsp) maps/<map>.bsp */
static int tzs_find_in(const unsigned char *p, size_t n, const char *dir, const char *map, const char *const *exts, int bsp, tzs_entry_t *out)
{
	size_t pos = 0, m = strlen(map), d = strlen(dir);
	while (pos + 46 <= n)
	{
		unsigned name, extra, comment, method, flags;
		const unsigned char *q = p + pos, *s;
		size_t len;
		int e;
		if (tzs_u32(q) != 0x02014b50u) return 0;
		name = tzs_u16(q+28); extra = tzs_u16(q+30); comment = tzs_u16(q+32);
		len = 46u + name + extra + comment;
		if (len > n-pos) return 0;
		flags = tzs_u16(q+8); method = tzs_u16(q+10); s = q+46;
		if (!(flags & 1) && (method == 0 || method == 8) && name == d + m + 4 && !memcmp(s, dir, d) && !memcmp(s+d, map, m))
			for (e = 0; exts[e]; e++)
				if (!memcmp(s+d+m, exts[e], 4))
				{
					out->compressed = tzs_u32(q+20); out->size = tzs_u32(q+24);
					out->offset = tzs_u32(q+42); out->crc = tzs_u32(q+16); out->method = method;
					out->namelen = name; out->bsp = bsp;
					memcpy(out->ext, s+d+m, 4); out->ext[4] = 0;
					if (bsp)
					{
						if (!out->size || out->size > TZS_MAX_BSP || !out->compressed || out->compressed > TZS_MAX_BSP_PACKED || (method == 0 && out->size != out->compressed)) return 0;
					}
					else if (!out->size || out->size > TZS_MAX_IMAGE || !out->compressed || out->compressed > TZS_MAX_IMAGE || (method == 0 && out->size != out->compressed)) return 0;
					return 1;
				}
		pos += len;
	}
	return 0;
}
/* the map's own picture: maps/<map>.jpg|png, else levelshots/<map>.jpg|png (the Quake 3 place) */
static int tzs_find(const unsigned char *p, size_t n, const char *map, tzs_entry_t *out)
{
	static const char *const exts[] = { ".jpg", ".png", NULL };
	return tzs_find_in(p, n, "maps/", map, exts, 0, out) || tzs_find_in(p, n, "levelshots/", map, exts, 0, out);
}
/* no picture in the pack: its BSP, from which a picture can be made (touch_bspshot.h) */
static int tzs_find_bsp(const unsigned char *p, size_t n, const char *map, tzs_entry_t *out)
{
	static const char *const exts[] = { ".bsp", NULL };
	return tzs_find_in(p, n, "maps/", map, exts, 1, out);
}
static uint32_t tzs_crc(const unsigned char *p, size_t n)
{
	uint32_t c = ~0u; size_t i; int b;
	for (i = 0; i < n; i++) { c ^= p[i]; for (b = 0; b < 8; b++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u))); }
	return ~c;
}
#endif

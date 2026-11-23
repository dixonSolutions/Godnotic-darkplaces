/*
Godnotic: custom player models, as pure functions (no engine headers) shared by
touch_playermodel.c and tests/touch-playermodel/playermodel_test.c.

An upload is a .pk3/.zip (or one bare model file). Before it is kept it must be
something Xonotic can use as a player model:

  - the format is one DarkPlaces loads for players: iqm, dpm, md3, zym
  - models/player/<name>.<fmt> has a real header and at least one frame
  - models/player/<name>_<n>.txt exists (the list entry: name, species, ...)
  - the animations Xonotic plays are there: a <name>.<fmt>.framegroups file with
    at least TPM_MIN_GROUPS lines, or, for iqm, every animation named in the file
  - a skin: <name>_<n>.skin, or a texture beside the model
  - the size is sane, and the name does not replace a stock model

Everything that needs the filesystem is gathered by the engine side into a
tpm_facts_t; TPM_Decide turns the facts into compatible or one plain reason.
*/
#ifndef TOUCH_PLAYERMODEL_LOGIC_H
#define TOUCH_PLAYERMODEL_LOGIC_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef TOUCH_SNPRINTF
#define TOUCH_SNPRINTF snprintf
#endif

#define TPM_MAX_PACK (32u * 1024u * 1024u)
#define TPM_MAX_MODEL (16u * 1024u * 1024u)
#define TPM_MIN_MODEL 256u
#define TPM_MIN_GROUPS 23   // die1 .. backleft, animdecide.qh

typedef struct
{
	int has_model;        // models/player/<name>.<fmt> found
	char fmt[8];          // iqm dpm md3 zym
	long model_size;
	int header_ok;
	int frames;           // frames (anims for iqm when it has no frames)
	int has_txt;          // <name>_<n>.txt
	int framegroup_lines; // lines in <name>.<fmt>.framegroups (0 when absent)
	int iqm_anims_named;  // of the 23 required, how many an iqm names itself
	int has_skin;         // <name>_<n>.skin or a texture beside the model
	int replaces_stock;   // the name is already a model that is not ours
	long pack_size;
} tpm_facts_t;

static inline unsigned tpm_le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static inline unsigned tpm_be32(const unsigned char *p) { return p[3] | (p[2] << 8) | (p[1] << 16) | ((unsigned)p[0] << 24); }

// "iqm" "dpm" "md3" "zym" from the magic, or "" when it is none of them.
static inline const char *TPM_FormatOf(const unsigned char *d, size_t n)
{
	if (n >= 16 && !memcmp(d, "INTERQUAKEMODEL", 16)) return "iqm";
	if (n >= 16 && !memcmp(d, "DARKPLACESMODEL", 16)) return "dpm";
	if (n >= 4 && !memcmp(d, "IDP3", 4)) return "md3";
	if (n >= 12 && !memcmp(d, "ZYMOTICMODEL", 12)) return "zym";
	return "";
}

// Frame count from the header (anim count for an iqm with no frame table); 0 when the
// header is too short or wrong.
static inline int TPM_Frames(const unsigned char *d, size_t n)
{
	const char *f = TPM_FormatOf(d, n);
	if (!strcmp(f, "iqm"))
	{
		if (n < 16 + 4 * 21 || tpm_le32(d + 16) != 2) return 0;
		unsigned frames = tpm_le32(d + 16 + 4 * 19), anims = tpm_le32(d + 16 + 4 * 17);
		return (int)(frames ? frames : anims);
	}
	if (!strcmp(f, "dpm")) return n >= 72 ? (int)tpm_be32(d + 68) : 0;
	if (!strcmp(f, "md3")) return n >= 80 ? (int)tpm_le32(d + 76) : 0;
	if (!strcmp(f, "zym")) return n >= 68 ? (int)tpm_be32(d + 64) : 0;
	return 0;
}

// Non-empty lines of a .framegroups file that start with a number: "first count fps loop".
static inline int TPM_FramegroupLines(const char *s, size_t n)
{
	int lines = 0, at_start = 1;
	for (size_t i = 0; i < n; ++i)
	{
		char c = s[i];
		if (c == '\n') { at_start = 1; continue; }
		if (at_start && c != ' ' && c != '\t' && c != '\r')
		{
			if (c >= '0' && c <= '9') ++lines;
			at_start = 0;
		}
	}
	return lines;
}

// The first alias of each animation Xonotic needs (animdecide.qh), in its order.
static const char *const tpm_required_anims[TPM_MIN_GROUPS] = {
	"dieone", "dietwo", "draw", "duck", "duckwalk", "duckjump", "duckidle", "idle", "jump", "painone",
	"paintwo", "shoot", "taunt", "run", "runbackwards", "strafeleft", "straferight", "deadone", "deadtwo",
	"forwardright", "forwardleft", "backright", "backleft"
};

// How many of the required animations an iqm names (its anim table: name, first, count, fps, flags).
static inline int TPM_IqmAnimsNamed(const unsigned char *d, size_t n)
{
	if (n < 16 + 4 * 21 || memcmp(d, "INTERQUAKEMODEL", 16) || tpm_le32(d + 16) != 2) return 0;
	unsigned ntext = tpm_le32(d + 16 + 4 * 3), otext = tpm_le32(d + 16 + 4 * 4);
	unsigned nanims = tpm_le32(d + 16 + 4 * 17), oanims = tpm_le32(d + 16 + 4 * 18);
	if (otext > n || ntext > n - otext || oanims > n || nanims > 4096 || (size_t)nanims * 20 > n - oanims) return 0;
	int named = 0;
	for (int r = 0; r < TPM_MIN_GROUPS; ++r)
	{
		for (unsigned a = 0; a < nanims; ++a)
		{
			unsigned nm = tpm_le32(d + oanims + a * 20);
			if (nm >= ntext) continue;
			const char *s = (const char *)d + otext + nm;
			size_t room = ntext - nm;
			if (strnlen(s, room) < room && !strcmp(s, tpm_required_anims[r])) { ++named; break; }
		}
	}
	return named;
}

// 1 when the facts make a usable player model; else 0 with a plain reason in why.
static inline int TPM_Decide(const tpm_facts_t *f, char *why, size_t whysize)
{
#define TPM_NO(...) do { TOUCH_SNPRINTF(why, whysize, __VA_ARGS__); return 0; } while (0)
	if (f->pack_size > (long)TPM_MAX_PACK) TPM_NO("The file is over %d MB.", (int)(TPM_MAX_PACK >> 20));
	if (!f->has_model) TPM_NO("No model in models/player/ (iqm, dpm, md3 or zym).");
	if (f->replaces_stock) TPM_NO("A model with this name is already in the game; rename yours.");
	if (f->model_size < (long)TPM_MIN_MODEL || f->model_size > (long)TPM_MAX_MODEL)
		TPM_NO("The model is %ld KB; a player model is between 1 KB and %d MB.", f->model_size >> 10, (int)(TPM_MAX_MODEL >> 20));
	if (!f->header_ok) TPM_NO("The file is not a valid %s model.", f->fmt);
	if (f->frames < 1) TPM_NO("The model has no frames.");
	if (!f->has_txt) TPM_NO("Missing <name>.<fmt>_0.txt (the model's name and details).");
	if (f->framegroup_lines < TPM_MIN_GROUPS && f->iqm_anims_named < TPM_MIN_GROUPS)
	{
		if (f->framegroup_lines > 0)
			TPM_NO("Only %d of the %d animations Xonotic needs are listed in .framegroups.", f->framegroup_lines, TPM_MIN_GROUPS);
		TPM_NO("No animations: needs a .framegroups file with the %d groups Xonotic plays (idle, run, jump ...).", TPM_MIN_GROUPS);
	}
	if (!f->has_skin) TPM_NO("No skin: needs <name>.<fmt>_0.skin or a texture beside the model.");
	return 1;
#undef TPM_NO
}

// ------------------------------------------------------------------ names

// A lower-case file stem of [a-z0-9_-], at most 40 chars, never empty. in may carry a path and ext.
static inline void TPM_Stem(const char *in, char *out, size_t size)
{
	const char *b = in;
	for (const char *p = in; *p; ++p)
		if (*p == '/' || *p == '\\') b = p + 1;
	size_t n = 0, end = strlen(b);
	for (size_t i = end; i > 0; --i)
		if (b[i - 1] == '.') { end = i - 1; break; }
	for (size_t i = 0; i < end && n < 40 && n + 1 < size; ++i)
	{
		char c = b[i];
		if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
		out[n++] = ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') ? c : '_';
	}
	if (!n) { TOUCH_SNPRINTF(out, size, "model"); return; }
	out[n] = 0;
}

// ------------------------------------------------------------------ zip

typedef struct { char name[256]; unsigned size; int method; } tpm_zent_t;

// The names in a zip's central directory; returns the count, or -1 when it is not a zip.
static inline int TPM_ZipList(const unsigned char *d, size_t n, tpm_zent_t *out, int max)
{
	if (n < 22 || memcmp(d, "PK\3\4", 4)) return -1;
	size_t e = n - 22, lim = n > 22 + 65535 ? n - 22 - 65535 : 0;
	while (memcmp(d + e, "PK\5\6", 4))
	{
		if (e == lim) return -1;
		--e;
	}
	unsigned count = d[e + 10] | (d[e + 11] << 8), cdsize = tpm_le32(d + e + 12), cdofs = tpm_le32(d + e + 16);
	if ((size_t)cdofs > n || (size_t)cdsize > n - cdofs) return -1;
	size_t p = cdofs, end = cdofs + cdsize;
	int k = 0;
	for (unsigned i = 0; i < count && p + 46 <= end; ++i)
	{
		if (memcmp(d + p, "PK\1\2", 4)) return -1;
		unsigned nl = d[p + 28] | (d[p + 29] << 8), xl = d[p + 30] | (d[p + 31] << 8), cl = d[p + 32] | (d[p + 33] << 8);
		if (p + 46 + nl > end) return -1;
		if (k < max)
		{
			size_t m = nl < sizeof(out[k].name) - 1 ? nl : sizeof(out[k].name) - 1;
			memcpy(out[k].name, d + p + 46, m);
			out[k].name[m] = 0;
			out[k].size = tpm_le32(d + p + 24);
			out[k].method = d[p + 10] | (d[p + 11] << 8);
			++k;
		}
		p += 46 + nl + xl + cl;
	}
	return k;
}

static inline unsigned tpm_crc32(const unsigned char *d, size_t n)
{
	unsigned c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; ++i)
	{
		c ^= d[i];
		for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
	}
	return ~c;
}

static inline void tpm_put16(unsigned char *p, unsigned v) { p[0] = v & 255; p[1] = (v >> 8) & 255; }
static inline void tpm_put32(unsigned char *p, unsigned v) { tpm_put16(p, v & 0xFFFF); tpm_put16(p + 2, v >> 16); }

// A zip of stored (uncompressed) entries. out must hold 128 + sum(46+30+2*namelen+size). Returns bytes.
static inline size_t TPM_StoredZip(unsigned char *out, int count, const char *const *names, const unsigned char *const *data, const size_t *sizes)
{
	size_t p = 0, cd;
	size_t ofs[8];
	if (count > 8) count = 8;
	for (int i = 0; i < count; ++i)
	{
		size_t nl = strlen(names[i]);
		ofs[i] = p;
		memcpy(out + p, "PK\3\4", 4); tpm_put16(out + p + 4, 10); tpm_put16(out + p + 6, 0); tpm_put16(out + p + 8, 0);
		tpm_put16(out + p + 10, 0); tpm_put16(out + p + 12, 0x21);
		tpm_put32(out + p + 14, tpm_crc32(data[i], sizes[i])); tpm_put32(out + p + 18, (unsigned)sizes[i]); tpm_put32(out + p + 22, (unsigned)sizes[i]);
		tpm_put16(out + p + 26, (unsigned)nl); tpm_put16(out + p + 28, 0);
		memcpy(out + p + 30, names[i], nl); memcpy(out + p + 30 + nl, data[i], sizes[i]);
		p += 30 + nl + sizes[i];
	}
	cd = p;
	for (int i = 0; i < count; ++i)
	{
		size_t nl = strlen(names[i]);
		memcpy(out + p, "PK\1\2", 4); tpm_put16(out + p + 4, 20); tpm_put16(out + p + 6, 10); tpm_put16(out + p + 8, 0);
		tpm_put16(out + p + 10, 0); tpm_put16(out + p + 12, 0); tpm_put16(out + p + 14, 0x21);
		tpm_put32(out + p + 16, tpm_crc32(data[i], sizes[i])); tpm_put32(out + p + 20, (unsigned)sizes[i]); tpm_put32(out + p + 24, (unsigned)sizes[i]);
		tpm_put16(out + p + 28, (unsigned)nl); tpm_put16(out + p + 30, 0); tpm_put16(out + p + 32, 0); tpm_put16(out + p + 34, 0);
		tpm_put16(out + p + 36, 0); tpm_put32(out + p + 38, 0); tpm_put32(out + p + 42, (unsigned)ofs[i]);
		memcpy(out + p + 46, names[i], nl);
		p += 46 + nl;
	}
	memcpy(out + p, "PK\5\6", 4); tpm_put16(out + p + 4, 0); tpm_put16(out + p + 6, 0);
	tpm_put16(out + p + 8, (unsigned)count); tpm_put16(out + p + 10, (unsigned)count);
	tpm_put32(out + p + 12, (unsigned)(p - cd)); tpm_put32(out + p + 16, (unsigned)cd); tpm_put16(out + p + 20, 0);
	return p + 22;
}

#endif

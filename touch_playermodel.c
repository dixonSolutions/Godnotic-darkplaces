// SPDX-License-Identifier: GPL-3.0-or-later
/*
Godnotic: uploaded player models.

Settings > Player lets the player pick a model file or a .pk3/.zip with the system file
picker. The renderer hands the engine a readable path (Android: a copy of the document in
the app's own folder; Linux: the portal's path); uib_pm_import checks it
(touch_playermodel_logic.h), keeps it as <gamedir>/godnotic-pm-<name>.pk3, rescans the
file system so models/player/ lists it next to the stock ones, and records it in
godnotic-playermodels.txt so the menu can tell an uploaded model from a stock one.

A bare model file is wrapped in a pk3 of its own (the model and a <name>_0.txt), which
only passes the check when it needs nothing else. Nothing is deleted except by
uib_pm_delete, which the player asks for with the model's delete button.

A custom model is drawn on other players' screens only if they have the same files
(the server sends the model name, not the model): the page says so.
*/
#include "quakedef.h"
#include "progsvm.h"
#include "prvm_cmds.h"
#include "fs.h"
#include "touch_playermodel.h"
#define TOUCH_SNPRINTF dpsnprintf
#include "touch_playermodel_logic.h"

#define PM_REGISTRY "godnotic-playermodels.txt"
#define PM_PREFIX "godnotic-pm-"

static cvar_t uib_pm = {CF_CLIENT | CF_READONLY, "uib_pm", "1", "Godnotic: the engine can import and check uploaded player models (uib_pm_import)"};
static char pm_report[320] = "err\tNothing imported yet.";

static void pm_fail(const char *why) { dpsnprintf(pm_report, sizeof(pm_report), "err\t%s", why); }

static char *pm_load_vfs(const char *name, fs_offset_t *size)
{
	return (char *)FS_LoadFile(name, tempmempool, true, size);
}

// ---------------------------------------------------------------- registry
// lines: pack \t model \t skin \t title

static char *pm_registry(fs_offset_t *size)
{
	char *d = pm_load_vfs(PM_REGISTRY, size);
	if (!d) { *size = 0; d = (char *)Mem_Alloc(tempmempool, 1); d[0] = 0; return d; }
	char *z = (char *)Mem_Alloc(tempmempool, (size_t)*size + 1);
	memcpy(z, d, (size_t)*size);
	z[*size] = 0;
	Mem_Free(d);
	return z;
}

// rewrite the registry without lines whose pack is `drop`, then append `add` (may be NULL)
static void pm_registry_edit(const char *drop, const char *add)
{
	fs_offset_t n;
	char *r = pm_registry(&n);
	size_t cap = (size_t)n + (add ? strlen(add) : 0) + 2;
	char *out = (char *)Mem_Alloc(tempmempool, cap);
	size_t o = 0;
	for (char *line = r; *line; )
	{
		char *e = strchr(line, '\n');
		size_t len = e ? (size_t)(e - line) : strlen(line);
		size_t pl = strcspn(line, "\t");
		int keep = len > 0 && !(drop && strlen(drop) == pl && !strncmp(line, drop, pl));
		if (keep) { memcpy(out + o, line, len); o += len; out[o++] = '\n'; }
		line += len + (e ? 1 : 0);
	}
	if (add) { size_t l = strlen(add); memcpy(out + o, add, l); o += l; }
	FS_WriteFile(PM_REGISTRY, out, (fs_offset_t)o);
	Mem_Free(out);
	Mem_Free(r);
}

// the pack that registered `model`, or "" (into out)
static void pm_pack_of(const char *model, char *out, size_t size)
{
	fs_offset_t n;
	char *r = pm_registry(&n);
	out[0] = 0;
	for (char *line = r; *line; )
	{
		char *e = strchr(line, '\n');
		size_t len = e ? (size_t)(e - line) : strlen(line);
		char *t1 = memchr(line, '\t', len);
		if (t1)
		{
			char *t2 = memchr(t1 + 1, '\t', len - (size_t)(t1 + 1 - line));
			if (t2 && (size_t)(t2 - t1 - 1) == strlen(model) && !strncmp(t1 + 1, model, strlen(model)))
			{
				size_t pl = (size_t)(t1 - line);
				if (pl < size) { memcpy(out, line, pl); out[pl] = 0; }
				break;
			}
		}
		line += len + (e ? 1 : 0);
	}
	Mem_Free(r);
}

// ---------------------------------------------------------------- import

static int ends_with(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && !strcasecmp(s + a - b, suf);
}

static int is_model_ext(const char *ext) { return !strcmp(ext, "iqm") || !strcmp(ext, "dpm") || !strcmp(ext, "md3") || !strcmp(ext, "zym"); }

static void pm_remove_pack(const char *pack)
{
	char path[MAX_OSPATH];
	dpsnprintf(path, sizeof(path), "%s%s", fs_gamedir, pack);
	remove(path);
	FS_Rescan();
}

static int pm_import(const char *src)
{
	fs_offset_t n = 0;
	unsigned char *raw = FS_SysLoadFile(src, tempmempool, true, &n);
	char why[200], stem[48], pack[96], model[64] = "", fmt[8] = "", skinname[16] = "0";
	if (!raw) { pm_fail("Could not read that file."); return 0; }
	// the renderer's copy of the picked file (Android) has done its job once it is in memory
	if (strstr(src, "/luastate/incoming/")) remove(src);
	if ((unsigned long)n > TPM_MAX_PACK) { dpsnprintf(why, sizeof(why), "The file is over %d MB.", (int)(TPM_MAX_PACK >> 20)); Mem_Free(raw); pm_fail(why); return 0; }
	TPM_Stem(FS_FileWithoutPath(src), stem, sizeof(stem));

	unsigned char *zip = raw;
	size_t zipsize = (size_t)n;
	unsigned char *built = NULL;
	const char *mfmt = TPM_FormatOf(raw, (size_t)n);
	if (*mfmt)
	{
		// one bare model: wrap it with a minimal list entry
		char mname[96], tname[96], txt[128];
		dpsnprintf(mname, sizeof(mname), "models/player/%s.%s", stem, mfmt);
		dpsnprintf(tname, sizeof(tname), "models/player/%s.%s_0.txt", stem, mfmt);
		dpsnprintf(txt, sizeof(txt), "name %s\nspecies human\nsex Undisclosed\n", stem);
		const char *names[2] = { mname, tname };
		const unsigned char *datas[2] = { raw, (const unsigned char *)txt };
		size_t sizes[2] = { (size_t)n, strlen(txt) };
		built = (unsigned char *)Mem_Alloc(tempmempool, (size_t)n + 1024);
		zipsize = TPM_StoredZip(built, 2, names, datas, sizes);
		zip = built;
	}
	else if (n < 4 || memcmp(raw, "PK\3\4", 4))
	{
		Mem_Free(raw);
		pm_fail("Not a model (iqm, dpm, md3, zym) or a .pk3/.zip.");
		return 0;
	}

	static tpm_zent_t ents[1024];
	int ne = TPM_ZipList(zip, zipsize, ents, 1024);
	if (ne < 0) { if (built) Mem_Free(built); Mem_Free(raw); pm_fail("The archive is damaged (no file list)."); return 0; }

	tpm_facts_t f;
	memset(&f, 0, sizeof(f));
	f.pack_size = (long)n;
	f.header_ok = 1; f.frames = 1; f.framegroup_lines = TPM_MIN_GROUPS;   // checked after the install
	for (int i = 0; i < ne && !model[0]; ++i)
	{
		const char *nm = ents[i].name;
		if (strncasecmp(nm, "models/player/", 14)) continue;
		const char *base = nm + 14;
		const char *dot = strrchr(base, '.');
		if (!dot || strchr(base, '/') || !is_model_ext(dot + 1)) continue;
		size_t bl = (size_t)(dot - base);
		if (bl >= sizeof(model) || (bl > 5 && !strncmp(base + bl - 5, "_lod", 4))) continue;
		memcpy(model, base, bl); model[bl] = 0;
		dp_strlcpy(fmt, dot + 1, sizeof(fmt));
		for (char *c = fmt; *c; ++c) *c = (char)tolower((unsigned char)*c);
		f.has_model = 1;
		dp_strlcpy(f.fmt, fmt, sizeof(f.fmt));
		f.model_size = (long)ents[i].size;
	}
	if (f.has_model)
	{
		// stock naming: models/player/erebus.iqm, erebus.iqm_0.txt, erebus.iqm_0.skin, erebus.iqm.framegroups
		char stemfmt[80], full[96];
		dpsnprintf(stemfmt, sizeof(stemfmt), "%s.%s_", model, fmt);
		size_t sl = strlen(stemfmt), ml = strlen(model);
		for (int i = 0; i < ne; ++i)
		{
			const char *nm = ents[i].name;
			if (strncasecmp(nm, "models/player/", 14) || strncasecmp(nm + 14, model, ml)) continue;
			const char *rest = nm + 14;
			if (!strncasecmp(rest, stemfmt, sl) && rest[sl] >= '0' && rest[sl] <= '9')
			{
				if (!strcasecmp(rest + sl + 1, ".txt") && !f.has_txt) { f.has_txt = 1; skinname[0] = rest[sl]; skinname[1] = 0; }
				if (!strcasecmp(rest + sl + 1, ".skin")) f.has_skin = 1;
			}
			if (ends_with(nm, ".tga") || ends_with(nm, ".png") || ends_with(nm, ".jpg") || ends_with(nm, ".jpeg") || ends_with(nm, ".dds")) f.has_skin = 1;
		}
		// a model of the game, or of a pack that is not this model's own, may not be replaced
		char owner[96], pat[160];
		dpsnprintf(full, sizeof(full), "models/player/%s.%s", model, fmt);
		pm_pack_of(full, owner, sizeof(owner));
		dpsnprintf(pat, sizeof(pat), "models/player/%s.%s_?.txt", model, fmt);
		fssearch_t *s = FS_Search(pat, true, true, NULL);
		if (s) { f.replaces_stock = s->numfilenames > 0 && !owner[0]; FS_FreeSearch(s); }
		// the same model name in another uploaded pack would borrow that pack's animations and skins
		char mypack[96];
		dpsnprintf(mypack, sizeof(mypack), PM_PREFIX "%s.pk3", stem);
		if (owner[0] && strcmp(owner, mypack))
		{
			if (built) Mem_Free(built);
			Mem_Free(raw);
			pm_fail("You already uploaded a model with this name; delete it first or rename this one.");
			return 0;
		}
	}
	if (!TPM_Decide(&f, why, sizeof(why))) { if (built) Mem_Free(built); Mem_Free(raw); pm_fail(why); return 0; }

	// install, then check what only the model file itself can tell
	dpsnprintf(pack, sizeof(pack), PM_PREFIX "%s.pk3", stem);
	if (!FS_WriteFile(pack, zip, (fs_offset_t)zipsize)) { if (built) Mem_Free(built); Mem_Free(raw); pm_fail("Could not save the model."); return 0; }
	if (built) Mem_Free(built);
	Mem_Free(raw);
	FS_Rescan();

	char path[160];
	fs_offset_t msz = 0, gsz = 0;
	dpsnprintf(path, sizeof(path), "models/player/%s.%s", model, fmt);
	unsigned char *md = FS_LoadFile(path, tempmempool, true, &msz);
	f.header_ok = md && !strcmp(TPM_FormatOf(md, (size_t)msz), fmt);
	f.frames = md ? TPM_Frames(md, (size_t)msz) : 0;
	f.iqm_anims_named = md && !strcmp(fmt, "iqm") ? TPM_IqmAnimsNamed(md, (size_t)msz) : 0;
	f.model_size = (long)msz;
	if (md) Mem_Free(md);
	dpsnprintf(path, sizeof(path), "models/player/%s.%s.framegroups", model, fmt);
	char *g = pm_load_vfs(path, &gsz);
	f.framegroup_lines = g ? TPM_FramegroupLines(g, (size_t)gsz) : 0;
	if (g) Mem_Free(g);
	if (!TPM_Decide(&f, why, sizeof(why))) { pm_remove_pack(pack); pm_fail(why); return 0; }

	// the list name from <model>_<skin>.txt
	char title[64];
	dp_strlcpy(title, model, sizeof(title));
	dpsnprintf(path, sizeof(path), "models/player/%s.%s_%s.txt", model, fmt, skinname);
	char *t = pm_load_vfs(path, &gsz);
	if (t)
	{
		for (char *line = t; line < t + gsz; )
		{
			char *e = memchr(line, '\n', (size_t)(t + gsz - line));
			size_t len = e ? (size_t)(e - line) : (size_t)(t + gsz - line);
			if (len > 5 && !strncmp(line, "name ", 5))
			{
				size_t m = len - 5 < sizeof(title) - 1 ? len - 5 : sizeof(title) - 1;
				memcpy(title, line + 5, m); title[m] = 0;
				while (m && (title[m - 1] == '\r' || title[m - 1] == ' ')) title[--m] = 0;
				break;
			}
			line += len + 1;
		}
		Mem_Free(t);
	}
	char entry[200];
	char full[96];
	dpsnprintf(full, sizeof(full), "models/player/%s.%s", model, fmt);   // what playermodel takes
	dpsnprintf(entry, sizeof(entry), "%s\t%s\t%s\t%s\n", pack, full, skinname, title);
	pm_registry_edit(pack, entry);
	dpsnprintf(pm_report, sizeof(pm_report), "ok\t%s\t%s\t%s", full, skinname, title);
	return 1;
}

// ---------------------------------------------------------------- builtins

void VM_uib_pm_import(prvm_prog_t *prog)
{
	PRVM_G_FLOAT(OFS_RETURN) = pm_import(PRVM_G_STRING(OFS_PARM0));
}

void VM_uib_pm_report(prvm_prog_t *prog)
{
	PRVM_G_INT(OFS_RETURN) = PRVM_SetTempString(prog, pm_report, strlen(pm_report));
}

// float uib_pm_delete(string model): only a model this feature imported
void VM_uib_pm_delete(prvm_prog_t *prog)
{
	char pack[96];
	const char *model = PRVM_G_STRING(OFS_PARM0);
	pm_pack_of(model, pack, sizeof(pack));
	if (!pack[0] || strncmp(pack, PM_PREFIX, strlen(PM_PREFIX)) || strchr(pack, '/') || strchr(pack, '\\') || !ends_with(pack, ".pk3"))
	{
		PRVM_G_FLOAT(OFS_RETURN) = 0;
		return;
	}
	pm_registry_edit(pack, NULL);
	pm_remove_pack(pack);
	PRVM_G_FLOAT(OFS_RETURN) = 1;
}

// for tests and the console: touch_playermodel_import <file>, touch_playermodel_delete <model>
static void PM_Import_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) < 2) { Con_Print("usage: touch_playermodel_import <file>\n"); return; }
	pm_import(Cmd_Argv(cmd, 1));
	Con_Printf("playermodel: %s\n", pm_report);
}

static void PM_Delete_f(cmd_state_t *cmd)
{
	char pack[96];
	if (Cmd_Argc(cmd) < 2) return;
	pm_pack_of(Cmd_Argv(cmd, 1), pack, sizeof(pack));
	if (!pack[0]) { Con_Print("playermodel: not an uploaded model\n"); return; }
	pm_registry_edit(pack, NULL);
	pm_remove_pack(pack);
	Con_Printf("playermodel: deleted %s\n", pack);
}

void TouchPM_Init(void)
{
	Cvar_RegisterVariable(&uib_pm);
	Cmd_AddCommand(CF_SHARED, "touch_playermodel_import", PM_Import_f, "check and keep an uploaded player model (a model file or .pk3/.zip)");
	Cmd_AddCommand(CF_SHARED, "touch_playermodel_delete", PM_Delete_f, "delete an uploaded player model by name");
}

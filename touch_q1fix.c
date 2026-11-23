/*
Godnotic: fixes for Quake 1 content on a Xonotic-engine server.

Public servers such as "Quake 1 Nostalgia Trip!" run Xonotic's own server and
a Quake mod (modname data_quake, the SMB mod's CSQC). The client downloads the
mod's packs from the server over HTTP: a map pack, an assets pack (HUD pictures
gfx/quake/*, sky boxes gfx/env/*, replacement textures), items, monsters. A tablet
that joins before every pack is in, or whose assets pack is incomplete, ran into:

  - the engine's scoreboard rows (Sbar_PrintScoreboardItem) step 8 units while the
    sbar font is drawn at 8 x its font scale, so with the Xolonium sbar font every
    row overlaps the next. Around Sbar_Draw the sbar font is drawn at scale 1
    (touch_sbar_rowfix 0 leaves it alone);
  - a Quake 1 world (BSP version 29 with the wad's textures) is lit with
    Quake's own lightmaps, which are far darker than the Xonotic maps the
    brightness defaults are tuned for. Its lightmaps are scaled by
    touch_q1_lightscale (2.5; 1 is exactly Quake).

Everything here is a hook called from an upstream file on its own line.
*/
#include "quakedef.h"
#include "touch_q1fix.h"
#include "image.h"
#include "draw.h"

static cvar_t touch_sbar_rowfix = {CF_CLIENT | CF_ARCHIVE, "touch_sbar_rowfix", "1",
	"1: the engine HUD's scoreboard and number rows are drawn at font scale 1 so that neighbouring rows do not overlap; 0: stock"};
static cvar_t touch_q1_lightscale = {CF_CLIENT | CF_ARCHIVE, "touch_q1_lightscale", "2.5",
	"multiplies the lightmaps of a Quake 1 world (BSP29), which are much darker than Xonotic maps; 1 is Quake's own light"};

static cvar_t touch_nomodel_draw = {CF_CLIENT | CF_ARCHIVE, "touch_nomodel_draw", "0",
	"1: an entity whose model is missing is drawn as DarkPlaces' debug marker, an octahedron with red, green and blue corners (a large one when it is the weapon in the player's hand); 0: it is not drawn"};

void TouchQ1_Init(void)
{
	Cvar_RegisterVariable(&touch_nomodel_draw);
	Cvar_RegisterVariable(&touch_sbar_rowfix);
	Cvar_RegisterVariable(&touch_q1_lightscale);
}

void TouchQ1_LightScale(float *lightmapintensity)
{
	if (touch_q1_lightscale.value > 0 && touch_q1_lightscale.value != 1 && cl.worldmodel && cl.worldmodel->type == mod_brushq1)
		*lightmapintensity *= touch_q1_lightscale.value;
}

static float tq_font_scale, tq_font_maxwidth;
static qbool tq_font_changed;

void TouchQ1_SbarBegin(void)
{
	dp_font_t *f = FONT_SBAR;
	tq_font_changed = false;
	if (!touch_sbar_rowfix.integer || f->settings.scale <= 1.0f)
		return;
	tq_font_scale = f->settings.scale;
	tq_font_maxwidth = f->maxwidth;
	f->maxwidth /= f->settings.scale; /* LoadFont multiplied it in */
	f->settings.scale = 1.0f;
	tq_font_changed = true;
}

void TouchQ1_SbarEnd(void)
{
	dp_font_t *f = FONT_SBAR;
	if (!tq_font_changed)
		return;
	f->settings.scale = tq_font_scale;
	f->maxwidth = tq_font_maxwidth;
	tq_font_changed = false;
}

// gl_rmain.c R_DrawNoModel: DarkPlaces draws an entity whose model could not be loaded
// (a pack that did not arrive, a model that failed to load) as a 32 unit octahedron whose
// six corners are dark blue, green and red. Held in front of the player, as a weapon, it is
// the large rainbow diamond. Nothing is a better picture than that.
qbool TouchQ1_HideNoModel(void)
{
	static int count;
	if (touch_nomodel_draw.integer)
		return false;
	if (++count == 1 || (count & 1023) == 0)
		Con_DPrintf("Touch: entity without a model not drawn (%d so far)\n", count);
	return true;
}

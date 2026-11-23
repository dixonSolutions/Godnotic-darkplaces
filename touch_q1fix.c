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
    touch_q1_lightscale (1.5; 1 is exactly Quake).

Everything here is a hook called from an upstream file on its own line.
*/
#include "quakedef.h"
#include "touch_q1fix.h"
#include "image.h"
#include "draw.h"

static cvar_t touch_sbar_rowfix = {CF_CLIENT | CF_ARCHIVE, "touch_sbar_rowfix", "1",
	"1: the engine HUD's scoreboard and number rows are drawn at font scale 1 so that neighbouring rows do not overlap; 0: stock"};
static cvar_t touch_q1_lightscale = {CF_CLIENT | CF_ARCHIVE, "touch_q1_lightscale", "1.5",
	"multiplies the lightmaps of a Quake 1 world (BSP29), which are much darker than Xonotic maps; 1 is Quake's own light"};

void TouchQ1_Init(void)
{
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

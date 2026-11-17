// Touch: a sky for a map whose own skybox is not in the installed data.
//
// A client that has only part of the data set (the split set, the phone packs
// without the maps pack) or that joins a server running a community map has
// none of that map's env/*.jpg. R_LoadSkyBox then loads no side at all, the
// sky surfaces are drawn against the clear colour, and the world reads as
// broken (solid black sky) with one console line nowhere to be seen. Nothing
// in the BSP or its shaders can stand in for those images, so this hooks the
// failure and loads a sky the data set does ship, saying which one it used.
//
//   r_sky.c R_LoadSkyBox:  if (Touch_SkyFallback(skyname, sizeof(skyname))) return R_LoadSkyBox();
//   r_sky.c R_Sky_Init:    Touch_Sky_Init();
//
// The retry re-enters R_LoadSkyBox, so a fallback that does not load either
// must not try again: the second call sees skyname already set to the
// fallback and returns false, which leaves the black sky as before.
// r_sky_fallback 0, or an empty r_sky_fallback_name, turns this off.

#ifndef TOUCH_SKY_H
#define TOUCH_SKY_H

static cvar_t r_sky_fallback = {CF_CLIENT | CF_ARCHIVE, "r_sky_fallback", "1", "when a map's skybox is not in the installed data (a server's community map, part of the set not downloaded yet), draw a sky the data set ships instead of a black void; 0: leave it black"};
static cvar_t r_sky_fallback_name = {CF_CLIENT | CF_ARCHIVE, "r_sky_fallback_name", "env/extragalactic/asteroids", "the sky r_sky_fallback draws (a map's sky key form, env/<name>/<name>); empty: no fallback"};

static void Touch_Sky_Init(void)
{
	Cvar_RegisterVariable(&r_sky_fallback);
	Cvar_RegisterVariable(&r_sky_fallback_name);
}

// True when skyname now holds a fallback the caller should load, i.e. the
// map's own skybox supplied no side at all.
static qbool Touch_SkyFallback(char *sky, size_t skysize)
{
	const char *name = r_sky_fallback_name.string;
	if (!r_sky_fallback.integer || !name[0])
		return false;
	if (!strcmp(sky, name)) // the fallback failed too: not a second try
		return false;
	Con_Printf(CON_WARN "skybox \"%s\" is not in the installed data, drawing the fallback sky \"%s\" (r_sky_fallback 0 leaves it black)\n", sky, name);
	dp_strlcpy(sky, name, skysize);
	return true;
}

#endif

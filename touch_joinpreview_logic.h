/*
Godnotic: the preview must never draw a world the client has replaced.

touch_joinpreview.c keeps the scene's world entity across frames. A reconnect
or a second join replaces cl.worldmodel; the scene pointer from the previous
map then still looks valid and is drawn (the recorded crash was in
R_Mod_DrawSky from a loading-screen redraw inside CL_KeepaliveMessage).

This header is the pure part the tests compile alone: what a preview frame may
use, and the scene model it must take.
*/
#ifndef TOUCH_JOINPREVIEW_LOGIC_H
#define TOUCH_JOINPREVIEW_LOGIC_H

/* All of it is needed before a world frame is drawable: the model, its BSP
 * leaves, and its surfaces (R_Mod_DrawSky walks the surface list). */
static inline int TouchJoinPreview_Drawable(int have_model, int leafs, int leaf_data, int surfaces, int surface_data)
{
	return have_model && leafs > 0 && leaf_data && surfaces > 0 && surface_data;
}

/* Before the world model is drawable there is still a picture of the map: the
 * levelshot named after the map the serverinfo already carries. A join that
 * names a map but has nothing to draw shows that, not the generic loading art.
 */
static inline int TouchJoinPreview_ShowsLevelshot(int preview_on, int joining, int map_known, int drawable)
{
	return preview_on && joining && map_known && !drawable;
}

/* A server join always has a picture behind it, never the logo screen: the map's
 * own picture if this install has it, else the one the server list's map name
 * finds, else a dimmed generic map picture. The world replaces it when drawable.
 * Unlike TouchJoinPreview_ShowsLevelshot this does not need the map name. */
static inline int TouchJoinPreview_ShowsPicture(int preview_on, int joining, int drawable)
{
	return preview_on && joining && !drawable;
}

#define TJP_PIC_NONE 0
#define TJP_PIC_MAP 1      /* the map's own picture (maps/<map>.jpg, levelshots/<map>) */
#define TJP_PIC_LIST 2     /* the picture the server list's map name finds */
#define TJP_PIC_GENERIC 3  /* any map picture, dimmed: the map is not here yet */

static inline int TouchJoinPreview_PictureChoice(int have_map, int have_list, int have_generic)
{
	if (have_map)
		return TJP_PIC_MAP;
	if (have_list)
		return TJP_PIC_LIST;
	return have_generic ? TJP_PIC_GENERIC : TJP_PIC_NONE;
}

/* Brightness of the picture: the generic one is dimmed so it reads as a backdrop. */
static inline float TouchJoinPreview_PictureBrightness(int choice)
{
	return choice == TJP_PIC_GENERIC ? 0.45f : 1.0f;
}

/* The precache step shown on the card: models or sounds being loaded, of the total
 * (cl.loadmodel_current of loadmodel_total). kind: 1 models, 2 sounds, 0 none. */
static inline int TouchJoinPreview_PrecacheKind(int begun, int finished, int model_cur, int model_total, int sound_cur, int sound_total)
{
	if (!begun || finished)
		return 0;
	if (model_cur < model_total)
		return 1;
	if (sound_cur < sound_total)
		return 2;
	return 0;
}

/* A retry of the picture lookup while the map's own picture is not there yet (it comes
 * with the map's pack, which is downloading), at most every half second; a found map
 * picture is final. */
static inline int TouchJoinPreview_RetryPicture(int choice, double now, double last)
{
	return choice != TJP_PIC_MAP && now - last >= 0.5;
}

/* The engine's own loading band (SCR_DrawLoadingStack: the step's text and the
 * progress strip along the bottom) is drawn only when no native card shows the same
 * step and fraction (connect load, load_progress, precache). */
static inline int TouchJoinPreview_DrawEngineBand(int card_up, int have_stack)
{
	return have_stack && !card_up;
}

/* The scene always uses the client's current world, never a cached one. */
static inline int TouchJoinPreview_SceneModel(int current, int cached)
{
	(void)cached;
	return current;
}

/* R_Modules_NewMap resets the renderer one module at a time, and the skybox and
 * texture loads inside it redraw the loading screen (CL_KeepaliveMonitor ->
 * SCR_UpdateLoadingScreen -> SCR_DrawScreen). A preview frame drawn there runs
 * R_RenderView through half-reset renderer state: the SIGSEGV at fault address
 * 0x233 (and once 0xffffffffffffe7, in Mem_Free) in R_RenderScene, stack
 * gl_main_newmap -> CL_ParseEntityLump -> R_SetSkyBox -> loadimagepixelsbgra ->
 * CL_KeepaliveMonitor, seen in 1.2.251 to 1.2.273. No preview frame while a
 * newmap runs; the loading page shows for that moment instead. */
static inline int TouchJoinPreview_Allowed(int drawable, int newmap_depth)
{
	return drawable && newmap_depth <= 0;
}

/* Nesting count of newmaps in progress (begin +1, end -1), never below zero. */
static inline int TouchJoinPreview_NewMapDepth(int depth, int begin)
{
	depth += begin ? 1 : -1;
	return depth < 0 ? 0 : depth;
}

#endif

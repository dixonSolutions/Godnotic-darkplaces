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

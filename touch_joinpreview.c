/*
Godnotic: the world while it loads.

The full-screen loading page covers the window from the first packet of a
join to the last signon step, though the map itself is loaded at signon step
2: what takes the seconds after that is the model, sound and CSQC precache
and the first snapshot. touch_join_preview 1 (the default) draws the world
from a spectator camera as soon as the world model is there, and the loading
screen is reduced to its progress bar at the bottom (cl_screen.c).

The camera follows an active player: the engine's entity states carry no
health, so "alive" is taken as drawn and not EF_NODRAW, and "active" as
having moved or fired recently (EF_MUZZLEFLASH, or an origin change since the
previous state). A player's model is followed from behind at 150 units and 45
above, with a slight downward pitch; the camera smooths towards it so a
jumping target does not shake the view. With no player to follow, the camera
sits above a corner of the map bounds looking at their centre.

There is no intermission camera to fall back to in the engine: Xonotic's is
drawn by its CSQC (CSQC_UpdateView), which does not run during a signon, so
the map overview is the fallback here.

Hand-over: touch_join_preview is off once cls.signon reaches SIGNONS, so the
last signon step and the frames up to SCR_EndLoadingPlaque already draw the
normal first-person view (cl_screen.c, CSQC_UpdateView), and the play view is
on the surface before the loading plaque is gone.

What a preview frame carries: CL_UpdateWorld links network entities only when
the signon is complete, so the scene here is the world model alone (and the
static entities are not linked yet either). The players in it appear with the
first in-game frame; until then the camera follows their positions, which the
signon's baselines and the entity updates carry.
*/

#include "quakedef.h"
#include "touch_joinpreview.h"
#include "touch_joinpreview_logic.h"
#include "touch_frontend.h" // TouchFE_MenuHidden: the native menu is not drawn
#include "touch_joinname.h" // TouchJoinName_MapFor: the map the server list names
#include "fs.h"

static cvar_t touch_join_preview = {CF_CLIENT, "touch_join_preview", "1",
	"draw the world from a spectator camera as soon as a joining client has it, with the load's progress as a bar, instead of the full-screen loading page (Godnotic); 0 = the loading page all the way"};

static int jp_target;            // entity number the camera follows, 0 = none
static model_t *jp_world;        // the world the camera was chosen in
static double jp_target_seen;    // last time it moved or fired, or was chosen
static qbool jp_drew;            // a preview frame was drawn since the last swap
static vec3_t jp_org, jp_ang;    // the smoothed camera
static qbool jp_have_cam;        // jp_org and jp_ang are usable
static double jp_cam_time;       // when the camera last moved
static int jp_newmap;            // R_Modules_NewMap in progress (touch_joinpreview_logic.h)
static char jp_localmap[MAX_QPATH]; // the map a local game is starting (SV_SpawnServer), "" otherwise

#define JP_FOLLOW_DIST 150.0f    // units behind the followed player
#define JP_FOLLOW_HEIGHT 45.0f   // above their origin
#define JP_FOLLOW_LOOK 32.0f     // the point looked at, above the origin
#define JP_STICKY 3.0            // seconds a followed player is kept without activity
#define JP_MOVE_EPS2 (2.0f * 2.0f)

void TouchJoinPreview_Init(void)
{
	Cvar_RegisterVariable(&touch_join_preview);
}

// The map is drawable: the world model is loaded (signon step 2) and its
// brush data is there. Before that the loading page has nothing to show.
qbool TouchJoinPreview_Active(void)
{
	if (cls.state != ca_connected)
		jp_newmap = 0; // a Host_Error longjmp out of a newmap never reached its end
	if (!touch_join_preview.integer)
		return false;
	if (key_dest != key_game && !TouchFE_MenuHidden())
		return false;   // the in-match menu is an overlay: no preview camera under it
		// (a menu the native front end hides is not drawn: the preview is behind
		// the app's own join page, so it stays on)
	if (cls.state != ca_connected || cls.signon >= SIGNONS)
		return false;
	if (!scr_refresh.integer || vid_hidden)
		return false;
	if (cls.demoplayback || cls.timedemo)
		return false;   // a demo or a timedemo has its own view
	if (!cl.worldmodel)
		return false;
	// A reconnect or a second join replaces cl.worldmodel. The camera state and
	// the scene from the old map do not belong to the new one; a scene model left
	// cached here was drawn after its map was freed (R_Mod_DrawSky).
	if (cl.worldmodel != jp_world)
	{
		jp_world = cl.worldmodel;
		jp_target = 0;
		jp_target_seen = 0;
		jp_have_cam = false;
		jp_cam_time = 0;
	}
	if (!TouchJoinPreview_Allowed(TouchJoinPreview_Drawable(1, cl.worldmodel->brush.num_leafs, cl.worldmodel->brush.data_leafs != NULL,
		cl.worldmodel->num_surfaces, cl.worldmodel->data_surfaces != NULL), jp_newmap))
		return false;
	if (key_consoleactive & KEY_CONSOLEACTIVE_FORCED)
		return false;
	return true;
}

// A local game names its map before any server or client exists: the player
// picked it. SV_SpawnServer says so as the loading plaque goes up; it ends with
// the plaque (scr_loading).
void TouchJoinPreview_SetMap(const char *map)
{
	dp_strlcpy(jp_localmap, map ? map : "", sizeof(jp_localmap));
	TouchFE_Loading("", 0); // the native card gets the map now, not at the first load step
}

static qbool JP_LocalLoading(void)
{
	if (!scr_loading)
		jp_localmap[0] = 0;
	return jp_localmap[0] != '\0';
}

qbool TouchJoinPreview_LocalLoading(void)
{
	return JP_LocalLoading();
}

// The map being joined: the local game's pick, else as the serverinfo names it
// ("maps/foo.bsp" -> "foo"), else the map the server list's row for this address
// names (it is known before the serverinfo arrives). *from_list says which.
static qbool JP_MapNameSrc(char *buf, int size, qbool *from_list)
{
	char *dot, *slash;
	buf[0] = 0;
	if (from_list)
		*from_list = false;
	if (JP_LocalLoading())
		dp_strlcpy(buf, jp_localmap, size);
	else
		InfoString_GetValue(cl.qw_serverinfo, "map", buf, size);
	// a DarkPlaces server sends no serverinfo map: the precache list's first entry is the
	// world model ("maps/foo.bsp"), there from the first reliable message of the signon
	if (buf[0] == '\0' && !JP_LocalLoading() && cls.state == ca_connected && cl.model_name[1][0] && !strncmp(cl.model_name[1], "maps/", 5))
		dp_strlcpy(buf, cl.model_name[1], size);
	if (buf[0] == '\0' && !JP_LocalLoading())
	{
		char addr[128] = "";
		if (cls.connect_trying)
			LHNETADDRESS_ToString(&cls.connect_address, addr, sizeof(addr), true);
		else if (cls.state == ca_connected && cls.netcon)
			LHNETADDRESS_ToString(&cls.netcon->peeraddress, addr, sizeof(addr), true);
		if (addr[0])
		{
			dp_strlcpy(buf, TouchJoinName_MapFor(addr), size);
			if (from_list)
				*from_list = buf[0] != '\0';
		}
	}
	if (buf[0] == '\0')
		return false;
	slash = strrchr(buf, '/');
	if (slash)
		memmove(buf, slash + 1, strlen(slash + 1) + 1);
	dot = strrchr(buf, '.');
	if (dot && !strcmp(dot, ".bsp"))
		*dot = 0;
	return buf[0] != '\0';
}

qbool TouchJoinPreview_MapName(char *buf, int size)
{
	return JP_MapNameSrc(buf, size, NULL);
}
#define JP_MapName TouchJoinPreview_MapName

// The picture file of a map in this install: maps/<map>.jpg (what the map list
// shows), or the Quake 3 name levelshots/<map>. Only a file that is there is
// returned: asking the picture cache for a missing one would cache the failure.
static qbool JP_FindPicture(const char *map, char *out, size_t size)
{
	static const char *dirs[] = {"maps/", "levelshots/", NULL};
	static const char *exts[] = {".jpg", ".png", ".tga", NULL};
	int d, e;
	if (!map[0] || strstr(map, ".."))
		return false;
	for (d = 0; dirs[d]; d++)
		for (e = 0; exts[e]; e++)
		{
			dpsnprintf(out, size, "%s%s%s", dirs[d], map, exts[e]);
			if (FS_FileExists(out))
				return true;
		}
	out[0] = 0;
	return false;
}

// A map of the same family (wind2 -> wind*.jpg): the server list's map name finds
// "any picture" when the exact one is not here.
static qbool JP_FindSimilar(const char *map, char *out, size_t size)
{
	char base[MAX_QPATH], pat[MAX_QPATH];
	fssearch_t *search;
	size_t n;
	qbool found = false;
	dp_strlcpy(base, map, sizeof(base));
	if (strchr(base, '_'))
		*strchr(base, '_') = 0;
	n = strlen(base);
	while (n > 0 && base[n - 1] >= '0' && base[n - 1] <= '9')
		base[--n] = 0;
	if (n < 4 || strpbrk(base, "*?[]"))
		return false;
	dpsnprintf(pat, sizeof(pat), "maps/%s*.jpg", base);
	search = FS_Search(pat, true, true, NULL);
	if (search)
	{
		if (search->numfilenames > 0)
		{
			dp_strlcpy(out, search->filenames[0], size);
			found = true;
		}
		FS_FreeSearch(search);
	}
	return found;
}

// Any map picture at all, for a join whose map this install has nothing for: the
// first one found, kept for the session.
static qbool JP_FindGeneric(char *out, size_t size)
{
	static char generic[MAX_QPATH];
	static double last;
	if (!generic[0] && Sys_DirtyTime() - last > 2.0)
	{
		fssearch_t *search = FS_Search("maps/*.jpg", true, true, NULL);
		last = Sys_DirtyTime();
		if (search)
		{
			if (search->numfilenames > 0)
				dp_strlcpy(generic, search->filenames[(int)(last * 1000.0) % search->numfilenames], sizeof(generic));
			FS_FreeSearch(search);
		}
	}
	dp_strlcpy(out, generic, size);
	return generic[0] != '\0';
}

// The picture behind a join: TJP_PIC_* (touch_joinpreview_logic.h) and its file
// ("maps/foo.jpg") in `name`. The map's own picture is final; anything else is asked
// again twice a second, so the pack that is downloading brings the right one in.
int TouchJoinPreview_Picture(char *name, int size)
{
	static char key[MAX_QPATH + 2], cname[MAX_QPATH];
	static int choice;
	static double last = -1;
	char map[MAX_QPATH], file[MAX_QPATH];
	qbool from_list = false, known = JP_MapNameSrc(map, sizeof(map), &from_list);
	char nkey[MAX_QPATH + 2];
	double now = Sys_DirtyTime();
	dpsnprintf(nkey, sizeof(nkey), "%c%s", from_list ? 'L' : 'S', known ? map : "");
	if (strcmp(nkey, key) || last < 0 || TouchJoinPreview_RetryPicture(choice, now, last))
	{
		int have_map = 0, have_list = 0, have_generic = 0;
		last = now;
		dp_strlcpy(key, nkey, sizeof(key));
		file[0] = 0;
		if (known && JP_FindPicture(map, file, sizeof(file)))
		{
			if (from_list)
				have_list = 1;
			else
				have_map = 1;
		}
		else if (known && JP_FindSimilar(map, file, sizeof(file)))
			have_list = 1;
		else if (JP_FindGeneric(file, sizeof(file)))
			have_generic = 1;
		choice = TouchJoinPreview_PictureChoice(have_map, have_list, have_generic);
		dp_strlcpy(cname, choice ? file : "", sizeof(cname));
	}
	dp_strlcpy(name, cname, size);
	return choice;
}

// The picture is drawn from the moment a join starts until the world is
// there: the player sees the map they are joining (or a dim backdrop), not a
// logo, for the part of the join that loads the rest of the signon.
qbool TouchJoinPreview_Waiting(void)
{
	qbool drawable = false;
	qbool local = JP_LocalLoading(); // the plaque of a local start covers the menu: it needs no client yet
	if (!touch_join_preview.integer)
		return false;
	if (!local && key_dest != key_game && !TouchFE_MenuHidden())
		return false;
	if (!local && (cls.state != ca_connected || cls.signon >= SIGNONS))
		return false;
	if (!scr_refresh.integer || vid_hidden)
		return false;
	if (cls.demoplayback || cls.timedemo)
		return false;
	if (cl.worldmodel && !(local && (cls.state != ca_connected || cls.signon >= SIGNONS))) // a new local map: the old world is not it
		drawable = TouchJoinPreview_Allowed(TouchJoinPreview_Drawable(1, cl.worldmodel->brush.num_leafs, cl.worldmodel->brush.data_leafs != NULL,
			cl.worldmodel->num_surfaces, cl.worldmodel->data_surfaces != NULL), jp_newmap) != 0; // inside a newmap the picture stays
	return TouchJoinPreview_ShowsPicture(1, 1, drawable) != 0;
}

qbool TouchJoinPreview_DrawLevelshot(void)
{
	static cachepic_t *pic;
	static char picname[MAX_QPATH];
	char path[MAX_QPATH];
	float x, y, w, h, f, bright;
	float verts[12], texcoords[8];
	int choice;
	if (!TouchJoinPreview_Waiting())
		return false;
	choice = TouchJoinPreview_Picture(path, sizeof(path));
	if (!choice)
		return false;
	if (!pic || strcmp(picname, path))
	{
		char stem[MAX_QPATH];
		dp_strlcpy(stem, path, sizeof(stem));
		FS_StripExtension(path, stem, sizeof(stem));
		pic = Draw_CachePic(stem);
		dp_strlcpy(picname, path, sizeof(picname));
	}
	if (!pic)
		return false;
	bright = TouchJoinPreview_PictureBrightness(choice);
	w = Draw_GetPicWidth(pic);
	h = Draw_GetPicHeight(pic);
	if (w < 16 || h < 16)
		return false;   // the placeholder for a picture this install does not have
	// cover the screen, keeping the picture's aspect
	f = max(vid_conwidth.integer / w, vid_conheight.integer / h);
	w *= f;
	h *= f;
	x = (vid_conwidth.integer - w) / 2;
	y = (vid_conheight.integer - h) / 2;
	verts[2] = verts[5] = verts[8] = verts[11] = 0;
	verts[0] = verts[9] = x;
	verts[1] = verts[4] = y;
	verts[3] = verts[6] = x + w;
	verts[7] = verts[10] = y + h;
	texcoords[0] = 0; texcoords[1] = 0;
	texcoords[2] = 1; texcoords[3] = 0;
	texcoords[4] = 1; texcoords[5] = 1;
	texcoords[6] = 0; texcoords[7] = 1;
	GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	GL_DepthRange(0, 1);
	GL_PolygonOffset(0, 0);
	GL_DepthTest(false);
	GL_Color(bright, bright, bright, 1);
	R_Mesh_PrepareVertices_Generic_Arrays(4, verts, NULL, texcoords);
	R_SetupShader_Generic(Draw_GetPicTexture(pic), true, true, false);
	R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
	return true;
}

void TouchJoinPreview_NewMap(qbool begin)
{
	jp_newmap = TouchJoinPreview_NewMapDepth(jp_newmap, begin);
}

qbool TouchJoinPreview_Drew(void)
{
	qbool drew = jp_drew;
	jp_drew = false;
	return drew;
}

// CL_UpdateWorld's Touch hook links the world, the map's static entities and the
// network entities for a preview frame. A draw that happens earlier than that
// hook (a loading-screen redraw from inside the signon) only has the world; it
// gets a world entity of its own so the map still shows.
static void JP_SetScene(void)
{
	if (cl.worldmodel)
	{
		// Always take the current model. Leaving a cached scene.worldentity or
		// scene.worldmodel in place is a use-after-free when a reconnect or a
		// second join has replaced the world since the last preview frame.
		entity_t *world = &cl.entities[0];
		world->render.model = cl.worldmodel;
		world->render.matrix = identitymatrix;
		world->render.flags = RENDER_SHADOW | (r_fullbright.integer ? 0 : RENDER_LIGHT);
		world->render.alpha = 1;
		VectorSet(world->render.colormod, 1, 1, 1);
		VectorSet(world->render.glowmod, 1, 1, 1);
		r_refdef.scene.worldentity = &world->render;
		r_refdef.scene.worldmodel = cl.worldmodel;
	}
	r_refdef.scene.time = cl.time;
}

qbool TouchJoinPreview_WantsEntities(void)
{
	return TouchJoinPreview_Active();
}

// The entity relink takes a baseline for an entity that is drawn only once an
// update marks it active (cl_ents.c). During the signon a server sends baselines
// and no updates, so the players to follow and to show would be missing from
// every preview frame without this. Nothing is marked outside a signon, and the
// first real update (or the signon's end) takes over anyway.
void TouchJoinPreview_Baseline(int entnum)
{
	if (!touch_join_preview.integer)
		return;
	if (cls.state != ca_connected || cls.signon >= SIGNONS)
		return;
	if (entnum <= 0 || entnum >= MAX_EDICTS || entnum >= cl.max_entities)
		return;
	cl.entities_active[entnum] = true;
}

// Xonotic's player models are models/player/*; anything else a server sends
// (map objects, dropped weapons, a corpse's body entity) is not followed.
static qbool JP_IsPlayer(const entity_t *e, int i)
{
	if (i == cl.viewentity)
		return true;
	if (e->render.model)
		return e->render.model->name[0] && strstr(e->render.model->name, "/player/") != NULL;
	if (e->state_current.modelindex > 0 && e->state_current.modelindex < MAX_MODELS)
		return cl.model_name[e->state_current.modelindex][0] && strstr(cl.model_name[e->state_current.modelindex], "/player/") != NULL;
	return false;
}

static qbool JP_Alive(const entity_t *e, int i)
{
	if (!e->state_current.active || (e->state_current.effects & EF_NODRAW))
		return false;
	// the local player's health is known to the engine (playerinfo)
	if (i == cl.viewentity && cl.stats[STAT_HEALTH] <= 0)
		return false;
	return JP_IsPlayer(e, i);
}

// The player to follow: one that fired this frame, else one that moved since
// the previous state, else the one followed last. A followed player that does
// nothing for JP_STICKY seconds is dropped for a more active one.
static void JP_ChooseTarget(void)
{
	int i, n = min(cl.num_entities, MAX_EDICTS), best = 0;
	float bestscore = 0;
	double now = Sys_DirtyTime();

	for (i = 1; i < n; i++)
	{
		const entity_t *e = &cl.entities[i];
		vec3_t delta;
		float score = 0;
		if (!JP_Alive(e, i))
			continue;
		VectorSubtract(e->state_current.origin, e->state_previous.origin, delta);
		if (VectorLength2(delta) > JP_MOVE_EPS2)
			score += 2;
		if (e->state_current.effects & EF_MUZZLEFLASH)
			score += 4;
		if (score > 0)
			jp_target_seen = now;
		if (score > bestscore)
		{
			bestscore = score;
			best = i;
		}
	}

	if (jp_target && jp_target < n && JP_Alive(&cl.entities[jp_target], jp_target)
		&& (now - jp_target_seen < JP_STICKY || !best))
		return;  // still active enough, or nothing better to follow

	jp_target = best;
	if (best)
		jp_target_seen = now;
}

// Where the camera should be this frame, before smoothing.
static void JP_TargetCamera(vec3_t org, vec3_t ang)
{
	if (jp_target && jp_target < cl.num_entities)
	{
		const entity_t *e = &cl.entities[jp_target];
		vec3_t focus, fwd, angles;

		VectorCopy(e->state_current.origin, focus);
		focus[2] += JP_FOLLOW_LOOK;
		// behind the player, along the yaw the server sent for it
		VectorSet(angles, 0, e->state_current.angles[YAW], 0);
		AngleVectors(angles, fwd, NULL, NULL);
		VectorMA(focus, -JP_FOLLOW_DIST, fwd, org);
		org[2] = focus[2] + JP_FOLLOW_HEIGHT - JP_FOLLOW_LOOK;
		// look at the player
		VectorSubtract(focus, org, fwd);
		ang[PITCH] = -atan2(fwd[2], sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1])) * (180.0 / M_PI);
		ang[YAW] = atan2(fwd[1], fwd[0]) * (180.0 / M_PI);
		ang[ROLL] = 0;
	}
	else
	{
		// aerial overview: above a corner of the map bounds, looking at their
		// centre (no player to follow yet, or none in view during the signon)
		model_t *m = cl.worldmodel;
		vec3_t centre, size;
		VectorAdd(m->normalmins, m->normalmaxs, centre);
		VectorScale(centre, 0.5f, centre);
		VectorSubtract(m->normalmaxs, m->normalmins, size);
		VectorSet(org, centre[0] + size[0] * 0.45f, centre[1] + size[1] * 0.45f, m->normalmaxs[2] + size[2] * 0.12f);
		VectorSubtract(centre, org, size);
		ang[PITCH] = -atan2(size[2], sqrt(size[0] * size[0] + size[1] * size[1])) * (180.0 / M_PI);
		ang[YAW] = atan2(size[1], size[0]) * (180.0 / M_PI);
		ang[ROLL] = 0;
	}
}

static float JP_AngleDelta(float from, float to)
{
	float d = to - from;
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	return d;
}

qbool TouchJoinPreview_Camera(void)
{
	vec3_t org, ang;
	double now;
	float f;

	if (!TouchJoinPreview_Active())
		return false;

	JP_SetScene();
	JP_ChooseTarget();
	JP_TargetCamera(org, ang);

	now = Sys_DirtyTime();
	if (!jp_have_cam)
	{
		VectorCopy(org, jp_org);
		VectorCopy(ang, jp_ang);
		jp_have_cam = true;
		// a jump from wherever the menu camera was: start at the target
		VectorCopy(org, r_refdef.view.origin);
		Matrix4x4_CreateFromQuakeEntity(&r_refdef.view.matrix, org[0], org[1], org[2], ang[0], ang[1], ang[2], 1);
	}
	else
	{
		// an exponential approach, so a moving target does not shake the view
		vec3_t delta;
		f = bound(0.0f, (float)((now - jp_cam_time) * 8.0), 1.0f);
		VectorSubtract(org, jp_org, delta);
		VectorMA(jp_org, f, delta, jp_org);
		jp_ang[PITCH] += JP_AngleDelta(jp_ang[PITCH], ang[PITCH]) * f;
		jp_ang[YAW] += JP_AngleDelta(jp_ang[YAW], ang[YAW]) * f;
		jp_ang[ROLL] = 0;
		VectorCopy(jp_org, r_refdef.view.origin);
		Matrix4x4_CreateFromQuakeEntity(&r_refdef.view.matrix, jp_org[0], jp_org[1], jp_org[2], jp_ang[0], jp_ang[1], jp_ang[2], 1);
	}
	jp_cam_time = now;
	jp_drew = true;
	return true;
}

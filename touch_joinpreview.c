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

static cvar_t touch_join_preview = {CF_CLIENT, "touch_join_preview", "1",
	"draw the world from a spectator camera as soon as a joining client has it, with the load's progress as a bar, instead of the full-screen loading page (Godnotic); 0 = the loading page all the way"};

static int jp_target;            // entity number the camera follows, 0 = none
static double jp_target_seen;    // last time it moved or fired, or was chosen
static qbool jp_drew;            // a preview frame was drawn since the last swap
static vec3_t jp_org, jp_ang;    // the smoothed camera
static qbool jp_have_cam;        // jp_org and jp_ang are usable
static double jp_cam_time;       // when the camera last moved

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
	if (!touch_join_preview.integer)
		return false;
	if (cls.state != ca_connected || cls.signon >= SIGNONS)
		return false;
	if (!scr_refresh.integer || vid_hidden)
		return false;
	if (cls.demoplayback || cls.timedemo)
		return false;   // a demo or a timedemo has its own view
	if (!cl.worldmodel || !cl.worldmodel->brush.num_leafs || !cl.worldmodel->brush.data_leafs)
		return false;
	if (key_consoleactive & KEY_CONSOLEACTIVE_FORCED)
		return false;
	return true;
}

qbool TouchJoinPreview_Drew(void)
{
	qbool drew = jp_drew;
	jp_drew = false;
	return drew;
}

// The world entity and the scene the renderer expects of a game frame: during
// a signon CL_UpdateWorld leaves the entity list empty, so the world can be
// drawn from it as it stands.
static void JP_SetScene(void)
{
	entity_t *world = &cl.entities[0];
	r_refdef.scene.numentities = 0;
	r_refdef.scene.numlights = 0;
	r_refdef.scene.time = cl.time;
	world->render.model = cl.worldmodel;
	world->render.matrix = identitymatrix;
	world->render.flags = RENDER_SHADOW | (r_fullbright.integer ? 0 : RENDER_LIGHT);
	world->render.alpha = 1;
	VectorSet(world->render.colormod, 1, 1, 1);
	VectorSet(world->render.glowmod, 1, 1, 1);
	r_refdef.scene.worldentity = &world->render;
	r_refdef.scene.worldmodel = cl.worldmodel;
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

/*
Copyright (C) 2026 Godnotic contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/

/*
The smart DASH decision (touch_dash.c): dodge, or fire an explosive at the
ground, a wall or the floor behind to launch the player up or forward -- a
weapon jump -- and which weapon, aim and dodge combination gets them where
they are going.

Pure decision, no engine calls. The world is reached through the callbacks of
touch_rescue.h (tr_world_t), so scripts/test-touch-dash.sh runs it against a
world of boxes; the includer supplies qbool, true and false.

What it works out
- Where the player is going: a gap ahead with ground on the far side (forward),
  or a ledge or platform within reach above (up). With neither, an open launch
  (as far as possible forward, as high as possible up), chosen by the move
  stick and the view pitch. The caller can force either direction.
- For every candidate -- weapon x fire direction x with or without a dodge --
  where the player comes down: the blast point is found with a ray along the
  fire direction from the eye (the shot flies to the point the view ray hits),
  the push follows the server's own rule (qcsrc/server/damage.qc,
  RadiusDamageForSource and damage_explosion_calcpush):
      damage = core * f + edge * (1 - f),  f = 1 - gap / radius
      E      = damageforcescale * force * damage / max(core, edge) * normalize(centre - blast)
      push   = E * max(0, 1 - (E . v) / (speedfactor * |E|^2))
  with v the player's velocity when the blast arrives, then the arc under
  gravity from there (TouchRescue_Predict). Air control is not modelled.
- Time: switching to a weapon not in hand costs switch_time before the shot,
  the projectile then flies at the weapon's speed; the player moves meanwhile
  (a dodge's impulse is applied at once).
- Safety: health after the blast's self damage (selfdamagepercent, armor
  absorbing armor_block of it) must stay at min_health, and one boost never
  costs more than max_self. Nothing is fired when no candidate passes.

The balance numbers come from the caller (td_cfg_t): the server's own where
the client can read them, the stock bal-wep-xonotic.cfg ones otherwise
(TouchDash_StockCfg), scaled by a per-weapon calibration the caller learns
from what a boost really did.
*/

#ifndef TOUCH_DASHPLAN_H
#define TOUCH_DASHPLAN_H

#include "touch_rescue.h"
#include "touch_weapons.h"

#define TD_MAX_ROWS 8

// Directions.
#define TD_AUTO 0
#define TD_UP   1
#define TD_FWD  2

// Actions.
#define TD_NONE   0
#define TD_DODGE  1  // a plain dash: +dodge
#define TD_BOOST  2  // a weapon boost (with a dodge too when plan.dodge)

// Player box and eye, Xonotic's PL_MIN / PL_MAX and view offset.
#define TD_MINS_Z (-24.0f)
#define TD_MAXS_Z 45.0f
#define TD_HALF_W 16.0f

// What a weapon gives a blast: the stock balance's primary fire.
typedef struct td_wep_s
{
	const char *netname;
	float core, edge, radius, force;
	float speed;      // projectile speed, units/s
	float cal;        // measured push over predicted push (1 until learned)
	qbool server;     // numbers read from the server, not the stock table
} td_wep_t;

typedef struct td_cfg_s
{
	td_wep_t rows[TD_MAX_ROWS];
	int nrows;
	float selfpercent;   // g_balance_selfdamagepercent
	float forcescale;    // g_player_damageforcescale (pushes on players)
	float forcefactor;   // g_weaponforcefactor
	float speedfactor;   // g_balance_damagepush_speedfactor
	float armor_block;   // g_balance_armor_blockpercent
	float center_h;      // height of the push centre above the origin (self blast: the shot origin)
	float eye_h;         // eye above the origin (viewheight)
	float gravity;
	float maxspeed;      // run speed, for a player pressing the stick
	float jump_speed;
	int no_push;         // the server does not push the shooter (force off, instagib-like)
	int dodging;         // 1 yes, 0 no, -1 unknown
	float dodge_horiz, dodge_up;
	float switch_time;   // seconds to bring up another weapon
	float min_health;    // health that must be left after the blast
	float max_self;      // the most health one boost may cost
	float margin;        // a boost must come this many units nearer the goal than a dodge to be chosen over it
	float reach_tol;     // a landing this near the goal (horizontal) is there
	float range;         // how far ahead a gap or ledge is looked for
} td_cfg_t;

typedef struct td_in_s
{
	float org[3], vel[3];
	int onground;
	float view_yaw, view_pitch;      // degrees, pitch positive looking down
	float stick_fwd, stick_side;     // -1..1, forward and right positive
	float health, armor;
	int held;                        // touch_weapons[] index in hand, -1 unknown
	unsigned owned;                  // bit per touch_weapons[] index
	int ammo[TW_NUM_WEAPONS];        // reserve, -1 unlimited
	int force_dir;                   // TD_AUTO, TD_UP, TD_FWD
	int force_weapon;                // touch_weapons[] index, -1 auto
} td_in_t;

typedef struct td_plan_s
{
	int action;                      // TD_NONE, TD_DODGE, TD_BOOST
	int dir;                         // TD_UP or TD_FWD (what it aims at)
	int weapon;                      // touch_weapons[] index, -1
	int dodge;                       // a dodge goes with the blast
	float ddir[2];                   // the dodge's direction, world horizontal unit vector
	float yaw, pitch;                // fire angles, degrees, pitch positive down
	float land[3];
	float land_time;
	int outcome;                     // TR_LAND, TR_VOID, TR_DEATH
	float self_damage;               // health the blast costs
	float fire_delay;                // seconds before the shot (weapon switch)
	float push[3];
	float blast[3];
	float gap;
	int have_goal;
	float goal[3];
	int goal_kind;                   // TD_UP ledge, TD_FWD gap, 0 none
	float miss;                      // landing to goal, horizontal + shortfall
	float dodge_miss, jump_miss;     // the same for a plain dodge and for a jump alone
	int reaches;
	int candidates;                  // evaluated, for the debug line
	const char *why;
} td_plan_t;

// clang-format off
// netname, core, edge, radius, force, speed. By push strength; the same rows
// as touch_boost.h, with the projectile speed. Mortar, Seeker, Fireball and
// Crylink are not here: lobbed, slow or pulling.
static void TouchDash_StockCfg(td_cfg_t *c)
{
	static const td_wep_t rows[] =
	{
		{ "devastator", 80,   40, 110, 400, 1100, 1, false },
		{ "blaster",    20,   10,  60, 375, 6000, 1, false },
		{ "electro",    40,   20, 100, 200, 2500, 1, false },
		{ "hagar",      25, 12.5,  65, 100, 2200, 1, false },
	};
	memset(c, 0, sizeof(*c));
	memcpy(c->rows, rows, sizeof(rows));
	c->nrows = (int)(sizeof(rows) / sizeof(rows[0]));
	c->selfpercent = 0.65f;
	c->forcescale = 2.0f;
	c->forcefactor = 1.0f;
	c->speedfactor = 2.5f;
	c->armor_block = 0.5f;
	c->center_h = 28.0f;
	c->eye_h = 35.0f;
	c->gravity = 800.0f;
	c->maxspeed = 320.0f;
	c->jump_speed = 270.0f;
	c->dodging = -1;
	c->dodge_horiz = 400.0f;
	c->dodge_up = 200.0f;
	c->switch_time = 0.35f;
	c->min_health = 25.0f;
	c->max_self = 70.0f;
	c->margin = 48.0f;
	c->reach_tol = 110.0f;
	c->range = 900.0f;
}
// clang-format on

// ---------------------------------------------------------------------------
// Small vector helpers
// ---------------------------------------------------------------------------
static float td_hlen(const float a[3]) { return sqrtf(a[0] * a[0] + a[1] * a[1]); }

static void td_dir(float yaw, float pitch, float out[3])
{
	float cp = cosf(pitch * TR_DEG2RAD);
	out[0] = cp * cosf(yaw * TR_DEG2RAD);
	out[1] = cp * sinf(yaw * TR_DEG2RAD);
	out[2] = -sinf(pitch * TR_DEG2RAD);
}

// The row for a touch_weapons[] index, or NULL.
static td_wep_t *TouchDash_Row(td_cfg_t *c, int weapon)
{
	int i;
	if (weapon < 0 || weapon >= TW_NUM_WEAPONS)
		return NULL;
	for (i = 0; i < c->nrows; i++)
		if (!strcmp(touch_weapons[weapon].netname, c->rows[i].netname))
			return &c->rows[i];
	return NULL;
}

static int td_row_weapon(const td_wep_t *r)
{
	int i;
	for (i = 0; i < TW_NUM_WEAPONS; i++)
		if (!strcmp(touch_weapons[i].netname, r->netname))
			return i;
	return -1;
}

// ---------------------------------------------------------------------------
// The player's motion before the blast arrives
// ---------------------------------------------------------------------------
// With a dodge the player leaves the ground at once at the dodge's velocity;
// on the ground without one they stand (or keep running); airborne they fall.
static void td_motion(const td_cfg_t *c, const td_in_t *in, int dodge, const float dodge_dir[3], float t, float pos[3], float vel[3])
{
	float v0[3];
	int air = !in->onground;
	memcpy(v0, in->vel, sizeof(v0));
	if (dodge)
	{
		v0[0] += dodge_dir[0] * c->dodge_horiz;
		v0[1] += dodge_dir[1] * c->dodge_horiz;
		v0[2] = (v0[2] > 0 ? v0[2] : 0) + c->dodge_up;
		air = 1;
	}
	pos[0] = in->org[0] + v0[0] * t;
	pos[1] = in->org[1] + v0[1] * t;
	vel[0] = v0[0];
	vel[1] = v0[1];
	if (air)
	{
		pos[2] = in->org[2] + v0[2] * t - 0.5f * c->gravity * t * t;
		vel[2] = v0[2] - c->gravity * t;
	}
	else
	{
		pos[2] = in->org[2];
		vel[2] = 0;
	}
}

// Distance from point p to the player's box at origin pos.
static float td_box_gap(const float pos[3], const float p[3])
{
	float d[3], lo, hi;
	int i;
	for (i = 0; i < 3; i++)
	{
		lo = pos[i] + (i == 2 ? TD_MINS_Z : -TD_HALF_W);
		hi = pos[i] + (i == 2 ? TD_MAXS_Z : TD_HALF_W);
		d[i] = p[i] < lo ? lo - p[i] : p[i] > hi ? p[i] - hi : 0;
	}
	return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// One blast on the player at `pos` moving at `vel`: the push, and the damage
// before self-damage scaling. 0 when out of reach.
static float td_blast_push(const td_cfg_t *c, const td_wep_t *r, const float blast[3], const float pos[3], const float vel[3], float push[3], float *gap_out)
{
	float gap = td_box_gap(pos, blast), f, dmg, frac, centre[3], e[3], len, ee, mult, big;
	*gap_out = gap;
	push[0] = push[1] = push[2] = 0;
	if (gap >= r->radius)
		return 0;
	f = 1 - gap / r->radius;
	dmg = r->core * f + r->edge * (1 - f);
	if (dmg <= 0)
		return 0;
	big = r->core > r->edge ? r->core : r->edge;
	frac = dmg / big;
	centre[0] = pos[0];
	centre[1] = pos[1];
	centre[2] = pos[2] + c->center_h;
	e[0] = centre[0] - blast[0];
	e[1] = centre[1] - blast[1];
	e[2] = centre[2] - blast[2];
	len = sqrtf(tr_dot(e, e));
	if (len < 0.01f)
	{
		e[0] = 0; e[1] = 0; e[2] = 1; len = 1;
	}
	{
		float s = frac * r->force * c->forcefactor * c->forcescale / len;
		e[0] *= s; e[1] *= s; e[2] *= s;
	}
	ee = tr_dot(e, e);
	mult = 1;
	if (c->speedfactor >= 1 && ee > 0)
	{
		mult = 1 - tr_dot(e, vel) / (c->speedfactor * ee);
		if (mult < 0)
			mult = 0;
	}
	mult *= r->cal;
	push[0] = e[0] * mult;
	push[1] = e[1] * mult;
	push[2] = e[2] * mult;
	return dmg;
}

// Health lost to a blast of `dmg` to the box: selfdamagepercent, and armor
// takes armor_block of it while it lasts.
static float td_health_loss(const td_cfg_t *c, float dmg, float armor)
{
	float take = dmg * c->selfpercent, save = take * c->armor_block;
	if (save > armor)
		save = armor;
	return take - save;
}

// ---------------------------------------------------------------------------
// Where the player is going
// ---------------------------------------------------------------------------
// The way they head: the stick, else the way they are moving, else the view.
static void td_heading(const td_in_t *in, float h[3])
{
	float yr = in->view_yaw * TR_DEG2RAD, fx = cosf(yr), fy = sinf(yr), rx = sinf(yr), ry = -cosf(yr);
	float sx, sy, l;
	sx = fx * in->stick_fwd + rx * in->stick_side;
	sy = fy * in->stick_fwd + ry * in->stick_side;
	l = sqrtf(sx * sx + sy * sy);
	if (l > 0.3f)
	{
		h[0] = sx / l; h[1] = sy / l; h[2] = 0;
		return;
	}
	l = td_hlen(in->vel);
	if (l > 150)
	{
		h[0] = in->vel[0] / l; h[1] = in->vel[1] / l; h[2] = 0;
		return;
	}
	h[0] = fx; h[1] = fy; h[2] = 0;
}

static float td_stick(const td_in_t *in) { return sqrtf(in->stick_fwd * in->stick_fwd + in->stick_side * in->stick_side); }

// The ground the player stands on or would fall to: its origin height.
static int td_ground(tr_world_t *w, const float org[3], float *z)
{
	tr_fall_t f;
	if (TouchRescue_Below(w, org, 0, &f) != TR_LAND)
		return 0;
	*z = f.point[2];
	return 1;
}

// A gap ahead: ground at about the player's level breaks off, and ground at
// least that high continues further on. goal is a point well inside the far
// side; *edge the distance to the near lip.
static int TouchDash_FindGap(tr_world_t *w, const td_cfg_t *c, const td_in_t *in, const float h[3], float goal[3], float *edge)
{
	float z0, d, near_d = -1, far_d = -1, far_z = 0;
	int bad = 0;
	if (!td_ground(w, in->org, &z0))
		return 0;
	for (d = 32; d <= c->range; d += 32)
	{
		float p[3], z;
		tr_trace_t tr;
		tr_fall_t f;
		int land;
		p[0] = in->org[0] + h[0] * d;
		p[1] = in->org[1] + h[1] * d;
		p[2] = z0 + 36;
		// a wall at about waist height ends the search: that is a ledge, not a gap
		{
			float q[3];
			q[0] = in->org[0] + h[0] * (d - 32);
			q[1] = in->org[1] + h[1] * (d - 32);
			q[2] = z0 + 36;
			tr_trace(w, q, p, 1, &tr);
			if (tr.fraction < 1 && tr.hit == TR_HIT_SOLID && tr.normal[2] < TR_FLOOR_NZ)
				return 0;
		}
		land = TouchRescue_Below(w, p, 0, &f) == TR_LAND;
		z = f.point[2];
		if (land && z >= z0 - 40 && z <= z0 + 40)
		{
			if (bad >= 2 && near_d >= 0)
			{
				far_d = d;
				far_z = z;
				break;
			}
			bad = 0;
			near_d = -1;
		}
		else
		{
			if (!bad)
				near_d = d - 32;
			bad++;
			if (land && z > z0 + 40)
				return 0; // a wall of ground rising: a ledge
		}
	}
	if (far_d < 0)
		return 0;
	{
		float g[3];
		g[0] = in->org[0] + h[0] * (far_d + 48);
		g[1] = in->org[1] + h[1] * (far_d + 48);
		g[2] = far_z;
		// keep the goal on ground when 48 further is the far wall
		memcpy(goal, g, sizeof(g));
		if (td_ground(w, g, &g[2]))
			goal[2] = g[2];
		else
			goal[2] = far_z;
	}
	*edge = near_d;
	return 1;
}

// A ledge or platform within reach: a wall ahead, ground on top of it. goal is
// a point 40 units in from the lip.
static int TouchDash_FindLedge(tr_world_t *w, const td_cfg_t *c, const td_in_t *in, const float h[3], float goal[3], float *height)
{
	float z0, dw = -1, H;
	tr_trace_t tr;
	float a[3], b[3];
	(void)c;
	if (!td_ground(w, in->org, &z0))
		z0 = in->org[2];
	// the wall: a trace at waist height
	a[0] = in->org[0]; a[1] = in->org[1]; a[2] = z0 + 8;
	b[0] = a[0] + h[0] * 192; b[1] = a[1] + h[1] * 192; b[2] = a[2];
	tr_trace(w, a, b, 1, &tr);
	if (tr.fraction >= 1 || tr.hit != TR_HIT_SOLID || tr.normal[2] >= TR_FLOOR_NZ)
		return 0;
	dw = tr.fraction * 192;
	for (H = 56; H <= 400; H += 24)
	{
		float p[3], q[3];
		tr_fall_t f;
		p[0] = in->org[0]; p[1] = in->org[1]; p[2] = z0 + H;
		q[0] = p[0] + h[0] * (dw + 56); q[1] = p[1] + h[1] * (dw + 56); q[2] = p[2];
		tr_trace(w, p, q, 1, &tr);
		if (tr.fraction < 1 || tr.startsolid)
			continue; // still wall at this height
		// clear: ground on top?
		if (TouchRescue_Below(w, q, 120, &f) == TR_LAND && f.point[2] >= z0 + 40 && f.point[2] <= z0 + H + 8)
		{
			goal[0] = in->org[0] + h[0] * (dw + 40);
			goal[1] = in->org[1] + h[1] * (dw + 40);
			goal[2] = f.point[2];
			*height = f.point[2] - z0;
			return 1;
		}
		return 0;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Outcome of a launch
// ---------------------------------------------------------------------------
typedef struct td_out_s
{
	float land[3], time;
	int outcome;
	float apex;       // the highest the player gets (origin z)
} td_out_t;

// The flight after a launch: stepped finely (TouchRescue_Predict's long
// chords clip the lip of a short hop), sliding along walls and ceilings,
// until it lands, enters a death volume or falls out of the world.
// A landing this close to the lowest point of the map is not trusted.
#define TD_BOTTOM_MARGIN 160.0f
// ...nor, with no goal to reach, a drop this far below where the player stands.
#define TD_DROP_MAX 320.0f
#define TD_STEP    0.04f
#define TD_MAXSTEP 90
static void td_flight(tr_world_t *w, const td_cfg_t *c, const float pos0[3], const float vel0[3], td_out_t *o)
{
	float pos[3], v[3], g = w->gravity > 0 ? w->gravity : c->gravity, t = 0;
	int i;
	memcpy(pos, pos0, sizeof(pos));
	memcpy(v, vel0, sizeof(v));
	o->apex = pos[2];
	o->outcome = TR_VOID;
	for (i = 0; i < TD_MAXSTEP; i++)
	{
		float next[3], df;
		tr_trace_t tr;
		next[0] = pos[0] + v[0] * TD_STEP;
		next[1] = pos[1] + v[1] * TD_STEP;
		next[2] = pos[2] + v[2] * TD_STEP - 0.5f * g * TD_STEP * TD_STEP;
		tr_trace(w, pos, next, 1, &tr);
		df = tr_death(w, pos, next);
		if (tr.startsolid && i == 0)
		{
			memcpy(o->land, pos, sizeof(o->land));
			o->time = 0;
			o->outcome = TR_LAND;
			return;
		}
		if (df <= (tr.fraction < 1 ? tr.fraction : 1.0f) && df <= 1)
		{
			o->land[0] = pos[0] + (next[0] - pos[0]) * df;
			o->land[1] = pos[1] + (next[1] - pos[1]) * df;
			o->land[2] = pos[2] + (next[2] - pos[2]) * df;
			o->time = t + TD_STEP * df;
			o->outcome = TR_DEATH;
			return;
		}
		if (tr.fraction < 1)
		{
			float vn;
			if (tr.hit == TR_HIT_DEATH)
			{
				memcpy(o->land, tr.endpos, sizeof(o->land));
				o->time = t + TD_STEP * tr.fraction;
				o->outcome = TR_DEATH;
				return;
			}
			if (tr.hit == TR_HIT_WATER || (tr.normal[2] >= TR_FLOOR_NZ && v[2] <= 0))
			{
				memcpy(o->land, tr.endpos, sizeof(o->land));
				o->time = t + TD_STEP * tr.fraction;
				o->outcome = TR_LAND;
				return;
			}
			// a wall, a ceiling, or a slope on the way up: slide along it
			vn = tr_dot(v, tr.normal);
			if (vn < 0)
			{
				v[0] -= tr.normal[0] * vn;
				v[1] -= tr.normal[1] * vn;
				v[2] -= tr.normal[2] * vn;
			}
			pos[0] = tr.endpos[0] + tr.normal[0] * 0.2f;
			pos[1] = tr.endpos[1] + tr.normal[1] * 0.2f;
			pos[2] = tr.endpos[2] + tr.normal[2] * 0.2f;
		}
		else
			memcpy(pos, next, sizeof(pos));
		v[2] -= g * TD_STEP;
		t += TD_STEP;
		if (pos[2] > o->apex)
			o->apex = pos[2];
		if (pos[2] < w->floor_z)
		{
			memcpy(o->land, pos, sizeof(o->land));
			o->time = t;
			o->outcome = TR_VOID;
			return;
		}
	}
	memcpy(o->land, pos, sizeof(o->land));
	o->time = t;
	o->outcome = TR_VOID;
}

// How far a landing is from being on the goal's surface: the goal sits 40-48
// units inside the far side of a gap or the top of a ledge, so a landing that
// far in, or anywhere beyond (up to TD_OVER, a platform's depth), counts.
// Shortfall along the heading, overshoot, lateral miss past TD_LATERAL, and
// twice any height short of the goal's surface add up.
#define TD_INSIDE  48.0f
#define TD_OVER    700.0f
#define TD_LATERAL 120.0f
static float td_miss(const float land[3], const float goal[3], const float h[3])
{
	float dx = land[0] - goal[0], dy = land[1] - goal[1], s = goal[2] - 24 - land[2];
	float along = dx * h[0] + dy * h[1], lat = fabsf(dx * h[1] - dy * h[0]);
	float m = 0;
	if (-along > TD_INSIDE)
		m += -along - TD_INSIDE;
	if (along > TD_OVER)
		m += along - TD_OVER;
	if (lat > TD_LATERAL)
		m += lat - TD_LATERAL;
	if (s > 0)
		m += 2 * s;
	return m;
}

static int td_reaches(const td_cfg_t *c, const td_out_t *o, const float goal[3], const float h[3])
{
	(void)c;
	return o->outcome == TR_LAND && td_miss(o->land, goal, h) <= 12.0f;
}

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------
typedef struct td_cand_s
{
	int weapon, dodge, dir;
	float yaw, pitch;
	float blast[3], gap, push[3], health_loss, t_fire;
	td_out_t out;
	float score;       // lower is better
} td_cand_t;

// One candidate's blast and flight. Returns 0 when nothing is hit, it is out
// of reach, or it would hurt too much.
static int td_eval(tr_world_t *w, const td_cfg_t *c, const td_in_t *in, const td_wep_t *r, int weapon, int dodge, const float ddir[3],
	float yaw, float pitch, td_cand_t *out)
{
	float t_f = weapon != in->held ? c->switch_time : 0;
	float pos[3], vel[3], eye[3], d[3], end[3], t_hit, dist, push[3], dmg, gap, hl;
	tr_trace_t tr;
	float pos2[3], vel2[3], v2[3];
	td_motion(c, in, dodge, ddir, t_f, pos, vel);
	eye[0] = pos[0]; eye[1] = pos[1]; eye[2] = pos[2] + c->eye_h;
	td_dir(yaw, pitch, d);
	tr_ma(eye, 260.0f, d, end);
	tr_trace(w, eye, end, 0, &tr);
	if (tr.fraction >= 1 || tr.startsolid || tr.hit != TR_HIT_SOLID)
		return 0;
	dist = tr.fraction * 260.0f;
	if (dist < 12)
		return 0; // the muzzle is inside the surface
	t_hit = t_f + dist / (r->speed > 0 ? r->speed : 1000.0f);
	td_motion(c, in, dodge, ddir, t_hit, pos2, vel2);
	if (c->no_push)
		return 0;
	dmg = td_blast_push(c, r, tr.endpos, pos2, vel2, push, &gap);
	if (dmg <= 0)
		return 0;
	hl = td_health_loss(c, dmg, in->armor);
	if (hl > c->max_self || in->health - hl < c->min_health)
		return 0;
	v2[0] = vel2[0] + push[0];
	v2[1] = vel2[1] + push[1];
	v2[2] = vel2[2] + push[2];
	if (v2[2] < 60 && in->onground && !dodge)
		return 0; // not off the ground: no launch
	out->weapon = weapon;
	out->dodge = dodge;
	out->yaw = yaw;
	out->pitch = pitch;
	memcpy(out->blast, tr.endpos, sizeof(out->blast));
	out->gap = gap;
	memcpy(out->push, push, sizeof(out->push));
	out->health_loss = hl;
	out->t_fire = t_f;
	td_flight(w, c, pos2, v2, &out->out);
	// the bottom of the map is where it kills (a nodrop or hurt floor the client cannot see): never a landing there
	if (out->out.outcome == TR_LAND && out->out.land[2] < w->floor_z + TD_BOTTOM_MARGIN)
		out->out.outcome = TR_VOID;
	return 1;
}

static void td_better(td_cand_t *best, int *have, const td_cand_t *c)
{
	if (!*have || c->score < best->score)
	{
		*best = *c;
		*have = 1;
	}
}

/*
The decision. Fills `plan`; plan->action says what to do. dir is TD_AUTO,
TD_UP or TD_FWD (from the player's choice, or the in->force_dir).
*/
static void TouchDash_Plan(tr_world_t *w, td_cfg_t *c, const td_in_t *in, td_plan_t *plan)
{
	float h[3], ddir[3], stick = td_stick(in);
	float fgoal[3], lgoal[3], edge = 0, height = 0;
	int have_gap = 0, have_ledge = 0, goal_kind = 0, want_dir, forced = in->force_dir != TD_AUTO;
	float goal[3] = { 0, 0, 0 };
	int have_goal = 0, i, ci;
	int weps[TD_MAX_ROWS], nweps = 0, dodge_ok = c->dodging != 0;
	td_cand_t best, cand;
	int have_best = 0;
	td_out_t base, dod;
	float dod_miss = 0;
	float dyaw_up[] = { 0, 60, -60, 120, -120, 180 };
	float dyaw_fwd[] = { 180, 150, -150, 120, -120, 90, -90 };
	float pit_up[] = { 89, 80, 70, 60, 50, 40 };
	float pit_fwd[] = { 15, 22, 30, 38, 46, 54, 62 };

	memset(plan, 0, sizeof(*plan));
	memset(&best, 0, sizeof(best));
	memset(&dod, 0, sizeof(dod));
	plan->weapon = -1;
	plan->why = "";
	w->gravity = w->gravity > 0 ? w->gravity : c->gravity;
	if (in->health <= 0)
	{
		plan->why = "dead";
		return;
	}
	td_heading(in, h);
	memcpy(ddir, h, sizeof(ddir));
	if (stick > 0.3f)
	{
		float yr = in->view_yaw * TR_DEG2RAD, sx, sy, l;
		sx = cosf(yr) * in->stick_fwd + sinf(yr) * in->stick_side;
		sy = sinf(yr) * in->stick_fwd - cosf(yr) * in->stick_side;
		l = sqrtf(sx * sx + sy * sy);
		if (l > 0) { ddir[0] = sx / l; ddir[1] = sy / l; ddir[2] = 0; }
	}

	plan->ddir[0] = ddir[0];
	plan->ddir[1] = ddir[1];

	// where they are going
	if (in->force_dir != TD_UP)
		have_gap = TouchDash_FindGap(w, c, in, h, fgoal, &edge);
	if (in->force_dir != TD_FWD)
		have_ledge = TouchDash_FindLedge(w, c, in, h, lgoal, &height);
	if (have_ledge && (!have_gap || edge > 160 || stick < 0.3f))
	{
		memcpy(goal, lgoal, sizeof(goal));
		goal_kind = TD_UP;
		have_goal = 1;
	}
	else if (have_gap)
	{
		memcpy(goal, fgoal, sizeof(goal));
		goal_kind = TD_FWD;
		have_goal = 1;
	}
	if (have_goal)
	{
		plan->have_goal = 1;
		memcpy(plan->goal, goal, sizeof(plan->goal));
		plan->goal_kind = goal_kind;
	}
	if (in->force_dir)
		want_dir = in->force_dir;
	else if (have_goal)
		want_dir = goal_kind;
	else if (in->view_pitch < -25.0f)
		want_dir = TD_UP;
	else
		want_dir = stick > 0.3f ? TD_FWD : TD_UP;
	plan->dir = want_dir;

	// without a jump and with one: what the player reaches on their own
	{
		float pos[3], vel[3];
		td_motion(c, in, 0, ddir, 0, pos, vel);
		if (stick > 0.3f && in->onground)
		{
			float sp = td_hlen(vel);
			if (sp < c->maxspeed)
			{
				vel[0] += ddir[0] * (c->maxspeed - sp);
				vel[1] += ddir[1] * (c->maxspeed - sp);
			}
		}
		if (in->onground)
			vel[2] = c->jump_speed;
		td_flight(w, c, pos, vel, &base);
	}
	if (have_goal)
		plan->jump_miss = td_miss(base.land, goal, h);
	// a plain dodge
	if (dodge_ok)
	{
		float pos[3], vel[3];
		td_motion(c, in, 1, ddir, 0, pos, vel);
		td_flight(w, c, pos, vel, &dod);
		if (have_goal)
			plan->dodge_miss = dod_miss = td_miss(dod.land, goal, h);
	}

	// the weapons that can do it
	for (i = 0; i < c->nrows; i++)
	{
		int wi = td_row_weapon(&c->rows[i]);
		if (wi < 0)
			continue;
		if (in->force_weapon >= 0 && in->force_weapon != wi)
			continue;
		if (!(in->owned & (1u << wi)))
			continue;
		if (touch_weapons[wi].cost > 0 && in->ammo[wi] >= 0 && in->ammo[wi] < touch_weapons[wi].cost)
			continue;
		weps[nweps++] = wi;
	}
	if (!nweps)
		plan->why = "no weapon";

	for (ci = 0; ci < nweps; ci++)
	{
		const td_wep_t *r = TouchDash_Row(c, weps[ci]);
		int dd, k, m;
		for (dd = 0; dd <= 1; dd++)
		{
			float *dy = want_dir == TD_UP ? dyaw_up : dyaw_fwd, *pp = want_dir == TD_UP ? pit_up : pit_fwd;
			int ny = want_dir == TD_UP ? 6 : 7, np = want_dir == TD_UP ? 6 : 7;
			if (dd && (!dodge_ok || stick < 0.3f))
				continue;
			for (k = 0; k < ny; k++)
				for (m = 0; m < np; m++)
				{
					float yaw = atan2f(h[1], h[0]) * (1 / TR_DEG2RAD) + dy[k];
					if (!td_eval(w, c, in, r, weps[ci], dd, ddir, yaw, pp[m], &cand))
						continue;
					plan->candidates++;
					cand.dir = want_dir;
					if (have_goal)
					{
						cand.score = td_miss(cand.out.land, goal, h) + (cand.out.outcome == TR_LAND ? 0 : 5000);
					}
					else if (want_dir == TD_UP)
					{
						// as high as possible, and not drifting off: a clean launch up
						float dx = cand.out.land[0] - in->org[0], dy = cand.out.land[1] - in->org[1];
						cand.score = -cand.out.apex + 0.5f * sqrtf(dx * dx + dy * dy) + (cand.out.outcome == TR_LAND ? 0 : 5000);
					}
					else
					{
						float along = (cand.out.land[0] - in->org[0]) * h[0] + (cand.out.land[1] - in->org[1]) * h[1];
						cand.score = -along + (cand.out.outcome == TR_LAND ? 0 : 5000);
					}
					if (!have_goal && cand.out.land[2] < in->org[2] - TD_DROP_MAX)
						cand.score += 5000;
					// a held weapon, less self damage, no dodge needed: small tie-breakers
					cand.score += cand.health_loss * (have_goal ? 0.3f : 2.0f) + (cand.t_fire > 0 ? 6 : 0) + (cand.dodge ? 3 : 0);
					td_better(&best, &have_best, &cand);
				}
		}
	}

	// choose
	if (have_best)
	{
		plan->weapon = best.weapon;
		plan->dodge = best.dodge;
		plan->yaw = best.yaw;
		plan->pitch = best.pitch;
		memcpy(plan->land, best.out.land, sizeof(plan->land));
		plan->land_time = best.out.time;
		plan->outcome = best.out.outcome;
		plan->self_damage = best.health_loss;
		plan->fire_delay = best.t_fire;
		memcpy(plan->push, best.push, sizeof(plan->push));
		memcpy(plan->blast, best.blast, sizeof(plan->blast));
		plan->gap = best.gap;
		if (have_goal)
		{
			plan->miss = td_miss(best.out.land, goal, h);
			plan->reaches = td_reaches(c, &best.out, goal, h);
		}
	}

	if (have_goal)
	{
		int dodge_reaches = dodge_ok && td_reaches(c, &dod, goal, h);
		int jump_reaches = td_reaches(c, &base, goal, h);
		if (!forced && (dodge_reaches || (jump_reaches && !dodge_ok)))
		{
			plan->action = dodge_ok ? TD_DODGE : TD_NONE;
			plan->why = dodge_ok ? "a dodge gets there" : "a jump gets there";
			plan->weapon = -1;
			return;
		}
		if (have_best && plan->reaches && (!dodge_ok || forced || plan->miss + c->margin < dod_miss))
		{
			plan->action = TD_BOOST;
			plan->why = "a boost reaches the goal";
			return;
		}
		if (forced && have_best && plan->outcome == TR_LAND)
		{
			plan->action = TD_BOOST;
			plan->why = "forced direction, nearest";
			return;
		}
		plan->action = dodge_ok ? TD_DODGE : TD_NONE;
		plan->weapon = -1;
		plan->why = have_best ? "no boost reaches the goal" : (nweps ? "no safe boost" : "no weapon");
		return;
	}

	// no goal: open launch
	if (!forced && dodge_ok)
	{
		plan->action = TD_DODGE;
		plan->weapon = -1;
		plan->why = "nothing to reach: a plain dash";
		return;
	}
	if (have_best && plan->outcome == TR_LAND)
	{
		plan->action = TD_BOOST;
		plan->why = want_dir == TD_UP ? "open launch up" : "open launch forward";
		return;
	}
	plan->action = TD_NONE;
	plan->weapon = -1;
	plan->why = have_best ? "no safe landing" : (nweps ? "no safe boost" : "no weapon");
}

#endif

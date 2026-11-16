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
Touch auto boost: the decision to fire an explosive at the floor to launch
the player upward (a blaster or Devastator jump without the finger gymnastics).

Pure decision, no engine calls: the caller passes the view pitch, the
distance a straight-down trace measured from the eye to the floor, and the
weapon in hand; the function answers whether to fire, which weapon to switch
to and what pitch to fire along. It is plain C (the includer supplies qbool,
true and false, as for touch_weapons.h) so that scripts/test-touch-boost.sh
can test it without the rest of the engine.

Physics (qcsrc/server/damage.qc, RadiusDamageForSource): an explosion `gap`
units from the player's box does
    damage = core * f + edge * (1 - f),  f = 1 - gap / radius
and pushes along (player view origin - blast point) with magnitude
    force * damage / max(core, edge).
Self-damage is that damage times g_balance_selfdamagepercent (0.65, stock
balance-xonotic.cfg). The numbers below are bal-wep-xonotic.cfg, primary fire.

Geometry of the launch: the eye stands `floor_dist` above the floor; the eye
is 59 units above the feet (origin at feet + 24, view offset +35:
qcsrc/common/constants.qh PL_MIN/PL_MAX, sv_player_viewoffset), and the
player's box is half a body wide (16) and 69 tall. The shot is fired so the
blast lands on the floor 16 units ahead of the eye's ground projection -- at
the near edge of the player's own box -- so the gap to the box is 0 while the
player stands on the floor, and (floor_dist - 59) once they have fallen that
far above it. From there the push points almost straight up at the eye
(h / sqrt(16^2 + h^2), about 0.96 at a standing eye height) and only its
vertical part lifts.
*/

#ifndef TOUCH_BOOST_H
#define TOUCH_BOOST_H

#include "touch_weapons.h"

// The least downward pitch (degrees; in this engine's convention positive
// pitch looks down) at which a boost may fire. Level play -- aiming at
// enemies, walls, corners -- holds well inside +-45; past 60 the player is
// looking at the floor below, which is the intent the trigger answers, and
// the pitch this function then chooses (atan2(floor_dist, 16), 69..87 for
// every floor in range) is within about 27 of it.
#define TB_PITCH_MIN 60.0f

// The least floor distance (eye to floor) a boost may fire over: below that
// the straight-down trace is inside the player's own box (the eye is 59
// above the feet standing, less crouched) -- there is no floor to push off.
// A trace that found nothing is passed as 0 and fails this too.
#define TB_FLOOR_MIN 32.0f

// Where the blast lands, ahead of the eye's ground projection: the half
// width of the player's own box, so the blast sits at the box's near edge
// and the gap to it is 0.
#define TB_BLAST_AHEAD 16.0f

// Standing eye height above the feet (24 + 35).
#define TB_EYE_HEIGHT 59.0f

// Half the player's box width; the blast-to-box gap uses it too.
#define TB_PLAYER_RADIUS 16.0f

// Share of a self-inflicted blast that reaches health
// (g_balance_selfdamagepercent, balance-xonotic.cfg).
#define TB_SELF_PERCENT 0.65f

// The most self-damage a boost may cost: about one Devastator blast's share
// (80 * 0.65 = 52) -- survivable from 100 health with no armor. It keeps the
// Fireball (130) out of the choice; the smaller weapons pass at every gap.
#define TB_SELF_DAMAGE_MAX 55.0f

// The weapon in hand is kept (no switch, no raise delay) unless the best
// switchable boost weapon pushes this much harder: a 0.2-0.4 s switch costs
// more than the difference for a one-shot jump.
#define TB_HELD_MARGIN 0.9f

// What a weapon offers a floor blast: the stock balance's primary fire.
typedef struct touch_boost_wep_s
{
	const char *netname;
	float core;    // coredamage at the blast centre
	float edge;    // edgedamage at the radius
	float radius;  // splash radius
	float force;   // push strength (forceintensity)
} touch_boost_wep_t;

// clang-format off
// By push strength, best first. Crylink is not here: its force is -50, it
// pulls the player down into the blast. HLAC (15) pushes too little to
// matter and Mine Layer is never auto-switched to. Seeker missile and
// Fireball are never auto-switched to either, but stay in the table for
// when they are already in hand.
static const touch_boost_wep_t touch_boost_weapons[] =
{
	// netname        core  edge  radius  force
	{ "devastator",    80,   40,    110,   400 },
	{ "blaster",       20,   10,     60,   375 },
	{ "mortar",        55,   25,    120,   250 },
	{ "electro",       40,   20,    100,   200 },
	{ "hagar",         25, 12.5,     65,   100 },
	{ "seeker",        30,   10,     80,   150 },
	{ "fireball",     200,   50,    200,   600 },
};
// clang-format on
#define TB_NUM_BOOST_WEAPONS ((int)(sizeof(touch_boost_weapons) / sizeof(touch_boost_weapons[0])))

// The player's box reaches this far above the blast when it has fallen to
// the floor: the feet sit floor_dist - TB_EYE_HEIGHT above it. Reachable
// while that is inside the weapon's splash radius: 119 for the Blaster, 169
// for the Devastator and Mortar.
static float TouchBoost_Reach(const touch_boost_wep_t *b)
{
	return TB_EYE_HEIGHT + b->radius;
}

// Damage one blast does to the player's box at `gap` from it, and the share
// of it that reaches health.
static float TouchBoost_SelfDamage(const touch_boost_wep_t *b, float gap)
{
	float f = 1 - gap / b->radius;
	float dmg = b->core * f + b->edge * (1 - f);
	return dmg > 0 ? dmg * TB_SELF_PERCENT : 0;
}

// Vertical launch one blast gives: the push along the blast-to-eye
// direction, vertical part only (the blast is TB_BLAST_AHEAD ahead of the
// eye's ground projection, `floor_dist` below the eye).
static float TouchBoost_VerticalPush(const touch_boost_wep_t *b, float gap, float floor_dist)
{
	float f = 1 - gap / b->radius;
	float dmg = b->core * f + b->edge * (1 - f);
	float frac = dmg / (b->core > b->edge ? b->core : b->edge);
	float vert = floor_dist / sqrt(TB_BLAST_AHEAD * TB_BLAST_AHEAD + floor_dist * floor_dist);
	return b->force * frac * vert;
}

// The boost row for a weapon index into touch_weapons[] (touch_weapons.h),
// or NULL for an unknown one.
static const touch_boost_wep_t *TouchBoost_Row(int weapon)
{
	int i;
	if (weapon < 0 || weapon >= TW_NUM_WEAPONS)
		return NULL;
	for (i = 0; i < TB_NUM_BOOST_WEAPONS; i++)
		if (!strcmp(touch_weapons[weapon].netname, touch_boost_weapons[i].netname))
			return &touch_boost_weapons[i];
	return NULL;
}

// The touch_weapons[] index of a boost row, or -1.
static int TouchBoost_Index(const touch_boost_wep_t *b)
{
	int i;
	for (i = 0; i < TW_NUM_WEAPONS; i++)
		if (!strcmp(touch_weapons[i].netname, b->netname))
			return i;
	return -1;
}

// Whether a boost row is usable for this floor: within reach and not too
// much self-damage.
static qbool TouchBoost_Usable(const touch_boost_wep_t *b, float gap)
{
	return gap < b->radius && TouchBoost_SelfDamage(b, gap) <= TB_SELF_DAMAGE_MAX;
}

/*
The decision. pitch_deg: view pitch, degrees, positive looking down.
floor_dist: what a straight-down trace from the eye measured to the floor
(0: no floor). weapon: the touch_weapons[] index of the weapon in hand
(-1 unknown). want: the player asks for a boost (trigger held).

Answers: *fire whether to fire; *chosen_weapon the touch_weapons[] index to
switch to and fire (the held one when no switch is needed); *aim_pitch the
pitch to fire along, positive down. When *fire is false the held weapon and
pitch come back unchanged.
*/
static void TouchBoost_Decide(float pitch_deg, float floor_dist, int weapon, qbool want,
	qbool *fire, int *chosen_weapon, float *aim_pitch)
{
	float gap, pitch;
	const touch_boost_wep_t *held_row, *best_row = NULL;
	float best_push = 0, held_push = 0;
	int i, best_i = -1, held_i = -1;

	*fire = false;
	*chosen_weapon = weapon;
	*aim_pitch = pitch_deg;

	if (!want)
		return;
	if (pitch_deg < TB_PITCH_MIN)
		return;
	if (floor_dist < TB_FLOOR_MIN)
		return;

	// The blast lands at the near edge of the player's own box, on the
	// floor below; the box has fallen (floor_dist - 59) above it.
	gap = floor_dist > TB_EYE_HEIGHT ? floor_dist - TB_EYE_HEIGHT : 0;
	pitch = atan2(floor_dist, TB_BLAST_AHEAD) * TW_DEG;

	held_row = TouchBoost_Row(weapon);
	if (held_row && TouchBoost_Usable(held_row, gap))
	{
		held_i = weapon;
		held_push = TouchBoost_VerticalPush(held_row, gap, floor_dist);
	}

	for (i = 0; i < TB_NUM_BOOST_WEAPONS; i++)
	{
		const touch_boost_wep_t *b = &touch_boost_weapons[i];
		const touch_wep_t *w;
		float push;
		if (b == held_row)
			continue; // judged as the held weapon, switchable or not
		w = TouchWep_Find(b->netname);
		if (!w || (w->flags & TW_NO_SWITCH_TO))
			continue; // never auto-switched to
		if (!TouchBoost_Usable(b, gap))
			continue;
		push = TouchBoost_VerticalPush(b, gap, floor_dist);
		if (push > best_push)
		{
			best_push = push;
			best_row = b;
			best_i = TouchBoost_Index(b);
		}
	}

	// What is already in hand is kept unless a switch pushes clearly harder.
	if (held_i >= 0 && (!best_row || held_push >= TB_HELD_MARGIN * best_push))
	{
		best_row = held_row;
		best_i = held_i;
	}

	if (!best_row)
		return;

	*fire = true;
	*chosen_weapon = best_i;
	*aim_pitch = pitch;
}

#endif

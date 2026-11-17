/*
Godnotic: who touch auto-shoot and the aim helpers may aim at.

The engine reads the CSQC's copy of each player -- the server's own CSQC in a
local match, the server's on a public one -- so only what the server networks
with a player can be used here:

- dead: SOLID_CORPSE (CSQCModel), or the CSQC's own .csqcmodel_isdead;
- frozen: .anim_state & ANIMSTATE_FROZEN (BIT(3), common/animdecide.qh). The
  Freeze Tag mutator sets it in its PlayerAnim hook and player_anim sets it for
  the ice status effect; CSQCModel networks the whole anim_state. A frozen
  player takes no damage, so aiming at one only means the enemy beside it is
  not being shot -- and in Freeze Tag it is the enemy who is standing there
  waiting to be shot while the crosshair sits on the ice block;
- spectator or out of the round: the scoreboard's FRAGS_* markers;
- teammate: the same team colour in a team game;
- not drawn: no model, EF_NODRAW, or an alpha fade (invisibility).

Respawn protection (spawnshieldtime) is not networked to the CSQC, so it
cannot be excluded here: nothing in the client's copy of a player says it.

Kept out of touch_aim.c so tests/touch-aim can check it without the engine
(the caller has qbool, as with touch_weapons.h).
*/
#ifndef TOUCH_AIM_TARGETS_H
#define TOUCH_AIM_TARGETS_H

// common/animdecide.qh: ANIMSTATE_FROZEN = BIT(3)
#define TOUCH_AIM_ANIMSTATE_FROZEN 8

typedef enum
{
	TOUCH_AIM_TARGET_OK = 0,
	TOUCH_AIM_TARGET_NO_NAME,   // a slot with nobody in it
	TOUCH_AIM_TARGET_SPECTATOR, // spectator, or out of the round
	TOUCH_AIM_TARGET_MATE,      // a teammate in a team game
	TOUCH_AIM_TARGET_DEAD,      // a corpse, or the CSQC's isdead
	TOUCH_AIM_TARGET_FROZEN,    // frozen: cannot be damaged
	TOUCH_AIM_TARGET_UNDRAWN,   // no model, nodraw, or faded out
	TOUCH_AIM_TARGET_COUNT
} touch_aim_target_t;

typedef struct touch_aim_cand_s
{
	qbool has_name;
	qbool spectator;
	qbool teammate;
	qbool dead;
	qbool frozen;
	qbool drawn;    // modelindex and drawmask set, and no EF_NODRAW
	float alpha;    // 0 = drawn solid
} touch_aim_cand_t;

// The server's anim_state says frozen (Freeze Tag, the ice status effect).
static qbool TouchAim_AnimFrozen(int anim_state)
{
	return (anim_state & TOUCH_AIM_ANIMSTATE_FROZEN) != 0;
}

// Why this player may not be the target, or TOUCH_AIM_TARGET_OK when it may.
// The order is the order the reasons are worth printing in: a frozen player
// is reported as frozen even though it is also standing still and undrawn in
// no way at all.
static int TouchAim_TargetCheck(const touch_aim_cand_t *c)
{
	if (!c->has_name)
		return TOUCH_AIM_TARGET_NO_NAME;
	if (c->spectator)
		return TOUCH_AIM_TARGET_SPECTATOR;
	if (c->teammate)
		return TOUCH_AIM_TARGET_MATE;
	if (c->dead)
		return TOUCH_AIM_TARGET_DEAD;
	if (c->frozen)
		return TOUCH_AIM_TARGET_FROZEN;
	if (!c->drawn)
		return TOUCH_AIM_TARGET_UNDRAWN;
	// invisibility: never reveal a hidden player
	if (c->alpha < 0 || (c->alpha > 0 && c->alpha < 0.4f))
		return TOUCH_AIM_TARGET_UNDRAWN;
	return TOUCH_AIM_TARGET_OK;
}

#endif

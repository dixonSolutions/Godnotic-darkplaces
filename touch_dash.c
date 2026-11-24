/*
Godnotic: the smart DASH button (see touch_dash.h and docs/CONTROLS.md).

A tap does the smart thing: a plain dash (+dodge) where the server has dodging
and that gets the player where they are going, else a weapon boost -- a shot at
the floor, a wall or behind them that launches them up or forward. Which one,
with which weapon and along which aim, is decided in touch_dashplan.h from the
client's own state: position, velocity, health and armor, the weapons held and
their ammo, the move stick and view, the map around (client traces), the
server's mutators (dodging) and, where the server is this machine, its balance
cvars. Where the client cannot read a number it uses the stock one and learns
from what each boost really did (the push it gave against the push predicted).

A boost: switch to the weapon if it is not in hand, turn the view to the aim
(the plan is made again once the weapon is up, from the real state), fire, then
turn the view back and take the old weapon back. Never while a menu, the
console or the chat sheet is open, and never when the blast would leave the
player under the health floor (cl_touch_dash_minhealth).
*/

#include "quakedef.h"
#include "csprogs.h"
#include "touch_hud.h"
#include "touch_aim.h"
#include "touch_hook.h"
#include "touch_dash.h"
#include "touch_dashplan.h"

extern cvar_t vid_touchscreen;
extern kbutton_t in_button6;

cvar_t cl_touch_dash_boost = {CF_CLIENT | CF_ARCHIVE, "cl_touch_dash_boost", "1",
	"Touch controls: DASH can also boost with a weapon (a shot at the floor or a wall) when that gets you where you are going better than a dodge, or dodging is off on the server"};
cvar_t cl_touch_dash_dir = {CF_CLIENT | CF_ARCHIVE, "cl_touch_dash_dir", "0",
	"Touch controls: what DASH boosts toward: 0 whichever it works out, 1 up, 2 forward (long-press DASH to pick)"};
cvar_t cl_touch_dash_weapon = {CF_CLIENT | CF_ARCHIVE, "cl_touch_dash_weapon", "",
	"Touch controls: the weapon DASH boosts with (devastator, blaster, electro, hagar), empty for the best one you have (long-press DASH to pick)"};
cvar_t cl_touch_dash_minhealth = {CF_CLIENT | CF_ARCHIVE, "cl_touch_dash_minhealth", "25",
	"Touch controls: a boost is never fired when the blast would leave you under this much health"};
cvar_t cl_touch_dash_maxself = {CF_CLIENT | CF_ARCHIVE, "cl_touch_dash_maxself", "70",
	"Touch controls: the most health one boost may cost"};
cvar_t cl_touch_dash_switch = {CF_CLIENT | CF_ARCHIVE, "cl_touch_dash_switch", "0.35",
	"Touch controls: seconds a weapon switch takes, counted against the time to impact (a weapon in hand is preferred)"};
cvar_t cl_touch_dash_margin = {CF_CLIENT | CF_ARCHIVE, "cl_touch_dash_margin", "48",
	"Touch controls: a boost must land this many units nearer the goal than a plain dodge to be chosen over it"};
cvar_t cl_touch_dash_range = {CF_CLIENT | CF_ARCHIVE, "cl_touch_dash_range", "900",
	"Touch controls: how far ahead DASH looks for a gap to cross or a ledge to reach (units)"};
cvar_t cl_touch_dash_longpress = {CF_CLIENT | CF_ARCHIVE, "cl_touch_dash_longpress", "0.35",
	"Touch controls: hold DASH this long to pick the direction and weapon (the tap then acts on release); 0 acts on the press and has no picker"};
cvar_t cl_touch_dash_turn = {CF_CLIENT | CF_ARCHIVE, "cl_touch_dash_turn", "1500",
	"Touch controls: how fast the view turns to the boost's aim and back, degrees per second"};
cvar_t cl_touch_dash_learn = {CF_CLIENT | CF_ARCHIVE, "cl_touch_dash_learn", "1",
	"Touch controls: scale each weapon's predicted push by what boosts really gave on this server"};
cvar_t cl_touch_dash_centerh = {CF_CLIENT | CF_ARCHIVE, "cl_touch_dash_centerh", "28",
	"Touch controls: height above the player's origin the server pushes from (the shot origin for your own blast)"};
cvar_t cl_touch_dash_debug = {CF_CLIENT, "cl_touch_dash_debug", "0",
	"Touch controls: print what DASH decides and how each boost went (1), and every candidate count (2)"};

enum { ST_IDLE, ST_SWITCH, ST_AIM, ST_FIRE, ST_RESTORE };

static struct
{
	// the boost
	int st;
	td_plan_t plan;
	int weapon;              // touch_weapons[] index
	int prev_hud;            // TouchHUD index of the weapon to take back, -1
	double t_start, t_state, t_fire;
	float saved[2];          // yaw, pitch to give back
	float aim[2];
	int iter;
	qbool attack;
	qbool dodge_done;
	qbool burst;
	double burst_t0;
	float burst_dir[2];
	int burst_buttons;
	// measure
	qbool measuring, landed_pending;
	float v0[3], pred_push[3], pred_land[3], fire_org[3];
	float health0;
	int fired_weapon;
	double t_measure;
	qbool airborne_seen;
	int nopush_streak;
	// learned about this server
	float cal[TW_NUM_WEAPONS];
	qbool learned_nopush;
	int dodge_ok, dodge_fail;
	// a plain dash being watched
	double dodge_t;
	float dodge_v0, dodge_maxvz;
	qbool dodge_ground;
	// the HUD
	float bx, by, br;
	qbool button_seen;
	double press_time;
	qbool pressing, picker;
	float fx, fy;
	// preview
	double pv_next;
	td_plan_t pv;
	qbool pv_valid;
	char label[24];
	// results
	int boosts, aborted, dashes;
	char last[160];
} td;

static float cvf(const char *name, float def, qbool *found)
{
	cvar_t *v = Cvar_FindVar(&cvars_all, name, ~0);
	if (found)
		*found = v != NULL;
	return v ? v->value : def;
}

static int tw_index(const char *netname)
{
	int i;
	for (i = 0; i < TW_NUM_WEAPONS; i++)
		if (!strcmp(touch_weapons[i].netname, netname))
			return i;
	return -1;
}

static void debug(int level, const char *fmt, ...) DP_FUNC_PRINTF(2);
static void debug(int level, const char *fmt, ...)
{
	va_list args;
	char msg[512];
	if (cl_touch_dash_debug.integer < level)
		return;
	va_start(args, fmt);
	dpvsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);
	Con_Printf("touch dash: %s\n", msg);
}

// ---------------------------------------------------------------------------
// What the client knows
// ---------------------------------------------------------------------------
// Whether the server has dodging: 1 yes, 0 no, -1 not known. A local game
// says so itself (g_dodging); a stock server does not (its mutator list leaves
// dodging out, so the CSQC's active_mutators bit 0 is only ever set by a server
// that adds it), so otherwise what dashes did decides: one that moved the
// player settles it for yes, two that did nothing for no; until then DASH tries.
static int dodging_state(void)
{
	prvm_prog_t *prog = CLVM_prog;
	if (prog && prog->loaded && PRVM_ED_FindGlobal(prog, "active_mutators") && TouchHook_Mutator(0))
		return 1;
	if (sv.active)
		return cvf("g_dodging", 0, NULL) > 0 ? 1 : 0;
	if (td.dodge_ok > 0)
		return 1;
	if (td.dodge_fail >= 2)
		return 0;
	return -1;
}

static const char *dodging_source(void)
{
	prvm_prog_t *prog = CLVM_prog;
	if (prog && prog->loaded && PRVM_ED_FindGlobal(prog, "active_mutators") && TouchHook_Mutator(0))
		return "the server's mutator list";
	if (sv.active)
		return "this machine's server";
	if (td.dodge_ok > 0 || td.dodge_fail >= 2)
		return "learned from dashes";
	return "not announced by the server; DASH tries it";
}

static float csqc_global_float(const char *name, float def)
{
	prvm_prog_t *prog = CLVM_prog;
	mdef_t *d;
	if (!prog || !prog->loaded)
		return def;
	d = PRVM_ED_FindGlobal(prog, name);
	if (!d)
		return def;
	if ((d->type & ~DEF_SAVEGLOBAL) == ev_float)
		return PRVM_GLOBALFIELDVALUE(d->ofs)->_float;
	return def;
}

static void read_balance(td_cfg_t *c)
{
	static const struct { const char *name, *dmg, *edge, *rad, *force, *speed; } rows[] =
	{
		{ "devastator", "g_balance_devastator_damage", "g_balance_devastator_edgedamage", "g_balance_devastator_radius", "g_balance_devastator_force", "g_balance_devastator_speedstart" },
		{ "blaster", "g_balance_blaster_primary_damage", "g_balance_blaster_primary_edgedamage", "g_balance_blaster_primary_radius", "g_balance_blaster_primary_force", "g_balance_blaster_primary_speed" },
		{ "electro", "g_balance_electro_primary_damage", "g_balance_electro_primary_edgedamage", "g_balance_electro_primary_radius", "g_balance_electro_primary_force", "g_balance_electro_primary_speed" },
		{ "hagar", "g_balance_hagar_primary_damage", "g_balance_hagar_primary_edgedamage", "g_balance_hagar_primary_radius", "g_balance_hagar_primary_force", "g_balance_hagar_primary_speed" },
	};
	int i, k;
	qbool f[6], any;
	float blockpct;
	// the armor share is sent to every client (the CSQC's armorblockpercent)
	blockpct = csqc_global_float("armorblockpercent", -1);
	if (blockpct >= 0 && blockpct <= 1)
		c->armor_block = blockpct;
	if (!sv.active)
		return; // a remote server: the stock numbers, scaled by what boosts really did
	for (i = 0; i < (int)(sizeof(rows) / sizeof(rows[0])); i++)
	{
		td_wep_t *r = TouchDash_Row(c, tw_index(rows[i].name));
		float v[5];
		if (!r)
			continue;
		v[0] = cvf(rows[i].dmg, r->core, &f[0]);
		v[1] = cvf(rows[i].edge, r->edge, &f[1]);
		v[2] = cvf(rows[i].rad, r->radius, &f[2]);
		v[3] = cvf(rows[i].force, r->force, &f[3]);
		v[4] = cvf(rows[i].speed, r->speed, &f[4]);
		any = f[0] && f[1] && f[2] && f[3];
		if (!any)
			continue;
		r->core = v[0];
		r->edge = v[1];
		r->radius = v[2];
		r->force = v[3];
		if (f[4] && v[4] > 0)
			r->speed = v[4];
		r->server = true;
	}
	c->selfpercent = cvf("g_balance_selfdamagepercent", c->selfpercent, NULL);
	c->forcescale = cvf("g_player_damageforcescale", c->forcescale, NULL);
	c->forcefactor = cvf("g_weaponforcefactor", c->forcefactor, NULL);
	c->speedfactor = cvf("g_balance_damagepush_speedfactor", c->speedfactor, NULL);
	c->dodge_horiz = cvf("sv_dodging_horiz_force_slowest", c->dodge_horiz, NULL);
	c->dodge_up = cvf("sv_dodging_up_speed", c->dodge_up, NULL);
	if (c->forcefactor <= 0 || c->forcescale <= 0)
		c->no_push = 1;
	(void)k;
}

// The client's view of the world and the player, into the planner's inputs.
static qbool gather(td_in_t *in, td_cfg_t *c, tr_world_t *w)
{
	vec3_t eye;
	float vh, fs, ss, fwd = 0, side = 0;
	int i, held;
	memset(in, 0, sizeof(*in));
	TouchDash_StockCfg(c);
	TouchHook_World(w);
	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, eye);
	vh = cl.stats[STAT_VIEWHEIGHT] ? cl.stats[STAT_VIEWHEIGHT] : 35;
	VectorCopy(eye, in->org);
	in->org[2] -= vh;
	VectorCopy(cl.mvelocity[0], in->vel);
	in->onground = cl.onground;
	in->view_yaw = cl.viewangles[YAW];
	in->view_pitch = cl.viewangles[PITCH];
	fs = cl_forwardspeed.value > 1 ? cl_forwardspeed.value : 400;
	ss = cl_sidespeed.value > 1 ? cl_sidespeed.value : 350;
	TouchHUD_MoveAxes(&fwd, &side);
	in->stick_fwd = bound(-1.0f, fwd / fs, 1.0f);
	in->stick_side = bound(-1.0f, side / ss, 1.0f);
	in->health = cl.stats[STAT_HEALTH];
	in->armor = max(0, cl.stats[STAT_ARMOR]);
	held = TouchHUD_HeldWeapon();
	in->held = held >= 0 ? tw_index(TouchHUD_WeaponName(held)) : -1;
	for (i = 0; i < TW_NUM_WEAPONS; i++)
		in->ammo[i] = -1;
	for (i = 0; i < TouchHUD_NumWeapons(); i++)
	{
		int j = tw_index(TouchHUD_WeaponName(i));
		if (j < 0)
			continue;
		if (TouchHUD_WeaponOwned(i))
			in->owned |= 1u << j;
		in->ammo[j] = TouchHUD_WeaponAmmo(i);
	}
	in->force_dir = bound(0, cl_touch_dash_dir.integer, 2);
	in->force_weapon = cl_touch_dash_weapon.string[0] ? tw_index(cl_touch_dash_weapon.string) : -1;

	c->eye_h = vh;
	c->gravity = w->gravity > 0 ? w->gravity : 800;
	if (cl.movevars_maxspeed > 0)
		c->maxspeed = cl.movevars_maxspeed;
	if (cl.movevars_jumpvelocity > 0)
		c->jump_speed = cl.movevars_jumpvelocity;
	c->center_h = cl_touch_dash_centerh.value;
	c->min_health = cl_touch_dash_minhealth.value;
	c->max_self = cl_touch_dash_maxself.value;
	c->switch_time = bound(0.05f, cl_touch_dash_switch.value, 2.0f);
	c->margin = cl_touch_dash_margin.value;
	c->range = bound(200.0f, cl_touch_dash_range.value, 2500.0f);
	read_balance(c);
	for (i = 0; i < c->nrows; i++)
	{
		int j = td_row_weapon(&c->rows[i]);
		if (j >= 0 && td.cal[j] > 0 && cl_touch_dash_learn.integer)
			c->rows[i].cal = td.cal[j];
	}
	if (td.learned_nopush)
		c->no_push = 1;
	c->dodging = dodging_state();
	return true;
}

// ---------------------------------------------------------------------------
// The boost
// ---------------------------------------------------------------------------
static float wrap180(float a)
{
	while (a > 180)
		a -= 360;
	while (a < -180)
		a += 360;
	return a;
}

static float aim_err(float yaw, float pitch)
{
	float ey = wrap180(yaw - cl.viewangles[YAW]), ep = pitch - cl.viewangles[PITCH];
	return sqrt(ey * ey + ep * ep);
}

static void turn_toward(float yaw, float pitch, float dt)
{
	float ey = wrap180(yaw - cl.viewangles[YAW]), ep = pitch - cl.viewangles[PITCH];
	float err = sqrt(ey * ey + ep * ep), rate = bound(200.0f, cl_touch_dash_turn.value, 6000.0f), step;
	if (err < 0.01f)
		return;
	step = min(err, rate * dt);
	cl.viewangles[YAW] += ey / err * step;
	cl.viewangles[PITCH] = bound(-89.0f, cl.viewangles[PITCH] + ep / err * step, 89.0f);
}

// A dodge. The mutator takes a dodge on a movement key going down while the
// dodge button is held (or a double tap), so a stick already held would never
// dodge: the move is let go of for a few frames, then pressed in the dodge's
// direction with the button (the way touch_dodge.c does it), for 0.24 s.
#define DODGE_BUTTON  8192   // button11: +dodge
#define BURST_RELEASE 0.12
#define BURST_END     0.36
static void send_dodge(const float dir[2])
{
	td.burst = true;
	td.burst_t0 = host.realtime;
	td.burst_dir[0] = dir[0];
	td.burst_dir[1] = dir[1];
}

static void burst_frame(double now)
{
	double dt = now - td.burst_t0;
	td.burst_buttons = 0;
	if (!td.burst)
		return;
	if (dt >= BURST_END || dt < 0)
	{
		td.burst = false;
		return;
	}
	if (dt < BURST_RELEASE)
	{
		cl.cmd.forwardmove = 0;
		cl.cmd.sidemove = 0;
		return;
	}
	{
		vec3_t ang, fwd, right;
		float df, ds;
		VectorSet(ang, 0, cl.viewangles[YAW], 0);
		AngleVectors(ang, fwd, right, NULL);
		df = td.burst_dir[0] * fwd[0] + td.burst_dir[1] * fwd[1];
		ds = td.burst_dir[0] * right[0] + td.burst_dir[1] * right[1];
		cl.cmd.forwardmove = df * cl_forwardspeed.value;
		cl.cmd.sidemove = ds * cl_sidespeed.value;
		td.burst_buttons = DODGE_BUTTON;
	}
}

static void weapon_cmd(int hud_index)
{
	char cmd[64];
	if (hud_index < 0)
		return;
	dpsnprintf(cmd, sizeof(cmd), "weapon_%s\n", TouchHUD_WeaponName(hud_index));
	Cbuf_AddText(cmd_local, cmd);
}

static int hud_index_of(int tw)
{
	int i;
	for (i = 0; i < TouchHUD_NumWeapons(); i++)
		if (!strcmp(TouchHUD_WeaponName(i), touch_weapons[tw].netname))
			return i;
	return -1;
}

static void end_boost(const char *why)
{
	if (td.st != ST_IDLE)
		debug(1, "boost ended: %s", why);
	td.st = ST_IDLE;
	td.attack = false;
}

static void begin_boost(const td_plan_t *p)
{
	double now = host.realtime;
	int active = TouchHUD_ActiveWeapon(), held = TouchHUD_HeldWeapon();
	td.plan = *p;
	td.weapon = p->weapon;
	td.t_start = td.t_state = now;
	td.saved[0] = cl.viewangles[YAW];
	td.saved[1] = cl.viewangles[PITCH];
	td.prev_hud = held >= 0 ? held : active;
	td.iter = 0;
	td.dodge_done = false;
	td.attack = false;
	td.boosts++;
	debug(1, "boost: %s %s%s, aim yaw %.0f pitch %.0f, predicted landing %.0f %.0f %.0f in %.2f s, costs %.0f health, %s",
		touch_weapons[p->weapon].netname, p->dir == TD_UP ? "up" : "forward", p->dodge ? " with a dodge" : "", p->yaw, p->pitch,
		p->land[0], p->land[1], p->land[2], p->land_time, p->self_damage, p->why);
	VectorCopy(p->land, td.pred_land);
	if (p->dodge)
	{
		send_dodge(p->ddir);
		td.dodge_done = true;
	}
	if ((active >= 0 ? active : held) != hud_index_of(td.weapon))
	{
		weapon_cmd(hud_index_of(td.weapon));
		td.st = ST_SWITCH;
	}
	else
		td.st = ST_AIM;
}

static const char *run_plan(td_in_t *in, td_cfg_t *c, tr_world_t *w, td_plan_t *p)
{
	(void)in;
	TouchDash_Plan(w, c, in, p);
	return p->why;
}

static void measure_fire(void)
{
	td_in_t in;
	td_cfg_t c;
	tr_world_t w;
	gather(&in, &c, &w);
	VectorCopy(in.vel, td.v0);
	VectorCopy(td.plan.push, td.pred_push);
	VectorCopy(in.org, td.fire_org);
	td.health0 = in.health;
	td.fired_weapon = td.weapon;
	td.t_measure = host.realtime;
	td.measuring = true;
	td.landed_pending = true;
	td.airborne_seen = false;
}

static void state_frame(const char *blocked, double now, float dt)
{
	int target;
	if (td.st == ST_IDLE)
		return;
	if (blocked && td.st != ST_RESTORE)
	{
		end_boost(blocked);
		td.aborted++;
		return;
	}
	if (cl.stats[STAT_HEALTH] <= 0)
	{
		end_boost("died");
		return;
	}
	target = hud_index_of(td.weapon);
	switch (td.st)
	{
	case ST_SWITCH:
		if (TouchHUD_ActiveWeapon() == target || (TouchHUD_ActiveWeapon() < 0 && TouchHUD_HeldWeapon() == target && now - td.t_state > 0.4))
		{
			td.st = ST_AIM;
			td.t_state = now;
		}
		else if (now - td.t_state > 1.2)
		{
			end_boost("the weapon switch never landed");
			td.aborted++;
			weapon_cmd(td.prev_hud);
		}
		break;
	case ST_AIM:
	{
		// a short settle once the weapon is up, so the shot is not eaten by its raise
		float err;
		if (now - td.t_state < 0.08)
			break;
		if (now - td.t_start > 2.0)
		{
			end_boost("aiming took too long");
			td.aborted++;
			break;
		}
		if (td.iter == 0 || aim_err(td.aim[0], td.aim[1]) < 2.5f)
		{
			td_in_t in;
			td_cfg_t c;
			tr_world_t w;
			td_plan_t p;
			gather(&in, &c, &w);
			in.held = td.weapon;
			in.force_weapon = td.weapon;
			in.force_dir = td.plan.dir;
			c.dodging = 0; // a dodge, if there was one, is already in the velocity
			run_plan(&in, &c, &w, &p);
			if (p.action != TD_BOOST)
			{
				end_boost("the boost no longer works from here");
				td.aborted++;
				weapon_cmd(td.prev_hud);
				td.st = ST_IDLE;
				break;
			}
			td.plan = p;
			VectorCopy(p.land, td.pred_land);
			if (td.iter > 0 && aim_err(p.yaw, p.pitch) < 3.0f)
			{
				td.aim[0] = p.yaw;
				td.aim[1] = p.pitch;
				goto fire;
			}
			td.aim[0] = p.yaw;
			td.aim[1] = p.pitch;
			td.iter++;
			if (td.iter > 3)
				goto fire;
		}
		err = aim_err(td.aim[0], td.aim[1]);
		turn_toward(td.aim[0], td.aim[1], dt);
		(void)err;
		break;
	fire:
		td.attack = true;
		td.t_fire = now;
		td.st = ST_FIRE;
		cl.viewangles[YAW] = td.aim[0];
		cl.viewangles[PITCH] = td.aim[1];
		measure_fire();
		debug(1, "fired %s at yaw %.0f pitch %.0f (predicted push %.0f %.0f %.0f)", touch_weapons[td.weapon].netname, td.aim[0], td.aim[1],
			td.plan.push[0], td.plan.push[1], td.plan.push[2]);
		break;
	}
	case ST_FIRE:
		// hold the aim and the button for a few frames: if the weapon was
		// still raising, the shot goes as soon as it can
		cl.viewangles[YAW] = td.aim[0];
		cl.viewangles[PITCH] = td.aim[1];
		if (now - td.t_fire >= 0.10)
		{
			td.attack = false;
			td.st = ST_RESTORE;
			td.t_state = now;
			if (td.prev_hud >= 0 && td.prev_hud != target)
				weapon_cmd(td.prev_hud);
		}
		break;
	case ST_RESTORE:
		turn_toward(td.saved[0], td.saved[1], dt);
		if (aim_err(td.saved[0], td.saved[1]) < 0.5f || now - td.t_state > 0.6)
			end_boost("done");
		break;
	}
}

// What a boost really did: the push it gave against the one predicted, and
// where the player came down.
static void measure_frame(double now)
{
	vec3_t eye, org;
	float vh;
	if (!td.measuring && !td.landed_pending)
		return;
	vh = cl.stats[STAT_VIEWHEIGHT] ? cl.stats[STAT_VIEWHEIGHT] : 35;
	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, eye);
	VectorCopy(eye, org);
	org[2] -= vh;
	if (td.measuring && now - td.t_measure >= 0.30)
	{
		td.measuring = false;
		if (!cl.onground)
		{
			float g = cl.movevars_gravity > 0 ? cl.movevars_gravity : 800, dt = (float)(now - td.t_measure), got[3], plen, along, ratio;
			vec3_t unit;
			got[0] = cl.mvelocity[0][0] - td.v0[0];
			got[1] = cl.mvelocity[0][1] - td.v0[1];
			got[2] = cl.mvelocity[0][2] + g * dt - td.v0[2];
			plen = sqrt(DotProduct(td.pred_push, td.pred_push));
			if (plen > 10 && !td.plan.dodge)
			{
				int wi = td.fired_weapon;
				VectorScale(td.pred_push, 1.0f / plen, unit);
				along = DotProduct(got, unit);
				ratio = along / plen;
				debug(1, "push measured %.0f %.0f %.0f against %.0f %.0f %.0f predicted (%.2f of it); health %.0f -> %.0f",
					got[0], got[1], got[2], td.pred_push[0], td.pred_push[1], td.pred_push[2], ratio, td.health0, (float)cl.stats[STAT_HEALTH]);
				if (ratio > 0.15f)
				{
					float cur = td.cal[wi] > 0 ? td.cal[wi] : 1;
					td.cal[wi] = bound(0.25f, cur * (0.5f + 0.5f * bound(0.2f, ratio, 2.5f)), 2.5f);
					td.nopush_streak = 0;
				}
				else if (++td.nopush_streak >= 3)
				{
					td.learned_nopush = true;
					debug(1, "three boosts without a push: this server seems to have weapon jumps off");
				}
			}
		}
	}
	if (td.landed_pending)
	{
		if (!cl.onground)
			td.airborne_seen = true;
		if ((td.airborne_seen && cl.onground && now - td.t_measure > 0.2) || now - td.t_measure > 5.0)
		{
			float dx = org[0] - td.pred_land[0], dy = org[1] - td.pred_land[1], dz = org[2] - td.pred_land[2];
			td.landed_pending = false;
			dpsnprintf(td.last, sizeof(td.last), "from %.0f %.0f %.0f landed %.0f %.0f %.0f predicted %.0f %.0f %.0f error %.0f (horizontal %.0f, vertical %.0f)",
				td.fire_org[0], td.fire_org[1], td.fire_org[2], org[0], org[1], org[2], td.pred_land[0], td.pred_land[1], td.pred_land[2],
				sqrt(dx * dx + dy * dy + dz * dz), sqrt(dx * dx + dy * dy), dz);
			debug(1, "landed: %s", td.last);
		}
	}
}

// A plain dash watched: did the dodge lift the player? A dodge leaves the
// ground at once at its up speed (200); walking, which the dash's own move
// also does, never goes up. (A server whose mutators the client cannot read
// is found out this way.)
static void dodge_watch(double now)
{
	if (td.dodge_t <= 0)
		return;
	if (cl.mvelocity[0][2] > td.dodge_maxvz)
		td.dodge_maxvz = cl.mvelocity[0][2];
	if (now - td.dodge_t < 0.4)
		return;
	{
		float vh = sqrt(cl.mvelocity[0][0] * cl.mvelocity[0][0] + cl.mvelocity[0][1] * cl.mvelocity[0][1]);
		qbool dodged = td.dodge_maxvz > 100;
		debug(1, "dash: horizontal speed %.0f -> %.0f, highest upward speed %.0f in 0.4 s (%s)", td.dodge_v0, vh, td.dodge_maxvz,
			td.dodge_ground ? (dodged ? "dodged" : "no dodge") : "from the air");
		if (td.dodge_ground)
		{
			if (dodged)
			{
				td.dodge_ok++;
				td.dodge_fail = 0;
			}
			else if (++td.dodge_fail >= 2 && !td.dodge_ok)
				debug(1, "two dashes did nothing: this server seems to have dodging off");
		}
	}
	td.dodge_t = 0;
}

// ---------------------------------------------------------------------------
// A tap
// ---------------------------------------------------------------------------
static void tap_with(int force_dir, int force_weapon)
{
	td_in_t in;
	td_cfg_t c;
	tr_world_t w;
	td_plan_t p;
	double t0;
	char goalbuf[96];
	const char *blocked = TouchAim_BlockReason();
	if (td.st != ST_IDLE)
		return;
	if (!cl_touch_dash_boost.integer || blocked)
	{
		{
			float fwd2[2];
			fwd2[0] = cos(cl.viewangles[YAW] * M_PI / 180);
			fwd2[1] = sin(cl.viewangles[YAW] * M_PI / 180);
			send_dodge(fwd2);
		}
		return;
	}
	t0 = Sys_DirtyTime();
	gather(&in, &c, &w);
	goalbuf[0] = 0;
	if (force_dir >= 0)
		in.force_dir = force_dir;
	if (force_weapon >= 0)
		in.force_weapon = force_weapon;
	run_plan(&in, &c, &w, &p);
	if (p.have_goal)
		dpsnprintf(goalbuf, sizeof(goalbuf), " at %.0f %.0f %.0f", p.goal[0], p.goal[1], p.goal[2]);
	debug(1, "tap: %s (%s) in %.1f ms, %d candidates, goal %s%s", p.action == TD_BOOST ? "boost" : p.action == TD_DODGE ? "dodge" : "nothing", p.why,
		(Sys_DirtyTime() - t0) * 1000, p.candidates, p.have_goal ? (p.goal_kind == TD_UP ? "ledge" : "gap") : "none",
		goalbuf);
	if (p.action == TD_DODGE)
	{
		td.dodge_t = host.realtime;
		td.dodge_maxvz = 0;
		td.dodge_v0 = sqrt(in.vel[0] * in.vel[0] + in.vel[1] * in.vel[1]);
		td.dodge_ground = in.onground;
		td.dashes++;
		send_dodge(p.ddir);
	}
	else if (p.action == TD_BOOST)
		begin_boost(&p);
}

void TouchDash_Frame(void)
{
	double now = host.realtime;
	float dt = bound(0.001f, (float)cl.realframetime, 0.1f);
	const char *blocked = TouchAim_BlockReason();
	state_frame(blocked, now, dt);
	burst_frame(now);
	measure_frame(now);
	dodge_watch(now);
	// the indicator: what a tap would do, a few times a second while DASH is on screen
	if (td.button_seen && cl_touch_dash_boost.integer && !blocked && td.st == ST_IDLE && now >= td.pv_next)
	{
		td_in_t in;
		td_cfg_t c;
		tr_world_t w;
		gather(&in, &c, &w);
		TouchDash_Plan(&w, &c, &in, &td.pv);
		td.pv_valid = true;
		td.pv_next = now + 0.3;
		if (td.pv.action == TD_BOOST && td.pv.weapon >= 0)
		{
			static const char *const names[][2] = { { "devastator", "Devastator" }, { "blaster", "Blaster" }, { "electro", "Electro" }, { "hagar", "Hagar" } };
			const char *a = "";
			int i;
			for (i = 0; i < 4; i++)
				if (!strcmp(names[i][0], touch_weapons[td.pv.weapon].netname))
					a = names[i][1];
			dpsnprintf(td.label, sizeof(td.label), "%s %s", a, td.pv.dir == TD_UP ? "up" : "forward");
		}
		else if (td.pv.action == TD_DODGE)
			dp_strlcpy(td.label, "Dodge", sizeof(td.label));
		else
			dp_strlcpy(td.label, "No boost", sizeof(td.label));
	}
	else if (!cl_touch_dash_boost.integer || blocked)
	{
		td.label[0] = 0;
		td.pv_valid = false;
	}
	td.button_seen = false;
}

int TouchDash_Buttons(void)
{
	return (td.attack ? 1 : 0) | td.burst_buttons;
}

qbool TouchDash_Busy(void)
{
	return td.st != ST_IDLE || td.burst;
}

// ---------------------------------------------------------------------------
// The HUD's DASH button
// ---------------------------------------------------------------------------
void TouchDash_NoteButton(float cx, float cy, float r)
{
	td.bx = cx;
	td.by = cy;
	td.br = r;
	td.button_seen = true;
}

const char *TouchDash_Label(void)
{
	return cl_touch_dash_boost.integer ? td.label : "";
}

qbool TouchDash_Shown(void)
{
	return cl_touch_dash_boost.integer != 0;
}

void TouchDash_Press(double now)
{
	td.pressing = true;
	td.picker = false;
	td.press_time = now;
	if (cl_touch_dash_longpress.value <= 0 || !cl_touch_dash_boost.integer)
	{
		td.pressing = false;
		tap_with(-1, -1);
	}
}

static const struct { const char *label; int dir; } dir_chips[] = { { "Auto", 0 }, { "Up", 1 }, { "Forward", 2 } };
static const char *const wep_names[] = { "", "devastator", "blaster", "electro", "hagar" };
static const char *const wep_shown[] = { "Auto", "Devastator", "Blaster", "Electro", "Hagar" };

// A chip as wide as its text and a margin each side, so no label touches an edge.
static float chip_hx(const char *label, float hy)
{
	return hy * (0.8f + 0.27f * (float)strlen(label));
}

static void chip_set(touch_dash_chip_t *c, const char *label, float x_left, float y, float hy, qbool current)
{
	memset(c, 0, sizeof(*c));
	dp_strlcpy(c->label, label, sizeof(c->label));
	c->hx = chip_hx(label, hy);
	c->hy = hy;
	c->x = x_left + c->hx;
	c->y = y;
	c->current = current;
	c->hot = fabs(td.fx - c->x) <= c->hx && fabs(td.fy - c->y) <= c->hy;
}

#define DIR_ROW_Y 2.2f
#define WEP_ROW_Y 4.2f
#define ROW_LABEL_H 0.34f

// Two rows above DASH: the direction, and the weapon. Their left edge stays right of the
// chat log's icon row (the stock chat area docks four small icons at the screen's left), and
// the rows are a full chip apart.
int TouchDash_Chips(touch_dash_chip_t *out, int max)
{
	int n = 0, i;
	float hy = td.br * 0.5f, gap = td.br * 0.3f, left0, left, y;
	if (!td.picker || max < 8)
		return 0;
	left0 = max(td.bx - td.br, 0.14f * max(1.0f, vid_conwidth.value));
	left = left0;
	y = td.by - td.br * DIR_ROW_Y;
	for (i = 0; i < 3 && n < max; i++, n++)
	{
		chip_set(&out[n], dir_chips[i].label, left, y, hy, cl_touch_dash_dir.integer == dir_chips[i].dir);
		left += 2 * out[n].hx + gap;
	}
	y = td.by - td.br * WEP_ROW_Y;
	left = left0;
	for (i = 0; i < 5 && n < max; i++)
	{
		if (i > 0)
		{
			int j = tw_index(wep_names[i]), h = j >= 0 ? hud_index_of(j) : -1;
			if (h < 0 || !TouchHUD_WeaponOwned(h))
				continue;
		}
		chip_set(&out[n], wep_shown[i], left, y, hy, !strcmp(cl_touch_dash_weapon.string, wep_names[i]));
		left += 2 * out[n].hx + gap;
		n++;
	}
	return n;
}

int TouchDash_RowLabels(touch_dash_rowlabel_t *out, int max)
{
	float left = max(td.bx - td.br, 0.14f * max(1.0f, vid_conwidth.value)), above = td.br * (0.5f + 0.3f);
	if (!td.picker || max < 2)
		return 0;
	dp_strlcpy(out[0].text, "Direction", sizeof(out[0].text));
	out[0].x = left;
	out[0].y = td.by - td.br * DIR_ROW_Y - above;
	out[0].size = td.br * ROW_LABEL_H;
	dp_strlcpy(out[1].text, "Weapon", sizeof(out[1].text));
	out[1].x = left;
	out[1].y = td.by - td.br * WEP_ROW_Y - above;
	out[1].size = td.br * ROW_LABEL_H;
	return 2;
}

qbool TouchDash_PickerOpen(void)
{
	return td.picker;
}

void TouchDash_Hold(float px, float py, double held)
{
	td.fx = px;
	td.fy = py;
	if (td.pressing && !td.picker && cl_touch_dash_longpress.value > 0 && held >= cl_touch_dash_longpress.value)
	{
		touch_dash_chip_t chips[10];
		int n, i;
		td.picker = true;
		n = TouchDash_Chips(chips, 10);
		for (i = 0; i < n; i++)
			debug(1, "picker chip %s at %.3f %.3f (%s)", chips[i].label, chips[i].x / max(1.0f, vid_conwidth.value), chips[i].y / max(1.0f, vid_conheight.value),
				chips[i].current ? "current" : "-");
	}
}

void TouchDash_Release(float px, float py)
{
	touch_dash_chip_t chips[10];
	int n, i;
	if (!td.pressing)
		return;
	td.pressing = false;
	if (!td.picker)
	{
		tap_with(-1, -1);
		return;
	}
	td.fx = px;
	td.fy = py;
	n = TouchDash_Chips(chips, 10);
	td.picker = false;
	for (i = 0; i < n; i++)
	{
		if (!chips[i].hot)
			continue;
		if (i < 3)
			Cvar_SetValueQuick(&cl_touch_dash_dir, dir_chips[i].dir);
		else
		{
			int k;
			for (k = 0; k < 5; k++)
				if (!strcmp(wep_shown[k], chips[i].label))
					Cvar_SetQuick(&cl_touch_dash_weapon, wep_names[k]);
		}
		td.pv_next = 0;
		debug(1, "picked %s (dir %d, weapon \"%s\")", chips[i].label, cl_touch_dash_dir.integer, cl_touch_dash_weapon.string);
		return;
	}
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
static void TouchDash_Cmd_f(cmd_state_t *cmd)
{
	int dir = -1, wep = -1;
	if (Cmd_Argc(cmd) > 1)
	{
		const char *a = Cmd_Argv(cmd, 1);
		dir = !strcmp(a, "up") ? TD_UP : !strcmp(a, "fwd") || !strcmp(a, "forward") ? TD_FWD : !strcmp(a, "auto") ? TD_AUTO : -1;
	}
	if (Cmd_Argc(cmd) > 2)
		wep = tw_index(Cmd_Argv(cmd, 2));
	tap_with(dir, wep);
}

static void TouchDash_Status_f(cmd_state_t *cmd)
{
	td_in_t in;
	td_cfg_t c;
	tr_world_t w;
	td_plan_t p;
	int i;
	char tapbuf[160];
	gather(&in, &c, &w);
	TouchDash_Plan(&w, &c, &in, &p);
	tapbuf[0] = 0;
	if (p.action == TD_BOOST)
		dpsnprintf(tapbuf, sizeof(tapbuf), ", %s %s, aim yaw %.0f pitch %.0f, lands %.0f %.0f %.0f", touch_weapons[p.weapon].netname, p.dir == TD_UP ? "up" : "forward", p.yaw, p.pitch, p.land[0], p.land[1], p.land[2]);
	Con_Printf("touch dash: boost %d, dir %d, weapon \"%s\"; dodging %d (%s); health %.0f armor %.0f; self damage %.2f, push scale %.2f, speedfactor %.2f; no_push %d\n",
		cl_touch_dash_boost.integer, in.force_dir, cl_touch_dash_weapon.string, c.dodging,
		dodging_source(), in.health, in.armor,
		c.selfpercent, c.forcescale, c.speedfactor, c.no_push);
	for (i = 0; i < c.nrows; i++)
	{
		int j = td_row_weapon(&c.rows[i]);
		Con_Printf("touch dash:   %-10s core %.0f edge %.0f radius %.0f force %.0f speed %.0f (%s) calibration %.2f; %s%s\n", c.rows[i].netname, c.rows[i].core, c.rows[i].edge,
			c.rows[i].radius, c.rows[i].force, c.rows[i].speed, c.rows[i].server ? "the server's" : "stock", c.rows[i].cal,
			j >= 0 && (in.owned & (1u << j)) ? "owned" : "not owned", j >= 0 && in.held == j ? ", in hand" : "");
	}
	Con_Printf("touch dash: a tap now: %s (%s)%s%s; %d boosts, %d aborted, %d dashes\n", p.action == TD_BOOST ? "boost" : p.action == TD_DODGE ? "dodge" : "nothing", p.why,
		tapbuf,
		p.have_goal ? (p.goal_kind == TD_UP ? ", a ledge ahead" : ", a gap ahead") : "", td.boosts, td.aborted, td.dashes);
	if (td.last[0])
		Con_Printf("touch dash: last boost: %s\n", td.last);
}

void TouchDash_ProgsChanged(void)
{
	int i;
	for (i = 0; i < TW_NUM_WEAPONS; i++)
		td.cal[i] = 0;
	td.learned_nopush = false;
	td.nopush_streak = 0;
	td.dodge_ok = td.dodge_fail = 0;
	td.st = ST_IDLE;
	td.attack = false;
	td.measuring = td.landed_pending = false;
	td.pv_valid = false;
	td.label[0] = 0;
}

void TouchDash_Init(void)
{
	Cvar_RegisterVariable(&cl_touch_dash_boost);
	Cvar_RegisterVariable(&cl_touch_dash_dir);
	Cvar_RegisterVariable(&cl_touch_dash_weapon);
	Cvar_RegisterVariable(&cl_touch_dash_minhealth);
	Cvar_RegisterVariable(&cl_touch_dash_maxself);
	Cvar_RegisterVariable(&cl_touch_dash_switch);
	Cvar_RegisterVariable(&cl_touch_dash_margin);
	Cvar_RegisterVariable(&cl_touch_dash_range);
	Cvar_RegisterVariable(&cl_touch_dash_longpress);
	Cvar_RegisterVariable(&cl_touch_dash_turn);
	Cvar_RegisterVariable(&cl_touch_dash_learn);
	Cvar_RegisterVariable(&cl_touch_dash_centerh);
	Cvar_RegisterVariable(&cl_touch_dash_debug);
	Cmd_AddCommand(CF_CLIENT, "touch_dash", TouchDash_Cmd_f, "the smart DASH: a dodge or a weapon boost; touch_dash up|forward|auto [weapon] forces the direction (and weapon) for this tap");
	Cmd_AddCommand(CF_CLIENT, "touch_dash_status", TouchDash_Status_f, "print what DASH knows about the server and the player, what a tap would do now, and how the last boost went");
}

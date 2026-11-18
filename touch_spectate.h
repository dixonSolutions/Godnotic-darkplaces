/*
Godnotic: what the touch controls do while spectating, as a pure table
both HUDs follow (the engine's touch_hud.c on servers, the port's CSQC in a
local match mirrors it in touch_spectate.qc), checked by
scripts/test-touch-spectate.sh.

The server's spectator code (ObserverOrSpectatorThink, server/client.qc in
0.8.6 and later) reads, for a client that is not playing:
  observing (free-fly)   +attack            start following a player
                         +jump              join, when joining is allowed
                         +moveup/+movedown  fly up / down (cl_upspeed)
  following a player     +attack, weapnext  next player
                         weapprev           previous player
                         +attack2           back to free-fly
                         +jump              join
  out of the round (Clan Arena and the like, frags -616, -669 since 0.8.6):
  following or free-flying as above, but the join waits for the next round.
Jump is the join request in every state, and joining is Xonotic's join
dialog (touch_hudmode.h TouchJoinDlg_*), never a button or a gesture on the
HUD: so no spectator control sends +jump, and the flying controls use
+moveup / +movedown, never +jump / +crouch.

A spectator's controls are stripped to what a spectator uses: the move stick
and the fly rocker (UP / DOWN) while free-flying, and one compact bar,
previous | follow or free | next. No weapons, fire, jump, crouch, hook,
nade or turn.
*/
#ifndef TOUCH_SPECTATE_H
#define TOUCH_SPECTATE_H

// Spectator states (TouchUI_SpectatorState).
#define TOUCHSPEC_NONE      0
#define TOUCHSPEC_OBSERVING 1
#define TOUCHSPEC_FOLLOWING 2
#define TOUCHSPEC_OUT       3   // out of the round, following a player
#define TOUCHSPEC_OUT_FREE  4   // out of the round, free-flying

// The state from what the client knows of itself: its scoreboard frags (the
// server marks a spectator -666 and a player out of the round -616, -669
// since 0.8.6), whose eyes the view is in, and the health stat.
//
// It fails toward playing. With the view our own, a spectator is free-flying
// and its health is the spectator marker too (negative) on every server
// checked (0.8.2 to git, instagib and other mods); health above zero there
// means we are alive in the game, whatever the scoreboard says. A player
// shown spectator controls cannot play; a spectator shown play controls still
// has MENU.
static inline int TouchSpec_StateFrom(int frags, int me, int viewentity, int maxclients, int health)
{
	int out;
	if (frags == -669)
		frags = -616;
	if (frags != -666 && frags != -616)
		return TOUCHSPEC_NONE;
	out = frags == -616;
	if (viewentity != me && viewentity >= 1 && viewentity <= maxclients)
		return out ? TOUCHSPEC_OUT : TOUCHSPEC_FOLLOWING;
	if (health > 0)
		return TOUCHSPEC_NONE;
	return out ? TOUCHSPEC_OUT_FREE : TOUCHSPEC_OBSERVING;
}

// The spectator bar's parts, left to right.
#define TOUCHSPEC_BAR_PREV  0
#define TOUCHSPEC_BAR_MAIN  1
#define TOUCHSPEC_BAR_NEXT  2
#define TOUCHSPEC_BAR_JOIN  3   // JOIN, beside the bar, when joining is allowed
#define TOUCHSPEC_BAR_PARTS 4

// What a part does.
#define TOUCHSPEC_ACT_NONE    0
#define TOUCHSPEC_ACT_FOLLOW  1   // +attack while held: follow a player
#define TOUCHSPEC_ACT_NEXT    2   // +attack while held: next player
#define TOUCHSPEC_ACT_PREV    3   // weapprev on the press: previous player
#define TOUCHSPEC_ACT_FREE    4   // +attack2 while held: back to free-fly
#define TOUCHSPEC_ACT_UP      5   // +moveup while held (fly rocker)
#define TOUCHSPEC_ACT_DOWN    6   // +movedown while held (fly rocker)
#define TOUCHSPEC_ACT_JOIN    7   // on the press: open the join dialog, or auto-join

static inline int TouchSpec_Flying(int state)
{
	return state == TOUCHSPEC_OBSERVING || state == TOUCHSPEC_OUT_FREE;
}

static inline int TouchSpec_BarAction(int state, int part)
{
	if (part == TOUCHSPEC_BAR_JOIN)
		return state != TOUCHSPEC_NONE ? TOUCHSPEC_ACT_JOIN : TOUCHSPEC_ACT_NONE;
	if (TouchSpec_Flying(state))
		// Free-flying, either arrow or the middle starts following; next /
		// previous weapon would only change the flying speed.
		return part >= 0 && part < TOUCHSPEC_BAR_PARTS ? TOUCHSPEC_ACT_FOLLOW : TOUCHSPEC_ACT_NONE;
	switch (state)
	{
	case TOUCHSPEC_FOLLOWING:
		return part == TOUCHSPEC_BAR_PREV ? TOUCHSPEC_ACT_PREV
			: part == TOUCHSPEC_BAR_MAIN ? TOUCHSPEC_ACT_FREE
			: part == TOUCHSPEC_BAR_NEXT ? TOUCHSPEC_ACT_NEXT
			: TOUCHSPEC_ACT_NONE;
	case TOUCHSPEC_OUT:
		// Out of the round the server keeps the view on a player.
		return part == TOUCHSPEC_BAR_PREV ? TOUCHSPEC_ACT_PREV
			: part == TOUCHSPEC_BAR_NEXT ? TOUCHSPEC_ACT_NEXT
			: TOUCHSPEC_ACT_NONE;
	default:
		return TOUCHSPEC_ACT_NONE;
	}
}

static inline const char *TouchSpec_ActionLabel(int action)
{
	switch (action)
	{
	case TOUCHSPEC_ACT_FOLLOW: return "FOLLOW";
	case TOUCHSPEC_ACT_NEXT: return "NEXT";
	case TOUCHSPEC_ACT_PREV: return "PREV";
	case TOUCHSPEC_ACT_FREE: return "FREE";
	case TOUCHSPEC_ACT_UP: return "UP";
	case TOUCHSPEC_ACT_DOWN: return "DOWN";
	case TOUCHSPEC_ACT_JOIN: return "JOIN";
	default: return "";
	}
}

// What the bar part shows: arrows for the sides, a word in the middle.
static inline const char *TouchSpec_BarLabel(int state, int part)
{
	int action = TouchSpec_BarAction(state, part);
	if (part == TOUCHSPEC_BAR_JOIN)
		return action == TOUCHSPEC_ACT_JOIN ? "JOIN" : "";
	if (action == TOUCHSPEC_ACT_NONE)
		return part == TOUCHSPEC_BAR_MAIN && state != TOUCHSPEC_NONE ? "WATCHING" : "";
	if (part == TOUCHSPEC_BAR_PREV)
		return "<";
	if (part == TOUCHSPEC_BAR_NEXT)
		return ">";
	return action == TOUCHSPEC_ACT_FOLLOW ? "FREE" : TouchSpec_ActionLabel(action);
}

// The move stick, the look drag and the fly rocker work only while free-
// flying; following someone, the view is theirs.
static inline int TouchSpec_StickShown(int state)
{
	return TouchSpec_Flying(state);
}

// The spectator bar is always available, including for existing installs.
#define TOUCHSPEC_BAR_DEFAULT 1

static inline int TouchSpec_BarShown(int state, int visible)
{
	(void)visible;
	return state != TOUCHSPEC_NONE;
}

// The roles a finger holds on the spectator bar.
#define TOUCHSPEC_ROLE_ATTACK 300   // FOLLOW / NEXT: +attack while held
#define TOUCHSPEC_ROLE_PREV   301   // weapprev on the press
#define TOUCHSPEC_ROLE_FREE   302   // +attack2 while held: back to free-fly
#define TOUCHSPEC_ROLE_UP     303   // +moveup while held
#define TOUCHSPEC_ROLE_DOWN   304   // +movedown while held
#define TOUCHSPEC_ROLE_JOIN   305   // the JOIN button, on the press

static inline int TouchSpec_BarRole(int role)
{
	return role == TOUCHSPEC_ROLE_ATTACK || role == TOUCHSPEC_ROLE_PREV || role == TOUCHSPEC_ROLE_FREE
		|| role == TOUCHSPEC_ROLE_JOIN;
}

// Whether a finger with this role acts this frame.
static inline int TouchSpec_FingerActs(int state, int visible, int role)
{
	return !TouchSpec_BarRole(role) || TouchSpec_BarShown(state, visible);
}

// What one finger's role contributes to the spectator frame.
typedef struct touchspec_keys_s
{
	int fire, fire2, prev, up, down, join;
} touchspec_keys_t;

// Fold one held finger into the frame's outputs. Both the engine HUD and the
// tests go through this, including when an old setting was saved as off.
static inline void TouchSpec_AddFinger(touchspec_keys_t *k, int state, int visible, int role, int first)
{
	if (state == TOUCHSPEC_NONE || !TouchSpec_FingerActs(state, visible, role))
		return;
	switch (role)
	{
	case TOUCHSPEC_ROLE_ATTACK: k->fire = 1; break;
	case TOUCHSPEC_ROLE_FREE: k->fire2 = 1; break;
	case TOUCHSPEC_ROLE_PREV: if (first) k->prev = 1; break;
	case TOUCHSPEC_ROLE_UP: k->up = 1; break;
	case TOUCHSPEC_ROLE_DOWN: k->down = 1; break;
	case TOUCHSPEC_ROLE_JOIN: if (first) k->join = 1; break;
	default: break;
	}
}

// ---------------------------------------------------------------------------
// After a join from the join dialog ("cmd join"): a join the server does not
// take (joining locked, teams full, spectate-only) leaves the player
// spectating; the HUD then says so once instead of failing silently, and
// _touch_hud_join reads "locked" for a while.
// ---------------------------------------------------------------------------
#define TOUCHSPEC_JOIN_PRESS  0.25  // (the old +jump join: how long it was held)
#define TOUCHSPEC_JOIN_WAIT   3.0   // a join not taken by then was refused
#define TOUCHSPEC_JOIN_LOCKED 15.0  // how long _touch_hud_join then reads locked

typedef struct touchspec_join_s
{
	double sent_time;     // the join was sent, 0 = not
	double locked_until;  // shown as locked until
} touchspec_join_t;

// Join is offered observing and following (the stock HUD's "press jump to
// join"); out of the round the player is already in the game.
static inline int TouchSpec_JoinShown(int state)
{
	return state == TOUCHSPEC_OBSERVING || state == TOUCHSPEC_FOLLOWING;
}

static inline void TouchSpec_JoinSend(touchspec_join_t *j, double now)
{
	j->sent_time = now;
	j->locked_until = 0;
}

static inline int TouchSpec_JoinPending(const touchspec_join_t *j)
{
	return j->sent_time != 0;
}

static inline int TouchSpec_JoinLocked(const touchspec_join_t *j, double now)
{
	return now < j->locked_until;
}

// One frame. Returns whether +jump is to be held this frame.
static inline int TouchSpec_JoinFrame(touchspec_join_t *j, int state, double now)
{
	if (state == TOUCHSPEC_NONE)
		j->locked_until = 0;
	if (!j->sent_time)
		return 0;
	if (state == TOUCHSPEC_NONE)
	{
		// Taken: playing. A lock from a slow answer goes too, or the next
		// time this player spectates the handle would say LOCKED.
		j->sent_time = 0;
		j->locked_until = 0;
		return 0;
	}
	if (now - j->sent_time >= TOUCHSPEC_JOIN_WAIT)
	{
		// Still a spectator well after the join went out: the server said no.
		j->sent_time = 0;
		j->locked_until = now + TOUCHSPEC_JOIN_LOCKED;
		return 0;
	}
	return now - j->sent_time < TOUCHSPEC_JOIN_PRESS;
}


// The bar's geometry, pure and shared: all four parts (previous, main, next,
// JOIN) in one row. The row is laid out around the wanted centre but is moved
// and shrunk uniformly until it fits the screen's safe width and height, so
// JOIN can never be pushed off the right edge. Widths are in units of the
// half-height: 2 + 4.4 + 2 + 2.3 and three 0.25 gaps = 11.45.
#define TOUCHSPEC_BAR_GAP     0.25f
#define TOUCHSPEC_BAR_TOTAL  11.45f
#define TOUCHSPEC_BAR_TOTAL_NOJOIN 9.15f
#define TOUCHSPEC_BAR_MARGIN_DP 8.0f
typedef struct touchspec_bar_rect_s { float cx, cy, hx, hy; } touchspec_bar_rect_t;

static inline void TouchSpec_BarLayout(float w, float h, float want_x, float want_y, float hy, float u, int join, touchspec_bar_rect_t out[TOUCHSPEC_BAR_PARTS])
{
	float margin = TOUCHSPEC_BAR_MARGIN_DP * u, total, scale = 1.0f, left, cy, x, gap;
	int part;
	for (part = 0; part < TOUCHSPEC_BAR_PARTS; part++)
		out[part].cx = out[part].cy = out[part].hx = out[part].hy = 0;
	if (w <= 0 || h <= 0 || hy <= 0)
		return;
	if (margin * 2 >= w) margin = 0;
	if (margin * 2 >= h) margin = 0;
	total = join ? TOUCHSPEC_BAR_TOTAL : TOUCHSPEC_BAR_TOTAL_NOJOIN;
	if (2 * hy > h - 2 * margin)
		scale = (h - 2 * margin) / (2 * hy);
	if (total * hy * scale > w - 2 * margin)
		scale = (w - 2 * margin) / (total * hy);
	hy *= scale;
	if (hy <= 0)
		return;
	total *= hy;
	left = want_x - total * 0.5f;
	if (left < margin) left = margin;
	if (left + total > w - margin) left = w - margin - total;
	if (left < 0) left = 0;
	cy = want_y;
	if (cy - hy < margin) cy = margin + hy;
	if (cy + hy > h - margin) cy = h - margin - hy;
	gap = TOUCHSPEC_BAR_GAP * hy;
	x = left;
	out[TOUCHSPEC_BAR_PREV].cx = x + hy; out[TOUCHSPEC_BAR_PREV].hx = hy; x += 2 * hy + gap;
	out[TOUCHSPEC_BAR_MAIN].cx = x + 2.2f * hy; out[TOUCHSPEC_BAR_MAIN].hx = 2.2f * hy; x += 4.4f * hy + gap;
	out[TOUCHSPEC_BAR_NEXT].cx = x + hy; out[TOUCHSPEC_BAR_NEXT].hx = hy; x += 2 * hy + gap;
	if (join) { out[TOUCHSPEC_BAR_JOIN].cx = x + 1.15f * hy; out[TOUCHSPEC_BAR_JOIN].hx = 1.15f * hy; }
	for (part = 0; part < TOUCHSPEC_BAR_PARTS; part++)
	{
		out[part].cy = cy;
		out[part].hy = hy;
	}
}

static inline float TouchSpec_Abs(float v)
{
	return v < 0 ? -v : v;
}

// Which part of a laid-out bar is under a point, or -1. The HUD draws and
// hit-tests with the same rectangles and the same forgiveness (12% of the
// half-height sideways, 15% up and down), including the desk strip whose
// parts take a finger when the touch controls are off.
static inline int TouchSpec_BarPartAt(const touchspec_bar_rect_t *r, int join, float px, float py)
{
	int part;
	for (part = 0; part < TOUCHSPEC_BAR_PARTS; part++)
	{
		if (part == TOUCHSPEC_BAR_JOIN && !join)
			continue;
		if (r[part].hy <= 0)
			continue;
		if (TouchSpec_Abs(px - r[part].cx) <= r[part].hx + r[part].hy * 0.12f
			&& TouchSpec_Abs(py - r[part].cy) <= r[part].hy * 1.15f)
			return part;
	}
	return -1;
}

#endif
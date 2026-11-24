/*
Godnotic: target-velocity filtering, shot timing and aim break-out for the
touch aim helpers (touch_aim.c). Pure maths on plain floats, no engine, so
tests/touch-aim/predict_test.c checks it directly (the caller has qbool and
touch_weapons.h, as with touch_aim_targets.h).

Four small pieces, all cheap enough to run per player per frame. What each is
for was measured against a simulated person who strafes, jumps and reverses at
random (tests/touch-aim/predict_test.c prints the numbers); a lead shrunk
toward "where a runner that may reverse is on average" lost far more direct
hits than it gained in blast hits, so the lead stays the constant-velocity
intercept and the work goes into the estimate and the moment of the shot.

1. Velocity filter. The CSQC's velocity (derived from network updates) is
   smoothed with a time constant that follows how jittery it is: 30 ms when
   the samples agree (little lag at a reversal), up to 110 ms when they
   scatter. Each sample may move the estimate by at most TP_INNOV_MAX, so one
   garbage sample (a teleport, a lost packet's catch-up) cannot throw the lead.

2. Confidence. The jitter of the samples (their second difference, which a
   smooth change of speed does not raise) says how far the estimate can be
   trusted; low confidence widens the fire window a little.

3. Settle. A person who has just changed direction is moving at neither the
   old nor the new velocity yet, and a lead taken then misses far more often
   (a fast and a slow estimate that disagree say so). Fire at a long-flight
   projectile waits for them to agree, for at most TP_SETTLE_MAX.

4. Break-out. Magnetism and the aim pull put the view back on the target every
   frame, so a drag that leaves it is undone before it shows. The player's own
   turn each frame (the pull is not in it) is compared with the direction to
   the target; the part that is not toward it is summed with a short decay.
   Enough of it, or one very fast flick, releases that target at once: no pull,
   no friction, no auto-fire at it, until a cooldown ends or the player turns
   back onto it.
*/
#ifndef TOUCH_AIM_PREDICT_H
#define TOUCH_AIM_PREDICT_H

#include <math.h>
#include "touch_weapons.h"

// ---------------------------------------------------------------------------
// 1 and 2. Velocity filter and confidence
// ---------------------------------------------------------------------------

#define TP_TAU_MIN     0.03f   // smoothing time constant of clean samples (seconds)
#define TP_TAU_MAX     0.11f   // and of the most scattered
#define TP_TAU_PER_U   0.0009f // seconds added per unit/s of jitter
#define TP_INNOV_MAX   250.0f  // largest step one sample may move the estimate (units/s)
#define TP_SLOW_TAU    0.30f   // the slow estimate's time constant
#define TP_NOISE_TAU   0.25f   // seconds over which the jitter is averaged
#define TP_NOISE_REF   60.0f   // units/s of jitter that costs half the confidence, plus
#define TP_NOISE_SPEED 0.35f   // this fraction of the target's own speed
#define TP_WINDOW_GAIN 0.8f    // widening of the fire window at no confidence

typedef struct touch_pred_s
{
	float vel[3];   // the estimate the lead uses
	float slow[3];  // the same, slowly
	float s1[3], s2[3]; // the last two samples
	int n;          // samples seen
	float noise;    // jitter, units/s
} touch_pred_t;

static void TouchPred_Reset(touch_pred_t *p, const float v[3])
{
	int i;
	for (i = 0; i < 3; i++)
		p->vel[i] = p->slow[i] = p->s1[i] = p->s2[i] = v ? v[i] : 0.0f;
	p->n = 0;
	p->noise = 0;
}

static float tp_len3(const float a[3])
{
	return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

// The smoothing time constant for this much jitter.
static float TouchPred_Tau(float noise)
{
	float t = TP_TAU_MIN + TP_TAU_PER_U * noise;
	return t > TP_TAU_MAX ? TP_TAU_MAX : t;
}

// Fold one velocity sample, dt seconds after the last, into the estimate.
static void TouchPred_Update(touch_pred_t *p, const float sample[3], float dt)
{
	float d[3], step, f, fs, tau;
	int i;
	if (dt <= 0)
		return;
	// jitter: the second difference of the samples (zero for a steady or
	// smoothly changing velocity), scaled to a standard deviation
	if (p->n >= 2)
	{
		float sd[3];
		for (i = 0; i < 3; i++)
			sd[i] = sample[i] - 2.0f * p->s1[i] + p->s2[i];
		f = dt / TP_NOISE_TAU;
		f = f > 1 ? 1 : f;
		step = 0.41f * tp_len3(sd); // 1 / sqrt(6)
		step = step > 2.0f * TP_INNOV_MAX ? 2.0f * TP_INNOV_MAX : step;
		p->noise += f * (step - p->noise);
	}
	for (i = 0; i < 3; i++)
	{
		p->s2[i] = p->s1[i];
		p->s1[i] = sample[i];
		d[i] = sample[i] - p->vel[i];
	}
	p->n++;
	// one sample moves the estimate only so far
	step = tp_len3(d);
	if (step > TP_INNOV_MAX)
		for (i = 0; i < 3; i++)
			d[i] *= TP_INNOV_MAX / step;
	tau = TouchPred_Tau(p->noise);
	f = dt / tau;
	f = f > 1 ? 1 : f;
	fs = dt / TP_SLOW_TAU;
	fs = fs > 1 ? 1 : fs;
	for (i = 0; i < 3; i++)
	{
		p->vel[i] += f * d[i];
		p->slow[i] += fs * (sample[i] - p->slow[i]);
	}
}

// 0..1: how far the estimate can be trusted, for the jitter and the speed.
static float TouchPred_Confidence(float noise, float speed)
{
	return 1.0f / (1.0f + noise / (TP_NOISE_REF + TP_NOISE_SPEED * speed));
}

// Multiplier for the fire window: 1 at full confidence, wider below.
static float TouchPred_WindowScale(float conf)
{
	return 1.0f + TP_WINDOW_GAIN * (1.0f - conf);
}

// ---------------------------------------------------------------------------
// 3. Settle
// ---------------------------------------------------------------------------

#define TP_SETTLE_DIS 90.0f  // units/s the fast and slow estimates may differ by and still count as settled
#define TP_SETTLE_MAX 0.25f  // longest a shot waits for that
#define TP_SETTLE_MIN_FLIGHT 0.15f // a shot that arrives sooner than this (flight plus latency) is not held

// How far the target is from a steady run: the fast and slow estimates'
// disagreement, units/s, sideways and along the floor only (a fall changes
// the vertical velocity all the time, which says nothing about the run).
static float TouchPred_Unsettled(const touch_pred_t *p)
{
	float dx = p->vel[0] - p->slow[0], dy = p->vel[1] - p->slow[1];
	return sqrtf(dx * dx + dy * dy);
}

// Whether a shot at this target is held this frame. since: when it began to
// be held (0 = not held now), updated here; now: the clock; horizon: flight
// time plus latency of the shot. A shot is held only while the target is
// unsettled, only for TP_SETTLE_MAX of one stretch, and only when the flight
// is long enough for a changed direction to matter.
static qbool TouchPred_Hold(double *since, double now, float unsettled, float horizon)
{
	if (horizon < TP_SETTLE_MIN_FLIGHT || unsettled <= TP_SETTLE_DIS)
	{
		*since = 0;
		return false;
	}
	if (*since <= 0)
		*since = now;
	return now - *since < TP_SETTLE_MAX;
}

// ---------------------------------------------------------------------------
// Lobs: where the arc of a grenade runs
// ---------------------------------------------------------------------------

// A point on the lob that reaches a spot dx away horizontally and dz above the
// muzzle, at fraction u of its flight: horizontal distance and height from
// the muzzle. False when the spot is out of reach. Same launch model as
// TouchWep_Ballistic (low arc).
static qbool TouchPred_ArcAt(const touch_wep_fire_t *f, float dx, float dz, float g, float u, float *px, float *pz)
{
	float elev, t, v, phi, s;
	if (!TouchWep_Ballistic(dx, dz, f->speed, f->speed_up, g, &elev, &t))
		return false;
	v = sqrtf(f->speed * f->speed + f->speed_up * f->speed_up);
	phi = elev + atan2f(f->speed_up, f->speed);
	s = u * t;
	*px = v * cosf(phi) * s;
	*pz = v * sinf(phi) * s - 0.5f * g * s * s;
	return true;
}

// ---------------------------------------------------------------------------
// 3. Break-out
// ---------------------------------------------------------------------------

typedef struct touch_brk_cfg_s
{
	float angle;     // degrees of drag away from the target that release it
	float speed;     // deg/s: slower turns are a resting thumb, not a drag
	float flick;     // deg/s: one frame this fast releases at once
	float cooldown;  // seconds before the released target may be taken again
	float ret;       // deg: turning back to within this of it takes it again
} touch_brk_cfg_t;

#define TB_OUT 8 // released targets held out at once: with one, two close enemies took turns

typedef struct touch_brk_s
{
	float acc_y, acc_p; // net drag leaving the target, with decay (degrees)
	int on;             // the target it was summed against
	struct { int ent; double until; } out[TB_OUT]; // the targets released, and until when
} touch_brk_t;

#define TB_DECAY  0.25f // seconds over which the summed drag fades
#define TB_FLICK_DT (1.0f / 60.0f) // a touch event lands in one frame, however short: a flick is judged over this much
#define TB_FOLLOW 1.5f  // a turn up to this many times the way to the target follows it

static float tb_len(float a, float b)
{
	return sqrtf(a * a + b * b);
}

// The part of the player's turn (ty, tp degrees this frame) that goes to the
// target (ey, ep degrees from the view to where it is now): a drag that follows
// a moving target is not leaving it, one that runs well past it is. The rest of
// the turn, which leaves the target, goes to ly, lp.
static float TouchBrk_Toward(float ty, float tp, float ey, float ep, float *ly, float *lp)
{
	float e = tb_len(ey, ep), along = 0;
	*ly = ty;
	*lp = tp;
	if (e <= 0)
		return 0;
	along = (ty * ey + tp * ep) / e;
	if (along <= 0)
		return 0;
	if (along > TB_FOLLOW * e)
		along = TB_FOLLOW * e;
	*ly = ty - along * ey / e;
	*lp = tp - along * ep / e;
	return along;
}

// The slot of a released target, or -1.
static int TouchBrk_Slot(const touch_brk_t *b, int ent)
{
	int i;
	for (i = 0; i < TB_OUT; i++)
		if (ent && b->out[i].ent == ent)
			return i;
	return -1;
}

// One frame. ty, tp: the player's own turn this frame; ey, ep: from the view
// (before that turn) to the target the pull was holding; ent: that target, 0
// when there was none. True when this frame releases it. Only a drag that
// keeps going one way does: a wobble sums to nothing. Firing never asks this.
static qbool TouchBrk_Step(touch_brk_t *b, const touch_brk_cfg_t *c, double now, float dt, float ty, float tp, int ent, float ey, float ep)
{
	float ly, lp, k, rate, flick;
	int i, s;
	if (dt <= 0)
		return false;
	if (!ent || ent != b->on)
		b->acc_y = b->acc_p = 0; // a drag that brought the view here is not leaving it
	b->on = ent;
	if (!ent)
		return false;
	TouchBrk_Toward(ty, tp, ey, ep, &ly, &lp);
	rate = tb_len(ly, lp) / dt;
	flick = tb_len(ly, lp) / (dt > TB_FLICK_DT ? dt : TB_FLICK_DT);
	k = dt >= TB_DECAY ? 0.0f : 1.0f - dt / TB_DECAY;
	b->acc_y *= k;
	b->acc_p *= k;
	if (rate >= c->speed)
	{
		b->acc_y += ly;
		b->acc_p += lp;
	}
	if (tb_len(b->acc_y, b->acc_p) >= c->angle || flick >= c->flick)
	{
		s = TouchBrk_Slot(b, ent);
		for (i = 0; s < 0 && i < TB_OUT; i++)
			if (!b->out[i].ent || b->out[i].until <= now)
				s = i;
		if (s < 0) // all taken: the one that ends first
			for (s = 0, i = 1; i < TB_OUT; i++)
				if (b->out[i].until < b->out[s].until)
					s = i;
		b->out[s].ent = ent;
		b->out[s].until = now + c->cooldown;
		b->acc_y = b->acc_p = 0;
		return true;
	}
	return false;
}

// Whether the released target is still held out.
static qbool TouchBrk_Held(const touch_brk_t *b, double now, int ent)
{
	int s = TouchBrk_Slot(b, ent);
	return s >= 0 && now < b->out[s].until;
}

// A released target the drag is still moving away from stays released, or the
// pull would take it back in the middle of the turn.
static void TouchBrk_Leaving(touch_brk_t *b, const touch_brk_cfg_t *c, double now, float dt, int ent, float ty, float tp, float ey, float ep)
{
	int s = TouchBrk_Slot(b, ent);
	if (s >= 0 && dt > 0 && ty * ey + tp * ep < 0 && tb_len(ty, tp) / dt >= c->speed)
		b->out[s].until = now + c->cooldown;
}

// The player turns back onto a released target: the turn this frame is toward
// it, at least at the break speed, and the crosshair is within `ret` degrees of
// its edge. gap: degrees from the view to its edge (0 on it). True and the
// hold is dropped.
static qbool TouchBrk_Return(touch_brk_t *b, const touch_brk_cfg_t *c, float dt, int ent, float ty, float tp, float ey, float ep, float gap)
{
	float ly, lp;
	int s = TouchBrk_Slot(b, ent);
	if (dt <= 0 || gap > c->ret)
		return false;
	if (TouchBrk_Toward(ty, tp, ey, ep, &ly, &lp) / dt < c->speed)
		return false;
	if (s >= 0)
	{
		b->out[s].until = 0;
		b->out[s].ent = 0;
	}
	return true;
}

#endif

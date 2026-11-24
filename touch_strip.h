#ifndef TOUCH_STRIP_H
#define TOUCH_STRIP_H

/*
Godnotic: where the touch weapon strip (the side rail) puts its rows, and which
row a finger lands on. Pure geometry with no engine types, so
tests/touch-strip checks every case without a build.

From the top: the owned weapons, then the HOOK entry (the grappling hook is
picked from this list), then a gap, then the NADE action row, a little taller
for a thumb, then the resize grip.

One finger lands on exactly one band. The bands share edges and never overlap:
the forgiving pad is sideways, and upwards over the first row only (nothing
is above it). A row that exists but cannot be pressed now (a grenade on its
way, a hook out of fuel) swallows the finger (TSTRIP_DEAD) instead of handing
it to the row beside it. The old strip padded every row on all four sides and
tested the NADE row only while a grenade was ready, so a finger on the
waiting NADE row's top edge fell into the pad of the weapon row above, which
could be the Hook weapon (impulse 0), and the hook came out.
*/

#define TSTRIP_ROW_H  26.0f  // a weapon row and the HOOK entry, in strip units
#define TSTRIP_ACT_H  34.0f  // the NADE action row
#define TSTRIP_GAP     6.0f  // between the list and the action row
#define TSTRIP_PAD     4.0f
#define TSTRIP_GRIP_H 12.0f
#define TSTRIP_FORGIVE 0.12f // sideways (and upward over the first row), of a row height

enum { TSTRIP_MISS = -1, TSTRIP_WEAPON = 0, TSTRIP_HOOK, TSTRIP_NADE, TSTRIP_DEAD };

typedef struct tstrip_s
{
	int n;             // weapon rows (the Hook weapon is not one: it is the HOOK entry)
	int hook;          // a HOOK entry
	int hook_press;    // ... that can be pressed now
	int nade;          // a NADE row
	int nade_press;    // ... that can be pressed now
	float u;           // unit
	float cx, top;     // centre line, first row's top
	float row_w;       // row width in pixels
} tstrip_t;

// Height of the rows, gap and action row, before pad and grip, in strip units.
static float TouchStrip_Units(int n, int hook, int nade)
{
	return (n + (hook ? 1 : 0)) * TSTRIP_ROW_H + (nade ? TSTRIP_GAP + TSTRIP_ACT_H : 0);
}

static float TouchStrip_HookTop(const tstrip_t *s) { return s->top + s->n * TSTRIP_ROW_H * s->u; }
static float TouchStrip_ListEnd(const tstrip_t *s) { return s->top + (s->n + (s->hook ? 1 : 0)) * TSTRIP_ROW_H * s->u; }
static float TouchStrip_NadeTop(const tstrip_t *s) { return TouchStrip_ListEnd(s) + TSTRIP_GAP * s->u; }
static float TouchStrip_GripTop(const tstrip_t *s)
{
	return s->nade ? TouchStrip_NadeTop(s) + TSTRIP_ACT_H * s->u : TouchStrip_ListEnd(s);
}

// What a finger at (px, py) lands on: TSTRIP_WEAPON (row in *row), TSTRIP_HOOK,
// TSTRIP_NADE, TSTRIP_DEAD (a row or gap that takes the finger and does
// nothing) or TSTRIP_MISS.
static int TouchStrip_Hit(const tstrip_t *s, float px, float py, int *row)
{
	float rh = TSTRIP_ROW_H * s->u, f = TSTRIP_FORGIVE * rh;
	float list_end = TouchStrip_ListEnd(s), nade_top, grip_top;
	if (row)
		*row = -1;
	if (px < s->cx - s->row_w * 0.5f - f || px > s->cx + s->row_w * 0.5f + f)
		return TSTRIP_MISS;
	if (py < s->top - f)
		return TSTRIP_MISS;
	if (py < list_end)
	{
		int i = (int)((py - s->top) / rh);
		if (py < s->top)
			i = 0;
		if (i < s->n)
		{
			if (row)
				*row = i;
			return TSTRIP_WEAPON;
		}
		return s->hook_press ? TSTRIP_HOOK : TSTRIP_DEAD;
	}
	if (!s->nade)
		return TSTRIP_MISS;
	nade_top = TouchStrip_NadeTop(s);
	grip_top = TouchStrip_GripTop(s);
	if (py < nade_top)
		return TSTRIP_DEAD;
	if (py < grip_top)
		return s->nade_press ? TSTRIP_NADE : TSTRIP_DEAD;
	return TSTRIP_MISS;
}

// An old saved layout's touch_hook_visible (0 never, 1 always, 2 while a hook
// is to hand) as the HOOK entry's switch: the entry is part of the strip, there
// while the server has a hook, so only "never" still means something.
static int TouchStrip_HookEntryOn(int touch_hook_visible)
{
	return touch_hook_visible != 0;
}

#endif

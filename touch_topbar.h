/*
Godnotic: the in-match top bar, one layout for everything that sits on the
top edge while the touch controls are on.

  [vitals]            [timer]        (i) [>_] [MENU] [SCORE] [CHAT]
                      [mode icons]        info messages   | scores
                                          kill feed

The chrome row hugs the top corner on the look hand's side (right; left when
left-handed): CHAT outermost, then SCORE, MENU, the console button and, at
the inner end, the vote chip's slot (kept free, so nothing jumps when a vote
is called). Sizes are in dp, positions are measured from the edge and the
safe-area insets, so the row is the same physical size on a phone and a
tablet and its members can never touch each other, whatever the aspect
ratio. A row that would reach the match clock scales its members uniformly. Everything the game itself draws near that corner (score list, info
messages, kill feed, vote print, mode icons, radar) is placed below or
beside the row by cvar (touch_hud.c TouchHUD_SyncStockPanels), under the
port's CSQC and a server's alike.

Pure: scripts/test-touch-controls.sh compiles this alone and checks that no
two rects intersect at 1920x1200, 2340x1080 and 1280x720.
*/
#ifndef TOUCH_TOPBAR_H
#define TOUCH_TOPBAR_H

#define TOUCHBAR_MARGIN_DP   8.0f    // from the edge (after the safe inset)
#define TOUCHBAR_GAP_DP      8.0f    // between members
#define TOUCHBAR_TOP_DP      6.0f
#define TOUCHBAR_CHIP_DP     44.0f   // the vote chip's slot
#define TOUCHBAR_CLOCK_DP    64.0f   // half of what the match clock keeps in the middle
#define TOUCHBAR_MIN_ASPECT  1.6f    // a pill is never narrower than this times its height

typedef struct touchbar_rect_s { float x, y, w, h; } touchbar_rect_t;

typedef struct touchbar_in_s
{
	float w, h;          // the 2D space (console units)
	float u;             // console units per dp
	float in_l, in_t, in_r;  // safe-area insets, console units
	float pill_h;        // legacy common height
	float menu_h, score_h, chat_h; // measured heights (0 uses pill_h)
	float menu_w, score_w, chat_w; // measured widths (0 uses common aspect)
	float aspect;        // a pill's width / height when there is room
	float console;       // the console button's side, console units (0: not shown)
	int menu, score, chat;   // shown
	int left_handed;
} touchbar_in_t;

typedef struct touchbar_s
{
	touchbar_rect_t chip, console, menu, score, chat;   // w == 0: not there
	float aspect;    // the pill aspect used
	float bottom;    // the game's own panels start below this
	float inner;     // the row's inner end (x), for what sits beside it
} touchbar_t;

static inline int TouchTopBar_Intersect(const touchbar_rect_t *a, const touchbar_rect_t *b)
{
	if (a->w <= 0 || b->w <= 0)
		return 0;
	return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static inline void TouchTopBar_Layout(const touchbar_in_t *in, touchbar_t *out)
{
	float u = in->u, m = TOUCHBAR_MARGIN_DP * u, g = TOUCHBAR_GAP_DP * u;
	float chip = TOUCHBAR_CHIP_DP * u;
	float inset = in->left_handed ? in->in_l : in->in_r;
	float rowh, cy, x, aspect = in->aspect, avail, fixed;
	float scale = 1, total = 0, maxheight = 0, vertical;
	touchbar_rect_t *order[5];
	float width[5], height[5];
	int n = 0, i;
	touchbar_rect_t zero = {0, 0, 0, 0};
	out->chip = out->console = out->menu = out->score = out->chat = zero;
	// Measure each actual pill before fitting the complete row. All members
	// shrink by the same factor; gaps stay fixed unless the screen itself is
	// narrower than the gaps.
	if (in->chat) { order[n] = &out->chat; height[n] = in->chat_h > 0 ? in->chat_h : in->pill_h; width[n] = in->chat_w > 0 ? in->chat_w : height[n] * aspect; n++; }
	if (in->score) { order[n] = &out->score; height[n] = in->score_h > 0 ? in->score_h : in->pill_h; width[n] = in->score_w > 0 ? in->score_w : height[n] * aspect; n++; }
	if (in->menu) { order[n] = &out->menu; height[n] = in->menu_h > 0 ? in->menu_h : in->pill_h; width[n] = in->menu_w > 0 ? in->menu_w : height[n] * aspect; n++; }
	if (in->console > 0) { order[n] = &out->console; width[n] = height[n] = in->console; n++; }
	order[n] = &out->chip; width[n] = height[n] = chip; n++;
	avail = in->w * 0.5f - TOUCHBAR_CLOCK_DP * u - inset - m;
	// If density or insets consume the clock reservation, use the safe width.
	if (avail <= 0) avail = in->w - in->in_l - in->in_r - m;
	if (avail < 0) avail = 0;
	if (n > 1 && g * (n - 1) > avail * 0.5f) g = avail * 0.5f / (n - 1);
	fixed = g * (n - 1);
	for (i = 0; i < n; i++) { total += width[i]; if (height[i] > maxheight) maxheight = height[i]; }
	if (total > 0 && total > avail - fixed) scale = (avail - fixed) / total;
	vertical = in->h - in->in_t - TOUCHBAR_TOP_DP * u;
	if (vertical < 0) vertical = 0;
	if (maxheight > 0 && maxheight * scale > vertical) scale = vertical / maxheight;
	rowh = 0;
	for (i = 0; i < n; i++) {
		width[i] *= scale; height[i] *= scale;
		if (height[i] > rowh) rowh = height[i];
	}
	cy = in->in_t + TOUCHBAR_TOP_DP * u + rowh * 0.5f;
	x = in->w - inset - m;
	for (i = 0; i < n; i++)
	{
		order[i]->w = width[i];
		order[i]->h = height[i];
		order[i]->x = x - width[i];
		order[i]->y = cy - height[i] * 0.5f;
		x -= width[i] + g;
	}
	out->inner = x + g;
	if (in->left_handed)
	{
		for (i = 0; i < n; i++)
			order[i]->x = in->w - order[i]->x - order[i]->w;
		out->inner = in->w - out->inner;
	}
	out->aspect = aspect;
	out->bottom = in->in_t + TOUCHBAR_TOP_DP * u + rowh + TOUCHBAR_TOP_DP * u;
}

#endif

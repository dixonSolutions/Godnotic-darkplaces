/*
Godnotic: engine-side copy of the Touch HUD (see touch_hud.h).

Everything here mirrors client/touch_*.qc in xonotic-data.pk3dir:
  touch_layout.qc   -> TouchHUD_Center / Radius / Half / zones
  touch_shape.qc    -> the pic-based disc / ring / shadow / capsule primitives
  touch_widget.qc   -> Button / Capsule / ChromePill (shadow, surface, rim, ink)
  touch_draw.qc     -> which controls draw, and the move stick
  touch_input.qc    -> sticky finger roles, stick thresholds, hop latch
  touch_look.qc     -> drag look with the one-euro filter
The cvar names and defaults are the CSQC's own, so a profile or a saved layout
applies to both.
*/

#include "quakedef.h"
#include "touch_uib.h" // TouchUIB_LoadingCardUp
#include "touch_frontend.h" // TouchFE_MenuHidden
#include "libcurl.h"
#include "cl_screen.h"
#include "csprogs.h"
#include "touch_hud.h"
#include "touch_aim.h"
#include "touch_hook.h"
#include "touch_nade.h"
#include "touch_dash.h"
#include "touch_strip.h"
#include "touch_turn.h"
#include "touch_ui.h"
#include "touch_altfire.h"
#include "touch_spectate.h"
#include "touch_hudmode.h"
#include "touch_topbar.h"
#include "touch_cursor.h"
#include "touch_feel.h"
#include "touch_mention.h"

extern cvar_t vid_touchscreen;
extern cvar_t vid_conwidth;
extern cvar_t vid_conheight;
extern cvar_t cl_forwardspeed;
extern cvar_t cl_sidespeed;
extern cvar_t cl_touch_aim_debug;
extern qbool cl_touch_csqc_active;

// vid_sdl.c's finger table: [i][0] = finger id + 1 (0 when up), [i][1..2] =
// position normalised to the window. The last slot is the mouse-as-finger
// used for desktop testing.
#define TOUCHHUD_ENGINE_FINGERS 11
extern float multitouch[TOUCHHUD_ENGINE_FINGERS][3];

// ---------------------------------------------------------------------------
// Theme (touch_theme.qh)
// ---------------------------------------------------------------------------
static const float C_SURFACE[3]      = {0.07f, 0.08f, 0.10f};
static const float C_RIM[3]          = {1, 1, 1};
static const float C_INK[3]          = {1, 1, 1};
static const float C_SHADOW[3]       = {0, 0, 0};
static const float C_ACCENT[3]       = {0.208f, 0.518f, 0.894f};
static const float C_ACCENT_LIGHT[3] = {0.506f, 0.816f, 1.0f};
static const float C_DANGER[3]       = {0.902f, 0.176f, 0.259f};
static const float C_SUCCESS[3]      = {0.227f, 0.580f, 0.290f};
static const float C_WARNING[3]      = {0.784f, 0.533f, 0.000f};

#define A_SURFACE        0.34f
#define A_SURFACE_ACTIVE 0.58f
#define A_SURFACE_LATCH  0.46f
#define A_RIM            0.62f
#define A_RIM_ACTIVE     0.95f
#define A_CONTOUR        0.50f
#define A_SHADOW         0.36f
#define A_INK            0.95f
#define A_GHOST          0.16f
#define A_GLOW           0.30f
#define A_CHROME_REST    0.55f
#define A_CHROME_FILL    0.88f
#define W_RING_THIN      0.035f
#define W_CONTOUR_INSET  0.030f
// Border width of every control, in UI units at the reference size (touch_theme.qh TOUCH_W_STROKE).
#define W_STROKE         1.5f
#define UI_REF_SHORT_AXIS 540.0f
#define ELEVATION_Y      0.045f
#define FONT_MIN         9.0f
#define FONT_MAX         20.0f
#define FONT_FIT_H       0.75f
#define FONT_FIT_W       0.70f
#define FONT_ADVANCE     0.55f
#define FONT_READOUT     24.0f
// The pills kept with the controls off (keyboard and mouse) draw fainter.
#define DESK_ALPHA       0.6f

// ---------------------------------------------------------------------------
// Cvars: the CSQC registers these (touch_init.qc); when only a server's CSQC
// has run this session they may not exist, so every read carries the default.
// ---------------------------------------------------------------------------
static float cv(const char *name, float def)
{
	cvar_t *var = Cvar_FindVar(&cvars_all, name, ~0);
	if (!var)
		return def;
	return var->value;
}

// ---------------------------------------------------------------------------
// Layout (touch_layout.qc)
// ---------------------------------------------------------------------------
static float ui_w(void) { return max(1.0f, vid_conwidth.value); }
static float ui_h(void) { return max(1.0f, vid_conheight.value); }
static float screen_min(void) { return min(ui_w(), ui_h()); }
static float widget_scale(void) { return max(0.01f, cv("touch_scale", 1.0f)); }
static float ui_scale(void) { return screen_min() / UI_REF_SHORT_AXIS; }
static float mirror_x(float nx) { return cv("touch_handedness", 0) ? 1.0f - nx : nx; }
static float crouch_x(float nx) { return ui_w() < ui_h() ? nx - 0.04f : nx; }
static void widget_center(float nx, float ny, float *cx, float *cy)
{
	*cx = mirror_x(nx) * ui_w();
	*cy = ny * ui_h();
}
// UI units per density-independent pixel (touch_hudmode.h TouchDp_Density):
// sizes never go under the dp floors, whatever touch_scale and the screen.
static float dp_units(void)
{
	float px_short = (float)min(vid.mode.width, vid.mode.height);
	float units_per_px = vid.mode.height > 0 ? ui_h() / (float)vid.mode.height : 1.0f;
	// a desk with no touchscreen: a mouse needs no finger-sized targets
	if (!vid_touchscreen.integer && !VID_SDL_HasTouchDevices() && !vid_touchscreen_detected.integer)
		return units_per_px;
	return TouchDp_Density(cv("vid_touchscreen_ydpi", 0), px_short) * units_per_px;
}
static float widget_radius(float nsize)
{
	// a circle is a touch target: never under 48 dp across
	return max(screen_min() * nsize * 0.5f * widget_scale(), TOUCHDP_MIN_TARGET * 0.5f * dp_units());
}
// The move stick: its ring is the thumb's travel, so it has its own floor.
#define TOUCHDP_STICK_RADIUS 64.0f
static float cv(const char *name, float def);
static float stick_radius(void)
{
	return max(widget_radius(cv("touch_move_size", 0.400f)), TOUCHDP_STICK_RADIUS * dp_units());
}
static void stick_center(float *cx, float *cy)
{
	float r = stick_radius();
	widget_center(cv("touch_move_x", 0.170f), cv("touch_move_y", 0.680f), cx, cy);
	if (ui_w() < ui_h() && 2 * r <= ui_w() && 2 * r <= ui_h())
	{
		*cx = bound(r, *cx, ui_w() - r);
		*cy = bound(r, *cy, ui_h() - r);
	}
}
static void widget_half(float nsize, float aspect, float *hx, float *hy)
{
	// a pill is drawn 36 dp tall at least; hit_rect grows its target to 48 dp
	float short_axis = max(screen_min() * nsize * widget_scale(), TOUCHDP_PILL_HEIGHT * dp_units());
	float long_axis = short_axis * max(1.0f, aspect);
	*hx = long_axis * 0.5f;
	*hy = short_axis * 0.5f;
}
static qbool hit_circle(float px, float py, float nx, float ny, float nsize, float grab)
{
	float cx, cy, dx, dy, r;
	widget_center(nx, ny, &cx, &cy);
	r = widget_radius(nsize) * max(1.0f, grab);
	dx = px - cx; dy = py - cy;
	return dx * dx + dy * dy <= r * r;
}
static qbool hit_rect(float px, float py, float nx, float ny, float nsize, float aspect, float grab)
{
	float cx, cy, hx, hy;
	widget_center(nx, ny, &cx, &cy);
	widget_half(nsize, aspect, &hx, &hy);
	hx *= max(1.0f, grab); hy *= max(1.0f, grab);
	hy = max(hy, TOUCHDP_MIN_TARGET * 0.5f * dp_units());
	hx = max(hx, hy);
	return px >= cx - hx && px <= cx + hx && py >= cy - hy && py <= cy + hy
		&& px >= 0 && py >= 0 && px <= ui_w() && py <= ui_h();
}
static qbool in_edge_deadzone(float px, float py)
{
	float dz = cv("touch_edge_deadzone_px", 10);
	return px < dz || py < dz || px > ui_w() - dz || py > ui_h() - dz;
}
static qbool in_move_zone(float px, float py)
{
	float w = cv("touch_move_zone_w", 0.32f);
	float left = 0, right;
	if (w <= 0)
		w = cv("touch_look_zone_left", 0.32f);
	right = w * ui_w();
	if (cv("touch_handedness", 0))
	{
		left = (1.0f - w) * ui_w();
		right = ui_w();
	}
	return px >= left && px <= right && py > ui_h() * 0.42f;
}
static qbool in_look_zone(float px, float py)
{
	float left = cv("touch_look_zone_left", 0.32f);
	float right = cv("touch_look_zone_right", 1.0f);
	if (cv("touch_handedness", 0))
	{
		float t = 1.0f - right;
		right = 1.0f - left;
		left = t;
	}
	return px >= left * ui_w() && px <= right * ui_w();
}

// ---------------------------------------------------------------------------
// Shapes (touch_shape.qc): everything is a tinted pic
// ---------------------------------------------------------------------------
static cachepic_t *pic_disc, *pic_disc_soft, *pic_ring_thin, *pic_shadow;
static qbool pics_ready, pics_probed;

static void shapes_probe(void)
{
	if (pics_probed)
		return;
	pics_probed = true;
	pic_disc = Draw_CachePic_Flags("gfx/touch/disc", CACHEPICFLAG_FAILONMISSING);
	pic_disc_soft = Draw_CachePic_Flags("gfx/touch/disc_soft", CACHEPICFLAG_FAILONMISSING);
	pic_ring_thin = Draw_CachePic_Flags("gfx/touch/ring_thin", CACHEPICFLAG_FAILONMISSING);
	pic_shadow = Draw_CachePic_Flags("gfx/touch/shadow", CACHEPICFLAG_FAILONMISSING);
	pics_ready = Draw_IsPicLoaded(pic_disc) && Draw_IsPicLoaded(pic_ring_thin)
		&& Draw_IsPicLoaded(pic_shadow) && Draw_IsPicLoaded(pic_disc_soft);
	if (!pics_ready)
		Con_Printf(CON_WARN "TouchHUD: gfx/touch shape masks missing, drawing flat\n");
}

static void shape_centered(cachepic_t *pic, float cx, float cy, float hx, float hy, const float *rgb, float alpha)
{
	if (hx < 0.5f || hy < 0.5f || alpha <= 0)
		return;
	if (Draw_IsPicLoaded(pic))
		DrawQ_Pic(cx - hx, cy - hy, pic, hx * 2, hy * 2, rgb[0], rgb[1], rgb[2], alpha, 0);
	else // Touch: no mask (headless, a basedir without gfx/touch): round, never a solid square behind a round button
		DrawQ_RoundedBox(cx, cy, hx, hy, min(hx, hy), 0, rgb[0], rgb[1], rgb[2], alpha * 0.6f, 0);
}
// Discs, rings and capsules are geometry (DrawQ_RoundedBox): antialiased at any
// size, and a border the same width all the way round. The masks are kept
// for the soft shapes (glow, shadow).
static void shape_disc(float cx, float cy, float r, const float *rgb, float a) { DrawQ_RoundedBox(cx, cy, r, r, r, 0, rgb[0], rgb[1], rgb[2], a, 0); }
// Touch: the glow and the shade are stacked discs, not the soft masks: the masks drew as solid
// squares behind every round button where their alpha is lost (Android, offscreen), and a
// bright square behind a held DASH.
static void shape_glow(float cx, float cy, float r, const float *rgb, float a)
{
	int i;
	for (i = 0; i < 3; i++)
		DrawQ_RoundedBox(cx, cy, r * (1.0f - 0.18f * i), r * (1.0f - 0.18f * i), r * (1.0f - 0.18f * i), 0, rgb[0], rgb[1], rgb[2], a * 0.4f, 0);
}
static void shape_shadow(float cx, float cy, float r, float a)
{
	int i;
	for (i = 0; i < 3; i++)
		DrawQ_RoundedBox(cx, cy, r * (1.0f - 0.08f * i), r * (1.0f - 0.08f * i), r * (1.0f - 0.08f * i), 0, C_SHADOW[0], C_SHADOW[1], C_SHADOW[2], a * 0.4f, 0);
}
static void shape_ring_stroke(float cx, float cy, float r, float stroke, const float *rgb, float a) { DrawQ_RoundedBox(cx, cy, r, r, r, stroke, rgb[0], rgb[1], rgb[2], a, 0); }
static void shape_ring(float cx, float cy, float r, const float *rgb, float a) { shape_ring_stroke(cx, cy, r, r * W_RING_THIN, rgb, a); }

// Left half / right half of a pic, for capsule caps (drawsubpic in QC).
static void shape_half_pic(cachepic_t *pic, float x, float y, float w, float h, float s0, float s1, const float *rgb, float a)
{
	if (w < 0.5f || h < 0.5f || a <= 0)
		return;
	if (!Draw_IsPicLoaded(pic))
	{
		DrawQ_Fill(x, y, w, h, rgb[0], rgb[1], rgb[2], a, 0);
		return;
	}
	DrawQ_SuperPic(x, y, pic, w, h,
		s0, 0, rgb[0], rgb[1], rgb[2], a,
		s1, 0, rgb[0], rgb[1], rgb[2], a,
		s0, 1, rgb[0], rgb[1], rgb[2], a,
		s1, 1, rgb[0], rgb[1], rgb[2], a, 0);
}

// As Touch_Shape_CapsuleFrom: the middle is the mask's centre column stretched
// across, so its top and bottom edges match the caps' for a fill, a ring or a
// shadow. Solid fills there gave a ring a thin top edge and a thick bottom one.
#define CAPSULE_MID_UV 0.001f
static void shape_capsule_from(cachepic_t *pic, float cx, float cy, float hx, float hy, const float *rgb, float a)
{
	float cap = hy, midw;
	if (hx < 0.5f || hy < 0.5f || a <= 0)
		return;
	if (hx <= cap)
	{
		shape_centered(pic, cx, cy, hy, hy, rgb, a);
		return;
	}
	shape_half_pic(pic, cx - hx, cy - cap, cap, cap * 2, 0.0f, 0.5f, rgb, a);
	shape_half_pic(pic, cx + hx - cap, cy - cap, cap, cap * 2, 0.5f, 1.0f, rgb, a);
	midw = (hx - cap) * 2;
	shape_half_pic(pic, cx - hx + cap, cy - cap, midw, cap * 2, 0.5f - CAPSULE_MID_UV, 0.5f + CAPSULE_MID_UV, rgb, a);
}
static void shape_capsule(float cx, float cy, float hx, float hy, const float *rgb, float a) { DrawQ_RoundedBox(cx, cy, hx, hy, hy, 0, rgb[0], rgb[1], rgb[2], a, 0); }
static void shape_capsule_ring_stroke(float cx, float cy, float hx, float hy, float stroke, const float *rgb, float a) { DrawQ_RoundedBox(cx, cy, hx, hy, hy, stroke, rgb[0], rgb[1], rgb[2], a, 0); }
static void shape_capsule_ring(float cx, float cy, float hx, float hy, const float *rgb, float a) { shape_capsule_ring_stroke(cx, cy, hx, hy, hy * W_RING_THIN, rgb, a); }
static void shape_capsule_shadow(float cx, float cy, float hx, float hy, float a)
{
	int i;
	for (i = 0; i < 3; i++)
		DrawQ_RoundedBox(cx, cy, hx * (1.0f - 0.03f * i), hy * (1.0f - 0.08f * i), hy * (1.0f - 0.08f * i), 0, C_SHADOW[0], C_SHADOW[1], C_SHADOW[2], a * 0.4f, 0);
}

static float label_width(float fs, const char *label)
{
	float w = DrawQ_TextWidth(label, 0, fs, fs, false, FONT_CHAT);
	if (w > 0)
		return w;
	return strlen(label) * fs * FONT_ADVANCE;
}
static void shape_label(float cx, float cy, float fs, const char *label, const float *rgb, float a)
{
	float w;
	if (!label || !label[0] || a <= 0 || fs < 1)
		return;
	w = label_width(fs, label);
	DrawQ_String(cx - w * 0.5f, cy - fs * 0.5f, label, 0, fs, fs, rgb[0], rgb[1], rgb[2], a, 0, NULL, false, FONT_CHAT);
}
static void shape_label_right(float rx, float cy, float fs, const char *label, const float *rgb, float a)
{
	float w;
	if (!label || !label[0] || a <= 0 || fs < 1)
		return;
	w = label_width(fs, label);
	DrawQ_String(rx - w, cy - fs * 0.5f, label, 0, fs, fs, rgb[0], rgb[1], rgb[2], a, 0, NULL, false, FONT_CHAT);
}

// ---------------------------------------------------------------------------
// Widgets (touch_widget.qc)
// ---------------------------------------------------------------------------
static float label_size(float hx, float hy, const char *label)
{
	// Measured at the size it will be drawn at, as in touch_widget.qc: font
	// size snapping makes a unit-size measurement several times too wide.
	float fs = bound(FONT_MIN, hy * FONT_FIT_H, FONT_MAX);
	float w = label_width(fs, label);
	float room = hx * 2 * FONT_FIT_W;
	if (w > room)
		fs *= room / w;
	return bound(FONT_MIN, fs, FONT_MAX);
}
static float elevation(float r) { return max(1.0f, r * ELEVATION_Y); }

// As Touch_Widget_Rim: one border width for every control, the light line
// centred in a dark band twice its width.
static float widget_border(void) { return W_STROKE * ui_scale(); }
static void widget_rim(float cx, float cy, float r, const float *rgb, float a)
{
	float b = widget_border();
	shape_ring_stroke(cx, cy, r, b * 2, C_SHADOW, a * A_CONTOUR);
	shape_ring_stroke(cx, cy, r - b * 0.5f, b, rgb, a);
}
static void widget_capsule_rim(float cx, float cy, float hx, float hy, const float *rgb, float a)
{
	float b = widget_border();
	shape_capsule_ring_stroke(cx, cy, hx, hy, b * 2, C_SHADOW, a * A_CONTOUR);
	shape_capsule_ring_stroke(cx, cy, hx - b * 0.5f, hy - b * 0.5f, b, rgb, a);
}

// touch_glass_hollow, as Touch_GlassBody in touch_widget.qc: 0 tinted glass,
// 1 hollow (outline and label only). Scales fill and shadow; a press keeps
// some fill, since under the thumb it is the only feedback left.
static float glass_body(qbool pressed)
{
	float body = 1 - bound(0.0f, cv("touch_glass_hollow", 0), 1.0f);
	return pressed ? max(body, 0.45f) : body;
}

// Overlay panels need a readable backing even when the HUD glass is hollow.
void TouchHUD_GlassPanel(float x, float y, float w, float h, float radius, float alpha)
{
	float cx = x + w * 0.5f, cy = y + h * 0.5f;
	float hx = w * 0.5f, hy = h * 0.5f;
	float r = min(radius, min(hx, hy));
	DrawQ_RoundedBox(cx, cy + r * 0.15f, hx + r * 0.12f, hy + r * 0.12f, r, 0, C_SHADOW[0], C_SHADOW[1], C_SHADOW[2], alpha * A_SHADOW * glass_body(false), 0);
	DrawQ_RoundedBox(cx, cy, hx, hy, r, 0, C_SURFACE[0], C_SURFACE[1], C_SURFACE[2], alpha * (0.88f + 0.1f * glass_body(false)), 0);
	DrawQ_RoundedBox(cx, cy, hx, hy, r, max(1.0f, r * W_RING_THIN), C_RIM[0], C_RIM[1], C_RIM[2], alpha * A_RIM * 0.6f, 0);
}

void TouchHUD_GlassControl(float cx, float cy, float hx, float hy, qbool pressed, qbool selected, float alpha)
{
	qbool active = pressed || selected;
	const float *color = active ? C_ACCENT : C_SURFACE;
	shape_capsule_shadow(cx, cy + elevation(min(hx, hy)), hx, hy, alpha * A_SHADOW * glass_body(false));
	shape_capsule(cx, cy, hx, hy, color, alpha * (pressed ? A_SURFACE_ACTIVE : selected ? A_SURFACE_LATCH : A_SURFACE) * glass_body(active));
	widget_capsule_rim(cx, cy, hx, hy, active ? C_ACCENT_LIGHT : C_RIM, alpha * (active ? A_RIM_ACTIVE : A_RIM));
}

static void widget_button(float cx, float cy, float r, const char *label, qbool pressed, float alpha, const float *accent)
{
	const float *surface = pressed ? accent : C_SURFACE;
	const float *rim = pressed ? accent : C_RIM;
	float a_surface = pressed ? A_SURFACE_ACTIVE : A_SURFACE;
	float a_rim = pressed ? A_RIM_ACTIVE : A_RIM;
	float fs;
	if (r < 2 || alpha <= 0)
		return;
	fs = label_size(r, r, label);
	shape_shadow(cx, cy + elevation(r), r * 1.12f, alpha * A_SHADOW * glass_body(false));
	shape_disc(cx, cy, r, surface, alpha * a_surface * glass_body(pressed));
	if (pressed)
		shape_glow(cx, cy, r * 1.35f, accent, alpha * A_GLOW);
	widget_rim(cx, cy, r, rim, alpha * a_rim);
	shape_label(cx, cy, fs, label, C_INK, alpha * A_INK);
}

static void widget_capsule(float cx, float cy, float hx, float hy, const char *label, qbool pressed, qbool latched, float alpha, const float *accent)
{
	qbool tinted = pressed || latched;
	const float *surface = tinted ? accent : C_SURFACE;
	const float *rim = tinted ? accent : C_RIM;
	float a_surface = A_SURFACE;
	float a_rim = tinted ? A_RIM_ACTIVE : A_RIM;
	float fs;
	if (hx < 2 || hy < 2 || alpha <= 0)
		return;
	if (pressed)
		a_surface = A_SURFACE_ACTIVE;
	else if (latched)
		a_surface = A_SURFACE_LATCH;
	fs = label_size(hx, hy, label);
	shape_capsule_shadow(cx, cy + elevation(hy), hx + hy * 0.10f, hy + hy * 0.10f, alpha * A_SHADOW * glass_body(false));
	shape_capsule(cx, cy, hx, hy, surface, alpha * a_surface * glass_body(tinted));
	if (pressed)
		shape_capsule(cx, cy, hx + hy * 0.18f, hy + hy * 0.18f, accent, alpha * A_GLOW);
	widget_capsule_rim(cx, cy, hx, hy, rim, alpha * a_rim);
	shape_label(cx, cy, fs, label, C_INK, alpha * A_INK);
	if (latched)
	{
		float pip_x = cx + (hx - hy);
		float pip_r = max(2.0f, hy * 0.26f);
		shape_disc(pip_x, cy, pip_r, C_INK, alpha * A_INK);
		shape_ring(pip_x, cy, pip_r * 1.5f, C_INK, alpha * A_RIM);
	}
}

static void widget_chrome_pill(float cx, float cy, float hx, float hy, const char *label, qbool pressed, float alpha, const float *accent)
{
	float a = pressed ? max(0.85f, alpha) : max(A_CHROME_REST, alpha * 0.8f);
	const float *surface = pressed ? accent : C_SURFACE;
	const float *rim = pressed ? accent : C_RIM;
	float fs;
	if (hx < 2 || hy < 2)
		return;
	fs = min(label_size(hx, hy, label), label_size(hx, hy, "CONSOLE"));
	shape_capsule(cx, cy, hx, hy, surface, a * A_CHROME_FILL * glass_body(pressed));
	widget_capsule_rim(cx, cy, hx, hy, rim, a * A_RIM);
	shape_label(cx, cy, fs, label, C_INK, a * A_INK);
}

// ---------------------------------------------------------------------------
// Input state (touch_input.qc, touch_look.qc)
// ---------------------------------------------------------------------------
enum
{
	ROLE_NONE = -1, ROLE_IGNORED = -2,
	ROLE_MOVE = 0, ROLE_LOOK, ROLE_FIRE, ROLE_FIRE2, ROLE_HOOK,
	ROLE_JUMP = 100, ROLE_CROUCH, ROLE_WEAPON, ROLE_ZOOM, ROLE_DODGE, ROLE_RELOAD, ROLE_CON, ROLE_PAUSE, ROLE_SHEET, ROLE_CHAT, ROLE_SCORES, ROLE_VYES, ROLE_VNO, ROLE_TURN, ROLE_HOPLOCK, ROLE_NADE,
	ROLE_WEPSTRIP = 200, ROLE_WEPGRIP
};

#define MAX_FINGERS 4
typedef struct finger_s
{
	qbool used;
	int src;            // multitouch id (nonzero)
	int role;
	float down_x, down_y, last_x, last_y;
	double down_time;
	double lost_time;   // 0 while reported
	qbool first_frame;
	qbool fired;        // role-specific one-shot (pill tap, weapon swipe)
	qbool draglook;     // FIRE or HOOK finger that has escaped into a look drag
	float prev_look_x, prev_look_y;
	int impulse;        // ROLE_WEPSTRIP: the row's impulse
	float grab_u;       // ROLE_WEPGRIP: the strip's unit when the grip was taken
	int grab_n;         // ROLE_WEPGRIP: its rows then
	qbool resized;      // ROLE_WEPGRIP: the scale changed
} finger_t;

static finger_t fingers[MAX_FINGERS];
static float move_off_x, move_off_y;   // the knob, -1..1, for drawing
// The move stick (touch_feel.h): where its centre floats to while a thumb
// holds it, and what it asks for as an analog move (x right, y down, length
// 0..1). csqc_axis is the same from the port's CSQC HUD (touch_move).
static touchstick_t stick;
static qbool stick_held;
static float stick_axis_x, stick_axis_y;
static float csqc_axis_x, csqc_axis_y;
static cvar_t touch_move_analog_cap = {CF_CLIENT | CF_READONLY, "_touch_move_analog", "1", "this engine takes an analog move from the touch stick (the touch_move command); read-only"};
static qbool key_forward, key_back, key_left, key_right, key_attack, key_attack2, key_jump, key_crouch, key_scores, key_zoom;
static int strip_selected_impulse = -1;  // last row tapped: the only "current" we can know here
static qbool hop_latched, hop_cancelling;
static double hop_press_time;
static double tapfire_until;
static qbool was_active;

// look
static touchlook_t look_state;   // touch_feel.h
static qbool look_active;
static double look_prev_time;

// Touch holds a button under its own key number, as a key would. A bare
// -forward clears every holder, so letting go of the stick used to drop a W
// still held on the keyboard too. Real keys stop below 0xAC00
// (MAX_KEY_BINDS); the port's CSQC uses 50001 (TOUCH_HOLD_KEY).
#define TOUCH_HUD_HOLD_KEY 50000

// Run a game command now, not at the next frame's turn of the command buffer:
// this runs inside IN_Move, before the frame's move is built, so a press goes
// out in this frame's move rather than the next (one frame of input lag, 33 ms
// at the thermal profile's 30 fps). Only for commands that just set input
// state (buttons, impulses); anything that opens a screen keeps the buffer.
static void exec_now(const char *text)
{
	Cmd_ExecuteString(cmd_local, text, strlen(text), src_local, false); // maybe inside a buffered command already: no lock
}

static void set_key(const char *name, qbool *state, qbool down)
{
	char buf[64];
	if (*state == down)
		return;
	*state = down;
	dpsnprintf(buf, sizeof(buf), "%s%s %d", down ? "+" : "-", name, TOUCH_HUD_HOLD_KEY);
	exec_now(buf);
}

static void spec_release(void);

// The contacts the finger table still holds when the app has been away: their
// releases never come, and TouchHUD_ReleaseAll alone let the next frame claim
// them again as new fingers (a held hook, grenade or FIRE stuck on).
void TouchHUD_DropContacts(void)
{
	int i;
	for (i = 0; i < TOUCHHUD_ENGINE_FINGERS - 1; i++)
		multitouch[i][0] = 0;
}

void TouchHUD_ReleaseAll(void)
{
	int i;
	spec_release();
	set_key("forward", &key_forward, false);
	set_key("back", &key_back, false);
	set_key("moveleft", &key_left, false);
	set_key("moveright", &key_right, false);
	set_key("attack", &key_attack, false);
	set_key("attack2", &key_attack2, false);
	set_key("jump", &key_jump, false);
	set_key("crouch", &key_crouch, false);
	set_key("showscores", &key_scores, false);
	set_key("zoom", &key_zoom, false);
	TouchHook_SetHudButton(false);
	TouchNade_SetHudButton(false);
	hop_latched = false;
	hop_cancelling = false;
	tapfire_until = 0;
	TouchAim_SetHudAimTouch(false);
	move_off_x = move_off_y = 0;
	stick_held = false;
	stick_axis_x = stick_axis_y = 0;
	look_active = false;
	TouchLook_Reset(&look_state);
	for (i = 0; i < MAX_FINGERS; i++)
		fingers[i].used = false;
}

// ---------------------------------------------------------------------------
// First-person weapon model. touch/xonotic.cfg and the thermal profile every
// launch runs used to force r_drawviewmodel 0, so every install archived it,
// and the launcher runs config.cfg after the shipped defaults: dropping those
// lines alone would never bring the arm and weapon back to anyone who had
// played before. Turned on once, after every startup config has run; from
// then on the archived value is the player's own choice.
//
// Revision 2: a device that scripts/dev-deploy.sh had written profiles to kept
// the old thermal.cfg in the engine userdir, above the package's, and it
// switched the model off again after revision 1. The launcher now removes
// those copies (quarantine_stale_dev_overlays), and this turns it on once more.
// ---------------------------------------------------------------------------
#define TOUCH_VIEWMODEL_REV 2
static cvar_t touch_viewmodel_restored = {CF_CLIENT | CF_ARCHIVE, "_touch_viewmodel_restored", "0", "revision of the one-time r_drawviewmodel restore already applied (see touch_hud.c)"};

// The weapon strip's size (see strip_layout).
static cvar_t cl_touch_weplist_scale = {CF_CLIENT | CF_ARCHIVE, "cl_touch_weplist_scale", "1.3",
	"Touch controls: size of the weapon strip, 0.7 - 2.4 (also set by dragging the grip under the strip)"};

static cvar_t touch_topbar_auto = {CF_CLIENT | CF_ARCHIVE, "touch_topbar_auto", "1", "Godnotic: 1 lays out the in-match top bar (console button, MENU, SCORE, CHAT, vote chip) in the top corner in dp, clear of each other and of the game's own score list, messages and clock; 0 leaves touch_pause_x/y, touch_scores_x/y and touch_chat_x/y where the layout editor put them"};
// Haptics: a short tick when a button takes a finger. The platform says it
// can vibrate by setting TouchLua_VibrateHook (touch_lua.h); until one does,
// _touch_haptics_available is 0 and the setting is not offered.
static cvar_t touch_haptics = {CF_CLIENT | CF_ARCHIVE, "touch_haptics", "1", "Godnotic: a short vibration when a touch button is pressed, on a device whose app can vibrate (_touch_haptics_available)"};
static cvar_t touch_haptics_ms = {CF_CLIENT | CF_ARCHIVE, "touch_haptics_ms", "8", "length of the button-press vibration in milliseconds"};
static cvar_t touch_haptics_available = {CF_CLIENT | CF_READONLY, "_touch_haptics_available", "0", "1 when the platform gave the engine a way to vibrate; read-only"};
extern qbool (*TouchLua_VibrateHook)(int ms);
static void haptic_tick(void)
{
	if (touch_haptics.integer && TouchLua_VibrateHook)
		TouchLua_VibrateHook(bound(1, touch_haptics_ms.integer, 50));
}
static void TouchHaptic_f(cmd_state_t *cmd)
{
	haptic_tick();
}
// touch_move <x> <y>: the port's CSQC HUD hands its stick over (x right, y
// down, length 0..1; "touch_move 0 0" when the thumb lifts).
static void TouchMove_f(cmd_state_t *cmd)
{
	float x = (float)atof(Cmd_Argv(cmd, 1)), y = (float)atof(Cmd_Argv(cmd, 2));
	float mag = sqrt(x * x + y * y);
	if (mag > 1)
	{
		x /= mag;
		y /= mag;
	}
	csqc_axis_x = x;
	csqc_axis_y = y;
}

// Called by CL_Input after the movement keys: adds the stick's analog move.
// Full deflection is exactly what the keys give (cl_forwardspeed and the
// rest), so top speed, strafing and bunny-hopping are the keyboard's; the
// angle in between is the thumb's.
void TouchHUD_MoveAxes(float *forwardmove, float *sidemove)
{
	float x, y;
	if (key_dest != key_game || (key_consoleactive & KEY_CONSOLEACTIVE_USER) || cls.state != ca_connected)
	{
		csqc_axis_x = csqc_axis_y = 0;
		return;
	}
	if (cl_touch_csqc_active)
	{
		x = csqc_axis_x;
		y = csqc_axis_y;
	}
	else
	{
		csqc_axis_x = csqc_axis_y = 0;
		if (!was_active)
			return;
		x = stick_axis_x;
		y = stick_axis_y;
	}
	if (x == 0 && y == 0)
		return;
	*forwardmove += (y < 0 ? cl_forwardspeed.value : cl_backspeed.value) * -y;
	*sidemove += cl_sidespeed.value * x;
}

static void TouchHUD_RestoreViewmodel_f(cmd_state_t *cmd)
{
	if (touch_viewmodel_restored.integer >= TOUCH_VIEWMODEL_REV)
		return;
	Cvar_SetValueQuick(&touch_viewmodel_restored, TOUCH_VIEWMODEL_REV);
	if (r_drawviewmodel.integer)
		return;
	Cvar_SetValueQuick(&r_drawviewmodel, 1);
	Con_Printf("Touch: first-person weapon model turned back on (r_drawviewmodel 1)\n");
}

static void TouchHUD_CancelJoin_f(cmd_state_t *cmd);

// Test hooks for the live input tests (tests/touch-detection/match_live.py):
// a finger without a touchscreen, and the view angles to read back.
static void TouchHUD_TestFinger_f(cmd_state_t *cmd)
{
	int slot = Cmd_Argc(cmd) > 1 ? atoi(Cmd_Argv(cmd, 1)) : 0;
	if (Cmd_Argc(cmd) < 3 || slot < 0 || slot >= TOUCHHUD_ENGINE_FINGERS - 1)
	{
		Con_Print("usage: _touch_test_finger <slot> <0|1 down> [x y]  (x, y 0..1)\n");
		return;
	}
	// the overlays see a finger first, as from vid_sdl.c
	if (TouchHUD_OverlayFinger(atoi(Cmd_Argv(cmd, 2)) ? 0 : 2, slot + 1, Cmd_Argc(cmd) > 4 ? atof(Cmd_Argv(cmd, 3)) : 0, Cmd_Argc(cmd) > 4 ? atof(Cmd_Argv(cmd, 4)) : 0))
		return;
	multitouch[slot][0] = atoi(Cmd_Argv(cmd, 2)) ? slot + 1 : 0;
	if (Cmd_Argc(cmd) > 4)
	{
		multitouch[slot][1] = atof(Cmd_Argv(cmd, 3));
		multitouch[slot][2] = atof(Cmd_Argv(cmd, 4));
	}
}

static void TouchHUD_TestStrip_f(cmd_state_t *cmd);

// Test hook: what the app leaving does to the fingers (touch_android.c).
static void TouchHUD_TestAway_f(cmd_state_t *cmd)
{
	TouchHUD_ReleaseAll();
	TouchHUD_DropContacts();
}

static void TouchHUD_TestView_f(cmd_state_t *cmd)
{
	Con_Printf("TESTVIEW yaw=%.2f pitch=%.2f\n", cl.viewangles[YAW], cl.viewangles[PITCH]);
}

static cvar_t touch_spectate_inputs = {CF_CLIENT | CF_ARCHIVE, "touch_spectate_inputs", "1", "show the followed player's inputs"};
static cvar_t touch_spectate_view = {CF_CLIENT | CF_ARCHIVE, "touch_spectate_view", "0", "view of the followed player: 0 first person, 1 third person, 2 front, 3 top down"};
static cvar_t touch_spectate_view_saved = {CF_CLIENT | CF_ARCHIVE, "touch_spectate_view_saved", "", "internal: the chase camera settings to put back after spectating"};
static void TouchHUD_SpectateView_f(cmd_state_t *cmd);
static int spec_test_mask = -1, spec_test_keyboard = -1;
static unsigned int spec_draw_count;
static qbool spec_inputs_mask(int *mask)
{
	prvm_prog_t *prog = CLVM_prog;
	mdef_t *g;
	int state = TouchUI_SpectatorState();
	if (!touch_spectate_inputs.integer || (state != 2 && state != 3))
		return false;
	g = prog && prog->loaded ? PRVM_ED_FindGlobal(prog, "g_stat_PRESSED_KEYS") : NULL;
	if (!g || prog->numglobals <= 0 || g->ofs >= (unsigned int)prog->numglobals)
		return false;
	*mask = (g->type & ~DEF_SAVEGLOBAL) == ev_float ? (int)PRVM_GLOBALFIELDFLOAT(g->ofs) : PRVM_GLOBALFIELDVALUE(g->ofs)->_int;
	return true;
}

static void TouchHUD_TestInputs_f(cmd_state_t *cmd)
{
	int mask = -1;
	if (Cmd_Argc(cmd) > 1)
	{
		spec_test_mask = atoi(Cmd_Argv(cmd, 1));
		spec_test_keyboard = Cmd_Argc(cmd) > 2 ? atoi(Cmd_Argv(cmd, 2)) : -1;
	}
	spec_inputs_mask(&mask);
	Con_Printf("SPECTATE_INPUTS status=%d live=%d injected=%d keyboard=%d draws=%u\n", TouchUI_SpectateeStatus(), mask, spec_test_mask, spec_test_keyboard, spec_draw_count);
}

static char spec_saved_panel[128];
static cvar_t *spec_panel;
void TouchHUD_SpecInputsBegin(void)
{
	int mask;
	spec_panel = NULL;
	if (!spec_inputs_mask(&mask))
	{
		// never the stock arrows box beside touch controls
		int state = TouchUI_SpectatorState();
		if ((state != 2 && state != 3) || vid_keyboard_detected.integer || !(vid_touchscreen.integer || vid_touchscreen_detected.integer))
			return;
	}
	spec_panel = Cvar_FindVar(&cvars_all, "hud_panel_pressedkeys", ~0);
	if (spec_panel)
	{
		dp_strlcpy(spec_saved_panel, spec_panel->string, sizeof(spec_saved_panel));
		Cvar_SetQuick(spec_panel, "0");
	}
}
void TouchHUD_SpecInputsEnd(void)
{
	if (spec_panel)
	{
		Cvar_SetQuick(spec_panel, spec_saved_panel);
		spec_panel = NULL;
	}
}

void TouchHUD_Init(void)
{
	TouchHudMode_Init();
	Cvar_RegisterVariable(&touch_spectate_inputs);
	Cvar_RegisterVariable(&touch_spectate_view);
	Cvar_RegisterVariable(&touch_spectate_view_saved);
	Cmd_AddCommand(CF_CLIENT, "touch_spectate_view_step", TouchHUD_SpectateView_f, "view of the followed player: next or prev (no argument prints it)");
	Cmd_AddCommand(CF_CLIENT, "_touch_test_spectate_inputs", TouchHUD_TestInputs_f, "test hook: mask and keyboard override, -1 disables");
	Cvar_RegisterVariable(&touch_viewmodel_restored);
	Cvar_RegisterVariable(&cl_touch_weplist_scale);
	Cvar_RegisterVariable(&touch_topbar_auto);
	Cvar_RegisterVariable(&touch_move_analog_cap);
	Cvar_RegisterVariable(&touch_haptics);
	Cvar_RegisterVariable(&touch_haptics_ms);
	Cvar_RegisterVariable(&touch_haptics_available);
	Cmd_AddCommand(CF_CLIENT, "touch_haptic", TouchHaptic_f, "a button-press vibration, if touch_haptics is on and the device can (the CSQC HUD asks for it)");
	Cmd_AddCommand(CF_CLIENT, "touch_move", TouchMove_f, "the touch stick's analog move from the CSQC HUD: touch_move <x right> <y down>, length 0..1");
	Cmd_AddCommand(CF_CLIENT, "_touch_cancel_join", TouchHUD_CancelJoin_f, "stop a connect attempt (CANCEL on the loading screen)");
	Cmd_AddCommand(CF_CLIENT, "_touch_test_finger", TouchHUD_TestFinger_f, "test hook: _touch_test_finger <slot> <0|1> [x y] puts a finger down or up");
	Cmd_AddCommand(CF_CLIENT, "_touch_test_away", TouchHUD_TestAway_f, "test hook: release every finger as the app going to the background does");
	Cmd_AddCommand(CF_CLIENT, "_touch_test_strip", TouchHUD_TestStrip_f, "test hook: print where the weapon strip's HOOK and NADE bands are (0..1)");
	Cmd_AddCommand(CF_CLIENT, "_touch_test_view", TouchHUD_TestView_f, "test hook: print the view angles");
	Cmd_AddCommand(CF_CLIENT, "_touch_restore_viewmodel", TouchHUD_RestoreViewmodel_f, "turn r_drawviewmodel back on once for installs that archived the old forced 0 (queued by Host_Init)");
}

// ---------------------------------------------------------------------------
// Stock panels. The touch readouts (vitals, weapon strip) stand in for the
// stock healtharmor / ammo / weapons panels only while they draw, which is
// while the touch controls are on. Decided here every frame because this is
// the one place that runs under both CSQCs: the port's own, and a server's,
// which knows nothing of touch and kept whatever the port last left in these
// archived cvars. The port's CSQC used to decide on touch_mobile_hud alone and
// stopped looking once a keyboard hid the controls, so a keyboard player had
// no health, armour, ammo or weapons in a local match or on a server.
// ---------------------------------------------------------------------------
// The vitals and ammo are for the touch controls. The weapon strip is not:
// with a keyboard and a mouse at the desk it stays in view, as long as a CSQC
// is there to draw the HUD (the port's, or a server's).
static int stat_weapons = -1;
static qbool readouts_shown(void)
{
	return vid_touchscreen.integer && cv("touch_mobile_hud", 1);
}

// The touch strip replaces the stock weapons panel only when it has rows: a
// CSQC whose weapon list could not be read, or a player who owns nothing,
// keeps the stock panel.
static int owned_weapon_count(void);
static qbool strip_shown(void)
{
	return CLVM_prog->loaded && cv("touch_mobile_hud", 1) && cv("touch_weplist_visible", 1)
		&& stat_weapons >= 0 && owned_weapon_count() > 0
		// spectating, the weapons are the followed player's: the stock panel
		// shows them, and a row tap would send an impulse to the spectator code
		&& !TouchUI_IsSpectating();
}

static void stock_panel(const char *name, qbool on)
{
	cvar_t *var = Cvar_FindVar(&cvars_all, name, ~0);
	if (var && (var->integer != 0) != on)
		Cvar_SetValueQuick(var, on);
}

// ---------------------------------------------------------------------------
// The top bar (touch_topbar.h): the chrome row laid out in dp from the top
// corner, written into the pills' own layout cvars (touch_pause_*,
// touch_scores_*, touch_chat_*) so both HUD copies, the layout editor and
// the hit tests see one answer; the console button and the vote chip ask
// TouchHUD_TopBarRect. touch_topbar_auto 0 leaves the cvars to the player
// (a hand-placed top bar).
// ---------------------------------------------------------------------------
static qbool in_play(void);
static touchbar_t topbar;
static qbool topbar_valid;

static void layout_cvar(const char *name, float value)
{
	cvar_t *var = Cvar_FindVar(&cvars_all, name, ~0);
	char text[32];
	if (var && fabs(var->value - value) < 0.00000001f)
		return;
	dpsnprintf(text, sizeof(text), "%.9f", value);
	if (var)
		Cvar_SetQuick(var, text);
	else
		Cvar_Get(&cvars_all, name, text, CF_CLIENT, NULL);
}

static void panel_cvar(const char *name, const char *value)
{
	cvar_t *var = Cvar_FindVar(&cvars_all, name, ~0);
	if (var && strcmp(var->string, value))
		Cvar_SetQuick(var, value);
}

static void panel_place(const char *panel, float x, float y, float w, float h)
{
	char name[64], value[48];
	dpsnprintf(name, sizeof(name), "hud_panel_%s_pos", panel);
	dpsnprintf(value, sizeof(value), "%.3f %.3f", x, y);
	panel_cvar(name, value);
	dpsnprintf(name, sizeof(name), "hud_panel_%s_size", panel);
	dpsnprintf(value, sizeof(value), "%.3f %.3f", w, h);
	panel_cvar(name, value);
}

// A saved, hand-placed top bar can hold positions from an older default and put
// two pills on top of each other. Asked once per frame while the automatic row
// is off: if they overlap, the row takes over and says so.
static qbool topbar_user_overlaps(void)
{
	static const char *pill[3] = { "touch_pause", "touch_scores", "touch_chat" };
	touchbar_rect_t r[3];
	char name[48];
	int i, j, n = 0;
	for (i = 0; i < 3; i++)
	{
		float hx, hy, size, aspect;
		dpsnprintf(name, sizeof(name), "%s_visible", pill[i]);
		if (cv(name, 1) == 0)
			continue;
		dpsnprintf(name, sizeof(name), "%s_size", pill[i]);
		size = cv(name, 0.060f);
		dpsnprintf(name, sizeof(name), "%s_aspect", pill[i]);
		aspect = cv(name, 2.4f);
		widget_half(size, aspect, &hx, &hy);
		dpsnprintf(name, sizeof(name), "%s_x", pill[i]);
		r[n].x = cv(name, 0.5f) * ui_w() - hx;
		dpsnprintf(name, sizeof(name), "%s_y", pill[i]);
		r[n].y = cv(name, 0.5f) * ui_h() - hy;
		r[n].w = hx * 2;
		r[n].h = hy * 2;
		n++;
	}
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (TouchTopBar_Intersect(&r[i], &r[j]))
				return true;
	return false;
}

static void topbar_sync(void)
{
	touchbar_in_t in;
	float in_l = 0, in_t = 0, in_r = 0, in_b = 0, dens = 0, hx, hy, fb, W = ui_w(), H = ui_h();
	float px2con = vid.mode.width > 0 ? W / (float)vid.mode.width : 1.0f;
	const char *insets = Cvar_VariableString(&cvars_all, "_touch_safe_insets", ~0);
	qbool flip = cv("touch_handedness", 0) != 0;
	topbar_valid = false;
	if ((TouchLua_VibrateHook != NULL) != (touch_haptics_available.integer != 0))
		Cvar_SetValueQuick(&touch_haptics_available, TouchLua_VibrateHook != NULL);
	if (!touch_topbar_auto.integer && (vid_touchscreen.integer || vid_touchscreen_detected.integer) && cls.state == ca_connected && W >= 16 && H >= 16
		&& topbar_user_overlaps())
	{
		Con_Printf("Touch HUD: the saved top bar had overlapping buttons, using the automatic row\n");
		Cvar_SetValueQuick(&touch_topbar_auto, 1);
	}
	if ((!touch_topbar_auto.integer && !TouchMode_IsMatchEnd(TouchHudMode_Current()->mode)) || !(vid_touchscreen.integer || vid_touchscreen_detected.integer) || cls.state != ca_connected || W < 16 || H < 16)
	{
		layout_cvar("_touch_topbar_active", 0);
		return;
	}
	if (insets && insets[0])
		sscanf(insets, "%f %f %f %f %f", &in_l, &in_t, &in_r, &in_b, &dens);
	memset(&in, 0, sizeof(in));
	in.w = W;
	in.h = H;
	in.u = dp_units();
	in.in_l = in_l * px2con;
	in.in_t = in_t * px2con;
	in.in_r = in_r * px2con;
	// Use the larger of the engine and CSQC density floors for each saved size.
	{
		float floor_h = TOUCHDP_PILL_HEIGHT * in.u;
		float vw = cv("vid_width", 0), vh = cv("vid_height", 0);
		if (vw > 0 && vh > 0)
			floor_h = max(floor_h, TOUCHDP_PILL_HEIGHT * TouchDp_Density(cv("vid_touchscreen_ydpi", 0), min(vw, vh)) * H / vh);
		widget_half(cv("touch_pause_size", 0.060f), 1, &hx, &hy); in.menu_h = max(hy * 2, floor_h); in.menu_w = in.menu_h * max(1.0f, cv("touch_pause_aspect", 2.4f));
		widget_half(cv("touch_scores_size", 0.060f), 1, &hx, &hy); in.score_h = max(hy * 2, floor_h); in.score_w = in.score_h * max(1.0f, cv("touch_scores_aspect", 2.4f));
		widget_half(cv("touch_chat_size", 0.060f), 1, &hx, &hy); in.chat_h = max(hy * 2, floor_h); in.chat_w = in.chat_h * max(1.0f, cv("touch_chat_aspect", 2.4f));
	}
	in.aspect = 2.4f;
	in.console = Cvar_VariableValue(&cvars_all, "touch_shell_button", ~0) >= 2
		? bound(32.0f, Cvar_VariableValue(&cvars_all, "touch_shell_button_size", ~0), 96.0f) * in.u : 0;
	in.menu = cv("touch_pause_visible", 1) != 0;
	in.score = cv("touch_scores_visible", 1) != 0;
	in.chat = cv("touch_chat_visible", 1) != 0;
	in.left_handed = flip;
	TouchTopBar_Layout(&in, &topbar);
	topbar_valid = true;
	layout_cvar("_touch_topbar_active", 1);
	// the pills' own cvars: centres as fractions, before the left-handed mirror
#define BAR_CVARS(rect, prefix) \
	if ((rect).w > 0) \
	{ \
		float nx = ((rect).x + (rect).w * 0.5f) / W; \
		layout_cvar(prefix "_x", flip ? 1 - nx : nx); \
		layout_cvar(prefix "_y", ((rect).y + (rect).h * 0.5f) / H); \
		layout_cvar(prefix "_bar_w", (rect).w / W); \
		layout_cvar(prefix "_bar_h", (rect).h / H); \
	}
	BAR_CVARS(topbar.menu, "touch_pause")
	BAR_CVARS(topbar.score, "touch_scores")
	BAR_CVARS(topbar.chat, "touch_chat")
#undef BAR_CVARS
	// The game's own panels, out from under the row (right-handed layout; a
	// left-handed one keeps the stock places, where the corner is free).
	if (flip)
		return;
	fb = topbar.bottom / H;
	panel_place("score", 0.880f, fb, 0.115f, 0.080f);
	panel_place("infomessages", 0.590f, fb, 0.280f, 0.080f);
	panel_place("notify", 0.520f, fb + 0.090f, 0.380f, 0.190f);
	panel_cvar("hud_panel_notify_flip", "1");
	panel_place("timer", 0.440f, 0.004f, 0.120f, 0.052f);
	panel_place("modicons", 0.430f, 0.060f, 0.140f, 0.060f);
	panel_place("vote", 0.400f, 0.125f, 0.200f, 0.085f);
	panel_place("radar", 0.280f, 0.015f, 0.140f, 0.200f);
	panel_place("chat", 0.020f, 0.230f, 0.290f, 0.190f);
	// item timers (spectators by default): off the chat feed, on the right
	// edge under the scores, where a spectator has no weapon strip
	panel_place("itemstime", 0.925f, fb + 0.095f, 0.065f, 0.230f);
}

// The console button's and the vote chip's place in the row, console units.
// False: no row is laid out now (touch_topbar_auto 0, controls off, match end).
qbool TouchHUD_TopBarRect(int slot, float *x, float *y, float *w, float *h)
{
	const touchbar_rect_t *r = slot == 0 ? &topbar.console : &topbar.chip;
	if (!topbar_valid || r->w <= 0 || !in_play())
		return false;
	*x = r->x; *y = r->y; *w = r->w; *h = r->h;
	return true;
}

void TouchHUD_SyncStockPanels(void)
{
	qbool stock = !readouts_shown();
	stock_panel("hud_panel_healtharmor", stock);
	stock_panel("hud_panel_ammo", stock);
	stock_panel("hud_panel_weapons", !strip_shown());
	topbar_sync();
}

// In a match with the game holding the keys, under any CSQC.
static qbool in_play(void)
{
	if (!CLVM_prog->loaded)
		return false;
	if (cls.state != ca_connected || cls.signon != SIGNONS)
		return false;
	if (key_dest != key_game || (key_consoleactive & KEY_CONSOLEACTIVE_USER))
		return false;
	return true;
}

// The loaded CSQC shows its own pointer (setcursormode): the map and
// gametype vote, the quick menu, minigames, hud_configure.
static qbool cursor_wanted(void)
{
	return in_play() && cl.csqc_wantsmousemove;
}

static qbool touch_device(void)
{
	return VID_SDL_HasTouchDevices() || vid_touchscreen_detected.integer;
}

// Controls off (a keyboard and a mouse are attached, or Keyboard & mouse
// mode) on a touchscreen, under a server's CSQC: this module keeps the weapon
// strip, its grip and the chrome pills on screen and tappable, and every
// other finger turns the view. The port's own CSQC does the same locally.
static qbool desk_active(void)
{
	return !vid_touchscreen.integer && !cl_touch_csqc_active && touch_device()
		&& in_play() && !cl.csqc_wantsmousemove && !TouchMode_IsMatchEnd(TouchHudMode_Current()->mode);
}

static qbool desk_pills(void)
{
	return cv("touch_desk_pills", 1) != 0;
}

qbool TouchHUD_Active(void)
{
	if (!vid_touchscreen.integer || cl_touch_csqc_active)
		return false;
	// The CSQC's pointer UI takes the fingers (TouchHUD_CursorFrame), as the
	// port's own HUD steps aside for it.
	// Match end (scoreboard, next-map vote): draw_endmenu owns the top bar.
	// The in-match menu is an overlay on the live game: it owns the screen,
	// so the touch controls step aside while it is open.
	return key_dest == key_game && in_play() && !cl.csqc_wantsmousemove
		&& !TouchMode_IsMatchEnd(TouchHudMode_Current()->mode);
}

// ---------------------------------------------------------------------------
// Weapons: read from the CSQC the server sent, not from a table. Xonotic sorts
// both its weapons registry and its stats registry by name before numbering
// them (REGISTRY_SORT), so a weapon's id -- its WepSet bit -- and the index of
// the WEAPONS stat differ from build to build: STAT_WEAPONS is 32 in this
// port's own data and 164 on one public server. Whatever CSQC is loaded holds
// the answer in its globals: STAT_WEAPONS.m_id, and the weapon list from
// Weapons_first along .enemy (REGISTRY_NEXT), each with .m_id, .impulse,
// .netname and .model2 (the HUD icon). Bound again on every CSQC load.
// ---------------------------------------------------------------------------
#define MAX_WEAPONS 72
// ammo_stat: the stat holding this weapon's reserve (-1 = none: infinite).
typedef struct wepdef_s { char name[32]; char pic[64]; char ammo_icon[32]; int impulse; int id; int ammo_stat; } wepdef_t;
static wepdef_t weapons[MAX_WEAPONS];
static int num_weapons = 0;
// STAT_TOUCH_ALTFIRE when the server's CSQC registers it (this port's game
// code), -1 on any other server: touch_altfire.h then judges from the stock
// balance.
static int stat_altfire = -1;
static float fire2_fade, zoom_fade;
// The CSQC's viewmodels[0] (the wepent) tells the held weapon and its clip,
// which no stat does: global offset plus the fields read from it each frame.
typedef struct qcfield_s { int ofs; int type; } qcfield_t;
static int viewmodels_ofs = -1;
static qcfield_t f_switchweapon, f_activeweapon, f_clip_load, f_clip_size, f_wep_id;

static qcfield_t qcfield(prvm_prog_t *prog, const char *name)
{
	qcfield_t f = {-1, 0};
	mdef_t *d = PRVM_ED_FindField(prog, name);
	if (d)
	{
		f.ofs = d->ofs;
		f.type = d->type & ~DEF_SAVEGLOBAL;
	}
	return f;
}

static int qcfield_int(prvm_prog_t *prog, prvm_edict_t *ed, qcfield_t f)
{
	prvm_eval_t *v;
	if (f.ofs < 0)
		return 0;
	v = PRVM_EDICTFIELDVALUE(ed, f.ofs);
	return f.type == ev_float ? (int)v->_float : v->_int;
}

static int def_type(const mdef_t *d)
{
	return d->type & ~DEF_SAVEGLOBAL;
}

// A field as an int, whether the QC declared it int or float.
static int field_int(prvm_prog_t *prog, prvm_edict_t *ed, const mdef_t *f)
{
	prvm_eval_t *v = PRVM_EDICTFIELDVALUE(ed, f->ofs);
	return def_type(f) == ev_float ? (int)v->_float : v->_int;
}

static const char *field_string(prvm_prog_t *prog, prvm_edict_t *ed, const mdef_t *f)
{
	return f ? PRVM_GetString(prog, PRVM_EDICTFIELDVALUE(ed, f->ofs)->string) : "";
}

static prvm_edict_t *edict_or_null(prvm_prog_t *prog, int n)
{
	if (n <= 0 || n >= prog->num_edicts)
		return NULL;
	return PRVM_EDICT_NUM(n);
}

static prvm_edict_t *global_edict(prvm_prog_t *prog, const char *name)
{
	mdef_t *g = PRVM_ED_FindGlobal(prog, name);
	if (!g || def_type(g) != ev_entity)
		return NULL;
	return edict_or_null(prog, PRVM_GLOBALFIELDEDICT(g->ofs));
}

static void bind_weapons(void)
{
	prvm_prog_t *prog = CLVM_prog;
	mdef_t *f_id, *f_impulse, *f_pic, *f_name, *f_next;
	prvm_edict_t *ed;
	int guard;
	mdef_t *f_ammo_type, *f_ammo_icon, *g_viewmodels;
	int res_none = 0, stat_fuel = -1, stat_plasma = -1;
	num_weapons = 0;
	stat_weapons = -1;
	stat_altfire = -1;
	viewmodels_ofs = -1;
	strip_selected_impulse = -1;
	if (!prog->loaded)
		return;
	f_id = PRVM_ED_FindField(prog, "m_id");
	f_impulse = PRVM_ED_FindField(prog, "impulse");
	f_next = PRVM_ED_FindField(prog, "enemy");
	f_pic = PRVM_ED_FindField(prog, "model2");
	f_name = PRVM_ED_FindField(prog, "netname");
	f_ammo_type = PRVM_ED_FindField(prog, "ammo_type");
	f_ammo_icon = PRVM_ED_FindField(prog, "m_icon");
	if (!f_id || !f_impulse || !f_next)
		return;
	ed = global_edict(prog, "STAT_WEAPONS");
	if (ed)
	{
		stat_weapons = field_int(prog, ed, f_id);
		if (stat_weapons < 0 || stat_weapons + 2 >= MAX_CL_STATS)
			stat_weapons = -1;
	}
	ed = global_edict(prog, "STAT_TOUCH_ALTFIRE");
	if (ed)
	{
		stat_altfire = field_int(prog, ed, f_id);
		if (stat_altfire < 0 || stat_altfire >= MAX_CL_STATS)
			stat_altfire = -1;
	}
	// GetAmmoStat: shells/bullets/rockets/cells are engine stats, fuel and
	// plasma registered ones; RES_NONE is a weapon that uses no ammo.
	ed = global_edict(prog, "STAT_FUEL");
	if (ed)
		stat_fuel = field_int(prog, ed, f_id);
	ed = global_edict(prog, "STAT_PLASMA");
	if (ed)
		stat_plasma = field_int(prog, ed, f_id);
	ed = global_edict(prog, "RES_NONE");
	if (ed)
		res_none = PRVM_NUM_FOR_EDICT(ed);
	g_viewmodels = PRVM_ED_FindGlobal(prog, "viewmodels");
	if (g_viewmodels && def_type(g_viewmodels) == ev_entity)
		viewmodels_ofs = g_viewmodels->ofs;
	f_switchweapon = qcfield(prog, "switchweapon");
	f_activeweapon = qcfield(prog, "activeweapon");
	f_clip_load = qcfield(prog, "clip_load");
	f_clip_size = qcfield(prog, "clip_size");
	f_wep_id = qcfield(prog, "m_id");
	// The registry's linked list, in the order the CSQC's own FOREACH walks it.
	for (ed = global_edict(prog, "Weapons_first"), guard = 0; ed && guard < 256 && num_weapons < MAX_WEAPONS; guard++)
	{
		int id = field_int(prog, ed, f_id);
		int impulse = field_int(prog, ed, f_impulse);
		if (id > 0 && impulse >= 0)
		{
			wepdef_t *wd = &weapons[num_weapons++];
			wd->id = id;
			wd->impulse = impulse;
			dp_strlcpy(wd->name, field_string(prog, ed, f_name), sizeof(wd->name));
			dp_strlcpy(wd->pic, field_string(prog, ed, f_pic), sizeof(wd->pic));
			wd->ammo_stat = -1;
			wd->ammo_icon[0] = 0;
			if (f_ammo_type)
			{
				int res = PRVM_EDICTFIELDEDICT(ed, f_ammo_type->ofs);
				prvm_edict_t *re = (res != res_none) ? edict_or_null(prog, res) : NULL;
				if (re)
				{
					const char *kind = field_string(prog, re, f_name);
					if (!strcmp(kind, "shells")) wd->ammo_stat = STAT_SHELLS;
					else if (!strcmp(kind, "bullets")) wd->ammo_stat = STAT_NAILS;
					else if (!strcmp(kind, "rockets")) wd->ammo_stat = STAT_ROCKETS;
					else if (!strcmp(kind, "cells")) wd->ammo_stat = STAT_CELLS;
					else if (!strcmp(kind, "fuel")) wd->ammo_stat = stat_fuel;
					else if (!strcmp(kind, "plasma")) wd->ammo_stat = stat_plasma;
					if (wd->ammo_stat >= MAX_CL_STATS)
						wd->ammo_stat = -1;
					dp_strlcpy(wd->ammo_icon, field_string(prog, re, f_ammo_icon), sizeof(wd->ammo_icon));
				}
			}
		}
		ed = edict_or_null(prog, PRVM_EDICTFIELDEDICT(ed, f_next->ofs));
	}
	if (!cl_touch_csqc_active)
		Con_Printf("Touch HUD: %d weapons from the server's CSQC, WEAPONS stat %d\n", num_weapons, stat_weapons);
	else
		Con_DPrintf("Touch HUD: %d weapons from the local CSQC, WEAPONS stat %d\n", num_weapons, stat_weapons);
}

void TouchHUD_ProgsChanged(void)
{
	bind_weapons();
}

// The wepent (viewmodels[0]) of the loaded CSQC, or NULL.
static prvm_edict_t *wepent(void)
{
	prvm_prog_t *prog = CLVM_prog;
	if (viewmodels_ofs < 0 || !prog->loaded)
		return NULL;
	return edict_or_null(prog, PRVM_GLOBALFIELDEDICT(viewmodels_ofs));
}

// Index in weapons[] of the weapon being switched to (held, once the switch
// lands), like the CSQC's wepent.switchweapon; -1 when the CSQC does not say.
static int held_weapon_index(void)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_edict_t *ve = wepent(), *we = NULL;
	int id, i;
	if (!ve)
		return -1;
	if (f_switchweapon.ofs >= 0)
		we = edict_or_null(prog, PRVM_EDICTFIELDEDICT(ve, f_switchweapon.ofs));
	if (!we && f_activeweapon.ofs >= 0)
		we = edict_or_null(prog, PRVM_EDICTFIELDEDICT(ve, f_activeweapon.ofs));
	if (!we)
		return -1;
	id = qcfield_int(prog, we, f_wep_id);
	for (i = 0; i < num_weapons; i++)
		if (weapons[i].id == id)
			return i;
	return -1;
}

// WepSet_FromWeapon: bit id-1 of the first stat for ids 1..24, then the next two.
static qbool weapon_owned(int index)
{
	int bit = weapons[index].id - 1;
	if (stat_weapons < 0 || bit < 0)
		return false;
	if (bit < 24)
		return (cl.stats[stat_weapons] & (1 << bit)) != 0;
	if (bit < 48)
		return (cl.stats[stat_weapons + 1] & (1 << (bit - 24))) != 0;
	return (cl.stats[stat_weapons + 2] & (1 << (bit - 48))) != 0;
}

// What +fire2 would do now (TOUCH_ALTFIRE_*): the server's stat when it sends
// one, else judged from the held weapon and its ammo (touch_altfire.h).
static int altfire_state(void)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_edict_t *ve;
	const wepdef_t *wd;
	int held, reserve = 0, clip = -1;
	if (stat_altfire >= 0)
		return cl.stats[stat_altfire];
	// A spectator's +fire2 goes back to observing, as the stat would say.
	if (TouchUI_IsSpectating())
		return TouchUI_IsWatching() ? TOUCH_ALTFIRE_READY : TOUCH_ALTFIRE_NONE;
	if (cl.stats[STAT_HEALTH] <= 0)
		return TOUCH_ALTFIRE_NONE;
	held = held_weapon_index();
	if (held < 0)
		return TOUCH_ALTFIRE_NONE;
	wd = &weapons[held];
	if (wd->ammo_stat >= 0)
		reserve = max(0, cl.stats[wd->ammo_stat]);
	ve = wepent();
	if (ve && qcfield_int(prog, ve, f_clip_size) > 0)
		clip = qcfield_int(prog, ve, f_clip_load);
	return TouchAltFire_Fallback(wd->name, reserve, clip,
		(cl.stats[STAT_ITEMS] & 1) || wd->ammo_stat < 0); // IT_UNLIMITED_AMMO
}

static qbool fire2_available(void)
{
	return TouchAltFire_Available((int)cv("touch_fire2_visible", 2), altfire_state());
}

// ZOOM, contextual like ALT (touch_zoom_visible 2): while the weapon in hand
// has a scope. Not for an observer, who holds no weapon; a spectator watching
// a player sees that player's weapon and may zoom with it.
static qbool zoom_available(void)
{
	int mode = (int)cv("touch_zoom_visible", 2), held;
	if (mode != 2)
		return TouchZoom_Available(mode, false);
	if (TouchUI_IsSpectating() && !TouchUI_IsWatching())
		return false;
	if (!TouchUI_IsSpectating() && cl.stats[STAT_HEALTH] <= 0)
		return false;
	held = held_weapon_index();
	return held >= 0 && TouchZoom_Scoped(weapons[held].name);
}

// The HUD skin's pic, like Touch_Hud_SkinPicPath: hud_skin first, default second.
static cachepic_t *skin_pic(const char *name)
{
	char path[128];
	cachepic_t *pic;
	const char *skin = Cvar_VariableString(&cvars_all, "hud_skin", ~0);
	if (skin && skin[0])
	{
		dpsnprintf(path, sizeof(path), "gfx/hud/%s/%s", skin, name);
		pic = Draw_CachePic_Flags(path, CACHEPICFLAG_FAILONMISSING | CACHEPICFLAG_QUIET);
		if (Draw_IsPicLoaded(pic))
			return pic;
	}
	dpsnprintf(path, sizeof(path), "gfx/hud/luma/%s", name);
	pic = Draw_CachePic_Flags(path, CACHEPICFLAG_FAILONMISSING | CACHEPICFLAG_QUIET);
	if (Draw_IsPicLoaded(pic))
		return pic;
	dpsnprintf(path, sizeof(path), "gfx/hud/default/%s", name);
	pic = Draw_CachePic_Flags(path, CACHEPICFLAG_FAILONMISSING | CACHEPICFLAG_QUIET);
	if (Draw_IsPicLoaded(pic))
		return pic;
	return NULL;
}

// Fit a pic inside a box without distortion; returns the drawn width.
static float skin_pic_fit(cachepic_t *pic, float cx, float cy, float box_w, float box_h, float a)
{
	float pw, ph, aspect, h, w;
	if (!pic)
		return 0;
	pw = Draw_GetPicWidth(pic);
	ph = Draw_GetPicHeight(pic);
	aspect = (pw > 0 && ph > 0) ? pw / ph : 1.0f;
	h = min(box_h, box_w / aspect);
	w = h * aspect;
	DrawQ_Pic(cx - w * 0.5f, cy - h * 0.5f, pic, w, h, 1, 1, 1, a, 0);
	return w;
}

// The weapon strip's own size, the same geometry as the port's CSQC
// (touch_hud.qc): rows 26 units high and 46 wide, a 4-unit pad, and a 12-unit
// grip under the last row that a finger drags to resize it. The unit is the
// UI scale times cl_touch_weplist_scale. That is an engine cvar rather than a
// touch_* one: touch.layout.cfg pins those at launch, and a resize made on a
// server must be saved.
#define WEP_ROW_H 26.0f
#define WEP_ROW_W 46.0f
#define WEP_PAD   4.0f
#define WEP_GRIP_H 12.0f
#define WEP_SCALE_MIN 0.7f
#define WEP_SCALE_MAX 2.4f
// Tallest the plate may be, and the band it stays inside, as parts of the
// screen height: from below the top row of pills (MENU, CHAT and the vote
// chip at the end of it, touch_vote.c) to near the bottom edge.
#define WEP_FIT_H 0.77f
#define WEP_MARGIN_TOP 0.19f
#define WEP_MARGIN_Y 0.04f
// The grip's touch area reaches at least this far down, as a part of the
// screen's short side.
#define WEP_GRIP_GRAB 0.07f

static float weplist_scale(void)
{
	return bound(WEP_SCALE_MIN, cl_touch_weplist_scale.value, WEP_SCALE_MAX);
}

static int owned_weapon_count(void)
{
	int i, n = 0;
	for (i = 0; i < num_weapons; i++)
		if (weapons[i].impulse >= 0 && weapon_owned(i))
			n++;
	return n;
}

static qbool role_owned(int role);

typedef struct strip_s
{
	int n;                 // owned rows
	int index[MAX_WEAPONS];// weapons[] of each row, in the order the CSQC draws them
	float u;               // unit after the fit rule
	float cx, cy;          // plate centre
	float half_w, half_h;  // plate half size (rows, grip and pad)
	float row_w, row_h, pad;
	float top;             // first row's top
	int hook;              // the HOOK entry after the weapons (touch_hook.c)
	int nade;              // a NADE action row under the list (the nades mutator)
	float nade_top;        // its top
	float act_h;           // its height
	float grip_top;        // the grip's top (the last row's bottom)
} strip_t;

// Where everything of the strip is this frame. Returns the row count.
static int strip_layout(strip_t *st)
{
	float total, w = ui_w(), h = ui_h();
	int i;
	st->n = 0;
	// The HOOK entry stands for the Hook weapon too (it is held, not selected),
	// so that weapon is not also a row of its own.
	st->hook = TouchHook_RowState() != TOUCH_HOOKROW_HIDDEN;
	for (i = 0; i < num_weapons && st->n < MAX_WEAPONS; i++)
		if (weapons[i].impulse >= 0 && weapon_owned(i) && !(st->hook && !strcmp(weapons[i].name, "hook")))
			st->index[st->n++] = i;
	// The HOOK entry's and the NADE row's places are kept for as long as the
	// server has a hook / nades, so the weapon rows never move when one comes or goes.
	st->nade = TouchNade_State() != TOUCH_NADE_HIDDEN;
	// Fit: a strip taller than WEP_FIT_H of the screen shrinks, draw and
	// hit-test alike, so no row ever runs off it.
	total = TouchStrip_Units(st->n, st->hook, st->nade) + TSTRIP_GRIP_H + 2 * TSTRIP_PAD;
	st->u = ui_scale() * weplist_scale();
	if (total * st->u > WEP_FIT_H * h)
		st->u = WEP_FIT_H * h / total;
	st->row_w = WEP_ROW_W * st->u;
	st->row_h = WEP_ROW_H * st->u;
	st->pad = WEP_PAD * st->u;
	st->half_w = st->row_w * 0.5f + st->pad;
	st->half_h = total * st->u * 0.5f;
	widget_center(cv("touch_weplist_x", 0.962f), cv("touch_weplist_y", 0.360f), &st->cx, &st->cy);
	st->cy = bound(WEP_MARGIN_TOP * h + st->half_h, st->cy, (1 - WEP_MARGIN_Y) * h - st->half_h);
	st->cx = bound(st->half_w, st->cx, w - st->half_w);
	st->top = st->cy - st->half_h + st->pad;
	st->act_h = TSTRIP_ACT_H * st->u;
	st->nade_top = st->top + (st->n + st->hook) * st->row_h + TSTRIP_GAP * st->u;
	st->grip_top = st->nade ? st->nade_top + st->act_h : st->top + (st->n + st->hook) * st->row_h;
	return st->n + st->hook + st->nade;
}

// Whether this module draws the strip this frame: under a server's CSQC, in
// play, for a live player, with the controls on or off. A strip that is not
// drawn takes no taps.
static qbool strip_drawn(void)
{
	return strip_shown() && !cl_touch_csqc_active && in_play() && !cl.csqc_wantsmousemove
		&& cl.stats[STAT_HEALTH] > 0;
}

// The grip is for fingers: on a touchscreen, or with the controls on (the
// mouse stands in for a finger when testing on a desktop).
static qbool grip_shown(void)
{
	return strip_drawn() && (vid_touchscreen.integer || touch_device());
}

// On the grip: its width, from its top down by at least WEP_GRIP_GRAB of the
// short side, never up over the last row.
static qbool grip_hit(float px, float py)
{
	strip_t st;
	if (!grip_shown() || strip_layout(&st) <= 0)
		return false;
	return px >= st.cx - st.half_w && px <= st.cx + st.half_w
		&& py >= st.grip_top && py <= st.grip_top + max(WEP_GRIP_H * st.u, WEP_GRIP_GRAB * screen_min());
}

// What a finger at (px, py) lands on in the strip (touch_strip.h): one band,
// never two. *row is the weapon row for TSTRIP_WEAPON.
static int strip_kind(float px, float py, int *row)
{
	strip_t st;
	tstrip_t g;
	int ns = TouchNade_State();
	if (row)
		*row = -1;
	if (!strip_drawn() || strip_layout(&st) <= 0)
		return TSTRIP_MISS;
	g.n = st.n;
	g.hook = st.hook;
	g.hook_press = st.hook && (TouchHook_RowState() == TOUCH_HOOKROW_READY || TouchHook_RowState() == TOUCH_HOOKROW_HELD)
		&& !role_owned(ROLE_NADE);
	g.nade = st.nade;
	g.nade_press = st.nade && (ns == TOUCH_NADE_READY || ns == TOUCH_NADE_COOKING) && !role_owned(ROLE_HOOK);
	g.u = st.u;
	g.cx = st.cx;
	g.top = st.top;
	g.row_w = st.row_w;
	return TouchStrip_Hit(&g, px, py, row);
}

// Tap on a strip weapon row: the row's impulse, or -1.
static int strip_hit(float px, float py)
{
	strip_t st;
	int row;
	if (strip_kind(px, py, &row) != TSTRIP_WEAPON || strip_layout(&st) <= 0)
		return -1;
	return weapons[st.index[row]].impulse;
}

// Test hook for the finger tests (scripts/test-touch-strip-live.sh):
//   _touch_test_strip                         print where the strip's bands are (0..1 of the screen)
//   _touch_test_strip <slot> <band> <frac>    put finger <slot> down in a band, <frac> 0..1 down it:
//                                             hook, nade, gap (between the list and NADE), weapon<N>
// The finger goes through the real finger table (assign_role), like a touch.
static void TouchHUD_TestStrip_f(cmd_state_t *cmd)
{
	strip_t st;
	float w = ui_w(), h = ui_h(), y0 = 0, y1 = 0;
	if (!strip_drawn() || strip_layout(&st) <= 0)
	{
		Con_Print("STRIP none\n");
		return;
	}
	if (Cmd_Argc(cmd) < 4)
	{
		Con_Printf("STRIP x=%.4f w0y=%.4f hooktop=%.4f hookbot=%.4f nadetop=%.4f nadebot=%.4f gripy=%.4f rowh=%.4f n=%d hook=%d nade=%d\n",
			st.cx / w, (st.top + st.row_h * 0.5f) / h,
			st.hook ? (st.top + st.n * st.row_h) / h : -1.0f, st.hook ? (st.top + (st.n + 1) * st.row_h) / h : -1.0f,
			st.nade ? st.nade_top / h : -1.0f, st.nade ? (st.nade_top + st.act_h) / h : -1.0f,
			st.grip_top / h, st.row_h / h, st.n, st.hook, st.nade);
		return;
	}
	{
		int slot = atoi(Cmd_Argv(cmd, 1));
		const char *band = Cmd_Argv(cmd, 2);
		float frac = bound(0.0f, (float)atof(Cmd_Argv(cmd, 3)), 0.999f);
		if (slot < 0 || slot >= TOUCHHUD_ENGINE_FINGERS - 1)
			return;
		if (!strcmp(band, "hook"))
			y0 = st.top + st.n * st.row_h, y1 = y0 + st.row_h;
		else if (!strcmp(band, "nade"))
			y0 = st.nade_top, y1 = st.nade_top + st.act_h;
		else if (!strcmp(band, "gap"))
			y0 = st.top + (st.n + st.hook) * st.row_h, y1 = st.nade_top;
		else if (!strncmp(band, "weapon", 6))
			y0 = st.top + atoi(band + 6) * st.row_h, y1 = y0 + st.row_h;
		else
			return;
		multitouch[slot][0] = slot + 1;
		multitouch[slot][1] = st.cx / w;
		multitouch[slot][2] = (y0 + (y1 - y0) * frac) / h;
	}
}

static void draw_rrect(float cx, float cy, float hx, float hy, const float *rgb, float a)
{
	// Rounded plate: a capsule end at each side reads the same at these sizes,
	// and needs no fourth mask.
	shape_capsule(cx, cy, hx, hy, rgb, a);
}

static void draw_weapon_strip(float a)
{
	strip_t st;
	float h, w, pad, num_fs, num_slot;
	int i, held;
	// strip_shown() off means the stock weapons panel is back, and two
	// strips would sit on the right edge.
	if (!strip_drawn() || strip_layout(&st) <= 0)
		return;
	held = held_weapon_index();
	h = st.row_h;
	w = st.row_w;
	pad = st.pad;
	num_fs = 12 * st.u;
	num_slot = label_width(num_fs, "9");
	// Use the same glass body and rim as the other controls.
	TouchHUD_GlassControl(st.cx, st.cy, st.half_w, st.half_h, false, false, a);
	for (i = 0; i < st.n; i++)
	{
		const wepdef_t *wd = &weapons[st.index[i]];
		float y = st.top + i * h + h * 0.5f;
		// The held weapon when the CSQC's wepent tells it (the local rule);
		// the last tapped row only when it does not.
		qbool current = held >= 0 ? (st.index[i] == held) : (strip_selected_impulse >= 0 && wd->impulse == strip_selected_impulse);
		float left = st.cx - w * 0.5f + pad;
		float icon_left, icon_right;
		char num[8];
		if (current)
			draw_rrect(st.cx, y, w * 0.5f, h * 0.5f, C_ACCENT, a * 0.72f);
		dpsnprintf(num, sizeof(num), "%d", wd->impulse);
		shape_label_right(left + num_slot, y, num_fs, num, C_INK, a * A_INK * 0.9f);
		icon_left = left + num_slot + pad;
		icon_right = st.cx + w * 0.5f - pad;
		if (!skin_pic_fit(skin_pic(wd->pic), (icon_left + icon_right) * 0.5f, y, icon_right - icon_left, h - pad * 2,
			a * (current ? A_INK : A_INK * 0.55f)))
			shape_label((icon_left + icon_right) * 0.5f, y, num_fs, wd->name, C_INK, a * (current ? A_INK : A_INK * 0.55f));
	}
	// The HOOK entry, after the weapons: the hook is picked from this list.
	// Held while the finger is down (or the fall rescue holds it); faint when
	// the server has a hook but none is to hand (no fuel).
	if (st.hook)
	{
		int rs = TouchHook_RowState();
		float y = st.top + st.n * h + h * 0.5f, left = st.cx - w * 0.5f + pad;
		float icon_left = left + num_slot + pad, icon_right = st.cx + w * 0.5f - pad;
		int wi;
		const wepdef_t *hw = NULL;
		float ia = rs == TOUCH_HOOKROW_EMPTY ? A_INK * 0.25f : (rs == TOUCH_HOOKROW_HELD ? A_INK : A_INK * 0.55f);
		if (rs == TOUCH_HOOKROW_HELD || role_owned(ROLE_HOOK))
			draw_rrect(st.cx, y, w * 0.5f, h * 0.5f, C_ACCENT, a * 0.72f);
		for (wi = 0; wi < num_weapons; wi++)
			if (!strcmp(weapons[wi].name, "hook"))
				hw = &weapons[wi];
		shape_label_right(left + num_slot, y, num_fs, "H", C_INK, a * ia * 0.7f);
		// the Hook weapon's own icon, with the word beside it where there is room
		if (!hw || !skin_pic_fit(skin_pic(hw->pic), (icon_left + icon_right) * 0.5f, y, icon_right - icon_left, h - pad * 2, a * ia))
			shape_label((icon_left + icon_right) * 0.5f, y, num_fs, "HOOK", C_INK, a * ia);
	}
	// The NADE row: the grenade icon and its bonus count; while it cooks, the
	// fuse spent fills it from the left, turning red near the end; while the
	// next is on its way, a faint label.
	if (st.nade)
	{
		int state = TouchNade_State(), bonus = TouchNade_Bonus();
		float y = st.nade_top + st.act_h * 0.5f, ah = st.act_h, left = st.cx - w * 0.5f + pad;
		float icon_left = left + num_slot + pad, icon_right = st.cx + w * 0.5f - pad;
		float ia = state == TOUCH_NADE_WAITING ? A_INK * 0.25f : A_INK;
		if (state == TOUCH_NADE_COOKING)
		{
			float t = TouchNade_Timer();
			const float *c = t > 0.6f ? C_DANGER : C_ACCENT;
			DrawQ_Fill(st.cx - w * 0.5f, y - ah * 0.5f, w * t, ah, c[0], c[1], c[2], a * A_SURFACE_ACTIVE, 0);
		}
		else if (role_owned(ROLE_NADE))
			draw_rrect(st.cx, y, w * 0.5f, ah * 0.5f, C_ACCENT, a * 0.72f);
		if (bonus > 0)
		{
			char num[8];
			dpsnprintf(num, sizeof(num), "%d", bonus);
			shape_label_right(left + num_slot, y, num_fs, num, C_INK, a * ia);
		}
		if (!skin_pic_fit(skin_pic("nade_normal"), (icon_left + icon_right) * 0.5f, y, icon_right - icon_left, ah - pad * 2, a * ia))
			shape_label((icon_left + icon_right) * 0.5f, y, num_fs, "NADE", C_INK, a * ia);
	}
	// The grip: one pill, the drag handle of a bottom sheet.
	if (grip_shown())
	{
		float gy = st.grip_top + WEP_GRIP_H * st.u * 0.5f;
		float lw = w * 0.4f, lh = max(2.0f, st.u * 3.0f);
		qbool held_grip = role_owned(ROLE_WEPGRIP);
		float ga = a * (held_grip ? A_INK : A_INK * 0.55f);
		shape_capsule(st.cx, gy, lw * 0.5f, lh * 0.5f, held_grip ? C_ACCENT : C_INK, ga);
	}
}

static qbool role_owned(int role)
{
	int i;
	for (i = 0; i < MAX_FINGERS; i++)
		if (fingers[i].used && fingers[i].role == role)
			return true;
	return false;
}

static qbool weapon_button_shown(void)
{
	// The strip is the switcher, as locally; the WEP button only when the
	// player asked for it, or when nothing is owned yet to tap on.
	return cv("touch_weapon_visible", 0) || (cv("touch_weplist_visible", 1) && owned_weapon_count() == 0);
}

static qbool topbar_hit(const touchbar_rect_t *r, float x, float y)
{
	return r->w > 0 && x >= r->x && x <= r->x + r->w && y >= r->y && y <= r->y + r->h;
}

// The chrome pills under a point, or ROLE_NONE.
static int pill_at(float px, float py)
{
	if (cv("touch_con_visible", 0) && hit_rect(px, py, cv("touch_con_x", 0.090f), cv("touch_con_y", 0.948f), cv("touch_con_size", 0.060f), cv("touch_con_aspect", 2.9f), 1.20f))
		return ROLE_CON;
	if (cv("touch_pause_visible", 1) && (topbar_valid ? topbar_hit(&topbar.menu, px, py) : hit_rect(px, py, cv("touch_pause_x", 0.655f), cv("touch_pause_y", 0.088f), cv("touch_pause_size", 0.060f), cv("touch_pause_aspect", 2.4f), 1.20f)))
		return ROLE_PAUSE;
	if (cv("touch_chat_visible", 1) && (topbar_valid ? topbar_hit(&topbar.chat, px, py) : hit_rect(px, py, cv("touch_chat_x", 0.889f), cv("touch_chat_y", 0.088f), cv("touch_chat_size", 0.060f), cv("touch_chat_aspect", 2.4f), 1.20f)))
		return ROLE_CHAT;
	if (cv("touch_scores_visible", 1) && (topbar_valid ? topbar_hit(&topbar.score, px, py) : hit_rect(px, py, cv("touch_scores_x", 0.772f), cv("touch_scores_y", 0.088f), cv("touch_scores_size", 0.060f), cv("touch_scores_aspect", 2.4f), 1.20f)))
		return ROLE_SCORES;
	return ROLE_NONE;
}

// With the controls off: the pills (touch_desk_pills), the spectator bar,
// the strip, and one look finger anywhere else. A tap fires nothing.
static int spec_bar_part_at(float px, float py);
static int spec_action_role(int action);
static qbool desk_specbar_shown(void);
static int assign_desk_role(float px, float py)
{
	int role = ROLE_NONE;
	if (desk_pills())
		role = pill_at(px, py);
	if (role != ROLE_NONE)
		return role;
	// The spectator bar is on screen in desk mode too (draw_desk_specbar).
	// A finger on it means what the same part means with the controls on; it
	// used to be drawn but only a real mouse click acted, so tapping <, > or
	// FREE did nothing exactly when a keyboard had put the controls away.
	if (desk_specbar_shown())
	{
		int state = TouchUI_SpectatorState();
		int part = spec_bar_part_at(px, py);
		if (part >= 0)
		{
			int action = TouchSpec_BarAction(state, part);
			return action != TOUCHSPEC_ACT_NONE ? spec_action_role(action) : ROLE_IGNORED;
		}
	}
	if (grip_hit(px, py))
		return role_owned(ROLE_WEPGRIP) ? ROLE_IGNORED : ROLE_WEPGRIP;
	if (strip_hit(px, py) >= 0)
		return ROLE_WEPSTRIP;
	return cv("vid_mouse_confirmed", 0) || role_owned(ROLE_LOOK) ? ROLE_IGNORED : ROLE_LOOK;
}

// touch_hop_mode 3: HOP is a plain button and the keep-jumping chip beside it
// (touch_hoplock_*) holds jump down while it is on, as in touch_input.qc.
#define HOP_CHIP 3
static qbool hoplock_shown(void)
{
	return (int)cv("touch_hop_mode", HOP_CHIP) == HOP_CHIP && cv("touch_hoplock_visible", 1);
}
static qbool hoplock_hit(float px, float py)
{
	return hit_rect(px, py, cv("touch_hoplock_x", 0.675f), cv("touch_hoplock_y", 0.760f), cv("touch_hoplock_size", 0.052f), 1, 1.10f);
}

static qbool spec_hud(void);
static int assign_spec_role(float px, float py);
static void spec_frame(qbool *fire, qbool *fire2, qbool *jump);
static qbool spec_bar_shown(void);
static qbool hud_has(int ctl);
static int hud_mode(void);
static qbool tospec_shown(void);
static qbool tospec_hit(float px, float py);
static qbool ready_hit(float px, float py);
static void extra_release(finger_t *f);
#define ROLE_TOSPEC  306
#define ROLE_READY   307
#define ROLE_RESPAWN 308
#define ROLE_SPEC_ATTACK TOUCHSPEC_ROLE_ATTACK
#define ROLE_SPEC_JOIN TOUCHSPEC_ROLE_JOIN
#define ROLE_SPEC_PREV   TOUCHSPEC_ROLE_PREV
#define ROLE_SPEC_FREE   TOUCHSPEC_ROLE_FREE
#define ROLE_SPEC_UP     TOUCHSPEC_ROLE_UP
#define ROLE_SPEC_DOWN   TOUCHSPEC_ROLE_DOWN

static int assign_role(float px, float py)
{
	int role;
	if (desk_active())
		return assign_desk_role(px, py);
	if (spec_hud())
		return assign_spec_role(px, py);
	role = pill_at(px, py);
	if (role != ROLE_NONE)
		return role;
	if (tospec_shown() && tospec_hit(px, py))
		return ROLE_TOSPEC;
	if (ready_hit(px, py))
		return ROLE_READY;
	if (hud_mode() == TOUCHMODE_DEAD)
	{
		// Waiting to respawn: RESPAWN (HOP's place, a little wider) and the look.
		if (!in_edge_deadzone(px, py) && hit_rect(px, py, cv("touch_jump_x", 0.855f), cv("touch_jump_y", 0.885f), cv("touch_jump_size", 0.072f), cv("touch_jump_aspect", 2.6f) * 1.25f, 1.10f))
			return ROLE_RESPAWN;
		return cv("vid_mouse_confirmed", 0) || role_owned(ROLE_LOOK) ? ROLE_IGNORED : ROLE_LOOK;
	}
	// The strip's grip, its NADE row, then its rows: tap a row to switch
	// (Touch_Weapons_Hit).
	if (grip_hit(px, py))
		return role_owned(ROLE_WEPGRIP) ? ROLE_IGNORED : ROLE_WEPGRIP;
	switch (strip_kind(px, py, NULL))
	{
	case TSTRIP_NADE:
		return role_owned(ROLE_NADE) ? ROLE_IGNORED : ROLE_NADE;
	case TSTRIP_HOOK:
		return role_owned(ROLE_HOOK) ? ROLE_IGNORED : ROLE_HOOK;
	case TSTRIP_WEAPON:
		return ROLE_WEPSTRIP;
	case TSTRIP_DEAD:
		return ROLE_IGNORED; // a row (or the gap) that cannot be pressed now keeps the finger: no neighbour gets it
	default:
		break;
	}
	if (in_edge_deadzone(px, py))
		return ROLE_IGNORED;
	if (!cv("vid_mouse_confirmed", 0) && cv("touch_fire_visible", 1) && hit_circle(px, py, cv("touch_fire_x", 0.855f), cv("touch_fire_y", 0.680f), cv("touch_fire_size", 0.160f), 1.15f))
		return ROLE_FIRE;
	if (!cv("vid_mouse_confirmed", 0) && fire2_available() && hit_circle(px, py, cv("touch_fire2_x", 0.770f), cv("touch_fire2_y", 0.755f), cv("touch_fire2_size", 0.100f), 1.15f))
		return ROLE_FIRE2;
	if (cv("touch_jump_visible", 1) && hit_rect(px, py, cv("touch_jump_x", 0.855f), cv("touch_jump_y", 0.885f), cv("touch_jump_size", 0.072f), cv("touch_jump_aspect", 2.6f), 1.10f))
		return ROLE_JUMP;
	if (hoplock_shown() && hoplock_hit(px, py))
		return ROLE_HOPLOCK;
	if (cv("touch_crouch_visible", 1) && hit_circle(px, py, crouch_x(cv("touch_crouch_x", 0.700f)), cv("touch_crouch_y", 0.885f), cv("touch_crouch_size", 0.080f), 1.10f))
		return ROLE_CROUCH;
	if (weapon_button_shown() && hit_circle(px, py, cv("touch_weapon_x", 0.540f), cv("touch_weapon_y", 0.860f), cv("touch_weapon_size", 0.100f), 1.10f))
		return ROLE_WEAPON;
	if (zoom_available() && hit_circle(px, py, cv("touch_zoom_x", 0.860f), cv("touch_zoom_y", 0.480f), cv("touch_zoom_size", 0.100f), 1.10f))
		return ROLE_ZOOM;
	if (TouchTurn_ButtonShown() && hit_circle(px, py, cv("touch_turn_x", 0.300f), cv("touch_turn_y", 0.840f), cv("touch_turn_size", 0.085f), 1.10f))
		return ROLE_TURN;
	if (cv("touch_reload_visible", 0) && hit_circle(px, py, cv("touch_reload_x", 0.720f), cv("touch_reload_y", 0.480f), cv("touch_reload_size", 0.100f), 1.10f))
		return ROLE_RELOAD;
	if (cv("touch_dodge_visible", 0) && hit_circle(px, py, cv("touch_dodge_x", 0.165f), cv("touch_dodge_y", 0.480f), cv("touch_dodge_size", 0.100f), 1.10f))
		return ROLE_DODGE;
	if (!cv("vid_keyboard_detected", 0) && (in_move_zone(px, py) || (cv("touch_move_visible", 1) && hit_circle(px, py, cv("touch_move_x", 0.170f), cv("touch_move_y", 0.680f), cv("touch_move_size", 0.400f), 1.0f))))
	{
		if (role_owned(ROLE_MOVE))
			return ROLE_IGNORED;
		return ROLE_MOVE;
	}
	if (!cv("vid_mouse_confirmed", 0) && in_look_zone(px, py))
	{
		if (role_owned(ROLE_LOOK))
			return ROLE_IGNORED;
		return ROLE_LOOK;
	}
	if (cv("vid_mouse_confirmed", 0) || role_owned(ROLE_LOOK))
		return ROLE_IGNORED;
	return ROLE_LOOK;
}

// The JOIN part: while a spectator may join -- signon finished, in the
// match, not the end screen.
static qbool spec_join_shown(void)
{
	return vid_touchscreen.integer && spec_hud() && !TouchMode_IsMatchEnd(hud_mode())
		&& cls.state == ca_connected && cls.signon >= SIGNONS && !cl.intermission;
}


// The look (touch_feel.h): degrees per dp of finger travel, a gain that rises
// with finger speed, and a smoothing that never loses travel.
static void look_cfg(touchlook_cfg_t *c)
{
	c->sens = cv("touch_sens_base", 3.6f);
	c->y_mult = cv("touch_sens_y_mult", 0.85f);
	c->accel = cv("touch_look_accel", 0.8f);
	c->accel_lo = cv("touch_look_accel_lo", 200);
	c->accel_hi = cv("touch_look_accel_hi", 800);
	c->filter = (int)cv("touch_look_filter", 1);
	c->fcmin = cv("touch_look_fcmin", 4.0f);
	c->beta = cv("touch_look_beta", 0.05f);
	// a clamp for contacts that jump, not a limit on turning: it rises with
	// the sensitivity and the acceleration
	c->max_rate = cv("touch_look_max_deg_per_s", 900) * max(1.0f, c->sens / 2.8f) * (1 + max(0.0f, c->accel));
}

static void look_turn(float yaw, float pitch)
{
	if (cv("touch_invert_y", 0))
		pitch = -pitch;
	// aim friction: the drag turns slower over an enemy (touch_aim.c)
	yaw *= TouchAim_LookScale();
	pitch *= TouchAim_LookScale();
	cl.viewangles[YAW] -= yaw;
	cl.viewangles[PITCH] += pitch;
	cl.viewangles[PITCH] = bound(-89, cl.viewangles[PITCH], 89);
	cl.viewangles[ROLL] = 0;
}

// raw_x, raw_y: finger travel this frame in console units.
static void look_apply(float raw_x, float raw_y, float dt)
{
	touchlook_cfg_t c;
	float yaw, pitch, u = max(0.001f, dp_units());
	look_cfg(&c);
	TouchLook_Step(&look_state, &c, raw_x / u, raw_y / u, dt, &yaw, &pitch);
	look_turn(yaw, pitch);
}

static void look_begin(void)
{
	if (!look_active)
		TouchLook_Reset(&look_state);
	look_active = true;
	look_prev_time = host.realtime;
}

// The last look finger lifted: the turn still owed is paid at once.
static void look_end(void)
{
	if (look_active)
	{
		float yaw, pitch;
		TouchLook_Flush(&look_state, &yaw, &pitch);
		look_turn(yaw, pitch);
	}
	look_active = false;
}

// The controls are hidden -- a keyboard is attached, or Keyboard & mouse mode
// -- but the screen is still a touchscreen: a finger dragged anywhere turns
// the view, with the touch look settings and filter, so a keyboard player with
// no mouse can still look around. Only one finger looks; taps do nothing.
// Runs under any CSQC, since the touch CSQC stands down with the controls.
void TouchHUD_LookWithoutControls(void)
{
	static int finger = -1;
	static float last_x, last_y;
	double now = host.realtime;
	int i;
	int claimed;
	// Under a server's CSQC this module runs the desk HUD: strip, grip and
	// pills take their fingers, the rest look (TouchHUD_Frame).
	if (desk_active())
	{
		finger = -1;
		TouchHUD_Frame();
		return;
	}
	if (was_active)
		TouchHUD_Frame(); // lets go of whatever the desk HUD held
	if (vid_touchscreen.integer || !in_play() || cl.csqc_wantsmousemove)
	{
		finger = -1;
		return;
	}
	if (finger >= 0 && !multitouch[finger][0])
	{
		finger = -1;
		look_end();
	}
	// The port's CSQC claims the fingers on its own strip, grip and pills
	// (one bit per finger slot); those do not turn the view.
	claimed = (int)cv("_cl_touch_desk_fingers", 0);
	if (finger >= 0 && (claimed & (1 << finger)))
		finger = -1;
	if (finger < 0)
	{
		for (i = 0; i < TOUCHHUD_ENGINE_FINGERS - 1; i++)
			if (multitouch[i][0] && !(claimed & (1 << i)))
			{
				finger = i;
				last_x = multitouch[i][1] * ui_w();
				last_y = multitouch[i][2] * ui_h();
				look_begin();
				break;
			}
		return;
	}
	{
		float x = multitouch[finger][1] * ui_w(), y = multitouch[finger][2] * ui_h();
		look_apply(x - last_x, y - last_y, (float)(now - look_prev_time));
		look_prev_time = now;
		last_x = x;
		last_y = y;
	}
}

// A held button is also an aim surface: a thumb that lands on FIRE and then
// drags turns the view while the button stays down ("fire and drag"), so
// shooting never costs the look. touch_fire_drag_look covers FIRE, HOOK and
// NADE; touch_button_drag_look covers ALT, HOP, DUCK and ZOOM. The drag
// starts once the finger has left the look deadzone, and adds to any other
// finger that is turning the view: two thumbs never fight over it.
static void button_drag_look(finger_t *f, qbool aim_button, qbool *look_seen, float *look_dx, float *look_dy)
{
	float ddx, ddy;
	if (!cv(aim_button ? "touch_fire_drag_look" : "touch_button_drag_look", 1))
		return;
	ddx = f->last_x - f->down_x;
	ddy = f->last_y - f->down_y;
	if (!f->draglook && sqrt(ddx * ddx + ddy * ddy) >= cv("touch_look_deadzone_px", 8))
	{
		f->draglook = true;
		look_begin();
		// the travel that proved it a drag counts too
		f->prev_look_x = f->down_x;
		f->prev_look_y = f->down_y;
	}
	if (f->draglook)
	{
		*look_seen = true;
		*look_dx += f->last_x - f->prev_look_x;
		*look_dy += f->last_y - f->prev_look_y;
		f->prev_look_x = f->last_x;
		f->prev_look_y = f->last_y;
	}
}

static void weapon_gesture(finger_t *f, qbool up)
{
	// touch_weapon_mode 2: tap = next weapon, swipe up/down = prev/next.
	float dy = f->last_y - f->down_y;
	float dx = f->last_x - f->down_x;
	float swipe = max(20.0f, widget_radius(cv("touch_weapon_size", 0.100f)) * 0.6f);
	if (f->fired)
		return;
	if (fabs(dy) > swipe && fabs(dy) > fabs(dx))
	{
		exec_now(dy < 0 ? "weapprev" : "weapnext");
		f->fired = true;
		return;
	}
	if (up && host.realtime - f->down_time < 0.35 && fabs(dx) < swipe && fabs(dy) < swipe)
	{
		exec_now("weapnext");
		f->fired = true;
	}
}

static void pill_release(finger_t *f)
{
	// Chrome pills act on release, like the CSQC's tap-with-slop rule.
	float slop = 10 + cv("touch_pause_slop_px", 10);
	float dx = f->last_x - f->down_x, dy = f->last_y - f->down_y;
	if (fabs(dx) > slop || fabs(dy) > slop)
		return;
	switch (f->role)
	{
	case ROLE_CON:
		Cbuf_AddText(cmd_local, "toggleconsole\n");
		break;
	case ROLE_CHAT:
		Cbuf_AddText(cmd_local, "messagemode\n");
		break;
	case ROLE_PAUSE:
		TouchHUD_ReleaseAll();
		if (Cvar_VariableValue(&cvars_all, "menu_gamemenu", ~0) && !cls.demoplayback
			&& Cvar_VariableValue(&cvars_all, "_menu_gamemenu_dialog_available", ~0))
			Cbuf_AddText(cmd_local, "\nmenu_showgamemenudialog\n");
		else
			Cbuf_AddText(cmd_local, "\ntogglemenu 1\n");
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// The loading screen: a join waiting for the server, downloading or loading
// the map, a local game's server starting, or a map change. It hides the
// menu, so Escape there opened a menu nobody could see, and a finger had
// nothing to press: a server that never answered, or a slow download, kept a
// touch player waiting with no way out. On a touchscreen a CANCEL pill sits
// in the top corner, and it and Back (Escape) leave for the main menu, for
// the whole join: from the connect request and its retries to the last
// signon, and whenever the loading screen is up. That includes the stretch
// after a server's "reconnect" (a map change) before its new map arrives,
// when neither the loading screen nor the game nor the menu is drawn.
//
// Not the app's own startup, nor a renderer restart: the loading screen is
// up for those too, but there is no join to leave. A local game's map load
// counts from SV_SpawnServer (TouchHUD_LocalGameLoading) until the loading
// screen is gone.
//
// A local game's server loads its map inside the `map` command, and every
// load runs inside one frame: 20 to 60 s on a tablet, most of it the QC's
// model and sound precaches, not the map. The loaders redraw the loading
// screen meanwhile (CL_KeepaliveMessage, SCR_PushLoadingScreen), but fingers
// and keys are only read once the frame is over. So each redraw inside a long
// frame also looks in SDL's queue for a tap on CANCEL or Back: the pill turns
// to CANCELLING at once. The join then stops at the start of the next item
// loaded (TouchHUD_LoadingAbortPoint, from SCR_PushLoadingScreen): the frame
// is unwound the way Host_Error unwinds it, without the error. Where it cannot
// be unwound, the disconnect runs once the command that is loading returns.
// ---------------------------------------------------------------------------
static int loading_cancel_src;   // finger that went down on CANCEL
static qbool loading_cancelling; // tapped: CANCELLING until the loading screen is gone
static qbool loading_localgame;  // SV_SpawnServer put the loading screen up

extern qbool scr_loading;
static qbool loading_cancellable(void)
{
	if (!scr_loading)
		loading_localgame = false;
	return touch_device()
		&& (cls.connect_trying || (cls.state == ca_connected && cls.signon < SIGNONS)
			|| (scr_loading && loading_localgame));
}

void TouchHUD_LocalGameLoading(void)
{
	loading_localgame = true;
}

// In the top corner where the CHAT pill sits by default, at the size of the
// MENU and CHAT pills, mirrored for the left hand like them. A fixed place,
// not the player's CHAT position: wherever CHAT was moved to, CANCEL stays
// clear of the loading picture and of the progress text and bar at the bottom.
static void loading_pill(float *cx, float *cy, float *hx, float *hy)
{
	widget_center(0.862f, 0.088f, cx, cy);
	widget_half(cv("touch_pause_size", 0.060f), cv("touch_pause_aspect", 2.9f), hx, hy);
}

// Back to the menu after a cancelled join. Once a server answers, the engine
// hands the keys to the game (netconn.c) without telling the menu QC, which
// still thinks it is open: `togglemenu 1` alone is then ignored, the menu
// hides itself on its next frame for not having the keys, and only the menu
// background is left. Closing it first puts the menu QC in step either way.
#define LOADING_MENU_BACK "togglemenu 0\ntogglemenu 1\n"

static void loading_cancel(void)
{
	Con_Printf("Touch: join cancelled from the loading screen\n");
	loading_cancel_src = 0;
	loading_cancelling = true;
	// disconnect alone does not stop a connect the server has not answered yet
	cls.connect_trying = false;
	// nor the join's map and package downloads, which it lets run on. This
	// stops every curl download (as "curl --cancel" does): libcurl.c has no
	// call for only the join's.
	if (Curl_Have_forthismap())
	{
		Con_Printf("Touch: the join's downloads are stopped\n");
		Curl_CancelAll();
	}
	// _touch_cancel_join: the connect a local game's `map` starts after its
	// server is up, when the tap came while that server was loading
	Cbuf_AddText(cmd_local, "\ndisconnect\n_touch_cancel_join\n" LOADING_MENU_BACK);
}

// The start of an item loaded (a model, a sound, a precache step), CANCEL
// tapped: stop the join now, as Host_Error would, instead of after the whole
// load. Every loader starts an item with SCR_PushLoadingScreen before it
// changes anything, so nothing is left half built; a model or sound not
// loaded yet is simply loaded the next time. A QC caught mid-precache is
// reset (its Shutdown skipped, as on a crash). Not while the menu QC runs:
// resetting it would leave no menu to go back to.
void TouchHUD_LoadingAbortPoint(void)
{
	cmd_buf_t *cbuf = cmd_local->cbuf;
	int i;
	if (!loading_cancelling || !loading_cancellable())
		return;
	if (host.framecount < 1 || host.state != host_active || cls.state == ca_dedicated)
		return;
#ifdef CONFIG_MENU
	if (MVM_prog->loaded && MVM_prog->depth > 0)
		return;
#endif
	Con_Printf("Touch: the join stops now\n");
	for (i = 0; i < PRVM_PROG_MAX; i++)
	{
		prvm_prog_t *prog = PRVM_GetProg(i);
		if (!prog->loaded || prog->depth <= 0)
			continue;
		if (prog == SVVM_prog)
			PRVM_serverfunction(SV_Shutdown) = 0;
		else if (prog == CLVM_prog)
			PRVM_clientfunction(CSQC_Shutdown) = 0;
		PRVM_Prog_Reset(prog);
	}
	CL_Parse_ErrorCleanUp();
	if (host.hook.SV_Shutdown)
		host.hook.SV_Shutdown();
	// The command that was loading (`map`) is still first in the buffer until
	// it returns: drop it, or the next frame runs it again, and what was
	// queued behind it (a local game's connect). Deferred commands stay, unlike
	// Host_Error's Cbuf_Clear.
	while (!List_Is_Empty(&cbuf->start))
	{
		cbuf->size -= List_Entry(cbuf->start.next, cmd_input_t, list)->length;
		List_Move_Tail(cbuf->start.next, &cbuf->free);
	}
	cls.connect_trying = false;
	loading_localgame = false;
	CL_Disconnect();
	Cbuf_AddText(cmd_local, LOADING_MENU_BACK);
	Host_AbortCurrentFrame();
}

qbool TouchHUD_LoadingCancelled(void)
{
	if (loading_cancelling)
		Con_Printf("Touch: the local game stops before its server spawns\n");
	return loading_cancelling;
}

static void TouchHUD_CancelJoin_f(cmd_state_t *cmd)
{
	cls.connect_trying = false;
}

qbool TouchHUD_LoadingBack(void)
{
	if (!loading_cancellable())
		return false;
	loading_cancel();
	return true;
}

static qbool loading_finger(int id, qbool down, float nx, float ny)
{
	float cx, cy, hx, hy, px = nx * ui_w(), py = ny * ui_h();
	qbool on;
	if (!loading_cancellable())
	{
		loading_cancel_src = 0;
		return false;
	}
	loading_pill(&cx, &cy, &hx, &hy);
	on = fabs(px - cx) <= hx * 1.2f && fabs(py - cy) <= hy * 1.2f;
	if (down && on)
		loading_cancel_src = id;
	else if (!down && id == loading_cancel_src)
	{
		if (on)
			loading_cancel();
		loading_cancel_src = 0;
	}
	return true;
}

static float console_pointer_x = 0.5f, console_pointer_y = 0.5f;

void TouchHUD_NotePointer(float nx, float ny)
{
	console_pointer_x = nx;
	console_pointer_y = ny;
}

void TouchHUD_DrawConsolePointer(void)
{
	float x, y, u, i, n;
	if (!TouchCursor_ConsolePointer(Cvar_VariableValue(&cvars_all, "vid_system_cursor", ~0) != 0,
			vid_pointer_mouse.integer, key_consoleactive != 0 && scr_con_current > 0))
		return;
	x = bound(0, console_pointer_x, 1) * vid_conwidth.value;
	y = bound(0, console_pointer_y, 1) * vid_conheight.value;
	u = vid_conheight.value / 300.0f; // one pixel of a 300-high arrow
	if (u < 1) u = 1;
	n = 14; // arrow: a white wedge with a dark rim, tip at the hotspot
	for (i = 0; i < n; i++)
	{
		float w = (i * 0.6f + 1);
		DrawQ_Fill(x - u, y + i * u * 1.4f - u, (w + 2) * u, u * 1.4f + 2 * u, 0, 0, 0, 0.9f, 0);
	}
	for (i = 0; i < n; i++)
	{
		float w = (i * 0.6f + 1);
		DrawQ_Fill(x, y + i * u * 1.4f, w * u, u * 1.4f, 1, 1, 1, 1, 0);
	}
}

void TouchHUD_DrawLoading(void)
{
	static unsigned int drawn_in_frame;
	float cx, cy, hx, hy;
	if (!loading_cancellable())
	{
		loading_cancelling = false;
		return;
	}
	if (!loading_cancelling && (TouchUIB_LoadingCardUp() || TouchFE_MenuHidden()))
		return; // the front end's card has its own Cancel: one button, not two
	shapes_probe();
	loading_pill(&cx, &cy, &hx, &hy);
	// Drawn again in the same frame: a load redrawing its progress, while
	// input waits for the frame to end (see above).
	if (drawn_in_frame == host.framecount && !loading_cancelling && loading_cancel_src == 0
		&& VID_TouchQueuedTap((cx - hx * 1.2f) / ui_w(), (cy - hy * 1.2f) / ui_h(), (cx + hx * 1.2f) / ui_w(), (cy + hy * 1.2f) / ui_h()))
		loading_cancel();
	drawn_in_frame = host.framecount;
	// As plain as the chrome in a match, at its full opacity: easy to find on
	// the dark loading screen without pulling the eye from the progress.
	widget_chrome_pill(cx, cy, hx, hy, loading_cancelling ? "CANCELLING" : "CANCEL",
		loading_cancel_src != 0 || loading_cancelling, 0.95f, C_DANGER);
}

// ---------------------------------------------------------------------------
// The CSQC's own pointer. A stock CSQC in cursor mode reads the pointer from
// the type-3 input events cl_input.c sends from in_windowmouse after IN_Move,
// works out what is under it while drawing (MapVote_Selection), and acts on a
// K_MOUSE1 press. Here a finger is that pointer: the first finger to land
// moves in_windowmouse, and its press reaches the CSQC only once a frame has
// been drawn since, or the click would land on whatever the pointer was over
// before. A tap whose down and up arrive in the same frame (Android) is held
// back the same way and then delivered as press and release. One finger at a
// time; fingers already on the glass when the pointer UI opens are not it.
// The touch controls stand down meanwhile, except MENU and CHAT, which sit
// above the stock pointer UIs.
// ---------------------------------------------------------------------------
enum { CURSOR_IDLE, CURSOR_PRESS_PENDING, CURSOR_PRESSED, CURSOR_TAP_PENDING };
static int cursor_state = CURSOR_IDLE;
static int cursor_src;                 // multitouch id of the pointer finger, 0 = none
static int cursor_pill_src;            // a finger on MENU or CHAT instead
static finger_t cursor_pill;
static unsigned int frames_drawn;      // TouchHUD_Draw calls: one per drawn frame
static unsigned int cursor_pos_frame;  // frames_drawn when the pointer finger landed
static qbool cursor_was;
static qbool cursor_finger_pointer;    // a finger, not the mouse, placed the pointer last

// MENU and CHAT of the engine HUD, kept over a pointer UI: drawn and tappable
// while this module draws the HUD (a server's CSQC), with the controls on, or
// off with touch_desk_pills.
static qbool cursor_pills_shown(void)
{
	return !cl_touch_csqc_active && touch_device() && (vid_touchscreen.integer || cv("touch_desk_pills", 1));
}

static int cursor_pill_at(float px, float py)
{
	if (!cursor_pills_shown())
		return ROLE_NONE;
	// match end: MENU is in the corner (draw_endmenu), and takes its finger first
	if (TouchMode_IsMatchEnd(TouchHudMode_Current()->mode))
		return ROLE_NONE;
	if (cv("touch_pause_visible", 1) && (topbar_valid ? topbar_hit(&topbar.menu, px, py) : hit_rect(px, py, cv("touch_pause_x", 0.655f), cv("touch_pause_y", 0.088f), cv("touch_pause_size", 0.060f), cv("touch_pause_aspect", 2.4f), 1.20f)))
		return ROLE_PAUSE;
	if (cv("touch_chat_visible", 1) && (topbar_valid ? topbar_hit(&topbar.chat, px, py) : hit_rect(px, py, cv("touch_chat_x", 0.889f), cv("touch_chat_y", 0.088f), cv("touch_chat_size", 0.060f), cv("touch_chat_aspect", 2.4f), 1.20f)))
		return ROLE_CHAT;
	return ROLE_NONE;
}

static void cursor_point(float nx, float ny)
{
	in_windowmouse_x = nx * vid.mode.width;
	in_windowmouse_y = ny * vid.mode.height;
	cursor_finger_pointer = true;
}

qbool TouchHUD_FingerIsPointer(void)
{
	return cursor_was && cursor_finger_pointer;
}

qbool TouchHUD_CursorFinger(int id, qbool down, float nx, float ny)
{
	// The loading screen takes every finger: the menu under it is not drawn,
	// and a tap there used to press whatever of it lay under the finger.
	if (loading_finger(id, down, nx, ny))
		return true;
	if (!cursor_wanted() || id <= 0)
		return false;
	if (down)
	{
		float px = nx * ui_w(), py = ny * ui_h();
		int role;
		if (cursor_src)
			return false;
		role = cursor_pill_at(px, py);
		if (role != ROLE_NONE)
		{
			if (cursor_pill_src)
				return false;
			memset(&cursor_pill, 0, sizeof(cursor_pill));
			cursor_pill.role = role;
			cursor_pill.down_x = cursor_pill.last_x = px;
			cursor_pill.down_y = cursor_pill.last_y = py;
			cursor_pill_src = id;
			return false;
		}
		// The press waits for one frame drawn with the landing point, not for
		// the finger to keep still: a finger on glass never quite does.
		cursor_src = id;
		cursor_point(nx, ny);
		cursor_pos_frame = frames_drawn;
		cursor_state = CURSOR_PRESS_PENDING;
	}
	else if (id == cursor_src)
	{
		cursor_src = 0;
		if (cursor_state == CURSOR_PRESSED)
		{
			Key_Event(K_MOUSE1, 0, false);
			cursor_state = CURSOR_IDLE;
		}
		else if (cursor_state == CURSOR_PRESS_PENDING)
			cursor_state = CURSOR_TAP_PENDING;
	}
	else if (id == cursor_pill_src)
	{
		cursor_pill.last_x = nx * ui_w();
		cursor_pill.last_y = ny * ui_h();
		cursor_pill_src = 0;
		pill_release(&cursor_pill);
	}
	return false;
}

static void cursor_end(void)
{
	int i, j = 0;
	if (cursor_state == CURSOR_PRESSED)
		Key_Event(K_MOUSE1, 0, false);
	cursor_state = CURSOR_IDLE;
	cursor_src = cursor_pill_src = 0;
	cursor_was = false;
	cursor_finger_pointer = false;
	// Fingers still on the glass are not new touches for the controls coming
	// back: lifting the finger that cast the last vote must not fire or jump.
	memset(fingers, 0, sizeof(fingers));
	for (i = 0; i < TOUCHHUD_ENGINE_FINGERS - 1 && j < MAX_FINGERS; i++)
		if (multitouch[i][0])
		{
			fingers[j].used = true;
			fingers[j].src = (int)multitouch[i][0];
			fingers[j].role = ROLE_IGNORED;
			fingers[j].down_time = host.realtime;
			j++;
		}
}

qbool TouchHUD_CursorFrame(void)
{
	int i;
	if (!cursor_wanted())
	{
		if (cursor_was)
			cursor_end();
		return false;
	}
	if (!cursor_was)
	{
		cursor_was = true;
		cursor_state = CURSOR_IDLE;
		cursor_src = cursor_pill_src = 0;
		cursor_finger_pointer = false;
		TouchHUD_ReleaseAll();
		was_active = false;
	}
	if (cursor_src)
		for (i = 0; i < TOUCHHUD_ENGINE_FINGERS - 1; i++)
			if ((int)multitouch[i][0] == cursor_src)
			{
				if (multitouch[i][1] * vid.mode.width != in_windowmouse_x || multitouch[i][2] * vid.mode.height != in_windowmouse_y)
					cursor_point(multitouch[i][1], multitouch[i][2]);
				break;
			}
	if ((cursor_state == CURSOR_PRESS_PENDING || cursor_state == CURSOR_TAP_PENDING) && frames_drawn != cursor_pos_frame)
	{
		Key_Event(K_MOUSE1, 0, true);
		if (cursor_state == CURSOR_TAP_PENDING)
		{
			Key_Event(K_MOUSE1, 0, false);
			cursor_state = CURSOR_IDLE;
		}
		else
			cursor_state = CURSOR_PRESSED;
	}
	return true;
}

void TouchHUD_Frame(void)
{
	int i, j, role;
	double now = host.realtime;
	float dt;
	float grace = cv("touch_finger_grace_ms", 120) * 0.001f;
	qbool hook_down = false, nade_down = false;
	qbool fire_count = false, fire2_down = false, zoom_down = false, jump_down = false, crouch_down = false, scores_down = false, move_active = false, look_seen = false;
	float look_dx = 0, look_dy = 0;
	qbool look_first = false;
	qbool desk = desk_active();
	static qbool was_desk;
	static int last_mode = -1, last_keyboard = -1, last_mouse = -1;
	int mode_now = hud_mode(), keyboard_now = (int)cv("vid_keyboard_detected", 0), mouse_now = (int)cv("vid_mouse_confirmed", 0);

	if (!desk && !TouchHUD_Active())
	{
		if (was_active)
			TouchHUD_ReleaseAll();
		was_active = false;
		return;
	}
	// Controls switched on or off under a finger: start over.
	if (was_active && desk != was_desk)
		TouchHUD_ReleaseAll();
	if (was_active && (mode_now != last_mode || keyboard_now != last_keyboard || mouse_now != last_mouse))
		TouchHUD_ReleaseAll();
	last_mode = mode_now; last_keyboard = keyboard_now; last_mouse = mouse_now;
	was_active = true;
	was_desk = desk;
	shapes_probe();

	dt = bound(1.0f / 240.0f, (float)(now - look_prev_time), 0.1f);

	// 1. match live contacts to the finger table by id; claim new ones
	for (i = 0; i < TOUCHHUD_ENGINE_FINGERS; i++)
	{
		int id = (int)multitouch[i][0];
		float px, py;
		finger_t *f = NULL;
		if (!id)
			continue;
		px = multitouch[i][1] * ui_w();
		py = multitouch[i][2] * ui_h();
		for (j = 0; j < MAX_FINGERS; j++)
			if (fingers[j].used && fingers[j].src == id)
				f = &fingers[j];
		if (!f)
		{
			for (j = 0; j < MAX_FINGERS; j++)
				if (!fingers[j].used)
				{
					f = &fingers[j];
					break;
				}
			if (!f)
				continue;
			// Pick the role before the slot is taken: a zeroed slot reads as
			// ROLE_MOVE (0), so role_owned() would find the new finger itself
			// and ignore every touch on the stick.
			role = assign_role(px, py);
			memset(f, 0, sizeof(*f));
			f->used = true;
			f->src = id;
			f->down_x = f->last_x = px;
			f->down_y = f->last_y = py;
			f->down_time = now;
			f->first_frame = true;
			f->role = role;
			if (f->role != ROLE_LOOK && f->role != ROLE_MOVE && f->role != ROLE_IGNORED)
				haptic_tick();
			if (f->role == ROLE_LOOK)
				look_begin();
			if (f->role == ROLE_MOVE)
			{
				// The stick stays fixed; an off-centre landing immediately moves relative to it.
				stick_held = true;
				stick_center(&stick.cx, &stick.cy);
			}
			if (f->role == ROLE_JUMP)
				hop_press_time = now;
			if (f->role == ROLE_WEPSTRIP)
			{
				char cmd[32];
				f->impulse = strip_hit(px, py);
				if (f->impulse >= 0)
				{
					dpsnprintf(cmd, sizeof(cmd), "impulse %d", f->impulse);
					exec_now(cmd);
					strip_selected_impulse = f->impulse;
				}
			}
			if (f->role == ROLE_WEPGRIP)
			{
				strip_t st;
				strip_layout(&st);
				f->grab_n = (int)(TouchStrip_Units(st.n, st.hook, st.nade) + 0.5f);
				f->grab_u = st.u;
			}
			if (f->role == ROLE_RELOAD)
				exec_now("weapon_reload");
			if (f->role == ROLE_TURN)
				TouchTurn_Start(); // on the press: nothing to undo
			if (f->role == ROLE_DODGE)
				TouchDash_Press(now); // Touch: the smart DASH (touch_dash.c): a dodge or a weapon boost
		}
		else
		{
			if (f->role == ROLE_LOOK && !f->first_frame)
			{
				look_dx += px - f->last_x;
				look_dy += py - f->last_y;
			}
			f->last_x = px;
			f->last_y = py;
			f->first_frame = false;
			if (f->role == ROLE_DODGE)
				TouchDash_Hold(px, py, now - f->down_time); // Touch: long-press picker
		}
		f->lost_time = 0;
	}

	// 2. contacts that vanished: keep them for the grace period, then release
	for (j = 0; j < MAX_FINGERS; j++)
	{
		finger_t *f = &fingers[j];
		qbool seen = false;
		if (!f->used)
			continue;
		for (i = 0; i < TOUCHHUD_ENGINE_FINGERS; i++)
			if ((int)multitouch[i][0] == f->src)
				seen = true;
		if (seen)
			continue;
		if (!f->lost_time)
			f->lost_time = now;
		if (now - f->lost_time < grace)
			continue;
		// released
		if (f->role == ROLE_DODGE)
			TouchDash_Release(f->last_x, f->last_y); // Touch: a tap acts on release
		if (f->role == ROLE_CON || f->role == ROLE_CHAT || f->role == ROLE_PAUSE)
			pill_release(f);
		else if (f->role == ROLE_TOSPEC || f->role == ROLE_READY)
			extra_release(f);
		else if (f->role == ROLE_HOPLOCK)
		{
			// a tap: lands on the chip and lifts on it; a finger that slid off
			// changes nothing, and one that started on HOP is never the chip's
			if (hoplock_shown() && hoplock_hit(f->last_x, f->last_y) && !TouchUI_IsSpectating() && cl.stats[STAT_HEALTH] > 0)
			{
				hop_latched = !hop_latched;
				if (cl_touch_aim_debug.integer)
					Con_Printf("touch: keep jumping %s\n", hop_latched ? "on" : "off");
			}
		}
		else if (f->role == ROLE_WEAPON)
			weapon_gesture(f, true);
		else if (f->role == ROLE_WEPGRIP && f->resized)
		{
			// Kept now, not only when the game next saves: Android may kill
			// the app without a clean exit.
			Host_SaveConfig(CONFIGFILENAME);
			Con_DPrintf("Touch HUD: weapon strip size %.2f saved\n", cl_touch_weplist_scale.value);
		}
		else if (f->role == ROLE_LOOK)
		{
			// tap in the look zone fires once (touch_look_tap_fire); not at
			// the desk, where the mouse fires
			float dx = f->last_x - f->down_x, dy = f->last_y - f->down_y;
			// nor spectating, where +attack is "follow" / "next player"
			if (!desk && !TouchUI_IsSpectating() && cv("touch_look_tap_fire", 1) && now - f->down_time < cv("touch_look_tap_ms", 200) * 0.001
				&& fabs(dx) < 12 && fabs(dy) < 12)
				tapfire_until = now + 0.08;
		}
		f->used = false;
	}

	// 3. roles -> outputs
	move_off_x = move_off_y = 0;
	for (j = 0; j < MAX_FINGERS; j++)
	{
		finger_t *f = &fingers[j];
		if (!f->used)
			continue;
		// a bar that was turned off under a held finger stops acting at once
		if (!TouchSpec_FingerActs(TouchUI_SpectatorState(), spec_bar_shown(), f->role))
			continue;
		switch (f->role)
		{
		case ROLE_MOVE:
			TouchStick_Read(&stick, f->last_x, f->last_y, stick_radius() * max(0.1f, cv("touch_stick_range", 1.0f)),
				false,
				cv("touch_stick_deadzone", 0.14f), cv("touch_stick_run", 0.6f), &stick_axis_x, &stick_axis_y, &move_off_x, &move_off_y);
			move_active = true;
			break;
		case ROLE_LOOK:
			look_seen = true;
			if (f->first_frame)
				look_first = true;
			break;
		case ROLE_FIRE:
			fire_count = true;
			button_drag_look(f, true, &look_seen, &look_dx, &look_dy);
			break;
		case ROLE_HOOK:
			hook_down = true;
			button_drag_look(f, true, &look_seen, &look_dx, &look_dy);
			break;
		case ROLE_NADE:
			// held: cooking; a drag off the row aims the throw, as FIRE does
			nade_down = true;
			button_drag_look(f, true, &look_seen, &look_dx, &look_dy);
			break;
		case ROLE_FIRE2:
			fire2_down = true;
			button_drag_look(f, false, &look_seen, &look_dx, &look_dy);
			break;
		case ROLE_ZOOM:
			zoom_down = true;
			button_drag_look(f, false, &look_seen, &look_dx, &look_dy);
			break;
		case ROLE_JUMP:
			jump_down = true;
			button_drag_look(f, false, &look_seen, &look_dx, &look_dy);
			break;
		case ROLE_RESPAWN:
			jump_down = true;
			break;
		case ROLE_CROUCH:
			crouch_down = true;
			button_drag_look(f, false, &look_seen, &look_dx, &look_dy);
			break;
		case ROLE_SCORES:
			scores_down = true;
			break;
		case ROLE_WEAPON:
			weapon_gesture(f, false);
			break;
		case ROLE_WEPGRIP:
		{
			// The grip follows the finger: the plate grows by twice the drag,
			// since it is anchored at its centre.
			float u_new = f->grab_u + 2 * (f->last_y - f->down_y) / (f->grab_n + WEP_GRIP_H);
			float scale = bound(WEP_SCALE_MIN, u_new / ui_scale(), WEP_SCALE_MAX);
			if (fabs(scale - cl_touch_weplist_scale.value) > 0.001f)
			{
				Cvar_SetValueQuick(&cl_touch_weplist_scale, scale);
				f->resized = true;
			}
			break;
		}
		default:
			break;
		}
	}

	// a finger on the look zone or FIRE is on the aim control: the only time
	// aim magnetism may pull (touch_aim.c)
	// spectating: the buttons' spectator meanings (touch_spectate.h)
	spec_frame(&fire_count, &fire2_down, &jump_down);

	TouchAim_SetHudAimTouch(!desk && (look_seen || fire_count));

	// The stick: an analog move (TouchHUD_MoveAxes adds it to the frame's
	// move), or the four movement keys with touch_stick_analog 0.
	if (!move_active)
	{
		stick_held = false;
		stick_axis_x = stick_axis_y = 0;
	}
	if (move_active && !cv("touch_stick_analog", 1))
	{
		set_key("forward", &key_forward, stick_axis_y < -0.38f);
		set_key("back", &key_back, stick_axis_y > 0.38f);
		set_key("moveleft", &key_left, stick_axis_x < -0.38f);
		set_key("moveright", &key_right, stick_axis_x > 0.38f);
		stick_axis_x = stick_axis_y = 0;
	}
	else
	{
		set_key("forward", &key_forward, false);
		set_key("back", &key_back, false);
		set_key("moveleft", &key_left, false);
		set_key("moveright", &key_right, false);
	}

	// look: every finger that turns the view adds up; when the last one has
	// lifted, what is still owed is paid
	if (!look_seen)
		look_end();
	else if (look_active && !look_first)
		look_apply(look_dx, look_dy, dt);
	look_prev_time = now;

	// fire: held button, or a look-zone tap; ALT is the secondary
	set_key("attack", &key_attack, fire_count || now < tapfire_until);
	set_key("attack2", &key_attack2, fire2_down);
	// touch_zoom_mode 1 toggles. A toggled zoom lets go when ZOOM goes away
	// (a switch to a weapon without a scope), or it would stay stuck on.
	{
		static qbool zoom_toggled, prev_zoom_down;
		if (cv("touch_zoom_mode", 0) == 1)
		{
			if (zoom_down && !prev_zoom_down)
				zoom_toggled = !zoom_toggled;
			if (!zoom_available())
				zoom_toggled = false;
			set_key("zoom", &key_zoom, zoom_toggled);
		}
		else
		{
			zoom_toggled = false;
			set_key("zoom", &key_zoom, zoom_down);
		}
		prev_zoom_down = zoom_down;
	}

	// hop: touch_hop_mode 1, hold past touch_hop_latch_ms latches bunny-hop and a
	// tap while latched clears it; 3, the keep-jumping chip sets and clears the
	// latch. A latch from one mode does not carry into another. Not for a
	// spectator: jump is "join" there, and a latch would carry into the first
	// life.
	{
		static int last_hop_mode = -1;
		int hop_mode = (int)cv("touch_hop_mode", HOP_CHIP);
		if (hop_mode != last_hop_mode)
		{
			hop_latched = false;
			hop_cancelling = false;
			last_hop_mode = hop_mode;
		}
	}
	if (TouchUI_IsSpectating())
	{
		hop_latched = false;
		hop_cancelling = false;
		set_key("jump", &key_jump, jump_down);
	}
	else if (cv("touch_hop_mode", HOP_CHIP) == 1)
	{
		static qbool prev_jump_down;
		if (jump_down && !prev_jump_down)
		{
			if (hop_latched)
			{
				hop_latched = false;
				hop_cancelling = true;
			}
		}
		if (!jump_down)
			hop_cancelling = false;
		if (jump_down && !hop_cancelling && !hop_latched && now - hop_press_time >= cv("touch_hop_latch_ms", 350) * 0.001)
			hop_latched = true;
		set_key("jump", &key_jump, hop_latched || (jump_down && !hop_cancelling));
		prev_jump_down = jump_down;
	}
	else if (cv("touch_hop_mode", HOP_CHIP) == HOP_CHIP)
	{
		// HOP is a plain button; only the chip sets and clears the latch. A
		// dead player drops it: held jump respawns.
		if (cl.stats[STAT_HEALTH] <= 0)
			hop_latched = false;
		set_key("jump", &key_jump, jump_down || hop_latched);
	}
	else
	{
		hop_latched = false;
		set_key("jump", &key_jump, jump_down);
	}

	set_key("crouch", &key_crouch, crouch_down);
	set_key("showscores", &key_scores, scores_down);
	// HOOK: held while the finger is down (touch_hook.c does the rest)
	TouchHook_SetHudButton(hook_down);
	TouchNade_SetHudButton(nade_down);
}

// ---------------------------------------------------------------------------
// Vitals readout (touch_hud.qc, condensed: no skin icons, same rows)
// ---------------------------------------------------------------------------
static void num_color(int value, int maxvalue, float *rgb)
{
	float frac = maxvalue > 0 ? (float)value / (float)maxvalue : 0;
	if (value <= 0 || frac <= 0.25f)
		VectorCopy(C_DANGER, rgb);
	else if (frac <= 0.5f)
		VectorCopy(C_WARNING, rgb);
	else if (frac < 1.0f)
		VectorCopy(C_INK, rgb);
	else
		VectorCopy(C_SUCCESS, rgb);
}

static void draw_bar(float lx, float cy, float w, float h, float frac, const float *fill, float a)
{
	float hx = w * 0.5f, hy = h * 0.5f, fw;
	frac = bound(0, frac, 1);
	shape_capsule(lx + hx, cy, hx, hy, C_SHADOW, a * A_SURFACE * glass_body(false));
	shape_capsule_ring(lx + hx, cy, hx, hy, C_RIM, a * A_GHOST);
	fw = w * frac;
	if (fw > h)
		shape_capsule(lx + fw * 0.5f, cy, fw * 0.5f, hy, fill, a * 0.92f);
	else if (frac > 0.001f)
		shape_disc(lx + hy, cy, hy, fill, a * 0.92f);
}

static void draw_stat_row(float ox, float oy, float hud_u, const char *icon_name, const char *tag, int value, int maxvalue, float a)
{
	float icon = 22 * hud_u, num_slot = 54 * hud_u, bar_w = 122 * hud_u, bar_h = 9 * hud_u, row_h = 30 * hud_u;
	float mid_y = oy + row_h * 0.5f;
	float rgb[3];
	char num[16];
	num_color(value, maxvalue, rgb);
	if (!skin_pic_fit(skin_pic(icon_name), ox + icon * 0.5f, mid_y, icon, icon, a * A_INK))
		shape_label(ox + icon * 0.5f, mid_y, 12 * hud_u, tag, rgb, a * A_INK);
	dpsnprintf(num, sizeof(num), "%d", value);
	shape_label_right(ox + icon + 6 * hud_u + num_slot, mid_y, FONT_READOUT * hud_u, num, rgb, a * A_INK);
	draw_bar(ox + icon + 6 * hud_u + num_slot + 12 * hud_u, mid_y, bar_w, bar_h, maxvalue > 0 ? (float)value / maxvalue : 0, rgb, a);
}

// Touch_Hud_DrawAmmoRow: the held weapon's ammo type icon and reserve, the
// clip and "/ reserve" for clip weapons, an infinity sign when it uses none.
static void draw_ammo_row(float ox, float oy, float hud_u, float a)
{
	float icon = 22 * hud_u, num_slot = 54 * hud_u, row_h = 30 * hud_u;
	float mid_y = oy + row_h * 0.5f, num_right = ox + icon + 6 * hud_u + num_slot;
	const float *rgb = C_INK;
	prvm_prog_t *prog = CLVM_prog;
	prvm_edict_t *ve = wepent();
	int held = held_weapon_index();
	int reserve = 0, clip = -1, clipmax = 0, shown;
	qbool infinite, clipped;
	char text[16];
	const char *icon_name = "ammo_shells";
	if (held < 0)
	{
		// No wepent to ask: the engine's own current-ammo stat, as before.
		reserve = max(0, cl.stats[STAT_AMMO]);
		infinite = false;
	}
	else
	{
		const wepdef_t *wd = &weapons[held];
		infinite = (cl.stats[STAT_ITEMS] & 1) || wd->ammo_stat < 0; // IT_UNLIMITED_AMMO
		if (wd->ammo_stat >= 0)
			reserve = max(0, cl.stats[wd->ammo_stat]);
		if (wd->ammo_icon[0])
			icon_name = wd->ammo_icon;
		if (ve)
		{
			clip = qcfield_int(prog, ve, f_clip_load);
			clipmax = qcfield_int(prog, ve, f_clip_size);
		}
	}
	clipped = !infinite && clipmax > 0 && clip >= 0;
	shown = clipped ? clip : reserve;
	if (infinite)
		dp_strlcpy(text, "\xE2\x88\x9E", sizeof(text)); // U+221E
	else
		dpsnprintf(text, sizeof(text), "%d", shown);
	if (!infinite)
	{
		qbool low = clipped ? (shown <= clipmax * 0.25f) : (shown < 10);
		if (shown <= 0)
			rgb = C_DANGER;
		else if (low)
			rgb = C_WARNING;
	}
	if (!infinite)
		skin_pic_fit(skin_pic(icon_name), ox + icon * 0.5f, mid_y, icon * 1.3f, icon * 1.3f, a * A_INK);
	shape_label_right(num_right, mid_y, FONT_READOUT * hud_u, text, rgb, a * A_INK);
	if (clipped)
	{
		float fs = FONT_READOUT * hud_u * 0.75f;
		char rest[24];
		dpsnprintf(rest, sizeof(rest), "/ %d", reserve);
		shape_label(num_right + 8 * hud_u + label_width(fs, rest) * 0.5f, mid_y, fs, rest, C_INK, a * A_INK * 0.8f);
	}
}

static void draw_vitals(float a)
{
	float hud_u = ui_scale() * max(0.4f, cv("touch_hud_scale", 0.85f));
	float w = (22 + 6 + 54 + 12 + 122) * hud_u;
	float h = (30 * 3 + 6 * 2) * hud_u;
	float cx, cy, ox, oy, pitch = (30 + 6) * hud_u;
	int health = max(0, cl.stats[STAT_HEALTH]);
	int armor = max(0, cl.stats[STAT_ARMOR]);
	if (!readouts_shown())
		return;
	widget_center(cv("touch_hud_x", 0.150f), cv("touch_hud_y", 0.115f), &cx, &cy);
	ox = cx - w * 0.5f;
	oy = cy - h * 0.5f;
	draw_stat_row(ox, oy, hud_u, "health", "HP", health, max(100, (int)cv("touch_hud_maxhealth", 100)), a);
	draw_stat_row(ox, oy + pitch, hud_u, "armor", "AR", armor, max(100, (int)cv("touch_hud_maxarmor", 100)), armor > 0 ? a : a * 0.7f);
	draw_ammo_row(ox, oy + pitch * 2, hud_u, a);
}

// ---------------------------------------------------------------------------
// Draw (touch_draw.qc)
// ---------------------------------------------------------------------------
static void draw_stick_at(float ox, float oy, qbool held, float a)
{
	float cx, cy, r, knob_r, knob_x, knob_y, mag;
	const float *knob_rgb = C_RIM;
	float knob_a = A_SURFACE_ACTIVE;
	qbool active;
	stick_center(&cx, &cy);
	r = stick_radius();
	if (r < 4)
		return;
	if (held && stick_held)
	{
		// Keep the visual ring fixed; only the knob follows the finger.
		cx = stick.cx;
		cy = stick.cy;
	}
	mag = sqrt(ox * ox + oy * oy);
	active = held && mag > cv("touch_stick_deadzone", 0.14f);
	knob_r = r * 0.34f;
	knob_x = cx + ox * (r - knob_r);
	knob_y = cy + oy * (r - knob_r);
	shape_shadow(cx, cy + elevation(r), r * 1.06f, a * A_SHADOW * 0.6f * glass_body(false));
	shape_disc(cx, cy, r, C_SURFACE, a * A_SURFACE * 0.75f * glass_body(false));
	widget_rim(cx, cy, r, C_RIM, a * A_RIM * 0.85f);
	if (active)
	{
		knob_rgb = C_ACCENT_LIGHT;
		knob_a = A_RIM_ACTIVE;
		shape_glow(knob_x, knob_y, knob_r * 1.6f, C_ACCENT, a * A_GLOW);
	}
	shape_shadow(knob_x, knob_y + elevation(knob_r), knob_r * 1.20f, a * A_SHADOW * glass_body(false));
	// The knob is what moves, so it keeps a body even when hollow.
	shape_disc(knob_x, knob_y, knob_r, knob_rgb, a * knob_a * glass_body(true));
	widget_rim(knob_x, knob_y, knob_r, C_RIM, a * A_RIM_ACTIVE);
}

static void draw_move_stick(float a)
{
	draw_stick_at(move_off_x, move_off_y, stick_held, a);
}

static float spec_lift; // screen share the watched player's buttons go up by

static void draw_circle_widget(const char *xk, float dx, const char *yk, float dy, const char *sk, float ds, const char *label, qbool pressed, float a, const float *accent)
{
	float cx, cy;
	float nx = cv(xk, dx);
	widget_center(!strcmp(xk, "touch_crouch_x") ? crouch_x(nx) : nx, cv(yk, dy), &cx, &cy);
	cy -= spec_lift * ui_h();
	widget_button(cx, cy, widget_radius(cv(sk, ds)), label, pressed, a, accent);
}

// Touch: what a tap on DASH would do, under the button, and the long-press
// picker (touch_dash.c decides; this only draws).
static void draw_dash_extras(float a)
{
	float cx, cy, r;
	const char *label;
	touch_dash_chip_t chips[10];
	int n, i;
	widget_center(cv("touch_dodge_x", 0.165f), cv("touch_dodge_y", 0.480f), &cx, &cy);
	r = widget_radius(cv("touch_dodge_size", 0.100f));
	TouchDash_NoteButton(cx, cy, r);
	label = TouchDash_Label();
	if (label[0] && !TouchDash_PickerOpen())
	{
		// as wide as the text and a margin each side
		float lhy = r * 0.32f, lhx = max(r * 0.95f, lhy * (0.8f + 0.27f * (float)strlen(label)));
		widget_capsule(cx, cy - r * 1.45f, lhx, lhy, label, false, false, a * 0.85f, C_ACCENT);
	}
	n = TouchDash_Chips(chips, 10);
	{
		touch_dash_rowlabel_t rl[2];
		int k, nl = TouchDash_RowLabels(rl, 2);
		for (k = 0; k < nl; k++)
			DrawQ_String(rl[k].x, rl[k].y - rl[k].size * 0.5f, rl[k].text, 0, rl[k].size, rl[k].size, C_INK[0], C_INK[1], C_INK[2], a * 0.8f, 0, NULL, false, FONT_CHAT);
	}
	for (i = 0; i < n; i++)
	{
		// the choice in force is a ring round the chip, never a mark over its text
		widget_capsule(chips[i].x, chips[i].y, chips[i].hx, chips[i].hy, chips[i].label, chips[i].hot, false, a, C_ACCENT);
		if (chips[i].current)
			shape_capsule_ring_stroke(chips[i].x, chips[i].y, chips[i].hx + chips[i].hy * 0.14f, chips[i].hy + chips[i].hy * 0.14f, chips[i].hy * 0.16f, C_ACCENT, a * A_RIM_ACTIVE);
	}
}

static void draw_pill(const char *xk, float dx, const char *yk, float dy, const char *sk, float ds, const char *ak, float da, const char *label, qbool pressed, float a)
{
	float cx, cy, hx, hy;
	widget_center(cv(xk, dx), cv(yk, dy), &cx, &cy);
	widget_half(cv(sk, ds), cv(ak, da), &hx, &hy);
	if (topbar_valid && (!strcmp(sk, "touch_pause_size") || !strcmp(sk, "touch_scores_size") || !strcmp(sk, "touch_chat_size")))
	{
		const touchbar_rect_t *r = !strcmp(sk, "touch_pause_size") ? &topbar.menu : !strcmp(sk, "touch_scores_size") ? &topbar.score : &topbar.chat;
		cx = r->x + r->w * 0.5f; cy = r->y + r->h * 0.5f; hx = r->w * 0.5f; hy = r->h * 0.5f;
	}
	widget_chrome_pill(cx, cy, hx, hy, label, pressed, a, C_ACCENT);
}

// The weapon strip with the touch controls off: play at a desk keeps its
// weapons in view (strip_drawn). Not while a touch CSQC is running, since it
// draws the strip itself. On a touchscreen the pills stay too (desk_active).
static void draw_chrome(float a);
static void draw_strip_only(void)
{
	float a = bound(0.1f, cv("touch_opacity", 0.90f), 1.0f);
	shapes_probe();
	if (desk_active() && desk_pills())
		draw_chrome(a * DESK_ALPHA);
	draw_weapon_strip(a);
}

// ---------------------------------------------------------------------------
// HUD modes (touch_hudmode.h, worked out in touch_hudmode.c): each mode has
// its own control set. Spectating is stripped to what a spectator uses: the
// move stick, look and the UP / DOWN rocker while free-flying, and one bar,
// previous | follow or free | next (touch_spectate.h). Joining is the swipe
// handle at the bottom middle, under any CSQC (below). At the end of the
// match (scoreboard, next-map vote) only MENU stays, in the top-left corner;
// the console button keeps the other.
// ---------------------------------------------------------------------------
// The spectator roles are defined above assign_role (300..304), next to the
// other roles; ROLE_TOSPEC 306 ("move to spectators", on release), ROLE_READY 307
// ("ready", on release) and ROLE_RESPAWN 308 (+jump while held, dead) are
// defined above assign_role.

static qbool key_moveup, key_movedown;
static int spec_last_state;
static double tospec_armed_until;   // the confirm tap's window
static double chrome_touch_time;    // last finger on the chrome, for the idle fade
static int chrome_mode_seen = -1;

static int hud_mode(void) { return TouchHudMode_Current()->mode; }
static int hud_flags(void) { return TouchHudMode_Current()->flags; }
static qbool hud_has(int ctl) { return (TouchMode_Controls(hud_mode(), hud_flags()) & ctl) != 0; }

// Foreign Xonotic CSQC builds the spectator prompts and pressed-key pictures
// itself, without our touch label hooks. Hide those two panels while a touch
// spectator has no keyboard; the engine's spectator bar supplies the actions.
qbool TouchHUD_HideStockKeyPanel(prvm_prog_t *prog)
{
	int i;
	if (prog != CLVM_prog || cl_touch_csqc_active || !TouchMode_IsSpectating(hud_mode())
		|| !(vid_touchscreen.integer || vid_touchscreen_detected.integer) || vid_keyboard_detected.integer)
		return false;
	for (i = -1; i < prog->depth; i++)
	{
		mfunction_t *f = i < 0 ? prog->xfunction : prog->stack[i].f;
		const char *name = f ? PRVM_GetString(prog, f->s_name) : "";
		if (strcmp(name, "HUD_PressedKeys") && TouchSpec_HideStockPanel(name, 1, 1, 0, 1))
		{
			static int reported;
			int bit = !strcmp(name, "HUD_InfoMessages") ? 1 : 2;
			if (developer.integer && !(reported & bit))
			{
				reported |= bit;
				Con_DPrintf("Touch HUD: suppressed stock %s\n", name);
			}
			return true;
		}
	}
	return false;
}
static qbool reduce_motion(void) { return cv("touch_reduce_motion", 0) != 0; }

static qbool spec_hud(void)
{
	// Before the join choice the observer HUD is not the player's screen: the
	// join dialog comes first, over the preview or as soon as observing starts.
	if (TouchHudMode_JoinWaiting())
		return false;
	return TouchHUD_Active() && TouchUI_SpectatorState() != TOUCHSPEC_NONE;
}

// The spectator controls remain available even with an older saved setting.
static qbool spec_bar_shown(void)
{
	return TouchSpec_BarShown(TouchUI_SpectatorState(), (int)cv("touch_specbar_visible", TOUCHSPEC_BAR_DEFAULT)) != 0;
}

static void spec_release(void)
{
	set_key("moveup", &key_moveup, false);
	set_key("movedown", &key_movedown, false);
}

// The spectator bar: three parts side by side, bottom right under the right
// thumb (mirrored left-handed). Secondary size, a 48 dp row.
static void spec_bar_layout_join(touchspec_bar_rect_t r[TOUCHSPEC_BAR_PARTS], qbool join)
{
	float u = dp_units(), bx, by, base;
	widget_center(cv("touch_specbar_x", 0.460f), cv("touch_specbar_y", 0.930f), &bx, &by);
	// without the touch HUD the stock health and ammo panels fill the bottom
	if (!vid_touchscreen.integer)
		by = min(by, ui_h() * 0.775f);
	base = max(TOUCHDP_SECONDARY * 0.5f * u, screen_min() * cv("touch_specbar_size", 0.085f) * 0.5f * widget_scale());
	{
		// the row ends where the lit DUCK button starts (QC Touch_Spec_BarFit)
		float lo = 0, hi = ui_w(), dx, dy, dr = widget_radius(cv("touch_crouch_size", 0.080f));
		int part;
		widget_center(crouch_x(cv("touch_crouch_x", 0.700f)), cv("touch_crouch_y", 0.885f), &dx, &dy);
		if (vid_touchscreen.integer && by + base > dy - dr && by - base < dy + dr)
		{
			if (dx > ui_w() * 0.5f) hi = dx - dr;
			else lo = dx + dr;
		}
		TouchSpec_BarLayout(hi - lo, ui_h(), bx - lo, by, base, u, join, r);
		for (part = 0; part < TOUCHSPEC_BAR_PARTS; part++)
			r[part].cx += lo;
	}
}

static void spec_bar_layout(touchspec_bar_rect_t r[TOUCHSPEC_BAR_PARTS])
{
	spec_bar_layout_join(r, spec_join_shown());
}

// The bar part under a point: the one test the drawn bar and the desk strip
// both use (spec_join_shown decides whether JOIN is part of the row).
static int spec_bar_part_at(float px, float py)
{
	touchspec_bar_rect_t r[TOUCHSPEC_BAR_PARTS];
	spec_bar_layout(r);
	return TouchSpec_BarPartAt(r, spec_join_shown(), px, py);
}

static void spec_bar_part(int part, float *cx, float *cy, float *hx, float *hy)
{
	touchspec_bar_rect_t r[TOUCHSPEC_BAR_PARTS];
	spec_bar_layout(r);
	if (part == TOUCHSPEC_BAR_JOIN && !spec_join_shown())
	{
		*cx = *cy = -1000;
		*hx = *hy = 0;
		return;
	}
	*cx = r[part].cx; *cy = r[part].cy; *hx = r[part].hx; *hy = r[part].hy;
}
// The fly rocker: UP over DOWN, beside the stick on its inner side.
static void spec_fly_part(qbool up, float *cx, float *cy, float *r)
{
	float u = dp_units(), mx, my, mr;
	stick_center(&mx, &my);
	mr = stick_radius();
	*r = TOUCHDP_MIN_TARGET * 0.5f * u;
	*cx = mx + (cv("touch_handedness", 0) ? -1 : 1) * (mr + *r * 1.6f);
	*cy = my + (up ? -1 : 1) * (*r * 1.15f);
}

static int spec_action_role(int action)
{
	switch (action)
	{
	case TOUCHSPEC_ACT_FOLLOW:
	case TOUCHSPEC_ACT_NEXT: return ROLE_SPEC_ATTACK;
	case TOUCHSPEC_ACT_PREV: return ROLE_SPEC_PREV;
	case TOUCHSPEC_ACT_FREE: return ROLE_SPEC_FREE;
	case TOUCHSPEC_ACT_UP: return ROLE_SPEC_UP;
	case TOUCHSPEC_ACT_DOWN: return ROLE_SPEC_DOWN;
	case TOUCHSPEC_ACT_JOIN: return ROLE_SPEC_JOIN;
	default: return ROLE_NONE;
	}
}

static int assign_spec_role(float px, float py)
{
	int state = TouchUI_SpectatorState(), part, role = pill_at(px, py);
	float cx, cy, r;
	if (role != ROLE_NONE)
		return role;
	if (in_edge_deadzone(px, py))
		return ROLE_IGNORED;
	if (spec_bar_shown())
	{
		part = spec_bar_part_at(px, py);
		if (part >= 0)
		{
			int action = TouchSpec_BarAction(state, part);
			return action != TOUCHSPEC_ACT_NONE ? spec_action_role(action) : ROLE_IGNORED;
		}
	}
	if (TouchSpec_StickShown(state))
	{
		int up;
		for (up = 0; up < 2; up++)
		{
			spec_fly_part(up, &cx, &cy, &r);
			if ((px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r * 1.1f)
				return up ? ROLE_SPEC_UP : ROLE_SPEC_DOWN;
		}
		if (!cv("vid_keyboard_detected", 0) && (in_move_zone(px, py)
			|| (cv("touch_move_visible", 1) && hit_circle(px, py, cv("touch_move_x", 0.170f), cv("touch_move_y", 0.680f), cv("touch_move_size", 0.400f), 1.0f))))
			return role_owned(ROLE_MOVE) ? ROLE_IGNORED : ROLE_MOVE;
	}
	return cv("vid_mouse_confirmed", 0) || role_owned(ROLE_LOOK) ? ROLE_IGNORED : ROLE_LOOK;
}

// The spectator roles' outputs for this frame, folded into TouchHUD_Frame's
// own: +attack (fire) and +attack2 (fire2) go out with the rest. Nothing a
// spectator touches sends +jump: that is the join, the swipe's alone.
static void spec_frame(qbool *fire, qbool *fire2, qbool *jump)
{
	int state = TouchUI_SpectatorState(), j;
	qbool up = false, down = false;
	(void)jump;
	if (state != spec_last_state)
	{
		Con_DPrintf("Touch HUD: spectator state %d -> %d (view %d of %d, frags %d)\n", spec_last_state, state, cl.viewentity, cl.realplayerentity,
			cl.scores && cl.realplayerentity >= 1 && cl.realplayerentity <= cl.maxclients ? cl.scores[cl.realplayerentity - 1].frags : 0);
		spec_last_state = state;
	}
	for (j = 0; j < MAX_FINGERS; j++)
	{
		finger_t *f = &fingers[j];
		touchspec_keys_t k;
		if (!f->used)
			continue;
		memset(&k, 0, sizeof(k));
		TouchSpec_AddFinger(&k, state, spec_bar_shown(), f->role, !f->fired);
		if (k.fire)
			*fire = true;
		if (k.fire2)
			*fire2 = true;
		if (k.up)
			up = true;
		if (k.down)
			down = true;
		if (k.prev)
		{
			exec_now("weapprev");
			f->fired = true;
		}
		if (k.join)
		{
			// the same entry point as MENU > Join game
			exec_now(cv("touch_join_dialog_auto", 1) != 0 ? "touch_join auto" : "touch_join_dialog");
			f->fired = true;
		}
	}
	set_key("moveup", &key_moveup, up && TouchSpec_StickShown(state));
	set_key("movedown", &key_movedown, down && TouchSpec_StickShown(state));
}

// The key to show for a command: a mouse button first when a mouse is there,
// a keyboard key otherwise (a tablet's keyboard cover has no mouse).
static const char *spec_key_name(const char *command, char *buf, size_t size)
{
	int keys[MAX_KEYS], j, key = -1;
	qbool mouse = cv("vid_mouse_confirmed", 0) != 0;
	const char *name;
	Key_FindKeysForCommand(command, keys, MAX_KEYS, -1);
	for (j = 0; j < MAX_KEYS && keys[j] >= 0; j++)
	{
		qbool is_mouse = keys[j] >= K_MOUSE1 && keys[j] < K_JOY1;
		if (keys[j] >= K_JOY1 || (is_mouse && !mouse))
			continue;
		if (key < 0 || (is_mouse && mouse && !(key >= K_MOUSE1 && key < K_JOY1)))
			key = keys[j];
	}
	if (key < 0)
		return NULL;
	if (key == K_MWHEELUP)
		return "WHEEL+";
	if (key == K_MWHEELDOWN)
		return "WHEEL-";
	if (key >= K_MOUSE1 && key < K_MOUSE1 + 9)
	{
		dpsnprintf(buf, size, "M%d", key - K_MOUSE1 + 1);
		return buf;
	}
	name = Key_KeynumToString(key, buf, size);
	if (name && name[0] >= 'a' && name[0] <= 'z' && !name[1])
	{
		char c = name[0] - 'a' + 'A';
		buf[0] = c;
		buf[1] = 0;
		name = buf;
	}
	// UPARROW and the like: the first word is enough
	if (name && strlen(name) > 5 && !strcmp(name + strlen(name) - 5, "ARROW"))
	{
		if (name != buf)
			dp_strlcpy(buf, name, size);
		buf[strlen(buf) - 5] = 0;
		name = buf;
	}
	return name;
}

// One glass keycap, the same material as the round buttons, lit when held.
static void spec_keycap(float cx, float cy, float hx, float hy, const char *name, qbool held, float a)
{
	const float *c = held ? C_ACCENT : C_SURFACE, *rim = held ? C_ACCENT : C_RIM;
	float r = hy * 0.38f, fs = label_size(hx, hy, name), w = label_width(fs, name);
	if (w > hx * 1.7f)
		fs *= hx * 1.7f / w;
	DrawQ_RoundedBox(cx, cy + hy * 0.10f, hx, hy, r, 0, C_SHADOW[0], C_SHADOW[1], C_SHADOW[2], a * A_SHADOW * glass_body(held), 0);
	DrawQ_RoundedBox(cx, cy, hx, hy, r, 0, c[0], c[1], c[2], a * (held ? A_SURFACE_ACTIVE : A_SURFACE) * glass_body(held), 0);
	DrawQ_RoundedBox(cx, cy, hx, hy, r, max(1.0f, hy * 0.07f), rim[0], rim[1], rim[2], a, 0);
	shape_label(cx, cy, fs, name, C_INK, a);
}

// The bound key as a small cap on a button's corner; nothing when unbound.
static void spec_keybadge(float cx, float cy, float half, const char *command, qbool held, float a)
{
	char buf[64];
	const char *name = spec_key_name(command, buf, sizeof(buf));
	float hx;
	if (!name || !name[0])
		return;
	hx = max(half, half * (0.45f + 0.36f * (float)strlen(name)));
	spec_keycap(cx, cy, hx, half, name, held, a);
}

static qbool spec_keys_shown(void)
{
	return touch_spectate_inputs.integer && (spec_test_keyboard >= 0 ? spec_test_keyboard != 0 : VID_HasKeyboard());
}

static void draw_spec_bar(int state, float a)
{
	int part;
	float cx, cy, hx, hy;
	const char *target = TouchHudMode_Target();
	for (part = 0; part < TOUCHSPEC_BAR_PARTS; part++)
	{
		int action = TouchSpec_BarAction(state, part);
		const char *label = part == TOUCHSPEC_BAR_MAIN ? TouchSpec_MainLabel(state, target) : TouchSpec_BarLabel(state, part);
		char fitted[128];
		qbool held = false;
		if (!label[0])
			continue;
		spec_bar_part(part, &cx, &cy, &hx, &hy);
		if (part == TOUCHSPEC_BAR_MAIN)
		{
			// a long name is cut to the pill, not run over the arrows
			size_t n;
			dp_strlcpy(fitted, label, sizeof(fitted));
			n = strlen(fitted);
			while (n > 3 && label_width(label_size(hx, hy, fitted), fitted) > hx * 2 * 0.92f)
			{
				do n--; while (n > 1 && (fitted[n] & 0xC0) == 0x80);
				dp_strlcpy(fitted + n, "..", sizeof(fitted) - n);
				if (label_width(label_size(hx, hy, fitted), fitted) <= hx * 2 * 0.92f)
					break;
				fitted[n] = 0;
			}
			label = fitted;
		}
		if (action == TOUCHSPEC_ACT_PREV)
			held = role_owned(ROLE_SPEC_PREV);
		else if (action == TOUCHSPEC_ACT_FREE)
			held = role_owned(ROLE_SPEC_FREE);
		else if (action == TOUCHSPEC_ACT_JOIN)
			held = role_owned(ROLE_SPEC_JOIN);
		else if (action != TOUCHSPEC_ACT_NONE)
			held = role_owned(ROLE_SPEC_ATTACK) && key_attack;
		widget_capsule(cx, cy, hx, hy, label, held || (part == TOUCHSPEC_BAR_MAIN && TouchSpec_Flying(state)), false, action == TOUCHSPEC_ACT_NONE ? a * 0.6f : a, C_ACCENT);
	}
}

static void draw_spec_hud(float a)
{
	int state = TouchUI_SpectatorState();
	if (TouchSpec_StickShown(state))
	{
		float cx, cy, r;
		if (cv("touch_move_visible", 1) && !cv("vid_keyboard_detected", 0))
			draw_move_stick(a);
		spec_fly_part(true, &cx, &cy, &r);
		widget_button(cx, cy, r, "UP", key_moveup, a * 0.9f, C_ACCENT);
		spec_fly_part(false, &cx, &cy, &r);
		widget_button(cx, cy, r, "DOWN", key_movedown, a * 0.9f, C_ACCENT);
	}
	if (spec_bar_shown())
		draw_spec_bar(state, a);
	// following someone: their health, armour and ammo, read only
	if ((state == TOUCHSPEC_FOLLOWING || state == TOUCHSPEC_OUT) && cl.stats[STAT_HEALTH] > 0)
		draw_vitals(a);
}

// ---------------------------------------------------------------------------
// At a desk (keyboard and mouse, controls off) the spectator bar stays as a
// quieter strip, clickable while the pointer is free (no grab: the web page
// before pointer lock, a released mouse). With the mouse grabbed the stock
// keys and buttons do the same: fire follows / next, weapprev previous,
// secondary fire free-fly.
// ---------------------------------------------------------------------------
static qbool desk_specbar_shown(void)
{
	return !vid_touchscreen.integer && in_play() && !cl.csqc_wantsmousemove && hud_has(TOUCHCTL_SPECBAR)
		&& !cv("_touch_owns_screen", 0) && cv("touch_desk_specbar", 1) != 0;
}

static void draw_desk_specbar(void)
{
	float a;
	if (!desk_specbar_shown())
		return;
	shapes_probe();
	a = bound(0.3f, cv("touch_opacity", 0.90f), 1.0f) * DESK_ALPHA;
	draw_spec_bar(TouchUI_SpectatorState(), a);
}

static void desk_specbar_click(float px, float py)
{
	int part, state = TouchUI_SpectatorState();
	float cx, cy, hx, hy;
	for (part = 0; part < TOUCHSPEC_BAR_PARTS; part++)
	{
		spec_bar_part(part, &cx, &cy, &hx, &hy);
		if (fabs(px - cx) > hx || fabs(py - cy) > hy)
			continue;
		Con_DPrintf("Touch HUD: spectator bar click, part %d, action %d\n", part, TouchSpec_BarAction(state, part));
		switch (TouchSpec_BarAction(state, part))
		{
		case TOUCHSPEC_ACT_FOLLOW:
		case TOUCHSPEC_ACT_NEXT: Cbuf_AddText(cmd_local, "+attack 50003\nwait\nwait\n-attack 50003\n"); break;
		case TOUCHSPEC_ACT_PREV: Cbuf_AddText(cmd_local, "weapprev\n"); break;
		case TOUCHSPEC_ACT_FREE: Cbuf_AddText(cmd_local, "+attack2 50003\nwait\nwait\n-attack2 50003\n"); break;
		default: break;
		}
		return;
	}
}

static qbool desk_specbar_hit(float px, float py)
{
	int part;
	float cx, cy, hx, hy;
	if (!desk_specbar_shown())
		return false;
	for (part = 0; part < TOUCHSPEC_BAR_PARTS; part++)
	{
		spec_bar_part(part, &cx, &cy, &hx, &hy);
		if (fabs(px - cx) <= hx && fabs(py - cy) <= hy)
			return true;
	}
	return false;
}

static qbool desk_bar_held;

// A real mouse's left button (not SDL's made-up one) on the desk spectator
// bar, while the pointer is free to point (no grab: the web page before
// pointer lock, a windowed game with the mouse released). True: eaten.
// (The name is from when the join handle was clickable too; joining is the
// join dialog now, touch_hudmode.c.)
qbool TouchHUD_JoinMouse(qbool down, int button, qbool synthetic, float wx, float wy, qbool pointer_free)
{
	float px, py;
	if (button != 1 || synthetic || vid.mode.width <= 0 || vid.mode.height <= 0)
		return false;
	px = wx / (float)vid.mode.width * ui_w();
	py = wy / (float)vid.mode.height * ui_h();
	if (down)
	{
		if (pointer_free && desk_specbar_hit(px, py))
		{
			desk_bar_held = true;
			return true;
		}
		return false;
	}
	if (desk_bar_held)
	{
		desk_bar_held = false;
		desk_specbar_click(px, py);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Match end: the fitted bar takes its fingers before the finger table and
// CSQC pointer, leaving vote cards to the game.
// ---------------------------------------------------------------------------
static struct { int id, role; qbool drawn; float cx, cy, hx, hy; } endmenu;
static qbool end_scores;

static qbool endmenu_shown(void)
{
	return TouchMode_IsMatchEnd(hud_mode()) && in_play() && touch_device()
		&& (vid_touchscreen.integer || cv("touch_desk_pills", 1));
}

static void endmenu_place(void)
{
	endmenu.cx = topbar.menu.x + topbar.menu.w * 0.5f;
	endmenu.cy = topbar.menu.y + topbar.menu.h * 0.5f;
	endmenu.hx = topbar.menu.w * 0.5f;
	endmenu.hy = topbar.menu.h * 0.5f;
}

static void draw_endmenu(void)
{
	float a = bound(0.3f, cv("touch_opacity", 0.90f), 1.0f);
	endmenu.drawn = false;
	if (!endmenu_shown() || !topbar_valid)
	{
		if (end_scores) { Cbuf_AddText(cmd_local, "-showscores\n"); end_scores = false; }
		return;
	}
	shapes_probe();
	endmenu_place();
	endmenu.drawn = true;
	if (cv("touch_pause_visible", 1))
		draw_pill("touch_pause_x", 0.655f, "touch_pause_y", 0.088f, "touch_pause_size", 0.060f, "touch_pause_aspect", 2.4f, "MENU", endmenu.id && endmenu.role == ROLE_PAUSE, a);
	if (cv("touch_scores_visible", 1))
		draw_pill("touch_scores_x", 0.772f, "touch_scores_y", 0.088f, "touch_scores_size", 0.060f, "touch_scores_aspect", 2.4f, "SCORE", end_scores || (endmenu.id && endmenu.role == ROLE_SCORES), a);
	if (cv("touch_chat_visible", 1))
		draw_pill("touch_chat_x", 0.889f, "touch_chat_y", 0.088f, "touch_chat_size", 0.060f, "touch_chat_aspect", 2.4f, "CHAT", endmenu.id && endmenu.role == ROLE_CHAT, a);
}

static qbool endmenu_finger(int type, int id, float nx, float ny)
{
	float px = nx * ui_w(), py = ny * ui_h();
	if (type == 0)
	{
		int role;
		if (endmenu.id || !endmenu.drawn || !endmenu_shown())
			return false;
		role = pill_at(px, py);
		if (role != ROLE_PAUSE && role != ROLE_CHAT && role != ROLE_SCORES)
			return false;
		endmenu.id = id;
		endmenu.role = role;
		return true;
	}
	if (id != endmenu.id || !id)
		return false;
	if (type == 2)
	{
		int role = endmenu.role;
		endmenu.id = 0;
		if (endmenu_shown() && pill_at(px, py) == role)
		{
			if (role == ROLE_SCORES)
			{
				end_scores = !end_scores;
				Cbuf_AddText(cmd_local, end_scores ? "+showscores\n" : "-showscores\n");
			}
			else
			{
				finger_t f;
				memset(&f, 0, sizeof(f));
				f.role = role;
				pill_release(&f);
			}
		}
	}
	return true;
}

static qbool viewstrip_finger(int type, int id, float nx, float ny);

// Real mouse clicks share the finger hit paths, in the same window-to-console
// coordinates as the pointer. Synthetic mouse events belong to the finger path.
qbool TouchHUD_OverlayMouse(qbool down, int button, qbool synthetic, float wx, float wy, qbool pointer_free)
{
	static qbool loading_held, menu_held;
	float nx, ny;
	if (button != 1 || synthetic || vid.mode.width <= 0 || vid.mode.height <= 0)
		return false;
	nx = wx / (float)vid.mode.width;
	ny = wy / (float)vid.mode.height;
	if (down)
	{
		float cx, cy, hx, hy;
		if (!pointer_free) return false;
		if (loading_cancellable())
		{
			loading_pill(&cx, &cy, &hx, &hy);
			if (fabs(nx * ui_w() - cx) <= hx * 1.2f && fabs(ny * ui_h() - cy) <= hy * 1.2f)
			{
				loading_held = true;
				loading_finger(-1, true, nx, ny);
				return true;
			}
		}
		if (endmenu_finger(0, -1, nx, ny) || viewstrip_finger(0, -1, nx, ny))
		{
			menu_held = true;
			return true;
		}
	}
	else if (loading_held)
	{
		loading_held = false;
		loading_finger(-1, false, nx, ny);
		return true;
	}
	else if (menu_held)
	{
		menu_held = false;
		if (!endmenu_finger(2, -1, nx, ny))
			viewstrip_finger(2, -1, nx, ny);
		return true;
	}
	return false;
}

// Every finger, from TouchVote_Finger before the finger table.
qbool TouchHUD_OverlayFinger(int type, int id, float nx, float ny)
{
	return endmenu_finger(type, id, nx, ny) || viewstrip_finger(type, id, nx, ny);
}

// ---------------------------------------------------------------------------
// Play extras: MOVE TO SPECTATORS (touch_tospec_*, placed in the layout
// editor, off by default: the MENU sheet has it too), READY in warmup, and
// RESPAWN in HOP's place while dead.
// ---------------------------------------------------------------------------
static qbool tospec_shown(void)
{
	return hud_has(TOUCHCTL_TOSPEC) && cv("touch_tospec_visible", 0);
}

static qbool tospec_hit(float px, float py)
{
	return hit_rect(px, py, cv("touch_tospec_x", 0.075f), cv("touch_tospec_y", 0.290f), cv("touch_tospec_size", 0.060f), cv("touch_tospec_aspect", 2.4f), 1.15f);
}

static void ready_place(float *cx, float *cy, float *hx, float *hy)
{
	float u = dp_units();
	*hy = TOUCHDP_PILL_HEIGHT * 0.5f * u;
	*hx = *hy * 2.6f;
	*cx = ui_w() * 0.5f;
	*cy = ui_h() - *hy - 10 * u;
}

static qbool ready_hit(float px, float py)
{
	float cx, cy, hx, hy, grow = TOUCHDP_MIN_TARGET * 0.5f * dp_units();
	ready_place(&cx, &cy, &hx, &hy);
	return hud_has(TOUCHCTL_READY) && fabs(px - cx) <= hx + grow * 0.2f && fabs(py - cy) <= max(hy, grow) + grow * 0.2f;
}

// On release, a tap: MOVE TO SPECTATORS asks for a second tap when it would
// forfeit the round or the objective carried (touch_hudmode.h).
static void extra_release(finger_t *f)
{
	if (fabs(f->last_x - f->down_x) > 24 || fabs(f->last_y - f->down_y) > 24)
		return;
	if (f->role == ROLE_READY && ready_hit(f->last_x, f->last_y))
		Cbuf_AddText(cmd_local, "ready\n");
	else if (f->role == ROLE_TOSPEC && tospec_hit(f->last_x, f->last_y))
	{
		const touchmode_t *m = TouchHudMode_Current();
		if (TouchAct_State(m, TOUCHACT_SPECTATE) == TOUCHACT_CONFIRM && host.realtime >= tospec_armed_until)
		{
			tospec_armed_until = host.realtime + 3.0;
			return;
		}
		tospec_armed_until = 0;
		Cbuf_AddText(cmd_local, "touch_action spectate confirm\n");
	}
}

static void draw_play_extras(float a)
{
	float cx, cy, hx, hy;
	if (tospec_shown())
	{
		qbool armed = host.realtime < tospec_armed_until;
		widget_center(cv("touch_tospec_x", 0.075f), cv("touch_tospec_y", 0.290f), &cx, &cy);
		widget_half(cv("touch_tospec_size", 0.060f), cv("touch_tospec_aspect", 2.4f), &hx, &hy);
		widget_chrome_pill(cx, cy, hx, hy, armed ? "SURE?" : "SPECTATE", role_owned(ROLE_TOSPEC) || armed, a, armed ? C_WARNING : C_ACCENT);
	}
	if (hud_has(TOUCHCTL_READY))
	{
		ready_place(&cx, &cy, &hx, &hy);
		widget_capsule(cx, cy, hx, hy, "READY", role_owned(ROLE_READY), false, a, C_SUCCESS);
	}
}

// The chrome (MENU, SCORE, CHAT) fades back while playing and untouched,
// so it does not sit bright over the fight; any finger on it, or a change of
// mode, brings it back.
static float chrome_alpha(float a)
{
	double idle;
	float t;
	if (hud_mode() != chrome_mode_seen)
	{
		chrome_mode_seen = hud_mode();
		chrome_touch_time = host.realtime;
	}
	if (role_owned(ROLE_PAUSE) || role_owned(ROLE_CHAT) || role_owned(ROLE_SCORES))
		chrome_touch_time = host.realtime;
	if (hud_mode() != TOUCHMODE_PLAYING || cv("touch_chrome_idle", 6) <= 0)
		return a;
	idle = host.realtime - chrome_touch_time - cv("touch_chrome_idle", 6);
	if (idle <= 0)
		return a;
	t = reduce_motion() ? 1 : (float)bound(0.0, idle / 0.4, 1.0);
	return a * (1 - 0.45f * t);
}

static void draw_chrome(float a)
{
	float ca = chrome_alpha(a);
	if (cv("touch_pause_visible", 1))
		draw_pill("touch_pause_x", 0.655f, "touch_pause_y", 0.088f, "touch_pause_size", 0.060f, "touch_pause_aspect", 2.4f, "MENU", role_owned(ROLE_PAUSE), ca);
	if (cv("touch_scores_visible", 1))
		draw_pill("touch_scores_x", 0.772f, "touch_scores_y", 0.088f, "touch_scores_size", 0.060f, "touch_scores_aspect", 2.4f, "SCORE", key_scores, ca);
	if (cv("touch_chat_visible", 1))
		draw_pill("touch_chat_x", 0.889f, "touch_chat_y", 0.088f, "touch_chat_size", 0.060f, "touch_chat_aspect", 2.4f, "CHAT", role_owned(ROLE_CHAT), ca);
	// The CONSOLE pill is off by default: the console button (touch_shellbtn.c)
	// is the console's. touch_con_visible 1 brings it back.
	if (cv("touch_con_visible", 0))
		draw_pill("touch_con_x", 0.090f, "touch_con_y", 0.948f, "touch_con_size", 0.060f, "touch_con_aspect", 2.9f, "CONSOLE", role_owned(ROLE_CON), ca);
}

// The bar's keys, drawn here under any CSQC: ours draws its own bar, same layout.
static void draw_spec_barkeys(void)
{
	touchspec_bar_rect_t r[TOUCHSPEC_BAR_PARTS];
	qbool join;
	int part, state = TouchUI_SpectatorState();
	float a = cv("touch_alpha", 1.0f) * (vid_touchscreen.integer ? 1 : DESK_ALPHA);
	if (!spec_keys_shown() || !hud_has(TOUCHCTL_SPECBAR) || Cvar_VariableValue(&cvars_all, "_touch_join_waiting", ~0))
		return;
	// our CSQC's bar always ends in JOIN
	join = cl_touch_csqc_active || spec_join_shown();
	spec_bar_layout_join(r, join);
	for (part = 0; part < TOUCHSPEC_BAR_PARTS; part++)
	{
		float half;
		int action = TouchSpec_BarAction(state, part);
		const char *command = action == TOUCHSPEC_ACT_PREV ? "weapprev" : action == TOUCHSPEC_ACT_FREE ? "+fire2"
			: action == TOUCHSPEC_ACT_JOIN ? "+jump" : "+fire";
		if (action == TOUCHSPEC_ACT_NONE || (part == TOUCHSPEC_BAR_JOIN && !join))
			continue;
		half = max(9.0f * ui_h() / 360.0f, r[part].hy * 0.42f);
		spec_keybadge(r[part].cx, r[part].cy - r[part].hy - half * 0.75f, half, command, false, a);
	}
}

static void draw_spectate_inputs(void)
{
	int mask, i;
	float u = ui_h() / 360.0f, lift;
	qbool keyboard;
	if (!in_play() || cl.intermission || cursor_wanted() || TouchChatLog_PanelHasInput())
		return;
	shapes_probe();
	draw_spec_barkeys();
	if (!spec_inputs_mask(&mask))
		return;
	if (spec_test_mask >= 0)
		mask = spec_test_mask;
	keyboard = spec_test_keyboard >= 0 ? spec_test_keyboard != 0 : VID_HasKeyboard();
	shapes_probe();
	spec_draw_count++;
	// the player's own controls, where they are in play, lit by what is held
	{
		float a = bound(0.3f, cv("touch_opacity", 0.90f), 1.0f), cx, cy, hx, hy;
		if (!vid_touchscreen.integer)
		{
			a *= DESK_ALPHA;
			spec_lift = 0.1f; // clear of the stock panels and kill feed
		}
		lift = spec_lift;
		if (!keyboard)
		{
			float dx = ((mask & 8) != 0) - ((mask & 4) != 0);
			float dy = ((mask & 2) != 0) - ((mask & 1) != 0);
			if (dx && dy)
			{
				dx *= 0.7071f;
				dy *= 0.7071f;
			}
			draw_stick_at(dx, dy, dx || dy, a);
		}
		if (keyboard)
		{
			// W A S D where the stick would be, inside its ring
			static const char *const move[4] = { "+forward", "+back", "+moveleft", "+moveright" };
			static const float kx[4] = { 0, 0, -1, 1 }, ky[4] = { -0.5f, 0.5f, 0.5f, 0.5f };
			float ring = widget_radius(cv("touch_move_size", 0.400f)), half = ring * 0.29f, pitch = ring * 0.64f;
			widget_center(cv("touch_move_x", 0.170f), cv("touch_move_y", 0.680f) - spec_lift, &cx, &cy);
			for (i = 0; i < 4; i++)
			{
				char buf[64];
				const char *name = spec_key_name(move[i], buf, sizeof(buf));
				spec_keycap(cx + kx[i] * pitch, cy + ky[i] * pitch, half, half, name ? name : "-", (mask & (1 << i)) != 0, a);
			}
		}
		draw_circle_widget("touch_fire_x", 0.855f, "touch_fire_y", 0.680f, "touch_fire_size", 0.160f, "FIRE", (mask & 64) != 0, a, C_DANGER);
		draw_circle_widget("touch_fire2_x", 0.770f, "touch_fire2_y", 0.755f, "touch_fire2_size", 0.100f, "ALT", (mask & 128) != 0, a, C_DANGER);
		draw_circle_widget("touch_crouch_x", 0.700f, "touch_crouch_y", 0.885f, "touch_crouch_size", 0.080f, "DUCK", (mask & 32) != 0, a, C_ACCENT);
		widget_center(cv("touch_jump_x", 0.855f), cv("touch_jump_y", 0.885f) - spec_lift, &cx, &cy);
		spec_lift = 0;
		widget_half(cv("touch_jump_size", 0.072f), cv("touch_jump_aspect", 2.6f), &hx, &hy);
		widget_capsule(cx, cy, hx, hy, "HOP", (mask & 16) != 0, false, a, C_ACCENT);
		if (keyboard)
		{
			static const char *const act[3] = { "+fire", "+fire2", "+crouch" };
			static const int bit[3] = { 64, 128, 32 };
			float half;
			half = max(9 * u, hy * 0.62f);
			spec_keybadge(cx - hx, cy - hy, half, "+jump", (mask & 16) != 0, a);
			for (i = 0; i < 3; i++)
			{
				float r;
				widget_center(i == 0 ? cv("touch_fire_x", 0.855f) : i == 1 ? cv("touch_fire2_x", 0.770f) : crouch_x(cv("touch_crouch_x", 0.700f)),
					(i == 0 ? cv("touch_fire_y", 0.680f) : i == 1 ? cv("touch_fire2_y", 0.755f) : cv("touch_crouch_y", 0.885f)) - lift, &cx, &cy);
				r = widget_radius(i == 0 ? cv("touch_fire_size", 0.160f) : i == 1 ? cv("touch_fire2_size", 0.100f) : cv("touch_crouch_size", 0.080f));
				half = max(9 * u, r * 0.36f);
				// DUCK's sits on top, clear of the bar beside it
				spec_keybadge(i == 2 ? cx : cx - r * 0.74f, cy - r * (i == 2 ? 0.95f : 0.74f), half, act[i], (mask & bit[i]) != 0, a);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// The view of the followed player: a small strip at the top, < view >.
// The choice is remembered (touch_spectate_view) and applied only while
// following; chase_active and friends go back when that ends. They are
// archived, so what they were is kept in an archived cvar meanwhile: a game
// killed while spectating puts them back on the next start.
// ---------------------------------------------------------------------------
static const char *const view_names[] = { "First person", "Third person", "Front", "Top down" };
#define VIEW_COUNT 4
static struct { int id, part; qbool drawn; float cx, cy, hx, hy; } viewstrip;
static qbool view_applied;

static qbool view_following(void)
{
	int state = TouchUI_SpectatorState();
	return in_play() && !cl.intermission && (state == TOUCHSPEC_FOLLOWING || state == TOUCHSPEC_OUT);
}

static void view_set(const char *name, int value)
{
	cvar_t *var = Cvar_FindVar(&cvars_all, name, ~0);
	if (var && var->integer != value)
		Cvar_SetValueQuick(var, value);
}

static void view_restore(void)
{
	int active = 0, front = 0, overhead = 0;
	if (touch_spectate_view_saved.string[0])
	{
		sscanf(touch_spectate_view_saved.string, "%d %d %d", &active, &front, &overhead);
		view_set("chase_active", max(0, active));
		view_set("chase_front", front);
		view_set("chase_overhead", overhead);
		Cvar_SetQuick(&touch_spectate_view_saved, "");
	}
	view_applied = false;
}

static void view_update(void)
{
	int view = (int)bound(0, touch_spectate_view.integer, VIEW_COUNT - 1);
	// "First person" is the game's own view, left alone
	if (!view_following() || !view)
	{
		if (view_applied || touch_spectate_view_saved.string[0])
			view_restore();
		return;
	}
	if (!view_applied)
	{
		char was[48];
		if (touch_spectate_view_saved.string[0])
			view_restore();
		dpsnprintf(was, sizeof(was), "%d %d %d", (int)cv("chase_active", 0), (int)cv("chase_front", 0), (int)cv("chase_overhead", 0));
		Cvar_SetQuick(&touch_spectate_view_saved, was);
		view_applied = true;
	}
	view_set("chase_active", 1);
	view_set("chase_front", view == 2);
	view_set("chase_overhead", view == 3);
}

static void view_step(int by)
{
	Cvar_SetValueQuick(&touch_spectate_view, (((int)bound(0, touch_spectate_view.integer, VIEW_COUNT - 1) + by) % VIEW_COUNT + VIEW_COUNT) % VIEW_COUNT);
}

static void TouchHUD_SpectateView_f(cmd_state_t *cmd)
{
	const char *arg = Cmd_Argc(cmd) > 1 ? Cmd_Argv(cmd, 1) : "";
	if (!strcmp(arg, "next"))
		view_step(1);
	else if (!strcmp(arg, "prev"))
		view_step(-1);
	Con_Printf("SPECTATE_VIEW %d \"%s\" following=%d applied=%d chase=%d front=%d top=%d strip=%d %.1f %.1f %.1f %.1f\n", touch_spectate_view.integer,
		view_names[(int)bound(0, touch_spectate_view.integer, VIEW_COUNT - 1)], view_following(), view_applied,
		(int)cv("chase_active", 0), (int)cv("chase_front", 0), (int)cv("chase_overhead", 0), viewstrip.drawn, viewstrip.cx, viewstrip.cy, viewstrip.hx, viewstrip.hy);
}

static qbool viewstrip_shown(void)
{
	return view_following() && !cursor_wanted() && !TouchChatLog_PanelHasInput() && !cv("_touch_owns_screen", 0)
		&& (vid_touchscreen.integer || vid_touchscreen_detected.integer || cv("touch_desk_specbar", 1) != 0);
}

static void viewstrip_place(void)
{
	float u = dp_units(), hud_u = ui_scale() * max(0.4f, cv("touch_hud_scale", 0.85f));
	float lo = 8 * u, hi = ui_w() - 8 * u, vx, vy, vw;
	viewstrip.hy = max(18 * u, ui_h() * 0.036f);
	viewstrip.hx = viewstrip.hy * 4.4f;
	viewstrip.cy = max(ui_h() * 0.042f + viewstrip.hy, topbar_valid ? topbar.menu.y + topbar.menu.h * 0.5f : 0); // below the timer
	// between the vitals and the MENU / SCORE / CHAT row
	widget_center(cv("touch_hud_x", 0.150f), cv("touch_hud_y", 0.115f), &vx, &vy);
	vw = (22 + 6 + 54 + 12 + 122) * hud_u * 0.5f;
	if (readouts_shown() || vid_touchscreen.integer)
	{
		if (vx < ui_w() * 0.5f) lo = max(lo, vx + vw + 8 * u);
		else hi = min(hi, vx - vw - 8 * u);
	}
	if (topbar_valid)
	{
		float l = min(topbar.menu.x, min(topbar.score.x, topbar.chat.x));
		float r = max(topbar.menu.x + topbar.menu.w, max(topbar.score.x + topbar.score.w, topbar.chat.x + topbar.chat.w));
		if (l > ui_w() * 0.5f) hi = min(hi, l - 8 * u);
		else if (r < ui_w() * 0.5f) lo = max(lo, r + 8 * u);
	}
	if (hi - lo < viewstrip.hx * 2)
	{
		// no room beside them: under the row, in the middle
		lo = 8 * u; hi = ui_w() - 8 * u;
		viewstrip.cy = max(viewstrip.cy + viewstrip.hy * 2.6f, vy + (30 * 3 + 6 * 2) * hud_u * 0.5f + viewstrip.hy + 6 * u);
		if (hi - lo < viewstrip.hx * 2)
		{
			viewstrip.hx = (hi - lo) * 0.5f;
			viewstrip.hy = viewstrip.hx / 4.4f;
		}
	}
	viewstrip.cx = bound(lo + viewstrip.hx, ui_w() * 0.5f, hi - viewstrip.hx);
}

static void draw_viewstrip(void)
{
	float a = bound(0.3f, cv("touch_opacity", 0.90f), 1.0f);
	viewstrip.drawn = false;
	if (!viewstrip_shown())
	{
		viewstrip.id = 0;
		return;
	}
	shapes_probe();
	viewstrip_place();
	viewstrip.drawn = true;
	{
		float hy = viewstrip.hy, cx = viewstrip.cx, cy = viewstrip.cy;
		float mid = viewstrip.hx - hy * 2.1f;
		widget_capsule(cx - viewstrip.hx + hy, cy, hy, hy, "<", viewstrip.id && viewstrip.part < 0, false, a, C_ACCENT);
		widget_capsule(cx + viewstrip.hx - hy, cy, hy, hy, ">", viewstrip.id && viewstrip.part > 0, false, a, C_ACCENT);
		widget_capsule(cx, cy, mid, hy * 0.82f, view_names[(int)bound(0, touch_spectate_view.integer, VIEW_COUNT - 1)], false, false, a * 0.9f, C_ACCENT);
	}
}

static int viewstrip_part(float px, float py)
{
	float dx = px - viewstrip.cx;
	if (fabs(py - viewstrip.cy) > viewstrip.hy * 1.3f || fabs(dx) > viewstrip.hx + viewstrip.hy * 0.3f)
		return 0;
	return dx < 0 ? -1 : 1; // the halves step: a thumb need not hit the arrow itself
}

static qbool viewstrip_finger(int type, int id, float nx, float ny)
{
	float px = nx * ui_w(), py = ny * ui_h();
	if (type == 0)
	{
		int part;
		if (viewstrip.id || !viewstrip.drawn || !viewstrip_shown() || !(part = viewstrip_part(px, py)))
			return false;
		viewstrip.id = id;
		viewstrip.part = part;
		return true;
	}
	if (id != viewstrip.id || !id)
		return false;
	if (type == 2)
	{
		viewstrip.id = 0;
		if (viewstrip_shown() && viewstrip_part(px, py) == viewstrip.part)
			view_step(viewstrip.part);
	}
	return true;
}

void TouchHUD_Draw(void)
{
	float a;
	float cx, cy, hx, hy;
	frames_drawn++;
	TouchHudMode_Update();
	{
		// The bar moved from under HOP and DUCK; an install that still holds
		// the old place follows, a hand-placed bar stays.
		static qbool moved;
		if (!moved && cls.state == ca_connected)
		{
			moved = true;
			if (fabs(cv("touch_specbar_x", 0) - 0.815f) < 0.0005f && fabs(cv("touch_specbar_y", 0) - 0.885f) < 0.0005f)
			{
				panel_cvar("touch_specbar_x", "0.460");
				panel_cvar("touch_specbar_y", "0.930");
			}
		}
	}
	view_update();
	draw_spectate_inputs();
	draw_viewstrip();
	TouchDodge_DrawCue(); // over the port's CSQC HUD too
	draw_endmenu();
	if (TouchMode_IsMatchEnd(hud_mode()))
		return;
	if (cursor_wanted())
	{
		if (!cursor_pills_shown())
			return;
		shapes_probe();
		a = bound(0.1f, cv("touch_opacity", 0.90f), 1.0f);
		if (!vid_touchscreen.integer)
			a *= DESK_ALPHA;
		if (cv("touch_pause_visible", 1))
			draw_pill("touch_pause_x", 0.655f, "touch_pause_y", 0.088f, "touch_pause_size", 0.060f, "touch_pause_aspect", 2.4f, "MENU", cursor_pill_src && cursor_pill.role == ROLE_PAUSE, a);
		if (cv("touch_chat_visible", 1))
			draw_pill("touch_chat_x", 0.889f, "touch_chat_y", 0.088f, "touch_chat_size", 0.060f, "touch_chat_aspect", 2.4f, "CHAT", cursor_pill_src && cursor_pill.role == ROLE_CHAT, a);
		return;
	}
	if (!TouchHUD_Active())
	{
		draw_strip_only();
		draw_desk_specbar();
		return;
	}
	shapes_probe();
	a = bound(0.1f, cv("touch_opacity", 0.90f), 1.0f);

	draw_chrome(a);
	if (spec_hud())
	{
		draw_spec_hud(a);
		return;
	}
	if (hud_mode() == TOUCHMODE_DEAD)
	{
		// Waiting to respawn: RESPAWN where HOP is, the extras, nothing to fight with.
		widget_center(cv("touch_jump_x", 0.855f), cv("touch_jump_y", 0.885f), &cx, &cy);
		widget_half(cv("touch_jump_size", 0.072f), cv("touch_jump_aspect", 2.6f), &hx, &hy);
		widget_capsule(cx, cy, hx * 1.25f, hy, "RESPAWN", key_jump, false, a, C_SUCCESS);
		draw_play_extras(a);
		return;
	}

	if (cv("touch_move_visible", 1) && !cv("vid_keyboard_detected", 0))
		draw_move_stick(a);
	if (cv("touch_fire_visible", 1) && !cv("vid_mouse_confirmed", 0))
		draw_circle_widget("touch_fire_x", 0.855f, "touch_fire_y", 0.680f, "touch_fire_size", 0.160f, "FIRE", key_attack || TouchAim_AutoFiring(), a, C_DANGER);
	if (cv("touch_jump_visible", 1))
	{
		widget_center(cv("touch_jump_x", 0.855f), cv("touch_jump_y", 0.885f), &cx, &cy);
		widget_half(cv("touch_jump_size", 0.072f), cv("touch_jump_aspect", 2.6f), &hx, &hy);
		widget_capsule(cx, cy, hx, hy, "HOP", key_jump && !hop_latched, hop_latched, a, C_ACCENT);
	}
	if (hoplock_shown())
	{
		widget_center(cv("touch_hoplock_x", 0.675f), cv("touch_hoplock_y", 0.760f), &cx, &cy);
		widget_half(cv("touch_hoplock_size", 0.052f), 1, &hx, &hy);
		widget_capsule(cx, cy, hx, hy, "KEEP", role_owned(ROLE_HOPLOCK), hop_latched, a, C_ACCENT);
	}
	if (cv("touch_crouch_visible", 1))
		draw_circle_widget("touch_crouch_x", 0.700f, "touch_crouch_y", 0.885f, "touch_crouch_size", 0.080f, "DUCK", key_crouch, a, C_ACCENT);
	if (weapon_button_shown())
		draw_circle_widget("touch_weapon_x", 0.540f, "touch_weapon_y", 0.860f, "touch_weapon_size", 0.100f, "WEP", role_owned(ROLE_WEAPON), a, C_ACCENT);
	// ALT fades out rather than vanishing, so the eye sees why it went; it
	// takes no new touch from the moment it is unavailable (assign_role).
	// ZOOM does the same for weapons with a scope.
	{
		static double fade_time;
		float fade_dt = (float)bound(0.0, host.realtime - fade_time, 0.1);
		fade_time = host.realtime;
		fire2_fade = cv("touch_fire2_visible", 2) > 0 ? TouchAltFire_Fade(fire2_fade, fire2_available(), fade_dt) : 0;
		zoom_fade = cv("touch_zoom_visible", 2) > 0 ? TouchAltFire_Fade(zoom_fade, zoom_available() || key_zoom, fade_dt) : 0;
		if (fire2_fade > 0 && !cv("vid_mouse_confirmed", 0))
			draw_circle_widget("touch_fire2_x", 0.770f, "touch_fire2_y", 0.755f, "touch_fire2_size", 0.100f, "ALT", key_attack2, a * fire2_fade, C_DANGER);
		if (zoom_fade > 0)
			draw_circle_widget("touch_zoom_x", 0.860f, "touch_zoom_y", 0.480f, "touch_zoom_size", 0.100f, "ZOOM", key_zoom, a * zoom_fade, C_ACCENT);
	}
	if (TouchTurn_ButtonShown())
		draw_circle_widget("touch_turn_x", 0.300f, "touch_turn_y", 0.840f, "touch_turn_size", 0.085f, "TURN", TouchTurn_Spinning() || role_owned(ROLE_TURN), a, C_ACCENT);
	if (cv("touch_reload_visible", 0))
		draw_circle_widget("touch_reload_x", 0.720f, "touch_reload_y", 0.480f, "touch_reload_size", 0.100f, "RLD", role_owned(ROLE_RELOAD), a, C_ACCENT);
	if (cv("touch_dodge_visible", 0))
	{
		draw_circle_widget("touch_dodge_x", 0.165f, "touch_dodge_y", 0.480f, "touch_dodge_size", 0.100f, "DASH", role_owned(ROLE_DODGE), a, C_ACCENT);
		draw_dash_extras(a); // Touch: indicator and picker
	}

	// Readouts draw only for a live player, like Touch_Hud_Draw.
	if (cl.stats[STAT_HEALTH] > 0)
	{
		draw_vitals(a);
		draw_weapon_strip(a);
	}
	draw_play_extras(a);
}

// ---------------------------------------------------------------------------
// touch_hud_state rects: every control this HUD has on screen in the mode it
// is in, and the game's own panels near the top bar, as lines the tests read
// (scripts/test-touch-controls-live.sh asserts that none intersect):
//   TOUCHRECT r <name> <x> <y> <w> <h>     a box, console units
//   TOUCHRECT c <name> <cx> <cy> <radius>  a circle
// ---------------------------------------------------------------------------
static void rect_box(const char *name, float cx, float cy, float hx, float hy)
{
	Con_Printf("TOUCHRECT r %s %.1f %.1f %.1f %.1f\n", name, cx - hx, cy - hy, hx * 2, hy * 2);
}

static void rect_pill(const char *name, const char *xk, float dx, const char *yk, float dy, const char *sk, float ds, const char *ak, float da)
{
	float cx, cy, hx, hy;
	widget_center(cv(xk, dx), cv(yk, dy), &cx, &cy);
	widget_half(cv(sk, ds), cv(ak, da), &hx, &hy);
	if (topbar_valid && (!strcmp(sk, "touch_pause_size") || !strcmp(sk, "touch_scores_size") || !strcmp(sk, "touch_chat_size")))
	{
		const touchbar_rect_t *r = !strcmp(sk, "touch_pause_size") ? &topbar.menu : !strcmp(sk, "touch_scores_size") ? &topbar.score : &topbar.chat;
		cx = r->x + r->w * 0.5f; cy = r->y + r->h * 0.5f; hx = r->w * 0.5f; hy = r->h * 0.5f;
	}
	rect_box(name, cx, cy, hx, hy);
}

static void rect_circle(const char *name, const char *xk, float dx, const char *yk, float dy, const char *sk, float ds)
{
	float cx, cy;
	float nx = cv(xk, dx);
	widget_center(!strcmp(xk, "touch_crouch_x") ? crouch_x(nx) : nx, cv(yk, dy), &cx, &cy);
	Con_Printf("TOUCHRECT c %s %.1f %.1f %.1f\n", name, cx, cy, widget_radius(cv(sk, ds)));
}

static void rect_panel(const char *panel)
{
	char name[64];
	float x = 0, y = 0, w = 0, h = 0;
	dpsnprintf(name, sizeof(name), "hud_panel_%s", panel);
	if (!Cvar_VariableValue(&cvars_all, name, ~0))
		return;
	dpsnprintf(name, sizeof(name), "hud_panel_%s_pos", panel);
	sscanf(Cvar_VariableString(&cvars_all, name, ~0), "%f %f", &x, &y);
	dpsnprintf(name, sizeof(name), "hud_panel_%s_size", panel);
	sscanf(Cvar_VariableString(&cvars_all, name, ~0), "%f %f", &w, &h);
	Con_Printf("TOUCHRECT r panel_%s %.1f %.1f %.1f %.1f\n", panel, x * ui_w(), y * ui_h(), w * ui_w(), h * ui_h());
}

void TouchHUD_PrintRects(void)
{
	float cx, cy, hx, hy, r, x, y, w, h;
	int mode = hud_mode(), part, state = TouchUI_SpectatorState();
	Con_Printf("TOUCHRECTS mode=%s size=%.0fx%.0f dp=%.3f csqc_hud=%d\n", TouchMode_Name(mode), ui_w(), ui_h(), dp_units(), cl_touch_csqc_active);
	if (mode == TOUCHMODE_OFF)
		return;
	if (sscanf(Cvar_VariableString(&cvars_all, "_touch_shell_rect", ~0), "%f %f %f %f", &x, &y, &w, &h) == 4)
		Con_Printf("TOUCHRECT r console %.1f %.1f %.1f %.1f\n", x * ui_w(), y * ui_h(), w * ui_w(), h * ui_h());
	if (TouchMode_IsMatchEnd(mode))
	{
		if (endmenu_shown())
		{
			endmenu_place();
			rect_box("MENU", endmenu.cx, endmenu.cy, endmenu.hx, endmenu.hy);
		}
		return;
	}
	// a menu, the console or the CSQC's own pointer UI has the screen: no controls are drawn
	if (!vid_touchscreen.integer || !TouchHUD_Active())
		return;
	if (cv("touch_pause_visible", 1))
		rect_pill("MENU", "touch_pause_x", 0.655f, "touch_pause_y", 0.088f, "touch_pause_size", 0.060f, "touch_pause_aspect", 2.4f);
	if (cv("touch_scores_visible", 1))
		rect_pill("SCORE", "touch_scores_x", 0.772f, "touch_scores_y", 0.088f, "touch_scores_size", 0.060f, "touch_scores_aspect", 2.4f);
	if (cv("touch_chat_visible", 1))
		rect_pill("CHAT", "touch_chat_x", 0.889f, "touch_chat_y", 0.088f, "touch_chat_size", 0.060f, "touch_chat_aspect", 2.4f);
	if (TouchHUD_TopBarRect(1, &x, &y, &w, &h))
		Con_Printf("TOUCHRECT r votechip %.1f %.1f %.1f %.1f\n", x, y, w, h);
	rect_panel("score");
	rect_panel("timer");
	rect_panel("infomessages");
	if (state != TOUCHSPEC_NONE)
	{
		for (part = 0; spec_bar_shown() && part < TOUCHSPEC_BAR_PARTS; part++)
		{
			char name[16];
			if (!TouchSpec_BarLabel(state, part)[0])
				continue;
			spec_bar_part(part, &cx, &cy, &hx, &hy);
			dpsnprintf(name, sizeof(name), "specbar%d", part);
			rect_box(name, cx, cy, hx, hy);
		}
		if (viewstrip.drawn)
			rect_box("viewstrip", viewstrip.cx, viewstrip.cy, viewstrip.hx, viewstrip.hy);
		{
			int mask;
			if (spec_inputs_mask(&mask))
			{
				if (!cv("vid_keyboard_detected", 0))
				{
					stick_center(&cx, &cy);
					Con_Printf("TOUCHRECT c stick %.1f %.1f %.1f\n", cx, cy, stick_radius());
				}
				rect_circle("FIRE", "touch_fire_x", 0.855f, "touch_fire_y", 0.680f, "touch_fire_size", 0.160f);
				rect_circle("ALT", "touch_fire2_x", 0.770f, "touch_fire2_y", 0.755f, "touch_fire2_size", 0.100f);
				rect_circle("DUCK", "touch_crouch_x", 0.700f, "touch_crouch_y", 0.885f, "touch_crouch_size", 0.080f);
				rect_pill("HOP", "touch_jump_x", 0.855f, "touch_jump_y", 0.885f, "touch_jump_size", 0.072f, "touch_jump_aspect", 2.6f);
			}
		}
		if (TouchSpec_StickShown(state))
		{
			stick_center(&cx, &cy);
			Con_Printf("TOUCHRECT c stick %.1f %.1f %.1f\n", cx, cy, stick_radius());
			spec_fly_part(true, &cx, &cy, &r);
			Con_Printf("TOUCHRECT c UP %.1f %.1f %.1f\n", cx, cy, r);
			spec_fly_part(false, &cx, &cy, &r);
			Con_Printf("TOUCHRECT c DOWN %.1f %.1f %.1f\n", cx, cy, r);
		}
		return;
	}
	if (mode == TOUCHMODE_DEAD)
	{
		widget_center(cv("touch_jump_x", 0.855f), cv("touch_jump_y", 0.885f), &cx, &cy);
		widget_half(cv("touch_jump_size", 0.072f), cv("touch_jump_aspect", 2.6f), &hx, &hy);
		rect_box("RESPAWN", cx, cy, hx * 1.25f, hy);
		return;
	}
	if (cv("touch_move_visible", 1) && !cv("vid_keyboard_detected", 0))
	{
		stick_center(&cx, &cy);
		Con_Printf("TOUCHRECT c stick %.1f %.1f %.1f\n", cx, cy, stick_radius());
	}
	if (cv("touch_fire_visible", 1) && !cv("vid_mouse_confirmed", 0))
		rect_circle("FIRE", "touch_fire_x", 0.855f, "touch_fire_y", 0.680f, "touch_fire_size", 0.160f);
	if (cv("touch_fire2_visible", 2) > 0 && !cv("vid_mouse_confirmed", 0))
		rect_circle("ALT", "touch_fire2_x", 0.770f, "touch_fire2_y", 0.755f, "touch_fire2_size", 0.100f);
	if (cv("touch_jump_visible", 1))
		rect_pill("HOP", "touch_jump_x", 0.855f, "touch_jump_y", 0.885f, "touch_jump_size", 0.072f, "touch_jump_aspect", 2.6f);
	if (hoplock_shown())
	{
		widget_center(cv("touch_hoplock_x", 0.675f), cv("touch_hoplock_y", 0.760f), &cx, &cy);
		widget_half(cv("touch_hoplock_size", 0.052f), 1, &hx, &hy);
		rect_box("KEEP", cx, cy, hx, hy);
	}
	if (cv("touch_crouch_visible", 1))
		rect_circle("DUCK", "touch_crouch_x", 0.700f, "touch_crouch_y", 0.885f, "touch_crouch_size", 0.080f);
	if (TouchTurn_ButtonShown())
		rect_circle("TURN", "touch_turn_x", 0.300f, "touch_turn_y", 0.840f, "touch_turn_size", 0.085f);
	if (cv("touch_zoom_visible", 2) > 0)
		rect_circle("ZOOM", "touch_zoom_x", 0.860f, "touch_zoom_y", 0.480f, "touch_zoom_size", 0.100f);
	if (readouts_shown())
	{
		float hud_u = ui_scale() * max(0.4f, cv("touch_hud_scale", 0.85f));
		widget_center(cv("touch_hud_x", 0.150f), cv("touch_hud_y", 0.115f), &cx, &cy);
		rect_box("vitals", cx, cy, (22 + 6 + 54 + 12 + 122) * hud_u * 0.5f, (30 * 3 + 6 * 2) * hud_u * 0.5f);
	}
	if (strip_drawn())
	{
		strip_t st;
		strip_layout(&st);
		rect_box("weapons", st.cx, st.cy, st.half_w, st.half_h);
	}
}

// ---------------------------------------------------------------------------
// Read-only weapon state for touch auto-shoot (touch_aim.c), from the same
// binding of the loaded CSQC the weapon strip uses, so it holds under the
// port's CSQC and a public server's alike.
// ---------------------------------------------------------------------------
int TouchHUD_NumWeapons(void)
{
	return num_weapons;
}

const char *TouchHUD_WeaponName(int index)
{
	return index >= 0 && index < num_weapons ? weapons[index].name : "";
}

qbool TouchHUD_WeaponOwned(int index)
{
	return index >= 0 && index < num_weapons && weapon_owned(index);
}

int TouchHUD_WeaponAmmo(int index)
{
	const wepdef_t *wd;
	if (index < 0 || index >= num_weapons)
		return 0;
	wd = &weapons[index];
	if (wd->ammo_stat < 0 || (cl.stats[STAT_ITEMS] & 1)) // IT_UNLIMITED_AMMO
		return -1;
	return max(0, cl.stats[wd->ammo_stat]);
}

int TouchHUD_HeldWeapon(void)
{
	return held_weapon_index();
}

// The weapon in hand once a switch has landed (the wepent's activeweapon;
// switchweapon changes as soon as the server takes the request).
int TouchHUD_ActiveWeapon(void)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_edict_t *ve = wepent(), *we;
	int id, i;
	if (!ve || f_activeweapon.ofs < 0)
		return -1;
	we = edict_or_null(prog, PRVM_EDICTFIELDEDICT(ve, f_activeweapon.ofs));
	if (!we)
		return -1;
	id = qcfield_int(prog, we, f_wep_id);
	for (i = 0; i < num_weapons; i++)
		if (weapons[i].id == id)
			return i;
	return -1;
}

int TouchHUD_HeldClip(void)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_edict_t *ve = wepent();
	if (!ve || qcfield_int(prog, ve, f_clip_size) <= 0)
		return -1;
	return qcfield_int(prog, ve, f_clip_load);
}

int TouchHUD_AltFireState(void)
{
	return altfire_state();
}

// Stock Xonotic CSQC (including a server-provided one) draws these skin
// pictures as its pointer. SDL supplies the pointer on this platform.
qbool TouchHUD_HideCursorPic(const char *picname)
{
	return TouchCursor_HidePic(picname, vid_touchscreen.integer, vid_pointer_mouse.integer,
		Cvar_VariableValue(&cvars_all, "vid_system_cursor", ~0) != 0);
}

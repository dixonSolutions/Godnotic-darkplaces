#ifndef TOUCH_DASH_H
#define TOUCH_DASH_H

#include "qtypes.h"

// Godnotic: the smart DASH button -- a dodge, or a weapon boost (a rocket or
// blaster shot at the floor, a wall or behind the player) that launches them
// up or forward, chosen by where they are going. Decisions in touch_dashplan.h
// (pure, tested by scripts/test-touch-dash.sh); this file reads the client's
// state, runs the boost (switch weapon, aim, fire, give the view and the weapon
// back) and learns from what each one really did. See docs/CONTROLS.md.

// Cvars and commands (from TouchHook_Init).
void TouchDash_Init(void);
// A CSQC was loaded or unloaded: what is learned about the server is dropped.
void TouchDash_ProgsChanged(void);
// Once per client frame from the hook's frame, before the aim helpers.
void TouchDash_Frame(void);
// Button bits for the outgoing move: 1 (attack) for the frame the boost fires.
int TouchDash_Buttons(void);
// A boost is under way (switching, aiming, firing, giving the view and weapon
// back): auto-shoot and auto-switch stand down.
qbool TouchDash_Busy(void);

// The engine HUD's DASH button. NoteButton is called from the draw with the
// button's centre and radius (UI units); Press, Hold and Release from its finger.
void TouchDash_NoteButton(float cx, float cy, float r);
void TouchDash_Press(double now);
void TouchDash_Hold(float px, float py, double held);
void TouchDash_Release(float px, float py);

// The long-press picker: direction and weapon chips. A chip is selected by
// letting go over it.
typedef struct touch_dash_chip_s
{
	char label[16];
	float x, y, hx, hy;   // centre and half size, UI units
	qbool current;        // the choice in force
	qbool hot;            // the finger is over it
} touch_dash_chip_t;
// A small caption above each picker row ("Direction", "Weapon"), left edge x, middle y, text height.
typedef struct touch_dash_rowlabel_s
{
	char text[16];
	float x, y, size;
} touch_dash_rowlabel_t;
int TouchDash_RowLabels(touch_dash_rowlabel_t *out, int max);
qbool TouchDash_PickerOpen(void);
int TouchDash_Chips(touch_dash_chip_t *out, int max);

// The indicator under the button: what a tap would do ("BLA UP", "DODGE"),
// "" when there is nothing to show. TouchDash_Shown: the weapon part is on.
const char *TouchDash_Label(void);
qbool TouchDash_Shown(void);

#endif

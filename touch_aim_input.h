// Input-independent aim engagement: a physical turn or either fire button
// counts alongside the touch HUD, including when the controls are visible.
static qbool TouchAim_InputActive(qbool hud, qbool touchscreen, qbool mouselook, float turn, qbool manual)
{
	return hud || !touchscreen || mouselook || turn > 0 || manual;
}

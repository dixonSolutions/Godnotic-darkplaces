// Input-independent aim engagement: a physical turn or either fire button
// counts alongside the touch HUD, including when the controls are visible.
static qbool TouchAim_InputActive(qbool hud, qbool touchscreen, qbool mouselook, float turn, qbool manual)
{
	return hud || !touchscreen || mouselook || turn > 0 || manual;
}
// A mouse used alongside touch controls is desktop input too.
static qbool TouchAim_EnabledForInput(qbool touchscreen, qbool mouselook, qbool desktop_enabled)
{
	return (touchscreen && !mouselook) || desktop_enabled;
}

// Input-independent aim engagement: a physical turn or either fire button
// counts alongside the touch HUD, including when the controls are visible.
static qbool TouchAim_InputActive(qbool hud, qbool touchscreen, qbool mouselook, float turn, qbool manual)
{
	return hud || !touchscreen || mouselook || turn > 0 || manual;
}
// A mouse used alongside touch controls is desktop input too.
static qbool TouchAim_EnabledForInput(qbool mouse_present, qbool desktop_enabled)
{
	return !mouse_present || desktop_enabled;
}

// Auto-switch follows the same desktop opt-in and server permission as aim helpers.
static qbool TouchAim_AutoSwitchEnabled(qbool mouse_present, qbool desktop_enabled, qbool enabled, qbool allowed)
{
	return enabled && allowed && TouchAim_EnabledForInput(mouse_present, desktop_enabled);
}

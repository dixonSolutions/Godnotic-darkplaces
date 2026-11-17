#ifndef TOUCH_JOINPREVIEW_H
#define TOUCH_JOINPREVIEW_H

// Godnotic: the world while it loads.
//
// A join used to be a full-screen loading page from the first packet to the
// last signon step, though the map is drawable long before that: the BSP is
// loaded at signon step 2 and the rest of the signon (models, sounds, CSQC,
// the first snapshot) is what takes the seconds. This module draws the world
// from a spectator camera as soon as the world model is there, so the page is
// replaced by play. When the signon completes the normal view takes over in
// the same frame, so the hand-over is a camera cut, not a black gap.
//
// The camera follows a player, then an aerial overview of the map bounds.
// There is no health or aliveness in the engine's entity states: a player is
// "alive" here when it is drawn, is not EF_NODRAW and moved or fired lately
// (or, for the local player, reports health above 0). See TP_Follow.
//
// The engine links network entities only once the signon is complete
// (CL_UpdateWorld), so a preview frame carries the world, not the players in
// it: the map is what shows, and the players appear the moment the signon
// ends.

qbool TouchJoinPreview_Active(void);
// True when this frame is a preview: sets the scene and camera for the
// caller's world render (cl_screen.c SCR_DrawScreen). False during the
// signon's last step, where the normal view draws.
qbool TouchJoinPreview_Camera(void);
// True once after a preview frame was drawn; read by the load trace
// (touch_loadtime.c) after the video swap.
qbool TouchJoinPreview_Drew(void);

void TouchJoinPreview_Init(void);

#endif

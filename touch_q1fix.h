#ifndef TOUCH_Q1FIX_H
#define TOUCH_Q1FIX_H

#include "qtypes.h"
#include "render.h"

/*
Godnotic: Quake 1 content on a Xonotic-engine server (touch_q1fix.c).

  TouchQ1_Init            cvars (called from touch_hook.c)
  TouchQ1_LightScale      gl_rmain.c: brightens a Quake 1 world's lightmaps
  TouchQ1_SbarBegin/End   cl_screen.c around Sbar_Draw: the engine HUD's rows keep their pitch
  TouchQ1_HideNoModel     gl_rmain.c R_DrawNoModel: an entity without a model draws nothing (not the debug diamond)
*/
void TouchQ1_Init(void);
void TouchQ1_LightScale(float *lightmapintensity);
void TouchQ1_SbarBegin(void);
void TouchQ1_SbarEnd(void);
qbool TouchQ1_HideNoModel(void);

#endif

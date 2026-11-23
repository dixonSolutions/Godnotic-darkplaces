#ifndef TOUCH_PRECACHE_H
#define TOUCH_PRECACHE_H

// Godnotic: what a join's precache is doing, for the card and for profiling.
// Touch-owned; the upstream files carry only the hook lines (// Touch:).

void TouchPre_Init(void);

// SCR_PushLoadingScreen (via TouchFE_Loading): a new item begins. msg is the
// step's name, min/len its place on the whole bar (0..1).
void TouchPre_Item(const char *msg, float min, float len);

// Inside one long item (the world model): stage name and n of m done.
void TouchPre_Sub(const char *stage, int n, int m);

// A part of the world load just finished; adds its time to the profile and
// moves the sub-progress on to the next part.
void TouchPre_Mark(const char *part);
// TouchPre_Mark only on the first pass of a loop (i == 0), so an upstream
// "if (i == 0) one statement" keeps its shape
void TouchPre_MarkFirst(int i, const char *part);

// The values the front end publishes ("load_stage", "load_n", "load_m",
// "load_sub"): stage is "" outside a precache.
const char *TouchPre_Stage(void);
int TouchPre_N(void);
int TouchPre_M(void);
// 0..1: the share of the current item that is done; the bar position is
// the item's min + len * this.
float TouchPre_SubFraction(void);

// touch_loadtime.c, at the first game frame: the top ten items and parts.
void TouchPre_PrintSummary(void);
// A new load begins: forget the previous one's figures.
void TouchPre_Reset(void);

// cl_screen.c, in the loading screen's redraw throttle: true when a redraw now
// would take more than touch_precache_redraw_duty of the load's time.
qbool TouchPre_RedrawTooSoon(double now);

#endif

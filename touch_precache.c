/*
Godnotic: what a join's precache is doing.

A map load runs inside one frame; the loading screen's stack (cl_screen.c)
gives the bar one step per model and sound, and the world model, the one item
that takes seconds on a phone, is a single step. This file

  - names the step for the front end's card: _touch_precache_stage ("models",
    "world", "worldsetup", "sounds"), _touch_precache_n / _touch_precache_m
    (item n of m; inside the world model, texture n of m), and the share of
    the current item that is done, which moves the card's bar between the
    item's two ends (touch_frontend.c publishes them as load_stage, load_n,
    load_m, load_progress);
  - with touch_precache_trace 1 prints, at the first game frame after a load
    (touch_loadtime.c), the ten slowest items and where the world model
    spent its time (PRECACHE lines); 2 also prints each item over 5 ms, 3 also the sub-progress.
*/

#include "quakedef.h"
#include "touch_precache.h"
#include "touch_frontend.h"
#include "touch_loadtime.h"
#ifdef __linux__
#include <time.h>
#endif

// CPU time of this thread in ms: unlike the wall clock it does not grow
// when the machine is busy, so profiles from a shared box are comparable.
static double TP_CPU(void)
{
#ifdef __linux__
	struct timespec ts;
	if (!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts))
		return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
	return 0;
}

static cvar_t touch_precache_trace = {CF_CLIENT, "touch_precache_trace", "0", "print the slowest precache items and the world model's parts at the first game frame after a load (Godnotic): 1 = top ten, 2 = also every item over 5 ms, 3 = also the sub-progress"};
static cvar_t touch_precache_redraw_duty = {CF_CLIENT, "touch_precache_redraw_duty", "0.2", "most of a map load the loading screen may spend redrawing (Godnotic): after a redraw that took t, the next waits t * (1 - duty) / duty; 0 = only the scr_loadingscreen_maxfps limit. Without it a frame slower than 1 / maxfps makes the load one redraw after each model and sound"};
static cvar_t touch_precache_stage = {CF_CLIENT | CF_READONLY, "_touch_precache_stage", "", "what a join is precaching now: models, world, worldsetup, sounds (Godnotic); empty outside a load"};
static cvar_t touch_precache_n = {CF_CLIENT | CF_READONLY, "_touch_precache_n", "0", "the item (or inside the world model, the texture) being precached (Godnotic)"};
static cvar_t touch_precache_m = {CF_CLIENT | CF_READONLY, "_touch_precache_m", "0", "of this many items (Godnotic)"};
static cvar_t touch_precache_sub = {CF_CLIENT | CF_READONLY, "_touch_precache_sub", "0", "0..1: how much of the current item is done (Godnotic)"};

static char tp_stage[16];
static int tp_n, tp_m;
static float tp_sub;

#define TP_TOP 10
typedef struct tp_item_s { char name[64]; double ms, cpu; } tp_item_t;
static tp_item_t tp_top[TP_TOP];
static int tp_ntop;
static char tp_last[64];
static double tp_last_time, tp_last_cpu;
static double tp_models_ms, tp_sounds_ms, tp_other_ms, tp_models_cpu, tp_sounds_cpu, tp_other_cpu;
static int tp_models, tp_sounds;

#define TP_PARTS 32
static struct { const char *name; double ms, cpu; } tp_parts[TP_PARTS];
static int tp_nparts;
static double tp_mark_time, tp_mark_cpu;
static double tp_draw_ms0, tp_first_time, tp_first_cpu;
static int tp_draw_n0;

// how far each part of the world model's load is along it (cumulative end)
static const struct { const char *part; float end; } tp_part_end[] =
{
	{"bsp_read", 0.02f}, {"entities", 0.03f}, {"textures", 0.35f}, {"collision", 0.45f},
	{"geometry", 0.50f}, {"lightmaps", 0.60f}, {"faces", 0.70f}, {"models", 0.72f},
	{"tree", 0.78f}, {"grid_pvs", 0.80f}, {"portals", 0.85f}, {"submodels", 0.95f},
	{"vbos", 0.98f}, {"sorted", 1.0f},
};

void TouchPre_Init(void)
{
	Cvar_RegisterVariable(&touch_precache_trace);
	Cvar_RegisterVariable(&touch_precache_redraw_duty);
	Cvar_RegisterVariable(&touch_precache_stage);
	Cvar_RegisterVariable(&touch_precache_n);
	Cvar_RegisterVariable(&touch_precache_m);
	Cvar_RegisterVariable(&touch_precache_sub);
}

void TouchPre_Reset(void)
{
	tp_ntop = 0;
	tp_last[0] = 0;
	tp_last_time = 0;
	tp_models_ms = tp_sounds_ms = tp_other_ms = tp_models_cpu = tp_sounds_cpu = tp_other_cpu = 0;
	tp_last_cpu = tp_mark_cpu = 0;
	tp_first_time = tp_first_cpu = 0;
	tp_models = tp_sounds = 0;
	tp_nparts = 0;
	tp_mark_time = 0;
	tp_stage[0] = 0;
	tp_n = tp_m = 0;
	tp_sub = 0;
}

const char *TouchPre_Stage(void) { return tp_stage; }
int TouchPre_N(void) { return tp_n; }
int TouchPre_M(void) { return tp_m; }
float TouchPre_SubFraction(void) { return tp_sub; }

static void TP_SetCvars(void)
{
	static float printed = -1;
	if (touch_precache_trace.integer >= 3 && (tp_sub >= printed + 0.05f || tp_sub < printed))
	{
		printed = tp_sub;
		Con_Printf("PRECACHE progress %s %d/%d sub=%.2f\n", tp_stage, tp_n, tp_m, tp_sub);
	}
	if (strcmp(touch_precache_stage.string, tp_stage))
		Cvar_SetQuick(&touch_precache_stage, tp_stage);
	if (touch_precache_n.integer != tp_n)
		Cvar_SetValueQuick(&touch_precache_n, tp_n);
	if (touch_precache_m.integer != tp_m)
		Cvar_SetValueQuick(&touch_precache_m, tp_m);
	Cvar_SetValueQuick(&touch_precache_sub, tp_sub);
}

static void TP_Account(const char *name, double ms, double cpu)
{
	int i;
	if (!name[0])
		return;
	if (!strcmp(tp_stage, "models") || !strcmp(tp_stage, "world"))
		tp_models_ms += ms, tp_models_cpu += cpu, tp_models++;
	else if (!strcmp(tp_stage, "sounds"))
		tp_sounds_ms += ms, tp_sounds_cpu += cpu, tp_sounds++;
	else
		tp_other_ms += ms, tp_other_cpu += cpu;
	if (touch_precache_trace.integer >= 2 && ms >= 5.0)
		Con_Printf("PRECACHE item %-40s %8.1f ms (cpu %.1f)\n", name, ms, cpu);
	if (tp_ntop == TP_TOP && ms <= tp_top[TP_TOP - 1].ms)
		return;
	if (tp_ntop < TP_TOP)
		tp_ntop++;
	for (i = tp_ntop - 1; i > 0 && tp_top[i - 1].ms < ms; i--)
		tp_top[i] = tp_top[i - 1];
	dp_strlcpy(tp_top[i].name, name, sizeof(tp_top[i].name));
	tp_top[i].ms = ms;
	tp_top[i].cpu = cpu;
}

void TouchPre_Item(const char *msg, float min, float len)
{
	double now = Sys_DirtyTime();
	const char *stage = "";
	int n = 0, m = 0;
	(void)min; (void)len;
	if (!tp_first_time)
	{
		tp_first_time = now;
		tp_first_cpu = TP_CPU();
		tp_draw_ms0 = TouchLoadTime_PhaseMs(TLT_SCRDRAW);
		tp_draw_n0 = TouchLoadTime_PhaseCalls(TLT_SCRDRAW);
	}
	if (tp_last[0] && tp_last_time)
		TP_Account(tp_last, (now - tp_last_time) * 1000.0, TP_CPU() - tp_last_cpu);
	if (!msg)
		msg = "";
	if (cl.loadmodel_current >= 1 && cl.loadmodel_current < cl.loadmodel_total && cl.loadmodel_current < MAX_MODELS
		&& !strcmp(msg, cl.model_name[cl.loadmodel_current]))
	{
		stage = cl.loadmodel_current == 1 ? "world" : "models";
		n = cl.loadmodel_current;
		m = cl.loadmodel_total - 1;
		if (cl.loadmodel_current == 1)
			tp_mark_time = now, tp_mark_cpu = TP_CPU();
	}
	else if (cl.loadsound_current >= 1 && cl.loadsound_current < cl.loadsound_total && cl.loadsound_current < MAX_SOUNDS
		&& !strcmp(msg, cl.sound_name[cl.loadsound_current]))
	{
		stage = "sounds";
		n = cl.loadsound_current;
		m = cl.loadsound_total - 1;
	}
	else if (!strcmp(msg, "world model setup"))
		stage = "worldsetup";
	else if (!strcmp(msg, "Loading precached models"))
	{
		stage = "models";
		n = 0;
		m = cl.loadmodel_total - 1;
	}
	else if (!strcmp(msg, "Loading precached sounds"))
	{
		stage = "sounds";
		n = 0;
		m = cl.loadsound_total - 1;
	}
	dp_strlcpy(tp_stage, stage, sizeof(tp_stage));
	tp_n = n;
	tp_m = m;
	tp_sub = 0;
	dp_strlcpy(tp_last, msg, sizeof(tp_last));
	tp_last_time = now;
	tp_last_cpu = TP_CPU();
	TP_SetCvars();
}

void TouchPre_Sub(const char *stage, int n, int m)
{
	size_t i;
	float lo = 0, hi = 0;
	if (!tp_last_time)
		return;
	// inside the world model: textures n of m
	for (i = 0; i < sizeof(tp_part_end) / sizeof(*tp_part_end); i++)
		if (!strcmp(tp_part_end[i].part, stage))
		{
			hi = tp_part_end[i].end;
			lo = i ? tp_part_end[i - 1].end : 0;
			break;
		}
	if (m > 0 && hi > lo)
	{
		float v = lo + (hi - lo) * (float)n / (float)m;
		if (v > tp_sub) // the bar never steps back inside an item
			tp_sub = v;
	}
	if (!strcmp(tp_stage, "world"))
	{
		tp_n = n;
		tp_m = m;
	}
	TP_SetCvars();
	TouchFE_LoadingTick();
}

void TouchPre_Mark(const char *part)
{
	double now = Sys_DirtyTime();
	int i;
	size_t k;
	if (!tp_last_time || strcmp(tp_stage, "world"))
		return;
	if (tp_mark_time)
	{
		for (i = 0; i < tp_nparts; i++)
			if (!strcmp(tp_parts[i].name, part))
				break;
		if (i == tp_nparts && tp_nparts < TP_PARTS)
		{
			tp_parts[i].name = part; // literals only
			tp_parts[i].ms = tp_parts[i].cpu = 0;
			tp_nparts++;
		}
		if (i < tp_nparts)
			tp_parts[i].ms += (now - tp_mark_time) * 1000.0, tp_parts[i].cpu += TP_CPU() - tp_mark_cpu;
	}
	tp_mark_time = now;
	tp_mark_cpu = TP_CPU();
	for (k = 0; k < sizeof(tp_part_end) / sizeof(*tp_part_end); k++)
		if (!strcmp(tp_part_end[k].part, part) && strcmp(part, "submodels")) // submodels report each one through TouchPre_Sub
		{
			if (tp_part_end[k].end > tp_sub)
				tp_sub = tp_part_end[k].end;
			break;
		}
	TP_SetCvars();
	TouchFE_LoadingTick();
}

qbool TouchPre_RedrawTooSoon(double now)
{
	static double seen_ms, last_start, avg_ms;
	static int seen_n, skipped;
	double d = touch_precache_redraw_duty.value, ms = TouchLoadTime_PhaseMs(TLT_SCRDRAW);
	int n = TouchLoadTime_PhaseCalls(TLT_SCRDRAW);
	double gap;
	if (n < seen_n || ms < seen_ms) // the table was reset
		seen_n = 0, seen_ms = 0, avg_ms = 0;
	if (n > seen_n) // frames drawn since the last look: their mean cost
	{
		double each = (ms - seen_ms) / (n - seen_n);
		avg_ms = avg_ms ? avg_ms * 0.5 + each * 0.5 : each;
		seen_n = n;
		seen_ms = ms;
	}
	if (d <= 0 || d >= 1 || avg_ms <= 0 || !last_start)
	{
		last_start = now;
		return false;
	}
	gap = avg_ms / 1000.0 * (1.0 - d) / d;
	if (gap > 1.0)
		gap = 1.0;
	if (now - last_start >= 0 && now - last_start < gap + avg_ms / 1000.0)
	{
		skipped++;
		return true;
	}
	last_start = now;
	(void)skipped;
	return false;
}

void TouchPre_PrintSummary(void)
{
	int i;
	if (touch_precache_trace.integer < 1)
		return;
	if (tp_last[0] && tp_last_time)
		TP_Account(tp_last, (Sys_DirtyTime() - tp_last_time) * 1000.0, TP_CPU() - tp_last_cpu);
	tp_last[0] = 0;
	Con_Printf("PRECACHE models %d in %.1f ms (cpu %.1f), sounds %d in %.1f ms (cpu %.1f), other %.1f ms (cpu %.1f)\n",
		tp_models, tp_models_ms, tp_models_cpu, tp_sounds, tp_sounds_ms, tp_sounds_cpu, tp_other_ms, tp_other_cpu);
	Con_Printf("PRECACHE span %.1f ms (cpu %.1f), screen redraws in it: %d, %.1f ms\n", (Sys_DirtyTime() - tp_first_time) * 1000.0,
		TP_CPU() - tp_first_cpu, TouchLoadTime_PhaseCalls(TLT_SCRDRAW) - tp_draw_n0, TouchLoadTime_PhaseMs(TLT_SCRDRAW) - tp_draw_ms0);
	for (i = 0; i < tp_ntop; i++)
		Con_Printf("PRECACHE top%-2d %-44s %8.1f ms (cpu %.1f)\n", i + 1, tp_top[i].name, tp_top[i].ms, tp_top[i].cpu);
	for (i = 0; i < tp_nparts; i++)
		Con_Printf("PRECACHE world part %-12s %8.1f ms (cpu %.1f)\n", tp_parts[i].name, tp_parts[i].ms, tp_parts[i].cpu);
}

void TouchPre_MarkFirst(int i, const char *part)
{
	if (i == 0)
		TouchPre_Mark(part);
}

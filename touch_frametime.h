// Touch: per-frame time of a running game, for scripts/bench-frames.sh.
//
// Records the CPU time each Host_Frame spends (two Sys_DirtyTime calls and a
// store per frame; no allocation, no I/O during play) for frames of a game
// that is fully signed on, and prints one summary at shutdown:
//
//   FRAMETIME map=stormkeep frames=1823 median=6.83 p95=11.20 p99=21.44
//   hitches33=9 (0.5%) max=41.02 mean=7.31 dropped=0
//   FRAMETIME_HITCHES t=0.51,3.2,7.9,...          (first 32 over 33ms, s)
//   FRAMETIME_STATS draws=.. elements=.. lights=.. lighttris=.. shadowtris=..
//                   particles=.. decals=.. lightmaps=.. vbup=..KB ibup=..KB
//
// so a run's frame-time distribution can be compared without a profiler, and a
// hitch can be placed in time (a burst right after the game starts is preload
// still finishing; a spread of single hitches is work done during play).
// The figures are the main thread's cost per frame; on the offscreen Mesa build
// that includes software rasterization, and on a device the GPU's own time
// shows up only where the driver blocks.
//
//   host.c Host_Frame:   TouchFrameTime_Frame();    (first line)
//   host.c Host_Frame:   TouchFrameTime_EndFrame(); (before the return)
//   host.c Host_Shutdown: TouchFrameTime_Shutdown();
//
// The buffer is static (FT_MAX * 4 bytes) and never grows: a frame that would
// overflow it is counted but not stored.

#ifndef TOUCH_FRAMETIME_H
#define TOUCH_FRAMETIME_H

#include "render.h"

#define FT_MAX 240000
#define FT_HITCH_MAX 32

static struct
{
	qbool active;
	double begin;
	int count;          // frames stored
	int total;          // frames seen in game
	int dropped;        // frames the buffer had no room for
	float ms[FT_MAX];
	char map[MAX_QPATH];
	// hitches, as seconds since the first in-game frame
	double start;
	int hitchn;
	double hitch[FT_HITCH_MAX];
	// renderer work summed over the frames, for a per-frame average
	double stsum[12];
	int stframes;
} ft;

// Renderer work of the frame that just ended: its counters are cleared when
// the next scene starts, so they are read here and not after the frame.
static void TouchFrameTime_Stats(void)
{
	static const int which[12] = {
		r_stat_draws, r_stat_draws_elements, r_stat_lights,
		r_stat_lights_lighttriangles, r_stat_lights_shadowtriangles,
		r_stat_particles, r_stat_drawndecals, r_stat_lightmapupdates,
		r_stat_vertexbufferuploadcount, r_stat_vertexbufferuploadsize,
		r_stat_indexbufferuploadcount, r_stat_indexbufferuploadsize
	};
	int i;
	if (!ft.count || !r_refdef.scene.worldmodel)
		return;
	for (i = 0; i < 12; i++)
		ft.stsum[i] += r_refdef.stats[which[i]];
	ft.stframes++;
}

static void TouchFrameTime_Frame(void)
{
	TouchFrameTime_Stats();
	// only a game the player is in: no menu, no loading screen
	ft.active = cls.state == ca_connected && cls.signon == SIGNONS;
	ft.begin = ft.active ? Sys_DirtyTime() : 0;
}

static void TouchFrameTime_EndFrame(void)
{
	double now, ms;
	if (!ft.active || ft.begin <= 0)
		return;
	now = Sys_DirtyTime();
	ms = (now - ft.begin) * 1000.0;
	if (cl.worldbasename[0] && strcmp(ft.map, cl.worldbasename))
	{
		// a new map: the previous one's summary is already out of date
		dp_strlcpy(ft.map, cl.worldbasename, sizeof(ft.map));
		ft.count = 0;
		ft.total = 0;
		ft.dropped = 0;
		ft.hitchn = 0;
		ft.stframes = 0;
	}
	if (!ft.count)
		ft.start = now;
	ft.total++;
	if (ms > 33.0 && ft.hitchn < FT_HITCH_MAX)
		ft.hitch[ft.hitchn++] = now - ft.start;
	if (ft.count < FT_MAX)
		ft.ms[ft.count++] = (float)ms;
	else
		ft.dropped++;
	ft.begin = 0;
}

static int FT_Compare(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static float FT_Percentile(const float *sorted, int n, double p)
{
	int i = (int)(p * (n - 1) + 0.5);
	return sorted[bound(0, i, n - 1)];
}

// Resident and peak resident memory of the process in MB (Linux and Android:
// /proc/self/status); -1 where there is no such file.
static void FT_Memory(double *rss, double *peak)
{
	*rss = *peak = -1;
#if defined(__linux__) || defined(__ANDROID__)
	{
		char line[128];
		FILE *f = fopen("/proc/self/status", "r");
		long kb;
		if (!f)
			return;
		while (fgets(line, sizeof(line), f))
		{
			if (sscanf(line, "VmRSS: %ld kB", &kb) == 1)
				*rss = kb / 1024.0;
			else if (sscanf(line, "VmHWM: %ld kB", &kb) == 1)
				*peak = kb / 1024.0;
		}
		fclose(f);
	}
#endif
}

static void TouchFrameTime_Shutdown(void)
{
	static float sorted[FT_MAX];
	double sum = 0;
	int i, hitches = 0;
	// a summary also when the buffer is small, but not for an empty one
	if (ft.count < 30)
		return;
	memcpy(sorted, ft.ms, ft.count * sizeof(sorted[0]));
	qsort(sorted, ft.count, sizeof(sorted[0]), FT_Compare);
	for (i = 0; i < ft.count; i++)
	{
		sum += ft.ms[i];
		if (ft.ms[i] > 33.0f)
			hitches++;
	}
	Con_Printf("FRAMETIME map=%s frames=%d median=%.2f p95=%.2f p99=%.2f hitches33=%d (%.1f%%) max=%.2f mean=%.2f dropped=%d\n",
		ft.map[0] ? ft.map : "-", ft.count,
		FT_Percentile(sorted, ft.count, 0.50), FT_Percentile(sorted, ft.count, 0.95),
		FT_Percentile(sorted, ft.count, 0.99), hitches, 100.0 * hitches / ft.count,
		sorted[ft.count - 1], sum / ft.count, ft.dropped);
	{
		double rss, peak;
		FT_Memory(&rss, &peak);
		Con_Printf("FRAMETIME_MEM rss=%.1f peak=%.1f (MB, this process)\n", rss, peak);
	}
	if (ft.hitchn)
	{
		Con_Printf("FRAMETIME_HITCHES n=%d t=", ft.hitchn);
		for (i = 0; i < ft.hitchn; i++)
			Con_Printf("%s%.2f", i ? "," : "", ft.hitch[i]);
		Con_Printf("\n");
	}
	if (ft.stframes)
	{
		double n = ft.stframes;
		Con_Printf("FRAMETIME_STATS draws=%.1f elements=%.0f lights=%.2f lighttris=%.0f shadowtris=%.0f particles=%.1f decals=%.1f lightmaps=%.2f vbup=%.1fKB ibup=%.1fKB\n",
			ft.stsum[0] / n, ft.stsum[1] / n, ft.stsum[2] / n, ft.stsum[3] / n,
			ft.stsum[4] / n, ft.stsum[5] / n, ft.stsum[6] / n, ft.stsum[7] / n,
			ft.stsum[8] / n, (ft.stsum[9] / n) / 1024.0,
			ft.stsum[10] / n, (ft.stsum[11] / n) / 1024.0);
	}
}

#endif

/* Godnotic: never sign on to an unrenderable world after a map download fails. */
#ifndef TOUCH_JOIN_H
#define TOUCH_JOIN_H

static void TouchJoin_RequireWorld(qbool renderable, const char *mapname)
{
	if (gamemode == GAME_XONOTIC && !renderable)
		Host_Error("Godnotic: cannot load map %.48s. Enable server downloads and reconnect.\n", mapname && *mapname ? mapname : "(unknown)");
}

#endif

#ifndef TOUCH_MUSIC_H
#define TOUCH_MUSIC_H

#include <stddef.h>
#include <string.h>
#include <strings.h>

/* -1: stop, 0: leave alone, 1: start (the menu's own switch, kept for tests). */
static inline int TouchMusic_Policy(int in_game, int theme_present, int playing)
{
    if (in_game && playing)
        return -1;
    if (!in_game && theme_present && !playing)
        return 1;
    return 0;
}

/* Stages of a game start. The theme is the music of a menu, a connect, a
 * download, a load and a join preview; the match owns the music from its first
 * playable frame, and its map or server may bring a track of its own. */
#define TM_STAGE_MENU 0
#define TM_STAGE_JOINING 1
#define TM_STAGE_GAME 2
#define TM_STAGE_DOWNLOAD 3

/* -1: stop the theme, 0: leave it alone, 1: start it.
 *
 * A join keeps the theme playing until the map's own music arrives: a `cd loop`
 * from the map or the server replaces the stream, so `map_music` says the theme
 * has nothing left to do and the new track takes over (the mixer fades it in).
 * In a match the normal rules apply: the theme stops, unless the map has no
 * music at all and the player allows the theme to stay.
 */
static inline int TouchMusic_JoinPolicy(int stage, int theme_present, int theme_playing, int map_music, int allow_in_game)
{
    if (stage == TM_STAGE_GAME)
    {
        if (!theme_playing)
            return 0;
        return (!map_music && allow_in_game) ? 0 : -1;
    }
    /* The data-download screen is not a match: the theme belongs there just
     * as it does in the menu. */
    if (map_music)
        return theme_playing ? -1 : 0;
    /* Never start it during a join: loading the track (and the rescan before
     * it) stalls the connect and the map load for seconds on a phone. A theme
     * already playing carries on. */
    if (stage == TM_STAGE_JOINING)
        return 0;
    if (!theme_playing && theme_present)
        return 1;
    return 0;
}

/* Is this the menu theme? The track is asked for by name, but a map may name a
 * path (sound/cdtracks/...) and a pack may add an extension. */
static inline int TouchMusic_IsTheme(const char *track)
{
    const char *base = track ? strrchr(track, '/') : NULL;
    const char *theme = "rising-of-the-phoenix";
    size_t n = strlen(theme);
    if (!track || !track[0])
        return 0;
    if (base)
        base++;
    else
        base = track;
    return !strncasecmp(base, theme, n) && (base[n] == 0 || base[n] == '.');
}

/* When to look again after an ask: a track that started takes a moment to
 * report a position, and an ask that failed must not spin. */
static inline double TouchMusic_NextCheck(double now, int asked)
{
    if (asked)
        return now + 2.0;
    return now + (now < 60.0 ? 0.5 : 5.0);
}

void TouchMusic_Update(void);
#endif

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

/* The menu theme stops during a join; the map owns its music. */
#define TM_STAGE_MENU 0
#define TM_STAGE_JOINING 1
#define TM_STAGE_GAME 2
#define TM_STAGE_DOWNLOAD 3

/* -1: stop the theme, 0: leave it alone, 1: start it. */
static inline int TouchMusic_JoinPolicy(int stage, int theme_present, int theme_playing, int map_music, int allow_in_game)
{
    (void)allow_in_game;
    if (stage == TM_STAGE_JOINING || stage == TM_STAGE_GAME)
        return theme_playing ? -1 : 0;
    /* The data-download screen is not a match: the theme belongs there just
     * as it does in the menu. */
    if (map_music)
        return theme_playing ? -1 : 0;
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

#ifndef TOUCH_MUSIC_H
#define TOUCH_MUSIC_H

/* -1: stop, 0: leave alone, 1: start. */
static inline int TouchMusic_Policy(int in_game, int theme_present, int playing)
{
    if (in_game && playing)
        return -1;
    if (!in_game && theme_present && !playing)
        return 1;
    return 0;
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

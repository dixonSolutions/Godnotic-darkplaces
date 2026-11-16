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

void TouchMusic_Update(void);
#endif

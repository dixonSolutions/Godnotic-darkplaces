#ifndef TOUCH_CURSOR_H
#define TOUCH_CURSOR_H

#include <string.h>

/* Only Xonotic's pointer skins: chat carets and unrelated pictures stay. */
static inline int TouchCursor_HidePic(const char *name, int touchscreen, int pointer_mouse, int system_cursor)
{
	const char *base;
	if (!name || strncmp(name, "gfx/menu/", 9))
		return 0;
	base = strrchr(name, '/');
	if (!base || (strcmp(base, "/cursor") && strcmp(base, "/cursor_move")
		&& strcmp(base, "/cursor_resize") && strcmp(base, "/cursor_resize2")))
		return 0;
	return (touchscreen && !pointer_mouse) || system_cursor;
}

static inline int TouchCursor_HideSystem(int touchscreen, int pointer_mouse, int mouse_aims, int csqc_cursor, int prydon_cursor)
{
	return (touchscreen && !pointer_mouse) || (mouse_aims && !csqc_cursor && !prydon_cursor);
}

/* Android's system pointer is not trustworthy: SDL hides it with a null
 * PointerIcon and a mouse on a tablet showed no cursor in the menu, the
 * in-game pointer screens or the console. There the engine and the menu draw
 * the pointer themselves and SDL's stays hidden. Elsewhere SDL's own pointer
 * is used (vid_system_cursor 1). */
#ifdef __ANDROID__
#define TOUCH_CURSOR_SYSTEM_DEFAULT "0"
#else
#define TOUCH_CURSOR_SYSTEM_DEFAULT "1"
#endif

/* hide: what the caller decided for SDL's pointer. When the engine draws the
 * pointer (system_cursor 0) SDL's is always hidden, so it is never drawn twice. */
static inline int TouchCursor_HideSDL(int hide, int system_cursor)
{
	return hide || !system_cursor;
}

/* The engine draws a pointer over the console only where it draws pointers at
 * all, a mouse (not a finger) was used last, and the console is down. */
static inline int TouchCursor_ConsolePointer(int system_cursor, int pointer_mouse, int console_down)
{
	return !system_cursor && pointer_mouse && console_down;
}
#endif

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
#endif

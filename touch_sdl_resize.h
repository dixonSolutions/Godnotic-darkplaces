#ifndef TOUCH_SDL_RESIZE_H
#define TOUCH_SDL_RESIZE_H

/* Preserve the last usable drawable while a compositor replaces its surface. */
static int touch_resize_width, touch_resize_height;
static void Touch_SDLResizeBefore(unsigned char event, int width, int height)
{
	if ((event == SDL_WINDOWEVENT_RESIZED || event == SDL_WINDOWEVENT_SIZE_CHANGED)
		&& width > 0 && height > 0)
	{
		touch_resize_width = width;
		touch_resize_height = height;
	}
}

static void Touch_SDLResizeAfter(unsigned char event, SDL_Window *window, int *width, int *height)
{
	int w, h;
	if (event != SDL_WINDOWEVENT_RESIZED && event != SDL_WINDOWEVENT_SIZE_CHANGED)
		return;
	SDL_GL_GetDrawableSize(window, &w, &h);
	if (w > 0 && h > 0)
	{
		*width = w;
		*height = h;
	}
	else if (touch_resize_width > 0 && touch_resize_height > 0)
	{
		*width = touch_resize_width;
		*height = touch_resize_height;
	}
}
#endif

#ifndef TOUCH_SDL_RESIZE_CVARS_H
#define TOUCH_SDL_RESIZE_CVARS_H

/* These cvars are also used by input and QC. Updating them must not invoke
 * their display-mode callbacks: the window has already changed size. */
static void Touch_SDLResizeCvars(unsigned char event, int width, int height)
{
	void (*width_callback)(cvar_t *), (*height_callback)(cvar_t *);
	if ((event != SDL_WINDOWEVENT_RESIZED && event != SDL_WINDOWEVENT_SIZE_CHANGED)
		|| width <= 0 || height <= 0)
		return;
	width_callback = vid_width.callback;
	height_callback = vid_height.callback;
	vid_width.callback = vid_height.callback = NULL;
	if (vid_width.integer != width)
		Cvar_SetValueQuick(&vid_width, width);
	if (vid_height.integer != height)
		Cvar_SetValueQuick(&vid_height, height);
	vid_width.callback = width_callback;
	vid_height.callback = height_callback;
}
#endif

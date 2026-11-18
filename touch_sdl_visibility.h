// SDL does not send HIDDEN when a desktop window is minimised.
// Keep vid_hidden in step so both idle sound modes see the transition.
static void Touch_SDLVisibility(unsigned char event, qbool *hidden)
{
#if defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
	if (event == SDL_WINDOWEVENT_MINIMIZED)
		*hidden = true;
	else if (event == SDL_WINDOWEVENT_RESTORED)
		*hidden = false;
#else
	(void)event;
	(void)hidden;
#endif
}

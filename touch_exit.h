/*
Godnotic: how the engine leaves the process, and the signals it leaves alone, on Android.

An Android app process is not the engine's alone. When the engine quits
(Sys_Frame after Host_Shutdown) other threads are still running: ART's,
hwui's RenderThread and hwuiTask workers, AndySU's andysu-updater, SDL's
audio. exit() runs every library's static destructors and atexit handlers
under them. Device logs from 1.2.251 to 1.2.276 show what that does: a
FORTIFY abort "pthread_mutex_lock called on a destroyed mutex" in
andysu-updater, and SIGSEGVs in RenderThread and hwuiTask1, each with no
tombstone and the process recorded as EXIT_SELF status 0. Config and files
are already written by Host_Shutdown, so on Android the engine ends the
process at once with _exit() instead.

darkplaces also catches SIGPIPE to quit cleanly. In an Android app a broken
pipe or socket is an ordinary error (EPIPE) that the writer handles; turning
it into a quit closed the game in the middle of a download with nothing in
the log (EXIT_SELF status 0, no crash). Android ignores SIGPIPE.

The pure parts are what scripts/test-android-stability.sh compiles alone.
*/
#ifndef TOUCH_EXIT_H
#define TOUCH_EXIT_H

#include <signal.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif

enum { TOUCH_EXIT_LIBC = 0, TOUCH_EXIT_NOW = 1 };
enum { TOUCH_SIG_ENGINE = 0, TOUCH_SIG_IGNORE = 1 };

/* How the engine ends the process after Host_Shutdown. */
static inline int TouchExit_Mode(int android)
{
	return android ? TOUCH_EXIT_NOW : TOUCH_EXIT_LIBC;
}

/* Whether the engine's own handler (Sys_HandleSignal: quit) takes a signal. */
static inline int TouchExit_SignalAction(int sig, int android)
{
#ifdef SIGPIPE
	if (android && sig == SIGPIPE)
		return TOUCH_SIG_IGNORE;
#endif
	(void)sig;
	(void)android;
	return TOUCH_SIG_ENGINE;
}

/* End the process now: no atexit handlers, no static destructors. */
static inline void TouchExit_Now(int status)
{
#ifdef _WIN32
	_Exit(status);
#else
	_exit(status);
#endif
}

/* Give the signals the engine must not quit on back to SIG_IGN. */
static inline void TouchExit_IgnoreSignals(int android)
{
#ifdef SIGPIPE
	if (TouchExit_SignalAction(SIGPIPE, android) == TOUCH_SIG_IGNORE)
		signal(SIGPIPE, SIG_IGN);
#else
	(void)android;
#endif
}

#ifdef __ANDROID__
#define TOUCH_EXIT_ANDROID 1
#else
#define TOUCH_EXIT_ANDROID 0
#endif

/* Sys_InitSignals hook, after the engine installed its handlers. */
static inline void TouchExit_Signals(void)
{
	TouchExit_IgnoreSignals(TOUCH_EXIT_ANDROID);
}

/* Sys_Frame hook, right before exit(status): returns only where exit() is right. */
static inline void TouchExit_Leave(int status)
{
	if (TouchExit_Mode(TOUCH_EXIT_ANDROID) == TOUCH_EXIT_NOW)
		TouchExit_Now(status);
}

#endif

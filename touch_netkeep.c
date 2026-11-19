/*
Godnotic: the connection to a server outlives one long frame.

A server drops a client it has heard nothing from for net_connecttimeout
seconds before the client has begun and net_messagetimeout after (both 30 on
Xonotic servers, xonotic-server.cfg). The client speaks only from its main
thread: once per frame, and inside a load from the loaders' calls to
CL_KeepaliveMessage. A step that does neither -- loading the CSQC program
and running its CSQC_Init, a GPU driver compiling the shaders of the first
preview or game frame, storage that stalls -- leaves the server hearing
nothing for as long as the step takes. On a tablet that passes 30 s, the
server drops a player who is still joining, and the client, finding
"Server disconnected" after a frame that long, took it for a freeze and
joined again: "Reconnecting to ... (attempt 1 of 5)" over the precache
(owner, 2026-11-20). The same in a match ends in a reconnect mid-game.

A watcher thread sends what the main thread cannot: once the main thread
has gone TNK_STALL seconds without finishing a frame, and the connection
has sent nothing for TNK_EVERY seconds, it sends an unreliable clc_nop on
the connection, every TNK_EVERY seconds until the frame ends. A stock
server takes it as any keepalive: it moves the client's timeout and is
otherwise ignored. net_keepalive_thread 0 turns the watcher off.

Sharing: the main thread hands the connection over at the end of every
frame and takes it back under the mutex when the connection or the client's
sockets close (TouchNet_Close, NetConn_CloseClientPorts), so the watcher
never touches a freed connection or socket; it holds the mutex for the whole
send. While it sends, the main thread is inside one long frame. The only
connection state both change is the unreliable sequence number: should a
loader's CL_KeepaliveMessage send at the same instant, two packets may carry
the same number and the server keeps one of them -- one nop is lost.
Encrypted (crypto) and QuakeWorld connections, a local game and a demo are
left alone. Builds without threads (web, dedicated) do nothing.
*/

#include "quakedef.h"
#include "thread.h"
#include "lhnet.h"
#include "netconn.h"
#include "crypto.h"
#include "touch_netkeep.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define TNK_STALL 2.5   // seconds the main thread is stuck in one frame before the watcher speaks
#define TNK_EVERY 2.0   // seconds between keepalives while it is stuck
#define TNK_TICK 0.25   // how often the watcher looks

static cvar_t net_keepalive_thread = {CF_CLIENT, "net_keepalive_thread", "1", "keep a server connection alive from a second thread while one frame takes seconds (a long load step on a slow device), so the server does not time the player out (Godnotic)"};

static struct
{
	void *mutex;
	void *thread;
	netconn_t *conn;        // the connection being watched, NULL = none (under mutex)
	volatile double beat;   // Sys_DirtyTime at the end of the main thread's last frame
	volatile int on;        // net_keepalive_thread
	int seen_sent;          // conn->packetsSent the watcher last saw
	double seen_time;       // when it changed
	volatile int sent;      // keepalives the watcher sent (for the log)
}
tnk;

static void TNK_Sleep(double seconds)
{
#ifdef _WIN32
	Sleep((DWORD)(seconds * 1000));
#else
	struct timespec ts;
	ts.tv_sec = (time_t)seconds;
	ts.tv_nsec = (long)((seconds - ts.tv_sec) * 1e9);
	nanosleep(&ts, NULL);
#endif
}

// the watcher: under the mutex, conn is alive and its socket open
static void TNK_Check(double now)
{
	unsigned char packet[NET_HEADERSIZE + 1];
	netconn_t *conn = tnk.conn;
	if (!conn)
	{
		tnk.seen_time = now;
		return;
	}
	if (conn->packetsSent != tnk.seen_sent)
	{
		tnk.seen_sent = conn->packetsSent;
		tnk.seen_time = now;
	}
	if (!tnk.on || now - tnk.beat < TNK_STALL || now - tnk.seen_time < TNK_EVERY)
		return;
	StoreBigLong(packet, (NET_HEADERSIZE + 1) | NETFLAG_UNRELIABLE);
	StoreBigLong(packet + 4, conn->outgoing_unreliable_sequence);
	packet[NET_HEADERSIZE] = clc_nop;
	conn->outgoing_unreliable_sequence++;
	if (LHNET_Write(conn->mysocket, packet, sizeof(packet), &conn->peeraddress) == (int)sizeof(packet))
	{
		conn->packetsSent++;
		conn->unreliableMessagesSent++;
		tnk.sent++;
	}
	tnk.seen_sent = conn->packetsSent;
	tnk.seen_time = now;
}

static int TNK_Thread(void *unused)
{
	for (;;)
	{
		TNK_Sleep(TNK_TICK);
		Thread_LockMutex(tnk.mutex);
		TNK_Check(Sys_DirtyTime());
		Thread_UnlockMutex(tnk.mutex);
	}
	return 0;
}

// a connection the watcher may keep alive
static netconn_t *TNK_Watchable(void)
{
	lhnetaddresstype_t type;
	if (cls.state != ca_connected || !cls.netcon || sv.active || cls.demoplayback || cls.protocol == PROTOCOL_QUAKEWORLD)
		return NULL;
	if (cls.netcon->crypto.authenticated || !cls.netcon->mysocket)
		return NULL;
	type = LHNETADDRESS_GetAddressType(&cls.netcon->peeraddress);
	if (type != LHNETADDRESSTYPE_INET4 && type != LHNETADDRESSTYPE_INET6)
		return NULL;
	return cls.netcon;
}

// the main thread is alive; the connection to watch, if any
static void TNK_Handover(void)
{
	netconn_t *conn;
	int sent;
	static int logged;
	tnk.beat = Sys_DirtyTime();
	tnk.on = net_keepalive_thread.integer != 0;
	conn = TNK_Watchable();
	if (!tnk.mutex)
	{
		if (!conn || !tnk.on || !Thread_HasThreads())
			return;
		tnk.mutex = Thread_CreateMutex();
		if (!tnk.mutex)
			return;
		tnk.thread = Thread_CreateThread(TNK_Thread, NULL);
		if (!tnk.thread)
		{
			Con_DPrintf("net_keepalive_thread: no thread\n");
			return; // the mutex stays: no second try every frame
		}
	}
	if (!tnk.thread)
		return;
	if (conn != tnk.conn)
	{
		Thread_LockMutex(tnk.mutex);
		tnk.conn = conn;
		tnk.seen_sent = conn ? conn->packetsSent : 0;
		tnk.seen_time = tnk.beat;
		Thread_UnlockMutex(tnk.mutex);
	}
	sent = tnk.sent;
	if (sent != logged)
	{
		// after the long frame: say what kept the connection
		Con_Printf("Kept the connection alive through a long frame (%d keepalive%s from the watcher)\n", sent - logged, sent - logged == 1 ? "" : "s");
		logged = sent;
	}
}

void TouchNetKeep_Closing(netconn_t *conn)
{
	if (!tnk.mutex || !conn)
		return;
	Thread_LockMutex(tnk.mutex);
	if (tnk.conn == conn)
		tnk.conn = NULL;
	Thread_UnlockMutex(tnk.mutex);
}

void TouchNetKeep_PortsClosing(void)
{
	if (!tnk.mutex)
		return;
	Thread_LockMutex(tnk.mutex);
	tnk.conn = NULL;
	Thread_UnlockMutex(tnk.mutex);
}

// the main thread stuck for N seconds: what a long load step on a slow
// device does, for tests (scripts/test-net-away.sh stall-*). "join": at the
// end of the first frame in the middle of a signon (after the connection is
// accepted, before the player is in the game).
static double tnk_stall_armed;

static void TNK_Stall(double secs)
{
	double until;
	Con_Printf("Stalling the main thread for %.1f s (signon %d of %d)\n", secs, cls.signon, SIGNONS);
	until = Sys_DirtyTime() + secs;
	while (Sys_DirtyTime() < until)
		TNK_Sleep(0.05);
}

static void TNK_StallFrame(void)
{
	if (tnk_stall_armed > 0 && cls.state == ca_connected && cls.signon > 0 && cls.signon < SIGNONS)
	{
		double secs = tnk_stall_armed;
		tnk_stall_armed = 0;
		TNK_Stall(secs);
	}
}

static void TouchNetKeep_Stall_f(cmd_state_t *cmd)
{
	double secs = Cmd_Argc(cmd) > 1 ? atof(Cmd_Argv(cmd, 1)) : 0;
	if (secs <= 0 || secs > 600)
	{
		Con_Printf("touch_net_stall <seconds> [join]: block the main thread that long, now or in the next signon (a test of net_keepalive_thread)\n");
		return;
	}
	if (Cmd_Argc(cmd) > 2 && !strcmp(Cmd_Argv(cmd, 2), "join"))
		tnk_stall_armed = secs;
	else
		TNK_Stall(secs);
}

void TouchNetKeep_Frame(void)
{
	TNK_Handover();
	TNK_StallFrame();
}

void TouchNetKeep_Init(void)
{
	Cvar_RegisterVariable(&net_keepalive_thread);
	Cmd_AddCommand(CF_CLIENT, "touch_net_stall", TouchNetKeep_Stall_f, "block the main thread for this many seconds, as a long load step on a slow device does (for tests)");
}

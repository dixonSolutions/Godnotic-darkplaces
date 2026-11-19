/*
Godnotic: the server's own download (csprogs.dat) at high ping.

A public server whose csprogs.dat is not ours sends it over the game
connection (svc_downloaddata, about 4 MB). The server streams blocks and
keeps one expected position: an ack whose start is that position moves it
on by the ack's size; an ack with any other start makes the server seek its
file back to the expected position and send everything again from there
(sv_user.c, clc_ackdownloaddata). It is go-back-N, and stock clients ack
every block as it arrives: on a link with a 300-500 ms round trip, a little
loss and some jitter, every lost or overtaken block costs a resend of all
the blocks after it, and each of the acks for the blocks already on the way
seeks the server back once more. On a link that reorders packets the
download crawled (219 s for 4 MB at 500 ms, 5% loss, 150 ms jitter, against
101 s here); on one that keeps their order each loss costs the server a
round trip of resends either way (the server, not the client, decides
that), about 90 s at 500 ms and 5% loss, 25 s without loss. A first join
to such a server spent minutes on "Downloading" or failed outright (below);
a later try got in (a file that did arrive is cached): "high-ping servers
need several join attempts" (owner, 2026-11-20).

Here the client keeps a map of the bytes it has (the stock code writes every
block at its own position, whatever is acked) and acks so that the server is
never sent back over what the client has:
  - acks carry the server from where the last ones left it to the first byte
    not received, up to 32767 bytes per ack (one per few dozen blocks, not
    one per block: an input packet has room for four);
  - a block beyond a gap is kept and not acked (a block overtaken by jitter
    fills the gap a moment later);
  - a gap that stays for TDL_REORDER seconds is a loss: one out-of-place ack
    makes the server seek back to it, and no other is asked for until a
    resend can have arrived (twice the measured round trip);
  - a block from before the first missing byte (the server still sending
    after a seek, or an ack of ours that got lost) is acked too: if the
    server expects it, it moves on and is carried on from there; if not, it
    seeks to where it expects -- forward, never past what the client has.
The server's finished message and the CRC check are the stock ones, but the
download's length is the furthest byte received (TouchNetDL_Stored): the
stock code took the end of the last block processed, so a block arriving
late -- overtaken, or resent after a seek, with stock acks too -- made a
complete download "corrupt", the bundled csprogs.dat was used, and the
server dropped the join: "Your csprogs.dat is not the same version as the
server" (the owner's tablet hit it too: csprogs-report.md).

net_download_inorder 0 restores the stock acks.
*/

#include "quakedef.h"
#include "client.h"
#include "touch_netdl.h"

#define TDL_SPANS 512      // separate runs of received bytes remembered
#define TDL_REORDER 0.25   // seconds a gap may last before it counts as a loss
#define TDL_MAXACK 32767   // the largest size an ack carries (a signed short)

static cvar_t net_download_inorder = {CF_CLIENT, "net_download_inorder", "1", "ack the server's download so a lost or overtaken block costs one resend of that block, not of everything after it (Godnotic; 0 = stock acks)"};

typedef struct { int start, end; } tdl_span_t;

static struct
{
	int number;                  // the download these blocks belong to (cls.qw_downloadnumber)
	int pos;                     // the first byte not received: the end of the run from 0
	int acked;                   // acks sent carry the server to here (it may lag: a lost packet)
	tdl_span_t have[TDL_SPANS];  // received bytes, sorted, merged
	int nhave;
	int dup_start, dup_size;     // a block from before pos to ack (0 size and start = none)
	qbool dup;
	qbool seek;                  // send an out-of-place ack: the server seeks to where it expects
	double gap_since;            // when data beyond pos first arrived, 0 = none
	double asked;                // when the last seek was asked for, 0 = not waiting
	int asked_pos;
	double rtt;                  // round trip estimate: ask to resend arriving
	int asks;                    // seeks asked for in this download
	qbool done;
}
tdl;

// Fill the free slots of the next input packet (CL_SendMove sends the slots
// up to the first empty one), most useful first: a block from before pos
// (it moves a lagging server on, or makes it seek to where it expects), then
// acks that carry the server from where our acks left it to pos, up to
// TDL_MAXACK bytes each, then a seek asked for a lost block.
static void TDL_Feed(void)
{
	int i;
	for (i = 0; i < CL_MAX_DOWNLOADACKS; i++)
	{
		cl_downloadack_t *a = &cls.dp_downloadack[i];
		if (a->start || a->size)
			continue;
		if (tdl.dup)
		{
			a->start = tdl.dup_start;
			a->size = tdl.dup_size;
			tdl.dup = false;
			// a server that expected it goes on from its end: carry it from there
			if (tdl.dup_start + tdl.dup_size < tdl.acked)
				tdl.acked = tdl.dup_start + tdl.dup_size;
		}
		else if (tdl.acked < tdl.pos)
		{
			a->start = tdl.acked;
			a->size = min(TDL_MAXACK, tdl.pos - tdl.acked);
			tdl.acked += a->size;
		}
		else if (tdl.seek)
		{
			a->start = tdl.pos + 1; // not what the server expects: it seeks
			a->size = 0;
			tdl.seek = false;
		}
		else
			break;
	}
}

// remember [start, end) as received
static void TDL_Have(int start, int end)
{
	int i, j;
	if (end <= start)
		return;
	for (i = 0; i < tdl.nhave && tdl.have[i].end < start; i++)
		;
	if (i < tdl.nhave && tdl.have[i].start <= end)
	{
		// overlaps or touches span i: grow it, then swallow the ones it reaches
		tdl.have[i].start = min(tdl.have[i].start, start);
		tdl.have[i].end = max(tdl.have[i].end, end);
		for (j = i + 1; j < tdl.nhave && tdl.have[j].start <= tdl.have[i].end; j++)
			tdl.have[i].end = max(tdl.have[i].end, tdl.have[j].end);
		memmove(tdl.have + i + 1, tdl.have + j, (tdl.nhave - j) * sizeof(tdl.have[0]));
		tdl.nhave -= j - i - 1;
		return;
	}
	if (tdl.nhave >= TDL_SPANS)
		return; // it comes again after a seek back
	memmove(tdl.have + i + 1, tdl.have + i, (tdl.nhave - i) * sizeof(tdl.have[0]));
	tdl.have[i].start = start;
	tdl.have[i].end = end;
	tdl.nhave++;
}

// pos is always the first byte not received: the end of the run from 0.
// The end of the received run that holds pos, or pos when none does
static int TDL_RunEnd(int pos)
{
	int i;
	for (i = 0; i < tdl.nhave; i++)
		if (tdl.have[i].start <= pos && pos < tdl.have[i].end)
			return tdl.have[i].end;
	return pos;
}

// is there received data past pos (a gap before it)?
static qbool TDL_Beyond(int pos)
{
	return tdl.nhave > 0 && tdl.have[tdl.nhave - 1].end > pos && TDL_RunEnd(pos) < tdl.have[tdl.nhave - 1].end;
}

qbool TouchNetDL_Received(int start, int size)
{
	double now;
	int oldpos;
	if (!net_download_inorder.integer || !cls.qw_downloadname[0] || !cls.qw_downloadmemory)
		return false;
	now = Sys_DirtyTime();
	if (tdl.number != cls.qw_downloadnumber)
	{
		// a new download (also of the same file again, into a buffer that may
		// well have the old one's address: number, not memory, tells them apart)
		memset(&tdl, 0, sizeof(tdl));
		tdl.number = cls.qw_downloadnumber;
		tdl.rtt = 0.5;
	}
	TDL_Have(start, start + size);
	oldpos = tdl.pos;
	if (start < tdl.pos)
	{
		// sent before a seek, or our ack of it got lost: ack it (the newest
		// such block), it moves a lagging server on or seeks it forward
		tdl.dup = true;
		tdl.dup_start = start;
		tdl.dup_size = size;
	}
	tdl.pos = TDL_RunEnd(tdl.pos);
	if (tdl.asked && tdl.pos > tdl.asked_pos)
	{
		tdl.rtt = bound(0.05, 0.7 * tdl.rtt + 0.3 * (now - tdl.asked), 3);
		tdl.asked = 0;
	}
	if (!TDL_Beyond(tdl.pos))
		tdl.gap_since = 0;
	else if (!tdl.gap_since || tdl.pos != oldpos)
		tdl.gap_since = now; // a new gap: give a block overtaken by jitter its moment
	// a gap that lasts is a loss: ask the server to seek back to it (an ack
	// out of place does that), once per round trip
	if (tdl.gap_since && now - tdl.gap_since >= TDL_REORDER && (!tdl.asked || now - tdl.asked >= max(1.0, 2 * tdl.rtt + TDL_REORDER)))
	{
		tdl.seek = true;
		tdl.asked = now;
		tdl.asked_pos = tdl.pos;
		tdl.asks++;
	}
	if (!tdl.done && tdl.pos >= cls.qw_downloadmemorymaxsize)
	{
		tdl.done = true;
		Con_Printf("Download of %s: %d seek back%s asked, round trip %.0f ms\n", cls.qw_downloadname, tdl.asks, tdl.asks == 1 ? "" : "s", tdl.rtt * 1000);
	}
	TDL_Feed();
	return true;
}

void TouchNetDL_Stored(int start, int size)
{
	static int number = -1, furthest;
	if (number != cls.qw_downloadnumber)
	{
		number = cls.qw_downloadnumber;
		furthest = 0;
	}
	furthest = max(furthest, start + size);
	cls.qw_downloadmemorycursize = furthest;
}

void TouchNetDL_Init(void)
{
	Cvar_RegisterVariable(&net_download_inorder);
}

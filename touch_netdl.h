#ifndef TOUCH_NETDL_H
#define TOUCH_NETDL_H

// Godnotic: the server's own download (csprogs.dat) acked in order, so it
// keeps its pace at high ping (touch_netdl.c).

void TouchNetDL_Init(void);
// CL_ParseDownload: a block arrived; true = its ack is handled here (the
// stock code then records none)
qbool TouchNetDL_Received(int start, int size);

// CL_ParseDownload, after the block is written: the download's length is the
// furthest byte received. The stock code takes the last block's end, so a
// block arriving late (overtaken, or resent after a seek) made a complete
// download look short and "corrupt", and the join then failed with "Your
// csprogs.dat is not the same version as the server".
void TouchNetDL_Stored(int start, int size);

// The server's download in progress (csprogs.dat), for the join card: bytes received from 0, the file's
// size as sent (0 until the server says), a speed in bytes/s over the last second or so. False when none.
qbool TouchNetDL_Progress(double *got, double *total, double *speed);

#endif

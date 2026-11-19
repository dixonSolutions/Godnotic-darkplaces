/*
Godnotic: answering votes with a finger.

A small round "i" chip sits in the touch HUD throughout a match. Tapping it
opens the called-vote card, including after this player has voted. The game's
own map and gametype votes use their on-screen selection instead.

It is drawn by the engine over whatever CSQC is loaded, so it is the same in a
local match (the port's CSQC) and on any server (the server's CSQC), with the
touch controls on or hidden. The vote state comes from the loaded CSQC's vote
globals where they can be read, and from the server's vote prints otherwise
(touch_vote_parse.h). Fingers that land on the chip or the open card are kept
out of the engine finger table, so neither touch HUD nor the CSQC sees them.
*/
#ifndef TOUCH_VOTE_H
#define TOUCH_VOTE_H

#include "qtypes.h"

// Cvars (SCR_Init).
void TouchVote_Init(void);

// Every server print, before the CSQC or the console gets it (CSQC_AddPrintText).
void TouchVote_ServerPrint(const char *msg, size_t len);

// Reads the vote state and draws the chip and the card (2D overlay pass).
void TouchVote_Draw(void);

// SDL finger events, before the finger table: true when the finger belongs to
// the chip or the card, and must be kept from everything else. type 0 down,
// 1 motion, 2 up; id as in the finger table (finger id + 1).
qbool TouchVote_Finger(int type, int id, float nx, float ny);

// Escape / Android Back in the game: true if it closed the card.
qbool TouchVote_Back(void);

#endif

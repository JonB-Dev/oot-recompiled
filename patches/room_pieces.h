#ifndef ROOM_PIECES_H
#define ROOM_PIECES_H

struct PlayState;

// The painted elements (display lists) of the rooms loaded where the player is, listed to the
// program for the Debugging menu and each shown, painted out or hidden as its switch says
// (patches/room_pieces.c). Called once a frame, before the frame is drawn.
void RoomPieces_Update(struct PlayState* play);

#endif

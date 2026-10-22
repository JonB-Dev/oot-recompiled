#ifndef AIR_MOVERS_H
#define AIR_MOVERS_H

struct PlayState;

// The bodies near the eye the dust collides with, each actor's own collision cylinder and its
// motion this update, published for the program (patches/air_movers.c). Called once per update,
// after it.
void AirMovers_Publish(struct PlayState* play);

#endif

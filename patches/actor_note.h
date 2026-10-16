#ifndef ACTOR_NOTE_H
#define ACTOR_NOTE_H

struct PlayState;

// How full the scene is and where the Sun's Song stands, written to the program's log
// (patches/actor_note.c). Called once per update, after it.
void ActorNote_Update(struct PlayState* play);

#endif

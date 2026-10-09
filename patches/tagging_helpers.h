// What the tagging patches share: the renderer's matrix group vocabulary with the component
// choices spelled out, the actor's identity from the native registry, and the three moves every
// skeleton path makes (push a limb's group, push its post-limb group, pop).
//
// HOW A TAG WORKS. `gEXMatrixGroup` is a two word display list command the renderer understands
// and the console never did. It opens a GROUP: every matrix loaded until the matching
// `gEXPopMatrixGroup` belongs to it, and the group's ID is how the renderer pairs this frame's
// matrix with last frame's to draw the frames in between. DECOMPOSED interpolation splits the
// matrix into position, rotation, scale and skew and interpolates each, which is what a limb
// wants: a joint that rotated thirty degrees should sweep through fifteen, not slide along the
// chord between its two positions. (The camera uses SIMPLE interpolation for the opposite
// reason; see camera_tagging.c.)
//
// THE TWO WORDS matter to anyone stepping through a display list: the second holds the ID and
// the packed component flags, and a walker that reads it as a command sees garbage. The guard
// in src/game/dl_check.cpp knows to step over it.
//
// Every macro here passes the renderer's own defaults for the two components the reference
// project's older renderer did not have: texture coordinates are never interpolated (a texture
// that scrolls is a tile change, not a coordinate change) and the lighting look-at vector is
// left to the renderer's judgment, which is what an untagged group gets.

#ifndef TAGGING_HELPERS_H
#define TAGGING_HELPERS_H

#include "patches.h"
#include "transform_ids.h"

#include "ultra64.h"
#include "rt64_extended_gbi.h"

// Position, rotation, scale, skew and perspective interpolated; vertex positions not (a mesh
// that deforms is the skin path's business, below). The ordinary limb.
#define gEXMatrixGroupDecomposedNormal(cmd, id, push, proj, edit) \
    gEXMatrixGroupDecomposed(cmd, id, push, proj, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, \
        G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_SKIP, \
        G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR, edit, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO)

// As above with the rotation held: a billboard, which faces the camera by construction and must
// not turn edge-on between two camera frames (phase 39).
#define gEXMatrixGroupDecomposedSkipRot(cmd, id, push, proj, edit) \
    gEXMatrixGroupDecomposed(cmd, id, push, proj, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_SKIP, \
        G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_SKIP, \
        G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR, edit, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO)

// Position and rotation held, the rest interpolated: something that teleported this frame but
// whose scale animation should still be smooth.
#define gEXMatrixGroupDecomposedSkipPosRot(cmd, id, push, proj, edit) \
    gEXMatrixGroupDecomposed(cmd, id, push, proj, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, \
        G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_SKIP, \
        G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR, edit, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO)

// Nothing interpolated but the tiles: the group is named so the renderer knows it is the same
// thing, and drawn where the game put it this frame.
#define gEXMatrixGroupDecomposedSkipAll(cmd, id, push, proj, edit) \
    gEXMatrixGroupDecomposed(cmd, id, push, proj, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, \
        G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, \
        G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR, edit, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO)

// Everything including the vertex positions: a skin mesh, whose vertices are deformed on the
// CPU every frame and so carry the motion themselves (phase 38).
#define gEXMatrixGroupDecomposedVerts(cmd, id, push, proj, edit) \
    gEXMatrixGroupDecomposed(cmd, id, push, proj, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, \
        G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, \
        G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR, edit, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO)

// EVERY GROUP BELOW IS OPENED EDITABLE (G_EX_EDIT_ALLOW). The billboard tracking (phase 39,
// billboard_tagging.c) holds a billboard's rotation on the frames where the camera cut by
// editing its group by the matrix's address at the end of the frame, and the renderer refuses
// that edit on a group opened with G_EX_EDIT_NONE. A fairy's glow is a billboard drawn inside
// the fairy's limb group, so the limb groups have to accept it. Editable costs the renderer
// nothing more than remembering the address.

// The native registry's answer for an address: the spawn index of the live actor there, or 0.
// The address crosses as a plain integer and is validated before it is looked up, so any pointer
// the game hands a skeleton draw can be asked about safely.
DECLARE_FUNC(u32, recomp_actor_index, u32 actor_address);

// The base transform ID for whatever the skeleton code was handed as its `arg`. By convention
// that is the actor being drawn, but nothing enforces it, so the registry decides: an address it
// does not know (a NULL, a struct that is not an actor, an actor that never registered) gives
// TRANSFORM_ID_NONE and the draw goes out untagged, exactly as it did before this plan.
static inline u32 tag_actor_id(void* arg) {
    if (arg == NULL) {
        return TRANSFORM_ID_NONE;
    }
    return ACTOR_TRANSFORM_ID(recomp_actor_index((u32)arg));
}

// Open a limb's group before its matrix is loaded. Nothing is written for an untagged actor,
// so the pop below must test the same ID.
static inline Gfx* tag_push_limb(Gfx* dlist, u32 actor_id, s32 limb_index) {
    if (actor_id != TRANSFORM_ID_NONE) {
        gEXMatrixGroupDecomposedNormal(dlist++, actor_id + ACTOR_TRANSFORM_LIMB_OFFSET + (u32)limb_index,
                                       G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);
    }
    return dlist;
}

// Open the group for whatever the limb's post-draw callback draws: a held item, a shield, an
// eye. Several matrices under one ID are fine; the renderer pairs them in the order they appear,
// which is stable for as long as the callback draws the same things.
static inline Gfx* tag_push_post_limb(Gfx* dlist, u32 actor_id, s32 limb_index) {
    if (actor_id != TRANSFORM_ID_NONE) {
        gEXMatrixGroupDecomposedNormal(dlist++, actor_id + ACTOR_TRANSFORM_POST_LIMB_OFFSET + (u32)limb_index,
                                       G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);
    }
    return dlist;
}

// Open a skin mesh limb's group (phase 38): position, rotation AND the vertices interpolated,
// because a skin limb is posed by moving its vertices on the CPU rather than by a matrix, so
// the motion is in the vertex data. Numbered from 1 like the other limbs, so slot 0 stays the
// whole-actor tag's.
static inline Gfx* tag_push_skin_limb(Gfx* dlist, u32 actor_id, s32 limb_index) {
    if (actor_id != TRANSFORM_ID_NONE) {
        gEXMatrixGroupDecomposedVerts(dlist++, actor_id + ACTOR_TRANSFORM_LIMB_OFFSET + 1U + (u32)limb_index,
                                      G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);
    }
    return dlist;
}

// Close whichever group was opened last, if one was.
static inline Gfx* tag_pop(Gfx* dlist, u32 actor_id) {
    if (actor_id != TRANSFORM_ID_NONE) {
        gEXPopMatrixGroup(dlist++, G_MTX_MODELVIEW);
    }
    return dlist;
}

#endif // TAGGING_HELPERS_H

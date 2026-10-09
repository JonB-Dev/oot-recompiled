// The room pieces behind the camera, kept for the traced lighting (upgrades phase 57).
//
// WHY. A scene with the traced lighting on holds only what the game drew this frame. A room
// of the cullable kind (Hyrule Field, Kokiri Forest, most of the outdoors) is drawn piece by
// piece, and the game drops every piece whose bounding sphere lies entirely behind the camera.
// That piece is then not in the traced scene at all: the shadow it casts forward onto the
// visible ground ends where the culling does, its reflection in the water is missing, and the
// edge of both moves as the camera turns (the user, 2026-09-24: "a moving edge ... im able to
// jump backward out of this and see NO shadows or lighting"; "the same issue applies to
// reflections too"; and their own guess, "how the game would unrender things out of view").
//
// WHAT. With the traced lighting on, a piece is kept while its bounding sphere is within
// ROOM_BEHIND_REACH world units behind the camera as well as in front. Sideways the game never
// culled these (only the depth is tested), so nothing changes there. With the lighting off the
// test is the game's own, bit for bit. The debug modes of the original (registers no retail
// build sets) are reproduced as the off case only.
//
// Reproduced from the decompilation's z_room.c at the pinned commit; a patch replaces the whole
// function, so everything it did is here.

#include "patches.h"

#include "actor.h"
#include "gfx.h"
#include "gfx_setupdl.h"
#include "light.h"
#include "play_state.h"
#include "regs.h"
#include "room.h"
#include "segmented_address.h"
#include "skin_matrix.h"
#include "sys_matrix.h"

// The game's own, local to z_room.c: an entry in the depth sorted list of pieces to draw.
typedef struct RoomShapeCullableEntryLinked {
    /* 0x00 */ RoomShapeCullableEntry* entry;
    /* 0x04 */ f32 boundsNearZ;
    /* 0x08 */ struct RoomShapeCullableEntryLinked* prev;
    /* 0x0C */ struct RoomShapeCullableEntryLinked* next;
} RoomShapeCullableEntryLinked; // size = 0x10

// How far behind the camera a piece is kept, from the Shadow casters row through patches/culling.c
// (which reads the row once a frame): zero with the lighting off or the row at the game's own.
f32 culling_room_behind_reach(void);

// The game's own, in z_room.c and z_actor.c: the room's fog settings and the two frame setups.
extern Vec3f D_801270A0;
void func_800342EC(Vec3f* object, PlayState* play);
void func_8003435C(Vec3f* object, PlayState* play);
void func_80093C80(PlayState* play);

// How far behind the camera a piece is kept, in world units, is the Shadow casters row's
// (culling.c: nothing, 1200, 3000, or everything). A long shadow at a low sun reaches a few
// hundred units; a cliff's reflection in a lake can come from further; the cost is the pieces
// behind the player, which the culling of the sides never saved anyway.

// @recomp Patched so the pieces behind the camera stay in the frame while the lighting is traced.
RECOMP_PATCH void Room_DrawCullable(PlayState* play, Room* room, u32 flags) {
    RoomShapeCullable* roomShape;
    RoomShapeCullableEntry* roomShapeCullableEntry;
    RoomShapeCullableEntryLinked linkedEntriesBuffer[ROOM_SHAPE_CULLABLE_MAX_ENTRIES];
    RoomShapeCullableEntryLinked* head = NULL;
    RoomShapeCullableEntryLinked* tail = NULL;
    RoomShapeCullableEntryLinked* iter;
    RoomShapeCullableEntryLinked* insert;
    s32 i;
    Vec3f pos;
    Vec3f projectedPos;
    f32 projectedW;
    f32 entryBoundsNearZ;
    // @recomp The reach behind the camera: the game's own zero, or the traced lighting's.
    f32 behind = culling_room_behind_reach();

    OPEN_DISPS(play->state.gfxCtx, "../z_room.c", 287);

    if (flags & ROOM_DRAW_OPA) {
        func_800342EC(&D_801270A0, play);
        gSPSegment(POLY_OPA_DISP++, 0x03, room->segment);
        func_80093C80(play);
        gSPMatrix(POLY_OPA_DISP++, &gIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }

    if (flags & ROOM_DRAW_XLU) {
        func_8003435C(&D_801270A0, play);
        gSPSegment(POLY_XLU_DISP++, 0x03, room->segment);
        Gfx_SetupDL_25Xlu(play->state.gfxCtx);
        gSPMatrix(POLY_XLU_DISP++, &gIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }

    roomShape = &room->roomShape->cullable;
    roomShapeCullableEntry = SEGMENTED_TO_VIRTUAL(roomShape->entries);
    insert = linkedEntriesBuffer;

    // Pick and sort entries by depth
    for (i = 0; i < roomShape->numEntries; i++, roomShapeCullableEntry++) {
        pos.x = roomShapeCullableEntry->boundsSphereCenter.x;
        pos.y = roomShapeCullableEntry->boundsSphereCenter.y;
        pos.z = roomShapeCullableEntry->boundsSphereCenter.z;
        SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, &pos, &projectedPos, &projectedW);

        // @recomp The near side of the test reaches behind the camera by `behind`; zero is the
        // game's own test.
        if (-(f32)roomShapeCullableEntry->boundsSphereRadius - behind < projectedPos.z) {
            entryBoundsNearZ = projectedPos.z - roomShapeCullableEntry->boundsSphereRadius;
            if (entryBoundsNearZ < play->lightCtx.zFar) {
                insert->entry = roomShapeCullableEntry;
                insert->boundsNearZ = entryBoundsNearZ;

                iter = head;
                if (iter == NULL) {
                    head = tail = insert;
                    insert->prev = insert->next = NULL;
                } else {
                    do {
                        if (insert->boundsNearZ < iter->boundsNearZ) {
                            break;
                        }
                        iter = iter->next;
                    } while (iter != NULL);

                    if (iter == NULL) {
                        insert->prev = tail;
                        insert->next = NULL;
                        tail->next = insert;
                        tail = insert;
                    } else {
                        insert->prev = iter->prev;
                        if (insert->prev == NULL) {
                            head = insert;
                        } else {
                            insert->prev->next = insert;
                        }
                        iter->prev = insert;
                        insert->next = iter;
                    }
                }
                insert++;
            }
        }
    }

    R_ROOM_CULL_NUM_ENTRIES = roomShape->numEntries & 0xFFFF;

    // Draw entries, from nearest to furthest
    for (i = 1; head != NULL; head = head->next, i++) {
        Gfx* displayList;

        roomShapeCullableEntry = head->entry;
        if (flags & ROOM_DRAW_OPA) {
            displayList = roomShapeCullableEntry->opa;
            if (displayList != NULL) {
                gSPDisplayList(POLY_OPA_DISP++, displayList);
            }
        }
        if (flags & ROOM_DRAW_XLU) {
            displayList = roomShapeCullableEntry->xlu;
            if (displayList != NULL) {
                gSPDisplayList(POLY_XLU_DISP++, displayList);
            }
        }
    }

    R_ROOM_CULL_USED_ENTRIES = i - 1;

    CLOSE_DISPS(play->state.gfxCtx, "../z_room.c", 430);
}

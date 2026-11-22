// THE PAINTED ELEMENTS OF THE ROOMS WHERE THE PLAYER IS, EACH WITH ITS OWN SWITCH (2026-10-08).
//
// A tool for finding what draws something, in any scene. It began with the Master Sword room's hard
// bright spot where the old beam landed, which survives everything the lighting can do (with the
// local lights off it stays) and which two repaints of one room never touched: the player was in
// room 0 and both repainted room 1. The user, rather than another guess: "add all rooms into the
// debug menu so I can cycle through each one"; then "make it a dynamic based on the room that you're
// in ... I can actually just go down the list and turn on and off a painted element within the place
// that I'm currently at. That would give us the most flexibility and the easiest time for anything
// in the future that also has this".
//
// So every display list of every room loaded right now is a piece: the lists the room's shape
// names, and every list they call, in the order the game draws them. Each is reported to the
// program once when its room is loaded (recomp_room_piece_add), the Debugging menu lists them, and
// each has a switch the program holds (recomp_room_piece_state): shown as the game draws it, its
// painted white taken out (every vertex painted white takes the color of the vertices around it in
// the same piece), or hidden outright (its first command made the end of the list, so neither it
// nor anything it calls is drawn). Hiding a piece answers whether it is the drawing at all;
// taking its paint out answers whether the light is in its vertex colors or in its texture.
//
// Nothing is written anywhere but the rooms' own loaded copies, which the game reloads from the
// ROM whenever it loads a room, and everything changed is put back when the switch goes back or
// before the rooms are listed again.
#include "patches.h"
#include "room_pieces.h"

#include "play_state.h"
#include "room.h"

void recomp_room_pieces_begin(s32 scene, s32 room);
void recomp_room_piece_add(s32 room, u32 offset, u32 flags, u32 counts);
u32 recomp_room_pieces_present(s32 scene, s32 current, s32 previous);
s32 recomp_room_piece_state(s32 room, u32 offset);

#define PIECES_MAX 256
#define PIECE_VERTICES_MAX 8192
#define PIECE_DEPTH_MAX 6
#define PIECE_COMMANDS_MAX 8192
#define PIECE_WHITE 250
#define PIECE_RADIUS 80
#define PIECE_LEVEL 10

#define STATE_SHOWN 0
#define STATE_PAINT_OUT 1
#define STATE_HIDDEN 2

typedef struct RoomPiece {
    Gfx* dl;
    void* segment;
    u32 offset;
    u32 first[2];
    s32 vtxStart;
    s32 vtxCount;
    s16 room;
    s16 white;
    u8 translucent;
    u8 applied;
} RoomPiece;

typedef struct PieceVertex {
    Vtx* vtx;
    u8 color[3];
    u8 replacement;
} PieceVertex;

static RoomPiece sPieces[PIECES_MAX];
static s32 sPieceCount;
static PieceVertex sVertices[PIECE_VERTICES_MAX];
static s32 sVertexCount;
static void* sSlotSegment[2];
static s32 sSlotRoom[2] = { -1, -1 };
static s32 sScene = -1;
static u32 sGeneration;
static s32 sGenerationKnown;

static void* RoomPieces_Resolve(void* segment, u32 address) {
    if ((address >> 24) == 0x03) {
        return (u8*)segment + (address & 0x00FFFFFF);
    }
    if ((address >> 24) == 0x80) {
        return (void*)address;
    }
    return NULL;
}

static s32 RoomPieces_IsWhite(const u8* color) {
    return (color[0] >= PIECE_WHITE) && (color[1] >= PIECE_WHITE) && (color[2] >= PIECE_WHITE);
}

static s32 RoomPieces_Find(Gfx* dl) {
    s32 i;

    for (i = 0; i < sPieceCount; i++) {
        if (sPieces[i].dl == dl) {
            return i;
        }
    }
    return -1;
}

// The vertices one list loads itself, not those of the lists it calls (each is a piece of its own).
static void RoomPieces_Collect(void* segment, Gfx* dl, RoomPiece* piece) {
    s32 i;
    s32 k;
    s32 j;

    for (i = 0; i < PIECE_COMMANDS_MAX; i++) {
        u32 w0 = dl[i].words.w0;
        u32 w1 = dl[i].words.w1;
        u32 op = w0 >> 24;

        if (op == G_ENDDL) {
            return;
        }
        if ((op == G_DL) && (((w0 >> 16) & 0xFF) == G_DL_NOPUSH)) {
            return;
        }
        if (op != G_VTX) {
            continue;
        }

        {
            Vtx* vtx = (Vtx*)RoomPieces_Resolve(segment, w1);
            s32 count = (s32)((w0 >> 12) & 0xFF);

            if (vtx == NULL) {
                continue;
            }
            for (k = 0; (k < count) && (sVertexCount < PIECE_VERTICES_MAX); k++) {
                s32 seen = false;

                for (j = piece->vtxStart; j < sVertexCount; j++) {
                    if (sVertices[j].vtx == &vtx[k]) {
                        seen = true;
                        break;
                    }
                }
                if (seen) {
                    continue;
                }
                sVertices[sVertexCount].vtx = &vtx[k];
                sVertices[sVertexCount].color[0] = vtx[k].v.cn[0];
                sVertices[sVertexCount].color[1] = vtx[k].v.cn[1];
                sVertices[sVertexCount].color[2] = vtx[k].v.cn[2];
                sVertices[sVertexCount].replacement = 0;
                sVertexCount++;
            }
        }
    }
}

// What a white vertex becomes with the paint taken out: the median brightness of the vertices that
// are not white in the same piece, within the radius on its own level, else in the whole piece.
static u8 RoomPieces_Around(RoomPiece* piece, PieceVertex* at, s32 radius) {
    u8 values[64];
    s32 count = 0;
    s32 i;
    s32 j;

    for (i = piece->vtxStart; (i < piece->vtxStart + piece->vtxCount) && (count < 64); i++) {
        PieceVertex* other = &sVertices[i];
        s32 dx;
        s32 dy;
        s32 dz;

        if (RoomPieces_IsWhite(other->color)) {
            continue;
        }
        dx = other->vtx->v.ob[0] - at->vtx->v.ob[0];
        dy = other->vtx->v.ob[1] - at->vtx->v.ob[1];
        dz = other->vtx->v.ob[2] - at->vtx->v.ob[2];
        if ((radius > 0) && ((dy > PIECE_LEVEL) || (dy < -PIECE_LEVEL) || ((dx * dx + dz * dz) > (radius * radius)))) {
            continue;
        }
        for (j = count; (j > 0) && (values[j - 1] > other->color[0]); j--) {
            values[j] = values[j - 1];
        }
        values[j] = other->color[0];
        count++;
    }

    return (count > 0) ? values[count / 2] : 0;
}

static void RoomPieces_Walk(s32 room, void* segment, Gfx* dl, s32 depth, s32 translucent) {
    RoomPiece* piece;
    s32 i;

    if ((dl == NULL) || (sPieceCount >= PIECES_MAX) || (RoomPieces_Find(dl) >= 0)) {
        return;
    }

    piece = &sPieces[sPieceCount++];
    piece->dl = dl;
    piece->segment = segment;
    piece->offset = (u32)((u8*)dl - (u8*)segment);
    piece->first[0] = dl[0].words.w0;
    piece->first[1] = dl[0].words.w1;
    piece->room = room;
    piece->translucent = translucent;
    piece->applied = STATE_SHOWN;
    piece->vtxStart = sVertexCount;
    RoomPieces_Collect(segment, dl, piece);
    piece->vtxCount = sVertexCount - piece->vtxStart;
    piece->white = 0;
    for (i = piece->vtxStart; i < sVertexCount; i++) {
        if (RoomPieces_IsWhite(sVertices[i].color)) {
            u8 around = RoomPieces_Around(piece, &sVertices[i], PIECE_RADIUS);

            if (around == 0) {
                around = RoomPieces_Around(piece, &sVertices[i], 0);
            }
            sVertices[i].replacement = around;
            piece->white++;
        }
    }

    recomp_room_piece_add(room, piece->offset, (u32)translucent | ((u32)depth << 8),
                          (u32)piece->vtxCount | ((u32)piece->white << 16));

    if (depth >= PIECE_DEPTH_MAX) {
        return;
    }
    for (i = 0; i < PIECE_COMMANDS_MAX; i++) {
        u32 w0 = dl[i].words.w0;
        u32 op = w0 >> 24;

        if (op == G_ENDDL) {
            return;
        }
        if (op == G_DL) {
            RoomPieces_Walk(room, segment, (Gfx*)RoomPieces_Resolve(segment, dl[i].words.w1), depth + 1, translucent);
            if (((w0 >> 16) & 0xFF) == G_DL_NOPUSH) {
                return;
            }
        }
    }
}

static void RoomPieces_List(Room* room) {
    RoomShapeNormal* shape = (RoomShapeNormal*)RoomPieces_Resolve(room->segment, (u32)room->roomShape);
    RoomShapeDListsEntry* entries;
    s32 i;

    recomp_room_pieces_begin(sScene, room->num);
    if ((shape == NULL) || (shape->base.type != ROOM_SHAPE_TYPE_NORMAL)) {
        return;
    }
    entries = (RoomShapeDListsEntry*)RoomPieces_Resolve(room->segment, (u32)shape->entries);
    if (entries == NULL) {
        return;
    }
    for (i = 0; i < shape->numEntries; i++) {
        RoomPieces_Walk(room->num, room->segment, (Gfx*)RoomPieces_Resolve(room->segment, (u32)entries[i].opa), 0, false);
        RoomPieces_Walk(room->num, room->segment, (Gfx*)RoomPieces_Resolve(room->segment, (u32)entries[i].xlu), 0, true);
    }
}

// Puts a piece back as the game drew it, then does what the switch says.
static void RoomPieces_Apply(RoomPiece* piece, s32 state) {
    s32 i;
    s32 k;

    for (i = piece->vtxStart; i < piece->vtxStart + piece->vtxCount; i++) {
        PieceVertex* v = &sVertices[i];
        s32 out = (state == STATE_PAINT_OUT) && RoomPieces_IsWhite(v->color);

        for (k = 0; k < 3; k++) {
            v->vtx->v.cn[k] = out ? v->replacement : v->color[k];
        }
    }
    if (state == STATE_HIDDEN) {
        piece->dl[0].words.w0 = (u32)G_ENDDL << 24;
        piece->dl[0].words.w1 = 0;
    } else {
        piece->dl[0].words.w0 = piece->first[0];
        piece->dl[0].words.w1 = piece->first[1];
    }
    piece->applied = state;
}

// The room in this slot as it is now: its number and its copy, or none.
static void RoomPieces_Slot(Room* room, void** segment, s32* num) {
    if ((room->segment != NULL) && (room->num >= 0)) {
        *segment = room->segment;
        *num = room->num;
    } else {
        *segment = NULL;
        *num = -1;
    }
}

void RoomPieces_Update(PlayState* play) {
    void* segments[2];
    s32 nums[2];
    s32 i;
    u32 generation;

    RoomPieces_Slot(&play->roomCtx.curRoom, &segments[0], &nums[0]);
    RoomPieces_Slot(&play->roomCtx.prevRoom, &segments[1], &nums[1]);

    if ((play->sceneId != sScene) || (segments[0] != sSlotSegment[0]) || (segments[1] != sSlotSegment[1]) ||
        (nums[0] != sSlotRoom[0]) || (nums[1] != sSlotRoom[1])) {
        // Put back whatever was changed in a room that is still loaded in the same copy, so the
        // listing below reads the game's own drawing; a room that went away took its copy with it.
        for (i = 0; i < sPieceCount; i++) {
            RoomPiece* piece = &sPieces[i];
            s32 stays = (play->sceneId == sScene) &&
                        (((piece->segment == segments[0]) && (piece->room == nums[0])) ||
                         ((piece->segment == segments[1]) && (piece->room == nums[1])));

            if (stays && (piece->applied != STATE_SHOWN)) {
                RoomPieces_Apply(piece, STATE_SHOWN);
            }
        }

        sScene = play->sceneId;
        sPieceCount = 0;
        sVertexCount = 0;
        for (i = 0; i < 2; i++) {
            sSlotSegment[i] = segments[i];
            sSlotRoom[i] = nums[i];
        }
        if (segments[0] != NULL) {
            RoomPieces_List(&play->roomCtx.curRoom);
        }
        if ((segments[1] != NULL) && (segments[1] != segments[0])) {
            RoomPieces_List(&play->roomCtx.prevRoom);
        }
        sGenerationKnown = false;
    }

    generation = recomp_room_pieces_present(sScene, nums[0], nums[1]);
    if (sGenerationKnown && (generation == sGeneration)) {
        return;
    }
    sGeneration = generation;
    sGenerationKnown = true;
    for (i = 0; i < sPieceCount; i++) {
        s32 state = recomp_room_piece_state(sPieces[i].room, sPieces[i].offset);

        if (state != sPieces[i].applied) {
            RoomPieces_Apply(&sPieces[i], state);
        }
    }
}

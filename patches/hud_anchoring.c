// The interface anchored to the frame's edges (phase 44, OPEN-1). The game draws its interface
// in the console's 320 by 240 coordinates; in a wider frame the renderer keeps those at the
// center unless a rectangle or a viewport is told which edge it belongs to. This reproduces
// Interface_Draw with that told at each group's boundary: the hearts, the rupee and key
// counters and the magic meter to the left edge; the minimap with its marks and compass
// arrows, the B, C and start buttons with their icons and counts, and the A button (drawn in a
// small perspective viewport of its own, which takes whatever origin is in force) to the
// right edge; the targeting reticle, the carrots, the timers and the letters at the center as
// before; and the icon of an item being equipped, which travels from the centered pause menu
// to a button at the edge, with an origin that travels with it. The offsets are the console's
// own margins: nothing moves at 4:3 or with the interface ratio at "Original", where the
// renderer collapses every origin to the center, and at "Follow the view" the groups sit at
// the frame's edges with the margins they always had. Vertex-drawn elements move with the
// viewport the view emits, so each origin is set before the view is applied, as the reference
// project does.
//
// Everything else in the function is the game's, line for line: the diff against the source is
// the alignment calls, this comment, a copy of one file-static setup list the reproduction
// cannot reach by name, and the prototypes of the file's own helpers.
//
// Minimap_Draw follows, reproduced for one condition. The map data parks the dungeon-entrance
// icon at (1, 0), the frame's corner, for an overworld map with no entrance to mark, and flags
// it to draw always; a television's overscan hid that corner, this frame shows it, and the
// right-anchored minimap would carry the parked icon out of the corner into the sky. It is
// skipped where it is parked; a real entrance, placed on the minimap, draws as before.

#include "patches.h"

#include "array_count.h"
#include "attributes.h"
#include "controller.h"
#include "gfx.h"
#include "gfx_setupdl.h"
#include "language_array.h"
#include "regs.h"
#include "segmented_address.h"
#include "sys_matrix.h"
#include "printf.h"
#include "translation.h"
#include "sequence.h"
#include "sfx.h"
#include "audio.h"
#include "actor.h"
#include "interface.h"
#include "item.h"
#include "lifemeter.h"
#include "map.h"
#include "message.h"
#include "ocarina.h"
#include "pause.h"
#include "play_state.h"
#include "player.h"
#include "save.h"
#include "main.h"

#include "rt64_extended_gbi.h"

// Two statics of z_parameter.c. The archery score digits are scratch this function alone
// writes and reads, so the reproduction keeps its own. The environment hazard flag is raised
// by Interface_Update when a hot room or the water starts the timer and consumed here when
// the timer runs out (the player dies of the hazard), so it has to be the game's own: it is
// not in the exported table (the matching compiler keeps no symbol for a static), so its
// address was read from the recompiled code of the timer setters that clear it and is given
// to the recompiler in patches/game_static_syms.toml.
static u16 sHBAScoreDigits[] = { 0, 0, 0, 0 };
extern s16 sEnvHazardActive;

// The textures the function draws, and the sizes of two of them, which the game's source takes
// from its extracted asset headers; the patch build sees the headers the decompilation keeps
// under include/ and not those, so the symbols (all in this revision's table) are declared
// here and the two sizes written out (do_action_static.h, icon_item_static.h).
extern u64 gRupeeCounterIconTex[];
extern u64 gSmallKeyCounterIconTex[];
extern u64 gCounterDigit0Tex[];
extern u64 gClockIconTex[];
extern u64 gCarrotIconTex[];
extern u64 gArcheryScoreIconTex[];
extern u64 gMagicArrowEquipEffectTex[];
#define gMagicArrowEquipEffectTex_WIDTH 32
#define gMagicArrowEquipEffectTex_HEIGHT 32
#define DO_ACTION_TEX_WIDTH 48
#define DO_ACTION_TEX_HEIGHT 16
#define DO_ACTION_TEX_SIZE (DO_ACTION_TEX_WIDTH * DO_ACTION_TEX_HEIGHT / 2)

// The file's own helpers, declared in no header. All real symbols in this revision's table.
Gfx* Gfx_TextureIA8(Gfx* displayListHead, void* texture, s16 textureWidth, s16 textureHeight, s16 rectLeft, s16 rectTop,
                    s16 rectWidth, s16 rectHeight, u16 dsdx, u16 dtdy);
Gfx* Gfx_TextureI8(Gfx* displayListHead, void* texture, s16 textureWidth, s16 textureHeight, s16 rectLeft, s16 rectTop,
                   s16 rectWidth, s16 rectHeight, u16 dsdx, u16 dtdy);
void Interface_InitVertices(PlayState* play);
void func_8008A994(InterfaceContext* interfaceCtx);
void func_8008A8B8(PlayState* play, s32 topY, s32 bottomY, s32 leftX, s32 rightX);
void Interface_DrawItemButtons(PlayState* play);
void Interface_DrawItemIconTexture(PlayState* play, void* texture, s16 button);
void Interface_DrawAmmoCount(PlayState* play, s16 button, s16 alpha);
void Interface_DrawActionButton(PlayState* play);
void Interface_DrawActionLabel(GraphicsContext* gfxCtx, void* texture);
void Interface_LoadItemIcon1(PlayState* play, u16 button);
void Magic_DrawMeter(PlayState* play);

// The setup list the final fill uses, a static of z_parameter.c with no entry in the symbol
// table, so the reproduction carries its own copy, command for command.
static Gfx sInterfaceFillSetupDL[] = {
    gsDPPipeSync(),
    gsSPClearGeometryMode(G_ZBUFFER | G_SHADE | G_CULL_BOTH | G_FOG | G_LIGHTING | G_TEXTURE_GEN |
                          G_TEXTURE_GEN_LINEAR | G_SHADING_SMOOTH | G_LOD),
    gsDPSetOtherMode(G_AD_DISABLE | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE |
                         G_TD_CLAMP | G_TP_NONE | G_CYC_1CYCLE | G_PM_1PRIMITIVE,
                     G_AC_NONE | G_ZS_PIXEL | G_RM_CLD_SURF | G_RM_CLD_SURF2),
    gsDPSetCombineMode(G_CC_PRIMITIVE, G_CC_PRIMITIVE),
    gsSPEndDisplayList(),
};

// One origin for both kinds of element, the console's coordinates carried across: at the left
// edge they need nothing, at the right edge they are the console's width to the left of it,
// and with no origin the renderer places them at the center as it always did. The offsets are
// 10.2 fixed point, as the renderer reads them.
static void hud_align(GraphicsContext* gfxCtx, s32 origin) {
    s32 offset = (origin == G_EX_ORIGIN_RIGHT) ? -SCREEN_WIDTH * 4 : 0;

    OPEN_DISPS(gfxCtx, "../z_parameter.c", 0);
    gEXSetRectAlign(OVERLAY_DISP++, origin, origin, offset, 0, offset, 0);
    gEXSetViewportAlign(OVERLAY_DISP++, origin, offset, 0);
    CLOSE_DISPS(gfxCtx, "../z_parameter.c", 0);
}

// The icon of an item being equipped moves from its slot in the pause menu to a C button over
// a few frames (pauseCtx->equipAnimX, in tenths of the overlay's orthographic units). The
// menu is drawn at the center and the button sits at the right edge, so the origin follows the
// icon: the fraction of the way it has come picks a point between the center and the right
// edge, and the offset between the two, which is how the reference project moves it. The
// button positions are the pause menu's own table (z_kaleido_item.c, sCButtonPosX).
static void hud_align_equip(PlayState* play) {
    static const s16 sCButtonPosX[] = { 660, 900, 1140 };
    PauseContext* pauseCtx = &play->pauseCtx;
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    s32 origin = G_EX_ORIGIN_NONE;
    s32 offset = 0;

    if ((pauseCtx->state == PAUSE_STATE_MAIN) && (pauseCtx->mainState == PAUSE_MAIN_STATE_3) &&
        (pauseCtx->equipTargetCBtn < 3)) {
        s32 startX = pauseCtx->itemVtx[pauseCtx->equipTargetSlot * 4].v.ob[0] * 10;
        s32 endX = sCButtonPosX[pauseCtx->equipTargetCBtn];
        s32 span = endX - startX;
        s32 fraction = 0;

        if (span > 0) {
            fraction = ((pauseCtx->equipAnimX - startX) * 256) / span;
        } else if (span < 0) {
            fraction = ((startX - pauseCtx->equipAnimX) * 256) / -span;
        }
        if (fraction < 0) {
            fraction = 0;
        } else if (fraction > 256) {
            fraction = 256;
        }
        origin = G_EX_ORIGIN_CENTER + ((G_EX_ORIGIN_RIGHT - G_EX_ORIGIN_CENTER) * fraction) / 256;
        offset = (-(SCREEN_WIDTH / 2) - ((SCREEN_WIDTH / 2) * fraction) / 256) * 4;
    }

    OPEN_DISPS(gfxCtx, "../z_parameter.c", 0);
    gEXSetRectAlign(OVERLAY_DISP++, origin, origin, offset, 0, offset, 0);
    gEXSetViewportAlign(OVERLAY_DISP++, origin, offset, 0);
    CLOSE_DISPS(gfxCtx, "../z_parameter.c", 0);
}

// Photo mode (src/main/photo.h): 1 while it has the game's HUD hidden.
s32 recomp_photo_hud_hidden(void);

// THE HUD'S FADING (2026-10-09, src/game/hud.h): what the game is doing this update, and each part's
// opacity (bits 0 to 8, 256 the game's own) and color (bits 16 to 24, 256 full, 0 gray).
void recomp_hud_note(u32 activity);
u32 recomp_hud_part(s32 part);
// Each part's size (bits 0 to 9, percent).
u32 recomp_hud_layout(s32 part);
// Where the HUD sits when HUD sits is Custom: 0 otherwise; bit 31 set, then the distances from the
// frame's left, right, top and bottom edges in whole percent, bits 0, 8, 16 and 24 on. And the
// frame's width over the console's 320, 16.16 (src/game/hud.h).
u32 recomp_hud_place(void);
u32 recomp_hud_frame_q16(void);

#define HUD_PART_HEARTS 0
#define HUD_PART_MAGIC 1
#define HUD_PART_RUPEES 2
#define HUD_PART_BUTTONS 3
#define HUD_PART_MAP 4
#define HUD_PART_NONE -1

// THE CORNERS. The parts that share a corner move together when the HUD is placed (the magic
// meter stays under the hearts), and a part grows from the corner it sits in.
#define HUD_GROUP_TOP_LEFT 0       // the hearts and the magic meter
#define HUD_GROUP_BOTTOM_LEFT 1    // the rupee and key counters
#define HUD_GROUP_TOP_RIGHT 2      // the buttons
#define HUD_GROUP_BOTTOM_RIGHT 3   // the map
#define HUD_GROUPS 4

static s32 Hud_Group(s32 part) {
    switch (part) {
        case HUD_PART_HEARTS:
        case HUD_PART_MAGIC:
            return HUD_GROUP_TOP_LEFT;
        case HUD_PART_RUPEES:
            return HUD_GROUP_BOTTOM_LEFT;
        case HUD_PART_BUTTONS:
            return HUD_GROUP_TOP_RIGHT;
        default:
            return HUD_GROUP_BOTTOM_RIGHT;
    }
}

#define HUD_GROUP_RIGHT(group) (((group) == HUD_GROUP_TOP_RIGHT) || ((group) == HUD_GROUP_BOTTOM_RIGHT))
#define HUD_GROUP_BOTTOM(group) (((group) == HUD_GROUP_BOTTOM_LEFT) || ((group) == HUD_GROUP_BOTTOM_RIGHT))

#define HUD_PLACE_ON (1u << 31)

// The console's frame in quarter pixels, which is what rectangles, viewports and scissors use.
#define HUD_FRAME_W (SCREEN_WIDTH * 4)
#define HUD_FRAME_H (SCREEN_HEIGHT * 4)

// HOW MANY WORDS A COMMAND TAKES. Every pass over a part's commands steps by this, never one word
// at a time: the renderer's extended commands run to two or three words whose later words are
// data, and the right edge's alignment carries -1280 as an offset, whose top byte, 0xFB, reads as
// a set environment color if it is taken for a command of its own (it was, by the color pass,
// until 2026-10-09). The lengths are the macros' in rt64_extended_gbi.h.
static s32 Hud_Words(Gfx* g) {
    if ((g->words.w0 >> 24) != RT64_EXTENDED_OPCODE) {
        return 1;
    }
    switch (g->words.w0 & 0xFFFFFF) {
        case G_EX_TEXRECT_V1:
        case G_EX_SETSCISSORALIGN_V1:
            return 3;
        case G_EX_SETVIEWPORT_V1:
        case G_EX_SETSCISSOR_V1:
        case G_EX_SETRECTALIGN_V1:
        case G_EX_SETVIEWPORTALIGN_V1:
        case G_EX_MATRIXGROUP_V1:
        case G_EX_EDITGROUPBYADDRESS_V1:
        case G_EX_VERTEX_V1:
        case G_EX_MATRIX_FLOAT_V1:
        case G_EX_SETVERTEXSEGMENT_V1:
            return 2;
        default:
            return 1;
    }
}

#define HUD_ACT_TARGETING (1 << 0)
#define HUD_ACT_C_BUTTONS (1 << 1)
#define HUD_ACT_TALKING (1 << 2)
#define HUD_ACT_HEALTH_MAGIC (1 << 3)
#define HUD_ACT_RUPEES_ITEMS (1 << 4)
#define HUD_ACT_ANY_BUTTON (1 << 5)
#define HUD_ACT_HEALTH_LOW (1 << 6)
#define HUD_ACT_PAUSED (1 << 7)

// The game's own alphas as the update left them, put back after the draw, so the update goes on
// fading them from where it was and never from our scaled values.
typedef struct {
    u16 a;
    u16 b;
    u16 cLeft;
    u16 cDown;
    u16 cRight;
    u16 health;
    u16 magic;
    u16 minimap;
    s16 start;
} HudAlphas;

static HudAlphas sHudSaved;

// Where each part's commands begin in the overlay list this draw, for the color pass.
#define HUD_MARKS_MAX 8
static Gfx* sHudMarkAt[HUD_MARKS_MAX];
static s32 sHudMarkPart[HUD_MARKS_MAX];
static s32 sHudMarks;

static u32 sHudValue[5];
static u32 sHudLayout[5];

// Where each part's own drawing begins: after the viewport and, when placed, the origin that
// Hud_Part sets for it, so a pass can tell the part's commands from the ones put there for it.
static Gfx* sHudMarkBody[HUD_MARKS_MAX];

// Where the HUD sits this draw (recomp_hud_place, 0 unless Custom) and the frame's width over
// the console's 320, 16.16.
static u32 sHudPlace;
static u32 sHudFrameQ16;

// Each corner group's width as last drawn, in quarter pixels, so a placed group's origin is chosen
// where the whole group fits. A frame late, which a group whose width changes (a heart gained)
// spends one frame catching up on.
static s32 sHudGroupWidth[HUD_GROUPS];

// THE PIECES THE GAME DREW 1:1 AND THE SIZE PASS SCALED, this draw (Hud_Resize records them,
// Hud_Native wraps them). The renderer draws a 1:1 rectangle with the console's own per-pixel
// texture coordinates and a scaled one continuously, so a scaled piece is asked to be drawn the
// 1:1 way, to look at any size as it does at its own (renderer patch 0055).
#define HUD_NATIVE_MAX 96
static Gfx* sHudNative[HUD_NATIVE_MAX];
static s32 sHudNatives;

// The renderer's force upscale command with the value 2 (renderer patch 0055): every rectangle
// drawn while it stands takes the console's own per-pixel texture coordinates; 0 puts it back.
#define gEXForceNative2D(cmd, on)                                                         \
    G_EX_COMMAND1(cmd, PARAM(RT64_EXTENDED_OPCODE, 8, 24) | PARAM(G_EX_FORCEUPSCALE2D_V1, 24, 0), \
                  PARAM(((on) ? 2 : 0), 2, 0))

// WHERE A PLACED PART'S ORIGIN IS (the user, 2026-10-09: "custom positioning only up to the window
// size so stuff never gets hidden when scaling", and "the user can set a percentage away from the
// edge of the window so when the window does scale down and it starts to shrink, the user can
// choose its distance from where it actually sits, from all edges of the screen"). The renderer
// places an element by an origin, a share of the frame's width (0 the left edge, 0x400 the
// right), with an offset in the console's coordinates: a left corner's origin is its distance
// from the left edge, a right corner's is the frame less its distance from the right edge, and
// the size pass then puts the group's own edge exactly on it (Hud_Place). Each is kept where the
// whole group, as wide as it was last drawn, still fits in the frame.
static void Hud_PlaceOrigin(PlayState* play, s32 part) {
    s32 group = Hud_Group(part);
    f32 frame = (f32)sHudFrameQ16 * (1.0f / 65536.0f) * (f32)HUD_FRAME_W;
    f32 fits = (frame > 0.0f) ? ((f32)sHudGroupWidth[group] / frame) : 0.0f;
    f32 share;
    s32 origin;
    s32 offset;

    if (HUD_GROUP_RIGHT(group)) {
        share = 1.0f - (f32)((sHudPlace >> 8) & 0xFF) * 0.01f;
        if (share < fits) {
            share = fits;
        }
        offset = -HUD_FRAME_W;
    } else {
        share = (f32)(sHudPlace & 0xFF) * 0.01f;
        if (share > 1.0f - fits) {
            share = 1.0f - fits;
        }
        offset = 0;
    }
    if (share < 0.0f) {
        share = 0.0f;
    }
    if (share > 1.0f) {
        share = 1.0f;
    }
    origin = (s32)((share * (f32)G_EX_ORIGIN_RIGHT) + 0.5f);

    OPEN_DISPS(play->state.gfxCtx, "../z_parameter.c", 0);
    gEXSetRectAlign(OVERLAY_DISP++, origin, origin, offset, 0, offset, 0);
    gEXSetViewportAlign(OVERLAY_DISP++, origin, offset, 0);
    CLOSE_DISPS(play->state.gfxCtx, "../z_parameter.c", 0);
}

static u16 Hud_Scale(u16 alpha, u32 value) {
    return (u16)((alpha * (value & 0x1FF)) >> 8);
}

// A part begins: its alpha, scaled from the saved one, and where its commands begin.
static void Hud_Part(PlayState* play, s32 part) {
    InterfaceContext* interfaceCtx = &play->interfaceCtx;

    if (sHudMarks < HUD_MARKS_MAX) {
        Vp* vp = GRAPH_ALLOC(play->state.gfxCtx, sizeof(Vp));
        s32 mark = sHudMarks;

        sHudMarkAt[mark] = play->state.gfxCtx->overlay.p;
        sHudMarkPart[mark] = part;
        sHudMarks++;
        // THE PART'S OWN VIEWPORT (2026-10-09, the HUD's size): a copy of the interface's, so the
        // size pass can scale it with the part's rectangles and every other part keeps its own.
        *vp = interfaceCtx->view.vp;
        gSPViewport(play->state.gfxCtx->overlay.p++, vp);
        // ITS OWN ORIGIN, when the HUD is placed (HUD sits, Custom).
        if ((part >= 0) && (sHudPlace & HUD_PLACE_ON)) {
            Hud_PlaceOrigin(play, part);
        }
        sHudMarkBody[mark] = play->state.gfxCtx->overlay.p;
    }
    switch (part) {
        case HUD_PART_HEARTS:
            interfaceCtx->healthAlpha = Hud_Scale(sHudSaved.health, sHudValue[part]);
            break;
        case HUD_PART_MAGIC:
        case HUD_PART_RUPEES:
            interfaceCtx->magicAlpha = Hud_Scale(sHudSaved.magic, sHudValue[part]);
            break;
        case HUD_PART_MAP:
            interfaceCtx->minimapAlpha = Hud_Scale(sHudSaved.minimap, sHudValue[part]);
            break;
        case HUD_PART_BUTTONS:
            interfaceCtx->aAlpha = Hud_Scale(sHudSaved.a, sHudValue[part]);
            interfaceCtx->bAlpha = Hud_Scale(sHudSaved.b, sHudValue[part]);
            interfaceCtx->cLeftAlpha = Hud_Scale(sHudSaved.cLeft, sHudValue[part]);
            interfaceCtx->cDownAlpha = Hud_Scale(sHudSaved.cDown, sHudValue[part]);
            interfaceCtx->cRightAlpha = Hud_Scale(sHudSaved.cRight, sHudValue[part]);
            interfaceCtx->startAlpha = (s16)Hud_Scale((u16)sHudSaved.start, sHudValue[part]);
            // The hearts' alpha is also the most C up's may be, and C up is a button.
            interfaceCtx->healthAlpha = Hud_Scale(sHudSaved.health, sHudValue[part]);
            break;
        default:
            break;
    }
}

// What changed or is happening this update, for what brings the HUD back.
static u32 Hud_Activity(PlayState* play) {
    static s16 sLastHealth = -1;
    static s8 sLastMagic = -1;
    static s16 sLastRupees = -1;
    static s8 sLastKeys = -1;
    static u32 sLastItems = 0;
    Player* player = GET_PLAYER(play);
    Input* input = &play->state.input[0];
    u32 bits = 0;
    u32 items;
    s8 keys;
    s32 i;

    if ((player != NULL) &&
        (player->stateFlags1 & (PLAYER_STATE1_HOSTILE_LOCK_ON | PLAYER_STATE1_FRIENDLY_ACTOR_FOCUS | PLAYER_STATE1_PARALLEL))) {
        bits |= HUD_ACT_TARGETING;
    }
    if (input->cur.button & (BTN_CUP | BTN_CDOWN | BTN_CLEFT | BTN_CRIGHT)) {
        bits |= HUD_ACT_C_BUTTONS;
    }
    if (play->msgCtx.msgMode != MSGMODE_NONE) {
        bits |= HUD_ACT_TALKING;
    }
    if (input->press.button != 0) {
        bits |= HUD_ACT_ANY_BUTTON;
    }
    if (Health_IsCritical()) {
        bits |= HUD_ACT_HEALTH_LOW;
    }
    if (play->pauseCtx.state != PAUSE_STATE_OFF) {
        bits |= HUD_ACT_PAUSED;
    }

    if ((sLastHealth >= 0) && ((sLastHealth != gSaveContext.save.info.playerData.health) ||
                               (sLastMagic != gSaveContext.save.info.playerData.magic))) {
        bits |= HUD_ACT_HEALTH_MAGIC;
    }
    sLastHealth = gSaveContext.save.info.playerData.health;
    sLastMagic = gSaveContext.save.info.playerData.magic;

    keys = (gSaveContext.mapIndex < 19)
               ? gSaveContext.save.info.inventory.dungeonKeys[gSaveContext.mapIndex]
               : 0;
    items = 0;
    for (i = 0; i < 4; i++) {
        items = (items * 31) + gSaveContext.save.info.equips.buttonItems[i];
    }
    for (i = 0; i < 16; i++) {
        items = (items * 31) + (u8)gSaveContext.save.info.inventory.ammo[i];
    }
    if ((sLastRupees >= 0) && ((sLastRupees != gSaveContext.save.info.playerData.rupees) || (sLastKeys != keys) ||
                               (sLastItems != items))) {
        bits |= HUD_ACT_RUPEES_ITEMS;
    }
    sLastRupees = gSaveContext.save.info.playerData.rupees;
    sLastKeys = keys;
    sLastItems = items;
    return bits;
}

// Toward gray by the part's color, in place: every primitive and environment color the part set.
// An item's icon keeps its own colors, which are in its texture rather than in these.
static void Hud_Colors(Gfx* from, Gfx* to, u32 value) {
    u32 color = (value >> 16) & 0x1FF;
    Gfx* g;

    if (color >= 256) {
        return;
    }
    for (g = from; g < to; g += Hud_Words(g)) {
        u32 op = g->words.w0 >> 24;
        if ((op == G_SETPRIMCOLOR) || (op == G_SETENVCOLOR)) {
            u32 w1 = g->words.w1;
            s32 r = (w1 >> 24) & 0xFF;
            s32 gr = (w1 >> 16) & 0xFF;
            s32 bl = (w1 >> 8) & 0xFF;
            s32 luma = (r * 77 + gr * 150 + bl * 29) >> 8;
            r = luma + (((r - luma) * (s32)color) >> 8);
            gr = luma + (((gr - luma) * (s32)color) >> 8);
            bl = luma + (((bl - luma) * (s32)color) >> 8);
            g->words.w1 = ((u32)r << 24) | ((u32)gr << 16) | ((u32)bl << 8) | (w1 & 0xFF);
        }
    }
}

// A part's box, in the console's quarter pixels: round its rectangles and round any viewport it
// draws in that is smaller than the frame (the A button's and its label's), so a part drawn with
// vertices is measured by where it really is. Empty when minX is above maxX.
typedef struct {
    s32 minX;
    s32 minY;
    s32 maxX;
    s32 maxY;
} HudBox;

static void Hud_Box(Gfx* from, Gfx* to, HudBox* box) {
    Gfx* g;

    box->minX = 0xFFF;
    box->minY = 0xFFF;
    box->maxX = 0;
    box->maxY = 0;
    for (g = from; g < to; g += Hud_Words(g)) {
        u32 op = g->words.w0 >> 24;
        if ((op == G_TEXRECT) || (op == G_TEXRECTFLIP) || (op == G_FILLRECT)) {
            box->minX = MIN(box->minX, (s32)((g->words.w1 >> 12) & 0xFFF));
            box->minY = MIN(box->minY, (s32)(g->words.w1 & 0xFFF));
            box->maxX = MAX(box->maxX, (s32)((g->words.w0 >> 12) & 0xFFF));
            box->maxY = MAX(box->maxY, (s32)(g->words.w0 & 0xFFF));
        } else if ((op == G_MOVEMEM) && ((g->words.w0 & 0xFF) == G_MV_VIEWPORT)) {
            Vp* vp = (Vp*)g->words.w1;
            s32 halfW = ABS(vp->vp.vscale[0]);
            s32 halfH = ABS(vp->vp.vscale[1]);

            if ((halfW * 2 < HUD_FRAME_W) || (halfH * 2 < HUD_FRAME_H)) {
                box->minX = MIN(box->minX, vp->vp.vtrans[0] - halfW);
                box->minY = MIN(box->minY, vp->vp.vtrans[1] - halfH);
                box->maxX = MAX(box->maxX, vp->vp.vtrans[0] + halfW);
                box->maxY = MAX(box->maxY, vp->vp.vtrans[1] + halfH);
            }
        }
    }
}

// Where a part's commands stop being its own: the first rectangle alignment in its body. Only the
// buttons have one, the icon of an item being equipped in the pause menu, which flies to its button
// by an origin of its own (hud_align_equip) and is not moved or resized with the buttons.
static Gfx* Hud_Stop(Gfx* body, Gfx* to) {
    Gfx* g;

    for (g = body; g < to; g += Hud_Words(g)) {
        if (((g->words.w0 >> 24) == RT64_EXTENDED_OPCODE) && ((g->words.w0 & 0xFFFFFF) == G_EX_SETRECTALIGN_V1)) {
            return g;
        }
    }
    return to;
}

static s32 Hud_Clamp(s32 v) {
    return (v < 0) ? 0 : ((v > 0xFFF) ? 0xFFF : v);
}

// ON WHOLE PIXELS (the user's frame of 2026-10-09, after the third fix of the magic meter: the fill
// one console pixel low over the frame's bottom border, its last row dark). The console places
// every piece of its HUD on whole pixels. A rectangle whose top is part way into a pixel has its
// texture moved down by a row (RDP::drawRect, vFractionOffset) and its edges rounded by its own
// coverage, so two pieces sized or moved by quarter pixels stopped lining up. Every edge the size
// and the places compute is put on the nearest whole pixel, in quarter pixels.
static s32 Hud_Whole(s32 v) {
    return (v >= 0) ? (((v + 2) >> 2) << 2) : -((((-v) + 2) >> 2) << 2);
}

static s32 Hud_Coord(s32 v, s32 anchor, s32 scale) {
    return Hud_Clamp(Hud_Whole(anchor + (((v - anchor) * scale) / 100)));
}

// A scissor the frame's own size: the one the rest of the interface is drawn under.
static s32 Hud_FullScissor(Gfx* g) {
    return (((g->words.w0 >> 12) & 0xFFF) == 0) && ((g->words.w0 & 0xFFF) == 0) &&
           (((g->words.w1 >> 12) & 0xFFF) >= HUD_FRAME_W) && ((g->words.w1 & 0xFFF) >= HUD_FRAME_H);
}

// THE HUD'S SIZE (2026-10-09, the user: "control hud sizeing per item. like scale hearts and scale c
// button both down and make map independently larger", and "the ability to scale all at once or
// independent"). After the draw, each part's own commands are rewritten in place: its rectangles,
// textured or filled, scaled about the corner of its box it sits in (the hearts and the magic
// meter from the top left, the counters from the bottom left, the buttons from the top right, the
// map from the bottom right), their texture steps scaled to match so a texture still fills its
// rectangle, and its viewports scaled the same way, so what it draws with vertices (the beating
// heart, the A button, the map's arrows) stays with the rest. A part grows inward from its corner,
// so a larger one stays on the screen. Coordinates here are the console's, in quarter pixels.
//
// THE SCISSOR FOLLOWS ITS VIEWPORT (the user, 2026-10-09, a frame of a larger Buttons size:
// "scaling cuts this button"). The A button and its label are drawn each in a small view of their
// own, and the game sets a scissor exactly that view's size (View_ApplyPerspectiveToOverlay); left
// as it was, it cut the scaled button at its old edge. The frame's own scissor is left alone: it is
// what the game draws the rest of the interface under, and a shrunken one would cut what comes
// after this part.
// A step that would carry a rectangle past the edge of its texture, held to the edge. Steps are
// signed 5.10 texels a pixel, sizes in quarter pixels, the start 10.5 texels.
static s32 Hud_KeepInside(s32 step, s32 size, s32 start, s32 texels) {
    s32 room = (texels * 32) - start;   // what is left of the texture past the start, 10.5

    if ((step <= 0) || (size <= 0) || (texels <= 0) || (room <= 0)) {
        return step;
    }
    // The span, size / 4 pixels * step / 1024, against the room, room / 32 texels.
    if ((size * step) > (room * 128)) {
        step = ((room * 128) - 1) / size;
    }
    return (step > 0) ? step : 1;
}

static void Hud_Resize(PlayState* play, Gfx* from, Gfx* to, s32 scale, s32 anchorX, s32 anchorY) {
    // Each tile's size in texels and whether it wraps (a mask), as the part last set them.
    s32 tileW[8] = { 0 };
    s32 tileH[8] = { 0 };
    s32 tileMaskS[8] = { 0 };
    s32 tileMaskT[8] = { 0 };
    Gfx* g;

    for (g = from; g < to; g += Hud_Words(g)) {
        u32 op = g->words.w0 >> 24;
        if (op == G_SETTILE) {
            s32 tile = (g->words.w1 >> 24) & 7;
            tileMaskS[tile] = (g->words.w1 >> 4) & 0xF;
            tileMaskT[tile] = (g->words.w1 >> 14) & 0xF;
        } else if (op == G_SETTILESIZE) {
            s32 tile = (g->words.w1 >> 24) & 7;
            tileW[tile] = ((((s32)(g->words.w1 >> 12) & 0xFFF) - ((s32)(g->words.w0 >> 12) & 0xFFF)) >> 2) + 1;
            tileH[tile] = ((((s32)g->words.w1 & 0xFFF) - ((s32)g->words.w0 & 0xFFF)) >> 2) + 1;
        } else if ((op == G_TEXRECT) || (op == G_TEXRECTFLIP) || (op == G_FILLRECT)) {
            s32 oldW = (s32)((g->words.w0 >> 12) & 0xFFF) - (s32)((g->words.w1 >> 12) & 0xFFF);
            s32 oldH = (s32)(g->words.w0 & 0xFFF) - (s32)(g->words.w1 & 0xFFF);
            s32 xh = Hud_Coord((g->words.w0 >> 12) & 0xFFF, anchorX, scale);
            s32 yh = Hud_Coord(g->words.w0 & 0xFFF, anchorY, scale);
            s32 xl = Hud_Coord((g->words.w1 >> 12) & 0xFFF, anchorX, scale);
            s32 yl = Hud_Coord(g->words.w1 & 0xFFF, anchorY, scale);
            s32 tile = (g->words.w1 >> 24) & 7;
            g->words.w0 = (g->words.w0 & 0xFF000000) | ((u32)xh << 12) | (u32)yh;
            g->words.w1 = (g->words.w1 & 0xFF000000) | ((u32)xl << 12) | (u32)yl;
            // A textured rectangle's steps follow it, two commands on, so its texture still
            // spans it: dsdx and dtdy, signed 5.10. From the rectangle's own old and new sizes,
            // so it covers exactly the texels it did, whatever the rounding of its corners.
            if ((op != G_FILLRECT) && (g + 2 < to) && ((g[2].words.w0 >> 24) == G_RDPHALF_2)) {
                s32 dsdx = (s16)(g[2].words.w1 >> 16);
                s32 dtdy = (s16)(g[2].words.w1 & 0xFFFF);
                s32 newW = xh - xl;
                // A piece the game drew 1:1, now scaled: drawn the 1:1 way (Hud_Native).
                if ((op == G_TEXRECT) && (ABS(dsdx) == 1024) && (ABS(dtdy) == 1024) &&
                    ((g[1].words.w0 >> 24) == G_RDPHALF_1) && (sHudNatives < HUD_NATIVE_MAX)) {
                    sHudNative[sHudNatives++] = g;
                }
                s32 newH = yh - yl;
                // A flipped rectangle runs s down and t across.
                if (op == G_TEXRECTFLIP) {
                    s32 swap = newW;
                    newW = newH;
                    newH = swap;
                    swap = oldW;
                    oldW = oldH;
                    oldH = swap;
                }
                dsdx = (newW > 0) ? ((dsdx * oldW) / newW) : ((dsdx * 100) / scale);
                dtdy = (newH > 0) ? ((dtdy * oldH) / newH) : ((dtdy * 100) / scale);
                // KEPT INSIDE ITS TEXTURE (the user, 2026-10-09, of the magic meter at 70%: "the
                // green bar in the magic bar is too tall ... extends past the bottom of the what
                // bounding box", with frames at 100, 90 and 70 percent). The meter's middle and
                // its fill are drawn wider than their textures (24 and 16 texels) on tiles with no
                // mask, so the console clamps and repeats the last column; the renderer draws a
                // 1:1 rectangle that way, but a scaled one through its upscaling path, which read
                // past the texture's edge into the rows that follow: the middle squeezed, its
                // bottom border lost under the fill, the fill's own rows and the bytes after it
                // stacked. Held to the texture's edge instead, which for these textures, the same
                // in every column, is the console's look exactly. Only a tile with no mask: one
                // with a mask wraps or mirrors on purpose (the meter's right end mirrors its left).
                if ((op == G_TEXRECT) && ((g[1].words.w0 >> 24) == G_RDPHALF_1)) {
                    s32 s0 = (s16)(g[1].words.w1 >> 16);
                    s32 t0 = (s16)(g[1].words.w1 & 0xFFFF);
                    if (tileMaskS[tile] == 0) {
                        dsdx = Hud_KeepInside(dsdx, xh - xl, s0, tileW[tile]);
                    }
                    if (tileMaskT[tile] == 0) {
                        dtdy = Hud_KeepInside(dtdy, yh - yl, t0, tileH[tile]);
                    }
                }
                g[2].words.w1 = ((u32)(dsdx & 0xFFFF) << 16) | (u32)(dtdy & 0xFFFF);
            }
        } else if ((op == G_MOVEMEM) && ((g->words.w0 & 0xFF) == G_MV_VIEWPORT)) {
            // A copy, so a viewport the game keeps for itself is never changed.
            Vp* old = (Vp*)g->words.w1;
            Vp* vp = GRAPH_ALLOC(play->state.gfxCtx, sizeof(Vp));
            s32 i;

            *vp = *old;
            for (i = 0; i < 2; i++) {
                s32 anchor = (i == 0) ? anchorX : anchorY;
                vp->vp.vscale[i] = (s16)((vp->vp.vscale[i] * scale) / 100);
                vp->vp.vtrans[i] = (s16)(anchor + (((vp->vp.vtrans[i] - anchor) * scale) / 100));
            }
            g->words.w1 = (u32)vp;
        } else if ((op == G_SETSCISSOR) && !Hud_FullScissor(g)) {
            s32 ulx = Hud_Coord((g->words.w0 >> 12) & 0xFFF, anchorX, scale);
            s32 uly = Hud_Coord(g->words.w0 & 0xFFF, anchorY, scale);
            s32 lrx = Hud_Coord((g->words.w1 >> 12) & 0xFFF, anchorX, scale);
            s32 lry = Hud_Coord(g->words.w1 & 0xFFF, anchorY, scale);
            g->words.w0 = (g->words.w0 & 0xFF000000) | ((u32)ulx << 12) | (u32)uly;
            g->words.w1 = (g->words.w1 & 0xFF000000) | ((u32)lrx << 12) | (u32)lry;
        }
    }
}

// A part moved, whole: its rectangles, its viewports (copies, as above) and its small scissors.
// When it is PLACED its origin is no longer the one the interface's scissor alignment assumes
// (the scissor's left edge measured from the frame's left, its right from the frame's right), so
// there a small scissor opens to the frame's own rather than following; a scissor that cut the A
// button to its own square is not needed to draw it.
static void Hud_Move(PlayState* play, Gfx* from, Gfx* to, s32 dx, s32 dy, s32 placed) {
    Gfx* g;

    for (g = from; g < to; g += Hud_Words(g)) {
        u32 op = g->words.w0 >> 24;
        if ((op == G_TEXRECT) || (op == G_TEXRECTFLIP) || (op == G_FILLRECT)) {
            s32 xh = Hud_Clamp((s32)((g->words.w0 >> 12) & 0xFFF) + dx);
            s32 yh = Hud_Clamp((s32)(g->words.w0 & 0xFFF) + dy);
            s32 xl = Hud_Clamp((s32)((g->words.w1 >> 12) & 0xFFF) + dx);
            s32 yl = Hud_Clamp((s32)(g->words.w1 & 0xFFF) + dy);
            g->words.w0 = (g->words.w0 & 0xFF000000) | ((u32)xh << 12) | (u32)yh;
            g->words.w1 = (g->words.w1 & 0xFF000000) | ((u32)xl << 12) | (u32)yl;
        } else if ((op == G_MOVEMEM) && ((g->words.w0 & 0xFF) == G_MV_VIEWPORT)) {
            Vp* old = (Vp*)g->words.w1;
            Vp* vp = GRAPH_ALLOC(play->state.gfxCtx, sizeof(Vp));

            *vp = *old;
            vp->vp.vtrans[0] = (s16)(vp->vp.vtrans[0] + dx);
            vp->vp.vtrans[1] = (s16)(vp->vp.vtrans[1] + dy);
            g->words.w1 = (u32)vp;
        } else if ((op == G_SETSCISSOR) && !Hud_FullScissor(g)) {
            s32 ulx = placed ? 0 : Hud_Clamp((s32)((g->words.w0 >> 12) & 0xFFF) + dx);
            s32 uly = placed ? 0 : Hud_Clamp((s32)(g->words.w0 & 0xFFF) + dy);
            s32 lrx = placed ? HUD_FRAME_W : Hud_Clamp((s32)((g->words.w1 >> 12) & 0xFFF) + dx);
            s32 lry = placed ? HUD_FRAME_H : Hud_Clamp((s32)(g->words.w1 & 0xFFF) + dy);
            g->words.w0 = (g->words.w0 & 0xFF000000) | ((u32)ulx << 12) | (u32)uly;
            g->words.w1 = (g->words.w1 & 0xFF000000) | ((u32)lrx << 12) | (u32)lry;
        }
    }
}

// THE PASS AFTER THE DRAW: each part's colors, its size, the magic meter kept under the hearts,
// and, when the HUD is placed, each corner moved to its distances from the edges.
static void Hud_Arrange(PlayState* play, Gfx* end) {
    Gfx* stop[HUD_MARKS_MAX];
    HudBox box[HUD_MARKS_MAX];
    s32 heartsGrewBy = 0;
    s32 i;

    sHudNatives = 0;
    for (i = 0; i < sHudMarks; i++) {
        Gfx* next = (i + 1 < sHudMarks) ? sHudMarkAt[i + 1] : end;
        s32 part = sHudMarkPart[i];
        s32 group;
        s32 scale;

        stop[i] = next;
        box[i].minX = 0xFFF;
        box[i].maxX = 0;
        if (part < 0) {
            continue;
        }
        Hud_Colors(sHudMarkAt[i], next, sHudValue[part]);
        stop[i] = Hud_Stop(sHudMarkBody[i], next);
        Hud_Box(sHudMarkAt[i], stop[i], &box[i]);
        if (box[i].minX > box[i].maxX) {
            continue;   // nothing drawn this frame: nothing to size or place
        }

        group = Hud_Group(part);
        scale = (s32)(sHudLayout[part] & 0x3FF);
        if ((scale > 0) && (scale != 100)) {
            s32 anchorX = HUD_GROUP_RIGHT(group) ? box[i].maxX : box[i].minX;
            s32 anchorY = HUD_GROUP_BOTTOM(group) ? box[i].maxY : box[i].minY;
            s32 oldMaxY = box[i].maxY;

            Hud_Resize(play, sHudMarkAt[i], stop[i], scale, anchorX, anchorY);
            box[i].minX = Hud_Coord(box[i].minX, anchorX, scale);
            box[i].minY = Hud_Coord(box[i].minY, anchorY, scale);
            box[i].maxX = Hud_Coord(box[i].maxX, anchorX, scale);
            box[i].maxY = Hud_Coord(box[i].maxY, anchorY, scale);
            if (part == HUD_PART_HEARTS) {
                heartsGrewBy = Hud_Whole(box[i].maxY - oldMaxY);
            }
        }
        // THE MAGIC METER STAYS UNDER THE HEARTS: it moves down (or up) by as much as the hearts'
        // bottom did, so larger hearts never cover it. The hearts are marked before it.
        if ((part == HUD_PART_MAGIC) && (heartsGrewBy != 0)) {
            Hud_Move(play, sHudMarkAt[i], stop[i], 0, heartsGrewBy, false);
            box[i].minY += heartsGrewBy;
            box[i].maxY += heartsGrewBy;
        }
    }

    if (!(sHudPlace & HUD_PLACE_ON)) {
        return;
    }

    // THE PLACES. Each corner group as one box, put with its own edge on the origin Hud_Part set
    // across, and at its distance from the top or the bottom down, kept inside the frame.
    {
        s32 group;

        for (group = 0; group < HUD_GROUPS; group++) {
            HudBox all;
            s32 dx;
            s32 dy;
            s32 target;

            all.minX = 0xFFF;
            all.minY = 0xFFF;
            all.maxX = 0;
            all.maxY = 0;
            for (i = 0; i < sHudMarks; i++) {
                if ((sHudMarkPart[i] >= 0) && (Hud_Group(sHudMarkPart[i]) == group) && (box[i].minX <= box[i].maxX)) {
                    all.minX = MIN(all.minX, box[i].minX);
                    all.minY = MIN(all.minY, box[i].minY);
                    all.maxX = MAX(all.maxX, box[i].maxX);
                    all.maxY = MAX(all.maxY, box[i].maxY);
                }
            }
            if (all.minX > all.maxX) {
                continue;
            }
            sHudGroupWidth[group] = all.maxX - all.minX;

            dx = HUD_GROUP_RIGHT(group) ? (HUD_FRAME_W - all.maxX) : -all.minX;
            if (HUD_GROUP_BOTTOM(group)) {
                target = HUD_FRAME_H - (s32)(((sHudPlace >> 24) & 0x7F) * HUD_FRAME_H / 100);
                dy = target - all.maxY;
            } else {
                target = (s32)(((sHudPlace >> 16) & 0xFF) * HUD_FRAME_H / 100);
                dy = target - all.minY;
            }
            // Inside the frame, the top kept in view if the group is taller than the frame.
            if (all.maxY + dy > HUD_FRAME_H) {
                dy = HUD_FRAME_H - all.maxY;
            }
            if (all.minY + dy < 0) {
                dy = -all.minY;
            }
            // On whole pixels, like every other edge here (Hud_Whole).
            dx = Hud_Whole(dx);
            dy = Hud_Whole(dy);

            for (i = 0; i < sHudMarks; i++) {
                if ((sHudMarkPart[i] >= 0) && (Hud_Group(sHudMarkPart[i]) == group)) {
                    Hud_Move(play, sHudMarkAt[i], stop[i], dx, dy, true);
                }
            }
        }
    }
}

// THE SCALED 1:1 PIECES DRAWN AS THE CONSOLE DRAWS THEM (the user, 2026-10-09, of the magic meter
// at 70% after two fixes: "test is better but not fixed"). Last, after the sizes and the places
// have moved them: each recorded rectangle's three words (the rectangle and its two halves) are
// copied into a small list of its own between the renderer's console sampling and its release, and
// replaced where they stood by a call to that list and two no-ops, so the list keeps its length.
static void Hud_Native(PlayState* play) {
    s32 i;

    for (i = 0; i < sHudNatives; i++) {
        Gfx* g = sHudNative[i];
        Gfx* dl = GRAPH_ALLOC(play->state.gfxCtx, sizeof(Gfx) * 6);
        Gfx* p = dl;

        gEXForceNative2D(p++, 1);
        p[0] = g[0];
        p[1] = g[1];
        p[2] = g[2];
        p += 3;
        gEXForceNative2D(p++, 0);
        gSPEndDisplayList(p++);

        gSPDisplayList(&g[0], dl);
        gDPNoOp(&g[1]);
        gDPNoOp(&g[2]);
    }
}

static void Interface_DrawBody(PlayState* play);

// @recomp The HUD's fading around the game's own draw (2026-10-09): the parts' alphas scaled and
// put back, their colors toward gray after. With every part at its own values this is the draw
// as it was.
RECOMP_PATCH void Interface_Draw(PlayState* play) {
    InterfaceContext* interfaceCtx = &play->interfaceCtx;
    Gfx* end;
    s32 i;

    // @recomp Nothing while photo mode has the HUD hidden (2026-10-09, the user: "a photo mode control
    // for toggling the game UI HUD"). The game is frozen then, so nothing this would have moved on
    // (the timers it steps here) has anywhere to go.
    if (recomp_photo_hud_hidden()) {
        return;
    }

    recomp_hud_note(Hud_Activity(play));
    for (i = 0; i < 5; i++) {
        sHudValue[i] = recomp_hud_part(i);
        sHudLayout[i] = recomp_hud_layout(i);
    }
    sHudPlace = recomp_hud_place();
    sHudFrameQ16 = recomp_hud_frame_q16();
    sHudSaved.a = interfaceCtx->aAlpha;
    sHudSaved.b = interfaceCtx->bAlpha;
    sHudSaved.cLeft = interfaceCtx->cLeftAlpha;
    sHudSaved.cDown = interfaceCtx->cDownAlpha;
    sHudSaved.cRight = interfaceCtx->cRightAlpha;
    sHudSaved.health = interfaceCtx->healthAlpha;
    sHudSaved.magic = interfaceCtx->magicAlpha;
    sHudSaved.minimap = interfaceCtx->minimapAlpha;
    sHudSaved.start = interfaceCtx->startAlpha;
    sHudMarks = 0;

    Interface_DrawBody(play);

    interfaceCtx->aAlpha = sHudSaved.a;
    interfaceCtx->bAlpha = sHudSaved.b;
    interfaceCtx->cLeftAlpha = sHudSaved.cLeft;
    interfaceCtx->cDownAlpha = sHudSaved.cDown;
    interfaceCtx->cRightAlpha = sHudSaved.cRight;
    interfaceCtx->healthAlpha = sHudSaved.health;
    interfaceCtx->magicAlpha = sHudSaved.magic;
    interfaceCtx->minimapAlpha = sHudSaved.minimap;
    interfaceCtx->startAlpha = sHudSaved.start;

    end = play->state.gfxCtx->overlay.p;
    Hud_Arrange(play, end);
    Hud_Native(play);
}

// @recomp Patched to set each group's origin; see the top of the file. The game's Interface_Draw,
// called by the HUD's fading above, which marks where each part begins (Hud_Part).
static void Interface_DrawBody(PlayState* play) {
    static s16 magicArrowEffectsR[] = { 255, 100, 255 };
    static s16 magicArrowEffectsG[] = { 0, 100, 255 };
    static s16 magicArrowEffectsB[] = { 0, 255, 100 };
    static s16 timerDigitLeftPos[] = { 16, 25, 34, 42, 51 };
    static s16 sDigitWidths[] = { 9, 9, 8, 9, 9 };
    // unused, most likely colors
    static s16 D_80125B1C[][3] = {
        { 0, 150, 0 }, { 100, 255, 0 }, { 255, 255, 255 }, { 0, 0, 0 }, { 255, 255, 255 },
    };
    static s16 rupeeDigitsFirst[] = { 1, 0, 0 };
    static s16 rupeeDigitsCount[] = { 2, 3, 3 };
    static s16 spoilingItemEntrances[] = { ENTR_LOST_WOODS_2, ENTR_ZORAS_DOMAIN_3, ENTR_ZORAS_DOMAIN_3 };
    static f32 D_80125B54[] = { -40.0f, -35.0f }; // unused
    static s16 D_80125B5C[] = { 91, 91 };         // unused
    static s16 sTimerNextSecondTimer;
    static s16 sTimerStateTimer;
    static s16 sSubTimerNextSecondTimer;
    static s16 sSubTimerStateTimer;
    static s16 sTimerDigits[5];
    InterfaceContext* interfaceCtx = &play->interfaceCtx;
    PauseContext* pauseCtx = &play->pauseCtx;
    MessageContext* msgCtx = &play->msgCtx;
    Player* player = GET_PLAYER(play);
    s16 svar1;
    s16 svar2;
    s16 svar3;
    s16 svar4;
    s16 svar5;
    s16 timerId;

    OPEN_DISPS(play->state.gfxCtx, "../z_parameter.c", 3405);

    gSPSegment(OVERLAY_DISP++, 0x02, interfaceCtx->parameterSegment);
    gSPSegment(OVERLAY_DISP++, 0x07, interfaceCtx->doActionSegment);
    gSPSegment(OVERLAY_DISP++, 0x08, interfaceCtx->iconItemSegment);
    gSPSegment(OVERLAY_DISP++, 0x0B, interfaceCtx->mapSegment);

    if (pauseCtx->debugState == PAUSE_DEBUG_STATE_CLOSED) {
        Interface_InitVertices(play);
        // @recomp The interface's scissor spans the frame, so an element moved to an edge is
        // not clipped at the console's; then the hearts, the counters and the magic meter to
        // the left edge.
        gEXSetScissorAlign(OVERLAY_DISP++, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, -SCREEN_WIDTH, 0, 0, 0,
                           SCREEN_WIDTH, SCREEN_HEIGHT);
        hud_align(play->state.gfxCtx, G_EX_ORIGIN_LEFT);
        func_8008A994(interfaceCtx);
        Hud_Part(play, HUD_PART_HEARTS);   // @recomp the HUD's fading
        Health_DrawMeter(play);

        Gfx_SetupDL_39Overlay(play->state.gfxCtx);

        Hud_Part(play, HUD_PART_RUPEES);   // @recomp the HUD's fading

        // Rupee Icon
        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 200, 255, 100, interfaceCtx->magicAlpha);
        gDPSetEnvColor(OVERLAY_DISP++, 0, 80, 0, 255);
        OVERLAY_DISP = Gfx_TextureIA8(OVERLAY_DISP, gRupeeCounterIconTex, 16, 16, 26, 206, 16, 16, 1 << 10, 1 << 10);

        switch (play->sceneId) {
            case SCENE_FOREST_TEMPLE:
            case SCENE_FIRE_TEMPLE:
            case SCENE_WATER_TEMPLE:
            case SCENE_SPIRIT_TEMPLE:
            case SCENE_SHADOW_TEMPLE:
            case SCENE_BOTTOM_OF_THE_WELL:
            case SCENE_ICE_CAVERN:
            case SCENE_GANONS_TOWER:
            case SCENE_GERUDO_TRAINING_GROUND:
            case SCENE_THIEVES_HIDEOUT:
            case SCENE_INSIDE_GANONS_CASTLE:
            case SCENE_GANONS_TOWER_COLLAPSE_INTERIOR:
            case SCENE_INSIDE_GANONS_CASTLE_COLLAPSE:
            case SCENE_TREASURE_BOX_SHOP:
                if (gSaveContext.save.info.inventory.dungeonKeys[gSaveContext.mapIndex] >= 0) {
                    // Small Key Icon
                    gDPPipeSync(OVERLAY_DISP++);
                    gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 200, 230, 255, interfaceCtx->magicAlpha);
                    gDPSetEnvColor(OVERLAY_DISP++, 0, 0, 20, 255);
                    OVERLAY_DISP = Gfx_TextureIA8(OVERLAY_DISP, gSmallKeyCounterIconTex, 16, 16, 26, 190, 16, 16,
                                                  1 << 10, 1 << 10);

                    // Small Key Counter
                    gDPPipeSync(OVERLAY_DISP++);
                    gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, interfaceCtx->magicAlpha);
                    gDPSetCombineLERP(OVERLAY_DISP++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE,
                                      TEXEL0, 0, PRIMITIVE, 0);

                    interfaceCtx->counterDigits[2] = 0;
                    interfaceCtx->counterDigits[3] =
                        gSaveContext.save.info.inventory.dungeonKeys[gSaveContext.mapIndex];

                    while (interfaceCtx->counterDigits[3] >= 10) {
                        interfaceCtx->counterDigits[2]++;
                        interfaceCtx->counterDigits[3] -= 10;
                    }

                    svar3 = 42;

                    if (interfaceCtx->counterDigits[2] != 0) {
                        OVERLAY_DISP = Gfx_TextureI8(
                            OVERLAY_DISP, ((u8*)gCounterDigit0Tex + (8 * 16 * interfaceCtx->counterDigits[2])), 8, 16,
                            svar3, 190, 8, 16, 1 << 10, 1 << 10);
                        svar3 += 8;
                    }

                    OVERLAY_DISP = Gfx_TextureI8(OVERLAY_DISP,
                                                 ((u8*)gCounterDigit0Tex + (8 * 16 * interfaceCtx->counterDigits[3])),
                                                 8, 16, svar3, 190, 8, 16, 1 << 10, 1 << 10);
                }
                break;
            default:
                break;
        }

        // Rupee Counter
        gDPPipeSync(OVERLAY_DISP++);

        if (gSaveContext.save.info.playerData.rupees == CUR_CAPACITY(UPG_WALLET)) {
            gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 120, 255, 0, interfaceCtx->magicAlpha);
        } else if (gSaveContext.save.info.playerData.rupees != 0) {
            gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, interfaceCtx->magicAlpha);
        } else {
            gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 100, 100, 100, interfaceCtx->magicAlpha);
        }

        gDPSetCombineLERP(OVERLAY_DISP++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE, TEXEL0, 0,
                          PRIMITIVE, 0);

        interfaceCtx->counterDigits[0] = interfaceCtx->counterDigits[1] = 0;
        interfaceCtx->counterDigits[2] = gSaveContext.save.info.playerData.rupees;

        if ((interfaceCtx->counterDigits[2] > 9999) || (interfaceCtx->counterDigits[2] < 0)) {
            interfaceCtx->counterDigits[2] &= 0xDDD;
        }

        while (interfaceCtx->counterDigits[2] >= 100) {
            interfaceCtx->counterDigits[0]++;
            interfaceCtx->counterDigits[2] -= 100;
        }

        while (interfaceCtx->counterDigits[2] >= 10) {
            interfaceCtx->counterDigits[1]++;
            interfaceCtx->counterDigits[2] -= 10;
        }

        svar2 = rupeeDigitsFirst[CUR_UPG_VALUE(UPG_WALLET)];
        svar4 = rupeeDigitsCount[CUR_UPG_VALUE(UPG_WALLET)];

        for (svar1 = 0, svar3 = 42; svar1 < svar4; svar1++, svar2++, svar3 += 8) {
            OVERLAY_DISP =
                Gfx_TextureI8(OVERLAY_DISP, ((u8*)gCounterDigit0Tex + (8 * 16 * interfaceCtx->counterDigits[svar2])), 8,
                              16, svar3, 206, 8, 16, 1 << 10, 1 << 10);
        }

        Hud_Part(play, HUD_PART_MAGIC);   // @recomp the HUD's fading
        Magic_DrawMeter(play);
        // @recomp The minimap, its marks and the compass arrows to the right edge.
        hud_align(play->state.gfxCtx, G_EX_ORIGIN_RIGHT);
        func_8008A994(interfaceCtx);
        Hud_Part(play, HUD_PART_MAP);   // @recomp the HUD's fading
        Minimap_Draw(play);

        // @recomp The targeting reticle sits where the game projects its target, and the
        // centered console frame and the widened view agree on that: no origin.
        Hud_Part(play, HUD_PART_NONE);   // @recomp the HUD's fading: not a part
        hud_align(play->state.gfxCtx, G_EX_ORIGIN_NONE);
        func_8008A994(interfaceCtx);
        if ((R_PAUSE_BG_PRERENDER_STATE != PAUSE_BG_PRERENDER_PROCESS) &&
            (R_PAUSE_BG_PRERENDER_STATE != PAUSE_BG_PRERENDER_READY)) {
            Attention_Draw(&play->actorCtx.attention, play);
        }

        // @recomp The B, C and start buttons, their icons and counts, and the A button, whose
        // own perspective viewport takes the origin in force, to the right edge.
        hud_align(play->state.gfxCtx, G_EX_ORIGIN_RIGHT);
        func_8008A994(interfaceCtx);
        Gfx_SetupDL_39Overlay(play->state.gfxCtx);

        Hud_Part(play, HUD_PART_BUTTONS);   // @recomp the HUD's fading
        Interface_DrawItemButtons(play);

        gDPPipeSync(OVERLAY_DISP++);
        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, interfaceCtx->bAlpha);
        gDPSetCombineMode(OVERLAY_DISP++, G_CC_MODULATERGBA_PRIM, G_CC_MODULATERGBA_PRIM);

        if (!(interfaceCtx->unk_1FA)) {
            // B Button Icon & Ammo Count
            if (gSaveContext.save.info.equips.buttonItems[0] != ITEM_NONE) {
                Interface_DrawItemIconTexture(play, interfaceCtx->iconItemSegment, 0);

                if ((player->stateFlags1 & PLAYER_STATE1_23) || (play->shootingGalleryStatus > 1) ||
                    ((play->sceneId == SCENE_BOMBCHU_BOWLING_ALLEY) && Flags_GetSwitch(play, 0x38))) {
                    gDPPipeSync(OVERLAY_DISP++);
                    gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE,
                                      0, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
                    Interface_DrawAmmoCount(play, 0, interfaceCtx->bAlpha);
                }
            }
        } else {
            // B Button Do Action Label
            gDPPipeSync(OVERLAY_DISP++);
            gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                              PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
            gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, interfaceCtx->bAlpha);

            gDPLoadTextureBlock_4b(OVERLAY_DISP++, interfaceCtx->doActionSegment + DO_ACTION_TEX_SIZE, G_IM_FMT_IA,
                                   DO_ACTION_TEX_WIDTH, DO_ACTION_TEX_HEIGHT, 0, G_TX_NOMIRROR | G_TX_WRAP,
                                   G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

            R_B_LABEL_DD = (1 << 10) / (R_B_LABEL_SCALE(gSaveContext.language) / 100.0f);
            gSPTextureRectangle(OVERLAY_DISP++, R_B_LABEL_X(gSaveContext.language) << 2,
                                R_B_LABEL_Y(gSaveContext.language) << 2,
                                (R_B_LABEL_X(gSaveContext.language) + DO_ACTION_TEX_WIDTH) << 2,
                                (R_B_LABEL_Y(gSaveContext.language) + DO_ACTION_TEX_HEIGHT) << 2, G_TX_RENDERTILE, 0, 0,
                                R_B_LABEL_DD, R_B_LABEL_DD);
        }

        gDPPipeSync(OVERLAY_DISP++);

        // C-Left Button Icon & Ammo Count
        if (gSaveContext.save.info.equips.buttonItems[1] < 0xF0) {
            gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, interfaceCtx->cLeftAlpha);
            gDPSetCombineMode(OVERLAY_DISP++, G_CC_MODULATERGBA_PRIM, G_CC_MODULATERGBA_PRIM);
            Interface_DrawItemIconTexture(play, interfaceCtx->iconItemSegment + 0x1000, 1);
            gDPPipeSync(OVERLAY_DISP++);
            gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                              PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
            Interface_DrawAmmoCount(play, 1, interfaceCtx->cLeftAlpha);
        }

        gDPPipeSync(OVERLAY_DISP++);

        // C-Down Button Icon & Ammo Count
        if (gSaveContext.save.info.equips.buttonItems[2] < 0xF0) {
            gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, interfaceCtx->cDownAlpha);
            gDPSetCombineMode(OVERLAY_DISP++, G_CC_MODULATERGBA_PRIM, G_CC_MODULATERGBA_PRIM);
            Interface_DrawItemIconTexture(play, interfaceCtx->iconItemSegment + 0x2000, 2);
            gDPPipeSync(OVERLAY_DISP++);
            gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                              PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
            Interface_DrawAmmoCount(play, 2, interfaceCtx->cDownAlpha);
        }

        gDPPipeSync(OVERLAY_DISP++);

        // C-Right Button Icon & Ammo Count
        if (gSaveContext.save.info.equips.buttonItems[3] < 0xF0) {
            gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, interfaceCtx->cRightAlpha);
            gDPSetCombineMode(OVERLAY_DISP++, G_CC_MODULATERGBA_PRIM, G_CC_MODULATERGBA_PRIM);
            Interface_DrawItemIconTexture(play, interfaceCtx->iconItemSegment + 0x3000, 3);
            gDPPipeSync(OVERLAY_DISP++);
            gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                              PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
            Interface_DrawAmmoCount(play, 3, interfaceCtx->cRightAlpha);
        }

        // A Button
        Gfx_SetupDL_42Overlay(play->state.gfxCtx);
        func_8008A8B8(play, R_A_BTN_Y, R_A_BTN_Y + 45, R_A_BTN_X, R_A_BTN_X + 45);
        gSPClearGeometryMode(OVERLAY_DISP++, G_CULL_BOTH);
        gDPSetCombineMode(OVERLAY_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, R_A_BTN_COLOR(0), R_A_BTN_COLOR(1), R_A_BTN_COLOR(2),
                        interfaceCtx->aAlpha);
        Interface_DrawActionButton(play);
        gDPPipeSync(OVERLAY_DISP++);
        func_8008A8B8(play, R_A_ICON_Y, R_A_ICON_Y + 45, R_A_ICON_X, R_A_ICON_X + 45);
        gSPSetGeometryMode(OVERLAY_DISP++, G_CULL_BACK);
        gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                          PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, interfaceCtx->aAlpha);
        gDPSetEnvColor(OVERLAY_DISP++, 0, 0, 0, 0);
        Matrix_Translate(0.0f, 0.0f, R_A_LABEL_Z(gSaveContext.language) / 10.0f, MTXMODE_NEW);
        Matrix_Scale(1.0f, 1.0f, 1.0f, MTXMODE_APPLY);
        Matrix_RotateX(interfaceCtx->unk_1F4 / 10000.0f, MTXMODE_APPLY);
        MATRIX_FINALIZE_AND_LOAD(OVERLAY_DISP++, play->state.gfxCtx, "../z_parameter.c", 3701);
        gSPVertex(OVERLAY_DISP++, &interfaceCtx->actionVtx[4], 4, 0);

        if ((interfaceCtx->unk_1EC < 2) || (interfaceCtx->unk_1EC == 3)) {
            Interface_DrawActionLabel(play->state.gfxCtx, interfaceCtx->doActionSegment);
        } else {
            Interface_DrawActionLabel(play->state.gfxCtx, interfaceCtx->doActionSegment + DO_ACTION_TEX_SIZE);
        }

        gDPPipeSync(OVERLAY_DISP++);

        // @recomp The icon of an item being equipped: its origin travels with it from the
        // center to the right edge (see hud_align_equip).
        hud_align_equip(play);
        func_8008A994(interfaceCtx);

        if ((pauseCtx->state == PAUSE_STATE_MAIN) && (pauseCtx->mainState == PAUSE_MAIN_STATE_3)) {
            // Inventory Equip Effects
            gSPSegment(OVERLAY_DISP++, 0x08, pauseCtx->iconItemSegment);
            Gfx_SetupDL_42Overlay(play->state.gfxCtx);
            gDPSetCombineMode(OVERLAY_DISP++, G_CC_MODULATERGBA_PRIM, G_CC_MODULATERGBA_PRIM);
            gSPMatrix(OVERLAY_DISP++, &gIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

            // PAUSE_CURSOR_QUAD_4
            pauseCtx->cursorVtx[16].v.ob[0] = pauseCtx->cursorVtx[18].v.ob[0] = pauseCtx->equipAnimX / 10;
            pauseCtx->cursorVtx[17].v.ob[0] = pauseCtx->cursorVtx[19].v.ob[0] =
                pauseCtx->cursorVtx[16].v.ob[0] + WREG(90) / 10;
            pauseCtx->cursorVtx[16].v.ob[1] = pauseCtx->cursorVtx[17].v.ob[1] = pauseCtx->equipAnimY / 10;
            pauseCtx->cursorVtx[18].v.ob[1] = pauseCtx->cursorVtx[19].v.ob[1] =
                pauseCtx->cursorVtx[16].v.ob[1] - WREG(90) / 10;

            if (pauseCtx->equipTargetItem < 0xBF) {
                // Normal Equip (icon goes from the inventory slot to the C button when equipping it)
                gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, pauseCtx->equipAnimAlpha);
                gSPVertex(OVERLAY_DISP++, &pauseCtx->cursorVtx[PAUSE_CURSOR_QUAD_4 * 4], 4, 0);

                gDPLoadTextureBlock(OVERLAY_DISP++, gItemIcons[pauseCtx->equipTargetItem], G_IM_FMT_RGBA, G_IM_SIZ_32b,
                                    ITEM_ICON_WIDTH, ITEM_ICON_HEIGHT, 0, G_TX_NOMIRROR | G_TX_WRAP,
                                    G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            } else {
                // Magic Arrow Equip Effect
                svar1 = pauseCtx->equipTargetItem - 0xBF;
                gDPSetPrimColor(OVERLAY_DISP++, 0, 0, magicArrowEffectsR[svar1], magicArrowEffectsG[svar1],
                                magicArrowEffectsB[svar1], pauseCtx->equipAnimAlpha);

                if ((pauseCtx->equipAnimAlpha > 0) && (pauseCtx->equipAnimAlpha < 255)) {
                    svar1 = (pauseCtx->equipAnimAlpha / 8) / 2;
                    // PAUSE_CURSOR_QUAD_4
                    pauseCtx->cursorVtx[16].v.ob[0] = pauseCtx->cursorVtx[18].v.ob[0] =
                        pauseCtx->cursorVtx[16].v.ob[0] - svar1;
                    pauseCtx->cursorVtx[17].v.ob[0] = pauseCtx->cursorVtx[19].v.ob[0] =
                        pauseCtx->cursorVtx[16].v.ob[0] + 32 + svar1 * 2;
                    pauseCtx->cursorVtx[16].v.ob[1] = pauseCtx->cursorVtx[17].v.ob[1] =
                        pauseCtx->cursorVtx[16].v.ob[1] + svar1;
                    pauseCtx->cursorVtx[18].v.ob[1] = pauseCtx->cursorVtx[19].v.ob[1] =
                        pauseCtx->cursorVtx[16].v.ob[1] - 32 - svar1 * 2;
                }

                gSPVertex(OVERLAY_DISP++, &pauseCtx->cursorVtx[PAUSE_CURSOR_QUAD_4 * 4], 4, 0);
                gDPLoadTextureBlock(OVERLAY_DISP++, gMagicArrowEquipEffectTex, G_IM_FMT_IA, G_IM_SIZ_8b,
                                    gMagicArrowEquipEffectTex_WIDTH, gMagicArrowEquipEffectTex_HEIGHT, 0,
                                    G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK,
                                    G_TX_NOLOD, G_TX_NOLOD);
            }

            gSP1Quadrangle(OVERLAY_DISP++, 0, 2, 3, 1, 0);
        }

        // @recomp Everything from here (the carrots, the timers, the letters) is centered.
        Hud_Part(play, HUD_PART_NONE);   // @recomp the HUD's fading: the buttons end here
        hud_align(play->state.gfxCtx, G_EX_ORIGIN_NONE);
        func_8008A994(interfaceCtx);
        Gfx_SetupDL_39Overlay(play->state.gfxCtx);

        if (!IS_PAUSED(&play->pauseCtx)) {
            if (gSaveContext.minigameState != 1) {
                // Carrots rendering if the action corresponds to riding a horse
                if (interfaceCtx->unk_1EE == 8) {
                    // Load Carrot Icon
                    gDPLoadTextureBlock(OVERLAY_DISP++, gCarrotIconTex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 16, 16, 0,
                                        G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK,
                                        G_TX_NOLOD, G_TX_NOLOD);

                    // Draw 6 carrots
                    for (svar1 = 1, svar5 = ZREG(14); svar1 < 7; svar1++, svar5 += 16) {
                        // Carrot Color (based on availability)
                        if ((interfaceCtx->numHorseBoosts == 0) || (interfaceCtx->numHorseBoosts < svar1)) {
                            gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 0, 150, 255, interfaceCtx->aAlpha);
                        } else {
                            gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, interfaceCtx->aAlpha);
                        }

                        gSPTextureRectangle(OVERLAY_DISP++, svar5 << 2, ZREG(15) << 2, (svar5 + 16) << 2,
                                            (ZREG(15) + 16) << 2, G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);
                    }
                }
            } else {
                // Score for the Horseback Archery
                svar5 = WREG(32);
                gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, interfaceCtx->bAlpha);

                // Target Icon
                gDPLoadTextureBlock(OVERLAY_DISP++, gArcheryScoreIconTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 24, 16, 0,
                                    G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK,
                                    G_TX_NOLOD, G_TX_NOLOD);

                gSPTextureRectangle(OVERLAY_DISP++, (svar5 + 28) << 2, ZREG(15) << 2, (svar5 + 52) << 2,
                                    (ZREG(15) + 16) << 2, G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);

                // Score Counter
                gDPPipeSync(OVERLAY_DISP++);
                gDPSetCombineLERP(OVERLAY_DISP++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE,
                                  TEXEL0, 0, PRIMITIVE, 0);

                svar5 = WREG(32) + 6 * 9;

                for (svar1 = svar2 = 0; svar1 < 4; svar1++) {
                    if (sHBAScoreDigits[svar1] != 0 || (svar2 != 0) || (svar1 >= 3)) {
                        OVERLAY_DISP = Gfx_TextureI8(
                            OVERLAY_DISP, ((u8*)gCounterDigit0Tex + (8 * 16 * sHBAScoreDigits[svar1])), 8, 16, svar5,
                            (ZREG(15) - 2), sDigitWidths[0], VREG(42), VREG(43) << 1, VREG(43) << 1);
                        svar5 += 9;
                        svar2++;
                    }
                }

                gDPPipeSync(OVERLAY_DISP++);
                gDPSetCombineMode(OVERLAY_DISP++, G_CC_MODULATERGBA_PRIM, G_CC_MODULATERGBA_PRIM);
            }
        }

        if ((gSaveContext.subTimerState == SUBTIMER_STATE_RESPAWN) &&
            (Message_GetState(&play->msgCtx) == TEXT_STATE_EVENT)) {
            // Trade quest timer reached 0
            sSubTimerStateTimer = 40;
            gSaveContext.save.cutsceneIndex = CS_INDEX_NONE;
            play->transitionTrigger = TRANS_TRIGGER_START;
            play->transitionType = TRANS_TYPE_FADE_WHITE;
            gSaveContext.subTimerState = SUBTIMER_STATE_OFF;

            if ((gSaveContext.save.info.equips.buttonItems[0] != ITEM_SWORD_KOKIRI) &&
                (gSaveContext.save.info.equips.buttonItems[0] != ITEM_SWORD_MASTER) &&
                (gSaveContext.save.info.equips.buttonItems[0] != ITEM_SWORD_BIGGORON) &&
                (gSaveContext.save.info.equips.buttonItems[0] != ITEM_GIANTS_KNIFE)) {
                if (gSaveContext.buttonStatus[0] != BTN_ENABLED) {
                    gSaveContext.save.info.equips.buttonItems[0] = gSaveContext.buttonStatus[0];
                } else {
                    gSaveContext.save.info.equips.buttonItems[0] = ITEM_NONE;
                }
            }

            // Revert any spoiling trade quest items
            for (svar1 = 0; svar1 < ARRAY_COUNT(gSpoilingItems); svar1++) {
                if (INV_CONTENT(ITEM_TRADE_ADULT) == gSpoilingItems[svar1]) {
#if OOT_VERSION >= NTSC_1_1
                    gSaveContext.eventInf[EVENTINF_INDEX_HORSES] &=
                        (u16) ~(EVENTINF_INGO_RACE_STATE_MASK | EVENTINF_MASK(EVENTINF_INGO_RACE_HORSETYPE) |
                                EVENTINF_MASK(EVENTINF_INGO_RACE_LOST_ONCE) |
                                EVENTINF_MASK(EVENTINF_INGO_RACE_SECOND_RACE) | EVENTINF_MASK(EVENTINF_INGO_RACE_0F));
                    PRINTF("EVENT_INF=%x\n", gSaveContext.eventInf[EVENTINF_INDEX_HORSES]);
#endif
                    play->nextEntranceIndex = spoilingItemEntrances[svar1];
                    INV_CONTENT(gSpoilingItemReverts[svar1]) = gSpoilingItemReverts[svar1];

                    for (svar2 = 1; svar2 < 4; svar2++) {
                        if (gSaveContext.save.info.equips.buttonItems[svar2] == gSpoilingItems[svar1]) {
                            gSaveContext.save.info.equips.buttonItems[svar2] = gSpoilingItemReverts[svar1];
                            Interface_LoadItemIcon1(play, svar2);
                        }
                    }
                }
            }
        }

        if (!IS_PAUSED(&play->pauseCtx) && (play->gameOverCtx.state == GAMEOVER_INACTIVE) &&
            (msgCtx->msgMode == MSGMODE_NONE) && !(player->stateFlags2 & PLAYER_STATE2_24) &&
            (play->transitionTrigger == TRANS_TRIGGER_OFF) && (play->transitionMode == TRANS_MODE_OFF) &&
            !Play_InCsMode(play) && (gSaveContext.minigameState != 1) && (play->shootingGalleryStatus <= 1) &&
            !((play->sceneId == SCENE_BOMBCHU_BOWLING_ALLEY) && Flags_GetSwitch(play, 0x38))) {

            timerId = TIMER_ID_MAIN;

            switch (gSaveContext.timerState) {
                case TIMER_STATE_ENV_HAZARD_INIT:
                    sTimerStateTimer = 20;
                    sTimerNextSecondTimer = 20;
                    gSaveContext.timerSeconds = gSaveContext.save.info.playerData.health >> 1;
                    gSaveContext.timerState = TIMER_STATE_ENV_HAZARD_PREVIEW;
                    break;

                case TIMER_STATE_ENV_HAZARD_PREVIEW:
                    sTimerStateTimer--;
                    if (sTimerStateTimer == 0) {
                        sTimerStateTimer = 20;
                        gSaveContext.timerState = TIMER_STATE_ENV_HAZARD_MOVE;
                    }
                    break;

                case TIMER_STATE_DOWN_INIT:
                case TIMER_STATE_UP_INIT:
                    sTimerStateTimer = 20;
                    sTimerNextSecondTimer = 20;
                    if (gSaveContext.timerState == TIMER_STATE_DOWN_INIT) {
                        gSaveContext.timerState = TIMER_STATE_DOWN_PREVIEW;
                    } else {
                        gSaveContext.timerState = TIMER_STATE_UP_PREVIEW;
                    }
                    break;

                case TIMER_STATE_DOWN_PREVIEW:
                case TIMER_STATE_UP_PREVIEW:
                    sTimerStateTimer--;
                    if (sTimerStateTimer == 0) {
                        sTimerStateTimer = 20;
                        if (gSaveContext.timerState == TIMER_STATE_DOWN_PREVIEW) {
                            gSaveContext.timerState = TIMER_STATE_DOWN_MOVE;
                        } else {
                            gSaveContext.timerState = TIMER_STATE_UP_MOVE;
                        }
                    }
                    break;

                case TIMER_STATE_ENV_HAZARD_MOVE:
                case TIMER_STATE_DOWN_MOVE:
                    svar1 = (gSaveContext.timerX[TIMER_ID_MAIN] - 26) / sTimerStateTimer;
                    gSaveContext.timerX[TIMER_ID_MAIN] -= svar1;

                    if (gSaveContext.save.info.playerData.healthCapacity > 0xA0) {
                        svar1 = (gSaveContext.timerY[TIMER_ID_MAIN] - 54) / sTimerStateTimer; // two rows of hearts
                    } else {
                        svar1 = (gSaveContext.timerY[TIMER_ID_MAIN] - 46) / sTimerStateTimer; // one row of hearts
                    }
                    gSaveContext.timerY[TIMER_ID_MAIN] -= svar1;

                    sTimerStateTimer--;
                    if (sTimerStateTimer == 0) {
                        sTimerStateTimer = 20;
                        gSaveContext.timerX[TIMER_ID_MAIN] = 26;

                        if (gSaveContext.save.info.playerData.healthCapacity > 0xA0) {
                            gSaveContext.timerY[TIMER_ID_MAIN] = 54; // two rows of hearts
                        } else {
                            gSaveContext.timerY[TIMER_ID_MAIN] = 46; // one row of hearts
                        }

                        if (gSaveContext.timerState == TIMER_STATE_ENV_HAZARD_MOVE) {
                            gSaveContext.timerState = TIMER_STATE_ENV_HAZARD_TICK;
                        } else {
                            gSaveContext.timerState = TIMER_STATE_DOWN_TICK;
                        }
                    }
                    FALLTHROUGH;
                case TIMER_STATE_ENV_HAZARD_TICK:
                case TIMER_STATE_DOWN_TICK:
                    if ((gSaveContext.timerState == TIMER_STATE_ENV_HAZARD_TICK) ||
                        (gSaveContext.timerState == TIMER_STATE_DOWN_TICK)) {
                        if (gSaveContext.save.info.playerData.healthCapacity > 0xA0) {
                            gSaveContext.timerY[TIMER_ID_MAIN] = 54; // two rows of hearts
                        } else {
                            gSaveContext.timerY[TIMER_ID_MAIN] = 46; // one row of hearts
                        }
                    }

                    if ((gSaveContext.timerState >= TIMER_STATE_ENV_HAZARD_MOVE) && (msgCtx->msgLength == 0)) {
                        if (--sTimerNextSecondTimer == 0) {
                            if (gSaveContext.timerSeconds != 0) {
                                gSaveContext.timerSeconds--;
                            }

                            sTimerNextSecondTimer = 20;

                            if (gSaveContext.timerSeconds == 0) {
                                // Out of time
                                gSaveContext.timerState = TIMER_STATE_STOP;
                                if (sEnvHazardActive) {
                                    gSaveContext.save.info.playerData.health = 0;
                                    play->damagePlayer(play, -(gSaveContext.save.info.playerData.health + 2));
                                }
                                sEnvHazardActive = false;
                            } else if (gSaveContext.timerSeconds > 60) {
                                // Beep at "xx:x1" (every 10 seconds)
                                if (sTimerDigits[4] == 1) {
                                    SFX_PLAY_CENTERED(NA_SE_SY_MESSAGE_WOMAN);
                                }
                            } else if (gSaveContext.timerSeconds > 10) {
                                // Beep on alternating seconds
                                if ((sTimerDigits[4] % 2) != 0) {
                                    SFX_PLAY_CENTERED(NA_SE_SY_WARNING_COUNT_N);
                                }
                            } else {
                                // Beep every second
                                SFX_PLAY_CENTERED(NA_SE_SY_WARNING_COUNT_E);
                            }
                        }
                    }
                    break;

                case TIMER_STATE_UP_MOVE:
                    svar1 = (gSaveContext.timerX[TIMER_ID_MAIN] - 26) / sTimerStateTimer;
                    gSaveContext.timerX[TIMER_ID_MAIN] -= svar1;

                    if (gSaveContext.save.info.playerData.healthCapacity > 0xA0) {
                        svar1 = (gSaveContext.timerY[TIMER_ID_MAIN] - 54) / sTimerStateTimer; // two rows of hearts
                    } else {
                        svar1 = (gSaveContext.timerY[TIMER_ID_MAIN] - 46) / sTimerStateTimer; // one row of hearts
                    }
                    gSaveContext.timerY[TIMER_ID_MAIN] -= svar1;

                    sTimerStateTimer--;
                    if (sTimerStateTimer == 0) {
                        sTimerStateTimer = 20;
                        gSaveContext.timerX[TIMER_ID_MAIN] = 26;
                        if (gSaveContext.save.info.playerData.healthCapacity > 0xA0) {
                            gSaveContext.timerY[TIMER_ID_MAIN] = 54; // two rows of hearts
                        } else {
                            gSaveContext.timerY[TIMER_ID_MAIN] = 46; // one row of hearts
                        }

                        gSaveContext.timerState = TIMER_STATE_UP_TICK;
                    }
                    FALLTHROUGH;
                case TIMER_STATE_UP_TICK:
                    if (gSaveContext.timerState == TIMER_STATE_UP_TICK) {
                        if (gSaveContext.save.info.playerData.healthCapacity > 0xA0) {
                            gSaveContext.timerY[TIMER_ID_MAIN] = 54; // two rows of hearts
                        } else {
                            gSaveContext.timerY[TIMER_ID_MAIN] = 46; // one row of hearts
                        }
                    }

                    if (gSaveContext.timerState >= TIMER_STATE_ENV_HAZARD_MOVE) {
                        sTimerNextSecondTimer--;
                        if (sTimerNextSecondTimer == 0) {
                            gSaveContext.timerSeconds++;
                            sTimerNextSecondTimer = 20;

                            if (gSaveContext.timerSeconds == 3599) { // 59 minutes, 59 seconds
                                sTimerStateTimer = 40;
                                gSaveContext.timerState = TIMER_STATE_UP_FREEZE;
                            } else {
                                SFX_PLAY_CENTERED(NA_SE_SY_WARNING_COUNT_N);
                            }
                        }
                    }
                    break;

                case TIMER_STATE_STOP:
                    if (gSaveContext.subTimerState != SUBTIMER_STATE_OFF) {
                        sSubTimerStateTimer = 20;
                        sSubTimerNextSecondTimer = 20;
                        gSaveContext.timerX[TIMER_ID_SUB] = 140;
                        gSaveContext.timerY[TIMER_ID_SUB] = 80;

                        if (gSaveContext.subTimerState <= SUBTIMER_STATE_STOP) {
                            gSaveContext.subTimerState = SUBTIMER_STATE_DOWN_PREVIEW;
                        } else {
                            gSaveContext.subTimerState = SUBTIMER_STATE_UP_PREVIEW;
                        }

                        gSaveContext.timerState = TIMER_STATE_OFF;
                    } else {
                        gSaveContext.timerState = TIMER_STATE_OFF;
                    }
                    FALLTHROUGH;
                case TIMER_STATE_UP_FREEZE:
                    break;

                default: // TIMER_STATE_OFF
                    // Process the subTimer only if the main timer is off
                    timerId = TIMER_ID_SUB;

                    switch (gSaveContext.subTimerState) {
                        case SUBTIMER_STATE_DOWN_INIT:
                        case SUBTIMER_STATE_UP_INIT:
                            sSubTimerStateTimer = 20;
                            sSubTimerNextSecondTimer = 20;
                            gSaveContext.timerX[TIMER_ID_SUB] = 140;
                            gSaveContext.timerY[TIMER_ID_SUB] = 80;
                            if (gSaveContext.subTimerState == SUBTIMER_STATE_DOWN_INIT) {
                                gSaveContext.subTimerState = SUBTIMER_STATE_DOWN_PREVIEW;
                            } else {
                                gSaveContext.subTimerState = SUBTIMER_STATE_UP_PREVIEW;
                            }
                            break;

                        case SUBTIMER_STATE_DOWN_PREVIEW:
                        case SUBTIMER_STATE_UP_PREVIEW:
                            sSubTimerStateTimer--;
                            if (sSubTimerStateTimer == 0) {
                                sSubTimerStateTimer = 20;
                                if (gSaveContext.subTimerState == SUBTIMER_STATE_DOWN_PREVIEW) {
                                    gSaveContext.subTimerState = SUBTIMER_STATE_DOWN_MOVE;
                                } else {
                                    gSaveContext.subTimerState = SUBTIMER_STATE_UP_MOVE;
                                }
                            }
                            break;

                        case SUBTIMER_STATE_DOWN_MOVE:
                        case SUBTIMER_STATE_UP_MOVE:
                            PRINTF("event_xp[1]=%d,  event_yp[1]=%d  TOTAL_EVENT_TM=%d\n",
                                   ((void)0, gSaveContext.timerX[TIMER_ID_SUB]),
                                   ((void)0, gSaveContext.timerY[TIMER_ID_SUB]), gSaveContext.subTimerSeconds);
                            svar1 = (gSaveContext.timerX[TIMER_ID_SUB] - 26) / sSubTimerStateTimer;
                            gSaveContext.timerX[TIMER_ID_SUB] -= svar1;
                            if (gSaveContext.save.info.playerData.healthCapacity > 0xA0) {
                                // two rows of hearts
                                svar1 = (gSaveContext.timerY[TIMER_ID_SUB] - 54) / sSubTimerStateTimer;
                            } else {
                                // one row of hearts
                                svar1 = (gSaveContext.timerY[TIMER_ID_SUB] - 46) / sSubTimerStateTimer;
                            }
                            gSaveContext.timerY[TIMER_ID_SUB] -= svar1;

                            sSubTimerStateTimer--;
                            if (sSubTimerStateTimer == 0) {
                                sSubTimerStateTimer = 20;
                                gSaveContext.timerX[TIMER_ID_SUB] = 26;

                                if (gSaveContext.save.info.playerData.healthCapacity > 0xA0) {
                                    gSaveContext.timerY[TIMER_ID_SUB] = 54; // two rows of hearts
                                } else {
                                    gSaveContext.timerY[TIMER_ID_SUB] = 46; // one row of hearts
                                }

                                if (gSaveContext.subTimerState == SUBTIMER_STATE_DOWN_MOVE) {
                                    gSaveContext.subTimerState = SUBTIMER_STATE_DOWN_TICK;
                                } else {
                                    gSaveContext.subTimerState = SUBTIMER_STATE_UP_TICK;
                                }
                            }
                            FALLTHROUGH;
                        case SUBTIMER_STATE_DOWN_TICK:
                        case SUBTIMER_STATE_UP_TICK:
                            if ((gSaveContext.subTimerState == SUBTIMER_STATE_DOWN_TICK) ||
                                (gSaveContext.subTimerState == SUBTIMER_STATE_UP_TICK)) {
                                if (gSaveContext.save.info.playerData.healthCapacity > 0xA0) {
                                    gSaveContext.timerY[TIMER_ID_SUB] = 54; // two rows of hearts
                                } else {
                                    gSaveContext.timerY[TIMER_ID_SUB] = 46; // one row of hearts
                                }
                            }

                            if (gSaveContext.subTimerState >= SUBTIMER_STATE_DOWN_MOVE) {
                                sSubTimerNextSecondTimer--;
                                if (sSubTimerNextSecondTimer == 0) {
                                    sSubTimerNextSecondTimer = 20;
                                    if (gSaveContext.subTimerState == SUBTIMER_STATE_DOWN_TICK) {
                                        gSaveContext.subTimerSeconds--;
                                        PRINTF("TOTAL_EVENT_TM=%d\n", gSaveContext.subTimerSeconds);

#if OOT_VERSION < PAL_1_0
                                        if (gSaveContext.subTimerSeconds == 0)
#else
                                        if (gSaveContext.subTimerSeconds <= 0)
#endif
                                        {
                                            // Out of time
                                            if (!Flags_GetSwitch(play, 0x37) ||
                                                ((play->sceneId != SCENE_GANON_BOSS) &&
                                                 (play->sceneId != SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR) &&
                                                 (play->sceneId != SCENE_GANONS_TOWER_COLLAPSE_INTERIOR) &&
                                                 (play->sceneId != SCENE_INSIDE_GANONS_CASTLE_COLLAPSE))) {
                                                sSubTimerStateTimer = 40;
                                                gSaveContext.subTimerState = SUBTIMER_STATE_RESPAWN;
                                                gSaveContext.save.cutsceneIndex = CS_INDEX_NONE;
                                                Message_StartTextbox(play, 0x71B0, NULL);
                                                Player_SetCsActionWithHaltedActors(play, NULL, PLAYER_CSACTION_8);
                                            } else {
                                                sSubTimerStateTimer = 40;
                                                gSaveContext.subTimerState = SUBTIMER_STATE_STOP;
                                            }
                                        } else if (gSaveContext.subTimerSeconds > 60) {
                                            // Beep at "xx:x1" (every 10 seconds)
                                            if (sTimerDigits[4] == 1) {
                                                SFX_PLAY_CENTERED(NA_SE_SY_MESSAGE_WOMAN);
                                            }
                                        } else if (gSaveContext.subTimerSeconds > 10) {
                                            // Beep on alternating seconds
                                            if ((sTimerDigits[4] % 2) != 0) {
                                                SFX_PLAY_CENTERED(NA_SE_SY_WARNING_COUNT_N);
                                            }
                                        } else {
                                            // Beep every second
                                            SFX_PLAY_CENTERED(NA_SE_SY_WARNING_COUNT_E);
                                        }
                                    } else { // SUBTIMER_STATE_UP_TICK
                                        gSaveContext.subTimerSeconds++;

                                        // Special case for the running-man race
                                        if (GET_EVENTINF(EVENTINF_MARATHON_ACTIVE) &&
                                            (gSaveContext.subTimerSeconds == MARATHON_TIME_LIMIT)) {
                                            // After 4 minutes, cancel the timer
                                            Message_StartTextbox(play, 0x6083, NULL);
                                            CLEAR_EVENTINF(EVENTINF_MARATHON_ACTIVE);
                                            gSaveContext.subTimerState = SUBTIMER_STATE_OFF;
                                        }
                                    }

                                    // Beep at the minute mark
                                    if ((gSaveContext.subTimerSeconds % 60) == 0) {
                                        SFX_PLAY_CENTERED(NA_SE_SY_WARNING_COUNT_N);
                                    }
                                }
                            }
                            break;

                        case SUBTIMER_STATE_STOP:
                            sSubTimerStateTimer--;
                            if (sSubTimerStateTimer == 0) {
                                gSaveContext.subTimerState = SUBTIMER_STATE_OFF;
                            }
                            break;
                    }
                    break;
            }

            if (((gSaveContext.timerState != TIMER_STATE_OFF) && (gSaveContext.timerState != TIMER_STATE_STOP)) ||
                (gSaveContext.subTimerState != SUBTIMER_STATE_OFF)) {
                sTimerDigits[0] = sTimerDigits[1] = sTimerDigits[3] = 0;
                sTimerDigits[2] = 10; // digit 10 is used as ':' (colon)

                if (gSaveContext.timerState != TIMER_STATE_OFF) {
                    sTimerDigits[4] = gSaveContext.timerSeconds;
                } else {
                    sTimerDigits[4] = gSaveContext.subTimerSeconds;
                }

                while (sTimerDigits[4] >= 60) {
                    sTimerDigits[1]++;
                    if (sTimerDigits[1] >= 10) {
                        sTimerDigits[0]++;
                        sTimerDigits[1] -= 10;
                    }
                    sTimerDigits[4] -= 60;
                }

                while (sTimerDigits[4] >= 10) {
                    sTimerDigits[3]++;
                    sTimerDigits[4] -= 10;
                }

                // Clock Icon
                gDPPipeSync(OVERLAY_DISP++);
                gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, 255);
                gDPSetEnvColor(OVERLAY_DISP++, 0, 0, 0, 0);
                OVERLAY_DISP =
                    Gfx_TextureIA8(OVERLAY_DISP, gClockIconTex, 16, 16, ((void)0, gSaveContext.timerX[timerId]),
                                   ((void)0, gSaveContext.timerY[timerId]) + 2, 16, 16, 1 << 10, 1 << 10);

                // Timer Counter
                gDPPipeSync(OVERLAY_DISP++);
                gDPSetCombineLERP(OVERLAY_DISP++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE,
                                  TEXEL0, 0, PRIMITIVE, 0);

                if (gSaveContext.timerState != TIMER_STATE_OFF) {
                    // TIMER_ID_MAIN
                    if ((gSaveContext.timerSeconds < 10) && (gSaveContext.timerState <= TIMER_STATE_STOP)) {
                        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 50, 0, 255);
                    } else {
                        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, 255);
                    }
                } else {
                    // TIMER_ID_SUB
                    if ((gSaveContext.subTimerSeconds < 10) && (gSaveContext.subTimerState <= SUBTIMER_STATE_RESPAWN)) {
                        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 50, 0, 255);
                    } else {
                        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 0, 255);
                    }
                }

                for (svar1 = 0; svar1 < ARRAY_COUNT(sTimerDigits); svar1++) {
                    OVERLAY_DISP =
                        Gfx_TextureI8(OVERLAY_DISP, ((u8*)gCounterDigit0Tex + (8 * 16 * sTimerDigits[svar1])), 8, 16,
                                      ((void)0, gSaveContext.timerX[timerId]) + timerDigitLeftPos[svar1],
                                      ((void)0, gSaveContext.timerY[timerId]), sDigitWidths[svar1], VREG(42),
                                      VREG(43) << 1, VREG(43) << 1);
                }
            }
        }
    }

#if DEBUG_FEATURES
    if (pauseCtx->debugState == PAUSE_DEBUG_STATE_FLAG_SET_OPEN) {
        FlagSet_Update(play);
    }
#endif

    if (interfaceCtx->unk_244 != 0) {
        gDPPipeSync(OVERLAY_DISP++);
        gSPDisplayList(OVERLAY_DISP++, sInterfaceFillSetupDL);
        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 0, 0, 0, interfaceCtx->unk_244);
        gDPFillRectangle(OVERLAY_DISP++, 0, 0, gScreenWidth - 1, gScreenHeight - 1);
    }

    // @recomp Back to the console's frame for whatever draws next.
    gEXSetScissorAlign(OVERLAY_DISP++, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 0, 0, 0, 0, SCREEN_WIDTH,
                       SCREEN_HEIGHT);
    hud_align(play->state.gfxCtx, G_EX_ORIGIN_NONE);
    CLOSE_DISPS(play->state.gfxCtx, "../z_parameter.c", 4269);
}

// The minimap's own symbols, declared in no header the patch build sees.
extern s16 sEntranceIconMapIndex;
extern u64 gMapDungeonEntranceIconTex[];
void Minimap_DrawCompassIcons(PlayState* play);
void MapMark_Draw(PlayState* play);

// @recomp Patched to skip the dungeon-entrance icon where the map data parks it at the
// frame's corner (see the top of the file); a real entrance draws as before.
RECOMP_PATCH void Minimap_Draw(PlayState* play) {
    s32 pad[2];
    InterfaceContext* interfaceCtx = &play->interfaceCtx;
    s32 mapIndex = gSaveContext.mapIndex;

    OPEN_DISPS(play->state.gfxCtx, "../z_map_exp.c", 626);

    if (play->pauseCtx.state <= PAUSE_STATE_INIT) {
        switch (play->sceneId) {
            case SCENE_DEKU_TREE:
            case SCENE_DODONGOS_CAVERN:
            case SCENE_JABU_JABU:
            case SCENE_FOREST_TEMPLE:
            case SCENE_FIRE_TEMPLE:
            case SCENE_WATER_TEMPLE:
            case SCENE_SPIRIT_TEMPLE:
            case SCENE_SHADOW_TEMPLE:
            case SCENE_BOTTOM_OF_THE_WELL:
            case SCENE_ICE_CAVERN:
                if (!R_MINIMAP_DISABLED) {
                    Gfx_SetupDL_39Overlay(play->state.gfxCtx);
                    gDPSetCombineLERP(OVERLAY_DISP++, 1, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0, 1, 0, PRIMITIVE, 0,
                                      TEXEL0, 0, PRIMITIVE, 0);

                    if (CHECK_DUNGEON_ITEM(DUNGEON_MAP, mapIndex)) {
                        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 100, 255, 255, interfaceCtx->minimapAlpha);

                        gDPLoadTextureBlock_4b(OVERLAY_DISP++, interfaceCtx->mapSegment, G_IM_FMT_I, MAP_I_TEX_WIDTH,
                                               MAP_I_TEX_HEIGHT, 0, G_TX_NOMIRROR | G_TX_WRAP,
                                               G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD,
                                               G_TX_NOLOD);

                        gSPTextureRectangle(OVERLAY_DISP++, R_DGN_MINIMAP_X << 2, R_DGN_MINIMAP_Y << 2,
                                            (R_DGN_MINIMAP_X + MAP_I_TEX_WIDTH) << 2,
                                            (R_DGN_MINIMAP_Y + MAP_I_TEX_HEIGHT) << 2, G_TX_RENDERTILE, 0, 0, 1 << 10,
                                            1 << 10);
                    }

                    if (CHECK_DUNGEON_ITEM(DUNGEON_COMPASS, mapIndex)) {
                        Minimap_DrawCompassIcons(play); // Draw icons for the player spawn and current position
                        Gfx_SetupDL_39Overlay(play->state.gfxCtx);
                        MapMark_Draw(play);
                    }
                }

                if (CHECK_BTN_ALL(play->state.input[0].press.button, BTN_L) && !Play_InCsMode(play)) {
                    PRINTF("Game_play_demo_mode_check=%d\n", Play_InCsMode(play));
                    // clang-format off
                    if (!R_MINIMAP_DISABLED) { SFX_PLAY_CENTERED(NA_SE_SY_CAMERA_ZOOM_UP);
                    } else {
                        SFX_PLAY_CENTERED(NA_SE_SY_CAMERA_ZOOM_DOWN);
                    }
                    // clang-format on
                    R_MINIMAP_DISABLED ^= 1;
                }

                break;
            case SCENE_HYRULE_FIELD:
            case SCENE_KAKARIKO_VILLAGE:
            case SCENE_GRAVEYARD:
            case SCENE_ZORAS_RIVER:
            case SCENE_KOKIRI_FOREST:
            case SCENE_SACRED_FOREST_MEADOW:
            case SCENE_LAKE_HYLIA:
            case SCENE_ZORAS_DOMAIN:
            case SCENE_ZORAS_FOUNTAIN:
            case SCENE_GERUDO_VALLEY:
            case SCENE_LOST_WOODS:
            case SCENE_DESERT_COLOSSUS:
            case SCENE_GERUDOS_FORTRESS:
            case SCENE_HAUNTED_WASTELAND:
            case SCENE_HYRULE_CASTLE:
            case SCENE_DEATH_MOUNTAIN_TRAIL:
            case SCENE_DEATH_MOUNTAIN_CRATER:
            case SCENE_GORON_CITY:
            case SCENE_LON_LON_RANCH:
            case SCENE_OUTSIDE_GANONS_CASTLE:
                if (!R_MINIMAP_DISABLED) {
                    Gfx_SetupDL_39Overlay(play->state.gfxCtx);

                    gDPSetCombineMode(OVERLAY_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
                    gDPSetPrimColor(OVERLAY_DISP++, 0, 0, R_MINIMAP_COLOR(0), R_MINIMAP_COLOR(1), R_MINIMAP_COLOR(2),
                                    interfaceCtx->minimapAlpha);

                    gDPLoadTextureBlock_4b(OVERLAY_DISP++, interfaceCtx->mapSegment, G_IM_FMT_IA,
                                           gMapData->owMinimapWidth[mapIndex], gMapData->owMinimapHeight[mapIndex], 0,
                                           G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK,
                                           G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

                    gSPTextureRectangle(OVERLAY_DISP++, R_OW_MINIMAP_X << 2, R_OW_MINIMAP_Y << 2,
                                        (R_OW_MINIMAP_X + gMapData->owMinimapWidth[mapIndex]) << 2,
                                        (R_OW_MINIMAP_Y + gMapData->owMinimapHeight[mapIndex]) << 2, G_TX_RENDERTILE, 0,
                                        0, 1 << 10, 1 << 10);

                    if (((play->sceneId != SCENE_KAKARIKO_VILLAGE) && (play->sceneId != SCENE_KOKIRI_FOREST) &&
                         (play->sceneId != SCENE_ZORAS_FOUNTAIN)) ||
                        (LINK_AGE_IN_YEARS != YEARS_ADULT)) {
                        // @recomp Parked at the corner: no entrance on this map, nothing to show.
                        if (!((gMapData->owEntranceIconPosX[sEntranceIconMapIndex] == 1) &&
                              (gMapData->owEntranceIconPosY[sEntranceIconMapIndex] == 0)))
                        if ((gMapData->owEntranceFlag[sEntranceIconMapIndex] == 0xFFFF) ||
                            ((gMapData->owEntranceFlag[sEntranceIconMapIndex] != 0xFFFF) &&
                             (gSaveContext.save.info.infTable[INFTABLE_INDEX_1AX] &
                              gBitFlags[gMapData->owEntranceFlag[mapIndex]]))) {

                            gDPLoadTextureBlock(OVERLAY_DISP++, gMapDungeonEntranceIconTex, G_IM_FMT_RGBA, G_IM_SIZ_16b,
                                                8, 8, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP,
                                                G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

                            gSPTextureRectangle(OVERLAY_DISP++,
                                                gMapData->owEntranceIconPosX[sEntranceIconMapIndex] << 2,
                                                gMapData->owEntranceIconPosY[sEntranceIconMapIndex] << 2,
                                                (gMapData->owEntranceIconPosX[sEntranceIconMapIndex] + 8) << 2,
                                                (gMapData->owEntranceIconPosY[sEntranceIconMapIndex] + 8) << 2,
                                                G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);
                        }
                    }

                    if ((play->sceneId == SCENE_ZORAS_FOUNTAIN) &&
                        (gSaveContext.save.info.infTable[INFTABLE_INDEX_1AX] & gBitFlags[INFTABLE_1A9_SHIFT])) {
                        gDPLoadTextureBlock(OVERLAY_DISP++, gMapDungeonEntranceIconTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8,
                                            8, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK,
                                            G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

                        gSPTextureRectangle(OVERLAY_DISP++, 270 << 2, 154 << 2, 278 << 2, 162 << 2, G_TX_RENDERTILE, 0,
                                            0, 1 << 10, 1 << 10);
                    }

                    Minimap_DrawCompassIcons(play); // Draw icons for the player spawn and current position
                }

                if (CHECK_BTN_ALL(play->state.input[0].press.button, BTN_L) && !Play_InCsMode(play)) {
                    // clang-format off
                    if (!R_MINIMAP_DISABLED) { SFX_PLAY_CENTERED(NA_SE_SY_CAMERA_ZOOM_UP);
                    } else {
                        SFX_PLAY_CENTERED(NA_SE_SY_CAMERA_ZOOM_DOWN);
                    }
                    // clang-format on
                    R_MINIMAP_DISABLED ^= 1;
                }

                break;
        }
    }

    CLOSE_DISPS(play->state.gfxCtx, "../z_map_exp.c", 782);
}

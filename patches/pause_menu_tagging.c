// The pause menu tagged (phase 45): its rotating cube of pages, its cursor, and its two views.
//
// The cube is four pages, each loaded as one matrix by KaleidoScope_DrawPages: the pages that
// are not being looked at first, then the one that is, then the prompt's page when a prompt is
// up. Each load gets a group of its own by page, decomposed and editable, so a page turn (the
// pitch values the game animates) is drawn between game frames; the prompt's page is a fifth.
// The cursor is four quads drawn by vertex positions under the active page's matrix, so it
// gets a group with the vertices interpolated (the pulse the game gives it), every component
// held on the frame it jumps to another slot, and the page's matrix loaded again under it and
// once more after it, because the renderer attaches a group to a matrix at the load. The
// menu's view is applied twice a frame by KaleidoScope_SetView with two eyes; each gets an id
// of its own (the play camera's id is left to the play camera, the phase 36 note) and both are
// smooth by construction: neither eye moves while the menu is up.
//
// The page background tables are statics of the overlay with no symbol in the table, so their
// definitions are copied from the source below, verbatim; every entry is an asset symbol the
// table knows. The dungeon flag the function reads is a static the update sets from the scene,
// and pause_in_dungeon decides it the same way from the same list. The stick repeat timers are
// the function's own statics and stay so. Everything else is the game's, line for line.

#include "patches.h"
#include "tagging_helpers.h"

#include "overlays/misc/ovl_kaleido_scope/z_kaleido_scope.h"
#include "array_count.h"
#include "controller.h"
#include "gfx.h"
#include "gfx_setupdl.h"
#include "gfxalloc.h"
#include "language_array.h"
#include "map.h"
#include "printf.h"
#include "regs.h"
#include "segment_symbols.h"
#include "segmented_address.h"
#include "sfx.h"
#include "sys_matrix.h"
#include "translation.h"
#include "audio.h"
#include "play_state.h"
#include "player.h"
#include "save.h"
#include "view.h"

// From camera_tagging.c: the id and verdict the next View_Apply uses instead of the play
// camera's.
void camera_next_view(u32 id, s32 smooth);

// The overlay's own color constants (z_kaleido_scope.c, the console's values).
#define KALEIDO_PROMPT_CURSOR_R 100
#define KALEIDO_PROMPT_CURSOR_G 100
#define KALEIDO_PROMPT_CURSOR_B 255
#define KALEIDO_COLOR_CURSOR_UNK_R 0
#define KALEIDO_COLOR_CURSOR_UNK_G 50
#define KALEIDO_COLOR_CURSOR_UNK_B 255

// Statics of the overlay this function shares with its other functions, reached by address
// through patches/game_static_syms.toml: the cursor's environment color, which
// KaleidoScope_DrawCursor reads.
extern s16 D_8082AB8C;
extern s16 D_8082AB90;
extern s16 D_8082AB94;

// The prompt's cursor arrows, display lists in the pause assets.
extern Gfx gPromptCursorLeftDL[];
extern Gfx gPromptCursorRightDL[];

// The overlay's own functions the reproduction calls, declared in its header or here.
Gfx* KaleidoScope_DrawPageSections(Gfx* gfx, Vtx* vertices, void** textures);

// The pause menu's textures are asset symbols (the JPN and ENG sets this revision ships).
extern u64 gContinuePlayingENGTex[];
extern u64 gContinuePlayingFRATex[];
extern u64 gContinuePlayingGERTex[];
extern u64 gContinuePlayingJPNTex[];
extern u64 gPauseEquipment00Tex[];
extern u64 gPauseEquipment01Tex[];
extern u64 gPauseEquipment02Tex[];
extern u64 gPauseEquipment03Tex[];
extern u64 gPauseEquipment04Tex[];
extern u64 gPauseEquipment10ENGTex[];
extern u64 gPauseEquipment10JPNTex[];
extern u64 gPauseEquipment11Tex[];
extern u64 gPauseEquipment12Tex[];
extern u64 gPauseEquipment13Tex[];
extern u64 gPauseEquipment14Tex[];
extern u64 gPauseEquipment20Tex[];
extern u64 gPauseEquipment21Tex[];
extern u64 gPauseEquipment22Tex[];
extern u64 gPauseEquipment23Tex[];
extern u64 gPauseEquipment24Tex[];
extern u64 gPauseGameOver10Tex[];
extern u64 gPauseMap00Tex[];
extern u64 gPauseMap01Tex[];
extern u64 gPauseMap02Tex[];
extern u64 gPauseMap03Tex[];
extern u64 gPauseMap04Tex[];
extern u64 gPauseMap10ENGTex[];
extern u64 gPauseMap10JPNTex[];
extern u64 gPauseMap11Tex[];
extern u64 gPauseMap12Tex[];
extern u64 gPauseMap13Tex[];
extern u64 gPauseMap14Tex[];
extern u64 gPauseMap20Tex[];
extern u64 gPauseMap21Tex[];
extern u64 gPauseMap22Tex[];
extern u64 gPauseMap23Tex[];
extern u64 gPauseMap24Tex[];
extern u64 gPauseNoENGTex[];
extern u64 gPauseNoFRATex[];
extern u64 gPauseNoGERTex[];
extern u64 gPauseNoJPNTex[];
extern u64 gPauseQuestStatus00ENGTex[];
extern u64 gPauseQuestStatus00JPNTex[];
extern u64 gPauseQuestStatus01Tex[];
extern u64 gPauseQuestStatus02Tex[];
extern u64 gPauseQuestStatus03Tex[];
extern u64 gPauseQuestStatus04Tex[];
extern u64 gPauseQuestStatus10ENGTex[];
extern u64 gPauseQuestStatus10JPNTex[];
extern u64 gPauseQuestStatus11Tex[];
extern u64 gPauseQuestStatus12Tex[];
extern u64 gPauseQuestStatus13Tex[];
extern u64 gPauseQuestStatus14Tex[];
extern u64 gPauseQuestStatus20ENGTex[];
extern u64 gPauseQuestStatus20JPNTex[];
extern u64 gPauseQuestStatus21Tex[];
extern u64 gPauseQuestStatus22Tex[];
extern u64 gPauseQuestStatus23Tex[];
extern u64 gPauseQuestStatus24Tex[];
extern u64 gPauseSave00Tex[];
extern u64 gPauseSave01Tex[];
extern u64 gPauseSave02Tex[];
extern u64 gPauseSave03Tex[];
extern u64 gPauseSave04Tex[];
extern u64 gPauseSave10ENGTex[];
extern u64 gPauseSave10JPNTex[];
extern u64 gPauseSave11Tex[];
extern u64 gPauseSave12Tex[];
extern u64 gPauseSave13Tex[];
extern u64 gPauseSave14Tex[];
extern u64 gPauseSave20Tex[];
extern u64 gPauseSave21Tex[];
extern u64 gPauseSave22Tex[];
extern u64 gPauseSave23Tex[];
extern u64 gPauseSave24Tex[];
extern u64 gPauseSaveConfirmationENGTex[];
extern u64 gPauseSaveConfirmationFRATex[];
extern u64 gPauseSaveConfirmationGERTex[];
extern u64 gPauseSaveConfirmationJPNTex[];
extern u64 gPauseSavePromptENGTex[];
extern u64 gPauseSavePromptFRATex[];
extern u64 gPauseSavePromptGERTex[];
extern u64 gPauseSavePromptJPNTex[];
extern u64 gPauseSelectItem00ENGTex[];
extern u64 gPauseSelectItem00JPNTex[];
extern u64 gPauseSelectItem01Tex[];
extern u64 gPauseSelectItem02Tex[];
extern u64 gPauseSelectItem03Tex[];
extern u64 gPauseSelectItem04Tex[];
extern u64 gPauseSelectItem10ENGTex[];
extern u64 gPauseSelectItem10JPNTex[];
extern u64 gPauseSelectItem11Tex[];
extern u64 gPauseSelectItem12Tex[];
extern u64 gPauseSelectItem13Tex[];
extern u64 gPauseSelectItem14Tex[];
extern u64 gPauseSelectItem20ENGTex[];
extern u64 gPauseSelectItem20JPNTex[];
extern u64 gPauseSelectItem21Tex[];
extern u64 gPauseSelectItem22Tex[];
extern u64 gPauseSelectItem23Tex[];
extern u64 gPauseSelectItem24Tex[];
extern u64 gPauseYesENGTex[];
extern u64 gPauseYesFRATex[];
extern u64 gPauseYesGERTex[];
extern u64 gPauseYesJPNTex[];

// The page background tables, copied from z_kaleido_scope.c (NTSC: the JPN set, the ENG set,
// the game over set, and the macros that pick by language).
static void* sEquipPageBgQuadsJPNTexs[] = {
    // column 1
    gPauseEquipment00Tex,
    gPauseEquipment01Tex,
    gPauseEquipment02Tex,
    gPauseEquipment03Tex,
    gPauseEquipment04Tex,
    // column 2
    gPauseEquipment10JPNTex,
    gPauseEquipment11Tex,
    gPauseEquipment12Tex,
    gPauseEquipment13Tex,
    gPauseEquipment14Tex,
    // column 3
    gPauseEquipment20Tex,
    gPauseEquipment21Tex,
    gPauseEquipment22Tex,
    gPauseEquipment23Tex,
    gPauseEquipment24Tex,
};

static void* sItemPageBgQuadsJPNTexs[] = {
    // column 1
    gPauseSelectItem00JPNTex,
    gPauseSelectItem01Tex,
    gPauseSelectItem02Tex,
    gPauseSelectItem03Tex,
    gPauseSelectItem04Tex,
    // column 2
    gPauseSelectItem10JPNTex,
    gPauseSelectItem11Tex,
    gPauseSelectItem12Tex,
    gPauseSelectItem13Tex,
    gPauseSelectItem14Tex,
    // column 3
    gPauseSelectItem20JPNTex,
    gPauseSelectItem21Tex,
    gPauseSelectItem22Tex,
    gPauseSelectItem23Tex,
    gPauseSelectItem24Tex,
};

static void* sMapPageBgQuadsJPNTexs[] = {
    // column 1
    gPauseMap00Tex,
    gPauseMap01Tex,
    gPauseMap02Tex,
    gPauseMap03Tex,
    gPauseMap04Tex,
    // column 2
    gPauseMap10JPNTex,
    gPauseMap11Tex,
    gPauseMap12Tex,
    gPauseMap13Tex,
    gPauseMap14Tex,
    // column 3
    gPauseMap20Tex,
    gPauseMap21Tex,
    gPauseMap22Tex,
    gPauseMap23Tex,
    gPauseMap24Tex,
};

static void* sQuestPageBgQuadsJPNTexs[] = {
    // column 1
    gPauseQuestStatus00JPNTex,
    gPauseQuestStatus01Tex,
    gPauseQuestStatus02Tex,
    gPauseQuestStatus03Tex,
    gPauseQuestStatus04Tex,
    // column 2
    gPauseQuestStatus10JPNTex,
    gPauseQuestStatus11Tex,
    gPauseQuestStatus12Tex,
    gPauseQuestStatus13Tex,
    gPauseQuestStatus14Tex,
    // column 3
    gPauseQuestStatus20JPNTex,
    gPauseQuestStatus21Tex,
    gPauseQuestStatus22Tex,
    gPauseQuestStatus23Tex,
    gPauseQuestStatus24Tex,
};

static void* sSavePromptBgQuadsJPNTexs[] = {
    // column 1
    gPauseSave00Tex,
    gPauseSave01Tex,
    gPauseSave02Tex,
    gPauseSave03Tex,
    gPauseSave04Tex,
    // column 2
    gPauseSave10JPNTex,
    gPauseSave11Tex,
    gPauseSave12Tex,
    gPauseSave13Tex,
    gPauseSave14Tex,
    // column 3
    gPauseSave20Tex,
    gPauseSave21Tex,
    gPauseSave22Tex,
    gPauseSave23Tex,
    gPauseSave24Tex,
};

static void* sEquipPageBgQuadsENGTexs[] = {
    // column 1
    gPauseEquipment00Tex,
    gPauseEquipment01Tex,
    gPauseEquipment02Tex,
    gPauseEquipment03Tex,
    gPauseEquipment04Tex,
    // column 2
    gPauseEquipment10ENGTex,
    gPauseEquipment11Tex,
    gPauseEquipment12Tex,
    gPauseEquipment13Tex,
    gPauseEquipment14Tex,
    // column 3
    gPauseEquipment20Tex,
    gPauseEquipment21Tex,
    gPauseEquipment22Tex,
    gPauseEquipment23Tex,
    gPauseEquipment24Tex,
};

static void* sItemPageBgQuadsENGTexs[] = {
    // column 1
    gPauseSelectItem00ENGTex,
    gPauseSelectItem01Tex,
    gPauseSelectItem02Tex,
    gPauseSelectItem03Tex,
    gPauseSelectItem04Tex,
    // column 2
    gPauseSelectItem10ENGTex,
    gPauseSelectItem11Tex,
    gPauseSelectItem12Tex,
    gPauseSelectItem13Tex,
    gPauseSelectItem14Tex,
    // column 3
    gPauseSelectItem20ENGTex,
    gPauseSelectItem21Tex,
    gPauseSelectItem22Tex,
    gPauseSelectItem23Tex,
    gPauseSelectItem24Tex,
};

static void* sMapPageBgQuadsENGTexs[] = {
    // column 1
    gPauseMap00Tex,
    gPauseMap01Tex,
    gPauseMap02Tex,
    gPauseMap03Tex,
    gPauseMap04Tex,
    // column 2
    gPauseMap10ENGTex,
    gPauseMap11Tex,
    gPauseMap12Tex,
    gPauseMap13Tex,
    gPauseMap14Tex,
    // column 3
    gPauseMap20Tex,
    gPauseMap21Tex,
    gPauseMap22Tex,
    gPauseMap23Tex,
    gPauseMap24Tex,
};

static void* sQuestPageBgQuadsENGTexs[] = {
    // column 1
    gPauseQuestStatus00ENGTex,
    gPauseQuestStatus01Tex,
    gPauseQuestStatus02Tex,
    gPauseQuestStatus03Tex,
    gPauseQuestStatus04Tex,
    // column 2
    gPauseQuestStatus10ENGTex,
    gPauseQuestStatus11Tex,
    gPauseQuestStatus12Tex,
    gPauseQuestStatus13Tex,
    gPauseQuestStatus14Tex,
    // column 3
    gPauseQuestStatus20ENGTex,
    gPauseQuestStatus21Tex,
    gPauseQuestStatus22Tex,
    gPauseQuestStatus23Tex,
    gPauseQuestStatus24Tex,
};

static void* sSavePromptBgQuadsENGTexs[] = {
    // column 1
    gPauseSave00Tex,
    gPauseSave01Tex,
    gPauseSave02Tex,
    gPauseSave03Tex,
    gPauseSave04Tex,
    // column 2
    gPauseSave10ENGTex,
    gPauseSave11Tex,
    gPauseSave12Tex,
    gPauseSave13Tex,
    gPauseSave14Tex,
    // column 3
    gPauseSave20Tex,
    gPauseSave21Tex,
    gPauseSave22Tex,
    gPauseSave23Tex,
    gPauseSave24Tex,
};

static void* sGameOverTexs[] = {
    // column 1
    gPauseSave00Tex,
    gPauseSave01Tex,
    gPauseSave02Tex,
    gPauseSave03Tex,
    gPauseSave04Tex,
    // column 2
    gPauseGameOver10Tex,
    gPauseSave11Tex,
    gPauseSave12Tex,
    gPauseSave13Tex,
    gPauseSave14Tex,
    // column 3
    gPauseSave20Tex,
    gPauseSave21Tex,
    gPauseSave22Tex,
    gPauseSave23Tex,
    gPauseSave24Tex,
};


static void* sSavePromptMessageTexs[] =
    LANGUAGE_ARRAY(gPauseSavePromptJPNTex, gPauseSavePromptENGTex, gPauseSavePromptGERTex, gPauseSavePromptFRATex);

static void* sSaveConfirmationTexs[] = LANGUAGE_ARRAY(gPauseSaveConfirmationJPNTex, gPauseSaveConfirmationENGTex,
                                                      gPauseSaveConfirmationGERTex, gPauseSaveConfirmationFRATex);

static void* sContinuePromptTexs[] =
    LANGUAGE_ARRAY(gContinuePlayingJPNTex, gContinuePlayingENGTex, gContinuePlayingGERTex, gContinuePlayingFRATex);

static void* sPromptChoiceTexs[][2] = {
#if OOT_NTSC
    { gPauseYesJPNTex, gPauseNoJPNTex },
    { gPauseYesENGTex, gPauseNoENGTex },
#else
    { gPauseYesENGTex, gPauseNoENGTex },
    { gPauseYesGERTex, gPauseNoGERTex },
    { gPauseYesFRATex, gPauseNoFRATex },
#endif
};
#define EQUIPMENT_TEXS(language) ((language) != LANGUAGE_JPN ? sEquipPageBgQuadsENGTexs : sEquipPageBgQuadsJPNTexs)
#define SELECT_ITEM_TEXS(language) ((language) != LANGUAGE_JPN ? sItemPageBgQuadsENGTexs : sItemPageBgQuadsJPNTexs)
#define MAP_TEXS(language) ((language) != LANGUAGE_JPN ? sMapPageBgQuadsENGTexs : sMapPageBgQuadsJPNTexs)
#define QUEST_STATUS_TEXS(language) ((language) != LANGUAGE_JPN ? sQuestPageBgQuadsENGTexs : sQuestPageBgQuadsJPNTexs)
#define SAVE_TEXS(language) ((language) != LANGUAGE_JPN ? sSavePromptBgQuadsENGTexs : sSavePromptBgQuadsJPNTexs)

// The scenes the game's update calls a dungeon for the map page, the same list as its switch.
static s32 pause_in_dungeon(PlayState* play) {
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
                case SCENE_DEKU_TREE_BOSS:
                case SCENE_DODONGOS_CAVERN_BOSS:
                case SCENE_JABU_JABU_BOSS:
                case SCENE_FOREST_TEMPLE_BOSS:
                case SCENE_FIRE_TEMPLE_BOSS:
                case SCENE_WATER_TEMPLE_BOSS:
                case SCENE_SPIRIT_TEMPLE_BOSS:
                case SCENE_SHADOW_TEMPLE_BOSS:
            return true;
        default:
            return false;
    }
}

// One group per page matrix. The renderer attaches a group to the matrix loaded inside it, so
// the group is opened before the load and closed right after it; the page's draws that follow
// use that matrix, tagged.
static void tag_page_begin(GraphicsContext* gfxCtx, u32 page) {
    OPEN_DISPS(gfxCtx, "../z_kaleido_scope_PAL.c", 0);
    gEXMatrixGroupDecomposedNormal(POLY_OPA_DISP++, PAUSE_PAGE_TRANSFORM_ID_START + page, G_EX_PUSH, G_MTX_MODELVIEW,
                                   G_EX_EDIT_ALLOW);
    CLOSE_DISPS(gfxCtx, "../z_kaleido_scope_PAL.c", 0);
}

static void tag_page_end(GraphicsContext* gfxCtx) {
    OPEN_DISPS(gfxCtx, "../z_kaleido_scope_PAL.c", 0);
    gEXPopMatrixGroup(POLY_OPA_DISP++, G_MTX_MODELVIEW);
    CLOSE_DISPS(gfxCtx, "../z_kaleido_scope_PAL.c", 0);
}

// The cursor's group: the vertices interpolate (its pulse), every component is held on the
// frame it jumps to another slot, and the active page's matrix is loaded again under the
// group so the cursor's quads belong to it. Its previous place is kept in this patch's own
// storage, which persists since phase 41.
static void tag_cursor_begin(PlayState* play) {
    static s16 sCursorLastX = 0;
    static s16 sCursorLastY = 0;
    PauseContext* pauseCtx = &play->pauseCtx;
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    s16 x = pauseCtx->cursorVtx[0].v.ob[0];
    s16 y = pauseCtx->cursorVtx[0].v.ob[1];

    OPEN_DISPS(gfxCtx, "../z_kaleido_scope_PAL.c", 0);
    if ((x == sCursorLastX) && (y == sCursorLastY)) {
        gEXMatrixGroupDecomposedVerts(POLY_OPA_DISP++, PAUSE_CURSOR_TRANSFORM_ID, G_EX_PUSH, G_MTX_MODELVIEW,
                                      G_EX_EDIT_ALLOW);
    } else {
        gEXMatrixGroupDecomposedSkipAll(POLY_OPA_DISP++, PAUSE_CURSOR_TRANSFORM_ID, G_EX_PUSH, G_MTX_MODELVIEW,
                                        G_EX_EDIT_ALLOW);
    }
    MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, gfxCtx, "../z_kaleido_scope_PAL.c", 0);
    CLOSE_DISPS(gfxCtx, "../z_kaleido_scope_PAL.c", 0);
    sCursorLastX = x;
    sCursorLastY = y;
}

// Close the cursor's group and load the page's matrix once more under the page's group, for
// whatever the page draws after its cursor (the map's marks).
static void tag_cursor_end(PlayState* play) {
    GraphicsContext* gfxCtx = play->state.gfxCtx;

    OPEN_DISPS(gfxCtx, "../z_kaleido_scope_PAL.c", 0);
    gEXPopMatrixGroup(POLY_OPA_DISP++, G_MTX_MODELVIEW);
    MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, gfxCtx, "../z_kaleido_scope_PAL.c", 0);
    CLOSE_DISPS(gfxCtx, "../z_kaleido_scope_PAL.c", 0);
}

// @recomp Patched to give each of the menu's two views an id of its own, smooth: the eye of
// the cube's view and the eye of the panel's view both stand still while the menu is up, and
// neither is the play camera. Otherwise the game's function.
RECOMP_PATCH void KaleidoScope_SetView(PauseContext* pauseCtx, f32 x, f32 y, f32 z) {
    Vec3f eye;
    Vec3f lookAt;
    Vec3f up;

    eye.x = x;
    eye.y = y;
    eye.z = z;
    lookAt.x = lookAt.y = lookAt.z = 0.0f;
    up.x = up.z = 0.0f;
    up.y = 1.0f;

    // @recomp The panel's view is the one applied at (0, 0, 64); the cube's is the other.
    camera_next_view(((x == 0.0f) && (y == 0.0f) && (z == 64.0f)) ? PAUSE_PANEL_VIEW_TRANSFORM_ID
                                                                   : PAUSE_VIEW_TRANSFORM_ID,
                     1);

    View_LookAt(&pauseCtx->view, &eye, &lookAt, &up);
    View_Apply(&pauseCtx->view,
               VIEW_ALL | VIEW_FORCE_VIEWING | VIEW_FORCE_VIEWPORT | VIEW_FORCE_PROJECTION_PERSPECTIVE);
}

// @recomp Patched to tag each page's matrix and the cursor; see the top of the file.
RECOMP_PATCH void KaleidoScope_DrawPages(PlayState* play, GraphicsContext* gfxCtx) {
    static s16 D_8082ACF4[][3] = {
        { 0, 0, 0 },
        { 0, 0, 0 },
        { 0, 0, 0 },
        { 0, 0, 0 },
        { 255, 255, 0 },
        { 0, 0, 0 },
        { 0, 0, 0 },
        { 255, 255, 0 },
        { KALEIDO_COLOR_CURSOR_UNK_R, KALEIDO_COLOR_CURSOR_UNK_G, KALEIDO_COLOR_CURSOR_UNK_B },
        { 0, 0, 0 },
        { 0, 0, 0 },
        { KALEIDO_COLOR_CURSOR_UNK_R, KALEIDO_COLOR_CURSOR_UNK_G, KALEIDO_COLOR_CURSOR_UNK_B },
    };
    static s16 D_8082AD3C = 20;
    static s16 D_8082AD40 = 0;
    static s16 sStickXRepeatTimer = 0;
    static s16 sStickYRepeatTimer = 0;
    static s16 sStickXRepeatState = 0;
    static s16 sStickYRepeatState = 0;
    PauseContext* pauseCtx = &play->pauseCtx;
    s16 stepR;
    s16 stepG;
    s16 stepB;

    OPEN_DISPS(gfxCtx, "../z_kaleido_scope_PAL.c", 1100);

    if (!IS_PAUSE_STATE_GAMEOVER(pauseCtx)) {
        if (pauseCtx->state != PAUSE_STATE_SAVE_PROMPT) {
            stepR = ABS(D_8082AB8C - D_8082ACF4[pauseCtx->cursorColorSet + D_8082AD40][0]) / D_8082AD3C;
            stepG = ABS(D_8082AB90 - D_8082ACF4[pauseCtx->cursorColorSet + D_8082AD40][1]) / D_8082AD3C;
            stepB = ABS(D_8082AB94 - D_8082ACF4[pauseCtx->cursorColorSet + D_8082AD40][2]) / D_8082AD3C;
            if (D_8082AB8C >= D_8082ACF4[pauseCtx->cursorColorSet + D_8082AD40][0]) {
                D_8082AB8C -= stepR;
            } else {
                D_8082AB8C += stepR;
            }
            if (D_8082AB90 >= D_8082ACF4[pauseCtx->cursorColorSet + D_8082AD40][1]) {
                D_8082AB90 -= stepG;
            } else {
                D_8082AB90 += stepG;
            }
            if (D_8082AB94 >= D_8082ACF4[pauseCtx->cursorColorSet + D_8082AD40][2]) {
                D_8082AB94 -= stepB;
            } else {
                D_8082AB94 += stepB;
            }

            D_8082AD3C--;
            if (D_8082AD3C == 0) {
                D_8082AB8C = D_8082ACF4[pauseCtx->cursorColorSet + D_8082AD40][0];
                D_8082AB90 = D_8082ACF4[pauseCtx->cursorColorSet + D_8082AD40][1];
                D_8082AB94 = D_8082ACF4[pauseCtx->cursorColorSet + D_8082AD40][2];
                D_8082AD3C = ZREG(28 + D_8082AD40);
                D_8082AD40++;
                if (D_8082AD40 >= 4) {
                    D_8082AD40 = 0;
                }
            }

            if (pauseCtx->stickAdjX < -30) {
                if (sStickXRepeatState == -1) {
                    sStickXRepeatTimer--;
                    if (sStickXRepeatTimer < 0) {
                        sStickXRepeatTimer = R_PAUSE_STICK_REPEAT_DELAY;
                    } else {
                        pauseCtx->stickAdjX = 0;
                    }
                } else {
                    sStickXRepeatTimer = R_PAUSE_STICK_REPEAT_DELAY_FIRST;
                    sStickXRepeatState = -1;
                }
            } else if (pauseCtx->stickAdjX > 30) {
                if (sStickXRepeatState == 1) {
                    sStickXRepeatTimer--;
                    // NOLINTBEGIN
                    if (sStickXRepeatTimer < 0)
                        sStickXRepeatTimer = R_PAUSE_STICK_REPEAT_DELAY;
                    else
                        pauseCtx->stickAdjX = 0;
                    // NOLINTEND
                } else {
                    sStickXRepeatTimer = R_PAUSE_STICK_REPEAT_DELAY_FIRST;
                    sStickXRepeatState = 1;
                }
            } else {
                sStickXRepeatState = 0;
            }

            if (pauseCtx->stickAdjY < -30) {
                if (sStickYRepeatState == -1) {
                    sStickYRepeatTimer--;
                    if (sStickYRepeatTimer < 0) {
                        sStickYRepeatTimer = R_PAUSE_STICK_REPEAT_DELAY;
                    } else {
                        pauseCtx->stickAdjY = 0;
                    }
                } else {
                    sStickYRepeatTimer = R_PAUSE_STICK_REPEAT_DELAY_FIRST;
                    sStickYRepeatState = -1;
                }
            } else if (pauseCtx->stickAdjY > 30) {
                if (sStickYRepeatState == 1) {
                    sStickYRepeatTimer--;
                    // NOLINTBEGIN
                    if (sStickYRepeatTimer < 0)
                        sStickYRepeatTimer = R_PAUSE_STICK_REPEAT_DELAY;
                    else
                        pauseCtx->stickAdjY = 0;
                    // NOLINTEND
                } else {
                    sStickYRepeatTimer = R_PAUSE_STICK_REPEAT_DELAY_FIRST;
                    sStickYRepeatState = 1;
                }
            } else {
                sStickYRepeatState = 0;
            }
        }

        // Draw non-active pages (not the one being looked at)

        if (pauseCtx->pageIndex) { // pageIndex != PAUSE_ITEM
            gDPPipeSync(POLY_OPA_DISP++);
            gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA, G_CC_MODULATEIA);

            Matrix_Translate(0.0f, (f32)R_PAUSE_PAGES_Y_ORIGIN_2 / 100.0f, -(f32)R_PAUSE_DEPTH_OFFSET / 100.0f,
                             MTXMODE_NEW);
            Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
            Matrix_RotateX(-pauseCtx->itemPagePitch / 100.0f, MTXMODE_APPLY);

            // @recomp
            tag_page_begin(gfxCtx, 0);
            MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, gfxCtx, "../z_kaleido_scope_PAL.c", 1173);
            // @recomp
            tag_page_end(gfxCtx);

            POLY_OPA_DISP = KaleidoScope_DrawPageSections(POLY_OPA_DISP, pauseCtx->itemPageVtx,
                                                          SELECT_ITEM_TEXS(gSaveContext.language));

            KaleidoScope_DrawItemSelect(play);
        }

        if (pauseCtx->pageIndex != PAUSE_EQUIP) {
            gDPPipeSync(POLY_OPA_DISP++);
            gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA, G_CC_MODULATEIA);

            Matrix_Translate(-(f32)R_PAUSE_DEPTH_OFFSET / 100.0f, (f32)R_PAUSE_PAGES_Y_ORIGIN_2 / 100.0f, 0.0f,
                             MTXMODE_NEW);
            Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
            Matrix_RotateZ(pauseCtx->equipPagePitch / 100.0f, MTXMODE_APPLY);
            Matrix_RotateY(1.57f, MTXMODE_APPLY);

            // @recomp
            tag_page_begin(gfxCtx, 1);
            MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, gfxCtx, "../z_kaleido_scope_PAL.c", 1196);
            // @recomp
            tag_page_end(gfxCtx);

            POLY_OPA_DISP = KaleidoScope_DrawPageSections(POLY_OPA_DISP, pauseCtx->equipPageVtx,
                                                          EQUIPMENT_TEXS(gSaveContext.language));

            KaleidoScope_DrawEquipment(play);
        }

        if (pauseCtx->pageIndex != PAUSE_QUEST) {
            gDPPipeSync(POLY_OPA_DISP++);
            gDPSetTextureFilter(POLY_OPA_DISP++, G_TF_BILERP);
            gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA, G_CC_MODULATEIA);

            Matrix_Translate(0.0f, (f32)R_PAUSE_PAGES_Y_ORIGIN_2 / 100.0f, (f32)R_PAUSE_DEPTH_OFFSET / 100.0f,
                             MTXMODE_NEW);
            Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
            Matrix_RotateX(pauseCtx->questPagePitch / 100.0f, MTXMODE_APPLY);
            Matrix_RotateY(3.14f, MTXMODE_APPLY);

            // @recomp
            tag_page_begin(gfxCtx, 2);
            MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, gfxCtx, "../z_kaleido_scope_PAL.c", 1220);
            // @recomp
            tag_page_end(gfxCtx);

            POLY_OPA_DISP = KaleidoScope_DrawPageSections(POLY_OPA_DISP, pauseCtx->questPageVtx,
                                                          QUEST_STATUS_TEXS(gSaveContext.language));

            KaleidoScope_DrawQuestStatus(play, gfxCtx);
        }

        if (pauseCtx->pageIndex != PAUSE_MAP) {
            gDPPipeSync(POLY_OPA_DISP++);

            gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA, G_CC_MODULATEIA);

            Matrix_Translate((f32)R_PAUSE_DEPTH_OFFSET / 100.0f, (f32)R_PAUSE_PAGES_Y_ORIGIN_2 / 100.0f, 0.0f,
                             MTXMODE_NEW);
            Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
            Matrix_RotateZ(-pauseCtx->mapPagePitch / 100.0f, MTXMODE_APPLY);
            Matrix_RotateY(-1.57f, MTXMODE_APPLY);

            // @recomp
            tag_page_begin(gfxCtx, 3);
            MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, gfxCtx, "../z_kaleido_scope_PAL.c", 1243);
            // @recomp
            tag_page_end(gfxCtx);

            POLY_OPA_DISP =
                KaleidoScope_DrawPageSections(POLY_OPA_DISP, pauseCtx->mapPageVtx, MAP_TEXS(gSaveContext.language));

            if (pause_in_dungeon(play)) {
                KaleidoScope_DrawDungeonMap(play, gfxCtx);
                Gfx_SetupDL_42Opa(gfxCtx);

                gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);

                if (CHECK_DUNGEON_ITEM(DUNGEON_COMPASS, gSaveContext.mapIndex)) {
                    PauseMapMark_Draw(play);
                }
            } else {
                KaleidoScope_DrawWorldMap(play, gfxCtx);
            }
        }

        // Update and draw the active page being looked at

        gDPPipeSync(POLY_OPA_DISP++);
        gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA, G_CC_MODULATEIA);

        switch (pauseCtx->pageIndex) {
            case PAUSE_ITEM:
                Matrix_Translate(0.0f, (f32)R_PAUSE_PAGES_Y_ORIGIN_2 / 100.0f, -(f32)R_PAUSE_DEPTH_OFFSET / 100.0f,
                                 MTXMODE_NEW);
                Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
                Matrix_RotateX(-pauseCtx->itemPagePitch / 100.0f, MTXMODE_APPLY);

                // @recomp
                tag_page_begin(gfxCtx, 0);
                MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, gfxCtx, "../z_kaleido_scope_PAL.c", 1281);
                // @recomp
                tag_page_end(gfxCtx);

                POLY_OPA_DISP = KaleidoScope_DrawPageSections(POLY_OPA_DISP, pauseCtx->itemPageVtx,
                                                              SELECT_ITEM_TEXS(gSaveContext.language));

                KaleidoScope_DrawItemSelect(play);
                break;

            case PAUSE_MAP:
                Matrix_Translate((f32)R_PAUSE_DEPTH_OFFSET / 100.0f, (f32)R_PAUSE_PAGES_Y_ORIGIN_2 / 100.0f, 0.0f,
                                 MTXMODE_NEW);
                Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
                Matrix_RotateZ(-pauseCtx->mapPagePitch / 100.0f, MTXMODE_APPLY);
                Matrix_RotateY(-1.57f, MTXMODE_APPLY);

                // @recomp
                tag_page_begin(gfxCtx, 3);
                MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, gfxCtx, "../z_kaleido_scope_PAL.c", 1303);
                // @recomp
                tag_page_end(gfxCtx);

                POLY_OPA_DISP =
                    KaleidoScope_DrawPageSections(POLY_OPA_DISP, pauseCtx->mapPageVtx, MAP_TEXS(gSaveContext.language));

                if (pause_in_dungeon(play)) {
                    KaleidoScope_DrawDungeonMap(play, gfxCtx);
                    Gfx_SetupDL_42Opa(gfxCtx);

                    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);

                    if (pauseCtx->cursorSpecialPos == 0) {
                        // @recomp
                        tag_cursor_begin(play);
                        KaleidoScope_DrawCursor(play, PAUSE_MAP);
                        // @recomp
                        tag_cursor_end(play);
                    }

                    if (CHECK_DUNGEON_ITEM(DUNGEON_COMPASS, gSaveContext.mapIndex)) {
                        PauseMapMark_Draw(play);
                    }
                } else {
                    KaleidoScope_DrawWorldMap(play, gfxCtx);
                }
                break;

            case PAUSE_QUEST:
                gDPSetTextureFilter(POLY_OPA_DISP++, G_TF_BILERP);

                Matrix_Translate(0.0f, (f32)R_PAUSE_PAGES_Y_ORIGIN_2 / 100.0f, (f32)R_PAUSE_DEPTH_OFFSET / 100.0f,
                                 MTXMODE_NEW);
                Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
                Matrix_RotateX(pauseCtx->questPagePitch / 100.0f, MTXMODE_APPLY);
                Matrix_RotateY(3.14f, MTXMODE_APPLY);

                // @recomp
                tag_page_begin(gfxCtx, 2);
                MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, gfxCtx, "../z_kaleido_scope_PAL.c", 1343);
                // @recomp
                tag_page_end(gfxCtx);

                POLY_OPA_DISP = KaleidoScope_DrawPageSections(POLY_OPA_DISP, pauseCtx->questPageVtx,
                                                              QUEST_STATUS_TEXS(gSaveContext.language));

                KaleidoScope_DrawQuestStatus(play, gfxCtx);

                if (pauseCtx->cursorSpecialPos == 0) {
                    // @recomp
                    tag_cursor_begin(play);
                    KaleidoScope_DrawCursor(play, PAUSE_QUEST);
                    // @recomp
                    tag_cursor_end(play);
                }
                break;

            case PAUSE_EQUIP:
                Matrix_Translate(-(f32)R_PAUSE_DEPTH_OFFSET / 100.0f, (f32)R_PAUSE_PAGES_Y_ORIGIN_2 / 100.0f, 0.0f,
                                 MTXMODE_NEW);
                Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
                Matrix_RotateZ(pauseCtx->equipPagePitch / 100.0f, MTXMODE_APPLY);
                Matrix_RotateY(1.57f, MTXMODE_APPLY);

                // @recomp
                tag_page_begin(gfxCtx, 1);
                MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, gfxCtx, "../z_kaleido_scope_PAL.c", 1367);
                // @recomp
                tag_page_end(gfxCtx);

                POLY_OPA_DISP = KaleidoScope_DrawPageSections(POLY_OPA_DISP, pauseCtx->equipPageVtx,
                                                              EQUIPMENT_TEXS(gSaveContext.language));

                // @recomp THE PAGE'S MATRIX KEPT ACROSS THE PLAYER'S PREVIEW (the user, 2026-10-09:
                // "this screen only has issues with the selection box now showing", the equipment
                // page alone). The preview (Player_DrawPauseImpl) sets the current matrix to the
                // player's place and scale with no push. The game's cursor never minds, because it
                // draws under the page matrix already loaded and the preview's commands run from
                // the work list; but tag_cursor_begin loads the current matrix again as the page's,
                // which here was the player's, and the cursor was drawn at the player's tiny scale.
                Matrix_Push();
                KaleidoScope_DrawEquipment(play);
                Matrix_Pop();

                if (pauseCtx->cursorSpecialPos == 0) {
                    // @recomp
                    tag_cursor_begin(play);
                    KaleidoScope_DrawCursor(play, PAUSE_EQUIP);
                    // @recomp
                    tag_cursor_end(play);
                }
                break;
        }
    }

    // Update and draw prompt (save or gameover)

    Gfx_SetupDL_42Opa(gfxCtx);

    if ((pauseCtx->state == PAUSE_STATE_SAVE_PROMPT) || IS_PAUSE_STATE_GAMEOVER(pauseCtx)) {
        KaleidoScope_UpdatePrompt(play);

        gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA, G_CC_MODULATEIA);

        if ((u32)pauseCtx->pageIndex == PAUSE_ITEM) {
            pauseCtx->itemPagePitch = pauseCtx->promptPitch + 314.0f;

            Matrix_Translate(0.0f, (f32)R_PAUSE_PAGES_Y_ORIGIN_2 / 100.0f, -pauseCtx->promptDepthOffset / 10.0f,
                             MTXMODE_NEW);
            Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
            Matrix_RotateX(-pauseCtx->promptPitch / 100.0f, MTXMODE_APPLY);
        } else if (pauseCtx->pageIndex == PAUSE_MAP) {
            pauseCtx->mapPagePitch = pauseCtx->promptPitch + 314.0f;

            Matrix_Translate(pauseCtx->promptDepthOffset / 10.0f, (f32)R_PAUSE_PAGES_Y_ORIGIN_2 / 100.0f, 0.0f,
                             MTXMODE_NEW);
            Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
            Matrix_RotateZ(-pauseCtx->promptPitch / 100.0f, MTXMODE_APPLY);
            Matrix_RotateY(-1.57f, MTXMODE_APPLY);
        } else if (pauseCtx->pageIndex == PAUSE_QUEST) {
            pauseCtx->questPagePitch = pauseCtx->promptPitch + 314.0f;

            Matrix_Translate(0.0f, (f32)R_PAUSE_PAGES_Y_ORIGIN_2 / 100.0f, pauseCtx->promptDepthOffset / 10.0f,
                             MTXMODE_NEW);
            Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
            Matrix_RotateX(pauseCtx->promptPitch / 100.0f, MTXMODE_APPLY);
            Matrix_RotateY(3.14f, MTXMODE_APPLY);
        } else {
            pauseCtx->equipPagePitch = pauseCtx->promptPitch + 314.0f;

            Matrix_Translate(-pauseCtx->promptDepthOffset / 10.0f, (f32)R_PAUSE_PAGES_Y_ORIGIN_2 / 100.0f, 0.0f,
                             MTXMODE_NEW);
            Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
            Matrix_RotateZ(pauseCtx->promptPitch / 100.0f, MTXMODE_APPLY);
            Matrix_RotateY(1.57f, MTXMODE_APPLY);
        }

        // @recomp
        tag_page_begin(gfxCtx, 4);
        MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, gfxCtx, "../z_kaleido_scope_PAL.c", 1424);
        // @recomp
        tag_page_end(gfxCtx);

        if (IS_PAUSE_STATE_GAMEOVER(pauseCtx)) {
            POLY_OPA_DISP = KaleidoScope_DrawPageSections(POLY_OPA_DISP, pauseCtx->promptPageVtx, sGameOverTexs);
        } else { // PAUSE_STATE_SAVE_PROMPT
            POLY_OPA_DISP =
                KaleidoScope_DrawPageSections(POLY_OPA_DISP, pauseCtx->promptPageVtx, SAVE_TEXS(gSaveContext.language));
        }

        //! @bug Loads 32 vertices, but there are only 20 to load
        gSPVertex(POLY_OPA_DISP++, &pauseCtx->promptPageVtx[PAGE_BG_QUADS * 4], 32, 0);

        if (((pauseCtx->state == PAUSE_STATE_SAVE_PROMPT) &&
             (pauseCtx->savePromptState < PAUSE_SAVE_PROMPT_STATE_SAVED)) ||
            (pauseCtx->state == PAUSE_STATE_GAME_OVER_SAVE_PROMPT)) {
            POLY_OPA_DISP = KaleidoScope_QuadTextureIA8(POLY_OPA_DISP, sSavePromptMessageTexs[gSaveContext.language],
                                                        152, 16, PROMPT_QUAD_MESSAGE * 4);

            gDPSetCombineLERP(POLY_OPA_DISP++, 1, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0, 1, 0, PRIMITIVE, 0, TEXEL0,
                              0, PRIMITIVE, 0);
            gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, KALEIDO_PROMPT_CURSOR_R, KALEIDO_PROMPT_CURSOR_G,
                            KALEIDO_PROMPT_CURSOR_B, R_KALEIDO_PROMPT_CURSOR_ALPHA);

            if (pauseCtx->promptChoice == 0) {
                // PROMPT_QUAD_CURSOR_LEFT
                gSPDisplayList(POLY_OPA_DISP++, gPromptCursorLeftDL);
            } else {
                // PROMPT_QUAD_CURSOR_RIGHT
                gSPDisplayList(POLY_OPA_DISP++, gPromptCursorRightDL);
            }

            gDPPipeSync(POLY_OPA_DISP++);
            gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA, G_CC_MODULATEIA);
            gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, 255, 255, pauseCtx->alpha);

            POLY_OPA_DISP = KaleidoScope_QuadTextureIA8(POLY_OPA_DISP, sPromptChoiceTexs[gSaveContext.language][0], 48,
                                                        16, PROMPT_QUAD_CHOICE_YES * 4);

            POLY_OPA_DISP = KaleidoScope_QuadTextureIA8(POLY_OPA_DISP, sPromptChoiceTexs[gSaveContext.language][1], 48,
                                                        16, PROMPT_QUAD_CHOICE_NO * 4);
        } else if (((pauseCtx->state == PAUSE_STATE_SAVE_PROMPT) &&
                    (pauseCtx->savePromptState >= PAUSE_SAVE_PROMPT_STATE_SAVED)) ||
                   pauseCtx->state == PAUSE_STATE_GAME_OVER_SAVED) {
#if !PLATFORM_GC
            POLY_OPA_DISP = KaleidoScope_QuadTextureIA8(POLY_OPA_DISP, sSaveConfirmationTexs[gSaveContext.language],
                                                        152, 16, PROMPT_QUAD_MESSAGE * 4);
#endif
        } else if (((pauseCtx->state == PAUSE_STATE_GAME_OVER_CONTINUE_PROMPT) ||
                    (pauseCtx->state == PAUSE_STATE_GAME_OVER_FINISH))) {
            POLY_OPA_DISP = KaleidoScope_QuadTextureIA8(POLY_OPA_DISP, sContinuePromptTexs[gSaveContext.language], 152,
                                                        16, PROMPT_QUAD_MESSAGE * 4);

            gDPSetCombineLERP(POLY_OPA_DISP++, 1, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0, 1, 0, PRIMITIVE, 0, TEXEL0,
                              0, PRIMITIVE, 0);
            gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, KALEIDO_PROMPT_CURSOR_R, KALEIDO_PROMPT_CURSOR_G,
                            KALEIDO_PROMPT_CURSOR_B, R_KALEIDO_PROMPT_CURSOR_ALPHA);

            if (pauseCtx->promptChoice == 0) {
                // PROMPT_QUAD_CURSOR_LEFT
                gSPDisplayList(POLY_OPA_DISP++, gPromptCursorLeftDL);
            } else {
                // PROMPT_QUAD_CURSOR_RIGHT
                gSPDisplayList(POLY_OPA_DISP++, gPromptCursorRightDL);
            }

            gDPPipeSync(POLY_OPA_DISP++);
            gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA, G_CC_MODULATEIA);
            gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, 255, 255, pauseCtx->alpha);

            POLY_OPA_DISP = KaleidoScope_QuadTextureIA8(POLY_OPA_DISP, sPromptChoiceTexs[gSaveContext.language][0], 48,
                                                        16, PROMPT_QUAD_CHOICE_YES * 4);

            POLY_OPA_DISP = KaleidoScope_QuadTextureIA8(POLY_OPA_DISP, sPromptChoiceTexs[gSaveContext.language][1], 48,
                                                        16, PROMPT_QUAD_CHOICE_NO * 4);
        }

        gDPPipeSync(POLY_OPA_DISP++);
        gDPSetCombineLERP(POLY_OPA_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                          PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);

        if ((pauseCtx->state != PAUSE_STATE_GAME_OVER_CONTINUE_PROMPT) &&
            (pauseCtx->state != PAUSE_STATE_GAME_OVER_FINISH)) {
            gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, 255, 0, pauseCtx->alpha);
            gDPSetEnvColor(POLY_OPA_DISP++, 0, 0, 0, 0);
        }
    }

    CLOSE_DISPS(gfxCtx, "../z_kaleido_scope_PAL.c", 1577);
}

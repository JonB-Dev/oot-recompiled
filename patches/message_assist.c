// TEXT AT THE PERSON'S PACE, AND A OR B ALWAYS GETS THROUGH IT (2026-10-08).
//
// The user: "faster dialog typing so it doesn't animate in so slowly, and the ability to always
// skip press the a or b to auto-finish the typing and skip if needed rgardless if the game
// normally allows". The reference project has no such option; this is ours.
//
// HOW THE GAME TYPES. Each frame the message drawing (Message_DrawText) walks the decoded box from
// its start up to msgCtx->textDrawPos, and moves that position on by one character whenever the
// delay counter (textDelayTimer, reloaded from the message's own speed code) has run out: one
// character a game frame at the default, twenty a second. A box stop (a break, the end, a fade, an
// event, a delayed break) acts wherever the walk meets it, so the position may move on several
// characters at once and every stop still acts; only three codes act at the exact last position
// (the ocarina prompt, a wait for a button, quick text), so the position is never moved past them.
//
// HOW THE GAME SKIPS. Message_Update lets B, and only B, show the rest of the box at once, unless
// the box is the bottom of the screen kind or the message marks itself unskippable; and that skip
// carries on through the message's later boxes without waiting at their breaks, until something
// needs an answer. A does nothing until a box is fully shown.
//
// WHAT THIS DOES, every frame, from the one small function the game calls just before it draws
// the text of an ordinary box (Message_DrawTextBox, below, which is otherwise the game's own line
// for line), and only for the English box (the Japanese one is drawn from a wide buffer), never the
// credits and never the ocarina's box:
//   Text speed   moves the position on by one more character a frame (Fast), three more
//                (Faster), or to the next stop (Instant), and runs a slow message's delay down
//                as many times faster; a box still waits at its breaks for the press to go on.
//   Skip text    "always": while a box types, A shows the rest of that box (and waits at its end),
//                and B is the game's own skip, in every message, the unskippable ones included,
//                but never past an ocarina prompt; and while a box waits on a clock (a fading
//                box, a delayed break), A or B ends the wait. A box that waits for the game itself
//                (an event, a persistent box) is left alone, because its end is the game's cue.
#include "patches.h"

#include "controller.h"
#include "gfx.h"
#include "message.h"
#include "message_data_fmt.h"
#include "play_state.h"
#include "regs.h"
#include "save.h"

// The program's side (src/game/recomp_api.cpp): bits 0 and 1 the Text speed row, bit 2 the Skip
// text row's "always".
s32 recomp_message_assist(void);

// The message file's own state (z_message.c), reached by name through the game's symbols.
extern char sTextboxSkipped;
extern s16 sTextIsCredits;
// The treble clef the ocarina's box draws, in parameter_static.
extern u64 gOcarinaTrebleClefTex[];

#define MSG_BUF_DECODED (msgCtx->msgBufDecoded)

// The textbox kinds, from the decomp's message_data_static.h (TextBoxType), which cannot be
// included here: it pulls in the message tables the decomp's own build generates.
#define TEXTBOX_TYPE_BLACK   0
#define TEXTBOX_TYPE_BLUE    2
#define TEXTBOX_TYPE_OCARINA 3

// How many argument bytes follow a control code in the decoded box, as the game's drawing reads them.
static s32 MessageAssist_ArgCount(u8 c) {
    switch (c) {
        case MESSAGE_COLOR:
        case MESSAGE_SHIFT:
        case MESSAGE_BOX_BREAK_DELAYED:
        case MESSAGE_FADE:
        case MESSAGE_ITEM_ICON:
        case MESSAGE_TEXT_SPEED:
            return 1;
        case MESSAGE_FADE2:
        case MESSAGE_SFX:
        case MESSAGE_TEXTID:
            return 2;
        default:
            return 0;
    }
}

// A code the drawing must reach on its own: a stop, or one that acts only at the exact position.
static s32 MessageAssist_IsStop(u8 c) {
    switch (c) {
        case MESSAGE_BOX_BREAK:
        case MESSAGE_END:
        case MESSAGE_TEXTID:
        case MESSAGE_PERSISTENT:
        case MESSAGE_EVENT:
        case MESSAGE_BOX_BREAK_DELAYED:
        case MESSAGE_FADE:
        case MESSAGE_FADE2:
        case MESSAGE_AWAIT_BUTTON_PRESS:
        case MESSAGE_OCARINA:
        case MESSAGE_QUICKTEXT_ENABLE:
        case MESSAGE_TWO_CHOICE:
        case MESSAGE_THREE_CHOICE:
            return true;
        default:
            return false;
    }
}

// The drawn position moved on by up to `steps` characters, never past a stop.
static u16 MessageAssist_Advance(MessageContext* msgCtx, u16 pos, s32 steps) {
    while ((steps > 0) && (pos < msgCtx->decodedTextLen)) {
        u8 c = MSG_BUF_DECODED[pos];

        if (MessageAssist_IsStop(c)) {
            break;
        }
        pos += 1 + MessageAssist_ArgCount(c);
        steps--;
    }
    return (pos < msgCtx->decodedTextLen) ? pos : msgCtx->decodedTextLen;
}

// Whether an ocarina prompt lies ahead in this box, which the game's skip would jump past.
static s32 MessageAssist_OcarinaAhead(MessageContext* msgCtx) {
    u16 pos = msgCtx->textDrawPos;

    while (pos < msgCtx->decodedTextLen) {
        u8 c = MSG_BUF_DECODED[pos];

        if (c == MESSAGE_OCARINA) {
            return true;
        }
        pos += 1 + MessageAssist_ArgCount(c);
    }
    return false;
}

static void MessageAssist_Update(PlayState* play) {
    MessageContext* msgCtx = &play->msgCtx;
    Input* input = &play->state.input[0];
    const s32 mode = recomp_message_assist();
    const s32 speed = mode & 3;
    const s32 always = (mode & 4) != 0;
    const s32 pressedA = CHECK_BTN_ALL(input->press.button, BTN_A);
    const s32 pressedB = CHECK_BTN_ALL(input->press.button, BTN_B);

    if (mode == 0) {
        return;
    }
    if ((gSaveContext.language == LANGUAGE_JPN) || sTextIsCredits || (msgCtx->textBoxType >= TEXTBOX_TYPE_OCARINA)) {
        return;
    }

    if (msgCtx->msgMode == MSGMODE_TEXT_DISPLAYING) {
        if (always && !sTextboxSkipped) {
            // B: the game's own skip (Message_Update does exactly this where it allows it, earlier
            // in the same frame, which is why a skip already under way is left alone here).
            if (pressedB && !MessageAssist_OcarinaAhead(msgCtx)) {
                sTextboxSkipped = true;
                msgCtx->textDrawPos = msgCtx->decodedTextLen;
                return;
            }
            // A (or B before an ocarina prompt): the rest of this box, up to its next stop.
            if (pressedA || pressedB) {
                msgCtx->textDrawPos = MessageAssist_Advance(msgCtx, msgCtx->textDrawPos, 0x7FFF);
                msgCtx->textDelayTimer = 0;
                return;
            }
        }
        if ((speed != 0) && !sTextboxSkipped) {
            const s32 extra = (speed == 1) ? 1 : ((speed == 2) ? 3 : 0x7FFF);

            if (msgCtx->textDelayTimer != 0) {
                msgCtx->textDelayTimer = (msgCtx->textDelayTimer > extra) ? (msgCtx->textDelayTimer - extra) : 0;
            }
            if (msgCtx->textDelayTimer == 0) {
                msgCtx->textDrawPos = MessageAssist_Advance(msgCtx, msgCtx->textDrawPos, extra);
            }
        }
        return;
    }

    // A box waiting on a clock: A or B ends the wait, and Message_Update closes or continues the
    // box on the next frame exactly as if the clock had run out.
    if (always && (pressedA || pressedB) && (msgCtx->stateTimer > 1)) {
        if ((msgCtx->msgMode == MSGMODE_TEXT_DELAYED_BREAK) ||
            ((msgCtx->msgMode == MSGMODE_TEXT_DONE) && (msgCtx->textboxEndType == TEXTBOX_ENDTYPE_FADING))) {
            msgCtx->stateTimer = 1;
        }
    }
}

// @recomp The game's textbox drawing, with the text rows applied first (see above).
RECOMP_PATCH void Message_DrawTextBox(PlayState* play, Gfx** p) {
    MessageContext* msgCtx = &play->msgCtx;
    Gfx* gfx = *p;

    // @recomp
    MessageAssist_Update(play);

    gDPPipeSync(gfx++);
    gDPSetPrimColor(gfx++, 0, 0, msgCtx->textboxColorRed, msgCtx->textboxColorGreen, msgCtx->textboxColorBlue,
                    msgCtx->textboxColorAlphaCurrent);

    if (!msgCtx->textBoxType /* TEXTBOX_TYPE_BLACK */ || msgCtx->textBoxType == TEXTBOX_TYPE_BLUE) {
        gDPLoadTextureBlock_4b(gfx++, msgCtx->textboxSegment, G_IM_FMT_I, 128, 64, 0, G_TX_MIRROR, G_TX_NOMIRROR, 7, 0,
                               G_TX_NOLOD, G_TX_NOLOD);
    } else {
        if (msgCtx->textBoxType == TEXTBOX_TYPE_OCARINA) {
            gDPSetEnvColor(gfx++, 0, 0, 0, 255);
        } else {
            gDPSetEnvColor(gfx++, 50, 20, 0, 255);
        }

        gDPLoadTextureBlock_4b(gfx++, msgCtx->textboxSegment, G_IM_FMT_IA, 128, 64, 0, G_TX_MIRROR, G_TX_MIRROR, 7, 0,
                               G_TX_NOLOD, G_TX_NOLOD);
    }

    gSPTextureRectangle(gfx++, R_TEXTBOX_X << 2, R_TEXTBOX_Y << 2, (R_TEXTBOX_X + R_TEXTBOX_WIDTH) << 2,
                        (R_TEXTBOX_Y + R_TEXTBOX_HEIGHT) << 2, G_TX_RENDERTILE, 0, 0, R_TEXTBOX_TEXWIDTH << 1,
                        R_TEXTBOX_TEXHEIGHT << 1);

    // Draw treble clef
    if (msgCtx->textBoxType == TEXTBOX_TYPE_OCARINA) {
        gDPPipeSync(gfx++);
        gDPSetCombineLERP(gfx++, 1, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0, 1, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE,
                          0);
        gDPSetPrimColor(gfx++, 0, 0, 255, 100, 0, 255);
        gDPLoadTextureBlock_4b(gfx++, gOcarinaTrebleClefTex, G_IM_FMT_I, 16, 32, 0, G_TX_MIRROR, G_TX_MIRROR,
                               G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        gSPTextureRectangle(gfx++, R_TEXTBOX_CLEF_XPOS << 2, R_TEXTBOX_CLEF_YPOS << 2, (R_TEXTBOX_CLEF_XPOS + 16) << 2,
                            (R_TEXTBOX_CLEF_YPOS + 32) << 2, G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);
    }

    *p = gfx;
}

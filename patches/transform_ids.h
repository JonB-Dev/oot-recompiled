// The transform ID layout for this game. ONE FILE, every subsystem allocates from it.
//
// RT64 interpolates a transform group between two consecutive frames by its ID, so an ID has to be
// unique within a frame and stable across frames for as long as the thing it names exists. Two
// subsystems handing out the same ID would make the renderer interpolate between unrelated
// transforms, and nothing would crash: the picture would simply be wrong in a way that is hard to
// trace. The layout is written down here so that collision cannot happen by construction.
//
// Laid out fresh for Ocarina of Time. The reference project's numbers are a different game's; the
// SHAPE (fixed IDs low, ranges above, actors in a large block of their own) is what is borrowed.
// Every constant names the phase that consumes it, so an unused one is visible.

#ifndef TRANSFORM_IDS_H
#define TRANSFORM_IDS_H

// 0 is "no identity". A patch that gets 0 from the registry emits nothing, and the renderer draws
// the transform untagged, which is what it did before this plan.
#define TRANSFORM_ID_NONE                      0x0U

// Fixed IDs: things there is exactly one of.
#define CAMERA_TRANSFORM_ID                    0x10U   // phase 36, the projection
#define LETTERBOX_TRANSFORM_ID                 0x11U   // phase 43
#define TEXTBOX_TRANSFORM_ID                   0x12U   // phase 43
#define TITLE_CARD_TRANSFORM_ID                0x13U   // phase 43
#define PAUSE_CURSOR_TRANSFORM_ID              0x14U   // phase 45
#define PAUSE_PANEL_TRANSFORM_ID               0x15U   // phase 45
#define PAUSE_PAGES_TRANSFORM_ID               0x16U   // phase 45
#define SUN_MOON_TRANSFORM_ID                  0x17U   // phase 41, the sun
#define MOON_TRANSFORM_ID                      0x18U   // phase 41, the moon (its own matrix in the same draw)
#define RAIN_TRANSFORM_ID                      0x19U   // phase 42, the rain drops (held: placed at random every frame)
#define RAIN_RINGS_TRANSFORM_ID                0x1AU   // phase 42, the rain's rings on the ground (held, same reason)
#define PAUSE_VIEW_TRANSFORM_ID                0x1BU   // phase 45, the pause menu's view of its cube of pages
#define PAUSE_PANEL_VIEW_TRANSFORM_ID          0x1CU   // phase 45, the pause menu's view of its panel and cursor
#define LENS_FLARE_TRANSFORM_ID_START          0x20U   // phase 41, one per flare: the sun's, then the custom one
#define LENS_FLARE_TRANSFORM_ID_COUNT          0x10U
// Phase 45: the plan gave the pages one id (0x16, above, now unused); they are five matrices,
// the four pages of the cube and the prompt's page, so each has its own from here.
#define PAUSE_PAGE_TRANSFORM_ID_START          0x30U
#define PAUSE_PAGE_TRANSFORM_ID_COUNT          0x8U

// Ranges: things there are a bounded number of, indexed by the game's own table slot.
#define SKYBOX_TRANSFORM_ID_START              0x100U  // phase 41, one per skybox face
#define SKYBOX_TRANSFORM_ID_COUNT              0x10U
#define PARTICLE_TRANSFORM_ID_START            0x200U  // phase 40, EffectSs table, 100 entries in this game
#define PARTICLE_TRANSFORM_ID_COUNT            0x100U
#define EFFECT_TRANSFORM_ID_START              0x400U  // phase 40, Effect table (blure, spark, shield particle)
#define EFFECT_TRANSFORM_ID_COUNT              0x100U
// Inside the effect range: the three tables this game keeps (3 sparks, 25 trails, 3 shield
// sparks) in the lower half by their slot, and the same effect's translucent list group in
// the upper half, so a draw that writes to both lists never shares an id between them.
#define EFFECT_TRANSFORM_SPARK_OFFSET          0x00U
#define EFFECT_TRANSFORM_BLURE_OFFSET          0x10U
#define EFFECT_TRANSFORM_SHIELD_OFFSET         0x30U
#define EFFECT_TRANSFORM_XLU_OFFSET            0x80U
#define ITEM_TRANSFORM_ID_START                0x600U  // phase 42, collectibles that are not actors of their own
#define ITEM_TRANSFORM_ID_COUNT                0x100U

// Actors: a large block of their own, 512 IDs per spawn index: 256 for limbs and 256 for
// whatever an actor draws after its skeleton (a shadow, a held item, a post-draw pass). The
// spawn index comes from the native registry (phases 35, 37, 38, 42) and never repeats while the
// program runs, so a new actor can never inherit a dead one's transform.
#define ACTOR_TRANSFORM_LIMB_COUNT             256U
#define ACTOR_TRANSFORM_ID_COUNT               (ACTOR_TRANSFORM_LIMB_COUNT * 2U)
#define ACTOR_TRANSFORM_ID_START               0x1000000U

// Inside an actor's block (phases 37, 38). Limbs are numbered from 1 by the skeleton code, so
// slot 0 of each half is free: the limb half's slot 0 tags an actor that draws with a single
// matrix and no skeleton (the opaque list), the post-limb half's slot 0 the same actor's
// translucent list. A limb's own drawing goes at limb half + limb index, and whatever its
// post-limb callback draws (a held sword, a shield) at post-limb half + limb index.
#define ACTOR_TRANSFORM_WHOLE_OPA_OFFSET       0U
#define ACTOR_TRANSFORM_WHOLE_XLU_OFFSET       ACTOR_TRANSFORM_LIMB_COUNT
#define ACTOR_TRANSFORM_LIMB_OFFSET            0U
#define ACTOR_TRANSFORM_POST_LIMB_OFFSET       ACTOR_TRANSFORM_LIMB_COUNT

// Given a spawn index from the registry, the base ID for that actor, or TRANSFORM_ID_NONE.
#define ACTOR_TRANSFORM_ID(spawn_index) \
    ((spawn_index) == 0U ? TRANSFORM_ID_NONE : (ACTOR_TRANSFORM_ID_START + (spawn_index) * ACTOR_TRANSFORM_ID_COUNT))

#endif // TRANSFORM_IDS_H

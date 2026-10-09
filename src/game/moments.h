// A saved moment: the file a slot holds, byte for byte (phase 74, .scaffold/states/states-design.md),
// and the native side's surface for capturing and reading one (phase 75).
//
// A moment is not a machine snapshot (the design says why: the game's threads are native and
// cannot be thawed in a later process). It is the game's own save block plus the handful of
// context fields that say where the player is and what the scene is doing, plus a record of
// Link's spot and a header of ours. This header fixes the LAYOUT: no decomp header is included
// here, because src/ never includes one (CLAUDE.md, the two compilation worlds); the sizes that
// come from the game are constants, and the patch side asserts them against the real structs
// (`_Static_assert(sizeof(Save) == 0x1354)` in patches/harness_warp.c), so a decomp bump that
// moved a field fails the patch build rather than corrupting a file.
//
// Every multi byte field is little endian, as the machine writes it. Every offset below is
// asserted, so the table in the design and this struct cannot drift apart silently.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace oot::moments {

    // The game's `Save` struct, as `Sram_WriteSave` writes it: everything up to `fileNum` in the
    // `SaveContext` of the pinned decompilation (`upstream/oot/include/save.h`, ntsc-1.0).
    constexpr uint32_t SAVE_SIZE = 0x1354;

    // One `RespawnData` (pos, yaw, playerParams, entranceIndex, roomIndex, data, two flag words).
    constexpr uint32_t RESPAWN_SIZE = 0x1C;
    constexpr uint32_t RESPAWN_COUNT = 3;   // down, return, top

    // The context fields a moment carries besides the save block, each widened to 32 bits where
    // the game keeps fewer, in this order: sceneLayer, respawnFlag, respawn[3], nextDayTime,
    // nextTransitionType, nextCutsceneIndex, magicState, magicCapacity.
    constexpr uint32_t CONTEXT_SIZE = 4 + 4 + RESPAWN_SIZE * RESPAWN_COUNT + 4 + 4 + 4 + 4 + 4;
    static_assert(CONTEXT_SIZE == 0x70, "the context block is 112 bytes by the design");

    constexpr char MAGIC[8] = { 'O', 'o', 'T', 'R', 'M', 'O', 'M', '1' };
    constexpr uint32_t LAYOUT_VERSION = 1;

    // The header, exactly the table in states-design.md section 4.
#pragma pack(push, 1)
    struct Header {
        char     magic[8];          // 0x0000  "OoTRMOM1"
        uint32_t layout_version;    // 0x0008  1
        uint32_t total_size;        // 0x000C  must equal the file's size
        char     program_version[16]; // 0x0010  "0.2.2", zero padded
        uint64_t captured_at;       // 0x0020  seconds since 1970
        uint32_t entrance;          // 0x0028  the game's entranceIndex as saved
        int32_t  scene;             // 0x002C
        int32_t  room;              // 0x0030
        float    position[3];       // 0x0034  Link's world position
        int32_t  yaw;               // 0x0040  Link's facing, the game's s16 widened
        int32_t  link_age;          // 0x0044
        uint32_t day_time;          // 0x0048  the game's u16 widened
        uint32_t flags;             // 0x004C  bit 0 thumbnail present, bit 1 autosave
        char     name[64];          // 0x0050  UTF-8, zero terminated within the field
        uint32_t save_size;         // 0x0090  must equal SAVE_SIZE
        uint32_t context_size;      // 0x0094  must equal CONTEXT_SIZE
        uint64_t reserved;          // 0x0098  zero
    };                              // 0x00A0
#pragma pack(pop)

    static_assert(offsetof(Header, layout_version) == 0x08, "layout");
    static_assert(offsetof(Header, total_size) == 0x0C, "layout");
    static_assert(offsetof(Header, program_version) == 0x10, "layout");
    static_assert(offsetof(Header, captured_at) == 0x20, "layout");
    static_assert(offsetof(Header, entrance) == 0x28, "layout");
    static_assert(offsetof(Header, scene) == 0x2C, "layout");
    static_assert(offsetof(Header, room) == 0x30, "layout");
    static_assert(offsetof(Header, position) == 0x34, "layout");
    static_assert(offsetof(Header, yaw) == 0x40, "layout");
    static_assert(offsetof(Header, link_age) == 0x44, "layout");
    static_assert(offsetof(Header, day_time) == 0x48, "layout");
    static_assert(offsetof(Header, flags) == 0x4C, "layout");
    static_assert(offsetof(Header, name) == 0x50, "layout");
    static_assert(offsetof(Header, save_size) == 0x90, "layout");
    static_assert(offsetof(Header, context_size) == 0x94, "layout");
    static_assert(offsetof(Header, reserved) == 0x98, "layout");
    static_assert(sizeof(Header) == 0xA0, "the header is 160 bytes by the design");

    constexpr uint32_t FLAG_THUMBNAIL = 1u << 0;
    constexpr uint32_t FLAG_AUTOSAVE  = 1u << 1;

    // After the header: the save block, the context block, then a CRC32 of everything before it.
    constexpr uint32_t CRC_SIZE = 4;
    constexpr uint32_t FILE_SIZE = sizeof(Header) + SAVE_SIZE + CONTEXT_SIZE + CRC_SIZE;
    static_assert(FILE_SIZE == 0xA0 + 0x1354 + 0x70 + 4, "the file is one fixed size");

    // Slots: eight the player picks, and FIVE the autosave owns (the user, 2026-09-25:
    // "autosaves must keep the most recent 5 saves and then have a divider and then have the 8
    // user created save slots"). The file names are "1" to "8" for the player's, and "auto",
    // "auto2" to "auto5" for the autosave's, under saves\moments\ in the data folder.
    //
    // THE NUMBERING KEEPS SLOT 0 MEANING WHAT IT ALWAYS DID, which is where the next autosave is
    // written, so every path that asks for AUTOSAVE_SLOT is unchanged and the file it has always
    // written keeps its name. The four older ones are NEGATIVE, oldest last, and they are filled
    // by rotation: before each autosave the oldest is dropped, each survivor moves one step back,
    // and the new one is written to slot 0. A slot is an autosave when it is not positive.
    constexpr int SLOT_COUNT = 8;
    constexpr int AUTOSAVE_SLOT = 0;     // where the next autosave goes, and so the newest of them
    constexpr int AUTOSAVE_KEPT = 5;     // how many autosaves are kept at once
    constexpr int AUTOSAVE_OLDEST = AUTOSAVE_SLOT - (AUTOSAVE_KEPT - 1);   // -4, the one dropped next

    constexpr bool is_autosave(int slot) {
        return slot <= AUTOSAVE_SLOT;
    }

    constexpr bool slot_exists(int slot) {
        return (slot >= AUTOSAVE_OLDEST) && (slot <= SLOT_COUNT);
    }

    // The file a slot's moment, or its thumbnail, lives in: ".moment", ".rgba" or ".png" after the
    // slot's name. THE ONE PLACE THE NAMES ARE DECIDED. The Moments document once built them
    // itself and named the older autosaves "-1" to "-4", so the four the rotation kept were on
    // disk and shown as Empty (the user, 2026-10-09: "I cannot revert back to a prior auto-save").
    std::filesystem::path slot_file(int slot, const char* extension);

    // The thumbnail beside a slot: "<slot>.png", this wide, the height following the frame.
    constexpr int THUMBNAIL_WIDTH = 320;

    // The place record the patch fills at capture: where Link stands and what the scene is.
    // Mirrors `MomentPlace` in patches/harness_warp.c word for word (ten 32 bit words).
    struct Place {
        float position[3];
        int32_t yaw;
        int32_t room;
        int32_t scene;
        int32_t entrance;
        int32_t link_age;
        uint32_t day_time;
        uint32_t reserved;
    };
    static_assert(sizeof(Place) == 40, "the place record is ten words");

    // ---- the request (phase 75) ----------------------------------------------------------------

    // Ask for a capture into `slot` (0 the autosave, 1 to 8 the player's). The patch answers on
    // the next update it judges safe; the request lapses after `lifetime_ticks` game updates
    // (twenty a second) if no safe moment comes, with the last reason logged. `name` is the
    // slot's name, or empty for the default (the scene and the time).
    // `delay_ticks` holds the request that many game updates after play is first seen, so a
    // harness can walk before the capture; 0 for the surface and the quick keys.
    void request_capture(int slot, uint32_t lifetime_ticks, const std::string& name, uint32_t delay_ticks = 0);

    // For the patch, once a frame: the slot to capture into, or -1. Held while the game is not in
    // ordinary play, the way a pending warp is held.
    int take_pending_capture();

    // For the patch: the moment was not safe this frame, for `reason` (1 to 8; see the patch).
    void note_refused(int slot, int reason);

    // For the patch, through the export: the blocks as read out of RDRAM, one 32 bit word each.
    void on_captured(int slot, const std::vector<uint32_t>& save_words,
                     const std::vector<uint32_t>& context_words, const Place& place);

    // ---- the quick keys and the autosave (phases 77 and 78) -------------------------------------

    // Quick save: the FIRST EMPTY slot of the eight, never over one that holds a moment (the
    // user, 2026-09-23: "adds a new state and never overwrites"). Refuses with a line when all
    // eight are full.
    void quick_save();

    // Quick load: the NEWEST moment by its capture time, whichever slot holds it.
    void quick_load();

    // ---- saying so on screen --------------------------------------------------------------------

    // A QUICK SAVE IS OTHERWISE INVISIBLE (the user, 2026-09-27: "when using the assigned Quick
    // Save button, or quick resume button, can we make it show a small overlay, saying State
    // saved. And State restored"). Pressing the key does nothing a player can see: the file
    // appears on disk and the game carries on, so the only way to know it worked was to open the
    // Moments list. These two leave a line for the interface to show.
    //
    // It is a LINE RATHER THAN A CALL INTO THE INTERFACE, so this module keeps knowing nothing
    // about RmlUi or the shell: the shell already looks in once a frame for its own toast, and it
    // takes this at the same time. `take_notice` hands over what is waiting and clears it, so a
    // line is shown once and never twice.
    void notice(const char* text);
    bool take_notice(std::string& out);
    // Resume on start, asked by the console logo before its first frame (phase 78b, 2026-09-24):
    // when the row is on and a moment exists that knows its file, stages the newest and returns
    // its slot for the patch to write into the game and hand to Play_Init directly; -1 otherwise.
    // Once per launch.
    int take_boot_load();

    // The harness's override of the autosave interval, in game updates; 0 leaves the row in charge.
    void set_autosave_override(uint32_t ticks);

    // ---- save on quit, resume on start (phase 78b) ---------------------------------------------

    // The program was asked to quit (the menu's Quit, the window's close). With Save on quit on and
    // the game in play, this keeps a moment in the autosave slot first and returns true: the caller
    // must NOT quit yet, but call quit_ready() every frame and quit when it says so (the capture
    // landed, was refused, or a few seconds passed). False: quit now, nothing to wait for.
    bool on_quit_requested(bool game_started);
    bool quit_ready();
    // True while a quit is waiting on its capture.
    bool quit_pending();

    // ---- the resume (phase 76) -----------------------------------------------------------------

    // Read, validate and stage a slot for the game to take on its next safe frame. False, with the
    // reason logged as MOMENT_REFUSED, when the file is missing or damaged; the game is untouched.
    bool request_load(int slot);

    // For the patch, once a frame: the slot staged for a resume, or -1. Held outside play.
    int take_pending_load();

    // For the export: hand over the staged words and place, once. False when nothing is staged.
    bool take_staged(std::vector<uint32_t>& save_words, std::vector<uint32_t>& context_words, Place& place);

    // For the export: the words were written into the game.
    void note_resumed();

    // ---- the file ------------------------------------------------------------------------------

    // Read and validate a slot file. False with `why` on any fault; the game is not touched.
    bool read_slot(const std::filesystem::path& file, Header& header, std::vector<uint32_t>& save_words,
                   std::vector<uint32_t>& context_words, std::string& why);

    // `--moment-dump <file>`: print the header, return 0, or print the refusal and return 1.
    int dump(const std::filesystem::path& file);

} // namespace oot::moments

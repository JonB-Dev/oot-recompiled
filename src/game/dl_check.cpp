#include "game/dl_check.h"

#include <cstdio>

namespace {

    // Whether the verbose reporting is on. The GUARD itself always runs: a crash does
    // not wait for diagnostics to be enabled. Declared here rather than beside the
    // public functions because the walker needs to see it.
    bool g_on = false;

    // The runtime maps emulated 0x80000000 at rdram + 0, and the region above what it maps is left
    // unmapped ON PURPOSE to catch bad addresses. Anything at or past this offset is the fault
    // waiting to happen.
    constexpr uint32_t MAPPED_BYTES = 0x20000000;

    // F3DEX2, which is the microcode this game uses. Only the opcodes that carry an address are
    // listed, because only those can be wrong in the way we are hunting.
    constexpr uint8_t OP_VTX      = 0x01;
    constexpr uint8_t OP_BRANCH_Z = 0x04;
    constexpr uint8_t OP_MTX      = 0xDA;
    constexpr uint8_t OP_MOVEMEM  = 0xDC;
    constexpr uint8_t OP_DL       = 0xDE;
    constexpr uint8_t OP_ENDDL    = 0xDF;
    constexpr uint8_t OP_SETTIMG  = 0xFD;
    constexpr uint8_t OP_SETZIMG  = 0xFE;
    constexpr uint8_t OP_SETCIMG  = 0xFF;
    constexpr uint8_t OP_MOVEWORD = 0xDB;

    // RT64's extended GBI, which the post-parity patches emit. The opcode is the header's
    // default, RT64_EXTENDED_OPCODE, and the game enables it every frame (patches/frame_setup.c).
    constexpr uint8_t OP_RT64_EXTENDED = 0x64;

    // G_MOVEWORD's index for a segment base. The segment number is the command's offset over four.
    constexpr uint8_t MW_SEGMENT = 0x06;

    // One table per walk. 16 segments, and `set` matters as much as the value: a base of zero that
    // was never written is the bug, and a base of zero that the game wrote deliberately (segment 0
    // is physical addressing) is not.
    struct Segments {
        uint32_t base[16] = {};
        bool set[16] = {};
    };

    const char* opcode_name(uint8_t op) {
        switch (op) {
            case OP_VTX:      return "G_VTX, a vertex buffer";
            case OP_BRANCH_Z: return "G_BRANCH_Z, a conditional branch";
            case OP_MTX:      return "G_MTX, a matrix";
            case OP_MOVEMEM:  return "G_MOVEMEM, lights or a viewport";
            case OP_DL:       return "G_DL, a nested display list";
            case OP_SETTIMG:  return "G_SETTIMG, a texture image";
            case OP_SETZIMG:  return "G_SETZIMG, the depth buffer";
            case OP_SETCIMG:  return "G_SETCIMG, the color buffer";
            default:          return "an opcode that does not carry an address";
        }
    }

    bool carries_address(uint8_t op) {
        return op == OP_VTX || op == OP_BRANCH_Z || op == OP_MTX || op == OP_MOVEMEM ||
               op == OP_DL || op == OP_SETTIMG || op == OP_SETZIMG || op == OP_SETCIMG;
    }

    uint32_t read_word(const uint8_t* rdram, uint32_t physical) {
        return *reinterpret_cast<const uint32_t*>(rdram + physical);
    }

    // Replace a command with a no-op, in place. Two zero words are G_NOOP in F3DEX2, so the
    // renderer steps straight over it and the rest of the list draws normally.
    void neutralize(uint8_t* rdram, uint32_t physical) {
        *reinterpret_cast<uint32_t*>(rdram + physical) = 0;
        *reinterpret_cast<uint32_t*>(rdram + physical + 4) = 0;
    }

    // A segmented address has the segment in its top byte, 0 to 15.
    bool is_segmented(uint32_t address) {
        return (address >> 24) <= 0x0F;
    }

    uint32_t segment_of(uint32_t address) {
        return (address >> 24) & 0x0F;
    }

    // Where a direct address lands in the memory the runtime maps. KSEG0 and KSEG1 are two views
    // of the same physical memory on the console, which is exactly why this bug is harmless there
    // and fatal here: the runtime maps the cached view only.
    uint32_t to_offset(uint32_t address) {
        return address & 0x1FFFFFFF;
    }

    // The console had 8MB with the Expansion Pak and 4MB without. Our runtime maps 512MB, which
    // is what let this bug hide: a wild address lands in memory that is mapped and readable, so
    // every "is this address valid" check passes, and the fault only arrives 260MB later when the
    // renderer has walked all the way off the end.
    //
    // So the question is not whether an address is MAPPED, it is whether it is PLAUSIBLE. And
    // plausible is wider than the console since 2026-09-23: the frame's own display lists are
    // built in pools that live in the runtime's patch RAM, above the console's 8 MB and below
    // where mods begin (librecomp addresses.hpp: patch_rdram_start 0x80801000, mod_rdram_start
    // 0x81000000). This was CONSOLE_RAM at 8 MB until then, and with the pools moved it flagged
    // every frame's own lists as bad, two per frame, on the run that then crashed for a different
    // reason (the renderer had not been told about the extra memory; see frame_setup.c).
    constexpr uint32_t PLAUSIBLE_RAM = 0x01000000;

    bool out_of_range(uint32_t address) {
        return to_offset(address) >= MAPPED_BYTES || address >= 0xA0000000;
    }

    // A display list address that no real frame could contain: past the console's memory, or not
    // on an 8 byte boundary. Commands are 8 bytes, so an unaligned target is not a display list
    // at all, and reading one produces nonsense opcodes that never happen to be G_ENDDL.
    bool implausible_list(uint32_t address) {
        const uint32_t offset = to_offset(address);
        return offset >= PLAUSIBLE_RAM || (offset & 7) != 0;
    }

    // Segment 0 is physical addressing, so a base of zero there is not in itself a complaint.
    // Any other segment used before its base was set resolves to zero, and that is the fault.
    bool unset_segment(const Segments& segs, uint32_t address) {
        if (!is_segmented(address)) {
            return false;
        }
        const uint32_t s = segment_of(address);
        if (s == 0) {
            return false;
        }
        return !segs.set[s] || segs.base[s] == 0;
    }

    // A NULL IMAGE ADDRESS, which the first version could not see.
    //
    // An address of literally zero has a top byte of 0x00, so `is_segmented` calls it segment 0,
    // and segment 0 was exempted as legitimate physical addressing. That exemption is right in
    // general and wrong here: a null is exactly what we are hunting, and for the commands that set
    // an image there is no reading of zero that is meant. The renderer turns it into 0xA0000000,
    // which is physical zero in uncached space, and follows it off the end of mapped memory.
    bool null_image(uint8_t op, uint32_t address) {
        if (address != 0) {
            return false;
        }
        return op == OP_SETCIMG || op == OP_SETTIMG || op == OP_SETZIMG ||
               op == OP_DL || op == OP_VTX;
    }

    uint32_t resolve(const Segments& segs, uint32_t address) {
        if (!is_segmented(address)) {
            return to_offset(address);
        }
        const uint32_t s = segment_of(address);
        return to_offset(segs.base[s]) + (address & 0x00FFFFFF);
    }

    // Why the walk stops where it does. COUNTED rather than reasoned about: three versions of this
    // checker in a row found nothing, and from outside every one of them looked identical to a
    // checker that had looked everywhere and found the list clean.
    struct Coverage {
        int max_depth = 0;
        int dl_followed = 0;
        int dl_skipped_unset_segment = 0;
        int dl_skipped_out_of_range = 0;
        int enddl = 0;
    };

    Coverage g_cov;

    // The chain of lists that led here, one start address per depth. Knowing that a bad command
    // sits nine levels down says nothing about what drew it; knowing the nine lists above it says
    // which buffer each level came from, and the top of that chain is the frame's own structure.
    constexpr int MAX_PATH = 72;
    uint32_t g_group_pops = 0;      // matrix group pops seen, against the pushes the tally counts
    uint32_t g_path[MAX_PATH] = {};

    // The raw word the parent G_DL carried to get into the current list, per depth. A
    // segmented value here says the game asked for 'this segment plus an offset', which is
    // what identifies whether the wrong object is in that segment.
    uint32_t g_entry[MAX_PATH] = {};

    int walk(uint8_t* rdram, uint32_t physical, int depth, int& commands, Segments& segs) {
        // Kept so a list that never ends can name where it began. The address is what identifies
        // which buffer it is; the depth alone says nothing about where to look.
        const uint32_t list_start = physical;
        if (depth >= 0 && depth < MAX_PATH) {
            g_path[depth] = list_start;
        }

        if (depth > g_cov.max_depth) {
            g_cov.max_depth = depth;
        }
        // 8 WAS TOO SHALLOW, and that is very likely why the first versions found nothing. This
        // game nests display lists through the scene, the room, each actor, its skeleton and then
        // every limb of it, which is comfortably past eight before anything unusual happens. A
        // walk that stops at eight silently covers the frame's chrome and none of its contents.
        // 64 is still far below anything that could be a genuine loop.
        if (depth > 64) {
            return 0;
        }

        int found = 0;
        for (int i = 0; i < 200000; ++i) {
            // RUNNING OFF THE END IS THE BUG, NOT A NORMAL ENDING.
            //
            // Every earlier version returned quietly here and counted the walk as clean. That is
            // the precise condition that kills the renderer: a list with no G_ENDDL is walked
            // forward until the pointer leaves mapped memory, and the first address past the end
            // is rdram + 0x20000000, which is exactly the address every one of these crashes
            // reads. The checker was silently passing the only thing it needed to catch.
            if (physical + 8 > MAPPED_BYTES) {
                ++found;
                std::fprintf(stderr,
                             "[dl] LIST RAN OFF THE END OF MEMORY at 0x%08X, depth %d, after %d"
                             " commands, with no G_ENDDL. The renderer walks past the top of"
                             " emulated memory here.\n",
                             physical, depth, i);
                return found;
            }

            if (i == 199999) {
                std::fprintf(stderr,
                             "[dl] UNTERMINATED LIST: started at 0x%06X, depth %d, still going"
                             " after %d commands with no G_ENDDL. The renderer has no such limit,"
                             " so it walks this one off the end of memory.\n",
                             list_start, depth, i + 1);
            }

            const uint32_t word0 = read_word(rdram, physical);
            const uint32_t word1 = read_word(rdram, physical + 4);
            const uint8_t op = static_cast<uint8_t>(word0 >> 24);
            ++commands;

            if (op == OP_ENDDL) {
                ++g_cov.enddl;
                return found;
            }

            // RT64'S EXTENDED COMMANDS ARE NOT ALL ONE WORD, and the ones that are not fooled
            // this walker the first time a patch emitted one. A matrix group is two 8 byte
            // words, and the second word is a packed field of interpolation flags whose top
            // byte happened to read as G_VTX with a null address: two thousand false reports
            // per minute, none of them a defect. Extended commands carry no address this walker
            // follows, so the right treatment is to step over every word of them. The sub
            // opcode sits in the low 24 bits of the first word; the lengths are from the
            // vendored header's G_EX_COMMAND2 and G_EX_COMMAND3 users.
            if (op == OP_RT64_EXTENDED) {
                const uint32_t sub = word0 & 0x00FFFFFF;
                int extra_words = 0;

                // The one extended command whose VALUE decides whether the renderer generates
                // frames between the game's: the game's own rate, which must be below the
                // display's. Printed when it changes, so a wrong rate is one line in the trace
                // rather than a silent "the picture will not smooth".
                if (sub == 0x09 && g_on) {
                    static uint32_t last_rate = UINT32_MAX;
                    static uint32_t seen = 0;
                    static uint32_t seen_at_20 = 0;
                    static uint32_t seen_at_60 = 0;
                    const uint32_t rate = word1 & 0xFFFF;
                    ++seen;
                    if (rate == 20) ++seen_at_20;
                    if (rate == 60) ++seen_at_60;
                    if (rate != last_rate) {
                        last_rate = rate;
                        std::fprintf(stderr, "[dl] refresh rate command = %u\n", rate);
                    }
                    // A running tally as well as the change line, because a value that flips
                    // every other frame prints two change lines and then looks steady.
                    if (seen % 600 == 0) {
                        std::fprintf(stderr, "[dl] refresh rate commands seen %u: %u at 20, %u at 60\n",
                                     seen, seen_at_20, seen_at_60);
                    }
                }

                // The camera's group (phase 36) decides each frame whether the camera moved
                // smoothly or cut, and says so in the flags word of its second command word:
                // every component interpolated, or every component skipped. Tallied so a pan
                // that the cut heuristic keeps calling a cut shows up as a number rather than
                // as "the picture will not smooth".
                if (sub == 0x0C && g_on && word1 == 0x10) {
                    static uint32_t cam_seen = 0;
                    static uint32_t cam_interpolated = 0;
                    static uint32_t cam_skipped = 0;
                    const uint32_t flags = read_word(rdram, physical + 8);
                    ++cam_seen;
                    // Bit 3 is the position component; interpolated is 1, skipped is 0.
                    if ((flags >> 3) & 1) ++cam_interpolated; else ++cam_skipped;
                    if (cam_seen % 600 == 0) {
                        std::fprintf(stderr, "[dl] camera groups seen %u: %u interpolated, %u skipped\n",
                                     cam_seen, cam_interpolated, cam_skipped);
                    }
                }

                // The actors' groups (phases 37 onward): IDs from 0x1000000 up, 512 per spawn
                // index, the low 256 for limbs and a single-matrix actor's opaque draw, the
                // high 256 for what a limb's post-draw callback draws and the translucent
                // draw. Tallied so "is the tag being emitted at all, and with a real identity"
                // is a number in the trace before any picture is measured; a registry that
                // answered 0 would leave every skeleton untagged and this line at zero.
                if (sub == 0x0C && g_on && word1 >= 0x1000000u) {
                    static uint32_t actor_seen = 0;
                    static uint32_t whole_opaque = 0;
                    static uint32_t limbs = 0;
                    static uint32_t whole_translucent = 0;
                    static uint32_t post_limbs = 0;
                    static uint32_t actor_skipped = 0;
                    const uint32_t flags = read_word(rdram, physical + 8);
                    const uint32_t slot = (word1 - 0x1000000u) & 0x1FFu;
                    ++actor_seen;
                    if (slot == 0) ++whole_opaque;
                    else if (slot < 256) ++limbs;
                    else if (slot == 256) ++whole_translucent;
                    else ++post_limbs;
                    if (((flags >> 3) & 1) == 0) ++actor_skipped;
                    // The first few in full, so a run with none at all is told apart from a run
                    // with fewer than the tally's interval.
                    if (actor_seen <= 3) {
                        std::fprintf(stderr, "[dl] actor group id 0x%08X (spawn index %u, slot %u) flags 0x%08X\n",
                                     word1, (word1 - 0x1000000u) >> 9, slot, flags);
                    }
                    if (actor_seen % 5000 == 0) {
                        std::fprintf(stderr, "[dl] actor groups seen %u: %u whole opaque, %u limb, %u whole translucent, %u post-limb, %u with position skipped; %u pops so far\n",
                                     actor_seen, whole_opaque, limbs, whole_translucent, post_limbs, actor_skipped, g_group_pops);
                    }
                }
                // The fixed groups (phase 41, and the 2D phases after it): the sun 0x17, the moon
                // 0x18, the rain 0x19 and its rings 0x1A (phase 42, held), the flares 0x20 up,
                // the pause menu's two views 0x1B and 0x1C and its pages 0x30 to 0x37 (phase 45),
                // the skybox 0x100 up, and the letterbox, text box and title card in between.
                // Tallied by id so "the sky's group is there every frame and the sun's when it
                // is up" is a line in the trace.
                if (sub == 0x0C && g_on && word1 > 0x10u && word1 < 0x200u) {
                    static uint32_t fixed_seen = 0;
                    static uint32_t fixed_sky = 0;
                    static uint32_t fixed_sun = 0;
                    static uint32_t fixed_moon = 0;
                    static uint32_t fixed_rain = 0;
                    static uint32_t fixed_flare = 0;
                    static uint32_t fixed_pause = 0;
                    static uint32_t fixed_other = 0;
                    static uint32_t fixed_skipped = 0;
                    const uint32_t flags = read_word(rdram, physical + 8);
                    ++fixed_seen;
                    if (word1 >= 0x100u) ++fixed_sky;
                    else if (word1 == 0x17u) ++fixed_sun;
                    else if (word1 == 0x18u) ++fixed_moon;
                    else if (word1 == 0x19u || word1 == 0x1Au) ++fixed_rain;
                    else if (word1 == 0x1Bu || word1 == 0x1Cu || (word1 >= 0x30u && word1 < 0x38u)) ++fixed_pause;
                    else if (word1 >= 0x20u && word1 < 0x30u) ++fixed_flare;
                    else ++fixed_other;
                    if (((flags >> 3) & 1) == 0) ++fixed_skipped;
                    if (fixed_seen <= 3 || fixed_seen % 2000 == 0) {
                        std::fprintf(stderr, "[dl] fixed groups seen %u: %u skybox, %u sun, %u moon, %u rain, %u flare, %u pause, %u other, %u with position skipped; last id 0x%03X flags 0x%08X\n",
                                     fixed_seen, fixed_sky, fixed_sun, fixed_moon, fixed_rain, fixed_flare, fixed_pause, fixed_other, fixed_skipped, word1, flags);
                    }
                }

                // The item groups (phase 42): 0x600 to 0x6FF, the item the player holds up in
                // the get-item pose, by its draw id. Rare by nature (one item, held for a few
                // seconds), so every one is printed rather than tallied at an interval.
                if (sub == 0x0C && g_on && word1 >= 0x600u && word1 < 0x700u) {
                    static uint32_t item_seen = 0;
                    const uint32_t flags = read_word(rdram, physical + 8);
                    ++item_seen;
                    if (item_seen <= 3 || item_seen % 200 == 0) {
                        std::fprintf(stderr, "[dl] item group id 0x%03X flags 0x%08X (%u so far)\n", word1, flags, item_seen);
                    }
                }

                // The particle and effect groups (phase 40): 0x200 to 0x2FF a soft sprite by
                // its table slot, 0x400 to 0x4FF a vertex effect by its slot. Tallied with the
                // skipped ones apart, because a slot's first draw after a reset is skipped by
                // design and the count of those says the reset mark is being set and cleared.
                if (sub == 0x0C && g_on && word1 >= 0x200u && word1 < 0x500u) {
                    static uint32_t fx_seen = 0;
                    static uint32_t fx_particles = 0;
                    static uint32_t fx_effects = 0;
                    static uint32_t fx_skipped = 0;
                    const uint32_t flags = read_word(rdram, physical + 8);
                    ++fx_seen;
                    if (word1 < 0x300u) ++fx_particles; else ++fx_effects;
                    if (((flags >> 3) & 1) == 0) ++fx_skipped;
                    if (fx_seen <= 3 || fx_seen % 5000 == 0) {
                        std::fprintf(stderr, "[dl] effect groups seen %u: %u particle, %u vertex effect, %u with position skipped; last id 0x%03X flags 0x%08X\n",
                                     fx_seen, fx_particles, fx_effects, fx_skipped, word1, flags);
                    }
                }

                // Every group pushed must be popped in the same list, or the renderer's group
                // stack carries the imbalance into whatever is drawn next. Counted beside the
                // pushes so the two can be compared in the trace.
                if (sub == 0x0D && g_on) {
                    ++g_group_pops;
                }

                // A group edit by address (phase 39): written at the end of a frame on which the
                // camera cut, one per billboard matrix, holding its rotation. The first few in
                // full and a tally, so "the edits are emitted on cut frames and only then" is
                // readable from the trace beside the `[cam]` verdicts.
                if (sub == 0x13 && g_on) {
                    static uint32_t edits_seen = 0;
                    const uint32_t flags = read_word(rdram, physical + 8);
                    ++edits_seen;
                    if (edits_seen <= 3 || edits_seen % 2000 == 0) {
                        std::fprintf(stderr, "[dl] group edit by address 0x%08X flags 0x%08X (%u so far)\n",
                                     word1, flags, edits_seen);
                    }
                }
                switch (sub) {
                    case 0x02:   // texture rectangle
                    case 0x08:   // scissor align
                        extra_words = 2;
                        break;
                    case 0x04:   // viewport
                    case 0x05:   // scissor
                    case 0x06:   // rect align
                    case 0x07:   // viewport align
                    case 0x0C:   // matrix group
                    case 0x13:   // edit group by address
                    case 0x14:   // vertex
                    case 0x30:   // matrix float
                    case 0x31:   // vertex segment
                        extra_words = 1;
                        break;
                    default:
                        break;
                }
                physical += 8 + 8 * extra_words;
                commands += extra_words;
                continue;
            }

            // Track the segment table as the list sets it. This is the part the first version
            // was missing, and it was missing the bug with it.
            if (op == OP_MOVEWORD && ((word0 >> 16) & 0xFF) == MW_SEGMENT) {
                const uint32_t seg = ((word0 & 0xFFFF) / 4) & 0x0F;
                segs.base[seg] = word1;
                segs.set[seg] = true;

                // CHECK THE BASE ITSELF, which the earlier versions recorded and never looked at.
                // A segment whose base is uncached or out of range sends EVERYTHING addressed
                // through it into unmapped memory, and none of the per address checks would see
                // that: each individual address looks like an ordinary small segmented value.
                // Naming the segment set is also far more useful than naming the hundredth
                // address that used it.
                if (word1 != 0 && out_of_range(word1)) {
                    ++found;
                    std::fprintf(stderr,
                                 "[dl] BAD SEGMENT BASE: segment %u set to 0x%08X at offset 0x%06X,"
                                 " depth %d\n",
                                 seg, word1, physical, depth);
                    std::fprintf(stderr,
                                 "[dl]   everything addressed through segment %u from here on"
                                 " resolves outside mapped memory\n", seg);
                }
            }

            if (carries_address(op)) {
                const bool unset = unset_segment(segs, word1);
                const bool wild = !is_segmented(word1) && out_of_range(word1);
                const bool null_addr = null_image(op, word1);
                if (unset || wild || null_addr) {
                    ++found;
                    std::fprintf(stderr,
                                 "[dl] BAD ADDRESS 0x%08X at list offset 0x%06X, depth %d, from %s\n",
                                 word1, physical, depth, opcode_name(op));
                    if (null_addr) {
                        std::fprintf(stderr,
                                     "[dl]   a null address. The renderer reads it as uncached"
                                     " physical zero, 0xA0000000, which is off the end of what the"
                                     " runtime maps.\n");
                    }
                    if (unset) {
                        std::fprintf(stderr,
                                     "[dl]   segment %u was never given a base in this list, so it"
                                     " resolves to zero\n",
                                     segment_of(word1));
                    }
                    std::fprintf(stderr, "[dl]   command words 0x%08X 0x%08X\n", word0, word1);
                }
            }

            // Follow a nested list. The low bit of the parameter byte distinguishes a call from a
            // branch in F3DEX2; both are worth following, and a branch does not return.
            if (op == OP_DL && unset_segment(segs, word1)) {
                ++g_cov.dl_skipped_unset_segment;
            }
            if (op == OP_DL && !unset_segment(segs, word1)) {
                const uint32_t target = resolve(segs, word1);

                // Report the PARENT that carries a bad target, not the child that then runs
                // forever. The child is the symptom; this command is the defect, and its address
                // is what leads back to the game code that wrote it.
                if (implausible_list(target)) {
                    ++found;

                    // NEUTRALIZE IT. Two zero words are a no-op, so the renderer steps over this
                    // command and draws the rest of the frame instead of walking off the end of
                    // memory. One object goes undrawn for one frame; the alternative is the
                    // program dying.
                    neutralize(rdram, physical);

                    if (!g_on) {
                        physical += 8;
                        continue;
                    }

                    std::fprintf(stderr,
                                 "[dl] IMPLAUSIBLE NESTED LIST: G_DL at 0x%06X (depth %d) points at"
                                 " 0x%08X, which resolves to 0x%06X.\n",
                                 physical, depth, word1, target);
                    std::fprintf(stderr,
                                 "[dl]   %s%s. The renderer follows it and never finds an end.\n",
                                 (to_offset(target) >= PLAUSIBLE_RAM)
                                     ? "that is past the console's 8MB of memory" : "",
                                 ((target & 7) != 0)
                                     ? ", and it is not on an 8 byte boundary so it is not a"
                                       " display list at all" : "");

                    // The chain that led here. The top of it is the frame's own structure, and
                    // each step down says which buffer that level was drawn from.
                    std::fprintf(stderr, "[dl]   chain:");
                    for (int d = 0; d <= depth && d < MAX_PATH; ++d) {
                        std::fprintf(stderr, " 0x%06X(via %08X)", g_path[d], g_entry[d]);
                    }
                    std::fprintf(stderr, "\n");

                    // THE GAME'S OWN SEGMENT TABLE, gSegments at 0x80120C38.
                    //
                    // It is supposed to hold PHYSICAL addresses, because the conversion is
                    // gSegments[n] + offset + 0x80000000. So an entry that already carries a
                    // virtual address makes that sum wrap past 32 bits, and the truncated result
                    // is a value with no top bit set: exactly the shape of the bad target here.
                    // Any entry at or above 0x80000000 below is the defect.
                    // IS THIS EVEN A DISPLAY LIST?
                    //
                    // F3DEX2 opcodes occupy 0x00 to 0x07 and 0xD3 to 0xFF. Everything between is
                    // unused, so counting how many of this list's commands carry a recognized
                    // opcode separates two very different situations: data that IS a display list
                    // and that we are misreading, versus memory that was never a display list at
                    // all, which is what an object that has not finished loading looks like.
                    {
                        int recognized = 0;
                        int looked_at = 0;
                        for (uint32_t k = 0; k < 64; ++k) {
                            const uint32_t at = list_start + k * 8;
                            if (at + 8 >= MAPPED_BYTES) {
                                break;
                            }
                            const uint8_t o = static_cast<uint8_t>(read_word(rdram, at) >> 24);
                            ++looked_at;
                            if (o <= 0x07 || o >= 0xD3) {
                                ++recognized;
                            }
                        }
                        std::fprintf(stderr,
                                     "[dl]   of the first %d commands in this list, %d carry a"
                                     " recognized opcode\n", looked_at, recognized);
                    }

                    std::fprintf(stderr, "[dl]   gSegments:");
                    for (uint32_t s = 0; s < 16; ++s) {
                        const uint32_t at = 0x00120C38 + s * 4;
                        const uint32_t v = (at + 4 < MAPPED_BYTES) ? read_word(rdram, at) : 0;
                        std::fprintf(stderr, " %u=%08X%s", s, v, (v >= 0x80000000) ? "<<BAD" : "");
                    }
                    std::fprintf(stderr, "\n");
                }
                else if (target + 8 < MAPPED_BYTES) {
                    ++g_cov.dl_followed;
                    if (depth + 1 < MAX_PATH) {
                        g_entry[depth + 1] = word1;
                    }
                    Segments nested = segs;
                    found += walk(rdram, target, depth + 1, commands, nested);
                }
                else {
                    ++g_cov.dl_skipped_out_of_range;
                }
                const uint8_t branch = static_cast<uint8_t>((word0 >> 16) & 0xFF);
                if (branch != 0) {
                    return found;   // a branch, so this list ends here
                }
            }

            physical += 8;
        }
        return found;
    }

} // namespace

namespace oot::dl_check {

    // A tiny ring of the most recent tasks. Four is enough: the interesting one is always the
    // last, and its neighbors say whether anything was already odd before it.
    struct Task { uint32_t ucode, data_ptr, data_size; };
    Task g_recent[4] = {};
    unsigned g_recent_at = 0;
    unsigned g_task_count = 0;

    void note_task(uint32_t ucode, uint32_t data_ptr, uint32_t data_size) {
        g_recent[g_recent_at] = Task{ ucode, data_ptr, data_size };
        g_recent_at = (g_recent_at + 1) % 4;
        ++g_task_count;
    }

    uint32_t checksum(const uint8_t* rdram, uint32_t physical_start) {
        if (rdram == nullptr || physical_start + 8 >= MAPPED_BYTES) {
            return 0;
        }
        // ONLY THE HEAD OF THE LIST, and deliberately a small window.
        //
        // The first version covered 16KB, which is larger than the whole display list in a small
        // room (about 440 commands, so 3.5KB). Everything past the list is other per frame data
        // that changes every frame BY DESIGN, so a mismatch there proves nothing at all. That
        // would have been a false positive dressed up as a race, and it nearly was.
        //
        // 256 words is 2KB, comfortably inside even the smallest list this game submits.
        uint32_t sum = 2166136261u;
        const uint32_t words = 256;
        for (uint32_t i = 0; i < words; ++i) {
            const uint32_t at = physical_start + i * 4;
            if (at + 4 >= MAPPED_BYTES) {
                break;
            }
            sum ^= read_word(rdram, at);
            sum *= 16777619u;
        }
        return sum;
    }

    void note_mismatch(uint32_t before, uint32_t after) {
        static int mismatches = 0;
        static int checks = 0;
        ++checks;
        if (before != after) {
            ++mismatches;
            if (mismatches <= 5 || mismatches % 100 == 0) {
                std::fprintf(stderr,
                             "[dl] THE LIST CHANGED WHILE THE RENDERER WALKED IT: 0x%08X became"
                             " 0x%08X (%d of %d frames)\n",
                             before, after, mismatches, checks);
            }
        }
    }

    void note_segment_move(const oot::game_state::Snapshot& before,
                           const oot::game_state::Snapshot& after) {
        if (!before.valid || !after.valid) {
            return;
        }
        static int moves = 0;
        static int walks = 0;
        ++walks;
        for (int s = 2; s <= 6; ++s) {
            if (before.segment[s] != after.segment[s]) {
                ++moves;
                std::fprintf(stderr,
                             "[dl] THE BANK MOVED WHILE THE RENDERER WAS READING IT: segment"
                             " %d went from 0x%06X to 0x%06X during a single walk"
                             " (%d of %d walks)\n",
                             s, before.segment[s], after.segment[s], moves, walks);
            }
        }
    }

    void report_recent_tasks() {
        std::fprintf(stderr,
                     "\n  the last graphics tasks handed to the renderer, newest last"
                     " (of %u in total):\n", g_task_count);
        for (unsigned i = 0; i < 4; ++i) {
            const Task& t = g_recent[(g_recent_at + i) % 4];
            std::fprintf(stderr, "    ucode 0x%08X  list 0x%08X  size %u\n",
                         t.ucode, t.data_ptr, t.data_size);
        }
    }

    void enable(bool on) { g_on = on; }
    bool enabled() { return g_on; }

    int scan(uint8_t* rdram, uint32_t physical_start) {
        // The GUARD runs always; only the reporting is gated on the probe. A crash does not
        // wait for diagnostics to be switched on.
        if (rdram == nullptr || physical_start >= MAPPED_BYTES) {
            return 0;
        }
        int commands = 0;
        Segments segs{};
        const int found = walk(rdram, physical_start, 0, commands, segs);

        // MEASURE THE INSTRUMENT. A walk that terminates after five commands finds nothing, and
        // looks exactly like a walk that found nothing. Reporting how much of the list was
        // actually covered is what tells those two apart, and this session has already been
        // caught out twice by trusting a check it had not verified.
        static int scans = 0;
        static int most = 0;
        static int least = 1 << 30;
        if (commands > most) { most = commands; }
        if (commands < least) { least = commands; }
        if (++scans % 120 == 0) {
            std::fprintf(stderr,
                         "[dl] %d lists, %d to %d commands, max depth %d, nested followed %d,"
                         " skipped %d unset-segment and %d out-of-range, %d ends\n",
                         scans, least, most, g_cov.max_depth, g_cov.dl_followed,
                         g_cov.dl_skipped_unset_segment, g_cov.dl_skipped_out_of_range,
                         g_cov.enddl);
            least = 1 << 30;
            most = 0;
            g_cov = Coverage{};
        }
        if (found > 0) {
            std::fprintf(stderr, "[dl] %d bad address(es) in a list of %d commands starting at 0x%06X\n",
                         found, commands, physical_start);
        }
        return found;
    }

} // namespace oot::dl_check

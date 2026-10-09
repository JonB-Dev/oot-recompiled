// This game, described to the runtime.
//
// Everything here is measured from our own ROM by tools/rom_identity.cpp, not copied. A wrong
// rom_hash rejects the user's correct ROM; a wrong internal_name turns "you have the wrong version"
// into "this is not the right game", which sends them looking in the wrong place.

#include "game/game.h"

namespace {

    // What the boot ROM leaves in memory, deposited before the game's first instruction.
    //
    // The runtime's own init writes the television type, the ROM base, the reset type and the
    // memory size ("Initialize variables normally set by IPL3" in librecomp's recomp.cpp) and stops
    // there, with `osCicId` named in a comment and left unset. This game reads it: `osCicId` must
    // be 6105, this cartridge's CIC, or Zelda's hair is drawn misshapen (z_en_zl2.c, one of the
    // game's three anti-piracy checks; the other two, the collapse bars' and the fishing pond's,
    // read through the uncached view and are handled in patches/fixes/anti_piracy_reads.c, whose
    // header tells the whole story). It is an ordinary variable at 0x80000310 (libultra's
    // parameters.s), below the boot segment the runtime copies in at 0x80000400, so the deposit
    // survives that copy and the boot's own clearing of its uninitialized data.
    //
    // Words are stored natively in the runtime's memory; the address arithmetic matches the
    // runtime's own macros (rdram + (address - 0x80000000)). Written before the game runs, so no
    // display list can refer to it yet and the PHASE-26 timing rule is not in play.
    void write_word(uint8_t* rdram, uint32_t address, uint32_t value) {
        *reinterpret_cast<uint32_t*>(rdram + (address - 0x80000000u)) = value;
    }

    void entrypoint(uint8_t* rdram, recomp_context* ctx) {
        write_word(rdram, 0x80000310u, 6105u);        // osCicId
        recomp_entrypoint(rdram, ctx);
    }

    // From tools/rom_identity.cpp, run against the retail NTSC-U 1.0 ROM:
    //
    //     size          33554432 bytes
    //     internal name "THE LEGEND OF ZELDA"
    //     rom_hash      0x9C427099CC30D135ULL
    //
    // The hash is XXH3_64bits over the whole file, which is what librecomp's check_hash computes,
    // and it is of the RETAIL COMPRESSED ROM: the one the user actually has, not the decompressed
    // one the recompiler read. The runtime hashes what it is given, after normalizing byte order.
    constexpr uint64_t ROM_HASH = 0x9C427099CC30D135ULL;

    const recomp::GameEntry entry = {
        .rom_hash = ROM_HASH,

        // Read from ROM offset 0x20, space padding trimmed. The runtime compares this to tell a
        // wrong REVISION of this game from a different game entirely.
        .internal_name = "THE LEGEND OF ZELDA",

        .display_name = "The Legend of Zelda: Ocarina of Time",

        // Version is explicit in the id because everything in this project is specific to NTSC-U
        // 1.0: the addresses, the symbols, the overlay list, the microcode offsets. A second
        // revision would be a separate entry, not a flag.
        .game_id = u8"oot.n64.us.1.0",
        .mod_game_id = "oot",

        // Ocarina of Time saves to SRAM. Majora's Mask uses Flashram, which is why the reference
        // project's entry says otherwise and why this is not a field to copy.
        .save_type = recomp::SaveType::Sram,

        // Empty. The runtime shows this in a launcher that lists several games; this build launches
        // one. Named explicitly rather than left out, because designated initializers must name
        // fields in declaration order and skipping one is an error under our warnings-as-errors.
        .thumbnail_bytes = {},

        .is_enabled = true,

        // Mod function hooking is out of scope (app-type.md), and that is the only thing these two
        // serve. The game's own code handles its compressed segments, and making that work through
        // the runtime's DMA is Phase 16's job rather than a flag here.
        .decompression_routine = nullptr,
        .has_compressed_code = false,

        // The ROM header's entrypoint. The ELF exports a function symbol there, which the context
        // dump renamed to recomp_entrypoint.
        //
        // THE CAST TO int32_t IS LOAD BEARING. gpr is uint64_t, and every game address has to be
        // SIGN EXTENDED: the runtime's memory macros compute `rdram + (address - 0xFFFFFFFF80000000)`,
        // so 0x80000400 must arrive as 0xFFFFFFFF80000400 and not as 0x0000000080000400.
        //
        // Written as the bare constant first, which is positive, so that subtraction wrapped and the
        // runtime's very first ROM read wrote to 0x207198E0403. The crash was inside
        // recomp::do_rom_read with nothing pointing back at this line. The recompiler emits this
        // same value as `(gpr)(int32_t)0x80000400u` for exactly this reason.
        .entrypoint_address = static_cast<gpr>(static_cast<int32_t>(0x80000400u)),
        // Through the wrapper above, which deposits what the boot ROM leaves in memory and then
        // runs the ROM's entrypoint.
        .entrypoint = entrypoint,
    };

} // namespace

const recomp::GameEntry& oot::game_entry() {
    return entry;
}

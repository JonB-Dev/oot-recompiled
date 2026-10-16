/* Linker script for the MIPS patch ELF.
 *
 * Patches are ordinary C compiled to MIPS and linked into an ELF that the recompiler then
 * translates AHEAD of the main output, so the linker substitutes a patched function for the
 * original. Nothing here is this game's code: these are addresses in the extra RAM the runtime
 * allocates beyond the console's, which is where anything new lives.
 *
 * RAMBASE and PATCH_RAM_END are NOT chosen here. They are librecomp's own figures, from
 * lib/N64ModernRuntime/librecomp/include/librecomp/addresses.hpp:
 *
 *     constexpr int32_t patch_rdram_start = 0x80801000;
 *     constexpr int32_t mod_rdram_start   = 0x81000000;
 *
 * Getting them wrong does not fail here; it puts patch data where the runtime will later put
 * something else.
 */

RAMBASE       = 0x80801000; /* librecomp's patch_rdram_start: where new symbols live */
PATCH_RAM_END = 0x81000000; /* librecomp's mod_rdram_start: the ceiling */

MEMORY {
    extram : ORIGIN = RAMBASE, LENGTH = 64M
    rom    : ORIGIN = 0,       LENGTH = 64M
}

SECTIONS {
    .ctors         : { *(.ctors*) *(.init_array*)                } >extram AT >rom
    .dtors         : { *(.dtors*)                                } >extram AT >rom
    .text          : { *(.text*)                                 } >extram AT >rom
    .recomp_patch  : { *(.recomp_patch*) *(.recomp_force_patch*) } >extram AT >rom
    .recomp_export : { *(.recomp_export*)                        } >extram AT >rom
    .recomp_event  : { *(.recomp_event*)                         } >extram AT >rom
    .rodata        : { *(.rodata*)                               } >extram AT >rom
    .data          : { *(.data*)                                 } >extram AT >rom
    .bss  (NOLOAD) : { *(.bss*)  *(COMMON)                       } >extram

    ASSERT(. <= PATCH_RAM_END, "Maxed out the patch RAM. See librecomp addresses.hpp.")

    .reloc    0 : { *(.reloc*)  }
    .symtab   0 : { *(.symtab)  }
    .strtab   0 : { *(.strtab)  }
    .shstrtab 0 : { *(.shstrtab) }

    /DISCARD/ : { *(*); }
}

# Build the MIPS patch ELF.
#
# THIS RUNS IN WSL, NOT ON WINDOWS, and that is a finding rather than a preference. No clang
# available on Windows carries a MIPS backend: the one in Visual Studio Build Tools does not, and
# neither does the official llvm.org release, which as of 22.1.8 registers AArch64, ARM, BPF, NVPTX,
# RISC-V, Thumb, WebAssembly and X86 and no MIPS at all. Ubuntu's clang has mips, mipsel, mips64 and
# mips64el. See .scaffold/execution/issues.md, PHASE-15.
#
# Invoke it through tools/build_patches.cmd from Windows, which handles the crossing.

TARGET  := patches.elf
BINARY  := patches.bin

# := and not ?=. CC and LD are BUILT IN make variables, already set to `cc` and `ld`, so `?=` never
# takes effect and the build silently uses the host's x86 linker. It fails with
# "/usr/bin/x86_64-linux-gnu-ld.bfd: no input files", which names the wrong linker in a way that is
# easy to read past.
CC      := clang
LD      := ld.lld

# The decomp, for its headers. Same commit as the ELF every address came from, which matters: a
# header describing a different revision's structs would compile and be wrong.
DECOMP  := ../upstream/oot

# Flags proven by the reference project's patch Makefile. -mips2 rather than a later ISA because
# that is what the original compiler targeted; -G0 disables the small data area, which the game's
# own linker layout does not expect; -mno-abicalls because this is not position independent code.
# -fno-builtin-sinf and -fno-builtin-cosf: with fast math, clang fuses a sinf and a cosf of the
# same argument into one call to sincosf, which the game does not have. The recompiler then
# stops with "Undefined symbol: sincosf" and writes nothing, and a native build that follows
# relinks the PREVIOUS patch output and reports success. Found by the matrix library patches of
# phase 39, which take a sine and a cosine of every angle they are given.
CFLAGS   := -target mips -mips2 -mabi=32 -O2 -G0 -mno-abicalls -mno-odd-spreg \
            -mno-check-zero-division -fomit-frame-pointer -ffast-math \
            -fno-unsafe-math-optimizations -fno-builtin-memset \
            -fno-builtin-sinf -fno-builtin-cosf \
            -Wall -Wextra -Wno-incompatible-library-redeclaration -Wno-unused-parameter \
            -Wno-unknown-pragmas -Wno-unused-variable -Wno-missing-braces \
            -Wno-unsupported-floating-point-opt

# -nostdinc: this is freestanding. There is no libc here, and picking up the host's headers is how
# a patch ends up calling something that does not exist on the console side.
# dummy_headers supplies the few decomp headers that need a definition the build has not produced.
# The include list mirrors what the decomp's own Makefile passes, because its headers include each
# other by paths that assume it. `include/libc` in particular holds its freestanding assert.h,
# stdbool.h and friends: with -nostdinc and without that directory, ultra64.h fails on its very
# first include and the error names assert.h rather than the include path.
# THE DEFINES BELOW ARE NOT OPTIONAL, and their absence hid for two phases. They are exactly what
# the decomp's own Makefile passes for VERSION=ntsc-1.0 (CPP_DEFINES and GBI_DEFINES: N64 platform,
# US region, revision 0, libultra I with the patch, no debug features), and each one selects
# something the headers get WRONG without it:
#
#   OOT_VERSION     struct layouts and line numbers. Undefined, the preprocessor treats it as 0:
#                   every `< NTSC_1_1` test happened to come out right for this revision, every
#                   `== NTSC_1_0` test came out wrong, and line_numbers.h refused outright.
#   F3DEX_GBI_2     THE MICROCODE. Without it gbi.h encodes every display list macro for F3D, the
#                   original microcode, and this game runs F3DEX2. A patch that wrote
#                   gSPBranchList produced an F3D G_DL (0x06), which F3DEX2 reads as G_TRI2, and
#                   the renderer walked off the list and died in tri2. The extended GBI header
#                   likewise picks its hook opcode by this define. The earlier patches survived
#                   only because none of them wrote a display list command.
#   the rest        platform, region, libultra and debug selections the headers test directly.
CPPFLAGS := -nostdinc -D_LANGUAGE_C -DMIPS \
            -DOOT_VERSION=NTSC_1_0 -DOOT_REVISION=0 -DOOT_REGION=REGION_US \
            -DPLATFORM_N64=1 -DPLATFORM_GC=0 -DPLATFORM_IQUE=0 \
            -DLIBULTRA_VERSION=LIBULTRA_VERSION_I -DLIBULTRA_PATCH=1 \
            -DDEBUG_FEATURES=0 -DNDEBUG \
            -DF3DEX_GBI_2 \
            -I . \
            -I dummy_headers \
            -I $(DECOMP)/include \
            -I $(DECOMP)/include/libc \
            -I $(DECOMP)/include/ultra64 \
            -I $(DECOMP)/include/audio \
            -I $(DECOMP)/include/tables \
            -I $(DECOMP)/src \
            -I $(DECOMP) \
            -I ../lib/rt64/include
# The last one is RT64's extended GBI, which is how a patch asks the renderer for things the
# console could not do: tagging a transform so it can be interpolated between frames, and telling
# a 2D rect how to behave when the view is wider than 4:3. It is a self contained header of macros
# that write display list words, so it needs nothing from RT64 itself and does not make the patch
# build depend on the renderer.

# --unresolved-symbols=ignore-all because a patch calls game functions whose addresses come from
# the recompiler later, not from this link. --emit-relocs because the recompiler needs them.
LDFLAGS  := -nostdlib -T patches.ld -T syms.ld -Map patches.map \
            --unresolved-symbols=ignore-all --emit-relocs

C_SRCS := $(wildcard *.c) $(wildcard required/*.c) $(wildcard fixes/*.c)
C_OBJS := $(C_SRCS:.c=.o)
C_DEPS := $(C_SRCS:.c=.d)

all: $(TARGET)

# An EMPTY patch set is a valid and useful state: Phase 15 proves the machinery works before any
# patch depends on it. ld.lld is happy to link no objects at all.
$(TARGET): $(C_OBJS) patches.ld syms.ld
	$(LD) $(C_OBJS) $(LDFLAGS) -o $@

$(C_OBJS): %.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -MMD -MF $(@:.o=.d) -c -o $@

clean:
	rm -rf $(C_OBJS) $(C_DEPS) $(TARGET) $(BINARY) patches.map

-include $(C_DEPS)

.PHONY: all clean

# OoT: Recompiled
#
# Phase 12 scope: the skeleton. This configures, builds the runtime, and links a shell that starts
# and exits. The renderer window arrives at Phase 13 and the recompiler output is linked in at
# Phase 14, per .scaffold/phases.md. Each of those is its own phase because each has its own way of
# failing, and bringing them up together makes a link error impossible to attribute.

cmake_minimum_required(VERSION 3.20)

project(OoTRecompiled VERSION 0.5.0 LANGUAGES C CXX)

# The executable's icon is a Windows resource (src/main/app.rc), compiled by the resource
# compiler the Build Tools environment provides; the mark it holds is cut by tools/make_icon.py.
enable_language(RC)
# THE RESOURCE COMPILER RUNS ON ITS OWN. With clang as the C++ compiler, CMake wraps rc.exe in its
# own preprocessing step (cmake -E cmake_llvm_rc, clang-cl -E first), and on 2026-09-24 that wrapper
# began failing silently on this machine, in this tree and in a plain path alike, with clang-cl and
# rc.exe each working by hand. The two .rc files here (the icon, the setup file's payload) need no
# preprocessing, so the standard rule stands in for the wrapper and no dependency file is asked for.
set(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> <DEFINES> <INCLUDES> <FLAGS> /fo<OBJECT> <SOURCE>")
unset(CMAKE_DEPFILE_FLAGS_RC)
unset(CMAKE_RC_DEPFILE_FORMAT)
# And Ninja's own dependency scanner for RC (cmcldeps) stays off: it drives clang-cl with cl's flags
# for the scan and fails the same way.
set(CMAKE_NINJA_CMCLDEPS_RC 0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Our own find modules. cmake/FindFreetype.cmake presents the prebuilt freetype in
# lib/freetype-windows-binaries as the imported target RmlUi looks for; without it RmlUi's
# configure stops with "Freetype could not be found", which on Windows it never would be.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# This project is Windows and D3D12 only, by scope rather than by accident. Saying so here turns a
# wrong-platform build into one clear message instead of a pile of missing headers.
if (NOT WIN32)
    message(FATAL_ERROR
        "This project is scoped to Windows and D3D12. See .scaffold/app-type.md. "
        "Building elsewhere is a re-planning decision, not a configuration change.")
endif()

# ---------------------------------------------------------------------------------------------
# The runtime: ultramodern (N64 OS HLE) and librecomp (the recompiled-code support library).
# Consumed UNMODIFIED, with this game registered as an additional entry through
# recomp::register_game. See .scaffold/scratch/stack-decisions.md, OPEN-2.
# ---------------------------------------------------------------------------------------------

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)

# ---------------------------------------------------------------------------------------------
# SDL2, for the window and the gamepad.
#
# Fetched rather than vendored or found, pinned to an exact version, matching how the reference
# project does it. This is a BUILD TIME download and does not touch the "no network" rule in
# CLAUDE.md, which is about what the shipped program connects to while it runs. The program itself
# still opens no sockets.
# ---------------------------------------------------------------------------------------------

include(FetchContent)

set(SDL2_VERSION "2.30.3" CACHE STRING "SDL2 version to fetch")

FetchContent_Declare(
    sdl2
    URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip
)
FetchContent_MakeAvailable(sdl2)

# ---------------------------------------------------------------------------------------------
# The renderer and the UI toolkit.
#
# RT64 is D3D12 here. RmlUi draws our own menus, with lunasvg for vector icons and the prebuilt
# freetype for text. None of them is modified: a local change to anything under lib/ is a fork,
# which is a recorded decision rather than an edit.
# ---------------------------------------------------------------------------------------------

# RT64 as a STATIC library, using the option it provides for exactly this.
#
# Its default is a DLL, and a DLL only exports what it annotates. Several RT64::Application methods
# this project calls (end, updateUserConfig, updateMultisampling, updateEnhancementConfig) are
# declared in the header and not exported, so the build compiles cleanly and then fails at link with
# four undefined symbols that are plainly right there in rt64_application.h. Static linking makes
# the whole class available and removes rt64.dll from what has to ship beside the executable.
set(RT64_STATIC ON CACHE BOOL "" FORCE)

# Where the renderer's source is (phase 52). lib/rt64 stays at its pin, unmodified. Every change
# to the renderer is a patch in rt64-patches/, and tools/build_rt64.cmd mirrors the submodule to
# build-cmake/rt64-src/ and applies the series onto the copy. When that copy exists it is the
# source and the ray tracing path is built; when it does not, the pinned tree builds exactly as
# it always has. The SHA the About surface shows is still read from the submodule, which is the
# version the series is written for. rt64-patches/README.md is the series' own record.
if(EXISTS "${CMAKE_BINARY_DIR}/rt64-src/CMakeLists.txt")
    set(RT64_SRC_DIR "${CMAKE_BINARY_DIR}/rt64-src")
    set(RT64_RAYTRACING ON CACHE BOOL "" FORCE)
    message(STATUS "RT64: the patched copy at ${RT64_SRC_DIR}, ray tracing path on")
else()
    set(RT64_SRC_DIR "${CMAKE_SOURCE_DIR}/lib/rt64")
    message(STATUS "RT64: the pinned submodule, no series applied")
endif()

# The SPIR-V headers' own project() caches its source directory, and re-spirv skips adding that
# directory whenever the variable already exists. So a tree that has built the copy and switches
# back to the pin (the Off proof does exactly that, and so does removing the series) keeps
# pointing at the copy, deleted or stale, and the renderer stops compiling on a missing header
# (2026-09-24, phase 53). Dropped whenever it is not under the source chosen above.
if(DEFINED SPIRV-Headers_SOURCE_DIR)
    string(FIND "${SPIRV-Headers_SOURCE_DIR}" "${RT64_SRC_DIR}/" _rt64_spirv_under)
    if(NOT _rt64_spirv_under EQUAL 0)
        unset(SPIRV-Headers_SOURCE_DIR CACHE)
        unset(SPIRV-Headers_BINARY_DIR CACHE)
        unset(SPIRV-Headers_IS_TOP_LEVEL CACHE)
        message(STATUS "RT64: the SPIR-V headers were cached from the other source, dropped")
    endif()
endif()
add_subdirectory(${RT64_SRC_DIR} ${CMAKE_BINARY_DIR}/rt64)

set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/lunasvg ${CMAKE_BINARY_DIR}/lunasvg)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RMLUI_BACKEND "native" CACHE STRING "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/RmlUi ${CMAKE_BINARY_DIR}/RmlUi)

# ---------------------------------------------------------------------------------------------
# Our own sources.
#
# src/main only, at this phase. src/game, src/ui and the generated RecompiledFuncs are added by the
# phases that bring them up.
# ---------------------------------------------------------------------------------------------

# The recompiled functions: 267 generated C files carrying 13343 translated functions, plus the
# lookup table the runtime uses to find them. GLOB rather than a written list because the file count
# is decided by the recompiler's functions_per_output_file, not by us, and a stale hand-maintained
# list would drop functions silently.
#
# CONFIGURE_DEPENDS so that regenerating them re-runs the glob. Without it, adding or removing a
# generated file leaves the build using yesterday's list and the link error names a symbol rather
# than the cause.
file(GLOB RECOMPILED_FUNCS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c")
list(LENGTH RECOMPILED_FUNCS RECOMPILED_FUNCS_COUNT)
if (RECOMPILED_FUNCS_COUNT EQUAL 0)
    message(FATAL_ERROR
        "RecompiledFuncs/ is empty. Run the recompiler first, from the config directory: "
        "N64Recomp.exe ntsc-1.0.toml")
endif()
message(STATUS "Recompiled function files: ${RECOMPILED_FUNCS_COUNT}")

# Generated microcode. src/rsp is gitignored for the same reason RecompiledFuncs is.
file(GLOB RSP_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/rsp/*.cpp")

# The recompiled PATCHES. Generated from patches/patches.elf by config/patches.toml.
#
# Optional: the patch set is empty until Phase 15 and this build must work before then.
file(GLOB RECOMPILED_PATCHES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/RecompiledPatches/*.c")
list(LENGTH RECOMPILED_PATCHES RECOMPILED_PATCHES_COUNT)
message(STATUS "Recompiled patch files: ${RECOMPILED_PATCHES_COUNT}")

# ---------------------------------------------------------------------------------------------
# HOW A PATCH ACTUALLY REPLACES A FUNCTION. This is a link order property, not a feature.
#
# The main recompiler output still contains the original function, and so does the patch output;
# both define, say, Rand_Next. Nothing marks either as weak: RECOMP_FUNC expands to plain
# __declspec(noinline) under MSVC and clang-cl, so two object files defining the same symbol would
# be a duplicate symbol error.
#
# So the recompiled functions go into a STATIC LIBRARY and the patches are compiled straight into
# the executable. A static library member is pulled in only if a symbol is STILL UNDEFINED when the
# archive is searched, and by then the patch object has already defined it, so the member that would
# have defined the original is never extracted. The patch wins by being there first.
#
# The consequence to remember: patches must never be put in a library alongside the functions, and
# the functions must never be compiled directly into the executable. Either change turns the
# substitution into a duplicate symbol error, or worse, silently into the original.
# ---------------------------------------------------------------------------------------------

add_library(RecompiledFuncs STATIC ${RECOMPILED_FUNCS})

# ---------------------------------------------------------------------------------------------
# The interface: shaders, and the provenance the About surface reads.
# ---------------------------------------------------------------------------------------------

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The pinned commits, read from the checkouts at BUILD time rather than typed into a document.
# A provenance line somebody has to remember to update is a provenance line that is wrong.
function(read_sha OUT_VAR DIR)
    execute_process(
        COMMAND git -C "${DIR}" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if (SHA STREQUAL "")
        set(SHA "unknown")
    endif()
    set(${OUT_VAR} "${SHA}" PARENT_SCOPE)
endfunction()

read_sha(SHA_RECOMPILER "${CMAKE_SOURCE_DIR}/upstream/N64Recomp")
read_sha(SHA_RUNTIME    "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime")
read_sha(SHA_RENDERER   "${CMAKE_SOURCE_DIR}/lib/rt64")
read_sha(SHA_DECOMP     "${CMAKE_SOURCE_DIR}/upstream/oot")
string(TIMESTAMP BUILD_STAMP "%Y-%m-%d" UTC)

configure_file(
    "${CMAKE_SOURCE_DIR}/src/ui/build_info.h.in"
    "${CMAKE_BINARY_DIR}/generated/build_info.h"
    @ONLY)

# The interface's one shader, compiled to DXIL and embedded. RT64 has CMake helpers for this, but
# they resolve paths against RT64's own PROJECT_SOURCE_DIR, so they cannot see a shader in our
# tree. Ours does the same job without reaching into a vendored library's internals.
set(UI_SHADER    "${CMAKE_SOURCE_DIR}/src/ui/shaders/ui.hlsl")
set(UI_DXC       "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc.exe")
set(UI_SHADER_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${UI_SHADER_DIR}")

add_custom_command(
    OUTPUT "${UI_SHADER_DIR}/ui_vs.hlsl.dxil.h"
    COMMAND "${UI_DXC}" -T vs_6_0 -E VSMain -Fo "${UI_SHADER_DIR}/ui_vs.dxil" "${UI_SHADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/embed_binary.py"
            "${UI_SHADER_DIR}/ui_vs.dxil" ui_vs_dxil "${UI_SHADER_DIR}/ui_vs.hlsl.dxil.h"
    DEPENDS "${UI_SHADER}" "${CMAKE_SOURCE_DIR}/tools/embed_binary.py"
    COMMENT "Compiling the interface vertex shader")

add_custom_command(
    OUTPUT "${UI_SHADER_DIR}/ui_ps.hlsl.dxil.h"
    COMMAND "${UI_DXC}" -T ps_6_0 -E PSMain -Fo "${UI_SHADER_DIR}/ui_ps.dxil" "${UI_SHADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/embed_binary.py"
            "${UI_SHADER_DIR}/ui_ps.dxil" ui_ps_dxil "${UI_SHADER_DIR}/ui_ps.hlsl.dxil.h"
    DEPENDS "${UI_SHADER}" "${CMAKE_SOURCE_DIR}/tools/embed_binary.py"
    COMMENT "Compiling the interface pixel shader")

# The mark's pixel shader (phase 50), a third entry point in the same file: the three stacked
# triangles and the gleam that crosses them, drawn by RmlUi's shader decorator.
add_custom_command(
    OUTPUT "${UI_SHADER_DIR}/ui_mark_ps.hlsl.dxil.h"
    COMMAND "${UI_DXC}" -T ps_6_0 -E PSMark -Fo "${UI_SHADER_DIR}/ui_mark_ps.dxil" "${UI_SHADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/embed_binary.py"
            "${UI_SHADER_DIR}/ui_mark_ps.dxil" ui_mark_ps_dxil "${UI_SHADER_DIR}/ui_mark_ps.hlsl.dxil.h"
    DEPENDS "${UI_SHADER}" "${CMAKE_SOURCE_DIR}/tools/embed_binary.py"
    COMMENT "Compiling the mark's pixel shader")

# The patch binary, embedded so the runtime can copy a patch's own data (its constants and
# initialized statics) into the emulated memory at start. The patch CODE is linked natively and
# runs without this; the DATA does not exist in that memory without it, and a patch that reads a
# float constant from its own read-only data then reads zero. That was the case for five phases
# of the post-parity plan before a sun drawn at the camera's eye gave it away (issues.md,
# PHASE-41). Same embedder as the interface shaders.
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/generated/patches_bin.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/embed_binary.py"
            "${CMAKE_SOURCE_DIR}/patches/patches.bin" oot_patches_bin "${CMAKE_BINARY_DIR}/generated/patches_bin.h"
    DEPENDS "${CMAKE_SOURCE_DIR}/patches/patches.bin" "${CMAKE_SOURCE_DIR}/tools/embed_binary.py"
    COMMENT "Embedding the patch binary")

add_executable(OoTRecompiled
    src/main/main.cpp
    src/game/register_patches.cpp
    "${CMAKE_BINARY_DIR}/generated/patches_bin.h"
    src/main/boot.cpp
    src/main/keyvalue.cpp
    src/main/input_bindings.cpp
    src/main/input.cpp
    src/main/crash_handler.cpp
    src/main/probe.cpp
    src/ui/ui_render.cpp
    src/ui/ui_shell.cpp
    src/ui/ui_settings.cpp
    src/ui/ui_lighting.cpp
    src/ui/ui_controls.cpp
    src/ui/ui_warps.cpp
    src/main/launch.cpp
    src/main/rom_source.cpp
    src/main/signature.cpp
    src/main/places.cpp
    src/main/install.cpp
    src/main/updates.cpp
    src/main/recorder.cpp
    src/main/sweep.cpp
    src/main/screenshots.cpp
    src/main/video.cpp
    src/main/photo.cpp
    src/game/hud.cpp
    src/main/microphone.cpp
    src/main/shots.cpp
    src/game/moments.cpp
    src/ui/ui_system.cpp
    "${UI_SHADER_DIR}/ui_vs.hlsl.dxil.h"
    "${UI_SHADER_DIR}/ui_ps.hlsl.dxil.h"
    "${UI_SHADER_DIR}/ui_mark_ps.hlsl.dxil.h"
    src/game/rt64_render_context.cpp
    src/game/texture_packs.cpp
    src/game/sections.cpp
    src/game/game_init.cpp
    src/game/game_state.cpp
    src/game/light_list.cpp
    src/game/air_movers.cpp
    src/game/room_pieces.cpp
    src/game/dl_check.cpp
    src/game/actor_registry.cpp
    src/game/camera_cuts.cpp
    src/game/libultra_shims.cpp
    src/game/recomp_api.cpp
    ${RECOMPILED_PATCHES}
    ${RSP_SOURCES}
)

# Warnings as errors is for OUR code. The recompiled output is a machine translation of thirty year
# old machine code and is not ours to make warning-clean, per stack.md. Turning them off here rather
# than globally keeps src/ strict.
target_compile_options(RecompiledFuncs PRIVATE /W0)
target_compile_definitions(RecompiledFuncs PRIVATE NOMINMAX)
target_include_directories(RecompiledFuncs PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    # recomp.h lives in N64Recomp, not librecomp, and is not on librecomp's PUBLIC include path.
    # Every generated file includes it, directly or through src/game/recomp_extra_decls.h.
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)
target_link_libraries(RecompiledFuncs PRIVATE librecomp ultramodern)

# The generated microcode gets the same exemption, and for a second reason as well: it includes
# librecomp's rsp_vu_impl.hpp, which is somebody else's header reached through a PUBLIC include
# path rather than a SYSTEM one, so /WX turns forty two -Wsign-compare and -Wmissing-braces
# warnings inside it into errors in our build. Neither the generated file nor that header is ours
# to make warning-clean.
#
# THE SSE FLAGS ARE NOT OPTIONAL AND NOT A WARNING SETTING. That header implements the RSP's vector
# unit with SSE intrinsics, and without these the build fails with
#
#     always_inline function '_mm_shuffle_epi8' requires target feature 'ssse3'
#     always_inline function '_mm_blendv_epi8' requires target feature 'sse4.1'
#
# SSE4.1 is already this project's documented hardware floor: RT64 requires it, and Phase 1
# verified this machine has it with a compiled CPUID probe rather than by reading a model name. So
# asking the compiler to emit for it changes nothing about who can run the result.
set_source_files_properties(${RSP_SOURCES} PROPERTIES
    COMPILE_OPTIONS "/W0;/clang:-mssse3;/clang:-msse4.1")

# The recompiled patches are generated too.
set_source_files_properties(${RECOMPILED_PATCHES} PROPERTIES COMPILE_OPTIONS "/W0")

target_include_directories(OoTRecompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Third party headers come in as SYSTEM includes, which suppresses their warnings without
# suppressing ours. Without this, /WX turns every -Wunused-parameter inside RT64's own headers into
# an error in OUR build, and the project rule is warnings-as-errors for src/ and tools/, not for
# code we vendor and do not modify.
#
# RT64 exposes no install interface, so its source tree IS its include path. The bundled dxc headers
# must win over the Windows SDK's copy of dxcapi.h.
# The icon resource (src/main/app.rc) compiles in a target of its own, with no include
# directories. The resource compiler is handed the executable's whole include list twice (a
# preprocess and the compile), and once the renderer built from its copy under build-cmake
# (phase 52) that command line passed the length a Windows shell accepts, and the step failed
# with a message about a program not being recognized. The script includes nothing, so it needs
# none of those paths.
add_library(app_resources OBJECT src/main/app.rc)
target_sources(OoTRecompiled PRIVATE $<TARGET_OBJECTS:app_resources>)

target_include_directories(OoTRecompiled SYSTEM PRIVATE
    ${sdl2_SOURCE_DIR}/include
    # librecomp's addresses.hpp includes recomp.h, which lives in N64Recomp rather than librecomp
    # and is not on librecomp's PUBLIC include path. Phase 14 needs it too: every generated file in
    # RecompiledFuncs includes it.
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    # The renderer's headers come from the same tree the library was built from, so a struct the
    # series changes has one layout on both sides of the link.
    ${RT64_SRC_DIR}/src
    ${RT64_SRC_DIR}/src/contrib
    ${RT64_SRC_DIR}/src/contrib/hlslpp/include
    ${RT64_SRC_DIR}/src/contrib/dxc/inc
)

# Debug info in the release build. RelWithDebInfo note: the crash handler resolves an address to a
# symbol name through dbghelp, which needs a PDB. Without it every crash in 13343 generated
# functions reports an offset and no name, which is barely better than nothing.
target_compile_options(OoTRecompiled PRIVATE /Zi)
target_link_options(OoTRecompiled PRIVATE -Xlinker /DEBUG)

target_compile_definitions(OoTRecompiled PRIVATE
    # Windows.h defines min and max as MACROS, which turns every std::max( into
    # "expected unqualified-id" at the call site, with a note pointing at minwindef.h. RT64 defines
    # this for its own sources and we inherit none of it, so our translation units need it too.
    NOMINMAX
    # RT64's headers switch between HLSL and C++ type definitions on this.
    HLSL_CPU
)

# WIN32_LEAN_AND_MEAN is deliberately NOT defined here. It looks like an obvious companion to
# NOMINMAX and it breaks this build: RT64's headers reach dxcapi.h, which needs the COM interfaces
# (IUnknown, IStream) that WIN32_LEAN_AND_MEAN excludes, and the failure appears as a wall of
# "expected class name" errors inside a Windows SDK header with nothing pointing back here. RT64
# itself compiles with NOMINMAX and without it, which is the configuration these headers expect.
# Individual translation units that do not touch RT64, such as src/main/main.cpp, still define it
# locally before including Windows.h, where it is both safe and useful.

target_link_directories(OoTRecompiled PRIVATE
    ${sdl2_SOURCE_DIR}/lib/x64
)

target_link_libraries(OoTRecompiled PRIVATE
    # RecompiledFuncs is listed here, as a LIBRARY, on purpose. See the note above about how a
    # patch replaces a function: the archive is searched after the patch objects are already in.
    RecompiledFuncs
    librecomp
    ultramodern
    rt64
    RmlUi::Core
    SDL2
    Winmm.lib
    # The update check, which uses the operating system's own HTTP client rather than a
    # library: dependencies.md forbids any dependency that opens a network connection, and
    # using WinHTTP keeps that rule whole instead of carving an exception into it.
    Winhttp.lib
    # SHA-512 for the update's digest check, through the operating system's own CNG.
    Bcrypt.lib
    # dbghelp resolves a crash address to a symbol name.
    Dbghelp.lib
    # The executable's own signature (the launcher's foot and About, 2026-09-19) is read through
    # Windows' trust verification, offline.
    wintrust.lib
    crypt32.lib
    # The file picker for the ROM browser (the one native dialog the user's rule allows).
    comdlg32.lib
    # The video recorder (main/video.h): Windows' own Media Foundation writes the MP4, with the
    # graphics card's H.264 encoder where there is one. Part of the operating system, so nothing
    # is added to the download.
    mfplat.lib
    mfreadwrite.lib
    mfuuid.lib
)

# The interface reaches two levels of RT64: its render hook, and plume, the render abstraction the
# hook hands us. Both are under lib/rt64/src, which is not a public include directory, so it is
# added as SYSTEM: those headers are not ours to make warning clean, and /WX would otherwise fail
# our build on somebody else's sign comparison.
target_include_directories(OoTRecompiled SYSTEM PRIVATE
    "${RT64_SRC_DIR}/src")

target_include_directories(OoTRecompiled PRIVATE
    "${CMAKE_BINARY_DIR}/generated")

# The changelog (assets/changelog.json, read by About) is parsed with the JSON library the
# runtime already vendors, as a system include so its warnings stay its own.
target_include_directories(OoTRecompiled SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty")

# The documents, the stylesheets and any fonts we are entitled to ship. Copied rather than compiled
# in, so a stylesheet can be edited and the result seen by restarting rather than rebuilding, which
# is the difference between tuning a palette in an evening and tuning it over a week.
add_custom_command(TARGET OoTRecompiled POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:OoTRecompiled>/assets")

# SDL2 and the DirectX shader compiler ship as DLLs and have to sit beside the executable, or the
# program builds and then fails to start with a dialog that names a DLL and nothing else.
add_custom_command(TARGET OoTRecompiled POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
        $<TARGET_FILE_DIR:OoTRecompiled>
)

# Warnings are errors for OUR code and only ours. RecompiledFuncs and src/rsp are machine generated
# and are added later with this deliberately relaxed, per .scaffold/stack.md.
if (MSVC)
    target_compile_options(OoTRecompiled PRIVATE /W4 /WX)
else()
    target_compile_options(OoTRecompiled PRIVATE -Wall -Wextra -Werror)
endif()

# The Windows subsystem, since the play milestone: a double click opens the window and nothing
# else. The console the project was brought up with is not lost: main.cpp sends everything that
# was printed to a log beside the executable when no standard handles were given, and keeps the
# handles when they were (the harness redirects them into its trace files, exactly as before).
# The entry point stays `main`, so the crash handler and the boot diagnostics are unchanged.
target_link_options(OoTRecompiled PRIVATE -Xlinker /SUBSYSTEM:WINDOWS -Xlinker /ENTRY:mainCRTStartup)

# Before that, a console subsystem while the project was being brought up, so the crash handler
# and the early boot diagnostics had somewhere to print.
#
# Nothing else is set here on purpose. A plain add_executable is already a console program, and CMake
# emits `-Xlinker /subsystem:console` for it. Adding `LINK_FLAGS "/SUBSYSTEM:CONSOLE"` on top passes
# that MSVC spelling straight to clang++, which reads it as a FILENAME and fails with
# "no such file or directory: '/SUBSYSTEM:CONSOLE'". Under a clang driver, a linker flag needs
# -Xlinker, and in this case it is not needed at all.
#
# Phase 19 will want /SUBSYSTEM:WINDOWS for the shipped build; the spelling there is
#     target_link_options(OoTRecompiled PRIVATE -Xlinker /SUBSYSTEM:WINDOWS -Xlinker /ENTRY:mainCRTStartup)
# rather than the LINK_FLAGS form the reference project uses, because that project builds with MSVC
# and this one builds with clang.

# ---------------------------------------------------------------------------------------------
# THE SETUP FILE: the one file a person downloads.
#
# A tiny second executable carrying the whole program inside itself as a zip resource (the game,
# SDL2.dll, dxcompiler.dll, dxil.dll and assets\). It writes them into the user's own application
# data, makes the Start menu and uninstall entries, and starts the game.
#
# IT IS A SEPARATE BINARY AND HAS TO BE. The obvious design, where the game unpacks its own DLLs,
# cannot work: Windows resolves the import table before main() runs, so a game binary with no
# SDL2.dll beside it fails at load time and never reaches the code that would have written one.
# Delay loading the imports gets past that and then dies anyway, in a static initializer that
# reaches SDL before the first line of main, with ERROR_MOD_NOT_FOUND and no output whatsoever.
# This stub imports nothing but the operating system, so none of that can happen. It was built
# only after that failure was measured; see .tasks.md.
#
# Off by default. The payload is twenty four megabytes and packing it means re-zipping and
# relinking, which does not belong in the loop where somebody is tuning a stylesheet. A build tree
# runs portable and needs none of it. tools/make_share.ps1 turns it on for a release.
# ---------------------------------------------------------------------------------------------

# The small program that finishes an uninstall from OUTSIDE the folder being removed. A running
# program cannot delete its own executable, the DLLs it has loaded, or the log it holds open, so
# the game does what it can (the shortcut, the registry entry, moving any copy of the game out of
# harm's way) and this takes the folder away afterward. Built always, because it ships in the
# payload and the portable download has no use for it but costs nothing by carrying it.
add_executable(OoTUninstall src/setup/finish.cpp)
set_target_properties(OoTUninstall PROPERTIES OUTPUT_NAME "uninstall")
target_compile_options(OoTUninstall PRIVATE /W4 /WX)
target_link_options(OoTUninstall PRIVATE -Xlinker /SUBSYSTEM:WINDOWS -Xlinker /ENTRY:mainCRTStartup)

option(OOT_PAYLOAD "Build the setup file that carries the program inside itself" OFF)

if (OOT_PAYLOAD)
    set(PAYLOAD_ZIP "${CMAKE_BINARY_DIR}/generated/payload.zip")

    # At BUILD time and after the game links, because the game is IN the payload. Depending on the
    # target rather than on its file keeps this correct when the game is rebuilt.
    add_custom_command(
        OUTPUT "${PAYLOAD_ZIP}"
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/tools/make_payload.ps1"
                -Out "${PAYLOAD_ZIP}"
                -Game "$<TARGET_FILE:OoTRecompiled>"
                -Finish "$<TARGET_FILE:OoTUninstall>"
                -Sdl2 "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
                -Dxil "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
                -Dxc  "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
        DEPENDS OoTRecompiled OoTUninstall
        COMMENT "Packing the setup payload"
        VERBATIM)
    add_custom_target(OoTPayload DEPENDS "${PAYLOAD_ZIP}")

    set(ICON_PATH "${CMAKE_SOURCE_DIR}/assets/icon/mark.ico")
    set(PAYLOAD_PATH "${PAYLOAD_ZIP}")
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/setup/setup.rc.in"
        "${CMAKE_BINARY_DIR}/generated/setup.rc"
        @ONLY)

    # The resource script NAMES the payload but does not contain it, so nothing would otherwise
    # tell the build system that re-packing the archive means re-compiling the resource. Without
    # this, the first payload build produces a setup file exactly the same size as the one before
    # it, which looks like the embedding silently failed and is really a missing edge in the graph.
    set_source_files_properties("${CMAKE_BINARY_DIR}/generated/setup.rc"
        PROPERTIES OBJECT_DEPENDS "${PAYLOAD_ZIP};${ICON_PATH}")

    add_executable(OoTRecompiledSetup
        src/setup/setup.cpp
        # Shared with the game rather than reimplemented, so the two can never disagree about
        # where files go or what the uninstall entry says.
        src/main/places.cpp
        src/main/install.cpp
        "${CMAKE_BINARY_DIR}/generated/setup.rc")
    add_dependencies(OoTRecompiledSetup OoTPayload)

    target_include_directories(OoTRecompiledSetup PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_BINARY_DIR}/generated"
        "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty/miniz"
        "${CMAKE_BINARY_DIR}/N64ModernRuntime/librecomp/miniz")
    target_compile_definitions(OoTRecompiledSetup PRIVATE NOMINMAX MINIZ_STATIC_DEFINE)
    # Gdi32 and Msimg32 paint the setup window (the case, its gradient, the mark, the buttons);
    # Comctl32 is not used, the controls are drawn by hand in the program's own look.
    target_link_libraries(OoTRecompiledSetup PRIVATE miniz Shell32.lib Ole32.lib Advapi32.lib Gdi32.lib Msimg32.lib User32.lib)
    target_compile_options(OoTRecompiledSetup PRIVATE /W4 /WX)

    # Windows subsystem, like the game: a double clicked setup file must not flash a console.
    # The entry point stays `main`.
    target_link_options(OoTRecompiledSetup PRIVATE
        -Xlinker /SUBSYSTEM:WINDOWS -Xlinker /ENTRY:mainCRTStartup)
endif()

# Point CMake at the prebuilt Freetype in lib/freetype-windows-binaries.
#
# RmlUi requires Freetype and looks for it with find_package, which on Windows finds nothing: there
# is no system Freetype and no package manager in this project. The submodule carries prebuilt
# static libraries, so this shim presents one as the imported target RmlUi expects.
#
# Put on CMAKE_MODULE_PATH by the top level CMakeLists so find_package(Freetype) resolves here.
#
# THE TOOLSET DIRECTORY IS SEARCHED, NOT HARDCODED. The reference project names
# "release static/vs2015-2022/win64", and at the version pinned here (v2.14.3) the path is
# "release static/vs2026/x64": both the toolset folder and the architecture folder were renamed.
# Hardcoding either one means the next submodule bump fails with a message about a missing
# submodule, which is what the first version of this file did say, and it was wrong.

set(_ft_root "${CMAKE_SOURCE_DIR}/lib/freetype-windows-binaries")

file(GLOB _ft_candidates
    "${_ft_root}/release static/*/x64/freetype.lib"
    "${_ft_root}/release static/*/win64/freetype.lib"
)

if (NOT _ft_candidates)
    message(FATAL_ERROR
        "No prebuilt 64 bit freetype under '${_ft_root}/release static'. "
        "If the submodule is missing, run: git submodule update --init --recursive. "
        "If it is present, its directory layout has changed again and the glob in "
        "cmake/FindFreetype.cmake needs widening.")
endif()

# Newest toolset last, so the most recent build wins when several are shipped.
list(SORT _ft_candidates)
list(GET _ft_candidates -1 FREETYPE_LIBRARIES)

set(FREETYPE_INCLUDE_DIRS "${_ft_root}/include")

if (NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype STATIC IMPORTED)
    set_target_properties(Freetype::Freetype PROPERTIES
        IMPORTED_LOCATION "${FREETYPE_LIBRARIES}"
    )
    target_include_directories(Freetype::Freetype INTERFACE
        "${FREETYPE_INCLUDE_DIRS}"
    )
endif()

message(STATUS "Freetype: ${FREETYPE_LIBRARIES}")

set(FREETYPE_FOUND TRUE)
set(Freetype_FOUND TRUE)

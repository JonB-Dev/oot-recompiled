# Native Apple Silicon build. Keep its configuration separate from the Windows build.
if (NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    message(FATAL_ERROR "The macOS port currently targets Apple Silicon only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

add_library(oot_macos_places STATIC src/main/places_macos.cpp)
target_include_directories(oot_macos_places PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_options(oot_macos_places PRIVATE -Wall -Wextra -Werror)

add_executable(places_macos_test tests/places_macos_test.cpp)
target_link_libraries(places_macos_test PRIVATE oot_macos_places)
target_compile_options(places_macos_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME places_macos COMMAND places_macos_test "${CMAKE_BINARY_DIR}/test-data/places")

add_executable(pointer_coordinates_macos_test tests/pointer_coordinates_macos_test.cpp)
target_include_directories(pointer_coordinates_macos_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(pointer_coordinates_macos_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME macos_retina_pointer_targets COMMAND pointer_coordinates_macos_test)

add_executable(texture_packs_macos_test
    tests/texture_packs_macos_test.cpp
    src/game/texture_packs.cpp)
target_link_libraries(texture_packs_macos_test PRIVATE oot_macos_places)
target_compile_options(texture_packs_macos_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME texture_packs_macos COMMAND texture_packs_macos_test "${CMAKE_BINARY_DIR}/test-data/packs")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(UI_TOKEN_HEADER "${CMAKE_BINARY_DIR}/generated/ui_tokens.h")
add_custom_command(
    OUTPUT "${UI_TOKEN_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/tokens_header.py" "${UI_TOKEN_HEADER}"
    DEPENDS "${CMAKE_SOURCE_DIR}/assets/ui/tokens.rcss" "${CMAKE_SOURCE_DIR}/tools/tokens_header.py")
add_executable(ui_tests
    src/ui/tests/ui_tests_main.cpp
    src/ui/input/ui_input_core.cpp
    src/ui/nav/nav_engine.cpp
    src/ui/nav/nav_values.cpp
    src/ui/nav/nav_mouse.cpp
    src/ui/shell/shell.cpp
    src/ui/models/credits.cpp
    src/ui/prompts/prompts.cpp
    src/ui/prompts/photo_guide.cpp
    src/ui/keyboard/keyboard.cpp
    src/ui/sound/synth.cpp
    src/ui/sound/game_queue.cpp
    src/ui/ui_gradient.cpp
    "${UI_TOKEN_HEADER}")
target_include_directories(ui_tests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(ui_tests PRIVATE OOT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(ui_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME ui_logic COMMAND ui_tests)

option(OOT_MACOS_BUILD_RUNTIME "Compile the generated game and pinned runtime" OFF)
if (OOT_MACOS_BUILD_RUNTIME)
    add_subdirectory("${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime" "${CMAKE_BINARY_DIR}/N64ModernRuntime")
    add_executable(lighting_quality_macos_test tests/lighting_quality_macos_test.cpp
        src/ui/ui_lighting.cpp src/main/keyvalue.cpp)
    target_include_directories(lighting_quality_macos_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(lighting_quality_macos_test PRIVATE librecomp ultramodern)
    target_compile_options(lighting_quality_macos_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME lighting_quality_preserves_existing_settings COMMAND lighting_quality_macos_test
        "${CMAKE_BINARY_DIR}/test-data/lighting")
    add_executable(gameplay_presets_macos_test tests/gameplay_presets_macos_test.cpp
        src/ui/ui_lighting.cpp src/main/keyvalue.cpp)
    target_include_directories(gameplay_presets_macos_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(gameplay_presets_macos_test PRIVATE librecomp ultramodern)
    target_compile_options(gameplay_presets_macos_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME macos_gameplay_presets COMMAND gameplay_presets_macos_test)
    file(GLOB RECOMPILED_FUNCS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c")
    file(GLOB RECOMPILED_PATCHES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/RecompiledPatches/*.c")
    file(GLOB RSP_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/rsp/*.cpp")
    if (NOT RECOMPILED_FUNCS OR NOT RECOMPILED_PATCHES OR NOT RSP_SOURCES)
        message(FATAL_ERROR "Generate the ROM-derived functions, patches, and RSP sources first")
    endif()

    add_library(RecompiledFuncs STATIC ${RECOMPILED_FUNCS})
    target_include_directories(RecompiledFuncs PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include")
    target_link_libraries(RecompiledFuncs PRIVATE librecomp ultramodern)
    target_compile_options(RecompiledFuncs PRIVATE -fno-strict-aliasing -Wno-unused-variable)

    add_library(OoTGeneratedPatches OBJECT ${RECOMPILED_PATCHES})
    target_include_directories(OoTGeneratedPatches PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/RecompiledFuncs"
        "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include")
    target_link_libraries(OoTGeneratedPatches PRIVATE librecomp ultramodern)
    target_compile_options(OoTGeneratedPatches PRIVATE -fno-strict-aliasing -Wno-unused-variable)

    add_library(OoTGeneratedRSP OBJECT ${RSP_SOURCES})
    target_include_directories(OoTGeneratedRSP PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include")
    target_link_libraries(OoTGeneratedRSP PRIVATE librecomp ultramodern)
    target_compile_options(OoTGeneratedRSP PRIVATE -fno-strict-aliasing -Wno-unused-variable)

    add_custom_target(oot_generated_check DEPENDS RecompiledFuncs OoTGeneratedPatches OoTGeneratedRSP librecomp ultramodern)
endif()

option(OOT_MACOS_BUILD_RENDERER "Build the pinned, patched RT64 Metal renderer" OFF)
if (OOT_MACOS_BUILD_RENDERER)
    set(RT64_STATIC ON CACHE BOOL "" FORCE)
    if (EXISTS "${CMAKE_BINARY_DIR}/rt64-src/CMakeLists.txt")
        set(RT64_SRC_DIR "${CMAKE_BINARY_DIR}/rt64-src")
    else()
        message(FATAL_ERROR "Run tools/prepare_rt64_macos.py before configuring the renderer")
    endif()
    add_subdirectory("${RT64_SRC_DIR}" "${CMAKE_BINARY_DIR}/rt64")
    add_executable(metal_as_macos_test tests/metal_as_macos_test.cpp)
    target_include_directories(metal_as_macos_test PRIVATE
        "${RT64_SRC_DIR}/src/contrib/plume/contrib/metal-cpp")
    target_link_libraries(metal_as_macos_test PRIVATE plume
        "-framework Metal" "-framework QuartzCore" "-framework Foundation"
        "-framework AppKit" "-framework IOKit")
    add_test(NAME metal_acceleration_structures COMMAND metal_as_macos_test)
    add_test(NAME metal_capture_readback COMMAND metal_as_macos_test)
    add_test(NAME metal_drawable_texture_types COMMAND metal_as_macos_test --texture-types-only)
    add_test(NAME metal_command_resource_lifetime COMMAND metal_as_macos_test --command-lifetime-only)
    add_executable(transform_processor_macos_test tests/transform_processor_macos_test.cpp)
    target_include_directories(transform_processor_macos_test PRIVATE
        "${RT64_SRC_DIR}/src" "${RT64_SRC_DIR}/src/contrib"
        "${RT64_SRC_DIR}/src/contrib/hlslpp/include" "${RT64_SRC_DIR}/src/contrib/imgui")
    target_compile_definitions(transform_processor_macos_test PRIVATE HLSL_CPU=1)
    target_link_libraries(transform_processor_macos_test PRIVATE rt64)
    add_test(NAME unmatched_tracing_transforms COMMAND transform_processor_macos_test)
    add_executable(packed_indices_macos_test tests/packed_indices_macos_test.cpp)
    target_include_directories(packed_indices_macos_test PRIVATE
        "${RT64_SRC_DIR}/src" "${RT64_SRC_DIR}/src/contrib/hlslpp/include")
    target_compile_definitions(packed_indices_macos_test PRIVATE HLSL_CPU=1)
    add_test(NAME packed_fog_indices COMMAND packed_indices_macos_test)
    add_executable(ray_passes_macos_test tests/ray_passes_macos_test.cpp)
    target_include_directories(ray_passes_macos_test PRIVATE "${RT64_SRC_DIR}/src")
    add_test(NAME inactive_ray_effects COMMAND ray_passes_macos_test)
    add_executable(present_policy_macos_test tests/present_policy_macos_test.cpp)
    target_include_directories(present_policy_macos_test PRIVATE "${RT64_SRC_DIR}/src")
    target_compile_options(present_policy_macos_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME original_rate_tracing_presents_completed_frames COMMAND present_policy_macos_test)
    set(LIGHT_FILTER_TEST "${CMAKE_BINARY_DIR}/light-filter-macos-test")
    add_custom_command(OUTPUT "${LIGHT_FILTER_TEST}"
        COMMAND xcrun swiftc -O "${CMAKE_SOURCE_DIR}/tests/light_filter_macos_test.swift" -o "${LIGHT_FILTER_TEST}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/light_filter_macos_test.swift"
        VERBATIM)
    add_custom_target(metal_light_filter_test ALL DEPENDS "${LIGHT_FILTER_TEST}")
    add_dependencies(metal_light_filter_test rt64)
    add_test(NAME light_filter_preserves_edges_and_partial_tiles
        COMMAND "${LIGHT_FILTER_TEST}" "${CMAKE_BINARY_DIR}/src/shaders")
    add_test(NAME spirv_cross_padding
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_spirv_cross_padding.py")
    add_test(NAME rt_temporal_metal
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_rt_temporal_metal.py"
            "${RT64_SRC_DIR}/src/contrib/dxc/bin/arm64/dxc-macos" "$<TARGET_FILE:spirv_cross_msl>")
endif()

option(OOT_MACOS_BUILD_APP "Compile the macOS game application" OFF)
if (OOT_MACOS_BUILD_APP)
    if (NOT OOT_MACOS_BUILD_RENDERER OR NOT OOT_MACOS_BUILD_RUNTIME)
        message(FATAL_ERROR "The macOS app requires both renderer and generated runtime")
    endif()
    find_package(SDL2 REQUIRED CONFIG)
    find_package(Freetype REQUIRED)
    enable_language(OBJCXX)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/lib/lunasvg" "${CMAKE_BINARY_DIR}/lunasvg")
    set(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)
    set(RMLUI_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/lib/RmlUi" "${CMAKE_BINARY_DIR}/RmlUi")
    set(UI_SHADER "${CMAKE_SOURCE_DIR}/src/ui/shaders/ui.hlsl")
    set(UI_DXC "${RT64_SRC_DIR}/src/contrib/dxc/bin/arm64/dxc-macos")
    foreach(UI_STAGE IN ITEMS vs ps mark_ps gradient_ps)
        if(UI_STAGE STREQUAL "vs")
            set(UI_ENTRY VSMain)
            set(UI_PROFILE vs_6_3)
            set(UI_EXTRA -fvk-invert-y)
        elseif(UI_STAGE STREQUAL "ps")
            set(UI_ENTRY PSMain)
            set(UI_PROFILE ps_6_3)
            set(UI_EXTRA)
        elseif(UI_STAGE STREQUAL "mark_ps")
            set(UI_ENTRY PSMark)
            set(UI_PROFILE ps_6_3)
            set(UI_EXTRA)
        else()
            set(UI_ENTRY PSGradient)
            set(UI_PROFILE ps_6_3)
            set(UI_EXTRA)
        endif()
        set(UI_OUT "${CMAKE_BINARY_DIR}/generated/ui_${UI_STAGE}")
        add_custom_command(
            OUTPUT "${UI_OUT}.hlsl.metal.h"
            COMMAND "${CMAKE_COMMAND}" -E env
                "DYLD_LIBRARY_PATH=${RT64_SRC_DIR}/src/contrib/dxc/lib/arm64"
                "${UI_DXC}" -spirv -fspv-target-env=vulkan1.0 -fvk-use-dx-layout
                -T "${UI_PROFILE}" -E "${UI_ENTRY}" -fvk-s-shift 1 0 ${UI_EXTRA}
                "${UI_SHADER}" -Fo "${UI_OUT}.spv"
            COMMAND "$<TARGET_FILE:spirv_cross_msl>" "${UI_OUT}.spv" "${UI_OUT}.metal"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_metal_ui_bindings.py" "${UI_OUT}.metal"
            COMMAND xcrun --sdk macosx metal -c "${UI_OUT}.metal" -o "${UI_OUT}.ir"
            COMMAND xcrun --sdk macosx metallib "${UI_OUT}.ir" -o "${UI_OUT}.metallib"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/embed_binary.py"
                "${UI_OUT}.metallib" "ui_${UI_STAGE}_metal" "${UI_OUT}.hlsl.metal.h"
            DEPENDS "${UI_SHADER}" "${CMAKE_SOURCE_DIR}/tools/embed_binary.py"
                "${CMAKE_SOURCE_DIR}/tools/check_metal_ui_bindings.py" spirv_cross_msl
            VERBATIM)
        list(APPEND UI_SHADER_HEADERS "${UI_OUT}.hlsl.metal.h")
    endforeach()
    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE SHA_RECOMPILER OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE SHA_RUNTIME OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}/lib/rt64" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE SHA_RENDERER OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}/upstream/oot" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE SHA_DECOMP OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(TIMESTAMP BUILD_STAMP "%Y-%m-%d" UTC)
    configure_file("${CMAKE_SOURCE_DIR}/src/ui/build_info.h.in"
        "${CMAKE_BINARY_DIR}/generated/build_info.h" @ONLY)

    set(PATCH_BINARY_HEADER "${CMAKE_BINARY_DIR}/generated/patches_bin.h")
    add_custom_command(
        OUTPUT "${PATCH_BINARY_HEADER}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/embed_binary.py"
            "${CMAKE_SOURCE_DIR}/patches/patches.bin" oot_patches_bin "${PATCH_BINARY_HEADER}"
        DEPENDS "${CMAKE_SOURCE_DIR}/patches/patches.bin" "${CMAKE_SOURCE_DIR}/tools/embed_binary.py")
    file(GLOB GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/game/*.cpp")
    add_executable(OoTRecompiled MACOSX_BUNDLE
        src/main/main.cpp
        src/main/boot.cpp
        src/main/keyvalue.cpp
        src/main/input_bindings.cpp
        src/main/input.cpp
        src/main/probe.cpp
        src/main/recorder.cpp
        src/main/sweep.cpp
        src/main/screenshots.cpp
        src/main/capture_size.cpp
        src/main/photo.cpp
        src/main/microphone.cpp
        src/main/shots.cpp
        src/main/rom_source.cpp
        src/main/launch_macos.cpp
        src/main/rom_picker_macos.mm
        src/main/macos_services.cpp
        src/ui/ui_shell.cpp
        src/ui/ui_render.cpp
        src/ui/ui_fonts.cpp
        src/ui/ui_vectors.cpp
        src/ui/ui_gradient.cpp
        src/ui/input/ui_input_core.cpp
        src/ui/input/ui_input.cpp
        src/ui/nav/nav_engine.cpp
        src/ui/nav/nav_values.cpp
        src/ui/nav/nav_mouse.cpp
        src/ui/nav/nav_rml.cpp
        src/ui/models/rows_model.cpp
        src/ui/models/shell_model.cpp
        src/ui/models/credits.cpp
        src/ui/prompts/prompts.cpp
        src/ui/prompts/photo_guide.cpp
        src/ui/keyboard/keyboard.cpp
        src/ui/sound/ui_sound.cpp
        src/ui/sound/synth.cpp
        src/ui/sound/game_queue.cpp
        src/ui/shell/shell.cpp
        src/ui/ui_settings.cpp
        src/ui/ui_lighting.cpp
        src/ui/ui_controls.cpp
        src/ui/ui_warps.cpp
        src/ui/ui_system.cpp
        ${GAME_SOURCES}
        "${PATCH_BINARY_HEADER}" "${UI_TOKEN_HEADER}" ${UI_SHADER_HEADERS})
    target_include_directories(OoTRecompiled PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_BINARY_DIR}/generated"
        "${RT64_SRC_DIR}/src"
        "${RT64_SRC_DIR}/src/contrib"
        "${RT64_SRC_DIR}/src/contrib/hlslpp/include"
        "${RT64_SRC_DIR}/src/contrib/dxc/inc"
        "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include")
    target_compile_definitions(OoTRecompiled PRIVATE HLSL_CPU=1)
    if (RT64_RAYTRACING)
        target_compile_definitions(OoTRecompiled PRIVATE OOT_MACOS_TRACING_APP=1)
    endif()
    target_link_libraries(OoTRecompiled PRIVATE
        RecompiledFuncs OoTGeneratedPatches OoTGeneratedRSP
        librecomp ultramodern rt64 SDL2::SDL2 oot_macos_places
        RmlUi::Core Freetype::Freetype lunasvg::lunasvg)
    target_link_libraries(OoTRecompiled PRIVATE "${APPKIT_FRAMEWORK}")
    add_custom_command(TARGET OoTRecompiled POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_BUNDLE_CONTENT_DIR:OoTRecompiled>/Resources/assets"
        COMMENT "Copying the game's public assets into the local app bundle")
endif()

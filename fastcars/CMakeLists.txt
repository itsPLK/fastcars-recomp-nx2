# fastcars: the macOS desktop build, for debugging the Switch 2 port (AGENTS.md, "Testing on the Mac
# first"). The Switch 2 NRO is built by switch2/game/CMakeLists.txt from the same sources.
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(fastcars LANGUAGES CXX)
if(NOT APPLE)
    message(FATAL_ERROR "fastcars/ builds the macOS debug build only; the Switch 2 build is switch2/game.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FASTCARS_GAME_DLL: fastcars_fast holds no recompiled game code; it loads fastcars_game.dylib, built
# separately (tools/game_dll/build_game_dll.py, fs/fastcars_pch.h). The exe then needs no generated
# sources at all.
option(FASTCARS_GAME_DLL "Build fastcars_fast against a separately built fastcars_game.dylib" OFF)

if(FASTCARS_GAME_DLL)
    find_package(rexglue 0.10.0 REQUIRED CONFIG)
else()
    include(generated/rexglue.cmake)
endif()

# Sources
set(FASTCARS_SOURCES
    src/main.cpp
    src/crash_handler.cpp
    src/cutscene_audio_fix.cpp
    src/frame_limit.cpp
    src/wait_profile.cpp
    src/gpu_fence_wait.cpp
    src/native/stats.cpp
    src/native/capture.cpp
    src/native/d3d_probe.cpp
    src/native/d3d_hooks.cpp
    src/native/renderer.cpp
    src/native/shaders.cpp
    src/native/menu.cpp
    src/native/backend.cpp
    src/native/deferred_list.cpp
    src/native/textures.cpp
    src/native/write_watch.cpp
    src/native/shader_compiler.cpp
    src/native/touch_controls.cpp
)

add_executable(fastcars ${FASTCARS_SOURCES})
# (macOS executables export their global symbols by default: the SDK's Vulkan command processor
# finds rex_skip_rendering_swap with dlsym.)

if(FASTCARS_GAME_DLL)
    target_sources(fastcars PRIVATE src/game_bridge.cpp)
    target_compile_definitions(fastcars PRIVATE FASTCARS_GAME_DLL=1)
    target_include_directories(fastcars PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(fastcars PRIVATE rex::runtime)
    rexglue_configure_target(fastcars)
    # Pre-link: list the hooks and the game functions the exe calls (names only) and link against
    # a stub fastcars_game.dylib (@rpath = the exe folder); -export_dynamic keeps the hooks and the
    # bridge, which only the game library calls, in the export trie.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(FASTCARS_INTERFACE_DIR ${CMAKE_BINARY_DIR}/game_interface)
    find_program(FASTCARS_LLVM_NM NAMES llvm-nm nm REQUIRED)
    get_filename_component(FASTCARS_LLVM_BIN "${FASTCARS_LLVM_NM}" DIRECTORY)
    add_custom_command(TARGET fastcars PRE_LINK
        COMMAND ${CMAKE_COMMAND} -E env FASTCARS_STUB_SDK=macosx
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/game_dll/gen_interface.py
                ${FASTCARS_LLVM_BIN} ${CMAKE_BINARY_DIR}/CMakeFiles/fastcars.dir ${FASTCARS_INTERFACE_DIR}
                $<TARGET_FILE:rex::runtime> ${CMAKE_CXX_COMPILER}
        VERBATIM)
    target_link_options(fastcars PRIVATE "${FASTCARS_INTERFACE_DIR}/stub/fastcars_game.dylib"
                        "LINKER:-export_dynamic")
    set_target_properties(fastcars PROPERTIES OUTPUT_NAME fastcars_fast)
else()
    rexglue_setup_target(fastcars)
endif()

# The SDK exports REXGLUE_ENABLE_PROFILING / _PERF_COUNTERS to every non-Release build: each
# indirect guest call then does an atomic add on one shared counter in rexruntime (~3% of every
# game thread in a Windows profile, plus cache-line traffic between them). Off for play.
option(FASTCARS_SDK_COUNTERS "Keep the SDK's guest call counters in the game code" OFF)
if(NOT FASTCARS_SDK_COUNTERS)
    foreach(_fastcars_target fastcars fastcars_recomp)
        if(TARGET ${_fastcars_target})
            target_compile_options(${_fastcars_target} PRIVATE -UREXGLUE_ENABLE_PROFILING -UREXGLUE_ENABLE_PERF_COUNTERS)
        endif()
    endforeach()
endif()

# The generated registration function (one call per game function, ~100k) takes clang's arm64
# backend hours (-O3, and GlobalISel at -O0); it runs once at startup: unoptimized, SelectionDAG.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/generated/default/fastcars_register.cpp
                            PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)

# Profile-guided optimization of the recompiled game code (clang): FASTCARS_PGO=GENERATE builds an
# instrumented game (writes LLVM_PROFILE_FILE when it exits normally or on the profile flush); merge
# the .profraw files with llvm-profdata into FASTCARS_PGO_PROFILE and build with FASTCARS_PGO=USE.
set(FASTCARS_PGO "OFF" CACHE STRING "Profile-guided optimization of the game code: OFF | GENERATE | USE")
set(FASTCARS_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata for FASTCARS_PGO=USE")
if(FASTCARS_PGO STREQUAL "GENERATE")
    target_compile_options(fastcars_recomp PRIVATE -fprofile-instr-generate)
    target_link_options(fastcars PRIVATE -fprofile-instr-generate)
    target_sources(fastcars PRIVATE src/pgo_dump.cpp)
elseif(FASTCARS_PGO STREQUAL "USE")
    if(NOT EXISTS "${FASTCARS_PGO_PROFILE}")
        message(FATAL_ERROR "FASTCARS_PGO=USE needs FASTCARS_PGO_PROFILE (a merged .profdata)")
    endif()
    target_compile_options(fastcars_recomp PRIVATE "-fprofile-instr-use=${FASTCARS_PGO_PROFILE}"
                           -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
endif()

# Native renderer: Plume (Vulkan, SDL3 surface). SDL3 itself comes from librexruntime, which
# exports it, so only the headers are needed here.
find_package(SDL3 REQUIRED CONFIG COMPONENTS Headers)
set(PLUME_SDL3_VULKAN_ENABLED ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/plume ${CMAKE_BINARY_DIR}/plume EXCLUDE_FROM_ALL)
set_target_properties(plume PROPERTIES POSITION_INDEPENDENT_CODE ON)
option(FASTCARS_NATIVE_VALIDATION "Enable the Vulkan validation layer on the native renderer's instance" OFF)
if(FASTCARS_NATIVE_VALIDATION)
    target_compile_definitions(plume PRIVATE VULKAN_VALIDATION_LAYER_ENABLED)
endif()
target_link_libraries(fastcars PRIVATE plume)

# The texture decoder uses the SDK's Xenos texture helpers (TextureInfo, tiling), which live in
# the xenos GPU plugin library.
find_library(REXGPU_XENOS rexgpu-xenos PATHS ${rexglue_DIR}/../.. NO_DEFAULT_PATH REQUIRED)
target_link_libraries(fastcars PRIVATE ${REXGPU_XENOS})

# Native renderer's own shaders (present blit), compiled to SPIR-V headers with XenosRecomp's DXC.
set(FASTCARS_DXC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/XenosRecomp/thirdparty/dxc-bin)
set(FASTCARS_DXC ${CMAKE_COMMAND} -E env DYLD_LIBRARY_PATH=${FASTCARS_DXC_DIR}/lib/arm64 ${FASTCARS_DXC_DIR}/bin/arm64/dxc-macos)
set(FASTCARS_NATIVE_SHADER_DIR ${CMAKE_BINARY_DIR}/native_shaders)
set(FASTCARS_BLIT_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/native_blit.hlsl)
set(FASTCARS_RECTLIST_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/native_rectlist.hlsl)
set(FASTCARS_MEMORY_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/native_memory.hlsl)
set(FASTCARS_MENU_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/native_menu.hlsl)
set(FASTCARS_FSR_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/native_fsr.hlsl)
set(FASTCARS_TAA_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/native_taa.hlsl)
# Optional: copy prebuilt headers from FASTCARS_PREBUILT_NATIVE_SHADERS instead of running dxc.
set(FASTCARS_PREBUILT_NATIVE_SHADERS "" CACHE PATH "Directory of prebuilt native shader headers (skips dxc)")
if(FASTCARS_PREBUILT_NATIVE_SHADERS)
    file(GLOB _fastcars_prebuilt ${FASTCARS_PREBUILT_NATIVE_SHADERS}/*.h)
    file(COPY ${_fastcars_prebuilt} DESTINATION ${FASTCARS_NATIVE_SHADER_DIR})
else()
add_custom_command(
    OUTPUT ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${FASTCARS_NATIVE_SHADER_DIR}
    COMMAND ${FASTCARS_DXC}
            -T vs_6_0 -E VSMain -spirv -fspv-target-env=vulkan1.1 -Vn g_native_blit_vs
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_vs.h ${FASTCARS_BLIT_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSMain -spirv -fspv-target-env=vulkan1.1 -Vn g_native_blit_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_ps.h ${FASTCARS_BLIT_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSDepth -spirv -fspv-target-env=vulkan1.1 -Vn g_native_depth_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_depth_ps.h ${FASTCARS_BLIT_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSResolve -spirv -fspv-target-env=vulkan1.1 -Vn g_native_resolve_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_resolve_ps.h ${FASTCARS_BLIT_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSDepthPack -spirv -fspv-target-env=vulkan1.1 -Vn g_native_depth_pack_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_depth_pack_ps.h ${FASTCARS_BLIT_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T gs_6_0 -E main -spirv -fspv-target-env=vulkan1.1 -Vn g_native_rectlist_gs
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_rectlist_gs.h ${FASTCARS_RECTLIST_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T cs_6_0 -E CSResolveToMemory -spirv -fspv-target-env=vulkan1.1 -Vn g_native_resolve_memory_cs
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_resolve_memory_cs.h ${FASTCARS_MEMORY_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSDigits -spirv -fspv-target-env=vulkan1.1 -Vn g_native_digits_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_digits_ps.h ${FASTCARS_BLIT_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSAlpha -spirv -fspv-target-env=vulkan1.1 -Vn g_native_alpha_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_alpha_ps.h ${FASTCARS_BLIT_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T vs_6_0 -E VSMenu -HV 2021 -spirv -Vn g_native_menu_vs
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_menu_vs.h ${FASTCARS_MENU_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSMenu -HV 2021 -spirv -Vn g_native_menu_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_menu_ps.h ${FASTCARS_MENU_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSDiagFlat -spirv -fspv-target-env=vulkan1.1 -Vn g_native_diag_flat_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_diag_flat_ps.h ${FASTCARS_BLIT_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSEasu -HV 2018 -spirv -fspv-target-env=vulkan1.1 -Vn g_native_fsr_easu_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_fsr_easu_ps.h ${FASTCARS_FSR_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSRcas -HV 2018 -spirv -fspv-target-env=vulkan1.1 -Vn g_native_fsr_rcas_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_fsr_rcas_ps.h ${FASTCARS_FSR_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSTaa -spirv -fspv-target-env=vulkan1.1 -Vn g_native_taa_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_taa_ps.h ${FASTCARS_TAA_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSTaaPrepare -spirv -fspv-target-env=vulkan1.1 -Vn g_native_taa_prepare_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_taa_prepare_ps.h ${FASTCARS_TAA_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSTaaTileMv -spirv -fspv-target-env=vulkan1.1 -Vn g_native_taa_tile_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_taa_tile_ps.h ${FASTCARS_TAA_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSFrameGen -spirv -fspv-target-env=vulkan1.1 -Vn g_native_taa_fg_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_taa_fg_ps.h ${FASTCARS_TAA_HLSL}
    COMMAND ${FASTCARS_DXC}
            -T ps_6_0 -E PSTaaId -spirv -fspv-target-env=vulkan1.1 -Vn g_native_taa_id_ps
            -Fh ${FASTCARS_NATIVE_SHADER_DIR}/native_taa_id_ps.h ${FASTCARS_BLIT_HLSL}
    COMMAND ${CMAKE_COMMAND} -E echo "#pragma once" > ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_fsr_easu_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_fsr_rcas_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_taa_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_taa_prepare_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_taa_tile_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_taa_fg_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_taa_id_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_digits_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_diag_flat_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_alpha_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_resolve_memory_cs.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_rectlist_gs.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_depth_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_resolve_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_depth_pack_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_blit_vs.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_blit_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_menu_vs.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"native_menu_ps.h\"" >> ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h
    DEPENDS ${FASTCARS_BLIT_HLSL} ${FASTCARS_RECTLIST_HLSL} ${FASTCARS_MEMORY_HLSL} ${FASTCARS_MENU_HLSL} ${FASTCARS_FSR_HLSL} ${FASTCARS_TAA_HLSL}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/fsr1/ffx_a.h ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/fsr1/ffx_fsr1.h
    VERBATIM)
add_custom_target(fastcars_native_shaders DEPENDS ${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h)
add_dependencies(fastcars fastcars_native_shaders)
endif()
target_include_directories(fastcars PRIVATE ${FASTCARS_NATIVE_SHADER_DIR})

# Background shader compiler tools (shader_compiler.cpp): <exe>/tools/XenosRecomp, dxc and
# shader_common.h.
# tools/XenosRecomp and tools/dxc (dxc-macos) find tools/lib/libdxcompiler.dylib through an
# @executable_path/lib rpath (re-signed ad hoc after install_name_tool).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(FASTCARS_XR_BIN ${CMAKE_CURRENT_SOURCE_DIR}/../build/xenosrecomp-mac/XenosRecomp/XenosRecomp CACHE FILEPATH
    "XenosRecomp binary for the background shader compiler")
set(FASTCARS_TOOLS_DIR $<TARGET_FILE_DIR:fastcars>/tools)
add_custom_command(TARGET fastcars POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${FASTCARS_TOOLS_DIR}/lib
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FASTCARS_DXC_DIR}/lib/arm64/libdxcompiler.dylib ${FASTCARS_TOOLS_DIR}/lib/libdxcompiler.dylib
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/XenosRecomp/XenosRecomp/shader_common.h ${FASTCARS_TOOLS_DIR}/shader_common.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/mac/macho_tool.py
            ${FASTCARS_XR_BIN} ${FASTCARS_TOOLS_DIR}/XenosRecomp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/mac/macho_tool.py
            ${FASTCARS_DXC_DIR}/bin/arm64/dxc-macos ${FASTCARS_TOOLS_DIR}/dxc
    VERBATIM)

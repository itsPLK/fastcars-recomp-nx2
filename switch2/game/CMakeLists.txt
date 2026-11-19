# fastcars-recomp-nx2: one statically linked NRO (host + recompiled game code + SDK + plume + OunceVK).
#
# The recompiled game code is built like the release's fastcars_game library (fastcars/fs/fastcars_pch.h,
# freestanding, plus REX_GUEST_FOLD: no memory aliasing on Switch 2) from codegen output the owner
# made from their own discs (FASTCARS_GENERATED_DIR, outside the repo: never commit it). The host is
# fastcars/src in its FASTCARS_GAME_DLL form; host and game meet at link time instead of at load time.
#
# Prerequisites, then build (from the repo root; ../fastcars-mac holds the codegen output):
#   python3 switch2/game/native_shaders.py switch2/game/build/native_shaders     # on the Mac (DXC)
#   docker run --rm -v "$PWD:/repo" -v "$PWD/../fastcars-mac:/fastcars-mac" -w /repo fastcars-switch2 sh -c '
#     cmake -S switch2/game -B switch2/game/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#           -DCMAKE_TOOLCHAIN_FILE=/repo/switch2/cmake/switch2-clang.cmake -Wno-dev &&
#     cmake --build switch2/game/build'                      -> build/fastcars.nro
cmake_minimum_required(VERSION 3.25)
project(fastcars_switch2 LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 23)

set(FASTCARS_GENERATED_DIR "/fastcars-mac/build/generated/default" CACHE PATH
    "ReXGlue codegen output for the owner's discs (fastcars_recomp.*.cpp, fastcars_init.cpp)")
set(FASTCARS_NATIVE_SHADER_DIR "${CMAKE_BINARY_DIR}/native_shaders" CACHE PATH
    "Native renderer shader headers from switch2/game/native_shaders.py")
if(NOT EXISTS "${FASTCARS_GENERATED_DIR}/fastcars_init.cpp")
    message(FATAL_ERROR "No codegen output in FASTCARS_GENERATED_DIR (${FASTCARS_GENERATED_DIR})")
endif()
if(NOT EXISTS "${FASTCARS_NATIVE_SHADER_DIR}/native_blit_shaders.h")
    message(FATAL_ERROR "No native shader headers in ${FASTCARS_NATIVE_SHADER_DIR}: run switch2/game/native_shaders.py")
endif()
set(FASTCARS ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcars)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_subdirectory(../support support)
add_subdirectory(${REPO}/rexglue-sdk rexglue-sdk EXCLUDE_FROM_ALL)
add_subdirectory(${REPO}/thirdparty/plume plume EXCLUDE_FROM_ALL)
target_include_directories(plume BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../support/include-stubs)

#----------------------------------------------------------------------------------------------
# Host (fastcars/src, FASTCARS_GAME_DLL form)
#----------------------------------------------------------------------------------------------
add_library(fastcars_host OBJECT
    ${FASTCARS}/src/main.cpp
    ${FASTCARS}/src/game_bridge.cpp
    ${FASTCARS}/src/crash_handler.cpp
    ${FASTCARS}/src/cutscene_audio_fix.cpp
    ${FASTCARS}/src/frame_limit.cpp
    ${FASTCARS}/src/switch2_clocks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/splash.cpp
    ${FASTCARS}/src/wait_profile.cpp
    ${FASTCARS}/src/gpu_fence_wait.cpp
    ${FASTCARS}/src/native/stats.cpp
    ${FASTCARS}/src/native/capture.cpp
    ${FASTCARS}/src/native/d3d_probe.cpp
    ${FASTCARS}/src/native/d3d_hooks.cpp
    ${FASTCARS}/src/native/renderer.cpp
    ${FASTCARS}/src/native/shaders.cpp
    ${FASTCARS}/src/native/menu.cpp
    ${FASTCARS}/src/native/backend.cpp
    ${FASTCARS}/src/native/deferred_list.cpp
    ${FASTCARS}/src/native/textures.cpp
    ${FASTCARS}/src/native/write_watch.cpp
    ${FASTCARS}/src/native/shader_compiler.cpp
    ${FASTCARS}/src/native/touch_controls_switch2.cpp
)
target_compile_definitions(fastcars_host PRIVATE FASTCARS_GAME_DLL=1 REXGLUE_BUILD_CONFIG="Release")
# The D3D call probe (--fastcars_d3d_probe) wraps ~2,900 game functions; left out unless asked for.
option(FASTCARS_D3D_PROBE "Build the D3D call probe's wrappers (diagnostics)" OFF)
if(FASTCARS_D3D_PROBE)
    target_compile_definitions(fastcars_host PRIVATE FASTCARS_D3D_PROBE=1)
endif()
# fastcars_version.h (v<VERSION>-<commit>, for the shaders screen), refreshed on every build.
add_custom_target(fastcars_version ALL
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/make_version.sh ${CMAKE_CURRENT_SOURCE_DIR}/../..
            ${CMAKE_CURRENT_BINARY_DIR}/fastcars_version.h
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/fastcars_version.h VERBATIM)
add_dependencies(fastcars_host fastcars_version)
# game_title.h / game_title.txt: the game's title from the player's own disc (Disc 1's nxeart) for
# the start screen and the homebrew menu; the repository never contains it (disc_title.py).
set(FASTCARS_NXEART "" CACHE FILEPATH "The player's nxeart package (data/game/nxeart): the game's title")
if(FASTCARS_NXEART AND EXISTS "${FASTCARS_NXEART}")
    set(_fastcars_title_src "${FASTCARS_NXEART}")
else()
    set(_fastcars_title_src "-")
endif()
add_custom_target(fastcars_title ALL
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/disc_title.py ${_fastcars_title_src} ${CMAKE_CURRENT_BINARY_DIR}
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/game_title.h ${CMAKE_CURRENT_BINARY_DIR}/game_title.txt VERBATIM)
add_dependencies(fastcars_host fastcars_title)
# icon.jpg: the NRO icon from the same nxeart (make_icon.py; needs Pillow, else an existing icon.jpg
# is kept). make_nro.sh uses it when present.
if(FASTCARS_NXEART AND EXISTS "${FASTCARS_NXEART}")
    add_custom_target(fastcars_icon ALL
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/make_icon.py ${FASTCARS_NXEART} ${CMAKE_BINARY_DIR}/icon.jpg
        VERBATIM)
    add_dependencies(fastcars_host fastcars_icon)
endif()
target_include_directories(fastcars_host PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(fastcars_host PRIVATE ${FASTCARS} ${FASTCARS}/src ${FASTCARS_NATIVE_SHADER_DIR})
target_link_libraries(fastcars_host PRIVATE rex::runtime rex::ui rexgpu-xenos plume)

#----------------------------------------------------------------------------------------------
# fastcars_hooks.h: the guest functions the host replaces (names only, from the host's objects)
#----------------------------------------------------------------------------------------------
set(FASTCARS_INTERFACE_DIR ${CMAKE_BINARY_DIR}/game_interface)
add_custom_command(OUTPUT ${FASTCARS_INTERFACE_DIR}/fastcars_hooks.h
    COMMAND python3 ${REPO}/tools/game_dll/gen_interface.py /usr/bin
            ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/fastcars_host.dir ${FASTCARS_INTERFACE_DIR} - --switch2
    DEPENDS fastcars_host $<TARGET_OBJECTS:fastcars_host> ${REPO}/tools/game_dll/gen_interface.py
    VERBATIM)
# The generated sources include "fastcars_pch.h" etc. by quotes, which finds their own folder first:
# stage the prefix header and the hooks there, as the installer and build_game_dll.py do.
set(FASTCARS_STAGE_STAMP ${CMAKE_BINARY_DIR}/game_interface/staged.stamp)
add_custom_command(OUTPUT ${FASTCARS_STAGE_STAMP}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FASTCARS}/fs/fastcars_pch.h ${FASTCARS}/fs/layout_check.h
            ${FASTCARS_INTERFACE_DIR}/fastcars_hooks.h ${FASTCARS_GENERATED_DIR}
    COMMAND ${CMAKE_COMMAND} -E touch ${FASTCARS_STAGE_STAMP}
    DEPENDS ${FASTCARS_INTERFACE_DIR}/fastcars_hooks.h ${FASTCARS}/fs/fastcars_pch.h ${FASTCARS}/fs/layout_check.h
    VERBATIM)
add_custom_target(fastcars_game_staged DEPENDS ${FASTCARS_STAGE_STAMP})

#----------------------------------------------------------------------------------------------
# Recompiled game code: game_code/ (its own directory, for its own CMAKE_CXX_FLAGS)
#----------------------------------------------------------------------------------------------
add_subdirectory(game_code)

#----------------------------------------------------------------------------------------------
# The NRO
#----------------------------------------------------------------------------------------------
add_executable(fastcars_switch2)
rexglue_configure_target(fastcars_switch2)
target_link_libraries(fastcars_switch2 PRIVATE
    rex_switch2_support fastcars_host
    -Wl,--start-group fastcars_game rex::runtime rex::ui rexgpu-xenos plume -Wl,--end-group
    rex_switch2_vulkan nx)

# fastcars.nro: version = the commit, icon = build/icon.jpg when present (make_nro.sh).
add_custom_command(TARGET fastcars_switch2 POST_BUILD
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/make_nro.sh ${NACPTOOL} ${ELF2NRO} $<TARGET_FILE:fastcars_switch2>
            ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../..
    VERBATIM)

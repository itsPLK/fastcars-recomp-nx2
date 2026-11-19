# The recompiled game code (see ../CMakeLists.txt). Built freestanding like the release's
# fastcars_game: no C++ standard library. The Switch 2 toolchain names libstdc++'s include folders
# explicitly (-isystem, clang cannot find devkitA64's on its own), which -nostdinc++ does not
# undo, so they are taken out of this directory's flags: otherwise simde sees <cmath>.
string(REGEX REPLACE "-isystem [^ ]*/include/c\\+\\+/[^ ]*" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

file(GLOB FASTCARS_GAME_SOURCES ${FASTCARS_GENERATED_DIR}/fastcars_recomp.*.cpp)
list(APPEND FASTCARS_GAME_SOURCES ${FASTCARS_GENERATED_DIR}/fastcars_init.cpp ${FASTCARS}/fs/game_dll.cpp)
add_library(fastcars_game STATIC ${FASTCARS_GAME_SOURCES})
add_dependencies(fastcars_game fastcars_game_staged)
target_compile_definitions(fastcars_game PRIVATE REX_GUEST_FOLD=1)
# Guest memory access kinds (fastcars/fs/fastcars_pch.h; only codegen made with memory_classes names
# stack and in-loop accesses): stack plain unless FASTCARS_VOLATILE_STACK, everything else volatile
# unless FASTCARS_PLAIN_MEMORY (experimental). Loads inside loops stay volatile either way.
option(FASTCARS_VOLATILE_STACK "Keep r1-relative guest accesses volatile" OFF)
option(FASTCARS_PLAIN_MEMORY "Plain (non-volatile) guest memory accesses outside loops (experimental)" OFF)
if(FASTCARS_VOLATILE_STACK)
    target_compile_definitions(fastcars_game PRIVATE FASTCARS_VOLATILE_STACK=1)
endif()
if(FASTCARS_PLAIN_MEMORY)
    target_compile_definitions(fastcars_game PRIVATE FASTCARS_PLAIN_MEMORY=1)
endif()
target_compile_options(fastcars_game PRIVATE
    -nostdinc++ -nostdlibinc -fno-stack-protector -O2 -fno-exceptions -fno-rtti
    -fno-threadsafe-statics -fno-math-errno -w)
target_include_directories(fastcars_game SYSTEM PRIVATE ${FASTCARS}/fs/include ${REPO}/rexglue-sdk/thirdparty/simde)
target_include_directories(fastcars_game PRIVATE ${FASTCARS_GENERATED_DIR})


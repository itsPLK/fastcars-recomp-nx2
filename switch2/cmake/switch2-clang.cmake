# CMake toolchain: Switch 2 homebrew (libounce / kaiHEN) built with clang.
#
# The ReXGlue SDK and the recompiled game code need clang (18+); devkitA64 ships GCC. This file
# compiles with clang for aarch64-none-elf against devkitA64's newlib and libstdc++ headers and
# libounce, and links with devkitA64's g++ driver and libounce's switch.specs, as libounce's own
# rules do. Use it inside the fastcars-switch2 Docker image (switch2/docker):
#
#   cmake -S rexglue-sdk -B out/switch2 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=/repo/switch2/cmake/switch2-clang.cmake
#
# Thread-local storage: libounce (like libnx) keeps the thread pointer at
# *(tpidrro_el0 + 0x1F8) and devkitA64 GCC reads it through __aarch64_read_tp (-mtp=soft). Clang
# reads tpidr_el0. The TLS layout is the same, so switch2/support (rex_switch2_support) copies the
# thread pointer into tpidr_el0 on the main thread and at the start of every thread. Link it into
# every executable built with this toolchain.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(REX_SWITCH2 ON CACHE BOOL "Building for Switch 2 homebrew" FORCE)
set(NINTENDO_SWITCH TRUE)
set(NINTENDO_SWITCH2 TRUE)

if(DEFINED ENV{DEVKITPRO})
    set(DEVKITPRO "$ENV{DEVKITPRO}")
else()
    set(DEVKITPRO /opt/devkitpro)
endif()
set(DEVKITA64 "${DEVKITPRO}/devkitA64")
set(NX_ROOT "${DEVKITPRO}/libounce")
set(OUNCE_PORTLIBS "${DEVKITPRO}/portlibs/switch2")
set(NX_PORTLIBS "${DEVKITPRO}/portlibs/switch")
set(DKA64_SYSROOT "${DEVKITA64}/aarch64-none-elf")

# GCC version whose libstdc++ we compile against.
file(GLOB _dka64_cxx_versions LIST_DIRECTORIES true "${DKA64_SYSROOT}/include/c++/*")
list(SORT _dka64_cxx_versions COMPARE NATURAL ORDER DESCENDING)
list(GET _dka64_cxx_versions 0 DKA64_CXX_INCLUDE)

# CPU flags from libounce (cortex-a78c+crypto).
file(STRINGS "${NX_ROOT}/switch2_cpu.mk" _cpu REGEX "^SWITCH2_CPU[ \t]*:=")
string(REGEX REPLACE "^SWITCH2_CPU[ \t]*:=[ \t]*" "" SWITCH2_CPU "${_cpu}")

find_program(CMAKE_C_COMPILER NAMES clang-19 clang REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang++-19 clang++ REQUIRED)
set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
find_program(CMAKE_AR NAMES llvm-ar-19 llvm-ar REQUIRED)
find_program(CMAKE_RANLIB NAMES llvm-ranlib-19 llvm-ranlib REQUIRED)
find_program(CMAKE_NM NAMES llvm-nm-19 llvm-nm)
find_program(CMAKE_OBJCOPY NAMES llvm-objcopy-19 llvm-objcopy)
set(CMAKE_C_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_CXX_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_ASM_COMPILER_TARGET aarch64-none-elf)

set(_switch2_common
    "${SWITCH2_CPU} -fPIC -fno-omit-frame-pointer -ftls-model=local-exec -ffunction-sections -fdata-sections"
    "-D__SWITCH__ -D__SWITCH2__ -D_GNU_SOURCE --sysroot=${DKA64_SYSROOT}"
    "-isystem ${NX_ROOT}/include -isystem ${OUNCE_PORTLIBS}/include -isystem ${NX_PORTLIBS}/include")
string(JOIN " " _switch2_common ${_switch2_common})
set(CMAKE_C_FLAGS_INIT "${_switch2_common}")
set(CMAKE_ASM_FLAGS_INIT "${_switch2_common}")
# libstdc++ from devkitA64 (clang's bare-metal driver would look for libc++).
set(CMAKE_CXX_FLAGS_INIT "${_switch2_common} -nostdinc++ -isystem ${DKA64_CXX_INCLUDE} -isystem ${DKA64_CXX_INCLUDE}/aarch64-none-elf -isystem ${DKA64_CXX_INCLUDE}/backward")

# Link with devkitA64's g++ driver: newlib, libstdc++, libgcc and libounce's crt0 and linker
# script come from switch.specs, exactly as for GCC-built homebrew. Objects are clang's.
set(DKA64_CXX "${DEVKITA64}/bin/aarch64-none-elf-g++")
set(SWITCH2_LINK_FLAGS "-specs=${NX_ROOT}/switch.specs ${SWITCH2_CPU} -fPIE -L${NX_ROOT}/lib -L${OUNCE_PORTLIBS}/lib -L${NX_PORTLIBS}/lib")
set(CMAKE_C_LINK_EXECUTABLE   "${DKA64_CXX} ${SWITCH2_LINK_FLAGS} <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE "${DKA64_CXX} ${SWITCH2_LINK_FLAGS} <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

# Compiler checks: no linking (the bare-metal driver cannot link a test program by itself).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(CMAKE_FIND_ROOT_PATH "${NX_ROOT}" "${OUNCE_PORTLIBS}" "${NX_PORTLIBS}" "${DKA64_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ELF2NRO "${DEVKITPRO}/tools/bin/elf2nro")
set(NACPTOOL "${DEVKITPRO}/tools/bin/nacptool")

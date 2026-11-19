# rex_switch2_support: runtime glue every Switch 2 executable built with switch2-clang.cmake links.
#   tls.c   thread pointer in tpidr_el0 for clang's TLS code (main thread and every new thread)
#   pread.c pread/pwrite (absent from libounce)
cmake_minimum_required(VERSION 3.25)
# OBJECT: the wrappers are referenced only from libounce.a, which links after us, so an archive
# member would never be pulled in.
add_library(rex_switch2_support OBJECT tls.c pread.c pthread_detach.c crash_hooks.cpp)
target_link_options(rex_switch2_support INTERFACE
    -Wl,--wrap=__libnx_init_thread
    -Wl,--wrap=threadCreate
    -Wl,--wrap=pthread_detach
    -Wl,--wrap=abort
    -Wl,--wrap=__cxa_throw)

# rex_switch2_vulkan: OunceVK for executables that also contain volk (plume). See
# localize_ouncevk.cmake; the driver's one exported entry is rex_ounce_vkGetInstanceProcAddr.
set(REX_OUNCEVK_IN "$ENV{DEVKITPRO}/portlibs/switch2/lib/libouncevk.a")
if(NOT EXISTS "${REX_OUNCEVK_IN}")
    set(REX_OUNCEVK_IN "/opt/devkitpro/portlibs/switch2/lib/libouncevk.a")
endif()
set(REX_OUNCEVK_OUT "${CMAKE_CURRENT_BINARY_DIR}/libouncevk_rex.a")
add_custom_command(OUTPUT "${REX_OUNCEVK_OUT}"
    COMMAND ${CMAKE_COMMAND}
            -DNM=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm
            -DOBJCOPY=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objcopy
            -DIN=${REX_OUNCEVK_IN} -DOUT=${REX_OUNCEVK_OUT}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/localize_ouncevk.cmake
    DEPENDS "${REX_OUNCEVK_IN}" ${CMAKE_CURRENT_SOURCE_DIR}/localize_ouncevk.cmake
    VERBATIM)
add_custom_target(rex_ouncevk_localized DEPENDS "${REX_OUNCEVK_OUT}")
add_library(rex_switch2_vulkan INTERFACE)
add_dependencies(rex_switch2_vulkan rex_ouncevk_localized)
target_link_libraries(rex_switch2_vulkan INTERFACE "${REX_OUNCEVK_OUT}" stdc++ m)

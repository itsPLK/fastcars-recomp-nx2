# Makes a copy of OunceVK's libouncevk.a whose Vulkan entry points are local symbols, except
# vkGetInstanceProcAddr, renamed rex_ounce_vkGetInstanceProcAddr.
#
# Why: plume uses volk, which defines global function-pointer variables named like the Vulkan
# commands (vkCreateInstance, ...). OunceVK is a static library whose one object defines all 249
# commands as functions, so linking both clashes (or binds calls to the wrong symbol). With this
# copy the only global vk* names in a Switch 2 executable are volk's, and everything (volk, the
# SDK's function tables) reaches the driver through rex_ounce_vkGetInstanceProcAddr, as on desktop
# where the loader is opened at run time. Safe because libouncevk.a is one prelinked object with no
# undefined vk* references.
#   cmake -DNM=... -DOBJCOPY=... -DIN=libouncevk.a -DOUT=libouncevk_rex.a -P localize_ouncevk.cmake
execute_process(COMMAND ${NM} -g --defined-only ${IN}
                OUTPUT_VARIABLE symbols RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nm ${IN} failed")
endif()
string(REGEX MATCHALL "[^\n]+" lines "${symbols}")
set(localize "")
set(count 0)
foreach(line IN LISTS lines)
    if(line MATCHES " [A-Za-z] (vk[A-Z][A-Za-z0-9_]*)$")
        set(name "${CMAKE_MATCH_1}")
        if(NOT name STREQUAL "vkGetInstanceProcAddr")
            string(APPEND localize "${name}\n")
            math(EXPR count "${count} + 1")
        endif()
    endif()
endforeach()
if(count LESS 100)
    message(FATAL_ERROR "only ${count} Vulkan symbols found in ${IN}: wrong library?")
endif()
file(WRITE ${OUT}.localize "${localize}")
execute_process(COMMAND ${OBJCOPY} --localize-symbols=${OUT}.localize
                        --redefine-sym vkGetInstanceProcAddr=rex_ounce_vkGetInstanceProcAddr
                        ${IN} ${OUT}
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "objcopy ${IN} failed")
endif()
message(STATUS "libouncevk_rex.a: ${count} Vulkan symbols localized")

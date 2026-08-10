cmake_minimum_required(VERSION 3.10)
project(tsanpr_min LANGUAGES C CXX)

# ---- anpr: the sample, no external libraries ---------------------------------
# The engine is loaded at runtime via LoadLibrary/dlopen, and image decoding uses
# the bundled single-header stb_image.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(anpr src/anpr.c src/tsanpr.c)
target_include_directories(anpr PRIVATE src)
if(UNIX)
    target_link_libraries(anpr PRIVATE dl)
endif()

# ---- x86emu: the emulator anpr.exe can be run under ---------------------------
# A copy of github.com/yomei-o/x86_emu_cpp with the hooks this engine needs.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB EMU_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/emu/src/*.cpp)
add_executable(x86emu ${EMU_SOURCES})
target_include_directories(x86emu PRIVATE emu/src)
if(WIN32)
    # WMI and the interface table, so a guest asking what machine it runs on gets
    # the host's own answer.  These are OS libraries, not third-party ones.
    target_link_libraries(x86emu PRIVATE ole32 oleaut32 wbemuuid iphlpapi setupapi hid)
endif()

# Both go next to engine/ and img/ regardless of build configuration, because
# anpr resolves those paths relative to its own location.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
foreach(target anpr x86emu)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

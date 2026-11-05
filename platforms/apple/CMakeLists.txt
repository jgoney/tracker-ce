cmake_minimum_required(VERSION 3.20)
project(TrackerApple LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo) # NDEBUG, like the hardware builds
endif()

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
get_filename_component(REPO_ROOT ${REPO_ROOT} ABSOLUTE)
set(CORE_DIR ${REPO_ROOT}/core)

# Version defines come from the same place as the hardware builds (version.mk).
# BUILD_TAG is not needed by core sources.
set(FW_EDITION_NAME CE)
file(STRINGS ${REPO_ROOT}/version.mk _ver_lines REGEX "^ORIGINAL_VER_(MAJOR|MINOR|PATCH|BETA) *:=")
foreach(_l IN LISTS _ver_lines)
    if(_l MATCHES "ORIGINAL_VER_([A-Z]+) *:= *([0-9]+)")
        set(FW_VER_${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
    endif()
endforeach()
file(STRINGS ${REPO_ROOT}/version.mk _ed_line REGEX "^EDITION_NAME *:=")
if(_ed_line MATCHES ":= *([A-Za-z0-9_]+)")
    set(FW_EDITION_NAME ${CMAKE_MATCH_1})
endif()

set(CORE_DIRS source libs interface effector FileManager Exporter AudioEngine synth)
set(CORE_CPP_SRCS)
foreach(d IN LISTS CORE_DIRS)
    file(GLOB_RECURSE _s CONFIGURE_DEPENDS ${CORE_DIR}/${d}/*.cpp)
    list(APPEND CORE_CPP_SRCS ${_s})
endforeach()
list(REMOVE_ITEM CORE_CPP_SRCS ${CORE_DIR}/effector/ptfx_loader.cpp)
file(GLOB_RECURSE CORE_C_SRCS CONFIGURE_DEPENDS ${CORE_DIR}/libs/*.c)

add_library(TrackerCore STATIC ${CORE_CPP_SRCS} ${CORE_C_SRCS})

set(CORE_INCLUDES
    include config source synth libs libs/Audio libs/ElapsedMillis libs/SerialFlash libs/SPI
    libs/Audio/utility libs/FT812 libs/fonts libs/NewDisplay interface effector FileManager
    Exporter AudioEngine libs/Audio/Polyverb AudioEngine/ConstantValues AudioEngine/MasterEnhancer)
foreach(i IN LISTS CORE_INCLUDES)
    target_include_directories(TrackerCore PUBLIC ${CORE_DIR}/${i})
endforeach()
target_include_directories(TrackerCore PUBLIC
    ${REPO_ROOT}/util/include ${REPO_ROOT}/util/3rdparty ${REPO_ROOT}/synth/include)

target_compile_definitions(TrackerCore PUBLIC
    PLATFORM_APPLE PLATFORM_IMX FEATURE_SYNTH USB_MIDI LAYOUT_US_ENGLISH
    AUDIO_PERFORMANCE_INFO_ENABLED MEMORY_USAGE_INFO_ENABLED
    FW_EDITION_NAME="${FW_EDITION_NAME}"
    FW_VER_MAJOR=${FW_VER_MAJOR} FW_VER_MINOR=${FW_VER_MINOR}
    FW_VER_PATCH=${FW_VER_PATCH} FW_VER_BETA=${FW_VER_BETA}
    # util/3rdparty/tiny/optional.h: arm64 pointers are 64-bit with a high non-canonical
    # range like x64, and libc++ has no _GLIBCXX_THROW_OR_ABORT (we build -fno-exceptions).
    TINY_OPTIONAL_x64)
# (function-like macro: has to go through options, add_compile_definitions drops it)
target_compile_options(TrackerCore PUBLIC "-D_GLIBCXX_THROW_OR_ABORT(x)=__builtin_abort()")

target_compile_options(TrackerCore PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-rtti;-fno-exceptions;SHELL:-include cstdint;SHELL:-include cassert>")

# --- Tests (Catch2 v3, vendored source tree) ---
file(GLOB_RECURSE CATCH_SRCS CONFIGURE_DEPENDS ${REPO_ROOT}/util/3rdparty/catch2/*.cpp)
list(FILTER CATCH_SRCS EXCLUDE REGEX "/benchmark/")
add_library(catch2_vendored STATIC ${CATCH_SRCS})
target_include_directories(catch2_vendored PUBLIC ${REPO_ROOT}/util/3rdparty)
target_compile_options(catch2_vendored PRIVATE -std=gnu++20 -w)

# EXCLUDE_FROM_ALL until the platform layer supplies the remaining symbols.
add_executable(apple_tests EXCLUDE_FROM_ALL tests/test_smoke.cpp)
target_compile_options(apple_tests PRIVATE -std=gnu++20 "SHELL:-include cstdint")
target_link_libraries(apple_tests PRIVATE TrackerCore catch2_vendored)

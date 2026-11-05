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

# dspinst.h host intrinsics: header + Catch2 only, built under UBSan (no recovery).
add_executable(dsp_tests tests/test_dspinst.cpp)
target_include_directories(dsp_tests PRIVATE ${CORE_DIR}/libs/Audio/utility)
target_compile_definitions(dsp_tests PRIVATE PLATFORM_APPLE)
target_compile_options(dsp_tests PRIVATE -std=gnu++20 -fsanitize=undefined -fno-sanitize-recover=all)
target_link_options(dsp_tests PRIVATE -fsanitize=undefined)
target_link_libraries(dsp_tests PRIVATE catch2_vendored)

# --- fmt (vendored sources; core and the PolySynth SDK headers call fmt::format) ---
add_library(fmt_vendored STATIC ${REPO_ROOT}/util/3rdparty/fmt/format.cc)
target_include_directories(fmt_vendored PUBLIC ${REPO_ROOT}/util/3rdparty)
target_compile_options(fmt_vendored PRIVATE -std=gnu++20 -w)
target_link_libraries(TrackerCore PUBLIC fmt_vendored)

# --- Silent PolySynth / ptfx stand-ins (see synth_stub/) ---
add_library(PolySynthStub STATIC
    synth_stub/PolySynthStub.cpp
    synth_stub/PtfxStub.cpp)
target_link_libraries(PolySynthStub PUBLIC TrackerCore)
target_compile_options(PolySynthStub PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-rtti;-fno-exceptions;SHELL:-include cstdint;SHELL:-include cassert>")

add_executable(synth_stub_tests tests/test_synth_stub.cpp)
target_compile_options(synth_stub_tests PRIVATE -std=gnu++20 "SHELL:-include cstdint")
target_link_libraries(synth_stub_tests PRIVATE PolySynthStub TrackerApple TrackerCore catch2_vendored)

# --- Apple HAL (PlatformApple + POSIX FileSystem + virtual sector map + display model) ---
add_library(TrackerApple STATIC
    hal/FileSystemPosix.cpp
    hal/VirtualSectorMap.cpp
    hal/PlatformApple.cpp
    hal/TimerApple.cpp
    hal/InputApple.cpp
    hal/StubsApple.cpp
    hal/interrupts_apple.cpp
    runtime/AudioRuntime.cpp
    runtime/CoreAudioOutput.cpp
    display/Ft812Model.cpp
    display/CoproDecoder.cpp
    display/DlDecoder.cpp
    display/CpuRaster.cpp
    display/Bitmaps.cpp
    display/ImageDecode.cpp)
target_include_directories(TrackerApple PUBLIC hal display runtime)
# ImageIO/CoreGraphics: CMD_LOADIMAGE JPEG/PNG decoding only (also available on iOS).
target_link_libraries(TrackerApple PUBLIC TrackerCore
    "-framework ImageIO" "-framework CoreGraphics" "-framework CoreFoundation"
    "-framework CoreAudio" "-framework AudioToolbox")
target_compile_options(TrackerApple PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-rtti;-fno-exceptions;-Wall;-Wextra>")

# operator new hook for AudioRuntime's render-thread allocation guard. Linked only into binaries
# that want it (a test may define its own operator new instead).
add_library(TrackerAllocGuard OBJECT runtime/AllocGuardNew.cpp)
target_link_libraries(TrackerAllocGuard PUBLIC TrackerCore)
target_include_directories(TrackerAllocGuard PRIVATE hal display runtime)
target_compile_options(TrackerAllocGuard PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-rtti;-fno-exceptions;-Wall;-Wextra>")

# AudioRuntime (tracker + render threads) against a fake Tracker.
add_executable(runtime_tests tests/test_runtime.cpp $<TARGET_OBJECTS:TrackerAllocGuard>)
target_compile_options(runtime_tests PRIVATE -std=gnu++20 "SHELL:-include cstdint")
target_link_libraries(runtime_tests PRIVATE TrackerApple TrackerCore catch2_vendored)

# Core Audio output against real devices (tag [hardware]; label "hardware", needs an output device).
add_executable(coreaudio_tests tests/test_coreaudio.cpp)
target_compile_options(coreaudio_tests PRIVATE -std=gnu++20 "SHELL:-include cstdint")
target_link_libraries(coreaudio_tests PRIVATE TrackerApple TrackerCore catch2_vendored)

# Whole-core headless boot + platform tests (smoke, boot, input, config, synth round trip).
add_executable(apple_tests tests/test_smoke.cpp tests/test_boot.cpp)
target_compile_options(apple_tests PRIVATE -std=gnu++20 "SHELL:-include cstdint")
target_link_libraries(apple_tests PRIVATE PolySynthStub TrackerApple TrackerCore catch2_vendored)

# Headless runner: boots the core, dumps the last frame (--screenshot), runs it under AudioRuntime
# (--audio-seconds); later --tone, --metal.
add_executable(trackerce-headless tools/trackerce_headless.cpp $<TARGET_OBJECTS:TrackerAllocGuard>)
target_compile_options(trackerce-headless PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-rtti;-fno-exceptions;-Wall;-Wextra;SHELL:-include cstdint>")
target_link_libraries(trackerce-headless PRIVATE PolySynthStub TrackerApple TrackerCore)

# FT812 model: coprocessor execution, display-list decoding, reference rasteriser.
add_executable(display_tests tests/test_display_decode.cpp)
target_compile_options(display_tests PRIVATE -std=gnu++20 "SHELL:-include cstdint")
target_link_libraries(display_tests PRIVATE TrackerApple TrackerCore catch2_vendored)

add_executable(filesystem_tests tests/test_filesystem.cpp)
target_compile_options(filesystem_tests PRIVATE -std=gnu++20 "SHELL:-include cstdint")
target_link_libraries(filesystem_tests PRIVATE TrackerApple TrackerCore catch2_vendored)

# --- ctest ---
enable_testing()
add_test(NAME dsp_tests COMMAND dsp_tests)
add_test(NAME synth_stub_tests COMMAND synth_stub_tests)
add_test(NAME filesystem_tests COMMAND filesystem_tests)
add_test(NAME apple_tests COMMAND apple_tests)
# Wall-clock boot to the main UI (~4 s), alone in its own process; hidden from apple_tests.
add_test(NAME apple_boot_frames COMMAND apple_tests "boot produces frames")
add_test(NAME display_tests COMMAND display_tests)
add_test(NAME runtime_tests COMMAND runtime_tests)
# Needs an audio output device: run with `ctest -L hardware`, skip with `ctest -LE hardware`.
add_test(NAME coreaudio_tests COMMAND coreaudio_tests "[hardware]")
set_tests_properties(coreaudio_tests PROPERTIES LABELS hardware)

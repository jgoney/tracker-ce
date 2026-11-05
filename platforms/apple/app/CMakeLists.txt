enable_language(Swift)

add_library(TrackerBridge STATIC Bridge.mm)
target_include_directories(TrackerBridge PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(TrackerBridge PRIVATE
    -std=gnu++20 -fobjc-arc -fno-rtti -fno-exceptions -Wall -Wextra "SHELL:-include cstdint")
target_link_libraries(TrackerBridge PUBLIC PolySynthStub TrackerMetal TrackerApple TrackerCore
    "-framework Foundation" "-framework Metal" "-framework CoreGraphics")

set(APP_NAME "Tracker CE (unofficial)")
add_executable(TrackerCE MACOSX_BUNDLE
    TrackerCEApp.swift TrackerView.swift KeyMap.swift Settings.swift
    $<TARGET_OBJECTS:TrackerAllocGuard>)
set_target_properties(TrackerCE PROPERTIES
    OUTPUT_NAME "${APP_NAME}"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_LIST_DIR}/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME "${APP_NAME}"
    MACOSX_BUNDLE_GUIDE_IDENTIFIER "local.trackerce.mac"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
    MACOSX_BUNDLE_BUNDLE_VERSION "1")
# CMake does not pass the deployment target to swiftc/the Swift link: do it explicitly.
if(CMAKE_OSX_ARCHITECTURES)
    list(GET CMAKE_OSX_ARCHITECTURES 0 _swift_arch)
else()
    set(_swift_arch ${CMAKE_HOST_SYSTEM_PROCESSOR})
endif()
set(_swift_target ${_swift_arch}-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET})
target_compile_options(TrackerCE PRIVATE
    -target ${_swift_target} -parse-as-library -swift-version 5
    "SHELL:-import-objc-header ${CMAKE_CURRENT_LIST_DIR}/Bridge.h")
target_link_libraries(TrackerCE PRIVATE TrackerBridge c++
    "-framework SwiftUI" "-framework AppKit" "-framework MetalKit")
target_link_options(TrackerCE PRIVATE -target ${_swift_target})
add_dependencies(TrackerCE TrackerBridge)

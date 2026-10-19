# rexglue_openxr.cmake — the Khronos OpenXR loader (Apache-2.0) for the
# OpenXR output (src/ui/vulkan/vulkan_openxr.cpp), which shows the title in a
# headset through the player's OpenXR runtime.
#
# Off by default. REXGLUE_OPENXR_SDK_DIR points at a checkout of
# github.com/KhronosGroup/OpenXR-SDK (a release tag: its headers and loader
# sources are pregenerated, so no Python is needed). The loader is built from
# that source as a static library, so no DLL ships beside the game.
#
# Sets REXGLUE_OPENXR_TARGET when available.

option(REXGLUE_OPENXR "Build the OpenXR output (needs REXGLUE_OPENXR_SDK_DIR)" OFF)
set(REXGLUE_OPENXR_SDK_DIR "" CACHE PATH "Checkout of github.com/KhronosGroup/OpenXR-SDK")

set(REXGLUE_OPENXR_TARGET "" CACHE INTERNAL "")
if(NOT REXGLUE_OPENXR OR NOT REXGLUE_USE_VULKAN)
    return()
endif()
if(NOT EXISTS "${REXGLUE_OPENXR_SDK_DIR}/include/openxr/openxr.h" OR
   NOT EXISTS "${REXGLUE_OPENXR_SDK_DIR}/src/loader/CMakeLists.txt")
    message(WARNING "REXGLUE_OPENXR_SDK_DIR (${REXGLUE_OPENXR_SDK_DIR}) is not an "
        "OpenXR-SDK checkout; the OpenXR output is left out.")
    return()
endif()

set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
add_subdirectory("${REXGLUE_OPENXR_SDK_DIR}" "${CMAKE_BINARY_DIR}/openxr-sdk" EXCLUDE_FROM_ALL)
if(NOT TARGET openxr_loader)
    message(WARNING "The OpenXR SDK did not define openxr_loader; the OpenXR output is left out.")
    return()
endif()
set(REXGLUE_OPENXR_TARGET openxr_loader CACHE INTERNAL "")
set(REXGLUE_OPENXR_INCLUDE_DIR "${REXGLUE_OPENXR_SDK_DIR}/include" CACHE INTERNAL "")

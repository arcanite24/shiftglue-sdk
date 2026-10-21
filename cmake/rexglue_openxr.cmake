# rexglue_openxr.cmake — the Khronos OpenXR loader (Apache-2.0) for the
# OpenXR output (src/ui/vulkan/vulkan_openxr.cpp), which shows the title in a
# headset through the player's OpenXR runtime.
#
# On by default on 64-bit Windows with Vulkan; the host still decides at run
# time whether to start a VR session (xr_enabled, off by default). The loader
# is built from the OpenXR-SDK release source as a static library, so no DLL
# ships beside the game. That source is downloaded here, at configure time,
# from the pinned github.com/KhronosGroup/OpenXR-SDK release tag and checked
# by SHA-256 (its headers and loader sources are pregenerated, so no Python
# is needed); REXGLUE_OPENXR_SDK_DIR may point at a local checkout instead.
# A failed download only leaves the OpenXR output out; it never stops the
# configure.
#
# Sets REXGLUE_OPENXR_TARGET and REXGLUE_OPENXR_INCLUDE_DIR when available.

set(_rexglue_openxr_default OFF)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_rexglue_openxr_default ON)
endif()
option(REXGLUE_OPENXR "Build the OpenXR output (VR headsets)" ${_rexglue_openxr_default})
set(REXGLUE_OPENXR_SDK_DIR "" CACHE PATH
    "Local checkout of github.com/KhronosGroup/OpenXR-SDK (empty: download the pinned release)")

set(REXGLUE_OPENXR_TARGET "" CACHE INTERNAL "")
if(NOT REXGLUE_OPENXR OR NOT REXGLUE_USE_VULKAN)
    return()
endif()

# OpenXR SDK 1.1.63 (release-1.1.63, f2448a8797c85814aa892efc1ab8707900fbcc78).
set(_rexglue_openxr_version 1.1.63)
set(_rexglue_openxr_sha256 5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f)

set(_rexglue_openxr_dir "${REXGLUE_OPENXR_SDK_DIR}")
if(NOT _rexglue_openxr_dir)
    set(_rexglue_openxr_dir "${CMAKE_BINARY_DIR}/openxr-sdk-${_rexglue_openxr_version}")
    if(NOT EXISTS "${_rexglue_openxr_dir}/src/loader/CMakeLists.txt")
        set(_archive "${CMAKE_BINARY_DIR}/openxr-sdk-${_rexglue_openxr_version}.tar.gz")
        # STATUS alone: with EXPECTED_HASH a failed download (offline) stops
        # the configure with "cannot compute hash" before STATUS is read. The
        # hash is checked below instead.
        file(DOWNLOAD
            "https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-${_rexglue_openxr_version}.tar.gz"
            "${_archive}" STATUS _status TLS_VERIFY ON)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            list(GET _status 1 _message)
            message(WARNING "The OpenXR SDK ${_rexglue_openxr_version} download failed "
                "(${_message}); the OpenXR output is left out.")
            file(REMOVE "${_archive}")
            return()
        endif()
        file(SHA256 "${_archive}" _rexglue_openxr_actual_sha256)
        if(NOT _rexglue_openxr_actual_sha256 STREQUAL _rexglue_openxr_sha256)
            message(WARNING "The OpenXR SDK ${_rexglue_openxr_version} download has SHA-256 "
                "${_rexglue_openxr_actual_sha256}, not ${_rexglue_openxr_sha256}; the OpenXR "
                "output is left out.")
            file(REMOVE "${_archive}")
            return()
        endif()
        set(_extract "${CMAKE_BINARY_DIR}/openxr-sdk-extract")
        file(REMOVE_RECURSE "${_extract}")
        file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_extract}")
        file(REMOVE_RECURSE "${_rexglue_openxr_dir}")
        file(RENAME "${_extract}/OpenXR-SDK-release-${_rexglue_openxr_version}"
            "${_rexglue_openxr_dir}")
        file(REMOVE_RECURSE "${_extract}")
        file(REMOVE "${_archive}")
    endif()
endif()
if(NOT EXISTS "${_rexglue_openxr_dir}/include/openxr/openxr.h" OR
   NOT EXISTS "${_rexglue_openxr_dir}/src/loader/CMakeLists.txt")
    message(WARNING "${_rexglue_openxr_dir} is not an OpenXR-SDK release; the OpenXR output "
        "is left out.")
    return()
endif()

set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
add_subdirectory("${_rexglue_openxr_dir}" "${CMAKE_BINARY_DIR}/openxr-sdk" EXCLUDE_FROM_ALL)
if(NOT TARGET openxr_loader)
    message(WARNING "The OpenXR SDK did not define openxr_loader; the OpenXR output is left out.")
    return()
endif()
set(REXGLUE_OPENXR_TARGET openxr_loader CACHE INTERNAL "")
set(REXGLUE_OPENXR_INCLUDE_DIR "${_rexglue_openxr_dir}/include" CACHE INTERNAL "")

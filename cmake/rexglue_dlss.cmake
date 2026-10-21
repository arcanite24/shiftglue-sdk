# rexglue_dlss.cmake — NVIDIA's NGX headers, loader library and DLSS runtime
# for the DLSS provider (src/ui/vulkan/vulkan_dlss_upscaler.cpp).
#
# On by default on 64-bit Windows: NVIDIA approved shipping DLSS with the
# game. Nothing of the SDK is committed (the RTX SDK licence must not be
# made subject to an open-source licence); the headers, the static loader
# and the DLSS runtime (nvngx_dlss.dll) are downloaded here, at configure
# time, from the pinned github.com/NVIDIA/DLSS commit and checked by
# SHA-256. The game stages the runtime and NVIDIA's licence in
# upscalers/dlss/<version>/ beside the executable (rexglue_helpers.cmake),
# where the provider looks first; a newer version a player puts in its own
# folder there wins.
#
# REXGLUE_DLSS_SDK_DIR may point at a local checkout of that repository
# instead of downloading.
#
# Sets REXGLUE_DLSS_INCLUDE_DIR and REXGLUE_DLSS_LIBRARY when available, and
# REXGLUE_DLSS_RUNTIME_DIR (nvngx_dlss.dll and LICENSE.txt) with
# REXGLUE_DLSS_VERSION when the runtime is too.

# Renamed from REXGLUE_DLSS (off by default) so build folders configured
# before keep no stale OFF.
option(REXGLUE_DLSS_PROVIDER "Build the DLSS provider and stage NVIDIA's DLSS runtime" ON)
set(REXGLUE_DLSS_SDK_DIR "" CACHE PATH "Local checkout of github.com/NVIDIA/DLSS")

set(REXGLUE_DLSS_INCLUDE_DIR "" CACHE INTERNAL "")
set(REXGLUE_DLSS_LIBRARY "" CACHE INTERNAL "")
set(REXGLUE_DLSS_RUNTIME_DIR "" CACHE INTERNAL "")
set(REXGLUE_DLSS_VERSION "310.9.16" CACHE INTERNAL "")
if(NOT REXGLUE_DLSS_PROVIDER OR NOT REXGLUE_USE_VULKAN OR NOT WIN32 OR
   NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    return()
endif()

# DLSS 310.9.16.
set(_rexglue_dlss_commit 9981c6350dc58c0ce21f024fd65e31e851c31689)
set(_rexglue_dlss_files
    "include/nvsdk_ngx_defs.h|ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"
    "include/nvsdk_ngx_defs_vk.h|0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031"
    "include/nvsdk_ngx_params.h|943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"
    "include/nvsdk_ngx_vk.h|2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d"
    "include/nvsdk_ngx_helpers_vk.h|c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623"
    "lib/Windows_x86_64/x64/nvsdk_ngx_d.lib|bc43df9ec032004fde681d3e3afd01cde239e03a0125b822e88674c84287f99c")
set(_rexglue_dlss_runtime_files
    "lib/Windows_x86_64/rel/nvngx_dlss.dll|d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"
    "LICENSE.txt|d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0")

if(REXGLUE_DLSS_SDK_DIR)
    set(_rexglue_dlss_dir "${REXGLUE_DLSS_SDK_DIR}")
else()
    set(_rexglue_dlss_dir "${CMAKE_BINARY_DIR}/nvidia-dlss")
    set(_rexglue_dlss_url
        "https://raw.githubusercontent.com/NVIDIA/DLSS/${_rexglue_dlss_commit}")
endif()

# The SHA-256 of a file as the repository stores it: text files hash with
# LF line endings, so a Windows checkout made with core.autocrlf matches.
function(_rexglue_dlss_hash path out)
    file(SHA256 "${path}" _hash)
    if(path MATCHES "\\.(h|txt)$")
        file(READ "${path}" _text)
        string(REPLACE "\r\n" "\n" _text "${_text}")
        string(SHA256 _hash "${_text}")
    endif()
    set(${out} ${_hash} PARENT_SCOPE)
endfunction()

# Sets <ok> to whether every "<path>|<sha256>" entry is present with the
# pinned hash, downloading what is missing or different. A failure only
# leaves DLSS out; it never stops the configure.
function(_rexglue_dlss_fetch ok)
    set(${ok} OFF PARENT_SCOPE)
    foreach(_entry IN LISTS ARGN)
        string(REPLACE "|" ";" _entry "${_entry}")
        list(GET _entry 0 _file)
        list(GET _entry 1 _hash)
        set(_path "${_rexglue_dlss_dir}/${_file}")
        if(EXISTS "${_path}")
            _rexglue_dlss_hash("${_path}" _actual)
            if(_actual STREQUAL _hash)
                continue()
            endif()
            if(REXGLUE_DLSS_SDK_DIR)
                message(WARNING "${_path} is not the pinned DLSS ${REXGLUE_DLSS_VERSION} file.")
                return()
            endif()
        elseif(REXGLUE_DLSS_SDK_DIR)
            message(WARNING "${_path} is missing.")
            return()
        endif()
        file(DOWNLOAD "${_rexglue_dlss_url}/${_file}" "${_path}" STATUS _status TLS_VERIFY ON)
        list(GET _status 0 _code)
        set(_actual "")
        if(_code EQUAL 0)
            _rexglue_dlss_hash("${_path}" _actual)
        endif()
        if(NOT _actual STREQUAL _hash)
            file(REMOVE "${_path}")
            message(WARNING "Could not download ${_file} from NVIDIA/DLSS "
                "(${_status}; SHA-256 ${_actual}).")
            return()
        endif()
    endforeach()
    set(${ok} ON PARENT_SCOPE)
endfunction()

_rexglue_dlss_fetch(_rexglue_dlss_ok ${_rexglue_dlss_files})
if(NOT _rexglue_dlss_ok)
    message(WARNING "The DLSS provider is left out.")
    return()
endif()
set(REXGLUE_DLSS_INCLUDE_DIR "${_rexglue_dlss_dir}/include" CACHE INTERNAL "")
set(REXGLUE_DLSS_LIBRARY "${_rexglue_dlss_dir}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib"
    CACHE INTERNAL "")

# The runtime is staged beside the game; without it DLSS is listed as
# unavailable until a player puts nvngx_dlss.dll in upscalers/dlss/<version>/.
_rexglue_dlss_fetch(_rexglue_dlss_ok ${_rexglue_dlss_runtime_files})
if(_rexglue_dlss_ok)
    set(_rexglue_dlss_runtime "${CMAKE_BINARY_DIR}/nvidia-dlss-runtime")
    file(COPY "${_rexglue_dlss_dir}/lib/Windows_x86_64/rel/nvngx_dlss.dll"
              "${_rexglue_dlss_dir}/LICENSE.txt"
         DESTINATION "${_rexglue_dlss_runtime}")
    set(REXGLUE_DLSS_RUNTIME_DIR "${_rexglue_dlss_runtime}" CACHE INTERNAL "")
else()
    message(WARNING "The DLSS runtime is not staged; DLSS shows as unavailable.")
endif()

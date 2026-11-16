# Builds AMD's FidelityFX API library for Vulkan (amd_fidelityfx_vk: FSR 2.3,
# the FSR 3.1 upscaler and FSR 3.1 frame generation) where AMD ships no signed runtime: Linux, Android
# and macOS. The provider (src/ui/vulkan/vulkan_ffx_upscaler.cpp) loads it
# at run time from upscalers/fidelityfx/<version>/ beside the game (from the
# package's libraries on Android), so this is a separate project:
#
#   cmake -S thirdparty/fidelityfx-vk -B <dir> -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build <dir> --target amd_fidelityfx_vk
#
# Android adds the NDK toolchain (-DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/
# android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=33).
#
# The SDK compiles its shader permutations only with the Windows
# FidelityFX_SC.exe, so vk-permutations.tar.gz holds its Vulkan output for
# the pinned commit (generated on Windows with the ffx_shader_permutations_vk
# target, configured with FFX_API_ENABLE_FRAMEGEN_PROVIDER=ON: FSR 2, the
# FSR 3 upscaler, frame interpolation and optical flow; .d files left out),
# checked by hash here.
#
# Frame generation is the API's frame generation provider, which the game
# dispatches itself (src/ui/vulkan/vulkan_ffx_frame_generator.cpp). AMD's
# replacement swapchain is Windows-only code and is not built.
cmake_minimum_required(VERSION 3.21)
project(rexglue_fidelityfx_vk C CXX)

set(REXGLUE_FFX_COMMIT eee08db1688ac3d1275a70b728f4a8ba22914213)
set(REXGLUE_FFX_PERMUTATIONS_SHA256
    71dae8a59dfffb66f0f8bae0016df8ed63cbee5a5cbb75a6522b6efd9900d92f)
set(REXGLUE_FFX_SOURCE_DIR "" CACHE PATH
    "A checkout of rexglue/FidelityFX-SDK at ${REXGLUE_FFX_COMMIT} (fetched when empty)")

if(REXGLUE_FFX_SOURCE_DIR)
    set(_ffx_source "${REXGLUE_FFX_SOURCE_DIR}")
else()
    include(FetchContent)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)  # FetchContent_Populate: no add_subdirectory
    endif()
    FetchContent_Declare(fidelityfx
        GIT_REPOSITORY https://github.com/rexglue/FidelityFX-SDK.git
        GIT_TAG ${REXGLUE_FFX_COMMIT})
    FetchContent_GetProperties(fidelityfx)
    if(NOT fidelityfx_POPULATED)
        FetchContent_Populate(fidelityfx)
    endif()
    set(_ffx_source "${fidelityfx_SOURCE_DIR}")
endif()

# Use the prebuilt permutations instead of rules that run FidelityFX_SC.exe.
set(_ffx_compile "${_ffx_source}/sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt")
file(READ "${_ffx_compile}" _ffx_text)
if(NOT _ffx_text MATCHES "FFX_PREBUILT_SHADER_PERMUTATIONS")
    string(REPLACE "\t\t# combine base and permutation args"
"\t\tif (FFX_PREBUILT_SHADER_PERMUTATIONS)
\t\t\tlist(APPEND _FFX_PERMUTATION_OUTPUTS ${WAVE32_PERMUTATION_HEADER} ${WAVE64_PERMUTATION_HEADER} ${WAVE32_16BIT_PERMUTATION_HEADER} ${WAVE64_16BIT_PERMUTATION_HEADER})
\t\t\tcontinue()
\t\tendif()
\t\t# combine base and permutation args" _ffx_patched "${_ffx_text}")
    if(_ffx_patched STREQUAL _ffx_text)
        message(FATAL_ERROR "FidelityFX: CMakeCompileShaders.txt is not the pinned version")
    endif()
    file(WRITE "${_ffx_compile}" "${_ffx_patched}")
endif()

# The Vulkan backend declares its per-effect context alignas(32) but places
# the array in its scratch buffer at 4-byte granularity. MSVC never relies on
# the alignment; clang zeroes the context with aligned SSE stores, which
# fault (FSR context creation crashed on the Steam Deck).
set(_ffx_vk "${_ffx_source}/sdk/src/backends/vk/ffx_vk.cpp")
file(READ "${_ffx_vk}" _ffx_text)
if(_ffx_text MATCHES "typedef struct alignas\\(32\\) EffectContext")
    string(REPLACE "typedef struct alignas(32) EffectContext" "typedef struct EffectContext"
        _ffx_text "${_ffx_text}")
    file(WRITE "${_ffx_vk}" "${_ffx_text}")
endif()
# For device-local memory the backend skips host-visible memory types, to keep
# its resources off a discrete GPU's small mappable heap. On unified memory
# (Adreno, Mali, Apple) every device-local type is host-visible, so no type
# qualified and context creation failed; fall back to one of them.
if(NOT _ffx_text MATCHES "fallbackCandidate")
    string(REPLACE "    uint32_t bestCandidate = UINT32_MAX;"
"    uint32_t bestCandidate = UINT32_MAX, fallbackCandidate = UINT32_MAX;
    VkMemoryPropertyFlags fallbackProperties = 0;" _ffx_text "${_ffx_text}")
    string(REPLACE
"if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))"
"if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (fallbackCandidate != UINT32_MAX || (fallbackCandidate = i, fallbackProperties = memProperties.memoryTypes[i].propertyFlags, true)))"
        _ffx_text "${_ffx_text}")
    string(REGEX REPLACE "\n    return bestCandidate;"
"\n    if (bestCandidate == UINT32_MAX && fallbackCandidate != UINT32_MAX) {
        outProperties = fallbackProperties;
        return fallbackCandidate;
    }
    return bestCandidate;" _ffx_text "${_ffx_text}")
    if(NOT _ffx_text MATCHES "return fallbackCandidate;")
        message(FATAL_ERROR "FidelityFX: ffx_vk.cpp is not the pinned version")
    endif()
    file(WRITE "${_ffx_vk}" "${_ffx_text}")
endif()

# The frame generation provider without AMD's swapchain replacement, whose
# implementation (FrameInterpolationSwapchain) the SDK only builds on Windows.
set(_ffx_provider "${_ffx_source}/ffx-api/src/ffx_provider.cpp")
file(READ "${_ffx_provider}" _ffx_text)
set(_ffx_swapchain_include "#include \"vk/ffx_provider_framegenerationswapchain_vk.h\"")
set(_ffx_swapchain_entry "&ffxProvider_FrameGenerationSwapChain_VK::Instance,")
string(FIND "${_ffx_text}" "${_ffx_swapchain_entry}" _ffx_at)
if(NOT _ffx_at EQUAL -1)
    string(FIND "${_ffx_text}" "${_ffx_swapchain_include}" _ffx_at)
    if(_ffx_at EQUAL -1)
        message(FATAL_ERROR "FidelityFX: ffx_provider.cpp is not the pinned version")
    endif()
    string(REPLACE "${_ffx_swapchain_include}" "// (no swapchain replacement)" _ffx_text "${_ffx_text}")
    string(REPLACE "${_ffx_swapchain_entry}" "" _ffx_text "${_ffx_text}")
    file(WRITE "${_ffx_provider}" "${_ffx_text}")
endif()
set(_ffx_api_lists "${_ffx_source}/ffx-api/CMakeLists.txt")
file(READ "${_ffx_api_lists}" _ffx_text)
if(NOT _ffx_text MATCHES "REXGLUE: no swapchain")
    set(_ffx_framegen_check "if (NOT FFX_API_ENABLE_FRAMEGEN_PROVIDER)")
    string(FIND "${_ffx_text}" "${_ffx_framegen_check}" _ffx_at)
    if(_ffx_at EQUAL -1)
        message(FATAL_ERROR "FidelityFX: ffx-api/CMakeLists.txt is not the pinned version")
    endif()
    string(REPLACE "${_ffx_framegen_check}"
        "# REXGLUE: no swapchain replacement\nlist(FILTER private_ffx_api_backend_vk EXCLUDE REGEX \"framegenerationswapchain_vk\")\n${_ffx_framegen_check}"
        _ffx_text "${_ffx_text}")
    file(WRITE "${_ffx_api_lists}" "${_ffx_text}")
endif()

# The backend's output path: <ffx-api binary>/sdk/src/backends/shaders/vk.
set(_ffx_archive "${CMAKE_CURRENT_SOURCE_DIR}/vk-permutations.tar.gz")
file(SHA256 "${_ffx_archive}" _ffx_archive_hash)
if(NOT _ffx_archive_hash STREQUAL REXGLUE_FFX_PERMUTATIONS_SHA256)
    message(FATAL_ERROR "FidelityFX: vk-permutations.tar.gz hash ${_ffx_archive_hash}")
endif()
file(ARCHIVE_EXTRACT INPUT "${_ffx_archive}"
     DESTINATION "${CMAKE_BINARY_DIR}/ffx-api/sdk/src/backends/shaders")

# The library takes most Vulkan functions from the application but calls a
# few from the loader, and the backend asks for the Vulkan package: the
# headers are ShiftGlue's. Android links the NDK's libvulkan.so; macOS links
# the loader the game stages in vulkan/lib (install name
# @rpath/libvulkan.1.dylib), so pass -DVulkan_LIBRARY=<that dylib> there.
if(NOT Vulkan_INCLUDE_DIR)
    set(Vulkan_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan-headers/include")
endif()
if(ANDROID AND NOT Vulkan_LIBRARY)
    find_library(Vulkan_LIBRARY vulkan REQUIRED)
elseif(UNIX AND NOT APPLE AND NOT Vulkan_LIBRARY)
    # The loader itself when no development package installed the
    # libvulkan.so link.
    find_library(Vulkan_LIBRARY NAMES vulkan libvulkan.so.1 REQUIRED)
endif()

set(FFX_API_BACKEND VK_X64 CACHE STRING "" FORCE)
set(FFX_API_ENABLE_FRAMEGEN_PROVIDER ON CACHE BOOL "" FORCE)
set(FFX_API_AUTO_COMPILE_SHADERS OFF CACHE BOOL "" FORCE)
set(FFX_PREBUILT_SHADER_PERMUTATIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory("${_ffx_source}/ffx-api" ffx-api)
unset(CMAKE_GENERATOR_PLATFORM CACHE)

# The SDK writes its static libraries to <top source>/bin/ffx_sdk: keep them
# in the build tree instead.
function(_ffx_keep_archives_in_build directory)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        get_target_property(_type ${_target} TYPE)
        if(_type STREQUAL "STATIC_LIBRARY")
            foreach(_config "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
                set_target_properties(${_target} PROPERTIES
                    ARCHIVE_OUTPUT_DIRECTORY${_config} "${CMAKE_BINARY_DIR}/ffx_sdk")
            endforeach()
        endif()
    endforeach()
    get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
        _ffx_keep_archives_in_build("${_subdirectory}")
    endforeach()
endfunction()
_ffx_keep_archives_in_build("${_ffx_source}/ffx-api")
if(APPLE)
    # Loaded from upscalers/fidelityfx/<version>/ beside the executable.
    set_target_properties(amd_fidelityfx_vk PROPERTIES
        INSTALL_RPATH "@executable_path/vulkan/lib" BUILD_WITH_INSTALL_RPATH ON)
endif()

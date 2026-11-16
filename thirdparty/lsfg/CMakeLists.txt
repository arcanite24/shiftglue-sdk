# Lossless Scaling frame generation (UP-5.2) inside ShiftGlue's Vulkan
# presenter. Two libraries, both from MIT/zlib sources with their notices
# (README.md):
#   lsfg_dxbc     DXVK 2.6.2's DXBC to SPIR-V compiler, trimmed to stand alone
#   lsfg_framegen lsfg-vk 1.0.0's LSFG 3.1 compute passes, run on the
#                 presenter's device and command buffer
# The shaders themselves are never part of the build: they are read at run
# time from the player's own Lossless.dll.

cmake_minimum_required(VERSION 3.20)

if(NOT TARGET lsfg_dxbc)
    file(GLOB _lsfg_dxbc_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/dxbc/src/dxbc/*.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/dxbc/src/spirv/*.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/loader/*.cpp")
    add_library(lsfg_dxbc STATIC ${_lsfg_dxbc_sources})
    target_compile_features(lsfg_dxbc PUBLIC cxx_std_20)
    target_include_directories(lsfg_dxbc
        PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/loader>"
        PRIVATE "${CMAKE_CURRENT_LIST_DIR}/dxbc/src" "${LSFG_SPIRV_HEADERS_DIR}")
    if(NOT TARGET Vulkan::Headers AND LSFG_VULKAN_HEADERS_DIR)
        target_include_directories(lsfg_dxbc PRIVATE "${LSFG_VULKAN_HEADERS_DIR}")
    elseif(TARGET Vulkan::Headers)
        target_link_libraries(lsfg_dxbc PRIVATE Vulkan::Headers)
    endif()
    target_compile_definitions(lsfg_dxbc PRIVATE NOMINMAX VK_NO_PROTOTYPES)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(lsfg_dxbc PRIVATE -w)
    endif()
    set_target_properties(lsfg_dxbc PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(NOT TARGET lsfg_framegen)
    set(_lsfg_fg "${CMAKE_CURRENT_LIST_DIR}/framegen")
    file(GLOB _lsfg_framegen_sources CONFIGURE_DEPENDS
        "${_lsfg_fg}/src/common/*.cpp"
        "${_lsfg_fg}/src/core/*.cpp"
        "${_lsfg_fg}/src/pool/*.cpp"
        "${_lsfg_fg}/v3.1_src/shaders/*.cpp"
        "${_lsfg_fg}/v3.1p_src/shaders/*.cpp"
        "${_lsfg_fg}/shim/volk.cpp"
        "${_lsfg_fg}/embed/lsfg_embedded.cpp")
    add_library(lsfg_framegen STATIC ${_lsfg_framegen_sources})
    target_compile_features(lsfg_framegen PUBLIC cxx_std_20)
    target_include_directories(lsfg_framegen
        PUBLIC "$<BUILD_INTERFACE:${_lsfg_fg}/embed>"
        PRIVATE "${_lsfg_fg}/include" "${_lsfg_fg}/shim"
                "${_lsfg_fg}/v3.1_include" "${_lsfg_fg}/v3.1p_include")
    if(NOT TARGET Vulkan::Headers AND LSFG_VULKAN_HEADERS_DIR)
        target_include_directories(lsfg_framegen PRIVATE "${LSFG_VULKAN_HEADERS_DIR}")
    elseif(TARGET Vulkan::Headers)
        target_link_libraries(lsfg_framegen PRIVATE Vulkan::Headers)
    endif()
    target_compile_definitions(lsfg_framegen PRIVATE NOMINMAX VK_NO_PROTOTYPES)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(lsfg_framegen PRIVATE -w)
    endif()
    set_target_properties(lsfg_framegen PROPERTIES POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden)
endif()

# rexgpu-fh1: Forza Horizon 1-specific D3D12 renderer
# Namespace: rex::graphics
#
# Runtime-loaded via rex::system::LoadGpuPlugin; consumers never link this
# target. The Xenos format layer is compiled into rexcore.

set(REXGPU_XENOS_SOURCES
    plugin_main.cpp

    # Core GPU types
    flags.cpp
    register_file.cpp
    registers.cpp
    pipeline/shader/shader.cpp
    format/ucode.cpp
    sampler_info.cpp

    # GPU system
    graphics_system.cpp
    command_processor.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/shader/interpreter.cpp
)

set(REXGPU_FH1_SHADER_ANALYSIS_SOURCES
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGPU_XENOS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shared_memory.cpp
    vulkan/texture_cache.cpp
)

# D3D12 backend
if(REXGLUE_USE_D3D12)
    # DXBC checksum is also used by FH1's runtime render-target conversion and
    # geometry-shader generators.
    set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # The runtime needs the DXBC container, but guest translation is confined
    # to the explicit offline producer.
    set(REXGPU_XENOS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_om.cpp
    )
    set(REXGPU_FH1_SHADER_PRODUCER_SOURCES
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
    )

    # D3D12 GPU backend
    set(REXGPU_XENOS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/primitive_processor.cpp
        d3d12/render_target_cache.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/deferred_command_list.cpp
        d3d12/fh1_shader_pack.cpp
        d3d12/pipeline_cache.cpp
    )
endif()

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_SPIRV_SOURCES} ${REXGPU_XENOS_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGPU_XENOS_SOURCES
        ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        ${REXGPU_XENOS_DXBC_SOURCES}
        ${REXGPU_XENOS_D3D12_SOURCES})
endif()

add_library(rexgpu-fh1 SHARED ${REXGPU_XENOS_SOURCES})
add_library(rex::gpu-fh1 ALIAS rexgpu-fh1)
if(REXGLUE_USE_D3D12)
    add_library(rexgpu-fh1-producer SHARED EXCLUDE_FROM_ALL
        ${REXGPU_XENOS_SOURCES}
        ${REXGPU_FH1_SHADER_ANALYSIS_SOURCES}
        ${REXGPU_FH1_SHADER_PRODUCER_SOURCES})
    target_compile_definitions(rexgpu-fh1-producer PRIVATE
        REXGPU_FH1_SHADER_PRODUCER=1)
endif()

foreach(rexgpu_fh1_target IN ITEMS rexgpu-fh1 rexgpu-fh1-producer)
    if(NOT TARGET ${rexgpu_fh1_target})
        continue()
    endif()
    set_target_properties(${rexgpu_fh1_target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    target_include_directories(${rexgpu_fh1_target} PRIVATE
        ${REXGLUE_ROOT}/thirdparty/renderdoc
        ${REXGLUE_ROOT})
    target_compile_options(${rexgpu_fh1_target} PRIVATE -Wno-unused-variable)
    rex_target_pch(${rexgpu_fh1_target}
        <mutex>
        <unordered_map>
        <rex/math.h>
        <rex/memory.h>
        <rex/thread/mutex.h>
        <rex/system/xmemory.h>
        <rex/graphics/xenos.h>
        <rex/graphics/registers.h>
        <rex/graphics/format/ucode.h>
        <rex/graphics/pipeline/shader/shader.h>)
    target_link_libraries(${rexgpu_fh1_target} PRIVATE
        rexruntime
        xxHash::xxhash)
    if(WIN32)
        target_link_libraries(${rexgpu_fh1_target} PRIVATE bcrypt)
    endif()
    if(REXGLUE_USE_D3D12)
        target_link_libraries(${rexgpu_fh1_target} PRIVATE d3d12 dxgi dxguid)
    endif()
endforeach()
set_target_properties(rexgpu-fh1 PROPERTIES EXPORT_NAME gpu-fh1)

if(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES)
    set_source_files_properties(${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# Everything PRIVATE: the plugin has no consumers. Never link the OBJECT
# libraries (rexcore/rexui) here; rexruntime already contains those objects
# and relinking them would duplicate global state.
if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexgpu-fh1 PRIVATE
        Vulkan::Headers
        glslang::SPIRV
        GPUOpen::VulkanMemoryAllocator
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
    )
endif()
